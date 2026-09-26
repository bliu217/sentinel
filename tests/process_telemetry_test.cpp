#include "monitoring_config.h"
#include "storage/process_history.h"
#include "telemetry/process_aggregator.h"
#include "telemetry/process_collector.h"
#include "telemetry/process_math.h"
#include "telemetry/process_tracker.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

using sentinel::storage::ProcessHistory;
using sentinel::telemetry::ProcessCollector;
using sentinel::telemetry::ProcessGroup;
using sentinel::telemetry::ProcessGroupSnapshot;
using sentinel::telemetry::ProcessIdentity;
using sentinel::telemetry::ProcessSample;
using sentinel::telemetry::ProcessSnapshot;
using sentinel::telemetry::ProcessTracker;
using sentinel::telemetry::RawProcessObservation;
using sentinel::telemetry::SampleTime;
using sentinel::telemetry::aggregateProcesses;
using sentinel::telemetry::formatProcessSummary;
using sentinel::telemetry::processCpuUsagePercent;
using sentinel::telemetry::selectProcessGroups;

namespace {

TEST(MonitoringConfig, RejectsInvalidRuntimeSettings) {
    sentinel::MonitoringConfig config;
    EXPECT_NO_THROW(config.validate());
    config.sampleInterval = std::chrono::milliseconds::zero();
    EXPECT_THROW(config.validate(), std::invalid_argument);
    config.sampleInterval = std::chrono::milliseconds(1000);
    config.inMemoryHistorySize = 0;
    EXPECT_THROW(config.validate(), std::invalid_argument);
    config.inMemoryHistorySize = 300;
    config.retentionPeriod = std::chrono::days::zero();
    EXPECT_THROW(config.validate(), std::invalid_argument);
    config.retentionPeriod = std::chrono::days(30);
    config.maxDatabaseSizeMiB = 0;
    EXPECT_THROW(config.validate(), std::invalid_argument);
}

RawProcessObservation raw(
    std::uint32_t pid,
    std::uint64_t created,
    std::uint64_t cpu,
    const wchar_t* image = L"Cursor.exe") {
    return RawProcessObservation{
        .identity = {pid, created},
        .imageName = image,
        .cpuTime = cpu,
        .workingSetBytes = 1024,
        .privateBytes = 512,
    };
}

ProcessSample process(
    std::uint32_t pid,
    const wchar_t* image,
    double cpu,
    std::uint64_t memory) {
    return ProcessSample{
        .identity = {pid, pid},
        .imageName = image,
        .cpuUsagePercent = cpu,
        .workingSetBytes = memory,
        .privateBytes = memory / 2,
    };
}

}  // namespace

TEST(ProcessCpuMath, DistinguishesZeroUsageFromInvalidDeltas) {
    EXPECT_EQ(processCpuUsagePercent(10, 48, 100, 200), 38.0);
    EXPECT_EQ(processCpuUsagePercent(10, 10, 100, 200), 0.0);
    EXPECT_EQ(processCpuUsagePercent(10, 60, 100, 200), 50.0);
    EXPECT_EQ(processCpuUsagePercent(10, 200, 100, 200), 100.0);
    EXPECT_FALSE(processCpuUsagePercent(10, 20, 100, 100).has_value());
    EXPECT_FALSE(processCpuUsagePercent(10, 20, 200, 100).has_value());
    EXPECT_FALSE(processCpuUsagePercent(20, 10, 100, 200).has_value());
}

TEST(ProcessCollector, SamplesItsOwnProcessAcrossTwoTicks) {
    ProcessCollector collector;
    const auto first = collector.collect();
    ASSERT_TRUE(first.has_value());
    const auto ownPid = GetCurrentProcessId();
    const auto findSelf = [&](const ProcessSnapshot& snapshot) {
        return std::find_if(snapshot.processes.begin(), snapshot.processes.end(), [&](const ProcessSample& process) {
            return process.identity.processId == ownPid;
        });
    };
    const auto firstSelf = findSelf(*first);
    ASSERT_NE(firstSelf, first->processes.end());
    EXPECT_FALSE(firstSelf->cpuUsagePercent.has_value());
    wchar_t executablePath[MAX_PATH]{};
    const DWORD pathLength = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
    ASSERT_GT(pathLength, 0u);
    ASSERT_LT(pathLength, static_cast<DWORD>(MAX_PATH));
    const std::wstring_view fullPath(executablePath, pathLength);
    const auto separator = fullPath.find_last_of(L"\\/");
    EXPECT_EQ(firstSelf->imageName, fullPath.substr(
        separator == std::wstring_view::npos ? 0 : separator + 1));

    Sleep(20);
    const auto second = collector.collect();
    ASSERT_TRUE(second.has_value());
    const auto secondSelf = findSelf(*second);
    ASSERT_NE(secondSelf, second->processes.end());
    EXPECT_EQ(secondSelf->identity, firstSelf->identity);
    EXPECT_TRUE(secondSelf->cpuUsagePercent.has_value());
    EXPECT_GT(secondSelf->workingSetBytes, 0u);
    EXPECT_GT(second->time.utc.time_since_epoch().count(), 0);
}

