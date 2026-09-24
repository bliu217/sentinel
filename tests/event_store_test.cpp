#include "storage/event_store.h"

#include <chrono>

#include <gtest/gtest.h>

using sentinel::detection::DetectionEvent;
using sentinel::detection::EventState;
using sentinel::detection::EventType;
using sentinel::storage::EventStore;

TEST(EventStore, AppendsEventsInOrder) {
    EventStore store;
    const DetectionEvent first{
        .type = EventType::HighCpu,
        .state = EventState::Started,
        .timestamp = std::chrono::steady_clock::time_point{std::chrono::seconds(1)},
        .value = 95.0,
    };
    const DetectionEvent second{
        .type = EventType::SamplingDelay,
        .state = EventState::Occurred,
        .timestamp = std::chrono::steady_clock::time_point{std::chrono::seconds(3)},
        .value = 2000.0,
    };

    store.append(first);
    store.append(second);

    const auto& events = store.events();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].type, EventType::HighCpu);
    EXPECT_EQ(events[0].state, EventState::Started);
    EXPECT_DOUBLE_EQ(events[0].value, 95.0);
    EXPECT_EQ(events[1].type, EventType::SamplingDelay);
    EXPECT_EQ(events[1].state, EventState::Occurred);
    EXPECT_DOUBLE_EQ(events[1].value, 2000.0);
}

TEST(EventStore, StartsEmpty) {
    const EventStore store;
    EXPECT_TRUE(store.events().empty());
}
