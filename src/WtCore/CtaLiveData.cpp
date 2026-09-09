#include "WtDtMgr.h"
#include "WtEngine.h"
#include "../Includes/WTSDataDef.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

USING_NS_WTP;
namespace {
using Value = rapidjson::Value;
using Writer = rapidjson::Writer<rapidjson::StringBuffer>;
const Value& field(const Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key)) throw std::invalid_argument("Missing controlled market checkpoint field");
    return v[key];
}
double readNumber(const Value& v) {
    if (!v.IsNumber() || !std::isfinite(v.GetDouble())) throw std::invalid_argument("Invalid controlled market number");
    return v.GetDouble();
}
uint64_t readInteger(const Value& v) {
    if (!v.IsUint64()) throw std::invalid_argument("Invalid controlled market integer"); return v.GetUint64();
}
uint32_t readUint(const Value& v) {
    if (!v.IsUint()) throw std::invalid_argument("Invalid controlled market uint32"); return v.GetUint();
}
std::string readText(const Value& v) {
    if (!v.IsString() || std::strlen(v.GetString()) != v.GetStringLength()) throw std::invalid_argument("Invalid controlled market text");
    return v.GetString();
}
template<size_t N> void readText(const Value& v, char (&target)[N]) {
    auto text = readText(v); if (text.size() >= N) throw std::invalid_argument("Controlled market text exceeds native capacity");
    std::memcpy(target, text.c_str(), text.size() + 1);
}
template<class Map, class F> void writeMap(Writer& w, Map* map, F write) {
    std::vector<std::string> keys;
    if (map) for (const auto& entry : *map) keys.push_back(entry.first);
    std::sort(keys.begin(), keys.end()); w.StartObject();
    for (const auto& key : keys) { w.Key(key.c_str()); write(map->get(key)); } w.EndObject();
}
void validateBar(const WTSBarStruct& b) {
    for (double value : {b.open, b.high, b.low, b.close, b.settle, b.money, b.vol, b.hold, b.add})
        if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite controlled bar field");
    if (!b.date || !b.time || b.low <= 0 || b.open < b.low || b.open > b.high || b.close < b.low || b.close > b.high
        || b.vol < 0 || std::floor(b.vol) != b.vol)
        throw std::invalid_argument("Invalid completed controlled bar");
}
}

void WtDtMgr::controlledBar(const char* stdCode, const WTSBarStruct& bar, bool notify) {
    if (!_engine->controlled()) throw std::logic_error("Explicit bars require controlled CTA");
    if (!stdCode || !*stdCode || std::strlen(stdCode) >= 32) throw std::invalid_argument("Invalid native bar code");
    validateBar(bar);
    if (!_bars_cache) _bars_cache = DataCacheMap::create();
    auto data = static_cast<WTSKlineData*>(_bars_cache->get(stdCode));
    if (data && bar.time <= data->at(0)->time) throw std::invalid_argument("Controlled bar cursor did not advance");
    if (!data) {
        data = WTSKlineData::create(stdCode, 1); data->setPeriod(KP_Minute5, 1);
        _bars_cache->add(stdCode, data, false);
    }
    *data->at(0) = bar;
    if (notify) _engine->on_bar(stdCode, "m", 5, data->at(0));
}

std::string WtDtMgr::liveSnapshot() const {
    if (!_engine->controlled() || !_bar_notifies.empty()) throw std::logic_error("Market snapshot requires a controlled event barrier");
    rapidjson::StringBuffer buffer; Writer w(buffer);
    w.StartObject(); w.Key("version"); w.Uint(1);
    w.Key("bars"); writeMap(w, _bars_cache, [&](WTSObject* object) {
        const auto& b = *static_cast<WTSKlineData*>(object)->at(0);
        w.StartObject(); w.Key("date"); w.Uint(b.date); w.Key("time"); w.Uint64(b.time); w.Key("reserve_"); w.Uint(b.reserve_);
#define D(member) w.Key(#member); w.Double(b.member)
        D(open); D(high); D(low); D(close); D(settle); D(money); D(vol); D(hold); D(add);
#undef D
        w.EndObject();
    });
    w.Key("ticks"); writeMap(w, _rt_tick_map, [&](WTSObject* object) {
        const auto& t = static_cast<WTSTickData*>(object)->getTickStruct();
        w.StartObject(); w.Key("code"); w.String(t.code); w.Key("exchg"); w.String(t.exchg);
#define D(member) w.Key(#member); w.Double(t.member)
#define U(member) w.Key(#member); w.Uint(t.member)
        D(price); D(open); D(high); D(low); D(settle_price); D(upper_limit); D(lower_limit);
        D(total_volume); D(volume); D(total_turnover); D(turn_over); D(open_interest); D(diff_interest);
        U(trading_date); U(action_date); U(action_time); U(reserve_);
        D(pre_close); D(pre_settle); D(pre_interest);
#undef D
#undef U
#define A(member) w.Key(#member); w.StartArray(); for (double v : t.member) w.Double(v); w.EndArray()
        A(bid_prices); A(ask_prices); A(bid_qty); A(ask_qty);
#undef A
        w.EndObject();
    });
    w.EndObject(); return buffer.GetString();
}

