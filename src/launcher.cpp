#include "cache.h"
#include "flyout.h"
#include "attachment.h"
#include <xamlom.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <cstdio>

namespace fs = std::filesystem;
namespace {
struct CloseHandleDeleter { void operator()(HANDLE value) const { if (value) CloseHandle(value); } };
using Handle = std::unique_ptr<void, CloseHandleDeleter>;
struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

bool ReadSettings(const fs::path& path, Settings& settings, std::vector<BYTE>* cacheContext = nullptr) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const auto size = file.tellg();
    if (size <= 0 || size > 32768) return false;
    file.seekg(0);
    std::vector<BYTE> bytes(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) return false;
    DATA_BLOB encrypted{static_cast<DWORD>(bytes.size()), bytes.data()}, clear{};
    if (!CryptUnprotectData(&encrypted, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &clear)) return false;
    if (clear.cbData == sizeof(Settings)) memcpy(&settings, clear.pbData, sizeof(Settings));
    const bool valid = clear.cbData == sizeof(Settings) && !settings.url[2047] && !settings.username[255] &&
        !settings.password[511] && settings.demo <= 1;
    SecureZeroMemory(clear.pbData, clear.cbData);
    LocalFree(clear.pbData);
    if (valid && cacheContext) *cacheContext = std::move(bytes);
    return valid;
}

bool IsNotificationWindow(HWND window) {
    if (!window) return false;
    wchar_t name[64];
    if (!GetClassNameW(window, name, ARRAYSIZE(name)) || wcscmp(name, L"Windows.UI.Core.CoreWindow")) return false;
    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    wchar_t executable[MAX_PATH];
    DWORD size = ARRAYSIZE(executable);
    wchar_t windows[MAX_PATH];
    if (!process || !QueryFullProcessImageNameW(process.get(), 0, executable, &size) ||
        !GetWindowsDirectoryW(windows, ARRAYSIZE(windows))) return false;
    const auto expected = fs::path(windows) / L"SystemApps" / L"ShellExperienceHost_cw5n1h2txyewy" / L"ShellExperienceHost.exe";
    return _wcsicmp(executable, expected.c_str()) == 0;
}

BOOL CALLBACK ProbeMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM value) {
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    const auto area = info.rcWork;
    const auto window = WindowFromPoint({area.right - 50, area.bottom - 96});
    if (!IsNotificationWindow(window)) return TRUE;
    *reinterpret_cast<HWND*>(value) = window;
    return FALSE;
}

constexpr ULONGLONG RefreshIntervalMs = 60000;
constexpr int ExtraDaysBefore = 62, ExtraDaysAfter = 62;

class CalendarSync {
public:
    CalendarSync(const Settings& settings, fs::path cacheFile, DATA_BLOB entropy, std::wofstream& status)
        : settings(settings), cacheFile(std::move(cacheFile)), entropy(entropy), status(status),
          home(LoadCalendarCache(this->cacheFile, entropy)) {
        if (home.first) status << L"Loaded the saved cache for " << home.days.size() << L" event dates.\n";
    }

    void Tick(FlyoutState& state, HANDLE mutex, bool isOpen) {
        SYSTEMTIME local{};
        GetLocalTime(&local);
        const LONG key = DayKey(local);
        const auto now = GetTickCount64();
        const bool opened = isOpen && !wasOpen;
        wasOpen = isOpen;
        if (key != today) {
            today = key;
            const auto date = DayFromKey(today);
            homeRange = {AddDays(date, -CacheDaysBeforeToday), AddDays(date, CacheDaysAfterToday + 1)};
            refreshHome = dirty = true;
        }
        if (opened || (now >= nextRefresh && (isOpen || !homeError.empty()))) {
            refreshHome = true;
            refreshExtra = isOpen;
            failedExtra = {};
            nextRefresh = now + RefreshIntervalMs;
        }

        FinishRequest();
        if (!pending.valid()) {
            if (refreshHome) {
                refreshHome = false;
                nextRefresh = now + RefreshIntervalMs;
                StartRequest(homeRange, true);
            } else if (isOpen) {
                const LONG visible = ReadShared(state.visibleDay);
                const LONG selected = ReadShared(state.requestedDay);
                const auto outsideHome = [this](LONG day) {
                    return day && (day < DayKey(homeRange.first) || day >= DayKey(homeRange.end));
                };
                const LONG distant = outsideHome(visible) ? visible : (outsideHome(selected) ? selected : 0);
                if (distant && !failedExtra.Contains(distant) && (refreshExtra || !extra.Contains(distant))) {
                    refreshExtra = false;
                    const auto date = DayFromKey(distant);
                    StartRequest({AddDays(date, -ExtraDaysBefore), AddDays(date, ExtraDaysAfter + 1)}, false);
                }
            }
        }
        if (dirty) Publish(state, mutex);
    }

private:
    void StartRequest(CalendarRange range, bool forHome) {
        requestIsHome = forHome;
        requestedRange = range;
        pending = std::async(std::launch::async, [settings = &settings, range] {
            const ComApartment apartment;
            if (FAILED(apartment.result)) return CalendarSnapshot{{}, L"Could not initialize calendar sync."};
            return QueryCalendar(*settings, range);
        });
    }

