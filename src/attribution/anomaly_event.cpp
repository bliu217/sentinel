#include "attribution/anomaly_event.h"

namespace sentinel::attribution {

AnomalyEvent attachProcessContext(
    detection::DetectionEvent detection,
    telemetry::SampleTime observedAt,
    const telemetry::ProcessGroupSnapshot* processSnapshot) {
    AnomalyEvent event{
        .detection = detection,
        .observedAt = observedAt,
    };
    if (processSnapshot != nullptr) {
        event.processContext = ProcessContext{
            .sampledAt = processSnapshot->time,
            .groups = processSnapshot->groups,
        };
    }
    return event;
}

}  // namespace sentinel::attribution
