#include "paths.h"

#include <stdexcept>
#include <string>

#include <shlobj.h>

namespace sentinel::paths {
namespace {

[[nodiscard]] std::filesystem::path knownFolder(const KNOWNFOLDERID& folder) {
    PWSTR raw = nullptr;
    const HRESULT result = SHGetKnownFolderPath(folder, 0, nullptr, &raw);
    if (FAILED(result)) {
        throw std::runtime_error("Cannot locate Windows known folder (HRESULT " +
            std::to_string(static_cast<unsigned long>(result)) + ")");
    }
    const std::filesystem::path path(raw);
    CoTaskMemFree(raw);
    return path;
}

}  // namespace

std::filesystem::path localDataDirectory() {
    return knownFolder(FOLDERID_LocalAppData) / L"Sentinel";
}

std::filesystem::path databasePath() {
    return localDataDirectory() / L"sentinel.db";
}

std::filesystem::path archiveDirectory() {
    return knownFolder(FOLDERID_Documents) / L"Sentinel" / L"investigations";
}

std::filesystem::path exportsDirectory() {
    return localDataDirectory() / L"exports";
}

}  // namespace sentinel::paths
