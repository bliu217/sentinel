#pragma once

#include "telemetry/process_sample.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace sentinel::telemetry {

struct ProcessIdentityHash {
    [[nodiscard]] std::size_t operator()(const ProcessIdentity& identity) const noexcept {
        const auto pid = std::hash<std::uint32_t>{}(identity.processId);
        const auto creation = std::hash<std::uint64_t>{}(identity.creationTime);
        return pid ^ (creation + 0x9e3779b9u + (pid << 6) + (pid >> 2));
    }
};

class ProcessTracker {
public:
    [[nodiscard]] ProcessSnapshot update(
        SampleTime time,
        std::uint64_t systemCpuTime,
        std::vector<RawProcessObservation> observations);

private:
    std::unordered_map<ProcessIdentity, std::uint64_t, ProcessIdentityHash> previousProcesses_;
    std::uint64_t previousSystemCpuTime_{};
    bool hasPreviousSystemCpuTime_{};
};

}  // namespace sentinel::telemetry
