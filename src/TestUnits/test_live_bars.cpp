#include "../WtDtCore/LiveBarSeries.h"
#include "../WtDtCore/LiveTickState.h"
#include "../WtDtCore/LiveBarWire.h"
#include <fstream>
#include "gtest/gtest/gtest.h"

namespace {
constexpr uint64_t minute = 60000000000ULL;
constexpr uint64_t start = 1793062800000000000ULL; // 2026-10-27 09:00 China
struct Session {
    wtp::WTSSessionInfo* value = wtp::WTSSessionInfo::create("day", "day");
    Session() { value->addTradingSection(900, 1015); value->addTradingSection(1030, 1130); }
    ~Session() { value->release(); }
};
bool put(LiveBarSeries& bars, uint32_t at, uint64_t ns, double price = 10, double lots = 1)
{
    wtp::WTSTickStruct value{};
    strcpy(value.code, "SHFE.rb.2610");
    value.action_date = value.trading_date = 20261027;
    value.action_time = at;
    value.price = price;
    value.volume = lots;
    value.turn_over = price * lots * 10;
    value.open_interest = 123;
    auto tick = wtp::WTSTickData::create(value);
    bool accepted = bars.tick(tick, ns, ns + 500000000);
    tick->release();
    return accepted;
}
}

TEST(test_live_bars, all_frequencies_use_native_ohlc_and_volume)
{
    Session s;
    for (uint32_t minutes : {1, 5, 15, 30, 60, 120, 0}) {
        LiveBarSeries bars("SHFE.rb.2610", s.value, minutes, 2, 2000000000ULL);
        const auto end = start + (minutes == 0 ? 150 : std::min(minutes, 75u)) * minute;
        bars.setWindows({{start, end, 20261027}}, start);
        ASSERT_TRUE(put(bars, 90000000, start, 10, 2));
        ASSERT_TRUE(put(bars, 90000500, start + 500000000, 12, 3));
        ASSERT_TRUE(put(bars, 90001000, start + 1000000000, 9, 1));
        EXPECT_FALSE(bars.read(0, end).interval.has_value());
        bars.advance(end + 1999999999);
        EXPECT_FALSE(bars.read(0, end).interval.has_value());
        bars.advance(end + 2000000000);
        auto page = bars.read(0, end);
        ASSERT_TRUE(page.interval.has_value());
        ASSERT_TRUE(page.interval->bar.has_value());
        const auto& bar = *page.interval->bar;
        EXPECT_EQ(bar.open, 10);
        EXPECT_EQ(bar.high, 12);
        EXPECT_EQ(bar.low, 9);
        EXPECT_EQ(bar.close, 9);
        EXPECT_EQ(bar.vol, 6);
        EXPECT_EQ(bar.money, 650);
        EXPECT_EQ(bar.hold, 123);
        EXPECT_EQ(page.next, 1);
        EXPECT_FALSE(page.has_more);
        EXPECT_EQ(page.processed_through_ns, end);
        EXPECT_FALSE(put(bars, 90001000, start + 1000000000));
    }
}

TEST(test_live_bars, new_tick_does_not_publish_old_interval_before_delay)
{
    Session s;
    LiveBarSeries bars("SHFE.rb.2610", s.value, 5, 2, 2000000000ULL);
    bars.setWindows({{start, start + 5 * minute, 20261027},
        {start + 5 * minute, start + 10 * minute, 20261027},
        {start + 10 * minute, start + 15 * minute, 20261027}}, start);
    ASSERT_TRUE(put(bars, 90459000, start + 5 * minute - 1000000000, 10));
    ASSERT_TRUE(put(bars, 90500000, start + 5 * minute, 20));
    EXPECT_FALSE(bars.read(0, start + 5 * minute).interval.has_value());
    bars.advance(start + 5 * minute + 2000000000);
    auto first = bars.read(0, start + 5 * minute);
    ASSERT_TRUE(first.interval->bar.has_value());
    EXPECT_EQ(first.interval->bar->close, 10);
    EXPECT_EQ(bars.current()->bar->close, 20);
    bars.advance(start + 15 * minute + 2000000000);
    EXPECT_TRUE(bars.read(0, start + 15 * minute).expired);
    auto second = bars.read(1, start + 15 * minute);
    EXPECT_TRUE(second.has_more);
    EXPECT_EQ(second.interval->bar->close, 20);
    auto empty = bars.read(second.next, start + 15 * minute);
    EXPECT_FALSE(empty.interval->bar.has_value());
    EXPECT_FALSE(empty.interval->coverage.gap);
    EXPECT_EQ(bars.retained(), 2);
    EXPECT_EQ(bars.read(1, start + 15 * minute).next, second.next);
}

