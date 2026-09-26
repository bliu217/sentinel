#include "attribution/anomaly_event.h"
#include "detection/anomaly_detector.h"
#include "monitoring_config.h"
#include "paths.h"
#include "storage/event_store.h"
#include "storage/archive_manager.h"
#include "storage/process_history.h"
#include "storage/ring_buffer.h"
#include "storage/sqlite_store.h"
#include "telemetry/process_aggregator.h"
#include "telemetry/process_collector.h"
#include "telemetry/system_collector.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <windows.h>

namespace {

std::atomic<bool> g_running{true};

BOOL WINAPI onConsoleCtrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_running.store(false, std::memory_order_relaxed);
        return TRUE;
    }
    return FALSE;
}

void printUsage() {
    std::cerr << "Usage:\n"
              << "  sentinel start [--max-db-size-mib N]\n"
              << "  sentinel start --benchmark --interval-ms N --benchmark-output FILE\n"
              << "      [--benchmark-duration-sec N] [--data-dir DIR] [--max-db-size-mib N]\n"
              << "  sentinel archive create SLUG --name NAME\n"
              << "  sentinel archive add SLUG --from UTC --to UTC [--app EXE]... "
                 "[--events cpu,memory,collection-lag] [--include-samples]\n";
}

class BenchmarkTimingLog {
public:
    explicit BenchmarkTimingLog(const std::filesystem::path& path)
        : out_(path, std::ios::binary | std::ios::trunc) {
        if (!out_) throw std::runtime_error("Cannot open benchmark timing log");
        buffer_.reserve(8192);
        buffer_.append("sample_index,target_interval_ms,delay_ms\n");
    }

    BenchmarkTimingLog(const BenchmarkTimingLog&) = delete;
    BenchmarkTimingLog& operator=(const BenchmarkTimingLog&) = delete;

    ~BenchmarkTimingLog() {
        try {
            flush();
        } catch (...) {
        }
    }

    void record(std::uint64_t index, std::int64_t intervalMs, double delayMs) {
        char line[96];
        const int written = std::snprintf(
            line, sizeof(line), "%llu,%lld,%.3f\n",
            static_cast<unsigned long long>(index),
            static_cast<long long>(intervalMs), delayMs);
        if (written <= 0) throw std::runtime_error("Failed to format benchmark timing row");
        buffer_.append(line, static_cast<std::size_t>(written));
        if (++pending_ >= 64 || buffer_.size() >= 32768) flush();
    }

private:
    void flush() {
        if (!out_ || buffer_.empty()) return;
        out_ << buffer_;
        out_.flush();
        if (!out_) throw std::runtime_error("Failed to write benchmark timing log");
        buffer_.clear();
        pending_ = 0;
    }

    std::ofstream out_;
    std::string buffer_;
    std::size_t pending_{0};
};

[[nodiscard]] std::string requireValue(int& index, int argc, char* argv[]);

[[nodiscard]] std::uint64_t parseWholeNumber(const std::string& value, const char* message) {
    if (value.empty() ||
        !std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        throw std::invalid_argument(message);
    }
    try {
        const unsigned long long parsed = std::stoull(value);
        if (parsed == 0 ||
            parsed > static_cast<unsigned long long>(std::numeric_limits<std::int64_t>::max())) {
            throw std::invalid_argument(message);
        }
        return static_cast<std::uint64_t>(parsed);
    } catch (const std::invalid_argument&) {
        throw std::invalid_argument(message);
    } catch (const std::out_of_range&) {
        throw std::invalid_argument(message);
    }
}

struct StartOptions {
    sentinel::MonitoringConfig config{};
    bool benchmark{false};
    bool hasBenchmarkOutput{false};
    bool hasDataDirectory{false};
    bool hasDuration{false};
    bool hasInterval{false};
    std::filesystem::path benchmarkOutput;
    std::filesystem::path dataDirectory;
    std::chrono::seconds benchmarkDuration{0};
};

