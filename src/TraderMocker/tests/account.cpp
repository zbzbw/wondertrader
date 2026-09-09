#include "../PaperAccount.h"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value) { if (!value) throw std::runtime_error("Account assertion failed"); }
template<class F> void rejects(F action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    check(rejected);
}
constexpr int64_t price(int64_t value) { return value * 1000000; }
PaperAccount::Rules rules() {
    PaperAccount::Rules r;
    r.contract = "TEST.test1"; r.source = "synthetic-test"; r.trading_day = 20260909;
    r.multiplier = 10; r.tick = price(1); r.lower_limit = price(80); r.upper_limit = price(120);
    r.margin_rate = {10000000, 20000000};
    r.fees = {PaperAccount::Fee{price(2), 0}, PaperAccount::Fee{price(3), 0}, PaperAccount::Fee{price(1), 0}};
    return r;
}
void lifecycle() {
    auto r = rules();
    PaperAccount a(r, 1000000, price(100));
    a.reserve("long", false, PaperAccount::Open, 4, price(100));
    check(a.balance().frozen_margin == 48000 && a.balance().frozen_fee == 800);
    check(a.balance().gross_exposure == 400000 && a.balance().daily_loss == 0);
    a.fill("long", 2, price(100));
    check(a.balance().cash == 999600 && a.balance().margin == 20000);
    check(a.balance().deposit == 1000000 && a.balance().day_fees == 400);
    check(a.balance().gross_exposure == 400000 && a.balance().daily_loss == 400);
    check(a.balance().frozen_margin == 24000 && a.balance().frozen_fee == 400);
    auto restored = PaperAccount::restore(a.snapshot());
    check(restored.snapshot() == a.snapshot());
    for (auto account : {&a, &restored}) {
        account->cancel("long");
        account->reserve("short", true, PaperAccount::Open, 1, price(100));
        account->fill("short", 1, price(100));
        account->mark(price(110));
    }
    check(restored.snapshot() == a.snapshot());
    check(a.balance().cash == 999400 && a.balance().unrealized == 10000);
    check(a.balance().margin == 44000 && a.balance().frozen_margin == 0);
    check(a.balance().gross_exposure == 330000 && a.balance().daily_loss == 0);
    auto before = a.snapshot();
    rejects([&] { a.reserve("huge", false, PaperAccount::Open, 1000, price(100)); });
    rejects([&] { a.reserve("wrong", false, PaperAccount::Yesterday, 1, price(100)); });
    auto tomorrow = r; tomorrow.trading_day = 20260910;
    rejects([&] { a.begin_day(tomorrow, price(110)); });
    check(a.snapshot() == before);
    a.reserve("day-order", false, PaperAccount::Open, 1, price(90));
    auto cashBeforeSettlement = a.balance().cash;
    a.expire_day(r.trading_day);
    check(a.orders().at("day-order").status == "expired" && a.balance().frozen_margin == 0);
    check(a.balance().cash == cashBeforeSettlement && !a.settled());
    rejects([&] { a.begin_day(tomorrow, price(110)); });
    a = PaperAccount::restore(a.snapshot());
    a.settle(r.trading_day, price(110));
    check(a.balance().cash == 1009400 && a.balance().unrealized == 0);
    check(a.position(false, PaperAccount::Yesterday, true) == 2);
    check(a.position(true, PaperAccount::Yesterday, true) == 1);
    check(a.orders().at("day-order").status == "expired");
    before = a.snapshot();
    a.settle(r.trading_day, price(110));
    rejects([&] { a.settle(r.trading_day, price(111)); });
    check(a.snapshot() == before);
    a = PaperAccount::restore(before);
    a.begin_day(tomorrow, price(110));
    check(a.balance().pre_balance == 1009400 && a.balance().deposit == 0
          && a.balance().day_fees == 0 && a.balance().day_realized == 0);
    rejects([&] { a.reserve("wrong-today", false, PaperAccount::Today, 1, price(110)); });
    a.reserve("close-long", false, PaperAccount::Yesterday, 2, price(110));
    check(a.position(false, PaperAccount::Yesterday, true) == 0);
    a.fill("close-long", 1, price(111));
    a.cancel("close-long");
    check(a.balance().cash == 1010300 && a.position(false, PaperAccount::Yesterday, true) == 1);
    a.reserve("close-short", true, PaperAccount::Yesterday, 1, price(110));
    a.fill("close-short", 1, price(109));
    check(a.balance().cash == 1011200 && a.balance().fees == 800);
    check(PaperAccount::restore(a.snapshot()).snapshot() == a.snapshot());
    before = a.snapshot();
    a.reserve("close-short", true, PaperAccount::Yesterday, 1, price(110));
    check(a.snapshot() == before);
    rejects([&] { a.reserve("close-short", true, PaperAccount::Yesterday, 2, price(110)); });
}
void precision() {
    check(PaperAccount::scaled_decimal("123.45", 2) == 12345);
    check(PaperAccount::decimal_string(-1, 2) == "-0.01");
    rejects([] { PaperAccount::scaled_decimal("0.001", 2); });
    rejects([] { PaperAccount::scaled_decimal("9223372036854775808", 0); });
    auto r = rules(); r.fees[0] = {6000, 0}; // 0.006 CNY per fill: round to one cent.
    PaperAccount a(r, 1000000, price(100));
    a.reserve("split", false, PaperAccount::Open, 3, price(100));
    check(a.balance().frozen_fee == 3);
    for (int i = 0; i != 3; ++i) {
        a.fill("split", 1, price(100));
        check(a.balance().fees == i + 1 && a.balance().frozen_fee == 2 - i);
        a = PaperAccount::restore(a.snapshot());
    }
    r.fees[0] = {0, 1000}; // 0.00001 of 100 * 10: one cent.
    PaperAccount proportional(r, 1000000, price(100));
    proportional.reserve("rate", false, PaperAccount::Open, 1, price(100));
    check(proportional.balance().frozen_fee == 2); // upper limit 120, round up.
    proportional.fill("rate", 1, price(100));
    check(proportional.balance().fees == 1);
    auto before = proportional.snapshot();
    rejects([&] { proportional.fill("rate", 1, price(100)); });
    check(proportional.snapshot() == before);
}
}
int main() {
    try { lifecycle(); precision(); std::cout << "Paper account lifecycle and precision passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
