#include "telemetry/process_tracker.h"

#include "telemetry/process_math.h"

#include <utility>

namespace sentinel::telemetry {

ProcessSnapshot ProcessTracker::update(
    SampleTime time,
    std::uint64_t systemCpuTime,
    std::vector<RawProcessObservation> observations) {
    ProcessSnapshot snapshot{.time = time};
    snapshot.processes.reserve(observations.size());

    std::unordered_map<ProcessIdentity, std::uint64_t, ProcessIdentityHash> currentProcesses;
    currentProcesses.reserve(observations.size());

    for (auto& observation : observations) {
        std::optional<double> cpuUsage;
        const auto previous = previousProcesses_.find(observation.identity);
        if (hasPreviousSystemCpuTime_ && previous != previousProcesses_.end()) {
            cpuUsage = processCpuUsagePercent(
                previous->second, observation.cpuTime, previousSystemCpuTime_, systemCpuTime);
        }

        snapshot.processes.push_back(ProcessSample{
            .identity = observation.identity,
            .imageName = std::move(observation.imageName),
            .cpuUsagePercent = cpuUsage,
            .workingSetBytes = observation.workingSetBytes,
            .privateBytes = observation.privateBytes,
        });
        currentProcesses.emplace(observation.identity, observation.cpuTime);
    }

    previousProcesses_ = std::move(currentProcesses);
    previousSystemCpuTime_ = systemCpuTime;
    hasPreviousSystemCpuTime_ = true;
    return snapshot;
}

}  // namespace sentinel::telemetry