TEST(test_live_bars, whole_interval_disconnect_and_lower_frequency_status)
{
    Session s;
    LiveBarSeries bars("SHFE.rb.2610", s.value, 60, 2, 0);
    bars.setWindows({{start, start + 60 * minute, 20261027}}, start + minute);
    bars.mark(false, true, BarDisconnected, start + 2 * minute, start + 60 * minute);
    auto historical = bars.read(0, start + minute);
    EXPECT_TRUE(historical.coverage.partial);
    EXPECT_FALSE(historical.coverage.gap);
    bars.advance(start + 5 * minute);
    auto before = bars.read(0, start + 5 * minute);
    EXPECT_EQ(before.processed_through_ns, start + 5 * minute);
    EXPECT_TRUE(before.coverage.gap);
    EXPECT_TRUE(before.coverage.partial);
    EXPECT_FALSE(before.interval.has_value());
    bars.advance(start + 60 * minute);
    auto page = bars.read(0, start + 60 * minute);
    ASSERT_TRUE(page.interval.has_value());
    EXPECT_FALSE(page.interval->bar.has_value());
    EXPECT_TRUE(page.interval->coverage.gap);
    EXPECT_TRUE(page.interval->coverage.partial);
    // A rest period advances the watermark without manufacturing an OHLC.
    bars.advance(start + 80 * minute);
    auto rest = bars.read(1, start + 80 * minute);
    EXPECT_FALSE(rest.interval.has_value());
    EXPECT_EQ(rest.processed_through_ns, start + 80 * minute);
    EXPECT_FALSE(rest.coverage.gap);
}

TEST(test_live_bars, bounded_clock_does_not_claim_unprocessed_empty_intervals)
{
    Session s;
    LiveBarSeries bars("SHFE.rb.2610", s.value, 1, 3, 0);
    bars.setWindows({{start, start + minute, 20261027},
        {start + minute, start + 2 * minute, 20261027},
        {start + 2 * minute, start + 3 * minute, 20261027}}, start);
    EXPECT_FALSE(bars.advance(start + 4 * minute, 1));
    EXPECT_EQ(bars.processedThrough(), start + minute);
    EXPECT_TRUE(bars.advance(start + 4 * minute, 2));
    EXPECT_EQ(bars.processedThrough(), start + 4 * minute);
}

