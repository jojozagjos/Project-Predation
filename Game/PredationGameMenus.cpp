#include "Game/PredationGame.h"
#include "Game/OverlayLayer.h"
#include "Game/PredationGameShared.h"

#include "Engine/Render/DepthConvention.h"
#include "Game/Creature/CreatureTuning.h"

#include "Engine/Core/BuildInfo.h"
#include "Engine/Core/CVar.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Engine/Debug/DebugCategories.h"
#include "Engine/Assets/GltfImport.h"
#include "Engine/Debug/ImGuiLayer.h"
#include "Engine/Render/DebugDraw.h"
#include "Engine/Render/Primitives.h"
#include "Game/Weapons/WeaponAppearance.h"
#include "Game/World/TestMap.h"
#include "Game/World/KestrelStation.h"
#include "Game/World/Surfaces.h"
#include "Engine/Audio/Sound.h"
#include "Engine/Debug/FrameStats.h"
#include "Engine/Platform/Window.h"
#include "Engine/Audio/SoundDesign.h"

#include <SDL3/SDL_events.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <limits>
#include <string>


// The menus: title, pause, settings, key bindings, the lobby's screens and the boot cards.

namespace pred
{

namespace
{

// Puts every setting back the way it shipped.
//
// Only the archived ones, which are the ones a settings screen can reach. The console can set a
// great many more and those are not the player's to lose: a developer who has turned occlusion off
// to look at something has not asked for their graphics settings to be restored.
void ResetSettingsToDefaults()
{
    for (CVarBase* cvar : CVarRegistry::Instance().All())
    {
        if (cvar != nullptr && cvar->HasFlag(CVarFlags::Archive) && !cvar->HasFlag(CVarFlags::ReadOnly))
        {
            cvar->ResetToDefault();
        }
    }
}

} // namespace

namespace
{

// A settings row: the name on the left, the control filling the right, in a two-column table so every
// control in a tab starts at the same place. A list of controls each with its own label trailing off
// its right-hand end reads as a form nobody laid out.
bool BeginSettingsTable(const char* id)
{
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX))
    {
        return false;
    }
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.42f);
    ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthStretch, 0.58f);
    return true;
}

// A setting's row: its name, and -- when it has been changed -- a button beside the name that puts that one setting back
// as it shipped. True the frame it is put back, for a setting that has to be applied as well as stored.
bool SettingsRow(const char* name, const char* setting = nullptr)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(name);
    bool reset = false;
    if (CVarBase* cvar = setting != nullptr ? CVarRegistry::Instance().Find(setting) : nullptr; cvar != nullptr && !cvar->IsDefault())
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("Default"))
        {
            SetSetting(setting, cvar->GetDefaultString());
            reset = true;
        }
        ImGui::SetItemTooltip("Put this back to %s", cvar->GetDefaultString().c_str());
    }
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-1.0f);
    return reset;
}

struct QualityPreset
{
    const char* name;
    float shadowDistance;
    float grass;   // how thick the grass is drawn (r.grass)
    float weather; // how much of what falls is drawn (r.weather)
    bool reflections;
    float reflectionDistance;
    int msaa;
    float ambientOcclusion; // r.ao (only without multisampling, which it cannot read the depth of)
};

// What each word means, in the settings it sets. Anything that does not match one of these exactly is
// Custom. None of them takes the light's shadows away -- the sun's, the torch's, a roof keeping the sky out of a room:
// they are what the game looks like, not a cost to save, and those stay on at every quality. What scales is how far the
// sun's shadows reach, how much grass and weather is drawn, mirrors and edges.
constexpr QualityPreset kPresets[] = {
    {"Low", 40.0f, 0.45f, 0.5f, false, 10.0f, 0, 0.0f},
    {"Medium", 52.0f, 0.7f, 0.75f, true, 18.0f, 0, 0.7f},
    {"High", 64.0f, 1.0f, 1.0f, true, 30.0f, 0, 0.7f},
    {"Ultra", 96.0f, 1.3f, 1.0f, true, 50.0f, 0, 0.7f},
};

int GetSettingInt(const char* name, int fallback)
{
    return static_cast<int>(GetSettingFloat(name, static_cast<float>(fallback)));
}

} // namespace

namespace
{

// Any key or button, this frame: what "press any key" waits for.
bool AnyKeyPressed()
{
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key)
    {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false))
        {
            return true;
        }
    }
    return ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
}

} // namespace

void PredationGame::DrawTitleBrowse()
{
    const ImVec2 wide{-1.0f, 34.0f};
    StartBrowsing();

    // Joining with a code, first: it is what most people came to this screen to do.
    ImGui::TextUnformatted("Got a code from a friend?");
    const float buttons = 64.0f * 2.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    ImGui::SetNextItemWidth(-buttons);
    const bool entered = ImGui::InputTextWithHint("##joinwhatever", "type the code here", m_joinInput,
                                                  sizeof(m_joinInput), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Paste", {64.0f, 0.0f}))
    {
        if (const char* clip = ImGui::GetClipboardText(); clip != nullptr)
        {
            // Trimmed, because a code copied out of a chat app usually brings a space or a line
            // break with it.
            std::string text = clip;
            text.erase(0, text.find_first_not_of(" \t\r\n"));
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            std::snprintf(m_joinInput, sizeof(m_joinInput), "%s", text.c_str());
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m_joinInput[0] == '\0');
    if (ImGui::Button("Join", {64.0f, 0.0f}) || (entered && m_joinInput[0] != '\0'))
    {
        ImGui::EndDisabled();
        JoinTyped(m_joinInput);
        return;
    }
    ImGui::EndDisabled();
    ImGui::Spacing();

    // The games on this network, which announce themselves.
    ImGui::TextDisabled("ON YOUR NETWORK");

    // A fixed-height list, so the window does not jump about every time somebody opens or closes a
    // game while it is being read.
    int shown = 0;
    ImGui::BeginChild("##games", {0.0f, 150.0f}, true);
    const auto row = [&](const std::string& name, int players, int most, bool started, bool joinable,
                         const char* why)
    {
        ImGui::BeginDisabled(!joinable);
        const bool join = ImGui::Button("Join", {64.0f, 0.0f});
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(name.empty() ? "a game" : name.c_str());
        ImGui::SameLine(0.0f, 12.0f);
        ImGui::TextDisabled("%d/%d", players, most);
        ImGui::SameLine(0.0f, 12.0f);
        if (why != nullptr)
        {
            ImGui::TextColored(kWarning, "%s", why);
        }
        else
        {
            ImGui::TextDisabled("%s", started ? "in progress" : "in the lobby");
        }
        ++shown;
        return join;
    };

    {
        const std::vector<LanLobby>& found = m_browser.Lobbies();
        for (size_t i = 0; i < found.size(); ++i)
        {
            const LanLobby& lobby = found[i];
            ImGui::PushID(static_cast<int>(i));
            // A different build is listed and greyed rather than hidden: "their game says it is a
            // different version" says what to do about it, and "it is not showing up" does not.
            const bool sameBuild = lobby.protocol == kProtocolVersion;
            const bool full = lobby.players >= lobby.maxPlayers;
            const bool join = row(lobby.name, lobby.players, lobby.maxPlayers, lobby.started, sameBuild && !full,
                                  !sameBuild ? "different version" : (full ? "full" : nullptr));
            ImGui::PopID();
            if (join)
            {
                const std::string address = lobby.address;
                const uint16_t port = lobby.port;
                ImGui::EndChild();
                JoinAddress(address, port);
                return;
            }
        }
        if (shown == 0)
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("Looking for games on your network...");
            if (!m_browser.Running())
            {
                ImGui::TextColored(kWarning, "%s", m_browser.Message().c_str());
            }
            ImGui::PopTextWrapPos();
        }
    }
    ImGui::EndChild();


    if (!m_titleStatus.empty())
    {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kWarning, "%s", m_titleStatus.c_str());
        ImGui::PopTextWrapPos();
    }
}

