#pragma once

// What the files of PredationGame share: the settings it keeps (console variables), the title's panel style, and small
// helpers more than one of them uses. Internal to the game: its own files include it, nothing else.

#include "Engine/Core/CVar.h"
#include "Engine/Net/LobbyClient.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Material.h"

#include <imgui.h>
#include <glm/vec3.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pred
{


// The title's panels, styled as the title is: dark and square, no borders, buttons outlined in grey that warm to amber
// under the pointer. Pushed before the window and popped after it, whichever way the page returns.
struct TitleStyle
{
    TitleStyle()
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(6, 8, 10, 255));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(90, 98, 104, 160));
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(16, 18, 22, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(64, 42, 18, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(110, 70, 26, 255));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(18, 20, 24, 255));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(30, 32, 36, 255));
        ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(64, 42, 18, 200));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(84, 54, 22, 220));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, IM_COL32(230, 150, 60, 230));
        ImGui::PushStyleColor(ImGuiCol_CheckMark, IM_COL32(230, 150, 60, 255));
        ImGui::PushStyleColor(ImGuiCol_Separator, IM_COL32(60, 64, 70, 200));
        ImGui::PushStyleColor(ImGuiCol_Tab, IM_COL32(16, 18, 22, 255));
        ImGui::PushStyleColor(ImGuiCol_TabHovered, IM_COL32(84, 54, 22, 255));
        ImGui::PushStyleColor(ImGuiCol_TabActive, IM_COL32(64, 42, 18, 255));
    }
    ~TitleStyle()
    {
        ImGui::PopStyleColor(15);
        ImGui::PopStyleVar(4);
    }
    TitleStyle(const TitleStyle&) = delete;
    TitleStyle& operator=(const TitleStyle&) = delete;
};


struct WrapText
{
    WrapText() { ImGui::PushTextWrapPos(0.0f); }
    // Pops and closes the window together. These panels return in a dozen places and each of those
    // used to close the window itself, so a wrap pushed inside the window was popped outside it and
    // ImGui rightly complained. One object owning both means neither can be forgotten.
    ~WrapText()
    {
        ImGui::PopTextWrapPos();
        ImGui::End();
    }
    WrapText(const WrapText&) = delete;
    WrapText& operator=(const WrapText&) = delete;
};

extern CVar<float> cv_fov;
extern CVar<float> cv_mouseSensitivity;
extern CVar<bool> cv_eyeLog;
extern CVar<bool> cv_invertY;
extern CVar<bool> cv_crouchToggle;
extern CVar<bool> cv_sprintToggle;
extern CVar<float> cv_flySpeed;
extern CVar<float> cv_exposure;
extern CVar<float> cv_contrast;
extern CVar<float> cv_torchRange;
extern CVar<float> cv_torchIntensity;
extern CVar<float> cv_torchInner;
extern CVar<float> cv_torchOuter;
extern CVar<float> cv_torchFollow;
extern CVar<float> cv_torchSourceRadius;
extern CVar<float> cv_torchReach;
extern CVar<float> cv_flashIntensity;
extern CVar<float> cv_flashRange;
extern CVar<bool> cv_sunShadows;
extern CVar<bool> cv_skyShadows;
extern CVar<float> cv_shadowDistance;
extern CVar<bool> cv_spotShadows;
extern CVar<bool> cv_lampShadows;
extern CVar<float> cv_grass;
extern CVar<float> cv_weatherDetail;
extern CVar<float> cv_ambientOcclusion;
extern CVar<float> cv_lensFlare;
extern CVar<int> cv_occlusionDebug;
extern CVar<bool> cv_reflections;
extern CVar<float> cv_reflectionDistance;
extern CVar<float> cv_indoorLight;
extern CVar<float> cv_sunShadowBias;
extern CVar<float> cv_sunShadowOffset;
extern CVar<float> cv_skyShadowBias;
extern CVar<float> cv_skyShadowOffset;
extern CVar<float> cv_skyBrightness;
extern CVar<float> cv_thirdDistance;
extern CVar<float> cv_effectsVolume;
extern CVar<float> cv_cameraShake;
extern CVar<float> cv_headBob;
extern CVar<float> cv_aimSensitivity;
extern CVar<float> cv_lampScale;
extern CVar<bool> cv_post;
extern CVar<float> cv_bloom;
extern CVar<bool> cv_grain;
extern CVar<float> cv_vignette;
extern CVar<float> cv_eyeAdapt;
extern CVar<float> cv_eyeReference;
extern CVar<bool> cv_crosshair;
extern CVar<bool> cv_showFps;
extern CVar<bool> cv_voiceEnabled;
// How a body goes down from the blow that killed it: the way it was struck, mostly along the floor, with
// only a little lift. Straight along the blow it could go up at six metres a second, into the ceiling.
glm::vec3 DeathPush(const glm::vec3& direction);

