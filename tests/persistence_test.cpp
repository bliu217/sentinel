#include "storage/archive_manager.h"
#include "storage/event_store.h"
#include "storage/sqlite_store.h"
#include "telemetry/process_aggregator.h"

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>
#include <sqlite3.h>
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
        {.identity = {4812, 100}, .imageName = L"Cursor.exe", .cpuUsagePercent = 68.3,
            .workingSetBytes = 1532 * 1024 * 1024ULL, .privateBytes = 500},
        {.identity = {4812, 200}, .imageName = L"VmmemWSL.exe", .cpuUsagePercent = std::nullopt,
            .workingSetBytes = 1000, .privateBytes = 900}};
    return snapshot;
}

[[nodiscard]] sentinel::telemetry::ProcessGroupSnapshot selectedProcesses(int seconds) {
    return sentinel::telemetry::selectTopProcesses(
        sentinel::telemetry::aggregateProcesses(processes(seconds)));
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

[[nodiscard]] sentinel::attribution::AnomalyEvent attributedEvent(int observedSecond, int occurredSecond) {
    auto value = event(observedSecond, occurredSecond);
    value.processContext = sentinel::attribution::ProcessContext{
        .sampledAt = {std::chrono::steady_clock::time_point{std::chrono::seconds(11)}, at(11)},
        .groups = {
            {.name = L"Cursor", .cpuUsagePercent = 38.0, .workingSetBytes = 3000,
                .privateBytes = 2000, .processCount = 2},
            {.name = L"WSL", .cpuUsagePercent = std::nullopt, .workingSetBytes = 6000,
                .privateBytes = 5000, .processCount = 1}}};
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

TEST(SQLiteStore, PersistsSelectedProcessGroupsAndEventTimingAcrossRestart) {
    TemporaryDirectory directory;
    const auto database = directory.path / "sentinel.db";
    {
        sentinel::storage::SQLiteStore store(database);
        sentinel::storage::EventStore events(&store);
        events.append(event(12, 10));
        events.commitTick(at(12), system(12), selectedProcesses(12));
        events.commitTick(at(13), std::nullopt, selectedProcesses(13));
    }
    sentinel::storage::SQLiteStore reopened(database);
    const auto ticks = reopened.readTicks(at(10), at(14));
    ASSERT_EQ(ticks.size(), 2u);
    ASSERT_TRUE(ticks[0].system);
    ASSERT_TRUE(ticks[0].processContext);
    ASSERT_EQ(ticks[0].processContext->groups.size(), 2u);
    EXPECT_EQ(ticks[0].processContext->time.utc, at(12));
    EXPECT_EQ(ticks[0].processContext->groups[0].name, L"Cursor");
    EXPECT_EQ(ticks[0].processContext->groups[1].name, L"WSL");
    EXPECT_FALSE(ticks[0].processContext->groups[1].cpuUsagePercent);
    ASSERT_EQ(ticks[0].anomalies.size(), 1u);
    EXPECT_EQ(ticks[0].anomalies[0].occurredUtcMilliseconds, 10000);
    EXPECT_EQ(ticks[0].anomalies[0].observedUtcMilliseconds, 12000);
    EXPECT_FALSE(ticks[1].system);
    EXPECT_TRUE(ticks[1].processContext);
}

TEST(SQLiteStore, WritesDefaultTopTenPerMetricWithoutRawProcessRows) {
    TemporaryDirectory directory;
    const auto database = directory.path / "sentinel.db";
    sentinel::telemetry::ProcessGroupSnapshot grouped;
    grouped.time.utc = at(12);
    grouped.groups.push_back({.name = L"Cursor", .cpuUsagePercent = 0.0,
        .workingSetBytes = 0, .processCount = 2});
    grouped.groups.push_back({.name = L"WSL", .cpuUsagePercent = 0.0,
        .workingSetBytes = 0, .processCount = 1});
    for (int index = 0; index < 24; ++index) {
        grouped.groups.push_back({.name = L"group" + std::to_wstring(index),
            .cpuUsagePercent = static_cast<double>(24 - index),
            .workingSetBytes = static_cast<std::uint64_t>(index + 1), .processCount = 1});
    }
    const auto selected = sentinel::telemetry::selectTopProcesses(grouped);
    ASSERT_EQ(selected.groups.size(), 22u);
    {
        sentinel::storage::SQLiteStore store(database);
        store.writeTick(at(12), system(12), selected, {});
    }
    sentinel::storage::SQLiteStore reopened(database);
    const auto ticks = reopened.readTicks(at(12), at(13));
    ASSERT_EQ(ticks.size(), 1u);
    ASSERT_TRUE(ticks[0].processContext);
    ASSERT_EQ(ticks[0].processContext->groups.size(), 22u);
    EXPECT_EQ(ticks[0].processContext->groups.front().name, L"Cursor");
    EXPECT_EQ(ticks[0].processContext->groups.back().name, L"group23");
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open16(database.c_str(), &db), SQLITE_OK);
    sqlite3_stmt* query = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='process_samples'",
        -1, &query, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(query), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(query, 0), 0);
    sqlite3_finalize(query);
    sqlite3_close(db);
}