[[nodiscard]] StartOptions parseStartOptions(int argc, char* argv[]) {
    StartOptions options;
    for (int index = 2; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--benchmark") {
            if (options.benchmark) throw std::invalid_argument("Duplicate --benchmark");
            options.benchmark = true;
        } else if (option == "--interval-ms") {
            if (options.hasInterval) throw std::invalid_argument("Duplicate --interval-ms");
            const auto intervalMs = parseWholeNumber(requireValue(index, argc, argv), "Invalid sampling interval");
            options.config.sampleInterval = std::chrono::milliseconds(intervalMs);
            options.hasInterval = true;
        } else if (option == "--benchmark-output") {
            if (options.hasBenchmarkOutput) throw std::invalid_argument("Duplicate --benchmark-output");
            options.benchmarkOutput = requireValue(index, argc, argv);
            if (options.benchmarkOutput.empty()) throw std::invalid_argument("Missing benchmark output path");
            options.hasBenchmarkOutput = true;
        } else if (option == "--benchmark-duration-sec") {
            if (options.hasDuration) throw std::invalid_argument("Duplicate --benchmark-duration-sec");
            const auto seconds = parseWholeNumber(requireValue(index, argc, argv), "Invalid benchmark duration");
            if (seconds > 24 * 60 * 60) throw std::invalid_argument("Invalid benchmark duration");
            options.benchmarkDuration = std::chrono::seconds(seconds);
            options.hasDuration = true;
        } else if (option == "--data-dir") {
            if (options.hasDataDirectory) throw std::invalid_argument("Duplicate --data-dir");
            options.dataDirectory = requireValue(index, argc, argv);
            if (options.dataDirectory.empty()) throw std::invalid_argument("Missing benchmark data directory");
            options.hasDataDirectory = true;
        } else if (option == "--max-db-size-mib") {
            const std::string value = requireValue(index, argc, argv);
            std::size_t consumed = 0;
            options.config.maxDatabaseSizeMiB = std::stoull(value, &consumed);
            if (consumed != value.size()) throw std::invalid_argument("Invalid database size cap");
        } else {
            throw std::invalid_argument("Unknown start option: " + std::string(option));
        }
    }
    if (options.benchmark) {
        if (!options.hasInterval) throw std::invalid_argument("--benchmark requires --interval-ms");
        if (!options.hasBenchmarkOutput) throw std::invalid_argument("--benchmark requires --benchmark-output");
    } else if (options.hasInterval || options.hasBenchmarkOutput || options.hasDuration || options.hasDataDirectory) {
        throw std::invalid_argument("Benchmark options require --benchmark");
    }
    options.config.validate();
    return options;
}

[[nodiscard]] std::chrono::system_clock::time_point parseUtc(const std::string& text) {
    if ((text.size() != 20 && text.size() != 24) || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':' || text.back() != 'Z' ||
        (text.size() == 24 && text[19] != '.')) {
        throw std::invalid_argument("UTC time must be YYYY-MM-DDTHH:MM:SS[.mmm]Z");
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index == 4 || index == 7 || index == 10 || index == 13 || index == 16 ||
            index == 19 || index == text.size() - 1) continue;
        if (text[index] < '0' || text[index] > '9') throw std::invalid_argument("Invalid UTC time");
    }
    const int year = std::stoi(text.substr(0, 4));
    const unsigned month = static_cast<unsigned>(std::stoi(text.substr(5, 2)));
    const unsigned day = static_cast<unsigned>(std::stoi(text.substr(8, 2)));
    const int hour = std::stoi(text.substr(11, 2));
    const int minute = std::stoi(text.substr(14, 2));
    const int second = std::stoi(text.substr(17, 2));
    const int millisecond = text.size() == 24 ? std::stoi(text.substr(20, 3)) : 0;
    const std::chrono::year_month_day date{std::chrono::year(year), std::chrono::month(month),
        std::chrono::day(day)};
    if (!date.ok() || hour > 23 || minute > 59 || second > 59) {
        throw std::invalid_argument("Invalid UTC time");
    }
    return std::chrono::sys_days(date) + std::chrono::hours(hour) +
        std::chrono::minutes(minute) + std::chrono::seconds(second) +
        std::chrono::milliseconds(millisecond);
}

