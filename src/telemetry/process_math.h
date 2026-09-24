#pragma once

#include <algorithm>
#include <cstdint>

namespace sentinel::telemetry {

[[nodiscard]] constexpr double processCpuUsagePercent(
    std::uint64_t previousProcessTime,
    std::uint64_t currentProcessTime,
    std::uint64_t previousSystemTime,
    std::uint64_t currentSystemTime) noexcept {
    if (currentProcessTime < previousProcessTime || currentSystemTime <= previousSystemTime) {
        return 0.0;
    }
    const double processDelta = static_cast<double>(currentProcessTime - previousProcessTime);
    const double systemDelta = static_cast<double>(currentSystemTime - previousSystemTime);
    return std::clamp(100.0 * processDelta / systemDelta, 0.0, 100.0);
}

}  // namespace sentinel::telemetry
