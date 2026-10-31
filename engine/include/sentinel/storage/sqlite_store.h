#pragma once

#include "sentinel/attribution/anomaly_event.h"
#include "sentinel/telemetry/process_sample.h"
#include "sentinel/telemetry/system_collector.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

struct sqlite3;

namespace sentinel::storage {

/** Preserve the SQLite result code when opening or maintaining a store throws. */
class SQLiteError : public std::runtime_error {
public:
    SQLiteError(int code, const std::string& message) : std::runtime_error(message), code_(code) {}
    [[nodiscard]] int code() const noexcept { return code_; }
private:
    int code_;
};

enum class CommitOutcome { Committed, Uncommitted, Unresolved };
enum class WriteStage { BeforeCommit, AfterCommit, Maintenance };

/** A maintenance error never changes a known committed outcome. */
struct WriteResult {
    CommitOutcome outcome{CommitOutcome::Uncommitted};
    std::string error;
    std::optional<int> code;
    std::string maintenanceError;
    std::optional<int> maintenanceCode;
};

struct StoredAnomaly {
    std::int64_t id{};
    detection::EventType type{};
    detection::EventState state{};
    double value{};
    std::int64_t occurredUtcMilliseconds{};
    std::int64_t observedUtcMilliseconds{};
    std::optional<attribution::ProcessContext> processContext;
};

struct StoredTick {
    std::int64_t id{};
    std::int64_t utcMilliseconds{};
    std::optional<telemetry::SystemSample> system;
    std::optional<telemetry::ProcessGroupSnapshot> processContext;
    std::vector<StoredAnomaly> anomalies;
};

class SQLiteStore {
public:
    explicit SQLiteStore(const std::filesystem::path& path, std::uint64_t maxBytes = 2ULL * 1024 * 1024 * 1024,
        std::function<void(WriteStage)> writeCheckpoint = {});
    ~SQLiteStore();
    SQLiteStore(const SQLiteStore&) = delete;
    SQLiteStore& operator=(const SQLiteStore&) = delete;

    /** Save or reconcile one stable identity, including across connection reopen. */
    [[nodiscard]] WriteResult saveTick(const std::string& identity,
        std::chrono::system_clock::time_point tickUtc,
        const std::optional<telemetry::SystemSample>& system,
        const std::optional<telemetry::ProcessGroupSnapshot>& processContext,
        const std::vector<attribution::AnomalyEvent>& anomalies);

    void writeTick(
        std::chrono::system_clock::time_point tickUtc,
        const std::optional<telemetry::SystemSample>& system,
        const std::optional<telemetry::ProcessGroupSnapshot>& processContext,
        const std::vector<attribution::AnomalyEvent>& anomalies);
    void pruneBefore(std::chrono::system_clock::time_point cutoff);
    void enforceCap();
    [[nodiscard]] std::vector<StoredTick> readTicks(
        std::chrono::system_clock::time_point from,
        std::chrono::system_clock::time_point to,
        std::int64_t afterUtcMilliseconds = INT64_MIN,
        std::int64_t afterId = 0,
        std::int64_t limit = 256) const;
    [[nodiscard]] std::int64_t countTicks() const;
    [[nodiscard]] const std::string& storeId() const noexcept { return storeId_; }

private:
    [[nodiscard]] bool deleteOldest(std::int64_t count);
    [[nodiscard]] std::uint64_t allocatedBytes() const;
    void writeTickOnce(
        const std::string& identity, std::chrono::system_clock::time_point tickUtc,
        const std::optional<telemetry::SystemSample>& system,
        const std::optional<telemetry::ProcessGroupSnapshot>& processContext,
        const std::vector<attribution::AnomalyEvent>& anomalies);

    sqlite3* db_{};
    std::uint64_t maxBytes_{};
    std::string storeId_;
    std::function<void(WriteStage)> writeCheckpoint_;
};

class RetentionManager {
public:
    RetentionManager(SQLiteStore& store, std::chrono::days retentionPeriod)
        : store_(store), retentionPeriod_(retentionPeriod) {}
    void pruneOnStartup(std::chrono::system_clock::time_point now);

private:
    SQLiteStore& store_;
    std::chrono::days retentionPeriod_;
};

}  // namespace sentinel::storage
