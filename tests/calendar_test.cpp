#include "../src/cache.h"
#include <iostream>

int main() {
    SYSTEMTIME day{};
    day.wYear = 2026;
    day.wMonth = 11;
    day.wDay = 17;
    const std::string prefix = R"(<d:multistatus xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav">)";
    const std::string response = prefix + R"(<d:response><d:propstat><d:prop><c:calendar-data><![CDATA[BEGIN:VCALENDAR
BEGIN:VEVENT
DTSTART;VALUE=DATE:20261116
DTEND;VALUE=DATE:20261118
SUMMARY:All-day retreat
END:VEVENT
BEGIN:VEVENT
DTSTART:20261117T110000
RECURRENCE-ID:20261117T110000
SUMMARY:Recurring\, team
 meeting
BEGIN:VALARM
SUMMARY:Ignored
END:VALARM
END:VEVENT
BEGIN:VEVENT
DTSTART:20261117T120000
SUMMARY:Cancelled
STATUS:CANCELLED
END:VEVENT
BEGIN:VEVENT
DTSTART;VALUE=DATE:20261118
SUMMARY:Another date
END:VEVENT
END:VCALENDAR]]></c:calendar-data></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>)";
    int failures{};
    auto check = [&](bool value, const char* message) {
        if (!value) { std::cerr << message << '\n'; ++failures; }
    };
    const auto agenda = ParseAgenda(response, day);
    check(agenda.error.empty(), "Valid DAV response should parse");
    check(agenda.events.size() == 2, "Expected two events on the selected day");
    if (agenda.events.size() == 2) {
        check(agenda.events[0] == L"All-day retreat", "Spanning all-day event should display its title only");
        check(agenda.events[1] == L"Recurring, teammeeting", "Folded recurring event should display its title only");
    }
    ++day.wDay;
    const auto tomorrow = ParseAgenda(response, day);
    check(tomorrow.events.size() == 1 && tomorrow.events[0] == L"Another date", "All-day DTEND is exclusive");
    const auto snapshot = ParseCalendar(response, {AddDays(day, -2), AddDays(day, 2)});
    check(snapshot.error.empty() && snapshot.days.size() == 3, "Only the three busy dates should be marked");
    check(snapshot.days.contains(20261116) && snapshot.days.contains(20261117) &&
        snapshot.days.contains(20261118) && !snapshot.days.contains(20261119),
        "Expanded events should be indexed by their occupied dates");
    const auto empty = ParseAgenda(prefix + "</d:multistatus>", day);
    check(empty.events.empty() && empty.error.empty(), "An empty calendar should be valid");
    check(!ParseAgenda("<html>not DAV</html>", day).error.empty(), "Unexpected XML should be rejected");
    check(!ParseAgenda("<!DOCTYPE d [<!ENTITY x SYSTEM 'file:///fake'>]>" + prefix + "</d:multistatus>", day).error.empty(),
        "XML DTDs should be rejected");
    Settings settings;
    wcscpy_s(settings.url, L"ftp://calendar.example/work/");
    check(!QueryAgenda(settings, day).error.empty(), "Unsupported URL schemes should be rejected before any connection");
    settings.demo = 1;
    check(QueryAgenda(settings, day).events.size() == 1, "Demo should work without a server");
    const auto path = std::filesystem::temp_directory_path() /
        (L"wincal-cache-test-" + std::to_wstring(GetCurrentProcessId()) + L".bin");
    BYTE binding = 1;
    DATA_BLOB entropy{1, &binding};
    const CachedCalendar cached{DayKey(AddDays(day, -CacheDaysBeforeToday)),
        DayKey(AddDays(day, CacheDaysAfterToday + 1)), snapshot.days};
    check(SaveCalendarCache(path, entropy, cached), "The calendar should be saved to disk");
    const auto offline = LoadCalendarCache(path, entropy);
    check(offline.Contains(DayKey(day)) && offline.days == snapshot.days,
        "The encrypted cache should restore titles and date coverage");
    const CalendarRange query{DayFromKey(cached.first), DayFromKey(cached.end)};
    const LONG distantDay = cached.end;
    const CachedCalendar extra{cached.first, DayKey(AddDays(DayFromKey(distantDay), 2)),
        {{20261119, {L"Stale overlap"}}, {distantDay, {L"Distant event"}}}};
    const auto agendaCache = PrepareAgenda(query, offline, extra, L"Sync failed", L"Outside fetch failed");
    const auto agendaText = [&](LONG date) { return agendaCache.index.TextForDay(date, agendaCache.text.data()); };
    check(agendaText(20261117) == L"All-day retreat\nRecurring, teammeeting",
        "A cached date should supply all its titles without a launcher round trip");
    check(agendaText(20261119) == L"No events on this date." && !agendaCache.index.Find(20261119),
        "A known empty date should be instant and should supersede a stale marker");
    check(agendaText(distantDay) == L"Distant event" && agendaCache.index.Find(distantDay),
        "Fetched dates outside the main cache should supply their titles and markers");
    check(agendaText(20990101) == L"Outside fetch failed", "Uncached dates should report their fetch error");
    binding = 2;
    check(!LoadCalendarCache(path, entropy).first, "A changed connection must not reuse the old cache");
    const CachedCalendar cleared{cached.first, cached.end, {}};
    check(SaveCalendarCache(path, entropy, cleared), "An empty calendar should replace the prior cache");
    check(LoadCalendarCache(path, entropy).Contains(DayKey(day)) && LoadCalendarCache(path, entropy).days.empty(),
        "The cache must retain the empty calendar's coverage");
    std::filesystem::remove(path);
    if (!failures) std::cout << "Calendar tests passed.\n";
    return failures != 0;
}
