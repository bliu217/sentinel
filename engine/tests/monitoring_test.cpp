#include "sentinel/monitoring/client.h"
#include "monitoring/runtime_inputs.h"

#include <gtest/gtest.h>
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace std::chrono_literals;
using namespace sentinel::monitoring;

namespace {

class ChildProcess {
public:
    explicit ChildProcess(std::wstring command, bool hasConsole = false,
        std::function<void(DWORD)> beforeRun = {}) {
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                (hasConsole ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW) | (beforeRun ? CREATE_SUSPENDED : 0),
                nullptr, nullptr, &startup, &process_)) throw std::runtime_error("Cannot start test child");
        if (beforeRun) {
            try {
                beforeRun(process_.dwProcessId);
                if (ResumeThread(process_.hThread) == static_cast<DWORD>(-1)) {
                    throw std::runtime_error("Cannot resume test child");
                }
            } catch (...) {
                TerminateProcess(process_.hProcess, 99);
                WaitForSingleObject(process_.hProcess, 2000);
                CloseHandle(process_.hThread);
                CloseHandle(process_.hProcess);
                throw;
            }
        }
        CloseHandle(process_.hThread);
    }
    ~ChildProcess() {
        if (WaitForSingleObject(process_.hProcess, 0) != WAIT_OBJECT_0) {
            TerminateProcess(process_.hProcess, 99);
            WaitForSingleObject(process_.hProcess, 2000);
        }
        CloseHandle(process_.hProcess);
    }
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    [[nodiscard]] DWORD getId() const noexcept { return process_.dwProcessId; }
    [[nodiscard]] DWORD wait() const {
        if (WaitForSingleObject(process_.hProcess, 5000) != WAIT_OBJECT_0) throw std::runtime_error("Test child timed out");
        DWORD code{};
        GetExitCodeProcess(process_.hProcess, &code);
        return code;
    }
private:
    PROCESS_INFORMATION process_{};
};

[[nodiscard]] std::filesystem::path executableDirectory() {
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return std::filesystem::path(path).parent_path();
}

[[nodiscard]] std::wstring quote(const std::filesystem::path& path) {
    return L"\"" + path.wstring() + L"\"";
}

class MonitoringTest : public testing::Test {
protected:
    inline static std::atomic<unsigned> nextId{0};
    const std::wstring channel = L"test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(nextId.fetch_add(1));
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / channel;
    std::chrono::steady_clock::time_point now{};
    bool hasWriteFailure{false};
    bool hasOpenFailure{false};
    bool hasMaintenanceFailure{false};
    bool hasUnresolvedCommit{false};
    detail::SamplingPass pass;
    std::vector<detail::PendingTick> saved;
    struct Engine : detail::MonitoringEngine {
        MonitoringTest& test;
        explicit Engine(MonitoringTest& value) : test(value) {}
        detail::SamplingPass collect() override { return test.pass; }
        sentinel::storage::WriteResult save(const detail::PendingTick& tick) override {
            if (test.hasWriteFailure) return {sentinel::storage::CommitOutcome::Uncommitted, "disk unavailable"};
            if (test.hasUnresolvedCommit) return {sentinel::storage::CommitOutcome::Unresolved, "commit unknown"};
            test.saved.push_back(tick);
            return {sentinel::storage::CommitOutcome::Committed, {}, {},
                test.hasMaintenanceFailure ? "maintenance unavailable" : ""};
        }
    };
    detail::RuntimeInputs inputs() {
        return {[this]() -> std::unique_ptr<detail::MonitoringEngine> {
            if (hasOpenFailure) throw std::runtime_error("open database failed");
            return std::make_unique<Engine>(*this);
        }, [this] { return now; }, [this] {
            return std::chrono::system_clock::time_point{std::chrono::duration_cast<std::chrono::system_clock::duration>(now.time_since_epoch())};
        }};
    }
    void SetUp() override {
        std::filesystem::create_directories(directory);
        pass.system.sample = sentinel::telemetry::SystemSample{.cpuUsagePercent = 42,
            .memoryUsedBytes = 73, .memoryAvailableBytes = 27};
    }
    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
};

TEST_F(MonitoringTest, ClientReadsOnlySavedPassAndKeepsIndependentCopies) {
    Monitor monitor({}, directory / "diagnostics.log", inputs());
    MonitoringOwner owner([&] { return monitor.read(); }, channel);
    MonitoringClient client(channel);
    EXPECT_FALSE(client.read().snapshot);
    monitor.poll();
    auto first = client.read();
    ASSERT_TRUE(first.snapshot);
    ASSERT_TRUE(first.snapshot->system);
    EXPECT_DOUBLE_EQ(first.snapshot->system->cpuUsagePercent, 42);
    EXPECT_FALSE(first.snapshot->processes);
    EXPECT_EQ(first.snapshot->sequence, 1u);
    first.snapshot->system->cpuUsagePercent = 0;
    EXPECT_DOUBLE_EQ(client.readAsync().get().snapshot->system->cpuUsagePercent, 42);
    EXPECT_THROW(MonitoringOwner duplicate([&] { return monitor.read(); }, channel), std::runtime_error);
}

