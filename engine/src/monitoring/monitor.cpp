#include "sentinel/monitoring/monitor.h"

#include "runtime_inputs.h"
#include "sentinel/detection/anomaly_detector.h"
#include "sentinel/storage/process_history.h"
#include "sentinel/storage/ring_buffer.h"
#include "sentinel/telemetry/process_collector.h"

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <fstream>
#include <mutex>
#include <random>
#include <utility>

namespace sentinel::monitoring {
namespace {

using SteadyClock = std::chrono::steady_clock;
using UtcClock = std::chrono::system_clock;
constexpr std::size_t DIAGNOSTIC_LIMIT = 64;
constexpr std::uintmax_t LOG_BYTES = 64 * 1024;
constexpr std::size_t MESSAGE_LIMIT = 2048;
constexpr std::chrono::seconds STABLE_INTERVAL{60};
constexpr std::chrono::seconds COVERAGE_INTERVAL{60};
constexpr std::chrono::seconds RECOVERY_DELAYS[]{std::chrono::seconds(1),
    std::chrono::seconds(2), std::chrono::seconds(4)};

[[nodiscard]] std::int64_t toMilliseconds(UtcClock::time_point time) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

[[nodiscard]] std::string createOwnerId() {
    std::random_device random;
    std::string result;
    constexpr char DIGITS[] = "0123456789abcdef";
    for (int index = 0; index < 32; ++index) result += DIGITS[random() & 15];
    return result;
}

[[nodiscard]] std::string operationName(telemetry::CollectionOperation operation) {
    switch (operation) {
    case telemetry::CollectionOperation::GetSystemTimes: return "GetSystemTimes";
    case telemetry::CollectionOperation::GlobalMemoryStatusEx: return "GlobalMemoryStatusEx";
    case telemetry::CollectionOperation::EnumerateProcesses: return "EnumProcesses";
    default: return "collect";
    }
}

class WindowsEngine final : public detail::MonitoringEngine {
public:
    WindowsEngine(const MonitoringConfig& config, const std::filesystem::path& databasePath)
        : database_(databasePath, config.maxDatabaseSizeMiB * 1024 * 1024) {
        storage::RetentionManager(database_, config.retentionPeriod).pruneOnStartup(UtcClock::now());
    }
    [[nodiscard]] detail::SamplingPass collect() override {
        return {system_.collectDetailed(), processes_.collectDetailed()};
    }
    [[nodiscard]] storage::WriteResult save(const detail::PendingTick& tick) override {
        return database_.saveTick(tick.identity, tick.utc, tick.system, tick.processes, tick.anomalies);
    }
private:
    storage::SQLiteStore database_;
    telemetry::SystemCollector system_;
    telemetry::ProcessCollector processes_;
};

/** Keep three finite files; logging is deliberately independent of SQLite. */
void appendLog(const std::filesystem::path& path, const Diagnostic& diagnostic) {
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    const std::string line = nlohmann::json{{"utcMs", diagnostic.utcMs}, {"source", diagnostic.source},
        {"operation", diagnostic.operation}, {"generation", diagnostic.generation},
        {"code", diagnostic.code ? nlohmann::json(*diagnostic.code) : nlohmann::json(nullptr)},
        {"message", diagnostic.message}}.dump() + "\n";
    if (std::filesystem::exists(path) && std::filesystem::file_size(path) + line.size() > LOG_BYTES) {
        std::filesystem::path first = path;
        first += ".1";
        std::filesystem::path second = path;
        second += ".2";
        std::filesystem::remove(second);
        if (std::filesystem::exists(first)) std::filesystem::rename(first, second);
        std::filesystem::rename(path, first);
    }
    std::ofstream output(path, std::ios::app | std::ios::binary);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output << line;
    output.flush();
}

void retainDiagnostic(MonitoringStatus& status, Diagnostic diagnostic) {
    if (status.diagnostics.size() == DIAGNOSTIC_LIMIT) status.diagnostics.erase(status.diagnostics.begin());
    status.diagnostics.push_back(std::move(diagnostic));
}

}  // namespace

struct Monitor::Impl {
    MonitoringConfig config;
    std::filesystem::path logPath;
    detail::RuntimeInputs inputs;
    mutable std::mutex statusMutex;
    std::mutex pollMutex;
    MonitoringStatus status;
    std::unique_ptr<detail::MonitoringEngine> engine;
    detection::AnomalyDetector detector;
    storage::RingBuffer samples;
    storage::ProcessHistory processHistory;
    std::optional<detail::PendingTick> pending;
    std::optional<SteadyClock::time_point> successfulSince;
    std::optional<SteadyClock::time_point> gapStarted;
    std::optional<SteadyClock::time_point> lastCoverageReport;
    SteadyClock::time_point nextWork{};
    std::uint64_t tickNumber{};
    std::uint64_t sequence{};
    bool hasStarted{false};
    bool allowRecoveryBaseline{false};

