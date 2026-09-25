#include "attribution/anomaly_event.h"
#include "detection/anomaly_detector.h"
#include "monitoring_config.h"
#include "storage/event_store.h"
#include "storage/archive_manager.h"
#include "storage/process_history.h"
#include "storage/ring_buffer.h"
#include "storage/sqlite_store.h"
#include "telemetry/process_aggregator.h"
#include "telemetry/process_collector.h"
#include "telemetry/system_collector.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <filesystem>
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
              << "  sentinel archive create SLUG --name NAME\n"
              << "  sentinel archive add SLUG --from UTC --to UTC [--app EXE]... "
                 "[--events cpu,memory,collection-lag] [--include-samples]\n";
}

[[nodiscard]] std::filesystem::path dataDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length == path.size()) throw std::runtime_error("Cannot locate Sentinel executable");
    path.resize(length);
    return std::filesystem::path(path).parent_path() / "data";
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

[[nodiscard]] int archiveCommand(int argc, char* argv[], const std::filesystem::path& data) {
    if (argc < 5) { printUsage(); return 1; }
    const std::string_view action = argv[2];
    const std::string slug = argv[3];
    const std::filesystem::path dbPath = data / "sentinel.db";
    if (action == "add" && !std::filesystem::exists(dbPath)) {
        throw std::runtime_error("No Sentinel database exists yet");
    }
    std::filesystem::create_directories(data / "exports");
    sentinel::storage::SQLiteStore store(dbPath, 0);
    sentinel::storage::ArchiveManager archives(store, data / "archive");
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

[[nodiscard]] int startCommand(int argc, char* argv[], const std::filesystem::path& data) {
    sentinel::MonitoringConfig config;
    if (argc != 2) {
        if (argc != 4 || std::string_view(argv[2]) != "--max-db-size-mib") {
            printUsage(); return 1;
        }
        const std::string value = argv[3];
        std::size_t consumed = 0;
        config.maxDatabaseSizeMiB = std::stoull(value, &consumed);
        if (consumed != value.size()) {
            throw std::invalid_argument("Invalid database size cap");
        }
    }
    config.validate();
    if (SetConsoleCtrlHandler(onConsoleCtrl, TRUE) == 0) {
        throw std::runtime_error("Failed to register console control handler");
    }
    std::filesystem::create_directories(data / "archive");
    std::filesystem::create_directories(data / "exports");
    sentinel::storage::SQLiteStore database(data / "sentinel.db", config.maxDatabaseSizeMiB * 1024 * 1024);
    sentinel::storage::RetentionManager retention(database, config.retentionPeriod);
    retention.pruneOnStartup(std::chrono::system_clock::now());
    sentinel::telemetry::SystemCollector collector;
    sentinel::telemetry::ProcessCollector processCollector;
    sentinel::storage::RingBuffer samples(config.inMemoryHistorySize);
    sentinel::storage::ProcessHistory processHistory(config.inMemoryHistorySize);
    sentinel::detection::AnomalyDetector detector;
    sentinel::storage::EventStore eventStore(&database);
    using Clock = std::chrono::steady_clock;
    auto nextTick = Clock::now();
    while (g_running.load(std::memory_order_relaxed)) {
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
            return startCommand(argc, argv, dataDirectory());
        }
        if (argc >= 2 && std::string_view(argv[1]) == "archive") {
            return archiveCommand(argc, argv, dataDirectory());
        }
        printUsage();
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "Sentinel: " << error.what() << '\n';
        return 1;
    }
}
