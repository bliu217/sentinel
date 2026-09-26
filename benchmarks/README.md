# Sentinel performance benchmarks

This harness measures how sampling frequency affects Sentinel's own CPU use, private memory, sampling reliability, and scheduling latency. It does not change the production sampling loop. The loop still records the next deadline and waits with `sleep_until`.

Benchmark output under `benchmarks/results/` is gitignored. Do not commit raw results. A short summary table can be added later if you want one in the repo.

Performance numbers are comparable only when every binary is a Release build and the machine is otherwise similarly loaded.

## Build a Release binary

Run these from an x64 Developer Command Prompt at the repository root so `cl` is on `PATH`. The Release preset writes to `build-release\`, separate from the Debug preset in `build\`.

```bat
cmake --preset windows-msvc-ninja-release
cmake --build --preset windows-msvc-ninja-release
```

Binary: `build-release\sentinel.exe`.

Historical worktrees do not have that preset. Configure them explicitly:

```bat
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
cmake --build build-release
```

## One 30-second run

From the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File .\benchmarks\run_benchmark.ps1 `
    -Executable .\build-release\sentinel.exe `
    -IntervalMs 1000 `
    -DurationSeconds 30 `
    -Run 1 `
    -RunId current
```

Results land in `benchmarks\results\current\1000ms-run1\`:

| File | Contents |
| --- | --- |
| `resource_usage.csv` | About 1 Hz external CPU and memory samples |
| `sample_timing.csv` | One raw scheduling delay per scheduled tick |
| `metadata.json` | Commit, interval, duration, machine, and paths |
| `data\` | SQLite database for this run only |

`--benchmark` is off unless the harness passes it. Normal `sentinel start` does not write timing data and still uses `%LOCALAPPDATA%\Sentinel`.

## Smoke test

Thirty seconds at 1000 ms, 100 ms, and 10 ms. This checks launch, monitoring, timing output, high-frequency sampling, and cleanup. It does not replace the full suite.

```powershell
powershell -ExecutionPolicy Bypass -File .\benchmarks\run_benchmark.ps1 `
    -Executable .\build-release\sentinel.exe `
    -Suite Smoke `
    -RunId smoke
```

## Full suite for the current commit

Five intervals (1000, 500, 100, 50, 10 ms), three repetitions, five minutes each. That is 15 runs and about 75 minutes. The script does not start this suite unless you pass `-Suite Full`.

```powershell
powershell -ExecutionPolicy Bypass -File .\benchmarks\run_benchmark.ps1 `
    -Executable .\build-release\sentinel.exe `
    -Suite Full `
    -RunId current
```

Use a different `-RunId` if `benchmarks\results\current\` already contains smoke or earlier runs. The script refuses to overwrite an existing run directory.

## Analyze

```powershell
python .\benchmarks\analyze.py .\benchmarks\results\smoke
```

The script prints a table and writes `summary.csv` in the directory you pass. CPU and private-memory rows are pooled across repetitions of the same interval. Scheduling percentiles are pooled from the raw delay column.

```powershell
python .\benchmarks\analyze.py .\benchmarks\results\current --late-threshold 0.10
python .\benchmarks\analyze.py .\benchmarks\results\current\1000ms-run1 --no-summary
```

A sample is late when its delay is greater than 10% of that row's interval. Change `--late-threshold` to recompute that label without running Sentinel again. Percentiles are linear interpolation between sorted samples.

## Historical commits

Keep this checkout as the only copy of the harness. Build old commits in separate worktrees and point `-Executable` at those Release binaries.

```bat
git worktree add ..\sentinel-anomaly 29250568
git worktree add ..\sentinel-process 061493c1
git worktree add ..\sentinel-persistence 3b60d809
git worktree add ..\sentinel-topn 65a652a2
```

Build each worktree into its own `build-release` directory with the explicit CMake command above. Then, from this repository:

```powershell
powershell -ExecutionPolicy Bypass -File .\benchmarks\run_benchmark.ps1 `
    -Executable ..\sentinel-anomaly\build-release\sentinel.exe `
    -IntervalMs 1000 `
    -DurationSeconds 300 `
    -Run 1 `
    -RunId anomaly `
    -Legacy
```

`-Legacy` launches `sentinel start` with no benchmark flags. Those binaries do not accept `--interval-ms` or `--benchmark`, and their built-in interval is 1000 ms. The harness records external CPU and memory only. `analyze.py` leaves scheduling columns as `n/a` instead of inventing delays.

`-Legacy` cannot be combined with `-Suite`, and it rejects any interval other than 1000 ms so a historical run is not labeled as 10 ms or 100 ms sampling.

## Measurement definitions

Scheduling delay uses `std::chrono::steady_clock`:

```text
delay = actual_sample_start - expected_sample_start
```

`expected_sample_start` is the same deadline the existing loop already passes to `sleep_until`. Sentinel writes each delay as CSV. It does not compute p95 or p99.

External CPU is the change in the process's cumulative processor time, divided by wall time and by `[Environment]::ProcessorCount`, then multiplied by 100. Fully using one logical core on a 16-thread machine is about 6.25%. The primary memory column is `PrivateMemorySize64`. `WorkingSet64` is stored in the same CSV for reference. The monitor samples about once per second.

Expected sample count is the number of deadlines inside `[start, start + duration)`. Actual count is the number of timing rows. Missed samples are `max(0, expected - actual)`.

If a tick takes longer than the interval, the next deadline is still one interval after the previous deadline. `sleep_until` returns immediately, and the delay carries forward into later rows. The CSV keeps that value. It does not replace it with the gap since the previous tick. On a rate the loop cannot sustain, p99 delay grows into seconds and missed samples increase. That is the current scheduler, measured as-is.

Windows often wakes a short sleep on a timer quantum of about 15 ms. A 10% late threshold is 100 ms at 1 Hz and 1 ms at 100 Hz, so the same quantum can look on-time at 1 Hz and late at 10 Hz.

## Limits

- Release only. The Debug preset in `build\` is not a valid benchmark binary. The harness warns if the executable path looks like that directory.
- The timing log is buffered and flushed every 64 rows. That write is part of a `--benchmark` run and is absent from historical binaries.
- Instrumented runs store SQLite data in the run's `data\` directory, starting from an empty database. `-Legacy` runs use the real `%LOCALAPPDATA%\Sentinel` database, because older builds resolve that location with the Windows known-folder API and ignore environment overrides. Those runs can add rows to your normal database, and their I/O cost depends on the database that is already there.
- CPU and memory samples include process startup, not only the steady sampling loop. That matters more for a 30-second smoke test than for a 5-minute run.
- Peak CPU is the highest 1-second external sample, so shorter spikes are averaged inside that second.
- Historical builds cannot be compared at 2 Hz through 100 Hz without modifying them. Do not modify those commits just to add instrumentation.
- Results depend on the host machine, power plan, and whatever else is running. Recorded metadata includes the commit, logical CPU count, CPU model, Windows version, and total memory so a run can be identified later.