// Brought up to a strong speaking level whatever the microphone: a quiet one as much as twelve times, a
// loud one hardly at all. Eased down fast and up slowly, so a shout is not pumped, and rounded off at the
// top rather than clipped. A fixed boost could never be enough for one person without distorting the next.
void LevelVoice(std::vector<float>& samples, float& level);
extern CVar<float> cv_voiceVolume;
extern CVar<bool> cv_voiceOpenMic;
extern CVar<float> cv_voiceThreshold;
// How long the meter goes on saying "sending" after the last packet. Longer than the gap between
// them, so the label holds steady instead of strobing between two states at twelve to one.
inline constexpr float kVoiceSendingHold = 0.20f;
// How far the view goes from straight out of a locker: either side, down and up.
inline constexpr float kLockerLookYawDegrees = 28.0f;
inline constexpr float kLockerLookDownDegrees = 22.0f;
inline constexpr float kLockerLookUpDegrees = 12.0f;
extern CVar<bool> cv_micMeter;
extern CVar<int> cv_voiceDevice;
// The lobby server: hands out codes and introduces players, so they can connect straight to each
// other (Engine/Net/LobbyProtocol.h, Docs/SERVER.md). The game's own is a free Cloudflare Worker that
// redeploys itself from this repository; it is built in so nobody has to type it.
inline constexpr const char* kDefaultLobbyServer = "https://project-predation.josephgslade.workers.dev";
extern CVar<std::string> cv_lobbyServer;
std::string LobbyServerAddress();
extern std::string g_lobbyOverride;
extern std::string g_stunOverride;
extern CVar<std::string> cv_lastAddress;
extern CVar<int> cv_lastPort;
extern CVar<std::string> cv_playerName;
extern CVar<bool> cv_friendlyFire;
extern CVar<float> cv_respawnSeconds;
extern CVar<bool> cv_showGrid;
extern CVar<bool> cv_wireframe;
extern CVar<float> cv_fogStart;
extern CVar<float> cv_fogEnd;
extern CVar<float> cv_sunIntensity;
extern CVar<bool> cv_fullbright;
extern CVar<bool> cv_clusteredLights;

// How a round is drawn. It travels rather than appearing as a whole lit line, because a line from
// the muzzle to the wall is a diagram of a shot rather than a shot. Fast enough to be over almost
// at once, slow enough that the eye catches the direction it went.
inline constexpr float kTracerSpeed = 260.0f;   // metres a second
inline constexpr float kTracerLength = 2.2f;    // how much of it is lit at any moment
// Long enough for the longest shot to arrive and its impact to fade.
inline constexpr float kTracerSeconds = 0.75f;

// The wire carries "no particular state" as a sentinel, because nine bits cannot hold a negative.
int LoadFromWire(uint16_t value);

uint16_t LoadToWire(int value);

inline constexpr float kPropRadius = 0.3f;
inline constexpr float kPropSize = 0.5f;
extern const Material kPropMaterial;

std::filesystem::path PlayerConfigPath();

// Which way the surface a round landed on faces.
//
// Not sent with the shot, and it does not need to be: the static world is the same on every machine,
// so the cheapest way to know how to lay a hole on a wall is to ask this machine's own copy of that
// wall. A short trace ending where the round did, rather than a new field on the wire and a protocol
// version to go with it.
//
// Falls back to facing the round, which is right for a flat surface hit square on and near enough
// for anything the trace misses.
//
// And what it belongs to.
//
// The body comes back as well as the normal because somebody else's round reaches this machine as a
// position and nothing more: the shot was resolved elsewhere. A mark left on a crate has to know it
// is on that crate, and this trace is already being done for the normal, so it is the one place that
// can say so without a second one.
glm::vec3 SurfaceNormalAt(PhysicsWorld& physics, const glm::vec3& from, const glm::vec3& at,
                          BodyHandle* hitBody = nullptr);


// A setting that lives in another translation unit, reached by name.
//
// The settings panel touches a few cvars the engine declares rather than the game: the volume, the
// vertical sync. Naming them here and letting the registry do the lookup is what the console already
// does, and it beats making every one of them a public symbol so that one panel can see it.
bool SettingBool(const char* name, bool fallback);

// Reads a cvar this file does not own. The registry is the one place that knows about all of them,
// and a setting declared in the engine is not visible to the game as a symbol.
bool GetSettingBool(const char* name, bool fallback);

float GetSettingFloat(const char* name, float fallback);

void SetSetting(const char* name, const std::string& value);

LobbyClient::Settings LobbySettings();

// Whether an address is one that only exists behind a router. If the router reports one of these as
// its outside address, there is another router beyond it -- usually the provider's -- and nobody on the
// internet can reach this machine by that address, whatever is opened here.
bool IsInsideAddress(const std::string& address);

inline const ImVec4 kWarning{0.90f, 0.55f, 0.35f, 1.0f};
inline const ImVec4 kCodeColour{0.70f, 0.95f, 0.75f, 1.0f};

} // namespace pred