TEST(SQLiteStore, ReadsLegacyPidRowsAsSelectedGroups) {
    TemporaryDirectory directory;
    const auto database = directory.path / "sentinel.db";
    { sentinel::storage::SQLiteStore store(database); }
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open16(database.c_str(), &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db,
        "CREATE TABLE process_samples (tick_id INTEGER NOT NULL REFERENCES ticks(id) ON DELETE CASCADE,"
        "pid INTEGER NOT NULL, creation_time INTEGER NOT NULL, image_name TEXT NOT NULL,"
        "cpu_percent REAL, working_set_bytes INTEGER NOT NULL, private_bytes INTEGER NOT NULL);"
        "INSERT INTO ticks(utc_ms,process_utc_ms) VALUES(12000,12000);"
        "INSERT INTO process_samples VALUES(1,4812,100,'Cursor.exe',20,1000,500);"
        "INSERT INTO process_samples VALUES(1,4813,200,'Cursor.exe',18,2000,700);",
        nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(db);
    sentinel::storage::SQLiteStore reopened(database);
    const auto ticks = reopened.readTicks(at(12), at(13));
    ASSERT_EQ(ticks.size(), 1u);
    ASSERT_TRUE(ticks[0].processContext);
    ASSERT_EQ(ticks[0].processContext->groups.size(), 1u);
    EXPECT_EQ(ticks[0].processContext->groups[0].name, L"Cursor");
    EXPECT_DOUBLE_EQ(*ticks[0].processContext->groups[0].cpuUsagePercent, 38.0);
    EXPECT_EQ(ticks[0].processContext->groups[0].workingSetBytes, 3000u);
    EXPECT_EQ(ticks[0].processContext->groups[0].processCount, 2u);
}

TEST(SQLiteStore, PersistsAnomalyContextIndependentlyOfTickProcessGroups) {
    TemporaryDirectory directory;
    const auto database = directory.path / "sentinel.db";
    {
        sentinel::storage::SQLiteStore store(database);
        store.writeTick(at(12), system(12), selectedProcesses(12),
            {attributedEvent(12, 10), event(12, 10)});
    }
    sentinel::storage::SQLiteStore reopened(database);
    const auto ticks = reopened.readTicks(at(12), at(13));
    ASSERT_EQ(ticks.size(), 1u);
    ASSERT_EQ(ticks[0].anomalies.size(), 2u);
    ASSERT_TRUE(ticks[0].anomalies[0].processContext);
    const auto& context = *ticks[0].anomalies[0].processContext;
    EXPECT_EQ(context.sampledAt.utc, at(11));
    ASSERT_EQ(context.groups.size(), 2u);
    EXPECT_EQ(context.groups[0].name, L"Cursor");
    EXPECT_DOUBLE_EQ(*context.groups[0].cpuUsagePercent, 38.0);
    EXPECT_EQ(context.groups[0].workingSetBytes, 3000u);
    EXPECT_EQ(context.groups[0].privateBytes, 2000u);
    EXPECT_EQ(context.groups[0].processCount, 2u);
    EXPECT_EQ(context.groups[1].name, L"WSL");
    EXPECT_FALSE(context.groups[1].cpuUsagePercent);
    EXPECT_FALSE(ticks[0].anomalies[1].processContext);
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
    store.writeTick(at(1), system(1), selectedProcesses(1), {event(1, 1)});
    store.writeTick(at(2), system(2), selectedProcesses(2), {});
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
        store.writeTick(at(second), system(second), selectedProcesses(second), {});
    }
    EXPECT_LE(std::filesystem::file_size(directory.path / "sentinel.db"), 64u * 1024u);
    EXPECT_LT(store.countTicks(), 150);
    const auto latest = store.readTicks(at(150), at(151));
    ASSERT_EQ(latest.size(), 1u);
    auto huge = selectedProcesses(151);
    huge.groups[0].name = std::wstring(100000, L'x');
    EXPECT_THROW(store.writeTick(at(151), system(151), huge, {}), std::exception);
    EXPECT_TRUE(store.readTicks(at(150), at(151)).size() == 1u);
}

