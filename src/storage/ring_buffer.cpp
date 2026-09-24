#include "storage/ring_buffer.h"

#include <stdexcept>

namespace sentinel::storage {

RingBuffer::RingBuffer(std::size_t capacity) : capacity_(capacity) {
    if (capacity_ == 0) {
        throw std::invalid_argument("RingBuffer capacity must be greater than zero");
    }
    buffer_.resize(capacity_);
}

void RingBuffer::push(const telemetry::SystemSample& sample) {
    std::lock_guard lock(mutex_);
    if (size_ == capacity_) {
        buffer_[head_] = sample;
        head_ = (head_ + 1) % capacity_;
        return;
    }

    buffer_[(head_ + size_) % capacity_] = sample;
    ++size_;
}

std::vector<telemetry::SystemSample> RingBuffer::snapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<telemetry::SystemSample> samples;
    samples.reserve(size_);
    for (std::size_t i = 0; i < size_; ++i) {
        samples.push_back(buffer_[(head_ + i) % capacity_]);
    }
    return samples;
}

std::size_t RingBuffer::size() const {
    std::lock_guard lock(mutex_);
    return size_;
}

std::size_t RingBuffer::capacity() const noexcept {
    return capacity_;
}

bool RingBuffer::empty() const {
    std::lock_guard lock(mutex_);
    return size_ == 0;
}

}  // namespace sentinel::storage
