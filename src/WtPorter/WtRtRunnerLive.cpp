#include "WtRtRunner.h"
#include "../Includes/WTSTradeDef.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../WTSTools/WTSDataFactory.h"
#include "../Share/BarReplay.hpp"
#include <deque>
#include <mutex>
#include <stdexcept>
#include <cmath>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace {
using Value = rapidjson::Value;
using Writer = rapidjson::Writer<rapidjson::StringBuffer>;
const Value& field(const Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key)) throw std::invalid_argument(std::string("Missing controlled request field: ") + key);
    return v[key];
}
std::string text(const Value& v, const char* key) {
    const auto& value = field(v, key);
    if (!value.IsString() || std::strlen(value.GetString()) != value.GetStringLength()) throw std::invalid_argument("Invalid controlled request text");
    return value.GetString();
}
uint64_t integer(const Value& v, const char* key) {
    const auto& value = field(v, key); if (!value.IsUint64()) throw std::invalid_argument("Invalid controlled request integer");
    return value.GetUint64();
}
uint32_t readUint32(const Value& v, const char* key) {
    const auto& value = field(v, key); if (!value.IsUint()) throw std::invalid_argument("Invalid controlled request uint32");
    return value.GetUint();
}
double readDecimal(const Value& v, const char* key) {
    auto source = text(v, key); size_t end = 0; double value = std::stod(source, &end);
    if (end != source.size() || !std::isfinite(value)) throw std::invalid_argument("Invalid controlled request decimal");
    return value;
}
std::string json(const Value& v) {
    rapidjson::StringBuffer b; Writer w(b); v.Accept(w); return b.GetString();
}
rapidjson::Document parse(const std::string& source) {
    rapidjson::Document d; d.Parse<rapidjson::kParseFullPrecisionFlag>(source.c_str());
    if (d.HasParseError() || !d.IsObject()) throw std::invalid_argument("Invalid controlled request JSON");
    return d;
}
std::string paperResult(const std::string& response) {
    auto d = parse(response);
    if (!field(d, "ok").IsBool() || !d["ok"].GetBool()) throw std::runtime_error(text(d, "error"));
    return json(field(d, "data"));
}
std::string barIdentity(const WTSBarStruct& bar) {
    rapidjson::StringBuffer b; Writer w(b); w.StartArray();
    w.Uint(bar.date); w.Uint64(bar.time); w.Uint(bar.reserve_);
    for (double value : {bar.open, bar.high, bar.low, bar.close, bar.settle, bar.money, bar.vol, bar.hold, bar.add}) {
        if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite replay bar");
        w.Double(value);
    }
    if (bar.low <= 0 || bar.open < bar.low || bar.open > bar.high || bar.close < bar.low || bar.close > bar.high)
        throw std::invalid_argument("Invalid replay OHLC prices");
    replayBarLiquidity(bar, 0); w.EndArray(); return b.GetString();
}
}

// The sole owner of callback consumption. Producer threads only append copied
// reports through TraderAdapter; no queue lock is held across native/Python code.
class WtControlledRuntime {
public:
    WtControlledRuntime(TraderAdapterPtr trader, WtCtaEngine& engine, WtDtMgr& data, const Value& request)
        : trader(std::move(trader)), engine(engine), data(data), owner(std::this_thread::get_id()) {
        mode = text(request, "mode");
        if (mode != "paper" && mode != "broker_sim") throw std::invalid_argument("Controlled runtime supports paper or broker_sim only");
        code = text(request, "instrument_id"); contract = engine.get_contract_info(code.c_str());
        if (!contract) throw std::invalid_argument("Controlled runtime requires one known physical contract");
        if ((mode == "paper") != this->trader->supportsPaperControl()) throw std::invalid_argument("Trader does not match the controlled account mode");
        run = text(request, "run_id"); generation = integer(request, "generation");
        event_ms = integer(request, "event_ms"); received_at = text(request, "received_at");
        const auto initialDay = readUint32(request, "trading_day");
        if (!event_ms || received_at.empty()) throw std::invalid_argument("Controlled input origin is required");
        this->trader->configureLive(run, generation, contract->getFullCode(), [this](CommonExecuter action) {
            std::lock_guard<std::mutex> lock{this->callback_mutex}; this->callbacks.push_back(std::move(action));
        });
        this->trader->stampLiveInput(0, event_ms, received_at, initialDay);
        engine.controlledDecisions(false);
    }
    ~WtControlledRuntime() { trader->blockLive(); if (active_bar) active_bar->release(); }
    void block() { trader->blockLive(); stopping = true; engine.controlledDecisions(false); }

