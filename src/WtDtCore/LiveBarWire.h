#pragma once
#include "LiveBarSeries.h"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <stdexcept>
#include <set>

namespace LiveBarWire {
constexpr uint32_t Enumerate = 0x300, Read = 0x301, Current = 0x302;
constexpr size_t MaxPacket = 1200;

struct Request {
    uint32_t type = 0;
    uint64_t request_id = 0, after = 0, through_ns = 0, index = 0;
    std::string stream, code, frequency;
};

inline uint64_t number(const rapidjson::Value& value)
{
    if (!value.IsString() || !value.GetStringLength() || value.GetStringLength() > 20)
        throw std::invalid_argument("invalid unsigned decimal string");
    const std::string text(value.GetString(), value.GetStringLength());
    if (text.size() > 1 && text.front() == '0') throw std::invalid_argument("leading zero");
    uint64_t result = 0;
    for (char c : text) {
        if (c < '0' || c > '9' || result > (UINT64_MAX - (c - '0')) / 10)
            throw std::invalid_argument("unsigned decimal overflow");
        result = result * 10 + c - '0';
    }
    return result;
}

inline std::string text(const rapidjson::Value& value, size_t limit, bool empty = false)
{
    if (!value.IsString() || value.GetStringLength() > limit || (!empty && !value.GetStringLength()))
        throw std::invalid_argument("invalid bar string length");
    std::string result(value.GetString(), value.GetStringLength());
    for (unsigned char c : result)
        if (c < 33 || c > 126) throw std::invalid_argument("invalid bar ASCII string");
    return result;
}

inline uint32_t minutes(const std::string& frequency)
{
    if (frequency == "1m") return 1;
    if (frequency == "5m") return 5;
    if (frequency == "15m") return 15;
    if (frequency == "30m") return 30;
    if (frequency == "1h") return 60;
    if (frequency == "2h") return 120;
    if (frequency == "1d") return 0;
    throw std::invalid_argument("unsupported bar frequency");
}

inline const rapidjson::Value& field(const rapidjson::Value& doc, const char* name)
{
    if (!doc.HasMember(name)) throw std::invalid_argument("missing bar request field");
    return doc[name];
}

inline Request parse(const char* packet, size_t length)
{
    if (length < 6 || length > MaxPacket) throw std::invalid_argument("invalid bar packet length");
    Request result;
    for (int i = 0; i < 4; ++i) result.type |= uint32_t(static_cast<unsigned char>(packet[i])) << (i * 8);
    if (result.type != Enumerate && result.type != Read && result.type != Current)
        throw std::invalid_argument("unknown bar request type");
    rapidjson::Document doc;
    doc.Parse(packet + 4, length - 4);
    if (doc.HasParseError() || !doc.IsObject()) throw std::invalid_argument("invalid bar request JSON");
    std::set<std::string> fields;
    for (auto i = doc.MemberBegin(); i != doc.MemberEnd(); ++i)
        if (!fields.insert(std::string(i->name.GetString(), i->name.GetStringLength())).second)
            throw std::invalid_argument("duplicate bar request field");
    result.request_id = number(field(doc, "request_id"));
    if (result.type == Enumerate) {
        if (fields.size() != 3) throw std::invalid_argument("unexpected enumerate field");
        result.stream = text(field(doc, "stream"), 36, true);
        result.index = number(field(doc, "index"));
    } else {
        result.code = text(field(doc, "instrument_id"), 31);
        result.frequency = text(field(doc, "frequency"), 3);
        minutes(result.frequency);
        if (result.type == Read) {
            if (fields.size() != 6) throw std::invalid_argument("unexpected read field");
            result.stream = text(field(doc, "stream"), 36);
            result.after = number(field(doc, "after"));
            result.through_ns = number(field(doc, "through_ns"));
        } else if (fields.size() != 3) throw std::invalid_argument("unexpected current field");
    }
    return result;
}

using Writer = rapidjson::Writer<rapidjson::StringBuffer>;
inline void ns(Writer& w, const char* key, uint64_t value)
{
    w.Key(key); w.String(std::to_string(value).c_str());
}
inline void coverage(Writer& w, const LiveBarCoverage& c)
{
    w.StartObject();
    w.Key("partial"); w.Bool(c.partial); w.Key("gap"); w.Bool(c.gap);
    w.Key("reasons"); w.Uint(c.reasons);
    ns(w, "first_ns", c.first_ns); ns(w, "last_ns", c.last_ns);
    w.EndObject();
}
inline void interval(Writer& w, const std::optional<LiveBarInterval>& value)
{
    if (!value) { w.Null(); return; }
    const auto& r = *value;
    w.StartObject();
    ns(w, "sequence", r.sequence); ns(w, "start_ns", r.start_ns); ns(w, "end_ns", r.end_ns);
    ns(w, "event_ns", r.event_ns); ns(w, "received_ns", r.received_ns);
    w.Key("trading_day"); w.Uint(r.trading_day);
    w.Key("coverage"); coverage(w, r.coverage);
    w.Key("bar");
    if (!r.bar) w.Null();
    else {
        const auto& b = *r.bar;
        for (double value : {b.open, b.high, b.low, b.close, b.vol, b.money, b.hold})
            if (!std::isfinite(value)) throw std::invalid_argument("nonfinite bar value");
        w.StartObject();
        w.Key("open"); w.Double(b.open); w.Key("high"); w.Double(b.high);
        w.Key("low"); w.Double(b.low); w.Key("close"); w.Double(b.close);
        w.Key("volume"); w.Double(b.vol); w.Key("turnover"); w.Double(b.money);
        w.Key("open_interest"); w.Double(b.hold);
        w.EndObject();
    }
    w.EndObject();
}
inline std::string packet(uint32_t kind, const rapidjson::StringBuffer& json)
{
    if (json.GetSize() + 4 > MaxPacket) throw std::length_error("bar response exceeds 1200 bytes");
    std::string result(4, '\0');
    for (int i = 0; i < 4; ++i) result[i] = char((kind >> (i * 8)) & 255);
    result.append(json.GetString(), json.GetSize());
    return result;
}
inline void head(Writer& w, const Request& req, const std::string& stream, const char* status)
{
    w.StartObject(); ns(w, "request_id", req.request_id);
    w.Key("stream"); w.String(stream.c_str()); w.Key("status"); w.String(status);
}
inline std::string page(const Request& req, const std::string& stream, const LiveBarPage& page)
{
    rapidjson::StringBuffer json;
    Writer w(json); head(w, req, stream, page.expired ? "cursor_expired" : "ok");
    w.Key("instrument_id"); w.String(req.code.c_str());
    w.Key("frequency"); w.String(req.frequency.c_str());
    ns(w, "through_ns", req.through_ns); ns(w, "after", req.after);
    ns(w, "earliest", page.earliest); ns(w, "next", page.next);
    ns(w, "processed_through_ns", page.processed_through_ns);
    w.Key("has_more"); w.Bool(page.has_more);
    w.Key("coverage"); coverage(w, page.coverage);
    w.Key("input"); w.StartArray(); w.Bool(page.connected); w.Bool(page.subscribed);
    w.Bool(page.session_closed); w.String(std::to_string(page.observed_ns).c_str()); w.EndArray();
    w.Key("interval"); interval(w, page.interval);
    w.EndObject(); return packet(Read, json);
}
inline std::string error(const Request& req, const std::string& stream, const char* code)
{
    // Callers supply one of the fixed internal error codes; never echo input.
    rapidjson::StringBuffer json; Writer w(json); head(w, req, stream, code);
    w.EndObject(); return packet(req.type, json);
}
inline std::string enumeratePage(const Request& req, const std::string& stream,
    const std::vector<std::pair<std::string, std::string>>& series, uint64_t total)
{
    rapidjson::StringBuffer json; Writer w(json); head(w, req, stream, "ok");
    auto end = req.index + series.size();
    ns(w, "index", req.index); ns(w, "next", end);
    w.Key("has_more"); w.Bool(end < total);
    w.Key("items"); w.StartArray();
    for (const auto& item : series) {
        w.StartArray(); w.String(item.first.c_str()); w.String(item.second.c_str()); w.EndArray();
    }
    w.EndArray(); w.EndObject(); return packet(Enumerate, json);
}
inline std::string enumerate(const Request& req, const std::string& stream,
    const std::vector<std::pair<std::string, std::string>>& series)
{
    if (req.index > series.size()) return error(req, stream, "index_invalid");
    const auto end = std::min<uint64_t>(series.size(), req.index + 8);
    return enumeratePage(req, stream, {series.begin() + req.index, series.begin() + end}, series.size());
}
inline std::string current(const Request& req, const std::string& stream,
    const std::optional<LiveBarInterval>& value, uint64_t processed, const LiveBarPage& state = LiveBarPage{})
{
    rapidjson::StringBuffer json; Writer w(json); head(w, req, stream, "ok");
    w.Key("instrument_id"); w.String(req.code.c_str());
    w.Key("frequency"); w.String(req.frequency.c_str());
    ns(w, "processed_through_ns", processed);
    w.Key("input"); w.StartArray(); w.Bool(state.connected); w.Bool(state.subscribed);
    w.Bool(state.session_closed); w.String(std::to_string(state.observed_ns).c_str()); w.EndArray();
    w.Key("interval"); interval(w, value); w.EndObject(); return packet(Current, json);
}
}