    void FinishRequest() {
        if (!pending.valid() || pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        auto result = pending.get();
        auto& error = requestIsHome ? homeError : extraError;
        if (error != result.error) {
            dirty = true;
            error = std::move(result.error);
            if (!error.empty()) status << error << L'\n';
        }
        CachedCalendar resolved{DayKey(requestedRange.first), DayKey(requestedRange.end), std::move(result.days)};
        if (!error.empty()) {
            if (!requestIsHome) failedExtra = std::move(resolved);
            return;
        }
        auto& target = requestIsHome ? home : extra;
        const bool changed = target.first != resolved.first || target.end != resolved.end || target.days != resolved.days;
        if (changed) {
            target = std::move(resolved);
            dirty = true;
        }
        if (requestIsHome) {
            saveNeeded = (changed || saveNeeded) && !SaveCalendarCache(cacheFile, entropy, home);
            if (saveNeeded) status << L"Could not save the calendar cache.\n";
            status << L"Checked CalDAV. Cached " << home.days.size() << L" event dates.\n";
        }
    }

    void Publish(FlyoutState& state, HANDLE mutex) {
        const auto snapshot = PrepareAgenda(homeRange, home, extra, homeError, extraError);
        const AgendaLock lock(mutex, INFINITE);
        if (!lock.OwnsMutex()) return;
        state.agenda.index = snapshot.index;
        std::copy(snapshot.text.begin(), snapshot.text.end(), state.agenda.text);
        InterlockedIncrement(&state.agendaRevision);
        dirty = false;
    }

    const Settings& settings;
    const fs::path cacheFile;
    const DATA_BLOB entropy;
    std::wofstream& status;
    CachedCalendar home, extra, failedExtra;
    std::wstring homeError, extraError;
    CalendarRange homeRange{}, requestedRange{};
    std::future<CalendarSnapshot> pending;
    LONG today{};
    ULONGLONG nextRefresh{};
    bool wasOpen{}, refreshHome{}, refreshExtra{}, requestIsHome{}, dirty{true}, saveNeeded{};
};

constexpr wchar_t BuildStatus[] = L"wincal 0.4.5.";
constexpr wchar_t FlyoutModule[] = L"CalendarFlyout-045.dll";

class FlyoutAttachment {
public:
    FlyoutAttachment(decltype(&InitializeXamlDiagnosticsEx) initialize, fs::path dll)
        : initialize(initialize), dll(std::move(dll)) {}

