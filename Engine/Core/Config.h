#pragma once

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <string_view>

namespace pred
{

// Layered configuration on top of the cvar registry.
//
// Layers, lowest to highest priority:
//   1. code defaults (the value passed to the CVar constructor)
//   2. Assets/Config/defaults.json      (project defaults, committed)
//   3. <user data>/settings.json         (per-user, written by SaveArchive)
//   4. command line  (--set name=value or +name value)
//   5. console at runtime
//
// JSON files are nested objects; keys are joined with '.' to form cvar names:
//   { "r": { "vsync": true } }  ->  r.vsync = true
class Config
{
public:
    struct LoadResult
    {
        bool fileFound = false;
        int applied = 0;
        int pending = 0; // cvar not registered yet; value is applied when it registers
        int errors = 0;  // parse errors or read-only violations
    };

    static LoadResult LoadFile(const std::filesystem::path& file);
    static LoadResult ApplyJson(const nlohmann::json& json, std::string_view sourceName);
    static bool ApplyOverride(std::string_view name, std::string_view value, std::string_view sourceName);

    // Writes every Archive-flagged cvar to the file as nested JSON.
    static bool SaveArchive(const std::filesystem::path& file);
    static nlohmann::json ArchiveToJson();
};

} // namespace pred
