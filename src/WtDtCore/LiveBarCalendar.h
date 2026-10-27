#pragma once
#include "LiveBarSeries.h"
#include "../WTSTools/WTSBaseDataMgr.h"
#include "../Includes/WTSContractInfo.hpp"
#include <chrono>
#include <algorithm>
#include <ctime>
#include <stdexcept>

// CTP civil times are Asia/Shanghai (UTC+8), regardless of the host timezone.
namespace LiveBarCalendar {
constexpr uint64_t Second = 1000000000ULL, Minute = 60 * Second;
inline uint64_t now()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
inline std::tm utc(time_t seconds)
{
    std::tm result{};
#ifdef _WIN32
    gmtime_s(&result, &seconds);
#else
    gmtime_r(&seconds, &result);
#endif
    return result;
}
inline uint64_t timestamp(uint32_t date, uint32_t time)
{
    std::tm value{};
    value.tm_year = date / 10000 - 1900;
    value.tm_mon = date / 100 % 100 - 1;
    value.tm_mday = date % 100;
    value.tm_hour = time / 10000000;
    value.tm_min = time / 100000 % 100;
    value.tm_sec = time / 1000 % 100;
    if (date < 19700102 || value.tm_hour > 23 || value.tm_min > 59 || value.tm_sec > 59)
        throw std::invalid_argument("invalid CTP civil time");
    const auto original = value;
#ifdef _WIN32
    auto seconds = _mkgmtime64(&value);
#else
    auto seconds = timegm(&value);
#endif
    if (seconds < 28800 || value.tm_year != original.tm_year || value.tm_mon != original.tm_mon ||
        value.tm_mday != original.tm_mday) throw std::invalid_argument("invalid CTP civil date");
    if (uint64_t(seconds - 28800) > (UINT64_MAX - (time % 1000) * 1000000ULL) / Second)
        throw std::invalid_argument("CTP civil time exceeds UTC nanosecond range");
    return (uint64_t(seconds) - 28800) * Second + (time % 1000) * 1000000ULL;
}
inline uint32_t date(uint64_t ns)
{
    const auto t = utc(ns / Second + 28800);
    return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}
inline uint32_t hhmm(uint64_t ns)
{
    const auto t = utc(ns / Second + 28800);
    return t.tm_hour * 100 + t.tm_min;
}
inline uint32_t nextDate(uint32_t day) { return date(timestamp(day, 0) + 86400 * Second); }

inline std::vector<LiveBarWindow> windows(WTSBaseDataMgr& base, wtp::WTSContractInfo& contract,
    uint32_t day, uint32_t minutes)
{
    auto comm = contract.getCommInfo();
    auto session = comm->getSessionInfo();
    if (!session || session->getOffsetMins() < 0 || !base.isTradingDate(comm->getFullPid(), day))
        throw std::invalid_argument("missing domestic futures trading calendar");
    std::vector<LiveBarWindow> sections, result;
    const uint32_t previous = base.getPrevTDate(comm->getFullPid(), day);
    // Holiday eves have no night session. Ordinary Friday-to-Monday sessions do.
    bool night = true;
    for (uint32_t d = nextDate(previous); d < day; d = nextDate(d)) {
        const auto weekday = utc(timestamp(d, 0) / Second + 28800).tm_wday;
        if (weekday != 0 && weekday != 6 && base.isHoliday(comm->getFullPid(), d)) night = false;
    }
    for (const auto& s : session->getTradingSections()) {
        uint32_t start_day = day;
        const bool evening = session->getOffsetMins() > 0 && s.first_raw > s.first;
        const bool morning = session->getOffsetMins() > 0 && s.first_raw < session->getCloseTime() && s.first_raw < 600;
        if ((evening || morning) && !night) continue;
        if (evening) start_day = previous;
        else if (morning) start_day = nextDate(previous);
        auto start = timestamp(start_day, s.first_raw * 100000);
        auto end = timestamp(start_day, (s.second_raw % 2400) * 100000);
        if (end <= start) end += 86400 * Second;
        sections.push_back({start, end, day});
    }
    if (sections.empty()) throw std::invalid_argument("empty futures trading session");
    std::sort(sections.begin(), sections.end(), [](const auto& a, const auto& b) { return a.start_ns < b.start_ns; });
    if (!minutes) return {{sections.front().start_ns, sections.back().end_ns, day}};
    if (minutes == UINT32_MAX) return sections;
    for (const auto& s : sections)
        for (uint64_t start = s.start_ns; start < s.end_ns; start += minutes * Minute)
            result.push_back({start, std::min(s.end_ns, start + minutes * Minute), day});
    return result;
}
}