    void Tick(HWND window, FlyoutState& state, HANDLE mapping, HANDLE agendaMutex, std::wofstream& status) {
        const auto now = GetTickCount64();
        attempt.Poll(now);
        if (window) {
            DWORD pid{};
            GetWindowThreadProcessId(window, &pid);
            if (pid != previousPid) {
                attempt.Reset();
                InterlockedExchange64(&state.attachmentOffer, 0);
                InterlockedExchange(&state.loadedPid, 0);
                previousPid = pid;
                endpoint = 1;
                nextAttempt = 0;
                lastStage = lastError = 0;
                InterlockedExchange(&state.error, 0);
                InterlockedExchange(&state.stage, 1);
            }
            if (pid && ReadShared64(state.attachmentOffer) >= 0 && !attempt.Pending() && now >= nextAttempt) {
                const auto result = attempt.Begin(initialize, pid, state, mapping, agendaMutex, dll.c_str(), endpoint, ++token, now);
                if (result != HRESULT_FROM_WIN32(ERROR_NOT_FOUND) && result != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
                    status << L"Endpoint " << endpoint << L": 0x" << std::hex << static_cast<unsigned long>(result) << std::dec << L'\n';
                endpoint = endpoint == 32 ? 1 : endpoint + 1;
                nextAttempt = now + (SUCCEEDED(result) || endpoint == 1 ? 2000 : 150);
            }
        }
        const wchar_t* stages[] = {L"", L"Stock calendar window detected.", L"Module loaded in the calendar process.",
            L"Reached the calendar window thread.", L"Found the stock CalendarControl.", L"Inserted Events into the stock calendar."};
        const LONG stage = ReadShared(state.stage);
        for (LONG index = lastStage + 1; index <= stage && index < ARRAYSIZE(stages); ++index)
            status << stages[index] << L'\n';
        lastStage = stage;
        const LONG error = ReadShared(state.error);
        if (error && error != lastError) {
            status << L"Flyout inspection error 0x" << std::hex << static_cast<unsigned long>(error) << std::dec << L'\n';
            lastError = error;
        }
    }

private:
    decltype(&InitializeXamlDiagnosticsEx) initialize;
    const fs::path dll;
    DWORD previousPid{};
    unsigned endpoint = 1;
    ULONGLONG nextAttempt{};
    LONG lastStage{}, lastError{};
    LONG64 token{};
    AttachmentAttempt attempt;
};

int Watch(const fs::path& root) {
    Handle instance(CreateMutexW(nullptr, TRUE, L"Local\\Win11CalendarFix.InFlyout"));
    if (!instance) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    const auto log = root / L"status.txt";
    std::wofstream status(log, std::ios::trunc);
    status << BuildStatus << L" Waiting for the stock calendar.\n" << std::flush;

    const auto settingsFile = root / L"settings.bin";
    Settings settings;
    std::vector<BYTE> cacheContext;
    struct Erase { Settings& settings; ~Erase() { SecureZeroMemory(&settings, sizeof(settings)); } } erase{settings};
    if (!ReadSettings(settingsFile, settings, &cacheContext)) {
        status << L"Could not read saved settings.\n";
        return 1;
    }
    const DATA_BLOB entropy{static_cast<DWORD>(cacheContext.size()), cacheContext.data()};
    CalendarSync sync(settings, root / L"events.bin", entropy, status);
    const ComApartment apartment;
    if (FAILED(apartment.result)) return 1;

    Handle agendaMutex(CreateMutexW(nullptr, FALSE, nullptr));
    Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(FlyoutState), nullptr));
    if (!agendaMutex || !mapping) return 1;
    auto view = static_cast<FlyoutState*>(MapViewOfFile(mapping.get(), FILE_MAP_WRITE, 0, 0, sizeof(FlyoutState)));
    if (!view) return 1;
    struct Unmap { FlyoutState* view; ~Unmap() { UnmapViewOfFile(view); } } unmap{view};
    GUID nonce{};
    if (FAILED(CoCreateGuid(&nonce))) return 1;
    view->protocol = FlyoutProtocol;
    static_assert(sizeof(nonce) == sizeof(view->nonce));
    memcpy(view->nonce, &nonce, sizeof(nonce));
    const auto xaml = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!xaml) {
        status << L"Windows XAML runtime is unavailable.\n";
        return 1;
    }
    const auto initialize = reinterpret_cast<decltype(&InitializeXamlDiagnosticsEx)>(GetProcAddress(xaml, "InitializeXamlDiagnosticsEx"));
    if (!initialize) return 1;

    FlyoutAttachment attachment(initialize, root / FlyoutModule);
    LONG lastSelection{};
    while (fs::exists(settingsFile)) {
        HWND window{};
        EnumDisplayMonitors(nullptr, nullptr, ProbeMonitor, reinterpret_cast<LPARAM>(&window));
        InterlockedExchange64(&view->window, reinterpret_cast<LONG64>(window));
        sync.Tick(*view, agendaMutex.get(), window != nullptr);
        attachment.Tick(window, *view, mapping.get(), agendaMutex.get(), status);
        const LONG selection = ReadShared(view->cachedSelection);
        if (selection != lastSelection) {
            status << L"Displayed " << selection << L" directly from the cached agenda.\n";
            lastSelection = selection;
        }
        if (status.tellp() >= 64 * 1024) {
            status.close();
            status.open(log, std::ios::trunc);
            status << BuildStatus << L" Recent calendar activity.\n";
        }
        status.flush();
        Sleep(200);
    }
    return 0;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        int count{};
        auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!args) return 1;
        const fs::path root = fs::path(args[0]).parent_path();
        const bool quiet = count == 3 && wcscmp(args[1], L"--check") == 0;
        const bool test = quiet || (count == 3 && wcscmp(args[1], L"--test") == 0);
        const fs::path settingsFile = test ? fs::path(args[2]) : root / L"settings.bin";
        LocalFree(args);
        if (!test) return Watch(root);

        Settings settings;
        Agenda agenda;
        if (ReadSettings(settingsFile, settings)) {
            SYSTEMTIME today{};
            GetLocalTime(&today);
            agenda = QueryAgenda(settings, today);
        } else agenda.error = L"Couldn't read the connection settings.";
        SecureZeroMemory(&settings, sizeof(settings));
        const auto success = L"Calendar connected. Events today: " + std::to_wstring(agenda.events.size());
        if (quiet && !agenda.error.empty()) fwprintf(stderr, L"%ls\n", agenda.error.c_str());
        if (!quiet) MessageBoxW(nullptr, agenda.error.empty() ? success.c_str() : agenda.error.c_str(), L"CalDAV connection", MB_OK);
        return agenda.error.empty() ? 0 : 1;
    } catch (...) { return 1; }
}
