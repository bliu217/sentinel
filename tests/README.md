# Tests

Tests cover the pure telemetry math, storage, anomaly detection, and v3 process grouping. One v3 smoke test checks that the Windows collector can sample its own process.

Framework: **GoogleTest** (v1.15.2 via CMake FetchContent) plus **CTest**. The test binary is `sentinel_tests`.

## Layout

| File | What it checks |
| --- | --- |
| `cpu_math_test.cpp` | `fileTimeToU64` dword packing; CPU % for idle-only, busy-only, mixed load, zero delta, backwards counters, idle exceeding total |
| `ring_buffer_test.cpp` | `RingBuffer` push/snapshot order, capacity-one and overflow behavior, snapshot copies, concurrent snapshots, and multiple writers |
| `anomaly_detector_test.cpp` | CPU/memory episode confirmation, threshold boundaries, simultaneous anomalies, and sampling-delay detection |
| `event_store_test.cpp` | In-memory event ordering and empty-store behavior |
| `persistence_test.cpp` | SQLite restart, retention, size cap, archive filtering, deduplication, and interrupted-append recovery |
| `event_attribution_test.cpp` | CPU events with Cursor/WSL context, missing process snapshots, and selected-group retention |
| `process_telemetry_test.cpp` | Process CPU math and invalid deltas, first-sample baselines, PID reuse, disappearance, Cursor/WSL grouping, top-group selection, bounded history, and summary formatting |

`SystemCollector` stays out of unit tests. Its CPU transform logic is tested separately; the Win32 reads are thin wrappers.

## Run

From an x64 Developer Command Prompt at the repo root (same preset as the app; tests are on by default):

```bat
cmake --preset windows-msvc-ninja
cmake --build --preset windows-msvc-ninja
ctest --preset windows-msvc-ninja --output-on-failure
```

Or run the binary directly:

```bat
build\sentinel_tests.exe
```

Disable tests at configure time with `-DSENTINEL_BUILD_TESTS=OFF`.

## Cases (`cpu_math_test.cpp`)

- **FILETIME:** high/low `DWORD`s combine as `(high << 32) | low`.
- **CPU %:** Windows kernel time includes idle, so `busy = Δkernel + Δuser − Δidle` and `total = Δkernel + Δuser`.
- **Guards:** `total == 0` → 0%; times going backwards → 0%; usage clamped to `[0, 100]`.
