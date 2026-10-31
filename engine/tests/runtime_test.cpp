#include "sentinel/runtime/monitor_runtime.h"
#include "sentinel/storage/sqlite_store.h"

#include <gtest/gtest.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
using sentinel::runtime::MonitorRuntime;
using sentinel::runtime::RuntimeState;

namespace {

class RuntimeTest : public testing::Test {
protected:
    void SetUp() override {
        static std::atomic<unsigned> next{};
        directory_ = std::filesystem::temp_directory_path() /
            ("sentinel-runtime-" + std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(next.fetch_add(1)));
        std::filesystem::create_directories(directory_);
        path_ = directory_ / "sentinel.db";
        config_.sampleInterval = 10ms;
        config_.inMemoryHistorySize = 3;
    }
    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }
    sentinel::runtime::RuntimeCollectors collectors() {
        auto counter = std::make_shared<unsigned>(0);
        return {
            [counter]() -> std::optional<sentinel::telemetry::SystemSample> {
                ++*counter;
                return sentinel::telemetry::SystemSample{
                    .timestamp = std::chrono::steady_clock::now(),
                    .cpuUsagePercent = 95.0,
                    .memoryUsedBytes = *counter,
                    .memoryAvailableBytes = 100000,
                    .utcTimestamp = std::chrono::system_clock::now()};
            },
            [counter]() -> std::optional<sentinel::telemetry::ProcessSnapshot> {
                sentinel::telemetry::ProcessSnapshot snapshot;
                snapshot.time = {std::chrono::steady_clock::now(), std::chrono::system_clock::now()};
                snapshot.processes.push_back({.identity = {1, 1}, .imageName = L"Cursor.exe",
                    .cpuUsagePercent = 95.0, .workingSetBytes = *counter, .privateBytes = *counter});
                return snapshot;
            }};
    }
    static bool await(const std::function<bool()>& predicate) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) return true;
            std::this_thread::sleep_for(1ms);
        }
        return predicate();
    }
    std::filesystem::path directory_;
    std::filesystem::path path_;
    sentinel::MonitoringConfig config_;
};

TEST_F(RuntimeTest, PublishesBoundedConsistentSnapshotsAfterPersistence) {
    MonitorRuntime runtime(config_, path_, collectors());
    runtime.start();
    ASSERT_TRUE(await([&] { return runtime.status().completedTicks >= 6; }));
    const auto retained = runtime.history();
    ASSERT_EQ(retained.size(), 3);
    const auto originalSequence = retained.front()->sequence;
    for (std::size_t index = 0; index < retained.size(); ++index) {
        const auto& tick = *retained[index];
        ASSERT_TRUE(tick.system);
        ASSERT_TRUE(tick.processGroups);
        ASSERT_EQ(tick.processGroups->groups.size(), 1);
        EXPECT_EQ(tick.sequence, originalSequence + index);
        EXPECT_EQ(tick.system->memoryUsedBytes, tick.processGroups->groups.front().workingSetBytes);
        EXPECT_EQ(tick.sequence, tick.system->memoryUsedBytes);
        EXPECT_TRUE(tick.persisted);
    }
    ASSERT_TRUE(await([&] { return runtime.status().completedTicks >= originalSequence + 6; }));
    EXPECT_EQ(retained.front()->sequence, originalSequence);
    runtime.requestStop();
    runtime.wait();
    EXPECT_EQ(runtime.status().state, RuntimeState::Stopped);
    sentinel::storage::SQLiteStore database(path_);
    const auto stored = database.readTicks(std::chrono::system_clock::now() - 1h,
        std::chrono::system_clock::now() + 1h);
    EXPECT_EQ(stored.size(), runtime.status().completedTicks);
    ASSERT_GE(stored.size(), 3);
    ASSERT_EQ(stored[2].anomalies.size(), 1);
    ASSERT_TRUE(stored[2].anomalies[0].processContext);
    EXPECT_EQ(stored[2].anomalies[0].processContext->groups[0].workingSetBytes, 3);
    EXPECT_THROW(runtime.start(), std::logic_error);
}

TEST_F(RuntimeTest, StopInterruptsLongSamplingWait) {
    config_.sampleInterval = 1h;
    MonitorRuntime runtime(config_, path_, collectors());
    runtime.start();
    ASSERT_TRUE(await([&] { return runtime.status().completedTicks >= 1; }));
    const auto started = std::chrono::steady_clock::now();
    runtime.requestStop();
    runtime.wait();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
}

