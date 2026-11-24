// Exercise the production panel in a disposable XAML Island, never the shell.
#include "../src/flyout.cpp"
#include "../src/cache.h"
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <future>
#include <iostream>

namespace {
int failures = 0;
void Check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; ++failures; }
}

void Pump(unsigned milliseconds) {
    const auto end = GetTickCount64() + milliseconds;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(1);
    } while (GetTickCount64() < end);
}

Windows::Foundation::DateTime LocalDate(LONG key) {
    auto local = DayFromKey(key);
    local.wHour = 12;
    SYSTEMTIME utc{};
    FILETIME file{};
    check_bool(TzSpecificLocalTimeToSystemTimeEx(nullptr, &local, &utc));
    check_bool(SystemTimeToFileTime(&utc, &file));
    ULARGE_INTEGER ticks{};
    ticks.LowPart = file.dwLowDateTime;
    ticks.HighPart = file.dwHighDateTime;
    return Windows::Foundation::DateTime{Windows::Foundation::TimeSpan{static_cast<int64_t>(ticks.QuadPart)}};
}

std::shared_ptr<Session> NewSession() {
    auto session = std::make_shared<Session>();
    session->controller = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    session->agendaMutex = CreateMutexW(nullptr, FALSE, nullptr);
    const auto mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(FlyoutState), nullptr);
    check_bool(mapping != nullptr);
    session->shared = static_cast<FlyoutState*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(FlyoutState)));
    CloseHandle(mapping);
    check_bool(session->controller && session->agendaMutex && session->shared);
    session->shared->protocol = FlyoutProtocol;
    session->token = 1;
    session->ownsClaim = true;
    InterlockedExchange64(&session->shared->attachmentOffer, -session->token);
    InterlockedExchange(&session->shared->loadedPid, static_cast<LONG>(GetCurrentProcessId()));
    return session;
}

void Publish(Session& session, const std::wstring& title) {
    const CachedCalendar home{20261116, 20261201, {{20261117, {title}}}};
    const auto publication = PrepareAgenda({DayFromKey(home.first), DayFromKey(home.end)}, home, {}, L"", L"");
    const AgendaLock lock(session.agendaMutex, INFINITE);
    session.shared->agenda.index = publication.index;
    std::copy(publication.text.begin(), publication.text.end(), session.shared->agenda.text);
    InterlockedIncrement(&session.shared->agendaRevision);
}

