#pragma once

#include "sentinel/monitoring_config.h"
#include "sentinel/telemetry/collection_result.h"
#include "sentinel/telemetry/system_collector.h"
#include "sentinel/telemetry/process_aggregator.h"

#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace sentinel::monitoring {

enum class MonitoringState { Starting, Operating, Recovering, Failed, Stopped };
enum class Availability { Unavailable, Baseline, Available };

/** Independent health for the latest collection attempt, not the saved snapshot. */
struct SourceHealth {
    Availability availability{Availability::Unavailable};
    std::optional<std::int64_t> lastSuccessUtcMs;
    std::uint64_t consecutiveFailures{};
    telemetry::CollectionReport report;
};

struct Diagnostic {
    std::int64_t utcMs{};
    std::string source;
    std::string operation;
    std::string message;
    std::optional<std::int64_t> code;
    std::uint64_t generation{};
};

/** A single saved sampling pass. Missing sources never use older values. */
struct SavedSnapshot {
    std::string ownerId;
    std::uint64_t generation{};
    std::uint64_t sequence{};
    std::int64_t tickUtcMs{};
    std::optional<telemetry::SystemSample> system;
    std::optional<telemetry::ProcessGroupSnapshot> processes;
};

struct MonitoringHealth {
    MonitoringState state{MonitoringState::Starting};
    SourceHealth system;
    SourceHealth processes;
    std::uint32_t recoveryAttempts{};
    std::optional<std::int64_t> nextRecoveryUtcMs;
    std::optional<std::int64_t> gapStartedUtcMs;
    std::optional<std::int64_t> lastGapMilliseconds;
    std::optional<Diagnostic> lastFailure;
    bool hasLoggingFailure{false};
};

/** Copyable values suitable for a WebView2 or MCP bridge. */
struct MonitoringStatus {
    std::string ownerId;
    std::uint64_t generation{1};
    MonitoringHealth health;
    std::optional<SavedSnapshot> snapshot;
    std::vector<Diagnostic> diagnostics;
};

namespace detail { struct RuntimeInputs; }

/** Reusable sampling, publication, diagnostics, and bounded recovery lifecycle. */
class Monitor {
public:
    explicit Monitor(MonitoringConfig config, std::filesystem::path databasePath,
        std::filesystem::path diagnosticLog);
    Monitor(MonitoringConfig config, std::filesystem::path diagnosticLog,
        detail::RuntimeInputs inputs);
    ~Monitor();
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;
    /** Run until cooperative stop, including while recovering or failed. */
    void run(std::stop_token stop);
    /** Perform due work; useful when an embedding host owns its event loop. */
    void poll();
    /** Obtain an independent consistent copy without waiting for collection/storage. */
    [[nodiscard]] MonitoringStatus read() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/** Encode/decode the versioned value contract used by the shared client. */
[[nodiscard]] std::string serializeStatus(const MonitoringStatus& status);
[[nodiscard]] MonitoringStatus deserializeStatus(const std::string& text);

}  // namespace sentinel::monitoring
