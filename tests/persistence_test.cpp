#include "storage/archive_manager.h"
#include "storage/event_store.h"
#include "storage/sqlite_store.h"

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>
#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>

using namespace std::chrono_literals;

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        static std::atomic<unsigned> next{0};
        path = std::filesystem::temp_directory_path() /
            ("sentinel-persistence-" + std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(next.fetch_add(1)));
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    std::filesystem::path path;
};

[[nodiscard]] std::chrono::system_clock::time_point at(int seconds) {
    return std::chrono::system_clock::time_point{std::chrono::seconds(seconds)};
}

[[nodiscard]] sentinel::telemetry::SystemSample system(int seconds) {
    return sentinel::telemetry::SystemSample{
        .timestamp = std::chrono::steady_clock::time_point{std::chrono::seconds(seconds)},
        .cpuUsagePercent = 97.2,
        .memoryUsedBytes = 73,
        .memoryAvailableBytes = 27,
        .utcTimestamp = at(seconds)};
}

[[nodiscard]] sentinel::telemetry::ProcessSnapshot processes(int seconds) {
    sentinel::telemetry::ProcessSnapshot snapshot;
    snapshot.time = {std::chrono::steady_clock::time_point{std::chrono::seconds(seconds)}, at(seconds)};
    snapshot.processes = {
        {.identity = {4812, 100}, .imageName = L"cursor.exe", .cpuUsagePercent = 68.3,
            .workingSetBytes = 1532 * 1024 * 1024ULL, .privateBytes = 500},
        {.identity = {4812, 200}, .imageName = L"wsl.exe", .cpuUsagePercent = std::nullopt,
            .workingSetBytes = 1000, .privateBytes = 900}};
    return snapshot;
}

[[nodiscard]] sentinel::attribution::AnomalyEvent event(int observedSecond, int occurredSecond) {
    sentinel::attribution::AnomalyEvent value;
    value.detection = {
        .type = sentinel::detection::EventType::HighCpu,
        .state = sentinel::detection::EventState::Started,
        .timestamp = std::chrono::steady_clock::time_point{std::chrono::seconds(occurredSecond)},
        .value = 97.2};
    value.observedAt = {
        std::chrono::steady_clock::time_point{std::chrono::seconds(observedSecond)}, at(observedSecond)};
    return value;
}

[[nodiscard]] std::vector<nlohmann::json> lines(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::vector<nlohmann::json> output;
    std::string line;
    while (std::getline(input, line)) output.push_back(nlohmann::json::parse(line));
    return output;
}

}  // namespace

TEST(SQLiteStore, PersistsAllProcessSamplesAndEventTimingAcrossRestart) {
    TemporaryDirectory directory;
    const auto database = directory.path / "sentinel.db";
    {
        sentinel::storage::SQLiteStore store(database);
        sentinel::storage::EventStore events(&store);
        events.append(event(12, 10));
        events.commitTick(at(12), system(12), processes(12));
        events.commitTick(at(13), std::nullopt, processes(13));
    }
    sentinel::storage::SQLiteStore reopened(database);
    const auto ticks = reopened.readTicks(at(10), at(14));
    ASSERT_EQ(ticks.size(), 2u);
    ASSERT_TRUE(ticks[0].system);
    ASSERT_TRUE(ticks[0].processes);
    ASSERT_EQ(ticks[0].processes->processes.size(), 2u);
    EXPECT_EQ(ticks[0].processes->processes[0].identity.processId, 4812u);
    EXPECT_NE(ticks[0].processes->processes[0].identity.creationTime,
              ticks[0].processes->processes[1].identity.creationTime);
    EXPECT_FALSE(ticks[0].processes->processes[1].cpuUsagePercent);
    ASSERT_EQ(ticks[0].anomalies.size(), 1u);
    EXPECT_EQ(ticks[0].anomalies[0].occurredUtcMilliseconds, 10000);
    EXPECT_EQ(ticks[0].anomalies[0].observedUtcMilliseconds, 12000);
    EXPECT_FALSE(ticks[1].system);
    EXPECT_TRUE(ticks[1].processes);
}