TEST_F(MonitoringTest, SourceWarmupFailureAndRecoveryRemainDistinctFromSavedData) {
    pass.system.sample.reset();
    pass.system.report.isBaseline = true;
    pass.system.report.hasCpuBaseline = true;
    Monitor monitor({}, directory / "diagnostics.log", inputs());
    MonitoringOwner owner([&] { return monitor.read(); }, channel);
    MonitoringClient client(channel);
    monitor.poll();
    EXPECT_EQ(client.read().health.system.availability, Availability::Baseline);
    EXPECT_FALSE(client.read().snapshot);
    pass.system.report.isBaseline = false;
    pass.system.report.operation = sentinel::telemetry::CollectionOperation::GlobalMemoryStatusEx;
    pass.system.report.errorCode = ERROR_ACCESS_DENIED;
    for (int second = 1; second <= 2; ++second) {
        now += 1s;
        monitor.poll();
    }
    auto failed = client.read();
    EXPECT_EQ(failed.health.system.consecutiveFailures, 2u);
    EXPECT_EQ(failed.health.system.report.errorCode, ERROR_ACCESS_DENIED);
    EXPECT_FALSE(failed.health.system.lastSuccessUtcMs);
    pass.system = {};
    pass.system.sample = sentinel::telemetry::SystemSample{.cpuUsagePercent = 12};
    pass.system.report.attemptedAt = std::chrono::system_clock::time_point{3s};
    pass.processes.sample.emplace();
    pass.processes.sample->time.utc = std::chrono::system_clock::time_point{3s};
    pass.processes.report.enumerated = 20;
    pass.processes.report.read = 18;
    pass.processes.report.skipped = 2;
    now += 1s;
    monitor.poll();
    auto recovered = client.read();
    EXPECT_EQ(recovered.health.system.consecutiveFailures, 0u);
    EXPECT_EQ(recovered.health.system.lastSuccessUtcMs, 3000);
    EXPECT_EQ(recovered.health.processes.report.skipped, 2u);
    ASSERT_TRUE(recovered.snapshot->processes);
    pass.processes.sample.reset();
    pass.processes.report.operation = sentinel::telemetry::CollectionOperation::EnumerateProcesses;
    now += 1s;
    monitor.poll();
    EXPECT_FALSE(client.read().snapshot->processes);
    EXPECT_TRUE(client.read().health.lastFailure);
}

TEST_F(MonitoringTest, RecoveryKeepsSavedSnapshotAndUsesOneTwoFourSecondBudget) {
    Monitor monitor({}, directory / "diagnostics.log", inputs());
    MonitoringOwner owner([&] { return monitor.read(); }, channel);
    MonitoringClient client(channel);
    monitor.poll();
    const auto original = client.read();
    hasWriteFailure = true;
    now += 1s;
    monitor.poll();
    EXPECT_EQ(client.read().health.state, MonitoringState::Recovering);
    EXPECT_EQ(client.read().snapshot->sequence, original.snapshot->sequence);
    EXPECT_EQ(client.read().health.nextRecoveryUtcMs, 2000);
    now += 999ms;
    monitor.poll();
    EXPECT_EQ(client.read().health.recoveryAttempts, 0u);
    now += 1ms;
    monitor.poll();
    EXPECT_EQ(client.read().health.recoveryAttempts, 1u);
    EXPECT_EQ(client.read().generation, 2u);
    EXPECT_EQ(client.read().snapshot->generation, 1u);
    EXPECT_EQ(client.read().health.nextRecoveryUtcMs, 4000);
    hasOpenFailure = true;
    now += 2s;
    monitor.poll();
    EXPECT_EQ(client.read().health.recoveryAttempts, 2u);
    EXPECT_EQ(client.read().health.nextRecoveryUtcMs, 8000);
    now += 3999ms;
    monitor.poll();
    EXPECT_EQ(client.read().health.state, MonitoringState::Recovering);
    now += 1ms;
    monitor.poll();
    auto exhausted = client.read();
    EXPECT_EQ(exhausted.health.state, MonitoringState::Failed);
    EXPECT_EQ(exhausted.health.recoveryAttempts, 3u);
    EXPECT_EQ(exhausted.snapshot->ownerId, original.ownerId);
    EXPECT_EQ(exhausted.snapshot->tickUtcMs, original.snapshot->tickUtcMs);
    now += 100s;
    hasWriteFailure = false;
    hasOpenFailure = false;
    monitor.poll();
    EXPECT_EQ(client.read().health.state, MonitoringState::Failed);
}

