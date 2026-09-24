#pragma once

#include "detection/anomaly_detector.h"

#include <vector>

namespace sentinel::storage {

class EventStore {
public:
    void append(const detection::DetectionEvent& event);

    [[nodiscard]] const std::vector<detection::DetectionEvent>& events() const noexcept;

private:
    std::vector<detection::DetectionEvent> events_;
};

}  // namespace sentinel::storage
