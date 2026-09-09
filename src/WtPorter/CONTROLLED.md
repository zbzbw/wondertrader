# Controlled CTA and account ABI 1

This opt-in mode serves one physical futures contract and one paper or broker_sim
trader. The caller owns the event thread, input journal, risk authorization and
publication boundary. Normal WT runners remain separate. ITraderApi's vtable is
unchanged; TraderMocker exposes an optional `wt_mocker_live`/step ABI.

Configure `env.controlled=true`, one trader and an external CTA target executer.
Do not attach ordinary parsers, executers or a second event owner. Configuration
and start require an initial civil date/time, trading day, event_ms and received_at.

| C entry point | Contract |
| --- | --- |
| wt_live_abi | Returns 1 |
| wt_live_configure/connect/start | Fresh owner/run/generation, connection/query barrier, explicit CTA initialization |
| wt_live_warmup | Completed native bar cache before start; no account execution |
| wt_live_arm/block | Explicit run/generation authorization; block disables decisions before cancellation |
| wt_live_step | Next sequence, monotone event_ms; tick, bar_quote, bar, clock, settle or begin_day |
| wt_live_submit/cancel | Stable command IDs; integer lots, physical contract, explicit close buckets, DAY limit; cancel owned only |
| wt_live_query | Current queue cut; refresh requires a blocked connected trader and the complete query chain |
| wt_live_snapshot/restore | Full native state; restore requires a fresh disconnected owner and restores no authority |
| wt_live_ack | Consume only the published contiguous native report watermark |

JSON calls return `{ok,data}` or `{ok:false,error}`. Returned strings are owned by
the ABI until the next call. All callbacks are copied into the owner queue; no
Python/IPC call runs under the queue or trader lock. A failed step/restore/start
requires a fresh process. Unknown sends retain their command identity and never
resend. Broker order ownership uses saved entrust IDs, not the plugin tag cache.

Snapshot version 1 includes PaperAccount orders/lots/counters, CTA contexts and
engine/session/cache state, unfinished real-tick bar, active OHLC bar/phase,
input clock/cursor, command mappings and unacknowledged raw reports. The caller
must commit its Python strategy/target/input state with this native cut before
publishing reports. Paper may replay a journaled uncommitted input from its last
complete cut; an external broker must only reconcile current facts.

Paper money is CNY cents, prices/per-lot fees use six decimal places and rates
eight. Long/short and today/yesterday are separate, with no margin offset.
DAY expiry releases orders independently of official settlement. begin_day
requires settlement of the previous day; duplicate settlement never charges twice.
The caller records the official settlement source and dated rule evidence.

OHLC uses open/high/low/close, floor(V/4) whole lots for the first three phases and
the remainder for close, shared by both sides in FIFO order. Only the completed
bar drives CTA decisions. Genuine ticks use WTSDataFactory's session assignment;
an explicit clock closes an unfinished bar without inventing another quote.

Build and run TraderMockerAccountTest, TraderMockerMatchingTest,
TraderAdapterLiveTest, CtaLiveStateTest and CtaLiveClockTest. `tests/controlled.py`
takes WtPorter, TraderMocker and a fresh output directory and compares uninterrupted
and restored DLL cuts. The downstream BWT installed-wheel tests additionally
exercise Python strategy state, tick bar recovery, publication crashes and stop.
Rebuild all downstream runtime components together with the fixed wrapper/CRT;
these synthetic checks are not evidence of an external account connection.