    std::string request(const std::string& operation, const std::string& source, const WTSTickStruct* tick, const WTSBarStruct* bar) {
        if (owner != std::this_thread::get_id() || consuming) throw std::logic_error("Controlled API requires the nonreentrant event owner");
        if (failed) throw std::logic_error("Controlled runtime failed; restore a complete checkpoint in a fresh process");
        consuming = true;
        try {
            auto d = parse(source.empty() ? "{}" : source);
            std::string result = "null";
            if (operation == "connect") {
                if (connected) throw std::logic_error("Controlled trader is already connecting");
                connected = true;
                if (!trader->run()) throw std::runtime_error("Controlled trader connection could not start");
            } else if (operation == "warmup") {
                if (started || !bar || tick) throw std::logic_error("Warmup bars must precede controlled strategy start");
                data.controlledBar(code.c_str(), *bar, false);
            } else if (operation == "start") {
                if (started || !trader->isReady()) throw std::logic_error("Controlled start requires reconciled native account facts");
                if (integer(d, "event_ms") < event_ms) throw std::invalid_argument("Controlled start precedes its input origin");
                event_ms = integer(d, "event_ms"); received_at = text(d, "received_at");
                trader->stampLiveInput(sequence, event_ms, received_at, readUint32(d, "trading_day"));
                engine.startControlled(readUint32(d, "date"), readUint32(d, "time"), readUint32(d, "trading_day"), event_ms);
                started = true;
            } else if (operation == "arm") {
                if (!started) throw std::logic_error("Controlled strategy is not started");
                trader->armLive(text(d, "run_id"), integer(d, "generation")); stopping = false; engine.controlledDecisions(true);
            } else if (operation == "submit") {
                if (stopping) throw std::logic_error("Controlled target execution is blocked");
                auto release = [](WTSEntrust* p) { p->release(); };
                std::unique_ptr<WTSEntrust, decltype(release)> order(
                    WTSEntrust::create(contract->getCode(), readDecimal(d, "quantity"), readDecimal(d, "price"), contract->getExchg()), release);
                order->setContractInfo(contract); order->setPriceType(WPT_LIMITPRICE); order->setOrderFlag(WOF_NOR);
                auto direction = text(d, "direction"), offset = text(d, "offset");
                if (direction != "long" && direction != "short") throw std::invalid_argument("Invalid controlled direction");
                order->setDirection(direction == "long" ? WDT_LONG : WDT_SHORT);
                if (offset == "open") order->setOffsetType(WOT_OPEN);
                else if (offset == "close_today") order->setOffsetType(WOT_CLOSETODAY);
                else if (offset == "close_yesterday") order->setOffsetType(WOT_CLOSEYESTERDAY);
                else throw std::invalid_argument("Controlled futures require explicit close buckets");
                auto local = trader->submitLive(text(d, "run_id"), integer(d, "generation"), text(d, "command_id"), order.get());
                result = std::to_string(local);
            } else if (operation == "cancel") {
                result = trader->cancelLive(text(d, "run_id"), integer(d, "generation"), text(d, "command_id"), text(d, "target_id")) ? "true" : "false";
            } else if (operation == "query") {
                const auto& refresh = field(d, "refresh");
                if (!refresh.IsBool()) throw std::invalid_argument("Query refresh must be a boolean");
                if (refresh.GetBool() && trader->queryLiveFacts() < 0) throw std::runtime_error("Native reconciliation query was rejected");
            } else if (operation == "ack") {
                trader->acknowledgeLiveReports(integer(d, "sequence"));
            } else if (operation == "restore") {
                if (connected || started) throw std::logic_error("Joint restore requires a fresh disconnected runtime");
                restore(d);
            } else if (operation == "step") {
                if (!started || !connected) throw std::logic_error("Controlled runtime is not started and connected");
                const bool observe = d.HasMember("observe_only") && d["observe_only"].IsBool() && d["observe_only"].GetBool();
                if (observe && (mode != "broker_sim" || !stopping))
                    throw std::logic_error("Input reconstruction requires a blocked external account");
                // Reconstruct only local CTA/cache state after a broker query.
                // The final trader gate and coordinator submit gate stay blocked.
                if (observe) engine.controlledDecisions(true);
                step(d, tick, bar);
                if (observe) engine.controlledDecisions(false);
            } else if (operation != "snapshot") throw std::invalid_argument("Unsupported controlled operation");
            pump();
            if (operation == "step") trader->recordLiveReport("onInput", nullptr, nullptr);
            if (operation == "snapshot") {
                rapidjson::StringBuffer buffer; Writer w(buffer); auto state = snapshot(); w.String(state.c_str()); result = buffer.GetString();
            }
            auto response = facts(result); consuming = false; return response;
        } catch (...) {
            block();
            if (operation == "step" || operation == "restore" || operation == "start") failed = true;
            consuming = false; throw;
        }
    }

private:
    void closeActiveBar(uint64_t ms) {
        if (!active_bar || !active_bar->size()) return;
        WTSBarStruct closed = *active_bar->at(-1);
        const auto stamp = closed.time + 199000000000ULL;
        data.controlledBar(code.c_str(), closed, true);
        engine.stepControlled(static_cast<uint32_t>(stamp / 10000), static_cast<uint32_t>(stamp % 10000) * 100000, ms);
        active_bar->release(); active_bar = nullptr;
    }
    void aggregateTick(const WTSTickStruct* tick, uint32_t date, uint32_t time, uint64_t ms) {
        const auto stamp = uint64_t(date) * 10000 + time / 100000;
        auto session = engine.get_session_info(code.c_str(), true);
        if (!session) throw std::logic_error("Controlled tick aggregation requires a native session");
        // Reuse the native bar assignment, including section-end ticks. Keep
        // only the unfinished bar; closed bars already belong to WtDtMgr.
        if (tick) {
            auto release = [](WTSTickData* p) { p->release(); };
            WTSTickStruct copy = *tick;
            std::unique_ptr<WTSTickData, decltype(release)> input(WTSTickData::create(copy), release);
            input->setCode(code.c_str()); input->setContractInfo(contract);
            auto releaseBars = [](WTSKlineData* p) { p->release(); };
            std::unique_ptr<WTSKlineData, decltype(releaseBars)> candidate(WTSKlineData::create(code.c_str(), 0), releaseBars);
            candidate->setPeriod(KP_Minute5, 1);
            WTSDataFactory factory;
            factory.updateKlineData(candidate.get(), input.get(), session, true);
            if (candidate->size()) {
                // WTSDataFactory's tick path does not assign settlement/reserve;
                // those fields are explicit zero, never inferred settlement.
                candidate->at(-1)->settle = 0; candidate->at(-1)->reserve_ = 0;
                if (active_bar && active_bar->at(-1)->time != candidate->at(-1)->time) closeActiveBar(ms);
                if (!active_bar) active_bar = candidate.release();
                else factory.updateKlineData(active_bar, input.get(), session, true);
            }
        }
        if (active_bar && active_bar->at(-1)->time + 199000000000ULL <= stamp) closeActiveBar(ms);
    }
    void pump() {
        for (;;) {
            if (mode == "paper") paper = paperResult(trader->paperControl("{\"op\":\"query\"}"));
            std::deque<CommonExecuter> pending;
            { std::lock_guard<std::mutex> lock{callback_mutex}; pending.swap(callbacks); }
            const bool empty = pending.empty();
            for (auto& action : pending) action();
            if (started && !trader->liveConnected()) block();
            // The mocker's queued query chain must also reach its barrier.
            // Broker responses arrive asynchronously and are consumed next poll.
            if (mode != "paper" || empty) break;
        }
    }
    void step(const Value& d, const WTSTickStruct* tick, const WTSBarStruct* bar) {
        const auto next = integer(d, "sequence"), ms = integer(d, "event_ms");
        if (sequence == UINT64_MAX || next != sequence + 1 || ms == 0 || ms < event_ms)
            throw std::invalid_argument("Controlled input cursor or time did not advance");
        const auto date = readUint32(d, "date"), time = readUint32(d, "time"), day = readUint32(d, "trading_day");
        auto kind = text(d, "kind"); auto received = text(d, "received_at");
        if (kind != "tick" && kind != "bar" && kind != "bar_quote" && kind != "clock" && kind != "settle" && kind != "begin_day")
            throw std::invalid_argument("Unsupported controlled input kind");
        if ((kind == "tick") != (tick != nullptr) || (kind == "bar" || kind == "bar_quote") != (bar != nullptr))
            throw std::invalid_argument("Controlled input payload does not match its kind");
        if (!bar_input.empty() && kind != "bar_quote" && kind != "bar")
            throw std::invalid_argument("Complete the active OHLC input before advancing the market stream");
        auto barSource = bar ? barIdentity(*bar) : std::string();
        WTSTickStruct simulated;
        if (kind == "bar_quote") {
            const auto phase = readUint32(d, "phase");
            if (mode != "paper" || phase != bar_phase || phase > 3 || (!bar_input.empty() && bar_input != barSource))
                throw std::invalid_argument("Bar quote must follow the active paper OHLC sequence");
            strcpy(simulated.exchg, contract->getExchg()); strcpy(simulated.code, contract->getCode());
            simulated.action_date = date; simulated.action_time = time; simulated.trading_date = day;
            simulated.price = simulated.ask_prices[0] = simulated.bid_prices[0] = replayBarPrice(*bar, phase);
            simulated.volume = simulated.ask_qty[0] = simulated.bid_qty[0] = replayBarLiquidity(*bar, phase);
            tick = &simulated;
        } else if (kind == "bar" && !bar_input.empty() && (bar_phase != 4 || bar_input != barSource))
            throw std::invalid_argument("Completed bar must follow all four matching OHLC quotes");
        if (kind != "begin_day" && day != engine.getTradingDate()) throw std::invalid_argument("Controlled input trading day mismatch");
        if (tick && (std::string(tick->exchg) + "." + tick->code != contract->getFullCode()
            || tick->action_date != date || tick->action_time != time || tick->trading_date != day))
            throw std::invalid_argument("Controlled quote identity or timestamp mismatch");
        if (bar && bar->time + 199000000000ULL != uint64_t(date) * 10000 + time / 100000)
            throw std::invalid_argument("Controlled bar must close at its native timestamp");
        if (kind == "settle" && (mode != "paper" || text(field(d, "settlement"), "op") != "settle"))
            throw std::invalid_argument("Settlement input must contain a paper settlement operation");
        if (kind == "begin_day" && mode == "paper" && text(field(d, "day_rules"), "op") != "begin_day")
            throw std::invalid_argument("Trading day input must contain a begin_day operation");
        trader->stampLiveInput(next, ms, received, day);
        if (mode == "paper") paper = paperResult(trader->paperStep(next, ms, tick, kind == "bar_quote"));
        pump();
        if (kind == "settle") {
            if (mode != "paper") throw std::logic_error("External accounts settle only at their broker");
            paper = paperResult(trader->paperControl(json(field(d, "settlement"))));
        } else if (kind == "begin_day") {
            if (mode == "paper") paper = paperResult(trader->paperControl(json(field(d, "day_rules"))));
            engine.startControlled(date, time, day, ms, false);
        } else {
            if (kind == "tick" || kind == "clock") aggregateTick(kind == "tick" ? tick : nullptr, date, time, ms);
            if (kind == "bar") data.controlledBar(code.c_str(), *bar, true);
            if (kind == "bar_quote") {
                // Keep the minute unclosed until its complete bar follows phase 3.
                engine.set_controlled_time(ms);
                engine.set_date_time(date, time / 100000, time % 100000, time / 100000);
            } else engine.stepControlled(date, time, ms);
            if (tick) {
                WTSTickStruct copy = *tick;
                auto release = [](WTSTickData* p) { p->release(); };
                std::unique_ptr<WTSTickData, decltype(release)> quote(WTSTickData::create(copy), release);
                quote->setCode(code.c_str()); quote->setContractInfo(contract);
                engine.on_tick(code.c_str(), quote.get());
            }
        }
        if (kind == "bar_quote") { bar_input = std::move(barSource); ++bar_phase; }
        else if (kind == "bar") { bar_input.clear(); bar_phase = 0; }
        auto clock = parse(engine.liveClockSnapshot());
        if (field(clock, "ended").GetBool()) {
            block();
            if (mode == "paper") {
                auto expiry = std::string("{\"op\":\"expire_day\",\"trading_day\":") + std::to_string(day) + "}";
                paper = paperResult(trader->paperControl(expiry));
            }
        }
        sequence = next; event_ms = ms; received_at = std::move(received);
    }
    std::string snapshot() const {
        if (!started) throw std::logic_error("Joint checkpoint requires initialized CTA state");
        rapidjson::StringBuffer buffer; Writer w(buffer);
        auto text = [&](const char* key, const std::string& value) { w.Key(key); w.String(value.c_str()); };
        w.StartObject(); w.Key("version"); w.Uint(1); text("mode", mode); text("instrument_id", code);
        w.Key("sequence"); w.Uint64(sequence); w.Key("event_ms"); w.Uint64(event_ms); text("received_at", received_at);
        w.Key("stopping"); w.Bool(stopping); text("engine", engine.liveSnapshot()); text("trader", trader->liveSnapshot());
        text("bar_input", bar_input); w.Key("bar_phase"); w.Uint(bar_phase);
        w.Key("active_bar");
        if (active_bar) { auto value = barIdentity(*active_bar->at(-1)); w.RawValue(value.c_str(), value.size(), rapidjson::kArrayType); }
        else w.Null();
        if (mode == "paper") { auto p = parse(paper); text("paper", ::text(p, "state")); }
        else { w.Key("paper"); w.Null(); }
        w.EndObject(); return buffer.GetString();
    }
    void restore(const Value& d) {
        if (integer(d, "version") != 1 || text(d, "mode") != mode || text(d, "instrument_id") != code)
            throw std::invalid_argument("Joint native checkpoint identity mismatch");
        auto restoredSequence = integer(d, "sequence"), restoredTime = integer(d, "event_ms");
        auto received = text(d, "received_at"); const auto& stop = field(d, "stopping");
        if (!stop.IsBool()) throw std::invalid_argument("Invalid controlled stop state");
        auto restoredBar = text(d, "bar_input"); auto restoredPhase = readUint32(d, "bar_phase");
        const auto& pendingBar = field(d, "active_bar");
        WTSBarStruct pending{};
        if (!pendingBar.IsNull()) {
            if (!pendingBar.IsArray() || pendingBar.Size() != 12 || !pendingBar[0].IsUint() || !pendingBar[1].IsUint64() || !pendingBar[2].IsUint())
                throw std::invalid_argument("Invalid unfinished native tick bar");
            pending.date = pendingBar[0].GetUint(); pending.time = pendingBar[1].GetUint64(); pending.reserve_ = pendingBar[2].GetUint();
            double* fields[] = {&pending.open, &pending.high, &pending.low, &pending.close, &pending.settle, &pending.money, &pending.vol, &pending.hold, &pending.add};
            for (unsigned i = 0; i != 9; ++i) {
                if (!pendingBar[i + 3].IsNumber()) throw std::invalid_argument("Invalid unfinished bar field");
                *fields[i] = pendingBar[i + 3].GetDouble();
            }
            barIdentity(pending);
        }
        if (restoredPhase > 4 || (restoredPhase == 0) != restoredBar.empty())
            throw std::invalid_argument("Invalid pending OHLC checkpoint");
        if (mode == "paper") {
            rapidjson::StringBuffer b; Writer w(b); w.StartObject(); w.Key("op"); w.String("restore");
            w.Key("state"); w.String(text(d, "paper").c_str()); w.EndObject();
            paper = paperResult(trader->paperControl(b.GetString()));
        }
        trader->restoreLive(text(d, "trader")); engine.restoreLive(text(d, "engine"));
        trader->stampLiveInput(restoredSequence, restoredTime, received, engine.getTradingDate());
        sequence = restoredSequence; event_ms = restoredTime; received_at = std::move(received);
        bar_input = std::move(restoredBar); bar_phase = restoredPhase;
        if (!pendingBar.IsNull()) {
            active_bar = WTSKlineData::create(code.c_str(), 1); active_bar->setPeriod(KP_Minute5, 1); *active_bar->at(0) = pending;
        }
        started = true; stopping = true; // Reconciliation and a new arm are mandatory after every restore.
    }
    std::string facts(const std::string& result) const {
        rapidjson::StringBuffer b; Writer w(b);
        w.StartObject(); w.Key("sequence"); w.Uint64(sequence); w.Key("event_ms"); w.Uint64(event_ms);
        w.Key("ready"); w.Bool(trader->isReady()); w.Key("connected"); w.Bool(trader->liveConnected());
        w.Key("trading_day"); w.Uint(trader->liveTradingDay());
        w.Key("stopping"); w.Bool(stopping); w.Key("reports"); auto reports = trader->liveReports();
        w.RawValue(reports.c_str(), reports.size(), rapidjson::kObjectType);
        w.Key("commands"); w.StartArray();
        for (const auto& item : trader->liveCommands()) {
            w.StartObject(); w.Key("command_id"); w.String(item.first.c_str());
            w.Key("entrust_id"); w.String(item.second.entrust_id.c_str());
            w.Key("local_id"); w.Uint(item.second.local_id);
            w.Key("status"); w.String(item.second.status.c_str()); w.EndObject();
        }
        w.EndArray();
        w.Key("paper"); if (paper.empty()) w.Null(); else w.RawValue(paper.c_str(), paper.size(), rapidjson::kObjectType);
        w.Key("result"); rapidjson::Document value; value.Parse(result.c_str()); value.Accept(w);
        w.EndObject(); return b.GetString();
    }
    TraderAdapterPtr trader;
    WtCtaEngine& engine;
    WtDtMgr& data;
    WTSContractInfo* contract = nullptr;
    std::thread::id owner;
    std::mutex callback_mutex;
    std::deque<CommonExecuter> callbacks;
    std::string mode, code, run, paper, received_at;
    std::string bar_input;
    WTSKlineData* active_bar = nullptr;
    uint32_t bar_phase = 0;
    uint64_t generation = 0, sequence = 0, event_ms = 0;
    bool consuming = false, failed = false, started = false, connected = false;
    std::atomic<bool> stopping{true};
};

std::string WtRtRunner::controlledRequest(const char* operation, const char* request, const WTSTickStruct* tick, const WTSBarStruct* bar) {
    if (std::string(operation) == "configure") {
        if (_controlled_runtime || _engine != &_cta_engine || !_cta_engine.controlled() || _traders.getAdapters().size() != 1)
            throw std::logic_error("Controlled runtime requires a fresh CTA configuration and one trader");
        auto d = parse(request ? request : "{}");
        auto trader = _traders.getAdapter(text(d, "trader_id").c_str());
        if (!trader) throw std::invalid_argument("Controlled trader is not configured");
        _controlled_runtime = std::make_shared<WtControlledRuntime>(trader, _cta_engine, _data_mgr, d);
        return "{\"configured\":true}";
    }
    if (!_controlled_runtime) throw std::logic_error("Controlled runtime is not configured");
    return _controlled_runtime->request(operation, request ? request : "{}", tick, bar);
}

void WtRtRunner::blockControlled() { if (_controlled_runtime) _controlled_runtime->block(); }
