#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../WTSTools/WTSDataFactory.h"
#include "gtest/gtest/gtest.h"

USING_NS_WTP;

TEST(test_kline_session, opening_tick_uses_next_section)
{
    auto session = WTSSessionInfo::create("night", "night", 180);
    session->addTradingSection(2100, 100);
    session->addTradingSection(900, 1015);
    session->addTradingSection(1030, 1130);
    session->addTradingSection(1330, 1500);
    WTSDataFactory factory;
    for (auto period : {KP_Minute1, KP_Minute5}) {
        const uint32_t minutes = period == KP_Minute1 ? 1 : 5;
        auto bars = WTSKlineData::create("SHFE.rb.2609", 0);
        bars->setPeriod(period, 1);
        struct Sample { uint32_t time; uint32_t close; };
        // End ticks remain in the last bar; break ticks produce no bar.
        // Each opening minute shares its cumulative index with the prior end.
        for (auto sample : {
            Sample{100, 100}, Sample{859, 100}, Sample{900, 900 + minutes},
            Sample{1015, 1015}, Sample{1029, 1015}, Sample{1030, 1030 + minutes},
            Sample{1130, 1130}, Sample{1329, 1130}, Sample{1330, 1330 + minutes},
            Sample{1500, 1500}}) {
            WTSTickStruct tick{};
            strcpy(tick.code, "SHFE.rb.2609");
            tick.action_date = tick.trading_date = 20260909;
            tick.action_time = sample.time * 100000 + 500;
            tick.price = 3500;
            tick.volume = 1;
            auto input = WTSTickData::create(tick);
            factory.updateKlineData(bars, input, session, true);
            ASSERT_GT(bars->size(), 0);
            EXPECT_EQ(bars->at(-1)->time, 3609090000ULL + sample.close);
            input->release();
        }
        EXPECT_EQ(bars->size(), 7);
        bars->release();
    }
    session->release();
}
