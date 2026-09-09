#include "TraderMocker.h"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/WTSTradeDef.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <set>
#include <stdexcept>

namespace {
void require(bool valid, const char* message) {
    if (!valid) throw std::invalid_argument(message);
}
const rapidjson::Value& field(const rapidjson::Value& value, const char* key) {
    require(value.IsObject() && value.HasMember(key), "Missing controlled state field");
    return value[key];
}
std::string text(const rapidjson::Value& value, const char* key) {
    const auto& item = field(value, key);
    require(item.IsString(), "Controlled state string required");
    return std::string(item.GetString(), item.GetStringLength());
}
uint64_t number(const rapidjson::Value& value, const char* key) {
    const auto& item = field(value, key);
    require(item.IsUint64(), "Controlled state unsigned integer required");
    return item.GetUint64();
}
std::string shortText(const rapidjson::Value& value, const char* key) {
    auto result = text(value, key);
    require(result.size() < 64 && result.find('\0') == std::string::npos, "Invalid WT identifier length");
    return result;
}
template<class T> struct Release { void operator()(T* value) const { if (value) value->release(); } };
}

// This is the TraderMocker component of the complete WtPorter checkpoint.
// CTA, strategy, parent command/report watermarks are bundled by the runner.
std::string TraderMocker::controlled_snapshot()
{
    controlled_barrier();
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> out(buffer);
    auto str = [&](const char* key, const char* value) { out.Key(key); out.String(value); };
    auto num = [&](const char* key, uint64_t value) { out.Key(key); out.Uint64(value); };
    out.StartObject(); num("version", 1);
    num("input_seq", _input_seq); num("event_ms", _event_ms);
    num("mocker_id", _mocker_id); num("order_counter", _auto_order_id);
    num("trade_counter", _auto_trade_id); num("entrust_counter", _auto_entrust_id);
    num("min_qty", static_cast<uint64_t>(_min_qty)); num("max_qty", static_cast<uint64_t>(_max_qty));
    str("account", _paper->snapshot().c_str());
    out.Key("orders"); out.StartArray();
    if (_orders) for (uint32_t index = 0; index != _orders->size(); ++index) {
        const auto order = static_cast<WTSOrderInfo*>(_orders->at(index));
        const auto& fact = _paper->orders().at(order->getEntrustID());
        require(fact.quantity == order->getVolume() && fact.remaining == order->getVolLeft()
                && fact.filled == order->getVolTraded(), "Native order/account quantity mismatch");
        out.StartObject();
        str("id", order->getOrderID()); str("command_id", order->getEntrustID());
        str("tag", order->getUserTag()); str("message", order->getStateMsg());
        num("date", order->getOrderDate()); num("time", order->getOrderTime());
        out.EndObject();
    }
    out.EndArray(); out.Key("trades"); out.StartArray();
    if (_trades) for (uint32_t index = 0; index != _trades->size(); ++index) {
        const auto trade = static_cast<WTSTradeInfo*>(_trades->at(index));
        out.StartObject();
        str("id", trade->getTradeID()); str("order_id", trade->getRefOrder());
        str("tag", trade->getUserTag());
        num("date", trade->getTradeDate()); num("time", trade->getTradeTime());
        num("quantity", static_cast<uint64_t>(trade->getVolume()));
        num("price", paper_price(trade->getPrice()));
        num("fee_cents", _trade_fees.at(trade->getTradeID()));
        out.EndObject();
    }
    out.EndArray(); out.EndObject();
    return std::string(buffer.GetString(), buffer.GetSize());
}

