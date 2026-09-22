# Sentinel

## Purpose

Sentinel is a lightweight C++ Windows diagnostics agent that continuosly records system telemetry so that when my computer lags, WSL crashes, Cursor freezes etc., I can inspect what happened immediately before the incident.

## v0

v0 is a CLI-only monitor. It samples machine-wide CPU and memory once per second and prints each sample to stdout. There is no ring buffer, incident file, or analyzer yet.

| Piece | Role |
| --- | --- |
| `SystemCollector` | Calls `GetSystemTimes` and `GlobalMemoryStatusEx`, then turns counters into a `SystemSample` |
| `cpu_math` | Converts `FILETIME` to `uint64_t` and computes CPU % from idle/kernel/user deltas (kernel time includes idle) |
| `sentinel start` | 1 Hz loop: collect, print one line, `sleep_until` the next tick; Ctrl+C stops |

The first `collect()` is a CPU baseline and produces no line. Win32 failures skip that tick instead of exiting.

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

Sample output:

```
2026-09-22T08:48:10Z cpu=8.8% mem_used=12.7GiB mem_avail=2.5GiB
2026-09-22T08:48:11Z cpu=8.5% mem_used=12.7GiB mem_avail=2.6GiB
```

Fields are UTC timestamp, CPU usage over the last interval, physical memory in use, and physical memory available.

See [tests/README.md](tests/README.md) for how tests are organized and how to run them.
