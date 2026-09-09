#include "PaperAccount.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>
#include <boost/multiprecision/cpp_int.hpp>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

namespace {
using Integer = boost::multiprecision::cpp_int;
int64_t narrow(const Integer& value)
{
    if (value > (std::numeric_limits<int64_t>::max)() || value < (std::numeric_limits<int64_t>::min)())
        throw std::overflow_error("Paper account monetary range exceeded");
    return value.convert_to<int64_t>();
}
int64_t rounded(const Integer& numerator, const Integer& denominator, bool ceiling = false)
{
    Integer positive = numerator < 0 ? -numerator : numerator;
    Integer bias = denominator / 2;
    if (ceiling) bias = denominator - 1;
    Integer result = (positive + bias) / denominator;
    return narrow(numerator < 0 ? -result : result);
}
void require(bool valid, const char* message)
{
    if (!valid) throw std::invalid_argument(message);
}
}

int64_t PaperAccount::scaled_decimal(const std::string& value, unsigned places)
{
    require(!value.empty() && places <= 8, "Invalid fixed decimal");
    size_t index = value[0] == '-' ? 1 : 0;
    require(index < value.size(), "Invalid fixed decimal");
    bool decimal = false, digit = false;
    unsigned fraction = 0;
    Integer result = 0;
    for (; index < value.size(); ++index)
    {
        char ch = value[index];
        if (ch == '.' && !decimal && digit) { decimal = true; continue; }
        require(ch >= '0' && ch <= '9', "Invalid fixed decimal character");
        digit = true;
        if (decimal) require(++fraction <= places, "Fixed decimal precision exceeded");
        result = result * 10 + (ch - '0');
    }
    require(digit && (!decimal || fraction > 0), "Invalid fixed decimal fraction");
    for (; fraction < places; ++fraction) result *= 10;
    return narrow(value[0] == '-' ? -result : result);
}

std::string PaperAccount::decimal_string(int64_t value, unsigned places)
{
    require(places <= 8, "Invalid decimal scale");
    Integer positive = value;
    if (positive < 0) positive = -positive;
    std::string result = positive.convert_to<std::string>();
    if (places)
    {
        if (result.size() <= places) result.insert(0, places + 1 - result.size(), '0');
        result.insert(result.size() - places, 1, '.');
    }
    return value < 0 ? "-" + result : result;
}

void PaperAccount::validate(const Rules& rules)
{
    require(!rules.contract.empty() && !rules.source.empty() && rules.trading_day > 0,
            "Contract, effective trading day and rule source required");
    require(rules.multiplier > 0 && rules.tick > 0 && rules.lower_limit > 0
            && rules.upper_limit >= rules.lower_limit, "Invalid contract price rules");
    require(rules.lower_limit % rules.tick == 0 && rules.upper_limit % rules.tick == 0,
            "Price limits must be on the tick grid");
    for (auto rate : rules.margin_rate)
        require(rate > 0 && rate <= 100000000, "Invalid full margin rate");
    for (auto cost : rules.fees)
        require(cost.per_lot >= 0 && cost.rate >= 0 && cost.rate <= 100000000
                && !(cost.per_lot && cost.rate), "Choose per-lot or proportional fees");
}

PaperAccount::PaperAccount(Rules rules, int64_t initial_cash, int64_t mark)
    : _rules(std::move(rules)), _cash(initial_cash), _mark(mark), _deposit(initial_cash)
{
    validate(_rules);
    require(initial_cash > 0, "Positive initial cash required at account creation");
    price_check(mark);
}

void PaperAccount::price_check(int64_t price) const
{
    require(price >= _rules.lower_limit && price <= _rules.upper_limit && price % _rules.tick == 0,
            "Price outside current contract rules");
}

int64_t PaperAccount::fee(Offset offset, int64_t quantity, int64_t price, bool freeze) const
{
    const auto& cost = _rules.fees.at(static_cast<size_t>(offset));
    if (freeze)
        return narrow(Integer(rounded(Integer(cost.per_lot) * 100000000
                      + Integer(price) * _rules.multiplier * cost.rate, Integer(1000000000000LL), true)) * quantity);
    return rounded(Integer(cost.per_lot) * quantity * 100000000
                   + Integer(price) * quantity * _rules.multiplier * cost.rate,
                   Integer(1000000000000LL), freeze);
}

