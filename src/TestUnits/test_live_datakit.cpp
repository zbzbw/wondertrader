#include "../WtDtCore/DataManager.h"
#include "../WtDtCore/ParserAdapter.h"
#include "../WtDtCore/UDPCaster.h"
#include "../WtDtCore/WtHelper.h"
#include "../WtDtCore/LiveBarCalendar.h"
#include "../WtDtCore/LiveBarWire.h"
#include "../WTSUtils/WTSCfgLoader.h"
#include "../Includes/WTSVariant.hpp"
#include "gtest/gtest/gtest.h"
#include <filesystem>
#include <fstream>

namespace {
using namespace LiveBarCalendar;
struct Base {
    WTSBaseDataMgr value;
    Base() {
        std::ofstream("live-bar-sessions.json") << R"({"day":{"name":"day","offset":0,"sections":[{"from":900,"to":1015},{"from":1030,"to":1130},{"from":1330,"to":1500}]},"night":{"name":"night","offset":300,"sections":[{"from":2100,"to":230},{"from":900,"to":1015},{"from":1030,"to":1130},{"from":1330,"to":1500}]}})";
        std::ofstream("live-bar-commodities.json") << R"({"SHFE":{"rb":{"name":"rebar","session":"day","holiday":"CHINA","pricetick":1,"volscale":10},"ag":{"name":"silver","session":"night","holiday":"CHINA","pricetick":1,"volscale":15}}})";
        std::ofstream("live-bar-contracts.json") << R"({"SHFE":{"rb2701":{"name":"rebar","exchg":"SHFE","product":"rb","opendate":20260101,"expiredate":20270115},"ag2701":{"name":"silver","exchg":"SHFE","product":"ag","opendate":20260101,"expiredate":20270115}}})";
        std::ofstream("live-bar-holidays.json") << R"({"CHINA":[20261001,20261002,20261005,20261006,20261007,20261008]})";
        EXPECT_TRUE(value.loadSessions("live-bar-sessions.json"));
        EXPECT_TRUE(value.loadCommodities("live-bar-commodities.json"));
        EXPECT_TRUE(value.loadContracts("live-bar-contracts.json"));
        EXPECT_TRUE(value.loadHolidays("live-bar-holidays.json"));
    }
};

std::string request(uint32_t kind, const std::string& json)
{
    std::string packet(4, '\0');
    for (int i = 0; i < 4; ++i) packet[i] = char((kind >> (8 * i)) & 255);
    return packet + json;
}
rapidjson::Document query(DataManager& data, uint32_t kind, const std::string& json)
{
    auto packet = request(kind, json);
    auto response = data.queryBars(packet.data(), packet.size());
    EXPECT_LE(response.size(), 1200u);
    rapidjson::Document result;
    result.Parse(response.data() + 4, response.size() - 4);
    EXPECT_FALSE(result.HasParseError());
    return result;
}
std::string stream(DataManager& data)
{
    auto result = query(data, LiveBarWire::Enumerate, R"({"request_id":"1","stream":"","index":"0"})");
    return result["stream"].GetString();
}
rapidjson::Document page(DataManager& data, const std::string& stream, const char* frequency, uint64_t after, uint64_t through)
{
    return query(data, LiveBarWire::Read, "{\"request_id\":\"2\",\"stream\":\"" + stream +
        "\",\"instrument_id\":\"SHFE.rb.2701\",\"frequency\":\"" + frequency +
        "\",\"after\":\"" + std::to_string(after) + "\",\"through_ns\":\"" + std::to_string(through) + "\"}");
}
void configure(DataManager& data, Base& base, uint64_t joined, uint64_t delay = 2000000000ULL, WTSVariant* writer = nullptr)
{
    EXPECT_TRUE(data.init(writer, &base.value, nullptr));
    const auto json = "{\"instruments\":[\"SHFE.rb.2701\"],\"frequencies\":[\"5m\",\"1h\",\"1d\"],\"history_count\":120,\"close_delay_ns\":\"" + std::to_string(delay) + "\"}";
    auto config = WTSCfgLoader::load_from_content(json.c_str(), false);
    EXPECT_TRUE(data.initBars(config, joined));
    config->release();
    CodeSet codes; codes.insert("SHFE.rb2701");
    data.parserSubscriptions("ctp", codes, joined);
    data.parserEvent("ctp", WPE_Login, 0, joined);
    data.parserSubscription("ctp", "rb2701", true, joined);
}

bool tick(DataManager& data, Base& base, uint32_t time, double total, uint64_t received)
{
    WTSTickStruct source{};
    strcpy(source.code, "rb2701"); strcpy(source.exchg, "SHFE");
    source.action_date = source.trading_date = 20261027;
    source.action_time = time; source.price = 10; source.total_volume = total;
    source.total_turnover = total * 100; source.open_interest = 123;
    auto value = WTSTickData::create(source);
    value->setContractInfo(base.value.getContract("rb2701", "SHFE"));
    const auto accepted = data.writeTick(value, 1, received);
    EXPECT_EQ(value->actiontime(), time);
    value->release();
    return accepted;
}
}