void PredationGame::DrawInvite()
{
    const ImVec2 wide{-1.0f, 30.0f};
    ImGui::PushTextWrapPos(0.0f);
    if (!LobbyServerConfigured())
    {
        ImGui::TextUnformatted("People on your network see this game in their list.");
    }
    else
    {
        const bool haveCode = !m_lobby.CodeText().empty();
        switch (m_lobby.Status())
        {
        case LobbyClient::State::Resolving:
        case LobbyClient::State::Contacting:
            ImGui::TextDisabled(haveCode ? "Opening the lobby again..." : "Getting a code...");
            break;
        case LobbyClient::State::Failed:
            ImGui::TextColored(kWarning, "%s", m_lobby.Message().c_str());
            break;
        default:
            break;
        }
        if (haveCode)
        {
            // The whole of the invitation: one code, and anybody who types it is in.
            ImGui::TextUnformatted("Send your friends this code:");
            ImGui::SetWindowFontScale(2.2f);
            ImGui::TextColored(kCodeColour, "%s", m_lobby.CodeText().c_str());
            ImGui::SetWindowFontScale(1.0f);
            if (ImGui::Button("Copy the code", wide))
            {
                ImGui::SetClipboardText(m_lobby.CodeText().c_str());
            }
            if (m_lobby.Arriving() > 0)
            {
                ImGui::TextColored({0.90f, 0.80f, 0.45f, 1.0f}, "Somebody is connecting...");
            }
            // Said up front rather than discovered by a friend who cannot get in.
            if (m_lobby.StrictRouter() && m_ports.Status() != PortMapper::State::Open)
            {
                ImGui::TextColored(kWarning, "Your router is strict, so some friends may not get "
                                             "through to you. If they cannot, let one of them host.");
            }
        }
    }

    // The other ways in, folded away: most people never need them, and the one who does is
    // usually on a network that blocks everything else.
    if (ImGui::TreeNode("Other ways to join"))
    {
        const std::string port = std::to_string(m_hostPort);
        static const std::vector<std::string> here = LocalNetworkAddresses();
        std::string addresses;
        for (const std::string& address : here)
        {
            addresses += (addresses.empty() ? "" : ", ") + address + ":" + port;
        }
        ImGui::TextDisabled("Same network: type %s into the join box.",
                            addresses.empty() ? ("this PC's address:" + port).c_str() : addresses.c_str());
        const std::string outside = m_ports.ExternalAddress();
        if (m_ports.Status() == PortMapper::State::Open && !outside.empty() && !IsInsideAddress(outside))
        {
            ImGui::TextDisabled("Anywhere: %s:%s (your router let the game in).", outside.c_str(), port.c_str());
        }
        ImGui::TreePop();
    }
    ImGui::PopTextWrapPos();
}

void PredationGame::DrawLobbyPlayers()
{
    struct Row
    {
        std::string name;
        bool host = false;
        bool you = false;
    };
    std::vector<Row> rows;
    if (m_sessionMode == SessionMode::Host)
    {
        rows.push_back({PlayerName(), true, true});
        for (const RemotePlayerView& other : m_host.Remotes())
        {
            rows.push_back({other.name, false, false});
        }
    }
    else
    {
        for (const NetClient::KnownPeer& peer : m_client.Peers())
        {
            rows.push_back({peer.name, peer.id == 0, peer.id == LocalPlayerId()});
        }
    }

    ImGui::Text("Players  %d of %d", static_cast<int>(rows.size()), static_cast<int>(kMaxPlayers));
    ImGui::BeginChild("##lobbyplayers", {0.0f, 24.0f * static_cast<float>(kMaxPlayers) + 8.0f}, true);
    for (const Row& entry : rows)
    {
        ImGui::TextUnformatted(entry.name.empty() ? "someone" : entry.name.c_str());
        if (entry.host || entry.you)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", entry.host && entry.you ? "(you, host)" : (entry.host ? "(host)" : "(you)"));
        }
    }
    ImGui::EndChild();
}

