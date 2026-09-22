---
name: sentinel-collector
description: >-
  Implements Sentinel Windows collectors that fetch Win32 metrics and transform
  counters into SystemSample telemetry. Use when adding or changing collectors,
  metrics, FILETIME conversion, GetSystemTimes, GlobalMemoryStatusEx, CPU
  usage deltas, or sampling intervals.
---

# Sentinel collector

Collector = Win32 fetch + transform. Do not write to the ring buffer or stdout from a collector.

## Counters vs gauges

- **Counter** (e.g. idle/kernel/user FILETIME): store previous raw values under a mutex; emit a rate from the delta. First `collect()` stores a baseline and returns `std::nullopt`.
- **Gauge** (e.g. memory used/available): read current value into the sample; no delta.

Windows: `GetSystemTimes` kernel time **includes idle**. Usage:

`busy = Δkernel + Δuser − Δidle`, `total = Δkernel + Δuser`, `cpu = 100 * busy / total`.

If `total == 0` or times went backwards, return 0. Clamp to `[0, 100]`.

## FILETIME

`(static_cast<uint64_t>(high) << 32) | low`. Put conversion and CPU math in `cpu_math` so tests do not need Win32.

## Contract

```cpp
[[nodiscard]] std::optional<SystemSample> collect();
```

- Thread-safe: mutex only around previous counter state.
- Win32 failure: return `nullopt`; do not throw or log on the hot path.
- Keep `collect()` allocation-free.