void WtDtMgr::restoreLive(const std::string& state) {
    if (!_engine->controlled() || _bars_cache || _rt_tick_map || !_bar_notifies.empty())
        throw std::logic_error("Market restore requires a fresh controlled data manager");
    rapidjson::Document d; d.Parse<rapidjson::kParseFullPrecisionFlag>(state.c_str());
    if (d.HasParseError() || !d.IsObject() || readUint(field(d, "version")) != 1)
        throw std::invalid_argument("Invalid controlled market checkpoint");
    const auto& bars = field(d, "bars"); const auto& ticks = field(d, "ticks");
    if (!bars.IsObject() || !ticks.IsObject()) throw std::invalid_argument("Invalid controlled market maps");
    auto release = [](DataCacheMap* p) { p->release(); };
    std::unique_ptr<DataCacheMap, decltype(release)> restoredBars(DataCacheMap::create(), release), restoredTicks(DataCacheMap::create(), release);
    for (auto it = bars.MemberBegin(); it != bars.MemberEnd(); ++it) {
        const auto code = readText(it->name); const auto& v = it->value;
        if (code.empty() || code.size() >= 32 || restoredBars->get(code)) throw std::invalid_argument("Invalid controlled bar identity");
        WTSBarStruct b; b.date = readUint(field(v, "date")); b.time = readInteger(field(v, "time")); b.reserve_ = readUint(field(v, "reserve_"));
#define D(member) b.member = readNumber(field(v, #member))
        D(open); D(high); D(low); D(close); D(settle); D(money); D(vol); D(hold); D(add);
#undef D
        validateBar(b);
        auto data = WTSKlineData::create(code.c_str(), 1); data->setPeriod(KP_Minute5, 1); *data->at(0) = b;
        restoredBars->add(code, data, false);
    }
    for (auto it = ticks.MemberBegin(); it != ticks.MemberEnd(); ++it) {
        const auto code = readText(it->name); const auto& v = it->value;
        auto contract = _engine->get_contract_info(code.c_str());
        if (!contract || restoredTicks->get(code)) throw std::invalid_argument("Invalid controlled tick identity");
        WTSTickStruct t; readText(field(v, "code"), t.code); readText(field(v, "exchg"), t.exchg);
#define D(member) t.member = readNumber(field(v, #member))
#define U(member) t.member = readUint(field(v, #member))
        D(price); D(open); D(high); D(low); D(settle_price); D(upper_limit); D(lower_limit);
        D(total_volume); D(volume); D(total_turnover); D(turn_over); D(open_interest); D(diff_interest);
        U(trading_date); U(action_date); U(action_time); U(reserve_);
        D(pre_close); D(pre_settle); D(pre_interest);
#undef D
#undef U
#define A(member) { const auto& a = field(v, #member); if (!a.IsArray() || a.Size() != 10) throw std::invalid_argument("Invalid native depth array"); for (unsigned i = 0; i != 10; ++i) t.member[i] = readNumber(a[i]); }
        A(bid_prices); A(ask_prices); A(bid_qty); A(ask_qty);
#undef A
        auto tick = WTSTickData::create(t); tick->setContractInfo(contract); restoredTicks->add(code, tick, false);
    }
    _bars_cache = restoredBars.release(); _rt_tick_map = restoredTicks.release();
}