TEST_F(RuntimeTest, RejectsSecondRecorderAndReleasesLockOnShutdown) {
    MonitorRuntime first(config_, path_, collectors());
    first.start();
    ASSERT_TRUE(await([&] { return first.status().completedTicks >= 1; }));
    MonitorRuntime second(config_, path_, collectors());
    second.start();
    EXPECT_THROW(second.wait(), std::runtime_error);
    EXPECT_EQ(second.status().state, RuntimeState::Failed);
    EXPECT_FALSE(second.status().lastError.empty());
    first.requestStop();
    first.wait();
    MonitorRuntime third(config_, path_, collectors());
    third.start();
    ASSERT_TRUE(await([&] { return third.status().completedTicks >= 1; }));
    third.requestStop();
    EXPECT_NO_THROW(third.wait());
}

TEST_F(RuntimeTest, ReportsUnavailableReadingsWithoutInventingZeroMeasurements) {
    sentinel::runtime::RuntimeCollectors unavailable{
        []() -> std::optional<sentinel::telemetry::SystemSample> { return std::nullopt; },
        []() -> std::optional<sentinel::telemetry::ProcessSnapshot> { return std::nullopt; }};
    MonitorRuntime runtime(config_, path_, unavailable);
    runtime.start();
    ASSERT_TRUE(await([&] { return runtime.status().completedTicks >= 3; }));
    runtime.requestStop();
    runtime.wait();
    const auto status = runtime.status();
    EXPECT_EQ(status.unavailableSystemTicks, status.completedTicks);
    EXPECT_EQ(status.unavailableProcessTicks, status.completedTicks);
    EXPECT_FALSE(status.lastPersistedAt);
    for (const auto& tick : runtime.history()) {
        EXPECT_FALSE(tick->system);
        EXPECT_FALSE(tick->processGroups);
        EXPECT_FALSE(tick->persisted);
    }
}

TEST_F(RuntimeTest, StorageStartupFailureIsObservableAndRethrown) {
    std::filesystem::create_directory(path_);
    MonitorRuntime runtime(config_, path_, collectors());
    runtime.start();
    EXPECT_THROW(runtime.wait(), std::runtime_error);
    EXPECT_EQ(runtime.status().state, RuntimeState::Failed);
    EXPECT_FALSE(runtime.status().lastError.empty());
    EXPECT_TRUE(runtime.history().empty());
}

TEST_F(RuntimeTest, WorkerFailureDoesNotPublishIncompleteTick) {
    auto adapters = collectors();
    adapters.processes = []() -> std::optional<sentinel::telemetry::ProcessSnapshot> {
        throw std::runtime_error("Injected collection failure");
    };
    MonitorRuntime runtime(config_, path_, adapters);
    runtime.start();
    EXPECT_THROW(runtime.wait(), std::runtime_error);
    EXPECT_EQ(runtime.status().lastError, "Injected collection failure");
    EXPECT_EQ(runtime.status().completedTicks, 0);
    EXPECT_TRUE(runtime.history().empty());
}

TEST_F(RuntimeTest, SlowCollectionSkipsDeadlines) {
    auto adapters = collectors();
    const auto collect = adapters.system;
    adapters.system = [collect] {
        std::this_thread::sleep_for(45ms);
        return collect();
    };
    MonitorRuntime runtime(config_, path_, adapters);
    runtime.start();
    ASSERT_TRUE(await([&] { return runtime.status().skippedDeadlines >= 3; }));
    runtime.requestStop();
    runtime.wait();
    EXPECT_GE(runtime.status().lastTickDuration, 45ms);
}

TEST_F(RuntimeTest, DestructionStopsWorkerAndInvalidConfigIsRejected) {
    {
        MonitorRuntime runtime(config_, path_, collectors());
        runtime.start();
        ASSERT_TRUE(await([&] { return runtime.status().completedTicks >= 1; }));
    }
    MonitorRuntime next(config_, path_, collectors());
    next.start();
    ASSERT_TRUE(await([&] { return next.status().completedTicks >= 1; }));
    next.requestStop();
    EXPECT_NO_THROW(next.wait());
    config_.inMemoryHistorySize = 0;
    EXPECT_THROW((MonitorRuntime(config_, path_)), std::invalid_argument);
}

}  // namespace
