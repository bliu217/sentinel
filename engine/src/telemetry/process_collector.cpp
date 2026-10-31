#include "sentinel/telemetry/process_collector.h"

#include "sentinel/telemetry/cpu_math.h"

#include <windows.h>
#include <psapi.h>

#include <cstdint>
#include <string>
#include <string_view>
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

[[nodiscard]] bool readProcess(DWORD processId, std::vector<wchar_t>& imagePath, RawProcessObservation& out) {
    UniqueHandle handle(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId));
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

    DWORD pathLength{};
    for (;;) {
        pathLength = static_cast<DWORD>(imagePath.size());
        if (QueryFullProcessImageNameW(handle.get(), 0, imagePath.data(), &pathLength) != 0) break;
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || imagePath.size() >= 32768) return false;
        imagePath.resize(imagePath.size() * 2);
    }
    if (pathLength == 0) return false;
    const std::wstring_view fullPath(imagePath.data(), pathLength);
    const auto separator = fullPath.find_last_of(L"\\/");
    const std::wstring_view imageName = fullPath.substr(
        separator == std::wstring_view::npos ? 0 : separator + 1);

    out = RawProcessObservation{
        .identity = {processId, asU64(created)},
        .imageName = std::wstring(imageName),
        .cpuTime = asU64(kernel) + asU64(user),
        .workingSetBytes = static_cast<std::uint64_t>(memory.WorkingSetSize),
        .privateBytes = static_cast<std::uint64_t>(memory.PrivateUsage),
    };
    return true;
}

}  // namespace

std::optional<ProcessSnapshot> ProcessCollector::collect() {
    return collectDetailed().sample;
}

CollectionResult<ProcessSnapshot> ProcessCollector::collectDetailed() {
    // Serialize reads and baseline updates so concurrent callers preserve counter order.
    std::lock_guard lock(mutex_);

    CollectionResult<ProcessSnapshot> result;
    result.report.attemptedAt = std::chrono::system_clock::now();
    std::vector<DWORD> processIds(1024);
    DWORD bytesReturned{};
    for (;;) {
        const DWORD bufferBytes = static_cast<DWORD>(processIds.size() * sizeof(DWORD));
        if (EnumProcesses(processIds.data(), bufferBytes, &bytesReturned) == 0) {
            result.report.operation = CollectionOperation::EnumerateProcesses;
            result.report.errorCode = GetLastError();
            return result;
        }
        if (bytesReturned < bufferBytes) break;
        processIds.resize(processIds.size() * 2);
    }

    result.report.enumerated = bytesReturned / sizeof(DWORD);
    std::vector<RawProcessObservation> observations;
    std::vector<wchar_t> imagePath(1024);
    for (std::size_t index = 0; index < bytesReturned / sizeof(DWORD); ++index) {
        RawProcessObservation observation;
        if (readProcess(processIds[index], imagePath, observation)) {
            observations.push_back(std::move(observation));
        }
    }

    result.report.read = observations.size();
    result.report.skipped = result.report.enumerated - result.report.read;
    std::uint64_t systemCpuTime{};
    if (!readSystemCpuTime(systemCpuTime)) {
        result.report.operation = CollectionOperation::GetSystemTimes;
        result.report.errorCode = GetLastError();
        return result;
    }
    const SampleTime time{std::chrono::steady_clock::now(), std::chrono::system_clock::now()};
    result.sample = tracker_.update(time, systemCpuTime, std::move(observations));
    result.report.hasCpuBaseline = true;
    for (const ProcessSample& process : result.sample->processes) {
        if (!process.cpuUsagePercent) result.report.hasCpuBaseline = false;
    }
    result.report.isBaseline = !result.report.hasCpuBaseline;
    return result;
}

}  // namespace sentinel::telemetry
