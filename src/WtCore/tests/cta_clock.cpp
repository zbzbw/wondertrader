#include "../WtCtaEngine.h"
#include "../WtDtMgr.h"
#include "../CtaStraBaseCtx.h"
#include "../../Includes/WTSDataDef.hpp"
#include "../../Includes/WTSRiskDef.hpp"
#include "../../WTSTools/WTSBaseDataMgr.h"
#include "../../WTSUtils/WTSCfgLoader.h"
#include "../../Includes/WTSVariant.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

USING_NS_WTP;
namespace {
void check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
template<class F> void rejects(F action) {
    bool failed = false; try { action(); } catch (const std::exception&) { failed = true; }
    check(failed, "invalid controlled clock operation must reject");
}
class Events : public IEngineEvtListener {
public:
    std::vector<uint64_t> closes;
    void on_schedule_event(uint32_t date, uint32_t time) override { closes.push_back(uint64_t(date) * 10000 + time); }
};
class Engine : public WtCtaEngine {
public:
    unsigned initializes = 0, begins = 0, ends = 0;
    void on_init() override { ++initializes; }
    void on_session_begin() override { ++begins; }
    void on_session_end() override { ++ends; }
    bool synchronous() {
        bool executed = false;
        push_task([&] { executed = true; });
        return executed && !_thrd_task && _task_queue.empty();
    }
    void seed(uint32_t context) {
        auto p = std::make_shared<PosInfo>(); p->_volume = 1; p->_closeprofit = 2; p->_dynprofit = 3;
        DetailInfo detail; detail._long = true; detail._price = 3500; detail._volume = 1;
        detail._opentime = 202609112100ULL; detail._opentdate = 20260914; detail._profit = 3;
        p->_details.push_back(detail); _pos_map["SHFE.rb.2609"] = p;
        _sig_map["SHFE.rb.2609"]._volume = 1; _sig_map["SHFE.rb.2609"]._gentime = 202609112100ULL;
        _price_map["SHFE.rb.2609"] = 3500; _factors_cache["SHFE.rb.2609"] = 1;
        _tick_sub_map["SHFE.rb.2609"][context] = std::make_pair(context, 0);
        _port_fund->fundInfo()._balance = 10000; _port_fund->fundInfo()._fees = 1;
    }
    bool subscribed(uint32_t context) const { return _tick_sub_map.at("SHFE.rb.2609").count(context) == 1; }
    bool emptyState() const { return _pos_map.empty() && _sig_map.empty() && !_ready && _controlled_time == 0; }
};
class Context : public CtaStraBaseCtx {
public:
    Context(WtCtaEngine* engine) : CtaStraBaseCtx(engine, "cross", 0) {}
    void on_calculate(uint32_t, uint32_t) override {}
    void on_bar_close(const char*, const char*, WTSBarStruct*) override {}
};
void configure(Engine& engine, WtDtMgr& data, WTSBaseDataMgr& base, const char* session) {
    const std::string json = std::string("{\"controlled\":true,\"poolsize\":0,\"product\":{\"session\":\"") + session + "\"}}";
    auto config = WTSCfgLoader::load_from_content(json);
    engine.init(config, &base, &data, nullptr, nullptr); config->release();
    auto dataConfig = WTSVariant::createObject();
    check(data.init(dataConfig, &engine, true), "controlled data manager must not need a disk reader"); dataConfig->release();
}
}
int main() {
    try {
        const char* path = "cta-clock-sessions.json";
        { std::ofstream f(path); f << R"({"rb":{"name":"rb","offset":180,"sections":[{"from":2100,"to":2300},{"from":900,"to":1015},{"from":1030,"to":1130},{"from":1330,"to":1500}]},"night":{"name":"night","offset":180,"sections":[{"from":2100,"to":230},{"from":900,"to":1500}]}})"; }
        WTSBaseDataMgr base; check(base.loadSessions(path), "load native session fixtures"); std::remove(path);
        WtDtMgr data; Engine engine; Events events;
        configure(engine, data, base, "rb"); engine.regEventListener(&events);
        check(engine.synchronous(), "controlled account tasks must finish on the caller thread");
        rejects([&] { engine.run(); });
        engine.startControlled(20260911, 210000000, 20260914, 1);
        engine.stepControlled(20260911, 225900000, 2);
        const auto state = engine.liveClockSnapshot();
        engine.stepControlled(20260914, 90000000, 3);
        check(events.closes.back() == 202609112300ULL, "weekend jump must close Friday's night section, not Monday's");
        engine.stepControlled(20260914, 90500000, 4);
        check(events.closes.back() == 202609140905ULL && engine.get_real_time() == 4,
              "day session uses supplied civil time and deterministic event time");
        engine.stepControlled(20260914, 150000000, 5);
        check(engine.ends == 1 && events.closes.back() == 202609141500ULL, "clock must close trading day without a quote");
        engine.stepControlled(20260914, 160000000, 6);
        check(engine.ends == 1, "trading day close must not repeat");
        const auto ended = engine.liveClockSnapshot();
        rejects([&] { engine.stepControlled(20260914, 150000000, 7); });
        check(engine.liveClockSnapshot() == ended && engine.get_real_time() == 6, "invalid clock event must not advance state");
        WtDtMgr restoredData; Engine restored; Events restoredEvents;
        configure(restored, restoredData, base, "rb"); restored.regEventListener(&restoredEvents);
        restored.set_trading_date(20260914); restored.restoreLiveClock(state);
        check(restored.initializes == 0 && restored.begins == 0, "clock restore must not invoke initialization or begin callbacks");
        restored.stepControlled(20260914, 90000000, 3);
        restored.stepControlled(20260914, 90500000, 4);
        restored.stepControlled(20260914, 150000000, 5);
        check(std::vector<uint64_t>(events.closes.begin() + 1, events.closes.end()) == restoredEvents.closes,
              "restored clock must emit exactly the uninterrupted close sequence");
        WtDtMgr nightData; Engine night; Events nightEvents;
        configure(night, nightData, base, "night"); night.regEventListener(&nightEvents);
        night.startControlled(20260911, 235900000, 20260914, 1);
        night.stepControlled(20260912, 0, 2);
        check(nightEvents.closes.back() == 202609120000ULL, "midnight close must advance civil date once");
        check(night.get_raw_time() == 0 && night.get_min_time() == 1, "midnight raw time must remain distinct from the next bar minute");
        night.stepControlled(20260914, 90000000, 3);
        check(nightEvents.closes.back() == 202609120230ULL, "weekend gap after midnight preserves Saturday night close");
        auto parallel = WTSCfgLoader::load_from_content(R"({"controlled":true,"poolsize":1})");
        Engine invalid; rejects([&] { invalid.init(parallel, &base, &data, nullptr, nullptr); }); parallel->release();
        WtDtMgr completeData; Engine complete; configure(complete, completeData, base, "rb");
        auto context = std::make_shared<Context>(&complete); complete.addContext(context);
        WTSBarStruct bar; bar.date = 20260911; bar.time = 3609112100ULL;
        bar.open = 3500; bar.high = 3510; bar.low = 3490; bar.close = 3505; bar.vol = 17;
        completeData.controlledBar("SHFE.rb.2609", bar, false);
        complete.startControlled(20260911, 210000000, 20260914, 10); complete.seed(context->id());
        const auto allState = complete.liveSnapshot();
        WtDtMgr nextData; Engine next; configure(next, nextData, base, "rb");
        auto nextContext = std::make_shared<Context>(&next); next.addContext(nextContext); next.restoreLive(allState);
        check(next.liveSnapshot() == allState && next.subscribed(nextContext->id()), "joint engine restore remaps context subscriptions and preserves all component state");
        auto latest = nextData.get_kline_slice("SHFE.rb.2609", KP_Minute5, 1, 1);
        check(latest && latest->at(0)->close == 3505 && latest->at(0)->vol == 17, "native completed bar survives restore"); latest->release();
        rejects([&] { nextData.controlledBar("SHFE.rb.2609", bar, false); });
        rejects([&] { nextData.get_kline_slice("SHFE.rb.2609", KP_Minute5, 1, 2); });
        complete.stepControlled(20260911, 210100000, 11); next.stepControlled(20260911, 210100000, 11);
        check(complete.liveSnapshot() == next.liveSnapshot(), "next controlled event after joint restore matches uninterrupted state");
        WtDtMgr badData; Engine bad; configure(bad, badData, base, "rb"); bad.addContext(std::make_shared<Context>(&bad));
        auto corrupt = allState; auto at = corrupt.find("cross"); check(at != std::string::npos, "context identity in joint checkpoint");
        corrupt.replace(at, 5, "wrong"); rejects([&] { bad.restoreLive(corrupt); });
        check(bad.emptyState(), "late context decode failure must not partially restore the engine");
        bad.startControlled(20260911, 210000000, 20260914, 10);
        std::cout << "CTA controlled clock regression passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
