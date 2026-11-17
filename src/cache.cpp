#include "cache.h"
#include <fstream>
#include <sstream>

namespace {
constexpr size_t MaxCacheBytes = 8 * 1024 * 1024;

struct ProtectedBuffer {
    DATA_BLOB blob{};
    ~ProtectedBuffer() {
        if (!blob.pbData) return;
        SecureZeroMemory(blob.pbData, blob.cbData);
        LocalFree(blob.pbData);
    }
};
}

AgendaPublication PrepareAgenda(CalendarRange query, const CachedCalendar& home, const CachedCalendar& extra,
    const std::wstring& homeError, const std::wstring& extraError) {
    AgendaPublication result;
    auto& index = result.index;
    index.home.first = home.first;
    index.home.end = home.end;
    index.extra.first = extra.first;
    index.extra.end = extra.end;
    index.homeQueryFirst = DayKey(query.first);
    index.homeQueryEnd = DayKey(query.end);
    wcsncpy_s(index.home.error, homeError.c_str(), _TRUNCATE);
    wcsncpy_s(index.extra.error, extraError.c_str(), _TRUNCATE);

    const auto append = [&](LONG day, const std::vector<std::wstring>& titles) {
        if (titles.empty() || index.count == MaxAgendaDays) return;
        constexpr std::wstring_view truncated = L"\nMore events are available in your calendar.";
        std::wstring text;
        for (const auto& title : titles) {
            if (text.size() + title.size() + 1 > MaxDayText - truncated.size()) {
                text += truncated;
                break;
            }
            if (!text.empty()) text += L'\n';
            text += title;
        }
        index.days[index.count++] = {day, static_cast<uint32_t>(result.text.size()), static_cast<uint32_t>(text.size())};
        result.text.insert(result.text.end(), text.begin(), text.end());
    };
    for (const auto& [day, titles] : home.days) append(day, titles);
    for (const auto& [day, titles] : extra.days)
        if (!home.Contains(day)) append(day, titles);
    std::sort(index.days, index.days + index.count,
        [](const AgendaEntry& a, const AgendaEntry& b) { return a.day < b.day; });
    index.textLength = static_cast<uint32_t>(result.text.size());
    return result;
}

CachedCalendar LoadCalendarCache(const std::filesystem::path& path, const DATA_BLOB& entropy) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return {};
    const auto size = input.tellg();
    if (size <= 0 || size > static_cast<std::streamoff>(MaxCacheBytes)) return {};
    std::vector<BYTE> encrypted(static_cast<size_t>(size));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(encrypted.data()), size)) return {};
    DATA_BLOB data{static_cast<DWORD>(encrypted.size()), encrypted.data()}, context = entropy;
    ProtectedBuffer clear;
    if (!CryptUnprotectData(&data, nullptr, &context, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &clear.blob) ||
        !clear.blob.cbData || clear.blob.cbData % sizeof(wchar_t)) return {};

    std::wistringstream records(std::wstring(reinterpret_cast<wchar_t*>(clear.blob.pbData),
        clear.blob.cbData / sizeof(wchar_t)));
    CachedCalendar calendar;
    std::wstring header;
    if (!std::getline(records, header)) return {};
    std::wistringstream fields(header);
    if (!(fields >> calendar.first >> calendar.end) || calendar.first >= calendar.end ||
        DayKey(AddDays(DayFromKey(calendar.first), CacheDaysBeforeToday + CacheDaysAfterToday + 1)) != calendar.end) return {};
    std::wstring line;
    while (std::getline(records, line)) {
        if (line.size() <= 9 || line.size() > 509 || line[8] != L'\t' ||
            line.find_first_not_of(L"0123456789") != 8) return {};
        const LONG day = std::stol(line.substr(0, 8));
        if (!calendar.Contains(day)) return {};
        calendar.days[day].push_back(line.substr(9));
    }
    return calendar;
}

bool SaveCalendarCache(const std::filesystem::path& path, const DATA_BLOB& entropy, const CachedCalendar& calendar) {
    std::wstring records = std::to_wstring(calendar.first) + L" " + std::to_wstring(calendar.end) + L"\n";
    for (const auto& [day, titles] : calendar.days) {
        for (const auto& title : titles) {
            records += std::to_wstring(day) + L"\t" + title + L"\n";
            if (records.size() * sizeof(wchar_t) > MaxCacheBytes / 2) return false;
        }
    }
    DATA_BLOB clear{static_cast<DWORD>(records.size() * sizeof(wchar_t)), reinterpret_cast<BYTE*>(records.data())};
    DATA_BLOB context = entropy;
    ProtectedBuffer encrypted;
    const bool protectedData =
        CryptProtectData(&clear, nullptr, &context, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &encrypted.blob);
    SecureZeroMemory(clear.pbData, clear.cbData);
    if (!protectedData) return false;

    auto staging = path;
    staging += L".tmp";
    {
        std::ofstream output(staging, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(encrypted.blob.pbData), encrypted.blob.cbData);
        output.close();
        if (!output) {
            DeleteFileW(staging.c_str());
            return false;
        }
    }
    if (MoveFileExW(staging.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileW(staging.c_str());
    return false;
}
