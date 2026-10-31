#include "sentinel/monitoring/client.h"

#include <windows.h>
#include <thread>

/** Separate-process fixture; no test controls are added to the product CLI. */
int wmain(int argc, wchar_t* argv[]) {
    if (argc != 4) return 2;
    const HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, argv[3]);
    if (!stop) return 3;
    try {
        const std::filesystem::path directory(argv[1]);
        sentinel::monitoring::Monitor monitor({}, directory / "sentinel.db", directory / "diagnostics.log");
        sentinel::monitoring::MonitoringOwner owner([&] { return monitor.read(); }, argv[2]);
        std::jthread worker([&](std::stop_token token) { monitor.run(token); });
        WaitForSingleObject(stop, INFINITE);
        worker.request_stop();
        worker.join();
    } catch (const std::exception&) {
        CloseHandle(stop);
        return 4;
    }
    CloseHandle(stop);
    return 0;
}
