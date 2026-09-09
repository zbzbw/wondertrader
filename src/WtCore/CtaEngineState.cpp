#include "WtCtaEngine.h"
#include "CtaStraBaseCtx.h"
#include "WtCtaTicker.h"
#include "WtDtMgr.h"
#include "../Includes/WTSRiskDef.hpp"
#include "../Includes/WTSVariant.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

USING_NS_WTP;
namespace {
using Value = rapidjson::Value;
using Writer = rapidjson::Writer<rapidjson::StringBuffer>;
const Value& field(const Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key)) throw std::invalid_argument("Missing CTA engine checkpoint field");
    return v[key];
}
double readNumber(const Value& v) {
    if (!v.IsNumber() || !std::isfinite(v.GetDouble())) throw std::invalid_argument("Invalid CTA engine number");
    return v.GetDouble();
}
uint64_t readInteger(const Value& v) {
    if (!v.IsUint64()) throw std::invalid_argument("Invalid CTA engine integer"); return v.GetUint64();
}
uint32_t readUint(const Value& v) {
    if (!v.IsUint()) throw std::invalid_argument("Invalid CTA engine uint32"); return v.GetUint();
}
bool readBool(const Value& v) {
    if (!v.IsBool()) throw std::invalid_argument("Invalid CTA engine flag"); return v.GetBool();
}
std::string readText(const Value& v) {
    if (!v.IsString() || std::strlen(v.GetString()) != v.GetStringLength()) throw std::invalid_argument("Invalid CTA engine text");
    return v.GetString();
}
template<class Map, class F> void writeMap(Writer& w, const Map& map, F write) {
    std::vector<std::string> keys; for (const auto& e : map) keys.push_back(e.first);
    std::sort(keys.begin(), keys.end()); w.StartObject();
    for (const auto& key : keys) { w.Key(key.c_str()); write(map.at(key)); } w.EndObject();
}
template<class F> void readMap(const Value& value, F read) {
    if (!value.IsObject()) throw std::invalid_argument("Invalid CTA engine map");
    wt_hashset<std::string> keys;
    for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it) {
        auto key = readText(it->name);
        if (!keys.insert(key).second) throw std::invalid_argument("Duplicate CTA engine map key");
        read(key, it->value);
    }
}
}

