#pragma once

#include "attribution/anomaly_event.h"
#include "telemetry/process_sample.h"
#include "telemetry/system_collector.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace sentinel::storage {

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
    explicit SQLiteStore(const std::filesystem::path& path, std::uint64_t maxBytes = 2ULL * 1024 * 1024 * 1024);
    ~SQLiteStore();
    SQLiteStore(const SQLiteStore&) = delete;
    SQLiteStore& operator=(const SQLiteStore&) = delete;

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
        std::chrono::system_clock::time_point tickUtc,
        const std::optional<telemetry::SystemSample>& system,
        const std::optional<telemetry::ProcessGroupSnapshot>& processContext,
        const std::vector<attribution::AnomalyEvent>& anomalies);

    sqlite3* db_{};
    std::uint64_t maxBytes_{};
    std::string storeId_;
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
