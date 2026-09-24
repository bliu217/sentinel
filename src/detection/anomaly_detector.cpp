#include "detection/anomaly_detector.h"

#include <chrono>
#include <cstdint>

namespace sentinel::detection {
namespace {

constexpr double kHighCpuPercent = 90.0;
constexpr double kHighMemoryPercent = 90.0;
constexpr std::size_t kConfirmationSamples = 3;
constexpr auto kSamplingDelayThreshold = std::chrono::milliseconds(1500);

[[nodiscard]] double memoryUsagePercent(const telemetry::SystemSample& sample) noexcept {
    const double totalBytes =
        static_cast<double>(sample.memoryUsedBytes) + static_cast<double>(sample.memoryAvailableBytes);
    if (totalBytes == 0.0) {
        return 0.0;
    }
    return 100.0 * static_cast<double>(sample.memoryUsedBytes) / totalBytes;
}

}  // namespace

std::vector<DetectionEvent> AnomalyDetector::analyze(const telemetry::SystemSample& sample) {
    std::vector<DetectionEvent> events;

    updateEpisode(
        EventType::HighCpu,
        sample.cpuUsagePercent >= kHighCpuPercent,
        sample.cpuUsagePercent,
        sample.timestamp,
        sample.utcTimestamp,
        cpuEpisode_,
        events);

    const double memoryPercent = memoryUsagePercent(sample);
    updateEpisode(
        EventType::HighMemory,
        memoryPercent >= kHighMemoryPercent,
        memoryPercent,
        sample.timestamp,
        sample.utcTimestamp,
        memoryEpisode_,
        events);

    if (previousTimestamp_.has_value()) {
        const auto gap = sample.timestamp - *previousTimestamp_;
        if (gap > kSamplingDelayThreshold) {
            events.push_back(DetectionEvent{
                .type = EventType::SamplingDelay,
                .state = EventState::Occurred,
                .timestamp = sample.timestamp,
                .value = std::chrono::duration<double, std::milli>(gap).count(),
                .occurredAtUtc = sample.utcTimestamp,
            });
        }
    }
    previousTimestamp_ = sample.timestamp;

    return events;
}

void AnomalyDetector::updateEpisode(
    EventType type,
    bool thresholdReached,
    double value,
    std::chrono::steady_clock::time_point timestamp,
    std::chrono::system_clock::time_point utcTimestamp,
    EpisodeState& episode,
    std::vector<DetectionEvent>& events) {
    const bool confirmsCurrentState = episode.active ? !thresholdReached : thresholdReached;
    if (!confirmsCurrentState) {
        episode.consecutiveSamples = 0;
        return;
    }

    if (episode.consecutiveSamples == 0) {
        episode.candidateTimestamp = timestamp;
        episode.candidateUtc = utcTimestamp;
        episode.candidateValue = value;
    }

    ++episode.consecutiveSamples;
    if (episode.consecutiveSamples < kConfirmationSamples) {
        return;
    }

    episode.active = !episode.active;
    episode.consecutiveSamples = 0;
    events.push_back(DetectionEvent{
        .type = type,
        .state = episode.active ? EventState::Started : EventState::Stopped,
        .timestamp = episode.candidateTimestamp,
        .value = episode.candidateValue,
        .occurredAtUtc = episode.candidateUtc,
    });
}

}  // namespace sentinel::detection
