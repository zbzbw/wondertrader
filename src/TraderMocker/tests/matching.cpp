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

int main()
{
    try { TraderMockerMatchingTest::run(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
