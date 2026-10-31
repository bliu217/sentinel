#include "sentinel/runtime/monitor_runtime.h"

#include "sentinel/detection/anomaly_detector.h"
#include "sentinel/storage/sqlite_store.h"
#include "sentinel/telemetry/process_collector.h"

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>

namespace sentinel::runtime {
namespace {

// A file handle with no sharing also protects against a second monitoring process.
class RecordingLock {
public:
    explicit RecordingLock(const std::filesystem::path& databasePath) {
        if (!databasePath.parent_path().empty()) {
            std::filesystem::create_directories(databasePath.parent_path());
        }
        std::filesystem::path lockPath = databasePath;
        lockPath += L".recording.lock";
        handle_ = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Cannot acquire recording lock (another Sentinel may be recording); Windows error " +
                std::to_string(GetLastError()));
        }
    }
    ~RecordingLock() { CloseHandle(handle_); }
    RecordingLock(const RecordingLock&) = delete;
    RecordingLock& operator=(const RecordingLock&) = delete;
private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

}  // namespace

class MonitorRuntime::Impl {
public:
    Impl(MonitoringConfig config, std::filesystem::path path, RuntimeCollectors collectors)
        : config_(std::move(config)), path_(std::move(path)), collectors_(std::move(collectors)) {
        config_.validate();
    }

    void run(std::stop_token stop) noexcept {
        try {
            record(stop);
            std::lock_guard lock(mutex_);
            status_.state = RuntimeState::Stopped;
        } catch (const std::exception& error) {
            fail(std::current_exception(), error.what());
        } catch (...) {
            fail(std::current_exception(), "Unknown monitoring failure");
        }
    }

    void record(std::stop_token stop) {
        const RecordingLock recordingLock(path_);
        storage::SQLiteStore database(path_, config_.maxDatabaseSizeMiB * 1024 * 1024);
        storage::RetentionManager retention(database, config_.retentionPeriod);
        retention.pruneOnStartup(std::chrono::system_clock::now());
        {
            std::lock_guard lock(mutex_);
            if (!stop.stop_requested()) status_.state = RuntimeState::Running;
        }
        using Clock = std::chrono::steady_clock;
        Clock::time_point nextTick = Clock::now();
        while (!stop.stop_requested()) {
            const Clock::time_point started = Clock::now();
            collectTick(database);
            const Clock::time_point finished = Clock::now();
            nextTick += config_.sampleInterval;
            std::uint64_t skipped = 0;
            if (nextTick <= finished) {
                skipped = static_cast<std::uint64_t>((finished - nextTick) / config_.sampleInterval) + 1;
                nextTick += config_.sampleInterval * skipped;
            }
            {
                std::lock_guard lock(mutex_);
                status_.lastTickDuration = finished - started;
                status_.skippedDeadlines += skipped;
            }
            std::unique_lock waitLock(waitMutex_);
            wake_.wait_until(waitLock, stop, nextTick, [] { return false; });
        }
    }

    void collectTick(storage::SQLiteStore& database) {
        auto tick = std::make_shared<TickSnapshot>();
        tick->sequence = ++sequence_;
        tick->capturedAt = {std::chrono::steady_clock::now(), std::chrono::system_clock::now()};
        tick->system = collectors_.system ? collectors_.system() : systemCollector_.collect();
        const auto processes = collectors_.processes ? collectors_.processes() : processCollector_.collect();
        if (processes) {
            tick->processGroups = telemetry::selectProcessGroups(telemetry::aggregateProcesses(*processes),
                config_.topProcessGroupsPerMetric, config_.pinnedProcessGroups);
        }
        if (tick->system) {
            const telemetry::SampleTime observedAt{tick->system->timestamp, tick->system->utcTimestamp};
            for (auto& event : detector_.analyze(*tick->system)) {
                tick->events.push_back(attribution::attachProcessContext(std::move(event), observedAt,
                    tick->processGroups ? &*tick->processGroups : nullptr));
            }
        }
        if (tick->system || tick->processGroups) {
            database.writeTick(tick->capturedAt.utc, tick->system, tick->processGroups, tick->events);
            tick->persisted = true;
        }
        std::lock_guard lock(mutex_);
        history_.push_back(tick);
        if (history_.size() > config_.inMemoryHistorySize) history_.pop_front();
        ++status_.completedTicks;
        if (!tick->system) ++status_.unavailableSystemTicks;
        if (!tick->processGroups) ++status_.unavailableProcessTicks;
        if (tick->persisted) status_.lastPersistedAt = tick->capturedAt.utc;
    }

    void fail(std::exception_ptr failure, const char* message) {
        std::lock_guard lock(mutex_);
        failure_ = std::move(failure);
        status_.lastError = message;
        status_.state = RuntimeState::Failed;
    }

    MonitoringConfig config_;
    std::filesystem::path path_;
    RuntimeCollectors collectors_;
    telemetry::SystemCollector systemCollector_;
    telemetry::ProcessCollector processCollector_;
    detection::AnomalyDetector detector_;
    std::uint64_t sequence_{};
    mutable std::mutex mutex_;
    RuntimeStatus status_;
    std::exception_ptr failure_;
    std::deque<std::shared_ptr<const TickSnapshot>> history_;
    std::mutex lifecycleMutex_;
    std::mutex waitMutex_;
    std::condition_variable_any wake_;
    std::stop_source stopSource_;
    std::jthread worker_;
};

MonitorRuntime::MonitorRuntime(MonitoringConfig config, std::filesystem::path databasePath,
    RuntimeCollectors collectors)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(databasePath), std::move(collectors))) {}

MonitorRuntime::~MonitorRuntime() {
    requestStop();
    if (impl_->worker_.joinable()) impl_->worker_.join();
}

void MonitorRuntime::start() {
    std::lock_guard lifecycleLock(impl_->lifecycleMutex_);
    std::lock_guard lock(impl_->mutex_);
    if (impl_->status_.state != RuntimeState::Idle) {
        throw std::logic_error("A monitoring runtime can only be started once");
    }
    impl_->status_.state = RuntimeState::Starting;
    try {
        impl_->worker_ = std::jthread([this](std::stop_token stop) { impl_->run(stop); });
        impl_->stopSource_ = impl_->worker_.get_stop_source();
    } catch (...) {
        impl_->status_.state = RuntimeState::Idle;
        throw;
    }
}

void MonitorRuntime::requestStop() {
    std::stop_source source;
    {
        std::lock_guard lock(impl_->mutex_);
        source = impl_->stopSource_;
        if (impl_->status_.state == RuntimeState::Starting || impl_->status_.state == RuntimeState::Running) {
            impl_->status_.state = RuntimeState::Stopping;
        }
    }
    source.request_stop();
}

void MonitorRuntime::wait() {
    std::lock_guard lifecycleLock(impl_->lifecycleMutex_);
    if (impl_->worker_.joinable()) impl_->worker_.join();
    std::exception_ptr failure;
    {
        std::lock_guard lock(impl_->mutex_);
        failure = impl_->failure_;
    }
    if (failure) std::rethrow_exception(failure);
}

RuntimeStatus MonitorRuntime::status() const {
    std::lock_guard lock(impl_->mutex_);
    return impl_->status_;
}

std::vector<std::shared_ptr<const TickSnapshot>> MonitorRuntime::history() const {
    std::lock_guard lock(impl_->mutex_);
    return {impl_->history_.begin(), impl_->history_.end()};
}

}  // namespace sentinel::runtime
