#include "Engine/Core/Paths.h"

#include <cstdlib>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace pred
{
namespace
{

struct PathState
{
    std::filesystem::path executableDir;
    std::filesystem::path assetsRoot;
    std::filesystem::path userDataDir;
    std::vector<std::filesystem::path> searchRoots;
};

PathState& State()
{
    static PathState state;
    return state;
}

std::filesystem::path FindExecutableDir(const char* argv0)
{
#ifdef _WIN32
    wchar_t buffer[MAX_PATH * 2];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length > 0 && length < std::size(buffer))
    {
        return std::filesystem::path(buffer).parent_path();
    }
#endif
    std::error_code ec;
    if (argv0 != nullptr)
    {
        const auto absolute = std::filesystem::absolute(argv0, ec);
        if (!ec)
        {
            return absolute.parent_path();
        }
    }
    return std::filesystem::current_path(ec);
}

bool LooksLikeAssetsRoot(const std::filesystem::path& candidate)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(candidate / "Config" / "defaults.json", ec);
}

std::filesystem::path DiscoverAssetsRoot(const std::filesystem::path& executableDir)
{
    // Packaged layout: Assets next to the executable.
    if (LooksLikeAssetsRoot(executableDir / "Assets"))
    {
        return executableDir / "Assets";
    }
    // Development layout: walk up from the build directory to the repository root.
    std::filesystem::path dir = executableDir;
    for (int i = 0; i < 8 && !dir.empty(); ++i)
    {
        if (LooksLikeAssetsRoot(dir / "Assets"))
        {
            return dir / "Assets";
        }
        const auto parent = dir.parent_path();
        if (parent == dir)
        {
            break;
        }
        dir = parent;
    }
#ifdef PRED_SOURCE_DIR
    if (LooksLikeAssetsRoot(std::filesystem::path(PRED_SOURCE_DIR) / "Assets"))
    {
        return std::filesystem::path(PRED_SOURCE_DIR) / "Assets";
    }
#endif
    return executableDir / "Assets";
}

std::filesystem::path DiscoverUserDataDir(const std::filesystem::path& executableDir)
{
#ifdef _WIN32
    if (const char* appData = std::getenv("APPDATA"); appData != nullptr && *appData != '\0')
    {
        return std::filesystem::path(appData) / "ACRD" / "ProjectPredation";
    }
#else
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
    {
        return std::filesystem::path(home) / ".local" / "share" / "ProjectPredation";
    }
#endif
    return executableDir / "Saved";
}

} // namespace

void Paths::Init(const char* argv0, const std::filesystem::path& assetsOverride)
{
    PathState& state = State();
    state.executableDir = FindExecutableDir(argv0);
    state.assetsRoot = assetsOverride.empty() ? DiscoverAssetsRoot(state.executableDir)
                                              : std::filesystem::absolute(assetsOverride);
    state.userDataDir = DiscoverUserDataDir(state.executableDir);

    std::error_code ec;
    std::filesystem::create_directories(state.userDataDir, ec);

    state.searchRoots.clear();
    state.searchRoots.push_back(state.executableDir / "Assets");
#ifdef PRED_GENERATED_ASSETS_DIR
    state.searchRoots.emplace_back(PRED_GENERATED_ASSETS_DIR);
#endif
    state.searchRoots.push_back(state.assetsRoot);
}

const std::filesystem::path& Paths::ExecutableDir()
{
    return State().executableDir;
}

const std::filesystem::path& Paths::AssetsRoot()
{
    return State().assetsRoot;
}

const std::filesystem::path& Paths::UserDataDir()
{
    return State().userDataDir;
}

const std::vector<std::filesystem::path>& Paths::SearchRoots()
{
    return State().searchRoots;
}

void Paths::AddSearchRoot(const std::filesystem::path& root)
{
    State().searchRoots.insert(State().searchRoots.begin(), root);
}

std::optional<std::filesystem::path> Paths::Resolve(const std::filesystem::path& relative)
{
    std::error_code ec;
    if (relative.is_absolute() && std::filesystem::is_regular_file(relative, ec))
    {
        return relative;
    }
    for (const auto& root : State().searchRoots)
    {
        const auto candidate = root / relative;
        if (std::filesystem::is_regular_file(candidate, ec))
        {
            return candidate;
        }
    }
    return std::nullopt;
}

std::string Paths::Describe()
{
    const PathState& state = State();
    std::ostringstream out;
    out << "executable=" << state.executableDir.string() << " assets=" << state.assetsRoot.string()
        << " user=" << state.userDataDir.string();
    return out.str();
}

} // namespace pred