TEST(SQLiteStore, KeepsCandidateUtcWhenWallClockChangesBeforeConfirmation) {
    TemporaryDirectory directory;
    sentinel::storage::SQLiteStore store(directory.path / "sentinel.db");
    sentinel::detection::AnomalyDetector detector;
    auto first = system(10);
    auto second = system(11);
    auto third = system(12);
    first.utcTimestamp = at(100);
    second.utcTimestamp = at(101);
    third.utcTimestamp = at(200);
    EXPECT_TRUE(detector.analyze(first).empty());
    EXPECT_TRUE(detector.analyze(second).empty());
    const auto detections = detector.analyze(third);
    ASSERT_EQ(detections.size(), 1u);
    sentinel::attribution::AnomalyEvent stored;
    stored.detection = detections[0];
    stored.observedAt = {third.timestamp, third.utcTimestamp};
    store.writeTick(at(200), third, std::nullopt, {stored});
    const auto ticks = store.readTicks(at(200), at(201));
    ASSERT_EQ(ticks.size(), 1u);
    ASSERT_EQ(ticks[0].anomalies.size(), 1u);
    EXPECT_EQ(ticks[0].anomalies[0].occurredUtcMilliseconds, 100000);
    EXPECT_EQ(ticks[0].anomalies[0].observedUtcMilliseconds, 200000);
}

TEST(SQLiteStore, PrunesCompleteOldTicksAndRespectsHalfOpenQueries) {
    TemporaryDirectory directory;
    sentinel::storage::SQLiteStore store(directory.path / "sentinel.db");
    store.writeTick(at(1), system(1), processes(1), {event(1, 1)});
    store.writeTick(at(2), system(2), processes(2), {});
    store.pruneBefore(at(2));
    EXPECT_EQ(store.countTicks(), 1);
    EXPECT_TRUE(store.readTicks(at(1), at(2)).empty());
    const auto remaining = store.readTicks(at(2), at(3));
    ASSERT_EQ(remaining.size(), 1u);
    EXPECT_TRUE(remaining[0].anomalies.empty());
}

TEST(SQLiteStore, AppliesThirtyDayStartupRetention) {
    TemporaryDirectory directory;
    sentinel::storage::SQLiteStore store(directory.path / "sentinel.db");
    const auto now = at(40 * 24 * 60 * 60);
    store.writeTick(now - 31 * 24h, system(1), std::nullopt, {event(1, 1)});
    store.writeTick(now - 30 * 24h, system(2), std::nullopt, {});
    sentinel::storage::RetentionManager retention(store);
    retention.pruneOnStartup(now);
    EXPECT_EQ(store.countTicks(), 1);
}

TEST(SQLiteStore, SizeCapKeepsNewestAndRejectsOversizedTick) {
    TemporaryDirectory directory;
    sentinel::storage::SQLiteStore store(directory.path / "sentinel.db", 64 * 1024);
    for (int second = 1; second <= 150; ++second) {
        store.writeTick(at(second), system(second), processes(second), {});
    }
    EXPECT_LE(std::filesystem::file_size(directory.path / "sentinel.db"), 64u * 1024u);
    EXPECT_LT(store.countTicks(), 150);
    const auto latest = store.readTicks(at(150), at(151));
    ASSERT_EQ(latest.size(), 1u);
    auto huge = processes(151);
    huge.processes[0].imageName = std::wstring(100000, L'x');
    EXPECT_THROW(store.writeTick(at(151), system(151), huge, {}), std::exception);
    EXPECT_TRUE(store.readTicks(at(150), at(151)).size() == 1u);
}

