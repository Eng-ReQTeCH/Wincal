#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <string_view>

// Shared by the launcher and the flyout DLL. Every field is a value or an
// offset into the text buffer so it is valid in either process.
inline constexpr int MaxAgendaDays = 512;
inline constexpr int MaxDayText = 4096;

struct AgendaCoverage {
    LONG first, end;
    wchar_t error[256];

    bool Contains(LONG day) const { return first && first <= day && day < end; }
};

struct AgendaEntry {
    LONG day;
    uint32_t offset, length;
};

struct AgendaIndex {
    AgendaCoverage home, extra;
    LONG homeQueryFirst, homeQueryEnd;
    uint32_t count, textLength;
    AgendaEntry days[MaxAgendaDays];

    const AgendaEntry* Find(LONG day) const {
        const auto end = days + count;
        const auto entry = std::lower_bound(days, end, day,
            [](const AgendaEntry& current, LONG key) { return current.day < key; });
        return entry != end && entry->day == day ? entry : nullptr;
    }

    std::wstring_view TextForDay(LONG day, const wchar_t* text) const {
        if (const auto entry = Find(day)) return {text + entry->offset, entry->length};
        if (home.Contains(day) || extra.Contains(day)) return L"No events on this date.";
        const auto& coverage = homeQueryFirst <= day && day < homeQueryEnd ? home : extra;
        if (*coverage.error) return coverage.error;
        return L"Loading calendar events…";
    }
};

struct SharedAgenda {
    AgendaIndex index;
    wchar_t text[MaxAgendaDays * MaxDayText];
};
