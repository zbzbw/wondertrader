"""Real DLL regression; each checkpoint branch runs in a fresh process.

Usage: python controlled.py WtPorter.dll TraderMocker.dll output-directory
No broker, sockets, timer, wtpy installation or account credentials are used.
"""
import ctypes as C
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


class Bar(C.Structure):
    _fields_ = [("date", C.c_uint32), ("reserve_", C.c_uint32), ("time", C.c_uint64)] + [
        (name, C.c_double) for name in "open high low close settle money vol hold add".split()
    ]


class Tick(C.Structure):
    _fields_ = [("exchg", C.c_char * 16), ("code", C.c_char * 32)] + [
        (name, C.c_double) for name in (
            "price open high low settle_price upper_limit lower_limit total_volume volume "
            "total_turnover turn_over open_interest diff_interest"
        ).split()
    ] + [(name, C.c_uint32) for name in "trading_date action_date action_time reserve_".split()] + [
        (name, C.c_double) for name in "pre_close pre_settle pre_interest".split()
    ] + [(name, C.c_double * 10) for name in "bid_prices ask_prices bid_qty ask_qty".split()]


def encoded(value):
    return json.dumps(value, separators=(",", ":")).encode()


def write(path, value):
    Path(path).write_bytes(encoded(value))


def child(porter, mocker, directory, restore=None, scenario="tick"):
    directory.mkdir(parents=True, exist_ok=True)
    os.chdir(directory)
    Path("traders").mkdir(exist_ok=True)
    shutil.copy2(mocker, "traders/TraderMocker.dll")
    dll = C.CDLL(str(porter))
    for name in "configure start arm submit cancel query ack restore".split():
        fn = getattr(dll, "wt_live_" + name)
        fn.restype = C.c_char_p
        fn.argtypes = [C.c_char_p]
    for name in "connect snapshot".split():
        fn = getattr(dll, "wt_live_" + name)
        fn.restype = C.c_char_p
        fn.argtypes = []
    dll.wt_live_step.restype = dll.wt_live_warmup.restype = C.c_char_p
    dll.wt_live_step.argtypes = [C.c_char_p, C.POINTER(Tick), C.POINTER(Bar)]
    dll.wt_live_warmup.argtypes = [C.POINTER(Bar)]
    dll.init_porter.argtypes = [C.c_char_p, C.c_bool, C.c_char_p]
    dll.config_porter.argtypes = [C.c_char_p, C.c_bool]
    dll.create_cta_context.argtypes = [C.c_char_p, C.c_int]
    dll.create_cta_context.restype = C.c_uint32

    def call(name, *args):
        response = json.loads(getattr(dll, "wt_live_" + name)(*args))
        assert response["ok"], (name, response)
        return response["data"]

    write("sessions.json", {"day": {"name": "day", "offset": 0, "sections": [{"from": 900, "to": 1500}]}})
    write("commodities.json", {"SHFE": {"rb": {
        "name": "rb", "session": "day", "holiday": "CHINA", "volscale": 10,
        "pricetick": 1, "precision": 0, "covermode": 1, "pricemode": 0, "trademode": 0,
    }}})
    write("contracts.json", {"SHFE": {"rb2609": {"name": "rb2609", "exchg": "SHFE", "product": "rb"}}})
    write("policy.json", {"default": {"order": [{"action": "open", "limit": 100}]}})
    rules = dict(contract="SHFE.rb2609", source="synthetic-test", trading_day=20260909,
                 multiplier="10", tick="1", upper_limit="4000", lower_limit="3000",
                 long_margin_rate="0.1", short_margin_rate="0.1", initial_cash="100000", mark="3500")
    for offset in ("open", "today", "yesterday"):
        rules[offset + "_fee_per_lot"] = "2"
        rules[offset + "_fee_rate"] = "0"
    config = {
        "basefiles": {"session": "sessions.json", "commodity": "commodities.json", "contract": "contracts.json"},
        "env": {"name": "cta", "controlled": True, "poolsize": 0, "product": {"session": "day"}},
        "data": {}, "bspolicy": "policy.json", "parsers": [], "executers": [],
        "traders": [{"id": "paper", "active": True, "module": "TraderMocker", "mockerid": 1, "account": rules}],
    }
    dll.init_porter(encoded({"root": {"level": "warn", "sinks": [{"type": "console_sink", "pattern": "%v"}]}}), False, b"generated/")
    dll.config_porter(encoded(config), False)
    dll.create_cta_context(b"cross", 0)
    identity = dict(run_id="restored" if restore else "original", generation=2 if restore else 1)
    call("configure", encoded(dict(mode="paper", instrument_id="SHFE.rb.2609", trader_id="paper",
                                   event_ms=1, received_at="synthetic:1", trading_day=20260909, **identity)))
    if restore:
        call("restore", restore.read_bytes())
    connected = call("connect")
    assert connected["ready"] and connected["connected"], connected
    if not restore:
        bar = Bar(date=20260909, time=3609090900, open=3500, high=3500, low=3500, close=3500, vol=4)
        call("warmup", C.byref(bar))
        call("start", encoded(dict(date=20260909, time=90000000, trading_day=20260909, event_ms=1, received_at="synthetic:1")))
    call("arm", encoded(identity))
    if not restore:
        placed = call("submit", encoded(dict(command_id="open-1", direction="long", offset="open", price="3501", quantity="2", **identity)))
        assert placed["result"] > 0, placed
        if scenario == "bar":
            call("submit", encoded(dict(command_id="open-short", direction="short", offset="open", price="3499", quantity="2", **identity)))
    tick = Tick(exchg=b"SHFE", code=b"rb2609", price=3500, action_date=20260909,
                action_time=90001000 if not restore else 90002000, trading_date=20260909)
    tick.ask_prices[0] = tick.bid_prices[0] = 3500
    tick.ask_qty[0] = tick.bid_qty[0] = 1
    sequence = 2 if restore else 1
    step = dict(sequence=sequence, event_ms=sequence + 1, date=20260909, time=tick.action_time,
                trading_day=20260909, received_at=f"synthetic:{sequence + 1}", kind="tick")
    replay = Bar(date=20260909, time=3609090905, open=3500, high=3500, low=3500, close=3500, vol=4)
    if scenario == "bar":
        step.update(kind="bar_quote", phase=sequence - 1, time=90500000)
    def advance():
        return call("step", encoded(step), C.byref(tick) if scenario == "tick" else None,
                    C.byref(replay) if scenario == "bar" else None)
    filled = advance()
    assert filled["paper"]["balance"]["cash"] == ("99996.00" if restore else "99998.00"), filled
    checkpoint = call("snapshot")["result"]
    Path("checkpoint.json").write_text(checkpoint, encoding="utf-8")
    # Snapshot/query do not advance the event cursor, consume raw reports, or fill again.
    assert call("snapshot")["result"] == checkpoint
    before = call("query", encoded({"refresh": False}))
    assert before["sequence"] == sequence and before["paper"] == filled["paper"]
    if not restore:
        tick.action_time = 90002000
        step.update(sequence=2, event_ms=3, time=tick.action_time, received_at="synthetic:3")
        if scenario == "bar":
            step.update(phase=1, time=90500000)
        advance()
    complete = json.loads(call("snapshot")["result"])
    # Run/generation and connection reports differ after reconciliation; native
    # account, CTA, data cache and event cursor must have the identical next cut.
    write("next-cut.json", {key: complete[key] for key in ("paper", "engine", "sequence", "event_ms", "received_at", "bar_input", "bar_phase")})
    if scenario == "bar":
        for phase in (2, 3):
            step.update(phase=phase, sequence=phase + 1, event_ms=phase + 2, received_at=f"synthetic:{phase + 2}")
            advance()
        step.update(kind="bar", sequence=5, event_ms=6, received_at="synthetic:6")
        closed = advance()
        assert closed["paper"]["balance"]["cash"] == "99992.00", closed
        end = json.loads(call("snapshot")["result"])
        assert end["bar_phase"] == 0 and end["bar_input"] == ""
    dll.wt_live_block()
    rejected = json.loads(dll.wt_live_submit(encoded(dict(command_id="blocked", direction="long", offset="open", price="3501", quantity="1", **identity))))
    assert not rejected["ok"]
    dll.release_porter()
    if os.name == "nt":
        import _ctypes
        _ctypes.FreeLibrary(dll._handle)
    print("Controlled DLL checkpoint branch passed", sequence, flush=True)


if __name__ == "__main__":
    porter, mocker, root = (Path(arg).resolve() for arg in sys.argv[1:4])
    if len(sys.argv) > 4:
        child(porter, mocker, root, Path(sys.argv[5]).resolve() if len(sys.argv) > 5 else None, sys.argv[4])
    else:
        command = [sys.executable, str(Path(__file__).resolve()), str(porter), str(mocker)]
        for scenario in ("tick", "bar"):
            output = root / scenario
            subprocess.run(command + [str(output / "original"), scenario], check=True, timeout=30)
            subprocess.run(command + [str(output / "restored"), scenario, str(output / "original/checkpoint.json")], check=True, timeout=30)
            assert (output / "original/next-cut.json").read_bytes() == (output / "restored/next-cut.json").read_bytes()
            print(scenario, "restored next native cut equals uninterrupted execution")