TEST(ArchiveManager, WritesFilteredJsonlAndDeduplicatesRepeatedAdds) {
    TemporaryDirectory directory;
    sentinel::storage::SQLiteStore store(directory.path / "sentinel.db");
    store.writeTick(at(12), system(12), processes(12), {event(12, 10)});
    sentinel::storage::ArchiveManager archive(store, directory.path / "archive");
    archive.createProject("cursor-investigation", "Cursor lag investigation");
    sentinel::storage::ArchiveSelection selection{
        .from = at(12), .to = at(13), .applications = {L"CURSOR.EXE"},
        .eventTypes = {sentinel::detection::EventType::HighCpu}, .includeSamples = true};
    archive.addToProject("cursor-investigation", selection);
    archive.addToProject("cursor-investigation", selection);
    const auto project = directory.path / "archive" / "cursor-investigation";
    const auto anomalies = lines(project / "anomalies.jsonl");
    const auto samples = lines(project / "samples.jsonl");
    ASSERT_EQ(anomalies.size(), 1u);
    ASSERT_EQ(samples.size(), 1u);
    EXPECT_EQ(anomalies[0]["event_type"], "cpu_anomaly");
    EXPECT_EQ(anomalies[0]["timestamp"], "1970-01-01T00:00:10.000Z");
    ASSERT_EQ(anomalies[0]["processes"].size(), 1u);
    EXPECT_EQ(anomalies[0]["processes"][0]["pid"], 4812);
    ASSERT_EQ(samples[0]["processes"].size(), 1u);
    EXPECT_EQ(samples[0]["system"]["cpu_percent"], 97.2);
    const auto manifest = nlohmann::json::parse(std::ifstream(project / "manifest.json"));
    EXPECT_EQ(manifest["contains"], nlohmann::json::array({"cpu_anomaly"}));
    EXPECT_EQ(manifest["files"]["samples"], "samples.jsonl");
}

TEST(ArchiveManager, RepairsUncommittedTailBeforeAppend) {
    TemporaryDirectory directory;
    sentinel::storage::SQLiteStore store(directory.path / "sentinel.db");
    store.writeTick(at(12), system(12), processes(12), {event(12, 10)});
    sentinel::storage::ArchiveManager archive(store, directory.path / "archive");
    archive.createProject("incident", "Incident");
    const sentinel::storage::ArchiveSelection selection{.from = at(12), .to = at(13)};
    archive.addToProject("incident", selection);
    const auto anomalyPath = directory.path / "archive" / "incident" / "anomalies.jsonl";
    {
        std::ofstream output(anomalyPath, std::ios::app);
        output << "partial broken line";
    }
    archive.addToProject("incident", selection);
    EXPECT_EQ(lines(anomalyPath).size(), 1u);
}

TEST(ArchiveManager, DifferentApplicationSelectionsKeepTheirDistinctProcessViews) {
    TemporaryDirectory directory;
    sentinel::storage::SQLiteStore store(directory.path / "sentinel.db");
    store.writeTick(at(12), system(12), processes(12), {event(12, 10)});
    sentinel::storage::ArchiveManager archive(store, directory.path / "archive");
    archive.createProject("incident", "Incident");
    sentinel::storage::ArchiveSelection cursor{.from = at(12), .to = at(13),
        .applications = {L"cursor.exe"}, .includeSamples = true};
    sentinel::storage::ArchiveSelection wsl{.from = at(12), .to = at(13),
        .applications = {L"wsl.exe"}, .includeSamples = true};
    archive.addToProject("incident", cursor);
    archive.addToProject("incident", wsl);
    const auto project = directory.path / "archive" / "incident";
    const auto samples = lines(project / "samples.jsonl");
    ASSERT_EQ(samples.size(), 2u);
    EXPECT_EQ(samples[0]["processes"][0]["name"], "cursor.exe");
    EXPECT_EQ(samples[1]["processes"][0]["name"], "wsl.exe");
}
