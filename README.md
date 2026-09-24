# Sentinel

## Purpose

Sentinel is a lightweight C++ Windows diagnostics agent that continuously records system telemetry so that when my computer lags, WSL crashes, Cursor freezes, etc., I can inspect what happened immediately before the incident.

## v0

v0 established the CLI collector. It samples machine-wide CPU and memory once per second and prints each sample to stdout.

| Piece | Role |
| --- | --- |
| `SystemCollector` | Calls `GetSystemTimes` and `GlobalMemoryStatusEx`, then turns counters into a `SystemSample` |
| `cpu_math` | Converts `FILETIME` to `uint64_t` and computes CPU % from idle/kernel/user deltas (kernel time includes idle) |
| `sentinel start` | 1 Hz loop: collect, print one line, and `sleep_until` the next tick; Ctrl+C stops |

The first `collect()` is a CPU baseline and produces no line. Win32 failures skip that tick instead of exiting. Startup and CLI error handling were hardened, and access to the collector's previous CPU counters was synchronized.

**Metrics:** UTC timestamp, CPU usage over the previous interval, physical memory used, and physical memory available.

## v1

v1 added bounded, thread-safe telemetry history through `RingBuffer`.

| Improvement | Detail |
| --- | --- |
| Bounded history | A fixed-capacity buffer prevents memory use from growing with runtime |
| Recent-data retention | New samples replace the oldest samples after the buffer reaches capacity |
| Ordered snapshots | Readers receive a chronological copy without exposing internal storage |
| Concurrent access | Push, snapshot, size, and empty operations are synchronized; tests cover concurrent readers and multiple writers |
| Edge-case coverage | Zero capacity is rejected, and capacity-one and wraparound behavior are tested |

**Metrics retained:** the v0 `SystemSample` fields—CPU %, memory used, memory available, and sample timestamp. With the current 1 Hz cadence and a capacity of 300, the buffer can hold five minutes of recent telemetry once connected to the monitor loop.

The history also makes rolling metrics possible, such as average and peak CPU, memory high-water marks, and time spent above a threshold. These derived metrics are not calculated in v1.

## v2

v2 connected the collector, a 300-sample ring buffer, anomaly analysis, and an in-memory event store in the monitor loop. The CLI now runs silently after startup while it retains samples and detection events for later inspection and persistence work.

| Improvement | Detail |
| --- | --- |
| High CPU detection | Starts an episode after three consecutive samples at or above 90%; stops it after three consecutive samples below 90% |
| High memory detection | Applies the same confirmation rules to `used / (used + available)` memory percentage |
| Sampling-delay detection | Emits an event when consecutive samples are more than 1.5 seconds apart |
| Event history | Stores detection events in occurrence order for the lifetime of the process |
| Monotonic timing | Uses `std::chrono::steady_clock` so elapsed-time and lag detection are not affected by wall-clock changes |
| Collector consistency | Keeps each Win32 read sequence and CPU-baseline update under the same lock across concurrent callers |

**Metrics and signals:** CPU usage %, memory used bytes, memory available bytes, derived memory usage %, sample interval in milliseconds, and `HighCpu`, `HighMemory`, and `SamplingDelay` events. Resource events carry `Started` or `Stopped` state; sampling delays carry `Occurred` state.

The event stream makes additional metrics possible, including anomaly count, episode duration, maximum value during an episode, sampling-delay frequency, and percentage of monitored time under CPU or memory pressure. These summaries are not yet calculated or persisted.

## v3

v3 adds `ProcessCollector`, a separate Windows process sampler. Each tick, it enumerates accessible processes and records their executable name, PID plus creation time, CPU usage, working-set bytes, and private committed bytes. PID plus creation time distinguishes a new process from a reused PID. The first sighting of each process establishes a CPU baseline; memory is available immediately.

Process CPU percentage is the change in its kernel-plus-user time divided by the change in machine-wide CPU time. This keeps process percentages on the same 0–100% scale as system CPU. A group reports CPU as pending when any member still needs a baseline. Failed process reads are skipped without stopping collection.

The aggregator combines `Cursor.exe` processes into **Cursor** and `VmmemWSL.exe` or `vmmem.exe` into **WSL**. Other processes are grouped by executable name. It sums group CPU, working-set bytes, and private committed bytes. Working set is resident memory and may include shared pages, so summed group memory is an estimate rather than exclusive RAM ownership. WSL here refers to its Windows VM host process, not individual Linux processes.

The monitor retains 300 process-group snapshots, with Cursor and WSL plus the top 10 groups by CPU and top 10 by working set per snapshot. A summary formatter can produce text such as “Cursor consumed 38.0% CPU and 2.0 GiB resident memory; WSL consumed 4.0% CPU and 6.0 GiB resident memory.” The current CLI still runs silently; the formatter is ready for a later GUI or query command.

System and process samples now include a UTC timestamp for future persistence while keeping monotonic timestamps for elapsed-time calculations. No process history is persisted yet.

## Build

Toolchain: Visual Studio 2026 Build Tools (`cl`), CMake, Ninja, Windows SDK. Run these from an x64 Developer Command Prompt so `cl` is on `PATH` (MSYS `g++` must not win).

```bat
cmake --preset windows-msvc-ninja
cmake --build --preset windows-msvc-ninja
```

Binary: `build/sentinel.exe`.

## Use

```bat
build\sentinel.exe start
```

Anything other than `start` prints `Usage: sentinel start` and exits with status 1.

The monitor runs silently after startup because samples and events are held in memory for later analysis and persistence work.

See [tests/README.md](tests/README.md) for how tests are organized and how to run them.
