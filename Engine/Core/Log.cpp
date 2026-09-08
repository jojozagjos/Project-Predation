#include "Engine/Core/Log.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

namespace pred
{
namespace
{

constexpr std::array<const char*, static_cast<size_t>(LogCategory::Count)> kCategoryNames = {
    "ENGINE", "PLATFORM", "RENDER", "AUDIO", "NETWORK", "AI", "ANIMATION", "PHYSICS", "GAMEPLAY", "ASSET", "DEBUG"};

constexpr const char* kPattern = "[%H:%M:%S.%e] [%^%-9n%$] [%l] %v";

struct LogState
{
    std::array<std::shared_ptr<spdlog::logger>, static_cast<size_t>(LogCategory::Count)> loggers;
    std::vector<spdlog::sink_ptr> sinks;
    std::mutex mutex;
    bool initialized = false;
};

LogState& State()
{
    static LogState state;
    return state;
}

std::shared_ptr<spdlog::logger> MakeLogger(size_t index, const std::vector<spdlog::sink_ptr>& sinks,
                                           spdlog::level::level_enum level)
{
    auto logger = std::make_shared<spdlog::logger>(kCategoryNames[index], sinks.begin(), sinks.end());
    logger->set_level(level);
    logger->set_pattern(kPattern);
    logger->flush_on(spdlog::level::warn);
    return logger;
}

} // namespace

const char* LogCategoryName(LogCategory category)
{
    const auto index = static_cast<size_t>(category);
    return index < kCategoryNames.size() ? kCategoryNames[index] : "UNKNOWN";
}

bool ParseLogCategory(std::string_view name, LogCategory& out)
{
    for (size_t i = 0; i < kCategoryNames.size(); ++i)
    {
        const std::string_view candidate = kCategoryNames[i];
        if (candidate.size() != name.size())
        {
            continue;
        }
        bool equal = true;
        for (size_t c = 0; c < name.size(); ++c)
        {
            if (std::toupper(static_cast<unsigned char>(name[c])) != candidate[c])
            {
                equal = false;
                break;
            }
        }
        if (equal)
        {
            out = static_cast<LogCategory>(i);
            return true;
        }
    }
    return false;
}

void Log::Init(const InitOptions& options)
{
    LogState& state = State();
    std::lock_guard lock(state.mutex);

    state.sinks.clear();
    if (options.consoleOutput)
    {
        state.sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }
    if (!options.logFile.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(options.logFile.parent_path(), ec);
        try
        {
            state.sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(options.logFile.string(), true));
        }
        catch (const spdlog::spdlog_ex& ex)
        {
            std::fprintf(stderr, "Failed to open log file '%s': %s\n", options.logFile.string().c_str(), ex.what());
        }
    }

    for (size_t i = 0; i < state.loggers.size(); ++i)
    {
        state.loggers[i] = MakeLogger(i, state.sinks, options.level);
    }
    state.initialized = true;
}

void Log::Shutdown()
{
    LogState& state = State();
    std::lock_guard lock(state.mutex);
    for (auto& logger : state.loggers)
    {
        if (logger)
        {
            logger->flush();
        }
        logger.reset();
    }
    state.sinks.clear();
    state.initialized = false;
}

bool Log::IsInitialized()
{
    return State().initialized;
}

spdlog::logger& Log::Get(LogCategory category)
{
    LogState& state = State();
    const auto index = std::min(static_cast<size_t>(category), state.loggers.size() - 1);
    if (!state.loggers[index])
    {
        // Logging before Init (static initializers, unit tests): create a console-only logger on demand.
        std::lock_guard lock(state.mutex);
        if (!state.loggers[index])
        {
            std::vector<spdlog::sink_ptr> sinks{std::make_shared<spdlog::sinks::stdout_color_sink_mt>()};
            state.loggers[index] = MakeLogger(index, sinks, spdlog::level::trace);
        }
    }
    return *state.loggers[index];
}

void Log::SetLevel(LogCategory category, spdlog::level::level_enum level)
{
    Get(category).set_level(level);
}

void Log::SetGlobalLevel(spdlog::level::level_enum level)
{
    for (size_t i = 0; i < static_cast<size_t>(LogCategory::Count); ++i)
    {
        Get(static_cast<LogCategory>(i)).set_level(level);
    }
}

void Log::Flush()
{
    for (size_t i = 0; i < static_cast<size_t>(LogCategory::Count); ++i)
    {
        Get(static_cast<LogCategory>(i)).flush();
    }
}

void Log::AddSink(const spdlog::sink_ptr& sink)
{
    LogState& state = State();
    std::lock_guard lock(state.mutex);
    state.sinks.push_back(sink);
    for (auto& logger : state.loggers)
    {
        if (logger)
        {
            logger->sinks().push_back(sink);
        }
    }
}

void Log::RemoveSink(const spdlog::sink_ptr& sink)
{
    LogState& state = State();
    std::lock_guard lock(state.mutex);
    std::erase(state.sinks, sink);
    for (auto& logger : state.loggers)
    {
        if (logger)
        {
            std::erase(logger->sinks(), sink);
        }
    }
}

} // namespace pred
