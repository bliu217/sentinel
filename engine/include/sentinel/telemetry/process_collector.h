#pragma once

#include "sentinel/telemetry/process_sample.h"
#include "sentinel/telemetry/process_tracker.h"
#include "sentinel/telemetry/collection_result.h"

#include <mutex>
#include <optional>

namespace sentinel::telemetry {

class ProcessCollector {
public:
    [[nodiscard]] std::optional<ProcessSnapshot> collect();
    /** Return one aggregate coverage report alongside the sampled processes. */
    [[nodiscard]] CollectionResult<ProcessSnapshot> collectDetailed();

private:
    std::mutex mutex_;
    ProcessTracker tracker_;
};

}  // namespace sentinel::telemetry