TEST_F(MonitoringTest, BriefRecoveriesDoNotReplenishBudgetButSixtySavedSecondsDo) {
    Monitor monitor({}, directory / "diagnostics.log", inputs());
    MonitoringOwner owner([&] { return monitor.read(); }, channel);
    MonitoringClient client(channel);
    monitor.poll();
    for (int attempt = 0; attempt < 3; ++attempt) {
        hasWriteFailure = true;
        now += 1s;
        monitor.poll();
        const auto oldSequence = client.read().snapshot->sequence;
        hasWriteFailure = false;
        now += std::chrono::seconds(1 << attempt);
        monitor.poll();
        const auto recovered = client.read();
        EXPECT_EQ(recovered.health.state, MonitoringState::Operating);
        EXPECT_EQ(recovered.health.recoveryAttempts, attempt + 1u);
        EXPECT_EQ(recovered.snapshot->generation, attempt + 2u);
        EXPECT_GT(recovered.snapshot->sequence, oldSequence);
        EXPECT_TRUE(recovered.health.lastFailure);
        EXPECT_EQ(recovered.health.lastGapMilliseconds, 1000 * (1 << attempt));
    }
    for (int second = 1; second <= 59; ++second) {
        now += 1s;
        monitor.poll();
    }
    EXPECT_EQ(client.read().health.recoveryAttempts, 3u);
    now += 1s;
    monitor.poll();
    EXPECT_EQ(client.read().health.recoveryAttempts, 0u);
    hasWriteFailure = true;
    now += 1s;
    monitor.poll();
    EXPECT_EQ(client.read().health.state, MonitoringState::Recovering);
}

TEST_F(MonitoringTest, FourthBriefFailureExhaustsBudgetEvenAfterSuccessfulReopens) {
    Monitor monitor({}, directory / "diagnostics.log", inputs());
    MonitoringOwner owner([&] { return monitor.read(); }, channel);
    MonitoringClient client(channel);
    monitor.poll();
    for (int attempt = 0; attempt < 3; ++attempt) {
        hasWriteFailure = true;
        now += 1s;
        monitor.poll();
        hasWriteFailure = false;
        now += std::chrono::seconds(1 << attempt);
        monitor.poll();
    }
    hasWriteFailure = true;
    now += 1s;
    monitor.poll();
    EXPECT_EQ(client.read().health.state, MonitoringState::Failed);
    EXPECT_EQ(client.read().health.recoveryAttempts, 3u);
}

TEST_F(MonitoringTest, RecoveryWithoutSavableCollectionExhaustsBudgetAndKeepsHealthReadable) {
    Monitor monitor({}, directory / "diagnostics.log", inputs());
    MonitoringOwner owner([&] { return monitor.read(); }, channel);
    MonitoringClient client(channel);
    monitor.poll();
    hasWriteFailure = true;
    now += 1s;
    monitor.poll();
    hasWriteFailure = false;
    pass.system.sample.reset();
    pass.system.report.operation = sentinel::telemetry::CollectionOperation::GetSystemTimes;
    pass.processes.report.operation = sentinel::telemetry::CollectionOperation::EnumerateProcesses;
    now += 1s;
    monitor.poll();
    const auto first = client.read();
    EXPECT_EQ(first.health.state, MonitoringState::Recovering);
    EXPECT_EQ(first.health.recoveryAttempts, 1u);
    EXPECT_EQ(first.health.nextRecoveryUtcMs, 4000);
    ASSERT_TRUE(first.snapshot);
    EXPECT_EQ(first.snapshot->generation, 1u); // Only the old pending pass was saved.
    now += 1999ms;
    monitor.poll();
    EXPECT_EQ(client.read().generation, first.generation);
    now += 1ms;
    monitor.poll();
    EXPECT_EQ(client.read().health.nextRecoveryUtcMs, 8000);
    now += 4s;
    monitor.poll();
    const auto failed = client.read();
    EXPECT_EQ(failed.health.state, MonitoringState::Failed);
    EXPECT_EQ(failed.health.recoveryAttempts, 3u);
    EXPECT_EQ(failed.snapshot->sequence, first.snapshot->sequence);
    EXPECT_FALSE(failed.health.nextRecoveryUtcMs);
}

TEST_F(MonitoringTest, RecoveryAllowsOneBaselinePassButRequiresANewSavedPass) {
    Monitor monitor({}, directory / "diagnostics.log", inputs());
    MonitoringOwner owner([&] { return monitor.read(); }, channel);
    MonitoringClient client(channel);
    monitor.poll();
    hasWriteFailure = true;
    now += 1s;
    monitor.poll();
    hasWriteFailure = false;
    pass.system.sample.reset();
    pass.system.report.isBaseline = true;
    now += 1s;
    monitor.poll();
    const auto baseline = client.read();
    EXPECT_EQ(baseline.health.state, MonitoringState::Recovering);
    EXPECT_EQ(baseline.health.system.availability, Availability::Baseline);
    EXPECT_FALSE(baseline.health.nextRecoveryUtcMs);
    EXPECT_EQ(baseline.health.recoveryAttempts, 1u);
    EXPECT_EQ(baseline.snapshot->generation, 1u);
    pass.system.report = {};
    pass.system.sample = sentinel::telemetry::SystemSample{.cpuUsagePercent = 12};
    now += 1s;
    monitor.poll();
    const auto recovered = client.read();
    EXPECT_EQ(recovered.health.state, MonitoringState::Operating);
    EXPECT_EQ(recovered.generation, 2u);
    EXPECT_EQ(recovered.snapshot->generation, 2u);
    EXPECT_EQ(recovered.health.recoveryAttempts, 1u);
    EXPECT_EQ(recovered.health.lastGapMilliseconds, 2000);
}

