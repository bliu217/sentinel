#pragma once

#include <algorithm>
#include <cstdint>

namespace sentinel::telemetry {

struct RawCpuSample {
    std::uint64_t idleTime{};
    std::uint64_t kernelTime{};
    std::uint64_t userTime{};
};

[[nodiscard]] constexpr std::uint64_t fileTimeToU64(std::uint32_t high,
                                                    std::uint32_t low) noexcept {
    return (static_cast<std::uint64_t>(high) << 32) | low;
}

[[nodiscard]] constexpr double cpuUsagePercent(const RawCpuSample& previous,
                                              const RawCpuSample& current) noexcept {
    if (current.idleTime < previous.idleTime || current.kernelTime < previous.kernelTime ||
        current.userTime < previous.userTime) {
        return 0.0;
    }

    const auto idle = current.idleTime - previous.idleTime;
    const auto kernel = current.kernelTime - previous.kernelTime;
    const auto user = current.userTime - previous.userTime;
    const auto total = kernel + user;
    if (total == 0) {
        return 0.0;
    }

    const double busy = static_cast<double>(total) - static_cast<double>(idle);
    const double usage = 100.0 * busy / static_cast<double>(total);
    return std::clamp(usage, 0.0, 100.0);
}

}  // namespace sentinel::telemetry