TEST(ProcessTracker, NewProcessNeedsBaselineAndPidReuseStartsNewBaseline) {
    ProcessTracker tracker;
    auto first = tracker.update({}, 100, {raw(42, 1, 10)});
    ASSERT_EQ(first.processes.size(), 1u);
    EXPECT_FALSE(first.processes[0].cpuUsagePercent.has_value());
    EXPECT_EQ(first.processes[0].workingSetBytes, 1024u);

    auto second = tracker.update({}, 200, {raw(42, 1, 48)});
    ASSERT_EQ(second.processes.size(), 1u);
    ASSERT_TRUE(second.processes[0].cpuUsagePercent.has_value());
    EXPECT_DOUBLE_EQ(*second.processes[0].cpuUsagePercent, 38.0);

    auto reused = tracker.update({}, 300, {raw(42, 2, 5)});
    ASSERT_EQ(reused.processes.size(), 1u);
    EXPECT_FALSE(reused.processes[0].cpuUsagePercent.has_value());
}

TEST(ProcessTracker, DisappearedProcessLosesItsBaseline) {
    ProcessTracker tracker;
    static_cast<void>(tracker.update({}, 100, {raw(7, 1, 10)}));
    static_cast<void>(tracker.update({}, 200, {}));
    auto returned = tracker.update({}, 300, {raw(7, 1, 30)});
    ASSERT_EQ(returned.processes.size(), 1u);
    EXPECT_FALSE(returned.processes[0].cpuUsagePercent.has_value());
}

TEST(ProcessTracker, InvalidDeltasRemainUnavailableAndRecoverNextTick) {
    ProcessTracker tracker;
    static_cast<void>(tracker.update({}, 100, {raw(7, 1, 10)}));

    const auto zeroSystemDelta = tracker.update({}, 100, {raw(7, 1, 20)});
    ASSERT_EQ(zeroSystemDelta.processes.size(), 1u);
    EXPECT_FALSE(zeroSystemDelta.processes[0].cpuUsagePercent.has_value());

    const auto backwardsProcessTime = tracker.update({}, 200, {raw(7, 1, 5)});
    ASSERT_EQ(backwardsProcessTime.processes.size(), 1u);
    EXPECT_FALSE(backwardsProcessTime.processes[0].cpuUsagePercent.has_value());

    const auto recovered = tracker.update({}, 300, {raw(7, 1, 5)});
    ASSERT_EQ(recovered.processes.size(), 1u);
    ASSERT_TRUE(recovered.processes[0].cpuUsagePercent.has_value());
    EXPECT_DOUBLE_EQ(*recovered.processes[0].cpuUsagePercent, 0.0);
}

TEST(ProcessAggregator, GroupsCursorAndWslWithoutLosingOtherProcesses) {
    ProcessSnapshot snapshot;
    snapshot.processes = {
        process(1, L"Cursor.exe", 20.0, 1024),
        process(2, L"CURSOR.EXE", 18.0, 2048),
        process(3, L"VmmemWSL.exe", 4.0, 4096),
        process(4, L"vmmem.exe", 1.0, 1024),
        process(5, L"Other.exe", 2.0, 512),
    };

    const auto grouped = aggregateProcesses(snapshot);
    ASSERT_EQ(grouped.groups.size(), 3u);
    EXPECT_EQ(grouped.groups[0].name, L"Cursor");
    EXPECT_EQ(grouped.groups[0].processCount, 2u);
    EXPECT_DOUBLE_EQ(*grouped.groups[0].cpuUsagePercent, 38.0);
    EXPECT_EQ(grouped.groups[0].workingSetBytes, 3072u);
    EXPECT_EQ(grouped.groups[1].name, L"WSL");
    EXPECT_DOUBLE_EQ(*grouped.groups[1].cpuUsagePercent, 5.0);
    EXPECT_EQ(grouped.groups[1].workingSetBytes, 5120u);
    EXPECT_EQ(grouped.groups[2].name, L"other.exe");
}

