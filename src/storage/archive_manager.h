#pragma once

#include "storage/sqlite_store.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace sentinel::storage {

struct ArchiveSelection {
    std::chrono::system_clock::time_point from;
    std::chrono::system_clock::time_point to;
    std::vector<std::wstring> applications;
    std::vector<detection::EventType> eventTypes;
    bool includeSamples{};
};

class ArchiveManager {
public:
    ArchiveManager(SQLiteStore& store, std::filesystem::path archiveRoot);
    void createProject(const std::string& slug, const std::string& name);
    void addToProject(const std::string& slug, const ArchiveSelection& selection);

private:
    [[nodiscard]] std::filesystem::path projectPath(const std::string& slug) const;

    SQLiteStore& store_;
    std::filesystem::path archiveRoot_;
};

}  // namespace sentinel::storage
