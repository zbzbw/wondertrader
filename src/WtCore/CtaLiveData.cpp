#include "WtDtMgr.h"
#include "WtEngine.h"
#include "../Includes/WTSDataDef.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <ctime>
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
    if (!b.date || b.low <= 0 || b.open < b.low || b.open > b.high || b.close < b.low || b.close > b.high
        || b.vol < 0 || std::floor(b.vol) != b.vol)
        throw std::invalid_argument("Invalid completed controlled bar");
}
std::string barKey(const char* code, WTSKlinePeriod& period, uint32_t& times) {
    if (!code || !*code || std::strlen(code) >= 32) throw std::invalid_argument("Invalid controlled bar code");
    if (period == KP_Minute5) {
        if (times > 24) throw std::invalid_argument("Unsupported controlled minute multiplier");
        period = KP_Minute1; times *= 5;
    }
    if (period == KP_Minute1) {
        if (times != 1 && times != 5 && times != 15 && times != 30 && times != 60 && times != 120)
            throw std::invalid_argument("Unsupported controlled minute period");
    } else if (period != KP_DAY || times != 1) throw std::invalid_argument("Unsupported controlled period");
    return std::string(code) + (period == KP_DAY ? "/d" : "/m") + std::to_string(times);
}
void writeBar(Writer& w, const WTSBarStruct& b) {
    w.StartObject(); w.Key("date"); w.Uint(b.date); w.Key("time"); w.Uint64(b.time); w.Key("reserve_"); w.Uint(b.reserve_);
#define D(member) w.Key(#member); w.Double(b.member)
    D(open); D(high); D(low); D(close); D(settle); D(money); D(vol); D(hold); D(add);
#undef D
    w.EndObject();
}
WTSBarStruct readBar(const Value& v) {
    WTSBarStruct b; b.date = readUint(field(v, "date")); b.time = readInteger(field(v, "time")); b.reserve_ = readUint(field(v, "reserve_"));
#define D(member) b.member = readNumber(field(v, #member))
    D(open); D(high); D(low); D(close); D(settle); D(money); D(vol); D(hold); D(add);
#undef D
    validateBar(b); return b;
}
uint64_t readNs(const Value& v) {
    auto value = readText(v);
    if (value.empty() || value.size() > 20 || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("Invalid controlled nanoseconds");
    return std::stoull(value);
}
uint64_t legacyCloseNs(const WTSBarStruct& bar) {
    // The legacy native m5 label is exchange-local YYYYMMDDHHMM minus
    // 199000000000. Decode at this native boundary without using the host TZ.
    const uint64_t label = bar.time + 199000000000ULL;
    const uint32_t date = static_cast<uint32_t>(label / 10000), minute = static_cast<uint32_t>(label % 10000);
    std::tm civil{};
    civil.tm_year = date / 10000 - 1900; civil.tm_mon = date / 100 % 100 - 1; civil.tm_mday = date % 100;
    civil.tm_hour = minute / 100; civil.tm_min = minute % 100;
#ifdef _WIN32
    const auto seconds = _mkgmtime64(&civil) - 8 * 3600;
#else
    const auto seconds = timegm(&civil) - 8 * 3600;
#endif
    if (seconds <= 0 || static_cast<uint64_t>(seconds) > UINT64_MAX / 1000000000ULL)
        throw std::invalid_argument("Legacy bar close is outside the supported UTC range");
    return static_cast<uint64_t>(seconds) * 1000000000ULL;
}
}

void WtDtMgr::controlledBar(const char* stdCode, const WTSBarStruct& bar, bool notify) {
    if (!bar.time) throw std::invalid_argument("Legacy m5 bar needs a close label");
    const auto end_ns = legacyCloseNs(bar);
    controlledLoadBar(stdCode, bar, KP_Minute1, 5, end_ns, 1);
    if (notify) controlledNotify(stdCode, KP_Minute1, 5, end_ns);
}

