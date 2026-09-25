#include "storage/archive_manager.h"

#include <nlohmann/json.hpp>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace sentinel::storage {
namespace {

using Json = nlohmann::json;

class ArchiveLock {
public:
    explicit ArchiveLock(const std::filesystem::path& root) {
        std::filesystem::create_directories(root);
        const std::filesystem::path path = root / ".archive.lock";
        handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) throw std::runtime_error("Another archive operation is active");
    }
    ~ArchiveLock() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    ArchiveLock(const ArchiveLock&) = delete;
    ArchiveLock& operator=(const ArchiveLock&) = delete;

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

[[nodiscard]] std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length == 0) throw std::runtime_error("Invalid application name encoding");
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), length, nullptr, nullptr) == 0) {
        throw std::runtime_error("Invalid application name encoding");
    }
    return result;
}

[[nodiscard]] std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

[[nodiscard]] std::string isoTime(std::int64_t milliseconds) {
    const std::time_t seconds = static_cast<std::time_t>(milliseconds / 1000);
    std::tm utc{};
    if (gmtime_s(&utc, &seconds) != 0) throw std::runtime_error("Invalid UTC timestamp");
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
           << std::setw(3) << std::setfill('0') << (milliseconds % 1000) << 'Z';
    return output.str();
}

