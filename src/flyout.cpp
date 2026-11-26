#include "flyout.h"
#include <ocidl.h>
#include <xamlom.h>
#undef GetCurrentTime
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

using namespace winrt;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Media;

namespace {
struct Session {
    FlyoutState* shared{};
    HANDLE controller{}, agendaMutex{};
    LONG64 token{};
    std::atomic_bool ownsClaim{};
    std::atomic_bool installed{}, detached{};
    void ReleaseClaim() {
        if (shared && ownsClaim.exchange(false)) ReleaseAttachmentClaim(*shared, token, static_cast<LONG>(GetCurrentProcessId()));
    }
    ~Session() {
        ReleaseClaim();
        if (shared) UnmapViewOfFile(shared);
        if (agendaMutex) CloseHandle(agendaMutex);
        if (controller) CloseHandle(controller);
    }
    bool Active() const {
        return !detached && (!token || ReadShared64(shared->attachmentOffer) == -token) &&
            WaitForSingleObject(controller, 0) == WAIT_TIMEOUT;
    }
    void Stage(LONG value) const {
        LONG current = ReadShared(shared->stage);
        while (current < value) {
            const LONG previous = InterlockedCompareExchange(&shared->stage, value, current);
            if (previous == current) break;
            current = previous;
        }
    }
    void Error(HRESULT value) const { InterlockedCompareExchange(&shared->error, value, 0); }
};

CalendarView FindCalendar(const DependencyObject& root, unsigned& remaining, unsigned depth = 0) {
    if (!root || !remaining || depth > 36) return nullptr;
    --remaining;
    if (auto control = root.try_as<CalendarView>(); control && control.Name() == L"CalendarControl") return control;
    const auto count = VisualTreeHelper::GetChildrenCount(root);
    for (int index = 0; index < count && remaining; ++index) {
        if (auto control = FindCalendar(VisualTreeHelper::GetChild(root, index), remaining, depth + 1)) return control;
    }
    return nullptr;
}

LONG CalendarDate(Windows::Foundation::DateTime value) {
    const auto fileTime = Windows::Foundation::DateTime::clock::to_file_time(value).value;
    FILETIME stamp{static_cast<DWORD>(fileTime), static_cast<DWORD>(fileTime >> 32)};
    SYSTEMTIME utc{}, local{};
    if (!FileTimeToSystemTime(&stamp, &utc) || !SystemTimeToTzSpecificLocalTimeEx(nullptr, &utc, &local)) return 0;
    return local.wYear * 10000L + local.wMonth * 100L + local.wDay;
}

struct Panel : std::enable_shared_from_this<Panel> {
    std::shared_ptr<Session> session;
    weak_ref<CalendarView> calendar;
    weak_ref<ScrollViewer> calendarScroll;
    StackPanel wrapper{nullptr};
    ScrollViewer agendaScroll{nullptr};
    Windows::Foundation::IInspectable originalContent{nullptr};
    ScrollBarVisibility originalVertical{};
    TextBlock text{nullptr};
    DispatcherTimer timer{nullptr};
    event_token tick{}, selected{}, dayChanging{};
    bool changedMode{}, listening{}, marking{};
    LONG day{}, agendaRevision{};
    ControlTemplate dot{nullptr};
    AgendaIndex agenda{};
    std::vector<wchar_t> agendaText;
    struct MarkedItem {
        weak_ref<CalendarViewDayItem> item;
        Windows::Foundation::IInspectable previous;
    };
    std::vector<MarkedItem> markedItems;

    void SelectDate(const CalendarView& control) {
        SYSTEMTIME date{};
        GetLocalTime(&date);
        const auto dates = control.SelectedDates();
        const LONG selectedDay = dates.Size() ? CalendarDate(dates.GetAt(0)) : 0;
        const LONG nextDay = selectedDay ? selectedDay : date.wYear * 10000L + date.wMonth * 100L + date.wDay;
        const bool changed = day != nextDay;
        day = nextDay;
        InterlockedExchange(&session->shared->requestedDay, day);
        RenderAgenda();
        if (changed) agendaScroll.ChangeView(nullptr, 0.0, nullptr, true);
        if (agenda.home.Contains(day) || agenda.extra.Contains(day))
            InterlockedExchange(&session->shared->cachedSelection, day);
    }

