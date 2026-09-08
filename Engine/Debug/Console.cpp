#include "Engine/Debug/Console.h"

#include "Engine/Core/CVar.h"
#include "Engine/Core/Log.h"

#include <imgui.h>
#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <cstring>

namespace pred
{
namespace
{

uint32_t ColorForLevel(spdlog::level::level_enum level)
{
    switch (level)
    {
    case spdlog::level::trace:
        return 0xff707070;
    case spdlog::level::debug:
        return Console::kColorMuted;
    case spdlog::level::info:
        return Console::kColorDefault;
    case spdlog::level::warn:
        return Console::kColorWarning;
    case spdlog::level::err:
        return Console::kColorError;
    case spdlog::level::critical:
        return 0xffff50ff;
    default:
        return Console::kColorDefault;
    }
}

class ConsoleLogSink final : public spdlog::sinks::base_sink<std::mutex>
{
public:
    explicit ConsoleLogSink(Console& console) : m_console(console) {}

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        spdlog::memory_buf_t formatted;
        base_sink<std::mutex>::formatter_->format(msg, formatted);
        std::string text(formatted.data(), formatted.size());
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        {
            text.pop_back();
        }
        m_console.Print(std::move(text), ColorForLevel(msg.level));
    }

    void flush_() override {}

private:
    Console& m_console;
};

std::string JoinFrom(const std::vector<std::string>& args, size_t start)
{
    std::string result;
    for (size_t i = start; i < args.size(); ++i)
    {
        if (i > start)
        {
            result += ' ';
        }
        result += args[i];
    }
    return result;
}

} // namespace

void Console::Init()
{
    RegisterBuiltins();
}

void Console::Shutdown()
{
    DetachLogSink();
    m_commands.clear();
}

void Console::RegisterCommand(std::string name, std::string description, CommandFn fn, std::string usage)
{
    if (m_commands.contains(name))
    {
        PRED_LOG_WARN(Debug, "Console command '{}' re-registered", name);
    }
    m_commands[std::move(name)] = Command{std::move(description), std::move(usage), std::move(fn)};
}

bool Console::UnregisterCommand(const std::string& name)
{
    return m_commands.erase(name) > 0;
}

bool Console::HasCommand(std::string_view name) const
{
    return m_commands.contains(std::string(name));
}

std::vector<Console::CommandInfo> Console::Commands() const
{
    std::vector<CommandInfo> result;
    result.reserve(m_commands.size());
    for (const auto& [name, command] : m_commands)
    {
        result.push_back({name, command.description, command.usage});
    }
    std::sort(result.begin(), result.end(), [](const CommandInfo& a, const CommandInfo& b) { return a.name < b.name; });
    return result;
}

std::vector<std::string> Console::Tokenize(std::string_view line)
{
    std::vector<std::string> tokens;
    std::string current;
    bool inQuotes = false;
    bool hasToken = false;
    for (const char c : line)
    {
        if (c == '"')
        {
            inQuotes = !inQuotes;
            hasToken = true;
            continue;
        }
        if (!inQuotes && (c == ' ' || c == '\t'))
        {
            if (hasToken)
            {
                tokens.push_back(current);
                current.clear();
                hasToken = false;
            }
            continue;
        }
        current += c;
        hasToken = true;
    }
    if (hasToken)
    {
        tokens.push_back(current);
    }
    return tokens;
}

void Console::Execute(std::string_view line)
{
    const std::vector<std::string> args = Tokenize(line);
    if (args.empty())
    {
        return;
    }

    Print("> " + std::string(line), kColorCommand);
    if (m_history.empty() || m_history.back() != line)
    {
        m_history.emplace_back(line);
    }
    m_historyPos = -1;

    const std::string& name = args[0];
    if (const auto it = m_commands.find(name); it != m_commands.end())
    {
        it->second.fn(args);
        return;
    }

    if (CVarBase* var = CVarRegistry::Instance().Find(name); var != nullptr)
    {
        if (args.size() == 1)
        {
            Print(var->Name() + " = " + var->GetString() + "  (" + CVarTypeName(var->Type()) +
                  ", default " + var->GetDefaultString() + ") " + var->Description());
        }
        else if (var->HasFlag(CVarFlags::ReadOnly))
        {
            PrintError(var->Name() + " is read-only");
        }
        else if (var->SetFromString(JoinFrom(args, 1)))
        {
            Print(var->Name() + " = " + var->GetString());
        }
        else
        {
            PrintError("Cannot parse '" + JoinFrom(args, 1) + "' as " + CVarTypeName(var->Type()));
        }
        return;
    }

    PrintError("Unknown command or cvar: " + name + "  (try 'help')");
}

void Console::Print(std::string text, uint32_t colorABGR)
{
    std::lock_guard lock(m_linesMutex);
    m_lines.push_back({std::move(text), colorABGR});
    while (m_lines.size() > m_maxLines)
    {
        m_lines.pop_front();
    }
    m_scrollToBottom = true;
}

