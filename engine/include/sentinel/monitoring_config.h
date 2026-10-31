#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace sentinel {

struct MonitoringConfig {
    std::chrono::milliseconds sampleInterval{1000};
    std::size_t topProcessGroupsPerMetric{10};
    std::vector<std::wstring> pinnedProcessGroups;
    std::size_t inMemoryHistorySize{300};
    std::chrono::days retentionPeriod{30};
    std::uint64_t maxDatabaseSizeMiB{2048};

    void validate() const {
        if (sampleInterval <= std::chrono::milliseconds::zero())
            throw std::invalid_argument("Sample interval must be greater than zero");
        if (inMemoryHistorySize == 0)
            throw std::invalid_argument("In-memory history size must be greater than zero");
        if (retentionPeriod <= std::chrono::days::zero())
            throw std::invalid_argument("Retention period must be greater than zero");
        if (maxDatabaseSizeMiB == 0 || maxDatabaseSizeMiB >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / (1024 * 1024)))
            throw std::invalid_argument("Invalid database size cap");
    }
};

}  // namespace sentinel
