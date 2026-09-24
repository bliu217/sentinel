#pragma once

#include "telemetry/system_collector.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <vector>

namespace sentinel::detection {

enum class EventType {
    HighCpu,
    HighMemory,
    SamplingDelay,
};

enum class EventState {
    Started,
    Stopped,
    Occurred,
};

struct DetectionEvent {
    EventType type{};
    EventState state{};
    std::chrono::steady_clock::time_point timestamp{};
    double value{};
    std::optional<std::chrono::system_clock::time_point> occurredAtUtc;
};

class AnomalyDetector {
public:
    [[nodiscard]] std::vector<DetectionEvent> analyze(const telemetry::SystemSample& sample);

private:
    struct EpisodeState {
        bool active{};
        std::size_t consecutiveSamples{};
        std::chrono::steady_clock::time_point candidateTimestamp{};
        std::chrono::system_clock::time_point candidateUtc{};
        double candidateValue{};
    };

    static void updateEpisode(
        EventType type,
        bool thresholdReached,
        double value,
        std::chrono::steady_clock::time_point timestamp,
        std::chrono::system_clock::time_point utcTimestamp,
        EpisodeState& episode,
        std::vector<DetectionEvent>& events);

    EpisodeState cpuEpisode_;
    EpisodeState memoryEpisode_;
    std::optional<std::chrono::steady_clock::time_point> previousTimestamp_;
};

}  // namespace sentinel::detection
