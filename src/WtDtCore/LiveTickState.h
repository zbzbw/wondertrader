#pragma once
#include "../Includes/WTSStruct.h"
#include <cmath>
#include <cstring>

// One state per physical instrument, shared by every configured frequency.
// This class never modifies source action_date/action_time.
class LiveTickState
{
public:
    enum Result { Accepted, Baseline, Duplicate, Late, Invalid, CumulativeReset };

    Result normalize(const wtp::WTSTickStruct& source, uint64_t event_ns,
        wtp::WTSTickStruct& output, bool cumulative)
    {
        if (!event_ns || !std::isfinite(source.price) || source.price <= 0 ||
            !std::isfinite(source.total_volume) || source.total_volume < 0 ||
            !std::isfinite(source.total_turnover) || source.total_turnover < 0 ||
            !std::isfinite(source.open_interest)) return Invalid;
        if (_seen && event_ns < _event_ns) return Late;
        if (_baseline && event_ns == _event_ns &&
            std::memcmp(&_last, &source, sizeof(source)) == 0) return Duplicate;
        output = source;
        const bool first = !_baseline || source.trading_date != _last.trading_date;
        if (cumulative) {
            if (!first && (source.total_volume < _last.total_volume ||
                source.total_turnover < _last.total_turnover)) {
                _baseline = false;
                return CumulativeReset;
            }
            output.volume = first ? 0 : source.total_volume - _last.total_volume;
            output.turn_over = first ? 0 : source.total_turnover - _last.total_turnover;
            output.diff_interest = first ? 0 : source.open_interest - _last.open_interest;
        }
        if (!std::isfinite(output.volume) || output.volume < 0 ||
            output.volume != std::floor(output.volume) ||
            !std::isfinite(output.turn_over) || output.turn_over < 0) return Invalid;
        _last = source;
        _event_ns = event_ns;
        _seen = _baseline = true;
        return first && cumulative ? Baseline : Accepted;
    }

    void disconnected() { _baseline = false; }

private:
    wtp::WTSTickStruct _last{};
    uint64_t _event_ns = 0;
    bool _seen = false, _baseline = false;
};
