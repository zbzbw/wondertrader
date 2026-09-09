// Direct native matching regression. No threads, sockets, broker or real data.
#include "../TraderMocker.h"
#include "../../Includes/IBaseDataMgr.h"
#include "../../Includes/WTSContractInfo.hpp"
#include "../../Includes/WTSTradeDef.hpp"
#include "../../Includes/WTSDataDef.hpp"
#include "../../Includes/WTSVariant.hpp"
#include <iostream>
#include <stdexcept>

std::vector<uint32_t> splitVolume(uint32_t, uint32_t, uint32_t);

static void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class Data : public IBaseDataMgr
{
public:
    WTSCommodityInfo* commodity = WTSCommodityInfo::create("test", "Test", "TEST", "ALLDAY", "CHINA");
    WTSContractInfo* contract = WTSContractInfo::create("test1", "Test", "TEST", "test");
    Data() { commodity->setCoverMode(CM_CoverToday); contract->setCommInfo(commodity); }
    ~Data() { contract->release(); commodity->release(); }
    WTSCommodityInfo* getCommodity(const char*) override { return commodity; }
    WTSCommodityInfo* getCommodity(const char*, const char*) override { return commodity; }
    WTSContractInfo* getContract(const char*, const char*, uint32_t) override { return contract; }
    WTSArray* getContracts(const char*, uint32_t) override { return nullptr; }
    WTSSessionInfo* getSession(const char*) override { return nullptr; }
    WTSSessionInfo* getSessionByCode(const char*, const char*) override { return nullptr; }
    WTSArray* getAllSessions() override { return nullptr; }
    bool isHoliday(const char*, uint32_t, bool) override { return false; }
    uint32_t calcTradingDate(const char*, uint32_t date, uint32_t, bool) override { return date; }
    uint64_t getBoundaryTime(const char*, uint32_t, bool, bool) override { return 0; }
};

class TraderMockerMatchingTest
{
public:
    static void run()
    {
        check(splitVolume(8, 2, 3) == std::vector<uint32_t>({3, 3, 2}), "stable splitting");
        check(splitVolume(1, 2, 3) == std::vector<uint32_t>({1}), "residual splitting");
        check(splitVolume(0, 1, 3).empty(), "no zero fills");
        auto invalid = WTSVariant::createObject();
        invalid->append("minqty", 3); invalid->append("maxqty", 2);
        { TraderMocker rejected; check(!rejected.init(invalid), "invalid quantity bounds"); }
        invalid->release();
        Data data;
        TraderMocker trader;
        trader._bd_mgr = &data;
        trader._mocker_id = 1;
        trader._min_qty = 1;
        trader._max_qty = 2;
        trader._use_newpx = false;
        trader._pos_file = "matching-positions.json";
        trader._orders = WTSArray::create();
        trader._awaits = TraderMocker::OrderCache::create();
        trader._ticks = TraderMocker::TickCache::create();
        trader._codes.insert("TEST.test1");
        auto add = [&](const char* id, const char* code, const char* exchange, double price) {
            auto order = WTSOrderInfo::create();
            order->setContractInfo(data.contract);
            order->setCode(code); order->setExchange(exchange); order->setOrderID(id);
            order->setDirection(WDT_LONG); order->setOffsetType(WOT_OPEN);
            order->setVolume(4); order->setVolLeft(4); order->setPrice(price);
            order->setPriceType(WPT_LIMITPRICE); order->setOrderState(WOS_NotTraded_Queuing);
            trader._orders->append(order, false); trader._awaits->add(id, order);
            return order;
        };
        auto foreignCode = add("foreign-code", "other", "TEST", 100);
        auto foreignExchange = add("foreign-exchange", "test1", "OTHER", 100);
        auto offPrice = add("off-price", "test1", "TEST", 99);
        // Lexical order differs deliberately; matching must use acceptance order.
        auto first = add("z-first", "test1", "TEST", 100);
        auto second = add("a-second", "test1", "TEST", 100);
        auto tick = [&](double askVolume, double bidVolume) {
            WTSTickStruct value{};
            strcpy(value.code, "test1"); strcpy(value.exchg, "TEST");
            value.price = value.ask_prices[0] = value.bid_prices[0] = 100;
            value.ask_qty[0] = askVolume; value.bid_qty[0] = bidVolume;
            auto input = WTSTickData::create(value);
            trader._ticks->add("TEST.test1", input); input->release();
            trader.match_once();
        };
        tick(5, 0);
        check(first->getVolTraded() == 4 && first->getVolLeft() == 0, "FIFO first fill");
        check(second->getVolTraded() == 1 && second->getVolLeft() == 3, "shared liquidity partial fill");
        check(foreignCode->getVolLeft() == 4 && foreignExchange->getVolLeft() == 4, "contract isolation");
        check(offPrice->getVolLeft() == 4, "limit order price");
        check(trader._trades->size() == 3, "fixed split count");
        tick(0, 0);
        check(second->getVolTraded() == 1, "zero liquidity does not fill");
        // A cancelled residual stays in order history but cannot match again.
        auto cancel = WTSEntrustAction::create("test1", "TEST");
        cancel->setOrderID(second->getOrderID());
        trader.orderAction(cancel); cancel->release();
        trader._io_service.poll();
        check(second->getOrderState() == WOS_Canceled, "native cancel callback");
        tick(10, 0);
        check(second->getVolTraded() == 1 && second->getVolLeft() == 3, "cancelled residual");
        check(trader._positions["TEST.test1"]._long._volume == 5, "position conservation");
        auto sellFirst = add("sell-first", "test1", "TEST", 100);
        auto sellSecond = add("sell-second", "test1", "TEST", 100);
        sellFirst->setDirection(WDT_SHORT); sellSecond->setDirection(WDT_SHORT);
        tick(0, 3);
        check(sellFirst->getVolTraded() == 3 && sellSecond->getVolTraded() == 0, "shared bid liquidity");
        tick(0, 2);
        check(sellFirst->getVolTraded() == 4 && sellSecond->getVolTraded() == 1, "partial fill resumes FIFO");
        check(trader._positions["TEST.test1"]._short._volume == 5, "short conservation");
        trader._awaits->release(); trader._awaits = nullptr;
        std::cout << "TraderMocker matching regression passed\n";
    }
};