// The key list, and the whole of rebinding.
//
// Every action is named here rather than enumerated out of the input system, because that map is in
// whatever order a hash table put it and a settings screen has to be in the order somebody thinks
// about the game: moving, then fighting, then carrying, then the rest.
void PredationGame::DrawKeyBindings()
{
    struct Row
    {
        const char* action;
        const char* label;
    };
    static constexpr Row kRows[] = {
        {nullptr, "Moving"},
        {"move_forward", "Forward"},
        {"move_back", "Back"},
        {"move_left", "Left"},
        {"move_right", "Right"},
        {"jump", "Jump"},
        {"sprint", "Sprint"},
        {"walk", "Walk"},
        {"crouch", "Crouch"},
        {"prone", "Go prone"},
        {"lean_left", "Lean left"},
        {"lean_right", "Lean right"},
        {nullptr, "Fighting"},
        {"fire", "Fire"},
        {"aim", "Aim"},
        {"reload", "Reload"},
        {nullptr, "Carrying"},
        {"interact", "Interact"},
        {"drop", "Drop"},
        {"inventory", "Inventory"},
        {"slot_1", "Slot 1"},
        {"slot_2", "Slot 2"},
        {"slot_3", "Slot 3"},
        {"slot_4", "Slot 4"},
        {"slot_5", "Slot 5"},
        {"slot_6", "Slot 6"},
        {nullptr, "Other"},
        {"voice", "Talk"},
        {"flashlight", "Flashlight"},
#if PRED_DEV_TOOLS
        // Developer keys: a free camera goes through walls and respawning heals, and in a player's
        // hands both are ways round the game rather than parts of it.
        {"toggle_camera", "Change camera"},
        {"respawn", "Respawn"},
#endif
    };

    Input& input = m_app->GetInput();
    // What each shipped as, for putting one key back on its own.
    static Input shipped;
    static bool shippedRead = shipped.LoadBindings(Paths::AssetsRoot() / "Config" / "input.json");
    const std::vector<std::string> changed = shippedRead ? ChangedActions() : std::vector<std::string>{};

    if (!m_rebinding.empty())
    {
        ImGui::TextColored({0.95f, 0.85f, 0.45f, 1.0f}, "Press a key or a mouse button.");
        ImGui::TextDisabled("Escape cancels. Backspace leaves it unbound.");
    }
    else
    {
        ImGui::TextDisabled("Click a key to change it.");
    }
    ImGui::Spacing();

    if (ImGui::BeginTable("##bindings", 3, ImGuiTableFlags_SizingStretchProp))
    {
        for (const Row& row : kRows)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (row.action == nullptr)
            {
                ImGui::Spacing();
                ImGui::TextDisabled("%s", row.label);
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                continue;
            }
            ImGui::TextUnformatted(row.label);
            ImGui::TableNextColumn();

            // Every key on the action, not only the first. Crouch ships bound to both Ctrl and C,
            // and a row that showed one of them would silently drop the other the moment anybody
            // changed it.
            std::string shown;
            const auto found = input.Bindings().find(row.action);
            if (found != input.Bindings().end())
            {
                for (const Input::Binding& binding : found->second)
                {
                    if (!shown.empty())
                    {
                        shown += " / ";
                    }
                    shown += Input::BindingName(binding);
                }
            }
            if (shown.empty())
            {
                shown = "--";
            }

            ImGui::PushID(row.action);
            const bool waiting = m_rebinding == row.action;
            if (waiting)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.45f, 0.15f, 1.0f));
            }
            if (ImGui::Button(waiting ? "press a key..." : shown.c_str(), {-1.0f, 0.0f}))
            {
                m_rebinding = waiting ? std::string() : row.action;
            }
            if (waiting)
            {
                ImGui::PopStyleColor();
            }
            // This one key back as it shipped, when it has been changed.
            ImGui::TableNextColumn();
            const auto original = shipped.Bindings().find(row.action);
            if (original != shipped.Bindings().end() && std::find(changed.begin(), changed.end(), row.action) != changed.end())
            {
                if (ImGui::SmallButton("Default"))
                {
                    input.SetAction(row.action, original->second);
                    input.SaveBindings(Paths::UserBindings(), ChangedActions());
                    m_rebinding.clear();
                }
                std::string was;
                for (const Input::Binding& binding : original->second)
                {
                    was += (was.empty() ? "" : " / ") + Input::BindingName(binding);
                }
                ImGui::SetItemTooltip("Put it back on %s", was.c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (ImGui::Button("Put all the keys back"))
    {
        std::error_code ec;
        std::filesystem::remove(Paths::UserBindings(), ec);
        input.LoadBindings(Paths::AssetsRoot() / "Config" / "input.json");
        m_rebinding.clear();
    }
    ImGui::SetItemTooltip("Throws away every key you have changed and reloads the shipped set.");
}

// Takes the next key or button pressed and gives it to whatever action is waiting for one.
//
// Read raw, because a settings window has the keyboard and the mouse: the filtered queries return
// nothing while an ImGui widget has focus, which is exactly the situation this runs in.
void PredationGame::UpdateRebinding()
{
    if (m_rebinding.empty())
    {
        return;
    }
    Input& input = m_app->GetInput();

    if (input.WasKeyPressedRaw(SDL_SCANCODE_ESCAPE))
    {
        m_rebinding.clear();
        return;
    }
    if (input.WasKeyPressedRaw(SDL_SCANCODE_BACKSPACE))
    {
        input.SetAction(m_rebinding, {});
        input.SaveBindings(Paths::UserBindings(), ChangedActions());
        m_rebinding.clear();
        return;
    }

    Input::Binding chosen;
    bool got = false;
    // From 4, which is where SDL's usable scancodes start: below it are reserved values and the
    // "unknown" slot, and a loop from zero picks one of those up on the first frame.
    for (int code = 4; code < SDL_SCANCODE_COUNT && !got; ++code)
    {
        if (input.WasKeyPressedRaw(static_cast<SDL_Scancode>(code)))
        {
            chosen.kind = Input::Binding::Kind::Key;
            chosen.code = code;
            got = true;
        }
    }
    // The mouse too. Fire and aim live on it, and a rebinding screen that cannot reach them is a
    // rebinding screen for everything except the two that matter most.
    for (int button = 0; button < static_cast<int>(MouseButton::Count) && !got; ++button)
    {
        if (input.WasMousePressedRaw(static_cast<MouseButton>(button)))
        {
            chosen.kind = Input::Binding::Kind::Mouse;
            chosen.code = button;
            got = true;
        }
    }
    if (!got)
    {
        return;
    }

    // Taken off whatever else had it. One key on two actions means pressing it does both, which is
    // never what somebody rebinding meant and is very hard to work out afterwards.
    std::vector<std::pair<std::string, std::vector<Input::Binding>>> edits;
    for (const auto& [action, bindings] : input.Bindings())
    {
        if (action == m_rebinding)
        {
            continue;
        }
        std::vector<Input::Binding> kept;
        for (const Input::Binding& binding : bindings)
        {
            if (binding.kind != chosen.kind || binding.code != chosen.code)
            {
                kept.push_back(binding);
            }
        }
        if (kept.size() != bindings.size())
        {
            edits.emplace_back(action, std::move(kept));
        }
    }
    // Applied after the walk rather than during it: SetAction writes into the map being iterated.
    for (auto& [action, kept] : edits)
    {
        input.SetAction(action, kept);
    }

    input.SetAction(m_rebinding, {chosen});
    input.SaveBindings(Paths::UserBindings(), ChangedActions());
    m_rebinding.clear();
}

// Which actions differ from the shipped file, so only those are written out. See Input::MergeBindings
// for why the player's file holds the difference rather than the whole set.
std::vector<std::string> PredationGame::ChangedActions() const
{
    // Read once: the key list asks every frame it is open.
    static Input shipped;
    static const bool read = shipped.LoadBindings(Paths::AssetsRoot() / "Config" / "input.json");
    if (!read)
    {
        return {}; // nothing to compare against, so write everything
    }
    std::vector<std::string> changed;
    for (const auto& [action, bindings] : m_app->GetInput().Bindings())
    {
        const auto other = shipped.Bindings().find(action);
        const bool same =
            other != shipped.Bindings().end() && other->second.size() == bindings.size() &&
            std::equal(bindings.begin(), bindings.end(), other->second.begin(),
                       [](const Input::Binding& a, const Input::Binding& b)
                       { return a.kind == b.kind && a.code == b.code; });
        if (!same)
        {
            changed.push_back(action);
        }
    }
    return changed;
}

void PredationGame::DrawLobbyRules()
{
    // The few things about a game that are the host's to choose, and the same for everybody in it.
    const bool hosting = m_sessionMode == SessionMode::Host;
    ImGui::TextDisabled(hosting ? "Game settings" : "Game settings (the host's)");
    bool changed = false;
    if (BeginSettingsTable("##rules"))
    {
        ImGui::BeginDisabled(!hosting);

        ImGui::PushID("ff");
        SettingsRow("Friendly fire", hosting ? "game.friendly_fire" : nullptr);
        bool friendly = GetSettingBool("game.friendly_fire", true);
        if (ImGui::Checkbox("##v", &friendly))
        {
            SetSetting("game.friendly_fire", friendly ? "true" : "false");
            changed = true;
        }
        ImGui::PopID();

        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    if (changed && hosting)
    {
        SendRules();
    }
}

void PredationGame::DrawSettings()
{
    // One panel, drawn from the menu and from the pause screen alike.
    //
    // Everything here is a cvar, which means the console already reaches all of it and the archived
    // ones are already remembered between runs. What this adds is a place to find them: a setting
    // nobody can find is a setting nobody has. Every change applies at once -- a setting that waits
    // for a restart cannot be judged by looking at it.
    ImGui::SetWindowFontScale(1.2f);
    ImGui::TextUnformatted("Settings");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::Spacing();

    // Every tab the same height, and scrolling inside it rather than growing: a panel that changed size
    // with its tab moved Back and Reset out from under the cursor.
    ImGui::BeginChild("##settingsbody", {0.0f, 430.0f}, ImGuiChildFlags_None);
    if (!ImGui::BeginTabBar("##settings"))
    {
        ImGui::EndChild();
        return;
    }

    // A tab can be asked for from outside -- the console, so each one can be captured headlessly --
    // and is pushed into the bar once, because the bar otherwise keeps its own idea of which is open.
    const auto tab = [this](const char* name)
    {
        const bool wanted = m_settingsTab == name;
        if (wanted)
        {
            m_settingsTab.clear();
        }
        return ImGui::BeginTabItem(name, nullptr, wanted ? ImGuiTabItemFlags_SetSelected : 0);
    };
    const auto check = [](const char* setting, bool value)
    {
        bool changed = value;
        if (ImGui::Checkbox("##v", &changed))
        {
            SetSetting(setting, changed ? "true" : "false");
        }
    };

    // --- Display ------------------------------------------------------------------------------------
    if (tab("Display"))
    {
        ImGui::BeginChild("##display");
        if (BeginSettingsTable("##displaytable"))
        {
            ImGui::PushID("mode");
            SettingsRow("Window", "r.fullscreen");
            const bool fullscreen = GetSettingBool("r.fullscreen", false);
            if (ImGui::BeginCombo("##v", fullscreen ? "Borderless fullscreen" : "Windowed"))
            {
                if (ImGui::Selectable("Borderless fullscreen", fullscreen))
                {
                    SetSetting("r.fullscreen", "true");
                }
                if (ImGui::Selectable("Windowed", !fullscreen))
                {
                    SetSetting("r.fullscreen", "false");
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();

            ImGui::PushID("size");
            SettingsRow("Window size");
            ImGui::BeginDisabled(fullscreen);
            const int width = GetSettingInt("r.width", 1600);
            const int height = GetSettingInt("r.height", 900);
            char current[32];
            std::snprintf(current, sizeof(current), "%d x %d", width, height);
            if (ImGui::BeginCombo("##v", current))
            {
                for (const auto& [w, h] : Window::DisplaySizes())
                {
                    char label[32];
                    std::snprintf(label, sizeof(label), "%d x %d", w, h);
                    if (ImGui::Selectable(label, w == width && h == height))
                    {
                        SetSetting("r.width", std::to_string(w));
                        SetSetting("r.height", std::to_string(h));
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            ImGui::PopID();

            ImGui::PushID("vsync");
            SettingsRow("V-Sync", "r.vsync");
            check("r.vsync", GetSettingBool("r.vsync", true));
            ImGui::PopID();

            ImGui::PushID("fps");
            SettingsRow("Frame limit", "r.max_fps");
            {
                static const int kLimits[] = {0, 30, 60, 90, 120, 144, 165, 240};
                const int cap = GetSettingInt("r.max_fps", 0);
                const std::string shown = cap <= 0 ? std::string("Unlimited") : std::to_string(cap) + " fps";
                if (ImGui::BeginCombo("##v", shown.c_str()))
                {
                    for (const int limit : kLimits)
                    {
                        const std::string label = limit == 0 ? std::string("Unlimited") : std::to_string(limit) + " fps";
                        if (ImGui::Selectable(label.c_str(), limit == cap))
                        {
                            SetSetting("r.max_fps", std::to_string(limit));
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::PopID();

            ImGui::PushID("fov");
            SettingsRow("Field of view", "r.fov");
            float fov = cv_fov.Get();
            if (ImGui::SliderFloat("##v", &fov, 70.0f, 120.0f, "%.0f deg"))
            {
                SetSetting("r.fov", std::to_string(fov));
            }
            ImGui::PopID();

            ImGui::PushID("brightness");
            SettingsRow("Brightness", "r.exposure");
            float exposure = cv_exposure.Get();
            if (ImGui::SliderFloat("##v", &exposure, 0.4f, 2.5f, "%.2f"))
            {
                SetSetting("r.exposure", std::to_string(exposure));
            }
            ImGui::PopID();

            ImGui::PushID("contrast");
            SettingsRow("Contrast", "r.contrast");
            float contrast = cv_contrast.Get();
            if (ImGui::SliderFloat("##v", &contrast, 0.6f, 1.8f, "%.2f"))
            {
                SetSetting("r.contrast", std::to_string(contrast));
            }
            ImGui::PopID();
            ImGui::EndTable();
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
    }

    // --- Graphics -----------------------------------------------------------------------------------
    if (tab("Graphics"))
    {
        ImGui::BeginChild("##graphics");
        const float shadowDistance = GetSettingFloat("r.shadow_distance", 64.0f);
        const float grass = GetSettingFloat("r.grass", 1.0f);
        const float weather = GetSettingFloat("r.weather", 1.0f);
        const bool reflections = GetSettingBool("r.reflections", true);
        const float reflectionDistance = GetSettingFloat("r.reflection_distance", 30.0f);
        const int msaa = GetSettingInt("r.msaa", 0);
        const float ambientOcclusion = GetSettingFloat("r.ao", 0.7f);

        // The preset the settings amount to right now, or Custom.
        const char* presetName = "Custom";
        for (const QualityPreset& preset : kPresets)
        {
            if (std::abs(preset.shadowDistance - shadowDistance) < 0.5f && std::abs(preset.grass - grass) < 0.01f &&
                std::abs(preset.weather - weather) < 0.01f && preset.reflections == reflections &&
                std::abs(preset.reflectionDistance - reflectionDistance) < 0.5f && preset.msaa == msaa && std::abs(preset.ambientOcclusion - ambientOcclusion) < 0.01f)
            {
                presetName = preset.name;
            }
        }
        if (BeginSettingsTable("##graphicstable"))
        {
            ImGui::PushID("preset");
            SettingsRow("Quality");
            if (ImGui::BeginCombo("##v", presetName))
            {
                for (const QualityPreset& preset : kPresets)
                {
                    if (ImGui::Selectable(preset.name, std::strcmp(preset.name, presetName) == 0))
                    {
                        SetSetting("r.shadow_distance", std::to_string(preset.shadowDistance));
                        SetSetting("r.grass", std::to_string(preset.grass));
                        SetSetting("r.weather", std::to_string(preset.weather));
                        SetSetting("r.reflections", preset.reflections ? "1" : "0");
                        SetSetting("r.reflection_distance", std::to_string(preset.reflectionDistance));
                        SetSetting("r.msaa", std::to_string(preset.msaa));
                        SetSetting("r.ao", std::to_string(preset.ambientOcclusion));
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();

            ImGui::PushID("shadowdistance");
            SettingsRow("Shadow distance", "r.shadow_distance");
            {
                float value = shadowDistance;
                if (ImGui::SliderFloat("##v", &value, 40.0f, 128.0f, "%.0f m"))
                {
                    SetSetting("r.shadow_distance", std::to_string(value));
                }
            }
            ImGui::PopID();

            ImGui::PushID("ao");
            SettingsRow("Ambient occlusion", "r.ao");
            {
                float value = ambientOcclusion;
                if (ImGui::SliderFloat("##v", &value, 0.0f, 1.0f, "%.2f"))
                {
                    SetSetting("r.ao", std::to_string(value));
                }
            }
            ImGui::PopID();

            ImGui::PushID("grass");
            SettingsRow("Grass", "r.grass");
            {
                float value = grass;
                if (ImGui::SliderFloat("##v", &value, 0.0f, 1.5f, "%.2f"))
                {
                    SetSetting("r.grass", std::to_string(value));
                }
            }
            ImGui::PopID();

            ImGui::PushID("weather");
            SettingsRow("Weather detail", "r.weather");
            {
                float value = weather;
                if (ImGui::SliderFloat("##v", &value, 0.3f, 1.0f, "%.2f"))
                {
                    SetSetting("r.weather", std::to_string(value));
                }
            }
            ImGui::PopID();

            ImGui::PushID("msaa");
            SettingsRow("Anti-aliasing", "r.msaa");
            {
                const std::string shown = msaa <= 0 ? std::string("Off") : std::to_string(msaa) + "x MSAA";
                if (ImGui::BeginCombo("##v", shown.c_str()))
                {
                    for (const int samples : {0, 2, 4, 8})
                    {
                        const std::string label = samples == 0 ? std::string("Off") : std::to_string(samples) + "x MSAA";
                        if (ImGui::Selectable(label.c_str(), samples == msaa))
                        {
                            SetSetting("r.msaa", std::to_string(samples));
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::PopID();

            ImGui::PushID("bloom");
            SettingsRow("Bloom", "r.bloom");
            {
                bool bloom = GetSettingFloat("r.bloom", 0.6f) > 0.01f;
                if (ImGui::Checkbox("##v", &bloom))
                {
                    SetSetting("r.bloom", bloom ? "0.6" : "0");
                }
            }
            ImGui::PopID();

            ImGui::PushID("grain");
            SettingsRow("Film grain", "r.film_grain");
            check("r.film_grain", GetSettingBool("r.film_grain", true));
            ImGui::PopID();

            ImGui::PushID("reflections");
            SettingsRow("Mirror reflections", "r.reflections");
            check("r.reflections", reflections);
            ImGui::PopID();

#if PRED_DEV_TOOLS
            // Development only: a way to look at a level without its lighting in the way.
            ImGui::PushID("fullbright");
            SettingsRow("Fullbright (dev)");
            check("r.fullbright", GetSettingBool("r.fullbright", false));
            ImGui::PopID();
#endif

            ImGui::EndTable();
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
    }

    // --- Audio --------------------------------------------------------------------------------------
    if (tab("Audio"))
    {
        ImGui::BeginChild("##audio");
        AudioEngine& audio = m_app->GetAudio();
        if (BeginSettingsTable("##audiotable"))
        {
            ImGui::PushID("master");
            if (SettingsRow("Master volume", "audio.volume"))
            {
                audio.SetMasterGain(GetSettingFloat("audio.volume", 1.0f));
            }
            float volume = audio.MasterGain();
            if (ImGui::SliderFloat("##v", &volume, 0.0f, 1.5f, "%.2f"))
            {
                audio.SetMasterGain(volume);
                SetSetting("audio.volume", std::to_string(volume));
            }
            ImGui::PopID();

            ImGui::PushID("effects");
            SettingsRow("Effects", "audio.effects");
            float effects = GetSettingFloat("audio.effects", 1.0f);
            if (ImGui::SliderFloat("##v", &effects, 0.0f, 1.5f, "%.2f"))
            {
                SetSetting("audio.effects", std::to_string(effects));
            }
            ImGui::PopID();

            ImGui::PushID("ambience");
            SettingsRow("Ambience", "audio.ambience");
            float ambience = GetSettingFloat("audio.ambience", 1.0f);
            if (ImGui::SliderFloat("##v", &ambience, 0.0f, 2.0f, "%.2f"))
            {
                SetSetting("audio.ambience", std::to_string(ambience));
            }
            ImGui::PopID();

            ImGui::PushID("music");
            SettingsRow("Music", "audio.music");
            float music = GetSettingFloat("audio.music", 0.7f);
            if (ImGui::SliderFloat("##v", &music, 0.0f, 2.0f, "%.2f"))
            {
                SetSetting("audio.music", std::to_string(music));
            }
            ImGui::PopID();

            ImGui::PushID("unfocused");
            SettingsRow("Mute in background", "audio.mute_unfocused");
            check("audio.mute_unfocused", GetSettingBool("audio.mute_unfocused", false));
            ImGui::PopID();
            ImGui::EndTable();
        }
        if (!audio.HasDevice())
        {
            ImGui::TextDisabled("  no sound device on this machine");
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Voice");
        bool voice = cv_voiceEnabled.Get();
        if (BeginSettingsTable("##voicetable"))
        {
            ImGui::PushID("voice");
            SettingsRow("Proximity voice", "audio.voice");
            if (ImGui::Checkbox("##v", &voice))
            {
                SetSetting("audio.voice", voice ? "true" : "false");
                if (!voice)
                {
                    StopTalking();
                }
            }
            ImGui::PopID();

            ImGui::BeginDisabled(!voice);
            ImGui::PushID("voicevolume");
            SettingsRow("Voice volume", "audio.voice_volume");
            float voiceVolume = cv_voiceVolume.Get();
            if (ImGui::SliderFloat("##v", &voiceVolume, 0.0f, 2.0f, "%.2f"))
            {
                SetSetting("audio.voice_volume", std::to_string(voiceVolume));
            }
            ImGui::PopID();

            ImGui::PushID("openmic");
            SettingsRow("Open mic", "audio.voice_open_mic");
            bool openMic = cv_voiceOpenMic.Get();
            if (ImGui::Checkbox("##v", &openMic))
            {
                SetSetting("audio.voice_open_mic", openMic ? "true" : "false");
            }
            ImGui::PopID();

            ImGui::PushID("threshold");
            SettingsRow("Open mic sensitivity", "audio.voice_threshold");
            ImGui::BeginDisabled(!openMic);
            float threshold = cv_voiceThreshold.Get();
            if (ImGui::SliderFloat("##v", &threshold, 0.005f, 0.30f, "%.3f"))
            {
                SetSetting("audio.voice_threshold", std::to_string(threshold));
            }
            ImGui::EndDisabled();
            ImGui::PopID();

            // Which microphone, listed fresh rather than remembered: devices come and go while the game
            // is running, and a list of what was plugged in at startup is worse than no list.
            ImGui::PushID("device");
            SettingsRow("Microphone", "audio.voice_device");
            const std::vector<VoiceCapture::Device> devices = VoiceCapture::Devices();
            const int chosen = cv_voiceDevice.Get();
            std::string current = "System default";
            for (const VoiceCapture::Device& device : devices)
            {
                if (static_cast<int>(device.id) == chosen)
                {
                    current = device.name;
                }
            }
            if (ImGui::BeginCombo("##v", current.c_str()))
            {
                if (ImGui::Selectable("System default", chosen == 0))
                {
                    SetSetting("audio.voice_device", "0");
                }
                for (const VoiceCapture::Device& device : devices)
                {
                    if (ImGui::Selectable(device.name.c_str(), static_cast<int>(device.id) == chosen))
                    {
                        SetSetting("audio.voice_device", std::to_string(device.id));
                    }
                }
                ImGui::EndCombo();
            }
            if (devices.empty())
            {
                ImGui::TextDisabled("no microphone found");
            }
            ImGui::PopID();
            ImGui::EndDisabled();
            ImGui::EndTable();
        }

        // The level meter, with the threshold drawn on it. Setting a threshold blind is guesswork;
        // watching your own voice cross a line is not. Clamped: a loud voice's peak exceeds one, and a
        // bar handed more than one draws past its own frame.
        ImGui::BeginDisabled(!voice);
        ImGui::ProgressBar(std::clamp(m_voiceLevel, 0.0f, 1.0f), ImVec2(-1.0f, 12.0f), "");
        if (cv_voiceOpenMic.Get())
        {
            const ImVec2 bar = ImGui::GetItemRectMin();
            const ImVec2 far = ImGui::GetItemRectMax();
            const float x = bar.x + (far.x - bar.x) * std::clamp(cv_voiceThreshold.Get(), 0.0f, 1.0f);
            ImGui::GetWindowDrawList()->AddLine({x, bar.y}, {x, far.y}, IM_COL32(240, 200, 120, 255), 2.0f);
        }
        if (ImGui::Button(m_micTest ? "Stop test" : "Test microphone", {-1.0f, 0.0f}))
        {
            m_micTest = !m_micTest;
        }
        ImGui::EndDisabled();
        ImGui::EndChild();
        ImGui::EndTabItem();
    }

    // --- Controls -----------------------------------------------------------------------------------
    if (tab("Controls"))
    {
        ImGui::BeginChild("##controls");
        if (BeginSettingsTable("##controlstable"))
        {
            ImGui::PushID("sensitivity");
            SettingsRow("Mouse sensitivity", "input.mouse_sensitivity");
            float sensitivity = cv_mouseSensitivity.Get();
            if (ImGui::SliderFloat("##v", &sensitivity, 0.02f, 0.60f, "%.3f"))
            {
                SetSetting("input.mouse_sensitivity", std::to_string(sensitivity));
            }
            ImGui::PopID();

            ImGui::PushID("aimsensitivity");
            SettingsRow("Aiming sensitivity", "input.aim_sensitivity");
            float aimSensitivity = GetSettingFloat("input.aim_sensitivity", 0.75f);
            if (ImGui::SliderFloat("##v", &aimSensitivity, 0.2f, 1.5f, "x%.2f"))
            {
                SetSetting("input.aim_sensitivity", std::to_string(aimSensitivity));
            }
            ImGui::PopID();

            ImGui::PushID("invert");
            SettingsRow("Invert up and down", "input.invert_y");
            check("input.invert_y", cv_invertY.Get());
            ImGui::PopID();

            ImGui::PushID("crouch");
            SettingsRow("Toggle crouch and prone", "input.crouch_toggle");
            check("input.crouch_toggle", cv_crouchToggle.Get());
            ImGui::PopID();

            ImGui::PushID("sprint");
            SettingsRow("Toggle sprint", "input.sprint_toggle");
            check("input.sprint_toggle", cv_sprintToggle.Get());
            ImGui::PopID();
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::SeparatorText("Keys");
        // A table rather than a paragraph. Twenty bindings written as prose is a sentence nobody
        // finishes, and the one key somebody came to look up is in the middle of it.
        DrawKeyBindings();
        ImGui::EndChild();
        ImGui::EndTabItem();
    }

    // --- Gameplay -----------------------------------------------------------------------------------
    if (tab("Gameplay"))
    {
        ImGui::BeginChild("##gameplay");
        if (BeginSettingsTable("##gameplaytable"))
        {
            ImGui::PushID("shake");
            SettingsRow("Camera shake", "cam.shake");
            float shake = GetSettingFloat("cam.shake", 1.0f);
            if (ImGui::SliderFloat("##v", &shake, 0.0f, 1.0f, "%.2f"))
            {
                SetSetting("cam.shake", std::to_string(shake));
            }
            ImGui::PopID();

            ImGui::PushID("bob");
            SettingsRow("Head bob", "cam.head_bob");
            float bob = GetSettingFloat("cam.head_bob", 1.0f);
            if (ImGui::SliderFloat("##v", &bob, 0.0f, 1.0f, "%.2f"))
            {
                SetSetting("cam.head_bob", std::to_string(bob));
            }
            ImGui::PopID();

            ImGui::PushID("crosshair");
            SettingsRow("Crosshair", "hud.crosshair");
            check("hud.crosshair", GetSettingBool("hud.crosshair", true));
            ImGui::PopID();

            ImGui::PushID("fps");
            SettingsRow("Show frame rate", "hud.show_fps");
            check("hud.show_fps", GetSettingBool("hud.show_fps", false));
            ImGui::PopID();
            ImGui::EndTable();
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
    }

    // --- Multiplayer --------------------------------------------------------------------------------
    if (tab("Multiplayer"))
    {
        ImGui::BeginChild("##multiplayer");
        // Not once a game is running, for the same reason the title screen refuses: everybody else was
        // told this name when the connection was made and nothing re-tells them.
        const bool inSession = m_sessionMode != SessionMode::Offline;
        if (BeginSettingsTable("##multiplayertable"))
        {
            ImGui::PushID("name");
            SettingsRow("Your name");
            ImGui::BeginDisabled(inSession);
            if (ImGui::InputText("##v", m_playerName, sizeof(m_playerName)))
            {
                cv_playerName.Set(m_playerName);
            }
            ImGui::EndDisabled();
            ImGui::PopID();
            ImGui::EndTable();
        }
        // The lobby server is net.lobby_server, from the console: only somebody running their own needs it.
        ImGui::EndChild();
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    ImGui::EndChild();
    ImGui::Spacing();
    if (ImGui::Button("Reset to defaults"))
    {
        ImGui::OpenPopup("##resetsettings");
    }
    // Asked about, because it throws away everything the player has set and there is no undo.
    if (ImGui::BeginPopupModal("##resetsettings", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
    {
        ImGui::TextUnformatted("Put every setting back the way it shipped?");
        ImGui::Spacing();
        if (ImGui::Button("Reset", {120.0f, 0.0f}))
        {
            ResetSettingsToDefaults();
            m_app->GetAudio().SetMasterGain(GetSettingFloat("audio.volume", 1.0f));
            std::snprintf(m_playerName, sizeof(m_playerName), "%s", cv_playerName.Get().c_str());
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep mine", {120.0f, 0.0f}))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void PredationGame::DrawPauseMenu()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 centre{viewport->Pos.x + viewport->Size.x * 0.5f,
                        viewport->Pos.y + viewport->Size.y * 0.5f};
    ImGui::SetNextWindowPos(centre, ImGuiCond_Always, {0.5f, 0.5f});
    // The settings panel keeps one size whichever tab is showing. Sized to the tab, the whole window
    // jumped between Controls and Audio and the buttons under it moved out from under the cursor.
    ImGui::SetNextWindowSize({m_settingsOpen ? 700.0f : 320.0f, m_settingsOpen ? 580.0f : 0.0f},
                             ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("##pause", nullptr, flags))
    {
        ImGui::End();
        return;
    }
    const WrapText wrap;

    const ImVec2 wide{-1.0f, 32.0f};

    if (m_settingsOpen)
    {
        DrawSettings();
        ImGui::Spacing();
        if (ImGui::Button("Back", wide))
        {
            m_settingsOpen = false;
        }
        return;
    }

    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted("Paused");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Resume", wide))
    {
        m_paused = false;
        m_wantMouseCaptured = true;
    }
    ImGui::Spacing();
    if (ImGui::Button("Settings", wide))
    {
        m_settingsOpen = true;
    }
    if (m_campaignOpen && !m_campaignFolder.empty())
    {
        ImGui::Spacing();
        if (ImGui::Button("Save the campaign", wide))
        {
            SaveCampaign(false, "from the pause menu");
        }
    }
    else if (m_campaignOpen)
    {
        ImGui::TextDisabled("The host saves the campaign.");
    }

    // How to let somebody else in, while hosting. People can join a game that has already started,
    // and a host who wants to let somebody in now only has to read the code out again.
    if (m_sessionMode == SessionMode::Host)
    {
        ImGui::Spacing();
        DrawInvite();
    }
    // In a session the world carries on without you, and saying so is better than letting somebody
    // believe they have stopped the game everyone else is in.
    if (m_sessionMode != SessionMode::Offline)
    {
        ImGui::TextDisabled("The others are still playing.");
    }

    ImGui::Spacing();
    if (ImGui::Button(m_sessionMode == SessionMode::Offline ? "Leave to menu" : "Leave the game", wide))
    {
        m_paused = false;
        ReturnToTitle();
        return;
    }

    ImGui::Spacing();
    if (ImGui::Button("Quit to desktop", wide))
    {
        m_app->RequestQuit();
    }

}

void PredationGame::ReturnToTitle()
{
    PRED_LOG_INFO(Gameplay, "Back to the title screen");
    CloseCampaign();
    StopSession();
    ClearCreatures();
    RemoveAllDrones();
    // Back to the two buttons, not to whichever page somebody was last on. Coming out of a game
    // onto a half-filled join box is a screen nobody asked for.
    m_titlePage = TitlePage::Root;
    m_settingsOpen = false;
    if (m_editor.IsOpen())
    {
        m_editor.SetOpen(m_editorScene, false);
        // Whatever was saved in there is what the game uses now: the models are read again, and the
        // reload times follow the reload clips.
        ForgetWeaponModels();
        ApplyWeaponClipTimings();
    }
    m_screen = Screen::Title;
    m_paused = false;
    m_titleStatus.clear();
    m_wantMouseCaptured = false;
    m_inventoryOpen = false;
    m_loadoutOpen = false;
    if (m_hidingSpot >= 0)
    {
        LeaveHidingSpot();
    }
    SetCameraMode(CameraMode::Fly);
}

void PredationGame::UpdateTitleCamera(float frameDeltaSeconds)
{
    // On a clock that runs evenly: a frame's measured time wobbles round the screen's steady refresh, and a camera moved
    // by it judders -- slowly enough to see, in a shot this slow. A spike (a load, a dragged window) is not followed.
    m_titleStep = m_titleStep <= 0.0f ? frameDeltaSeconds : glm::mix(m_titleStep, std::min(frameDeltaSeconds, 0.05f), 0.02f);
    m_titleClock += m_titleStep;

    // The ship in orbit -- the one cinematics fly, on the stage, so nothing of anybody's is near it -- and the camera
    // drifting slowly round it, the planet's lit face below. Held for a long time, as a title's shot is.
    const glm::vec3 centre = ShipSpec::kStage + glm::vec3(0.0f, 2.5f, 4.0f);
    const float angle = kTitleAngle + 0.32f * std::sin(m_titleClock * 0.018f);
    const float lift = 11.0f + 3.5f * std::sin(m_titleClock * 0.013f + 0.7f);
    m_camera.position = centre + glm::vec3(std::sin(angle), 0.0f, std::cos(angle)) * 66.0f + glm::vec3(0.0f, lift, 0.0f);
    // Looking a little to the ship's left of it, so it sits right of the middle, clear of the menu down the left.
    const glm::vec3 toShip = glm::normalize(centre - m_camera.position);
    const glm::vec3 across = glm::normalize(glm::cross(toShip, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 look = glm::normalize(centre - across * 19.0f + glm::vec3(0.0f, -6.5f, 0.0f) - m_camera.position);
    m_camera.yaw = std::atan2(look.x, -look.z);
    m_camera.pitch = std::asin(glm::clamp(look.y, -1.0f, 1.0f));
    m_ship.SetEngines(m_scene, 0.22f);
}

void PredationGame::DrawTitleScreen()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 size = viewport->WorkSize;

    // The planet behind it is the sky's (UpdateTitleCamera): over it, only a darkening down the left, so type reads.
    ImDrawList* backdrop = ImGui::GetBackgroundDrawList();
    const ImVec2 topLeft = viewport->WorkPos;
    // Everything of the title's own comes up together once the boot cards have gone: the shade, the name, the rest.
    m_titleShownFor += ImGui::GetIO().DeltaTime;
    const float titleFade = glm::smoothstep(0.0f, 1.2f, m_titleShownFor);
    const int shade = static_cast<int>(175.0f * titleFade);
    backdrop->AddRectFilledMultiColor(topLeft, {topLeft.x + size.x * 0.5f, topLeft.y + size.y}, IM_COL32(0, 0, 0, shade),
                                      IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, shade));
    // Who made it and which version, in the corner, on every page of it.
    {
        std::string foot = std::string("made by jojozagjos  |  ") + BuildInfo::Label();
#if PRED_DEV_TOOLS
        foot += "  |  developer build";
#endif
        const float footSize = ImGui::GetFontSize() * 0.9f;
        OverlayLayer("##titlefoot").Draw()->AddText(ImGui::GetFont(), footSize, {topLeft.x + 16.0f, topLeft.y + size.y - footSize - 14.0f},
                                                IM_COL32(150, 158, 164, static_cast<int>(255.0f * titleFade)), foot.c_str());
    }

    // First the name and "press any key"; everything else after a key.
    if (!m_titleAwake)
    {
        DrawTitleSplash();
        return;
    }

    // The first page is the title's own menu, drawn over the planet; the pages under it are a panel in the same style.
    const bool waitingForHost =
        m_sessionMode == SessionMode::Client && m_client.Connected() && !m_client.HostStarted();
    const bool joining = m_sessionMode == SessionMode::Client && !m_client.Connected();
    if (!m_inLobby && m_joinTransport == nullptr && !waitingForHost && !m_settingsOpen && !joining && m_titlePage == TitlePage::Root)
    {
        DrawTitleMenu();
        return;
    }
    const TitleStyle style;

    ImGui::SetNextWindowPos({viewport->WorkPos.x + size.x * 0.07f, viewport->WorkPos.y + size.y * 0.5f}, ImGuiCond_Always,
                            {0.0f, 0.5f});
    const bool lobby = m_inLobby || m_joinTransport != nullptr || waitingForHost;
    ImGui::SetNextWindowSize({m_settingsOpen ? 700.0f : lobby ? 680.0f : m_titlePage == TitlePage::Browse ? 440.0f : 520.0f, 0.0f},
                             ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.88f);

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize;
    if (!ImGui::Begin("##title", nullptr, flags))
    {
        ImGui::End();
        return;
    }
    const WrapText wrap;



    const ImVec2 wide{-1.0f, 34.0f};

    // The lobby: a host waiting for everybody, a guest waiting for the host to start, or a guest
    // still finding the host from a code.
    if (m_inLobby || m_joinTransport != nullptr || waitingForHost)
    {
        DrawLobby();
        return;
    }

    // The same panel the pause screen shows, because they are the same settings and a player who
    // finds them in one place should not have to find them again in the other.
    if (m_settingsOpen)
    {
        DrawSettings();
        ImGui::Spacing();
        if (ImGui::Button("Back", wide))
        {
            m_settingsOpen = false;
        }
        return;
    }

    if (m_sessionMode == SessionMode::Client && !m_client.Connected())
    {
        // Joining takes a moment and can fail, so it gets its own state rather than dropping the
        // player into an empty world and leaving them to work out that nothing happened.
        ImGui::TextUnformatted("Joining...");
        if (m_client.Rejection() == JoinRejection::ServerFull)
        {
            ImGui::TextColored({0.90f, 0.55f, 0.35f, 1.0f}, "That game is full.");
        }
        else if (m_client.Rejection() == JoinRejection::VersionMismatch)
        {
            ImGui::TextColored({0.90f, 0.55f, 0.35f, 1.0f}, "That host is running a different version.");
        }
        if (ImGui::Button("Cancel", wide))
        {
            StopSession();
        }
        return;
    }


    // Everything about ports, addresses and codes belongs on the screen for the thing it is part of, not on the first
    // screen somebody sees. (What others see you called is in the settings, under Multiplayer.)
    if (m_titlePage == TitlePage::Browse || m_titlePage == TitlePage::NewCampaign || m_titlePage == TitlePage::LoadCampaign)
    {
        if (m_titlePage == TitlePage::NewCampaign)
        {
            DrawTitleNewCampaign();
        }
        else if (m_titlePage == TitlePage::LoadCampaign)
        {
            DrawTitleLoadCampaign();
        }
        else
        {
            DrawTitleBrowse();
        }
        if (!m_titleStatus.empty() && m_titlePage != TitlePage::Browse)
        {
            ImGui::Spacing();
            ImGui::TextColored(kWarning, "%s", m_titleStatus.c_str());
        }
        ImGui::Spacing();
        if (ImGui::Button("Back", wide))
        {
            // Nothing to listen for from the root menu. The beacon is a different thing and keeps going, because it
            // belongs to a game rather than to this screen.
            StopBrowsing();
            m_titlePage = TitlePage::Root;
            m_titleStatus.clear();
        }
        return;
    }

    // The first page -- Play, Settings, Quit -- is DrawTitleMenu's.
}

bool PredationGame::DrawBootScreen()
{
    // Black and white, one card after another, each coming up out of the black and going back into it, straight on from the
    // loading screen (Application::ShowLoading), which looks the same: who made it, headphones, the name -- and then the
    // title. A key moves on to the next.
    static constexpr float kCards[] = {2.8f, 2.8f, 3.0f};
    constexpr int kCount = static_cast<int>(sizeof(kCards) / sizeof(kCards[0]));
    // A scripted run goes straight in: its first commands are for the game, not for waiting on cards.
    if (m_app->IsScripted() && !m_bootForced)
    {
        m_bootDone = true;
        m_titleAwake = true;
        return false;
    }
    const float dt = ImGui::GetIO().DeltaTime;
    m_bootClock += dt;
    if (m_bootCard < kCount && m_bootClock > 0.4f && AnyKeyPressed())
    {
        ++m_bootCard;
        m_bootClock = 0.0f;
    }
    if (m_bootCard < kCount && m_bootClock >= kCards[m_bootCard])
    {
        ++m_bootCard;
        m_bootClock = 0.0f;
    }
    // After the last card, the black itself lifts off the planet; the title's own things fade in after (DrawTitleScreen).
    constexpr float kLift = 1.2f;
    if (m_bootCard >= kCount && m_bootClock >= kLift)
    {
        m_bootDone = true;
        return false;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 origin = viewport->Pos;
    const ImVec2 size = viewport->Size;
    const OverlayLayer layer("##boot");
    ImDrawList* draw = layer.Draw();
    const auto alpha = [](float a) { return static_cast<int>(std::clamp(a, 0.0f, 1.0f) * 255.0f); };
    // The name as the title sets it -- PROJECT large, PREDATION under it -- with its top left at `at`.
    const float bigSize = ImGui::GetFontSize() * 3.2f;
    const float midSize = ImGui::GetFontSize() * 1.6f;
    const std::string project = std::string("PROJECT");
    const std::string predation = std::string("PREDATION");
    const auto name = [&](const ImVec2& at, float fade)
    {
        ImFont* nameFont = ImGui::GetFont();
        draw->AddText(nameFont, bigSize, at, IM_COL32(236, 240, 242, alpha(fade)), project.c_str());
        draw->AddText(nameFont, midSize, {at.x + 2.0f, at.y + bigSize * 1.02f}, IM_COL32(200, 208, 212, alpha(fade)), predation.c_str());
    };
    const ImVec2 nameExtent = ImGui::GetFont()->CalcTextSizeA(bigSize, FLT_MAX, 0.0f, project.c_str());
    const ImVec2 work = viewport->WorkPos;
    const ImVec2 workSize = viewport->WorkSize;
    const ImVec2 centred{work.x + (workSize.x - nameExtent.x) * 0.5f, work.y + (workSize.y - bigSize * 1.02f - midSize) * 0.5f};

    const float lifted = m_bootCard >= kCount ? std::clamp(m_bootClock / kLift, 0.0f, 1.0f) : 0.0f;
    const float black = m_bootCard >= kCount ? 1.0f - glm::smoothstep(0.0f, 1.0f, lifted) : 1.0f;
    draw->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, IM_COL32(0, 0, 0, alpha(black)));
    if (m_bootCard >= kCount)
    {
        m_titleShownFor = 0.0f;
        return true;
    }
    const float t = m_bootClock;
    const float length = kCards[m_bootCard];
    // Each card up out of the black and back into it.
    const float shown = std::min(std::clamp(t / 0.6f, 0.0f, 1.0f), std::clamp((length - t) / 0.6f, 0.0f, 1.0f));
    ImFont* font = ImGui::GetFont();
    const ImVec2 centre{origin.x + size.x * 0.5f, origin.y + size.y * 0.5f};
    const auto text = [&](const std::string& words, float y, float scale, float fade)
    {
        const float fontSize = ImGui::GetFontSize() * scale;
        const ImVec2 extent = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, words.c_str());
        draw->AddText(font, fontSize, {centre.x - extent.x * 0.5f, y - extent.y * 0.5f}, IM_COL32(236, 236, 236, alpha(fade)), words.c_str());
    };
    switch (m_bootCard)
    {
    case 0:
        text("made by", centre.y - 26.0f, 1.0f, shown * 0.6f);
        text("jojozagjos", centre.y + 12.0f, 2.6f, shown);
        break;
    case 1:
    {
        // Headphones: a band over two cups.
        const float r = 34.0f;
        const ImVec2 at{centre.x, centre.y - 40.0f};
        const ImU32 white = IM_COL32(236, 236, 236, alpha(shown));
        draw->PathArcTo(at, r, glm::pi<float>() * 1.05f, glm::pi<float>() * 1.95f, 24);
        draw->PathStroke(white, ImDrawFlags_None, 4.0f);
        draw->AddRectFilled({at.x - r - 8.0f, at.y - 2.0f}, {at.x - r + 6.0f, at.y + 26.0f}, white, 4.0f);
        draw->AddRectFilled({at.x + r - 6.0f, at.y - 2.0f}, {at.x + r + 8.0f, at.y + 26.0f}, white, 4.0f);
        text(std::string("BEST PLAYED WITH HEADPHONES"), centre.y + 40.0f, 1.05f, shown);
        break;
    }
    default:
        name(centred, shown);
        break;
    }
    return true;
}

void PredationGame::DrawTitleMenu()
{
    // The name top left, as the splash had it; under it a few words, each a choice. Under the pointer one brightens,
    // slides out a little and an amber bar comes up beside it (the hover and click sounds are MenuSounds', for every
    // control). Your name below them, and who made it at the foot.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 origin = viewport->WorkPos;
    const ImVec2 size = viewport->WorkSize;
    ImGui::SetNextWindowPos(origin);
    ImGui::SetNextWindowSize(size);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                        ImGuiWindowFlags_NoScrollWithMouse;
    if (!ImGui::Begin("##titlemenu", nullptr, kFlags))
    {
        ImGui::End();
        return;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float dt = ImGui::GetIO().DeltaTime;
    const float x = origin.x + size.x * 0.07f;
    const float top = origin.y + size.y * 0.12f;
    const float big = ImGui::GetFontSize() * 3.2f;
    const float mid = ImGui::GetFontSize() * 1.6f;
    draw->AddText(font, big, {x, top}, IM_COL32(236, 240, 242, 255), "PROJECT");
    draw->AddText(font, mid, {x + 2.0f, top + big * 1.02f}, IM_COL32(200, 208, 212, 255), "PREDATION");
    draw->AddRectFilled({x + 2.0f, top + big * 1.02f + mid + 14.0f}, {x + 64.0f, top + big * 1.02f + mid + 16.0f}, IM_COL32(230, 150, 60, 230));

    struct Entry
    {
        const char* label;
        int action;
    };
    // Carrying on comes first when there is something to carry on with.
    ReadCampaignSlots();
    std::vector<Entry> entries;
    if (!m_campaignSlots.empty())
    {
        entries.push_back({"CONTINUE", 4});
    }
    entries.push_back({"NEW CAMPAIGN", 5});
    if (!m_campaignSlots.empty())
    {
        entries.push_back({"LOAD CAMPAIGN", 6});
    }
    entries.push_back({"JOIN A GAME", 0});
    entries.push_back({"SETTINGS", 1});
#if PRED_DEV_TOOLS
    entries.push_back({"MODEL EDITOR", 2});
#endif
    entries.push_back({"QUIT", 3});

    const float entrySize = ImGui::GetFontSize() * 1.55f;
    bool anyHovered = false;
    float y = origin.y + size.y * 0.44f;
    int chosen = -1;
    for (const Entry& entry : entries)
    {
        // Plain words, not spaced out letter by letter: only the name above is set that way.
        const std::string text = entry.label;
        const ImVec2 extent = font->CalcTextSizeA(entrySize, FLT_MAX, 0.0f, text.c_str());
        ImGui::SetCursorScreenPos({x - 14.0f, y});
        if (ImGui::InvisibleButton(entry.label, {extent.x + 60.0f, entrySize + 16.0f}))
        {
            chosen = entry.action;
        }
        const bool hovered = ImGui::IsItemHovered();
        // A tick as the pointer comes onto an entry -- once for each one it moves onto, not for anything else on the page.
        if (hovered && m_titleHoveredEntry != entry.label)
        {
            m_titleHoveredEntry = entry.label;
            PlayNamed("UI/hover", m_renderEye, 0.5f, 1.0f, false);
        }
        anyHovered = anyHovered || hovered;
        float& shown = m_titleHover[entry.label];
        shown += ((hovered ? 1.0f : 0.0f) - shown) * std::min(dt * 14.0f, 1.0f);
        const auto mix = [&](int a, int b) { return static_cast<int>(static_cast<float>(a) + static_cast<float>(b - a) * shown); };
        draw->AddRectFilled({x - 14.0f, y + 6.0f}, {x - 11.0f, y + entrySize + 8.0f}, IM_COL32(230, 150, 60, static_cast<int>(230.0f * shown)));
        // A shadow under each, so it reads over the stars and the lit edge of the planet alike.
        draw->AddText(font, entrySize, {x + 12.0f * shown + 2.0f, y + 9.0f}, IM_COL32(0, 0, 0, 200), text.c_str());
        draw->AddText(font, entrySize, {x + 12.0f * shown, y + 7.0f}, IM_COL32(mix(200, 255), mix(206, 250), mix(212, 244), 255), text.c_str());
        y += entrySize + 20.0f;
        // Under Continue, which campaign and where its ship is.
        if (entry.action == 4 && !m_campaignSlots.empty())
        {
            const std::string what = m_campaignSlots.front().name + "  -  " + (m_campaignSlotWhere.empty() ? std::string() : m_campaignSlotWhere.front());
            draw->AddText(font, ImGui::GetFontSize() * 0.85f, {x + 12.0f * shown + 1.0f, y - 12.0f}, IM_COL32(150, 158, 164, 255), what.c_str());
            y += 14.0f;
        }
    }

    if (!anyHovered)
    {
        m_titleHoveredEntry.clear();
    }

    ImGui::End();

    switch (chosen)
    {
    case 0:
        m_titlePage = TitlePage::Browse;
        m_titleStatus.clear();
        break;
    case 4:
        m_titleStatus.clear();
        ContinueCampaign();
        break;
    case 5:
        m_titlePage = TitlePage::NewCampaign;
        m_titleStatus.clear();
        break;
    case 6:
        m_titlePage = TitlePage::LoadCampaign;
        m_campaignSlotsRead = false;
        m_titleStatus.clear();
        break;
    case 1:
        m_settingsOpen = true;
        break;
    case 2:
        EnterEditor(std::string());
        break;
    case 3:
        m_app->RequestQuit();
        break;
    default:
        break;
    }
}

void PredationGame::DrawTitleSplash()
{
    // The name to the left over the planet, and a prompt, breathing.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 origin = viewport->WorkPos;
    const ImVec2 size = viewport->WorkSize;
    const OverlayLayer layer("##splash");
    ImDrawList* draw = layer.Draw();
    ImFont* font = ImGui::GetFont();
    const float x = origin.x + size.x * 0.07f;
    const float y = origin.y + size.y * 0.12f;
    const float big = ImGui::GetFontSize() * 3.2f;
    const float mid = ImGui::GetFontSize() * 1.6f;
    const float appear = glm::smoothstep(0.0f, 1.2f, m_titleShownFor);
    const auto a = [&](float v) { return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    draw->AddText(font, big, {x, y}, IM_COL32(236, 240, 242, a(appear)), "PROJECT");
    draw->AddText(font, mid, {x + 2.0f, y + big * 1.02f}, IM_COL32(200, 208, 212, a(appear)), "PREDATION");
    const float breathe = 0.45f + 0.55f * (0.5f + 0.5f * std::sin(m_titleClock * 2.4f));
    const char* const prompt = "Press any key";
    const float promptSize = ImGui::GetFontSize() * 1.1f;
    const ImVec2 promptExtent = font->CalcTextSizeA(promptSize, FLT_MAX, 0.0f, prompt);
    draw->AddText(font, promptSize, {origin.x + (size.x - promptExtent.x) * 0.5f, origin.y + size.y * 0.8f}, IM_COL32(220, 226, 230, a(appear * breathe)),
                  prompt);

    if (m_titleClock > 0.6f && AnyKeyPressed())
    {
        m_titleAwake = true;
    }
}

void PredationGame::DrawSoundPanel()
{
    ImGui::SetNextWindowPos(ImVec2(420.0f, 470.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360.0f, 380.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Sound"))
    {
        ImGui::End();
        return;
    }

    AudioEngine& audio = m_app->GetAudio();
    if (!audio.HasDevice())
    {
        ImGui::TextColored({0.90f, 0.55f, 0.35f, 1.0f}, "No audio device. Nothing will be heard.");
    }

    float master = audio.MasterGain();
    if (ImGui::SliderFloat("Master", &master, 0.0f, 1.0f, "%.2f"))
    {
        audio.SetMasterGain(master);
    }

    const AudioEngine::Stats stats = audio.GetStats();
    ImGui::Text("%d voices, %u started, %u stolen, %u clipped", stats.voices, stats.started,
                stats.stolen, stats.clipped);

    ImGui::Spacing();
    ImGui::SeparatorText("Voice");
    if (m_sessionMode == SessionMode::Offline)
    {
        ImGui::TextDisabled("Nobody to talk to. Voice needs a game with somebody else in it.");
    }
    else
    {
        ImGui::Text("%s (hold %s)", m_talking ? "Talking" : "Not talking",
                    KeyFor(m_app->GetInput(), "voice").c_str());
        ImGui::ProgressBar(std::clamp(m_voiceLevel, 0.0f, 1.0f), ImVec2(-1.0f, 10.0f), "");
        ImGui::SetItemTooltip("How loud the microphone is hearing you. If this stays flat while you "
                              "talk, Windows is not letting the game hear it.");
        ImGui::Text("%d %s speaking", static_cast<int>(m_speakers.size()),
                    m_speakers.size() == 1 ? "person" : "people");
        if (!m_microphone.Message().empty() && !m_talking)
        {
            ImGui::TextDisabled("%s", m_microphone.Message().c_str());
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Footsteps");

    if (m_footsteps.empty())
    {
        ImGui::TextDisabled("No clips loaded. Falling back to Assets/Audio/step_hard.");
        if (ImGui::Button("Play the fallback step"))
        {
            PlaySound(m_sounds.step.Pick(), m_camera.position, 0.30f, 1.0f, false);
        }
    }
    else
    {
        // The surface the whole world walks on, for now. Changing it here is the point of the
        // panel: walk a few steps, change it, walk a few more, and the difference is obvious in a
        // way that no amount of looking at waveforms will tell you.
        std::string current = m_footsteps[static_cast<size_t>(m_footstepSurface)].name;
        if (ImGui::BeginCombo("Surface", current.c_str()))
        {
            for (int i = 0; i < static_cast<int>(m_footsteps.size()); ++i)
            {
                const bool selected = i == m_footstepSurface;
                if (ImGui::Selectable(m_footsteps[static_cast<size_t>(i)].name.c_str(), selected))
                {
                    m_footstepSurface = i;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        // What the floor says, which overrides the combo while standing on the surface row. Shown
        // rather than hidden, so a clip that sounds wrong can be blamed on the right surface.
        if (const char* underfoot = SurfaceUnderfoot(m_player.State().position.x,
                                                     m_player.State().position.z))
        {
            ImGui::TextColored({0.70f, 0.95f, 0.75f, 1.0f}, "standing on %s", underfoot);
        }
        else
        {
            ImGui::TextDisabled("off the surface row: using the choice above");
        }

        FootstepSurface& surface = m_footsteps[static_cast<size_t>(m_footstepSurface)];
        ImGui::SliderFloat("Surface gain", &surface.gain, 0.0f, 3.0f, "%.2f");
        ImGui::SetItemTooltip("Levels this surface against the others. Not saved: when it sounds "
                              "right, put the number in Assets/Data/footsteps.json.");

        // One step, and a run of them, because a footstep is judged in a sequence and not alone.
        // A single clip can sound fine and still turn into a machine gun at a sprint.
        if (ImGui::Button("One step"))
        {
            float gain = 0.30f;
            const SoundId clip = PickFootstep(gain, m_footstepSurface);
            PlaySound(clip, m_camera.position, gain, 1.0f, false);
        }
        ImGui::SameLine();
        if (ImGui::Button(m_footstepAudition > 0.0f ? "Stop walking" : "Walk on the spot"))
        {
            m_footstepAudition = m_footstepAudition > 0.0f ? 0.0f : 0.001f;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::SliderFloat("##cadence", &m_footstepCadence, 0.20f, 1.00f, "%.2f s");
        ImGui::SetItemTooltip("Seconds between steps. About 0.55 is a walk and 0.30 a sprint.");

        ImGui::Spacing();
        ImGui::TextDisabled("Clips in this surface");
        for (size_t i = 0; i < surface.clips.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton("Play"))
            {
                PlaySound(surface.clips[i], m_camera.position, 0.30f * surface.gain, 1.0f, false);
            }
            ImGui::SameLine();
            ImGui::Text("%zu", i + 1);
            ImGui::PopID();
        }
    }

    ImGui::End();
}


} // namespace pred
