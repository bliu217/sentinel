#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace sentinel::telemetry {

enum class CollectionOperation { None, GetSystemTimes, GlobalMemoryStatusEx, EnumerateProcesses };

/** Source-level evidence; per-process exclusions are summarized once per pass. */
struct CollectionReport {
    CollectionOperation operation{CollectionOperation::None};
    std::optional<std::uint32_t> errorCode;
    std::chrono::system_clock::time_point attemptedAt{};
    bool isBaseline{false};
    bool hasCpuBaseline{false};
    std::uint64_t enumerated{};
    std::uint64_t read{};
    std::uint64_t skipped{};
};

template<class Sample>
struct CollectionResult {
    std::optional<Sample> sample;
    CollectionReport report;
};

}  // namespace sentinel::telemetry
