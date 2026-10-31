#pragma once

#include "sentinel/attribution/anomaly_event.h"
#include "sentinel/monitoring_config.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sentinel::runtime {

enum class RuntimeState { Idle, Starting, Running, Stopping, Stopped, Failed };

/** One sampling pass. Published snapshots are immutable and retained as a bounded history. */
struct TickSnapshot {
    std::uint64_t sequence{};
    telemetry::SampleTime capturedAt{};
    std::optional<telemetry::SystemSample> system;
    std::optional<telemetry::ProcessGroupSnapshot> processGroups;
    std::vector<attribution::AnomalyEvent> events;
    bool persisted{};
};

/** Runtime health; unavailable readings include baseline setup as well as collection failures. */
struct RuntimeStatus {
    RuntimeState state{RuntimeState::Idle};
    std::uint64_t completedTicks{};
    std::uint64_t unavailableSystemTicks{};
    std::uint64_t unavailableProcessTicks{};
    std::uint64_t skippedDeadlines{};
    std::chrono::steady_clock::duration lastTickDuration{};
    std::optional<std::chrono::system_clock::time_point> lastPersistedAt;
    std::string lastError;
};

/** Optional collection adapters for embedding and deterministic runtime tests. */
struct RuntimeCollectors {
    std::function<std::optional<telemetry::SystemSample>()> system;
    std::function<std::optional<telemetry::ProcessSnapshot>()> processes;
};

/** Owns one monitoring session. Collection and database writes stay on its worker thread.
 * start() is one-shot; wait() joins and rethrows worker failures. requestStop(), status(),
 * and history() are safe during monitoring. Destruction requests shutdown and joins.
 */
class MonitorRuntime {
public:
    MonitorRuntime(MonitoringConfig config, std::filesystem::path databasePath,
        RuntimeCollectors collectors = {});
    ~MonitorRuntime();
    MonitorRuntime(const MonitorRuntime&) = delete;
    MonitorRuntime& operator=(const MonitorRuntime&) = delete;

    void start();
    void requestStop();
    void wait();
    [[nodiscard]] RuntimeStatus status() const;
    [[nodiscard]] std::vector<std::shared_ptr<const TickSnapshot>> history() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sentinel::runtime
