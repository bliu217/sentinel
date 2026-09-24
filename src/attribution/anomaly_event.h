#pragma once

#include "detection/anomaly_detector.h"
#include "telemetry/process_aggregator.h"

#include <optional>
#include <vector>

namespace sentinel::attribution {

struct ProcessContext {
    telemetry::SampleTime sampledAt{};
    std::vector<telemetry::ProcessGroup> groups;
};

struct AnomalyEvent {
    detection::DetectionEvent detection{};
    telemetry::SampleTime observedAt{};
    std::optional<ProcessContext> processContext;
};

[[nodiscard]] AnomalyEvent attachProcessContext(
    detection::DetectionEvent detection,
    telemetry::SampleTime observedAt,
    const telemetry::ProcessGroupSnapshot* processSnapshot);

}  // namespace sentinel::attribution