TEST(test_live_datakit, recording_filter_stop_and_restart_leave_shared_bars_running)
{
#ifdef _WIN32
    _putenv_s("BWT_DATAKIT_RUN_ID", "shared-bars-recording-test");
#else
    setenv("BWT_DATAKIT_RUN_ID", "shared-bars-recording-test", 1);
#endif
    const auto dll = std::getenv("BWT_TEST_WRITER_DLL");
    ASSERT_NE(dll, nullptr);
    const std::string previous = WtHelper::get_module_dir();
    const auto modules = std::filesystem::path(dll).parent_path().string() + "/";
    WtHelper::set_module_dir(modules.c_str());
    const auto root = std::filesystem::temp_directory_path() / ("live-bars-recording-" + std::to_string(now()));
    auto config = WTSCfgLoader::load_from_content(R"({"enabled":false,"cache":"cache.dmb","async":true,"groupsize":1000,"disablemin1":true,"disablemin5":true,"disableday":true,"disabletrans":true,"disableordque":true,"disableorddtl":true})", false);
    config->append("path", (root.string() + "/").c_str());
    Base base; DataManager data;
    const auto start = timestamp(20261027, 90000000);
    configure(data, base, start, 0, config);
    config->release();
    EXPECT_FALSE(std::filesystem::exists(root));
    ASSERT_TRUE(data.setRecordingInstruments({}));
    ASSERT_TRUE(data.startRecording());
    EXPECT_TRUE(tick(data, base, 90000000, 1000, start));
    EXPECT_TRUE(tick(data, base, 90001000, 1001, start + Second));
    bool recording, failed, exhausted; uint64_t received, accepted, persisted, segment;
    data.recordingStatus(recording, failed, received, accepted, persisted, segment, exhausted);
    EXPECT_EQ(accepted, 0u);
    DataWriterStopResult stopped;
    ASSERT_TRUE(data.stopRecording(stopped));
    ASSERT_TRUE(data.setRecordingInstruments({"SHFE.rb2701"}));
    ASSERT_TRUE(data.setRecordingLimit(1));
    ASSERT_TRUE(data.startRecording());
    EXPECT_TRUE(tick(data, base, 90500000, 1002, start + 5 * Minute));
    EXPECT_TRUE(tick(data, base, 90501000, 1003, start + 5 * Minute + Second));
    data.recordingStatus(recording, failed, received, accepted, persisted, segment, exhausted);
    EXPECT_TRUE(exhausted);
    EXPECT_EQ(accepted, 1u);
    ASSERT_TRUE(data.stopRecording(stopped));
    EXPECT_EQ(stopped.received_offset, 1u);
    EXPECT_EQ(stopped.persisted_offset, 1u);
    EXPECT_TRUE(tick(data, base, 90600000, 1004, start + 6 * Minute));
    data.advanceBars(start + 10 * Minute);
    const auto id = stream(data);
    auto bar = page(data, id, "5m", 1, start + 10 * Minute);
    EXPECT_EQ(bar["interval"]["bar"]["volume"].GetDouble(), 3);
    EXPECT_FALSE(bar["interval"]["coverage"]["gap"].GetBool());
    data.release();
    WtHelper::set_module_dir(previous.c_str());
}

