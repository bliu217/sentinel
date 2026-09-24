#include "attribution/anomaly_event.h"
#include "detection/anomaly_detector.h"
#include "storage/event_store.h"
#include "storage/process_history.h"
#include "storage/ring_buffer.h"
#include "telemetry/process_aggregator.h"
#include "telemetry/process_collector.h"
#include "telemetry/system_collector.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <optional>
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
    std::cerr << "Usage: sentinel start\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 2 || std::string_view(argv[1]) != "start") {
        printUsage();
        return 1;
    }

    if (SetConsoleCtrlHandler(onConsoleCtrl, TRUE) == 0) {
        std::cerr << "Failed to register console control handler\n";
        return 1;
    }

    sentinel::telemetry::SystemCollector collector;
    sentinel::telemetry::ProcessCollector processCollector;
    sentinel::storage::RingBuffer samples(300);
    sentinel::storage::ProcessHistory processHistory(300);
    sentinel::detection::AnomalyDetector detector;
    sentinel::storage::EventStore eventStore;
    using Clock = std::chrono::steady_clock;
    auto nextTick = Clock::now();

    while (g_running.load(std::memory_order_relaxed)) {
        const auto sample = collector.collect();
        const auto processSnapshot = processCollector.collect();

        if (sample) {
            samples.push(*sample);
        }
        std::optional<sentinel::telemetry::ProcessGroupSnapshot> selectedProcesses;
        if (processSnapshot) {
            selectedProcesses = sentinel::telemetry::selectTopProcesses(
                sentinel::telemetry::aggregateProcesses(*processSnapshot));
            processHistory.push(*selectedProcesses);
        }

        if (sample) {
            const sentinel::telemetry::SampleTime observedAt{sample->timestamp, sample->utcTimestamp};
            for (auto& detection : detector.analyze(*sample)) {
                eventStore.append(sentinel::attribution::attachProcessContext(
                    std::move(detection),
                    observedAt,
                    selectedProcesses ? &*selectedProcesses : nullptr));
            }
        }

        nextTick += std::chrono::seconds(1);
        std::this_thread::sleep_until(nextTick);
    }

    return 0;
}
