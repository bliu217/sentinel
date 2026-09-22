#include "system_collector.h"

#include <windows.h>

namespace sentinel::telemetry {
namespace {

[[nodiscard]] bool readCpuTimes(RawCpuSample& out) noexcept {
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetSystemTimes(&idle, &kernel, &user) == 0) {
        return false;
    }
    out.idleTime = fileTimeToU64(idle.dwHighDateTime, idle.dwLowDateTime);
    out.kernelTime = fileTimeToU64(kernel.dwHighDateTime, kernel.dwLowDateTime);
    out.userTime = fileTimeToU64(user.dwHighDateTime, user.dwLowDateTime);
    return true;
}

[[nodiscard]] bool readMemory(std::uint64_t& usedBytes, std::uint64_t& availableBytes) noexcept {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) == 0) {
        return false;
    }
    availableBytes = status.ullAvailPhys;
    usedBytes = status.ullTotalPhys - status.ullAvailPhys;
    return true;
}

}  // namespace

std::optional<SystemSample> SystemCollector::collect() {
    RawCpuSample cpu{};
    if (!readCpuTimes(cpu)) {
        return std::nullopt;
    }

    std::uint64_t memoryUsed = 0;
    std::uint64_t memoryAvailable = 0;
    if (!readMemory(memoryUsed, memoryAvailable)) {
        return std::nullopt;
    }

    const auto timestamp = std::chrono::system_clock::now();

    std::lock_guard lock(mutex_);
    if (!previousCpu_.has_value()) {
        previousCpu_ = cpu;
        return std::nullopt;
    }

    const double usage = cpuUsagePercent(*previousCpu_, cpu);
    previousCpu_ = cpu;

    return SystemSample{
        .timestamp = timestamp,
        .cpuUsagePercent = usage,
        .memoryUsedBytes = memoryUsed,
        .memoryAvailableBytes = memoryAvailable,
    };
}

}  // namespace sentinel::telemetry
