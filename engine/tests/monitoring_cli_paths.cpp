#include "sentinel/paths.h"

#include <windows.h>

namespace sentinel::paths {

/** Substitute only the filesystem boundary when testing the production CLI entrypoint. */
std::filesystem::path localDataDirectory() {
    return std::filesystem::temp_directory_path() /
        (L"sentinel-cli-test-" + std::to_wstring(GetCurrentProcessId()));
}

std::filesystem::path databasePath() { return localDataDirectory() / L"sentinel.db"; }
std::filesystem::path archiveDirectory() { return localDataDirectory() / L"investigations"; }
std::filesystem::path exportsDirectory() { return localDataDirectory() / L"exports"; }

}  // namespace sentinel::paths