[[nodiscard]] std::string requireValue(int& index, int argc, char* argv[]) {
    if (++index >= argc) throw std::invalid_argument("Missing command option value");
    return argv[index];
}

[[nodiscard]] std::wstring wideArgument(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_ACP, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length == 0) throw std::invalid_argument("Invalid command argument encoding");
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_ACP, 0, value.data(), static_cast<int>(value.size()),
            result.data(), length) == 0) {
        throw std::invalid_argument("Invalid command argument encoding");
    }
    return result;
}

[[nodiscard]] std::string utf8Argument(const std::string& value) {
    const std::wstring wide = wideArgument(value);
    if (wide.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (length == 0) throw std::invalid_argument("Invalid command argument encoding");
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
            static_cast<int>(wide.size()), result.data(), length, nullptr, nullptr) == 0) {
        throw std::invalid_argument("Invalid command argument encoding");
    }
    return result;
}

[[nodiscard]] int archiveCommand(int argc, char* argv[]) {
    if (argc < 5) { printUsage(); return 1; }
    const std::string_view action = argv[2];
    const std::string slug = argv[3];
    const std::filesystem::path dbPath = sentinel::paths::databasePath();
    if (action == "add" && !std::filesystem::exists(dbPath)) {
        throw std::runtime_error("No Sentinel database exists yet");
    }
    std::filesystem::create_directories(sentinel::paths::localDataDirectory());
    std::filesystem::create_directories(sentinel::paths::archiveDirectory());
    sentinel::storage::SQLiteStore store(dbPath, 0);
    sentinel::storage::ArchiveManager archives(store, sentinel::paths::archiveDirectory());
    if (action == "create") {
        if (argc != 6 || std::string_view(argv[4]) != "--name") {
            printUsage(); return 1;
        }
        archives.createProject(slug, utf8Argument(argv[5]));
        return 0;
    }
    if (action != "add") { printUsage(); return 1; }
    sentinel::storage::ArchiveSelection selection;
    bool hasFrom = false;
    bool hasTo = false;
    for (int index = 4; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--from") {
            if (hasFrom) throw std::invalid_argument("Duplicate --from");
            selection.from = parseUtc(requireValue(index, argc, argv));
            hasFrom = true;
        } else if (option == "--to") {
            if (hasTo) throw std::invalid_argument("Duplicate --to");
            selection.to = parseUtc(requireValue(index, argc, argv));
            hasTo = true;
        } else if (option == "--app") {
            const std::string value = requireValue(index, argc, argv);
            selection.applications.push_back(wideArgument(value));
        } else if (option == "--events") {
            const std::string value = requireValue(index, argc, argv);
            std::size_t position = 0;
            while (position <= value.size()) {
                const std::size_t end = value.find(',', position);
                const std::string token = value.substr(position, end == std::string::npos ? end : end - position);
                if (token == "cpu") selection.eventTypes.push_back(sentinel::detection::EventType::HighCpu);
                else if (token == "memory") selection.eventTypes.push_back(sentinel::detection::EventType::HighMemory);
                else if (token == "collection-lag") selection.eventTypes.push_back(sentinel::detection::EventType::SamplingDelay);
                else throw std::invalid_argument("Unknown event filter: " + token);
                if (end == std::string::npos) break;
                position = end + 1;
            }
        } else if (option == "--include-samples") {
            selection.includeSamples = true;
        } else {
            throw std::invalid_argument("Unknown archive option: " + std::string(option));
        }
    }
    if (!hasFrom || !hasTo) throw std::invalid_argument("Archive add requires --from and --to");
    archives.addToProject(slug, selection);
    return 0;
}

