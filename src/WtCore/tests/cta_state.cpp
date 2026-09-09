#include "../CtaStraBaseCtx.h"
#include <iostream>
#include <stdexcept>

USING_NS_WTP;
namespace {
void check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
template<class F> void rejects(F action) {
    bool failed = false; try { action(); } catch (const std::exception&) { failed = true; }
    check(failed, "invalid CTA checkpoint must reject");
}
class Context : public CtaStraBaseCtx {
public:
    Context(const char* name = "cross") : CtaStraBaseCtx(nullptr, name, 1) {}
    void on_bar_close(const char*, const char*, WTSBarStruct*) override { ++callbacks; }
    void on_calculate(uint32_t, uint32_t) override { ++callbacks; }
    unsigned callbacks = 0;
    void scheduling(bool active) { _is_in_schedule = active; }
    void populate() {
        _main_key = "SHFE.rb.2609#m5"; _main_code = "SHFE.rb.2609"; _main_period = "m5";
        _kline_tags[_main_key]._closed = true; _kline_tags[_main_key]._notify = true;
        _price_map[_main_code] = 3500.125; _price_map["a"] = 0.1;
        PosInfo p; p._volume = 2; p._closeprofit = 17; p._dynprofit = 3;
        p._last_entertime = 202609092105ULL; p._last_exittime = 202609090935ULL;
        p._frozen = 1; p._frozen_date = 20260910;
        DetailInfo d; d._long = true; d._price = 3400.125; d._volume = 2;
        d._opentime = p._last_entertime; d._opentdate = 20260910;
        d._max_profit = 21; d._max_loss = -10; d._max_price = 3510; d._min_price = 3390;
        d._profit = 3; strcpy(d._opentag, "entry:42"); d._open_barno = 42;
        p._details.push_back(d); _pos_map[_main_code] = p;
        SigInfo signal; signal._volume = 0; signal._usertag = "close:43"; signal._sigprice = 3500.125;
        signal._sigtype = 2; signal._gentime = 202609092110ULL; signal._triggered = true;
        _sig_map[_main_code] = signal;
        CondEntrust condition; condition._field = WCF_BIDPRICE; condition._alg = WCT_SmallerOrEqual;
        condition._target = 3490; condition._qty = 2; condition._action = COND_ACTION_CL;
        strcpy(condition._code, _main_code.c_str()); strcpy(condition._usertag, "stop:43");
        _condtions[_main_code].push_back(condition);
        _last_cond_min = 202609092110ULL; _last_barno = 43; _emit_times = 44;
        _total_calc_time = 123456; _user_datas["strategy"] = "checkpoint"; _ud_modified = true;
        _fund_info._total_profit = 17; _fund_info._total_dynprofit = 3; _fund_info._total_fees = 1.25;
        _tick_subs.insert(_main_code); _barevt_subs.insert(_main_key);
        _chart_code = _main_code; _chart_period = _main_period;
        ChartIndex index; index._name = "ma"; index._indexType = 1;
        ChartLine line; line._name = "fast"; line._lineType = 2;
        index._lines.emplace("fast", line); index._base_lines["zero"] = 0;
        _chart_indice.emplace("ma", index);
    }
    void checkRestored() {
        check(_sig_map.at(_main_code)._triggered && _sig_map.at(_main_code)._sigtype == 2,
              "restored pending condition signal must retain its execution semantics");
        const auto& p = _pos_map.at(_main_code);
        check(p._details.at(0)._open_barno == 42 && p._details.at(0)._max_loss == -10
              && p._frozen_date == 20260910 && p._frozen == 1, "position history and frozen date must survive");
        check(_kline_tags.at(_main_key)._closed && _last_barno == 43 && _emit_times == 44,
              "closed bar scheduling state must survive");
        check(_total_calc_time == 0 && callbacks == 0, "restore must not execute strategy callbacks or restore wall time");
    }
};
}
int main() {
    try {
        Context original; original.populate(); const auto state = original.liveSnapshot();
        Context resumed; const auto handle = resumed.id(); resumed.restoreLive(state);
        check(resumed.id() == handle, "restore must preserve this process context handle");
        resumed.checkRestored(); check(resumed.liveSnapshot() == state, "complete CTA checkpoint round trip");
        auto invalid = state;
        auto pos = invalid.find("\"zero\":0.0"); // Corrupt a late field to exercise all-or-nothing restore.
        check(pos != std::string::npos, "fixture baseline field");
        invalid.replace(pos, 10, "\"zero\":false");
        rejects([&] { resumed.restoreLive(invalid); });
        check(resumed.liveSnapshot() == state, "failed restore must leave live state unchanged");
        Context different("other"); rejects([&] { different.restoreLive(state); });
        resumed.scheduling(true); rejects([&] { resumed.liveSnapshot(); }); rejects([&] { resumed.restoreLive(state); });
        resumed.scheduling(false);
        Context next; check(next.id() == different.id() + 1, "restore must not consume native context identifiers");
        std::cout << "CTA live checkpoint regression passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