int64_t PaperAccount::margin(bool short_side, int64_t quantity, int64_t price) const
{
    return rounded(Integer(price) * quantity * _rules.multiplier * _rules.margin_rate[short_side],
                   Integer(1000000000000LL), true);
}

int64_t PaperAccount::position(bool short_side, Offset bucket, bool available) const
{
    require(bucket == Today || bucket == Yesterday, "Explicit position bucket required");
    Integer quantity = 0;
    for (const auto& lot : _lots)
        if (lot.short_side == short_side && (lot.opened_day == _rules.trading_day && !settled() ? Today : Yesterday) == bucket)
            quantity += lot.quantity;
    if (available)
        for (const auto& entry : _orders)
            if (entry.second.status == "open" && entry.second.short_side == short_side && entry.second.offset == bucket)
                quantity -= entry.second.remaining;
    return narrow(quantity);
}

PaperAccount::Balance PaperAccount::balance() const
{
    Balance result;
    result.cash = _cash; result.fees = _fees; result.realized = _realized;
    result.pre_balance = _pre_balance; result.deposit = _deposit;
    result.day_fees = narrow(Integer(_fees) - _fees_at_day_start);
    result.day_realized = narrow(Integer(_realized) - _realized_at_day_start);
    Integer used = 0, pnl = 0, frozenMargin = 0, frozenFee = 0;
    for (const auto& lot : _lots)
    {
        used += margin(lot.short_side, lot.quantity, _mark);
        pnl += rounded((Integer(_mark) - lot.basis) * lot.quantity * _rules.multiplier * (lot.short_side ? -1 : 1), 10000);
    }
    for (const auto& entry : _orders)
    {
        frozenMargin += entry.second.frozen_margin;
        frozenFee += entry.second.frozen_fee;
    }
    result.margin = narrow(used); result.unrealized = narrow(pnl);
    result.frozen_margin = narrow(frozenMargin); result.frozen_fee = narrow(frozenFee);
    result.equity = narrow(Integer(_cash) + pnl);
    result.available = narrow(Integer(result.equity) - used - frozenMargin - frozenFee);
    return result;
}

PaperAccount::PositionAmounts PaperAccount::position_amounts(bool short_side) const
{
    Integer quantity = 0, cost = 0, used = 0, pnl = 0;
    for (const auto& lot : _lots) if (lot.short_side == short_side) {
        quantity += lot.quantity;
        cost += Integer(lot.basis) * lot.quantity * _rules.multiplier;
        used += margin(short_side, lot.quantity, _mark);
        pnl += rounded((Integer(_mark) - lot.basis) * lot.quantity * _rules.multiplier * (short_side ? -1 : 1), 10000);
    }
    return {narrow(quantity), rounded(cost, 10000), narrow(used), narrow(pnl)};
}

void PaperAccount::reserve(const std::string& id, bool short_side, Offset offset,
                           int64_t quantity, int64_t limit_price)
{
    require(!id.empty() && quantity > 0 && offset >= Open && offset <= Yesterday, "Invalid paper order");
    auto old = _orders.find(id);
    if (old != _orders.end())
    {
        const auto& order = old->second;
        require(order.short_side == short_side && order.offset == offset && order.quantity == quantity
                && order.limit_price == limit_price, "Paper order identity conflict");
        return; // A known command is never resurrected.
    }
    price_check(limit_price);
    require(!settled(), "Trading day is already settled");
    Order order;
    order.short_side = short_side; order.offset = offset;
    order.quantity = order.remaining = quantity; order.limit_price = limit_price;
    order.frozen_fee = fee(offset, quantity, _rules.upper_limit, true);
    if (offset == Open)
    {
        order.frozen_margin = narrow(Integer(margin(short_side, 1, _rules.upper_limit)) * quantity);
        require(Integer(order.frozen_margin) + order.frozen_fee <= balance().available, "Insufficient paper funds");
    }
    else require(position(short_side, offset, true) >= quantity, "Insufficient available position bucket");
    _orders.emplace(id, std::move(order));
}

