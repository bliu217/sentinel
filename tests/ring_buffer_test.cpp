#include "storage/ring_buffer.h"

#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using sentinel::storage::RingBuffer;
using sentinel::telemetry::SystemSample;

namespace {

SystemSample sampleWithId(std::uint64_t id) {
    SystemSample sample;
    sample.memoryUsedBytes = id;
    return sample;
}

std::vector<std::uint64_t> idsOf(const std::vector<SystemSample>& samples) {
    std::vector<std::uint64_t> ids;
    ids.reserve(samples.size());
    for (const SystemSample& sample : samples) {
        ids.push_back(sample.memoryUsedBytes);
    }
    return ids;
}

}  // namespace

TEST(RingBuffer, EmptyBuffer) {
    const RingBuffer buffer(4);
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.size(), 0u);
    EXPECT_EQ(buffer.capacity(), 4u);
    EXPECT_TRUE(buffer.snapshot().empty());
}

TEST(RingBuffer, ZeroCapacityThrows) {
    EXPECT_THROW(RingBuffer{0}, std::invalid_argument);
}

TEST(RingBuffer, PushBelowCapacityPreservesOrder) {
    RingBuffer buffer(4);
    buffer.push(sampleWithId(10));
    buffer.push(sampleWithId(20));
    buffer.push(sampleWithId(30));

    EXPECT_FALSE(buffer.empty());
    EXPECT_EQ(buffer.size(), 3u);
    EXPECT_EQ(idsOf(buffer.snapshot()), (std::vector<std::uint64_t>{10, 20, 30}));
}

TEST(RingBuffer, PushToCapacityKeepsAllSamples) {
    RingBuffer buffer(3);
    buffer.push(sampleWithId(1));
    buffer.push(sampleWithId(2));
    buffer.push(sampleWithId(3));

    EXPECT_EQ(buffer.size(), 3u);
    EXPECT_EQ(idsOf(buffer.snapshot()), (std::vector<std::uint64_t>{1, 2, 3}));
}

TEST(RingBuffer, PushPastCapacityDropsOldest) {
    RingBuffer buffer(3);
    for (std::uint64_t id = 1; id <= 5; ++id) {
        buffer.push(sampleWithId(id));
    }

    EXPECT_EQ(buffer.size(), 3u);
    EXPECT_EQ(idsOf(buffer.snapshot()), (std::vector<std::uint64_t>{3, 4, 5}));
}

TEST(RingBuffer, SnapshotIsACopy) {
    RingBuffer buffer(2);
    buffer.push(sampleWithId(1));
    buffer.push(sampleWithId(2));

    std::vector<SystemSample> first = buffer.snapshot();
    ASSERT_EQ(first.size(), 2u);
    first[0].memoryUsedBytes = 99;

    EXPECT_EQ(idsOf(buffer.snapshot()), (std::vector<std::uint64_t>{1, 2}));
}

TEST(RingBuffer, ConcurrentPushAndSnapshot) {
    constexpr std::size_t kCapacity = 8;
    constexpr std::uint64_t kCount = 1000;

    RingBuffer buffer(kCapacity);
    std::atomic<bool> writerDone{false};

    std::thread writer([&] {
        for (std::uint64_t id = 0; id < kCount; ++id) {
            buffer.push(sampleWithId(id));
        }
        writerDone.store(true, std::memory_order_release);
    });

    while (!writerDone.load(std::memory_order_acquire)) {
        const std::vector<SystemSample> samples = buffer.snapshot();
        ASSERT_LE(samples.size(), kCapacity);
        for (std::size_t i = 1; i < samples.size(); ++i) {
            EXPECT_EQ(samples[i].memoryUsedBytes, samples[i - 1].memoryUsedBytes + 1);
        }
    }

    writer.join();

    const std::vector<std::uint64_t> finalIds = idsOf(buffer.snapshot());
    ASSERT_EQ(finalIds.size(), kCapacity);
    EXPECT_EQ(finalIds.front(), kCount - kCapacity);
    EXPECT_EQ(finalIds.back(), kCount - 1);
    for (std::size_t i = 1; i < finalIds.size(); ++i) {
        EXPECT_EQ(finalIds[i], finalIds[i - 1] + 1);
    }
}
