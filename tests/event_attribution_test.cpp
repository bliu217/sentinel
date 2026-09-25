#include "attribution/anomaly_event.h"
#include "detection/anomaly_detector.h"
#include "storage/event_store.h"
#include "telemetry/process_aggregator.h"

#include <chrono>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

using sentinel::attribution::attachProcessContext;
using sentinel::detection::AnomalyDetector;
using sentinel::detection::DetectionEvent;
using sentinel::detection::EventState;
using sentinel::detection::EventType;
using sentinel::storage::EventStore;
using sentinel::telemetry::ProcessSample;
using sentinel::telemetry::ProcessSnapshot;
using sentinel::telemetry::SystemSample;
using sentinel::telemetry::aggregateProcesses;
using sentinel::telemetry::selectProcessGroups;

namespace {

using Clock = std::chrono::steady_clock;

SystemSample highCpuSample(int second) {
    return SystemSample{
        .timestamp = Clock::time_point{std::chrono::seconds(second)},
        .cpuUsagePercent = 95.0,
        .memoryUsedBytes = 50,
        .memoryAvailableBytes = 50,
        .utcTimestamp = std::chrono::system_clock::time_point{std::chrono::seconds(100 + second)},
    };
}

ProcessSample process(std::uint32_t pid, const wchar_t* name, double cpu, std::uint64_t memory) {
    return ProcessSample{
        .identity = {pid, pid},
        .imageName = name,
        .cpuUsagePercent = cpu,
        .workingSetBytes = memory,
        .privateBytes = memory / 2,
    };
}

}  // namespace

TEST(EventAttribution, HighCpuEventCapturesCursorAndWslContext) {
    AnomalyDetector detector;
    EXPECT_TRUE(detector.analyze(highCpuSample(0)).empty());
    EXPECT_TRUE(detector.analyze(highCpuSample(1)).empty());
    const SystemSample system = highCpuSample(2);
    auto detections = detector.analyze(system);
    ASSERT_EQ(detections.size(), 1u);

    ProcessSnapshot processes{
        .time = {Clock::time_point{std::chrono::milliseconds(2050)},
                 std::chrono::system_clock::time_point{std::chrono::milliseconds(102050)}},
        .processes = {
            process(1, L"Cursor.exe", 20.0, 1000),
            process(2, L"CURSOR.EXE", 18.0, 2000),
            process(3, L"VmmemWSL.exe", 4.0, 6000),
        },
    };
    const auto selected = selectProcessGroups(aggregateProcesses(processes), 10, {});

    EventStore store;
    store.append(attachProcessContext(
        detections.front(), {system.timestamp, system.utcTimestamp}, &selected));

    ASSERT_EQ(store.events().size(), 1u);
    const auto& event = store.events().front();
    EXPECT_EQ(event.detection.type, EventType::HighCpu);
    EXPECT_EQ(event.detection.state, EventState::Started);
    EXPECT_EQ(event.detection.timestamp, Clock::time_point{std::chrono::seconds(0)});
    EXPECT_EQ(event.observedAt.steady, system.timestamp);
    ASSERT_TRUE(event.processContext.has_value());
    EXPECT_EQ(event.processContext->sampledAt.steady, processes.time.steady);
    ASSERT_EQ(event.processContext->groups.size(), 2u);
    EXPECT_EQ(event.processContext->groups[0].name, L"Cursor");
    EXPECT_DOUBLE_EQ(*event.processContext->groups[0].cpuUsagePercent, 38.0);
    EXPECT_EQ(event.processContext->groups[0].workingSetBytes, 3000u);
    EXPECT_EQ(event.processContext->groups[1].name, L"WSL");
    EXPECT_EQ(event.processContext->groups[1].workingSetBytes, 6000u);
}

TEST(EventAttribution, MissingProcessSnapshotLeavesContextAbsent) {
    EventStore store;
    const DetectionEvent detection{
        .type = EventType::HighMemory,
        .state = EventState::Started,
        .timestamp = Clock::time_point{std::chrono::seconds(1)},
        .value = 95.0,
    };
    store.append(attachProcessContext(detection, {}, nullptr));

    ASSERT_EQ(store.events().size(), 1u);
    EXPECT_EQ(store.events()[0].detection.type, EventType::HighMemory);
    EXPECT_FALSE(store.events()[0].processContext.has_value());
}

TEST(EventAttribution, ContextRetainsOnlySelectedGroups) {
    ProcessSnapshot processes;
    processes.processes = {
        process(1, L"Cursor.exe", 1.0, 1),
        process(2, L"VmmemWSL.exe", 1.0, 1),
        process(3, L"cpu.exe", 50.0, 2),
        process(4, L"memory.exe", 2.0, 100),
        process(5, L"small.exe", 0.0, 0),
    };
    auto selected = selectProcessGroups(aggregateProcesses(processes), 1, {L"Cursor", L"WSL"});
    const DetectionEvent detection{.type = EventType::SamplingDelay};
    EventStore store;
    store.append(attachProcessContext(detection, {}, &selected));
    selected.groups.clear();

    ASSERT_EQ(store.events().size(), 1u);
    const auto& event = store.events().front();
    ASSERT_TRUE(event.processContext.has_value());
    ASSERT_EQ(event.processContext->groups.size(), 4u);
    EXPECT_EQ(event.processContext->groups[0].name, L"Cursor");
    EXPECT_EQ(event.processContext->groups[1].name, L"WSL");
    EXPECT_EQ(event.processContext->groups[2].name, L"cpu.exe");
    EXPECT_EQ(event.processContext->groups[3].name, L"memory.exe");
}