TEST_F(MonitoringTest, RepeatedBaselineOnlyPassesCannotKeepOneRecoveryAttemptAlive) {
    Monitor monitor({}, directory / "diagnostics.log", inputs());
    MonitoringOwner owner([&] { return monitor.read(); }, channel);
    MonitoringClient client(channel);
    monitor.poll();
    hasWriteFailure = true;
    now += 1s;
    monitor.poll();
    hasWriteFailure = false;
    pass.system.sample.reset();
    pass.system.report.isBaseline = true;
    now += 1s;
    monitor.poll();
    EXPECT_FALSE(client.read().health.nextRecoveryUtcMs);
    now += 1s;
    monitor.poll();
    EXPECT_EQ(client.read().health.nextRecoveryUtcMs, 5000);
    EXPECT_EQ(client.read().health.recoveryAttempts, 1u);
}

TEST_F(MonitoringTest, RealSqliteRecoveryReplaysOnceAndDoesNotInventAnEpisodeEnding) {
    struct SqliteEngine final : detail::MonitoringEngine {
        sentinel::storage::SQLiteStore store;
        const detail::SamplingPass& pass;
        bool& loseAcknowledgement;
        SqliteEngine(const std::filesystem::path& path, const detail::SamplingPass& input, bool& lose)
            : store(path), pass(input), loseAcknowledgement(lose) {}
        detail::SamplingPass collect() override { return pass; }
        sentinel::storage::WriteResult save(const detail::PendingTick& tick) override {
            auto result = store.saveTick(tick.identity, tick.utc, tick.system, tick.processes, tick.anomalies);
            if (result.outcome == sentinel::storage::CommitOutcome::Committed && loseAcknowledgement) {
                loseAcknowledgement = false;
                return {sentinel::storage::CommitOutcome::Unresolved, "commit acknowledgement lost"};
            }
            return result;
        }
    };
    const auto database = directory / "sentinel.db";
    bool loseAcknowledgement = false;
    auto runtime = inputs();
    runtime.createEngine = [&] { return std::make_unique<SqliteEngine>(database, pass, loseAcknowledgement); };
    Monitor monitor({}, directory / "diagnostics.log", std::move(runtime));
    MonitoringOwner owner([&] { return monitor.read(); }, channel);
    MonitoringClient client(channel);
    pass.processes.sample.emplace();
    pass.processes.sample->processes.push_back({.identity = {42, 100}, .imageName = L"Cursor.exe",
        .cpuUsagePercent = 10, .workingSetBytes = 1000});
    const auto collectAt = [&](int second, double cpu) {
        now = std::chrono::steady_clock::time_point{std::chrono::seconds(second)};
        pass.system.sample->timestamp = now;
        pass.system.sample->utcTimestamp = std::chrono::system_clock::time_point{std::chrono::seconds(second)};
        pass.system.sample->cpuUsagePercent = cpu;
        pass.processes.sample->time = {now, pass.system.sample->utcTimestamp};
        monitor.poll();
    };
    collectAt(0, 97);
    collectAt(1, 97);
    loseAcknowledgement = true;
    collectAt(2, 97); // The high-CPU start commits, but its acknowledgement is lost.
    ASSERT_EQ(client.read().health.state, MonitoringState::Recovering);
    EXPECT_EQ(client.read().snapshot->tickUtcMs, 1000);
    collectAt(3, 10);
    ASSERT_EQ(client.read().health.state, MonitoringState::Operating);
    EXPECT_EQ(client.read().snapshot->generation, 2u);
    collectAt(4, 10);
    collectAt(5, 10); // A new generation must not invent the old episode's end.
    collectAt(6, 97);
    collectAt(7, 97);
    collectAt(8, 97);
    sentinel::storage::SQLiteStore reader(database);
    const auto ticks = reader.readTicks(std::chrono::system_clock::time_point{},
        std::chrono::system_clock::time_point{9s});
    ASSERT_EQ(ticks.size(), 9u);
    unsigned starts = 0;
    unsigned stops = 0;
    for (const auto& tick : ticks) {
        ASSERT_TRUE(tick.processContext);
        EXPECT_EQ(tick.processContext->groups.size(), 1u);
        for (const auto& event : tick.anomalies) {
            if (event.type != sentinel::detection::EventType::HighCpu) continue;
            if (event.state == sentinel::detection::EventState::Started) ++starts;
            if (event.state == sentinel::detection::EventState::Stopped) ++stops;
        }
    }
    EXPECT_EQ(starts, 2u);
    EXPECT_EQ(stops, 0u);
    const auto status = client.read();
    EXPECT_TRUE(std::any_of(status.diagnostics.begin(), status.diagnostics.end(),
        [](const Diagnostic& item) {
            return item.operation == "recreate" && item.message.find("continuity interrupted") != std::string::npos;
        }));
}