TEST(ProcessAggregator, MixedCpuBaselinesDoNotUnderreportAnApplication) {
    ProcessSnapshot snapshot;
    snapshot.processes = {
        process(1, L"Cursor.exe", 20.0, 1024),
        process(2, L"Cursor.exe", 0.0, 2048),
    };
    snapshot.processes[1].cpuUsagePercent.reset();

    const auto grouped = aggregateProcesses(snapshot);
    ASSERT_EQ(grouped.groups.size(), 1u);
    EXPECT_FALSE(grouped.groups[0].cpuUsagePercent.has_value());
    EXPECT_EQ(grouped.groups[0].workingSetBytes, 3072u);
}

TEST(ProcessAggregator, RetainsPinnedGroupsAndTopCpuAndMemory) {
    ProcessGroupSnapshot snapshot;
    snapshot.groups = {
        {.name = L"Cursor", .cpuUsagePercent = 1.0, .workingSetBytes = 1},
        {.name = L"WSL", .cpuUsagePercent = 1.0, .workingSetBytes = 1},
        {.name = L"cpu.exe", .cpuUsagePercent = 50.0, .workingSetBytes = 2},
        {.name = L"memory.exe", .cpuUsagePercent = 2.0, .workingSetBytes = 100},
        {.name = L"small.exe", .cpuUsagePercent = 0.0, .workingSetBytes = 0},
    };

    const auto selected = selectProcessGroups(std::move(snapshot), 1, {L"cursor.exe", L"vmmemwsl.exe"});
    ASSERT_EQ(selected.groups.size(), 4u);
    EXPECT_EQ(selected.groups[0].name, L"Cursor");
    EXPECT_EQ(selected.groups[1].name, L"WSL");
    EXPECT_EQ(selected.groups[2].name, L"cpu.exe");
    EXPECT_EQ(selected.groups[3].name, L"memory.exe");
}

TEST(ProcessAggregator, UnpinnedGroupsHaveNoSpecialTreatment) {
    ProcessGroupSnapshot snapshot;
    snapshot.groups = {
        {.name = L"Cursor", .cpuUsagePercent = 1.0, .workingSetBytes = 1},
        {.name = L"WSL", .cpuUsagePercent = 1.0, .workingSetBytes = 1},
        {.name = L"cpu.exe", .cpuUsagePercent = 50.0, .workingSetBytes = 2},
        {.name = L"memory.exe", .cpuUsagePercent = 2.0, .workingSetBytes = 100},
    };
    const auto selected = selectProcessGroups(std::move(snapshot), 1, {});
    ASSERT_EQ(selected.groups.size(), 2u);
    EXPECT_EQ(selected.groups[0].name, L"cpu.exe");
    EXPECT_EQ(selected.groups[1].name, L"memory.exe");
}

TEST(ProcessHistory, DropsOldestSnapshotAtCapacity) {
    ProcessHistory history(2);
    for (int second = 1; second <= 3; ++second) {
        ProcessGroupSnapshot snapshot;
        snapshot.time.utc = std::chrono::system_clock::time_point{std::chrono::seconds(second)};
        history.push(std::move(snapshot));
    }
    const auto snapshots = history.snapshot();
    ASSERT_EQ(snapshots.size(), 2u);
    EXPECT_EQ(snapshots[0].time.utc, std::chrono::system_clock::time_point{std::chrono::seconds(2)});
    EXPECT_EQ(snapshots[1].time.utc, std::chrono::system_clock::time_point{std::chrono::seconds(3)});
}

TEST(ProcessSummary, FormatsRequestedGroupsAndUnavailableCpu) {
    constexpr std::uint64_t gib = 1024ull * 1024ull * 1024ull;
    ProcessGroupSnapshot snapshot;
    snapshot.groups = {
        {.name = L"Cursor", .cpuUsagePercent = 38.0, .workingSetBytes = 2 * gib},
        {.name = L"WSL", .cpuUsagePercent = std::nullopt, .workingSetBytes = 6 * gib},
    };
    EXPECT_EQ(
        formatProcessSummary(snapshot, {L"Cursor", L"WSL"}),
        L"Cursor consumed 38.0% CPU and 2.0 GiB resident memory; "
        L"WSL consumed CPU unavailable and 6.0 GiB resident memory.");
}
