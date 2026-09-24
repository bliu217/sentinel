#pragma once

#include "attribution/anomaly_event.h"

#include <vector>

namespace sentinel::storage {

class SQLiteStore;

class EventStore {
public:
    explicit EventStore(SQLiteStore* persistentStore = nullptr) : persistentStore_(persistentStore) {}
    void append(attribution::AnomalyEvent event);
    void commitTick(
        std::chrono::system_clock::time_point tickUtc,
        const std::optional<telemetry::SystemSample>& system,
        const std::optional<telemetry::ProcessSnapshot>& processes);

    [[nodiscard]] const std::vector<attribution::AnomalyEvent>& events() const noexcept;

private:
    std::vector<attribution::AnomalyEvent> events_;
    SQLiteStore* persistentStore_{};
};

}  // namespace sentinel::storage
