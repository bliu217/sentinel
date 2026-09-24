#pragma once

#include "telemetry/sample_time.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sentinel::telemetry {

struct ProcessIdentity {
    std::uint32_t processId{};
    std::uint64_t creationTime{};

    [[nodiscard]] bool operator==(const ProcessIdentity&) const = default;
};

struct ProcessSample {
    ProcessIdentity identity{};
    std::wstring imageName;
    std::optional<double> cpuUsagePercent;
    std::uint64_t workingSetBytes{};
    std::uint64_t privateBytes{};
};

struct ProcessSnapshot {
    SampleTime time{};
    std::vector<ProcessSample> processes;
};

struct RawProcessObservation {
    ProcessIdentity identity{};
    std::wstring imageName;
    std::uint64_t cpuTime{};
    std::uint64_t workingSetBytes{};
    std::uint64_t privateBytes{};
};

}  // namespace sentinel::telemetry
