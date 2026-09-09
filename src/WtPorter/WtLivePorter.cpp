#include "WtPorter.h"
#include "WtRtRunner.h"
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

WtRtRunner& getRunner();
namespace {
const char* invoke(const char* operation, const char* request = "{}", const WTSTickStruct* tick = nullptr, const WTSBarStruct* bar = nullptr) {
    thread_local std::string result;
    rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
    w.StartObject();
    try {
        auto data = getRunner().controlledRequest(operation, request, tick, bar);
        w.Key("ok"); w.Bool(true); w.Key("data"); w.RawValue(data.c_str(), data.size(), rapidjson::kObjectType);
    } catch (const std::exception& error) {
        getRunner().blockControlled();
        w.Key("ok"); w.Bool(false); w.Key("error"); w.String(error.what());
    }
    w.EndObject(); result.assign(buffer.GetString(), buffer.GetSize()); return result.c_str();
}
}

uint32_t wt_live_abi() { return 1; }
const char* wt_live_configure(const char* request) { return invoke("configure", request); }
const char* wt_live_connect() { return invoke("connect"); }
const char* wt_live_start(const char* request) { return invoke("start", request); }
const char* wt_live_warmup(const WTSBarStruct* bar) { return invoke("warmup", "{}", nullptr, bar); }
const char* wt_live_step(const char* request, const WTSTickStruct* tick, const WTSBarStruct* bar) { return invoke("step", request, tick, bar); }
const char* wt_live_snapshot() { return invoke("snapshot"); }
const char* wt_live_restore(const char* state) { return invoke("restore", state); }
const char* wt_live_arm(const char* request) { return invoke("arm", request); }
const char* wt_live_submit(const char* request) { return invoke("submit", request); }
const char* wt_live_cancel(const char* request) { return invoke("cancel", request); }
const char* wt_live_query(const char* request) { return invoke("query", request); }
const char* wt_live_ack(const char* request) { return invoke("ack", request); }
void wt_live_block() { getRunner().blockControlled(); }
