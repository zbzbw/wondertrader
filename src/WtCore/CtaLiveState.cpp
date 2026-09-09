#include "CtaStraBaseCtx.h"
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

// Stable object order keeps checkpoints comparable across fresh processes.
template<class Map, class Function>
void writeMap(Writer& w, const Map& map, Function write) {
    std::vector<std::string> keys;
    for (const auto& item : map) keys.push_back(item.first);
    std::sort(keys.begin(), keys.end());
    w.StartObject();
    for (const auto& key : keys) { w.Key(key.c_str()); write(map.at(key)); }
    w.EndObject();
}
template<class Set> void writeSet(Writer& w, const Set& set) {
    std::vector<std::string> values(set.begin(), set.end());
    std::sort(values.begin(), values.end());
    w.StartArray(); for (const auto& value : values) w.String(value.c_str()); w.EndArray();
}
const Value& field(const Value& value, const char* key) {
    if (!value.IsObject() || !value.HasMember(key))
        throw std::invalid_argument(std::string("Missing CTA checkpoint field: ") + key);
    return value[key];
}
std::string readString(const Value& v) {
    if (!v.IsString() || std::strlen(v.GetString()) != v.GetStringLength())
        throw std::invalid_argument("Invalid CTA checkpoint string");
    return v.GetString();
}
double number(const Value& v) {
    if (!v.IsNumber() || !std::isfinite(v.GetDouble())) throw std::invalid_argument("Invalid CTA checkpoint number");
    return v.GetDouble();
}
uint64_t integer(const Value& v) {
    if (!v.IsUint64()) throw std::invalid_argument("Invalid CTA checkpoint integer");
    return v.GetUint64();
}
uint32_t uint32(const Value& v) {
    if (!v.IsUint()) throw std::invalid_argument("Invalid CTA checkpoint uint32");
    return v.GetUint();
}
bool boolean(const Value& v) {
    if (!v.IsBool()) throw std::invalid_argument("Invalid CTA checkpoint boolean");
    return v.GetBool();
}
template<size_t N> void text(char (&target)[N], const Value& value) {
    const auto source = readString(value);
    if (source.size() >= N) throw std::invalid_argument("CTA checkpoint text exceeds native capacity");
    std::memcpy(target, source.c_str(), source.size() + 1);
}
template<class Function> void readMap(const Value& v, Function read) {
    if (!v.IsObject()) throw std::invalid_argument("Invalid CTA checkpoint map");
    wt_hashset<std::string> keys;
    for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it) {
        auto key = readString(it->name);
        if (!keys.insert(key).second) throw std::invalid_argument("Duplicate CTA checkpoint map key");
        read(key, it->value);
    }
}
template<class Set> void readSet(const Value& v, Set& target) {
    if (!v.IsArray()) throw std::invalid_argument("Invalid CTA checkpoint set");
    for (const auto& item : v.GetArray())
        if (!target.insert(readString(item)).second) throw std::invalid_argument("Duplicate CTA checkpoint set value");
}
}

