#include "telemetry/system_collector.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <format>
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

[[nodiscard]] constexpr double bytesToGib(std::uint64_t bytes) noexcept {
    return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
}

void printSample(const sentinel::telemetry::SystemSample& sample) {
    const std::time_t time = std::chrono::system_clock::to_time_t(sample.timestamp);
    std::tm utc{};
    if (gmtime_s(&utc, &time) != 0) {
        return;
    }

    std::cout << std::format(
        "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z cpu={:.1f}% mem_used={:.1f}GiB mem_avail={:.1f}GiB\n",
        utc.tm_year + 1900,
        utc.tm_mon + 1,
        utc.tm_mday,
        utc.tm_hour,
        utc.tm_min,
        utc.tm_sec,
        sample.cpuUsagePercent,
        bytesToGib(sample.memoryUsedBytes),
        bytesToGib(sample.memoryAvailableBytes));
    std::cout.flush();
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
    using Clock = std::chrono::steady_clock;
    auto nextTick = Clock::now();

    while (g_running.load(std::memory_order_relaxed)) {
        if (const auto sample = collector.collect()) {
            printSample(*sample);
        }

        nextTick += std::chrono::seconds(1);
        std::this_thread::sleep_until(nextTick);
    }

    return 0;
}
