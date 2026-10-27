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

`recording_instruments(comma_separated_codes)` changes the writer's explicit
instrument set after a successful stop. Codes use the existing full contract
names and must belong to current parser subscriptions. A running writer rejects
changes. An empty string selects no instruments. The filter runs after latest
quote updates and broadcasting, before writer acceptance; excluded ticks do not
increase writer offsets. Parser subscriptions remain owned by market reception.

Stopping also compresses remaining accepted Tick rows to immutable
`raw/ticks/<trading-date>/<exchange>/<code>.dsb` files inside that segment.
`recording/closed.json` appears only after all raw files and the close record are
durable. It reports per-file rows, receive times in UTC Unix ns, and writer
offsets. These files are readable before market close. The close record has
`complete_day=false`; it does not advance session markers or daily completion.
Units already emitted by a daily close are omitted from stop-time output.
The existing daily unit facts remain available separately.

DMB files remain available after stopping. A later process starts a fresh
segment when a previous close record exists, so stopped DMBs are never reopened
for append. A failed compression or close-record write returns failed stop
evidence, retains original data, and publishes no successful close record.
Reception and broadcasting continue after that failure.

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
# Admission capacity

`recording_limit(accepted_limit)` sets an absolute bound on accepted recording
Ticks, including earlier segments in the same process. The owner calculates this
bound after reserving active blocks, initial per-contract allocation, compressed
close output, state and pending accepted input against its registered cache budget
and actual free volume. The native Tick path checks the bound before writer
acceptance, after shared latest-value updates and broadcasting.

On exhaustion it accepts no further recording input and reports
`capacity_exhausted` in `WtRecordingResult`, while leaving the writer available to
drain accepted input. The owner persists stopped intent and calls the existing
stop/drain operation. Increasing the limit does not clear exhaustion while the
writer remains active. A completed stop followed by an explicit start is required.
The additional result byte occupies reserved space; the result size is unchanged.

