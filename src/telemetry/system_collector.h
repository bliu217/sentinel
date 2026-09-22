#pragma once

#include "cpu_math.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

namespace sentinel::telemetry {

struct SystemSample {
    std::chrono::system_clock::time_point timestamp{};
    double cpuUsagePercent{};
    std::uint64_t memoryUsedBytes{};
    std::uint64_t memoryAvailableBytes{};
};

class SystemCollector {
public:
    [[nodiscard]] std::optional<SystemSample> collect();

private:
    std::mutex mutex_;
    std::optional<RawCpuSample> previousCpu_;
};

}  // namespace sentinel::telemetry
