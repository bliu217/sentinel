#pragma once

#include "sentinel/monitoring/monitor.h"

#include <future>
#include <functional>
#include <stdexcept>

namespace sentinel::monitoring {

/** Transport errors are distinct from a reachable owner's Failed health. */
class ConnectionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** Same-user named-pipe client; neither starts nor stops the owner. */
class MonitoringClient {
public:
    explicit MonitoringClient(std::wstring channel = L"default");
    [[nodiscard]] MonitoringStatus read() const;
    [[nodiscard]] std::future<MonitoringStatus> readAsync() const;
private:
    std::wstring channel_;
};

/** Own one user-scoped endpoint until destruction; readers do not own monitoring. */
class MonitoringOwner {
public:
    explicit MonitoringOwner(std::function<MonitoringStatus()> read,
        std::wstring channel = L"default");
    ~MonitoringOwner();
    MonitoringOwner(const MonitoringOwner&) = delete;
    MonitoringOwner& operator=(const MonitoringOwner&) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sentinel::monitoring