void TraderMocker::controlled_restore(const std::string& state)
{
    controlled_owner();
    require(_bd_mgr && !_controlled_connected && !_draining && !_orders && !_trades && _input_seq == 0,
            "Restore requires a fresh disconnected controlled trader");
    rapidjson::Document doc;
    doc.Parse(state.data(), state.size());
    require(!doc.HasParseError() && doc.IsObject() && number(doc, "version") == 1, "Invalid controlled state version");
    auto account = std::make_unique<PaperAccount>(PaperAccount::restore(text(doc, "account")));
    require(account->rules().contract == _paper->rules().contract, "Restore physical contract mismatch");
    auto inputSeq = number(doc, "input_seq"), eventMs = number(doc, "event_ms");
    auto mocker = number(doc, "mocker_id"), orderCounter = number(doc, "order_counter");
    auto tradeCounter = number(doc, "trade_counter"), entrustCounter = number(doc, "entrust_counter");
    auto minQty = number(doc, "min_qty"), maxQty = number(doc, "max_qty");
    require(mocker <= UINT32_MAX && orderCounter <= UINT32_MAX && tradeCounter <= UINT32_MAX
            && entrustCounter <= UINT32_MAX && minQty == _min_qty && maxQty == _max_qty
            && (inputSeq == 0 || eventMs > 0), "Controlled counter/configuration mismatch");
    const auto& sourceOrders = field(doc, "orders");
    const auto& sourceTrades = field(doc, "trades");
    require(sourceOrders.IsArray() && sourceTrades.IsArray()
            && sourceOrders.Size() == account->orders().size(), "Complete native order/trade arrays required");
    const auto& fullCode = account->rules().contract;
    auto dot = fullCode.find('.');
    auto code = fullCode.substr(dot + 1), exchange = fullCode.substr(0, dot);
    auto contract = _bd_mgr->getContract(code.c_str(), exchange.c_str());
    require(contract != nullptr, "Restore contract missing from base data");
    std::unique_ptr<WTSArray, Release<WTSArray>> orders(WTSArray::create()), trades(WTSArray::create());
    std::unique_ptr<OrderCache, Release<OrderCache>> awaits(OrderCache::create());
    std::map<std::string, WTSOrderInfo*> byOrder;
    std::set<std::string> commands, tradeIds;
    std::map<std::string, uint64_t> traded;
    std::map<std::string, int64_t> tradeFees, paid;
    for (const auto& value : sourceOrders.GetArray()) {
        auto id = shortText(value, "id"), command = shortText(value, "command_id");
        require(!id.empty() && commands.insert(command).second && !byOrder.count(id), "Duplicate native order/command");
        const auto found = account->orders().find(command);
        require(found != account->orders().end(), "Native order lacks account record");
        const auto& fact = found->second;
        require(fact.quantity <= INT32_MAX, "Native whole-lot quantity exceeded");
        auto date = number(value, "date"), time = number(value, "time");
        require(date > 0 && date <= account->rules().trading_day && time <= eventMs, "Invalid order timestamp");
        auto tag = shortText(value, "tag"), message = shortText(value, "message");
        auto order = WTSOrderInfo::create(); orders->append(order, false);
        order->setCode(code.c_str()); order->setExchange(exchange.c_str()); order->setContractInfo(contract);
        order->setOrderID(id.c_str()); order->setEntrustID(command.c_str()); order->setUserTag(tag.c_str());
        order->setStateMsg(message.c_str()); order->setOrderDate(static_cast<uint32_t>(date)); order->setOrderTime(time);
        order->setDirection(fact.short_side ? WDT_SHORT : WDT_LONG);
        order->setOffsetType(fact.offset == PaperAccount::Open ? WOT_OPEN : fact.offset == PaperAccount::Today ? WOT_CLOSETODAY : WOT_CLOSEYESTERDAY);
        order->setPriceType(WPT_LIMITPRICE); order->setOrderFlag(WOF_NOR);
        order->setPrice(fact.limit_price / 1000000.0); order->setVolume(static_cast<double>(fact.quantity));
        order->setVolLeft(static_cast<double>(fact.remaining)); order->setVolTraded(static_cast<double>(fact.filled));
        if (fact.status == "open") {
            order->setOrderState(fact.filled ? WOS_PartTraded_Queuing : WOS_NotTraded_Queuing);
            awaits->add(id, order);
        } else order->setOrderState(fact.status == "filled" ? WOS_AllTraded : WOS_Canceled);
        byOrder.emplace(id, order);
    }
    for (const auto& value : sourceTrades.GetArray()) {
        auto id = shortText(value, "id"), orderId = shortText(value, "order_id");
        require(!id.empty() && tradeIds.insert(id).second && byOrder.count(orderId), "Invalid native trade identity");
        auto order = byOrder.at(orderId);
        auto quantity = number(value, "quantity"), price = number(value, "price");
        auto fee = number(value, "fee_cents");
        auto totalFee = account->orders().at(order->getEntrustID()).charged_fee;
        require(fee <= static_cast<uint64_t>(totalFee) && paid[orderId] <= totalFee - static_cast<int64_t>(fee),
                "Trade fees exceed native charged fees");
        paid[orderId] += static_cast<int64_t>(fee); tradeFees.emplace(id, static_cast<int64_t>(fee));
        auto date = number(value, "date"), time = number(value, "time");
        require(quantity > 0 && quantity <= INT32_MAX && price > 0 && price <= INT64_MAX
                && price % account->rules().tick == 0 && date >= order->getOrderDate()
                && date <= account->rules().trading_day && time >= order->getOrderTime() && time <= eventMs,
                "Invalid native trade fields");
        auto tag = shortText(value, "tag");
        traded[orderId] += quantity;
        require(traded[orderId] <= order->getVolTraded(), "Trade volume exceeds order fills");
        auto trade = WTSTradeInfo::create(code.c_str(), exchange.c_str()); trades->append(trade, false);
        trade->setContractInfo(contract); trade->setTradeID(id.c_str()); trade->setRefOrder(orderId.c_str());
        trade->setUserTag(tag.c_str()); trade->setDirection(order->getDirection()); trade->setOffsetType(order->getOffsetType());
        trade->setVolume(static_cast<double>(quantity)); trade->setPrice(price / 1000000.0);
        trade->setTradeDate(static_cast<uint32_t>(date)); trade->setTradeTime(time);
    }
    for (const auto& entry : byOrder) {
        require(traded[entry.first] == entry.second->getVolTraded(), "Incomplete native trade history");
        require(paid[entry.first] == account->orders().at(entry.second->getEntrustID()).charged_fee, "Incomplete native trade fees");
    }
    // Commit only after the entire component validates. No callbacks or funding.
    _paper = std::move(account); _orders = orders.release(); _trades = trades.release();
    _trade_fees = std::move(tradeFees);
    if (_awaits) _awaits->release();
    _awaits = awaits.release(); _codes.clear();
    if (_orders->size()) _codes.insert(fullCode);
    _input_seq = inputSeq; _event_ms = eventMs; _mocker_id = static_cast<uint32_t>(mocker);
    _auto_order_id = static_cast<uint32_t>(orderCounter); _auto_trade_id = static_cast<uint32_t>(tradeCounter);
    _auto_entrust_id = static_cast<uint32_t>(entrustCounter);
}