std::string WtCtaEngine::liveSnapshot() const {
    if (!_controlled || _thrd_task || !_task_queue.empty() || _ctx_map.size() != 1)
        throw std::logic_error("Engine checkpoint requires the single-strategy controlled barrier");
    auto context = dynamic_cast<CtaStraBaseCtx*>(_ctx_map.begin()->second.get());
    if (!context) throw std::logic_error("Controlled CTA context does not support checkpointing");
    rapidjson::StringBuffer buffer; Writer w(buffer);
    auto text = [&](const char* key, const std::string& v) { w.Key(key); w.String(v.c_str()); };
    w.StartObject(); w.Key("version"); w.Uint(1);
    w.Key("date"); w.Uint(_cur_date); w.Key("time"); w.Uint(_cur_time);
    w.Key("raw_time"); w.Uint(_cur_raw_time); w.Key("seconds"); w.Uint(_cur_secs);
    w.Key("trading_day"); w.Uint(_cur_tdate); w.Key("event_ms"); w.Uint64(_controlled_time);
    w.Key("ready"); w.Bool(_ready); w.Key("risk_scale"); w.Double(_risk_volscale); w.Key("risk_date"); w.Uint(_risk_date);
    w.Key("fund"); w.StartObject(); const auto& f = _port_fund->fundInfo();
    w.Key("_predynbal"); w.Double(f._predynbal);
w.Key("_prebalance"); w.Double(f._prebalance);
w.Key("_balance"); w.Double(f._balance);
w.Key("_profit"); w.Double(f._profit);
w.Key("_dynprofit"); w.Double(f._dynprofit);
w.Key("_fees"); w.Double(f._fees);
w.Key("_max_dyn_bal"); w.Double(f._max_dyn_bal);
w.Key("_min_dyn_bal"); w.Double(f._min_dyn_bal);
    w.Key("_last_date"); w.Uint(f._last_date);
w.Key("_max_time"); w.Uint(f._max_time);
w.Key("_min_time"); w.Uint(f._min_time);
    w.Key("_update_time"); w.Int64(f._update_time);
    w.Key("max_md_date"); w.Uint(f._max_md_dyn_bal._date); w.Key("max_md_balance"); w.Double(f._max_md_dyn_bal._dyn_balance);
    w.Key("min_md_date"); w.Uint(f._min_md_dyn_bal._date); w.Key("min_md_balance"); w.Double(f._min_md_dyn_bal._dyn_balance); w.EndObject();
    w.Key("positions"); writeMap(w, _pos_map, [&](const PosInfoPtr& p) {
        w.StartObject(); w.Key("volume"); w.Double(p->_volume); w.Key("closeprofit"); w.Double(p->_closeprofit);
        w.Key("dynprofit"); w.Double(p->_dynprofit); w.Key("details"); w.StartArray();
        for (const auto& d : p->_details) {
            w.StartObject(); w.Key("long"); w.Bool(d._long); w.Key("price"); w.Double(d._price);
            w.Key("volume"); w.Double(d._volume); w.Key("opentime"); w.Uint64(d._opentime);
            w.Key("opentdate"); w.Uint(d._opentdate); w.Key("profit"); w.Double(d._profit); w.EndObject();
        } w.EndArray(); w.EndObject();
    });
    w.Key("signals"); writeMap(w, _sig_map, [&](const SigInfo& s) {
        w.StartObject(); w.Key("volume"); w.Double(s._volume); w.Key("gentime"); w.Uint64(s._gentime); w.EndObject();
    });
    w.Key("prices"); writeMap(w, _price_map, [&](double v) { w.Double(v); });
    w.Key("factors"); writeMap(w, _factors_cache, [&](double v) { w.Double(v); });
    auto subscriptions = [&](const StraSubMap& map) {
        writeMap(w, map, [&](const SubList& list) {
            if (list.empty()) { w.Null(); return; }
            if (list.size() != 1 || list.begin()->first != context->id())
                throw std::logic_error("Controlled subscription belongs to another context");
            w.Uint(list.begin()->second.second);
        });
    };
    w.Key("tick_subscriptions"); subscriptions(_tick_sub_map);
    w.Key("bar_subscriptions"); subscriptions(_bar_sub_map);
    w.Key("cached_targets"); writeMap(w, _exec_mgr._all_cached_targets, [&](const auto& targets) {
        writeMap(w, targets, [&](double v) { w.Double(v); });
    });
    text("context", context->liveSnapshot()); text("clock", liveClockSnapshot()); text("data", _data_mgr->liveSnapshot());
    w.EndObject(); return buffer.GetString();
}

