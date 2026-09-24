#pragma once

#include "telemetry/process_sample.h"
#include "telemetry/process_tracker.h"

#include <mutex>
#include <optional>

namespace sentinel::telemetry {

class ProcessCollector {
public:
    [[nodiscard]] std::optional<ProcessSnapshot> collect();

private:
    std::mutex mutex_;
    ProcessTracker tracker_;
};

}  // namespace sentinel::telemetry
