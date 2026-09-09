#include "WtCtaTicker.h"
#include "WtCtaEngine.h"
#include "WtDtMgr.h"
#include "../Includes/IDataReader.h"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Share/TimeUtils.hpp"
#include <stdexcept>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

USING_NS_WTP;
namespace {
void checkTime(uint32_t date, uint32_t time) {
    if (date < 19000101 || date > 99991231 || date % 100 == 0 || date % 100 > 31
        || date / 100 % 100 == 0 || date / 100 % 100 > 12
        || time / 10000000 > 23 || time / 100000 % 100 > 59 || time / 1000 % 100 > 59)
        throw std::invalid_argument("Invalid controlled market timestamp");
}
}

void WtCtaEngine::startControlled(uint32_t date, uint32_t time, uint32_t tradingDay, uint64_t event_ms, bool initialize) {
    if (!_controlled || event_ms < _controlled_time)
        throw std::logic_error("Invalid controlled engine start");
    if (!_tm_ticker) {
        auto product = _cfg->get("product");
        if (!product) throw std::invalid_argument("Controlled CTA requires a session configuration");
        std::unique_ptr<WtCtaRtTicker> ticker(new WtCtaRtTicker(this));
        ticker->initControlled(_data_mgr->reader(), product->getCString("session"));
        _tm_ticker = ticker.release();
    }
    _tm_ticker->startControlled(date, time, tradingDay, event_ms, initialize);
}

void WtCtaEngine::stepControlled(uint32_t date, uint32_t time, uint64_t event_ms) {
    if (!_controlled || !_tm_ticker || event_ms < _controlled_time)
        throw std::logic_error("Controlled engine time moved backwards or is not started");
    _tm_ticker->stepControlled(date, time, event_ms);
}

std::string WtCtaEngine::liveClockSnapshot() const {
    if (!_controlled || !_tm_ticker) throw std::logic_error("Controlled engine clock is not started");
    return _tm_ticker->liveSnapshot();
}

void WtCtaEngine::restoreLiveClock(const std::string& state) {
    if (!_controlled || _tm_ticker) throw std::logic_error("Clock restore requires a fresh controlled engine");
    auto product = _cfg->get("product");
    if (!product) throw std::invalid_argument("Controlled CTA requires a session configuration");
    std::unique_ptr<WtCtaRtTicker> ticker(new WtCtaRtTicker(this));
    ticker->initControlled(_data_mgr->reader(), product->getCString("session"));
    ticker->restoreLive(state);
    _tm_ticker = ticker.release();
}

void WtCtaRtTicker::initControlled(IDataReader* store, const char* sessionID) {
    if (_thrd || _controlled) throw std::logic_error("Controlled ticker requires a fresh instance");
    _s_info = _engine->get_session_info(sessionID);
    if (!_s_info || !_s_info->getTradingMins()) throw std::invalid_argument("Invalid controlled trading session");
    _store = store; _controlled = true; _date = 0; _time = UINT_MAX;
}

void WtCtaRtTicker::startControlled(uint32_t date, uint32_t time, uint32_t tradingDay, uint64_t event_ms, bool initialize) {
    if (!_controlled || (_date && !_session_ended))
        throw std::logic_error("Previous controlled trading session is still active");
    checkTime(date, time);
    if (!tradingDay) throw std::invalid_argument("Controlled trading day is required");
    if (_date && (tradingDay <= _engine->getTradingDate() || date < _date || (date == _date && time < _time)))
        throw std::invalid_argument("Controlled session moved backwards");
    const auto minute = time / 100000;
    auto position = _s_info->timeToMinutes(minute, true);
    if (position == UINT_MAX) throw std::invalid_argument("Controlled session must start before its close");
    _engine->set_controlled_time(event_ms);
    _date = date; _time = time; _cur_pos = position; _last_emit_pos = position;
    _session_ended = false;
    _engine->set_date_time(date, minute, time % 100000, minute);
    _engine->set_trading_date(tradingDay);
    if (initialize) _engine->on_init();
    _engine->on_session_begin();
}

