#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace sentinel::telemetry {

[[nodiscard]] constexpr std::optional<double> processCpuUsagePercent(
    std::uint64_t previousProcessTime,
    std::uint64_t currentProcessTime,
    std::uint64_t previousSystemTime,
    std::uint64_t currentSystemTime) noexcept {
    if (currentProcessTime < previousProcessTime || currentSystemTime <= previousSystemTime) {
        return std::nullopt;
    }
    const double processDelta = static_cast<double>(currentProcessTime - previousProcessTime);
    const double systemDelta = static_cast<double>(currentSystemTime - previousSystemTime);
    return std::clamp(100.0 * processDelta / systemDelta, 0.0, 100.0);
}

}  // namespace sentinel::telemetry