TEST(test_live_datakit, shared_clock_without_writer_preserves_source_and_closes_empty_intervals)
{
    Base base; DataManager data;
    const auto start = timestamp(20261027, 90000000);
    configure(data, base, start);
    auto id = stream(data);
    ASSERT_TRUE(tick(data, base, 90000000, 1000, start));
    ASSERT_TRUE(tick(data, base, 90000000, 1002, start + Second));
    EXPECT_FALSE(tick(data, base, 90000000, 1002, start + Second));
    auto quote = data.getCurTick("rb2701", "SHFE");
    ASSERT_NE(quote, nullptr);
    EXPECT_EQ(quote->actiontime(), 90000000u);
    EXPECT_EQ(quote->volume(), 2);
    quote->release();
    ASSERT_TRUE(tick(data, base, 90500000, 1004, start + 5 * Minute));
    EXPECT_TRUE(page(data, id, "5m", 0, start + 5 * Minute)["interval"].IsNull());
    data.advanceBars(start + 5 * Minute + 2 * Second);
    auto first = page(data, id, "5m", 0, start + 5 * Minute);
    ASSERT_TRUE(first["interval"]["bar"].IsObject());
    EXPECT_EQ(first["interval"]["bar"]["volume"].GetDouble(), 2);
    EXPECT_TRUE(first["interval"]["coverage"]["partial"].GetBool());
    auto duplicate = page(data, id, "5m", 0, start + 5 * Minute);
    EXPECT_EQ(LiveBarWire::number(first["next"]), LiveBarWire::number(duplicate["next"]));
    // The clock closes the last section/day even with no subsequent Tick or strategy.
    data.advanceBars(timestamp(20261027, 150000000) + 2 * Second);
    auto daily = page(data, id, "1d", 0, timestamp(20261027, 150000000));
    ASSERT_TRUE(daily["interval"]["bar"].IsObject());
    EXPECT_EQ(daily["interval"]["bar"]["volume"].GetDouble(), 4);
    auto empty = page(data, id, "5m", 2, timestamp(20261027, 150000000));
    ASSERT_TRUE(empty["interval"].IsObject());
    EXPECT_TRUE(empty["interval"]["bar"].IsNull());
    EXPECT_FALSE(empty["interval"]["coverage"]["gap"].GetBool());
    DataWriterStopResult stopped;
    EXPECT_TRUE(data.stopRecording(stopped));
    data.advanceBars(timestamp(20261027, 160000000));
    auto rest = page(data, id, "1d", 1, timestamp(20261027, 155959000));
    EXPECT_TRUE(rest["interval"].IsNull());
    EXPECT_FALSE(rest["coverage"]["gap"].GetBool());
    EXPECT_TRUE(rest["input"][2].GetBool());
    EXPECT_GE(LiveBarWire::number(rest["processed_through_ns"]), timestamp(20261027, 155958000));
}

TEST(test_live_datakit, disconnect_empty_page_recovery_baseline_and_subscription_rejection)
{
    Base base; DataManager data;
    const auto start = timestamp(20261027, 90000000);
    configure(data, base, start - Minute, 0);
    const auto id = stream(data);
    data.parserEvent("ctp", WPE_Close, 0, start);
    data.advanceBars(start + 5 * Minute);
    auto missing = page(data, id, "5m", 0, start + 5 * Minute);
    EXPECT_TRUE(missing["interval"]["bar"].IsNull());
    EXPECT_TRUE(missing["interval"]["coverage"]["gap"].GetBool());
    auto hourly = page(data, id, "1h", 0, start + 5 * Minute);
    EXPECT_TRUE(hourly["interval"].IsNull());
    EXPECT_TRUE(hourly["coverage"]["gap"].GetBool());
    EXPECT_EQ(LiveBarWire::number(hourly["processed_through_ns"]), start + 5 * Minute);
    data.parserEvent("ctp", WPE_Login, 0, start + 5 * Minute);
    data.parserSubscription("ctp", "rb2701", false, start + 5 * Minute);
    data.advanceBars(start + 6 * Minute);
    auto rejected = page(data, id, "1h", 0, start + 6 * Minute);
    EXPECT_TRUE(rejected["coverage"]["gap"].GetBool());
    data.parserSubscription("ctp", "rb2701", true, start + 6 * Minute);
    EXPECT_TRUE(tick(data, base, 90600000, 5000, start + 6 * Minute));
    EXPECT_TRUE(tick(data, base, 90601000, 5001, start + 6 * Minute + Second));
    data.advanceBars(start + 10 * Minute);
    auto recovered = page(data, id, "5m", 1, start + 10 * Minute);
    EXPECT_EQ(recovered["interval"]["bar"]["volume"].GetDouble(), 1);
    EXPECT_TRUE(recovered["interval"]["coverage"]["partial"].GetBool());
    EXPECT_TRUE(recovered["interval"]["coverage"]["gap"].GetBool());
    EXPECT_FALSE(tick(data, base, 90600000, 5002, start + 10 * Minute));
}

