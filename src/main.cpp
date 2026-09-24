#include "detection/anomaly_detector.h"
#include "storage/event_store.h"
#include "storage/ring_buffer.h"
#include "telemetry/system_collector.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>

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
    sentinel::storage::RingBuffer samples(300);
    sentinel::detection::AnomalyDetector detector;
    sentinel::storage::EventStore eventStore;
    using Clock = std::chrono::steady_clock;
    auto nextTick = Clock::now();

    while (g_running.load(std::memory_order_relaxed)) {
        if (const auto sample = collector.collect()) {
            samples.push(*sample);
            for (const auto& event : detector.analyze(*sample)) {
                eventStore.append(event);
            }
        }

        nextTick += std::chrono::seconds(1);
        std::this_thread::sleep_until(nextTick);
    }

    return 0;
}