[[nodiscard]] std::int64_t millis(std::chrono::system_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

[[nodiscard]] const char* eventName(detection::EventType type) {
    switch (type) {
    case detection::EventType::HighCpu: return "cpu_anomaly";
    case detection::EventType::HighMemory: return "memory_anomaly";
    case detection::EventType::SamplingDelay: return "collection_lag";
    }
    throw std::runtime_error("Unknown event type");
}

[[nodiscard]] const char* stateName(detection::EventState state) {
    switch (state) {
    case detection::EventState::Started: return "started";
    case detection::EventState::Stopped: return "stopped";
    case detection::EventState::Occurred: return "occurred";
    }
    throw std::runtime_error("Unknown event state");
}

[[nodiscard]] Json systemJson(const std::optional<telemetry::SystemSample>& sample) {
    if (!sample) return nullptr;
    const double total = static_cast<double>(sample->memoryUsedBytes) +
        static_cast<double>(sample->memoryAvailableBytes);
    return Json{{"timestamp", isoTime(millis(sample->utcTimestamp))},
        {"cpu_percent", sample->cpuUsagePercent},
        {"memory_percent", total > 0 ? 100.0 * static_cast<double>(sample->memoryUsedBytes) / total : 0.0},
        {"memory_used_bytes", sample->memoryUsedBytes},
        {"memory_available_bytes", sample->memoryAvailableBytes}};
}

[[nodiscard]] bool matchesApp(const telemetry::ProcessGroup& group, const ArchiveSelection& selection) {
    if (selection.applications.empty()) return true;
    const std::wstring name = lower(group.name);
    return std::any_of(selection.applications.begin(), selection.applications.end(), [&](const auto& app) {
        return name == lower(telemetry::processGroupName(app));
    });
}

[[nodiscard]] Json processGroupsJson(
    const std::vector<telemetry::ProcessGroup>& processGroups, const ArchiveSelection& selection) {
    Json groups = Json::array();
    for (const telemetry::ProcessGroup& group : processGroups) {
        if (!matchesApp(group, selection)) continue;
        groups.push_back(Json{{"name", utf8(group.name)},
            {"cpu_percent", group.cpuUsagePercent ? Json(*group.cpuUsagePercent) : Json(nullptr)},
            {"working_set_bytes", group.workingSetBytes},
            {"private_bytes", group.privateBytes},
            {"process_count", group.processCount}});
    }
    return groups;
}

[[nodiscard]] std::string applicationKey(const ArchiveSelection& selection) {
    std::vector<std::string> names;
    for (const std::wstring& app : selection.applications) names.push_back(utf8(lower(app)));
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    std::string result;
    for (const std::string& name : names) result += ":" + std::to_string(name.size()) + ":" + name;
    return result;
}

[[nodiscard]] bool eventMatches(const StoredAnomaly& event, const ArchiveSelection& selection) {
    if (!selection.eventTypes.empty() &&
        std::find(selection.eventTypes.begin(), selection.eventTypes.end(), event.type) == selection.eventTypes.end()) {
        return false;
    }
    if (selection.applications.empty()) return true;
    if (!event.processContext) return false;
    return std::any_of(event.processContext->groups.begin(), event.processContext->groups.end(),
        [&](const auto& group) { return matchesApp(group, selection); });
}

[[nodiscard]] Json anomalyJson(const StoredTick& tick, const StoredAnomaly& event,
    const ArchiveSelection& selection, const std::string& storeId) {
    return Json{{"record_id", storeId + ":anomaly:" + std::to_string(event.id) +
            applicationKey(selection)},
        {"timestamp", isoTime(event.occurredUtcMilliseconds)},
        {"observed_at", isoTime(event.observedUtcMilliseconds)},
        {"event_type", eventName(event.type)},
        {"state", stateName(event.state)},
        {"severity", event.state == detection::EventState::Stopped ? "info" : "warning"},
        {"value", event.value},
        {"system", systemJson(tick.system)},
        {"process_sampled_at", event.processContext ?
            Json(isoTime(millis(event.processContext->sampledAt.utc))) : Json(nullptr)},
        {"processes", event.processContext ?
            processGroupsJson(event.processContext->groups, selection) : Json::array()}};
}

[[nodiscard]] Json sampleJson(const StoredTick& tick, const ArchiveSelection& selection,
    const std::string& storeId) {
    return Json{{"record_id", storeId + ":tick:" + std::to_string(tick.id) +
            applicationKey(selection)},
        {"timestamp", isoTime(tick.utcMilliseconds)},
        {"system", systemJson(tick.system)},
        {"process_sampled_at", tick.processContext ?
            Json(isoTime(millis(tick.processContext->time.utc))) : Json(nullptr)},
        {"processes", tick.processContext ?
            processGroupsJson(tick.processContext->groups, selection) : Json::array()}};
}

void flushDurable(const std::filesystem::path& path) {
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot flush archive file");
    const BOOL flushed = FlushFileBuffers(handle);
    CloseHandle(handle);
    if (flushed == 0) throw std::runtime_error("Cannot flush archive file");
}

void writeManifest(const std::filesystem::path& path, const Json& manifest) {
    const std::filesystem::path temporary = path.parent_path() / "manifest.json.tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << manifest.dump(2) << '\n';
        output.flush();
        if (!output) throw std::runtime_error("Failed to write project manifest");
    }
    flushDurable(temporary);
    if (std::filesystem::exists(path)) {
        if (MoveFileExW(temporary.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
            throw std::runtime_error("Failed to replace project manifest (Windows error " +
                std::to_string(GetLastError()) + ")");
        }
    } else {
        std::filesystem::rename(temporary, path);
    }
}

[[nodiscard]] Json readManifest(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Project manifest is missing");
    Json manifest = Json::parse(input);
    if (manifest.at("schema_version") != 1) throw std::runtime_error("Unsupported project manifest version");
    return manifest;
}

void recoverFile(const std::filesystem::path& path, std::uint64_t committed) {
    if (!std::filesystem::exists(path)) {
        if (committed != 0) throw std::runtime_error("Committed archive file is missing");
        return;
    }
    const std::uint64_t length = std::filesystem::file_size(path);
    if (length < committed) throw std::runtime_error("Archive file is shorter than its committed length");
    if (length > committed) std::filesystem::resize_file(path, committed);
}

[[nodiscard]] std::unordered_set<std::string> existingIds(const std::filesystem::path& path) {
    std::unordered_set<std::string> ids;
    if (!std::filesystem::exists(path)) return ids;
    std::ifstream input(path, std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) ids.insert(Json::parse(line).at("record_id").get<std::string>());
    }
    if (!input.eof()) throw std::runtime_error("Failed to read archive records");
    return ids;
}

void appendLine(std::ofstream& output, const Json& record, std::unordered_set<std::string>& ids) {
    const std::string id = record.at("record_id").get<std::string>();
    if (ids.insert(id).second) output << record.dump() << '\n';
    if (!output) throw std::runtime_error("Failed to append archive record");
}

}  // namespace

ArchiveManager::ArchiveManager(SQLiteStore& store, std::filesystem::path archiveRoot)
    : store_(store), archiveRoot_(std::move(archiveRoot)) {}

