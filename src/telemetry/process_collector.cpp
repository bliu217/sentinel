#include "telemetry/process_collector.h"

#include "telemetry/cpu_math.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace sentinel::telemetry {
namespace {

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_;
};

[[nodiscard]] std::uint64_t asU64(const FILETIME& time) noexcept {
    return fileTimeToU64(time.dwHighDateTime, time.dwLowDateTime);
}

[[nodiscard]] bool readSystemCpuTime(std::uint64_t& total) noexcept {
    FILETIME kernel{};
    FILETIME user{};
    if (GetSystemTimes(nullptr, &kernel, &user) == 0) {
        return false;
    }
    total = asU64(kernel) + asU64(user);
    return true;
}

[[nodiscard]] bool readProcess(const PROCESSENTRY32W& entry, RawProcessObservation& out) {
    UniqueHandle handle(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, entry.th32ProcessID));
    if (!handle.valid()) {
        return false;
    }

    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(handle.get(), &created, &exited, &kernel, &user) == 0) {
        return false;
    }

    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(
            handle.get(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) == 0) {
        return false;
    }

    out = RawProcessObservation{
        .identity = {entry.th32ProcessID, asU64(created)},
        .imageName = entry.szExeFile,
        .cpuTime = asU64(kernel) + asU64(user),
        .workingSetBytes = static_cast<std::uint64_t>(memory.WorkingSetSize),
        .privateBytes = static_cast<std::uint64_t>(memory.PrivateUsage),
    };
    return true;
}

}  // namespace

std::optional<ProcessSnapshot> ProcessCollector::collect() {
    // Serialize reads and baseline updates so concurrent callers preserve counter order.
    std::lock_guard lock(mutex_);

    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.valid()) {
        return std::nullopt;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot.get(), &entry) == 0) {
        return std::nullopt;
    }

    std::vector<RawProcessObservation> observations;
    do {
        RawProcessObservation observation;
        if (readProcess(entry, observation)) {
            observations.push_back(std::move(observation));
        }
    } while (Process32NextW(snapshot.get(), &entry) != 0);

    std::uint64_t systemCpuTime{};
    if (!readSystemCpuTime(systemCpuTime)) {
        return std::nullopt;
    }
    const SampleTime time{std::chrono::steady_clock::now(), std::chrono::system_clock::now()};
    return tracker_.update(time, systemCpuTime, std::move(observations));
}

}  // namespace sentinel::telemetry
