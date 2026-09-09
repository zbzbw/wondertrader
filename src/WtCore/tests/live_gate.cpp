#include "../TraderAdapter.h"
#include "../../Includes/WTSTradeDef.hpp"
#include "../../Includes/WTSContractInfo.hpp"
#include <iostream>
#include <stdexcept>

USING_NS_WTP;
namespace {
void check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
template<class F> void rejects(F action) {
    bool failed = false; try { action(); } catch (const std::exception&) { failed = true; }
    check(failed, "invalid live operation must reject");
}
class Plugin : public ITraderApi {
public:
    unsigned ids = 0, inserts = 0, cancels = 0;
    bool fail = false;
    std::function<void()> before_id;
    bool init(WTSVariant*) override { return true; }
    void release() override {}
    void registerSpi(ITraderSpi*) override {}
    void connect() override {}
    void disconnect() override {}
    bool isConnected() override { return true; }
    bool makeEntrustID(char* buffer, int) override {
        auto id = "native-" + std::to_string(++ids); strcpy(buffer, id.c_str());
        if (before_id) before_id();
        return true;
    }
    int login(const char*, const char*, const char*) override { return 0; }
    int logout() override { return 0; }
    int orderInsert(WTSEntrust*) override { ++inserts; if (fail) throw std::runtime_error("unknown plugin result"); return 0; }
    int orderAction(WTSEntrustAction*) override { ++cancels; return 0; }
    int queryAccount() override { return 0; }
    int queryPositions() override { return 0; }
    int queryOrders() override { return 0; }
    int queryTrades() override { return 0; }
};
}

