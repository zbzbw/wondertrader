#pragma once

#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../WTSTools/WTSDataFactory.h"
#include <deque>
#include <memory>
#include <map>
#include <optional>
#include <vector>
#include <array>

// Access is serialized by DataManager's shared market-data lock.
struct LiveBarWindow
{
    uint64_t start_ns = 0, end_ns = 0;
    uint32_t trading_day = 0;
};

enum LiveBarReason : uint32_t
{
    BarJoined = 1, BarDisconnected = 2, BarSubscription = 4,
    BarRejected = 8, BarCumulativeReset = 16
};

struct LiveBarCoverage
{
    bool partial = false, gap = false;
    uint32_t reasons = 0;
    uint64_t first_ns = 0, last_ns = 0;
    void add(bool is_partial, bool is_gap, uint32_t reason, uint64_t first, uint64_t last);
    LiveBarCoverage until(uint64_t through) const;
private:
    struct Observation { bool present = false, partial = false, gap = false; uint64_t first = 0, last = 0; };
    std::array<Observation, 5> _observations{};
};

struct LiveBarInterval : LiveBarWindow
{
    uint64_t sequence = 0;
    uint64_t event_ns = 0, received_ns = 0;
    std::optional<wtp::WTSBarStruct> bar;
    LiveBarCoverage coverage;
};

struct LiveBarPage
{
    uint64_t earliest = 1, next = 0, processed_through_ns = 0;
    bool has_more = false, expired = false;
    std::optional<LiveBarInterval> interval;
    LiveBarCoverage coverage;
};

class LiveBarSeries
{
public:
    LiveBarSeries(const std::string& code, wtp::WTSSessionInfo* session,
        uint32_t minutes, size_t history_count, uint64_t close_delay_ns);
    ~LiveBarSeries();
    LiveBarSeries(const LiveBarSeries&) = delete;
    LiveBarSeries& operator=(const LiveBarSeries&) = delete;

    // Calendar supplies only actual trading intervals, in UTC order. Zero minutes
    // selects a daily interval spanning that trading day's sessions.
    void setWindows(std::vector<LiveBarWindow> windows, uint64_t joined_ns);
    bool tick(wtp::WTSTickData* tick, uint64_t event_ns, uint64_t received_ns);
    void mark(bool partial, bool gap, uint32_t reason, uint64_t first_ns, uint64_t last_ns);
    // Budget counts intervals, including empty ones; never skip history silently.
    bool advance(uint64_t now_ns, size_t budget = 256);
    LiveBarPage read(uint64_t after, uint64_t through_ns) const;
    std::optional<LiveBarInterval> current() const;
    uint64_t processedThrough() const { return _processed; }
    size_t retained() const { return _history.size(); }

private:
    LiveBarInterval& pending(size_t index);
    std::string _code;
    wtp::WTSSessionInfo* _session;
    wtp::WTSKlineData* _kline;
    WTSDataFactory _factory;
    size_t _history_count, _close_index = 0;
    uint64_t _delay, _processed = 0, _sequence = 0, _joined = 0;
    std::vector<LiveBarWindow> _windows;
    std::map<size_t, LiveBarInterval> _pending;
    std::deque<LiveBarInterval> _history;
    LiveBarCoverage _status;
};
