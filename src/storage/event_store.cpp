#include "storage/event_store.h"

namespace sentinel::storage {

void EventStore::append(const detection::DetectionEvent& event) {
    events_.push_back(event);
}

const std::vector<detection::DetectionEvent>& EventStore::events() const noexcept {
    return events_;
}

}  // namespace sentinel::storage
