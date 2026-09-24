#pragma once

#include "telemetry/system_collector.h"

#include <cstddef>
#include <mutex>
#include <vector>

namespace sentinel::storage {

class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity);
    void push(const telemetry::SystemSample& sample);

    [[nodiscard]] std::vector<telemetry::SystemSample> snapshot() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] bool empty() const;

private:
    mutable std::mutex mutex_;
    std::vector<telemetry::SystemSample> buffer_;
    std::size_t capacity_;
    std::size_t head_{};
    std::size_t size_{};
};

}  // namespace sentinel::storage