    Impl(MonitoringConfig value, std::filesystem::path log, detail::RuntimeInputs runtime)
        : config(std::move(value)), logPath(std::move(log)), inputs(std::move(runtime)),
          samples(config.inMemoryHistorySize), processHistory(config.inMemoryHistorySize) {
        config.validate();
        if (!inputs.createEngine || !inputs.steadyNow || !inputs.utcNow) {
            throw std::invalid_argument("Monitoring requires engine and clock inputs");
        }
        status.ownerId = createOwnerId();
    }

    void report(std::string source, std::string operation, std::string message,
        std::optional<std::int64_t> code = std::nullopt, bool isFailure = false) {
        Diagnostic diagnostic{toMilliseconds(inputs.utcNow()), std::move(source), std::move(operation),
            message.substr(0, MESSAGE_LIMIT), code, status.generation};
        {
            const std::lock_guard lock(statusMutex);
            if (isFailure) status.health.lastFailure = diagnostic;
            retainDiagnostic(status, diagnostic);
        }
        try {
            appendLog(logPath, diagnostic);
        } catch (const std::exception& error) {
            const std::lock_guard lock(statusMutex);
            if (!status.health.hasLoggingFailure) {
                retainDiagnostic(status, {diagnostic.utcMs, "logging", "append/rotate",
                    std::string(error.what()).substr(0, MESSAGE_LIMIT), {}, status.generation});
            }
            status.health.hasLoggingFailure = true;
        }
    }

    void updateSource(SourceHealth& source, const telemetry::CollectionReport& report, bool hasSample) {
        source.report = report;
        if (report.operation != telemetry::CollectionOperation::None) {
            source.availability = Availability::Unavailable;
            ++source.consecutiveFailures;
            return;
        }
        source.consecutiveFailures = 0;
        source.availability = hasSample ? Availability::Available :
            (report.isBaseline ? Availability::Baseline : Availability::Unavailable);
        if (hasSample) source.lastSuccessUtcMs = toMilliseconds(report.attemptedAt);
    }

    void updateCollection(const detail::SamplingPass& pass) {
        {
            const std::lock_guard lock(statusMutex);
            updateSource(status.health.system, pass.system.report, pass.system.sample.has_value());
            updateSource(status.health.processes, pass.processes.report, pass.processes.sample.has_value());
        }
        if (pass.system.report.operation != telemetry::CollectionOperation::None) {
            report("system", operationName(pass.system.report.operation), "Collection failed",
                pass.system.report.errorCode, true);
        }
        if (pass.processes.report.operation != telemetry::CollectionOperation::None) {
            report("processes", operationName(pass.processes.report.operation), "Collection failed",
                pass.processes.report.errorCode, true);
        }
        const auto now = inputs.steadyNow();
        if (pass.processes.report.skipped != 0 &&
                (!lastCoverageReport || now - *lastCoverageReport >= COVERAGE_INTERVAL)) {
            lastCoverageReport = now;
            report("processes", "coverage", "Enumerated " + std::to_string(pass.processes.report.enumerated) +
                "; read " + std::to_string(pass.processes.report.read) + "; skipped " +
                std::to_string(pass.processes.report.skipped));
        }
    }

    void scheduleRecovery(const std::string& error, std::optional<int> code = {}) {
        engine.reset();
        successfulSince.reset();
        report("persistence", "save/open", error, code, true);
        const SteadyClock::time_point now = inputs.steadyNow();
        if (!gapStarted) gapStarted = now;
        {
            const std::lock_guard lock(statusMutex);
            if (!status.health.gapStartedUtcMs) status.health.gapStartedUtcMs = toMilliseconds(inputs.utcNow());
            if (status.health.recoveryAttempts == std::size(RECOVERY_DELAYS)) {
                status.health.state = MonitoringState::Failed;
                status.health.nextRecoveryUtcMs.reset();
            } else {
                status.health.state = MonitoringState::Recovering;
                const std::chrono::seconds delay = RECOVERY_DELAYS[status.health.recoveryAttempts];
                nextWork = now + delay;
                status.health.nextRecoveryUtcMs = toMilliseconds(inputs.utcNow() + delay);
            }
        }
        report("recovery", "schedule", status.health.state == MonitoringState::Failed ?
            "Retry budget exhausted; explicit owner restart required" : "Collection paused until recovery attempt");
    }

    void publishPending() {
        const std::lock_guard lock(statusMutex);
        status.snapshot = SavedSnapshot{status.ownerId, pending->generation, ++sequence,
            toMilliseconds(pending->utc), pending->system, pending->processes};
        pending.reset();
    }

    [[nodiscard]] bool savePending() {
        const storage::WriteResult result = engine->save(*pending);
        if (result.outcome != storage::CommitOutcome::Committed) {
            scheduleRecovery((result.outcome == storage::CommitOutcome::Unresolved ?
                "Unresolved commit: " : "Uncommitted tick: ") + result.error, result.code);
            return false;
        }
        publishPending();
        if (!result.error.empty()) report("persistence", "reconcile", result.error, result.code, true);
        if (!result.maintenanceError.empty()) {
            report("persistence", "maintenance", result.maintenanceError, result.maintenanceCode, true);
        }
        return true;
    }

