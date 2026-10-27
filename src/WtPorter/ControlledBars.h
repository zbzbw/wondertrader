#pragma once
#include "../WtCore/WtDtMgr.h"
#include "../WtCore/WtCtaEngine.h"
#include "../WtDtCore/LiveBarCalendar.h"
#include <map>
#include <set>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

// Native progress belongs to the same joint checkpoint as the CTA cache.
class ControlledBars {
    using V = rapidjson::Value;
    using Key = std::pair<std::string, std::string>;
    struct Position { uint64_t cursor = 0; bool loaded = false; };
    struct Notification { Key key; uint64_t cursor; uint32_t day; };
    std::map<Key, Position> positions;
    std::vector<Notification> notifications;
    std::string stream;
    uint64_t target = 0, completed = 0;
    uint32_t history = 120;
    size_t next = 0;
    bool unavailable = false;
    uint32_t calendar_day = 0;
    std::vector<uint64_t> closes;

    static const V& field(const V& v, const char* key) {
        if (!v.IsObject() || !v.HasMember(key)) throw std::invalid_argument("Missing shared bar field");
        return v[key];
    }
    static std::string text(const V& v) {
        if (!v.IsString() || std::strlen(v.GetString()) != v.GetStringLength())
            throw std::invalid_argument("Invalid shared bar text");
        return v.GetString();
    }
    static uint64_t ns(const V& v) {
        auto s = text(v);
        if (s.empty() || s.size() > 20 || s.find_first_not_of("0123456789") != std::string::npos)
            throw std::invalid_argument("Invalid shared bar position");
        return std::stoull(s);
    }
    static uint32_t number(const V& v) {
        if (!v.IsUint()) throw std::invalid_argument("Invalid shared bar integer");
        return v.GetUint();
    }
    static bool flag(const V& v) {
        if (!v.IsBool()) throw std::invalid_argument("Invalid shared bar flag");
        return v.GetBool();
    }
    static uint32_t minutes(const std::string& f) {
        const std::map<std::string, uint32_t> values = {{"1m",1},{"5m",5},{"15m",15},{"30m",30},{"1h",60},{"2h",120},{"1d",0}};
        auto it = values.find(f);
        if (it == values.end()) throw std::invalid_argument("Unsupported shared bar frequency");
        return it->second;
    }
    static Key key(const V& v) { return {text(field(v,"instrument_id")), text(field(v,"frequency"))}; }
    static bool missing(const V& v) { return flag(field(v,"partial")) | flag(field(v,"gap")); }
public:
    bool enabled() const { return !positions.empty(); }
    uint64_t completedThrough() const { return completed; }
    uint64_t nextClose(WtCtaEngine& engine, uint32_t day, uint64_t after) {
        if (!enabled()) throw std::invalid_argument("Shared bar inputs are not configured");
        if (calendar_day != day) {
            std::set<uint64_t> unique;
            auto& base = *static_cast<WTSBaseDataMgr*>(engine.get_basedata_mgr());
            // Physical contracts of a product share its session and holiday calendar.
            std::set<std::pair<std::string,uint32_t>> sessions;
            for (const auto& row : positions) {
                auto contract = engine.get_contract_info(row.first.first.c_str());
                auto period = minutes(row.first.second);
                if (!sessions.emplace(contract->getCommInfo()->getFullPid(),period).second) continue;
                for (const auto& window : LiveBarCalendar::windows(base,*contract,day,period))
                    unique.insert(window.end_ns);
            }
            closes.assign(unique.begin(),unique.end()); calendar_day = day;
        }
        auto found = std::upper_bound(closes.begin(),closes.end(),after);
        return found == closes.end() ? 0 : *found;
    }
    void configure(const V& config, WtCtaEngine& engine, WtDtMgr& data) {
        stream = text(field(config,"stream")); history = number(field(config,"history_count"));
        if (stream.empty() || stream.size() > 36 || !history || history > 100000)
            throw std::invalid_argument("Invalid shared bar configuration");
        const auto& series = field(config,"series");
        if (!series.IsArray() || series.Empty()) throw std::invalid_argument("Shared bars require bound inputs");
        for (const auto& row : series.GetArray()) {
            if (!row.IsArray() || row.Size() != 2) throw std::invalid_argument("Invalid shared bar binding");
            Key k{text(row[0]),text(row[1])}; minutes(k.second);
            if (!engine.get_contract_info(k.first.c_str()) || !positions.emplace(k, Position{}).second)
                throw std::invalid_argument("Unknown or duplicate shared bar binding");
        }
        data.controlledVisibility(0);
    }
    void apply(const V& doc, WtDtMgr& data, const std::function<void(uint64_t,uint32_t)>& clock) {
        const auto through = ns(field(doc,"through_ns"));
        if (!enabled() || text(field(doc,"stream")) != stream || through <= completed || (target && target != through))
            throw std::invalid_argument("Shared bar horizon does not advance its input stream");
        target = through;
        auto action = text(field(doc,"action"));
        if (action == "load") {
            const auto& entries = field(doc,"entries");
            if (!entries.IsArray() || entries.Empty() || entries.Size() > 256)
                throw std::invalid_argument("Invalid shared bar load size");
            for (const auto& entry : entries.GetArray()) {
                const auto k = key(entry); auto found = positions.find(k);
                if (found == positions.end() || found->second.loaded)
                    throw std::invalid_argument("Shared bar input is unbound or already loaded");
                auto& p = found->second;
                const auto& interval = field(entry,"interval");
                if (!interval.IsNull()) {
                    const auto cursor = ns(field(interval,"sequence")), end = ns(field(interval,"end_ns"));
                    if (cursor != p.cursor + 1 || ns(field(interval,"start_ns")) >= end || end > target)
                        throw std::invalid_argument("Shared bar cursor or interval is invalid");
                    unavailable |= missing(field(interval,"coverage"));
                    const auto& value = field(interval,"bar");
                    if (!value.IsNull()) {
                        WTSBarStruct bar{}; bar.date = number(field(interval,"trading_day"));
                        bar.time = uint64_t(LiveBarCalendar::date(end)) * 10000 + LiveBarCalendar::hhmm(end) - 199000000000ULL;
                        auto read = [&](const char* name) {
                            const auto& v = field(value,name);
                            if (!v.IsNumber() || !std::isfinite(v.GetDouble())) throw std::invalid_argument("Invalid shared OHLC value");
                            return v.GetDouble();
                        };
                        bar.open=read("open"); bar.high=read("high"); bar.low=read("low"); bar.close=read("close");
                        bar.vol=read("volume"); bar.money=read("turnover"); bar.hold=read("open_interest");
                        const auto m = minutes(k.second);
                        data.controlledLoadBar(k.first.c_str(),bar,m ? KP_Minute1 : KP_DAY,m ? m : 1,end,history);
                        if (end == target) notifications.push_back({k,cursor,bar.date});
                    }
                    p.cursor = cursor;
                }
                const auto& tail = field(entry,"tail");
                if (!tail.IsNull()) {
                    if (ns(field(field(entry,"input_status"),"processed_through_ns")) < target)
                        throw std::invalid_argument("Shared input has not reached its horizon");
                    unavailable |= missing(tail); p.loaded = true;
                }
            }
            if (loaded()) {
                std::sort(notifications.begin(),notifications.end(),[](const auto& a,const auto& b){return a.key < b.key;});
                data.controlledVisibility(target);
            }
            return;
        }
        if (!loaded()) throw std::invalid_argument("Shared notification precedes complete loading");
        if (action == "notify") {
            const auto& n = field(doc,"notify");
            if (unavailable || next >= notifications.size() || number(field(doc,"position")) != next ||
                key(n) != notifications[next].key || ns(field(n,"sequence")) != notifications[next].cursor)
                throw std::invalid_argument("Shared notification differs from committed progress");
            const auto& item = notifications[next]; const auto m = minutes(item.key.second);
            clock(target,item.day);
            data.controlledNotify(item.key.first.c_str(),m ? KP_Minute1 : KP_DAY,m ? m : 1,target);
            if (++next < notifications.size()) return;
        } else if (action != "finish" || (!unavailable && !notifications.empty()))
            throw std::invalid_argument("Shared finish skipped a notification");
        completed = target; target = 0; next = 0; unavailable = false; notifications.clear();
        for (auto& entry : positions) entry.second.loaded = false;
    }
    bool loaded() const {
        return std::all_of(positions.begin(),positions.end(),[](const auto& row){return row.second.loaded;});
    }
    std::string snapshot() const {
        rapidjson::StringBuffer b; rapidjson::Writer<rapidjson::StringBuffer> w(b);
        w.StartObject(); w.Key("stream"); w.String(stream.c_str()); w.Key("history_count"); w.Uint(history);
        auto stamp=[&](const char* k,uint64_t v){w.Key(k);w.String(std::to_string(v).c_str());};
        stamp("target",target); stamp("completed",completed); w.Key("next");w.Uint64(next);
        w.Key("unavailable");w.Bool(unavailable); w.Key("positions");w.StartArray();
        for (const auto& row : positions) {w.StartArray();w.String(row.first.first.c_str());w.String(row.first.second.c_str());
            w.String(std::to_string(row.second.cursor).c_str());w.Bool(row.second.loaded);w.EndArray();} w.EndArray();
        w.Key("notifications");w.StartArray();
        for (const auto& row : notifications) {w.StartArray();w.String(row.key.first.c_str());w.String(row.key.second.c_str());
            w.String(std::to_string(row.cursor).c_str());w.Uint(row.day);w.EndArray();}w.EndArray();w.EndObject();return b.GetString();
    }
    void restore(const V& state) {
        if (text(field(state,"stream")) != stream || number(field(state,"history_count")) != history)
            throw std::invalid_argument("Shared checkpoint bindings changed");
        if (!field(state,"positions").IsArray() || !field(state,"notifications").IsArray())
            throw std::invalid_argument("Invalid shared checkpoint arrays");
        std::set<Key> seen;
        for (const auto& row : field(state,"positions").GetArray()) {
            if (!row.IsArray() || row.Size()!=4) throw std::invalid_argument("Invalid shared checkpoint position");
            Key k{text(row[0]),text(row[1])};
            if (!positions.count(k) || !seen.insert(k).second) throw std::invalid_argument("Shared checkpoint series changed");
            positions[k]={ns(row[2]),flag(row[3])};
        }
        if (seen.size()!=positions.size()) throw std::invalid_argument("Incomplete shared checkpoint bindings");
        target=ns(field(state,"target"));completed=ns(field(state,"completed"));next=number(field(state,"next"));
        unavailable=flag(field(state,"unavailable")); notifications.clear();
        for (const auto& row : field(state,"notifications").GetArray()) {
            if (!row.IsArray() || row.Size()!=4) throw std::invalid_argument("Invalid shared checkpoint notification");
            Key k{text(row[0]),text(row[1])};
            if (!positions.count(k) || ns(row[2])!=positions.at(k).cursor) throw std::invalid_argument("Invalid shared notification cursor");
            notifications.push_back({k,ns(row[2]),number(row[3])});
        }
        if ((target && target<=completed) || next>notifications.size() || (!target && (next || !notifications.empty())))
            throw std::invalid_argument("Invalid shared checkpoint progress");
    }
};