[[nodiscard]] int startCommand(int argc, char* argv[]) {
    const StartOptions options = parseStartOptions(argc, argv);
    const sentinel::MonitoringConfig& config = options.config;
    if (SetConsoleCtrlHandler(onConsoleCtrl, TRUE) == 0) {
        throw std::runtime_error("Failed to register console control handler");
    }
    const std::filesystem::path dataDirectory = options.hasDataDirectory
        ? options.dataDirectory
        : sentinel::paths::localDataDirectory();
    const std::filesystem::path databasePath = options.hasDataDirectory
        ? dataDirectory / L"sentinel.db"
        : sentinel::paths::databasePath();
    const std::filesystem::path exportsDirectory = options.hasDataDirectory
        ? dataDirectory / L"exports"
        : sentinel::paths::exportsDirectory();
    std::filesystem::create_directories(dataDirectory);
    std::filesystem::create_directories(exportsDirectory);
    if (options.hasBenchmarkOutput && options.benchmarkOutput.has_parent_path()) {
        std::filesystem::create_directories(options.benchmarkOutput.parent_path());
    }
    sentinel::storage::SQLiteStore database(databasePath,
        config.maxDatabaseSizeMiB * 1024 * 1024);
    sentinel::storage::RetentionManager retention(database, config.retentionPeriod);
    retention.pruneOnStartup(std::chrono::system_clock::now());
    sentinel::telemetry::SystemCollector collector;
    sentinel::telemetry::ProcessCollector processCollector;
    sentinel::storage::RingBuffer samples(config.inMemoryHistorySize);
    sentinel::storage::ProcessHistory processHistory(config.inMemoryHistorySize);
    sentinel::detection::AnomalyDetector detector;
    sentinel::storage::EventStore eventStore(&database);
    std::unique_ptr<BenchmarkTimingLog> timing;
    if (options.benchmark) timing = std::make_unique<BenchmarkTimingLog>(options.benchmarkOutput);
    using Clock = std::chrono::steady_clock;
    auto nextTick = Clock::now();
    std::optional<Clock::time_point> runDeadline;
    if (timing && options.hasDuration) runDeadline = nextTick + options.benchmarkDuration;
    std::uint64_t sampleIndex = 0;
    while (g_running.load(std::memory_order_relaxed)) {
        if (timing) {
            const auto actualStart = Clock::now();
            if (runDeadline && actualStart >= *runDeadline) break;
            const auto delay = std::chrono::duration<double, std::milli>(actualStart - nextTick);
            timing->record(++sampleIndex, config.sampleInterval.count(), delay.count());
        }
        const auto tickUtc = std::chrono::system_clock::now();
        const auto sample = collector.collect();
        const auto processSnapshot = processCollector.collect();
        if (sample) samples.push(*sample);
        std::optional<sentinel::telemetry::ProcessGroupSnapshot> selectedProcessGroups;
        if (processSnapshot) {
            selectedProcessGroups = sentinel::telemetry::selectProcessGroups(
                sentinel::telemetry::aggregateProcesses(*processSnapshot),
                config.topProcessGroupsPerMetric, config.pinnedProcessGroups);
            processHistory.push(*selectedProcessGroups);
        }
        if (sample) {
            const sentinel::telemetry::SampleTime observedAt{sample->timestamp, sample->utcTimestamp};
            for (auto& detection : detector.analyze(*sample)) {
                eventStore.append(sentinel::attribution::attachProcessContext(
                    std::move(detection), observedAt,
                    selectedProcessGroups ? &*selectedProcessGroups : nullptr));
            }
        }
        if (sample || selectedProcessGroups) eventStore.commitTick(tickUtc, sample, selectedProcessGroups);
        nextTick += config.sampleInterval;
        std::this_thread::sleep_until(nextTick);
    }
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc >= 2 && std::string_view(argv[1]) == "start") {
            return startCommand(argc, argv);
        }
        if (argc >= 2 && std::string_view(argv[1]) == "archive") {
            return archiveCommand(argc, argv);
        }
        printUsage();
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "Sentinel: " << error.what() << '\n';
        return 1;
    }
}
