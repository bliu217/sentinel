#include "storage/event_store.h"

#include <utility>

namespace sentinel::storage {

void EventStore::append(attribution::AnomalyEvent event) {
    events_.push_back(std::move(event));
}

const std::vector<attribution::AnomalyEvent>& EventStore::events() const noexcept {
    return events_;
}

}  // namespace sentinel::storage