    void finishSuccessfulPass() {
        const SteadyClock::time_point now = inputs.steadyNow();
        bool hasRecovered = false;
        {
            const std::lock_guard lock(statusMutex);
            hasRecovered = status.health.state == MonitoringState::Recovering;
            if (hasRecovered && status.health.gapStartedUtcMs) {
                status.health.lastGapMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now - *gapStarted).count();
                gapStarted.reset();
                status.health.gapStartedUtcMs.reset();
            }
            status.health.state = MonitoringState::Operating;
            status.health.nextRecoveryUtcMs.reset();
            if (!successfulSince) successfulSince = now;
            if (now - *successfulSince >= STABLE_INTERVAL) status.health.recoveryAttempts = 0;
        }
        if (hasRecovered) report("recovery", "complete", "New generation saved; gap ended; previous failure retained");
    }

    void collectPass() {
        const detail::SamplingPass pass = engine->collect();
        updateCollection(pass);
        const bool isRecoveryBaseline = std::exchange(allowRecoveryBaseline, false) &&
            (pass.system.report.isBaseline || pass.processes.report.isBaseline) &&
            pass.system.report.operation == telemetry::CollectionOperation::None &&
            pass.processes.report.operation == telemetry::CollectionOperation::None;
        detail::PendingTick tick;
        tick.identity = status.ownerId + ":" + std::to_string(++tickNumber);
        tick.generation = status.generation;
        tick.utc = inputs.utcNow();
        tick.system = pass.system.sample;
        if (tick.system) samples.push(*tick.system);
        if (pass.processes.sample) {
            tick.processes = telemetry::selectProcessGroups(telemetry::aggregateProcesses(*pass.processes.sample),
                config.topProcessGroupsPerMetric, config.pinnedProcessGroups);
            processHistory.push(*tick.processes);
        }
        if (tick.system) {
            const telemetry::SampleTime observedAt{tick.system->timestamp, tick.system->utcTimestamp};
            for (detection::DetectionEvent& event : detector.analyze(*tick.system)) {
                tick.anomalies.push_back(attribution::attachProcessContext(std::move(event), observedAt,
                    tick.processes ? &*tick.processes : nullptr));
            }
        }
        if (!tick.system && !tick.processes) {
            successfulSince.reset();
            if (status.health.state == MonitoringState::Recovering && !isRecoveryBaseline) {
                scheduleRecovery("Recovery produced no savable sampling pass");
            }
            return;
        }
        pending = std::move(tick);
        if (savePending()) finishSuccessfulPass();
    }

    void poll() {
        const std::lock_guard workLock(pollMutex);
        const SteadyClock::time_point now = inputs.steadyNow();
        if (status.health.state == MonitoringState::Failed || status.health.state == MonitoringState::Stopped) return;
        if (hasStarted && now < nextWork) return;
        nextWork = now + config.sampleInterval;
        try {
            if (!engine) {
                if (hasStarted) {
                    {
                        const std::lock_guard lock(statusMutex);
                        ++status.health.recoveryAttempts;
                        ++status.generation;
                        status.health.nextRecoveryUtcMs.reset();
                        status.health.system = {};
                        status.health.processes = {};
                    }
                    detector = {};
                    report("recovery", "recreate", "New monitoring generation; CPU baselines reset; anomaly episode continuity interrupted");
                } else {
                    hasStarted = true;
                    report("owner", "start", "Monitoring started");
                }
                engine = inputs.createEngine();
                allowRecoveryBaseline = status.health.state == MonitoringState::Recovering;
                // Reconcile the original pending identity before collecting in the new generation.
                if (pending && !savePending()) return;
            }
            collectPass();
        } catch (const storage::SQLiteError& error) {
            scheduleRecovery(error.what(), error.code());
        } catch (const std::exception& error) {
            scheduleRecovery(error.what());
        }
    }
};

Monitor::Monitor(MonitoringConfig config, std::filesystem::path databasePath,
    std::filesystem::path diagnosticLog)
    : Monitor(config, std::move(diagnosticLog), detail::RuntimeInputs{
        [config, databasePath = std::move(databasePath)] {
            return std::make_unique<WindowsEngine>(config, databasePath);
        }}) {}

Monitor::Monitor(MonitoringConfig config, std::filesystem::path diagnosticLog, detail::RuntimeInputs inputs)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(diagnosticLog), std::move(inputs))) {}

Monitor::~Monitor() = default;

void Monitor::poll() { impl_->poll(); }

MonitoringStatus Monitor::read() const {
    const std::lock_guard lock(impl_->statusMutex);
    return impl_->status;
}

void Monitor::run(std::stop_token stop) {
    std::mutex waitMutex;
    std::condition_variable_any wake;
    while (!stop.stop_requested()) {
        poll();
        std::unique_lock lock(waitMutex);
        wake.wait_for(lock, stop, std::chrono::milliseconds(20), [] { return false; });
    }
    impl_->report("owner", "stop", "Monitoring stopped cooperatively");
    const std::lock_guard lock(impl_->statusMutex);
    impl_->status.health.state = MonitoringState::Stopped;
}

}  // namespace sentinel::monitoring