void WtDtMgr::controlledLoadBar(const char* stdCode, const WTSBarStruct& bar,
    WTSKlinePeriod period, uint32_t times, uint64_t end_ns, uint32_t history_count) {
    if (!_engine->controlled()) throw std::logic_error("Explicit bars require controlled CTA");
    auto key = barKey(stdCode, period, times);
    if (!history_count || history_count > 100000 || !end_ns) throw std::invalid_argument("Invalid controlled bar bounds");
    validateBar(bar);
    if (period != KP_DAY && !bar.time) throw std::invalid_argument("Minute bar needs a close label");
    if (!_bars_cache) _bars_cache = DataCacheMap::create();
    auto data = static_cast<WTSKlineData*>(_bars_cache->get(key));
    auto& ends = _controlled_bar_ends[key];
    if (!ends.empty() && end_ns <= ends.back()) throw std::invalid_argument("Controlled bar cursor did not advance");
    if (!data) {
        data = WTSKlineData::create(stdCode, 0); data->setPeriod(period, times);
        _bars_cache->add(key, data, false);
    }
    data->appendBar(bar); ends.push_back(end_ns);
    // Retain the last previously visible value throughout partial group loading.
    size_t trim = 0;
    while (ends.size() - trim > history_count && ends[trim + 1] <= _controlled_visible_ns) ++trim;
    if (trim) {
        ends.erase(ends.begin(), ends.begin() + trim);
        auto& values = data->getDataRef(); values.erase(values.begin(), values.begin() + trim);
    }
}

void WtDtMgr::controlledVisibility(uint64_t through_ns) {
    if (!_engine->controlled() || (_controlled_shared_bars && through_ns < _controlled_visible_ns))
        throw std::logic_error("Controlled bar visibility moved backwards");
    _controlled_shared_bars = true; _controlled_visible_ns = through_ns;
}

void WtDtMgr::controlledNotify(const char* stdCode, WTSKlinePeriod period, uint32_t times, uint64_t end_ns) {
    auto key = barKey(stdCode, period, times);
    if (end_ns > _controlled_visible_ns) throw std::logic_error("Controlled bar is not visible yet");
    auto data = _bars_cache ? static_cast<WTSKlineData*>(_bars_cache->get(key)) : nullptr;
    if (!data) throw std::invalid_argument("Missing controlled notification bar");
    const auto& ends = _controlled_bar_ends.at(key);
    auto it = std::lower_bound(ends.begin(), ends.end(), end_ns);
    if (it == ends.end() || *it != end_ns) throw std::invalid_argument("Missing controlled notification time");
    _engine->on_bar(stdCode, period == KP_DAY ? "d" : "m", times, data->at(static_cast<int32_t>(it - ends.begin())));
}

WTSKlineSlice* WtDtMgr::controlledSlice(const char* stdCode, WTSKlinePeriod period, uint32_t times,
    uint32_t count, uint64_t etime) {
    auto key = barKey(stdCode, period, times);
    if (!count) throw std::invalid_argument("Controlled bar count must be positive");
    auto data = _bars_cache ? static_cast<WTSKlineData*>(_bars_cache->get(key)) : nullptr;
    if (!data) return nullptr;
    const auto& ends = _controlled_bar_ends.at(key);
    size_t size = std::upper_bound(ends.begin(), ends.end(), _controlled_visible_ns) - ends.begin();
    while (size && etime && (period == KP_DAY ? data->at(static_cast<int32_t>(size - 1))->date :
        data->at(static_cast<int32_t>(size - 1))->time) > etime) --size;
    if (!size) return nullptr;
    size_t begin = size > count ? size - count : 0;
    return WTSKlineSlice::create(stdCode, period, times, data->at(static_cast<int32_t>(begin)), static_cast<int32_t>(size - begin));
}

