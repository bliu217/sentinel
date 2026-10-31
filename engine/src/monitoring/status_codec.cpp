#include "sentinel/monitoring/monitor.h"

#include <nlohmann/json.hpp>
#include <windows.h>

namespace sentinel::monitoring {
namespace {

using Json = nlohmann::json;

template<class T>
[[nodiscard]] Json encodeOptional(const std::optional<T>& value) {
    return value ? Json(*value) : Json(nullptr);
}

template<class T>
void decodeOptional(const Json& value, std::optional<T>& result) {
    if (value.is_null()) result.reset();
    else result = value.get<T>();
}

[[nodiscard]] std::int64_t millis(std::chrono::system_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

[[nodiscard]] std::chrono::system_clock::time_point utc(std::int64_t value) {
    return std::chrono::system_clock::time_point{std::chrono::milliseconds(value)};
}

[[nodiscard]] std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size == 0) throw std::runtime_error("Invalid group name encoding");
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), size, nullptr, nullptr) == 0) {
        throw std::runtime_error("Invalid group name encoding");
    }
    return result;
}

[[nodiscard]] std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size == 0) throw std::runtime_error("Invalid UTF-8 group name");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), size) == 0) {
        throw std::runtime_error("Invalid UTF-8 group name");
    }
    return result;
}

}  // namespace

NLOHMANN_JSON_SERIALIZE_ENUM(MonitoringState, {
    {MonitoringState::Starting, "Starting"}, {MonitoringState::Operating, "Operating"},
    {MonitoringState::Recovering, "Recovering"}, {MonitoringState::Failed, "Failed"},
    {MonitoringState::Stopped, "Stopped"}})
NLOHMANN_JSON_SERIALIZE_ENUM(Availability, {
    {Availability::Unavailable, "Unavailable"}, {Availability::Baseline, "Baseline"},
    {Availability::Available, "Available"}})

void to_json(Json& json, const Diagnostic& value) {
    json = {{"utcMs", value.utcMs}, {"source", value.source}, {"operation", value.operation},
        {"message", value.message}, {"code", encodeOptional(value.code)}, {"generation", value.generation}};
}

void from_json(const Json& json, Diagnostic& value) {
    json.at("utcMs").get_to(value.utcMs);
    json.at("source").get_to(value.source);
    json.at("operation").get_to(value.operation);
    json.at("message").get_to(value.message);
    decodeOptional(json.at("code"), value.code);
    json.at("generation").get_to(value.generation);
}

void to_json(Json& json, const SourceHealth& value) {
    json = {{"availability", value.availability}, {"lastSuccessUtcMs", encodeOptional(value.lastSuccessUtcMs)},
        {"consecutiveFailures", value.consecutiveFailures}, {"attemptedUtcMs", millis(value.report.attemptedAt)},
        {"operation", static_cast<int>(value.report.operation)}, {"errorCode", encodeOptional(value.report.errorCode)},
        {"isBaseline", value.report.isBaseline}, {"hasCpuBaseline", value.report.hasCpuBaseline},
        {"enumerated", value.report.enumerated}, {"read", value.report.read}, {"skipped", value.report.skipped}};
}

void from_json(const Json& json, SourceHealth& value) {
    json.at("availability").get_to(value.availability);
    decodeOptional(json.at("lastSuccessUtcMs"), value.lastSuccessUtcMs);
    json.at("consecutiveFailures").get_to(value.consecutiveFailures);
    value.report.attemptedAt = utc(json.at("attemptedUtcMs").get<std::int64_t>());
    value.report.operation = static_cast<telemetry::CollectionOperation>(json.at("operation").get<int>());
    decodeOptional(json.at("errorCode"), value.report.errorCode);
    json.at("isBaseline").get_to(value.report.isBaseline);
    json.at("hasCpuBaseline").get_to(value.report.hasCpuBaseline);
    json.at("enumerated").get_to(value.report.enumerated);
    json.at("read").get_to(value.report.read);
    json.at("skipped").get_to(value.report.skipped);
}

