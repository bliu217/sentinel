#pragma once

#include "sentinel/storage/sqlite_store.h"
#include "sentinel/telemetry/process_sample.h"

#include <functional>
#include <memory>

namespace sentinel::monitoring::detail {

struct SamplingPass {
    telemetry::CollectionResult<telemetry::SystemSample> system;
    telemetry::CollectionResult<telemetry::ProcessSnapshot> processes;
};

struct PendingTick {
    std::string identity;
    std::uint64_t generation{};
    std::chrono::system_clock::time_point utc;
    std::optional<telemetry::SystemSample> system;
    std::optional<telemetry::ProcessGroupSnapshot> processes;
    std::vector<attribution::AnomalyEvent> anomalies;
};

/** One boundary for external collection and persistence inputs. Not a wire API. */
class MonitoringEngine {
public:
    virtual ~MonitoringEngine() = default;
    [[nodiscard]] virtual SamplingPass collect() = 0;
    [[nodiscard]] virtual storage::WriteResult save(const PendingTick& tick) = 0;
};

struct RuntimeInputs {
    std::function<std::unique_ptr<MonitoringEngine>()> createEngine;
    std::function<std::chrono::steady_clock::time_point()> steadyNow{
        [] { return std::chrono::steady_clock::now(); }};
    std::function<std::chrono::system_clock::time_point()> utcNow{
        [] { return std::chrono::system_clock::now(); }};
};

}  // namespace sentinel::monitoring::detail