void PaperAccount::fill_impl(const std::string& id, int64_t quantity, int64_t price)
{
    auto& order = _orders.at(id);
    require(order.status == "open" && quantity > 0 && quantity <= order.remaining, "Invalid paper fill quantity");
    price_check(price);
    bool buy = order.short_side == (order.offset != Open);
    require(buy ? price <= order.limit_price : price >= order.limit_price, "Fill violates limit price");
    int64_t cost = fee(order.offset, quantity, price, false);
    Integer realized = 0;
    if (order.offset == Open)
        _lots.push_back({order.short_side, _rules.trading_day, quantity, price, price});
    else
    {
        int64_t remaining = quantity;
        for (auto& lot : _lots)
        {
            Offset bucket = lot.opened_day == _rules.trading_day ? Today : Yesterday;
            if (lot.short_side != order.short_side || bucket != order.offset) continue;
            int64_t used = (std::min)(remaining, lot.quantity);
            realized += rounded((Integer(price) - lot.basis) * used * _rules.multiplier * (order.short_side ? -1 : 1), 10000);
            lot.quantity -= used; remaining -= used;
            if (!remaining) break;
        }
        require(remaining == 0, "Fill exceeds frozen position");
        _lots.erase(std::remove_if(_lots.begin(), _lots.end(), [](const Lot& lot) { return lot.quantity == 0; }), _lots.end());
    }
    _cash = narrow(Integer(_cash) + realized - cost);
    _fees = narrow(Integer(_fees) + cost); _realized = narrow(Integer(_realized) + realized);
    order.charged_fee = narrow(Integer(order.charged_fee) + cost);
    order.remaining -= quantity; order.filled += quantity;
    order.frozen_fee = fee(order.offset, order.remaining, _rules.upper_limit, true);
    order.frozen_margin = order.offset == Open ? narrow(Integer(margin(order.short_side, 1, _rules.upper_limit)) * order.remaining) : 0;
    if (!order.remaining) order.status = "filled";
    _mark = price;
    balance(); // Detect an unrepresentable account before publishing this event.
}

void PaperAccount::fill(const std::string& id, int64_t quantity, int64_t price)
{
    auto next = *this;
    next.fill_impl(id, quantity, price);
    *this = std::move(next);
}

void PaperAccount::cancel(const std::string& id)
{
    auto& order = _orders.at(id);
    if (order.status != "open") return;
    order.status = "cancelled";
    order.frozen_margin = order.frozen_fee = 0;
}

void PaperAccount::mark(int64_t price)
{
    price_check(price);
    auto next = *this;
    next._mark = price; next.balance();
    *this = std::move(next);
}

void PaperAccount::settle(uint32_t day, int64_t official_price)
{
    if (day == _settled_day)
    {
        require(official_price == _settlement_price, "Conflicting settlement price");
        return;
    }
    require(day == _rules.trading_day, "Settlement trading day mismatch");
    price_check(official_price);
    auto next = *this;
    next._mark = official_price;
    int64_t pnl = next.balance().unrealized;
    next._cash = narrow(Integer(next._cash) + pnl);
    next._realized = narrow(Integer(next._realized) + pnl);
    for (auto& lot : next._lots) lot.basis = official_price;
    for (auto& entry : next._orders)
        if (entry.second.status == "open")
        {
            entry.second.status = "expired";
            entry.second.frozen_margin = entry.second.frozen_fee = 0;
        }
    next._settled_day = day; next._settlement_price = official_price;
    next.balance();
    *this = std::move(next);
}

void PaperAccount::begin_day(Rules rules, int64_t price)
{
    validate(rules);
    require(settled() && rules.trading_day > _rules.trading_day && rules.contract == _rules.contract
            && rules.multiplier == _rules.multiplier && rules.tick == _rules.tick,
            "Prior settlement and same physical contract required for next day");
    auto next = *this;
    next._pre_balance = next._cash; next._deposit = 0;
    next._fees_at_day_start = next._fees; next._realized_at_day_start = next._realized;
    next._rules = std::move(rules); next.mark(price);
    *this = std::move(next);
}