TEST_F(MonitoringTest, MaintenanceFailuresPublishButUnresolvedCommitsDoNot) {
    hasMaintenanceFailure = true;
    Monitor monitor({}, directory / "diagnostics.log", inputs());
    MonitoringOwner owner([&] { return monitor.read(); }, channel);
    MonitoringClient client(channel);
    monitor.poll();
    auto committed = client.read();
    ASSERT_TRUE(committed.snapshot);
    EXPECT_EQ(committed.health.state, MonitoringState::Operating);
    EXPECT_EQ(committed.health.lastFailure->operation, "maintenance");
    hasUnresolvedCommit = true;
    now += 1s;
    monitor.poll();
    EXPECT_EQ(client.read().snapshot->sequence, committed.snapshot->sequence);
    EXPECT_EQ(client.read().health.state, MonitoringState::Recovering);
    hasUnresolvedCommit = false;
    hasMaintenanceFailure = false;
    now += 1s;
    monitor.poll();
    EXPECT_EQ(client.read().health.state, MonitoringState::Operating);
    EXPECT_EQ(saved.at(1).identity, committed.ownerId + ":2");
}

TEST_F(MonitoringTest, DiagnosticsStayBoundedAndRotate) {
    pass.system.sample.reset();
    pass.system.report.operation = sentinel::telemetry::CollectionOperation::GetSystemTimes;
    pass.system.report.errorCode = 5;
    Monitor monitor({}, directory / "diagnostics.log", inputs());
    MonitoringOwner owner([&] { return monitor.read(); }, channel);
    MonitoringClient client(channel);
    for (int second = 0; second < 1800; ++second) {
        now += 1s;
        monitor.poll();
    }
    const auto status = client.read();
    EXPECT_EQ(status.diagnostics.size(), 64u);
    EXPECT_EQ(status.diagnostics.back().code, 5);
    EXPECT_TRUE(std::filesystem::exists(directory / "diagnostics.log.1"));
    EXPECT_TRUE(std::filesystem::exists(directory / "diagnostics.log.2"));
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        EXPECT_LE(entry.file_size(), 64 * 1024u);
    }
}

TEST_F(MonitoringTest, PersistenceFailureEvidenceSurvivesEngineRecreationInHistoryAndLog) {
    const auto log = directory / "diagnostics.log";
    Monitor monitor({}, log, inputs());
    MonitoringOwner owner([&] { return monitor.read(); }, channel);
    MonitoringClient client(channel);
    monitor.poll();
    hasWriteFailure = true;
    now += 1s;
    monitor.poll();
    const auto failed = client.read();
    ASSERT_TRUE(failed.health.lastFailure);
    EXPECT_EQ(failed.health.lastFailure->message, "Uncommitted tick: disk unavailable");
    {
        std::ifstream input(log);
        std::string line;
        bool hasFailure = false;
        while (std::getline(input, line)) {
            const auto record = nlohmann::json::parse(line);
            if (record.at("operation") == "save/open") {
                hasFailure = true;
                EXPECT_EQ(record.at("source"), "persistence");
                EXPECT_EQ(record.at("utcMs"), 1000);
                EXPECT_EQ(record.at("generation"), 1);
            }
        }
        EXPECT_TRUE(hasFailure);
    }
    hasWriteFailure = false;
    now += 1s;
    monitor.poll();
    const auto recovered = client.read();
    EXPECT_EQ(recovered.health.state, MonitoringState::Operating);
    EXPECT_EQ(recovered.generation, 2u);
    ASSERT_TRUE(recovered.health.lastFailure);
    EXPECT_EQ(recovered.health.lastFailure->utcMs, failed.health.lastFailure->utcMs);
    EXPECT_TRUE(std::any_of(recovered.diagnostics.begin(), recovered.diagnostics.end(),
        [](const Diagnostic& item) { return item.operation == "save/open" && item.generation == 1; }));
}

TEST_F(MonitoringTest, RoutineProcessExclusionsDoNotCrowdOutActionableDiagnostics) {
    pass.system.sample.reset();
    pass.system.report.operation = sentinel::telemetry::CollectionOperation::GetSystemTimes;
    pass.system.report.errorCode = ERROR_ACCESS_DENIED;
    pass.processes.sample.emplace();
    pass.processes.report.enumerated = 20;
    pass.processes.report.read = 18;
    pass.processes.report.skipped = 2;
    Monitor monitor({}, directory / "diagnostics.log", inputs());
    MonitoringOwner owner([&] { return monitor.read(); }, channel);
    MonitoringClient client(channel);
    monitor.poll();
    pass.system.report = {};
    for (int second = 1; second <= 120; ++second) {
        now += 1s;
        monitor.poll();
    }
    const auto status = client.read();
    EXPECT_EQ(status.health.processes.report.skipped, 2u);
    EXPECT_EQ(std::count_if(status.diagnostics.begin(), status.diagnostics.end(),
        [](const Diagnostic& item) { return item.operation == "coverage"; }), 3);
    EXPECT_TRUE(std::any_of(status.diagnostics.begin(), status.diagnostics.end(),
        [](const Diagnostic& item) { return item.source == "system" && item.code == ERROR_ACCESS_DENIED; }));
}

