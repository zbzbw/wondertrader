#include "TraderMocker.h"
#include "../Includes/WTSDataDef.hpp"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <stdexcept>

namespace {
void require(bool condition) { if (!condition) throw std::invalid_argument("Invalid controlled mocker request"); }
const rapidjson::Value& field(const rapidjson::Value& value, const char* key) {
    require(value.IsObject() && value.HasMember(key)); return value[key];
}
std::string text(const rapidjson::Value& value, const char* key) {
    const auto& item = field(value, key); require(item.IsString());
    return std::string(item.GetString(), item.GetStringLength());
}
uint64_t number(const rapidjson::Value& value, const char* key) {
    const auto& item = field(value, key); require(item.IsUint64()); return item.GetUint64();
}
uint32_t small(const rapidjson::Value& value, const char* key) {
    auto result = number(value, key); require(result <= UINT32_MAX); return static_cast<uint32_t>(result);
}
}

std::string TraderMocker::controlled_request(const std::string& request)
{
    controlled_owner();
    rapidjson::Document doc;
    doc.Parse(request.data(), request.size());
    require(!doc.HasParseError() && doc.IsObject());
    auto operation = text(doc, "op");
    if (operation == "restore") controlled_restore(text(doc, "state"));
    else if (operation == "settle")
        controlled_settle(small(doc, "trading_day"), PaperAccount::scaled_decimal(text(doc, "price"), 6));
    else if (operation == "begin_day") {
        const auto& rules = field(doc, "rules");
        controlled_begin_day(paper_rules([&](const char* name) { return text(rules, name); }, small(rules, "trading_day")),
                             PaperAccount::scaled_decimal(text(doc, "mark"), 6));
    } else if (operation == "step") {
        auto seq = number(doc, "sequence"), time = number(doc, "event_ms");
        const auto& source = field(doc, "tick");
        if (source.IsNull()) controlled_step(seq, time, nullptr);
        else {
            WTSTickStruct tick{};
            auto code = text(source, "code"), exchange = text(source, "exchange");
            require(code.size() < sizeof(tick.code) && exchange.size() < sizeof(tick.exchg)
                    && code.find('\0') == std::string::npos && exchange.find('\0') == std::string::npos);
            strcpy(tick.code, code.c_str()); strcpy(tick.exchg, exchange.c_str());
            tick.action_date = small(source, "action_date"); tick.action_time = small(source, "action_time");
            tick.trading_date = small(source, "trading_day");
            auto price = [&](const char* key) { return PaperAccount::scaled_decimal(text(source, key), 6) / 1000000.0; };
            tick.price = price("price"); tick.ask_prices[0] = price("ask_price"); tick.bid_prices[0] = price("bid_price");
            tick.ask_qty[0] = small(source, "ask_quantity"); tick.bid_qty[0] = small(source, "bid_quantity");
            auto input = WTSTickData::create(tick);
            try { controlled_step(seq, time, input); } catch (...) { input->release(); throw; }
            input->release();
        }
    } else require(operation == "query" || operation == "snapshot");
    auto state = controlled_snapshot();
    auto balance = _paper->balance();
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> out(buffer);
    out.StartObject(); out.Key("state"); out.String(state.c_str());
    out.Key("balance"); out.StartObject();
    auto money = [&](const char* key, int64_t value) {
        out.Key(key); auto string = PaperAccount::decimal_string(value, 2); out.String(string.c_str());
    };
    money("cash", balance.cash); money("equity", balance.equity); money("available", balance.available);
    money("margin", balance.margin); money("frozen_margin", balance.frozen_margin); money("frozen_fee", balance.frozen_fee);
    money("fees", balance.fees); money("realized", balance.realized); money("unrealized", balance.unrealized);
    money("pre_balance", balance.pre_balance); money("deposit", balance.deposit);
    money("day_fees", balance.day_fees); money("day_realized", balance.day_realized);
    out.EndObject(); out.EndObject();
    return std::string(buffer.GetString(), buffer.GetSize());
}

extern "C" {
EXPORT_FLAG uint32_t wt_mocker_live_abi() { return 1; }

// Result belongs to this thread until the next call. Exceptions never cross C ABI.
EXPORT_FLAG const char* wt_mocker_live(ITraderApi* api, const char* request)
{
    thread_local std::string result;
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> out(buffer);
    out.StartObject();
    try {
        auto mocker = dynamic_cast<TraderMocker*>(api);
        require(mocker && request);
        auto data = mocker->controlled_request(request);
        out.Key("ok"); out.Bool(true); out.Key("data"); out.RawValue(data.c_str(), data.size(), rapidjson::kObjectType);
    } catch (const std::exception& error) {
        out.Key("ok"); out.Bool(false); out.Key("error"); out.String(error.what());
    }
    out.EndObject(); result.assign(buffer.GetString(), buffer.GetSize()); return result.c_str();
}

// Use the original WT binary64 tick directly: do not round an off-grid quote
// through a textual price formatter before PaperAccount validates it.
EXPORT_FLAG const char* wt_mocker_step(ITraderApi* api, uint64_t sequence, uint64_t event_ms, const WTSTickStruct* tick, uint32_t shared_liquidity)
{
    thread_local std::string result;
    rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> out(buffer);
    out.StartObject();
    try {
        auto mocker = dynamic_cast<TraderMocker*>(api); require(mocker && shared_liquidity <= 1);
        auto release = [](WTSTickData* p) { if (p) p->release(); };
        WTSTickStruct copy; if (tick) copy = *tick;
        std::unique_ptr<WTSTickData, decltype(release)> input(tick ? WTSTickData::create(copy) : nullptr, release);
        mocker->controlled_step(sequence, event_ms, input.get(), shared_liquidity != 0);
        auto data = mocker->controlled_request("{\"op\":\"query\"}");
        out.Key("ok"); out.Bool(true); out.Key("data"); out.RawValue(data.c_str(), data.size(), rapidjson::kObjectType);
    } catch (const std::exception& error) {
        out.Key("ok"); out.Bool(false); out.Key("error"); out.String(error.what());
    }
    out.EndObject(); result.assign(buffer.GetString(), buffer.GetSize()); return result.c_str();
}
}