TEST(test_live_bars, cumulative_baseline_duplicates_and_reconnect_preserve_source_time)
{
    LiveTickState state;
    wtp::WTSTickStruct input{}, output{};
    input.price = 10;
    input.total_volume = 1000;
    input.total_turnover = 100000;
    input.trading_date = input.action_date = 20261027;
    input.action_time = 90000000;
    EXPECT_EQ(state.normalize(input, start, output, true), LiveTickState::Baseline);
    EXPECT_EQ(output.volume, 0);
    EXPECT_EQ(output.turn_over, 0);
    EXPECT_EQ(state.normalize(input, start, output, true), LiveTickState::Duplicate);
    input.total_volume += 2;
    input.total_turnover += 200;
    EXPECT_EQ(state.normalize(input, start, output, true), LiveTickState::Accepted);
    EXPECT_EQ(output.volume, 2);
    EXPECT_EQ(output.action_time, input.action_time);
    EXPECT_EQ(state.normalize(input, start - 1, output, true), LiveTickState::Late);
    state.disconnected();
    input.total_volume += 500;
    input.total_turnover += 50000;
    EXPECT_EQ(state.normalize(input, start + minute, output, true), LiveTickState::Baseline);
    EXPECT_EQ(output.volume, 0);
    input.total_volume = 1;
    EXPECT_EQ(state.normalize(input, start + 2 * minute, output, true), LiveTickState::CumulativeReset);
    EXPECT_EQ(state.normalize(input, start + 3 * minute, output, true), LiveTickState::Baseline);
}

TEST(test_live_bars, all_wire_pages_remain_bounded_for_ten_thousand_series)
{
    using namespace LiveBarWire;
    const std::string stream(36, 's');
    std::vector<std::pair<std::string, std::string>> series;
    for (size_t i = 0; i < 10000; ++i)
        series.emplace_back(std::string(26, 'x') + std::to_string(10000 + i), "30m");
    Request req;
    req.type = Enumerate; req.request_id = UINT64_MAX;
    size_t seen = 0;
    while (seen < series.size()) {
        req.index = seen;
        auto packet = enumerate(req, stream, series);
        EXPECT_LE(packet.size(), MaxPacket);
        rapidjson::Document response;
        response.Parse(packet.data() + 4, packet.size() - 4);
        ASSERT_FALSE(response.HasParseError());
        seen = static_cast<size_t>(number(response["next"]));
    }
    req.type = Read; req.code = series.front().first; req.frequency = "30m";
    req.after = req.through_ns = UINT64_MAX;
    LiveBarPage result;
    result.earliest = result.next = result.processed_through_ns = UINT64_MAX;
    result.observed_ns = UINT64_MAX;
    result.coverage.add(true, true, 31, UINT64_MAX, UINT64_MAX);
    LiveBarInterval row;
    row.sequence = row.start_ns = row.end_ns = row.event_ns = row.received_ns = UINT64_MAX;
    row.trading_day = 99991231;
    row.coverage = result.coverage;
    wtp::WTSBarStruct bar;
    bar.open = bar.high = bar.low = bar.close = bar.vol = bar.money = bar.hold = 1.7976931348623157e308;
    row.bar = bar;
    result.interval = row;
    EXPECT_LE(page(req, stream, result).size(), MaxPacket);
    EXPECT_LE(current(req, stream, result.interval, UINT64_MAX).size(), MaxPacket);
    result.interval.reset();
    EXPECT_LE(page(req, stream, result).size(), MaxPacket);
    EXPECT_LE(error(req, stream, "cursor_expired").size(), MaxPacket);
    EXPECT_THROW(parse("x", 1), std::invalid_argument);
}

TEST(test_live_bars, native_wire_sample_for_installed_python_reader)
{
    const char* output = std::getenv("BWT_BAR_WIRE_SAMPLE");
    if (!output) return;
    Session session;
    LiveBarSeries bars("SHFE.rb.2610", session.value, 5, 2, 2000000000ULL);
    bars.setWindows({{start, start + 5 * minute, 20261027}}, start);
    ASSERT_TRUE(put(bars, 90000000, start, 10, 2));
    bars.advance(start + 5 * minute + 2000000000ULL);
    LiveBarWire::Request req;
    req.type = LiveBarWire::Read; req.request_id = 1;
    req.code = "SHFE.rb.2610"; req.frequency = "5m";
    req.through_ns = start + 5 * minute;
    const auto wire = LiveBarWire::page(req, "sample", bars.read(0, req.through_ns));
    std::ofstream file(output, std::ios::binary);
    file.write(wire.data(), wire.size());
    ASSERT_TRUE(file.good());
}
