#include "detection/anomaly_detector.h"

#include <chrono>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

using sentinel::detection::AnomalyDetector;
using sentinel::detection::DetectionEvent;
using sentinel::detection::EventState;
using sentinel::detection::EventType;
using sentinel::telemetry::SystemSample;

namespace {

using Clock = std::chrono::steady_clock;

SystemSample sampleAt(
    std::chrono::milliseconds time,
    double cpuPercent = 0.0,
    std::uint64_t memoryUsed = 50,
    std::uint64_t memoryAvailable = 50) {
    return SystemSample{
        .timestamp = Clock::time_point{time},
        .cpuUsagePercent = cpuPercent,
        .memoryUsedBytes = memoryUsed,
        .memoryAvailableBytes = memoryAvailable,
    };
}

std::vector<DetectionEvent> analyzeAt(
    AnomalyDetector& detector,
    int seconds,
    double cpuPercent = 0.0,
    std::uint64_t memoryUsed = 50,
    std::uint64_t memoryAvailable = 50) {
    return detector.analyze(sampleAt(std::chrono::seconds(seconds), cpuPercent, memoryUsed, memoryAvailable));
}

}  // namespace

TEST(AnomalyDetector, CpuThresholdIsInclusiveAndUsesFirstConfirmationSample) {
    AnomalyDetector detector;

    EXPECT_TRUE(analyzeAt(detector, 0, 90.0).empty());
    EXPECT_TRUE(analyzeAt(detector, 1, 90.0).empty());
    const auto events = analyzeAt(detector, 2, 90.0);

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::HighCpu);
    EXPECT_EQ(events[0].state, EventState::Started);
    EXPECT_EQ(events[0].timestamp, Clock::time_point{std::chrono::seconds(0)});
    EXPECT_DOUBLE_EQ(events[0].value, 90.0);
}

TEST(AnomalyDetector, MemoryThresholdIsInclusive) {
    AnomalyDetector detector;

    EXPECT_TRUE(analyzeAt(detector, 0, 0.0, 90, 10).empty());
    EXPECT_TRUE(analyzeAt(detector, 1, 0.0, 90, 10).empty());
    const auto events = analyzeAt(detector, 2, 0.0, 90, 10);

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::HighMemory);
    EXPECT_EQ(events[0].state, EventState::Started);
    EXPECT_DOUBLE_EQ(events[0].value, 90.0);
}

TEST(AnomalyDetector, InterruptedSequencesResetAndSustainedEpisodeDoesNotRepeat) {
    AnomalyDetector detector;

    EXPECT_TRUE(analyzeAt(detector, 0, 95.0).empty());
    EXPECT_TRUE(analyzeAt(detector, 1, 95.0).empty());
    EXPECT_TRUE(analyzeAt(detector, 2, 20.0).empty());
    EXPECT_TRUE(analyzeAt(detector, 3, 95.0).empty());
    EXPECT_TRUE(analyzeAt(detector, 4, 95.0).empty());

    const auto started = analyzeAt(detector, 5, 95.0);
    ASSERT_EQ(started.size(), 1u);
    EXPECT_EQ(started[0].state, EventState::Started);
    EXPECT_EQ(started[0].timestamp, Clock::time_point{std::chrono::seconds(3)});

    EXPECT_TRUE(analyzeAt(detector, 6, 95.0).empty());
    EXPECT_TRUE(analyzeAt(detector, 7, 20.0).empty());
    EXPECT_TRUE(analyzeAt(detector, 8, 20.0).empty());
    EXPECT_TRUE(analyzeAt(detector, 9, 95.0).empty());
    EXPECT_TRUE(analyzeAt(detector, 10, 20.0).empty());
    EXPECT_TRUE(analyzeAt(detector, 11, 20.0).empty());

    const auto stopped = analyzeAt(detector, 12, 20.0);
    ASSERT_EQ(stopped.size(), 1u);
    EXPECT_EQ(stopped[0].type, EventType::HighCpu);
    EXPECT_EQ(stopped[0].state, EventState::Stopped);
    EXPECT_EQ(stopped[0].timestamp, Clock::time_point{std::chrono::seconds(10)});
    EXPECT_DOUBLE_EQ(stopped[0].value, 20.0);
}

TEST(AnomalyDetector, CpuAndMemoryEventsCanStartTogether) {
    AnomalyDetector detector;

    EXPECT_TRUE(analyzeAt(detector, 0, 95.0, 95, 5).empty());
    EXPECT_TRUE(analyzeAt(detector, 1, 95.0, 95, 5).empty());
    const auto events = analyzeAt(detector, 2, 95.0, 95, 5);

    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].type, EventType::HighCpu);
    EXPECT_EQ(events[1].type, EventType::HighMemory);
}

TEST(AnomalyDetector, NormalAndExactThresholdSamplingGapsDoNotEmitEvents) {
    AnomalyDetector detector;

    EXPECT_TRUE(detector.analyze(sampleAt(std::chrono::milliseconds(0))).empty());
    EXPECT_TRUE(detector.analyze(sampleAt(std::chrono::milliseconds(1000))).empty());
    EXPECT_TRUE(detector.analyze(sampleAt(std::chrono::milliseconds(2500))).empty());
}

TEST(AnomalyDetector, DelayedGapEmitsOneEventWithGapInMilliseconds) {
    AnomalyDetector detector;

    EXPECT_TRUE(detector.analyze(sampleAt(std::chrono::milliseconds(0))).empty());
    const auto events = detector.analyze(sampleAt(std::chrono::milliseconds(1600)));

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::SamplingDelay);
    EXPECT_EQ(events[0].state, EventState::Occurred);
    EXPECT_EQ(events[0].timestamp, Clock::time_point{std::chrono::milliseconds(1600)});
    EXPECT_DOUBLE_EQ(events[0].value, 1600.0);

    EXPECT_TRUE(detector.analyze(sampleAt(std::chrono::milliseconds(2600))).empty());
}

TEST(AnomalyDetector, ZeroTotalMemoryIsNotAnAnomaly) {
    AnomalyDetector detector;

    EXPECT_TRUE(analyzeAt(detector, 0, 0.0, 0, 0).empty());
    EXPECT_TRUE(analyzeAt(detector, 1, 0.0, 0, 0).empty());
    EXPECT_TRUE(analyzeAt(detector, 2, 0.0, 0, 0).empty());
}