TEST(test_live_datakit, session_calendar_weekend_night_holiday_and_host_independent_ns)
{
    Base base;
    auto contract = base.value.getContract("ag2701", "SHFE");
    auto monday = windows(base.value, *contract, 20261026, 0);
    EXPECT_EQ(monday.front().start_ns, timestamp(20261023, 210000000));
    EXPECT_EQ(monday.back().end_ns, timestamp(20261026, 150000000));
    auto intraday = windows(base.value, *contract, 20261026, 60);
    EXPECT_EQ(intraday[5].end_ns, timestamp(20261024, 23000000));
    EXPECT_EQ(intraday[6].start_ns, timestamp(20261026, 90000000));
    auto holiday = windows(base.value, *contract, 20261009, 0);
    EXPECT_EQ(holiday.front().start_ns, timestamp(20261009, 90000000));
    EXPECT_EQ(timestamp(20261027, 90000000), 1793062800000000000ULL);
    EXPECT_THROW(timestamp(20260230, 90000000), std::invalid_argument);
}

TEST(test_live_datakit, later_disconnect_does_not_contaminate_a_completed_healthy_horizon)
{
    Base base; DataManager data;
    const auto start = timestamp(20261027, 90000000);
    configure(data, base, start, 0);
    const auto id = stream(data);
    data.parserEvent("ctp", WPE_Close, 0, start + Minute);
    data.parserEvent("ctp", WPE_Login, 0, start + 2 * Minute);
    data.parserSubscription("ctp", "rb2701", true, start + 2 * Minute);
    data.advanceBars(start + 10 * Minute);
    auto before = page(data, id, "5m", 1, start + 10 * Minute);
    EXPECT_FALSE(before["coverage"]["gap"].GetBool());
    data.parserEvent("ctp", WPE_Close, 0, start + 12 * Minute);
    data.advanceBars(start + 13 * Minute);
    auto after = page(data, id, "5m", 1, start + 10 * Minute);
    EXPECT_FALSE(after["coverage"]["gap"].GetBool());
    EXPECT_FALSE(after["interval"]["coverage"]["gap"].GetBool());
    // Current connection diagnostics have their own observation time.
    EXPECT_FALSE(after["input"][0].GetBool());
    EXPECT_EQ(LiveBarWire::number(after["input"][3]), start + 12 * Minute);
    data.advanceBars(start + 11 * Minute);
    EXPECT_EQ(LiveBarWire::number(page(data, id, "5m", 1, start + 13 * Minute)["processed_through_ns"]), start + 13 * Minute);
}

TEST(test_live_datakit, native_parser_adapter_forwards_connection_and_subscription_results)
{
    struct Parser : IParserApi {
        IParserSpi* sink = nullptr;
        void registerSpi(IParserSpi* value) override { sink = value; }
        bool init(WTSVariant*) override { return true; }
        bool connect() override { sink->handleEvent(WPE_Connect, 0); sink->handleEvent(WPE_Login, 0); return true; }
    };
    Base base; DataManager data;
    configure(data, base, now(), 0);
    auto parser = new Parser;
    ParserAdapter adapter(&base.value, &data, nullptr);
    ASSERT_TRUE(adapter.initExt("ctp", parser));
    ASSERT_TRUE(adapter.run());
    auto id = stream(data);
    auto state = page(data, id, "5m", 0, now());
    EXPECT_TRUE(state["input"][0].GetBool());
    EXPECT_FALSE(state["input"][1].GetBool());
    parser->sink->handleSubscription("rb2701", true);
    EXPECT_TRUE(page(data, id, "5m", 0, now())["input"][1].GetBool());
    parser->sink->handleSubscription("", false);
    EXPECT_FALSE(page(data, id, "5m", 0, now())["input"][1].GetBool());
    parser->sink->handleEvent(WPE_Close, 0);
    EXPECT_FALSE(page(data, id, "5m", 0, now())["input"][0].GetBool());
    adapter.release();
}

