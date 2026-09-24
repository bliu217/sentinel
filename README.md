# Sentinel

## Purpose

Sentinel is a lightweight C++ Windows diagnostics agent that continuously records system telemetry so that when my computer lags, WSL crashes, Cursor freezes, etc., I can inspect what happened immediately before the incident.

## v0

v0 is a CLI-only monitor. It samples machine-wide CPU and memory once per second, retains five minutes of samples in memory, detects resource and sampling anomalies, and records detection events in memory.

| Piece | Role |
| --- | --- |
| `SystemCollector` | Calls `GetSystemTimes` and `GlobalMemoryStatusEx`, then turns counters into a `SystemSample` |
| `cpu_math` | Converts `FILETIME` to `uint64_t` and computes CPU % from idle/kernel/user deltas (kernel time includes idle) |
| `RingBuffer` | Retains the most recent 300 samples in chronological order |
| `AnomalyDetector` | Detects sustained CPU/memory pressure and delayed sampling gaps |
| `EventStore` | Keeps the ordered detection-event log in memory |
| `sentinel start` | 1 Hz loop: collect, buffer, analyze, and `sleep_until` the next tick; Ctrl+C stops |

The first `collect()` is a CPU baseline and produces no sample. Win32 failures skip that tick instead of exiting. Samples use `std::chrono::steady_clock` timestamps so elapsed-time and lag detection are not affected by wall-clock adjustments.

CPU and memory episodes begin after three consecutive samples at or above 90% and end after three consecutive samples below 90%. A sampling gap greater than 1.5 seconds creates a `SamplingDelay` event. Events are retained only for the lifetime of the process in this version.

### Build

Toolchain: Visual Studio 2026 Build Tools (`cl`), CMake, Ninja, Windows SDK. Run these from an x64 Developer Command Prompt so `cl` is on `PATH` (MSYS `g++` must not win).

```bat
cmake --preset windows-msvc-ninja
cmake --build --preset windows-msvc-ninja
```

Binary: `build/sentinel.exe`.

### Use

```bat
build\sentinel.exe start
```

Anything other than `start` prints `Usage: sentinel start` and exits with status 1.

The monitor runs silently after startup because samples and events are held in memory for later analysis and persistence work.

See [tests/README.md](tests/README.md) for how tests are organized and how to run them.
