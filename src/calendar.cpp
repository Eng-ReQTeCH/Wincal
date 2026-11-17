#include "calendar.h"
#include <unknwn.h>
#include <objidl.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <xmllite.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <memory>
#include <string_view>

using Microsoft::WRL::ComPtr;
using unique_http = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;

namespace {
constexpr size_t MaxResponse = 4 * 1024 * 1024;
constexpr size_t MaxIndexedTitleUnits = 1024 * 1024;

std::wstring DatePart(const SYSTEMTIME& value) {
    wchar_t output[16];
    swprintf_s(output, L"%04u%02u%02u", value.wYear, value.wMonth, value.wDay);
    return output;
}

std::wstring UtcStamp(const SYSTEMTIME& local, bool includeMidnight = false) {
    SYSTEMTIME utc{};
    if (!TzSpecificLocalTimeToSystemTimeEx(nullptr, &local, &utc)) return {};
    if (includeMidnight) {
        FILETIME stamp{};
        if (!SystemTimeToFileTime(&utc, &stamp)) return {};
        ULARGE_INTEGER ticks{};
        ticks.LowPart = stamp.dwLowDateTime;
        ticks.HighPart = stamp.dwHighDateTime;
        if (ticks.QuadPart < 10000000ULL) return {};
        ticks.QuadPart -= 10000000ULL;
        stamp = {ticks.LowPart, ticks.HighPart};
        if (!FileTimeToSystemTime(&stamp, &utc)) return {};
    }
    wchar_t time[16];
    swprintf_s(time, L"T%02u%02u%02uZ", utc.wHour, utc.wMinute, utc.wSecond);
    return DatePart(utc) + time;
}

std::wstring LocalStamp(std::wstring value) {
    if (value.size() != 16 || value.back() != L'Z') return value;
    SYSTEMTIME utc{}, local{};
    if (swscanf_s(value.c_str(), L"%4hu%2hu%2huT%2hu%2hu%2huZ", &utc.wYear, &utc.wMonth,
        &utc.wDay, &utc.wHour, &utc.wMinute, &utc.wSecond) != 6 ||
        !SystemTimeToTzSpecificLocalTimeEx(nullptr, &utc, &local)) return {};
    wchar_t time[16];
    swprintf_s(time, L"T%02u%02u%02u", local.wHour, local.wMinute, local.wSecond);
    return DatePart(local) + time;
}

std::wstring Unescape(std::wstring_view value) {
    std::wstring output;
    for (size_t index = 0; index < value.size() && output.size() < 500; ++index) {
        wchar_t character = value[index];
        if (character == L'\\' && index + 1 < value.size()) {
            character = value[++index];
            if (character == L'n' || character == L'N') character = L' ';
        }
        output += character < L' ' ? L' ' : character;
    }
    return output;
}

struct Event {
    std::wstring start, end, title;
    bool cancelled{};
};

void ReadEvents(std::wstring_view ics, std::vector<Event>& events) {
    std::vector<std::wstring> lines;
    while (!ics.empty()) {
        const auto newline = ics.find(L'\n');
        auto line = ics.substr(0, newline);
        ics.remove_prefix(newline == ics.npos ? ics.size() : newline + 1);
        if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
        if (!lines.empty() && !line.empty() && (line.front() == L' ' || line.front() == L'\t')) {
            lines.back().append(line.substr(1));
        } else {
            lines.emplace_back(line);
        }
    }
    Event event;
    unsigned level = 0;
    for (const auto& line : lines) {
        const auto colon = line.find(L':');
        if (colon == line.npos) continue;
        auto name = line.substr(0, line.find_first_of(L";:"));
        CharUpperBuffW(name.data(), static_cast<DWORD>(name.size()));
        const auto value = line.substr(colon + 1);
        if (name == L"BEGIN" && _wcsicmp(value.c_str(), L"VEVENT") == 0 && !level) {
            event = {};
            level = 1;
        } else if (level && name == L"BEGIN") {
            ++level;
        } else if (level && name == L"END") {
            if (!--level && _wcsicmp(value.c_str(), L"VEVENT") == 0 && !event.start.empty() && !event.cancelled)
                events.push_back(std::move(event));
        } else if (level == 1) {
            if (name == L"DTSTART") event.start = LocalStamp(value);
            if (name == L"DTEND") event.end = LocalStamp(value);
            if (name == L"SUMMARY") event.title = Unescape(value);
            if (name == L"STATUS") event.cancelled = _wcsicmp(value.c_str(), L"CANCELLED") == 0;
        }
    }
}

std::wstring NetworkError() {
    return L"Couldn't reach the CalDAV server (Windows error " + std::to_wstring(GetLastError()) + L").";
}

bool OpenXml(const std::string& response, ComPtr<IXmlReader>& xml) {
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(response.data()), static_cast<UINT>(response.size())));
    return stream && SUCCEEDED(CreateXmlReader(IID_PPV_ARGS(&xml), nullptr)) &&
        SUCCEEDED(xml->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) &&
        SUCCEEDED(xml->SetInput(stream.Get()));
}