    bool ReadAgenda() {
        auto& shared = *session->shared;
        if (ReadShared(shared.agendaRevision) == agendaRevision) return false;
        // The UI always has a local snapshot. A concurrent publication must
        // never make a selection wait for the launcher.
        const AgendaLock lock(session->agendaMutex, 0);
        if (lock.result != WAIT_OBJECT_0) return false;
        const auto& published = shared.agenda;
        agendaText.assign(published.text, published.text + published.index.textLength);
        agenda = published.index;
        agendaRevision = ReadShared(shared.agendaRevision);
        return true;
    }

    void RenderAgenda() {
        const hstring value{agenda.TextForDay(day, agendaText.data())};
        if (text.Text() != value) text.Text(value);
    }

    void RestoreDay(const CalendarViewDayItem& item, const Windows::Foundation::IInspectable& previous) {
        if (item.Template() != dot) return;
        const auto property = Control::TemplateProperty();
        if (previous == DependencyProperty::UnsetValue()) item.ClearValue(property);
        else item.SetValue(property, previous);
    }

    void MarkDay(const CalendarViewDayItem& item, LONG key) {
        const bool hasEvents = key && agenda.Find(key);
        const auto tracked = std::find_if(markedItems.begin(), markedItems.end(),
            [&](const MarkedItem& entry) { return entry.item.get() == item; });
        if (hasEvents && tracked == markedItems.end()) {
            markedItems.push_back({make_weak(item), item.ReadLocalValue(Control::TemplateProperty())});
            item.Template(dot);
        } else if (!hasEvents && tracked != markedItems.end()) {
            const auto previous = tracked->previous;
            markedItems.erase(tracked);
            RestoreDay(item, previous);
        }
    }

    void MarkRealizedDays(const DependencyObject& node) {
        if (const auto item = node.try_as<CalendarViewDayItem>()) {
            MarkDay(item, CalendarDate(item.Date()));
            return;
        }
        for (int index = 0, count = VisualTreeHelper::GetChildrenCount(node); index < count; ++index)
            MarkRealizedDays(VisualTreeHelper::GetChild(node, index));
    }

    void UpdateMarkers(const CalendarView& control) {
        std::erase_if(markedItems, [](const MarkedItem& entry) { return !entry.item.get(); });
        MarkRealizedDays(control);
    }

    void Remove() {
        if (timer) { timer.Stop(); timer.Tick(tick); timer = nullptr; }
        if (!session->Active()) session->ReleaseClaim();
        if (const auto control = calendar.get()) {
            if (marking) control.CalendarViewDayItemChanging(dayChanging);
            if (listening) control.SelectedDatesChanged(selected);
            if (changedMode && control.SelectionMode() == CalendarViewSelectionMode::Single)
                control.SelectionMode(CalendarViewSelectionMode::None);
        }
        for (const auto& entry : markedItems)
            if (const auto item = entry.item.get()) RestoreDay(item, entry.previous);
        if (const auto viewer = calendarScroll.get(); viewer && (!viewer.Content() || viewer.Content() == wrapper)) {
            wrapper.Children().Clear();
            viewer.Content(originalContent);
            viewer.VerticalScrollBarVisibility(originalVertical);
        }
        session->installed = false;
    }

