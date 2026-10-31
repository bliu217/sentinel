#include "sentinel/telemetry/system_collector.h"

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
    return collectDetailed().sample;
}

CollectionResult<SystemSample> SystemCollector::collectDetailed() {
    // Keep the Win32 readings and baseline update in the same order across callers.
    std::lock_guard lock(mutex_);

    CollectionResult<SystemSample> result;
    result.report.attemptedAt = std::chrono::system_clock::now();
    result.report.hasCpuBaseline = previousCpu_.has_value();
    RawCpuSample cpu{};
    if (!readCpuTimes(cpu)) {
        result.report.operation = CollectionOperation::GetSystemTimes;
        result.report.errorCode = GetLastError();
        return result;
    }

    std::uint64_t memoryUsed = 0;
    std::uint64_t memoryAvailable = 0;
    if (!readMemory(memoryUsed, memoryAvailable)) {
        result.report.operation = CollectionOperation::GlobalMemoryStatusEx;
        result.report.errorCode = GetLastError();
        return result;
    }

    const SampleTime time{std::chrono::steady_clock::now(), std::chrono::system_clock::now()};

    if (!previousCpu_.has_value()) {
        previousCpu_ = cpu;
        result.report.isBaseline = true;
        result.report.hasCpuBaseline = true;
        return result;
    }

    const double usage = cpuUsagePercent(*previousCpu_, cpu);
    previousCpu_ = cpu;

    result.sample = SystemSample{
        .timestamp = time.steady,
        .cpuUsagePercent = usage,
        .memoryUsedBytes = memoryUsed,
        .memoryAvailableBytes = memoryAvailable,
        .utcTimestamp = time.utc,
    };
    return result;
}

}  // namespace sentinel::telemetry