bool IsCalendarCollection(const std::string& response) {
    ComPtr<IXmlReader> xml;
    if (!OpenXml(response, xml)) return false;
    XmlNodeType node{};
    UINT resourceDepth{};
    bool inResourceType = false, calendar = false;
    HRESULT status;
    while ((status = xml->Read(&node)) == S_OK) {
        if (node != XmlNodeType_Element && node != XmlNodeType_EndElement) continue;
        const wchar_t *name{}, *ns{};
        UINT depth{};
        if (FAILED(xml->GetLocalName(&name, nullptr)) || FAILED(xml->GetNamespaceUri(&ns, nullptr)) ||
            FAILED(xml->GetDepth(&depth))) return false;
        if (inResourceType && depth <= resourceDepth) inResourceType = false;
        if (node != XmlNodeType_Element) continue;
        if (std::wstring_view(ns) == L"DAV:" && std::wstring_view(name) == L"resourcetype" && !xml->IsEmptyElement()) {
            inResourceType = true;
            resourceDepth = depth;
        } else if (inResourceType && depth == resourceDepth + 1 && std::wstring_view(ns) == L"urn:ietf:params:xml:ns:caldav" &&
            std::wstring_view(name) == L"calendar") {
            calendar = true;
        }
    }
    return status == S_FALSE && calendar;
}

std::wstring SendDav(HINTERNET server, DWORD flags, const std::wstring& path, LPCWSTR method,
    const std::wstring& headers, std::string& body, std::string& response) {
    unique_http request(WinHttpOpenRequest(server, method, path.c_str(), nullptr, nullptr, nullptr, flags), WinHttpCloseHandle);
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    if (!request || !WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled))) return NetworkError();
    if (!WinHttpSendRequest(request.get(), headers.c_str(), DWORD(-1), body.data(), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) return NetworkError();
    DWORD status{}, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr)) return NetworkError();
    if (status == 401 || status == 403) return L"The CalDAV server denied access. Check your login and calendar permissions.";
    if (status != 207) return L"The CalDAV server returned HTTP " + std::to_wstring(status) + L". Check the calendar collection URL.";
    response.clear();
    char buffer[8192];
    DWORD read{};
    do {
        if (!WinHttpReadData(request.get(), buffer, sizeof(buffer), &read)) return NetworkError();
        if (response.size() + read > MaxResponse) return L"The calendar response was too large.";
        response.append(buffer, read);
    } while (read);
    return {};
}
}

LONG DayKey(SYSTEMTIME day) {
    return day.wYear * 10000L + day.wMonth * 100L + day.wDay;
}

SYSTEMTIME DayFromKey(LONG key) {
    SYSTEMTIME day{};
    day.wYear = static_cast<WORD>(key / 10000);
    day.wMonth = static_cast<WORD>(key / 100 % 100);
    day.wDay = static_cast<WORD>(key % 100);
    return day;
}

SYSTEMTIME AddDays(SYSTEMTIME day, int count) {
    FILETIME file{};
    if (!SystemTimeToFileTime(&day, &file)) return {};
    ULARGE_INTEGER ticks{};
    ticks.LowPart = file.dwLowDateTime;
    ticks.HighPart = file.dwHighDateTime;
    const auto shifted = static_cast<LONGLONG>(ticks.QuadPart) + 864000000000LL * count;
    if (shifted < 0) return {};
    ticks.QuadPart = shifted;
    file = {ticks.LowPart, ticks.HighPart};
    SYSTEMTIME result{};
    FileTimeToSystemTime(&file, &result);
    return result;
}

