#include "telemetry/process_aggregator.h"

#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace sentinel::telemetry {
namespace {

[[nodiscard]] std::wstring lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return text;
}

[[nodiscard]] std::wstring groupName(const std::wstring& imageName) {
    const std::wstring normalized = lower(imageName);
    if (normalized == L"cursor.exe") {
        return L"Cursor";
    }
    if (normalized == L"vmmemwsl.exe" || normalized == L"vmmem.exe") {
        return L"WSL";
    }
    return normalized;
}

}  // namespace

ProcessGroupSnapshot aggregateProcesses(const ProcessSnapshot& snapshot) {
    ProcessGroupSnapshot result{.time = snapshot.time};
    std::unordered_map<std::wstring, std::size_t> indexes;
    std::unordered_set<std::wstring> incompleteCpu;
    for (const ProcessSample& process : snapshot.processes) {
        const std::wstring name = groupName(process.imageName);
        const auto [position, inserted] = indexes.try_emplace(name, result.groups.size());
        if (inserted) {
            result.groups.push_back(ProcessGroup{.name = name});
        }
        ProcessGroup& group = result.groups[position->second];
        if (process.cpuUsagePercent.has_value()) {
            group.cpuUsagePercent = group.cpuUsagePercent.value_or(0.0) + *process.cpuUsagePercent;
        } else {
            incompleteCpu.insert(name);
        }
        group.workingSetBytes += process.workingSetBytes;
        group.privateBytes += process.privateBytes;
        ++group.processCount;
    }
    for (ProcessGroup& group : result.groups) {
        if (incompleteCpu.contains(group.name)) {
            group.cpuUsagePercent.reset();
        } else if (group.cpuUsagePercent) {
            group.cpuUsagePercent = std::clamp(*group.cpuUsagePercent, 0.0, 100.0);
        }
    }
    std::sort(result.groups.begin(), result.groups.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    return result;
}

ProcessGroupSnapshot selectTopProcesses(ProcessGroupSnapshot snapshot, std::size_t topPerMetric) {
    const std::size_t count = std::min(topPerMetric, snapshot.groups.size());
    std::unordered_set<std::wstring> keep{L"Cursor", L"WSL"};

    auto byCpu = snapshot.groups;
    std::sort(byCpu.begin(), byCpu.end(), [](const auto& left, const auto& right) {
        const double leftCpu = left.cpuUsagePercent.value_or(-1.0);
        const double rightCpu = right.cpuUsagePercent.value_or(-1.0);
        return leftCpu == rightCpu ? left.name < right.name : leftCpu > rightCpu;
    });
    for (std::size_t index = 0; index < count; ++index) {
        keep.insert(byCpu[index].name);
    }

    auto byMemory = snapshot.groups;
    std::sort(byMemory.begin(), byMemory.end(), [](const auto& left, const auto& right) {
        return left.workingSetBytes == right.workingSetBytes
                   ? left.name < right.name
                   : left.workingSetBytes > right.workingSetBytes;
    });
    for (std::size_t index = 0; index < count; ++index) {
        keep.insert(byMemory[index].name);
    }

    std::erase_if(snapshot.groups, [&](const ProcessGroup& group) {
        return !keep.contains(group.name);
    });
    return snapshot;
}

std::wstring formatProcessSummary(
    const ProcessGroupSnapshot& snapshot,
    const std::vector<std::wstring>& names) {
    std::wostringstream output;
    output << std::fixed << std::setprecision(1);
    bool first = true;
    for (const std::wstring& name : names) {
        const auto found = std::find_if(snapshot.groups.begin(), snapshot.groups.end(), [&](const auto& group) {
            return group.name == name;
        });
        if (found == snapshot.groups.end()) {
            continue;
        }
        if (!first) {
            output << L"; ";
        }
        first = false;
        output << found->name << L" consumed ";
        if (found->cpuUsagePercent) {
            output << *found->cpuUsagePercent << L"% CPU";
        } else {
            output << L"CPU pending baseline";
        }
        output << L" and "
               << static_cast<double>(found->workingSetBytes) / (1024.0 * 1024.0 * 1024.0)
               << L" GiB resident memory";
    }
    if (first) {
        return L"No matching processes were sampled.";
    }
    output << L'.';
    return output.str();
}

}  // namespace sentinel::telemetry
