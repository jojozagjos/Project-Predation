#pragma once

#include <spdlog/spdlog.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct ImGuiInputTextCallbackData;

namespace pred
{

// Developer console: command registry, cvar access, log mirror, history, and
// tab completion. Opened with the grave key by default (see input.json).
class Console
{
public:
    using CommandFn = std::function<void(const std::vector<std::string>& args)>;
    // What could be typed for a command's argument (1: the first after its name), for the list under the input.
    using Completer = std::function<std::vector<std::string>(size_t argument)>;

    struct CommandInfo
    {
        std::string name;
        std::string description;
        std::string usage;
    };

    static constexpr uint32_t kColorDefault = 0xffe6e6e6;
    static constexpr uint32_t kColorCommand = 0xff8ce6a0;
    static constexpr uint32_t kColorWarning = 0xff5ad2f0;
    static constexpr uint32_t kColorError = 0xff5a5af0;
    static constexpr uint32_t kColorMuted = 0xff8c8c8c;

    void Init();
    void Shutdown();

    void RegisterCommand(std::string name, std::string description, CommandFn fn, std::string usage = "");
    bool UnregisterCommand(const std::string& name);
    // The values a command's arguments can take, offered as they are typed.
    void SetCompleter(const std::string& command, Completer completer);
    bool HasCommand(std::string_view name) const;
    std::vector<CommandInfo> Commands() const; // sorted by name

    // Runs a command line: "<command> args..." or "<cvar> [value]".
    void Execute(std::string_view line);

    void Print(std::string text, uint32_t colorABGR = kColorDefault);
    void PrintWarning(std::string text) { Print(std::move(text), kColorWarning); }
    void PrintError(std::string text) { Print(std::move(text), kColorError); }
    void Clear();

    void Draw(int displayWidth, int displayHeight);
    bool IsOpen() const { return m_open; }
    void SetOpen(bool open);
    void Toggle() { SetOpen(!m_open); }

    // Mirrors every log line into the console.
    void AttachLogSink();
    void DetachLogSink();

    static std::vector<std::string> Tokenize(std::string_view line);

private:
    struct Line
    {
        std::string text;
        uint32_t color;
    };
    struct Command
    {
        std::string description;
        std::string usage;
        CommandFn fn;
    };

    // The live list of what could be typed, drawn under the input as it is typed.
    void DrawSuggestions(float x, float y, float width);
    // Everything that fits the word being typed (a command or cvar, or an argument by the command's Completer): those
    // that start with it first, then those with it anywhere; where that word starts in the line, and the command's usage
    // once its name is typed.
    std::vector<CommandInfo> Suggestions(size_t& wordStart, std::string& usage) const;
    void RegisterBuiltins();
    int TextEditCallback(ImGuiInputTextCallbackData* data);
    static int TextEditCallbackStub(ImGuiInputTextCallbackData* data);

    std::unordered_map<std::string, Command> m_commands;
    std::unordered_map<std::string, Completer> m_completers;
    // What the list under the input shows (for Tab and the arrow keys), and which of it is picked (-1: none).
    std::vector<std::string> m_shown;
    int m_picked = -1;
    std::deque<Line> m_lines;
    mutable std::mutex m_linesMutex;
    size_t m_maxLines = 4000;

    std::vector<std::string> m_history;
    int m_historyPos = -1;
    char m_inputBuffer[1024] = {};

    bool m_open = false;
    bool m_scrollToBottom = false;
    bool m_reclaimFocus = false;

    spdlog::sink_ptr m_logSink;
};

} // namespace pred