TEST_F(MonitoringTest, LoggingFailureIsVisibleWithoutHidingPersistenceFailure) {
    Monitor monitor({}, directory, inputs()); // A directory cannot be opened as a log file.
    MonitoringOwner owner([&] { return monitor.read(); }, channel);
    MonitoringClient client(channel);
    hasWriteFailure = true;
    monitor.poll();
    auto failed = client.read();
    EXPECT_TRUE(failed.health.hasLoggingFailure);
    EXPECT_EQ(failed.health.state, MonitoringState::Recovering);
    EXPECT_EQ(failed.health.lastFailure->source, "persistence");
    hasWriteFailure = false;
    now += 1s;
    monitor.poll();
    auto recovered = client.read();
    EXPECT_TRUE(recovered.health.hasLoggingFailure);
    EXPECT_TRUE(recovered.health.lastFailure);
    EXPECT_GT(recovered.diagnostics.size(), failed.diagnostics.size());
}

TEST_F(MonitoringTest, DatabaseOpenFailureExposesAvailableSqliteCode) {
    const auto database = directory / "blocked.db";
    std::filesystem::create_directory(database);
    Monitor monitor({}, database, directory / "diagnostics.log");
    MonitoringOwner owner([&] { return monitor.read(); }, channel);
    monitor.poll();
    const auto status = MonitoringClient(channel).read();
    EXPECT_EQ(status.health.state, MonitoringState::Recovering);
    ASSERT_TRUE(status.health.lastFailure);
    EXPECT_EQ(status.health.lastFailure->operation, "save/open");
    EXPECT_EQ(status.health.lastFailure->code, SQLITE_CANTOPEN);
    EXPECT_FALSE(status.snapshot);
}

TEST_F(MonitoringTest, MaintenanceFailureExposesAvailableCodeWithoutLosingSavedTelemetry) {
    struct MaintenanceEngine final : detail::MonitoringEngine {
        sentinel::storage::SQLiteStore store;
        explicit MaintenanceEngine(const std::filesystem::path& database)
            : store(database, 0, [](sentinel::storage::WriteStage stage) {
                if (stage == sentinel::storage::WriteStage::Maintenance) {
                    throw sentinel::storage::SQLiteError(SQLITE_IOERR, "injected maintenance I/O failure");
                }
            }) {}
        detail::SamplingPass collect() override {
            detail::SamplingPass pass;
            pass.system.sample = sentinel::telemetry::SystemSample{.cpuUsagePercent = 42};
            return pass;
        }
        sentinel::storage::WriteResult save(const detail::PendingTick& tick) override {
            return store.saveTick(tick.identity, tick.utc, tick.system, tick.processes, tick.anomalies);
        }
    };
    auto runtime = inputs();
    runtime.createEngine = [&] { return std::make_unique<MaintenanceEngine>(directory / "sentinel.db"); };
    Monitor monitor({}, directory / "diagnostics.log", std::move(runtime));
    MonitoringOwner owner([&] { return monitor.read(); }, channel);
    monitor.poll();
    const auto status = MonitoringClient(channel).read();
    EXPECT_EQ(status.health.state, MonitoringState::Operating);
    ASSERT_TRUE(status.snapshot);
    EXPECT_EQ(status.snapshot->system->cpuUsagePercent, 42);
    ASSERT_TRUE(status.health.lastFailure);
    EXPECT_EQ(status.health.lastFailure->operation, "maintenance");
    EXPECT_EQ(status.health.lastFailure->code, SQLITE_IOERR);
}

TEST_F(MonitoringTest, SimultaneousReadersAndDisconnectDoNotOwnMonitoring) {
    EXPECT_THROW(static_cast<void>(MonitoringClient(channel).read()), ConnectionError);
    Monitor monitor({}, directory / "diagnostics.log", inputs());
    {
        MonitoringOwner owner([&] { return monitor.read(); }, channel);
        monitor.poll();
        std::vector<std::future<MonitoringStatus>> readers;
        for (int index = 0; index < 16; ++index) readers.push_back(MonitoringClient(channel).readAsync());
        for (auto& reader : readers) EXPECT_EQ(reader.get().snapshot->sequence, 1u);
        now += 1s;
        monitor.poll();
        EXPECT_EQ(MonitoringClient(channel).read().snapshot->sequence, 2u);
    }
    EXPECT_THROW(static_cast<void>(MonitoringClient(channel).read()), ConnectionError);
    MonitoringOwner replacement([&] { return monitor.read(); }, channel);
    EXPECT_EQ(MonitoringClient(channel).read().snapshot->sequence, 2u);
}