TEST(test_live_datakit, real_udp_dispatch_and_timer_advance_without_consumers)
{
    Base base; DataManager data;
    auto joined = now() - 2 * Minute;
    configure(data, base, joined, 0);
    boost::asio::io_service io;
    using udp = boost::asio::ip::udp;
    udp::socket client(io, udp::endpoint(udp::v4(), 0));
    udp::socket reserve(io, udp::endpoint(udp::v4(), 0));
    const auto port = reserve.local_endpoint().port();
    reserve.close();
    auto config = WTSVariant::createObject();
    config->append("active", true); config->append("sport", uint32_t(port));
    UDPCaster caster;
    ASSERT_TRUE(caster.init(config, &base.value, &data));
    config->release();
    client.non_blocking(true);
    const udp::endpoint peer(boost::asio::ip::address_v4::loopback(), port);
    const auto packet = request(LiveBarWire::Enumerate, R"({"request_id":"1","stream":"","index":"0"})");
    client.send_to(boost::asio::buffer(packet), peer);
    char buffer[1201]; udp::endpoint sender;
    size_t count = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        boost::system::error_code error;
        count = client.receive_from(boost::asio::buffer(buffer), sender, 0, error);
        if (!error) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_GT(count, 4u); EXPECT_LE(count, 1200u);
    // A second independent reader receives the same page. Closing the first
    // reader has no effect on the shared timer or the remaining reader.
    udp::socket second(io, udp::endpoint(udp::v4(), 0));
    second.non_blocking(true);
    second.send_to(boost::asio::buffer(packet), peer);
    char other[1201]; size_t otherCount = 0;
    const auto secondDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < secondDeadline) {
        boost::system::error_code error;
        otherCount = second.receive_from(boost::asio::buffer(other), sender, 0, error);
        if (!error) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(std::string(buffer, count), std::string(other, otherCount));
    client.close();
    // Allow one timer period, then inspect the shared state; no owner poll runs.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    auto state = page(data, stream(data), "1h", 0, now());
    EXPECT_GT(LiveBarWire::number(state["processed_through_ns"]), joined);
    second.send_to(boost::asio::buffer(packet), peer);
    otherCount = 0;
    const auto remainingDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < remainingDeadline) {
        boost::system::error_code error;
        otherCount = second.receive_from(boost::asio::buffer(other), sender, 0, error);
        if (!error) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(std::string(buffer, count), std::string(other, otherCount));
    caster.stop();
}

TEST(test_live_datakit, all_active_series_enumerate_in_bounded_pages_and_explicit_empty_stays_empty)
{
    Base base;
    for (const auto instruments : {"\"all-active\"", "[]"}) {
        DataManager data;
        ASSERT_TRUE(data.init(nullptr, &base.value, nullptr));
        const auto json = std::string("{\"instruments\":") + instruments +
            R"(,"frequencies":["1m","5m","15m","30m","1h","2h","1d"],"history_count":120,"close_delay_ns":"2000000000"})";
        auto config = WTSCfgLoader::load_from_content(json.c_str(), false);
        ASSERT_TRUE(data.initBars(config, timestamp(20261027, 90000000)));
        config->release();
        auto first = query(data, LiveBarWire::Enumerate, R"({"request_id":"1","stream":"","index":"0"})");
        if (std::string(instruments) == "[]") {
            EXPECT_EQ(first["items"].Size(), 0u);
            EXPECT_FALSE(first["has_more"].GetBool());
        } else {
            EXPECT_EQ(first["items"].Size(), 8u);
            EXPECT_TRUE(first["has_more"].GetBool());
            auto second = query(data, LiveBarWire::Enumerate, std::string("{\"request_id\":\"2\",\"stream\":\"") +
                first["stream"].GetString() + "\",\"index\":\"8\"}");
            EXPECT_EQ(second["items"].Size(), 6u);
            EXPECT_FALSE(second["has_more"].GetBool());
        }
    }
}