void Console::Clear()
{
    std::lock_guard lock(m_linesMutex);
    m_lines.clear();
}

void Console::SetOpen(bool open)
{
    if (m_open == open)
    {
        return;
    }
    m_open = open;
    if (open)
    {
        m_reclaimFocus = true;
        m_scrollToBottom = true;
    }
}

void Console::AttachLogSink()
{
    if (m_logSink)
    {
        return;
    }
    auto sink = std::make_shared<ConsoleLogSink>(*this);
    sink->set_pattern("[%H:%M:%S] [%n] %v");
    m_logSink = sink;
    Log::AddSink(m_logSink);
}

void Console::DetachLogSink()
{
    if (m_logSink)
    {
        Log::RemoveSink(m_logSink);
        m_logSink.reset();
    }
}

void Console::Draw(int displayWidth, int displayHeight)
{
    if (!m_open)
    {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(displayWidth), static_cast<float>(displayHeight) * 0.45f));
    ImGui::SetNextWindowBgAlpha(0.94f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoScrollbar;
    if (!ImGui::Begin("##PredationConsole", nullptr, flags))
    {
        ImGui::End();
        return;
    }

    const float footerHeight = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginChild("##ConsoleScroll", ImVec2(0.0f, -footerHeight), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar))
    {
        std::lock_guard lock(m_linesMutex);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_lines.size()));
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const Line& line = m_lines[static_cast<size_t>(i)];
                ImGui::PushStyleColor(ImGuiCol_Text, line.color);
                ImGui::TextUnformatted(line.text.c_str());
                ImGui::PopStyleColor();
            }
        }
        if (m_scrollToBottom || ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        {
            ImGui::SetScrollHereY(1.0f);
        }
        m_scrollToBottom = false;
    }
    ImGui::EndChild();
    ImGui::Separator();

    const ImGuiInputTextFlags inputFlags = ImGuiInputTextFlags_EnterReturnsTrue |
                                           ImGuiInputTextFlags_CallbackCompletion |
                                           ImGuiInputTextFlags_CallbackHistory;
    ImGui::PushItemWidth(-1.0f);
    if (ImGui::InputText("##ConsoleInput", m_inputBuffer, sizeof(m_inputBuffer), inputFlags,
                         &Console::TextEditCallbackStub, this))
    {
        Execute(m_inputBuffer);
        m_inputBuffer[0] = '\0';
        m_reclaimFocus = true;
    }
    ImGui::PopItemWidth();
    ImGui::SetItemDefaultFocus();
    if (m_reclaimFocus)
    {
        ImGui::SetKeyboardFocusHere(-1);
        m_reclaimFocus = false;
    }

    ImGui::End();
}

int Console::TextEditCallbackStub(ImGuiInputTextCallbackData* data)
{
    return static_cast<Console*>(data->UserData)->TextEditCallback(data);
}

int Console::TextEditCallback(ImGuiInputTextCallbackData* data)
{
    switch (data->EventFlag)
    {
    case ImGuiInputTextFlags_CallbackCompletion:
    {
        // Complete the word under the cursor against commands and cvars.
        const char* wordEnd = data->Buf + data->CursorPos;
        const char* wordStart = wordEnd;
        while (wordStart > data->Buf && wordStart[-1] != ' ')
        {
            --wordStart;
        }
        const std::string_view prefix(wordStart, static_cast<size_t>(wordEnd - wordStart));

        std::vector<std::string> candidates;
        for (const auto& [name, command] : m_commands)
        {
            if (name.compare(0, prefix.size(), prefix) == 0)
            {
                candidates.push_back(name);
            }
        }
        for (const CVarBase* var : CVarRegistry::Instance().All())
        {
            if (var->Name().compare(0, prefix.size(), prefix) == 0)
            {
                candidates.push_back(var->Name());
            }
        }
        std::sort(candidates.begin(), candidates.end());

        if (candidates.empty())
        {
            Print("No match for '" + std::string(prefix) + "'", kColorMuted);
        }
        else if (candidates.size() == 1)
        {
            data->DeleteChars(static_cast<int>(wordStart - data->Buf), static_cast<int>(wordEnd - wordStart));
            data->InsertChars(data->CursorPos, candidates[0].c_str());
            data->InsertChars(data->CursorPos, " ");
        }
        else
        {
            // Complete to the longest common prefix and list the options.
            size_t common = candidates[0].size();
            for (const std::string& candidate : candidates)
            {
                size_t i = 0;
                while (i < common && i < candidate.size() && candidate[i] == candidates[0][i])
                {
                    ++i;
                }
                common = i;
            }
            if (common > prefix.size())
            {
                data->DeleteChars(static_cast<int>(wordStart - data->Buf), static_cast<int>(wordEnd - wordStart));
                data->InsertChars(data->CursorPos, candidates[0].substr(0, common).c_str());
            }
            std::string list = "Matches:";
            for (const std::string& candidate : candidates)
            {
                list += ' ';
                list += candidate;
            }
            Print(list, kColorMuted);
        }
        break;
    }
    case ImGuiInputTextFlags_CallbackHistory:
    {
        const int previous = m_historyPos;
        if (data->EventKey == ImGuiKey_UpArrow)
        {
            if (m_historyPos == -1)
            {
                m_historyPos = static_cast<int>(m_history.size()) - 1;
            }
            else if (m_historyPos > 0)
            {
                --m_historyPos;
            }
        }
        else if (data->EventKey == ImGuiKey_DownArrow)
        {
            if (m_historyPos != -1 && ++m_historyPos >= static_cast<int>(m_history.size()))
            {
                m_historyPos = -1;
            }
        }
        if (previous != m_historyPos)
        {
            const char* text = m_historyPos >= 0 ? m_history[static_cast<size_t>(m_historyPos)].c_str() : "";
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, text);
        }
        break;
    }
    default:
        break;
    }
    return 0;
}

