# Native matching regression (ZBW-35)

Build `TraderMockerMatchingTest` explicitly; it is excluded from default builds.
The target compiles the production `TraderMocker.cpp` and calls its real matching
and queued cancellation paths with synthetic WT orders and ticks. It opens no
sockets, starts no threads and connects to no broker. Run in an isolated writable
directory: the existing position writer creates `matching-positions.json` there.

```text
cmake --build BUILD --config Release --target TraderMockerMatchingTest TraderMocker
BUILD/Release/TraderMockerMatchingTest
cmake --build BUILD --config Release --target TraderMockerAccountTest
BUILD/Release/TraderMockerAccountTest
```

Use the normal WT C++17 dependencies. A standalone configuration of
`src/TraderMocker` accepts `INCS` and `LNKS` for dependency headers/libraries.
Windows VS2022 builds using Boost 1.72/vc142 need the same explicit Boost libraries
in `CMAKE_EXE_LINKER_FLAGS_RELEASE` as in `CMAKE_SHARED_LINKER_FLAGS_RELEASE`,
and `/DBOOST_ALL_NO_LIB /utf-8`, as used by the BWT Windows build entrypoint.

The test checks stable whole-lot split/residuals, invalid split bounds, FIFO shared
ask/bid liquidity, partial-fill continuation, zero liquidity, price limits,
contract/exchange isolation, real cancellation and gross position conservation.
Failures throw independently of `NDEBUG`, so Release builds retain all checks.

`TraderMockerAccountTest` exercises native integer accounting: per-lot and
proportional fees, conservative reservations, partial fills/cancels, insufficient
funds, gross long/short margin, explicit today/yesterday buckets, official
settlement, DAY expiry, missing/conflicting settlement, daily balances and account
restore without a second deposit. CNY amounts use cents, prices/per-lot fees use
millionths of CNY, and rates use 1e-8. Fees round half away from zero per actual
fill; reservations round each lot upward at the contract upper limit.

The matching target also exercises real queued order insertion, native account
and position queries, controlled steps and native component restore. It checks
that controlled login creates no worker, matching timer or UDP socket; callbacks
hold neither API nor order mutex; timestamps come from the supplied event; a
partial-fill checkpoint followed by cancellation produces the same complete
TraderMocker state as uninterrupted execution. Input gaps are rejected. A quote
is consumed once, including when no order is pending. The caller supplies the
calendar trading day and official settlement; no wall-clock date advances it.

Windows MSVC 19.44/Boost 1.72: both executables and TraderMocker DLL built and
tests passed on 2026-09-09. These are native component tests. The WtPorter driver,
CTA/strategy/report bundle, authority gate and parent checkpoint protocol remain
under implementation; this component snapshot is not a complete live checkpoint.
No broker connectivity, full CI or installed BWT lifecycle is claimed here.
