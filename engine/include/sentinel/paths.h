#pragma once

#include <filesystem>

namespace sentinel::paths {

[[nodiscard]] std::filesystem::path localDataDirectory();
[[nodiscard]] std::filesystem::path databasePath();
[[nodiscard]] std::filesystem::path archiveDirectory();
[[nodiscard]] std::filesystem::path exportsDirectory();

}  // namespace sentinel::paths
