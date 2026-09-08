#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pred
{

// Well-known directories and asset path resolution.
//
// Search roots, in order:
//   1. <executable dir>/Assets          (packaged builds)
//   2. build-time generated assets      (compiled shaders, development builds)
//   3. the repository Assets directory  (found by walking up from the executable,
//                                        or PRED_SOURCE_DIR as a fallback)
class Paths
{
public:
    static void Init(const char* argv0, const std::filesystem::path& assetsOverride = {});

    static const std::filesystem::path& ExecutableDir();
    static const std::filesystem::path& AssetsRoot();
    static const std::filesystem::path& UserDataDir();
    static const std::vector<std::filesystem::path>& SearchRoots();

    static void AddSearchRoot(const std::filesystem::path& root);

    // Finds the first existing file for an asset-relative path such as "Config/defaults.json".
    static std::optional<std::filesystem::path> Resolve(const std::filesystem::path& relative);

    static std::string Describe();
};

} // namespace pred
