#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// The controlled TraderMocker account is the only owner of these monetary facts.
// Money is CNY cents, prices/per-lot fees are millionths of CNY, rates are 1e-8.
class PaperAccount
{
public:
    enum Offset { Open = 0, Today = 1, Yesterday = 2 };
    struct Fee { int64_t per_lot = 0, rate = 0; };
    struct Rules
    {
        std::string contract, source;
        uint32_t trading_day = 0;
        int64_t multiplier = 0, tick = 0, upper_limit = 0, lower_limit = 0;
        std::array<int64_t, 2> margin_rate{}; // long, short; no netting benefit
        std::array<Fee, 3> fees{}; // open, close today, close yesterday
    };
    struct Lot
    {
        bool short_side = false;
        uint32_t opened_day = 0;
        int64_t quantity = 0, opening_price = 0, basis = 0;
    };
    struct Order
    {
        bool short_side = false;
        Offset offset = Open;
        int64_t quantity = 0, remaining = 0, filled = 0, limit_price = 0;
        int64_t frozen_margin = 0, frozen_fee = 0, charged_fee = 0;
        std::string status = "open";
    };
    struct Balance
    {
        int64_t cash = 0, equity = 0, available = 0, margin = 0;
        int64_t frozen_margin = 0, frozen_fee = 0, fees = 0;
        int64_t realized = 0, unrealized = 0;
        int64_t pre_balance = 0, deposit = 0, day_fees = 0, day_realized = 0;
    };
    struct PositionAmounts { int64_t quantity = 0, cost = 0, margin = 0, unrealized = 0; };

    PaperAccount(Rules rules, int64_t initial_cash, int64_t mark);
    void reserve(const std::string& order_id, bool short_side, Offset offset,
                 int64_t quantity, int64_t limit_price);
    void fill(const std::string& order_id, int64_t quantity, int64_t price);
    void cancel(const std::string& order_id);
    void mark(int64_t price);
    void settle(uint32_t day, int64_t official_price);
    void begin_day(Rules rules, int64_t mark);
    Balance balance() const;
    PositionAmounts position_amounts(bool short_side) const;
    int64_t position(bool short_side, Offset bucket, bool available) const;
    const std::map<std::string, Order>& orders() const { return _orders; }
    const std::vector<Lot>& lots() const { return _lots; }
    const Rules& rules() const { return _rules; }
    bool settled() const { return _settled_day == _rules.trading_day; }

    // Complete account state only: caller must bundle matching/CTA/strategy state.
    std::string snapshot() const;
    static PaperAccount restore(const std::string& state);
    static int64_t scaled_decimal(const std::string& value, unsigned places);
    static std::string decimal_string(int64_t value, unsigned places);

private:
    PaperAccount() = default;
    Rules _rules;
    int64_t _cash = 0, _mark = 0, _fees = 0, _realized = 0;
    int64_t _pre_balance = 0, _deposit = 0, _fees_at_day_start = 0, _realized_at_day_start = 0;
    uint32_t _settled_day = 0;
    int64_t _settlement_price = 0;
    std::vector<Lot> _lots;
    std::map<std::string, Order> _orders;
    static void validate(const Rules& rules);
    void price_check(int64_t price) const;
    int64_t fee(Offset offset, int64_t quantity, int64_t price, bool freeze) const;
    int64_t margin(bool short_side, int64_t quantity, int64_t price) const;
    void fill_impl(const std::string& order_id, int64_t quantity, int64_t price);
};