std::string WtDtMgr::liveSnapshot() const {
    if (!_engine->controlled() || !_bar_notifies.empty()) throw std::logic_error("Market snapshot requires a controlled event barrier");
    rapidjson::StringBuffer buffer; Writer w(buffer);
    w.StartObject(); w.Key("version"); w.Uint(2);
    w.Key("shared_bars"); w.Bool(_controlled_shared_bars);
    w.Key("visible_ns"); w.String(std::to_string(_controlled_visible_ns).c_str());
    w.Key("bars"); writeMap(w, _bars_cache, [&](WTSObject* object) {
        auto data = static_cast<WTSKlineData*>(object);
        auto period = data->period(); auto times = data->times();
        auto key = barKey(data->code(), period, times);
        w.StartObject(); w.Key("code"); w.String(data->code());
        w.Key("period"); w.Uint(static_cast<uint32_t>(period)); w.Key("times"); w.Uint(times);
        w.Key("ends_ns"); w.StartArray(); for (auto end : _controlled_bar_ends.at(key)) w.String(std::to_string(end).c_str()); w.EndArray();
        w.Key("values"); w.StartArray(); for (const auto& bar : data->getDataRef()) writeBar(w, bar); w.EndArray();
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
    if (d.HasParseError() || !d.IsObject() || (readUint(field(d, "version")) != 1 && readUint(field(d, "version")) != 2))
        throw std::invalid_argument("Invalid controlled market checkpoint");
    const auto& bars = field(d, "bars"); const auto& ticks = field(d, "ticks");
    if (!bars.IsObject() || !ticks.IsObject()) throw std::invalid_argument("Invalid controlled market maps");
    auto release = [](DataCacheMap* p) { p->release(); };
    std::unique_ptr<DataCacheMap, decltype(release)> restoredBars(DataCacheMap::create(), release), restoredTicks(DataCacheMap::create(), release);
    const auto version = readUint(field(d, "version"));
    std::map<std::string, std::vector<uint64_t>> restoredEnds;
    for (auto it = bars.MemberBegin(); it != bars.MemberEnd(); ++it) {
        const auto& v = it->value;
        auto code = version == 1 ? readText(it->name) : readText(field(v, "code"));
        auto period = version == 1 ? KP_Minute1 : static_cast<WTSKlinePeriod>(readUint(field(v, "period")));
        auto times = version == 1 ? 5u : readUint(field(v, "times"));
        auto key = barKey(code.c_str(), period, times);
        if (restoredBars->get(key) || (version == 2 && key != readText(it->name)))
            throw std::invalid_argument("Invalid controlled bar identity");
        auto data = WTSKlineData::create(code.c_str(), 0); data->setPeriod(period, times);
        restoredBars->add(key, data, false);
        auto& ends = restoredEnds[key];
        if (version == 1) {
            auto bar = readBar(v);
            if (!bar.time) throw std::invalid_argument("Legacy m5 bar needs a close label");
            data->appendBar(bar); ends.push_back(legacyCloseNs(bar));
        }
        else {
            const auto& values = field(v, "values"); const auto& stamps = field(v, "ends_ns");
            if (!values.IsArray() || !stamps.IsArray() || values.Size() != stamps.Size() || values.Empty())
                throw std::invalid_argument("Invalid controlled bar history");
            for (rapidjson::SizeType i = 0; i < values.Size(); ++i) {
                auto end = readNs(stamps[i]);
                if (!end || (!ends.empty() && end <= ends.back())) throw std::invalid_argument("Invalid controlled bar time order");
                ends.push_back(end); data->appendBar(readBar(values[i]));
            }
        }
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
    if (version == 2) {
        const auto& shared = field(d, "shared_bars");
        if (!shared.IsBool()) throw std::invalid_argument("Invalid controlled shared bar mode");
        _controlled_shared_bars = shared.GetBool(); _controlled_visible_ns = readNs(field(d, "visible_ns"));
    }
    _controlled_bar_ends = std::move(restoredEnds);
    _bars_cache = restoredBars.release(); _rt_tick_map = restoredTicks.release();
}