std::string PaperAccount::snapshot() const
{
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> out(buffer);
    auto number = [&](const char* key, int64_t value) {
        out.Key(key); auto text = std::to_string(value); out.String(text.c_str());
    };
    out.StartObject(); out.Key("version"); out.Int(1);
    out.Key("rules"); out.StartObject();
    out.Key("contract"); out.String(_rules.contract.c_str());
    out.Key("source"); out.String(_rules.source.c_str());
    number("trading_day", _rules.trading_day); number("multiplier", _rules.multiplier);
    number("tick", _rules.tick); number("upper_limit", _rules.upper_limit); number("lower_limit", _rules.lower_limit);
    number("long_margin_rate", _rules.margin_rate[0]); number("short_margin_rate", _rules.margin_rate[1]);
    out.Key("fees"); out.StartArray();
    for (const auto& cost : _rules.fees)
    {
        out.StartObject(); number("per_lot", cost.per_lot); number("rate", cost.rate); out.EndObject();
    }
    out.EndArray(); out.EndObject();
    number("cash", _cash); number("mark", _mark); number("fees", _fees); number("realized", _realized);
    number("pre_balance", _pre_balance); number("deposit", _deposit);
    number("fees_at_day_start", _fees_at_day_start); number("realized_at_day_start", _realized_at_day_start);
    number("settled_day", _settled_day); number("settlement_price", _settlement_price);
    out.Key("lots"); out.StartArray();
    for (const auto& lot : _lots)
    {
        out.StartObject(); out.Key("short"); out.Bool(lot.short_side);
        number("opened_day", lot.opened_day); number("quantity", lot.quantity);
        number("opening_price", lot.opening_price); number("basis", lot.basis); out.EndObject();
    }
    out.EndArray(); out.Key("orders"); out.StartArray();
    for (const auto& entry : _orders)
    {
        const auto& order = entry.second;
        out.StartObject(); out.Key("id"); out.String(entry.first.c_str());
        out.Key("short"); out.Bool(order.short_side); number("offset", order.offset);
        number("quantity", order.quantity); number("remaining", order.remaining); number("filled", order.filled);
        number("limit_price", order.limit_price); number("frozen_margin", order.frozen_margin);
        number("frozen_fee", order.frozen_fee); number("charged_fee", order.charged_fee);
        out.Key("status"); out.String(order.status.c_str()); out.EndObject();
    }
    out.EndArray(); out.EndObject();
    return std::string(buffer.GetString(), buffer.GetSize());
}