namespace wtp {
class TraderAdapterLiveTest {
public:
    static void run() {
        Plugin plugin;
        auto commodity = WTSCommodityInfo::create("test", "Test", "TEST", "ALLDAY", "CHINA");
        auto contract = WTSContractInfo::create("test1", "Test", "TEST", "test");
        contract->setCommInfo(commodity);
        TraderAdapter trader;
        std::vector<CommonExecuter> callbacks;
        auto queue = [&](CommonExecuter callback) { callbacks.push_back(std::move(callback)); };
        trader._trader_api = &plugin; trader._order_pattern = "otp.test";
        trader.configureLive("run-one", 4, "TEST.test1", queue);
        trader._live_connected = true; trader._state = TraderAdapter::AS_ALLREADY;
        auto entrust = WTSEntrust::create("test1", 1, 100, "TEST");
        entrust->setContractInfo(contract); entrust->setDirection(WDT_LONG);
        entrust->setOffsetType(WOT_OPEN); entrust->setPriceType(WPT_LIMITPRICE); entrust->setOrderFlag(WOF_NOR);
        rejects([&] { trader.submitLive("run-one", 4, "blocked", entrust); });
        trader.armLive("run-one", 4);
        check(trader.doEntrust(entrust) == UINT32_MAX && plugin.inserts == 0, "direct final outlet must reject without command identity");
        check(trader.openLong("test1", 100, 1, 0, contract) == UINT32_MAX, "legacy executor cannot bypass controlled outlet");
        rejects([&] { trader.submitLive("run-one", 3, "old-generation", entrust); });
        rejects([&] { trader.submitLive("wrong-run", 4, "wrong-run", entrust); });
        auto local = trader.submitLive("run-one", 4, "one", entrust);
        check(local != UINT32_MAX && plugin.inserts == 1, "authorized command reaches real final outlet");
        check(trader.submitLive("run-one", 4, "one", entrust) == local && plugin.inserts == 1, "duplicate command never resends");
        unsigned reports = 0;
        trader.setLiveReportSink([&](const char* kind, const WTSObject* payload, const WTSObject*) {
            check(std::string(kind) == "onRspAccount", "raw report type");
            auto values = static_cast<const WTSArray*>(payload);
            auto account = static_cast<const WTSAccountInfo*>(*values->begin());
            check(account->getBalance() == 12, "queued report is an immutable native copy");
            ++reports;
        });
        auto accounts = WTSArray::create(); auto account = WTSAccountInfo::create();
        account->setBalance(12); account->setMargin(0); accounts->append(account, false);
        std::thread source([&]() { trader.onRspAccount(accounts); }); source.join();
        account->setBalance(999); accounts->release();
        check(reports == 0 && callbacks.size() == 1, "broker callback only enqueues on the event queue");
        auto callback = std::move(callbacks.front()); callbacks.erase(callbacks.begin()); callback();
        check(reports == 1, "raw facts delivered on owner thread");
        const auto outbox = trader.liveReports();
        check(outbox.find("\"m_dBalance\":12.0") != std::string::npos && trader._live_reports.size() == 1,
              "complete raw account record remains native until parent acknowledgement");
        check(trader.liveReports() == outbox, "reading native outbox must not consume records");
        rejects([&] { trader.acknowledgeLiveReports(2); });
        entrust->setVolume(2);
        rejects([&] { trader.submitLive("run-one", 4, "one", entrust); });
        entrust->setVolume(1);
        plugin.fail = true;
        rejects([&] { trader.submitLive("run-one", 4, "unknown", entrust); });
        plugin.fail = false;
        check(trader.liveCommands().at("unknown").status == "unknown", "plugin exception retains unknown attempt");
        trader.submitLive("run-one", 4, "unknown", entrust);
        check(plugin.inserts == 2, "unknown command cannot be replayed");
        plugin.before_id = [&] { trader.blockLive(); };
        trader.submitLive("run-one", 4, "revoked-during-preparation", entrust);
        check(plugin.inserts == 2, "final gate rechecks revocation after ID preparation");
        plugin.before_id = {};
        auto order = WTSOrderInfo::create(entrust);
        order->setContractInfo(contract); order->setOrderID("owned-order");
        order->setCode(contract->getCode());
        order->setEntrustID(trader.liveCommands().at("one").entrust_id.c_str());
        order->setOrderState(WOS_NotTraded_Queuing);
        trader._orders = OrderMap::create(); trader._orders->add(local, order, false);
        check(!trader.cancel(local) && plugin.cancels == 0, "legacy cancel cannot bypass ownership gate");
        check(trader.cancelLive("run-one", 4, "cancel-one", "one") && plugin.cancels == 1,
              "owned cancellation allowed while new sends blocked");
        trader.cancelLive("run-one", 4, "cancel-one", "one");
        check(plugin.cancels == 1, "cancel command is not repeated");
        rejects([&] { trader.cancelLive("run-one", 4, "external", "unowned"); });
        trader._live_connected = false;
        rejects([&] { trader.armLive("run-one", 4); });
        rejects([&] { trader.cancelLive("run-one", 4, "disconnected", "one"); });
        auto checkpoint = trader.liveSnapshot();
        TraderAdapter restarted;
        restarted._trader_api = &plugin; restarted._order_pattern = "otp.test";
        restarted.configureLive("run-two", 5, "TEST.test1", queue);
        restarted.restoreLive(checkpoint);
        order->setUserTag("");
        check(restarted.liveOrderId(order) == local, "restored owned order resolves without the broker plugin tag cache");
        order->setEntrustID("external-entrust");
        order->setUserTag(("otp.test." + std::to_string(local)).c_str());
        check(restarted.liveOrderId(order) == 0, "an external order cannot claim ownership through a copied tag");
        order->setEntrustID(trader.liveCommands().at("one").entrust_id.c_str());
        check(restarted.liveSnapshot() == checkpoint && !restarted._live_enabled && !restarted._live_connected,
              "command restore preserves facts but not authority");
        check(restarted.liveReports() == outbox, "unacknowledged raw reports retain their original source after restart");
        restarted.acknowledgeLiveReports(1);
        restarted.acknowledgeLiveReports(1);
        check(restarted._live_reports.empty() && restarted._live_report_ack == 1, "parent ACK consumes exact native waterline idempotently");
        rejects([&] { restarted.acknowledgeLiveReports(0); });
        restarted.submitLive("run-two", 5, "unknown", entrust);
        check(plugin.inserts == 2, "restored unknown command is observed without resending");
        rejects([&] { restarted.submitLive("run-one", 4, "one", entrust); });
        rejects([&] { restarted.submitLive("run-two", 5, "new-blocked", entrust); });
        restarted.setLiveReportSink({});
        entrust->setEntrustID(restarted.liveCommands().at("unknown").entrust_id.c_str());
        auto error = WTSError::create(WEC_ORDERINSERT, "insufficient funds");
        restarted.onRspEntrust(entrust, error);
        restarted.onRspEntrust(entrust, error);
        error->release();
        while (!callbacks.empty()) {
            auto queued = std::move(callbacks.front()); callbacks.erase(callbacks.begin()); queued();
        }
        check(restarted.liveCommands().at("unknown").status == "rejected", "async duplicate rejection terminates original command");
        auto rejectedState = restarted.liveSnapshot();
        TraderAdapter rejectedRestore;
        rejectedRestore._order_pattern = "otp.test";
        rejectedRestore.configureLive("run-three", 6, "TEST.test1", queue);
        rejectedRestore.restoreLive(rejectedState);
        check(rejectedRestore.liveCommands().at("unknown").status == "rejected", "rejection terminal survives restore without an order report");
        check(rejectedRestore.liveReports().find("insufficient funds") != std::string::npos, "raw rejection remains queryable after restore");
        trader._orders->release(); trader._orders = nullptr;
        entrust->release(); contract->release(); commodity->release();
    }
};
}
int main() {
    try { TraderAdapterLiveTest::run(); std::cout << "TraderAdapter final live gate passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
