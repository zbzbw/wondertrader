## Independent DataKit recording

`recording_control(action, result)` controls persistence while the existing parser,
latest quotes and UDP broadcaster continue. Actions are 0 (status), 1 (start), and
2 (stop). This entry is separate from maintenance `request_stop` / `stop_and_flush`.

Only WtDataStorage supports these controls. DataKit initialization returns false
for unsupported writers, including WtDataStorageAD, and logs the required module;
starting a DataKit whose initialization failed does not start parser reception.
The companion Python wrapper raises on initialization failure.

DataManager slices snapshots and keeps one latest quote per known contract in
memory before broadcasting. WtDataStorage only persists already normalized ticks.
A storage error or stopped writer cannot prevent later valid ticks from reaching
the broadcaster or latest-quote queries.
Shared reception logs its valid Tick count at most once per second while ticks
arrive, including while recording is stopped or failed. Health uses this activity
separately from persistence status and historical file modification times.

Stopping detaches the writer, drains accepted work using `stopAndDrain`, flushes
its files, and returns the existing persistence evidence. A repeated stop returns
the same result. Starting creates a new writer under `segments/<number>/`, retaining
previous files and dumpers. A failed drain requires maintenance before another start.
Session processing can advance while persistence is stopped or failed.

`WtRecordingResult` reports `recording`, `failed`, `writer_drained`,
`checkpoint_persisted`, `received_offset`, `accepted_offset`, `persisted_offset`,
and `segment`. Reception counts valid shared ticks; writer offsets count accepted
and processed inputs across this process's segments. Durability is established by
the successful stop evidence. These counters restart with the process. The caller
persists explicit stop intent and applies it before starting parser reception.

Windows validation uses the BWT companion's
`scripts/validate_wondertrader_recording_lifecycle.py`: real extended-parser
callbacks, UDP packets decoded by the strategy feed, latest-quote queries while
stopped, a new recording segment, repeated stop, and write/flush failures.
Fresh-process WtDataHelper reads check both closed segments. This synthetic
validation does not establish exchange connectivity or full-market capacity.
