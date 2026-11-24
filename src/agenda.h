#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <string>
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

    std::wstring TextForDay(LONG day, const wchar_t* text) const {
        const auto& coverage = (homeQueryFirst <= day && day < homeQueryEnd) || home.Contains(day) ? home : extra;
        const bool cached = home.Contains(day) || extra.Contains(day);
        std::wstring status;
        if (*coverage.error) {
            if (!cached) return coverage.error;
            status = L"Refresh failed: ";
            status += coverage.error;
            status += L"\nShowing saved calendar data.\n\n";
        }
        if (const auto entry = Find(day)) return status + std::wstring(text + entry->offset, entry->length);
        if (cached) return status + L"No events on this date.";
        return L"Loading calendar events…";
    }
};

struct SharedAgenda {
    AgendaIndex index;
    wchar_t text[MaxAgendaDays * MaxDayText];
};
