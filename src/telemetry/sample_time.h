#pragma once

#include <chrono>

namespace sentinel::telemetry {

struct SampleTime {
    std::chrono::steady_clock::time_point steady{};
    std::chrono::system_clock::time_point utc{};
};

}  // namespace sentinel::telemetry
