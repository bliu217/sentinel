#include "storage/event_store.h"
#include "storage/sqlite_store.h"

#include <utility>

namespace sentinel::storage {

void EventStore::append(attribution::AnomalyEvent event) {
    events_.push_back(std::move(event));
}

void EventStore::commitTick(
    std::chrono::system_clock::time_point tickUtc,
    const std::optional<telemetry::SystemSample>& system,
    const std::optional<telemetry::ProcessSnapshot>& processes) {
    if (persistentStore_ == nullptr) return;
    persistentStore_->writeTick(tickUtc, system, processes, events_);
    events_.clear();
}

const std::vector<attribution::AnomalyEvent>& EventStore::events() const noexcept {
    return events_;
}

}  // namespace sentinel::storage