std::string CtaStraBaseCtx::liveSnapshot() const {
    if (_is_in_schedule) throw std::logic_error("CTA checkpoint requires an event barrier");
    rapidjson::StringBuffer buffer; Writer w(buffer);
    w.StartObject(); w.Key("version"); w.Uint(1);
    w.Key("name"); w.String(_name.c_str()); w.Key("slippage"); w.Int(_slippage);
    w.Key("main_key"); w.String(_main_key.c_str());
    w.Key("main_code"); w.String(_main_code.c_str());
    w.Key("main_period"); w.String(_main_period.c_str());
    w.Key("emit_times"); w.Uint(_emit_times);
    w.Key("last_cond_min"); w.Uint64(_last_cond_min);
    w.Key("last_barno"); w.Uint(_last_barno);
    w.Key("kline_tags"); writeMap(w, _kline_tags, [&](const KlineTag& tag) {
        w.StartObject(); w.Key("closed"); w.Bool(tag._closed); w.Key("notify"); w.Bool(tag._notify); w.EndObject();
    });
    w.Key("prices"); writeMap(w, _price_map, [&](double value) { w.Double(value); });
    w.Key("positions"); writeMap(w, _pos_map, [&](const PosInfo& p) {
        w.StartObject();
        w.Key("volume"); w.Double(p._volume); w.Key("closeprofit"); w.Double(p._closeprofit);
        w.Key("dynprofit"); w.Double(p._dynprofit); w.Key("last_entertime"); w.Uint64(p._last_entertime);
        w.Key("last_exittime"); w.Uint64(p._last_exittime); w.Key("frozen"); w.Double(p._frozen);
        w.Key("frozen_date"); w.Uint(p._frozen_date); w.Key("details"); w.StartArray();
        for (const auto& d : p._details) {
            w.StartObject(); w.Key("long"); w.Bool(d._long);
            w.Key("price"); w.Double(d._price); w.Key("volume"); w.Double(d._volume);
            w.Key("opentime"); w.Uint64(d._opentime); w.Key("opentdate"); w.Uint(d._opentdate);
            w.Key("max_profit"); w.Double(d._max_profit); w.Key("max_loss"); w.Double(d._max_loss);
            w.Key("max_price"); w.Double(d._max_price); w.Key("min_price"); w.Double(d._min_price);
            w.Key("profit"); w.Double(d._profit); w.Key("opentag"); w.String(d._opentag);
            w.Key("open_barno"); w.Uint(d._open_barno); w.EndObject();
        }
        w.EndArray(); w.EndObject();
    });
    w.Key("signals"); writeMap(w, _sig_map, [&](const SigInfo& s) {
        w.StartObject(); w.Key("volume"); w.Double(s._volume); w.Key("usertag"); w.String(s._usertag.c_str());
        w.Key("sigprice"); w.Double(s._sigprice); w.Key("sigtype"); w.Uint(s._sigtype);
        w.Key("gentime"); w.Uint64(s._gentime); w.Key("triggered"); w.Bool(s._triggered); w.EndObject();
    });
    w.Key("conditions"); writeMap(w, _condtions, [&](const CondList& conditions) {
        w.StartArray(); for (const auto& c : conditions) {
            w.StartObject(); w.Key("field"); w.Uint(c._field); w.Key("alg"); w.Uint(c._alg);
            w.Key("target"); w.Double(c._target); w.Key("qty"); w.Double(c._qty);
            w.Key("action"); w.Uint(c._action); w.Key("code"); w.String(c._code);
            w.Key("usertag"); w.String(c._usertag); w.EndObject();
        } w.EndArray();
    });
    w.Key("user_data"); writeMap(w, _user_datas, [&](const std::string& v) { w.String(v.c_str()); });
    w.Key("ud_modified"); w.Bool(_ud_modified);
    w.Key("fund"); w.StartObject();
    w.Key("profit"); w.Double(_fund_info._total_profit);
    w.Key("dynprofit"); w.Double(_fund_info._total_dynprofit);
    w.Key("fees"); w.Double(_fund_info._total_fees); w.EndObject();
    w.Key("tick_subs"); writeSet(w, _tick_subs); w.Key("bar_subs"); writeSet(w, _barevt_subs);
    w.Key("chart_code"); w.String(_chart_code.c_str()); w.Key("chart_period"); w.String(_chart_period.c_str());
    w.Key("chart_indices"); writeMap(w, _chart_indice, [&](const ChartIndex& index) {
        w.StartObject(); w.Key("name"); w.String(index._name.c_str()); w.Key("type"); w.Uint(index._indexType);
        w.Key("lines"); writeMap(w, index._lines, [&](const ChartLine& line) {
            w.StartObject(); w.Key("name"); w.String(line._name.c_str()); w.Key("type"); w.Uint(line._lineType); w.EndObject();
        });
        w.Key("baselines"); writeMap(w, index._base_lines, [&](double v) { w.Double(v); }); w.EndObject();
    });
    w.EndObject(); return buffer.GetString();
}

