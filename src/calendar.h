#pragma once
#include <windows.h>
#include <map>
#include <string>
#include <vector>

struct Settings {
    wchar_t url[2048]{};
    wchar_t username[256]{};
    wchar_t password[512]{};
    DWORD demo{};
};
static_assert(sizeof(Settings) == 5636);

struct Agenda {
    std::vector<std::wstring> events;
    std::wstring error;
};

struct CalendarRange {
    SYSTEMTIME first, end;
};

struct CalendarSnapshot {
    std::map<LONG, std::vector<std::wstring>> days;
    std::wstring error;
};

LONG DayKey(SYSTEMTIME day);
SYSTEMTIME DayFromKey(LONG key);
SYSTEMTIME AddDays(SYSTEMTIME day, int count);
CalendarSnapshot QueryCalendar(const Settings& settings, CalendarRange range);
CalendarSnapshot ParseCalendar(const std::string& response, CalendarRange range);
Agenda QueryAgenda(const Settings& settings, SYSTEMTIME day);
Agenda ParseAgenda(const std::string& response, SYSTEMTIME day);