PaperAccount PaperAccount::restore(const std::string& state)
{
    rapidjson::Document doc;
    doc.Parse(state.data(), state.size());
    require(!doc.HasParseError() && doc.IsObject() && doc.HasMember("version")
            && doc["version"].IsInt() && doc["version"].GetInt() == 1, "Invalid paper state version");
    auto field = [](const rapidjson::Value& object, const char* key) -> const rapidjson::Value& {
        require(object.IsObject() && object.HasMember(key), "Missing paper state field");
        return object[key];
    };
    auto text = [&](const rapidjson::Value& object, const char* key) {
        const auto& value = field(object, key);
        require(value.IsString(), "Paper state string required");
        return std::string(value.GetString(), value.GetStringLength());
    };
    auto number = [&](const rapidjson::Value& object, const char* key) { return scaled_decimal(text(object, key), 0); };
    auto day = [&](const rapidjson::Value& object, const char* key) {
        auto value = number(object, key);
        require(value >= 0 && value <= UINT32_MAX, "Invalid paper trading day");
        return static_cast<uint32_t>(value);
    };
    auto side = [&](const rapidjson::Value& object) {
        const auto& value = field(object, "short");
        require(value.IsBool(), "Paper position side required");
        return value.GetBool();
    };
    PaperAccount account;
    auto& rules = account._rules;
    const auto& source = field(doc, "rules");
    rules.contract = text(source, "contract"); rules.source = text(source, "source");
    rules.trading_day = day(source, "trading_day"); rules.multiplier = number(source, "multiplier");
    rules.tick = number(source, "tick"); rules.upper_limit = number(source, "upper_limit");
    rules.lower_limit = number(source, "lower_limit");
    rules.margin_rate = {number(source, "long_margin_rate"), number(source, "short_margin_rate")};
    const auto& costs = field(source, "fees");
    require(costs.IsArray() && costs.Size() == 3, "Complete open/today/yesterday fees required");
    for (size_t i = 0; i != 3; ++i)
        rules.fees[i] = {number(costs[static_cast<rapidjson::SizeType>(i)], "per_lot"), number(costs[static_cast<rapidjson::SizeType>(i)], "rate")};
    validate(rules);
    account._cash = number(doc, "cash"); account._mark = number(doc, "mark");
    account._fees = number(doc, "fees"); account._realized = number(doc, "realized");
    account._pre_balance = number(doc, "pre_balance"); account._deposit = number(doc, "deposit");
    account._fees_at_day_start = number(doc, "fees_at_day_start"); account._realized_at_day_start = number(doc, "realized_at_day_start");
    require(account._deposit >= 0 && account._fees_at_day_start >= 0 && account._fees_at_day_start <= account._fees
            && Integer(account._pre_balance) + account._deposit + account._realized - account._realized_at_day_start
               - account._fees + account._fees_at_day_start == account._cash, "Daily paper balance mismatch");
    account._settled_day = day(doc, "settled_day"); account._settlement_price = number(doc, "settlement_price");
    require(account._fees >= 0 && account._settled_day <= rules.trading_day, "Invalid paper account totals");
    require(account._settled_day ? (account._settlement_price > 0 && account._settlement_price % rules.tick == 0)
                                : account._settlement_price == 0, "Invalid settlement state");
    account.price_check(account._mark);
    const auto& lots = field(doc, "lots");
    require(lots.IsArray(), "Paper lot array required");
    for (const auto& value : lots.GetArray())
    {
        Lot lot{side(value), day(value, "opened_day"), number(value, "quantity"),
                number(value, "opening_price"), number(value, "basis")};
        require(lot.opened_day > 0 && lot.opened_day <= rules.trading_day && lot.quantity > 0
                && lot.opening_price > 0 && lot.basis > 0 && lot.opening_price % rules.tick == 0
                && lot.basis % rules.tick == 0, "Invalid native lot state");
        account._lots.push_back(lot);
    }
    const auto& orders = field(doc, "orders");
    require(orders.IsArray(), "Paper order array required");
    Integer totalFees = 0;
    for (const auto& value : orders.GetArray())
    {
        auto offset = number(value, "offset");
        require(offset >= Open && offset <= Yesterday, "Invalid order offset");
        Order order;
        order.short_side = side(value); order.offset = static_cast<Offset>(offset);
        order.quantity = number(value, "quantity"); order.remaining = number(value, "remaining");
        order.filled = number(value, "filled"); order.limit_price = number(value, "limit_price");
        order.frozen_margin = number(value, "frozen_margin"); order.frozen_fee = number(value, "frozen_fee");
        order.charged_fee = number(value, "charged_fee"); order.status = text(value, "status");
        require(order.limit_price > 0 && order.limit_price % rules.tick == 0, "Invalid historical order price");
        require(order.quantity > 0 && order.remaining >= 0 && order.filled >= 0
                && Integer(order.remaining) + order.filled == order.quantity && order.charged_fee >= 0,
                "Invalid order quantity or fees");
        require(order.status == "open" || order.status == "filled" || order.status == "cancelled" || order.status == "expired",
                "Invalid order status");
        bool active = order.status == "open";
        require((!active || (order.remaining > 0 && !account.settled()))
                && (order.status != "filled" || order.remaining == 0), "Invalid active order state");
        int64_t frozenMargin = active && order.offset == Open ? narrow(Integer(account.margin(order.short_side, 1, rules.upper_limit)) * order.remaining) : 0;
        int64_t frozenFee = active ? account.fee(order.offset, order.remaining, rules.upper_limit, true) : 0;
        require(order.frozen_margin == frozenMargin && order.frozen_fee == frozenFee, "Frozen order funds mismatch");
        auto id = text(value, "id");
        require(!id.empty() && account._orders.emplace(id, order).second, "Duplicate or empty order ID");
        totalFees += order.charged_fee;
    }
    require(totalFees == account._fees, "Cumulative fees mismatch");
    for (bool shortSide : {false, true})
        for (auto bucket : {Today, Yesterday})
            require(account.position(shortSide, bucket, true) >= 0, "Over-reserved position bucket");
    account.balance();
    return account;
}
