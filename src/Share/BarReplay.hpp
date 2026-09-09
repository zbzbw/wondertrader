#pragma once
#include "../Includes/WTSStruct.h"
#include <cmath>
#include <stdexcept>

// HisDataReplayer's established open/high/low/close order. Live paper shares
// this selector; this is a bar simulation, never a reconstruction of real ticks.
inline double replayBarPrice(const wtp::WTSBarStruct& bar, unsigned phase) {
    switch (phase) {
    case 0: return bar.open;
    case 1: return bar.high;
    case 2: return bar.low;
    case 3: return bar.close;
    default: throw std::invalid_argument("Invalid OHLC replay phase");
    }
}

inline uint32_t replayBarLiquidity(const wtp::WTSBarStruct& bar, unsigned phase) {
    if (phase > 3 || !std::isfinite(bar.vol) || bar.vol < 0 || bar.vol > UINT32_MAX || std::floor(bar.vol) != bar.vol)
        throw std::invalid_argument("Bar simulation requires whole-lot volume");
    auto volume = static_cast<uint32_t>(bar.vol);
    return phase == 3 ? volume - 3 * (volume / 4) : volume / 4;
}