CalendarSnapshot ParseCalendar(const std::string& response, CalendarRange range) {
    const LONG firstDay = DayKey(range.first), endDay = DayKey(range.end);
    if (!firstDay || firstDay >= endDay) return {{}, L"Invalid calendar date range."};
    if (response.empty() || response.size() > MaxResponse) return {{}, L"The CalDAV response was empty or too large."};
    ComPtr<IXmlReader> xml;
    if (!OpenXml(response, xml)) return {{}, L"Couldn't read the CalDAV response."};

    std::vector<Event> events;
    std::wstring calendarData;
    bool multistatus = false;
    bool inCalendar = false;
    XmlNodeType node{};
    HRESULT status;
    while ((status = xml->Read(&node)) == S_OK) {
        if (node == XmlNodeType_Element || node == XmlNodeType_EndElement) {
            const wchar_t *name{}, *ns{};
            xml->GetLocalName(&name, nullptr);
            xml->GetNamespaceUri(&ns, nullptr);
            if (!name || !ns) continue;
            if (node == XmlNodeType_Element && !multistatus) {
                if (std::wstring_view(ns) != L"DAV:" || std::wstring_view(name) != L"multistatus")
                    return {{}, L"The server returned an invalid CalDAV response."};
                multistatus = true;
            }
            if (std::wstring_view(ns) == L"urn:ietf:params:xml:ns:caldav" && std::wstring_view(name) == L"calendar-data") {
                if (node == XmlNodeType_Element) {
                    calendarData.clear();
                    inCalendar = !xml->IsEmptyElement();
                } else {
                    ReadEvents(calendarData, events);
                    inCalendar = false;
                }
            }
        } else if (inCalendar && (node == XmlNodeType_Text || node == XmlNodeType_CDATA || node == XmlNodeType_Whitespace)) {
            const wchar_t* value{};
            UINT length{};
            if (SUCCEEDED(xml->GetValue(&value, &length))) calendarData.append(value, length);
        }
    }
    if (FAILED(status) || !multistatus) return {{}, L"The server returned an invalid CalDAV response."};

    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.start < b.start; });
    const auto first = DatePart(range.first);
    CalendarSnapshot result;
    size_t titleUnits = 0;
    for (const auto& event : events) {
        const auto stamp = event.start.substr(0, 8);
        if (stamp.size() != 8 || stamp.find_first_not_of(L"0123456789") != stamp.npos) continue;
        auto day = stamp < first ? range.first : DayFromKey(std::stol(stamp));
        if (!DayKey(AddDays(day, 0))) continue;
        const auto title = event.title.empty() ? L"Untitled event" : event.title;
        for (LONG key = DayKey(day); key && key < endDay; day = AddDays(day, 1), key = DayKey(day)) {
            const auto date = DatePart(day);
            if (date != stamp && event.end <= (event.start.size() == 8 ? date : date + L"T000000")) break;
            titleUnits += title.size() + 10;
            if (titleUnits > MaxIndexedTitleUnits) return {{}, L"The calendar contains too many events to cache."};
            result.days[key].push_back(title);
        }
    }
    return result;
}

