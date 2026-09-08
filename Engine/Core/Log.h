#pragma once

#include <spdlog/spdlog.h>

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace pred
{

// Every log line belongs to a category so output can be filtered per system.
enum class LogCategory : uint8_t
{
    Engine,
    Platform,
    Render,
    Audio,
    Network,
    AI,
    Animation,
    Physics,
    Gameplay,
    Asset,
    Debug,
    Count
};

const char* LogCategoryName(LogCategory category);
bool ParseLogCategory(std::string_view name, LogCategory& out);

class Log
{
public:
    struct InitOptions
    {
        std::filesystem::path logFile; // empty = no file sink
        bool consoleOutput = true;
        spdlog::level::level_enum level = spdlog::level::trace;
    };

    static void Init(const InitOptions& options);
    static void Shutdown();
    static bool IsInitialized();

    static spdlog::logger& Get(LogCategory category);
    static void SetLevel(LogCategory category, spdlog::level::level_enum level);
    static void SetGlobalLevel(spdlog::level::level_enum level);
    static void Flush();

    // Extra sinks receive every category. Used by the in-game console.
    static void AddSink(const spdlog::sink_ptr& sink);
    static void RemoveSink(const spdlog::sink_ptr& sink);
};

} // namespace pred

#define PRED_LOG_TRACE(category, ...) ::pred::Log::Get(::pred::LogCategory::category).trace(__VA_ARGS__)
#define PRED_LOG_DEBUG(category, ...) ::pred::Log::Get(::pred::LogCategory::category).debug(__VA_ARGS__)
#define PRED_LOG_INFO(category, ...) ::pred::Log::Get(::pred::LogCategory::category).info(__VA_ARGS__)
#define PRED_LOG_WARN(category, ...) ::pred::Log::Get(::pred::LogCategory::category).warn(__VA_ARGS__)
#define PRED_LOG_ERROR(category, ...) ::pred::Log::Get(::pred::LogCategory::category).error(__VA_ARGS__)
#define PRED_LOG_CRITICAL(category, ...) ::pred::Log::Get(::pred::LogCategory::category).critical(__VA_ARGS__)