void Console::RegisterBuiltins()
{
    RegisterCommand(
        "help", "List commands, or describe one command",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() > 1)
            {
                const auto it = m_commands.find(args[1]);
                if (it == m_commands.end())
                {
                    PrintError("No such command: " + args[1]);
                    return;
                }
                Print(args[1] + " - " + it->second.description);
                if (!it->second.usage.empty())
                {
                    Print("  usage: " + it->second.usage, kColorMuted);
                }
                return;
            }
            Print("Commands:", kColorMuted);
            for (const CommandInfo& info : Commands())
            {
                Print("  " + info.name + " - " + info.description);
            }
            Print("Type a cvar name to read it, or '<cvar> <value>' to set it. Tab completes.", kColorMuted);
        },
        "help [command]");

    RegisterCommand("clear", "Clear the console output", [this](const std::vector<std::string>&) { Clear(); });

    RegisterCommand(
        "cvars", "List cvars, optionally filtered by prefix",
        [this](const std::vector<std::string>& args)
        {
            const std::string filter = args.size() > 1 ? args[1] : "";
            int shown = 0;
            for (const CVarBase* var : CVarRegistry::Instance().All())
            {
                if (var->HasFlag(CVarFlags::Hidden) || var->Name().compare(0, filter.size(), filter) != 0)
                {
                    continue;
                }
                std::string flags;
                if (var->HasFlag(CVarFlags::Archive))
                {
                    flags += " [archive]";
                }
                if (var->HasFlag(CVarFlags::ReadOnly))
                {
                    flags += " [readonly]";
                }
                Print("  " + var->Name() + " = " + var->GetString() + flags + "  " + var->Description(),
                      var->IsDefault() ? kColorDefault : kColorCommand);
                ++shown;
            }
            Print(std::to_string(shown) + " cvars", kColorMuted);
        },
        "cvars [prefix]");

    RegisterCommand(
        "set", "Set a cvar value",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 3)
            {
                PrintError("usage: set <cvar> <value>");
                return;
            }
            Execute(args[1] + " " + JoinFrom(args, 2));
        },
        "set <cvar> <value>");

    RegisterCommand(
        "get", "Print a cvar value",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 2)
            {
                PrintError("usage: get <cvar>");
                return;
            }
            Execute(args[1]);
        },
        "get <cvar>");

    RegisterCommand(
        "reset", "Reset a cvar to its default",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 2)
            {
                PrintError("usage: reset <cvar>");
                return;
            }
            CVarBase* var = CVarRegistry::Instance().Find(args[1]);
            if (var == nullptr)
            {
                PrintError("No such cvar: " + args[1]);
                return;
            }
            var->ResetToDefault();
            Print(var->Name() + " = " + var->GetString());
        },
        "reset <cvar>");

    RegisterCommand(
        "log_level", "Set log level for a category or 'all'",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 3)
            {
                PrintError("usage: log_level <category|all> <trace|debug|info|warn|error|critical|off>");
                return;
            }
            const spdlog::level::level_enum level = spdlog::level::from_str(args[2]);
            if (level == spdlog::level::off && args[2] != "off")
            {
                PrintError("Unknown log level: " + args[2]);
                return;
            }
            if (args[1] == "all")
            {
                Log::SetGlobalLevel(level);
                Print("All log categories set to " + args[2]);
                return;
            }
            LogCategory category;
            if (!ParseLogCategory(args[1], category))
            {
                PrintError("Unknown log category: " + args[1]);
                return;
            }
            Log::SetLevel(category, level);
            Print(std::string(LogCategoryName(category)) + " log level set to " + args[2]);
        },
        "log_level <category|all> <level>");

    RegisterCommand(
        "echo", "Print text", [this](const std::vector<std::string>& args) { Print(JoinFrom(args, 1)); },
        "echo <text>");
}

} // namespace pred