void to_json(Json& json, const SavedSnapshot& value) {
    json = {{"ownerId", value.ownerId}, {"generation", value.generation}, {"sequence", value.sequence},
        {"tickUtcMs", value.tickUtcMs}, {"system", nullptr}, {"processes", nullptr}};
    if (value.system) {
        json["system"] = {{"utcMs", millis(value.system->utcTimestamp)},
            {"cpuUsagePercent", value.system->cpuUsagePercent}, {"memoryUsedBytes", value.system->memoryUsedBytes},
            {"memoryAvailableBytes", value.system->memoryAvailableBytes}};
    }
    if (value.processes) {
        Json groups = Json::array();
        for (const telemetry::ProcessGroup& group : value.processes->groups) {
            groups.push_back({{"name", utf8(group.name)}, {"cpuUsagePercent", encodeOptional(group.cpuUsagePercent)},
                {"workingSetBytes", group.workingSetBytes}, {"privateBytes", group.privateBytes},
                {"processCount", group.processCount}});
        }
        json["processes"] = {{"utcMs", millis(value.processes->time.utc)}, {"groups", std::move(groups)}};
    }
}

void from_json(const Json& json, SavedSnapshot& value) {
    json.at("ownerId").get_to(value.ownerId);
    json.at("generation").get_to(value.generation);
    json.at("sequence").get_to(value.sequence);
    json.at("tickUtcMs").get_to(value.tickUtcMs);
    if (!json.at("system").is_null()) {
        const Json& system = json.at("system");
        value.system = telemetry::SystemSample{.cpuUsagePercent = system.at("cpuUsagePercent").get<double>(),
            .memoryUsedBytes = system.at("memoryUsedBytes").get<std::uint64_t>(),
            .memoryAvailableBytes = system.at("memoryAvailableBytes").get<std::uint64_t>(),
            .utcTimestamp = utc(system.at("utcMs").get<std::int64_t>())};
    }
    if (!json.at("processes").is_null()) {
        const Json& processes = json.at("processes");
        value.processes.emplace();
        value.processes->time.utc = utc(processes.at("utcMs").get<std::int64_t>());
        for (const Json& group : processes.at("groups")) {
            telemetry::ProcessGroup item;
            item.name = wide(group.at("name").get<std::string>());
            decodeOptional(group.at("cpuUsagePercent"), item.cpuUsagePercent);
            group.at("workingSetBytes").get_to(item.workingSetBytes);
            group.at("privateBytes").get_to(item.privateBytes);
            group.at("processCount").get_to(item.processCount);
            value.processes->groups.push_back(std::move(item));
        }
    }
}

void to_json(Json& json, const MonitoringHealth& value) {
    json = {{"state", value.state}, {"system", value.system}, {"processes", value.processes},
        {"recoveryAttempts", value.recoveryAttempts}, {"nextRecoveryUtcMs", encodeOptional(value.nextRecoveryUtcMs)},
        {"gapStartedUtcMs", encodeOptional(value.gapStartedUtcMs)},
        {"lastGapMilliseconds", encodeOptional(value.lastGapMilliseconds)},
        {"lastFailure", encodeOptional(value.lastFailure)}, {"hasLoggingFailure", value.hasLoggingFailure}};
}

void from_json(const Json& json, MonitoringHealth& value) {
    json.at("state").get_to(value.state);
    json.at("system").get_to(value.system);
    json.at("processes").get_to(value.processes);
    json.at("recoveryAttempts").get_to(value.recoveryAttempts);
    decodeOptional(json.at("nextRecoveryUtcMs"), value.nextRecoveryUtcMs);
    decodeOptional(json.at("gapStartedUtcMs"), value.gapStartedUtcMs);
    decodeOptional(json.at("lastGapMilliseconds"), value.lastGapMilliseconds);
    decodeOptional(json.at("lastFailure"), value.lastFailure);
    json.at("hasLoggingFailure").get_to(value.hasLoggingFailure);
}

std::string serializeStatus(const MonitoringStatus& status) {
    return Json{{"version", 1}, {"ownerId", status.ownerId}, {"generation", status.generation},
        {"health", status.health}, {"snapshot", encodeOptional(status.snapshot)},
        {"diagnostics", status.diagnostics}}.dump();
}

MonitoringStatus deserializeStatus(const std::string& text) {
    const Json json = Json::parse(text);
    if (json.at("version").get<int>() != 1) throw std::runtime_error("Unsupported status protocol version");
    MonitoringStatus result;
    json.at("ownerId").get_to(result.ownerId);
    json.at("generation").get_to(result.generation);
    json.at("health").get_to(result.health);
    decodeOptional(json.at("snapshot"), result.snapshot);
    json.at("diagnostics").get_to(result.diagnostics);
    return result;
}

}  // namespace sentinel::monitoring