TEST_F(MonitoringTest, CooperativeStopInterruptsRecoveryWithoutWaitingForBackoff) {
    hasWriteFailure = true;
    Monitor monitor({}, directory / "diagnostics.log", inputs());
    MonitoringOwner owner([&] { return monitor.read(); }, channel);
    MonitoringClient client(channel);
    std::jthread worker([&](std::stop_token stop) { monitor.run(stop); });
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (client.read().health.state == MonitoringState::Starting &&
            std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    EXPECT_EQ(client.read().health.state, MonitoringState::Recovering);
    const auto start = std::chrono::steady_clock::now();
    worker.request_stop();
    worker.join();
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
    EXPECT_EQ(client.read().health.state, MonitoringState::Stopped);
}

TEST_F(MonitoringTest, PipeHasProtectedDaclGrantingOnlyCurrentUser) {
    Monitor monitor({}, directory / "diagnostics.log", inputs());
    MonitoringOwner owner([&] { return monitor.read(); }, channel);
    HANDLE token{};
    ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    DWORD size{};
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> user(size);
    ASSERT_TRUE(GetTokenInformation(token, TokenUser, user.data(), size, &size));
    CloseHandle(token);
    const PSID userSid = reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
    LPWSTR sidText{};
    ASSERT_TRUE(ConvertSidToStringSidW(userSid, &sidText));
    const std::wstring name = L"\\\\.\\pipe\\Sentinel-" + std::wstring(sidText) + L"-" + channel;
    LocalFree(sidText);
    const HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | READ_CONTROL, 0, nullptr,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    ASSERT_NE(pipe, INVALID_HANDLE_VALUE);
    PSECURITY_DESCRIPTOR descriptor{};
    PACL dacl{};
    const DWORD result = GetSecurityInfo(pipe, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &dacl, nullptr, &descriptor);
    CloseHandle(pipe);
    ASSERT_EQ(result, ERROR_SUCCESS);
    ASSERT_NE(dacl, nullptr);
    EXPECT_EQ(dacl->AceCount, 1);
    void* rawAce{};
    ASSERT_TRUE(GetAce(dacl, 0, &rawAce));
    const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(rawAce);
    EXPECT_EQ(ace->Header.AceType, ACCESS_ALLOWED_ACE_TYPE);
    EXPECT_TRUE(EqualSid(const_cast<DWORD*>(&ace->SidStart), userSid));
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision{};
    ASSERT_TRUE(GetSecurityDescriptorControl(descriptor, &control, &revision));
    EXPECT_NE(control & SE_DACL_PROTECTED, 0);
    LocalFree(descriptor);
    // Abandon connections before reading; the endpoint must continue serving real clients.
    for (int index = 0; index < 100; ++index) {
        const HANDLE abandoned = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (abandoned != INVALID_HANDLE_VALUE) CloseHandle(abandoned);
        else WaitNamedPipeW(name.c_str(), 10);
    }
    EXPECT_EQ(MonitoringClient(channel).read().health.state, MonitoringState::Starting);
}

TEST_F(MonitoringTest, SeparateOwnerServesMultipleClientsAndReleasesOwnershipOnStop) {
    const std::wstring eventName = L"Local\\Sentinel-stop-" + channel;
    const HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
    ASSERT_NE(stop, nullptr);
    {
        ChildProcess child(quote(executableDirectory() / "sentinel_test_owner.exe") + L" " +
            quote(directory) + L" " + channel + L" " + eventName);
        MonitoringClient client(channel);
        std::optional<MonitoringStatus> status;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            try { status = client.read(); if (status->snapshot) break; }
            catch (const ConnectionError&) {}
            std::this_thread::sleep_for(10ms);
        }
        ASSERT_TRUE(status);
        ASSERT_TRUE(status->snapshot);
        std::vector<std::future<MonitoringStatus>> readers;
        for (int index = 0; index < 8; ++index) readers.push_back(client.readAsync());
        for (auto& reader : readers) EXPECT_EQ(reader.get().ownerId, status->ownerId);
        EXPECT_THROW(MonitoringOwner duplicate([] { return MonitoringStatus{}; }, channel), std::runtime_error);
        const auto oldSequence = status->snapshot->sequence;
        const auto progressDeadline = std::chrono::steady_clock::now() + 3s;
        while (client.read().snapshot->sequence == oldSequence &&
                std::chrono::steady_clock::now() < progressDeadline) std::this_thread::sleep_for(10ms);
        EXPECT_GT(client.read().snapshot->sequence, oldSequence);
        SetEvent(stop);
        EXPECT_EQ(child.wait(), 0u);
    }
    CloseHandle(stop);
    EXPECT_THROW(static_cast<void>(MonitoringClient(channel).read()), ConnectionError);
    MonitoringOwner replacement([] { return MonitoringStatus{}; }, channel);
    EXPECT_EQ(MonitoringClient(channel).read().health.state, MonitoringState::Starting);
}

