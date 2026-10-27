"""Shared bar/Tick interleaving through the real controlled DLL and CTA callbacks."""
import ctypes as C
from datetime import datetime, timezone
import json
from pathlib import Path

from controlled import Bar, Tick, encoded, write

origin = int(datetime(2026, 9, 9, 1, tzinfo=timezone.utc).timestamp()) * 1000
through = (origin + 3_600_000) * 1_000_000
bindings = dict(stream="shared-test", history_count=120, series=[
    [code, frequency] for code in ("SHFE.rb.2609", "SHFE.rb.2610") for frequency in ("1h", "5m")
])


def run_shared(dll, call, context, restore):
    dll.wt_live_shared_bars_abi.restype = C.c_uint32
    assert dll.wt_live_shared_bars_abi() == 1
    get_type = C.CFUNCTYPE(None, C.c_ulong, C.c_char_p, C.c_char_p, C.POINTER(Bar), C.c_uint32, C.c_bool)
    bar_type = C.CFUNCTYPE(None, C.c_ulong, C.c_char_p, C.c_char_p, C.POINTER(Bar))
    dll.cta_get_bars.argtypes = [C.c_ulong, C.c_char_p, C.c_char_p, C.c_uint32, C.c_bool, get_type]
    dll.cta_sub_bar_events.argtypes = [C.c_ulong, C.c_char_p, C.c_char_p]
    dll.cta_get_price.argtypes = [C.c_char_p]; dll.cta_get_price.restype = C.c_double
    dll.register_cta_callbacks.argtypes = [C.c_void_p] * 6
    errors, callbacks = [], []

    def bars(code, frequency):
        values = []
        @get_type
        def receive(handle, code, period, rows, count, last):
            values.extend((rows[i].time, rows[i].close) for i in range(count))
        period = "m60" if frequency == "1h" else "m5"
        dll.cta_get_bars(context, code.encode(), period.encode(), 120, False, receive)
        return values

    @bar_type
    def notified(handle, code, period, bar):
        try:
            assert dll.cta_get_date() == 20260909 and dll.cta_get_time() == 1000
            assert all(bars(*s) == [(3609091000, 3500)] for s in bindings["series"])
            callbacks.append([code.decode(), period.decode()])
        except Exception as error:
            errors.append(repr(error))

    dll.register_cta_callbacks(None, None, None, notified, None, None)
    for code, f in bindings["series"]:
        bars(code, f)
        dll.cta_sub_bar_events(context, code.encode(), ("m60" if f == "1h" else "m5").encode())
    coverage = dict(partial=False, gap=False, reasons=0, first_ns="0", last_ns="0")
    entries = []
    for code, f in bindings["series"]:
        entries.append(dict(instrument_id=code, frequency=f, tail=coverage,
            input_status=dict(input=[True, True, False, str(through)], processed_through_ns=str(through)),
            interval=dict(sequence="1", trading_day=20260909, start_ns=str(through - (3600 if f == "1h" else 300)*1_000_000_000),
                end_ns=str(through), coverage=coverage,
                bar=dict(open=3500, high=3500, low=3500, close=3500, volume=100, turnover=3500000, open_interest=20))))
    docs = {2: dict(action="load",entries=entries[:2]), 3: dict(action="load",entries=entries[2:])}
    for position, seq in enumerate((4, 6, 7, 8)):
        code,f = bindings["series"][position]
        docs[seq] = dict(action="notify",position=position,notify=dict(instrument_id=code,frequency=f,sequence="1"))
    start = json.loads(restore.read_text())["sequence"] if restore else 0
    before_callbacks = sum(seq <= start for seq in (4,6,7,8))
    for seq in range(start + 1, 10):
        tick = None
        meta = dict(sequence=seq,event_ms=origin+3_601_000+seq,date=20260909,time=100001000+seq,
                    trading_day=20260909,received_at=f"synthetic:{seq}",kind="clock")
        if seq in docs:
            meta.update(kind="market_bar",market_bar=dict(kind="market_bar",stream="shared-test",through_ns=str(through),**docs[seq]))
        elif seq in (1,5):
            meta["kind"] = "tick"
            tick = Tick(exchg=b"SHFE",code=b"rb2609",price=3500 if seq==1 else 3501,
                        action_date=20260909,action_time=95958000 if seq==1 else 95959000,trading_date=20260909)
            tick.ask_prices[0]=tick.bid_prices[0]=tick.price;tick.ask_qty[0]=tick.bid_qty[0]=1
        before = call("query",encoded(dict(refresh=False)))
        result = call("step",encoded(meta),C.byref(tick) if tick is not None else None,None)
        assert not errors, errors
        if seq in docs:
            assert result["paper"]["balance"] == before["paper"]["balance"]
            assert dll.cta_get_price(b"SHFE.rb.2609") == (3500 if seq<5 else 3501)
        if seq==2: assert all(not bars(*s) for s in bindings["series"])
        if seq>=3: assert all(bars(*s)==[(3609091000,3500)] for s in bindings["series"])
        assert result["paper"]["balance"]["cash"] == ("99998.00" if seq<5 else "99996.00")
        snapshot = call("snapshot")["result"]
        if seq in (1,2,3,4,7): Path(f"checkpoint-{seq}.json").write_text(snapshot,encoding="utf-8")
    assert callbacks == [[code,"m60" if f=="1h" else "m5"] for code,f in bindings["series"]][before_callbacks:]
    # No subsequent Tick: an empty final interval still closes the session.
    end = (origin + 6 * 3_600_000) * 1_000_000
    empty = [dict(instrument_id=code, frequency=f, interval=None, tail=coverage,
                  input_status=dict(input=[True, True, True, str(end)], processed_through_ns=str(end)))
             for code, f in bindings["series"]]
    for seq, document in ((10, dict(action="load", entries=empty)), (11, dict(action="finish"))):
        meta = dict(sequence=seq, event_ms=end // 1_000_000 + 2000, date=20260909,
                    time=150002000, trading_day=20260909, received_at=f"synthetic:{seq}",
                    kind="market_bar", market_bar=dict(stream="shared-test", through_ns=str(end), **document))
        result = call("step", encoded(meta), None, None)
        assert result["paper"]["balance"]["cash"] == "99996.00"
    assert result["stopping"]
    final = json.loads(call("snapshot")["result"])
    assert final["version"]==2 and final["paper_sequence"]==3
    assert final["shared_bars"]["completed"]==str(end)
    write("next-cut.json",{k:final[k] for k in ("paper","engine","sequence","event_ms","received_at","source_ns","paper_sequence","shared_bars")})