std::filesystem::path ArchiveManager::projectPath(const std::string& slug) const {
    if (slug.empty() || slug.size() > 64 || slug.front() == '-' || slug.back() == '-' ||
        !std::all_of(slug.begin(), slug.end(), [](unsigned char character) {
            return std::islower(character) || std::isdigit(character) || character == '-';
        })) {
        throw std::invalid_argument("Project slug must use lowercase letters, digits, and internal hyphens");
    }
    const std::filesystem::path path = archiveRoot_ / slug;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        throw std::invalid_argument("Archive project cannot be a link");
    }
    return path;
}

void ArchiveManager::createProject(const std::string& slug, const std::string& name) {
    if (name.empty()) throw std::invalid_argument("Project name cannot be empty");
    ArchiveLock lock(archiveRoot_);
    const std::filesystem::path project = projectPath(slug);
    if (std::filesystem::exists(project)) throw std::runtime_error("Project already exists");
    std::filesystem::create_directory(project);
    Json manifest{{"schema_version", 1}, {"project", name},
        {"created_at", isoTime(millis(std::chrono::system_clock::now()))},
        {"applications", Json::array()}, {"contains", Json::array()},
        {"files", Json::object()}, {"committed_bytes", Json::object()}};
    writeManifest(project / "manifest.json", manifest);
}

void ArchiveManager::addToProject(const std::string& slug, const ArchiveSelection& selection) {
    if (selection.from >= selection.to) throw std::invalid_argument("Archive time range must be increasing");
    ArchiveLock lock(archiveRoot_);
    const std::filesystem::path project = projectPath(slug);
    Json manifest = readManifest(project / "manifest.json");
    const std::filesystem::path anomaliesPath = project / "anomalies.jsonl";
    const std::filesystem::path samplesPath = project / "samples.jsonl";
    recoverFile(anomaliesPath, manifest["committed_bytes"].value("anomalies", 0ULL));
    recoverFile(samplesPath, manifest["committed_bytes"].value("samples", 0ULL));
    std::unordered_set<std::string> anomalyIds = existingIds(anomaliesPath);
    std::unordered_set<std::string> sampleIds = existingIds(samplesPath);
    std::ofstream anomalyOutput(anomaliesPath, std::ios::binary | std::ios::app);
    if (!anomalyOutput) throw std::runtime_error("Cannot open anomaly archive");
    std::ofstream sampleOutput;
    if (selection.includeSamples) {
        sampleOutput.open(samplesPath, std::ios::binary | std::ios::app);
        if (!sampleOutput) throw std::runtime_error("Cannot open sample archive");
    }
    std::int64_t cursorUtc = INT64_MIN;
    std::int64_t cursorId = 0;
    for (;;) {
        const std::vector<StoredTick> page = store_.readTicks(selection.from, selection.to, cursorUtc, cursorId);
        if (page.empty()) break;
        for (const StoredTick& tick : page) {
            if (selection.includeSamples) {
                appendLine(sampleOutput, sampleJson(tick, selection, store_.storeId()), sampleIds);
            }
            for (const StoredAnomaly& event : tick.anomalies) {
                if (!eventMatches(event, selection)) continue;
                appendLine(anomalyOutput, anomalyJson(tick, event, selection, store_.storeId()), anomalyIds);
                const std::string type = eventName(event.type);
                if (std::find(manifest["contains"].begin(), manifest["contains"].end(), type) ==
                    manifest["contains"].end()) manifest["contains"].push_back(type);
            }
        }
        cursorUtc = page.back().utcMilliseconds;
        cursorId = page.back().id;
    }
    anomalyOutput.flush();
    if (!anomalyOutput) throw std::runtime_error("Failed to flush anomaly archive");
    anomalyOutput.close();
    flushDurable(anomaliesPath);
    if (selection.includeSamples) {
        sampleOutput.flush();
        if (!sampleOutput) throw std::runtime_error("Failed to flush sample archive");
        sampleOutput.close();
        flushDurable(samplesPath);
    }
    for (const std::wstring& app : selection.applications) {
        const std::string name = utf8(lower(app));
        if (std::find(manifest["applications"].begin(), manifest["applications"].end(), name) ==
            manifest["applications"].end()) manifest["applications"].push_back(name);
    }
    manifest["files"]["anomalies"] = "anomalies.jsonl";
    manifest["committed_bytes"]["anomalies"] = std::filesystem::file_size(anomaliesPath);
    if (selection.includeSamples) {
        manifest["files"]["samples"] = "samples.jsonl";
        manifest["committed_bytes"]["samples"] = std::filesystem::file_size(samplesPath);
    }
    writeManifest(project / "manifest.json", manifest);
}

}  // namespace sentinel::storage
