#pragma once

#include "telemetry/process_aggregator.h"

#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

namespace sentinel::storage {

class ProcessHistory {
public:
    explicit ProcessHistory(std::size_t capacity);
    void push(telemetry::ProcessGroupSnapshot snapshot);
    [[nodiscard]] std::vector<telemetry::ProcessGroupSnapshot> snapshot() const;
    [[nodiscard]] std::size_t size() const;

private:
    mutable std::mutex mutex_;
    std::deque<telemetry::ProcessGroupSnapshot> snapshots_;
    std::size_t capacity_;
};

}  // namespace sentinel::storage
