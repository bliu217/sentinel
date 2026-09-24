#pragma once

#include "attribution/anomaly_event.h"

#include <vector>

namespace sentinel::storage {

class EventStore {
public:
    void append(attribution::AnomalyEvent event);

    [[nodiscard]] const std::vector<attribution::AnomalyEvent>& events() const noexcept;

private:
    std::vector<attribution::AnomalyEvent> events_;
};

}  // namespace sentinel::storage