    bool Install(const CalendarView& control) {
        ScrollViewer viewer{nullptr};
        for (auto parent = VisualTreeHelper::GetParent(control); parent; parent = VisualTreeHelper::GetParent(parent)) {
            const auto candidate = parent.try_as<ScrollViewer>();
            if (candidate && candidate.Name() == L"CalendarControlScrollViewer") { viewer = candidate; break; }
        }
        if (!viewer) return false;
        // Keep the native calendar as the first child of its existing scroll area.
        // This keeps the agenda reachable when the flyout has a fixed height.
        const auto nativeContent = viewer.Content().try_as<FrameworkElement>();
        if (!nativeContent || nativeContent.Name() == L"Win11CalendarFixContent") return false;

        StackPanel content;
        content.Spacing(6);
        TextBlock title;
        title.Text(L"Events");
        title.FontWeight({600});
        text = TextBlock();
        text.TextWrapping(TextWrapping::Wrap);
        content.Children().Append(title);
        content.Margin({16, 8, 16, 12});
        agendaScroll = ScrollViewer();
        agendaScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        agendaScroll.IsVerticalScrollChainingEnabled(false);
        agendaScroll.MaxHeight(96);
        agendaScroll.Content(text);
        content.Children().Append(agendaScroll);
        wrapper = StackPanel();
        wrapper.Name(L"Win11CalendarFixContent");

        calendar = make_weak(control);
        calendarScroll = make_weak(viewer);
        originalContent = viewer.Content();
        originalVertical = viewer.VerticalScrollBarVisibility();
        dot = Markup::XamlReader::Load(LR"(<ControlTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
            TargetType="CalendarViewDayItem">
            <Ellipse Width="4" Height="4" HorizontalAlignment="Center" VerticalAlignment="Bottom" Margin="0,0,0,2"
                Fill="{ThemeResource SystemControlHighlightAccentBrush}" IsHitTestVisible="False" />
            </ControlTemplate>)").as<ControlTemplate>();
        dayChanging = control.CalendarViewDayItemChanging([weak = weak_from_this()](auto, const CalendarViewDayItemChangingEventArgs& args) {
            if (const auto self = weak.lock()) {
                try {
                    const auto item = args.Item();
                    const LONG key = args.InRecycleQueue() ? 0 : CalendarDate(item.Date());
                    self->MarkDay(item, key);
                    if (key) InterlockedExchange(&self->session->shared->visibleDay, key);
                } catch (...) { self->session->Error(to_hresult()); }
            }
        });
        marking = true;
        viewer.Content(nullptr);
        wrapper.Children().Append(nativeContent);
        wrapper.Children().Append(content);
        viewer.Content(wrapper);
        viewer.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        if (control.SelectionMode() == CalendarViewSelectionMode::None) {
            control.SelectionMode(CalendarViewSelectionMode::Single);
            changedMode = true;
        }
        selected = control.SelectedDatesChanged([weak = weak_from_this()](auto sender, auto) {
            if (const auto self = weak.lock()) {
                try { self->SelectDate(sender); }
                catch (...) { self->session->Error(to_hresult()); }
            }
        });
        listening = true;
        ReadAgenda();
        SelectDate(control);
        UpdateMarkers(control);
        timer = DispatcherTimer();
        timer.Interval(std::chrono::milliseconds(250));
        tick = timer.Tick([self = shared_from_this()](auto, auto) {
            try {
                const auto control = self->calendar.get();
                const auto viewer = self->calendarScroll.get();
                if (!self->session->Active() || !control || !viewer || viewer.Content() != self->wrapper) self->Remove();
                else if (self->ReadAgenda()) {
                    self->UpdateMarkers(control);
                    self->RenderAgenda();
                }
            } catch (...) { self->session->Error(to_hresult()); }
        });
        timer.Start();
        return true;
    }
};

UINT ProbeMessage() { static const UINT value = RegisterWindowMessageW(L"wincal.LiveFlyout.045"); return value; }
std::atomic<std::shared_ptr<Session>> probing;

LRESULT CALLBACK OnFlyoutThread(int code, WPARAM sent, LPARAM data) noexcept {
    if (code == HC_ACTION) {
        const auto call = reinterpret_cast<const CWPSTRUCT*>(data);
        const auto session = probing.load();
        if (session && call && call->message == ProbeMessage() && call->hwnd == ReadFlyout(*session->shared) && session->Active()) {
            session->Stage(3);
            try {
                const auto window = Window::Current();
                if (window) {
                    unsigned remaining = 12000;
                    const auto calendar = FindCalendar(window.Content(), remaining);
                    if (calendar) {
                        session->Stage(4);
                        const auto panel = std::make_shared<Panel>();
                        panel->session = session;
                        try {
                            if (panel->Install(calendar)) {
                                session->installed = true;
                                session->Stage(5);
                            }
                        } catch (...) {
                            session->Error(to_hresult());
                            try { panel->Remove(); } catch (...) {}
                        }
                    }
                }
            } catch (...) { session->Error(to_hresult()); }
        }
    }
    return CallNextHookEx(nullptr, code, sent, data);
}

struct Tap : implements<Tap, IObjectWithSite> {
    com_ptr<IUnknown> site;
    std::shared_ptr<Session> session;

    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* value) noexcept override {
        if (!value) {
            if (session) { session->detached = true; session->ReleaseClaim(); }
            session.reset();
            site = nullptr;
            return S_OK;
        }
        if (session) return E_UNEXPECTED;
        try {
            site.copy_from(value);
            const auto diagnostics = site.as<IXamlDiagnostics>();
            BSTR data{};
            check_hresult(diagnostics->GetInitializationData(&data));
            unsigned long long mapping{}, controller{}, agendaMutex{}, nonce0{}, nonce1{};
            long long token{};
            const bool parsed = data && swscanf_s(data, L"%llu %llu %llu %lld %llu %llu",
                &mapping, &controller, &agendaMutex, &token, &nonce0, &nonce1) == 6;
            SysFreeString(data);
            if (!parsed || !mapping || !controller || !agendaMutex || token <= 0) return E_INVALIDARG;
            auto pending = std::make_shared<Session>();
            auto handle = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(mapping));
            pending->shared = static_cast<FlyoutState*>(MapViewOfFile(handle, FILE_MAP_WRITE, 0, 0, sizeof(FlyoutState)));
            if (!pending->shared) return HRESULT_FROM_WIN32(GetLastError());
            // Until the claim succeeds, none of the supplied handles belong to
            // this callback. A timed-out offer may already have been reclaimed.
            if (!ClaimAttachment(*pending->shared, token, nonce0, nonce1)) return E_ABORT;
            pending->token = token;
            pending->ownsClaim = true;
            pending->controller = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(controller));
            pending->agendaMutex = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(agendaMutex));
            CloseHandle(handle);
            session = std::move(pending);
            HMODULE pinned{};
            check_bool(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&OnFlyoutThread), &pinned));
            InterlockedExchange(&session->shared->loadedPid, static_cast<LONG>(GetCurrentProcessId()));
            std::thread([state = session] {
                if (state->Active()) state->Stage(2);
                while (state->Active()) {
                    if (!state->installed) {
                        const auto window = ReadFlyout(*state->shared);
                        DWORD pid{};
                        const auto thread = window ? GetWindowThreadProcessId(window, &pid) : 0;
                        if (thread && pid == GetCurrentProcessId() && ProbeMessage()) {
                            const auto hook = SetWindowsHookExW(WH_CALLWNDPROC, OnFlyoutThread, nullptr, thread);
                            if (hook) {
                                probing.store(state);
                                DWORD_PTR result{};
                                SendMessageTimeoutW(window, ProbeMessage(), 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &result);
                                UnhookWindowsHookEx(hook);
                                probing.store(nullptr);
                            } else state->Error(HRESULT_FROM_WIN32(GetLastError()));
                        }
                    }
                    WaitForSingleObject(state->controller, 750);
                }
                state->ReleaseClaim();
            }).detach();
            return S_OK;
        } catch (...) {
            const auto error = to_hresult();
            session.reset();
            site = nullptr;
            return error;
        }
    }
    HRESULT STDMETHODCALLTYPE GetSite(REFIID iid, void** result) noexcept override {
        if (!result) return E_POINTER;
        *result = nullptr;
        return site ? site->QueryInterface(iid, result) : E_FAIL;
    }
};

struct Factory : implements<Factory, IClassFactory> {
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** result) noexcept override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        try { return make_self<Tap>()->QueryInterface(iid, result); }
        catch (...) { return to_hresult(); }
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override { return S_OK; }
};
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** result) {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (clsid != FlyoutTap) return CLASS_E_CLASSNOTAVAILABLE;
    try { return make_self<Factory>()->QueryInterface(iid, result); }
    catch (...) { return to_hresult(); }
}

STDAPI DllCanUnloadNow() { return S_FALSE; }