void WtCtaEngine::restoreLive(const std::string& state) {
    if (!_controlled || _ready || _tm_ticker || _thrd_task || !_task_queue.empty() || _ctx_map.size() != 1)
        throw std::logic_error("Engine restore requires a fresh controlled single-strategy engine");
    auto context = dynamic_cast<CtaStraBaseCtx*>(_ctx_map.begin()->second.get());
    if (!context) throw std::logic_error("Controlled CTA context does not support checkpointing");
    rapidjson::Document d; d.Parse<rapidjson::kParseFullPrecisionFlag>(state.c_str());
    if (d.HasParseError() || !d.IsObject() || readUint(field(d, "version")) != 1)
        throw std::invalid_argument("Invalid CTA engine checkpoint");
    auto date = readUint(field(d, "date")), time = readUint(field(d, "time")), raw = readUint(field(d, "raw_time"));
    auto seconds = readUint(field(d, "seconds")), day = readUint(field(d, "trading_day"));
    auto event_ms = readInteger(field(d, "event_ms")); auto ready = readBool(field(d, "ready"));
    auto risk_scale = readNumber(field(d, "risk_scale")); auto risk_date = readUint(field(d, "risk_date"));
    WTSFundStruct fund; const auto& f = field(d, "fund");
    fund._predynbal = readNumber(field(f, "_predynbal"));
fund._prebalance = readNumber(field(f, "_prebalance"));
fund._balance = readNumber(field(f, "_balance"));
fund._profit = readNumber(field(f, "_profit"));
fund._dynprofit = readNumber(field(f, "_dynprofit"));
fund._fees = readNumber(field(f, "_fees"));
fund._max_dyn_bal = readNumber(field(f, "_max_dyn_bal"));
fund._min_dyn_bal = readNumber(field(f, "_min_dyn_bal"));
    fund._last_date = readUint(field(f, "_last_date"));
fund._max_time = readUint(field(f, "_max_time"));
fund._min_time = readUint(field(f, "_min_time"));
    if (!field(f, "_update_time").IsInt64()) throw std::invalid_argument("Invalid CTA fund event time");
    fund._update_time = f["_update_time"].GetInt64();
    fund._max_md_dyn_bal._date = readUint(field(f, "max_md_date")); fund._max_md_dyn_bal._dyn_balance = readNumber(field(f, "max_md_balance"));
    fund._min_md_dyn_bal._date = readUint(field(f, "min_md_date")); fund._min_md_dyn_bal._dyn_balance = readNumber(field(f, "min_md_balance"));
    PositionMap positions; SignalMap signals; PriceMap prices, factors; StraSubMap tickSubs, barSubs;
    decltype(_exec_mgr._all_cached_targets) cached;
    readMap(field(d, "positions"), [&](const std::string& key, const Value& v) {
        auto p = std::make_shared<PosInfo>(); p->_volume = readNumber(field(v, "volume"));
        p->_closeprofit = readNumber(field(v, "closeprofit")); p->_dynprofit = readNumber(field(v, "dynprofit"));
        const auto& details = field(v, "details");
        if (!details.IsArray()) throw std::invalid_argument("Invalid CTA engine position details");
        for (const auto& item : details.GetArray()) {
            DetailInfo detail; detail._long = readBool(field(item, "long")); detail._price = readNumber(field(item, "price"));
            detail._volume = readNumber(field(item, "volume")); detail._opentime = readInteger(field(item, "opentime"));
            detail._opentdate = readUint(field(item, "opentdate")); detail._profit = readNumber(field(item, "profit"));
            p->_details.push_back(detail);
        } positions.emplace(key, std::move(p));
    });
    readMap(field(d, "signals"), [&](const std::string& key, const Value& v) {
        SigInfo signal; signal._volume = readNumber(field(v, "volume")); signal._gentime = readInteger(field(v, "gentime"));
        signals.emplace(key, signal);
    });
    readMap(field(d, "prices"), [&](const std::string& key, const Value& v) { prices.emplace(key, readNumber(v)); });
    readMap(field(d, "factors"), [&](const std::string& key, const Value& v) { factors.emplace(key, readNumber(v)); });
    auto subscriptions = [&](const Value& value, StraSubMap& map) {
        readMap(value, [&](const std::string& key, const Value& v) {
            if (v.IsNull()) { map[key]; return; }
            auto option = readUint(v); if (option > 2) throw std::invalid_argument("Invalid native subscription option");
            map[key].emplace(context->id(), std::make_pair(context->id(), option));
        });
    };
    subscriptions(field(d, "tick_subscriptions"), tickSubs); subscriptions(field(d, "bar_subscriptions"), barSubs);
    readMap(field(d, "cached_targets"), [&](const std::string& key, const Value& v) {
        readMap(v, [&](const std::string& code, const Value& qty) { cached[key].emplace(code, readNumber(qty)); });
    });
    const auto contextState = readText(field(d, "context"));
    auto product = _cfg->get("product");
    if (!product) throw std::invalid_argument("Controlled CTA requires a session configuration");
    // Stage the data and clock without callbacks. The sole strategy context
    // validates and commits atomically; subsequent native swaps cannot dispatch.
    WtDtMgr data; auto config = WTSVariant::createObject(); data.init(config, this, true); config->release();
    data.restoreLive(readText(field(d, "data")));
    std::unique_ptr<WtCtaRtTicker> ticker(new WtCtaRtTicker(this));
    ticker->initControlled(nullptr, product->getCString("session")); ticker->restoreLive(readText(field(d, "clock")));
    context->restoreLive(contextState);
    _pos_map.swap(positions); _sig_map.swap(signals); _price_map.swap(prices); _factors_cache.swap(factors);
    _tick_sub_map.swap(tickSubs); _bar_sub_map.swap(barSubs); _exec_mgr._all_cached_targets.swap(cached);
    std::swap(_data_mgr->_bars_cache, data._bars_cache); std::swap(_data_mgr->_rt_tick_map, data._rt_tick_map);
    _port_fund->fundInfo() = fund; _risk_volscale = risk_scale; _risk_date = risk_date;
    set_date_time(date, time, seconds, raw); set_trading_date(day); _controlled_time = event_ms; _ready = ready;
    _tm_ticker = ticker.release();
}
