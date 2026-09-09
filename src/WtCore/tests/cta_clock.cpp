#include "../WtCtaEngine.h"
#include "../WtDtMgr.h"
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
        night.stepControlled(20260914, 90000000, 3);
        check(nightEvents.closes.back() == 202609120230ULL, "weekend gap after midnight preserves Saturday night close");
        auto parallel = WTSCfgLoader::load_from_content(R"({"controlled":true,"poolsize":1})");
        Engine invalid; rejects([&] { invalid.init(parallel, &base, &data, nullptr, nullptr); }); parallel->release();
        std::cout << "CTA controlled clock regression passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