class TraderMockerAccountTest
{
public:
    static void run()
    {
        Data data;
        TraderMocker trader;
        trader._bd_mgr = &data; trader._mocker_id = 2;
        trader._min_qty = 1; trader._max_qty = 2; trader._use_newpx = false;
        PaperAccount::Rules rules;
        rules.contract = "TEST.test1"; rules.source = "native-test"; rules.trading_day = 20260909;
        rules.multiplier = 10; rules.tick = 1000000; rules.lower_limit = 80000000; rules.upper_limit = 120000000;
        rules.margin_rate = {10000000, 20000000};
        rules.fees = {PaperAccount::Fee{2000000, 0}, PaperAccount::Fee{3000000, 0}, PaperAccount::Fee{1000000, 0}};
        trader._paper.reset(new PaperAccount(rules, 1000000, 100000000));
        trader._ticks = TraderMocker::TickCache::create();
        trader.connect();
        check(trader.login("synthetic", "", "") == 0, "controlled login");
        trader.controlled_step(1, 1788915600000ULL, nullptr);
        check(!trader._thrd_worker && !trader._thrd_match && !trader._b_socket, "controlled mode has no timer, socket or worker");
        auto drain = [&]() { trader.controlled_barrier(); };
        auto submit = [&](const char* id, double volume, WTSOffsetType offset) {
            auto entrust = WTSEntrust::create("test1", volume, 100, "TEST");
            entrust->setContractInfo(data.contract); entrust->setEntrustID(id);
            entrust->setDirection(WDT_LONG); entrust->setOffsetType(offset);
            entrust->setPriceType(WPT_LIMITPRICE);
            trader.orderInsert(entrust); entrust->release(); drain();
        };
        submit("accepted", 4, WOT_OPEN);
        check(trader._orders->size() == 1 && trader._paper->balance().frozen_margin == 48000, "native reserve");
        submit("accepted", 4, WOT_OPEN);
        submit("insufficient", 1000, WOT_OPEN);
        submit("wrong-bucket", 1, WOT_CLOSEYESTERDAY);
        check(trader._orders->size() == 1, "native idempotent/rejected order creates no extra order");
        WTSTickStruct value{};
        strcpy(value.code, "test1"); strcpy(value.exchg, "TEST");
        value.price = value.ask_prices[0] = value.bid_prices[0] = 100;
        value.trading_date = 20260909;
        value.ask_qty[0] = 3; value.bid_qty[0] = 0;
        auto tick = WTSTickData::create(value);
        trader.controlled_step(2, 1788915601000ULL, tick); tick->release();
        auto order = static_cast<WTSOrderInfo*>(trader._orders->at(0));
        check(order->getVolTraded() == 3 && order->getVolLeft() == 1, "native partial volume");
        check(order->getOrderTime() == 1788915600000ULL, "submission uses explicit event time");
        check(static_cast<WTSTradeInfo*>(trader._trades->at(0))->getTradeTime() == 1788915601000ULL,
              "fill uses explicit event time");
        check(trader._paper->balance().cash == 999400 && trader._paper->balance().frozen_margin == 12000,
              "native matching debits actual fees and releases filled reservation");
        auto checkpoint = trader.controlled_snapshot();
        struct Listener : ITraderSpi {
            Data* data;
            std::function<void(WTSArray*)> account;
            std::function<void(const WTSArray*)> positions;
            IBaseDataMgr* getBaseDataMgr() override { return data; }
            void handleEvent(WTSTraderEvent, int32_t) override {}
            void onLoginResult(bool, const char*, uint32_t) override {}
            void onRspAccount(WTSArray* values) override { account(values); }
            void onRspPosition(const WTSArray* values) override { positions(values); }
        } listener;
        listener.data = &data;
        unsigned queries = 0;
        listener.account = [&](WTSArray* values) {
            check(trader._mutex_api.try_lock(), "controlled callbacks hold no API mutex"); trader._mutex_api.unlock();
            check(trader._mtx_awaits.try_lock(), "controlled callbacks hold no order mutex"); trader._mtx_awaits.unlock();
            auto balance = static_cast<WTSAccountInfo*>(values->at(0));
            check(balance->getBalance() == 9994 && balance->getCommission() == 6
                  && balance->getDeposit() == 10000 && balance->getFrozenMargin() == 120,
                  "native account query exposes static cash, daily fees, deposit and remaining margin");
            ++queries;
        };
        listener.positions = [&](const WTSArray* values) {
            auto position = static_cast<WTSPositionItem*>(*values->begin());
            check(position->getNewPosition() == 3 && position->getPositionCost() == 3000
                  && position->getMargin() == 300 && position->getAvgPrice() == 100,
                  "native position query includes quantity, basis, margin and average");
            ++queries;
        };
        trader.registerSpi(&listener);
        trader.queryAccount(); trader.queryPositions(); drain();
        check(queries == 2 && trader.controlled_snapshot() == checkpoint, "queries are side effect free");
        trader._listener = nullptr;
        TraderMocker resumed;
        resumed._bd_mgr = &data; resumed._min_qty = 1; resumed._max_qty = 2; resumed._use_newpx = false;
        resumed._paper.reset(new PaperAccount(rules, 1, 100000000));
        auto incomplete = checkpoint;
        auto qtyField = incomplete.find("\"quantity\":2");
        check(qtyField != std::string::npos, "fixture contains actual split trade");
        incomplete.replace(qtyField, std::string("\"quantity\":2").size(), "\"quantity\":4");
        bool badState = false;
        try { resumed.controlled_restore(incomplete); } catch (const std::invalid_argument&) { badState = true; }
        check(badState && !resumed._orders && resumed._paper->balance().cash == 1, "inconsistent native history rejected atomically");
        resumed.controlled_restore(checkpoint);
        check(!resumed.isConnected() && resumed.controlled_snapshot() == checkpoint,
              "full matching/account component restores without connecting or adding initial funds");
        resumed.connect(); resumed.controlled_barrier();
        auto cancel = WTSEntrustAction::create("test1", "TEST");
        cancel->setOrderID(order->getOrderID());
        trader.orderAction(cancel); resumed.orderAction(cancel); cancel->release(); drain(); resumed.controlled_barrier();
        check(trader.controlled_snapshot() == resumed.controlled_snapshot(), "restored pending cancellation matches uninterrupted state");
        check(trader._paper->balance().frozen_margin == 0 && trader._paper->balance().frozen_fee == 0,
              "native cancel releases only residual");
        check(trader._paper->position(false, PaperAccount::Today, true) == 3, "native today position");
        check(PaperAccount::restore(trader._paper->snapshot()).snapshot() == trader._paper->snapshot(),
              "native generated account restores without new funds");
        submit("expire", 2, WOT_OPEN);
        trader.controlled_settle(20260909, 110000000);
        check(trader._paper->position(false, PaperAccount::Yesterday, true) == 3, "controlled settlement rolls today");
        auto expired = static_cast<WTSOrderInfo*>(trader._orders->at(1));
        check(expired->getOrderState() == WOS_Canceled && trader._paper->orders().at("expire").status == "expired",
              "DAY expiry changes matching and account together");
        auto before = trader._paper->snapshot();
        trader.controlled_settle(20260909, 110000000);
        check(trader._paper->snapshot() == before, "duplicate controlled settlement");
        rules.trading_day = 20260910;
        trader.controlled_begin_day(rules, 110000000);
        bool rejected = false;
        try { trader.controlled_step(4, 1788915602000ULL, nullptr); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected && trader._input_seq == 2, "input gaps rejected");
        trader.controlled_step(3, 1788915602000ULL, nullptr);
        std::cout << "TraderMocker account integration passed\n";
    }
};

int main()
{
    try { TraderMockerMatchingTest::run(); TraderMockerAccountTest::run(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
