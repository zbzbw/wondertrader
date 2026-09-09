# Native matching regression (ZBW-35)

Build `TraderMockerMatchingTest` explicitly; it is excluded from default builds.
The target compiles the production `TraderMocker.cpp` and calls its real matching
and queued cancellation paths with synthetic WT orders and ticks. It opens no
sockets, starts no threads and connects to no broker. Run in an isolated writable
directory: the existing position writer creates `matching-positions.json` there.

```text
cmake --build BUILD --config Release --target TraderMockerMatchingTest TraderMocker
BUILD/Release/TraderMockerMatchingTest
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

Windows MSVC 19.44/Boost 1.72: test executable and TraderMocker DLL built; executable
passed on 2026-09-09. This is matching evidence only. It does not prove controlled
accounting, settlement, runtime serialization, checkpoint restore or broker safety.