CalendarSnapshot QueryCalendar(const Settings& settings, CalendarRange range) {
    if (settings.demo) {
        SYSTEMTIME today{};
        GetLocalTime(&today);
        CalendarSnapshot result;
        const LONG key = DayKey(today);
        if (key >= DayKey(range.first) && key < DayKey(range.end)) result.days[key] = {L"Sample calendar event"};
        return result;
    }

    URL_COMPONENTS url{sizeof(url)};
    url.dwHostNameLength = url.dwUrlPathLength = url.dwExtraInfoLength = url.dwUserNameLength = url.dwPasswordLength = DWORD(-1);
    if (!WinHttpCrackUrl(settings.url, 0, 0, &url) ||
        (url.nScheme != INTERNET_SCHEME_HTTP && url.nScheme != INTERNET_SCHEME_HTTPS) ||
        !url.dwHostNameLength || !url.dwUrlPathLength || url.dwExtraInfoLength || url.dwUserNameLength || url.dwPasswordLength) {
        return {{}, L"Enter an HTTP or HTTPS calendar URL without a query, fragment, or credentials."};
    }
    const std::wstring host(url.lpszHostName, url.dwHostNameLength);
    std::wstring path(url.lpszUrlPath, url.dwUrlPathLength);
    if (path.back() != L'/') path += L'/';
    unique_http session(WinHttpOpen(L"wincal", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0), WinHttpCloseHandle);
    if (!session || !WinHttpSetTimeouts(session.get(), 5000, 5000, 5000, 7000)) return {{}, NetworkError()};
    unique_http server(WinHttpConnect(session.get(), host.c_str(), url.nPort, 0), WinHttpCloseHandle);
    if (!server) return {{}, NetworkError()};
    const DWORD flags = url.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;

    // Include zero-duration events exactly at midnight. Results are still
    // filtered to the requested local dates.
    const auto from = UtcStamp(range.first, true);
    const auto to = UtcStamp(range.end);
    if (from.empty() || to.empty()) return {{}, L"Couldn't calculate the calendar date range."};
    const std::wstring timeRange = L" start=\"" + from + L"\" end=\"" + to + L"\"";
    const auto xml = L"<C:calendar-query xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\">"
        L"<D:prop><C:calendar-data><C:expand" + timeRange + L"/></C:calendar-data></D:prop>"
        L"<C:filter><C:comp-filter name=\"VCALENDAR\"><C:comp-filter name=\"VEVENT\"><C:time-range" + timeRange +
        L"/></C:comp-filter></C:comp-filter></C:filter></C:calendar-query>";
    std::string body;
    body.reserve(xml.size());
    for (const auto character : xml) body.push_back(static_cast<char>(character));
    std::wstring headers = L"Content-Type: application/xml; charset=utf-8\r\nDepth: ";
    const size_t depth = headers.size();
    headers += L"1\r\n";
    struct ClearHeaders {
        std::wstring& value;
        ~ClearHeaders() { SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t)); }
    } clearHeaders{headers};
    if (settings.username[0]) {
        std::wstring login = std::wstring(settings.username) + L":" + settings.password;
        const auto count = WideCharToMultiByte(CP_UTF8, 0, login.data(), static_cast<int>(login.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(count, '\0');
        WideCharToMultiByte(CP_UTF8, 0, login.data(), static_cast<int>(login.size()), utf8.data(), count, nullptr, nullptr);
        SecureZeroMemory(login.data(), login.size() * sizeof(wchar_t));
        DWORD encodedCount = 0;
        CryptBinaryToStringW(reinterpret_cast<const BYTE*>(utf8.data()), count, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &encodedCount);
        std::wstring encoded(encodedCount, L'\0');
        const bool converted = CryptBinaryToStringW(reinterpret_cast<const BYTE*>(utf8.data()), count, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, encoded.data(), &encodedCount);
        SecureZeroMemory(utf8.data(), utf8.size());
        if (!converted) return {{}, L"Couldn't encode the CalDAV credentials."};
        headers.append(L"Authorization: Basic ").append(encoded.data(), encodedCount).append(L"\r\n");
        SecureZeroMemory(encoded.data(), encoded.size() * sizeof(wchar_t));
    }
    std::string response;
    auto error = SendDav(server.get(), flags, path, L"REPORT", headers, body, response);
    if (!error.empty()) return {{}, std::move(error)};
    auto calendar = ParseCalendar(response, range);
    if (!calendar.days.empty() || !calendar.error.empty()) return calendar;

    headers[depth] = L'0';
    body = R"(<D:propfind xmlns:D="DAV:"><D:prop><D:resourcetype/></D:prop></D:propfind>)";
    error = SendDav(server.get(), flags, path, L"PROPFIND", headers, body, response);
    if (!error.empty()) return {{}, std::move(error)};
    if (!IsCalendarCollection(response)) return {{}, L"This URL does not identify a CalDAV calendar. Use the full calendar collection URL from your provider."};
    return calendar;
}

Agenda ParseAgenda(const std::string& response, SYSTEMTIME day) {
    day = DayFromKey(DayKey(day));
    auto calendar = ParseCalendar(response, {day, AddDays(day, 1)});
    return {std::move(calendar.days[DayKey(day)]), std::move(calendar.error)};
}

Agenda QueryAgenda(const Settings& settings, SYSTEMTIME day) {
    day = DayFromKey(DayKey(day));
    if (settings.demo) return {{L"Sample calendar event"}, {}};
    auto calendar = QueryCalendar(settings, {day, AddDays(day, 1)});
    return {std::move(calendar.days[DayKey(day)]), std::move(calendar.error)};
}