TEST_F(MonitoringTest, RealStatusCommandsReadOwnerAndDuplicateStartIsRejected) {
    // Use the production endpoint without touching the user's telemetry database.
    std::unique_ptr<MonitoringOwner> owner;
    Monitor monitor({}, directory / "diagnostics.log", inputs());
    try { owner = std::make_unique<MonitoringOwner>([&] { return monitor.read(); }); }
    catch (const std::runtime_error&) { GTEST_SKIP() << "A monitoring owner already exists"; }
    monitor.poll();
    std::vector<std::unique_ptr<ChildProcess>> clients;
    for (int index = 0; index < 4; ++index) clients.push_back(std::make_unique<ChildProcess>(
        quote(executableDirectory() / "sentinel.exe") + L" status"));
    for (const auto& client : clients) EXPECT_EQ(client->wait(), 0u);
    ChildProcess duplicate(quote(executableDirectory() / "sentinel.exe") + L" start");
    EXPECT_EQ(duplicate.wait(), 1u);
    now += 1s;
    monitor.poll();
    EXPECT_EQ(MonitoringClient().read().snapshot->sequence, 2u);
}

TEST_F(MonitoringTest, RealStatusCommandsReadDuringRecoveryAndAfterBudgetExhaustion) {
    std::unique_ptr<MonitoringOwner> owner;
    Monitor monitor({}, directory / "diagnostics.log", inputs());
    try { owner = std::make_unique<MonitoringOwner>([&] { return monitor.read(); }); }
    catch (const std::runtime_error&) { GTEST_SKIP() << "A monitoring owner already exists"; }
    MonitoringClient client;
    monitor.poll();
    const auto original = client.read();
    const auto readWithStatusProcesses = [&] {
        std::vector<std::unique_ptr<ChildProcess>> clients;
        for (int index = 0; index < 4; ++index) clients.push_back(std::make_unique<ChildProcess>(
            quote(executableDirectory() / "sentinel.exe") + L" status"));
        std::vector<std::future<MonitoringStatus>> reads;
        for (int index = 0; index < 4; ++index) reads.push_back(client.readAsync());
        for (const auto& process : clients) EXPECT_EQ(process->wait(), 0u);
        for (auto& read : reads) EXPECT_EQ(read.get().ownerId, original.ownerId);
    };
    hasWriteFailure = true;
    now += 1s;
    monitor.poll();
    readWithStatusProcesses();
    EXPECT_EQ(client.read().health.state, MonitoringState::Recovering);
    EXPECT_EQ(client.read().snapshot->sequence, original.snapshot->sequence);
    hasWriteFailure = false;
    now += 1s;
    monitor.poll();
    EXPECT_EQ(client.read().health.state, MonitoringState::Operating);
    EXPECT_EQ(client.read().snapshot->generation, 2u);
    readWithStatusProcesses();
    const auto recovered = client.read();
    hasWriteFailure = true;
    now += 1s;
    monitor.poll();
    now += 2s;
    monitor.poll();
    now += 4s;
    monitor.poll();
    readWithStatusProcesses();
    EXPECT_EQ(client.read().health.state, MonitoringState::Failed);
    EXPECT_EQ(client.read().snapshot->sequence, recovered.snapshot->sequence);
}

TEST_F(MonitoringTest, ProductionCliEntryPointStopsOnCtrlCAndCanRestart) {
    try {
        static_cast<void>(MonitoringClient().read());
        GTEST_SKIP() << "A monitoring owner already exists";
    } catch (const ConnectionError&) {}
    for (int restart = 0; restart < 3; ++restart) {
        const bool duringRecovery = restart == 1;
        ChildProcess child(quote(executableDirectory() / "sentinel_test_cli.exe") + L" start", true,
            [duringRecovery](DWORD id) {
                if (duringRecovery) {
                    std::filesystem::create_directories(std::filesystem::temp_directory_path() /
                        (L"sentinel-cli-test-" + std::to_wstring(id)) / "sentinel.db");
                }
            });
        std::optional<MonitoringStatus> status;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            try {
                status = MonitoringClient().read();
                if (duringRecovery ? status->health.state == MonitoringState::Recovering :
                        status->snapshot.has_value()) break;
            }
            catch (const ConnectionError&) {}
            std::this_thread::sleep_for(10ms);
        }
        ASSERT_TRUE(status);
        if (duringRecovery) {
            ASSERT_EQ(status->health.state, MonitoringState::Recovering);
            EXPECT_FALSE(status->snapshot);
        } else {
            ASSERT_TRUE(status->snapshot);
        }
        ChildProcess reader(quote(executableDirectory() / "sentinel.exe") + L" status");
        EXPECT_EQ(reader.wait(), 0u);
        ChildProcess interrupt(quote(executableDirectory() / "sentinel_test_owner.exe") + L" --interrupt " +
            std::to_wstring(child.getId()));
        EXPECT_EQ(interrupt.wait(), 0u);
        EXPECT_EQ(child.wait(), 0u);
        EXPECT_THROW(static_cast<void>(MonitoringClient().read()), ConnectionError);
        std::filesystem::remove_all(std::filesystem::temp_directory_path() /
            (L"sentinel-cli-test-" + std::to_wstring(child.getId())));
    }
}

}  // namespace
