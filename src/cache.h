#pragma once
#include "agenda.h"
#include "calendar.h"
#include <wincrypt.h>
#include <filesystem>

inline constexpr int CacheDaysBeforeToday = 90;
inline constexpr int CacheDaysAfterToday = 180;

struct CachedCalendar {
    LONG first{}, end{};
    std::map<LONG, std::vector<std::wstring>> days;

    bool Contains(LONG day) const { return first <= day && day < end; }
};

struct AgendaPublication {
    AgendaIndex index{};
    std::vector<wchar_t> text;
};

AgendaPublication PrepareAgenda(CalendarRange query, const CachedCalendar& home, const CachedCalendar& extra,
    const std::wstring& homeError, const std::wstring& extraError);
CachedCalendar LoadCalendarCache(const std::filesystem::path& path, const DATA_BLOB& entropy);
bool SaveCalendarCache(const std::filesystem::path& path, const DATA_BLOB& entropy, const CachedCalendar& calendar);
