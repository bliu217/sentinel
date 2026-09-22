# Tests

v0 tests cover the pure telemetry math in `src/telemetry/cpu_math.h`. They do not call Win32, so they run without mocking `GetSystemTimes` or `GlobalMemoryStatusEx`.

Framework: **GoogleTest** (v1.15.2 via CMake FetchContent) plus **CTest**. The test binary is `sentinel_tests`.

## Layout

| File | What it checks |
| --- | --- |
| `cpu_math_test.cpp` | `fileTimeToU64` dword packing; CPU % for idle-only, busy-only, mixed load, zero delta, backwards counters, idle exceeding total |

`SystemCollector` stays out of unit tests in v0. Its transform logic is the functions above; the Win32 reads are thin wrappers.

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
