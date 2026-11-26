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
    if (!agenda.error.empty()) std::wcerr << L"Calendar parse error: " << agenda.error << L'\n';
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
    const auto eventResponse = [&](const std::string& properties, const std::string& title = "SUMMARY:Duration event") {
        return prefix + "<d:response><d:propstat><d:prop><c:calendar-data><![CDATA[BEGIN:VCALENDAR\nBEGIN:VEVENT\n" + properties +
            "\n" + title + "\nEND:VEVENT\nEND:VCALENDAR]]></c:calendar-data></d:prop>"
            "<d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>";
    };
    const CalendarRange durationRange{DayFromKey(20261116), DayFromKey(20261125)};
    for (const auto& stamp : {"20261116T000000", "20261117T000000", "20261117T110000", "20261117T235959",
        "20261115T235959", "20261125T000000", "20261116T000000Z", "20261117T220000Z", "20261117T235959Z"}) {
        const auto instantaneous = ParseCalendar(eventResponse(std::string("DTSTART:") + stamp + "\nDTEND:" + stamp), durationRange);
        const auto implicitEnd = ParseCalendar(eventResponse(std::string("DTSTART:") + stamp), durationRange);
        check(instantaneous.error.empty() && implicitEnd.error.empty() && instantaneous.days == implicitEnd.days,
            "An explicit zero-length timed event must occupy the same dates as an event without DTEND, including query boundaries");
    }
    const auto pointAtMidnight = ParseCalendar(eventResponse("DTSTART:20261117T000000\nDTEND:20261117T000000"), durationRange);
    check(pointAtMidnight.error.empty() && pointAtMidnight.days.size() == 1 && pointAtMidnight.days.contains(20261117),
        "A zero-length event at midnight must appear only on its start date");
    const auto pointBeforeWindow = ParseCalendar(eventResponse("DTSTART:20261115T235959\nDTEND:20261115T235959"), durationRange);
    const auto pointAfterWindow = ParseCalendar(eventResponse("DTSTART:20261125T000000\nDTEND:20261125T000000"), durationRange);
    check(pointBeforeWindow.error.empty() && pointBeforeWindow.days.empty() && pointAfterWindow.error.empty() && pointAfterWindow.days.empty(),
        "Zero-length events outside the requested window must not bleed into it");
    auto mixedResource = eventResponse("DTSTART:20261117T100000\nDTEND:20261117T110000", "SUMMARY:Normal event");
    mixedResource.insert(mixedResource.find("END:VCALENDAR"),
        "BEGIN:VEVENT\nDTSTART:20261117T000000\nDTEND:20261117T000000\nSUMMARY:Midnight point\nEND:VEVENT\n"
        "BEGIN:VEVENT\nDTSTART:20261117T120000Z\nDTEND:20261117T120000Z\nSUMMARY:UTC point\nEND:VEVENT\n");
    const auto mixedEvents = ParseCalendar(mixedResource, durationRange);
    size_t mixedCount = 0;
    for (const auto& [date, titles] : mixedEvents.days) mixedCount += titles.size();
    check(mixedEvents.error.empty() && mixedCount == 3, "Point events must not reject other events in the same calendar resource");
    auto multipleResources = eventResponse("DTSTART:20261117T100000\nDTEND:20261117T110000");
    const auto pointResource = eventResponse("DTSTART:20261117T120000Z\nDTEND:20261117T120000Z");
    multipleResources.insert(multipleResources.rfind("</d:multistatus>"),
        pointResource.substr(prefix.size(), pointResource.size() - prefix.size() - std::string("</d:multistatus>").size()));
    const auto resourceEvents = ParseCalendar(multipleResources, durationRange);
    size_t resourceCount = 0;
    for (const auto& [date, titles] : resourceEvents.days) resourceCount += titles.size();
    check(resourceEvents.error.empty() && resourceCount == 2, "An instantaneous event in one resource must not fail the entire CalDAV refresh");
    check(!ParseCalendar(eventResponse("DTSTART:20261117T120000\nDTEND:20261117T115959"), durationRange).error.empty(),
        "Timed events ending before their start must still be rejected");
    check(!ParseCalendar(eventResponse("DTSTART;VALUE=DATE:20261117\nDTEND;VALUE=DATE:20261117"), durationRange).error.empty(),
        "All-day events must retain their exclusive end-date validation");
    const auto durationDays = ParseCalendar(eventResponse("DTSTART;VALUE=DATE:20261117\nDURATION:P3D"), durationRange);
    check(durationDays.error.empty() && durationDays.days.size() == 3 &&
        durationDays.days.contains(20261117) && durationDays.days.contains(20261118) && durationDays.days.contains(20261119),
        "A three-day DURATION must mark all three occupied dates with an exclusive end");
    const auto clippedDuration = ParseCalendar(eventResponse("DURATION:+P1W\nDTSTART;VALUE=DATE:20261115"), durationRange);
    check(clippedDuration.error.empty() && clippedDuration.days.size() == 6 &&
        clippedDuration.days.contains(20261116) && clippedDuration.days.contains(20261121) && !clippedDuration.days.contains(20261122),
        "Week durations and property order should work when the start precedes the query");
    const auto midnightEnd = ParseCalendar(eventResponse("DTSTART:20261117T230000\nDURATION:PT1H"), durationRange);
    check(midnightEnd.error.empty() && midnightEnd.days.size() == 1 && midnightEnd.days.contains(20261117),
        "A timed duration ending exactly at midnight must not mark the following date");
    const auto acrossMidnight = ParseCalendar(eventResponse("DTSTART:20261117T235959\nDURATION:PT2S"), durationRange);
    check(acrossMidnight.error.empty() && acrossMidnight.days.size() == 2,
        "Seconds crossing midnight should mark both dates");
    const auto combinedDuration = ParseCalendar(eventResponse("DTSTART:20261117T230000\nDURATION:P1DT1H1M1S"), durationRange);
    check(combinedDuration.error.empty() && combinedDuration.days.size() == 3 && combinedDuration.days.contains(20261119),
        "Combined day, hour, minute and second duration components should be added");
    const auto leapDuration = ParseCalendar(eventResponse("DTSTART;VALUE=DATE:20280228\nDURATION:P2D"),
        {DayFromKey(20280228), DayFromKey(20280302)});
    check(leapDuration.error.empty() && leapDuration.days.size() == 2 && leapDuration.days.contains(20280229) &&
        !leapDuration.days.contains(20280301), "Duration arithmetic should handle leap days and month boundaries");
    for (const auto& start : {"20260308T060000Z", "20261029T200000Z", "20261101T040000Z"}) {
        std::string finish = start;
        // Six elapsed hours, across common DST transition dates. Explicit UTC
        // DTEND is the oracle, independent of the machine's current time zone.
        if (finish.substr(9, 2) == "20") { finish.replace(6, 2, "30"); finish.replace(9, 2, "02"); }
        else finish.replace(9, 2, finish.substr(9, 2) == "06" ? "12" : "10");
        const CalendarRange utcRange{DayFromKey(20260301), DayFromKey(20261201)};
        const auto duration = ParseCalendar(eventResponse(std::string("DTSTART:") + start + "\nDURATION:PT6H"), utcRange);
        const auto explicitEnd = ParseCalendar(eventResponse(std::string("DTSTART:") + start + "\nDTEND:" + finish), utcRange);
        check(duration.error.empty() && explicitEnd.error.empty() && !duration.days.empty() && duration.days == explicitEnd.days,
            "UTC durations must be applied before local conversion, including DST transitions");
    }
    for (const auto& invalid : {"", "P", "PT", "-P1D", "P0D", "PT0S", "P1Y", "P1M", "P1W1D", "P1DT", "PT1M1H",
        "PT1H1H", "PT1H1S", "P1D1D", "P999999999999999999999999999999D", "PT1.5H"})
        check(!ParseCalendar(eventResponse(std::string("DTSTART:20261117T230000\nDURATION:") + invalid), durationRange).error.empty(),
            "Malformed, non-positive and overflowing VEVENT durations should report an error");
    check(!ParseCalendar(eventResponse("DTSTART;VALUE=DATE:20261117\nDURATION:PT24H"), durationRange).error.empty(),
        "DATE events require day or week durations");
    check(!ParseCalendar(eventResponse("DTSTART:20261117T230000\nDTEND:20261118T010000\nDURATION:PT2H"), durationRange).error.empty(),
        "An event must not contain both DTEND and DURATION");
    check(!ParseCalendar(eventResponse("DTSTART;VALUE=DATE:99991231\nDURATION:P1D"), durationRange).error.empty(),
        "Duration arithmetic beyond the supported date range should report an error");
    check(!ParseCalendar(eventResponse("DTSTART;VALUE=DATE:20260230\nDURATION:P1D"), durationRange).error.empty(),
        "Invalid calendar dates should report an error");
    const auto lowerCase = ParseCalendar(eventResponse("dtstart:20261117t230000\nduration:pt1h"), durationRange);
    check(lowerCase.error.empty() && lowerCase.days == midnightEnd.days,
        "Temporal values should accept case-insensitive iCalendar tokens");
    const CalendarRange leapSecondRange{DayFromKey(20161230), DayFromKey(20170103)};
    const auto leapSecond = ParseCalendar(eventResponse("DTSTART:20161231T235960Z\nDURATION:PT1M"), leapSecondRange);
    const auto normalizedSecond = ParseCalendar(eventResponse("DTSTART:20161231T235959Z\nDURATION:PT1M"), leapSecondRange);
    check(leapSecond.error.empty() && !leapSecond.days.empty() && leapSecond.days == normalizedSecond.days,
        "UTC leap seconds must be normalized consistently before duration and local-time conversion");
    const auto leapEnd = ParseCalendar(eventResponse("DTSTART:20161231T230000Z\nDTEND:20161231T235960Z"), leapSecondRange);
    const auto normalizedEnd = ParseCalendar(eventResponse("DTSTART:20161231T230000Z\nDTEND:20161231T235959Z"), leapSecondRange);
    check(leapEnd.error.empty() && !leapEnd.days.empty() && leapEnd.days == normalizedEnd.days,
        "An explicit UTC DTEND leap second must be normalized before local conversion");
    const auto quotedTitle = ParseAgenda(eventResponse("DTSTART;VALUE=DATE:20261118",
        "SUMMARY;ALTREP=\"https://example.org/event\":Team meeting"), day);
    check(quotedTitle.error.empty() && quotedTitle.events == std::vector<std::wstring>{L"Team meeting"},
        "Colons in quoted property parameters must not be mistaken for the value separator");
    check(!ParseCalendar(eventResponse("DURATION:P3D"), durationRange).error.empty(),
        "A non-cancelled event missing DTSTART must not silently disappear");
    for (const auto& malformed : {"", "BEGIN:VCALENDAR", "BEGIN:VCALENDAR\nBEGIN:VEVENT\nDTSTART:20261117T120000\nEND:VALARM\nEND:VCALENDAR"}) {
        const auto broken = prefix + "<d:response><d:propstat><d:prop><c:calendar-data><![CDATA[" + malformed +
            "]]></c:calendar-data></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>";
        check(!ParseCalendar(broken, durationRange).error.empty(), "Empty or incomplete iCalendar data must not clear cached events");
    }
    for (const auto& code : {"403 Forbidden", "404 Not Found", "500 Internal Server Error"}) {
        const auto failed = prefix + "<d:response><d:propstat><d:prop><c:calendar-data/></d:prop><d:status>HTTP/1.1 " +
            code + "</d:status></d:propstat></d:response></d:multistatus>";
        check(!ParseCalendar(failed, durationRange).error.empty(), "Failed calendar properties must not become successful empty snapshots");
        auto partial = eventResponse("DTSTART;VALUE=DATE:20261117");
        partial.erase(partial.rfind("</d:multistatus>"));
        partial += "<d:response><d:status>HTTP/1.1 " + std::string(code) + "</d:status></d:response></d:multistatus>";
        const auto incomplete = ParseCalendar(partial, durationRange);
        check(!incomplete.error.empty() && incomplete.days.empty(), "Partial multistatus failures must not publish an incomplete agenda");
        auto failedWithData = eventResponse("DTSTART;VALUE=DATE:20261117");
        failedWithData.replace(failedWithData.find("200 OK"), 6, code);
        const auto rejected = ParseCalendar(failedWithData, durationRange);
        check(!rejected.error.empty() && rejected.days.empty(), "Calendar data with a failing property status must be rejected");
    }
    for (const auto& incomplete : {"<d:response/>", "<d:response><d:propstat/></d:response>",
        "<d:response><d:propstat><d:prop/><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"})
        check(!ParseCalendar(prefix + incomplete + "</d:multistatus>", durationRange).error.empty(),
            "A matched resource must include calendar data and its successful status");
    auto missingStatus = eventResponse("DTSTART;VALUE=DATE:20261117");
    const auto statusAt = missingStatus.find("<d:status>");
    missingStatus.erase(statusAt, missingStatus.find("</d:status>") + std::string("</d:status>").size() - statusAt);
    check(!ParseCalendar(missingStatus, durationRange).error.empty(), "Calendar data without property status must be rejected");
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
    const auto agendaCache = PrepareAgenda(query, offline, extra, L"", L"");
    const auto agendaText = [&](LONG date) { return agendaCache.index.TextForDay(date, agendaCache.text.data()); };
    check(agendaText(20261117) == L"All-day retreat\nRecurring, teammeeting",
        "A cached date should supply all its titles without a launcher round trip");
    check(agendaText(20261119) == L"No events on this date." && !agendaCache.index.Find(20261119),
        "A known empty date should be instant and should supersede a stale marker");
    check(agendaText(distantDay) == L"Distant event" && agendaCache.index.Find(distantDay),
        "Fetched dates outside the main cache should supply their titles and markers");
    check(agendaText(20990101) == L"Loading calendar events…", "Uncached dates should show loading before a fetch completes");
    const auto failedCache = PrepareAgenda(query, offline, extra, L"Sync failed", L"Outside fetch failed");
    const auto failedText = [&](LONG date) { return failedCache.index.TextForDay(date, failedCache.text.data()); };
    check(failedText(20261117).find(L"Sync failed") != std::wstring::npos &&
        failedText(20261117).find(L"Showing saved calendar data.") != std::wstring::npos &&
        failedText(20261117).find(L"All-day retreat\nRecurring, teammeeting") != std::wstring::npos,
        "Refresh errors must be visible alongside saved titles");
    check(failedText(20261119).find(L"Sync failed") != std::wstring::npos &&
        failedText(20261119).find(L"Showing saved calendar data.") != std::wstring::npos &&
        failedText(20261119).find(L"No events on this date.") != std::wstring::npos,
        "A cached empty date must not look current after a failed refresh");
    check(failedText(distantDay).find(L"Outside fetch failed") != std::wstring::npos &&
        failedText(distantDay).find(L"Distant event") != std::wstring::npos &&
        failedText(distantDay).find(L"Sync failed") == std::wstring::npos,
        "Distant cached dates should retain their titles and show the matching refresh error");
    check(failedText(20990101) == L"Outside fetch failed", "Uncached dates should report their fetch error");
    const auto homeFailure = PrepareAgenda(query, offline, extra, L"Sync failed", L"");
    check(homeFailure.index.TextForDay(distantDay, homeFailure.text.data()) == L"Distant event",
        "A home refresh failure must not mark healthy distant data as stale");
    const auto noCache = PrepareAgenda(query, {}, {}, L"Sync failed", L"Outside fetch failed");
    check(noCache.index.TextForDay(20261117, nullptr) == L"Sync failed",
        "A failed initial fetch should show the error without claiming cached coverage");
    check(agendaText(20261117).find(L"Refresh failed") == std::wstring::npos,
        "A successful refresh must clear stale warnings");
    CachedCalendar fullHome{20260101, DayKey(AddDays(DayFromKey(20260101), 271)), {}};
    for (int i = 0; i < 271; ++i)
        fullHome.days[DayKey(AddDays(DayFromKey(fullHome.first), i))] = std::vector<std::wstring>(20, std::wstring(500, L'H'));
    CachedCalendar fullExtra{DayKey(AddDays(DayFromKey(fullHome.end), -20)),
        DayKey(AddDays(DayFromKey(fullHome.end), 105)), {}};
    for (int i = 0; i < 125; ++i)
        fullExtra.days[DayKey(AddDays(DayFromKey(fullExtra.first), i))] = {L"Extra title"};
    const auto full = PrepareAgenda({DayFromKey(fullHome.first), DayFromKey(fullHome.end)}, fullHome, fullExtra, L"", L"");
    check(full.index.count == 376 && full.index.textLength == full.text.size() &&
        full.text.size() <= static_cast<size_t>(MaxAgendaDays) * MaxDayText,
        "Full home and extra windows must fit the shared publication buffer with overlap removed");
    LONG previousDay = 0;
    for (uint32_t i = 0; i < full.index.count; ++i) {
        const auto& entry = full.index.days[i];
        check(entry.day > previousDay && entry.length <= MaxDayText &&
            static_cast<size_t>(entry.offset) + entry.length <= full.text.size(),
            "Published dates must be sorted and unique, with every text slice inside the shared buffer");
        const auto value = full.index.TextForDay(entry.day, full.text.data());
        if (fullHome.Contains(entry.day))
            check(value.starts_with(L"HHHH") && value.find(L"More events are available") != std::wstring::npos,
                "Home data must supersede overlapping extra data and clearly indicate truncation");
        else check(value == L"Extra title", "Non-overlapping extra dates must retain their titles");
        previousDay = entry.day;
    }
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
