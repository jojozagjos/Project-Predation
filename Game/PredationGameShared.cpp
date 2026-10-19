#include "Game/PredationGameShared.h"

#include "Game/PredationGame.h"

#include "Engine/Core/Paths.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

// The settings PredationGame keeps and the helpers its files share (PredationGameShared.h).

namespace pred
{


CVar<float> cv_fov{"r.fov", 90.0f, "Horizontal field of view in degrees", CVarFlags::Archive};
CVar<float> cv_mouseSensitivity{"input.mouse_sensitivity", 0.12f, "Mouse look sensitivity in degrees per pixel",
                                CVarFlags::Archive};
CVar<bool> cv_eyeLog{"debug.eye_log", false, "Log the eye every frame: its height over the feet, before and after it is kept out of walls"};
CVar<bool> cv_invertY{"input.invert_y", false, "Invert vertical mouse look", CVarFlags::Archive};
CVar<bool> cv_crouchToggle{"input.crouch_toggle", true, "Crouch and prone toggle instead of being held",
                           CVarFlags::Archive};
CVar<bool> cv_sprintToggle{"input.sprint_toggle", false, "Sprint toggles instead of being held",
                           CVarFlags::Archive};
CVar<float> cv_flySpeed{"cam.fly_speed", 6.0f, "Fly camera speed in meters per second"};
// The picture, as opposed to the world. Both belong to the person looking at it: a monitor that
// crushes its low end makes a dark game unplayable, and the fix for that is not to light the game
// brighter for everybody.
CVar<float> cv_exposure{"r.exposure", 1.0f, "How much light reaches the picture", CVarFlags::Archive};
CVar<float> cv_contrast{"r.contrast", 1.05f, "How hard the picture separates dark from light",
                        CVarFlags::Archive};
// The flashlight, so it can be tuned by somebody standing in the dark room rather than by rebuilding.
CVar<float> cv_torchRange{"r.torch_range", 22.0f, "How far the flashlight reaches, in metres",
                          CVarFlags::Archive};
CVar<float> cv_torchIntensity{"r.torch_intensity", 34.0f, "How bright the flashlight is",
                              CVarFlags::Archive};
CVar<float> cv_torchInner{"r.torch_inner", 13.0f, "The flashlight's bright cone, in degrees",
                          CVarFlags::Archive};
CVar<float> cv_torchOuter{"r.torch_outer", 26.0f, "Where the flashlight's beam fades out, in degrees",
                          CVarFlags::Archive};
// How fast the beam catches up with the view. A torch is held in a hand on the end of an arm and
// none of that turns at the speed of a mouse; without the lag the beam is not a carried light but a
// spot painted on the middle of the screen.
CVar<float> cv_torchFollow{"r.torch_follow", 9.0f, "How quickly the flashlight catches up with the view",
                           CVarFlags::Archive};
// And how big the bulb is. The weapon is a hand span from the eye, and an inverse square from a
// mathematical point at that range is a hundred times the intensity.
CVar<float> cv_torchSourceRadius{"r.torch_source_radius", 0.75f,
                                 "How big the flashlight.s source is, in metres", CVarFlags::Archive};
CVar<float> cv_torchReach{"r.torch_reach", 0.85f,
                          "How far ahead of the eye the flashlight sits, in metres",
                          CVarFlags::Archive};
// The muzzle flash as a light. Brief and very bright: for the moment it exists it is the brightest
// thing in the level, and in a dark corridor it is the only thing that says where the walls are.
CVar<float> cv_flashIntensity{"r.muzzle_flash_intensity", 42.0f,
                              "How brightly a muzzle flash lights the room", CVarFlags::Archive};
CVar<float> cv_flashRange{"r.muzzle_flash_range", 11.0f,
                          "How far a muzzle flash throws light, in metres", CVarFlags::Archive};
// Occlusion. Everything a light is stopped by is worked out from the geometry, and these say how
// well and how far: see Engine/Render/ShadowMap.h for what the two maps are.
CVar<bool> cv_sunShadows{"r.shadows", true, "Whether the sun is stopped by anything",
                         CVarFlags::Archive};
CVar<bool> cv_skyShadows{"r.sky_occlusion", true, "Whether a roof keeps the sky out of a room",
                         CVarFlags::Archive};
CVar<float> cv_shadowDistance{"r.shadow_distance", 64.0f,
                              "How far from the player occlusion is worked out, in metres",
                              CVarFlags::Archive};
// The torch's own depth map. Without it a light that has a place has no occlusion at all and shines
// through walls, which in a game about what a beam reaches is the most visible thing on this page.
CVar<bool> cv_spotShadows{"r.torch_shadows", true, "Whether the flashlight is stopped by walls",
                          CVarFlags::Archive};
// The lamps' own shadows: walls keep a lamp's light in, and it comes through a doorway as far as it
// would. Off, each lamp is kept in its room by a box, and doorways are hard edges between one lamp and
// the next.
CVar<bool> cv_lampShadows{"r.lamp_shadows", true, "Whether the lamps are stopped by walls (off: kept in their rooms)",
                          CVarFlags::Archive};
// How much grass is drawn round the eye (0 none, 1 as the world has it), and how much of what falls from the sky.
CVar<float> cv_grass{"r.grass", 1.0f, "How thick the grass is drawn round you (0 none, 1 as the world grows it)", CVarFlags::Archive};
CVar<float> cv_weatherDetail{"r.weather", 1.0f, "How much of the falling weather is drawn (0.3 to 1)", CVarFlags::Archive};
CVar<float> cv_ambientOcclusion{"r.ao", 0.7f, "How much corners and the undersides of things are darkened (ambient occlusion; 0 off; not with multisampling)", CVarFlags::Archive};
CVar<float> cv_lensFlare{"r.flare", 0.35f, "How much the sun flares in the lens (0 none)", CVarFlags::Archive};
CVar<int> cv_occlusionDebug{"r.show_occlusion", 0,
                            "Draw occlusion instead of the scene: 1 the sun, 2 the sky"};
// Mirrors. A second pass over the whole scene, at half the window's width and height, so it is the
// most expensive thing that can be switched on here and the first thing to switch off.
CVar<bool> cv_reflections{"r.reflections", true, "Whether mirrors show the world",
                          CVarFlags::Archive};
CVar<float> cv_reflectionDistance{"r.reflection_distance", 30.0f,
                                  "How far from a mirror it stops being drawn, in metres",
                                  CVarFlags::Archive};
CVar<float> cv_indoorLight{"r.indoor_light", 0.06f,
                           "How much light is left where the sky cannot reach", CVarFlags::Archive};
// Slack, in metres. Too little and flat surfaces stripe themselves with their own shadow; too much
// and a shadow pulls away from the thing casting it.
CVar<float> cv_sunShadowBias{"r.shadow_bias", 0.05f, "Slack in the sun's occlusion test, in metres"};
CVar<float> cv_sunShadowOffset{"r.shadow_normal_offset", 0.06f,
                               "How far out along the surface the sun's test is taken, in metres"};
// Far larger than the sun.s, for two reasons that compound.
//
// The map it reads is coarse -- a texel is a quarter of a metre of world -- so a sloped surface
// crosses a lot of depth inside one. And it is read over a neighbourhood half a metre wide, which on
// a ramp means the taps up-slope are a third of a metre above the point being shaded. Without enough
// slack to ignore its own rise, a ramp rules itself in broad diagonal bands: not acne, but the
// occlusion honestly reporting that a ramp is partly under itself.
// It was 0.45, which was sized for a five-tap kernel over half a metre and for a slope term that
// called a ceiling maximally glancing. Both have changed: the neighbourhood is narrower now and the
// slope term uses abs(N.y), so the base no longer has to cover either. It also has to stay well
// under the thinnest thing expected to cast its own shade -- a 0.30 m roof over a room -- or a
// ceiling cannot shadow itself and the room lights up from above. At 0.16 a forty-five degree ramp
// still gets 0.32 m, which comfortably clears the 0.25 m its own slope crosses within the kernel.
CVar<float> cv_skyShadowBias{"r.sky_bias", 0.16f, "Slack in the sky's occlusion test, in metres"};
CVar<float> cv_skyShadowOffset{"r.sky_normal_offset", 0.05f,
                               "How far out along the surface the sky's test is taken, in metres"};
// How bright the backdrop is drawn. Only the backdrop: the ambient the world is lit by is separate,
// so turning the sky down darkens what is behind the level without flattening what is in it.
CVar<float> cv_skyBrightness{"r.sky_brightness", 0.62f, "How bright the sky is drawn",
                             CVarFlags::Archive};
// Adjustable because a prone body needs the camera much further back than a standing one, and
// inspecting the prone roll from three metres puts the camera inside the character.
CVar<float> cv_thirdDistance{"cam.third_distance", 3.2f, "How far the third-person camera sits behind the character",
                             CVarFlags::Archive};
// Proximity voice. Everything about it is a setting because everything about it is personal: whose
// microphone is too quiet, whose room is too loud, and who does not want to be heard at all.
// Everything that happens, as against the building's own sound and other people's voices.
CVar<float> cv_effectsVolume{"audio.effects", 1.0f, "How loud weapons, footsteps, doors and the creature are",
                             CVarFlags::Archive};
// Comfort, and taste: how much the view is thrown about by the things that throw it.
CVar<float> cv_cameraShake{"cam.shake", 1.0f, "How much shots and landings jolt the view, 0 to 1", CVarFlags::Archive};
CVar<float> cv_headBob{"cam.head_bob", 1.0f, "How much the view rises and falls with each step, 0 to 1",
                       CVarFlags::Archive};
CVar<float> cv_aimSensitivity{"input.aim_sensitivity", 0.75f,
                              "Mouse sensitivity with the sights up, as a share of the ordinary one", CVarFlags::Archive};
// How bright the level's own lamps are, all together: a knob for the look of the place.
CVar<float> cv_lampScale{"r.lamp_scale", 1.0f, "Multiplies the brightness of every lamp in the level"};
CVar<bool> cv_post{"r.post", true, "Finish the picture in a pass of its own: glow, grade, vignette, grain", CVarFlags::Archive};
CVar<float> cv_bloom{"r.bloom", 0.6f, "How much bright things glow", CVarFlags::Archive};
CVar<bool> cv_grain{"r.film_grain", true, "Film grain over the picture", CVarFlags::Archive};
CVar<float> cv_vignette{"r.vignette", 0.35f, "How much the edges of the picture darken"};
CVar<float> cv_eyeAdapt{"r.adapt", 0.25f, "How far the eye adapts to how bright the picture is: 0 not at all, 1 all the way"};
CVar<float> cv_eyeReference{"r.adapt_reference", 0.11f, "The brightness the picture is set for, that the eye adapts towards"};
CVar<bool> cv_crosshair{"hud.crosshair", true, "Draw the crosshair", CVarFlags::Archive};
CVar<bool> cv_showFps{"hud.show_fps", false, "Show the frame rate in the corner", CVarFlags::Archive};
CVar<bool> cv_voiceEnabled{"audio.voice", true, "Send and hear proximity voice", CVarFlags::Archive};
glm::vec3 DeathPush(const glm::vec3& direction)
{
    glm::vec3 along{direction.x, 0.0f, direction.z};
    along = glm::length(along) > 1e-3f ? glm::normalize(along) : glm::vec3(0.0f);
    return along * 3.5f + glm::vec3(0.0f, 0.8f, 0.0f);
}

void LevelVoice(std::vector<float>& samples, float& level)
{
    if (samples.empty())
    {
        return;
    }
    double sum = 0.0;
    for (const float sample : samples)
    {
        sum += static_cast<double>(sample) * sample;
    }
    const float rms = static_cast<float>(std::sqrt(sum / static_cast<double>(samples.size())));
    if (rms > 0.002f)
    {
        const float wanted = std::clamp(0.2f / rms, 1.0f, 12.0f);
        level += (wanted - level) * (wanted < level ? 0.6f : 0.08f);
    }
    for (float& sample : samples)
    {
        sample = std::tanh(sample * level);
    }
}


CVar<float> cv_voiceVolume{"audio.voice_volume", 1.0f, "How loud other people's voices are",
                           CVarFlags::Archive};
// Open mic instead of push to talk. Off by default: a game about listening for something in the
// dark is a bad place to broadcast somebody's room by accident.
CVar<bool> cv_voiceOpenMic{"audio.voice_open_mic", false,
                           "Transmit when you speak instead of while a key is held",
                           CVarFlags::Archive};
CVar<float> cv_voiceThreshold{"audio.voice_threshold", 0.04f,
                              "How loud you have to be before open mic transmits",
                              CVarFlags::Archive};
CVar<bool> cv_micMeter{"audio.mic_meter", true,
                       "Show the microphone level on screen", CVarFlags::Archive};
// Which microphone, by SDL id, or zero for the system default. An id rather than a name because
// names are not unique and change when a device is replugged; a stale id falls back to the default.
CVar<int> cv_voiceDevice{"audio.voice_device", 0, "Which microphone to record from, 0 for default",
                         CVarFlags::Archive};
// A different one, for anybody running their own. Empty -- which every settings file saved before the
// server existed has -- means the built-in one: an empty saved value would otherwise hide the default.
CVar<std::string> cv_lobbyServer{"net.lobby_server", "", "Web address of a lobby server of your own; empty for the game's",
                                 CVarFlags::Archive};
std::string LobbyServerAddress()
{
    return cv_lobbyServer.Get().empty() ? std::string(kDefaultLobbyServer) : cv_lobbyServer.Get();
}

// A lobby server, and a STUN server, for this run only, from lobby_use: never saved, so testing
// against the local server on one machine does not leave the game pointing at it afterwards.
std::string g_lobbyOverride;
std::string g_stunOverride;
// Remembered between runs, so rejoining the same friend does not mean typing the address again.
CVar<std::string> cv_lastAddress{"net.last_address", "127.0.0.1", "Address the join box opens with",
                                 CVarFlags::Archive};
CVar<int> cv_lastPort{"net.last_port", kDefaultPort, "Port the join box opens with", CVarFlags::Archive};
CVar<std::string> cv_playerName{"net.name", "operator", "What other players see you called",
                                CVarFlags::Archive};
// On by default: a four-player extraction game where rounds pass through your team is a different
// game, and a quieter one.
CVar<bool> cv_friendlyFire{"game.friendly_fire", true, "Rounds hurt other players", CVarFlags::Archive};
CVar<float> cv_respawnSeconds{"game.respawn_seconds", 6.0f, "How long you lie there before coming back",
                             CVarFlags::Archive};
CVar<bool> cv_showGrid{"debug.show_grid", false, "Draw the reference grid"};
CVar<bool> cv_wireframe{"r.wireframe", false, "Draw scene meshes as wireframe"};
CVar<float> cv_fogStart{"r.fog_start", 12.0f, "Fog start distance in meters"};
CVar<float> cv_fogEnd{"r.fog_end", 90.0f, "Fog end distance in meters"};
CVar<float> cv_sunIntensity{"r.sun_intensity", 2.2f, "Directional light intensity"};
CVar<bool> cv_fullbright{"r.fullbright", false, "Every surface its own colour, unlit: for seeing a level rather than its lighting",
                         CVarFlags::Archive};
CVar<bool> cv_clusteredLights{"r.clustered_lights", true,
                              "Light the view by clusters: each surface by the lamps that reach where it is. Off, each piece "
                              "of the level by the dozen lamps nearest it"};
int LoadFromWire(uint16_t value)
{
    return value >= kDefaultLoad ? -1 : static_cast<int>(value);
}

uint16_t LoadToWire(int value)
{
    return value < 0 ? kDefaultLoad : static_cast<uint16_t>(std::min(value, kDefaultLoad - 1));
}

const Material kPropMaterial = Material::Diffuse({0.70f, 0.55f, 0.25f}, 0.55f);
std::filesystem::path PlayerConfigPath()
{
    return Paths::AssetsRoot() / "Data" / "player.json";
}

glm::vec3 SurfaceNormalAt(PhysicsWorld& physics, const glm::vec3& from, const glm::vec3& at,
                          BodyHandle* hitBody)
{
    if (hitBody != nullptr)
    {
        *hitBody = BodyHandle{};
    }
    const glm::vec3 along = at - from;
    const float distance = glm::length(along);
    if (distance < 1e-4f)
    {
        return glm::vec3(0.0f, 1.0f, 0.0f);
    }
    const glm::vec3 direction = along / distance;
    // Started a little short of the impact so the trace has something to run into.
    const RayHit hit = physics.RayCast(at - direction * 0.25f, direction, 0.5f);
    if (hit && hitBody != nullptr)
    {
        *hitBody = hit.body;
    }
    return hit ? hit.normal : -direction;
}

bool SettingBool(const char* name, bool fallback)
{
    const CVarBase* var = CVarRegistry::Instance().Find(name);
    return var == nullptr ? fallback : var->GetString() == "true" || var->GetString() == "1";
}

bool GetSettingBool(const char* name, bool fallback)
{
    const CVarBase* cvar = CVarRegistry::Instance().Find(name);
    if (cvar == nullptr)
    {
        return fallback;
    }
    const std::string value = cvar->GetString();
    return value == "true" || value == "1";
}

float GetSettingFloat(const char* name, float fallback)
{
    const CVarBase* cvar = CVarRegistry::Instance().Find(name);
    if (cvar == nullptr)
    {
        return fallback;
    }
    try
    {
        return std::stof(cvar->GetString());
    }
    catch (...)
    {
        return fallback;
    }
}

void SetSetting(const char* name, const std::string& value)
{
    CVarRegistry::Instance().Set(name, value);
}

LobbyClient::Settings LobbySettings()
{
    LobbyClient::Settings settings;
    settings.server = g_lobbyOverride.empty() ? LobbyServerAddress() : g_lobbyOverride;
    if (!g_stunOverride.empty())
    {
        settings.stunServers = {g_stunOverride};
    }
    settings.version = kProtocolVersion;
    return settings;
}

bool IsInsideAddress(const std::string& address)
{
    unsigned a = 0;
    unsigned b = 0;
    if (std::sscanf(address.c_str(), "%u.%u", &a, &b) != 2)
    {
        return false;
    }
    return a == 10 || a == 127 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
           (a == 100 && b >= 64 && b <= 127);
}

} // namespace pred