TEST(ArchiveManager, WritesFilteredJsonlAndDeduplicatesRepeatedAdds) {
    TemporaryDirectory directory;
    sentinel::storage::SQLiteStore store(directory.path / "sentinel.db");
    store.writeTick(at(12), system(12), selectedProcesses(12), {attributedEvent(12, 10)});
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
    EXPECT_EQ(anomalies[0]["process_sampled_at"], "1970-01-01T00:00:11.000Z");
    EXPECT_EQ(anomalies[0]["processes"][0]["name"], "Cursor");
    EXPECT_EQ(anomalies[0]["processes"][0]["cpu_percent"], 38.0);
    EXPECT_EQ(anomalies[0]["processes"][0]["working_set_bytes"], 3000);
    EXPECT_EQ(anomalies[0]["processes"][0]["process_count"], 2);
    ASSERT_EQ(samples[0]["processes"].size(), 1u);
    EXPECT_EQ(samples[0]["processes"][0]["name"], "Cursor");
    EXPECT_FALSE(samples[0]["processes"][0].contains("pid"));
    EXPECT_EQ(samples[0]["system"]["cpu_percent"], 97.2);
    const auto manifest = nlohmann::json::parse(std::ifstream(project / "manifest.json"));
    EXPECT_EQ(manifest["contains"], nlohmann::json::array({"cpu_anomaly"}));
    EXPECT_EQ(manifest["files"]["samples"], "samples.jsonl");
}

TEST(ArchiveManager, ExportsStoredAnomalyContextRatherThanTickContext) {
    TemporaryDirectory directory;
    sentinel::storage::SQLiteStore store(directory.path / "sentinel.db");
    store.writeTick(at(12), system(12), selectedProcesses(12), {attributedEvent(12, 10)});
    sentinel::storage::ArchiveManager archive(store, directory.path / "archive");
    archive.createProject("incident", "Incident");
    archive.addToProject("incident", {.from = at(12), .to = at(13), .includeSamples = true});
    const auto project = directory.path / "archive" / "incident";
    const auto anomalies = lines(project / "anomalies.jsonl");
    const auto samples = lines(project / "samples.jsonl");
    ASSERT_EQ(anomalies.size(), 1u);
    ASSERT_EQ(samples.size(), 1u);
    ASSERT_EQ(anomalies[0]["processes"].size(), 2u);
    EXPECT_EQ(anomalies[0]["processes"][0]["name"], "Cursor");
    EXPECT_EQ(anomalies[0]["processes"][1]["name"], "WSL");
    EXPECT_TRUE(anomalies[0]["processes"][1]["cpu_percent"].is_null());
    EXPECT_FALSE(anomalies[0]["processes"][0].contains("pid"));
    ASSERT_EQ(samples[0]["processes"].size(), 2u);
    EXPECT_EQ(samples[0]["processes"][0]["name"], "Cursor");
}

TEST(ArchiveManager, RepairsUncommittedTailBeforeAppend) {
    TemporaryDirectory directory;
    sentinel::storage::SQLiteStore store(directory.path / "sentinel.db");
    store.writeTick(at(12), system(12), selectedProcesses(12), {event(12, 10)});
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
    store.writeTick(at(12), system(12), selectedProcesses(12), {event(12, 10)});
    sentinel::storage::ArchiveManager archive(store, directory.path / "archive");
    archive.createProject("incident", "Incident");
    sentinel::storage::ArchiveSelection cursor{.from = at(12), .to = at(13),
        .applications = {L"cursor.exe"}, .includeSamples = true};
    sentinel::storage::ArchiveSelection wsl{.from = at(12), .to = at(13),
        .applications = {L"vmmemwsl.exe"}, .includeSamples = true};
    archive.addToProject("incident", cursor);
    archive.addToProject("incident", wsl);
    const auto project = directory.path / "archive" / "incident";
    const auto samples = lines(project / "samples.jsonl");
    ASSERT_EQ(samples.size(), 2u);
    EXPECT_EQ(samples[0]["processes"][0]["name"], "Cursor");
    EXPECT_EQ(samples[1]["processes"][0]["name"], "WSL");
}