void WtCtaRtTicker::stepControlled(uint32_t date, uint32_t time, uint64_t event_ms) {
    if (!_controlled || !_date) throw std::logic_error("Controlled ticker is not started");
    checkTime(date, time);
    if (date < _date || (date == _date && time < _time))
        throw std::invalid_argument("Controlled market time moved backwards");
    _engine->set_controlled_time(event_ms);
    const auto minute = time / 100000;
    // Explicit clock events close the last observed minute. Gaps do not invent
    // prices or bars; the input journal supplies each actual bar before its
    // closing clock event, including the final minute without another quote.
    auto closed = _s_info->timeToMinutes(minute, true);
    if (closed == UINT_MAX && _s_info->offsetTime(minute, true) >= _s_info->getCloseTime(true))
        closed = _s_info->getTradingMins();
    if (!_session_ended && closed != UINT_MAX && closed > _last_emit_pos) {
        _last_emit_pos = closed;
        const auto closeTime = _s_info->minuteToTime(closed);
        // During a section break the closed bar belongs to the preceding
        // section. Its civil date is retained across a weekend clock jump.
        uint32_t closeDate = date;
        if (closeTime != minute && date != _date)
            closeDate = _date;
        if (closeTime < _time / 100000 && closeDate == _date)
            closeDate = TimeUtils::getNextDate(closeDate);
        const bool ending = closed == _s_info->getTradingMins();
        _engine->set_date_time(closeDate, closeTime, 0, closeTime);
        if (_store) _store->onMinuteEnd(closeDate, closeTime, ending ? _engine->getTradingDate() : 0);
        _engine->on_schedule(closeDate, closeTime);
        if (ending) { _session_ended = true; _engine->on_session_end(); }
    }
    _date = date; _time = time; _cur_pos = closed;
    const auto next = _s_info->timeToMinutes(minute);
    const auto barTime = next == UINT_MAX || _s_info->isLastOfSection(minute)
        ? minute : _s_info->minuteToTime(next + 1);
    _engine->set_date_time(date, barTime, time % 100000, minute);
}

std::string WtCtaRtTicker::liveSnapshot() const {
    if (!_controlled) throw std::logic_error("Only controlled tickers can be checkpointed");
    rapidjson::StringBuffer b; rapidjson::Writer<rapidjson::StringBuffer> w(b);
    w.StartObject(); w.Key("version"); w.Uint(1); w.Key("session"); w.String(_s_info->id());
    w.Key("date"); w.Uint(_date); w.Key("time"); w.Uint(_time);
    w.Key("position"); w.Uint(_cur_pos); w.Key("last_emit"); w.Uint(_last_emit_pos);
    w.Key("ended"); w.Bool(_session_ended); w.EndObject(); return b.GetString();
}

void WtCtaRtTicker::restoreLive(const std::string& state) {
    if (!_controlled || _date) throw std::logic_error("Ticker restore requires a fresh controlled instance");
    rapidjson::Document d; d.Parse(state.c_str());
    if (d.HasParseError() || !d.IsObject()) throw std::invalid_argument("Invalid controlled clock checkpoint");
    for (const char* key : {"version", "date", "time", "position", "last_emit"})
        if (!d.HasMember(key) || !d[key].IsUint()) throw std::invalid_argument("Invalid controlled clock integer");
    if (d["version"].GetUint() != 1 || !d.HasMember("session") || !d["session"].IsString()
        || std::string(d["session"].GetString()) != _s_info->id()
        || !d.HasMember("ended") || !d["ended"].IsBool())
        throw std::invalid_argument("Controlled clock checkpoint session mismatch");
    checkTime(d["date"].GetUint(), d["time"].GetUint());
    auto position = d["position"].GetUint(), last = d["last_emit"].GetUint();
    if ((position != UINT_MAX && position > _s_info->getTradingMins()) || last > _s_info->getTradingMins())
        throw std::invalid_argument("Invalid controlled clock position");
    _date = d["date"].GetUint(); _time = d["time"].GetUint();
    _cur_pos = position; _last_emit_pos = last; _session_ended = d["ended"].GetBool();
}
