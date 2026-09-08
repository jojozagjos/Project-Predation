#include "Engine/Core/Config.h"

#include "Engine/Core/CVar.h"
#include "Engine/Core/Log.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <functional>
#include <string>

namespace pred
{
namespace
{

void FlattenInto(const nlohmann::json& node, const std::string& prefix, Config::LoadResult& result,
                 std::string_view sourceName)
{
    if (!node.is_object())
    {
        return;
    }
    for (const auto& [key, value] : node.items())
    {
        const std::string name = prefix.empty() ? key : prefix + "." + key;
        if (value.is_object())
        {
            FlattenInto(value, name, result, sourceName);
            continue;
        }

        std::string text;
        if (value.is_string())
        {
            text = value.get<std::string>();
        }
        else if (value.is_boolean())
        {
            text = value.get<bool>() ? "true" : "false";
        }
        else if (value.is_number())
        {
            text = value.dump();
        }
        else
        {
            PRED_LOG_WARN(Engine, "Config '{}': key '{}' has unsupported JSON type, ignored", sourceName, name);
            ++result.errors;
            continue;
        }

        if (Config::ApplyOverride(name, text, sourceName))
        {
            if (CVarRegistry::Instance().Find(name) != nullptr)
            {
                ++result.applied;
            }
            else
            {
                ++result.pending;
            }
        }
        else
        {
            ++result.errors;
        }
    }
}

nlohmann::json ValueToJson(const CVarBase& var)
{
    const std::string text = var.GetString();
    switch (var.Type())
    {
    case CVarType::Bool:
        return text == "true";
    case CVarType::Int:
        return std::stoi(text);
    case CVarType::Float:
        return std::stod(text);
    case CVarType::String:
        return text;
    }
    return text;
}

} // namespace

Config::LoadResult Config::LoadFile(const std::filesystem::path& file)
{
    LoadResult result;
    std::ifstream stream(file);
    if (!stream.is_open())
    {
        PRED_LOG_INFO(Engine, "Config file not found: {}", file.string());
        return result;
    }
    result.fileFound = true;

    const nlohmann::json json = nlohmann::json::parse(stream, nullptr, false, true);
    if (json.is_discarded())
    {
        PRED_LOG_ERROR(Engine, "Config file has invalid JSON: {}", file.string());
        ++result.errors;
        return result;
    }

    const LoadResult applied = ApplyJson(json, file.filename().string());
    result.applied = applied.applied;
    result.pending = applied.pending;
    result.errors += applied.errors;
    PRED_LOG_INFO(Engine, "Loaded config {} ({} applied, {} pending, {} errors)", file.string(), result.applied,
                  result.pending, result.errors);
    return result;
}

Config::LoadResult Config::ApplyJson(const nlohmann::json& json, std::string_view sourceName)
{
    LoadResult result;
    result.fileFound = true;
    if (!json.is_object())
    {
        PRED_LOG_ERROR(Engine, "Config '{}': root must be a JSON object", sourceName);
        ++result.errors;
        return result;
    }
    FlattenInto(json, "", result, sourceName);
    return result;
}

bool Config::ApplyOverride(std::string_view name, std::string_view value, std::string_view sourceName)
{
    switch (CVarRegistry::Instance().Set(name, value))
    {
    case CVarRegistry::SetResult::Applied:
        PRED_LOG_TRACE(Engine, "Config '{}': {} = {}", sourceName, name, value);
        return true;
    case CVarRegistry::SetResult::Pending:
        PRED_LOG_TRACE(Engine, "Config '{}': {} = {} (pending registration)", sourceName, name, value);
        return true;
    case CVarRegistry::SetResult::ParseError:
        PRED_LOG_WARN(Engine, "Config '{}': cannot parse '{}' for cvar '{}'", sourceName, value, name);
        return false;
    case CVarRegistry::SetResult::ReadOnly:
        PRED_LOG_WARN(Engine, "Config '{}': cvar '{}' is read-only", sourceName, name);
        return false;
    }
    return false;
}

nlohmann::json Config::ArchiveToJson()
{
    nlohmann::json root = nlohmann::json::object();
    for (const CVarBase* var : CVarRegistry::Instance().All())
    {
        if (!var->HasFlag(CVarFlags::Archive))
        {
            continue;
        }
        nlohmann::json* node = &root;
        std::string remaining = var->Name();
        size_t dot = remaining.find('.');
        while (dot != std::string::npos)
        {
            node = &((*node)[remaining.substr(0, dot)]);
            remaining = remaining.substr(dot + 1);
            dot = remaining.find('.');
        }
        (*node)[remaining] = ValueToJson(*var);
    }
    return root;
}

bool Config::SaveArchive(const std::filesystem::path& file)
{
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream stream(file);
    if (!stream.is_open())
    {
        PRED_LOG_ERROR(Engine, "Cannot write settings file: {}", file.string());
        return false;
    }
    stream << ArchiveToJson().dump(2) << '\n';
    PRED_LOG_INFO(Engine, "Saved settings to {}", file.string());
    return true;
}

} // namespace pred
