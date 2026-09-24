#include "storage/process_history.h"

#include <stdexcept>
#include <utility>

namespace sentinel::storage {

ProcessHistory::ProcessHistory(std::size_t capacity) : capacity_(capacity) {
    if (capacity == 0) {
        throw std::invalid_argument("ProcessHistory capacity must be greater than zero");
    }
}

void ProcessHistory::push(telemetry::ProcessGroupSnapshot snapshot) {
    std::lock_guard lock(mutex_);
    if (snapshots_.size() == capacity_) {
        snapshots_.pop_front();
    }
    snapshots_.push_back(std::move(snapshot));
}

std::vector<telemetry::ProcessGroupSnapshot> ProcessHistory::snapshot() const {
    std::lock_guard lock(mutex_);
    return {snapshots_.begin(), snapshots_.end()};
}

std::size_t ProcessHistory::size() const {
    std::lock_guard lock(mutex_);
    return snapshots_.size();
}

}  // namespace sentinel::storage
