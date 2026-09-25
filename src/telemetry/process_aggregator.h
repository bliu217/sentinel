#pragma once

#include "telemetry/process_sample.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sentinel::telemetry {

struct ProcessGroup {
    std::wstring name;
    std::optional<double> cpuUsagePercent;
    std::uint64_t workingSetBytes{};
    std::uint64_t privateBytes{};
    std::size_t processCount{};
};

struct ProcessGroupSnapshot {
    SampleTime time{};
    std::vector<ProcessGroup> groups;
};

[[nodiscard]] std::wstring processGroupName(const std::wstring& imageName);
[[nodiscard]] ProcessGroupSnapshot aggregateProcesses(const ProcessSnapshot& snapshot);
[[nodiscard]] ProcessGroupSnapshot selectTopProcesses(
    ProcessGroupSnapshot snapshot,
    std::size_t topPerMetric = 10);
[[nodiscard]] std::wstring formatProcessSummary(
    const ProcessGroupSnapshot& snapshot,
    const std::vector<std::wstring>& names);

}  // namespace sentinel::telemetry