void CtaStraBaseCtx::restoreLive(const std::string& state) {
    if (_is_in_schedule) throw std::logic_error("CTA restore requires an event barrier");
    rapidjson::Document root;
    root.Parse<rapidjson::kParseFullPrecisionFlag>(state.c_str());
    if (root.HasParseError() || !root.IsObject() || uint32(field(root, "version")) != 1
        || readString(field(root, "name")) != _name || !field(root, "slippage").IsInt()
        || root["slippage"].GetInt() != _slippage)
        throw std::invalid_argument("CTA checkpoint identity mismatch");
    auto main_key = readString(field(root, "main_key")), main_code = readString(field(root, "main_code"));
    auto main_period = readString(field(root, "main_period"));
    auto emit_times = uint32(field(root, "emit_times")), last_barno = uint32(field(root, "last_barno"));
    auto last_cond_min = integer(field(root, "last_cond_min"));
    KlineTags tags; PriceMap prices; PositionMap positions; SignalMap signals; CondEntrustMap conditions;
    StringHashMap user_data; decltype(_tick_subs) tick_subs, bar_subs;
    decltype(_chart_indice) indices;
    readMap(field(root, "kline_tags"), [&](const std::string& key, const Value& v) {
        KlineTag tag; tag._closed = boolean(field(v, "closed")); tag._notify = boolean(field(v, "notify")); tags.emplace(key, tag);
    });
    readMap(field(root, "prices"), [&](const std::string& key, const Value& v) { prices.emplace(key, number(v)); });
    readMap(field(root, "positions"), [&](const std::string& key, const Value& v) {
        PosInfo p;
        p._volume = number(field(v, "volume")); p._closeprofit = number(field(v, "closeprofit"));
        p._dynprofit = number(field(v, "dynprofit")); p._last_entertime = integer(field(v, "last_entertime"));
        p._last_exittime = integer(field(v, "last_exittime")); p._frozen = number(field(v, "frozen"));
        p._frozen_date = uint32(field(v, "frozen_date"));
        const auto& details = field(v, "details");
        if (!details.IsArray()) throw std::invalid_argument("Invalid CTA checkpoint position details");
        for (const auto& item : details.GetArray()) {
            DetailInfo d;
            d._long = boolean(field(item, "long")); d._price = number(field(item, "price"));
            d._volume = number(field(item, "volume")); d._opentime = integer(field(item, "opentime"));
            d._opentdate = uint32(field(item, "opentdate")); d._max_profit = number(field(item, "max_profit"));
            d._max_loss = number(field(item, "max_loss")); d._max_price = number(field(item, "max_price"));
            d._min_price = number(field(item, "min_price")); d._profit = number(field(item, "profit"));
            text(d._opentag, field(item, "opentag")); d._open_barno = uint32(field(item, "open_barno"));
            p._details.push_back(d);
        }
        positions.emplace(key, std::move(p));
    });
    readMap(field(root, "signals"), [&](const std::string& key, const Value& v) {
        SigInfo s;
        s._volume = number(field(v, "volume")); s._usertag = readString(field(v, "usertag"));
        s._sigprice = number(field(v, "sigprice")); s._sigtype = uint32(field(v, "sigtype"));
        s._gentime = integer(field(v, "gentime")); s._triggered = boolean(field(v, "triggered"));
        signals.emplace(key, std::move(s));
    });
    readMap(field(root, "conditions"), [&](const std::string& key, const Value& v) {
        if (!v.IsArray()) throw std::invalid_argument("Invalid CTA checkpoint conditions");
        CondList list;
        for (const auto& item : v.GetArray()) {
            CondEntrust c;
            auto compare_field = uint32(field(item, "field")), algorithm = uint32(field(item, "alg"));
            if ((compare_field > WCF_PRICEDIFF && compare_field != WCF_NONE) || algorithm > WCT_SmallerOrEqual)
                throw std::invalid_argument("Invalid CTA checkpoint comparison");
            c._field = static_cast<WTSCompareField>(compare_field);
            c._alg = static_cast<WTSCompareType>(algorithm);
            c._target = number(field(item, "target")); c._qty = number(field(item, "qty"));
            auto action = uint32(field(item, "action"));
            if (action > COND_ACTION_SP) throw std::invalid_argument("Invalid CTA checkpoint action");
            c._action = static_cast<char>(action);
            text(c._code, field(item, "code")); text(c._usertag, field(item, "usertag")); list.push_back(c);
        }
        conditions.emplace(key, std::move(list));
    });
    readMap(field(root, "user_data"), [&](const std::string& key, const Value& v) { user_data.emplace(key, readString(v)); });
    auto modified = boolean(field(root, "ud_modified"));
    StraFundInfo fund; const auto& f = field(root, "fund");
    fund._total_profit = number(field(f, "profit")); fund._total_dynprofit = number(field(f, "dynprofit"));
    fund._total_fees = number(field(f, "fees"));
    readSet(field(root, "tick_subs"), tick_subs); readSet(field(root, "bar_subs"), bar_subs);
    auto chart_code = readString(field(root, "chart_code")), chart_period = readString(field(root, "chart_period"));
    readMap(field(root, "chart_indices"), [&](const std::string& key, const Value& v) {
        ChartIndex index; index._name = readString(field(v, "name")); index._indexType = uint32(field(v, "type"));
        readMap(field(v, "lines"), [&](const std::string& key, const Value& item) {
            ChartLine line; line._name = readString(field(item, "name")); line._lineType = uint32(field(item, "type"));
            index._lines.emplace(key, std::move(line));
        });
        readMap(field(v, "baselines"), [&](const std::string& key, const Value& item) { index._base_lines.emplace(key, number(item)); });
        indices.emplace(key, std::move(index));
    });
    // Commit only after decoding the entire checkpoint. Context handles, engine
    // pointers and output files belong to this process; elapsed profiling time
    // has no trading meaning and is deliberately restarted.
    _main_key.swap(main_key); _main_code.swap(main_code); _main_period.swap(main_period);
    _emit_times = emit_times; _last_cond_min = last_cond_min; _last_barno = last_barno; _total_calc_time = 0;
    _kline_tags.swap(tags); _price_map.swap(prices); _pos_map.swap(positions); _sig_map.swap(signals);
    _condtions.swap(conditions); _user_datas.swap(user_data); _ud_modified = modified; _fund_info = fund;
    _tick_subs.swap(tick_subs); _barevt_subs.swap(bar_subs);
    _chart_code.swap(chart_code); _chart_period.swap(chart_period); _chart_indice.swap(indices);
}