void Lifecycle() {
    using namespace Windows::UI::Xaml::Hosting;
    auto manager = WindowsXamlManager::InitializeForCurrentThread();
    const auto host = CreateWindowExW(0, L"STATIC", L"wincal native lifecycle test", WS_OVERLAPPEDWINDOW,
        0, 0, 480, 620, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check_bool(host != nullptr);
    {
        DesktopWindowXamlSource source;
        const auto native = source.as<IDesktopWindowXamlSourceNative>();
        check_hresult(native->AttachToWindow(host));
        HWND island{};
        check_hresult(native->get_WindowHandle(&island));
        SetWindowPos(island, nullptr, 0, 0, 480, 620, SWP_NOZORDER | SWP_SHOWWINDOW);
        Grid root;
        source.Content(root);
        for (int cycle = 0; cycle < 6; ++cycle) {
            auto session = NewSession();
            Publish(*session, L"Cached event");
            CalendarView calendar;
            calendar.Name(L"CalendarControl");
            calendar.MinDate(LocalDate(20261101));
            calendar.MaxDate(LocalDate(20261231));
            const auto originalMode = cycle % 2 ? CalendarViewSelectionMode::Single : CalendarViewSelectionMode::None;
            calendar.SelectionMode(originalMode);
            calendar.SetDisplayDate(LocalDate(20261117));
            ScrollViewer viewer;
            viewer.Name(L"CalendarControlScrollViewer");
            viewer.VerticalScrollBarVisibility(ScrollBarVisibility::Hidden);
            viewer.Content(calendar);
            root.Children().Append(viewer);
            root.Measure({480, 620});
            root.Arrange({0, 0, 480, 620});
            root.UpdateLayout();
            Pump(30);
            unsigned remaining = 12000;
            Check(FindCalendar(root, remaining) == calendar, "The native visual tree should expose its named calendar");
            auto panel = std::make_shared<Panel>();
            panel->session = session;
            Check(panel->Install(calendar), "The production panel should install into a native XAML visual tree");
            session->installed = true;
            root.UpdateLayout();
            calendar.SelectedDates().Clear();
            calendar.SelectedDates().Append(LocalDate(20261117));
            panel->SelectDate(calendar);
            Check(panel->text.Text() == L"Cached event" && ReadShared(session->shared->cachedSelection) == 20261117,
                "Selecting a cached date should immediately render its saved event");
            panel->UpdateMarkers(calendar);
            Check(!panel->markedItems.empty(), "The realized native calendar should mark its busy date");
            calendar.SelectedDates().Clear();
            calendar.SelectedDates().Append(LocalDate(20261118));
            panel->SelectDate(calendar);
            Check(panel->text.Text() == L"No events on this date.", "Selecting a known empty date should render an empty agenda");
            auto duplicate = std::make_shared<Panel>();
            duplicate->session = session;
            Check(!duplicate->Install(calendar), "Repeated attachment must not add a duplicate panel");
            CalendarViewDayItem item;
            const auto originalTemplate = item.ReadLocalValue(Control::TemplateProperty());
            panel->MarkDay(item, 20261117);
            Check(item.Template() == panel->dot, "A busy date should receive its marker");
            panel->MarkDay(item, 20261118);
            Check(item.ReadLocalValue(Control::TemplateProperty()) == originalTemplate,
                "Recycling a busy day into an empty date must restore its original template");

            Publish(*session, L"Updated event");
            std::promise<void> locked;
            const auto release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            std::thread holder([&] {
                const AgendaLock lock(session->agendaMutex, INFINITE);
                locked.set_value();
                WaitForSingleObject(release, 5000);
            });
            locked.get_future().wait();
            const auto before = GetTickCount64();
            Check(!panel->ReadAgenda() && GetTickCount64() - before < 100,
                "UI snapshot reads must return immediately while the publisher owns the mutex");
            SetEvent(release);
            holder.join();
            CloseHandle(release);
            Check(panel->ReadAgenda(), "A contended snapshot should be retried after the mutex becomes available");
            calendar.SelectedDates().Clear();
            calendar.SelectedDates().Append(LocalDate(20261117));
            panel->SelectDate(calendar);
            Check(panel->text.Text() == L"Updated event", "A published revision should update the agenda");

            std::atomic_bool done{};
            std::thread producer([&] {
                for (int i = 0; i < 100; ++i) { Publish(*session, std::wstring(500, i % 2 ? L'A' : L'B')); Sleep(1); }
                done = true;
            });
            do {
                if (panel->ReadAgenda()) {
                    const auto value = panel->agenda.TextForDay(20261117, panel->agendaText.data());
                    Check(value == L"Updated event" || value == std::wstring(500, L'A') || value == std::wstring(500, L'B'),
                        "Concurrent publication must never expose torn index or text data");
                }
                Pump(1);
            } while (!done);
            producer.join();
            // Use the actual detach entry point as well as controller-exit cleanup.
            if (cycle % 2) SetEvent(session->controller);
            else { const auto tap = make_self<Tap>(); tap->session = session; check_hresult(tap->SetSite(nullptr)); }
            const auto cleanupDeadline = GetTickCount64() + 2000;
            do { Pump(10); } while (session->installed && GetTickCount64() < cleanupDeadline);
            Check(!session->installed && viewer.Content() == calendar &&
                viewer.VerticalScrollBarVisibility() == ScrollBarVisibility::Hidden && calendar.SelectionMode() == originalMode,
                "Detach and controller exit must restore native content, scrolling and selection mode");
            Check(!ReadShared64(session->shared->attachmentOffer) && !ReadShared(session->shared->loadedPid),
                "A stopped session should allow reattachment to the same shell process");
            panel->Remove();
            const auto weak = std::weak_ptr<Panel>(panel);
            panel.reset();
            Pump(10);
            Check(weak.expired(), "A removed panel must not be retained by its timer or event handlers");
            root.Children().Clear();
            Pump(10);
        }
        source.Content(nullptr);
        source.Close();
    }
    DestroyWindow(host);
    manager.Close();
    Pump(20);
}
}

int main() {
    try {
        init_apartment(apartment_type::single_threaded);
        Lifecycle();
        if (!failures) std::cout << "Native XAML selection, publication, contention, marker and lifecycle tests passed.\n";
        return failures != 0;
    } catch (const hresult_error& error) {
        std::wcerr << L"Native XAML test failed: 0x" << std::hex << static_cast<unsigned>(error.code()) << L" " << error.message().c_str() << L'\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
