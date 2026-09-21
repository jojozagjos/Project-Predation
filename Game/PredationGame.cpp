#include "Game/PredationGame.h"

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
#include "Engine/Audio/Sound.h"

#include <SDL3/SDL_events.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <limits>
#include <string>

namespace pred
{
namespace
{

// Wraps every piece of text drawn while it is alive to the width of the window.
//
// ImGui does not wrap by default: a line longer than the panel runs off the right edge and the end
// of it simply is not there. Doing it per line means remembering, and the lines that were forgotten
// were the ones explaining what a button does, which are the ones somebody reading the menu for the
// first time most needs. Scoped rather than pushed and popped by hand because these panels return
// early in a dozen places and an unbalanced stack is an assert rather than a wrong-looking menu.
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

CVar<float> cv_fov{"r.fov", 90.0f, "Horizontal field of view in degrees", CVarFlags::Archive};
CVar<float> cv_mouseSensitivity{"input.mouse_sensitivity", 0.12f, "Mouse look sensitivity in degrees per pixel",
                                CVarFlags::Archive};
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
CVar<float> cv_shadowDistance{"r.shadow_distance", 32.0f,
                              "How far from the player occlusion is worked out, in metres",
                              CVarFlags::Archive};
// The torch's own depth map. Without it a light that has a place has no occlusion at all and shines
// through walls, which in a game about what a beam reaches is the most visible thing on this page.
CVar<bool> cv_spotShadows{"r.torch_shadows", true, "Whether the flashlight is stopped by walls",
                          CVarFlags::Archive};
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
CVar<bool> cv_voiceEnabled{"audio.voice", true, "Send and hear proximity voice", CVarFlags::Archive};
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
// How long the meter goes on saying "sending" after the last packet. Longer than the gap between
// them, so the label holds steady instead of strobing between two states at twelve to one.
constexpr float kVoiceSendingHold = 0.20f;
CVar<bool> cv_micMeter{"audio.mic_meter", true,
                       "Show the microphone level on screen", CVarFlags::Archive};
// Which microphone, by SDL id, or zero for the system default. An id rather than a name because
// names are not unique and change when a device is replugged; a stale id falls back to the default.
CVar<int> cv_voiceDevice{"audio.voice_device", 0, "Which microphone to record from, 0 for default",
                         CVarFlags::Archive};
// Remembered between runs, so rejoining the same friend does not mean typing the address again.
// Where the relay is, if anybody runs one. Empty, the ordinary case, means none: over the internet
// then works by address, straight to the host.
//
// Renamed from net.relay_host, which defaulted to this machine. Every copy of the game saved that
// default, so every "over the internet" game on a PC with no relay spent eight seconds asking
// nothing and then blamed a program nobody had been asked to run. A new name starts empty and
// leaves the old saved value behind.
CVar<std::string> cv_relayHost{"net.relay_server", "", "Address of a relay server, if you run one",
                              CVarFlags::Archive};
CVar<int> cv_relayPort{"net.relay_port", 27020, "UDP port the relay listens on", CVarFlags::Archive};
CVar<std::string> cv_lastAddress{"net.last_address", "127.0.0.1", "Address the join box opens with",
                                 CVarFlags::Archive};
CVar<int> cv_lastPort{"net.last_port", kDefaultPort, "Port the join box opens with", CVarFlags::Archive};
CVar<std::string> cv_playerName{"net.name", "operator", "What other players see you called",
                                CVarFlags::Archive};
// On by default: a four-player extraction game where rounds pass through your team is a different
// game, and a quieter one.
CVar<bool> cv_friendlyFire{"game.friendly_fire", true, "Rounds hurt other players", CVarFlags::Archive};
CVar<float> cv_migrationSeconds{"net.migration_seconds", 2.5f,
                              "How long to wait after losing the host before taking over",
                              CVarFlags::Archive};
CVar<float> cv_respawnSeconds{"game.respawn_seconds", 6.0f, "How long you lie there before coming back",
                             CVarFlags::Archive};
CVar<bool> cv_showGrid{"debug.show_grid", false, "Draw the reference grid"};
CVar<bool> cv_wireframe{"r.wireframe", false, "Draw scene meshes as wireframe"};
CVar<float> cv_fogStart{"r.fog_start", 12.0f, "Fog start distance in meters"};
CVar<float> cv_fogEnd{"r.fog_end", 90.0f, "Fog end distance in meters"};
CVar<float> cv_sunIntensity{"r.sun_intensity", 2.2f, "Directional light intensity"};

// How a round is drawn. It travels rather than appearing as a whole lit line, because a line from
// the muzzle to the wall is a diagram of a shot rather than a shot. Fast enough to be over almost
// at once, slow enough that the eye catches the direction it went.
constexpr float kTracerSpeed = 260.0f;   // metres a second
constexpr float kTracerLength = 2.2f;    // how much of it is lit at any moment
// Long enough for the longest shot to arrive and its impact to fade.
constexpr float kTracerSeconds = 0.75f;

// The wire carries "no particular state" as a sentinel, because nine bits cannot hold a negative.
int LoadFromWire(uint16_t value)
{
    return value >= kDefaultLoad ? -1 : static_cast<int>(value);
}

uint16_t LoadToWire(int value)
{
    return value < 0 ? kDefaultLoad : static_cast<uint16_t>(std::min(value, kDefaultLoad - 1));
}

constexpr float kPropRadius = 0.3f;
constexpr float kPropSize = 0.5f;
const Material kPropMaterial = Material::Diffuse({0.70f, 0.55f, 0.25f}, 0.55f);

std::filesystem::path PlayerConfigPath()
{
    return Paths::AssetsRoot() / "Data" / "player.json";
}

// Which way the surface a round landed on faces.
//
// Not sent with the shot, and it does not need to be: the static world is the same on every machine,
// so the cheapest way to know how to lay a hole on a wall is to ask this machine's own copy of that
// wall. A short trace ending where the round did, rather than a new field on the wire and a protocol
// version to go with it.
//
// Falls back to facing the round, which is right for a flat surface hit square on and near enough
// for anything the trace misses.
// Which way the surface a round arrived at faces, and what it belongs to.
//
// The body comes back as well as the normal because somebody else's round reaches this machine as a
// position and nothing more: the shot was resolved elsewhere. A mark left on a crate has to know it
// is on that crate, and this trace is already being done for the normal, so it is the one place that
// can say so without a second one.
glm::vec3 SurfaceNormalAt(PhysicsWorld& physics, const glm::vec3& from, const glm::vec3& at,
                          BodyHandle* hitBody = nullptr)
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
} // namespace
// Every recording of a sound that is on disk.
//
// The folder is the registration. Assets/Audio/<name>/ holding any number of wav files means the
// game plays those and picks a different one each time; an empty folder, or no folder, means that
// sound is silent. Nothing has to be listed anywhere and nothing has to be rebuilt, which is the
// point: a sound can be replaced by somebody who does not build the game, and adding a second and
// third file is the whole of how a sound stops repeating.
//
// Trailing silence is trimmed, as it is for footsteps, because a downloaded pack usually carries
// some and it delays every sound after it in a queue by however long it is.
SoundVariants LoadSoundVariants(AudioEngine& audio, const char* name)
{
    SoundVariants variants;
    const std::filesystem::path folder = Paths::AssetsRoot() / "Audio" / name;
    std::error_code ec;
    if (std::filesystem::is_directory(folder, ec))
    {
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            std::string extension = entry.path().extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (extension == ".wav")
            {
                files.push_back(entry.path());
            }
        }
        // Sorted, so the order a machine happens to list a directory in cannot change which sound
        // is which between two runs on two machines.
        std::sort(files.begin(), files.end());
        for (const std::filesystem::path& path : files)
        {
            std::ifstream wav(path, std::ios::binary);
            if (!wav)
            {
                continue;
            }
            const std::string bytes((std::istreambuf_iterator<char>(wav)),
                                    std::istreambuf_iterator<char>());
            SoundData data;
            std::string error;
            if (!LoadWav(bytes.data(), bytes.size(), data, error))
            {
                PRED_LOG_WARN(Gameplay, "{}: {}", path.string(), error);
                continue;
            }
            TrimTrailingSilence(data);
            const SoundId id =
                audio.Add(std::string("recorded/") + name + "/" + path.filename().string(),
                          std::move(data));
            if (id != kInvalidSound)
            {
                variants.ids.push_back(id);
            }
        }
    }

    if (!variants.ids.empty())
    {
        PRED_LOG_INFO(Gameplay, "{}: {} recording(s) from Assets/Audio/{}", name, variants.ids.size(),
                      name);
        return variants;
    }
    // No file, no sound, and said so rather than quietly substituting something.
    //
    // This used to fall back to a recipe synthesised at startup, which meant a missing or misnamed
    // file was inaudible as a problem: the game made a noise, just not the one anybody had put
    // there. Silence and a line in the log is the honest version, and it is the one that tells
    // somebody replacing a sound that they have put it in the wrong place.
    PRED_LOG_WARN(Gameplay, "No wav files in Assets/Audio/{}; that sound will be silent", name);
    return variants;
}


bool PredationGame::OnInit(Application& app)
{
    m_app = &app;

    BuildTestMap(m_scene, app.GetMeshes(), &app.GetPhysics());

    m_propSphereMesh = app.GetMeshes().Upload(Primitives::Sphere(kPropRadius, 20, 14), "prop_sphere");
    m_propBoxMesh = app.GetMeshes().Upload(Primitives::Box(glm::vec3(kPropSize)), "prop_box");

    PlayerConfig config;
    config.LoadFromFile(PlayerConfigPath());
    if (!m_player.Init(app.GetPhysics(), config, m_spawnPoint))
    {
        PRED_LOG_ERROR(Gameplay, "Player controller failed to initialize");
        return false;
    }

    // The pool of bullet holes, made once and moved about thereafter.
    //
    // Creating an entity per round would grow the scene without limit across a match and put a mesh
    // upload on the frame a trigger is pulled. A fixed ring costs its whole size up front, never
    // allocates again, and the oldest hole quietly becomes the newest.
    {
        // Each slot has a mesh of its own, because a hole that wraps what it landed on is a
        // different shape on every surface. They start as the flat crater, which is also what any
        // of them is before it is first used.
        m_bulletHoleShape = BuildBulletHoleShape();
        const Material holeMaterial = Material::Diffuse({0.05f, 0.045f, 0.04f}, 0.95f);
        m_bulletHoles.reserve(kMaxBulletHoles);
        m_bulletHoleMeshes.reserve(kMaxBulletHoles);
        m_bulletHoleAttachments.assign(kMaxBulletHoles, HoleAttachment{});
        for (size_t i = 0; i < kMaxBulletHoles; ++i)
        {
            const MeshHandle mesh =
                app.GetMeshes().Upload(m_bulletHoleShape, "bullet_hole_" + std::to_string(i));
            m_bulletHoleMeshes.push_back(mesh);
            const Entity hole =
                m_scene.CreateMeshEntity("bullet_hole", Transform{}, mesh, holeMaterial);
            if (MeshRenderer* renderer = m_scene.GetMeshRenderer(hole))
            {
                // Hidden until it is used, and never a shadow caster: a decal lying on a wall would
                // otherwise shadow the wall it is lying on.
                renderer->visible = false;
                renderer->castsShadow = false;
            }
            m_bulletHoles.push_back(hole);
        }
    }

    m_body.Build(m_scene, app.GetMeshes(), m_player.Config());
    m_body.SetTextureLibrary(app.GetTextures());

    // Every sound the game makes, from a file on disk. Nothing is generated at run time any more.
    //
    // They used to be built from recipes in sounds.json at startup, which was the right call while
    // there was nobody to record anything and the wrong one to keep: a recipe cannot be opened in
    // an editor, sent to anybody, or replaced without learning what `bite` means. Every sound is now
    // a wav in Assets/Audio/<name>/, and replacing one is dropping a file on top of another file --
    // no build, no JSON, no restart beyond the next run.
    //
    // The recipes and the synthesiser are still in the engine, reached only by the `sound_bake`
    // console command that produced these files. That is where a generated sound belongs: in the
    // thing that makes the placeholder, not in the thing that ships.
    {
        AudioEngine& audio = app.GetAudio();
        m_sounds.gunshot = LoadSoundVariants(audio, "gunshot");
        m_sounds.dryFire = LoadSoundVariants(audio, "dry_fire");
        m_sounds.reloadOut = LoadSoundVariants(audio, "reload_out");
        m_sounds.reloadIn = LoadSoundVariants(audio, "reload_in");
        m_sounds.step = LoadSoundVariants(audio, "step_hard");
        m_sounds.land = LoadSoundVariants(audio, "land");
        m_sounds.door = LoadSoundVariants(audio, "door");
        m_sounds.locker = LoadSoundVariants(audio, "locker");
        m_sounds.pickup = LoadSoundVariants(audio, "pickup");
        m_sounds.drop = LoadSoundVariants(audio, "drop");
        m_sounds.hurt = LoadSoundVariants(audio, "hurt");
        m_sounds.death = LoadSoundVariants(audio, "death");
    }

    LoadFootsteps(app.GetAudio());

    m_items.LoadFromFile(Paths::AssetsRoot() / "Data" / "items.json");
    // Weapons load before the icons, because a weapon item draws its icon from the weapon's own
    // model and would otherwise fall back to the placeholder block.
    m_weaponData.LoadFromFile(Paths::AssetsRoot() / "Data" / "weapons.json");
    m_itemIcons.Build(m_items, app.GetMeshes(), app.GetRenderer(), &m_weaponData, &app.GetTextures());
    m_world.SetTextures(app.GetTextures());
    m_world.Build(m_scene, app.GetMeshes(), app.GetPhysics(), m_interactions, m_items, &m_weaponData);
    app.GetPhysics().OptimizeBroadPhase();

    // Checked once the whole level exists, so a piece placed on top of another is caught here
    // rather than by walking into it. 10 mm is below anything anyone would notice.
    ReportMapOverlaps(0.01f);

    // Editing player.json on disk applies immediately, without a rebuild or a restart.
    app.GetFileWatcher().Watch(PlayerConfigPath(),
                               [this](const std::filesystem::path&) { ReloadPlayerConfig(); });

    // Yaw 0 looks down -Z, which is where the test map is from the spawn point.
    m_lookYaw = 0.0f;
    m_lookPitch = 0.0f;
    m_camera.position = {0.0f, 3.2f, 22.0f};

    m_editor.Init(app);

    RegisterCommands();
    RegisterNetCommands();
    // The game opens at the menu, with the world already built behind it.
    std::snprintf(m_joinAddress, sizeof(m_joinAddress), "%s", cv_lastAddress.Get().c_str());
    std::snprintf(m_playerName, sizeof(m_playerName), "%s", cv_playerName.Get().c_str());
    m_joinPort = std::clamp(cv_lastPort.Get(), 1024, 65535);
    ReturnToTitle();
    UpdateMouseCapture();

    PRED_LOG_INFO(Gameplay,
                  "Controls: WASD move, Space jump, Ctrl or C crouch, Z prone, Shift sprint, Alt "
                  "walk, Q and E lean. Left mouse fires, right mouse aims, R reloads. "
                  "F interact, G drop, 1 to 6 and the wheel select, Tab inventory. "
                  "P cycles first person, third person and free camera; in third person hold middle "
                  "mouse to orbit. F3 overlay, F5 respawn, backtick console, "
                  "Escape frees the cursor.");
#if PRED_DEV_TOOLS
    PRED_LOG_INFO(Gameplay, "Developer build: the model editor is on the title screen, or type "
                            "\"editor\" in the console.");
#endif
    if (!cv_crouchToggle.Get())
    {
        PRED_LOG_INFO(Gameplay, "Crouch and prone are hold-to-activate. Set input.crouch_toggle to "
                                "true in the console to make them toggle instead.");
    }
    return true;
}

std::string PredationGame::DescribeBody(BodyHandle body) const
{
    if (m_app == nullptr || !m_app->GetPhysics().IsValid(body))
    {
        return "<unknown>";
    }
    const glm::vec3 position = m_app->GetPhysics().GetTransform(body).position;

    std::string best = "<unnamed>";
    float bestDistance = std::numeric_limits<float>::max();
    m_scene.ForEachMeshRenderer([&](Entity entity, const Transform& transform, const MeshRenderer&) {
        const float distance = glm::distance(transform.position, position);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = m_scene.Name(entity);
        }
    });
    // A body far from every entity centre is one of several making up a compound object, such as a
    // locker wall, so say so rather than claiming a name that is only roughly right.
    return bestDistance > 1.5f ? best + "?" : best;
}

void PredationGame::ReportMapOverlaps(float minPenetration)
{
    if (m_app == nullptr)
    {
        return;
    }
    const std::vector<PhysicsWorld::StaticOverlap> overlaps =
        m_app->GetPhysics().FindStaticOverlaps(minPenetration);
    if (overlaps.empty())
    {
        PRED_LOG_INFO(Gameplay, "Map geometry check: no static solids overlap by more than {:.0f} mm",
                      minPenetration * 1000.0f);
        return;
    }

    PRED_LOG_WARN(Gameplay, "Map geometry check: {} overlapping pairs of static solids", overlaps.size());
    for (const PhysicsWorld::StaticOverlap& overlap : overlaps)
    {
        PRED_LOG_WARN(Gameplay, "  {} into {}, {:.0f} mm deep at ({:.2f}, {:.2f}, {:.2f})",
                      DescribeBody(overlap.a), DescribeBody(overlap.b), overlap.penetration * 1000.0f,
                      overlap.position.x, overlap.position.y, overlap.position.z);
    }
}

void PredationGame::ReloadPlayerConfig()
{
    PlayerConfig config;
    if (config.LoadFromFile(PlayerConfigPath()))
    {
        m_player.Config() = config;
        m_player.ApplyConfigToCharacter();
    }
}

void PredationGame::RegisterCommands()
{
    Console& console = m_app->GetConsole();

    console.RegisterCommand("respawn", "Return the player to the spawn point with full health",
                            [this](const std::vector<std::string>&) { RespawnLocalPlayer(m_spawnPoint); });

    console.RegisterCommand(
        "teleport", "Move the player to a position: teleport <x> <y> <z>",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 4)
            {
                m_app->GetConsole().PrintError("usage: teleport <x> <y> <z>");
                return;
            }
            m_player.Teleport({std::strtof(args[1].c_str(), nullptr), std::strtof(args[2].c_str(), nullptr),
                               std::strtof(args[3].c_str(), nullptr)});
        },
        "teleport <x> <y> <z>");

    console.RegisterCommand(
        "camera", "Switch camera: camera <first|third|fly>",
        [this](const std::vector<std::string>& args)
        {
            const std::string which = args.size() > 1 ? args[1] : "first";
            if (which == "first")
            {
                SetCameraMode(CameraMode::FirstPerson);
            }
            else if (which == "third")
            {
                SetCameraMode(CameraMode::ThirdPerson);
            }
            else if (which == "fly")
            {
                SetCameraMode(CameraMode::Fly);
            }
            else
            {
                m_app->GetConsole().PrintError("usage: camera <first|third|fly>");
            }
        },
        "camera <first|third|fly>");

    console.RegisterCommand("fly", "Toggle the free-flying inspection camera",
                            [this](const std::vector<std::string>&)
                            {
                                SetCameraMode(m_cameraMode == CameraMode::Fly ? CameraMode::FirstPerson
                                                                              : CameraMode::Fly);
                            });

    console.RegisterCommand(
        "cam_orbit", "Set the third-person orbit offset: cam_orbit <yaw> <pitch>",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 3)
            {
                m_app->GetConsole().PrintError("usage: cam_orbit <yaw> <pitch>");
                return;
            }
            m_orbitYaw = glm::radians(std::strtof(args[1].c_str(), nullptr));
            m_orbitPitch = glm::radians(std::strtof(args[2].c_str(), nullptr));
        },
        "cam_orbit <yaw> <pitch>");

    console.RegisterCommand(
        "stance", "Hold a stance for inspection: stance <stand|crouch|prone|auto>",
        [this](const std::vector<std::string>& args)
        {
            const std::string which = args.size() > 1 ? args[1] : "auto";
            m_forceCrouch = which == "crouch";
            m_forceProne = which == "prone";
            if (which != "stand" && which != "crouch" && which != "prone" && which != "auto")
            {
                m_app->GetConsole().PrintError("usage: stance <stand|crouch|prone|auto>");
                return;
            }
            m_app->GetConsole().Print("Stance override: " + which);
        },
        "stance <stand|crouch|prone|auto>");

    console.RegisterCommand(
        "player_yaw", "Face the player in a given direction, for inspection: player_yaw <degrees>",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 2)
            {
                m_app->GetConsole().PrintError("usage: player_yaw <degrees>");
                return;
            }
            m_player.State().yaw = glm::radians(std::strtof(args[1].c_str(), nullptr));
            m_app->GetConsole().Print("Player yaw set");
        },
        "player_yaw <degrees>");

    console.RegisterCommand(
        "map_overlaps", "List level geometry that intersects other level geometry",
        [this](const std::vector<std::string>& args)
        {
            const float minimum = args.size() >= 2 ? std::strtof(args[1].c_str(), nullptr) : 0.01f;
            const std::vector<PhysicsWorld::StaticOverlap> overlaps =
                m_app->GetPhysics().FindStaticOverlaps(minimum);
            if (overlaps.empty())
            {
                m_app->GetConsole().Print("No static solids overlap");
                return;
            }
            for (const PhysicsWorld::StaticOverlap& overlap : overlaps)
            {
                char buffer[256];
                std::snprintf(buffer, sizeof(buffer), "%s into %s, %.0f mm at %.2f %.2f %.2f",
                              DescribeBody(overlap.a).c_str(), DescribeBody(overlap.b).c_str(),
                              overlap.penetration * 1000.0f, overlap.position.x, overlap.position.y,
                              overlap.position.z);
                m_app->GetConsole().Print(buffer);
            }
        },
        "map_overlaps [minimum metres]");
    console.RegisterCommand("player_reload", "Reload player.json from disk",
                            [this](const std::vector<std::string>&) { ReloadPlayerConfig(); });

    console.RegisterCommand("player_save", "Write the current player tuning back to player.json",
                            [this](const std::vector<std::string>&)
                            {
                                if (m_player.Config().SaveToFile(PlayerConfigPath()))
                                {
                                    m_app->GetConsole().Print("Saved " + PlayerConfigPath().string());
                                }
                            });

    console.RegisterCommand("player_state", "Print the player's simulation state",
                            [this](const std::vector<std::string>&)
                            {
                                const PlayerState& state = m_player.State();
                                char buffer[256];
                                std::snprintf(buffer, sizeof(buffer),
                                              "pos %.2f %.2f %.2f  speed %.2f m/s  %s  %s  health %.0f",
                                              state.position.x, state.position.y, state.position.z,
                                              state.HorizontalSpeed(), PlayerStanceName(state.stance),
                                              state.grounded ? "grounded" : "airborne", state.health);
                                m_app->GetConsole().Print(buffer);
                            });

    console.RegisterCommand("cam_reset", "Reset the fly camera",
                            [this](const std::vector<std::string>&)
                            {
                                m_camera = FlyCamera{};
                                m_camera.position = {0.0f, 3.2f, 22.0f};
                            });

    console.RegisterCommand(
        "cam_pos", "Print or set the fly camera position, and optionally its yaw and pitch",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() >= 4)
            {
                m_camera.position = glm::vec3(std::strtof(args[1].c_str(), nullptr),
                                              std::strtof(args[2].c_str(), nullptr),
                                              std::strtof(args[3].c_str(), nullptr));
                m_cameraMode = CameraMode::Fly;
            }
            if (args.size() >= 6)
            {
                m_lookYaw = glm::radians(std::strtof(args[4].c_str(), nullptr));
                m_lookPitch = glm::radians(std::strtof(args[5].c_str(), nullptr));
            }
            char buffer[160];
            std::snprintf(buffer, sizeof(buffer), "camera %.2f %.2f %.2f  yaw %.1f pitch %.1f",
                          m_camera.position.x, m_camera.position.y, m_camera.position.z,
                          glm::degrees(m_lookYaw), glm::degrees(m_lookPitch));
            m_app->GetConsole().Print(buffer);
        },
        "cam_pos [x y z [yaw pitch]]");

    console.RegisterCommand(
        "give", "Put an item straight into the inventory: give <key> [count]",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 2)
            {
                m_app->GetConsole().PrintError("usage: give <key> [count]");
                return;
            }
            const ItemId id = m_items.IdOf(args[1]);
            if (id == kInvalidItem)
            {
                m_app->GetConsole().PrintError("no such item: " + args[1]);
                return;
            }
            const int count = args.size() >= 3 ? std::atoi(args[2].c_str()) : 1;
            const int stored = m_inventory.Add(m_items, id, std::max(count, 1));
            m_app->GetConsole().Print("Stored " + std::to_string(stored) + " " + args[1]);
        },
        "give <key> [count]");

    console.RegisterCommand(
        "move", "Drive the player from the console, for inspecting animation: move <x> <y> (-1..1)",
        [this](const std::vector<std::string>& args)
        {
            m_debugMove = args.size() >= 3 ? glm::vec2(std::strtof(args[1].c_str(), nullptr),
                                                       std::strtof(args[2].c_str(), nullptr))
                                           : glm::vec2(0.0f);
        },
        "move <x> <y>");

    console.RegisterCommand(
        "give_weapon", "Put a weapon in the inventory and select it: give_weapon <key>",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 2)
            {
                m_app->GetConsole().PrintError("usage: give_weapon <key>");
                return;
            }
            const WeaponDefinition* weapon = m_weaponData.Find(args[1]);
            if (weapon == nullptr)
            {
                m_app->GetConsole().PrintError("no such weapon: " + args[1]);
                return;
            }
            const ItemId id = m_items.IdOf(weapon->item);
            if (id == kInvalidItem || m_inventory.Add(m_items, id, 1) <= 0)
            {
                m_app->GetConsole().PrintError("no room for " + weapon->name);
                return;
            }
            for (int i = 0; i < m_inventory.SlotCount(); ++i)
            {
                if (m_inventory.At(i).item == id)
                {
                    m_inventory.SelectSlot(i);
                    break;
                }
            }
            SyncEquippedWeapon();
            m_app->GetConsole().Print("Equipped " + weapon->name);
        },
        "give_weapon <key>");

    console.RegisterCommand(
        "fire", "Hold the trigger for a number of ticks, for testing without a mouse: fire [ticks]",
        [this](const std::vector<std::string>& args)
        { m_debugTriggerTicks = args.size() >= 2 ? std::atoi(args[1].c_str()) : 1; }, "fire [ticks]");

    console.RegisterCommand("weapon_state", "Print the equipped weapon's simulation state",
                            [this](const std::vector<std::string>&)
                            {
                                const WeaponDefinition* weapon = EquippedWeapon();
                                if (weapon == nullptr)
                                {
                                    m_app->GetConsole().Print("Unarmed");
                                    return;
                                }
                                char buffer[224];
                                std::snprintf(buffer, sizeof(buffer),
                                              "%s  %d/%d  %s  aim %.2f  spread %.2f deg  shots %u",
                                              weapon->name.c_str(), m_weapon.rounds, m_weapon.reserve,
                                              m_weapon.IsReloading() ? "reloading" : "ready", m_weapon.aim,
                                              WeaponSim::CurrentSpread(*weapon, m_weapon),
                                              m_weapon.shotCount);
                                m_app->GetConsole().Print(buffer);
                            });

    console.RegisterCommand(
        "aim", "Hold or release the sights, for inspecting the hold without a mouse: aim [0|1]",
        [this](const std::vector<std::string>& args)
        { m_debugAim = args.size() < 2 || std::atoi(args[1].c_str()) != 0; }, "aim [0|1]");

    console.RegisterCommand("reload", "Start a reload",
                            [this](const std::vector<std::string>&) { m_reloadLatch = 30; });

#if PRED_DEV_TOOLS
    console.RegisterCommand(
        "editor", "Open or close the model and animation editor: editor [model name]",
        [this](const std::vector<std::string>& args)
        {
            EnterEditor(args.size() >= 2 ? args[1] : std::string());
        },
        "editor [model]");

    console.RegisterCommand(
        "bench_weapon",
        "Point a weapon at the model open in the editor: bench_weapon <key>",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 2)
            {
                m_app->GetConsole().PrintError("usage: bench_weapon <key>");
                return;
            }
            AssignModelToWeapon(m_weaponData.IdOf(args[1]));
        });

    console.RegisterCommand(
        "hold_report",
        "Print where the weapon sits relative to the eye, in the view's own frame: hold_report [frames]",
        [this](const std::vector<std::string>& args)
        {
            // Optionally after a wait. Commands from --exec all run before the first frame, when
            // nothing has been equipped and no body has been posed, so a report taken then is a
            // report about an empty hand.
            if (args.size() >= 2)
            {
                m_holdReportIn = std::max(std::atoi(args[1].c_str()), 1);
                return;
            }
            ReportHold();
        });

    console.RegisterCommand("solo", "Leave the title screen and start a game on your own",
                            [this](const std::vector<std::string>&)
                            {
                                if (m_screen == Screen::Title)
                                {
                                    StopSession();
                                    EnterWorld();
                                }
                            });

    console.RegisterCommand(
        "model_import",
        "Import a glTF binary as an editable model: model_import <file.glb> <name> [size] [turn x y z] "
        "[skip=part,part]",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 3)
            {
                m_app->GetConsole().PrintError(
                    "usage: model_import <file.glb> <name> [size] [turn x y z] [skip=part,part]");
                return;
            }
            GltfImportOptions options;
            // Images inside the file are written out beside the model, because a model file is meant
            // to stay something a person can open and a base colour image is two megabytes of it.
            options.textureDirectory = ModelDirectory() / "Textures";
            options.texturePrefix = args[2];
            if (args.size() >= 4)
            {
                options.targetSize = static_cast<float>(std::atof(args[3].c_str()));
            }
            if (args.size() >= 7)
            {
                options.rotationDegrees = {static_cast<float>(std::atof(args[4].c_str())),
                                           static_cast<float>(std::atof(args[5].c_str())),
                                           static_cast<float>(std::atof(args[6].c_str()))};
            }
            // Display props to leave behind -- the spare magazine and loose rounds a download often
            // comes with. Anywhere on the line, so it does not care which of the numbers were given.
            for (const std::string& arg : args)
            {
                if (arg.rfind("skip=", 0) != 0)
                {
                    continue;
                }
                std::string list = arg.substr(5);
                size_t start = 0;
                while (start <= list.size())
                {
                    const size_t comma = list.find(',', start);
                    const std::string name = list.substr(start, comma - start);
                    if (!name.empty())
                    {
                        options.skipParts.push_back(name);
                    }
                    if (comma == std::string::npos)
                    {
                        break;
                    }
                    start = comma + 1;
                }
            }
            ModelAsset imported;
            std::string error;
            if (!LoadGlbModel(args[1], options, imported, &error))
            {
                m_app->GetConsole().PrintError("import failed: " + error);
                return;
            }
            imported.name = args[2];
            if (!imported.SaveToFile(ModelPathFor(args[2], "Weapons")))
            {
                m_app->GetConsole().PrintError("could not write the model");
                return;
            }
            ForgetWeaponModels();
            RescanModels();
            m_app->GetConsole().Print("Imported " + std::to_string(imported.parts.size()) +
                                      " parts as " + args[2] +
                                      ". Open it with: editor " + args[2]);
        },
        "model_import <file.glb> <name> [size] [turn x y z]");

    console.RegisterCommand(
        "model_info", "Print a model's extents and sockets: model_info <name>",
        [this](const std::vector<std::string>& args)
        {
            // Placing a socket needs to know where the geometry actually is, and an imported model
            // arrives in whatever frame its author used. This says: how big it is, which way the
            // long axis runs, and what sockets it already has.
            Console& out = m_app->GetConsole();
            if (args.size() < 2)
            {
                out.PrintError("usage: model_info <name>");
                return;
            }
            ModelAsset model;
            const std::filesystem::path file = ModelPath(args[1]);
            if (file.empty() || !model.LoadFromFile(file))
            {
                out.PrintError("no model called " + args[1]);
                return;
            }
            glm::vec3 low{std::numeric_limits<float>::max()};
            glm::vec3 high{std::numeric_limits<float>::lowest()};
            size_t vertices = 0;
            for (const ModelPart& part : model.parts)
            {
                const glm::mat4 rest = part.LocalMatrix();
                for (const MeshVertex& vertex : part.mesh.vertices)
                {
                    const glm::vec3 at = glm::vec3(rest * glm::vec4(vertex.position, 1.0f));
                    low = glm::min(low, at);
                    high = glm::max(high, at);
                    ++vertices;
                }
            }
            char buffer[220];
            if (vertices == 0)
            {
                out.Print("no geometry");
            }
            else
            {
                std::snprintf(buffer, sizeof(buffer),
                              "%zu parts, %zu vertices, extents x %.3f..%.3f  y %.3f..%.3f  z "
                              "%.3f..%.3f  (size %.3f x %.3f x %.3f)",
                              model.parts.size(), vertices, low.x, high.x, low.y, high.y, low.z,
                              high.z, high.x - low.x, high.y - low.y, high.z - low.z);
                out.Print(buffer);
                PRED_LOG_INFO(Asset, "{}: {}", args[1], buffer);
            }
            for (const ModelSocket& socket : model.sockets)
            {
                std::snprintf(buffer, sizeof(buffer), "  socket %-10s %7.3f %7.3f %7.3f",
                              socket.name.c_str(), socket.position.x, socket.position.y,
                              socket.position.z);
                out.Print(buffer);
                PRED_LOG_INFO(Asset, "{}", buffer);
            }
        },
        "model_info <name>");

    console.RegisterCommand(
        "model_export",
        "Write a weapon's built-in shape out as an editable model: model_export <weapon> [model name]",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 2)
            {
                m_app->GetConsole().PrintError("usage: model_export <weapon> [model name]");
                return;
            }
            const WeaponDefinition* weapon = m_weaponData.Find(args[1]);
            if (weapon == nullptr)
            {
                m_app->GetConsole().PrintError("no such weapon: " + args[1]);
                return;
            }
            const std::string modelName = args.size() >= 3 ? args[2] : weapon->key;
            if (ExportWeaponModel(*weapon, modelName))
            {
                m_app->GetConsole().Print("Wrote model '" + modelName +
                                          "'. Open it with: editor " + modelName);
                m_app->GetConsole().Print("Add \"model\": \"" + modelName +
                                          "\" to the weapon in weapons.json to use it in the game.");
            }
        },
        "model_export <weapon> [model]");
#endif // PRED_DEV_TOOLS

    console.RegisterCommand(
        "look", "Point the view, for inspecting poses without a mouse: look <yaw> [pitch]",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 2)
            {
                m_app->GetConsole().PrintError("usage: look <yaw> [pitch]");
                return;
            }
            m_lookYaw = glm::radians(std::strtof(args[1].c_str(), nullptr));
            if (args.size() >= 3)
            {
                m_lookPitch = glm::radians(std::strtof(args[2].c_str(), nullptr));
            }
        },
        "look <yaw> [pitch]");

    console.RegisterCommand("interact", "Use whatever the player is looking at",
                            [this](const std::vector<std::string>&) { TryInteract(); });

    console.RegisterCommand("drop", "Drop the selected item in front of the player",
                            [this](const std::vector<std::string>&) { DropSelected(); });

    console.RegisterCommand("inventory", "Open or close the inventory panel",
                            [this](const std::vector<std::string>&)
                            {
                                m_inventoryOpen = !m_inventoryOpen;
                                UpdateMouseCapture();
                            });

    console.RegisterCommand(
        "sound_bake",
        "Write every sound recipe out as a wav in Assets/Audio, for replacing by hand",
        [this](const std::vector<std::string>&)
        {
            // Turns the generated sounds into files somebody can replace.
            //
            // A recipe in a JSON file is a fine placeholder while there is nobody to record
            // anything and a dead end the moment there is: it cannot be opened in an editor, sent
            // to anybody, or swapped without learning what `bite` means. This writes each one into
            // the folder the game already prefers recordings from, so replacing a sound is dropping
            // a file on top of another file.
            //
            // A command rather than a build step because it is run when the recipes change, which
            // is rarely, and baking on every build would rewrite twelve files nobody asked about.
            Console& out = m_app->GetConsole();
            const std::filesystem::path recipes = Paths::AssetsRoot() / "Data" / "sounds.json";
            std::ifstream file(recipes);
            if (!file)
            {
                out.PrintError("No " + recipes.string());
                return;
            }
            const std::string text((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
            const std::vector<SoundLibraryEntry> entries = LoadSoundRecipes(text);
            if (entries.empty())
            {
                out.PrintError("No recipes in " + recipes.filename().string());
                return;
            }

            int written = 0;
            for (const SoundLibraryEntry& entry : entries)
            {
                const SoundData data = Synthesise(entry.recipe, 48000);
                const std::vector<uint8_t> bytes = SaveWav(data);
                const std::filesystem::path folder = Paths::AssetsRoot() / "Audio" / entry.name;
                std::error_code ec;
                std::filesystem::create_directories(folder, ec);
                // Named for the sound and numbered, because the loader takes every wav in the
                // folder as a variant and picks between them. One now; drop in _2 and _3 and the
                // game alternates without being told.
                const std::filesystem::path target = folder / (entry.name + "_1.wav");
                std::ofstream wav(target, std::ios::binary | std::ios::trunc);
                if (!wav)
                {
                    out.PrintError("Could not write " + target.string());
                    continue;
                }
                wav.write(reinterpret_cast<const char*>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size()));
                ++written;
            }
            char buffer[160];
            std::snprintf(buffer, sizeof(buffer), "%d sound(s) written to Assets/Audio", written);
            out.Print(buffer);
        });

    console.RegisterCommand(
        "mic_probe", "List the recording devices and try to open the chosen one",
        [this](const std::vector<std::string>&)
        {
            // The settings panel says "microphone shut" when the device will not open, and that is
            // all it can say: it has one bool to work with. This says why, which is the difference
            // between a setting somebody can fix and one they can only stare at.
            Console& out = m_app->GetConsole();
            const std::vector<VoiceCapture::Device> devices = VoiceCapture::Devices();
            if (devices.empty())
            {
                out.PrintError("No recording devices at all. Windows may be denying microphone "
                               "access to this application: Settings > Privacy > Microphone.");
            }
            for (const VoiceCapture::Device& device : devices)
            {
                out.Print("  " + std::to_string(device.id) + "  " + device.name +
                          (static_cast<int>(device.id) == cv_voiceDevice.Get() ? "   <- chosen"
                                                                              : ""));
            }

            // And actually open it, because a device that lists and will not open is the common
            // case and the list alone looks like everything is fine.
            VoiceCapture probe;
            VoiceCapture::Settings settings;
            settings.sampleRate = 48000;
            settings.deviceId = static_cast<uint32_t>(std::max(cv_voiceDevice.Get(), 0));
            if (!probe.Start(settings))
            {
                out.PrintError("Could not open it: " + probe.Message());
                return;
            }
            out.Print("Opened. Say something and run this again to see whether it heard you.");
            probe.Stop();
        });

    console.RegisterCommand(
        "lobby_host", "Open a relay lobby from the console, for testing without the menu",
        [this](const std::vector<std::string>&)
        {
            StartLobby(true, 0);
            m_app->GetConsole().Print("Asking the relay for a lobby...");
        });

    console.RegisterCommand(
        "lobby_join", "Join a relay lobby by code: lobby_join <code>",
        [this](const std::vector<std::string>& args)
        {
            uint32_t code = 0;
            if (args.size() < 2 || !EncodeRelayCode(args[1], code))
            {
                m_app->GetConsole().PrintError("usage: lobby_join <six character code>");
                return;
            }
            StartLobby(false, code);
            m_app->GetConsole().Print("Looking for lobby " + args[1] + "...");
        },
        "lobby_join <code>");

    console.RegisterCommand(
        "lobby_state", "Say what the relay connection is doing",
        [this](const std::vector<std::string>&)
        {
            Console& out = m_app->GetConsole();
            if (m_relay == nullptr)
            {
                out.Print("No lobby.");
                return;
            }
            const char* what = "?";
            switch (m_relay->Status())
            {
            case RelayCarrier::State::Idle: what = "idle"; break;
            case RelayCarrier::State::Connecting: what = "waiting for the relay"; break;
            case RelayCarrier::State::Ready: what = "in a lobby"; break;
            case RelayCarrier::State::Failed: what = "failed"; break;
            }
            out.Print(std::string("Relay: ") + what + ", code " + m_relay->CodeText() + ", " +
                      std::to_string(m_relay->Links()) + " other(s)");
            if (!m_relay->Message().empty())
            {
                out.Print("  " + m_relay->Message());
            }
        });

    console.RegisterCommand("scene_stats", "Print scene and mesh statistics",
                            [this](const std::vector<std::string>&)
                            {
                                const SceneRenderer::Stats& stats = m_app->GetSceneRenderer().LastStats();
                                char buffer[220];
                                std::snprintf(buffer, sizeof(buffer),
                                              "entities %zu   meshes %zu   submitted %zu   triangles %zu",
                                              m_scene.EntityCount(), m_app->GetMeshes().Count(),
                                              stats.meshesSubmitted, stats.trianglesSubmitted);
                                m_app->GetConsole().Print(buffer);
                            });

    console.RegisterCommand(
        "phys_drop",
        "Spawn a dynamic prop in front of the view: phys_drop [sphere|box] [count] [speed]",
        [this](const std::vector<std::string>& args)
        {
            const bool sphere = args.size() < 2 || args[1] != "box";
            const int count = args.size() > 2 ? std::atoi(args[2].c_str()) : 1;
            // Thrown by default, which is what a person at the console wants. Zero sets it down
            // where it is, which is what a scripted run wants: a ball that has been thrown is
            // somewhere out of shot by the time anything is aimed at it.
            const float speed = args.size() > 3 ? std::strtof(args[3].c_str(), nullptr) : 6.0f;
            for (int i = 0; i < std::clamp(count, 1, 64); ++i)
            {
                SpawnProp(sphere, speed);
            }
            m_app->GetConsole().Print("Props in world: " + std::to_string(m_props.size()));
        },
        "phys_drop [sphere|box] [count] [speed]");

    console.RegisterCommand("phys_clear", "Remove every dynamic prop",
                            [this](const std::vector<std::string>&)
                            {
                                const size_t removed = m_props.size();
                                ClearProps();
                                m_app->GetConsole().Print("Removed " + std::to_string(removed) + " props");
                            });
}

// --- World replication -------------------------------------------------------------------------
//
// One rule decides every question here: the host owns the world. A client that opens a door has
// asked to open it, and what it draws is what the host sends back. Offline the local player is the
// host, so single player and hosting are the same code with nobody to send to.

uint8_t PredationGame::LocalPlayerId() const
{
    return m_sessionMode == SessionMode::Client ? m_client.PlayerId() : 0;
}

glm::vec3 PredationGame::PlayerPosition(uint8_t player) const
{
    if (player == LocalPlayerId())
    {
        return m_player.State().position;
    }
    for (const RemotePlayerView& remote : RemotePlayers())
    {
        if (remote.id == player)
        {
            return remote.position;
        }
    }
    return m_player.State().position;
}

bool PredationGame::PerformInteraction(InteractionKind kind, int index, uint8_t player)
{
    switch (kind)
    {
    case InteractionKind::Door:
    {
        if (!m_world.ToggleDoor(index, m_interactions))
        {
            return false;
        }
        // Heard by whoever did it, which is where every interaction sound in this function was
        // missing from.
        //
        // The sounds existed and were only ever played from the network event handlers -- the code
        // that runs on a machine being *told* something happened. The machine that actually did it
        // never runs those, so the player opening a door heard nothing, and in a solo game, where
        // there are no events at all, nothing made any sound ever. A client is not double-served
        // either: it sends a request and returns without performing, so it only hears the event.
        if (const WorldObjects::Door* opened = m_world.GetDoor(index); opened != nullptr)
        {
            PlaySound(m_sounds.door.Pick(), opened->hinge, 0.8f);
        }
        if (m_sessionMode == SessionMode::Host)
        {
            const WorldObjects::Door* door = m_world.GetDoor(index);
            WorldEventMessage event;
            event.kind = WorldEventKind::DoorMoved;
            event.index = static_cast<uint8_t>(index);
            event.flag = door != nullptr && std::abs(door->target - door->closedYaw) > 1e-3f;
            m_host.Broadcast(event);
        }
        return true;
    }

    case InteractionKind::Pickup:
    {
        WorldObjects::Pickup* pickup = m_world.GetPickup(index);
        if (pickup == nullptr || !pickup->alive)
        {
            return false;
        }
        // Only the player who asked gets it in their bag. Everyone else just sees it disappear,
        // which is all there is to see from outside.
        // Taken before the pickup is consumed, because consuming it destroys the entity this reads.
        glm::vec3 pickupAt{0.0f};
        if (const Transform* where = m_scene.GetTransform(pickup->entity); where != nullptr)
        {
            pickupAt = where->position;
        }
        const bool mine = player == LocalPlayerId();
        const int wanted = pickup->count;
        int stored = wanted;
        if (mine)
        {
            // The magazine comes with it, so a rifle dropped with three rounds left is picked up
            // with three rather than refilled by the act of taking it.
            stored = m_inventory.Add(m_items, pickup->item, pickup->count, pickup->rounds,
                                     pickup->reserve);
            if (stored <= 0)
            {
                m_app->GetConsole().Print("Inventory full");
                return false;
            }
        }

        if (stored >= wanted)
        {
            const ItemId taken = pickup->item;
            PlaySound(m_sounds.pickup.Pick(), pickupAt, 0.6f);
            m_world.ConsumePickup(index, m_scene, m_app->GetPhysics(), m_interactions);
            if (m_sessionMode == SessionMode::Host)
            {
                // Remembered, so the same client cannot later put down more than it took.
                if (player != 0)
                {
                    m_host.NoteCarried(player, static_cast<uint16_t>(taken), wanted);
                }
                WorldEventMessage event;
                event.kind = WorldEventKind::PickupTaken;
                event.index = static_cast<uint8_t>(index);
                event.player = player;
                m_host.Broadcast(event);
            }
        }
        else
        {
            // Partial pickup: leave the remainder on the floor rather than silently eating it.
            pickup->count -= stored;
            if (Interactable* interactable = m_interactions.Find(pickup->entity))
            {
                const ItemDefinition* definition = m_items.Get(pickup->item);
                interactable->name = definition != nullptr
                                         ? definition->name + " x" + std::to_string(pickup->count)
                                         : "Item";
            }
        }
        return true;
    }

    case InteractionKind::HidingSpot:
    {
        // One interaction, both directions: get in if it is empty, get out if you are the one in it.
        //
        // It used to be "get in" only. Leaving was a local call that told nobody, so everybody else
        // kept the locker shut and occupied for the rest of the match, and the player who had been
        // inside it could never be seen to come out. Making it a toggle means the same message, the
        // same path and the same broadcast carry both halves.
        WorldObjects::HidingSpot* spot = m_world.GetHidingSpot(index);
        if (spot == nullptr)
        {
            return false;
        }
        const bool leaving = spot->occupied && spot->occupant == player;
        if (spot->occupied && !leaving)
        {
            return false; // somebody else is in there
        }
        PlaySound(m_sounds.locker.Pick(), spot->insidePosition, 0.8f);

        if (leaving)
        {
            spot->occupied = false;
            spot->occupant = 0;
            m_world.SetDoorOpen(spot->doorIndex, true, m_interactions);
            m_interactions.SetVerb(spot->entity, "Hide in");
            if (player == LocalPlayerId())
            {
                LeaveHidingSpot();
            }
        }
        else if (player == LocalPlayerId())
        {
            EnterHidingSpot(index);
            spot->occupant = player;
        }
        else
        {
            // Somebody else got in. Their body is drawn from their replicated position anyway, so
            // all this machine has to do is swing the door and mark the locker taken.
            spot->occupied = true;
            spot->occupant = player;
            m_world.SetDoorOpen(spot->doorIndex, false, m_interactions);
        }

        if (m_sessionMode == SessionMode::Host)
        {
            WorldEventMessage event;
            event.kind = WorldEventKind::LockerUsed;
            event.index = static_cast<uint8_t>(index);
            event.player = player;
            event.flag = !leaving;
            m_host.Broadcast(event);
        }
        return true;
    }

    case InteractionKind::AmmoCrate:
    {
        if (player == LocalPlayerId())
        {
            TakeAmmunition(index);
        }
        else if (!m_world.DrawFromAmmoCrate(index, m_interactions))
        {
            return false;
        }
        if (m_sessionMode == SessionMode::Host)
        {
            WorldEventMessage event;
            event.kind = WorldEventKind::AmmoTaken;
            event.index = static_cast<uint8_t>(index);
            event.player = player;
            m_host.Broadcast(event);
        }
        return true;
    }

    case InteractionKind::Generic:
    default:
        return false;
    }
}

void PredationGame::ServeClientRequests()
{
    if (m_sessionMode != SessionMode::Host)
    {
        return;
    }

    for (const NetHost::InteractRequest& request : m_host.TakeInteractRequests())
    {
        if (request.kind >= static_cast<uint8_t>(InteractionKind::AmmoCrate) + 1)
        {
            continue;
        }
        const auto kind = static_cast<InteractionKind>(request.kind);

        // A client says what it wants, never where it is. The host checks the distance itself
        // against the position it simulated, so reach cannot be claimed.
        const Interactable* target = m_interactions.FindByPayload(kind, request.index);
        if (target == nullptr || !target->enabled)
        {
            continue;
        }
        const Transform* transform = m_scene.GetTransform(target->entity);
        if (transform == nullptr)
        {
            continue;
        }
        const glm::vec3 focus = transform->position + target->focusOffset;
        const glm::vec3 from = PlayerPosition(request.player) + glm::vec3(0.0f, 1.2f, 0.0f);
        // A little slack over the prompt's own range, because the client asked at a position the
        // host has since moved them on from.
        if (glm::distance(focus, from) > target->range + 1.0f)
        {
            PRED_LOG_DEBUG(Network, "Player {} asked for something out of reach", request.player);
            continue;
        }
        PerformInteraction(kind, request.index, request.player);
    }

    for (const NetHost::ShotRequest& request : m_host.TakeShotRequests())
    {
        // The client has already played the shot for itself. The host decides what it hit.
        const WeaponDefinition* definition = EquippedWeapon();
        FireEvent shot;
        shot.origin = request.shot.origin;
        shot.direction = glm::normalize(request.shot.direction);
        shot.range = definition != nullptr ? definition->range : 60.0f;
        shot.damage = definition != nullptr ? definition->damage : 20.0f;
        shot.sequence = request.shot.shotNumber;

        ShotResult result = ResolveShot(m_app->GetPhysics(), shot);
        // Rewound to the moment this client says it was looking at, clamped by the host to what a
        // playable connection could justify.
        ResolvePlayerHits(shot, request.player, result, m_host.PosesAt(request.shot.renderTick));

        WorldEventMessage event;
        event.kind = WorldEventKind::ShotFired;
        event.player = request.player;
        // The trace starts at the eye, so that what is under the crosshair is what gets hit. The
        // drawn line has to start at the muzzle instead, or everyone else watches rounds come out
        // of the shooter's face. Where their muzzle is, is where this machine is drawing their gun.
        event.position = MuzzleOf(request.player, shot.origin, shot.direction);
        event.direction = result ? result.position : shot.origin + shot.direction * shot.range;
        event.flag = result.hit;
        event.flag2 = result.surface;
        m_host.Broadcast(event);

        Tracer tracer;
        // The host has the eye this was traced from, because the client sent it.
        tracer.from = event.position;
        tracer.origin = shot.origin;
        tracer.to = event.direction;
        tracer.hit = event.flag;
        tracer.surface = event.flag2;
        tracer.normal = SurfaceNormalAt(m_app->GetPhysics(), tracer.origin, tracer.to, &tracer.body);
        AnchorTracer(tracer);
        m_tracers.push_back(tracer);

        // The host draws the shooter too, so their weapon has to kick here as well. The event goes
        // out to everybody else; nobody sends it back to the machine that made it.
        if (RemoteAvatar* avatar = AvatarFor(request.player))
        {
            avatar->weaponKick = 1.0f;
        }
    }

    for (const NetHost::DropRequest& request : m_host.TakeDropRequests())
    {
        // Only what the host actually handed them. Otherwise dropping is a way to make items out of
        // nothing, which is what let one get duplicated.
        if (!m_host.TakeCarried(request.player, request.drop.item, request.drop.count))
        {
            continue;
        }
        const int index = m_world.SpawnPickup(
            m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_items,
            static_cast<ItemId>(request.drop.item), request.drop.count, request.drop.position,
            request.drop.velocity, LoadFromWire(request.drop.rounds),
            LoadFromWire(request.drop.reserve));
        if (index < 0)
        {
            continue;
        }
        m_host.Broadcast(PickupSpawnedEvent(static_cast<uint8_t>(index), request.drop.item,
                                            request.drop.count, request.drop.rounds,
                                            request.drop.reserve, request.drop.position,
                                            request.drop.velocity));
    }

    for (const uint8_t player : m_host.TakeJoined())
    {
        SendWorldToPlayer(player);
    }

    // Whatever somebody walked out with goes back on the floor where they were standing. It came
    // out of the world and it belongs to the world: a player who quits holding the keycard
    // otherwise takes it with them, and everybody else is left looking at an item hanging in the
    // air where their body used to be.
    for (const NetHost::Departure& departure : m_host.TakeDeparted())
    {
        for (const auto& [item, count] : departure.carried)
        {
            const int index = m_world.SpawnPickup(m_scene, m_app->GetMeshes(), m_app->GetPhysics(),
                                                  m_interactions, m_items, static_cast<ItemId>(item),
                                                  count, departure.position + glm::vec3(0.0f, 0.4f, 0.0f),
                                                  glm::vec3(0.0f));
            if (index < 0)
            {
                continue;
            }
            WorldEventMessage event;
            event.kind = WorldEventKind::PickupSpawned;
            event.index = static_cast<uint8_t>(index);
            event.item = item;
            event.other = static_cast<uint8_t>(count);
            event.position = departure.position + glm::vec3(0.0f, 0.4f, 0.0f);
            m_host.Broadcast(event);
        }

    }
}

void PredationGame::SendWorldToPlayer(uint8_t player)
{
    // Somebody who has just walked in has to be told what has already happened, or every door that
    // was opened before they arrived is shut on their screen for the rest of the game.
    for (size_t i = 0; i < m_world.Doors().size(); ++i)
    {
        const WorldObjects::Door& door = m_world.Doors()[i];
        if (!door.IsOpen())
        {
            continue;
        }
        WorldEventMessage event;
        event.kind = WorldEventKind::DoorMoved;
        event.index = static_cast<uint8_t>(i);
        event.flag = true;
        m_host.SendTo(player, event);
    }

    // Every slot, in both directions. Telling them only what has been taken leaves out everything
    // that has been dropped since, and leaves a slot that has been taken and then filled with
    // something else showing whatever the map originally put there. Taking first and then spawning
    // is also how a slot gets corrected: a spawn at an occupied index replaces what is there.
    for (size_t i = 0; i < m_world.Pickups().size(); ++i)
    {
        if (m_world.Pickups()[i].alive)
        {
            continue;
        }
        WorldEventMessage event;
        event.kind = WorldEventKind::PickupTaken;
        event.index = static_cast<uint8_t>(i);
        event.player = 0;
        m_host.SendTo(player, event);
    }

    for (size_t i = 0; i < m_world.Pickups().size(); ++i)
    {
        const WorldObjects::Pickup& pickup = m_world.Pickups()[i];
        if (!pickup.alive)
        {
            continue;
        }
        WorldEventMessage event;
        event.kind = WorldEventKind::PickupSpawned;
        event.index = static_cast<uint8_t>(i);
        event.item = static_cast<uint16_t>(pickup.item);
        event.other = static_cast<uint8_t>(pickup.count);
        event.rounds = LoadToWire(pickup.rounds);
        event.reserve = LoadToWire(pickup.reserve);
        event.position = m_app->GetPhysics().IsValid(pickup.body)
                             ? m_app->GetPhysics().GetTransform(pickup.body).position
                             : pickup.netPosition;
        m_host.SendTo(player, event);
    }

    for (size_t i = 0; i < m_world.HidingSpots().size(); ++i)
    {
        if (!m_world.HidingSpots()[i].occupied)
        {
            continue;
        }
        WorldEventMessage event;
        event.kind = WorldEventKind::LockerUsed;
        event.index = static_cast<uint8_t>(i);
        event.player = 0;
        event.flag = true;
        m_host.SendTo(player, event);
    }
}

PredationGame::RemoteAvatar* PredationGame::AvatarFor(uint8_t id)
{
    for (auto& candidate : m_avatars)
    {
        if (candidate->id == id)
        {
            return candidate.get();
        }
    }
    return nullptr;
}
void PredationGame::ApplyWorldEvent(const WorldEventMessage& event)
{
    switch (event.kind)
    {
    case WorldEventKind::DoorMoved:
        m_world.SetDoorOpen(event.index, event.flag, m_interactions);
        // Heard where the door is rather than where the player is, which is the point of a door
        // opening somewhere else in the building.
        if (const WorldObjects::Door* moved = m_world.GetDoor(event.index); moved != nullptr)
        {
            PlaySound(m_sounds.door.Pick(), moved->hinge, 0.8f);
        }
        break;

    case WorldEventKind::PickupTaken:
    {
        // This is the host answering. Asking put nothing in the bag, because asking is all a client
        // does, so the item goes in now. Without this the thing simply vanished: the host took it
        // out of the world and nobody ever gave it to anyone.
        //
        // What it was is read from this machine's own copy rather than sent, because both ends
        // built the same world and have applied the same events, so the index means the same thing
        // on both. Sending it again would be sending something already known.
        const WorldObjects::Pickup* pickup = m_world.GetPickup(event.index);
        if (pickup != nullptr && pickup->alive && event.player == LocalPlayerId())
        {
            // With whatever it was carrying, which is the whole point of a dropped weapon keeping
            // its magazine. This asked for the item and the count and nothing else, so a client
            // picking up a rifle with three rounds left in it got a full one, and a client picking
            // up a full one that the host had emptied got an empty one: the ammunition was the one
            // thing about a pickup that only ever crossed the wire in one direction.
            const int stored = m_inventory.Add(m_items, pickup->item, pickup->count, pickup->rounds,
                                               pickup->reserve);
            if (stored < pickup->count)
            {
                // The bag filled between asking and being answered. Rare, and worth saying out loud
                // rather than losing the difference in silence.
                PRED_LOG_WARN(Gameplay, "Only had room for {} of {}", stored, pickup->count);
            }
        }
        PlaySound(m_sounds.pickup.Pick(), event.position, 0.6f, 1.0f, event.player != LocalPlayerId());
        m_world.ConsumePickup(event.index, m_scene, m_app->GetPhysics(), m_interactions);
        break;
    }

    case WorldEventKind::PickupSpawned:
        // At the index the host chose, not one of this machine's own choosing. Everything after
        // this refers to the pickup by that number.
        m_world.SpawnPickup(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_items,
                            static_cast<ItemId>(event.item), static_cast<int>(event.other),
                            event.position, event.direction, LoadFromWire(event.rounds),
                            LoadFromWire(event.reserve), static_cast<int>(event.index));
        PlaySound(m_sounds.drop.Pick(), event.position, 0.7f);
        break;

    case WorldEventKind::LockerUsed:
        if (WorldObjects::HidingSpot* spot = m_world.GetHidingSpot(event.index))
        {
            spot->occupied = event.flag;
            spot->occupant = event.flag ? event.player : 0;
            m_world.SetDoorOpen(spot->doorIndex, !event.flag, m_interactions);
            // Including when it is about us.
            //
            // A client asks and does nothing else: the host decides, and the answer comes back as
            // this event. Skipping it when the name matched -- on the reasoning that we had already
            // done it when we asked -- meant a client could never get into a locker at all. It is
            // only the host that has already applied its own, and the host does not receive its own
            // broadcasts.
            if (event.player == LocalPlayerId() && m_sessionMode != SessionMode::Host)
            {
                if (event.flag)
                {
                    EnterHidingSpot(static_cast<int>(event.index));
                }
                else if (m_hidingSpot == static_cast<int>(event.index))
                {
                    LeaveHidingSpot();
                }
            }
            if (!event.flag)
            {
                m_interactions.SetVerb(spot->entity, "Hide in");
            }
        }
        break;

    case WorldEventKind::AmmoTaken:
        if (event.player != LocalPlayerId())
        {
            m_world.DrawFromAmmoCrate(event.index, m_interactions);
        }
        break;

    case WorldEventKind::ShotFired:
        if (event.player != LocalPlayerId())
        {
            // Somebody else's round. Drawn, never resolved: what it hit was decided by the host.
            Tracer tracer;
            tracer.from = event.position;
            // Their eye never crosses the wire, so the muzzle is the best origin there is.
            tracer.origin = event.position;
            tracer.to = event.direction;
            tracer.hit = event.flag;
            tracer.surface = event.flag2;
            tracer.normal = SurfaceNormalAt(m_app->GetPhysics(), tracer.origin, tracer.to, &tracer.body);
            AnchorTracer(tracer);
            m_tracers.push_back(tracer);
            // And it is heard where it was fired from, which is most of what tells a player there
            // is somebody else in the building and roughly where.
            PlaySound(m_sounds.gunshot.Pick(), event.position, 1.0f, 1.0f, true);

            // And their weapon kicks and flashes. A snapshot cannot carry this: firing happens on
            // one frame and snapshots go out on others, so the moment would be missed most times.
            // It rides the shot message instead, which is the one thing that is sent exactly when
            // a round leaves the barrel.
            if (RemoteAvatar* avatar = AvatarFor(event.player))
            {
                avatar->weaponKick = 1.0f;
            }
        }
        break;

    case WorldEventKind::PlayerDamaged:
        if (event.player == LocalPlayerId())
        {
            // The host is the authority on health, so this is set rather than subtracted.
            m_player.State().health = std::max(m_player.State().health - event.amount, 0.0f);
            PlaySound(m_sounds.hurt.Pick(), m_player.State().position, 0.8f, 1.0f, false);
        }
        break;

    case WorldEventKind::PlayerDied:
        if (event.player == LocalPlayerId())
        {
            m_player.State().health = 0.0f;
            m_player.State().alive = false;
            m_deathImpulse = event.direction;
            PlaySound(m_sounds.death.Pick(), m_player.State().position, 1.0f, 1.0f, false);
        }
        break;

    case WorldEventKind::PlayerRespawned:
        if (event.player == LocalPlayerId())
        {
            RespawnLocalPlayer(event.position);
        }
        break;

    case WorldEventKind::Count:
        break;
    }
}

void PredationGame::SendDynamicBodies()
{
    // Nobody to send to is the common case for a game somebody opened and then played alone, and
    // walking every loose body in the world to build a packet for an empty room is the most
    // expensive thing hosting does.
    if (m_sessionMode != SessionMode::Host || m_host.ConnectedCount() == 0)
    {
        return;
    }

    // At the snapshot rate, not the tick rate. Sending loose objects sixty times a second doubles
    // what they cost for a value nobody can see change twice in a thirtieth of a second, and it is
    // the largest message the host sends.
    m_worldStateTimer += 1.0f / 60.0f;
    constexpr float kInterval = 1.0f / 30.0f;
    if (m_worldStateTimer < kInterval)
    {
        return;
    }
    m_worldStateTimer = std::fmod(m_worldStateTimer, kInterval);

    // Loose objects are sent as state rather than as events: a crate sliding across the floor is a
    // value that will be sent again in a thirtieth of a second, so losing one costs nothing.
    WorldStateMessage state;
    PhysicsWorld& physics = m_app->GetPhysics();

    for (size_t i = 0; i < m_world.Pickups().size() && state.count < kMaxDynamicBodies; ++i)
    {
        const WorldObjects::Pickup& pickup = m_world.Pickups()[i];
        if (!pickup.alive || !physics.IsValid(pickup.body))
        {
            continue;
        }
        const Transform transform = physics.GetTransform(pickup.body);
        DynamicBodyState& entry = state.bodies[state.count++];
        entry.id = static_cast<uint8_t>(i);
        entry.position = transform.position;
        entry.rotation = transform.rotation;
    }

    m_host.SendWorldState(state);
}

void PredationGame::ApplyDynamicBodies(const WorldStateMessage& state)
{
    // Recorded, not applied. A client running its own physics for a dropped rifle would disagree
    // with everyone else within a second, so the host is the authority; but the host speaks thirty
    // times a second and the screen draws at least twice that, so setting the transform on arrival
    // drew a falling item in visible steps. WorldObjects::FollowNetworkState eases towards these
    // every frame instead.
    for (uint8_t i = 0; i < state.count; ++i)
    {
        const DynamicBodyState& entry = state.bodies[i];
        m_world.SetNetworkState(entry.id, entry.position, entry.rotation);
    }
}

// --- Damage ------------------------------------------------------------------------------------

void PredationGame::UpdateHostMigration(float dt)
{
    // The host has gone. Rather than the game ending for everybody left, the lowest surviving
    // player number takes over and the rest connect to it. Everyone was given the same roster while
    // the old host was alive, so everyone reaches the same answer without having to agree on one,
    // which matters because the machine they would have agreed through is the one that left.
    //
    // The world survives the handover because every machine already has a complete copy of it: that
    // is what applying the same events to the same starting state buys. The new host's copy becomes
    // the authoritative one.
    if (m_sessionMode != SessionMode::Client || !m_client.HostLost())
    {
        return;
    }

    m_migrationTimer += dt;
    // A moment of quiet first. A host that drops off for a second and comes back would otherwise
    // race with the successor for the same port, and the settling time is also long enough that
    // everybody has noticed rather than only the client with the best connection.
    if (m_migrationTimer < cv_migrationSeconds.Get())
    {
        return;
    }

    const bool takeOver = m_client.ShouldBecomeHost();
    const std::string successor = m_client.SuccessorAddress();
    // Where everyone will look for the new host: the port they were already on.
    const uint16_t port = m_client.SessionPort();
    m_migrationTimer = 0.0f;

    if (takeOver)
    {
        PRED_LOG_INFO(Network, "Host went; taking over on port {}", port);
        m_client.Disconnect();
        m_sessionMode = SessionMode::Offline;

        NetHost::Config config;
        config.port = port;
        auto transport = CreateUdpTransport();
        transport->SetConditions(m_simulatedConditions);
        if (m_host.Start(std::move(transport), config, m_app->GetPhysics(), m_player.Config(),
                         m_spawnPoint))
        {
            m_sessionMode = SessionMode::Host;
            m_app->GetConsole().Print("The host left. You are hosting now.");
        }
        else
        {
            m_app->GetConsole().PrintError("The host left and this machine could not take over.");
            ReturnToTitle();
        }
        return;
    }

    if (successor.empty())
    {
        // Nobody left to play with.
        m_app->GetConsole().Print("The host left and there is nobody else here.");
        ReturnToTitle();
        return;
    }

    PRED_LOG_INFO(Network, "Host went; following {}", successor);
    auto transport = CreateUdpTransport();
    transport->SetConditions(m_simulatedConditions);
    NetClient::Config config;
    // The address carries its own port, because it is where that machine was seen from, not where
    // it listens. The successor listens on the game's port.
    const std::string host = successor.substr(0, successor.find(':'));
    if (m_client.Connect(std::move(transport), host, port, PlayerName(), config))
    {
        m_app->GetConsole().Print("The host left. Reconnecting to " + host);
    }
    else
    {
        m_app->GetConsole().PrintError("The host left and " + host + " could not be reached.");
        ReturnToTitle();
    }
}

void PredationGame::UpdateRespawns(float dt)
{
    // Death is a pause, not an end. Offline and as a host the clock is run here; a client is told
    // when it comes back, because whether it is alive is not its own decision to make.
    if (m_sessionMode == SessionMode::Client)
    {
        return;
    }

    // A client does not decide when it comes back, and this ran everywhere.
    //
    // Both ends counted the same seconds and each revived the player it owned, which agrees only
    // while both ends agree that the player died in the first place. A client that killed itself
    // locally, on fall damage from a climb the host had not run the same way, then revived itself
    // too: alive and playing on its own screen and lying on the floor for good on everybody else's.
    // Dying and coming back are the host's to say, like everything else about the world.
    if (m_sessionMode != SessionMode::Client && !m_player.State().alive && m_respawnTimer > 0.0f)
    {
        m_respawnTimer -= dt;
        if (m_respawnTimer <= 0.0f)
        {
            RespawnLocalPlayer(m_spawnPoint);
            if (m_sessionMode == SessionMode::Host)
            {
                WorldEventMessage event;
                event.kind = WorldEventKind::PlayerRespawned;
                event.player = 0;
                event.position = m_spawnPoint;
                m_host.Broadcast(event);
            }
        }
    }

    if (m_sessionMode != SessionMode::Host)
    {
        return;
    }
    for (auto& entry : m_remoteRespawnTimers)
    {
        if (entry.second <= 0.0f)
        {
            continue;
        }
        entry.second -= dt;
        if (entry.second > 0.0f)
        {
            continue;
        }
        m_host.RespawnPlayer(entry.first, m_spawnPoint);
        WorldEventMessage event;
        event.kind = WorldEventKind::PlayerRespawned;
        event.player = entry.first;
        event.position = m_spawnPoint;
        m_host.Broadcast(event);
    }
}

void PredationGame::UpdateSpectating()
{
    // Dead, you watch a teammate through their own eyes. A free camera is deliberately not offered:
    // it would show you where the creature is, which is the one thing being dead should not tell
    // you. Cycling is by the interact key, because there is nothing else to press.
    if (m_player.State().alive)
    {
        m_spectating = -1;
        m_spectateEyeHeight = 0.0f;
        return;
    }

    const std::vector<RemotePlayerView>& remotes = RemotePlayers();
    const auto living = [&](int id)
    {
        for (const RemotePlayerView& remote : remotes)
        {
            if (remote.id == id && remote.alive)
            {
                return true;
            }
        }
        return false;
    };

    // Asked to watch somebody else, which is the only control a dead player has.
    if (m_spectateNext && m_spectating >= 0)
    {
        // The next living player after the one being watched, wrapping. Everybody is on the same
        // roster in the same order, so "next" means the same thing on every machine.
        for (size_t step = 1; step <= remotes.size(); ++step)
        {
            size_t at = 0;
            for (size_t i = 0; i < remotes.size(); ++i)
            {
                if (static_cast<int>(remotes[i].id) == m_spectating)
                {
                    at = i;
                    break;
                }
            }
            const RemotePlayerView& candidate = remotes[(at + step) % remotes.size()];
            if (candidate.alive)
            {
                m_spectating = candidate.id;
                break;
            }
        }
    }
    m_spectateNext = false;

    if (m_spectating >= 0 && living(m_spectating))
    {
        return;
    }
    m_spectating = -1;
    for (const RemotePlayerView& remote : remotes)
    {
        if (remote.alive)
        {
            m_spectating = remote.id;
            break;
        }
    }
}

glm::vec3 PredationGame::MuzzleOf(uint8_t player, const glm::vec3& eye, const glm::vec3& direction) const
{
    // Where that player's weapon is being drawn on this machine. Falls back to a point in front of
    // the eye when there is no body for them, which is better than the eye itself: a round leaving
    // somebody's face is the thing this exists to stop.
    for (const auto& avatar : m_avatars)
    {
        if (avatar->id == player && avatar->body.HasWeapon())
        {
            return avatar->body.MuzzlePoint();
        }
    }
    return eye + direction * 0.45f - glm::vec3(0.0f, 0.12f, 0.0f);
}

std::vector<NetHost::PlayerPose> PredationGame::PosesNow() const
{
    // Who is where at this instant. The host's own shots are aimed at what is on its own screen,
    // which is the present, so there is nothing to rewind.
    std::vector<NetHost::PlayerPose> poses;
    poses.push_back({LocalPlayerId(), m_player.State().position, m_player.State().stance,
                     m_player.State().alive});
    for (const RemotePlayerView& remote : RemotePlayers())
    {
        poses.push_back({remote.id, remote.position, remote.stance, remote.alive});
    }
    return poses;
}

void PredationGame::ResolvePlayerHits(const FireEvent& shot, uint8_t shooter, ShotResult& worldHit,
                                      const std::vector<NetHost::PlayerPose>& poses)
{
    if (!cv_friendlyFire.Get())
    {
        return;
    }

    // Players are tested against directly rather than through the physics world, because a
    // character capsule is a moving query volume rather than a body a ray can find. Doing it here
    // also puts every hit decision in one place, which is where rewinding for lag will go.
    const float maxDistance = worldHit ? worldHit.distance : shot.range;
    const float radius = m_player.Config().radius;

    uint8_t bestPlayer = kMaxPlayers;
    float bestDistance = maxDistance;
    glm::vec3 bestPoint{0.0f};

    const auto testPlayer = [&](uint8_t id, const glm::vec3& feet, PlayerStance stance)
    {
        if (id == shooter)
        {
            return;
        }
        const float height = m_player.Config().HeightForStance(stance);
        // The capsule as a segment from its lower to its upper centre, which is what the character
        // controller actually is.
        const glm::vec3 low = feet + glm::vec3(0.0f, radius, 0.0f);
        const glm::vec3 high = feet + glm::vec3(0.0f, std::max(height - radius, radius), 0.0f);

        // Closest approach between the shot ray and the capsule's axis.
        const glm::vec3 axis = high - low;
        const glm::vec3 toLow = low - shot.origin;
        const float axisLengthSq = glm::dot(axis, axis);
        const float rayDotAxis = glm::dot(shot.direction, axis);
        const float rayDotToLow = glm::dot(shot.direction, toLow);
        const float axisDotToLow = glm::dot(axis, toLow);

        const float denominator = axisLengthSq - rayDotAxis * rayDotAxis;
        float alongRay = 0.0f;
        float alongAxis = 0.0f;
        if (std::abs(denominator) > 1e-5f)
        {
            alongRay = (rayDotToLow * axisLengthSq - axisDotToLow * rayDotAxis) / denominator;
            alongAxis = (alongRay * rayDotAxis - axisDotToLow) / std::max(axisLengthSq, 1e-5f);
        }
        else
        {
            alongRay = rayDotToLow;
        }
        alongRay = std::clamp(alongRay, 0.0f, bestDistance);
        alongAxis = std::clamp(alongAxis, 0.0f, 1.0f);

        const glm::vec3 onRay = shot.origin + shot.direction * alongRay;
        const glm::vec3 onAxis = low + axis * alongAxis;
        if (glm::distance(onRay, onAxis) > radius || alongRay <= 0.05f || alongRay >= bestDistance)
        {
            return;
        }
        bestPlayer = id;
        bestDistance = alongRay;
        bestPoint = onRay;
    };

    for (const NetHost::PlayerPose& pose : poses)
    {
        if (pose.alive)
        {
            testPlayer(pose.id, pose.position, pose.stance);
        }
    }

    if (bestPlayer >= kMaxPlayers)
    {
        return;
    }

    // A player in the way stops the round before whatever the world trace found.
    worldHit.hit = true;
    worldHit.position = bestPoint;
    worldHit.distance = bestDistance;
    // And a person is not somewhere a hole can stay. Whatever surface the trace found behind them
    // is no longer what was struck, so the mark belongs nowhere: they walk away and it would be
    // left hanging in the air.
    worldHit.surface = false;
    ApplyPlayerDamage(bestPlayer, shot.damage, shooter, shot.direction);
}

void PredationGame::ApplyPlayerDamage(uint8_t player, float amount, uint8_t killer,
                                      const glm::vec3& direction)
{
    if (m_sessionMode == SessionMode::Client)
    {
        return; // clients never decide damage
    }

    float remaining = 0.0f;
    if (player == 0)
    {
        m_player.ApplyDamage(amount, "gunfire");
        remaining = m_player.State().health;
    }
    else
    {
        // Into the controller the host is simulating for them. The published view is rebuilt from
        // that controller every tick, so subtracting from the view changed nothing and their health
        // came back the moment it was read again: nobody could be killed by anything short of a
        // single fatal round, and the two machines then disagreed about who was alive.
        m_host.ApplyDamageTo(player, amount);
        remaining = m_host.HealthOf(player);
    }

    if (m_sessionMode == SessionMode::Host)
    {
        WorldEventMessage event;
        event.kind = WorldEventKind::PlayerDamaged;
        event.player = player;
        event.other = killer;
        event.amount = amount;
        m_host.Broadcast(event);
    }

    if (remaining <= 0.0f)
    {
        KillPlayer(player, direction);
    }
}

void PredationGame::KillPlayer(uint8_t player, const glm::vec3& direction)
{
    if (m_sessionMode == SessionMode::Host)
    {
        WorldEventMessage event;
        event.kind = WorldEventKind::PlayerDied;
        event.player = player;
        event.direction = direction * 6.0f;
        m_host.Broadcast(event);
    }
    if (player == LocalPlayerId())
    {
        m_player.State().alive = false;
        m_player.State().health = 0.0f;
        m_deathImpulse = direction * 6.0f;
        m_respawnTimer = cv_respawnSeconds.Get();
        m_spectating = -1;
    }
    else if (m_sessionMode == SessionMode::Host)
    {
        // The host runs the clock for everybody, because the host is what decides they are dead.
        m_remoteRespawnTimers[player] = cv_respawnSeconds.Get();
    }
    PRED_LOG_INFO(Gameplay, "Player {} died", player);
}

// --- The front end ---------------------------------------------------------------------------
//
// The world is built and simulating behind the menu rather than being loaded when you press a
// button, so the menu has a moving backdrop and starting a game is instant. That also means there
// is only ever one world, which is what keeps hosting, joining and leaving from needing their own
// loading paths.

void PredationGame::ResetWorld()
{
    // Everything that can be used up is put back. Starting a game has to start a game: doors shut,
    // lockers empty, crates full, and every item back on the bench, including the ones somebody
    // walked off with last time.
    m_world.Clear(m_scene, m_app->GetPhysics(), m_interactions);
    m_world.Build(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_items,
                  &m_weaponData);

    m_inventory.Clear();
    m_weapon = WeaponState{};
    m_heldItem = kInvalidItem;
    m_body.ClearHeldItem(m_scene);
    m_hidingSpot = -1;
    m_tracers.clear();
    ClearBulletHoles();
    m_remoteRespawnTimers.clear();
    m_respawnTimer = 0.0f;
    m_spectating = -1;
    m_deathImpulse = glm::vec3(0.0f);
    m_localCollapsed = false;
    m_body.Revive();
    RespawnLocalPlayer(m_spawnPoint);
    PRED_LOG_INFO(Gameplay, "World reset");
}

void PredationGame::EnterWorld()
{
    PRED_LOG_INFO(Gameplay, "Entering the world");
    // A new game starts from the beginning. The world has been simulating behind the menu, and
    // whatever was done to it last time is still done.
    ResetWorld();
    m_screen = Screen::Playing;
    m_paused = false;
    m_titleStatus.clear();
    SetCameraMode(CameraMode::FirstPerson);
    m_wantMouseCaptured = true;
}

float PredationGame::ReloadProgress() const
{
    // 0 at the start of a reload, 1 at the end. One function, because there were two: the pose used
    // the fraction and the wire sent one minus the seconds remaining, which is the same number only
    // for a reload that happens to take a second. At 2.2 seconds it stayed at zero until the last
    // one and then ran the whole movement inside it, so everybody else saw a reload begin as it
    // ended and play at more than twice the speed.
    if (!m_weapon.IsReloading())
    {
        return 0.0f;
    }
    const WeaponDefinition* weapon = EquippedWeapon();
    const float seconds = weapon != nullptr ? std::max(weapon->reloadSeconds, 0.01f) : 1.0f;
    return glm::clamp(1.0f - m_weapon.reloadRemaining / seconds, 0.0f, 1.0f);
}

// Where the weapon sits relative to the eye, in the view's own frame. The same numbers from the
// editor and from the game, so the two can be compared rather than argued about.
void PredationGame::ReportHold()
{
    // The same numbers from the editor and from the game, so the two can be compared rather
    // than argued about. Everything is in the view frame: across, up and out from the eye.
    const bool inEditor = m_screen == Screen::Editor && m_editorBodyBuilt;
    const PlayerBody& body = inEditor ? m_editorBody : m_body;
    const PlayerView& view = inEditor ? m_editorView : m_player.View();
    const glm::vec3 forward = view.Forward();
    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 up = glm::cross(right, forward);
    const auto say = [&](const char* what, const glm::vec3& at)
    {
        const glm::vec3 local = at - view.eyePosition;
        char line[160];
        std::snprintf(line, sizeof(line), "%-8s across %+.3f  up %+.3f  out %+.3f", what,
                      glm::dot(local, right), glm::dot(local, up), glm::dot(local, forward));
        m_app->GetConsole().Print(line);
        PRED_LOG_INFO(Gameplay, "{}", line);
    };
    m_app->GetConsole().Print(inEditor ? "The editor's body:" : "The player:");
    say("hold", body.HoldPoint());
    say("origin", body.WeaponOrigin());
    say("muzzle", body.MuzzlePoint());
    say("sight", body.SightPoint());
    char eye[96];
    std::snprintf(eye, sizeof(eye), "eye at %.3f m, pitch %.1f deg", view.eyeHeight,
                  glm::degrees(view.pitch));
    m_app->GetConsole().Print(eye);
    PRED_LOG_INFO(Gameplay, "{}", eye);
}


namespace
{

// A setting that lives in another translation unit, reached by name.
//
// The settings panel touches a few cvars the engine declares rather than the game: the volume, the
// vertical sync. Naming them here and letting the registry do the lookup is what the console already
// does, and it beats making every one of them a public symbol so that one panel can see it.
bool SettingBool(const char* name, bool fallback)
{
    const CVarBase* var = CVarRegistry::Instance().Find(name);
    return var == nullptr ? fallback : var->GetString() == "true" || var->GetString() == "1";
}

// Reads a cvar this file does not own. The registry is the one place that knows about all of them,
// and a setting declared in the engine is not visible to the game as a symbol.
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

// A line of explanation, shortened, with the rest of it on hover.
//
// Every one of these replaced a paragraph. A menu that explains itself in full, on screen, all the
// time, is a menu nobody reads: the prose crowds the buttons down the page and the one line that
// matters is somewhere in the middle of it. What somebody needs in order to choose is a few words,
// and what they need when the choice goes wrong is a paragraph -- so the few words stay and the
// paragraph moves into a tooltip, a hover away and costing nothing until it is wanted.
//
// The marker is part of the same text item rather than a separate one placed beside it. Put beside
// it with SameLine, a "(?)" following a line that already reaches the right-hand edge has nowhere to
// go and breaks apart down the margin, one character per line. As part of the text it wraps with the
// text, and the whole line is the thing you hover.
void Caption(const char* line, const char* detail = nullptr)
{
    ImGui::PushTextWrapPos(0.0f);
    if (detail == nullptr)
    {
        ImGui::TextDisabled("%s", line);
        ImGui::PopTextWrapPos();
        return;
    }
    ImGui::TextDisabled("%s  (?)", line);
    ImGui::PopTextWrapPos();
    if (ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
        ImGui::TextUnformatted(detail);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// The key an action is on, as the player has it bound, for putting in a sentence.
//
// Text that names a key has to name the one the player actually pressed. "[F] Open" after
// somebody has moved Interact to E is a prompt that is wrong about the one thing it is for, and
// every one of these was written with the shipped binding typed into it.
std::string KeyFor(const Input& input, const char* action)
{
    const auto found = input.Bindings().find(action);
    if (found == input.Bindings().end() || found->second.empty())
    {
        return "unbound";
    }
    return Input::BindingName(found->second.front());
}

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
// --- Finding games ------------------------------------------------------------------------------
//
// Two ways a game can be reachable, and the player is asked about neither of them.
//
// What used to be here was four pages of questions: over the internet or by address, and if by
// address then which address, and what port, and had the router agreed to forward it. All of that
// is true and none of it is the player's problem. A game on your own network announces itself and
// is simply in the list; a game over the internet is in the relay's list, because the relay is
// already the thing that opens and closes lobbies. Joining is clicking the row.
//
// The typed box has not gone away, because a code sent to one person is still the way to run a
// game that is not on a list. It is just no longer the first thing anybody sees.

std::string PredationGame::LobbyName() const
{
    if (m_lobbyName[0] != '\0')
    {
        return m_lobbyName;
    }
    // Named after whoever is hosting, so an unnamed game in a list is still the one thing anybody
    // needs to tell it apart: whose it is.
    return PlayerName() + "'s game";
}

void PredationGame::StartBrowsing()
{
    // Both lists at once, always, rather than whichever tab is showing. A beacon listener costs a
    // socket and nothing else, and the relay answers in well under a second, so by the time
    // somebody clicks the other tab the list behind it is already filled in instead of starting
    // empty and filling in while they look at it.
    if (!m_browser.Running() && !m_browser.Start() && m_titleStatus.empty())
    {
        m_titleStatus = m_browser.Message();
    }
    if (m_browseRelay == nullptr && RelayConfigured())
    {
        m_browseRelay = std::make_shared<RelayCarrier>();
        RelayCarrier::Settings settings;
        settings.relayHost = cv_relayHost.Get();
        settings.relayPort = static_cast<uint16_t>(std::clamp(cv_relayPort.Get(), 1, 65535));
        m_browseRelay->Browse(settings);
    }
}

void PredationGame::StopBrowsing()
{
    m_browser.Stop();
    if (m_browseRelay != nullptr)
    {
        m_browseRelay->Close();
        m_browseRelay.reset();
    }
}

void PredationGame::UpdateDiscovery(float frameDeltaSeconds)
{
    // Every frame, wherever the player is. Browsing stops when the menu is left, but the beacon
    // does not: people join a game that is already running, and a host that stopped announcing
    // itself the moment it went in would be a game nobody can find for exactly as long as it is
    // worth finding.
    m_browser.Tick(frameDeltaSeconds);

    // Said out loud when the list changes, not every frame.
    //
    // The relay learned this lesson the hard way: its only output was a line printed when the
    // number of lobbies changed, so a second player joining an existing lobby printed nothing, and
    // "did my friend reach it at all" could not be answered from its own console. A list on a
    // screen has the same problem from the other end -- when somebody says their game is not
    // showing up, the question is whether the beacon never arrived or arrived and was rejected,
    // and only the log can tell those apart.
    for (const LanLobby& lobby : m_browser.Lobbies())
    {
        const std::string where = lobby.address + ":" + std::to_string(lobby.port);
        if (std::find(m_seenOnLan.begin(), m_seenOnLan.end(), where) == m_seenOnLan.end())
        {
            m_seenOnLan.push_back(where);
            PRED_LOG_INFO(Network, "Found a game on this network: {} at {} ({}/{}, protocol {})",
                          lobby.name, where, lobby.players, lobby.maxPlayers, lobby.protocol);
        }
    }
    m_seenOnLan.erase(std::remove_if(m_seenOnLan.begin(), m_seenOnLan.end(),
                                     [this](const std::string& where)
                                     {
                                         for (const LanLobby& lobby : m_browser.Lobbies())
                                         {
                                             if (where == lobby.address + ":" +
                                                              std::to_string(lobby.port))
                                             {
                                                 return false;
                                             }
                                         }
                                         PRED_LOG_INFO(Network, "That game is gone: {}", where);
                                         return true;
                                     }),
                      m_seenOnLan.end());

    if (m_browseRelay != nullptr)
    {
        m_browseRelay->Poll(frameDeltaSeconds);
    }
    if (m_beacon.Running())
    {
        const int players =
            1 + (m_sessionMode == SessionMode::Host ? static_cast<int>(m_host.ConnectedCount()) : 0);
        LanLobby mine;
        mine.name = LobbyName();
        mine.port = static_cast<uint16_t>(m_hostPort);
        mine.players = static_cast<uint8_t>(std::min(players, 15));
        mine.maxPlayers = kMaxPlayers;
        mine.started = m_screen == Screen::Playing;
        mine.protocol = kProtocolVersion;
        m_beacon.Describe(mine);
        m_beacon.Tick(frameDeltaSeconds);
    }
}

void PredationGame::StartHostLocal(bool overInternet)
{
    StopSession();
    StopLobby();
    NetHost::Config config;
    config.port = static_cast<uint16_t>(m_hostPort);
    config.name = PlayerName();
    auto transport = CreateUdpTransport();
    transport->SetConditions(m_simulatedConditions);
    if (!m_host.Start(std::move(transport), config, m_app->GetPhysics(), m_player.Config(),
                      m_spawnPoint))
    {
        m_titleStatus = "Could not open port " + std::to_string(m_hostPort) +
                        ". Something else on this machine is probably already using it.";
        return;
    }
    m_sessionMode = SessionMode::Host;

    // And announce it, which is the whole of the invitation on a local network. Not fatal: the
    // game runs either way and anybody who types the address still gets in, so a beacon that could
    // not open is a warning in the log rather than a refusal to start.
    if (!m_beacon.Start())
    {
        PRED_LOG_WARN(Network, "Not announcing the game on the network: {}", m_beacon.Message());
    }

    StopBrowsing();
    if (overInternet)
    {
        // Over the internet there is something to hand out -- an address -- and the router has to
        // be asked to let people in before it is any use. So the host sees that first, on a
        // screen with a pointer to copy it with, rather than being dropped into the world with it
        // somewhere behind the pause menu.
        m_ports.Open(static_cast<uint16_t>(m_hostPort));
        m_openScreen = true;
        return;
    }
    // Straight in. There is nothing to hand out and nobody to wait for -- the beacon is the
    // invitation, and it keeps going while the game runs.
    EnterWorld();
}

bool PredationGame::JoinAddress(const std::string& address, int port)
{
    if (address.empty())
    {
        m_titleStatus = "Nothing to join.";
        return false;
    }
    StopSession();
    StopLobby();

    auto transport = CreateUdpTransport();
    transport->SetConditions(m_simulatedConditions);
    NetClient::Config config;
    if (!m_client.Connect(std::move(transport), address, static_cast<uint16_t>(port), PlayerName(),
                          config))
    {
        m_titleStatus = "Could not reach " + address + ":" + std::to_string(port) +
                        ". Check they are still hosting and that their firewall is letting the "
                        "game through.";
        return false;
    }

    std::snprintf(m_joinAddress, sizeof(m_joinAddress), "%s", address.c_str());
    m_joinPort = port;
    cv_lastAddress.Set(m_joinAddress);
    cv_lastPort.Set(m_joinPort);
    m_sessionMode = SessionMode::Client;
    // A client predicts where it will be, never whether it is alive.
    m_player.SetDecidesDamage(false);
    m_titleStatus.clear();
    StopBrowsing();
    return true;
}

bool PredationGame::JoinTyped(const std::string& text)
{
    // One box, and the game works out which of the two it was handed. A lobby code is six
    // characters of a known alphabet and nothing else looks like one, so there is nothing to ask
    // the player about.
    uint32_t lobby = 0;
    if (EncodeRelayCode(text, lobby))
    {
        StopBrowsing();
        StartLobby(false, lobby);
        return true;
    }

    // An address, then, with the port on the end of it if there is one. That is how anybody writes
    // one down and how the host page hands them out.
    std::string address = text;
    int port = m_joinPort;
    if (const size_t colon = address.rfind(':'); colon != std::string::npos)
    {
        const int typed = std::atoi(address.c_str() + colon + 1);
        if (typed >= 1024 && typed <= 65535)
        {
            port = typed;
        }
        address = address.substr(0, colon);
    }
    if (address.empty())
    {
        m_titleStatus = "Nothing to join. Paste the code or address they sent you.";
        return false;
    }
    return JoinAddress(address, port);
}

bool PredationGame::RelayConfigured() const
{
    // Empty means nobody has set one up, which is the ordinary case. The relay used to default to
    // this machine, so every "over the internet" game on a PC with no relay on it spent eight
    // seconds asking nothing and then reported that PredationRelay.exe was not running -- a program
    // nobody had asked to run.
    return !cv_relayHost.Get().empty();
}

void PredationGame::StartHostOnline()
{
    // One button, and the game picks whichever way of being reached will actually work.
    //
    // A relay, if one is set up and has answered: it works through every kind of router, needs
    // nothing opened on anybody's, and puts the game in everybody's list. Otherwise the router is
    // asked to let people in, and the host is given an address to send them. Nobody has to know in
    // advance which of those their situation is.
    if (RelayConfigured() && m_browseRelay != nullptr && m_browseRelay->Heard())
    {
        StopBrowsing();
        StartLobby(true, 0);
        return;
    }
    StartHostLocal(true);
}

namespace
{

// Whether an address is one that only exists behind a router. Used for the connection's outside
// address: if the router reports one of these, there is another router beyond it -- usually the
// provider's -- and nobody on the internet can reach this machine, whatever is opened here.
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

} // namespace

void PredationGame::DrawShareAddress()
{
    // What a host playing over the internet needs to see: whether the way in is open, and the
    // address to send people. In plain words, because the person reading this has just clicked
    // "Host" and is not expecting to learn how routers work.
    const ImVec2 wide{-1.0f, 30.0f};
    const std::string port = std::to_string(m_hostPort);
    switch (m_ports.Status())
    {
    case PortMapper::State::Working:
    case PortMapper::State::Idle:
        ImGui::TextDisabled("Asking your router to let your friends in...");
        break;

    case PortMapper::State::Open:
    {
        const std::string outside = m_ports.ExternalAddress();
        if (!outside.empty() && !IsInsideAddress(outside))
        {
            const std::string address = outside + ":" + port;
            ImGui::TextUnformatted("Send your friends this:");
            ImGui::SetWindowFontScale(1.6f);
            ImGui::TextColored({0.70f, 0.95f, 0.75f, 1.0f}, "%s", address.c_str());
            ImGui::SetWindowFontScale(1.0f);
            if (ImGui::Button("Copy it", wide))
            {
                ImGui::SetClipboardText(address.c_str());
            }
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("They press Play, paste it into the box under the list, and press "
                                "Join. Anyone with it can join, so only send it to people you want "
                                "in.");
            ImGui::PopTextWrapPos();
            break;
        }
        // The router agreed, but the address it gave is not one the internet can reach.
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored({0.90f, 0.75f, 0.45f, 1.0f},
                           "Your internet provider shares one address between many homes, so "
                           "nobody outside can reach your PC directly.");
        ImGui::TextDisabled("This cannot be fixed from your router. Let a friend host instead, or "
                            "both install Tailscale (free), host \"On your network\", and have "
                            "them join with your Tailscale address.");
        ImGui::PopTextWrapPos();
        break;
    }

    case PortMapper::State::Failed:
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored({0.90f, 0.75f, 0.45f, 1.0f},
                           "Your router would not let your friends in automatically.");
        ImGui::TextDisabled("Any one of these fixes it:");
        // Wrapped by hand: ImGui's BulletText does not wrap, and a long line ran off the edge of
        // the window with the one fix most people need cut off halfway through.
        const auto point = [](const std::string& text)
        {
            ImGui::Bullet();
            ImGui::SameLine();
            ImGui::TextWrapped("%s", text.c_str());
        };
        point("Let a friend host instead. Only the host needs this.");
        point("Both install Tailscale (free), host \"On your network\", and your friend joins "
              "with your Tailscale address and :" + port + " on the end.");
        point("In your router's settings, turn on \"UPnP\", then host again.");
        static const std::vector<std::string> here = LocalNetworkAddresses();
        const std::string thisPc = here.empty() ? std::string("this PC") : here.front();
        point("Or add a port forward in your router: UDP " + port + " to " + thisPc + ".");
        ImGui::TextDisabled("Then send friends your public address with :%s on the end. Search "
                            "\"what is my IP\" to find it.",
                            port.c_str());
        if (ImGui::TreeNode("What the router said"))
        {
            ImGui::TextDisabled("%s", m_ports.Message().c_str());
            ImGui::TreePop();
        }
        ImGui::PopTextWrapPos();
        break;
    }
    }
}

void PredationGame::DrawOpenGame()
{
    // Between pressing Start and going in, for a game over the internet: the one moment the host
    // needs the address in front of them with a pointer to copy it with. Inside the world the mouse
    // is looking around, so this is also in the pause menu, but it is here first.
    const ImVec2 wide{-1.0f, 34.0f};
    ImGui::SetWindowFontScale(1.2f);
    ImGui::TextUnformatted("Your game is open");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::Spacing();
    DrawShareAddress();
    ImGui::Spacing();
    ImGui::Text("%d of %d here", 1 + static_cast<int>(m_host.ConnectedCount()),
                static_cast<int>(kMaxPlayers));
    ImGui::TextDisabled("People can join before or after you go in. The address is in the pause "
                        "menu too.");
    ImGui::Spacing();
    if (ImGui::Button("Go in", wide))
    {
        m_openScreen = false;
        EnterWorld();
        return;
    }
    if (ImGui::Button("Stop hosting", wide))
    {
        m_openScreen = false;
        StopSession();
        m_titlePage = TitlePage::Browse;
    }
}

void PredationGame::DrawTitleBrowse()
{
    const ImVec2 wide{-1.0f, 34.0f};
    StartBrowsing();

    // Which list, as a tab rather than a question. Both are already filled in behind it.
    //
    // The tab bar keeps its own idea of which tab is showing, so a choice made anywhere else -- the
    // host page's radio buttons, the console -- is pushed into it once, or the bar would quietly put
    // the other tab back on the next frame.
    const bool wanted = m_online;
    const bool push = m_onlineShown != wanted;
    if (ImGui::BeginTabBar("##where"))
    {
        if (ImGui::BeginTabItem("On your network", nullptr,
                                push && !wanted ? ImGuiTabItemFlags_SetSelected : 0))
        {
            m_online = false;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Over the internet", nullptr,
                                push && wanted ? ImGuiTabItemFlags_SetSelected : 0))
        {
            m_online = true;
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (push)
    {
        // The bar takes the pushed tab on this frame, but reports the old one for the rest of it.
        m_online = wanted;
    }
    m_onlineShown = m_online;

    // A fixed-height list, so the window does not jump about every time somebody opens or closes a
    // game while it is being read.
    int shown = 0;
    ImGui::BeginChild("##games", {0.0f, 150.0f}, true);
    if (!m_online)
    {
        const std::vector<LanLobby>& found = m_browser.Lobbies();
        for (size_t i = 0; i < found.size(); ++i)
        {
            const LanLobby& lobby = found[i];
            ImGui::PushID(static_cast<int>(i));
            // A different build is listed and greyed rather than hidden. "Their game is not showing
            // up" is a far worse thing to be left with than "their game says it is a different
            // version", and only one of the two says what to do about it.
            const bool sameBuild = lobby.protocol == kProtocolVersion;
            const bool full = lobby.players >= lobby.maxPlayers;
            ImGui::BeginDisabled(!sameBuild || full);
            const bool join = ImGui::Button("Join", {64.0f, 0.0f});
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(lobby.name.c_str());
            ImGui::SameLine(0.0f, 12.0f);
            ImGui::TextDisabled("%d/%d", static_cast<int>(lobby.players),
                                static_cast<int>(lobby.maxPlayers));
            ImGui::SameLine(0.0f, 12.0f);
            if (!sameBuild)
            {
                ImGui::TextColored({0.90f, 0.55f, 0.35f, 1.0f}, "different version");
            }
            else
            {
                ImGui::TextDisabled("%s", full ? "full" : (lobby.started ? "in progress" : "waiting"));
            }
            ImGui::PopID();
            ++shown;
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
            ImGui::TextDisabled("  Looking for games on your wifi...");
            if (!m_browser.Running())
            {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored({0.90f, 0.55f, 0.35f, 1.0f}, "  %s", m_browser.Message().c_str());
                ImGui::PopTextWrapPos();
            }
        }
    }
    else if (!RelayConfigured())
    {
        // No relay, which is the ordinary case, and no reason to make it sound like a fault.
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted("Playing over the internet works by address.");
        ImGui::TextDisabled("To host: press Host a game, choose Over the internet, and send your "
                            "friends the address it gives you.");
        ImGui::TextDisabled("To join: paste the address they sent into the box below.");
        ImGui::PopTextWrapPos();
    }
    else
    {
        const std::vector<RelayLobbyInfo> open =
            m_browseRelay != nullptr ? m_browseRelay->Lobbies() : std::vector<RelayLobbyInfo>{};
        for (size_t i = 0; i < open.size(); ++i)
        {
            const RelayLobbyInfo& lobby = open[i];
            ImGui::PushID(static_cast<int>(i));
            const bool full = lobby.players >= kMaxPlayers;
            ImGui::BeginDisabled(full);
            const bool join = ImGui::Button("Join", {64.0f, 0.0f});
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(lobby.name.empty() ? "a game" : lobby.name.c_str());
            ImGui::SameLine(0.0f, 12.0f);
            ImGui::TextDisabled("%d/%d", static_cast<int>(lobby.players),
                                static_cast<int>(kMaxPlayers));
            ImGui::SameLine(0.0f, 12.0f);
            ImGui::TextDisabled("%s", full ? "full" : (lobby.started ? "in progress" : "waiting"));
            ImGui::PopID();
            ++shown;
            if (join)
            {
                const uint32_t code = lobby.code;
                ImGui::EndChild();
                StopBrowsing();
                StartLobby(false, code);
                return;
            }
        }
        if (shown == 0)
        {
            const std::string trouble =
                m_browseRelay != nullptr ? m_browseRelay->Message() : std::string();
            ImGui::PushTextWrapPos(0.0f);
            if (!trouble.empty())
            {
                // "Nobody is playing" and "the relay is not there" are the same empty list, and only
                // one of them is worth going and fixing.
                ImGui::TextColored({0.90f, 0.55f, 0.35f, 1.0f},
                                   "  Your relay server (%s) is not answering.",
                                   cv_relayHost.Get().c_str());
                ImGui::TextDisabled("  You can still host and join by address -- hosting falls "
                                    "back to it by itself.");
            }
            else if (m_browseRelay != nullptr && m_browseRelay->Heard())
            {
                ImGui::TextDisabled("  Nobody has a game open.");
            }
            else
            {
                ImGui::TextDisabled("  Asking your relay server what is open...");
            }
            ImGui::PopTextWrapPos();
        }
    }
    ImGui::EndChild();

    // Joining somebody who sent you something. Always here rather than folded away: it is how a
    // friend over the internet gets in, which makes it half of what this screen is for.
    ImGui::Spacing();
    ImGui::TextUnformatted("Got an address or a code from a friend?");
    const float buttons = 64.0f * 2.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    ImGui::SetNextItemWidth(-buttons);
    const bool entered =
        ImGui::InputTextWithHint("##joinwhatever", "paste it here", m_joinInput, sizeof(m_joinInput),
                                 ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Paste", {64.0f, 0.0f}))
    {
        if (const char* clip = ImGui::GetClipboardText(); clip != nullptr)
        {
            // Trimmed, because an address copied out of a chat app usually brings a space or a line
            // break with it, and " 81.2.3.4:27015" is not an address.
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
    if (ImGui::Button("Host a game", wide))
    {
        m_titlePage = TitlePage::Host;
        m_titleStatus.clear();
    }

    if (!m_titleStatus.empty())
    {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored({0.90f, 0.55f, 0.35f, 1.0f}, "%s", m_titleStatus.c_str());
        ImGui::PopTextWrapPos();
    }
}

void PredationGame::DrawTitleHost()
{
    const ImVec2 wide{-1.0f, 34.0f};
    ImGui::TextDisabled("Host a game");
    ImGui::Spacing();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Game name");
    ImGui::SameLine(86.0f);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##lobbyname", LobbyName().c_str(), m_lobbyName, sizeof(m_lobbyName));
    ImGui::Spacing();

    // Who it is for, which is the only thing on this page that changes anything.
    if (ImGui::RadioButton("On your network", !m_online))
    {
        m_online = false;
    }
    ImGui::TextDisabled("    Same house, same wifi. Nothing to set up.");
    if (ImGui::RadioButton("Over the internet", m_online))
    {
        m_online = true;
    }
    ImGui::TextDisabled("    Friends anywhere. You get an address to send them.");

    ImGui::Spacing();
    if (ImGui::Button("Start", wide))
    {
        m_titleStatus.clear();
        if (m_online)
        {
            StartHostOnline();
        }
        else
        {
            StartHostLocal(false);
        }
        return;
    }
    ImGui::PushTextWrapPos(0.0f);
    Caption("Windows may ask to allow the game. Say yes to both boxes.",
            "The first time the game opens a port, Windows Firewall asks whether to allow it on "
            "private and on public networks. Saying no to either is the most common reason a game "
            "nobody can find looks like it started correctly.");
    ImGui::PopTextWrapPos();

    if (!m_titleStatus.empty())
    {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored({0.90f, 0.55f, 0.35f, 1.0f}, "%s", m_titleStatus.c_str());
        ImGui::PopTextWrapPos();
    }
}

void PredationGame::StartLobby(bool asHost, uint32_t code)
{
    StopSession();
    StopLobby();
    m_inLobby = true;
    m_hostingLobby = asHost;
    m_relayFailed = false;
    m_titleStatus.clear();

    m_relay = std::make_shared<RelayCarrier>();
    RelayCarrier::Settings settings;
    settings.relayHost = cv_relayHost.Get();
    settings.relayPort = static_cast<uint16_t>(std::clamp(cv_relayPort.Get(), 1, 65535));
    PRED_LOG_INFO(Network, "Lobby: {} through relay {}:{}", asHost ? "hosting" : "joining",
                  settings.relayHost, settings.relayPort);
    const bool started =
        asHost ? m_relay->Host(settings, LobbyName()) : m_relay->Join(settings, code);
    if (!started)
    {
        PRED_LOG_WARN(Network, "Lobby failed to start: {}", m_relay->Message());
        m_titleStatus = m_relay->Message();
        m_relay.reset();
        m_inLobby = false;
    }
}

void PredationGame::StopLobby()
{
    m_inLobby = false;
    if (m_relay != nullptr)
    {
        m_relay->Close();
        m_relay.reset();
    }
}

void PredationGame::DrawLobby()
{
    const ImVec2 wide{-1.0f, 32.0f};
    ImGui::SetWindowFontScale(1.2f);
    ImGui::TextUnformatted(m_hostingLobby ? "Your lobby" : "Joining");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::Spacing();

    if (m_relay == nullptr)
    {
        StopLobby();
        return;
    }

    switch (m_relay->Status())
    {
    case RelayCarrier::State::Connecting:
        ImGui::TextUnformatted(m_hostingLobby ? "Asking the relay for a code..."
                                                : "Looking for that lobby...");
        break;

    case RelayCarrier::State::Ready:
        if (m_hostingLobby)
        {
            // The game starts listening the moment the lobby opens, not when the host presses the
            // button.
            //
            // A guest that types the code connects immediately, and if the host's transport does not
            // exist yet there is nothing for it to connect to: it dials, gets no answer and times
            // out, and the only way to be in the game is to have joined after the host pressed
            // Start. Nobody can arrange that. So the session runs from the moment there is a lobby,
            // and the button below is about this player entering the world rather than about the
            // game existing.
            if (m_sessionMode != SessionMode::Host)
            {
                NetHost::Config config;
                config.port = static_cast<uint16_t>(m_hostPort);
                config.name = PlayerName();
                auto transport = CreateCarrierTransport(m_relay);
                if (m_host.Start(std::move(transport), config, m_app->GetPhysics(),
                                 m_player.Config(), m_spawnPoint))
                {
                    m_sessionMode = SessionMode::Host;
                }
                else
                {
                    m_titleStatus = "The lobby is open but the game would not start.";
                }
            }

            // The whole of the invitation: one code, and anybody who types it is in. No swapping,
            // nothing to send back, and it does not go stale while somebody reads it out.
            ImGui::TextDisabled("Send this to anybody you want in the game.");
            ImGui::Spacing();
            ImGui::SetWindowFontScale(2.2f);
            ImGui::TextColored({0.70f, 0.95f, 0.75f, 1.0f}, "%s", m_relay->CodeText().c_str());
            ImGui::SetWindowFontScale(1.0f);
            ImGui::Spacing();
            if (ImGui::Button("Copy the code", wide))
            {
                ImGui::SetClipboardText(m_relay->CodeText().c_str());
            }
            ImGui::Spacing();
            // Two counts, not one, because they fail separately.
            //
            // The relay knows who has typed the code. The game knows who has finished a handshake
            // and has a player in the world. Somebody who reached the relay and never reached the
            // game is a firewall or a version mismatch; somebody who never reached the relay typed
            // the wrong code or cannot see the relay at all. Showing only the second number makes
            // those identical -- an empty lobby, with nothing to go on.
            const int inLobby = 1 + static_cast<int>(m_relay->Links());
            const int inGame = 1 + static_cast<int>(m_host.ConnectedCount());
            ImGui::Text("%d of %d here", inGame, static_cast<int>(kMaxPlayers));
            if (inLobby > inGame)
            {
                ImGui::TextColored({0.90f, 0.80f, 0.45f, 1.0f},
                                   "%d at the relay but not in the game yet", inLobby - inGame);
            }
            ImGui::Spacing();
            if (m_sessionMode == SessionMode::Host && ImGui::Button("Go in", wide))
            {
                m_inLobby = false;
                EnterWorld();
                return;
            }
            ImGui::TextDisabled("People can join at any time, before or after you go in.");
        }
        else
        {
            // A guest has nothing to decide. The moment the relay says it is in, the transport
            // dials the host and the title screen hands over to the joining state it already has.
            auto transport = CreateCarrierTransport(m_relay);
            NetClient::Config config;
            if (m_client.Connect(std::move(transport), "relay", 0, PlayerName(), config))
            {
                m_sessionMode = SessionMode::Client;
                m_player.SetDecidesDamage(false);
                m_inLobby = false;
                m_titleStatus.clear();
                return;
            }
            m_titleStatus = "Joined the lobby but could not reach the game.";
        }
        break;

    case RelayCarrier::State::Failed:
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.55f, 0.35f, 1.0f));
        ImGui::TextUnformatted(m_relay->Message().c_str());
        ImGui::PopStyleColor();
        break;

    case RelayCarrier::State::Idle:
    default:
        break;
    }

    if (!m_titleStatus.empty())
    {
        ImGui::TextColored({0.90f, 0.55f, 0.35f, 1.0f}, "%s", m_titleStatus.c_str());
    }

    ImGui::Spacing();
    if (ImGui::Button(m_hostingLobby ? "Close the lobby" : "Back", wide))
    {
        StopLobby();
    }
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

    if (ImGui::BeginTable("##bindings", 2, ImGuiTableFlags_SizingStretchProp))
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
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (ImGui::Button("Put the keys back"))
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
    Input shipped;
    if (!shipped.LoadBindings(Paths::AssetsRoot() / "Config" / "input.json"))
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

void PredationGame::DrawSettings()
{
    // One panel, drawn from the menu and from the pause screen alike.
    //
    // Everything here is a cvar, which means the console already reaches all of it and the archived
    // ones are already remembered between runs. What this adds is a place to find them: a setting
    // nobody can find is a setting nobody has.
    //
    // In tabs rather than one list, because the list had grown past a screen and the thing somebody
    // came to change was always below the fold.
    ImGui::SetWindowFontScale(1.2f);
    ImGui::TextUnformatted("Settings");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::Spacing();

    // Every tab gets the same height, whether or not it has that much in it.
    //
    // Sized to its contents, the panel was a different height on every tab: choosing Audio after
    // Controls shrank the window by half, which moved Back and Reset out from under the cursor and
    // made the whole thing jump about while being read. A settings screen should sit still.
    ImGui::BeginChild("##settingsbody", {0.0f, 360.0f}, ImGuiChildFlags_None);
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

    if (tab("Controls"))
    {
        float sensitivity = cv_mouseSensitivity.Get();
        if (ImGui::SliderFloat("Mouse sensitivity", &sensitivity, 0.02f, 0.60f, "%.3f"))
        {
            SetSetting("input.mouse_sensitivity", std::to_string(sensitivity));
        }
        bool invert = cv_invertY.Get();
        if (ImGui::Checkbox("Invert up and down", &invert))
        {
            SetSetting("input.invert_y", invert ? "true" : "false");
        }

        ImGui::Spacing();
        bool crouchToggle = cv_crouchToggle.Get();
        if (ImGui::Checkbox("Toggle crouch and prone", &crouchToggle))
        {
            SetSetting("input.crouch_toggle", crouchToggle ? "true" : "false");
        }
        ImGui::SetItemTooltip("On, press once to crouch and again to stand. Off, hold the key.");
        bool sprintToggle = cv_sprintToggle.Get();
        if (ImGui::Checkbox("Toggle sprint", &sprintToggle))
        {
            SetSetting("input.sprint_toggle", sprintToggle ? "true" : "false");
        }
        ImGui::SetItemTooltip("On, press once to sprint and again to stop. Off, hold the key.");

        ImGui::Spacing();
        ImGui::SeparatorText("Keys");
        // A table rather than a paragraph. Twenty bindings written as prose is a sentence nobody
        // finishes, and the one key somebody came to look up is in the middle of it.
        DrawKeyBindings();
        ImGui::EndTabItem();
    }

    if (tab("Audio"))
    {
        AudioEngine& audio = m_app->GetAudio();
        float volume = audio.MasterGain();
        if (ImGui::SliderFloat("Volume", &volume, 0.0f, 1.5f, "%.2f"))
        {
            audio.SetMasterGain(volume);
            SetSetting("audio.volume", std::to_string(volume));
        }
        if (!audio.HasDevice())
        {
            ImGui::TextDisabled("  no sound device on this machine");
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Voice");
        bool voice = cv_voiceEnabled.Get();
        if (ImGui::Checkbox("Proximity voice", &voice))
        {
            SetSetting("audio.voice", voice ? "true" : "false");
            if (!voice)
            {
                StopTalking();
            }
        }
        ImGui::BeginDisabled(!voice);
        float voiceVolume = cv_voiceVolume.Get();
        if (ImGui::SliderFloat("Voice volume", &voiceVolume, 0.0f, 2.0f, "%.2f"))
        {
            SetSetting("audio.voice_volume", std::to_string(voiceVolume));
        }
        bool openMic = cv_voiceOpenMic.Get();
        if (ImGui::Checkbox("Open mic", &openMic))
        {
            SetSetting("audio.voice_open_mic", openMic ? "true" : "false");
        }
        ImGui::SetItemTooltip("Off, hold %s to talk. On, it sends whenever you speak.",
                              KeyFor(m_app->GetInput(), "voice").c_str());
        ImGui::BeginDisabled(!openMic);
        float threshold = cv_voiceThreshold.Get();
        if (ImGui::SliderFloat("Open mic sensitivity", &threshold, 0.005f, 0.30f, "%.3f"))
        {
            SetSetting("audio.voice_threshold", std::to_string(threshold));
        }
        ImGui::SetItemTooltip("How loud you have to be before it sends. Lower picks up quieter speech, "
                              "and more of the room. The line on the meter below is where it is.");
        ImGui::EndDisabled();

        // The level meter, with the threshold drawn on it. Setting a threshold blind is guesswork;
        // watching your own voice cross a line is not.
        ImGui::TextUnformatted("Your microphone");
        // Clamped. LastLevel is a peak sample and a loud voice really does exceed one; a progress
        // bar handed a fraction above one draws past its own frame, which is what "the bar freaks
        // out and breaks" was.
        ImGui::ProgressBar(std::clamp(m_voiceLevel, 0.0f, 1.0f), ImVec2(-1.0f, 12.0f), "");
        if (openMic)
        {
            const ImVec2 bar = ImGui::GetItemRectMin();
            const ImVec2 far = ImGui::GetItemRectMax();
            const float x = bar.x + (far.x - bar.x) * std::clamp(threshold, 0.0f, 1.0f);
            ImGui::GetWindowDrawList()->AddLine({x, bar.y}, {x, far.y}, IM_COL32(240, 200, 120, 255),
                                                2.0f);
        }
        // Three states, not two. "Not sending" while the microphone is shut and while the gate is
        // holding it back mean quite different things, and showing one word for both is why this
        // only ever said "not sending".
        ImGui::TextDisabled(!m_microphone.Running() ? "  off until you talk or test it"
                            : m_voiceSending        ? "  sending"
                                                    : "  listening, too quiet to send");

        // Which microphone, listed fresh rather than remembered: devices come and go while the game
        // is running, and a list of what was plugged in at startup is worse than no list.
        ImGui::Spacing();
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
        if (ImGui::BeginCombo("Input device", current.c_str()))
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
            ImGui::TextDisabled("  no microphone found");
        }

        // And a way to hear yourself, because every one of these settings is otherwise invisible
        // until somebody else says whether they could hear you.
        if (ImGui::Button(m_micTest ? "Stop test" : "Test microphone", {-1.0f, 0.0f}))
        {
            m_micTest = !m_micTest;
        }
        Caption(m_micTest ? "Speak. You should hear yourself." : "Hear what the others would hear.",
                "Your microphone is played back out of your own speakers, through the same gate and "
                "the same codec the game sends through -- so what you are judging is what would "
                "actually reach the others, not a cleaner version of it. Use headphones, or the "
                "speakers feed straight back into the microphone.");
        ImGui::EndDisabled();
        ImGui::EndTabItem();
    }

    if (tab("Graphics"))
    {
        float fov = cv_fov.Get();
        if (ImGui::SliderFloat("Field of view", &fov, 70.0f, 120.0f, "%.0f deg"))
        {
            SetSetting("r.fov", std::to_string(fov));
        }

        bool vsync = GetSettingBool("r.vsync", true);
        if (ImGui::Checkbox("V-Sync", &vsync))
        {
            SetSetting("r.vsync", vsync ? "true" : "false");
        }
        ImGui::SetItemTooltip("On, the picture never tears. Off, the game responds a little quicker.");

        ImGui::Spacing();
        ImGui::SeparatorText("Light");
        // Brightness and contrast rather than gamma, because those are the words on a television.
        // They are also the two that matter in a game played in the dark: somebody whose monitor
        // crushes the low end cannot see anything the lighting is doing, and the answer to that is
        // not to make the game brighter for everybody.
        float exposure = cv_exposure.Get();
        if (ImGui::SliderFloat("Brightness", &exposure, 0.4f, 2.5f, "%.2f"))
        {
            SetSetting("r.exposure", std::to_string(exposure));
        }
        float contrast = cv_contrast.Get();
        if (ImGui::SliderFloat("Contrast", &contrast, 0.6f, 1.8f, "%.2f"))
        {
            SetSetting("r.contrast", std::to_string(contrast));
        }
        Caption("Set these by the darkest corner, not the brightest wall.",
                "Turn brightness up until you can just make out the darkest corner of a room, and "
                "no further. A monitor that crushes its low end makes a dark game unplayable, "
                "which is what these are for -- being able to see everything is not the game.");

        ImGui::Spacing();
        ImGui::SeparatorText("Mirrors");
        bool reflections = cv_reflections.Get();
        if (ImGui::Checkbox("Mirror reflections", &reflections))
        {
            SetSetting("r.reflections", reflections ? "1" : "0");
        }
        ImGui::BeginDisabled(!reflections);
        float reflectionDistance = cv_reflectionDistance.Get();
        if (ImGui::SliderFloat("Mirror distance", &reflectionDistance, 5.0f, 60.0f, "%.0f m"))
        {
            SetSetting("r.reflection_distance", std::to_string(reflectionDistance));
        }
        ImGui::EndDisabled();
        Caption("If the game runs slowly, lower the distance first.",
                "A mirror is the whole world drawn a second time from behind it, so this costs "
                "about half again as much as an ordinary frame while you are standing in front of "
                "one. The distance is where it stops being drawn at all -- lower it before turning "
                "it off, because a mirror going flat as you walk away is much less noticeable than "
                "one that was never there.");
        ImGui::EndTabItem();
    }

    if (tab("Multiplayer"))
    {
        // Not once a game is running, for the same reason the title screen refuses: everybody else
        // was told this name when the connection was made and nothing re-tells them.
        const bool inSession = m_sessionMode != SessionMode::Offline;
        ImGui::BeginDisabled(inSession);
        if (ImGui::InputText("Name", m_playerName, sizeof(m_playerName)))
        {
            cv_playerName.Set(m_playerName);
        }
        ImGui::EndDisabled();
        if (inSession)
        {
            ImGui::TextDisabled("Leave the game to change your name.");
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Relay server (optional)");
        static char relayHost[128] = "";
        if (relayHost[0] == '\0')
        {
            std::snprintf(relayHost, sizeof(relayHost), "%s", cv_relayHost.Get().c_str());
        }
        if (ImGui::InputText("Address", relayHost, sizeof(relayHost)))
        {
            SetSetting("net.relay_server", relayHost);
        }
        // The port only once there is an address for it to belong to. A number box under an empty
        // field is a question nobody without a relay can answer and nobody with one needs first.
        if (!cv_relayHost.Get().empty())
        {
            int relayPort = cv_relayPort.Get();
            if (ImGui::InputInt("Port", &relayPort, 0, 0))
            {
                SetSetting("net.relay_port", std::to_string(std::clamp(relayPort, 1024, 65535)));
            }
        }
        Caption("Leave empty unless your group runs one.",
                "A relay is one always-on machine everybody can reach, running PredationRelay.exe. "
                "It passes messages between players, so nobody has to open anything on their "
                "router, and it lists everybody's games. Without one, playing over the internet "
                "works by address: the host sends their friends an address to paste in.");
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
        Caption("Controls, audio, graphics and your name.",
                "Only the settings on this screen. Anything set from the console that is not on it "
                "keeps whatever you gave it.");
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
    ImGui::SameLine();
    Caption("Changes are saved as you make them.");
}

void PredationGame::DrawPauseMenu()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 centre{viewport->Pos.x + viewport->Size.x * 0.5f,
                        viewport->Pos.y + viewport->Size.y * 0.5f};
    ImGui::SetNextWindowPos(centre, ImGuiCond_Always, {0.5f, 0.5f});
    // The settings panel keeps one size whichever tab is showing. Sized to the tab, the whole window
    // jumped between Controls and Audio and the buttons under it moved out from under the cursor.
    ImGui::SetNextWindowSize({m_settingsOpen ? 440.0f : 320.0f, m_settingsOpen ? 520.0f : 0.0f},
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

    // The lobby code, while hosting a game opened over the internet.
    //
    // There is nothing to negotiate any more, so this is not a panel: it is the code, which anybody
    // who types it can still use. People can join a game that has already started, and a host who
    // wants to let somebody else in now only has to read it out again.
    if (m_sessionMode == SessionMode::Host && m_relay != nullptr && !m_relay->CodeText().empty())
    {
        ImGui::Spacing();
        ImGui::TextDisabled("Anybody with this can join:");
        ImGui::SetWindowFontScale(1.4f);
        ImGui::TextColored({0.70f, 0.95f, 0.75f, 1.0f}, "%s", m_relay->CodeText().c_str());
        ImGui::SetWindowFontScale(1.0f);
        if (ImGui::Button("Copy the code", wide))
        {
            ImGui::SetClipboardText(m_relay->CodeText().c_str());
        }
    }
    // Hosting over the internet without a relay: the address, and whether the router let people in.
    if (m_sessionMode == SessionMode::Host && m_relay == nullptr &&
        m_ports.Status() != PortMapper::State::Idle)
    {
        ImGui::Spacing();
        DrawShareAddress();
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
    StopSession();
    // Back to the two buttons, not to whichever page somebody was last on. Coming out of a game
    // onto a half-filled join box is a screen nobody asked for.
    StopLobby();
    m_titlePage = TitlePage::Root;
    m_settingsOpen = false;
    if (m_editor.IsOpen())
    {
        m_editor.SetOpen(m_editorScene, false);
    }
    m_screen = Screen::Title;
    m_paused = false;
    m_titleStatus.clear();
    m_wantMouseCaptured = false;
    m_inventoryOpen = false;
    if (m_hidingSpot >= 0)
    {
        LeaveHidingSpot();
    }
    SetCameraMode(CameraMode::Fly);
}

void PredationGame::UpdateTitleCamera(float frameDeltaSeconds)
{
    m_titleClock += frameDeltaSeconds;

    // A slow arc around the spawn area, looking back at it. Slow enough that it reads as a held
    // shot rather than as a camera being flown.
    constexpr float kRadius = 9.0f;
    constexpr float kHeight = 2.6f;
    const float angle = m_titleClock * 0.06f;
    const glm::vec3 centre = m_spawnPoint + glm::vec3(0.0f, 1.1f, -2.0f);

    m_camera.position = centre + glm::vec3(std::sin(angle) * kRadius, kHeight, std::cos(angle) * kRadius);
    const glm::vec3 toCentre = centre - m_camera.position;
    // Yaw zero looks down -Z and increases turning right, which is what this atan2 encodes. Writing
    // it the other way round aims the camera at the mirror image of where you meant.
    m_camera.yaw = std::atan2(toCentre.x, -toCentre.z);
    m_camera.pitch = std::asin(glm::clamp(glm::normalize(toCentre).y, -1.0f, 1.0f));
}

void PredationGame::DrawTitleScreen()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 size = viewport->WorkSize;

    // The menu stands on its own rather than in front of the developer test map, which is a grid of
    // grey boxes and says nothing about the game. A slow vertical fall of light over black: enough
    // to not be a flat void, quiet enough to read type against.
    ImDrawList* backdrop = ImGui::GetBackgroundDrawList();
    const ImVec2 topLeft = viewport->WorkPos;
    const ImVec2 bottomRight{topLeft.x + size.x, topLeft.y + size.y};
    backdrop->AddRectFilledMultiColor(topLeft, bottomRight, IM_COL32(10, 12, 15, 255),
                                      IM_COL32(10, 12, 15, 255), IM_COL32(22, 26, 30, 255),
                                      IM_COL32(16, 18, 22, 255));

    // A few slow motes drifting down it, so the screen is alive without being a scene.
    for (int i = 0; i < 40; ++i)
    {
        const float seed = static_cast<float>(i) * 12.9898f;
        const float column = std::fmod(std::sin(seed) * 43758.5f, 1.0f);
        const float speed = 6.0f + std::fmod(std::abs(std::cos(seed)) * 91.0f, 14.0f);
        const float y = std::fmod(m_titleClock * speed + static_cast<float>(i) * 37.0f, size.y);
        const float alpha = 18.0f + 26.0f * std::abs(std::sin(seed * 2.0f));
        backdrop->AddCircleFilled({topLeft.x + std::abs(column) * size.x, topLeft.y + y}, 1.4f,
                                  IM_COL32(150, 170, 190, static_cast<int>(alpha)), 6);
    }

    ImGui::SetNextWindowPos({viewport->WorkPos.x + size.x * 0.5f, viewport->WorkPos.y + size.y * 0.5f},
                            ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({460.0f, 0.0f}, ImGuiCond_Always);

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize;
    if (!ImGui::Begin("##title", nullptr, flags))
    {
        ImGui::End();
        return;
    }
    const WrapText wrap;

    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(226, 232, 236, 255));
    ImGui::SetWindowFontScale(2.4f);
    ImGui::TextUnformatted("PROJECT PREDATION");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const ImVec2 wide{-1.0f, 34.0f};

    // Swapping codes with the other player, which is its own screen because it is a conversation
    // rather than a button.
    if (m_inLobby)
    {
        DrawLobby();
        return;
    }
    if (m_openScreen)
    {
        DrawOpenGame();
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
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("by jojozagjos  |  v" PRED_VERSION_STRING);
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


    // The menu is two buttons and a name, because that is what anybody came here to do. Everything
    // about ports, addresses and codes belongs on the screen for the thing it is part of, not on
    // the first screen somebody sees.
    // Not once a game is running. Everybody else was told this name when the connection was made
    // and nothing re-tells them, so a name changed now is a name only this machine can see: the
    // player list, the kill messages and whatever anybody says over voice all still say the old one.
    // Better to be unable to change it than to change it and have it not take.
    const bool inSession = m_sessionMode != SessionMode::Offline;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name");
    ImGui::SameLine(86.0f);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::BeginDisabled(inSession);
    if (ImGui::InputText("##playername", m_playerName, sizeof(m_playerName)))
    {
        cv_playerName.Set(m_playerName);
    }
    ImGui::EndDisabled();
    if (inSession)
    {
        ImGui::TextDisabled("Leave the game to change your name.");
    }
    ImGui::Spacing();

    if (m_titlePage == TitlePage::Browse || m_titlePage == TitlePage::Host)
    {
        const bool hosting = m_titlePage == TitlePage::Host;
        if (hosting)
        {
            DrawTitleHost();
        }
        else
        {
            DrawTitleBrowse();
        }
        ImGui::Spacing();
        if (ImGui::Button("Back", wide))
        {
            // One step back, not all the way out: the host page is reached from the list.
            m_titlePage = hosting ? TitlePage::Browse : TitlePage::Root;
            if (!hosting)
            {
                // Nothing to listen for from the root menu. The beacon is a different thing and
                // keeps going, because it belongs to a game rather than to this screen.
                StopBrowsing();
            }
            m_titleStatus.clear();
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("by jojozagjos  |  v" PRED_VERSION_STRING);
        return;
    }

    // One button, because there is now one thing to do: look at the games. Hosting is on that
    // screen, next to the list of what hosting produces.
    if (ImGui::Button("Play", wide))
    {
        m_titlePage = TitlePage::Browse;
        m_titleStatus.clear();
    }

    ImGui::Spacing();
    if (ImGui::Button("Settings", wide))
    {
        m_settingsOpen = true;
    }

#if PRED_DEV_TOOLS
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Model editor", wide))
    {
        EnterEditor(std::string());
    }
#endif
    ImGui::Spacing();
    if (ImGui::Button("Quit", wide))
    {
        m_app->RequestQuit();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("by jojozagjos  |  v" PRED_VERSION_STRING);
#if PRED_DEV_TOOLS
    // Said on the title screen as well as in the log. The two builds look identical until
    // you go looking for a menu entry, and knowing which one you handed somebody matters.
    ImGui::SameLine();
    ImGui::TextDisabled("  |  developer build");
#endif

}

// --- Multiplayer -----------------------------------------------------------------------------
//
// The host runs the real simulation for everyone. A client predicts its own movement from local
// input and interpolates everybody else. Nothing a client sends is written into the world: the host
// runs the same movement code against its own physics and what comes out is what happened.

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

void PredationGame::StopTalking()
{
    if (!m_talking)
    {
        return;
    }
    m_talking = false;
    m_voiceLevel = 0.0f;
    // Not while the loopback test has it.
    //
    // There is one microphone and two things that want it, and the rule is that the test wins while
    // it is running -- UpdateVoice refuses to talk at all in that case. This is the same rule said
    // again at the point where the device is actually closed, because saying it only in the caller
    // is what let this go wrong: the test set a flag the voice path owned, the voice path read it a
    // frame later as "the player has stopped talking", and closed a device it did not have. Every
    // frame. A guard here costs one comparison and makes that impossible rather than unlikely.
    if (m_micTest)
    {
        return;
    }
    // Closed rather than left open and ignored. The operating system's microphone indicator is the
    // only thing a player has to go on, and it can only mean something if the device really is shut
    // when nobody is speaking.
    m_microphone.Stop();
}

void PredationGame::UpdateVoice(float dt)
{
    // --- Talking ---------------------------------------------------------------------------------
    const bool inSession = m_sessionMode != SessionMode::Offline;
    // Push to talk, or open mic if that is what somebody asked for. Either way the microphone is
    // only open while the game is being played and this player is alive: no talking from the pause
    // menu, the console, or a corpse.
    // Not while the loopback test is running either: one microphone, one owner.
    const bool allowed = inSession && cv_voiceEnabled.Get() && !m_micTest && m_screen == Screen::Playing &&
                         !m_paused && !m_app->IsConsoleOpen() && m_player.State().alive;
    const bool held = m_app->GetInput().IsActionDown("voice");
    // Open mic still needs the microphone open to hear whether anybody is speaking, so it opens on
    // being allowed and the gate below decides what is sent.
    const bool wants = allowed && (cv_voiceOpenMic.Get() || held);

    if (wants && !m_talking)
    {
        VoiceCapture::Settings settings;
        settings.sampleRate = 48000;
        settings.deviceId = static_cast<uint32_t>(std::max(cv_voiceDevice.Get(), 0));
        if (!m_voiceCodec.Ready() && !m_voiceCodec.Init(VoiceCodec::Settings{}))
        {
            // No codec, no voice. Said once by the codec itself; nothing here retries every frame.
            m_talking = false;
        }
        else if (m_microphone.Start(settings))
        {
            m_talking = true;
        }
    }
    else if (!wants && m_talking)
    {
        StopTalking();
    }

    // Decayed rather than cleared, so the indicator does not strobe between packets: see
    // m_voiceSendingFor.
    m_voiceSendingFor = std::max(m_voiceSendingFor - dt, 0.0f);
    m_voiceSending = m_voiceSendingFor > 0.0f;
    if (m_talking)
    {
        std::vector<float> frame;
        std::vector<uint8_t> packet;
        // Everything the microphone has managed since the last frame, which at a steady frame rate
        // is one and occasionally two. Bounded so a stall cannot turn into a burst that arrives all
        // at once and is all stale by the time it is played.
        for (int guard = 0; guard < 8 && m_microphone.ReadFrame(frame); ++guard)
        {
            // Open mic decides per frame whether this is speech or the room. Push to talk has
            // already been decided by the key being down, so it skips this entirely.
            if (cv_voiceOpenMic.Get() &&
                !m_microphone.ShouldTransmit(m_microphone.LastLevel(), cv_voiceThreshold.Get(), dt))
            {
                continue;
            }
            if (!m_voiceCodec.Encode(frame.data(), frame.size(), packet))
            {
                break;
            }
            ++m_voiceSequence;
            m_voiceSendingFor = kVoiceSendingHold;
            m_voiceSending = true;
            if (m_sessionMode == SessionMode::Host)
            {
                m_host.SendVoice(m_voiceSequence, packet, m_player.State().position);
            }
            else
            {
                m_client.SendVoice(m_voiceSequence, packet);
            }
        }
        m_voiceLevel = m_microphone.LastLevel();
    }

    // --- Listening -------------------------------------------------------------------------------
    //
    // Where a speaker is standing decides where their voice comes from, so it goes through exactly
    // the machinery a footstep does: the same attenuation, the same panning, the same distance cut.
    const auto positionOf = [&](uint8_t speaker) { return PlayerPosition(speaker); };

    if (m_sessionMode == SessionMode::Host)
    {
        for (NetHost::VoiceHeard& heard : m_host.TakeVoice())
        {
            HearVoice(heard.speaker, heard.frame, positionOf(heard.speaker));
        }
    }
    else if (m_sessionMode == SessionMode::Client)
    {
        for (NetClient::VoiceHeard& heard : m_client.TakeVoice())
        {
            HearVoice(heard.speaker, heard.frame, positionOf(heard.speaker));
        }
    }

    // A speaker who has stopped keeps their stream for a moment and then gives it back. Closing it
    // the instant a frame is missed would end the voice in every gap between two words.
    AudioEngine& audio = m_app->GetAudio();
    for (size_t i = 0; i < m_speakers.size();)
    {
        Speaker& speaker = *m_speakers[i];
        speaker.silentFor += dt;
        if (speaker.silentFor > 1.0f)
        {
            audio.CloseStream(speaker.stream);
            audio.Stop(speaker.voice);
            m_speakers.erase(m_speakers.begin() + static_cast<ptrdiff_t>(i));
            continue;
        }
        if (speaker.voice != kInvalidVoice)
        {
            audio.SetVoicePosition(speaker.voice, positionOf(speaker.id));
        }
        ++i;
    }
}

void PredationGame::HearVoice(uint8_t speaker, const std::vector<uint8_t>& frame, const glm::vec3& at)
{
    AudioEngine& audio = m_app->GetAudio();

    Speaker* found = nullptr;
    for (const std::unique_ptr<Speaker>& candidate : m_speakers)
    {
        if (candidate->id == speaker)
        {
            found = candidate.get();
            break;
        }
    }

    if (found == nullptr)
    {
        auto fresh = std::make_unique<Speaker>();
        fresh->id = speaker;
        if (!fresh->codec.Init(VoiceCodec::Settings{}))
        {
            return;
        }
        fresh->stream = audio.OpenStream(48000);
        if (fresh->stream == kInvalidStream)
        {
            return;
        }
        AudioEngine::PlayDesc desc;
        desc.stream = fresh->stream;
        desc.position = at;
        desc.positioned = true;
        // Their volume, which is a setting and was not being read: the slider moved and nothing
        // happened. A voice at full gain on top of the game is also most of the way to the mix
        // clipping on its own.
        desc.gain = std::clamp(cv_voiceVolume.Get(), 0.0f, 2.0f);
        // A voice carries further than a footstep and falls off gently: the point of proximity chat
        // is knowing roughly where somebody is, and a hard cut turns that into a switch.
        desc.nearDistance = 2.5f;
        desc.farDistance = kVoiceRange;
        fresh->voice = audio.Play(desc);
        m_speakers.push_back(std::move(fresh));
        found = m_speakers.back().get();
    }

    found->silentFor = 0.0f;
    std::vector<float> samples;
    if (found->codec.Decode(frame.data(), frame.size(), samples) && !samples.empty())
    {
        audio.PushStream(found->stream, samples.data(), samples.size());
    }
}

void PredationGame::UpdateMicrophoneTest(float dt)
{
    // Hear yourself, so the threshold and the device can be set without a second person.
    //
    // Everything about a microphone setting is invisible until somebody speaks into it and somebody
    // else says whether they heard anything. Picking a device from a list of four names, one of which
    // is the monitor, is a guess; setting a gate threshold by watching a bar is a better guess. This
    // closes the loop: what the other players would hear comes back out of the speakers, through the
    // same gate and the same codec, so what is being judged is the thing that will actually be sent.
    //
    // Played flat rather than positioned. It is not coming from anywhere in the world.
    AudioEngine& audio = m_app->GetAudio();
    if (!m_micTest)
    {
        if (m_micTestStream != kInvalidStream)
        {
            audio.CloseStream(m_micTestStream);
            m_micTestStream = kInvalidStream;
            m_microphone.Stop();
        }
        return;
    }

    if (m_micTestStream == kInvalidStream)
    {
        VoiceCapture::Settings settings;
        settings.sampleRate = 48000;
        settings.deviceId = static_cast<uint32_t>(std::max(cv_voiceDevice.Get(), 0));
        if (!m_microphone.Start(settings))
        {
            m_micTest = false;
            return;
        }
        m_micTestStream = audio.OpenStream(48000);
        if (m_micTestStream == kInvalidStream)
        {
            m_microphone.Stop();
            m_micTest = false;
            return;
        }
        AudioEngine::PlayDesc desc;
        desc.stream = m_micTestStream;
        desc.positioned = false;
        desc.gain = 1.0f;
        audio.Play(desc);
    }

    std::vector<float> frame;
    while (m_microphone.ReadFrame(frame))
    {
        m_voiceLevel = m_microphone.LastLevel();
        // Through the same gate the real thing uses, so a threshold set here is the threshold that
        // decides whether a word reaches anybody.
        const bool sending = !cv_voiceOpenMic.Get() ||
                             m_microphone.ShouldTransmit(m_voiceLevel, cv_voiceThreshold.Get(), dt);
        // `m_voiceSending`, not `m_talking`, and the difference is the whole of why this never
        // worked.
        //
        // `m_talking` means "the voice path owns the microphone". UpdateVoice runs first, and every
        // frame it saw m_talking set while its own conditions said it should not be talking -- its
        // conditions exclude the loopback test on purpose -- so it concluded the player had stopped
        // and called StopTalking, which destroys the capture device. The test then read from a
        // device that had just been closed and got nothing, the panel said "microphone shut", and
        // the game spent every frame tearing down and reopening a Windows capture stream, which is
        // what the output audio was being wrecked by.
        //
        // The test does not own m_talking and must not set it. What it has to report is whether the
        // gate is passing, which is a different thing and has its own flag.
        if (sending)
        {
            m_voiceSendingFor = kVoiceSendingHold;
        }
        m_voiceSending = m_voiceSendingFor > 0.0f;
        if (sending)
        {
            audio.PushStream(m_micTestStream, frame.data(), frame.size());
        }
    }
}

void PredationGame::LoadFootsteps(AudioEngine& audio)
{
    m_footsteps.clear();
    m_footstepSurface = 0;

    const std::filesystem::path file = Paths::AssetsRoot() / "Data" / "footsteps.json";
    std::ifstream stream(file);
    if (!stream)
    {
        PRED_LOG_INFO(Gameplay, "No {}; footsteps fall back to the synthesised one",
                      file.filename().string());
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object())
    {
        PRED_LOG_WARN(Gameplay, "{} is not valid JSON; footsteps fall back to the synthesised one",
                      file.filename().string());
        return;
    }

    const std::filesystem::path folder = Paths::AssetsRoot() / "Audio" / "Footsteps";
    const auto surfaces = root.find("surfaces");
    if (surfaces == root.end() || !surfaces->is_object())
    {
        PRED_LOG_WARN(Gameplay, "{} has no surfaces", file.filename().string());
        return;
    }

    size_t trimmed = 0;
    for (const auto& [name, value] : surfaces->items())
    {
        if (!value.is_object())
        {
            continue;
        }
        FootstepSurface surface;
        surface.name = name;
        const auto gain = value.find("gain");
        if (gain != value.end() && gain->is_number())
        {
            surface.gain = gain->get<float>();
        }
        const auto clips = value.find("clips");
        if (clips == value.end() || !clips->is_array())
        {
            continue;
        }
        for (const auto& clip : *clips)
        {
            if (!clip.is_string())
            {
                continue;
            }
            const std::string leaf = clip.get<std::string>();
            const std::filesystem::path path = folder / leaf;
            std::ifstream wav(path, std::ios::binary);
            if (!wav)
            {
                PRED_LOG_WARN(Gameplay, "Footstep clip missing: {}", path.string());
                continue;
            }
            const std::string bytes((std::istreambuf_iterator<char>(wav)),
                                    std::istreambuf_iterator<char>());
            SoundData data;
            std::string error;
            if (!LoadWav(bytes.data(), bytes.size(), data, error))
            {
                PRED_LOG_WARN(Gameplay, "Footstep clip {}: {}", leaf, error);
                continue;
            }
            trimmed += TrimTrailingSilence(data);
            const SoundId id = audio.Add("step/" + name + "/" + leaf, std::move(data));
            if (id != kInvalidSound)
            {
                surface.clips.push_back(id);
            }
        }
        if (!surface.clips.empty())
        {
            m_footsteps.push_back(std::move(surface));
        }
    }

    // Whichever surface the file names, or the first one it described.
    const auto preferred = root.find("default_surface");
    if (preferred != root.end() && preferred->is_string())
    {
        const std::string want = preferred->get<std::string>();
        for (size_t i = 0; i < m_footsteps.size(); ++i)
        {
            if (m_footsteps[i].name == want)
            {
                m_footstepSurface = static_cast<int>(i);
                break;
            }
        }
    }

    if (m_footsteps.empty())
    {
        PRED_LOG_WARN(Gameplay,
                      "No footstep clips loaded; steps fall back to Assets/Audio/step_hard");
        return;
    }
    int clips = 0;
    for (const FootstepSurface& surface : m_footsteps)
    {
        clips += static_cast<int>(surface.clips.size());
    }
    PRED_LOG_INFO(Gameplay, "Footsteps: {} clips over {} surfaces, on '{}'. Trimmed {:.2f}s of "
                            "trailing silence.",
                  clips, m_footsteps.size(), m_footsteps[m_footstepSurface].name,
                  static_cast<double>(trimmed) / 48000.0);
}

int PredationGame::SurfaceIndexAt(const glm::vec3& position) const
{
    // The test map's surface row, if this is standing on it. Everywhere else keeps whatever the
    // panel last chose, which is what makes the panel useful away from the row.
    if (const char* name = SurfaceUnderfoot(position.x, position.z))
    {
        for (size_t i = 0; i < m_footsteps.size(); ++i)
        {
            if (m_footsteps[i].name == name)
            {
                return static_cast<int>(i);
            }
        }
    }
    return m_footstepSurface;
}

SoundId PredationGame::PickFootstep(float& gain, int surfaceIndex) const
{
    if (m_footsteps.empty())
    {
        return m_sounds.step.Pick();
    }
    const FootstepSurface& surface =
        m_footsteps[static_cast<size_t>(std::clamp(surfaceIndex, 0,
                                                   static_cast<int>(m_footsteps.size()) - 1))];
    if (surface.clips.empty())
    {
        return m_sounds.step.Pick();
    }
    // Not round-robin. Two clips alternating strictly is a pattern the ear finds within about six
    // steps; picking at random and refusing an immediate repeat is not.
    size_t pick = static_cast<size_t>(m_footstepRandom() % surface.clips.size());
    if (surface.clips.size() > 1 && surface.clips[pick] == m_lastFootstep)
    {
        pick = (pick + 1) % surface.clips.size();
    }
    m_lastFootstep = surface.clips[pick];
    gain *= surface.gain;
    return m_lastFootstep;
}

void PredationGame::PlaySound(SoundId sound, const glm::vec3& at, float gain, float pitch,
                              bool positioned)
{
    if (sound == kInvalidSound)
    {
        return;
    }
    AudioEngine::PlayDesc desc;
    desc.sound = sound;
    desc.position = at;
    desc.positioned = positioned;
    desc.gain = gain;
    desc.pitch = pitch;
    m_app->GetAudio().Play(desc);
}

// How loud a step is for the stance it is taken in.
//
// Crouching is quieter and crawling quieter still, which is what anybody expects and what makes
// moving carefully worth doing. It matters more than it sounds: this is the first piece of the
// thing that will eventually decide whether the creature heard you, and putting the numbers in one
// place now means the hunt reads the same value the player hears.
namespace
{
float StanceLoudness(PlayerStance stance)
{
    switch (stance)
    {
    case PlayerStance::Crouching:
        return 0.45f;
    case PlayerStance::Prone:
        return 0.25f;
    case PlayerStance::Standing:
    default:
        return 1.0f;
    }
}
} // namespace

void PredationGame::UpdateSounds(float dt)
{
    AudioEngine& audio = m_app->GetAudio();

    // Where the ears are. The camera rather than the body, because the camera is what the player is
    // looking through: spectating a teammate, their ears are the ones that matter.
    audio.SetListener(m_camera.position, m_camera.Forward(), glm::vec3(0.0f, 1.0f, 0.0f));

    // Walking on the spot for the sound panel. Above the check below on purpose, so it keeps going
    // while the game is paused: judging a footstep means listening to it and not to the game.
    if (m_footstepAudition > 0.0f)
    {
        m_footstepAudition += dt;
        if (m_footstepAudition >= m_footstepCadence)
        {
            m_footstepAudition -= m_footstepCadence;
            float gain = 0.30f;
            const SoundId clip = PickFootstep(gain, m_footstepSurface);
            PlaySound(clip, m_camera.position, gain, 0.96f + 0.08f * m_footstepAudition, false);
        }
    }

    if (m_screen != Screen::Playing)
    {
        return;
    }

    // Footsteps, from the stride the body is already walking to.
    //
    // Not from a timer of its own: the leg animation runs on a phase that goes round once per pair
    // of steps, and a foot lands as it passes nought and a half. Reading that is the only way the
    // sound and the foot arrive together, and a footstep that lands a tenth of a second from the
    // foot is worse than none.
    const auto footfalls = [&](float phase, float& previous, const glm::vec3& where, bool moving,
                               float gain, bool positioned)
    {
        if (!moving)
        {
            previous = phase;
            return;
        }
        const auto crossed = [&](float mark)
        { return (previous < mark && phase >= mark) || (previous > phase && mark < 0.25f); };
        if (crossed(0.0f) || crossed(0.5f))
        {
            // A little variation in pitch, or twenty identical steps in a row read as a machine.
            // With recorded clips the random pick does most of that work and the pitch nudge only
            // has to stop two plays of the same clip being identical, so it is gentler than it was.
            float loudness = gain;
            const SoundId clip = PickFootstep(loudness, SurfaceIndexAt(where));
            const float wobble = 0.96f + 0.08f * std::fmod(std::abs(phase) * 7.13f, 1.0f);
            PlaySound(clip, where, loudness, wobble, positioned);
        }
        previous = phase;
    };

    const PlayerState& local = m_player.State();
    const bool localMoving = local.alive && local.grounded &&
                             glm::length(glm::vec2(local.velocity.x, local.velocity.z)) > 0.8f;
    // Your own steps are not placed in the world either: they come from under you, and panning
    // them puts your own feet in one ear.
    footfalls(local.stridePhase, m_lastStridePhase, local.position, localMoving,
              0.30f * StanceLoudness(local.stance), false);

    for (const std::unique_ptr<RemoteAvatar>& avatar : m_avatars)
    {
        const bool moving = avatar->state.alive && avatar->state.grounded &&
                            glm::length(glm::vec2(avatar->state.velocity.x,
                                                  avatar->state.velocity.z)) > 0.8f;
        footfalls(avatar->state.stridePhase, avatar->lastStridePhase, avatar->drawn, moving,
                  0.75f * StanceLoudness(avatar->state.stance), true);
    }

    // And landing, which is an event rather than a phase.
    if (local.landedThisTick && local.fallPeakSpeed > 2.0f)
    {
        const float force = std::clamp(local.fallPeakSpeed / 9.0f, 0.2f, 1.0f);
        PlaySound(m_sounds.land.Pick(), local.position, force * 0.7f, 1.0f, false);
    }

    (void)dt;
}

void PredationGame::UpdateMantleStow()
{
    // Climbing takes both hands, so whatever was in them is put away for the duration and taken
    // back out at the top.
    //
    // Done through the selected slot rather than by hiding the model, so it is the same put-away
    // and the same bring-up as any other swap: the weapon goes down, the arms are free for the
    // climb, and it comes back up afterwards. Hiding it instead would have left the simulation
    // thinking a rifle was in hands that were holding a ledge.
    // The hands stay empty until they are off the ledge, not until the climb ends.
    //
    // The arms go on fading out of the climb for a moment after the body arrives at the top, which
    // is them letting go. Putting a weapon back in them during that fade means the hand is being
    // asked to be on a ledge and on a handguard at once: what came out was the weapon appearing in
    // mid air and the hands snapping across to it. It is a fifth of a second of waiting.
    const bool climbing = m_player.State().mantling || m_body.ArmsAreClimbing();
    if (climbing == m_mantleStowing)
    {
        return;
    }
    m_mantleStowing = climbing;
    if (climbing)
    {
        m_mantleStowedSlot = m_inventory.SelectedSlot();
        if (m_mantleStowedSlot != Inventory::kNoSlot)
        {
            m_inventory.SelectSlot(Inventory::kNoSlot);
        }
    }
    else if (m_mantleStowedSlot != Inventory::kNoSlot)
    {
        m_inventory.SelectSlot(m_mantleStowedSlot);
        m_mantleStowedSlot = Inventory::kNoSlot;
    }
}

void PredationGame::RespawnLocalPlayer(const glm::vec3& position)
{
    m_player.Respawn(position);
    // Standing, whatever you were doing when you died.
    //
    // The controller already comes back standing, and it was put straight back down again: crouch
    // and prone are toggles, the toggle still held whatever it held when the player was killed, and
    // the very next tick of input asked for it. So dying prone meant coming back to life lying on
    // the floor of the spawn room, which is not a thing anybody chose.
    m_crouchToggleState = false;
    m_proneToggleState = false;
    m_forceCrouch = false;
    m_forceProne = false;
    m_sprintToggleState = false;
    // And upright rather than in a heap. The body is a ragdoll from the moment it is killed, and
    // its bones stay wherever they fell until something puts them back: coming back to life without
    // clearing it gave the new body its corpse's arms and legs for the first few frames, which is
    // where the legs over the head came from.
    m_localCollapsed = false;
    m_body.Revive();
    m_deathImpulse = glm::vec3(0.0f);
    m_spectating = -1;
}

std::string PredationGame::PlayerName() const
{
    // Trimmed and never empty. A blank name on somebody else's player list is a row with nothing in
    // it, and a name of pure spaces is the same thing with more effort.
    std::string name = m_playerName;
    const size_t first = name.find_first_not_of(" \t");
    const size_t last = name.find_last_not_of(" \t");
    name = first == std::string::npos ? std::string() : name.substr(first, last - first + 1);
    return name.empty() ? std::string("operator") : name;
}

const std::vector<RemotePlayerView>& PredationGame::RemotePlayers() const
{
    static const std::vector<RemotePlayerView> kNone;
    switch (m_sessionMode)
    {
    case SessionMode::Host: return m_host.Remotes();
    case SessionMode::Client: return m_client.Remotes();
    case SessionMode::Offline: break;
    }
    return kNone;
}

void PredationGame::StopSession()
{
    if (m_sessionMode == SessionMode::Host)
    {
        m_host.Stop();
    }
    else if (m_sessionMode == SessionMode::Client)
    {
        m_client.Disconnect();
    }
    // And shut the door the router was asked to open. A forwarded port pointing at a machine that
    // is no longer listening is worse than none.
    m_ports.Close();
    m_openScreen = false;
    // Stop announcing, whichever end this was. A beacon still going for a game that has ended is a
    // row in everybody's list that fails when it is clicked, which is worse than not being there.
    m_beacon.Stop();
    for (auto& avatar : m_avatars)
    {
        avatar->body.Destroy(m_scene);
    }
    m_avatars.clear();
    m_sessionMode = SessionMode::Offline;
    // On its own again, so the player is once more the authority on its own health.
    m_player.SetDecidesDamage(true);
    m_networkTick = 0;
}

bool PredationGame::StepSession(const PlayerInput& input, float dt)
{
    ++m_networkTick;

    // What is in the local player's hands, so everyone else sees it.
    const ItemDefinition* held = m_items.Get(m_inventory.Selected().item);
    const uint8_t heldId = held != nullptr ? static_cast<uint8_t>(held->id) : 0;
    // A fraction of the reload, not the seconds left of it. See the note on the client's copy of
    // this below: the two were wrong in the same way and for the same reason.
    const float reloadPlay = ReloadProgress();

    if (m_sessionMode == SessionMode::Host)
    {
        m_host.SetPlayerHeld(0, heldId, m_weapon.aim, m_weapon.IsReloading(), reloadPlay);
        m_host.SetPlayerTorch(0, m_torchOn);

        // The host is a player too: it steps itself first, then runs everyone else from what they
        // sent, then tells them all where everybody ended up.
        m_player.Step(input, dt);
        m_host.Tick(m_networkTick, m_player.State(), dt);
        ServeClientRequests();
        SendDynamicBodies();
        return true;
    }

    if (m_sessionMode == SessionMode::Client)
    {
        // Told to the host with the next input, or nobody else ever sees this player holding
        // anything: the host cannot see inside another machine.
        // How far through the reload is, as a fraction. It used to send one minus the seconds left,
        // which is a fraction only for a reload that takes exactly one second: with a 2.2 second
        // one it stayed at zero until the last second and then ran the whole movement in it, so
        // everyone else saw the reload start when it was nearly over and play at twice the speed.
        m_client.SetHeld(heldId, m_weapon.aim, m_weapon.IsReloading(), reloadPlay);
        m_client.SetTorch(m_torchOn);

        // The client's step happens inside prediction, so the same call is used for the first guess
        // and for every replay of it. Doing it here as well would run each input twice.
        m_client.Tick(input, m_player, dt);
        for (const WorldEventMessage& event : m_client.TakeWorldEvents())
        {
            ApplyWorldEvent(event);
        }
        if (m_client.HasWorldState())
        {
            ApplyDynamicBodies(m_client.LatestWorldState());
        }
        return true;
    }

    return false;
}

void PredationGame::SyncRemoteAvatars(float frameDeltaSeconds)
{
    const std::vector<RemotePlayerView>& remotes = RemotePlayers();

    // Anyone who has gone gets their body taken away.
    for (size_t i = 0; i < m_avatars.size();)
    {
        const uint8_t id = m_avatars[i]->id;
        const bool present = std::any_of(remotes.begin(), remotes.end(),
                                         [&](const RemotePlayerView& view) { return view.id == id; });
        if (present)
        {
            ++i;
        }
        else
        {
            m_avatars[i]->body.Destroy(m_scene);
            m_avatars.erase(m_avatars.begin() + static_cast<ptrdiff_t>(i));
        }
    }

    const PlayerConfig& config = m_player.Config();
    for (const RemotePlayerView& remote : remotes)
    {
        RemoteAvatar* avatar = nullptr;
        for (auto& candidate : m_avatars)
        {
            if (candidate->id == remote.id)
            {
                avatar = candidate.get();
                break;
            }
        }
        if (avatar == nullptr)
        {
            auto fresh = std::make_unique<RemoteAvatar>();
            fresh->id = remote.id;
            fresh->body.Build(m_scene, m_app->GetMeshes(), config);
            fresh->body.SetTextureLibrary(m_app->GetTextures());
            // Other people have heads. The body hides its own by default because in first person
            // the camera lives inside it, which is true of exactly one body on this machine at a
            // time, and while spectating it is theirs rather than yours. Set below, every frame.
            fresh->body.Tuning().hideHead = false;
            fresh->view.eyeHeight = config.EyeHeightForStance(remote.stance);
            fresh->built = true;
            m_avatars.push_back(std::move(fresh));
            avatar = m_avatars.back().get();
        }

        // Only where they are and what they are doing crosses the wire. The walk cycle, the lean,
        // the arms and the head all come out of the same procedural body the local player uses, run
        // here from replicated state. A gait is expensive to send and cheap to reproduce.
        // Eased towards where they are rather than set to it. The host rebuilds what it publishes
        // about everyone else once per simulation tick and the screen draws whenever it likes, so
        // taking that position straight moves a body in steps; from inside their head it is the
        // camera jumping back and forth, and the body it is attached to smears with it. A long way
        // out means a respawn or a teleport, where arriving beats sliding across the room.
        if (!avatar->drawnValid || glm::distance(avatar->drawn, remote.position) > 2.0f)
        {
            avatar->drawn = remote.position;
            avatar->drawnValid = true;
        }
        else
        {
            const float blend = 1.0f - std::exp(-26.0f * frameDeltaSeconds);
            avatar->drawn = glm::mix(avatar->drawn, remote.position, blend);
        }

        // Where they are looking, eased for the same reason their position is.
        //
        // On the host, what it publishes about everybody else is rebuilt once per simulation tick
        // while the screen draws whenever it likes, so the angles arrive in steps even though the
        // position no longer does. On a body across the room that is barely visible; from inside
        // their head, spectating, it is the whole view jumping sixty times a second. The short way
        // round for the yaw, or looking across the back of north spins the camera all the way about.
        {
            const float blend = 1.0f - std::exp(-26.0f * frameDeltaSeconds);
            if (!avatar->lookValid)
            {
                avatar->lookYaw = remote.yaw;
                avatar->lookPitch = remote.pitch;
                avatar->lookValid = true;
            }
            else
            {
                float delta = remote.yaw - avatar->lookYaw;
                while (delta > glm::pi<float>()) delta -= glm::two_pi<float>();
                while (delta < -glm::pi<float>()) delta += glm::two_pi<float>();
                avatar->lookYaw += delta * blend;
                avatar->lookPitch += (remote.pitch - avatar->lookPitch) * blend;
            }
        }

        avatar->state.position = avatar->drawn;
        avatar->state.velocity = remote.velocity;
        avatar->state.yaw = avatar->lookYaw;
        avatar->state.pitch = avatar->lookPitch;
        avatar->state.stance = remote.stance;
        avatar->state.desiredStance = remote.stance;
        avatar->state.leanAmount = remote.leanAmount;
        avatar->state.stridePhase = remote.stridePhase;
        avatar->state.health = remote.health;
        avatar->state.alive = remote.alive;
        avatar->state.grounded = remote.grounded;
        // The climb, so a remote player is seen hauling themselves over rather than sliding up a
        // wall. The duration is nominal here: only the phase matters for the pose.
        //
        // The edge and the phase are remembered rather than read fresh every frame. The snapshot
        // carries them only while somebody is climbing, and the hands go on fading off the ledge
        // for a moment after they stop; taking the wire's zero during that fade dragged their
        // weapon towards the world origin at full strength and then let it snap back.
        if (remote.mantling)
        {
            avatar->mantleEdge = remote.mantleEdge;
            avatar->mantlePhase = remote.mantlePhase;
        }
        else
        {
            avatar->mantlePhase = 1.0f;
        }
        avatar->state.mantling = remote.mantling;
        avatar->state.mantleDuration = 1.0f;
        avatar->state.mantleTime = avatar->mantlePhase;
        avatar->state.mantleEdge = avatar->mantleEdge;
        avatar->state.mantleFrom = avatar->drawn;
        avatar->state.mantleTo = avatar->drawn;

        // The eye eases towards the stance's height rather than being set to it, exactly as the
        // local player's own view does. The body is anchored to the eye, so setting it outright
        // dropped a remote player straight into a crouch in one frame while their own screen
        // showed them sinking into it.
        const float targetEye = config.EyeHeightForStance(remote.stance);
        avatar->view.eyeHeight +=
            (targetEye - avatar->view.eyeHeight) *
            (1.0f - std::exp(-config.eyeTransitionSpeed * frameDeltaSeconds));

        avatar->view.renderPosition = avatar->drawn;
        avatar->view.eyePosition = avatar->drawn + glm::vec3(0.0f, avatar->view.eyeHeight, 0.0f);

        // Leaning moves the eye out sideways, and the body is anchored to the eye, so a remote
        // player who is leaning has to have their eye moved the same way here. Without it they saw
        // round a corner while their body stayed squarely behind it: they could see you and you
        // could not see them, which is the worst thing a peek can be.
        const glm::vec3 leanRight{std::cos(remote.yaw), 0.0f, std::sin(remote.yaw)};
        avatar->view.eyePosition += leanRight * (remote.leanAmount * config.leanSideOffset);
        // Their torch, which only the wire can say: nothing about a body implies it.
        avatar->torchOn = remote.torchOn;
        avatar->view.yaw = remote.yaw;
        avatar->view.pitch = remote.pitch;
        avatar->view.leanRoll = glm::radians(config.leanAngleDegrees) * remote.leanAmount;

        // Put in their hands what the snapshot says they are holding. This was replicated and then
        // never used, which is why everybody else appeared empty-handed however obviously they were
        // carrying a rifle.
        if (avatar->heldItem != remote.heldItem)
        {
            avatar->heldItem = remote.heldItem;
            const ItemDefinition* item = m_items.Get(static_cast<ItemId>(remote.heldItem));
            const WeaponDefinition* weapon =
                item != nullptr ? m_weaponData.Get(m_weaponData.ForItem(item->key)) : nullptr;

            avatar->body.SetWeapon(m_scene, m_app->GetMeshes(), weapon);
            if (weapon == nullptr && item != nullptr)
            {
                avatar->body.SetHeldItem(m_scene, m_app->GetMeshes(), item->key,
                                         ItemMesh(*item, &m_weaponData), ItemMaterial(*item));
                avatar->body.SetHeldItemPlacement(item->holdOffset, item->holdRotation);
            }
            else
            {
                avatar->body.ClearHeldItem(m_scene);
            }
            // Brought up rather than appearing already shouldered.
            avatar->weaponDraw = 0.0f;
        }

        // Decayed per frame, exactly as the local one is. Firing is an event rather than a state,
        // so it arrives through the shot message and fades from there; without it, everybody else's
        // weapon fired with no flash and no recoil, which is why shots from other players read as
        // coming from nowhere.
        avatar->weaponKick = std::max(avatar->weaponKick - frameDeltaSeconds * 7.0f, 0.0f);
        avatar->weaponDraw = std::min(avatar->weaponDraw + frameDeltaSeconds * 3.2f, 1.0f);

        PlayerBody::WeaponPose weaponPose;
        weaponPose.aim = remote.aim;
        weaponPose.reloading = remote.reloading;
        weaponPose.reload = remote.reloadProgress;
        weaponPose.kick = avatar->weaponKick;
        weaponPose.draw = avatar->weaponDraw;
        avatar->body.SetWeaponPose(weaponPose);

        // Somebody else going down collapses the same way, from the state the host sent.
        if (!remote.alive && !avatar->collapsed)
        {
            avatar->body.Collapse(remote.velocity * 0.5f + glm::vec3(0.0f, 1.0f, 0.0f));
            avatar->collapsed = true;
        }
        else if (remote.alive && avatar->collapsed)
        {
            avatar->body.Revive();
            avatar->collapsed = false;
        }

        // Spectating puts the camera in their head, so their head has to come off, exactly as your
        // own does when you are alive. Without it the view spends the whole time inside a skull.
        avatar->body.Tuning().hideHead = !m_player.State().alive &&
                                         m_cameraMode == CameraMode::FirstPerson &&
                                         static_cast<int>(avatar->id) == m_spectating;

        avatar->body.Update(m_scene, avatar->state, avatar->view, config, m_app->GetPhysics(),
                            frameDeltaSeconds);
    }
}

void PredationGame::RegisterNetCommands()
{
    Console& console = m_app->GetConsole();

    console.RegisterCommand(
        "lobby", "Open a lobby through the relay, or join one: lobby [code]",
        [this](const std::vector<std::string>& args)
        {
            // So the relay path can be driven without clicking, which is what makes it testable
            // from a headless run.
            uint32_t code = 0;
            if (args.size() >= 2 && !EncodeRelayCode(args[1], code))
            {
                m_app->GetConsole().PrintError("That is not a lobby code.");
                return;
            }
            StartLobby(code == 0, code);
        },
        "lobby [code]");

    console.RegisterCommand(
        "play", "Enter the world on your own, without a session",
        [this](const std::vector<std::string>&)
        {
            // The menu has no solo button any more, on purpose: this is a game about a team. But
            // walking into the map to look at something is most of what development is, and doing
            // that through a lobby is three steps for no reason.
            if (m_screen != Screen::Playing)
            {
                m_sessionMode = SessionMode::Offline;
                EnterWorld();
            }
        });

    console.RegisterCommand(
        "torch", "Switch the flashlight on or off",
        [this](const std::vector<std::string>&) { m_torchOn = !m_torchOn; });

    console.RegisterCommand(
        "menu", "Show a page of the title screen: menu <root|browse|host|settings>",
        [this](const std::vector<std::string>& args)
        {
            // So a menu page can be looked at without clicking through to it, which is what makes
            // it possible to take a screenshot of one from a headless run and see what a change to
            // it actually did.
            const std::string page = args.size() >= 2 ? args[1] : "root";
            // The pause menu is over the world rather than a page of the title screen, so it is
            // opened where it is -- and with the settings on it when asked, the other way that
            // panel is reached.
            if (page == "pause")
            {
                m_paused = true;
                m_wantMouseCaptured = false;
                m_settingsOpen = args.size() >= 3;
                m_settingsTab = args.size() >= 3 ? args[2] : std::string();
                return;
            }
            if (page == "root")
            {
                m_titlePage = TitlePage::Root;
            }
            else if (page == "browse" || page == "online")
            {
                m_titlePage = TitlePage::Browse;
                m_online = page == "online";
            }
            else if (page == "host")
            {
                m_titlePage = TitlePage::Host;
            }
            else if (page == "settings")
            {
                m_settingsOpen = true;
                // And which tab, capitalised the way the tab reads: menu settings Graphics.
                m_settingsTab = args.size() >= 3 ? args[2] : std::string();
            }
            else
            {
                m_app->GetConsole().PrintError("Unknown page. Try root, browse, host or settings.");
                return;
            }
            m_screen = Screen::Title;
            m_titleStatus.clear();
        },
        "menu <root|browse|online|host|settings [tab]|pause [settings tab]>");

    console.RegisterCommand(
        "host_online",
        "Open a game over the internet, as the host page's Start does with that option chosen",
        [this](const std::vector<std::string>&)
        {
            StartBrowsing();
            StartHostOnline();
            PRED_LOG_INFO(Network, "host_online: {}", m_inLobby ? "through the relay"
                                                  : m_openScreen ? "straight to this machine, asking the router"
                                                                 : "did not start");
        });

    console.RegisterCommand(
        "host_lan", "Open a game on this network and go in, as the host page's Start button does",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() >= 2)
            {
                // Everything after the command, not just the first word: a game called "kitchen pc"
                // is two arguments and one name.
                std::string name = args[1];
                for (size_t i = 2; i < args.size(); ++i)
                {
                    name += " " + args[i];
                }
                std::snprintf(m_lobbyName, sizeof(m_lobbyName), "%s", name.c_str());
            }
            StartHostLocal(false);
            if (m_sessionMode != SessionMode::Host)
            {
                m_app->GetConsole().PrintError(m_titleStatus);
                return;
            }
            m_app->GetConsole().Print("Hosting \"" + LobbyName() + "\" on port " +
                                      std::to_string(m_hostPort) +
                                      (m_beacon.Running() ? ", announced on this network"
                                                          : ", NOT announced: " + m_beacon.Message()));
            // The console's own output does not reach the log file, and this is a command whose
            // whole purpose is to be checked from a headless run.
            PRED_LOG_INFO(Network, "host_lan: hosting \"{}\" on {}, announcing {}", LobbyName(),
                          m_hostPort, m_beacon.Running());
        },
        "host_lan [name]");

    console.RegisterCommand(
        "games", "List the games this machine can currently see",
        [this](const std::vector<std::string>&)
        {
            StartBrowsing();
            Console& out = m_app->GetConsole();
            const std::vector<LanLobby>& local = m_browser.Lobbies();
            out.Print("On this network: " + std::to_string(local.size()));
            for (const LanLobby& lobby : local)
            {
                const std::string line = "  " + lobby.name + "  " + lobby.address + ":" +
                                         std::to_string(lobby.port) + "  " +
                                         std::to_string(lobby.players) + "/" +
                                         std::to_string(lobby.maxPlayers);
                out.Print(line);
                PRED_LOG_INFO(Network, "games lan:{}", line);
            }
            const std::vector<RelayLobbyInfo> open =
                m_browseRelay != nullptr ? m_browseRelay->Lobbies() : std::vector<RelayLobbyInfo>{};
            out.Print("Over the internet: " + std::to_string(open.size()));
            for (const RelayLobbyInfo& lobby : open)
            {
                const std::string line = "  " + lobby.name + "  " + DecodeRelayCode(lobby.code) +
                                         "  " + std::to_string(lobby.players);
                out.Print(line);
                PRED_LOG_INFO(Network, "games relay:{}", line);
            }
        });

    console.RegisterCommand(
        "snd", "Play a sound by name, to hear one without making it happen: snd <name> [gain]",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 2)
            {
                m_app->GetConsole().PrintError("snd <name> [gain]");
                return;
            }
            AudioEngine& audio = m_app->GetAudio();
            const SoundId sound = audio.Find(args[1]);
            if (sound == kInvalidSound)
            {
                m_app->GetConsole().PrintError("No sound called '" + args[1] + "'");
                return;
            }
            const float gain = args.size() > 2 ? std::strtof(args[2].c_str(), nullptr) : 1.0f;
            PlaySound(sound, m_camera.position, gain, 1.0f, false);
            m_app->GetConsole().Print("Played " + args[1] +
                                      (audio.HasDevice() ? "" : " (no audio device, so silently)"));
        },
        "snd <name> [gain]");


    console.RegisterCommand(
        "net_host", "Start hosting on a UDP port: net_host [port]",
        [this](const std::vector<std::string>& args)
        {
            StopSession();
            NetHost::Config config;
            config.port = args.size() > 1 ? static_cast<uint16_t>(std::strtoul(args[1].c_str(), nullptr, 10))
                                          : kDefaultPort;
            config.name = PlayerName();
            auto transport = CreateUdpTransport();
            transport->SetConditions(m_simulatedConditions);
            if (!m_host.Start(std::move(transport), config, m_app->GetPhysics(), m_player.Config(),
                              m_spawnPoint))
            {
                m_app->GetConsole().PrintError("Could not open UDP port " + std::to_string(config.port));
                return;
            }
            m_sessionMode = SessionMode::Host;
            // Hosting from the console at the menu should put you in the game, the same as the
            // button does. Joining does not, because it is not a game until the host answers.
            EnterWorld();
            m_app->GetConsole().Print("Hosting on port " + std::to_string(config.port) + " for up to " +
                                      std::to_string(kMaxPlayers) + " players");
        });

    console.RegisterCommand(
        "net_join", "Join a host: net_join [address] [port]",
        [this](const std::vector<std::string>& args)
        {
            StopSession();
            const std::string address = args.size() > 1 ? args[1] : "127.0.0.1";
            const auto port = args.size() > 2
                                  ? static_cast<uint16_t>(std::strtoul(args[2].c_str(), nullptr, 10))
                                  : kDefaultPort;
            auto transport = CreateUdpTransport();
            transport->SetConditions(m_simulatedConditions);
            NetClient::Config config;
            if (!m_client.Connect(std::move(transport), address, port, PlayerName(), config))
            {
                m_app->GetConsole().PrintError("Could not reach " + address);
                return;
            }
            m_sessionMode = SessionMode::Client;
            m_player.SetDecidesDamage(false);
            m_app->GetConsole().Print("Joining " + address + ":" + std::to_string(port));
        });

    console.RegisterCommand("net_leave", "Leave the session, or stop hosting",
                            [this](const std::vector<std::string>&)
                            {
                                StopSession();
                                m_app->GetConsole().Print("Back to single player");
                            });

    console.RegisterCommand(
        "net_sim", "Simulate a bad connection: net_sim <latency ms> <jitter ms> <loss %>",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 4)
            {
                m_app->GetConsole().PrintError("usage: net_sim <latency ms> <jitter ms> <loss %>");
                return;
            }
            m_simulatedConditions.latencyMs = std::strtof(args[1].c_str(), nullptr);
            m_simulatedConditions.jitterMs = std::strtof(args[2].c_str(), nullptr);
            m_simulatedConditions.lossPercent = std::strtof(args[3].c_str(), nullptr);

            Transport* transport = m_sessionMode == SessionMode::Host   ? m_host.GetTransport()
                                   : m_sessionMode == SessionMode::Client ? m_client.GetTransport()
                                                                          : nullptr;
            if (transport != nullptr)
            {
                transport->SetConditions(m_simulatedConditions);
            }
            m_app->GetConsole().Print("Simulating " + args[1] + " ms latency, " + args[2] +
                                      " ms jitter, " + args[3] + "% loss");
        });

    console.RegisterCommand(
        "net_status", "Print the state of the session",
        [this](const std::vector<std::string>&)
        {
            Console& out = m_app->GetConsole();
            out.Print(std::string("Session: ") + SessionModeName(m_sessionMode));
            if (m_sessionMode == SessionMode::Host)
            {
                out.Print("Clients: " + std::to_string(m_host.ConnectedCount()));
                out.Print("Starved ticks: " + std::to_string(m_host.StarvedTicks()));
            }
            else if (m_sessionMode == SessionMode::Client)
            {
                out.Print(m_client.Connected() ? "Connected as player " + std::to_string(m_client.PlayerId())
                                               : "Still trying to join");
                out.Print("Corrections: " + std::to_string(m_client.CorrectionCount()));
            }
        });
}

void PredationGame::DrawNetworkPanel()
{
    ImGui::SetNextWindowPos(ImVec2(420.0f, 470.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340.0f, 260.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Network"))
    {
        ImGui::End();
        return;
    }

    ImGui::Text("Session: %s", SessionModeName(m_sessionMode));

    Transport* transport = m_sessionMode == SessionMode::Host     ? m_host.GetTransport()
                           : m_sessionMode == SessionMode::Client ? m_client.GetTransport()
                                                                  : nullptr;

    if (m_sessionMode == SessionMode::Offline)
    {
        ImGui::TextWrapped("Single player. Use net_host in the console to open a game, or net_join "
                           "<address> to enter one.");
    }
    else if (m_sessionMode == SessionMode::Host)
    {
        ImGui::Text("Clients: %d of %d", static_cast<int>(m_host.ConnectedCount()), kMaxPlayers - 1);
        ImGui::Text("Starved ticks: %u", m_host.StarvedTicks());
        ImGui::TextDisabled("A starved tick is one where a client's input had not arrived and the "
                            "host repeated the last one.");
    }
    else
    {
        ImGui::Text("%s", m_client.Connected() ? "Connected" : "Joining");
        ImGui::Text("Player %u, tick %u", m_client.PlayerId(), m_client.Sequence());
        ImGui::Text("Corrections: %u", m_client.CorrectionCount());
        const ReconciliationResult& last = m_client.LastReconciliation();
        ImGui::Text("Last error: %.3f m over %u replayed ticks", static_cast<double>(last.errorDistance),
                    last.replayedTicks);
        ImGui::Text("Hidden offset: %.3f m", static_cast<double>(glm::length(m_client.VisualOffset())));
    }

    if (transport != nullptr)
    {
        const NetStats& stats = transport->Stats();
        ImGui::Separator();
        ImGui::Text("Sent %llu packets, %llu bytes", static_cast<unsigned long long>(stats.packetsSent),
                    static_cast<unsigned long long>(stats.bytesSent));
        ImGui::Text("Received %llu packets, %llu bytes",
                    static_cast<unsigned long long>(stats.packetsReceived),
                    static_cast<unsigned long long>(stats.bytesReceived));
        ImGui::Text("Dropped %llu", static_cast<unsigned long long>(stats.packetsDropped));

        ImGui::Separator();
        ImGui::TextUnformatted("Simulated conditions");
        bool changed = ImGui::SliderFloat("Latency ms", &m_simulatedConditions.latencyMs, 0.0f, 300.0f);
        changed |= ImGui::SliderFloat("Jitter ms", &m_simulatedConditions.jitterMs, 0.0f, 150.0f);
        changed |= ImGui::SliderFloat("Loss %", &m_simulatedConditions.lossPercent, 0.0f, 50.0f);
        if (changed)
        {
            transport->SetConditions(m_simulatedConditions);
        }
    }

    ImGui::Separator();
    ImGui::Text("Other players: %d", static_cast<int>(RemotePlayers().size()));
    for (const RemotePlayerView& remote : RemotePlayers())
    {
        ImGui::Text("  %u %s  %.1f %.1f %.1f  %s", remote.id, remote.name.c_str(),
                    static_cast<double>(remote.position.x), static_cast<double>(remote.position.y),
                    static_cast<double>(remote.position.z), PlayerStanceName(remote.stance));
    }

    ImGui::End();
}

void PredationGame::OnShutdown()
{
    StopSession();
    DestroyEditorFirstPerson();
    m_editor.Shutdown(m_editorScene);
    if (m_editorBodyBuilt)
    {
        m_editorBody.Destroy(m_editorScene);
        m_editorBodyBuilt = false;
    }
    m_itemIcons.Shutdown();
    ClearProps();
    m_body.Destroy(m_scene);
    m_player.Shutdown();
    m_scene.Clear();
    PRED_LOG_INFO(Gameplay, "Game shutdown");
}

void PredationGame::OnEvent(const SDL_Event& event)
{
    switch (event.type)
    {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        // Clicking back into the world re-captures. Clicking an ImGui window must not, or dragging
        // a slider would yank the pointer away mid-drag.
        //
        // Nor while paused. The pause menu is a small box in the middle of the screen, so most of
        // the window is not over it, and a click out there was reading as "back to the game": the
        // pointer vanished, the view started turning, and the menu was still up in front of it.
        // Leaving the pause menu is a thing you do on purpose, with Escape or with Resume.
        if (!m_wantMouseCaptured && !m_paused && !m_app->IsConsoleOpen() &&
            !m_app->IsUiCapturingMouse())
        {
            m_wantMouseCaptured = true;
        }
        break;
    case SDL_EVENT_DROP_FILE:
        // Adding an asset, in one gesture.
        //
        // It used to mean finding the file in Explorer, copying its path, switching to the game,
        // opening the editor, pasting the path into a box and pressing a button. The window can
        // simply be handed the file. Dropped with the editor closed it opens the editor on it,
        // because somebody dragging a model at the title screen has said what they want clearly
        // enough.
        if (event.drop.data != nullptr)
        {
            OnFileDropped(event.drop.data);
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        m_windowFocused = false;
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        m_windowFocused = true;
        break;
    default:
        break;
    }
}

void PredationGame::UpdateMouseCapture()
{
    // Relative mode hides the cursor and feeds deltas, which is right for looking around and wrong
    // for everything else. The console suspends capture without discarding the player's intent, so
    // closing it hands the mouse straight back to the game.
    //
    // ImGui's hover state deliberately does not appear here. Under relative mode the OS cursor
    // stops moving, so ImGui's idea of the pointer freezes wherever it last was; if that happened
    // to be over a panel, testing it here would release capture, let the cursor reappear over the
    // same panel, and oscillate. Whether the UI is hovered only matters when deciding to re-capture
    // on a click, which is handled in OnEvent.
    // The menu is pointed at, so the pointer is never taken while it is up. Said here as well as
    // where the click is handled, because this is the one place capture is actually decided and a
    // rule that matters this much should not depend on every caller having remembered it.
    const bool shouldCapture = m_wantMouseCaptured && m_windowFocused && !m_paused &&
                               !m_app->IsConsoleOpen() && m_screen == Screen::Playing;

    if (shouldCapture != m_mouseCaptured)
    {
        m_mouseCaptured = shouldCapture;
        m_app->GetWindow().SetRelativeMouse(shouldCapture);
        if (shouldCapture)
        {
            m_discardNextMouseDelta = true;
        }
    }
}

void PredationGame::SampleLook(float /*dt*/)
{
    Input& input = m_app->GetInput();
    if (!m_mouseCaptured || m_app->IsConsoleOpen())
    {
        return;
    }

    const glm::vec2 delta = input.MouseDelta();
    if (m_discardNextMouseDelta)
    {
        // The first report after relative mode is enabled can carry the distance from wherever the
        // cursor happened to be, which snaps the view somewhere arbitrary.
        m_discardNextMouseDelta = false;
        return;
    }
    const float sensitivity = glm::radians(cv_mouseSensitivity.Get());
    const float vertical = (cv_invertY.Get() ? delta.y : -delta.y) * sensitivity;
    const float limit = glm::radians(m_player.Config().maxPitchDegrees);
    // Lying down, the eye is a third of a metre off the floor and there is a body in the way.
    // Looking straight down from there puts the camera through the ground and shows the underside
    // of the level, so the downward half of the range is cut to what a neck could manage anyway.
    const float downLimit = m_player.State().stance == PlayerStance::Prone
                                ? glm::radians(m_player.Config().pronePitchDownDegrees)
                                : limit;

    // Holding the look button in third person orbits the camera instead of turning the character,
    // which is the only way to see the animation from the front.
    if (m_cameraMode == CameraMode::ThirdPerson && input.IsActionDown("orbit"))
    {
        m_orbitYaw += delta.x * sensitivity;
        m_orbitPitch = std::clamp(m_orbitPitch + vertical, -limit, limit);
        return;
    }

    m_lookYaw += delta.x * sensitivity;
    m_lookPitch = std::clamp(m_lookPitch + vertical, -downLimit, limit);
    if (m_lookYaw > glm::pi<float>())
    {
        m_lookYaw -= glm::two_pi<float>();
    }
    else if (m_lookYaw < -glm::pi<float>())
    {
        m_lookYaw += glm::two_pi<float>();
    }
}

PlayerInput PredationGame::BuildPlayerInput()
{
    Input& input = m_app->GetInput();
    PlayerInput result;
    // Where the player is pointing, plus whatever the weapon has kicked it by.
    //
    // The recoil was already here and it only moved the rounds: the barrel climbed and the camera
    // did not, so a burst walked off the target while the crosshair sat still. That is the wrong way
    // round -- what a player feels as recoil is the view moving, and the rounds following it is what
    // makes the crosshair keep telling the truth while it does.
    //
    // Added to the input rather than to the camera alone, so it reaches the three places that have
    // to agree: the view, the body everybody else sees, and the shot. A head that does not kick when
    // the weapon does looks like somebody else is firing.
    //
    // It is an offset that decays to nothing, so it never moves where the player is actually aiming.
    // Pulling down against it works the way it should: the mouse moves m_lookPitch, the recoil
    // recovers to zero on top of it, and the two do not fight.
    const glm::vec2 aimed = AimAngles();
    result.yaw = aimed.x;
    result.pitch = aimed.y;

    if (m_app->IsConsoleOpen() || m_screen != Screen::Playing)
    {
        // Console open, paused, or at the title: keep looking where we are and stop everything else.
        //
        // The console case was here already. The paused one was not, and the pause screen is not a
        // screenshot of the game -- it is the game with a window over it, still running, still
        // reading the keyboard. So WASD walked the character around behind the menu while the player
        // was reading it, and anything else bound to a key did whatever it does. A pause that the
        // character walks through is not a pause.
        m_jumpLatch = false;
        m_crouchPressLatch = false;
        m_pronePressLatch = false;
        return result;
    }

    result.move.y = (input.IsActionDown("move_forward") ? 1.0f : 0.0f) -
                    (input.IsActionDown("move_back") ? 1.0f : 0.0f);
    result.move.x = (input.IsActionDown("move_right") ? 1.0f : 0.0f) -
                    (input.IsActionDown("move_left") ? 1.0f : 0.0f);
    // Console-driven movement, so animation can be inspected in a headless capture.
    result.move += m_debugMove;

    // The latch carries a press that landed between two fixed ticks.
    result.jump = m_jumpLatch;
    m_jumpLatch = false;

    result.sprint = cv_sprintToggle.Get() ? m_sprintToggleState : input.IsActionDown("sprint");

    // Toggles are applied here, in the tick, so a press can never fall between two of them.
    if (cv_crouchToggle.Get())
    {
        if (m_crouchPressLatch)
        {
            m_crouchToggleState = !m_crouchToggleState;
            m_proneToggleState = false;
        }
        if (m_pronePressLatch)
        {
            m_proneToggleState = !m_proneToggleState;
            m_crouchToggleState = false;
        }
    }
    m_crouchPressLatch = false;
    m_pronePressLatch = false;

    result.crouchHeld = cv_crouchToggle.Get() ? m_crouchToggleState : input.IsActionDown("crouch");
    result.proneHeld = cv_crouchToggle.Get() ? m_proneToggleState : input.IsActionDown("prone");
    result.walk = input.IsActionDown("walk");

    result.lean = (input.IsActionDown("lean_right") ? 1.0f : 0.0f) -
                  (input.IsActionDown("lean_left") ? 1.0f : 0.0f);

    // Debug override from the `stance` console command.
    result.crouchHeld = result.crouchHeld || m_forceCrouch;
    result.proneHeld = result.proneHeld || m_forceProne;
    return result;
}

const char* PredationGame::CameraModeName() const
{
    switch (m_cameraMode)
    {
    case CameraMode::ThirdPerson:
        return "third person";
    case CameraMode::Fly:
        return "free camera";
    case CameraMode::FirstPerson:
    default:
        return "first person";
    }
}

void PredationGame::SetCameraMode(CameraMode mode)
{
    m_cameraMode = mode;
    // Orbit is an inspection tool, not a persistent state; entering third person starts behind the
    // character rather than wherever it was last left.
    m_orbitYaw = 0.0f;
    m_orbitPitch = 0.0f;
    if (m_app != nullptr)
    {
        m_app->GetConsole().Print(std::string("Camera: ") + CameraModeName());
    }
}

const WeaponDefinition* PredationGame::EquippedWeapon() const
{
    return m_weaponData.Get(m_weapon.weapon);
}

void PredationGame::SyncEquippedWeapon()
{
    // The selected slot decides what is in your hands. Keeping it that way means there is no second
    // notion of "equipped" to fall out of step with the inventory.
    const int slotNow = m_inventory.SelectedSlot();
    const Inventory::Slot slot = m_inventory.Selected();
    const ItemDefinition* item = m_items.Get(slot.item);
    const WeaponId wanted = item != nullptr ? m_weaponData.ForItem(item->key) : kInvalidWeapon;

    const ItemId heldNow = (item != nullptr && wanted == kInvalidWeapon) ? item->id : kInvalidItem;

    if (slotNow == m_ammoSlot && wanted == m_weapon.weapon && heldNow == m_heldItem)
    {
        // Nothing is changing hands, so whatever was being put away is back.
        m_weaponHolster = 1.0f;
        return;
    }

    // A weapon already in the hands goes away before the next thing comes out. Two tenths of a
    // second is enough to see it leave, and it is what a model's "unequip" clip runs on: without it
    // a swap is one weapon vanishing and another appearing in the same frame, and there is no
    // moment for an authored put-away to happen in.
    //
    // Everything waits on it, the next weapon and the next item alike. The item used to be put in
    // the hand at the top of this function, before the wait, so selecting a medical kit while
    // holding a rifle put the kit in the hand and left the rifle beside it for a fifth of a second.
    if (m_weapon.HasWeapon() && m_weaponHolster > 0.0f)
    {
        m_weaponHolster = std::max(m_weaponHolster - m_lastFrameSeconds / 0.20f, 0.0f);
        if (m_weaponHolster > 0.0f)
        {
            return;
        }
    }

    // Anything that is not a weapon is still carried in a hand. Selecting a medical kit should put
    // the medical kit in your hand, not leave you empty-handed holding an inventory entry.
    if (heldNow != m_heldItem)
    {
        m_heldItem = heldNow;
        if (heldNow == kInvalidItem)
        {
            m_body.ClearHeldItem(m_scene);
        }
        else
        {
            m_body.SetHeldItem(m_scene, m_app->GetMeshes(), item->key,
                               ItemMesh(*item, &m_weaponData), ItemMaterial(*item));
            m_body.SetHeldItemPlacement(item->holdOffset, item->holdRotation);
        }
    }
    m_weaponHolster = 1.0f;

    // The magazine goes back in the slot it came out of before anything else changes hands.
    // Without this, putting a weapon away and taking it out again refilled it, which the key that
    // now holsters what is already out made trivial to do.
    if (m_ammoSlot != Inventory::kNoSlot && m_weapon.HasWeapon())
    {
        m_inventory.SetSlotAmmo(m_ammoSlot, m_weapon.rounds, m_weapon.reserve);
    }
    m_ammoSlot = slotNow;

    // Anything arriving in the hands is brought up rather than appearing already held.
    m_weaponDraw = 0.0f;

    if (wanted == kInvalidWeapon)
    {
        m_weapon = WeaponState{};
        return;
    }
    if (const WeaponDefinition* definition = m_weaponData.Get(wanted))
    {
        // Rounds do not carry between weapons; each comes with its own magazine and reserve.
        // Swapping back and forth would otherwise be a free reload.
        WeaponSim::Equip(*definition, m_weapon);
        // Unless this particular one has been carried before, in which case it is however the
        // player left it.
        if (slot.rounds >= 0)
        {
            m_weapon.rounds = slot.rounds;
            m_weapon.reserve = slot.reserve;
        }
        PRED_LOG_INFO(Gameplay, "Equipped {} ({} rounds, {} spare)", definition->name, m_weapon.rounds,
                      m_weapon.reserve);
    }
}

// Where the player is pointing, with the weapon's kick on top. One function, because the camera and
// the rounds both read it and they must never disagree: the whole reason the crosshair can be
// trusted while a weapon climbs is that the thing it climbs is the same thing the rounds leave
// along. Written out twice, they would drift the first time either was tuned.
glm::vec2 PredationGame::AimAngles() const
{
    // Just where the player is looking. Recoil is not an offset on top of this any more -- it has
    // already been added to the look angles themselves, a tick at a time, and left there. See
    // ApplyRecoilToView.
    return {m_lookYaw, std::clamp(m_lookPitch, glm::radians(-89.0f), glm::radians(89.0f))};
}

// Hands this tick's recoil to the player's own aim, where it stays.
//
// This is the difference between recoil you fight and recoil you watch. Added as a decaying offset,
// the weapon climbs on screen and then un-climbs, and the player's aim ends up exactly where it
// started -- there is never anything to pull down against. Added to the look angles, the climb is
// theirs: it stays until they move the mouse, and moving the mouse is the fight.
//
// It also means compensating works the way it should without anything being written to make it
// work. Pulling down moves m_lookPitch down while the recoil is moving it up, and the two simply
// sum; there is no second system trying to return the view to a remembered place and arguing with
// the player about where they were aiming.
void PredationGame::ApplyRecoilToView()
{
    if (std::abs(m_weapon.kickPitch) < 1e-5f && std::abs(m_weapon.kickYaw) < 1e-5f)
    {
        return;
    }
    m_lookPitch = std::clamp(m_lookPitch + glm::radians(m_weapon.kickPitch), glm::radians(-89.0f),
                             glm::radians(89.0f));
    m_lookYaw += glm::radians(m_weapon.kickYaw);
    if (m_lookYaw > glm::pi<float>())
    {
        m_lookYaw -= glm::two_pi<float>();
    }
    else if (m_lookYaw < -glm::pi<float>())
    {
        m_lookYaw += glm::two_pi<float>();
    }
}

glm::vec3 PredationGame::AimDirection() const
{
    // Rounds leave along the same line the camera looks down, so what you see under the crosshair is
    // what you hit.
    const glm::vec2 aimed = AimAngles();
    const float cp = std::cos(aimed.y);
    return {std::sin(aimed.x) * cp, std::sin(aimed.y), -std::cos(aimed.x) * cp};
}

glm::vec3 PredationGame::MuzzlePosition() const
{
    const WeaponDefinition* definition = EquippedWeapon();
    const glm::vec3 eye = m_player.View().eyePosition;
    if (definition == nullptr)
    {
        return eye;
    }
    // From the eye rather than from the model's barrel. A round that starts at the visible muzzle
    // can be on the far side of a doorframe the player is peering round, so they shoot the wall
    // they can see past. Starting at the eye means what is under the crosshair is what is hit.
    return eye + AimDirection() * (definition->muzzleForward * 0.5f);
}

void PredationGame::AgeTracers(float dt)
{
    for (Tracer& tracer : m_tracers)
    {
        tracer.age += dt;
    }
    std::erase_if(m_tracers, [](const Tracer& tracer) { return tracer.age > kTracerSeconds; });
}

void PredationGame::ResolveShots()
{
    if (m_shots.empty())
    {
        return;
    }
    PhysicsWorld& physics = m_app->GetPhysics();
    m_weaponKick = 1.0f;
    // A different flash every shot: rolled to a new angle and squashed a different way. Gas leaves a
    // barrel unevenly, and a flash that is identical each time is the thing the eye picks out as a
    // repeated picture rather than as fire.
    m_body.SetMuzzleFlashShape(
        glm::two_pi<float>() * static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX),
        0.78f + 0.44f * static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX));
    // Your own shot is played flat in both ears rather than placed in the world. It comes from a
    // weapon a forearm's length from your face: positioning it means panning it hard to whichever
    // side the barrel is on, which is both wrong and the most obvious way a mix sounds broken.
    for (size_t i = 0; i < m_shots.size(); ++i)
    {
        PlaySound(m_sounds.gunshot.Pick(), MuzzlePosition(), 0.85f, 1.0f, false);
    }

    for (const FireEvent& shot : m_shots)
    {
        if (m_sessionMode == SessionMode::Client)
        {
            // Fired for feel and sent up. The tracer is drawn straight away because a weapon that
            // waits a round trip to go off feels broken; what it actually hit is the host's answer.
            ShotMessage message;
            message.shotNumber = shot.sequence;
            message.origin = shot.origin;
            message.direction = shot.direction;
            // The moment this client was drawing everyone else at, so the host can test the shot
            // against what was actually on screen when the trigger went down.
            message.renderTick = m_client.RenderTick();
            m_client.SendShot(message);

            const ShotResult predicted = ResolveShot(physics, shot);
            Tracer tracer;
            // Traced from the eye so that what is under the crosshair is hit, drawn from the muzzle
            // so it looks like it came out of the gun. Those are different points and the round is
            // entitled to both.
            tracer.from = MuzzlePosition();
            tracer.origin = shot.origin;
            tracer.to = predicted ? predicted.position : shot.origin + shot.direction * shot.range;
            tracer.hit = predicted.hit;
            tracer.surface = predicted.surface;
            tracer.body = predicted.body;
            tracer.normal = predicted.normal;
            AnchorTracer(tracer);
            m_tracers.push_back(tracer);
            continue;
        }

        ShotResult result = ResolveShot(physics, shot);
        ResolvePlayerHits(shot, LocalPlayerId(), result, PosesNow());

        Tracer tracer;
        tracer.from = MuzzlePosition();
        tracer.origin = shot.origin;
        tracer.to = result ? result.position : shot.origin + shot.direction * shot.range;
        tracer.hit = result.hit;
        tracer.surface = result.surface;
        tracer.body = result.body;
        tracer.normal = result.normal;
        AnchorTracer(tracer);
        m_tracers.push_back(tracer);

        if (m_sessionMode == SessionMode::Host)
        {
            WorldEventMessage event;
            event.kind = WorldEventKind::ShotFired;
            event.player = 0;
            event.position = MuzzlePosition();
            event.direction = tracer.to;
            event.flag = tracer.hit;
            event.flag2 = tracer.surface;
            m_host.Broadcast(event);
        }

        if (!result)
        {
            continue;
        }
        // Nothing in the level has health yet, so for now a hit shows itself by shoving whatever it
        // struck. When creatures arrive this is where their damage is applied, and it stays on the
        // authority's side of the line.
        if (physics.IsValid(result.body))
        {
            physics.AddImpulse(result.body, shot.direction * (result.damage * 0.35f));
        }
    }
}


// The weapon bench.
//
// Placing a grip socket is not something that can be done by looking at the model on its own: the
// only question that matters is where the hand ends up, and the only way to answer it is to hold
// the thing. So the bench points a weapon at a model without a restart, drives everything the
// weapon does, and draws the sockets on the weapon as it is held, with the distance from each one
// to the hand that is supposed to be at it.
// The editor's try-it panel: someone holding what is being built.
//
// Placing a grip cannot be done by looking at the model. The only question is where the hand ends
// up, so the panel shows the distance from each socket to the hand meant to be at it, draws the line
// between them in the world, and plays every movement the weapon has on demand. Move a socket, watch
// the number come down.
void PredationGame::DrawWeaponBench()
{
    if (m_screen != Screen::Editor || !m_editorBodyBuilt)
    {
        return;
    }

    ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x - 372.0f, 24.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({350.0f, 520.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Hold it"))
    {
        ImGui::End();
        return;
    }

    // Everything in here wraps to the panel. The explanations are the useful part of this window
    // and a sentence that runs off the right edge is worse than no sentence: it reads as a bug.
    ImGui::PushTextWrapPos(0.0f);

    ImGui::TextDisabled("Whatever is open in the editor, in someone's hands.");
    ImGui::Separator();

    // What is in the hands: the model being edited, or one of the items that is not a weapon.
    //
    // One list rather than a list and a checkbox and a second list. They were separate because a
    // weapon and an item are placed by different machinery, and a person holding one thing at a
    // time does not care about that. Choosing an item also takes the weapon out of the hands, which
    // it has to: a weapon takes both of them and the one-handed carry never runs while one is held,
    // so the item was placed and then never posed and never appeared.
    {
        std::vector<const ItemDefinition*> holdable;
        for (const ItemDefinition& item : m_items.All())
        {
            if (item.id != kInvalidItem && !item.key.empty() &&
                m_weaponData.ForItem(item.key) == kInvalidWeapon)
            {
                holdable.push_back(&item);
            }
        }
        m_benchItem = std::clamp(m_benchItem, -1, static_cast<int>(holdable.size()) - 1);

        const auto label = [](const ItemDefinition* item)
        { return item->name.empty() ? item->key.c_str() : item->name.c_str(); };
        const char* current = m_benchItem < 0 ? "the model in the editor"
                                              : label(holdable[static_cast<size_t>(m_benchItem)]);
        if (ImGui::BeginCombo("In the hands", current))
        {
            if (ImGui::Selectable("the model in the editor", m_benchItem < 0))
            {
                m_benchItem = -1;
                m_benchItemHeld = false;
            }
            for (int i = 0; i < static_cast<int>(holdable.size()); ++i)
            {
                ImGui::PushID(i);
                if (ImGui::Selectable(label(holdable[static_cast<size_t>(i)]), i == m_benchItem))
                {
                    m_benchItem = i;
                    m_benchItemHeld = false;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }

        if (m_benchItem >= 0)
        {
            const ItemDefinition* chosen = holdable[static_cast<size_t>(m_benchItem)];
            if (ItemDefinition* editable = m_items.Mutable(chosen->id))
            {
                bool moved =
                    ImGui::DragFloat3("Offset", &editable->holdOffset.x, 0.002f, -0.5f, 0.5f, "%.3f m");
                moved |= ImGui::DragFloat3("Turn", &editable->holdRotation.x, 1.0f, -180.0f, 180.0f,
                                           "%.0f deg");
                if (moved)
                {
                    m_editorBody.SetHeldItemPlacement(editable->holdOffset, editable->holdRotation);
                }
                if (ImGui::Button("Write to items.json"))
                {
                    const std::filesystem::path file = Paths::AssetsRoot() / "Data" / "items.json";
                    m_app->GetConsole().Print(m_items.SaveHoldPlacements(file)
                                                  ? "Hold placements written to items.json"
                                                  : "Could not write items.json");
                }
                ImGui::TextDisabled("The offset is from the fingers and the turn is in the hand's "
                                    "own frame, so both follow the arm wherever it goes.");
            }
        }
    }
    ImGui::Separator();

    // What the body is doing. Nothing simulates in here, so every movement is a button: the point
    // is to watch one thing at a time and as often as you like.
    const char* stances[] = {"Standing", "Crouching", "Prone"};
    ImGui::Combo("Stance", &m_editorStance, stances, 3);
    ImGui::Checkbox("Walk on the spot", &m_editorWalking);
    ImGui::SliderFloat("Sights", &m_editorAim, 0.0f, 1.0f, "%.2f");

    if (ImGui::Button("Reload"))
    {
        m_editorReload = 0.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Draw"))
    {
        m_editorDraw = 0.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Put away"))
    {
        m_editorHolster = 1.0f;
        m_editorHolstering = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Fire"))
    {
        m_editorKick = 1.0f;
    }
    if (m_editorReload >= 0.0f)
    {
        ImGui::ProgressBar(m_editorReload, {-1.0f, 0.0f}, "reloading");
    }

    ImGui::Separator();

    // The clips the model carries, each playable on demand. A clip that only runs when it happens
    // to be named after something the weapon already does is most of a clip system nobody can use.
    const ModelAsset& model = m_editor.Model();
    if (model.clips.empty())
    {
        ImGui::TextDisabled("No clips on this model, so nothing moves on it: reloading and drawing "
                            "are the model's own now, not something written into the game. Add one "
                            "under Animation and name it reload, equip, unequip or fire. A fire "
                            "clip plays on top of the recoil rather than instead of it.");
    }
    else
    {
        int clipIndex = 0;
        for (const AnimationClip& clip : model.clips)
        {
            // By position, not by name. A clip whose name has been cleared would push an empty id.
            ImGui::PushID(clipIndex++);
            const bool playing = m_editorClip == clip.name;
            if (ImGui::Button(playing ? "Stop" : "Play"))
            {
                m_editorClip = playing ? std::string() : clip.name;
                m_editorClipTime = 0.0f;
            }
            ImGui::SameLine();
            ImGui::Text("%s  %.2fs  %d tracks", clip.name.c_str(), clip.duration,
                        static_cast<int>(clip.tracks.size()));
            if (playing)
            {
                ImGui::SliderFloat("##scrub", &m_editorClipTime, 0.0f, 1.0f, "%.2f");
                ImGui::SameLine();
                ImGui::Checkbox("Run", &m_editorClipPlaying);
            }
            ImGui::PopID();
        }
        if (model.FindPart("magazine") == nullptr)
        {
            ImGui::TextColored({0.90f, 0.70f, 0.45f, 1.0f},
                               "No part called 'magazine', so the built-in reload has nothing to "
                               "take out. Rename the part that is one and it will move.");
        }
    }

    ImGui::Separator();
    if (m_editorBody.HasWeapon())
        {
        ImGui::Checkbox("Draw sockets and the hands they belong to", &m_benchSockets);
        ImGui::TextDisabled("The carry socket is where the weapon sits: move it and the gun moves on "
                            "the screen. The grip is where the trigger hand closes: move that and the "
                            "hand moves along the weapon. Neither drags the other any more.");
        const WeaponVisual& visual = m_editorBody.Weapon();
        const glm::quat hold = m_editorBody.WeaponRotation();
        const glm::vec3 origin = m_editorBody.WeaponOrigin();
        // Two separate questions, and they used to be answered by one number that could not tell them
        // apart. A wrist sits a hand's length behind whatever the palm closes on, so a hand properly on
        // a grip still reads several centimetres away from it, and a support hand that slid back down
        // the barrel because the socket was out of reach read as a much larger miss than a hand that
        // simply could not get there. The useful readings are whether the fingers close on the socket,
        // and how hard the arm is working to hold that.
        const auto report = [&](const char* label, const glm::vec3& socket, int side)
        {
            const glm::vec3 world = origin + hold * socket;
            const glm::vec3 wrist = m_editorBody.GetPose().GlobalPosition(m_editorBody.Rig().hand[side]);
            const glm::vec3 shoulder =
                m_editorBody.GetPose().GlobalPosition(m_editorBody.Rig().shoulder[side]);
            const float gap = glm::distance(world, wrist);
            const float span =
                m_editorBody.Rig().upperArmLength + m_editorBody.Rig().lowerArmLength;
            const float working = glm::distance(wrist, shoulder) / std::max(span, 1e-3f);
    
            // A hand is about eleven centimetres from wrist to closed fingers, so anything inside that
            // is a hand on the grip. Beyond it the arm went somewhere else.
            const bool held = gap < 0.11f;
            ImGui::TextColored(held ? ImVec4(0.65f, 0.85f, 0.65f, 1.0f) : ImVec4(0.90f, 0.70f, 0.45f, 1.0f),
                               held ? "%s hand is on its socket, wrist %.1f cm behind it"
                                    : "%s hand ended up %.1f cm from its socket",
                               label, gap * 100.0f);
            ImGui::TextColored(working < 0.95f ? ImVec4(0.65f, 0.85f, 0.65f, 1.0f)
                                               : ImVec4(0.90f, 0.70f, 0.45f, 1.0f),
                               "   arm at %.0f%% of its reach", working * 100.0f);
        };
        report("Trigger", visual.triggerGrip, 1);
        report("Support", visual.supportGrip, 0);
    
        // What is wrong with the hold, said where the hold is being looked at. The same faults are
        // reported in the editor's Model panel, which is collapsed most of the time and is not where
        // anybody is looking while they place a grip.
        {
            const ImVec4 warn{0.95f, 0.55f, 0.45f, 1.0f};
            if (visual.muzzle.z <= visual.triggerGrip.z)
            {
                ImGui::TextColored(warn, "The muzzle socket is behind the grip, so this is being held "
                                         "by the barrel and pointed at its own stock. Turn the model "
                                         "round under Model, or set the grip's Turn to 0, 180, 0.");
            }
            const float across = std::max(std::abs(visual.triggerGrip.x), std::abs(visual.supportGrip.x));
            if (across > 0.08f)
            {
                ImGui::TextColored(warn,
                                   "A grip sits %.0f cm off the middle of the weapon. Hands close on the "
                                   "centre line of a gun, and an offset there twists the whole hold.",
                                   across * 100.0f);
            }
        }
        ImGui::TextDisabled("The support hand slides back down the barrel when its socket is out of "
                            "reach, so a socket out past the end of the handguard shows as a hand that "
                            "is not on it rather than as an arm stretched to nothing. An arm at its "
                            "full reach is the one to move: that is where the elbow locks straight and "
                            "the hold stops looking like a hold.");
    }


    ImGui::Separator();
    ImGui::TextDisabled("The weapon this model is worn by, for the game:");
    // Every row carries its own id, so two weapons that happen to share a name cannot collide.
    // The database's "no weapon" placeholder used to be in this list and had to be skipped by hand
    // here -- offering it crashed the game outright, because a row with an empty name is a row with
    // an empty id, and an empty id at the root of a popup is the one thing ImGui refuses. All()
    // no longer hands it out, so there is nothing to remember.
    const std::span<const WeaponDefinition> weapons = m_weaponData.All();
    if (!weapons.empty())
    {
        m_benchWeapon = std::clamp(m_benchWeapon, 0, static_cast<int>(weapons.size()) - 1);
        const auto label = [&](int index)
        {
            const WeaponDefinition& weapon = weapons[static_cast<size_t>(index)];
            return weapon.name.empty() ? weapon.key : weapon.name;
        };
        if (ImGui::BeginCombo("Weapon", label(m_benchWeapon).c_str()))
        {
            for (int i = 0; i < static_cast<int>(weapons.size()); ++i)
            {
                ImGui::PushID(i);
                if (ImGui::Selectable(label(i).c_str(), i == m_benchWeapon))
                {
                    m_benchWeapon = i;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button("Assign this model to it"))
        {
            AssignModelToWeapon(weapons[static_cast<size_t>(m_benchWeapon)].id);
        }
        ImGui::TextDisabled("Not written to weapons.json: set it there to keep it.");
    }

    ImGui::PopTextWrapPos();
    ImGui::End();
}

namespace
{

// The panel's own resolution. Small on purpose: it is redrawn every frame, and what it is for is
// the shape and placement of a hold, which reads at this size.
constexpr uint16_t kEditorEyeWidth = 640;
// The height is the window's, worked out per frame: see RenderEditorFirstPerson.

} // namespace

void PredationGame::RenderEditorFirstPerson()
{
    if (m_screen != Screen::Editor || !m_editorFirstPerson || !m_editorBodyBuilt)
    {
        return;
    }

    // The panel is the shape of the game's window, not a fixed 16:9. How much of a weapon is on
    // screen depends entirely on how wide the screen is, so a hold lined up against a panel of the
    // wrong shape comes out somewhere else in the game. The target is remade when the window
    // changes shape, which is rare enough to cost nothing.
    const Renderer& renderer = m_app->GetRenderer();
    const float windowAspect = renderer.Height() > 0 ? static_cast<float>(renderer.Width()) /
                                                           static_cast<float>(renderer.Height())
                                                     : 16.0f / 9.0f;
    const auto wantedHeight = static_cast<uint16_t>(
        std::clamp(static_cast<int>(std::lround(kEditorEyeWidth / std::max(windowAspect, 0.2f))), 180,
                   1200));
    if (wantedHeight != m_editorEyeHeightPixels)
    {
        DestroyEditorFirstPerson();
        m_editorEyeHeightPixels = wantedHeight;
    }

    if (!bgfx::isValid(m_editorEyeBuffer))
    {
        const uint64_t targetFlags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
        bgfx::TextureHandle attachments[2] = {
            bgfx::createTexture2D(kEditorEyeWidth, m_editorEyeHeightPixels, false, 1,
                                  bgfx::TextureFormat::BGRA8, targetFlags),
            bgfx::createTexture2D(kEditorEyeWidth, m_editorEyeHeightPixels, false, 1,
                                  bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT_WRITE_ONLY)};
        if (!bgfx::isValid(attachments[0]) || !bgfx::isValid(attachments[1]))
        {
            PRED_LOG_ERROR(Render, "Editor first-person panel: could not create its render target");
            m_editorFirstPerson = false;
            return;
        }
        // The framebuffer takes ownership of both, so destroying it destroys them.
        m_editorEyeBuffer = bgfx::createFrameBuffer(2, attachments, true);
        m_editorEyeTexture = attachments[0];
        if (!bgfx::isValid(m_editorEyeBuffer))
        {
            PRED_LOG_ERROR(Render, "Editor first-person panel: could not create its framebuffer");
            m_editorFirstPerson = false;
            return;
        }
    }

    const PlayerView& view = m_editorView;
    const glm::vec3 forward = view.Forward();
    const glm::mat4 viewMatrix =
        glm::lookAtRH(view.eyePosition, view.eyePosition + forward, glm::vec3(0.0f, 1.0f, 0.0f));
    // The game's own field of view, and the near plane it uses, because that is what decides how
    // much of a receiver the camera cuts through.
    const float aspect =
        static_cast<float>(kEditorEyeWidth) / static_cast<float>(m_editorEyeHeightPixels);
    const float vertical = 2.0f * std::atan(std::tan(glm::radians(cv_fov.Get()) * 0.5f) / aspect);
    const bool homogeneous = bgfx::getCaps()->homogeneousDepth;
    const glm::mat4 projection = homogeneous
                                     ? glm::perspectiveRH_NO(vertical, aspect, 0.05f, 200.0f)
                                     : glm::perspectiveRH_ZO(vertical, aspect, 0.05f, 200.0f);

    bgfx::setViewFrameBuffer(Renderer::kViewOffscreenLive, m_editorEyeBuffer);
    bgfx::setViewRect(Renderer::kViewOffscreenLive, 0, 0, kEditorEyeWidth, m_editorEyeHeightPixels);
    bgfx::setViewClear(Renderer::kViewOffscreenLive, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x14181cff,
                       1.0f, 0);
    bgfx::setViewTransform(Renderer::kViewOffscreenLive, glm::value_ptr(viewMatrix),
                           glm::value_ptr(projection));
    m_app->GetSceneRenderer().Draw(Renderer::kViewOffscreenLive, m_editorScene, m_app->GetMeshes(),
                                   view.eyePosition);
}

void PredationGame::DrawEditorFirstPerson()
{
    if (m_screen != Screen::Editor || !m_editorBodyBuilt)
    {
        return;
    }

    ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x - 492.0f, 470.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({470.0f, 350.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("First person"))
    {
        ImGui::End();
        return;
    }

    ImGui::Checkbox("Draw it", &m_editorFirstPerson);
    ImGui::SameLine();
    ImGui::Checkbox("Centre lines", &m_editorEyeReticle);
    if (m_editorFirstPerson && bgfx::isValid(m_editorEyeTexture))
    {
        // Fitted to the panel, keeping the shape of the frame it is standing in for. A stretched
        // one would answer the on-screen question wrongly, which is the question it exists for.
        const float aspect =
            static_cast<float>(kEditorEyeWidth) / static_cast<float>(m_editorEyeHeightPixels);
        const ImVec2 room = ImGui::GetContentRegionAvail();
        const float width = std::min(room.x, std::max(room.y, 1.0f) * aspect);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 size(width, width / aspect);
        ImGui::Image(static_cast<ImTextureID>(ImGuiLayer::TextureId(m_editorEyeTexture)), size);

        // Where the view axis is, drawn over the picture. Lining a sight up means putting it on
        // that axis, and by eye alone the middle of a small panel is a guess.
        if (m_editorEyeReticle)
        {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 centre(at.x + size.x * 0.5f, at.y + size.y * 0.5f);
            constexpr ImU32 kLine = IM_COL32(120, 230, 140, 150);
            draw->AddLine({at.x, centre.y}, {at.x + size.x, centre.y}, kLine);
            draw->AddLine({centre.x, at.y}, {centre.x, at.y + size.y}, kLine);
            draw->AddCircle(centre, size.y * 0.06f, kLine, 0, 1.0f);
        }
    }
    else if (!m_editorFirstPerson)
    {
        ImGui::TextDisabled("Off. The panel is redrawn every frame, so leave it off while working "
                            "on something else.");
    }

    ImGui::End();
}

void PredationGame::DestroyEditorFirstPerson()
{
    if (bgfx::isValid(m_editorEyeBuffer))
    {
        bgfx::destroy(m_editorEyeBuffer);
    }
    m_editorEyeBuffer = BGFX_INVALID_HANDLE;
    m_editorEyeTexture = BGFX_INVALID_HANDLE;
}

void PredationGame::AssignModelToWeapon(WeaponId weapon)
{
    WeaponDefinition* chosen = m_weaponData.Mutable(weapon);
    if (chosen == nullptr)
    {
        m_app->GetConsole().PrintError("No such weapon to assign a model to");
        return;
    }

    const std::string name = m_editor.Model().name;
    if (name.empty())
    {
        m_app->GetConsole().PrintError("The model has no name yet: give it one and save it first");
        return;
    }

    chosen->model = name;
    // Anything already built from the old model file is now wrong, so the cache goes and the next
    // weapon built reads the file again.
    ForgetWeaponModels();
    m_app->GetConsole().Print(chosen->name + " now wears " + name);
}



// The preview body in the editor: someone to hold what is being built.
//
// Driven from numbers rather than from a controller, because there is nothing to control. The body
// is the game's own, posed by the same code that poses the player, so a grip that looks held here is
// held in the game. A separate mannequin drawn by separate code would answer a different question.
void PredationGame::UpdateEditorBody(float frameDeltaSeconds)
{
    if (!m_editorBodyBuilt)
    {
        return;
    }

    m_editorClock += frameDeltaSeconds;

    const PlayerConfig& config = m_player.Config();
    const PlayerStance stance = m_editorStance == 2   ? PlayerStance::Prone
                                : m_editorStance == 1 ? PlayerStance::Crouching
                                                      : PlayerStance::Standing;
    m_editorState.stance = stance;
    m_editorState.grounded = true;
    m_editorState.alive = true;
    // Aside from the origin, where the model itself is laid out. Standing the two in the same place
    // means the body is always in front of the thing being edited.
    m_editorState.position = {1.1f, 0.0f, 0.0f};

    // Walking on the spot. The stride advances but the body does not travel, so the gait can be
    // watched from one place rather than chased across the floor.
    const float speed = m_editorWalking ? config.SpeedForStance(stance, false, false) : 0.0f;
    m_editorState.velocity = glm::vec3(0.0f, 0.0f, -speed);
    if (m_editorWalking)
    {
        m_editorState.strideDistance += speed * frameDeltaSeconds;
        m_editorState.stridePhase = glm::fract(
            m_editorState.stridePhase +
            speed * frameDeltaSeconds / std::max(config.StrideLength(speed, stance), 0.05f));
    }

    m_editorView.eyeHeight = config.EyeHeightForStance(stance);
    m_editorView.renderPosition = m_editorState.position;
    m_editorView.eyePosition = m_editorState.position + glm::vec3(0.0f, m_editorView.eyeHeight, 0.0f);
    // Facing straight ahead. It used to be turned a little towards the model, on the idea that the
    // weapon reads better presented than seen end-on, and what it actually looked like was a body
    // standing crooked for no reason anyone could see.
    m_editorView.yaw = 0.0f;
    m_editorState.yaw = 0.0f;
    m_editorView.pitch = 0.0f;
    m_editorView.leanRoll = 0.0f;

    // What it is holding is whatever the editor has open, rebuilt whenever the editor says the model
    // changed. That is the whole loop: move a socket, see the hand move.
    const ModelAsset& model = m_editor.Model();
    const bool geometryChanged = m_editor.TakeGeometryChanged();
    const bool anythingChanged = m_editor.TakePreviewChanged();

    // One thing in the hands at a time.
    //
    // A weapon takes both of them, and the one-handed carry never runs while one is held, so an
    // item put in the hand alongside a weapon was placed and then never posed: it stayed at the
    // origin and nobody ever saw it. Choosing an item takes the weapon away and choosing the model
    // brings it back.
    std::vector<const ItemDefinition*> holdable;
    for (const ItemDefinition& item : m_items.All())
    {
        if (item.id != kInvalidItem && !item.key.empty() &&
            m_weaponData.ForItem(item.key) == kInvalidWeapon)
        {
            holdable.push_back(&item);
        }
    }
    const int itemIndex =
        m_benchItem >= 0 && m_benchItem < static_cast<int>(holdable.size()) ? m_benchItem : -1;

    if (itemIndex >= 0)
    {
        if (m_editorBody.HasWeapon())
        {
            m_editorBody.SetWeapon(m_editorScene, m_app->GetMeshes(), nullptr);
        }
        // Built once rather than every frame: making a mesh per frame is how an editor comes to
        // feel slow, and dragging an offset does not change the mesh.
        if (!m_benchItemHeld)
        {
            const ItemDefinition* item = holdable[static_cast<size_t>(itemIndex)];
            m_editorBody.SetHeldItem(m_editorScene, m_app->GetMeshes(), item->key,
                                     ItemMesh(*item, &m_weaponData), ItemMaterial(*item));
            m_editorBody.SetHeldItemPlacement(item->holdOffset, item->holdRotation);
            m_benchItemHeld = true;
        }
    }
    else
    {
        if (m_benchItemHeld)
        {
            m_editorBody.ClearHeldItem(m_editorScene);
            m_benchItemHeld = false;
        }
        if (geometryChanged || !m_editorBody.HasWeapon())
        {
            m_editorPreviewWeapon = WeaponDefinition{};
            m_editorPreviewWeapon.id = static_cast<WeaponId>(1);
            m_editorPreviewWeapon.key = model.name.empty() ? "preview" : model.name;
            m_editorPreviewWeapon.name = m_editorPreviewWeapon.key;
            m_editorPreviewWeapon.model = model.name;
            m_editorPreviewWeapon.reloadSeconds = 2.2f;
            m_editorPreviewWeapon.magazineSize = 30;
            ForgetWeaponModels();
            m_editorBody.SetWeaponFromModel(m_editorScene, m_app->GetMeshes(), m_editorPreviewWeapon,
                                            model);
        }
    }

    if (anythingChanged && !geometryChanged)
    {
        // A socket or a clip. Nothing that is drawn has changed, so nothing that is drawn is
        // rebuilt: only the named points the hands are placed by are read again. Rebuilding the
        // meshes for a socket drag copied a quarter of a megabyte per part and destroyed and remade
        // every entity, on every frame of the drag.
        m_editorBody.RefreshWeaponSockets(m_editorPreviewWeapon, model);
    }

    // A clip being watched runs on its own clock, so it can be scrubbed or left running.
    if (!m_editorClip.empty() && m_editorClipPlaying)
    {
        const AnimationClip* watching = model.FindClip(m_editorClip);
        const float duration = watching != nullptr ? std::max(watching->duration, 0.05f) : 1.0f;
        m_editorClipTime = glm::fract(m_editorClipTime + frameDeltaSeconds / duration);
    }

    PlayerBody::WeaponPose pose;
    pose.aim = m_editorAim;
    pose.draw = m_editorDraw;
    pose.kick = m_editorKick;
    pose.reloading = m_editorReload >= 0.0f;
    pose.reload = m_editorReload;
    pose.clip = m_editorClip;
    pose.clipProgress = m_editorClipTime;
    pose.holster = m_editorHolster;
    m_editorBody.SetWeaponPose(pose);

    m_editorDraw = std::min(m_editorDraw + frameDeltaSeconds * 1.6f, 1.0f);
    if (m_editorHolstering)
    {
        m_editorHolster -= frameDeltaSeconds / 0.6f;
        if (m_editorHolster <= 0.0f)
        {
            // Held at nothing for a moment and then brought back out, so the put-away can be
            // watched over and over from one button.
            m_editorHolster = 1.0f;
            m_editorHolstering = false;
            m_editorDraw = 0.0f;
        }
    }
    m_editorKick = std::max(m_editorKick - frameDeltaSeconds * 7.0f, 0.0f);
    if (m_editorReload >= 0.0f)
    {
        m_editorReload += frameDeltaSeconds / 2.2f;
        if (m_editorReload > 1.0f)
        {
            m_editorReload = -1.0f;
        }
    }

    m_editorBody.Update(m_editorScene, m_editorState, m_editorView, config, m_app->GetPhysics(),
                        frameDeltaSeconds);
}

void PredationGame::OnFileDropped(const std::string& path)
{
#if PRED_DEV_TOOLS
    const std::filesystem::path file = path;
    if (!ModelEditor::IsImportableModel(file))
    {
        // Said rather than swallowed. A file that simply vanishes when you drop it on a window is
        // worse than one that is refused, because there is nothing to conclude from it.
        PRED_LOG_INFO(Asset, "Dropped {}, which is not a model the editor reads (OBJ, GLB or GLTF)",
                      file.filename().string());
        m_titleStatus = file.filename().string() + " is not an OBJ, GLB or GLTF.";
        return;
    }

    // Dropped anywhere but the editor, the editor is what was meant. Somebody dragging a model
    // onto the title screen has said what they want clearly enough to not be asked again.
    if (m_screen != Screen::Editor)
    {
        EnterEditor(std::string());
    }
    if (m_editor.DropFile(path))
    {
        PRED_LOG_INFO(Asset, "Imported {} by dropping it on the window", file.filename().string());
    }
#else
    // A shipped build has no editor to drop things into, and importing art at runtime is not a
    // thing a player does. Named in the log so a dropped file is not simply silence.
    PRED_LOG_INFO(Asset, "Ignoring dropped file {}: this build has no editor", path);
#endif
}

void PredationGame::EnterEditor(const std::string& modelName)
{
    // A place of its own rather than a mode switched on in the middle of a game. It has its own
    // scene, so what is being built stands against an empty floor instead of against whatever
    // happens to be at the spawn point, and nothing in the world simulates behind it.
    StopSession();
    m_screen = Screen::Editor;
    m_titleStatus.clear();
    m_inventoryOpen = false;

    m_editor.SetOpen(m_editorScene, true);
    if (!modelName.empty())
    {
        m_editor.Load(modelName);
    }

    if (!m_editorBodyBuilt)
    {
        // Someone to hold it. Built once and kept, because building a body is not free and the
        // editor is opened and closed a great deal.
        m_editorBody.Build(m_editorScene, m_app->GetMeshes(), m_player.Config());
        m_editorBody.SetTextureLibrary(m_app->GetTextures());
        m_editorBody.Tuning().hideHead = false;
        m_editorBodyBuilt = true;
    }
    m_editorState = PlayerState{};
    m_editorState.alive = true;
    m_editorState.grounded = true;
    m_editorClock = 0.0f;

    // Everything in an editor is done with the pointer, so the mouse is free and it is only taken
    // while the right button is held to look around.
    SetCameraMode(CameraMode::Fly);
    // Framed on the model, which sits at the origin, with the body it will be held by off to one
    // side and in shot.
    //
    // Written to the editor's camera, not the game's. The game's is copied from the editor's every
    // frame while the editor is open, so setting it here was undone one frame later and the view
    // jumped back to wherever the editor had last left it.
    m_editor.Camera().position = {0.30f, 0.62f, 1.15f};
    m_camera.position = m_editor.Camera().position;
    m_lookYaw = glm::radians(12.0f);
    m_lookPitch = glm::radians(-22.0f);
    m_wantMouseCaptured = false;
    UpdateMouseCapture();
    m_app->GetConsole().Print(
        "Model editor. WASD to move, Q and E for down and up, right mouse to look, Escape to leave.");
}

void PredationGame::TryInteract()
{
    const InteractionSystem::Focus& focus = m_interactions.CurrentFocus();
    if (m_hidingSpot >= 0)
    {
        // Leaving is the same interaction as entering, so it takes the same road: a client asks,
        // the host decides and tells everybody. Done as a local call it told nobody, and every
        // other machine kept the locker shut and occupied for the rest of the match.
        const int spot = m_hidingSpot;
        if (m_sessionMode == SessionMode::Client)
        {
            m_client.SendInteract(static_cast<uint8_t>(InteractionKind::HidingSpot),
                                  static_cast<uint8_t>(spot));
        }
        else
        {
            PerformInteraction(InteractionKind::HidingSpot, spot, LocalPlayerId());
        }
        return;
    }
    if (!focus.valid)
    {
        return;
    }

    // A client asks; it does not act. Everything in the world belongs to the host, so what happens
    // next arrives as an event and is applied the same way another player's interaction would be.
    if (m_sessionMode == SessionMode::Client)
    {
        // The one thing worth checking before asking: whether there is room. The host cannot know
        // what is in this player's bag, so if it hands over something that will not fit the item is
        // gone. Checking here costs nothing and is the same answer the offline path gives.
        if (focus.kind == InteractionKind::Pickup)
        {
            const WorldObjects::Pickup* pickup = m_world.GetPickup(focus.payload);
            if (pickup != nullptr && !m_inventory.CanAdd(m_items, pickup->item, 1))
            {
                m_app->GetConsole().Print("Inventory full");
                return;
            }
        }
        m_client.SendInteract(static_cast<uint8_t>(focus.kind), static_cast<uint8_t>(focus.payload));
        return;
    }

    PerformInteraction(focus.kind, focus.payload, LocalPlayerId());
}

void PredationGame::TakeAmmunition(int crateIndex)
{
    const WeaponDefinition* definition = EquippedWeapon();
    if (definition == nullptr)
    {
        m_app->GetConsole().Print("Nothing to load");
        return;
    }
    if (m_weapon.reserve >= definition->reserveOnPickup)
    {
        m_app->GetConsole().Print("Already carrying full spare magazines");
        return;
    }
    if (!m_world.DrawFromAmmoCrate(crateIndex, m_interactions))
    {
        m_app->GetConsole().Print("The crate is empty");
        return;
    }

    // Fills the spare rounds, not the magazine. What is in the weapon still has to be reloaded, so
    // resupplying never doubles as a free reload in the middle of a fight.
    const int taken = definition->reserveOnPickup - m_weapon.reserve;
    m_weapon.reserve = definition->reserveOnPickup;
    if (const WorldObjects::AmmoCrate* crate = m_world.GetAmmoCrate(crateIndex); crate != nullptr)
    {
        if (const Transform* where = m_scene.GetTransform(crate->entity); where != nullptr)
        {
            PlaySound(m_sounds.pickup.Pick(), where->position, 0.55f, 0.85f);
        }
    }
    m_app->GetConsole().Print("Took " + std::to_string(taken) + " rounds for the " + definition->name);
    PRED_LOG_INFO(Gameplay, "Resupplied {} rounds from crate {}", taken, crateIndex);
}

void PredationGame::DropSelected()
{
    if (m_hidingSpot >= 0)
    {
        return;
    }
    const Inventory::Slot slot = m_inventory.Selected();
    if (slot.IsEmpty())
    {
        return;
    }

    // What this one is carrying goes with it. For the weapon in hand that is the live state rather
    // than the slot's copy, which is only written back when something else comes out.
    const bool inHand = m_inventory.SelectedSlot() == m_ammoSlot && m_weapon.HasWeapon();
    const int rounds = inHand ? m_weapon.rounds : slot.rounds;
    const int reserve = inHand ? m_weapon.reserve : slot.reserve;

    const int removed = m_inventory.RemoveFromSlot(m_inventory.SelectedSlot(), 1);
    if (removed <= 0)
    {
        return;
    }
    if (inHand)
    {
        // The weapon has left the hand, so the state that was riding on it belongs to nothing now.
        m_weapon = WeaponState{};
        m_ammoSlot = Inventory::kNoSlot;
    }

    const PlayerView& view = m_player.View();
    const glm::vec3 origin = view.eyePosition + view.Forward() * 0.6f;
    const glm::vec3 throwVelocity = view.Forward() * 2.5f;

    DropIntoWorld(slot.item, removed, rounds, reserve, origin, throwVelocity);
}

// Puts one thing on the floor, wherever it came from.
//
// A client asks to put it down and waits to be told. Dropping locally made an item nobody else had:
// it could not be picked up, the indices the two machines used stopped agreeing, and dropping the
// same thing twice made two of it.
void PredationGame::DropIntoWorld(ItemId item, int count, int rounds, int reserve,
                                  const glm::vec3& origin, const glm::vec3& velocity)
{
    if (m_sessionMode == SessionMode::Client)
    {
        DropMessage message;
        message.item = static_cast<uint16_t>(item);
        message.count = static_cast<uint8_t>(count);
        message.rounds = LoadToWire(rounds);
        message.reserve = LoadToWire(reserve);
        message.position = origin;
        message.velocity = velocity;
        m_client.SendDrop(message);
        return;
    }

    const int index =
        m_world.SpawnPickup(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_items,
                            item, count, origin, velocity, rounds, reserve);

    // Everyone else has to see it land, and it has to be there to pick up. The throw is sent too,
    // so it arcs on their screen rather than appearing on the floor.
    if (m_sessionMode == SessionMode::Host && index >= 0)
    {
        m_host.Broadcast(PickupSpawnedEvent(static_cast<uint8_t>(index), static_cast<uint16_t>(item),
                                            static_cast<uint8_t>(count), LoadToWire(rounds),
                                            LoadToWire(reserve), origin, velocity));
    }
}

// Everything you were carrying goes on the floor where you fell.
//
// Death is meant to matter, and the way it matters most in an extraction game is that what you were
// carrying stops being yours. Scattered rather than stacked, so a kill leaves a spread of things to
// pick through rather than one pile occupying a single point.
void PredationGame::DropEverything()
{
    const glm::vec3 at = m_player.State().position + glm::vec3(0.0f, 0.35f, 0.0f);
    for (int i = 0; i < m_inventory.SlotCount(); ++i)
    {
        const Inventory::Slot slot = m_inventory.At(i);
        if (slot.IsEmpty())
        {
            continue;
        }
        // Whatever was in the hand carries its live magazine; the rest carry the slot's copy.
        const bool inHand = i == m_ammoSlot && m_weapon.HasWeapon();
        const int rounds = inHand ? m_weapon.rounds : slot.rounds;
        const int reserve = inHand ? m_weapon.reserve : slot.reserve;

        const float angle = glm::two_pi<float>() * static_cast<float>(i) /
                            static_cast<float>(std::max(m_inventory.SlotCount(), 1));
        const glm::vec3 scatter{std::cos(angle) * 1.2f, 1.4f, std::sin(angle) * 1.2f};
        DropIntoWorld(slot.item, slot.count, rounds, reserve, at, scatter);
    }

    m_inventory.Clear();
    m_weapon = WeaponState{};
    m_ammoSlot = Inventory::kNoSlot;
}

void PredationGame::EnterHidingSpot(int index)
{
    WorldObjects::HidingSpot* spot = m_world.GetHidingSpot(index);
    if (spot == nullptr || spot->occupied)
    {
        return;
    }

    spot->occupied = true;
    m_hidingSpot = index;
    // Attached, not teleported. A locker is barely wider than the player's capsule, so left to
    // simulate normally Jolt pushes them straight back out through the side of it.
    m_player.Attach(spot->insidePosition, spot->insideYaw);
    m_lookYaw = spot->insideYaw;
    // The door swings shut behind the player, which is most of what makes hiding feel like hiding.
    m_world.SetDoorOpen(spot->doorIndex, false, m_interactions);
    m_interactions.SetVerb(spot->entity, "Leave");
    m_app->GetConsole().Print("Hidden. Press " + KeyFor(m_app->GetInput(), "interact") + " to leave.");
}

void PredationGame::LeaveHidingSpot()
{
    WorldObjects::HidingSpot* spot = m_world.GetHidingSpot(m_hidingSpot);
    m_hidingSpot = -1;
    if (spot == nullptr)
    {
        return;
    }
    spot->occupied = false;
    m_world.SetDoorOpen(spot->doorIndex, true, m_interactions);
    m_interactions.SetVerb(spot->entity, "Hide in");
    m_player.Detach(spot->exitPosition);
}

void PredationGame::OnFixedUpdate(double fixedDt)
{
    const auto dt = static_cast<float>(fixedDt);
    m_world.Update(m_scene, m_app->GetPhysics(), m_interactions, dt);

    PlayerInput input = BuildPlayerInput();
    if (m_screen == Screen::Title)
    {
        // The world keeps simulating behind the menu so the backdrop is alive and starting a game
        // is instant, but nothing the player does at the menu reaches the character.
        input = PlayerInput{};
        input.yaw = m_player.State().yaw;
        input.pitch = m_player.State().pitch;
    }
    // Dead counts as restrained: a body on the floor does not fire, aim or reload.
    const bool restrained =
        m_hidingSpot >= 0 || m_cameraMode == CameraMode::Fly || !m_player.State().alive;

    // The weapon runs before the movement, because aiming down the sights slows the player and the
    // controller needs that this tick rather than next.
    WeaponInput weaponInput;
    if (!restrained && !m_inventoryOpen)
    {
        Input& raw = m_app->GetInput();
        weaponInput.trigger = raw.IsActionDown("fire") || m_debugTriggerTicks > 0;
        // And only where the sights would mean anything. Against a wall the body refuses to raise
        // them, so the simulation refuses too: otherwise the player would be walking at aiming
        // pace and shooting at aiming accuracy while looking at a weapon held at their hip.
        weaponInput.aim = (raw.IsActionDown("aim") || m_debugAim) && m_body.AimHasRoom();
        if (raw.WasActionPressed("reload"))
        {
            m_reloadLatch = 30;
        }
        weaponInput.reload = m_reloadLatch > 0;
    }
    m_debugTriggerTicks = std::max(m_debugTriggerTicks - 1, 0);

    m_shots.clear();
    if (const WeaponDefinition* definition = EquippedWeapon())
    {
        WeaponSim::Step(*definition, weaponInput, m_weapon, MuzzlePosition(), AimDirection(), dt, m_shots);
        input.speedScale = 1.0f - (1.0f - definition->aimSpeedScale) * m_weapon.aim;
    }
    else
    {
        WeaponSim::Step(WeaponDefinition{}, weaponInput, m_weapon, MuzzlePosition(), AimDirection(), dt,
                        m_shots);
    }

    // Whatever the weapon just kicked goes into the player's own aim, where it stays for them to
    // pull back down. Straight after the step, so the same tick that fires is the tick that moves.
    ApplyRecoilToView();

    // Single player, so this process is the authority. When there is a host, a client stops here and
    // sends m_shots instead; nothing above this line ever touches another player's health.
    ResolveShots();

    // The request is held for half a second rather than one tick, so pressing reload part way
    // through a shot still takes effect once the weapon can accept it.
    m_reloadLatch = m_weapon.IsReloading() ? 0 : std::max(m_reloadLatch - 1, 0);

    if (m_hidingSpot >= 0)
    {
        // Hidden: the player can still look around, and that is all. Crouching or going prone
        // inside a locker put the body through the floor of it, and there is nowhere to lean to.
        input.move = glm::vec2(0.0f);
        input.jump = false;
        input.lean = 0.0f;
        input.crouchHeld = false;
        input.proneHeld = false;
        m_crouchToggleState = false;
        m_proneToggleState = false;
        m_forceCrouch = false;
        m_forceProne = false;
    }
    if (m_cameraMode == CameraMode::Fly)
    {
        // The free camera is an inspection tool, not a different game mode. The player keeps
        // simulating underneath it, standing still and holding its own facing, so the body stays
        // alive and stances can be examined from outside. Skipping the step entirely froze the
        // player and made every stance look identical.
        input.move = glm::vec2(0.0f);
        input.jump = false;
        input.yaw = m_player.State().yaw;
        input.pitch = m_player.State().pitch;
    }

    // In a session the host and the client each own how the local player is stepped: the host runs
    // it directly and then everyone else, the client runs it inside prediction so that the first
    // guess and every replay of it go through exactly the same code.
    if (!StepSession(input, dt))
    {
        m_player.Step(input, dt);
    }

    UpdateHostMigration(dt);
    UpdateRespawns(dt);

    // The toggles mirror the stance the body is actually in, every tick, not just when a change is
    // refused. They are a request, and the body is the answer; a request that has been answered is
    // spent. Anything else lets the two drift apart, and then the next press asks for the stance you
    // are already in and nothing happens.
    //
    // On a client this matters twice over. The host owns the stance, and reconciliation writes its
    // answer over the prediction: with the toggle left alone, a client could hold "standing" while
    // the host held "prone", stand up locally every tick and be pulled back down by the next
    // snapshot. Spamming the key was the quickest way to get there.
    if (cv_crouchToggle.Get())
    {
        m_crouchToggleState = m_player.State().stance == PlayerStance::Crouching;
        m_proneToggleState = m_player.State().stance == PlayerStance::Prone;
    }
}

void PredationGame::OnUpdate(double dt, double alpha)
{
    m_time += dt;

    Application& app = *m_app;
    Input& input = app.GetInput();
    Renderer& renderer = app.GetRenderer();
    const auto deltaSeconds = static_cast<float>(dt);

    // --- Input that is sampled per frame, not per tick ------------------------------------------
    SampleLook(deltaSeconds);

    // The editor drives its own camera and hides the world, so nothing below has to know it exists.
    //
    // The cursor stays free, because everything in an editor is done by pointing at it. Holding the
    // right button takes the mouse for as long as it is held and turns the camera, then hands it
    // straight back. Keeping the mouse locked the whole time, as this used to, made every panel
    // unreachable; releasing it without this made the camera impossible to turn.
    if (m_editor.IsOpen())
    {
        const bool wantLook = input.IsMouseDown(MouseButton::Right) &&
                              (m_editorLooking || !app.IsUiCapturingMouse());
        if (wantLook != m_editorLooking)
        {
            m_editorLooking = wantLook;
            m_wantMouseCaptured = wantLook;
            UpdateMouseCapture();
        }
        if (m_editorLooking && !m_discardNextMouseDelta)
        {
            const glm::vec2 delta = input.MouseDelta();
            const float sensitivity = glm::radians(cv_mouseSensitivity.Get());
            m_lookYaw += delta.x * sensitivity;
            m_lookPitch = std::clamp(m_lookPitch - delta.y * sensitivity, glm::radians(-89.0f),
                                     glm::radians(89.0f));
        }
        m_discardNextMouseDelta = false;

        m_editor.Camera().yaw = m_lookYaw;
        m_editor.Camera().pitch = m_lookPitch;
        m_editor.Camera().moveSpeed = m_editor.CameraSpeed();
        // WASD moves whenever the editor has the keyboard, held button or not: an editor where you
        // have to hold a mouse button to walk is one you cannot drive with one hand.
        //
        // Ctrl is left out of it. The fly camera takes Ctrl as "down", and in here Ctrl is the
        // modifier on undo and redo, so every Ctrl+Z sank the view. Q and E do up and down instead,
        // which is where an editor usually puts them, and Space still lifts.
        m_editor.Camera().ctrlMovesDown = false;
        const bool typing = ImGui::GetIO().WantCaptureKeyboard;
        if (!typing)
        {
            m_editor.Camera().Update(input, deltaSeconds, false);
            const float vertical = (input.IsActionDown("lean_right") ? 1.0f : 0.0f) -
                                   (input.IsActionDown("lean_left") ? 1.0f : 0.0f);
            m_editor.Camera().position.y += vertical * m_editor.CameraSpeed() * deltaSeconds;
        }
        m_editor.Update(m_editorScene, app.GetMeshes(), deltaSeconds);
        UpdateEditorBody(deltaSeconds);

        // The editor knows how big the model is and the game owns the camera, so framing is asked
        // for rather than done. A model that opens off screen looks exactly like one that failed to
        // load, and either way the first thing anyone does is go hunting for it.
        glm::vec3 framePosition{0.0f};
        glm::vec3 frameTarget{0.0f};
        if (m_editor.TakeFrameRequest(framePosition, frameTarget))
        {
            m_camera.position = framePosition;
            const glm::vec3 toTarget = frameTarget - framePosition;
            if (glm::length(toTarget) > 1e-4f)
            {
                const glm::vec3 look = glm::normalize(toTarget);
                m_lookYaw = std::atan2(look.x, -look.z);
                m_lookPitch = std::asin(std::clamp(look.y, -1.0f, 1.0f));
            }
            m_editor.Camera().position = m_camera.position;
        }

        m_camera = m_editor.Camera();

        // Pointing at the viewport: grabbing a handle, dragging it, or selecting what is under it.
        // The game owns the camera and the pointer and the editor owns the model, so the ray is
        // built here and every question about it answered there. Not while the right button is
        // held, because that is looking around, and not over a panel, because that belongs to the
        // panel.
        if (!m_editorLooking && (!app.IsUiCapturingMouse() || m_editor.Dragging()))
        {
            const float width = static_cast<float>(std::max<int>(renderer.Width(), 1));
            const float height = static_cast<float>(std::max<int>(renderer.Height(), 1));
            const glm::vec2 pointer = input.MousePosition();

            // From the pointer through the near plane, in the same projection the frame was drawn
            // with. Working it out from the field of view rather than by unprojecting a matrix keeps
            // this readable, and the two agree because both start from the same cvar.
            const float aspect = width / height;
            const float halfHorizontal = std::tan(glm::radians(cv_fov.Get()) * 0.5f);
            const float halfVertical = halfHorizontal / aspect;
            const float x = (pointer.x / width) * 2.0f - 1.0f;
            const float y = 1.0f - (pointer.y / height) * 2.0f;

            const glm::vec3 forward = m_camera.Forward();
            const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
            const glm::vec3 up = glm::cross(right, forward);
            const glm::vec3 direction = glm::normalize(forward + right * (x * halfHorizontal) +
                                                       up * (y * halfVertical));
            // A press either grabs a handle or selects. Grabbing wins, because the handles sit on
            // top of the thing they move and a click on one is never a click on it.
            if (input.WasMousePressed(MouseButton::Left))
            {
                if (!m_editor.BeginDrag(m_camera.position, direction))
                {
                    m_editor.SelectUnderRay(m_camera.position, direction);
                }
            }
            else if (m_editor.Dragging())
            {
                if (input.IsMouseDown(MouseButton::Left))
                {
                    m_editor.UpdateDrag(m_camera.position, direction);
                }
                else
                {
                    m_editor.EndDrag();
                }
            }
        }

        // Undo. Ctrl+Z and Ctrl+Y, because those are the two every editor uses and an editor that
        // does not have them is one nobody dares experiment in.
        if (!app.IsConsoleOpen() && !app.IsUiCapturingKeyboard())
        {
            const bool control =
                input.IsKeyDown(SDL_SCANCODE_LCTRL) || input.IsKeyDown(SDL_SCANCODE_RCTRL);
            const bool shift =
                input.IsKeyDown(SDL_SCANCODE_LSHIFT) || input.IsKeyDown(SDL_SCANCODE_RSHIFT);
            if (control && input.WasKeyPressed(SDL_SCANCODE_Z))
            {
                if (shift)
                {
                    m_editor.Redo();
                }
                else
                {
                    m_editor.Undo();
                }
            }
            if (control && input.WasKeyPressed(SDL_SCANCODE_Y))
            {
                m_editor.Redo();
            }
            if (!control && input.WasKeyPressed(SDL_SCANCODE_F))
            {
                m_editor.FrameModel();
            }
        }
    }

    // Nothing bound to a game key does anything at the menu. The menu is pointed at and typed into,
    // and a stray W while filling in an address must not make the character walk.
    //
    // Nor does anything while dead. A body on the floor was still picking things up, opening doors,
    // getting into lockers, dropping its inventory and changing what it had in hand: none of the
    // actions asked whether the player was alive, because the camera moves to a teammate and it
    // looked as though nothing was being driven any more. Movement is already refused by the
    // controller; this is everything else.
    if (!app.IsConsoleOpen() && m_screen == Screen::Playing && m_player.State().alive)
    {
        if (input.WasActionPressed("jump"))
        {
            m_jumpLatch = true;
        }
        // Latched rather than applied here, for the same reason a jump is. Fixed updates run before
        // this function every frame, so a stance toggled here was not seen by the simulation until
        // the frame after, and on a frame with no fixed step at all it could be missed entirely.
        if (input.WasActionPressed("crouch"))
        {
            m_crouchPressLatch = true;
        }
        if (input.WasActionPressed("prone"))
        {
            m_pronePressLatch = true;
        }
        if (input.WasActionPressed("sprint") && cv_sprintToggle.Get())
        {
            m_sprintToggleState = !m_sprintToggleState;
        }
        if (input.WasActionPressed("interact"))
        {
            // Alive it opens doors; dead it is the only control there is, so it moves you to the
            // next teammate. A free camera is deliberately not offered: it would show a dead player
            // where the creature is, which is the one thing being dead should not tell them.
            if (m_player.State().alive)
            {
                TryInteract();
            }
            else
            {
                m_spectateNext = true;
            }
        }
        if (input.WasActionPressed("drop"))
        {
            DropSelected();
        }
        if (input.WasActionPressed("inventory"))
        {
            m_inventoryOpen = !m_inventoryOpen;
            // The panel is clickable, so it needs the pointer back.
            m_wantMouseCaptured = !m_inventoryOpen;
        }
        // Nothing changes hands while both of them are on a ledge. The climb has put away whatever
        // was out and will take it back out at the top; letting a number key run in the middle of
        // that would leave the wrong thing in the hands when the climb finished, because the climb
        // restores what it took rather than what is selected now.
        if (HandsAreFree())
        {
            for (int slot = 0; slot < 6; ++slot)
            {
                if (input.WasActionPressed("slot_" + std::to_string(slot + 1)))
                {
                    // Pressing the number for what is already out puts it away again. One key for
                    // both, because a separate holster key is one nobody finds.
                    m_inventory.SelectSlot(m_inventory.SelectedSlot() == slot ? Inventory::kNoSlot
                                                                              : slot);
                }
            }
            if (const float wheel = input.WheelDelta(); std::abs(wheel) > 0.1f)
            {
                m_inventory.SelectNext(wheel > 0.0f ? -1 : 1);
            }
        }
#if PRED_DEV_TOOLS
        if (input.WasActionPressed("toggle_camera"))
        {
            // Cycles first person, third person, fly. Third person exists so the body animation can
            // actually be watched, which is impossible from inside the head.
            SetCameraMode(m_cameraMode == CameraMode::FirstPerson    ? CameraMode::ThirdPerson
                          : m_cameraMode == CameraMode::ThirdPerson ? CameraMode::Fly
                                                                    : CameraMode::FirstPerson);
        }
#endif
        if (input.WasActionPressed("flashlight"))
        {
            m_torchOn = !m_torchOn;
            PlaySound((m_torchOn ? m_sounds.pickup : m_sounds.drop).Pick(), m_player.State().position, 0.25f,
                      m_torchOn ? 1.6f : 1.4f, false);
        }
#if PRED_DEV_TOOLS
        if (input.WasActionPressed("respawn"))
        {
            RespawnLocalPlayer(m_spawnPoint);
        }
#endif
        if (input.WasActionPressed("quit_capture"))
        {
            // Escape opens the pause menu and frees the pointer; Escape again closes it and takes
            // the pointer back. It used to leave the game outright on the second press, which meant
            // there was no way to let go of the mouse for a moment without ending up at the title
            // screen, and no way to leave deliberately either: the same key did both.
            //
            // Settings is a page inside the pause menu, so Escape there goes back one step rather
            // than all the way out. Escape should undo the last thing you opened, and dropping
            // straight into the game from three levels in is a surprise every time.
            if (m_paused && m_settingsOpen)
            {
                m_settingsOpen = false;
            }
            else
            {
                m_paused = !m_paused;
                m_wantMouseCaptured = !m_paused;
            }
        }
    }

    // Escape leaves the editor for the menu. It is a place rather than an overlay, so leaving it is
    // going somewhere else rather than switching something off.
    if (!app.IsConsoleOpen() && m_screen == Screen::Editor &&
        input.WasActionPressed("quit_capture") && !m_editorLooking)
    {
        ReturnToTitle();
    }
    UpdateMouseCapture();

    // --- Camera -----------------------------------------------------------------------------------
    // Always update the player's view, even while flying. It is presentation-only, and the body is
    // placed from its interpolated position, so skipping it would leave the body behind at the
    // origin whenever the free camera is active.
    m_player.UpdateView(deltaSeconds, static_cast<float>(alpha));

    glm::mat4 view;
    glm::vec3 viewPosition;
    switch (m_cameraMode)
    {
    case CameraMode::Fly:
        if (m_screen == Screen::Title)
        {
            // The menu drives the camera itself, and typing an address into it must not fly it.
            UpdateTitleCamera(deltaSeconds);
        }
        else
        {
            m_camera.yaw = m_lookYaw;
            m_camera.pitch = m_lookPitch;
            m_camera.moveSpeed = cv_flySpeed.Get();
            m_camera.Update(input, deltaSeconds, false); // look is applied above, this only moves
        }
        view = m_camera.View();
        viewPosition = m_camera.position;
        break;

    case CameraMode::ThirdPerson:
    {
        // Orbits the torso along the look direction. Framing on the body itself, rather than a fixed
        // height above the feet, keeps the character centred in every stance; a constant height
        // works for standing and then looks straight over the top of them once they lie down. The
        // body follows the interpolated position, so the camera inherits that and does not judder.
        const glm::vec3 focus = (m_body.GetPose().GlobalPosition(m_body.Rig().head) +
                                 m_body.GetPose().GlobalPosition(m_body.Rig().pelvis)) *
                                0.5f;
        const float orbitYaw = m_lookYaw + m_orbitYaw;
        const float orbitPitch = std::clamp(m_lookPitch + m_orbitPitch, glm::radians(-85.0f),
                                            glm::radians(85.0f));
        const float cp = std::cos(orbitPitch);
        const glm::vec3 lookDirection{std::sin(orbitYaw) * cp, std::sin(orbitPitch),
                                      -std::cos(orbitYaw) * cp};

        // Pull the camera in when something is in the way, so it does not end up inside a wall or a
        // pillar with the player hidden behind it.
        constexpr float kCameraSkin = 0.25f;
        m_thirdPersonDistance = std::clamp(cv_thirdDistance.Get(), 0.5f, 20.0f);
        float distance = m_thirdPersonDistance;
        const RayHit blocked =
            app.GetPhysics().RayCast(focus, -lookDirection, m_thirdPersonDistance + kCameraSkin);
        if (blocked)
        {
            distance = std::max(0.35f, blocked.distance - kCameraSkin);
        }

        viewPosition = focus - lookDirection * distance;
        // Never let the orbit drop the camera through the floor. Looking down at a prone character
        // puts the camera below a focus that is itself only a few centimetres up, and the occlusion
        // ray then pulls it hard into the body. The player's own feet are the ground reference.
        viewPosition.y = std::max(viewPosition.y, m_player.View().renderPosition.y + 0.4f);
        view = glm::lookAtRH(viewPosition, focus, glm::vec3(0.0f, 1.0f, 0.0f));
        break;
    }

    case CameraMode::FirstPerson:
    default:
    {
        // Dead, the camera moves to a living teammate's eyes. Their look angles come from the
        // snapshot, so you see what they see rather than steering a camera of your own.
        const RemotePlayerView* watched = nullptr;
        if (m_spectating >= 0)
        {
            for (const RemotePlayerView& remote : RemotePlayers())
            {
                if (remote.id == m_spectating)
                {
                    watched = &remote;
                    break;
                }
            }
        }

        // Their body is already being posed on this machine every frame, and it is anchored to an
        // eye that eases between stance heights exactly as your own does. Taking that eye rather
        // than working a second one out here is what stops the camera and the head it is inside
        // disagreeing: they were computed separately, so crouching moved one before the other and
        // the camera spent the difference inside the skull.
        RemoteAvatar* avatar = watched != nullptr ? AvatarFor(watched->id) : nullptr;
        if (avatar != nullptr)
        {
            PlayerView spectated = avatar->view;
            // The same eased angles their body is posed with, not the raw ones off the wire. Taking
            // the wire's meant the camera and the head it is inside were looking in slightly
            // different directions, and the difference arrived in steps.
            spectated.yaw = avatar->lookYaw;
            spectated.pitch = avatar->lookPitch;
            view = spectated.ViewMatrix();
            viewPosition = spectated.eyePosition;
            m_spectateEyeHeight = avatar->view.eyeHeight;
        }
        else if (watched != nullptr)
        {
            // No body for them yet, on the frame they joined. Eased, so the fallback does not snap
            // either.
            const float target = m_player.Config().EyeHeightForStance(watched->stance);
            m_spectateEyeHeight =
                m_spectateEyeHeight <= 0.0f
                    ? target
                    : m_spectateEyeHeight + (target - m_spectateEyeHeight) *
                                                (1.0f - std::exp(-m_player.Config().eyeTransitionSpeed *
                                                                 deltaSeconds));

            const glm::vec3 leanRight{std::cos(watched->yaw), 0.0f, std::sin(watched->yaw)};
            PlayerView spectated;
            spectated.eyePosition = watched->position + glm::vec3(0.0f, m_spectateEyeHeight, 0.0f) +
                                    leanRight * (watched->leanAmount * m_player.Config().leanSideOffset);
            spectated.yaw = watched->yaw;
            spectated.pitch = watched->pitch;
            spectated.leanRoll =
                glm::radians(m_player.Config().leanAngleDegrees) * watched->leanAmount;
            view = spectated.ViewMatrix();
            viewPosition = spectated.eyePosition;
        }
        else
        {
        {
            // The weapon's shove, on the camera and on nothing else.
            //
            // It never touches the look angles, so it cannot move where the player is aiming and
            // cannot fight them for the mouse: rounds still go exactly where the crosshair was. It
            // is the jolt a shot gives the head, and it is over in a fraction of a second.
            PlayerView shaken = m_player.View();
            shaken.pitch += glm::radians(m_weapon.shakePitch);
            shaken.yaw += glm::radians(m_weapon.shakeYaw);
            view = shaken.ViewMatrix();
        }
            viewPosition = m_player.View().eyePosition;
        }
        break;
    }
    }

    // What is in the player's hands has to be settled before the body is posed, not after. Deciding
    // it afterwards left a newly equipped weapon sitting at the world origin for one frame, which
    // reads as the gun flying in from the middle of the map.
    //
    // The selected slot decides what is held, so this is checked every frame rather than hooked
    // onto each of the several places a slot can change. A climb is one of those places: it puts
    // whatever was out away and takes it back at the top, which has to happen before the sync sees
    // the slot rather than a frame after it.
    UpdateMantleStow();
    SyncEquippedWeapon();
    // Inside a locker there is nowhere to hold a rifle: it is stowed rather than drawn, because a
    // metre of barrel held in front of the chest goes straight through the door.
    const WeaponDefinition* weapon = m_hidingSpot >= 0 ? nullptr : EquippedWeapon();
    m_body.SetWeapon(m_scene, app.GetMeshes(), weapon);
    if (weapon != nullptr)
    {
        PlayerBody::WeaponPose pose;
        pose.aim = m_weapon.aim;
        pose.reloading = m_weapon.IsReloading();
        // Runs 0 at the start of the reload to 1 at the end, so the animation does not have to know
        // how long any particular weapon takes.
        pose.reload = pose.reloading ? ReloadProgress() : -1.0f;
        pose.kick = m_weaponKick;
        pose.draw = m_weaponDraw;
        pose.holster = m_weaponHolster;
        m_body.SetWeaponPose(pose);
    }

    // The kick is presentation, so it decays per frame rather than per tick. Firing sets it to one
    // in ResolveShots, which is the only place that knows a round actually left the barrel.
    m_weaponKick = std::max(m_weaponKick - deltaSeconds * 7.0f, 0.0f);
    // Drawing a weapon runs 0 to 1 over its own moment, so swapping is a movement rather than a
    // substitution.
    m_weaponDraw = std::min(m_weaponDraw + deltaSeconds * 3.2f, 1.0f);
    m_lastFrameSeconds = deltaSeconds;
    if (m_holdReportIn > 0 && --m_holdReportIn == 0)
    {
        ReportHold();
    }

    // The body follows the simulation every frame. Its head is only drawn from the fly camera,
    // because in first person the camera sits inside it.
    m_body.Tuning().hideHead = m_cameraMode == CameraMode::FirstPerson && m_player.State().alive;

    // Death hands the body over to the ragdoll, once. Everything below it is animation, and that is
    // exactly what stops.
    if (!m_player.State().alive && !m_localCollapsed)
    {
        m_body.Collapse(m_deathImpulse);
        m_localCollapsed = true;
        // And the bag goes on the floor. Once, on the frame the player goes down, which is what the
        // collapse latch is already for.
        DropEverything();
    }
    else if (m_player.State().alive && m_localCollapsed)
    {
        m_body.Revive();
        m_localCollapsed = false;
    }

    m_body.Update(m_scene, m_player.State(), m_player.View(), m_player.Config(), app.GetPhysics(),
                  deltaSeconds);

    UpdateSpectating();

    // Everyone else, driven the same way from replicated state.
    if (m_sessionMode != SessionMode::Offline)
    {
        m_client.UpdateInterpolation(deltaSeconds);
        SyncRemoteAvatars(deltaSeconds);
    }
    // And the loose items, which the host owns and everyone else follows.
    if (m_sessionMode == SessionMode::Client)
    {
        m_world.FollowNetworkState(m_app->GetPhysics(), deltaSeconds);
    }

    // A join finishes when the host answers, which can be a moment after the button was pressed.
    if (m_screen == Screen::Title && m_sessionMode == SessionMode::Client && m_client.Connected())
    {
        EnterWorld();
        // And only now ask what has already happened. The world this answer describes exists as of
        // this line; asked any earlier, the answer arrives before there is anything to apply it to
        // and the build that follows wipes it.
        m_client.SendReady();
    }

    const float aspect = renderer.Height() > 0
                             ? static_cast<float>(renderer.Width()) / static_cast<float>(renderer.Height())
                             : 16.0f / 9.0f;
    const float horizontal = glm::radians(cv_fov.Get());
    const float verticalFov = 2.0f * std::atan(std::tan(horizontal * 0.5f) / aspect);
    const glm::mat4 projection = renderer.HomogeneousDepth()
                                     ? glm::perspectiveRH_NO(verticalFov, aspect, 0.05f, 500.0f)
                                     : glm::perspectiveRH_ZO(verticalFov, aspect, 0.05f, 500.0f);
    renderer.SetCamera(view, projection);

    // Where the eye actually is and which way it faces, whichever camera is driving.
    //
    // m_camera is the free-flying inspection camera and is not where the player is looking unless
    // that is the mode. Taking both out of the view matrix instead works for first person, third
    // person, the free camera and spectating somebody else, because it is the matrix all four of
    // them produced.
    const glm::vec3 eyePosition = viewPosition;
    const glm::vec3 eyeForward = -glm::vec3(view[0][2], view[1][2], view[2][2]);

    // --- World ------------------------------------------------------------------------------------
    Environment& environment = m_scene.GetEnvironment();
    environment.fogStart = cv_fogStart.Get();
    environment.fogEnd = cv_fogEnd.Get();
    environment.sunIntensity = cv_sunIntensity.Get();
    environment.exposure = cv_exposure.Get();
    environment.contrast = cv_contrast.Get();

    // The flashlight, carried at the shoulder rather than screwed to the eye.
    //
    // A torch held in the hand is the honest version and it is also the one that fights everything
    // else: the hand moves with the weapon, the weapon moves with the wall corrections, and the beam
    // would swing around the room every time the gun was nudged. Carried near the eye it points
    // roughly where the player is looking, which is what anybody wants from a torch.
    //
    // "Roughly", because it does not point exactly there. A torch is a thing with weight held in a
    // hand on the end of an arm, and none of that turns at the speed of a wrist on a mouse. The beam
    // trails the view and catches up, which is most of what makes a light read as carried rather
    // than as a property of the camera -- and in a dark room it is the difference between a beam
    // that sweeps a wall and a spot that is simply always in the middle of the screen.
    //
    // Offset down and to the side so the beam has some parallax against the walls: exactly on the
    // eye, every surface is lit face-on and the room reads flat.
    {
        PunctualLight& torch = environment.lights[0];
        // Not while dead, and not while looking out of somebody else's head.
        //
        // Spectating puts the camera at the spectated player's eye, and this light is placed from
        // that eye -- so a dead player's own torch was being hung on the person they were watching,
        // on top of that person's torch, at the same place and pointing the same way. Two lights in
        // one spot is twice the brightness, which is what "when you die it stacks your flashlight
        // when spectating" was. A corpse does not hold a torch.
        const bool torchLit = m_torchOn && m_screen == Screen::Playing &&
                              m_player.State().alive && m_spectating < 0;
        if (torchLit)
        {
            // Straight up or straight down leaves no sideways direction to offset along, and
            // normalising that zero vector would put the torch at NaN and take the whole frame's
            // shading with it. The offset simply goes away at the poles, which is where it matters
            // least: the beam is on the floor or the ceiling and has no wall to rake across.
            const glm::vec3 across = glm::cross(eyeForward, glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::vec3 right =
                glm::length(across) > 1e-3f ? glm::normalize(across) : glm::vec3(0.0f);

            // Snapped into place the first frame it comes on, then eased. Easing from wherever the
            // beam was last pointing means switching the torch on after a turn sweeps it across the
            // room to catch up, which looks like a fault rather than like weight.
            const glm::vec3 wanted = eyeForward;
            if (!m_torchAimed)
            {
                m_torchAim = wanted;
                m_torchAimed = true;
            }
            else
            {
                const float follow = 1.0f - std::exp(-cv_torchFollow.Get() * deltaSeconds);
                m_torchAim = glm::normalize(glm::mix(m_torchAim, wanted, follow) + glm::vec3(1e-5f));
            }

            // Ahead of the weapon, not level with the eye.
            //
            // A light beside the eye has the player's own rifle a hand span in front of its lens and
            // square in the middle of its beam, and an inverse square at that range blows the weapon
            // to white however the falloff is softened. Real weapon lights are clamped near the
            // muzzle for exactly this reason: put the source past the thing you are holding and the
            // thing you are holding stops being the brightest object in the room.
            torch.position = eyePosition + eyeForward * cv_torchReach.Get() + right * 0.10f -
                             glm::vec3(0.0f, 0.10f, 0.0f);
            torch.direction = m_torchAim;
            torch.color = glm::vec3(1.0f, 0.97f, 0.88f);
            torch.intensity = cv_torchIntensity.Get();
            torch.range = cv_torchRange.Get();
            torch.innerAngle = cv_torchInner.Get();
            torch.outerAngle = cv_torchOuter.Get();
        }
        else
        {
            torch.intensity = 0.0f;
            // So it snaps to the view next time rather than easing over from where it was left.
            m_torchAimed = false;
        }
        torch.sourceRadius = cv_torchSourceRadius.Get();
    }

    // And the flash throws light, which is most of what sells it.
    //
    // A muzzle flash that does not light anything is a picture stuck on the end of the barrel: the
    // room stays exactly as dark as it was while a fire the size of a fist burns in the middle of
    // it. One frame of a wall, a hand and the ceiling lit from below is worth more than any amount
    // of work on the shape, and in a dark corridor it is the only thing that tells you where you
    // are.
    //
    // A bulb rather than a cone -- an explosion at the crown throws light everywhere, including back
    // down the weapon and onto the player's own arms. Short range, because it is small and brief;
    // very bright, because for the moment it exists it is the brightest thing in the level.
    {
        PunctualLight& flash = environment.lights[1];
        const float strength = m_body.MuzzleFlashStrength();
        if (strength > 0.001f && m_screen == Screen::Playing)
        {
            flash.position = m_body.MuzzlePoint();
            flash.direction = glm::vec3(0.0f, -1.0f, 0.0f);
            flash.color = glm::vec3(1.0f, 0.86f, 0.62f);
            flash.intensity = cv_flashIntensity.Get() * strength;
            flash.range = cv_flashRange.Get();
            flash.innerAngle = 180.0f;
            flash.outerAngle = 180.0f;
            flash.sourceRadius = 0.35f;
        }
        else
        {
            flash.intensity = 0.0f;
        }
    }

    // And everybody else's torches and muzzle flashes, into whatever slots are left.
    //
    // Without this, a light is visible only to the player carrying it. Two people standing in the
    // same dark room saw two different rooms: each one's own torch lit the walls for them and for
    // nobody else, and a shot fired beside you lit nothing. That is the largest way two machines
    // disagreed about what the world looks like, and none of it was the renderer -- the lights were
    // simply never in anybody else's scene.
    //
    // There are four slots and up to seven things that want one, so they compete. The score is what
    // a light actually contributes at the eye -- its brightness over the square of how far away it
    // is -- which puts your own torch first by a mile, because it is at the eye, and picks the
    // nearest of everybody else's after that. Cutting the faintest is the one choice that cannot be
    // noticed.
    {
        struct Candidate
        {
            PunctualLight light;
            float score = 0.0f;
        };
        std::vector<Candidate> candidates;
        const auto consider = [&](const PunctualLight& light)
        {
            if (light.intensity <= 0.0f || light.range <= 0.0f)
            {
                return;
            }
            const float distance = glm::length(light.position - eyePosition);
            candidates.push_back({light, light.intensity / std::max(distance * distance, 0.05f)});
        };

        // The two already filled in above keep their meaning; they are re-entered here so they
        // compete on the same terms as everyone else's rather than being special.
        consider(environment.lights[0]);
        consider(environment.lights[1]);

        for (const std::unique_ptr<RemoteAvatar>& avatar : m_avatars)
        {
            if (avatar == nullptr || !avatar->built || !avatar->state.alive)
            {
                continue;
            }
            if (avatar->torchOn)
            {
                // From their eye, pointing where they are looking. Not eased the way the local one
                // is: that easing is about the weight of a thing held in your own hand, and from
                // outside it is a beam that already lags behind their head by a network round trip.
                const glm::vec3 forward = avatar->view.Forward();
                PunctualLight torch;
                torch.position = avatar->view.eyePosition + forward * cv_torchReach.Get();
                torch.direction = forward;
                torch.color = glm::vec3(1.0f, 0.97f, 0.88f);
                torch.intensity = cv_torchIntensity.Get();
                torch.range = cv_torchRange.Get();
                torch.innerAngle = cv_torchInner.Get();
                torch.outerAngle = cv_torchOuter.Get();
                torch.sourceRadius = cv_torchSourceRadius.Get();
                consider(torch);
            }

            const float strength = avatar->body.MuzzleFlashStrength();
            if (strength > 0.001f)
            {
                PunctualLight flash;
                flash.position = avatar->body.MuzzlePoint();
                flash.direction = glm::vec3(0.0f, -1.0f, 0.0f);
                flash.color = glm::vec3(1.0f, 0.86f, 0.62f);
                flash.intensity = cv_flashIntensity.Get() * strength;
                flash.range = cv_flashRange.Get();
                flash.innerAngle = 180.0f;
                flash.outerAngle = 180.0f;
                flash.sourceRadius = 0.35f;
                consider(flash);
            }
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
        for (size_t i = 0; i < kMaxPunctualLights; ++i)
        {
            if (i < candidates.size())
            {
                environment.lights[i] = candidates[i].light;
            }
            else
            {
                environment.lights[i] = PunctualLight{};
                environment.lights[i].intensity = 0.0f;
            }
        }
    }

    renderer.SetClearColor(0x11131aff);

    // What the player can reach, decided by where they are looking rather than by proximity.
    if (m_hidingSpot >= 0)
    {
        m_interactions.ClearFocus();
    }
    else
    {
        const PlayerView& playerView = m_player.View();
        m_interactions.UpdateFocus(m_scene, app.GetPhysics(), playerView.eyePosition,
                                   playerView.Forward());
    }

    AgeTracers(deltaSeconds);
    // Marks left on anything the physics moves go where it goes.
    FollowBulletHoles();
    // Beacons, out and in. Here rather than on the browser screen because the outgoing half belongs
    // to a running game: somebody hosting has to stay findable after they go in, which is exactly
    // when the screen that would have ticked it has stopped being drawn.
    UpdateDiscovery(deltaSeconds);
    // The relay carrier owns a socket and has to be read. Done here rather than inside the lobby
    // screen because the lobby screen stops being drawn the moment the game starts, and the socket
    // carries the game.
    if (m_relay != nullptr)
    {
        m_relay->Poll(deltaSeconds);
        // And said out loud if it has gone, once rather than every frame.
        //
        // A host whose lobby the relay has dropped keeps playing perfectly well -- the game is
        // host-authoritative and everybody already in it stays in it -- but the code on the
        // invitation is dead and nobody new can arrive. That is worth knowing, and from inside the
        // world there is nothing at all to see.
        const bool failed = m_relay->Status() == RelayCarrier::State::Failed;
        if (failed && !m_relayFailed)
        {
            PRED_LOG_WARN(Network, "The relay lobby has gone: {}", m_relay->Message());
            // Not copied into m_titleStatus while the lobby screen is up: that screen already prints
            // the carrier's own message, so putting it here as well printed the whole paragraph
            // twice, one above the other.
            if (!m_inLobby)
            {
                m_titleStatus = m_relay->Message();
            }
        }
        m_relayFailed = failed;
    }
    // Before the voice and the rest: while a key is being rebound, that key press belongs to the
    // settings screen and to nothing else.
    UpdateRebinding();
    UpdateVoice(deltaSeconds);
    UpdateMicrophoneTest(deltaSeconds);
    UpdateSounds(deltaSeconds);
    SyncDynamicProps();
    app.GetSceneRenderer().SetWireframe(cv_wireframe.Get());

    // Occlusion settings, pushed every frame rather than when they change, so the console and the
    // graphics page move the same numbers and neither has to tell the renderer it did.
    ShadowSettings& shadows = app.GetSceneRenderer().Shadows();
    shadows.sunEnabled = cv_sunShadows.Get();
    shadows.skyEnabled = cv_skyShadows.Get();
    shadows.distance = std::clamp(cv_shadowDistance.Get(), 10.0f, 120.0f);
    shadows.indoorLight = std::clamp(cv_indoorLight.Get(), 0.0f, 1.0f);
    shadows.sunBias = cv_sunShadowBias.Get();
    shadows.sunNormalOffset = cv_sunShadowOffset.Get();
    shadows.skyBias = cv_skyShadowBias.Get();
    shadows.skyNormalOffset = cv_skyShadowOffset.Get();
    shadows.spotEnabled = cv_spotShadows.Get();
    shadows.debugView = std::clamp(cv_occlusionDebug.Get(), 0, 4);
    app.SetEntityCount(m_scene.EntityCount());
}

void PredationGame::AnchorTracer(Tracer& tracer) const
{
    tracer.anchored = false;
    const PhysicsWorld& physics = m_app->GetPhysics();
    if (!tracer.hit || !tracer.body.IsValid() || !physics.IsValid(tracer.body) ||
        physics.MotionOf(tracer.body) == BodyMotion::Static)
    {
        return;
    }
    // Taken now, before the shove from this same round has had a physics step to act on it.
    const Transform body = physics.GetTransform(tracer.body);
    const glm::quat inverse = glm::inverse(body.rotation);
    tracer.localTo = inverse * (tracer.to - body.position);
    tracer.localNormal = inverse * tracer.normal;
    tracer.anchored = true;
}

void PredationGame::PlaceBulletHole(const glm::vec3& at, const glm::vec3& normal, BodyHandle on)
{
    // Laid onto the surface it hit, wrapped round it.
    //
    // It used to be one flat disc on the tangent plane at the point of impact, which is exact on a
    // wall and a flat plate stuck to anything round. The level's pillars and its sphere are triangle
    // meshes, so the normal a round finds on them is the normal of the one facet it struck, and near
    // the top of the sphere that facet is nearly level: the mark lay flat as a table while the
    // surface fell away round it. Every vertex is now dropped onto the real surface under it, so the
    // mark bends with the curve and shrinks at an edge instead of hanging off it.
    if (m_bulletHoles.empty() || m_bulletHoleMeshes.size() != m_bulletHoles.size())
    {
        return;
    }

    const size_t slot = m_nextBulletHole;
    const Entity hole = m_bulletHoles[slot];
    m_nextBulletHole = (m_nextBulletHole + 1) % m_bulletHoles.size();

    Transform* transform = m_scene.GetTransform(hole);
    MeshRenderer* renderer = m_scene.GetMeshRenderer(hole);
    if (transform == nullptr || renderer == nullptr)
    {
        return;
    }

    // Turned to a different angle each time and sized a little differently, so a wall somebody has
    // emptied a magazine into does not read as the same stamp repeated.
    const float spin = glm::two_pi<float>() * static_cast<float>(std::rand()) /
                       static_cast<float>(RAND_MAX);
    const float size = 0.85f + 0.3f * static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);

    PhysicsWorld& physics = m_app->GetPhysics();
    const BulletHolePlacement placed =
        ConformBulletHole(m_bulletHoleShape, physics, at, normal, spin, size);
    renderer->mesh = m_app->GetMeshes().Replace(m_bulletHoleMeshes[slot], placed.mesh);
    transform->position = placed.position;
    transform->rotation = placed.rotation;
    // The size is in the vertices now, because it had to be before they were dropped onto the
    // surface: scaling a wrapped mark afterwards would lift its edge off the curve it was fitted to.
    transform->scale = glm::vec3(1.0f);
    renderer->visible = renderer->mesh.IsValid();

    // And what it is stuck to, so it goes where that goes.
    //
    // A wall never moves and needs nothing. A crate, a dropped weapon or anything else the physics
    // simulates does, and a mark left at the world position the round arrived at simply hangs there
    // once the thing slides out from under it. Kept in the body's own frame, so following it is a
    // multiply rather than a running correction.
    HoleAttachment& attachment = m_bulletHoleAttachments[slot];
    attachment = HoleAttachment{};
    if (on.IsValid() && physics.IsValid(on) && physics.MotionOf(on) != BodyMotion::Static)
    {
        const Transform body = physics.GetTransform(on);
        const glm::quat inverse = glm::inverse(body.rotation);
        attachment.body = on;
        attachment.localPosition = inverse * (transform->position - body.position);
        attachment.localRotation = inverse * transform->rotation;
    }
}

// Every hole that is stuck to something, moved to where that something is now.
//
// Run every frame rather than on a change, because a physics body does not announce that it has
// moved. Ninety-six transforms is nothing next to the draw that follows it, and the alternative --
// asking each body whether it has settled -- costs more than doing the work.
void PredationGame::FollowBulletHoles()
{
    const PhysicsWorld& physics = m_app->GetPhysics();
    for (size_t i = 0; i < m_bulletHoles.size() && i < m_bulletHoleAttachments.size(); ++i)
    {
        HoleAttachment& attachment = m_bulletHoleAttachments[i];
        if (!attachment.body.IsValid())
        {
            continue;
        }
        MeshRenderer* renderer = m_scene.GetMeshRenderer(m_bulletHoles[i]);
        Transform* transform = m_scene.GetTransform(m_bulletHoles[i]);
        if (renderer == nullptr || transform == nullptr || !renderer->visible)
        {
            continue;
        }
        // The thing it was on has been destroyed -- a pickup taken, a prop cleared. The mark goes
        // with it rather than being left behind in the air, which is the same fault by another road.
        if (!physics.IsValid(attachment.body))
        {
            renderer->visible = false;
            attachment = HoleAttachment{};
            continue;
        }
        const Transform body = physics.GetTransform(attachment.body);
        transform->position = body.position + body.rotation * attachment.localPosition;
        transform->rotation = body.rotation * attachment.localRotation;
    }
}

// Every hole gone, for a new game.
//
// The pool is made once and reused, so nothing about starting a game touched it and a fresh match
// opened with the last one's gunfire still written on the walls.
void PredationGame::ClearBulletHoles()
{
    for (const Entity hole : m_bulletHoles)
    {
        if (MeshRenderer* renderer = m_scene.GetMeshRenderer(hole))
        {
            renderer->visible = false;
        }
    }
    m_bulletHoleAttachments.assign(m_bulletHoles.size(), HoleAttachment{});
    m_nextBulletHole = 0;
}

void PredationGame::SpawnProp(bool sphere, float impulse)
{
    PhysicsWorld& physics = m_app->GetPhysics();

    const glm::vec3 origin = m_cameraMode == CameraMode::Fly ? m_camera.position : m_player.View().eyePosition;
    const glm::vec3 forward = m_cameraMode == CameraMode::Fly ? m_camera.Forward() : m_player.View().Forward();

    Transform transform;
    // Spread successive props over a small grid: dropping several into the same point leaves them
    // deeply interpenetrating, and the separation impulse flings them across the map.
    //
    // Counted from the middle of the grid, so the first one lands where the view is pointing. It
    // used to start in a corner, which put a single dropped ball most of a metre to the side and
    // most of a metre closer than it was asked for -- under the gun, out of sight.
    const int index = static_cast<int>(m_props.size());
    const int cell = (index + 4) % 9;
    constexpr float spread = 0.9f;
    const glm::vec3 jitter{static_cast<float>(cell % 3 - 1) * spread,
                           static_cast<float>(index / 9) * spread,
                           static_cast<float>(cell / 3 - 1) * spread};
    transform.position = origin + forward * 2.5f + jitter;

    const BodyHandle body = sphere
                                ? physics.CreateSphere(kPropRadius, transform, BodyMotion::Dynamic, 400.0f)
                                : physics.CreateBox(glm::vec3(kPropSize * 0.5f), transform,
                                                    BodyMotion::Dynamic, 400.0f);
    if (!body.IsValid())
    {
        m_app->GetConsole().PrintError("Could not create a physics body for the prop");
        return;
    }
    physics.SetLinearVelocity(body, forward * impulse);

    const Entity entity = m_scene.CreateMeshEntity("prop", transform,
                                                   sphere ? m_propSphereMesh : m_propBoxMesh, kPropMaterial);
    m_props.push_back(DynamicProp{entity, body});
}

void PredationGame::ClearProps()
{
    PhysicsWorld& physics = m_app->GetPhysics();
    for (const DynamicProp& prop : m_props)
    {
        physics.DestroyBody(prop.body);
        m_scene.Destroy(prop.entity);
    }
    m_props.clear();
}

void PredationGame::SyncDynamicProps()
{
    PhysicsWorld& physics = m_app->GetPhysics();
    for (const DynamicProp& prop : m_props)
    {
        Transform* transform = m_scene.GetTransform(prop.entity);
        if (transform != nullptr && physics.IsValid(prop.body))
        {
            *transform = physics.GetTransform(prop.body);
        }
    }
}

void PredationGame::OnRender()
{
    Application& app = *m_app;

    // Inventory icons are drawn into their own offscreen target before the world is. It happens on
    // one frame only, because items do not change; the reserved view ids sort ahead of the UI that
    // samples the result.
    m_itemIcons.Render(app.GetSceneRenderer(), app.GetMeshes());

    // The menu draws its own backdrop, so the world is not drawn behind it. Nothing else changes:
    // it is still built and still simulating, so starting a game is still instant.
    if (m_screen == Screen::Title)
    {
        return;
    }

    // The editor draws its own scene. The level is not behind it, which is the point of it being a
    // place rather than a mode: a model is judged against an empty floor.
    if (m_screen == Screen::Editor)
    {
        // The panel first, into its own target: its view id sorts ahead of everything on screen, so
        // what the UI samples this frame is this frame's picture rather than the last one's.
        RenderEditorFirstPerson();
        app.GetSceneRenderer().Draw(Renderer::kViewMain, m_editorScene, app.GetMeshes(),
                                    m_camera.position);
        DrawDebugOverlays();
        return;
    }

    const glm::vec3 viewPosition = m_cameraMode == CameraMode::Fly ? m_camera.position : m_player.View().eyePosition;
    // Depth from the sun and depth from overhead, both fitted around the eye, before anything is
    // shaded. This is where a room with a roof on it becomes dark: nothing declares it dark, the
    // roof is simply between it and the sky.
    app.GetSceneRenderer().RenderShadows(Renderer::kViewSunShadow, Renderer::kViewSkyShadow,
                                         Renderer::kViewSpotShadow, m_scene, app.GetMeshes(),
                                         viewPosition);

    // And the mirrors, which is the world drawn a second time from a camera reflected across their
    // plane. Skipped entirely when the eye is behind them or too far away to see one: this is a
    // whole extra pass over the scene, and it is worth nothing at all from the wrong side.
    //
    // The near limit is generous rather than tight. A mirror going flat as you back away from it is
    // far more noticeable than a mirror you cannot make out, so this is set to where the panels are
    // a few pixels across rather than to where the reflection stops being informative.
    app.GetSkyRenderer().SetBrightness(cv_skyBrightness.Get());
    const float inFront =
        glm::dot(viewPosition, glm::vec3(TestMapSpec::kMirrorPlane)) + TestMapSpec::kMirrorPlane.w;
    if (cv_reflections.Get() && inFront > 0.05f && inFront < cv_reflectionDistance.Get())
    {
        app.GetSceneRenderer().RenderReflection(
            Renderer::kViewReflectionSky, Renderer::kViewReflection, m_scene, app.GetMeshes(),
            TestMapSpec::kMirrorPlane, app.GetRenderer().ViewMatrix(), app.GetRenderer().ProjectionMatrix(),
            viewPosition);
    }
    else
    {
        // Explicitly, rather than by leaving it alone: a stale reflection from wherever the camera
        // was standing last time it saw one is worse than no reflection at all.
        app.GetSceneRenderer().NoReflection();
    }
    // The sky first, into the same view, so the world covers it where there is world.
    app.GetSkyRenderer().Draw(Renderer::kViewSky, m_scene.GetEnvironment(),
                              app.GetRenderer().ViewMatrix(), app.GetRenderer().ProjectionMatrix());
    app.GetSceneRenderer().Draw(Renderer::kViewMain, m_scene, app.GetMeshes(), viewPosition);
    DrawDebugOverlays();
}

void PredationGame::DrawConnectionReadout()
{
    if (m_sessionMode == SessionMode::Offline)
    {
        return;
    }
    // Under the frame counter, top right.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 8.0f, 84.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.65f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##connection", nullptr, flags))
    {
        if (m_sessionMode == SessionMode::Client)
        {
            ImGui::Text("Ping %u ms", static_cast<unsigned>(m_client.PingMs()));
        }
        else
        {
            // The host has no ping of its own, so it sees everybody else's: the one number that says
            // whether a friend complaining of lag has a bad connection or the host does.
            const int players = 1 + static_cast<int>(m_host.ConnectedCount());
            ImGui::Text("Hosting, %d player%s", players, players == 1 ? "" : "s");
            for (const RemotePlayerView& remote : m_host.Remotes())
            {
                if (remote.id != 0)
                {
                    ImGui::Text("%s  %u ms", remote.name.c_str(), static_cast<unsigned>(remote.pingMs));
                }
            }
        }
    }
    ImGui::End();
}

void PredationGame::DrawDebugOverlays()
{
    DebugDraw& draw = m_app->GetDebugDraw();

    if (m_editor.IsOpen())
    {
        m_editor.DrawOverlays(draw);
        return;
    }

    // A round is drawn as something that travels, with a short lit section and a mark where it
    // arrives. Lighting the whole line at once, which is what this used to do, draws a diagram of
    // a shot rather than a shot, and it stayed on screen long enough to read as a laser.
    for (const Tracer& tracer : m_tracers)
    {
        const glm::vec3 travel = tracer.to - tracer.from;
        const float distance = glm::length(travel);
        if (distance < 1e-3f)
        {
            continue;
        }
        const glm::vec3 direction = travel / distance;
        const float head = tracer.age * kTracerSpeed;

        if (head < distance)
        {
            const float tail = std::max(head - kTracerLength, 0.0f);
            // Dimmer the further it has gone, so a long shot thins out down range.
            const float bright = 1.0f - 0.40f * (head / distance);
            const auto level = [bright](float channel)
            { return static_cast<uint8_t>(std::clamp(channel * bright, 0.0f, 255.0f)); };
            draw.Line(tracer.from + direction * tail, tracer.from + direction * head,
                      Color::RGBA(level(255.0f), level(226.0f), level(150.0f)));
        }
        else if (tracer.hit && tracer.surface && !tracer.marked)
        {
            // Arrived. The round leaves a hole where it landed and that is the whole of the effect:
            // a burst that flashes and vanishes says only that something happened, and a hole says
            // what happened and where, and is still saying it a minute later. Reading a room by the
            // marks in it is worth more to this game than a spark is.
            //
            // `marked` rather than a test on the age, so it happens on the one frame the round
            // arrives rather than on every frame the tracer survives afterwards.
            const_cast<Tracer&>(tracer).marked = true;
            //
            // Where the round struck is taken from the body it struck, as that body stands now, when
            // there is one that moves: the shove from this same round has had the whole flight of
            // the drawn tracer to carry it away from the point in the world where it was hit.
            glm::vec3 at = tracer.to;
            glm::vec3 facing = tracer.normal;
            const PhysicsWorld& physics = m_app->GetPhysics();
            if (tracer.anchored && physics.IsValid(tracer.body))
            {
                const Transform body = physics.GetTransform(tracer.body);
                at = body.position + body.rotation * tracer.localTo;
                facing = body.rotation * tracer.localNormal;
            }
            PlaceBulletHole(at, facing, tracer.body);
        }
    }

    // The line a round was actually traced along, which is not the line it is drawn along: rounds
    // are traced from the eye so the crosshair tells the truth, and drawn from the muzzle so they
    // look right. Seeing both at once is the only way to check that difference has not become a
    // lie, and it is a developer's question, so it lives behind a toggle rather than on screen.
    if (DebugCategories::IsEnabled(DebugCategory::Combat))
    {
        for (const Tracer& tracer : m_tracers)
        {
            draw.Line(tracer.origin, tracer.to, Color::kMagenta);
            draw.Line(tracer.from, tracer.to, tracer.hit ? Color::kYellow : Color::kGrey);
            if (tracer.hit)
            {
                draw.Sphere(tracer.to, 0.045f, Color::kRed, 8);
            }
        }
    }

    // The sockets on the weapon as it is actually held, with a line to the hand that is meant to be
    // at each. Placing a grip is a matter of looking at where the hand lands, and there is nothing
    // else that shows both at once.
    if (m_screen == Screen::Editor && m_benchSockets && m_editorBodyBuilt)
    {
        const glm::quat hold = m_editorBody.WeaponRotation();
        const glm::vec3 origin = m_editorBody.WeaponOrigin();
        const auto mark = [&](const glm::vec3& socket, BoneIndex bone, uint32_t colour)
        {
            const glm::vec3 world = origin + hold * socket;
            draw.Sphere(world, 0.018f, colour, 8);
            draw.Line(world, m_editorBody.GetPose().GlobalPosition(bone), colour);
        };
        mark(m_editorBody.Weapon().triggerGrip, m_editorBody.Rig().hand[1], Color::kGreen);
        mark(m_editorBody.Weapon().supportGrip, m_editorBody.Rig().hand[0], Color::kCyan);
        draw.Sphere(origin + hold * m_editorBody.Weapon().muzzle, 0.014f, Color::kYellow, 8);
        draw.Axes(glm::translate(glm::mat4(1.0f), origin) * glm::mat4_cast(hold), 0.12f);
    }

    if (cv_showGrid.Get())
    {
        draw.Grid(24.0f, 1.0f, 0.02f);
        draw.Axes(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.02f, 0.0f)), 1.5f);
    }

    if (DebugCategories::IsEnabled(DebugCategory::Physics))
    {
        m_app->GetPhysics().DebugDraw(draw);
    }

    if (DebugCategories::IsEnabled(DebugCategory::Player) && m_cameraMode != CameraMode::FirstPerson)
    {
        // Only worth drawing the capsule when it is not wrapped around the camera.
        m_player.DebugDraw(draw);
    }

    if (DebugCategories::IsEnabled(DebugCategory::Animation))
    {
        m_body.DebugDraw(draw);
    }

    if (DebugCategories::IsEnabled(DebugCategory::Rendering))
    {
        const MeshLibrary& meshes = m_app->GetMeshes();
        m_scene.ForEachMeshRenderer(
            [&](Entity, const Transform& transform, const MeshRenderer& renderer)
            {
                const Mesh* mesh = meshes.Get(renderer.mesh);
                if (mesh == nullptr || !mesh->bounds.IsValid())
                {
                    return;
                }
                // Drawn in the entity's own frame. Building an axis-aligned box from the position
                // and scale alone ignored the rotation, so anything turned showed a bounds box
                // sitting at an angle to the collider it was supposed to describe.
                const glm::mat4 model =
                    transform.Matrix() * glm::translate(glm::mat4(1.0f), mesh->bounds.Center());
                draw.BoxOriented(model, mesh->bounds.HalfExtents(), Color::kCyan);
            });
    }
}

void PredationGame::DrawPlayerPanel()
{
    PlayerConfig& config = m_player.Config();
    const PlayerState& state = m_player.State();
    const PlayerController::Debug& diagnostics = m_player.Diagnostics();

    ImGui::Text("Stance %-9s %s", PlayerStanceName(state.stance),
                state.stanceBlocked ? "(blocked overhead)" : "");
    ImGui::Text("Speed %5.2f / %5.2f m/s   %s", diagnostics.horizontalSpeed, diagnostics.targetSpeed,
                state.grounded ? "grounded" : (state.onSteepSlope ? "on steep slope" : "airborne"));
    ImGui::Text("Position %.2f %.2f %.2f", state.position.x, state.position.y, state.position.z);
    ImGui::Text("Vertical %6.2f m/s   slope %4.1f deg   step %+.3f m", state.velocity.y,
                diagnostics.slopeAngleDegrees, diagnostics.lastStepUp);
    ImGui::Text("Health %.0f %s", state.health, state.alive ? "" : "(dead)");
    ImGui::TextUnformatted(m_mouseCaptured ? "Mouse captured. Escape frees the cursor."
                                           : "Cursor free. Escape or click the world to look again.");
    if (state.landingImpactSpeed > 0.0f)
    {
        ImGui::Text("Last landing impact %.1f m/s", state.landingImpactSpeed);
    }

    ImGui::Separator();
    if (ImGui::Button("Respawn"))
    {
        RespawnLocalPlayer(m_spawnPoint);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save to player.json"))
    {
        config.SaveToFile(PlayerConfigPath());
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload"))
    {
        ReloadPlayerConfig();
    }

    bool capsuleChanged = false;

    if (ImGui::CollapsingHeader("Speeds", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("Move", &config.moveSpeed, 0.5f, 10.0f, "%.2f m/s");
        ImGui::SliderFloat("Sprint", &config.sprintSpeed, 0.5f, 14.0f, "%.2f m/s");
        ImGui::SliderFloat("Slow walk", &config.walkSpeed, 0.2f, 6.0f, "%.2f m/s");
        ImGui::SliderFloat("Crouch", &config.crouchSpeed, 0.2f, 6.0f, "%.2f m/s");
        ImGui::SliderFloat("Prone", &config.proneSpeed, 0.1f, 3.0f, "%.2f m/s");
        ImGui::SliderFloat("Backward scale", &config.backwardScale, 0.3f, 1.0f, "%.2f");
        ImGui::SliderFloat("Strafe scale", &config.strafeScale, 0.3f, 1.0f, "%.2f");
    }

    if (ImGui::CollapsingHeader("Acceleration", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("Ground accel", &config.groundAcceleration, 5.0f, 200.0f, "%.0f m/s2");
        ImGui::SliderFloat("Ground decel", &config.groundDeceleration, 5.0f, 200.0f, "%.0f m/s2");
        ImGui::SliderFloat("Air accel", &config.airAcceleration, 0.0f, 60.0f, "%.1f m/s2");
    }

    if (ImGui::CollapsingHeader("Jump and gravity", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("Jump height", &config.jumpHeight, 0.1f, 2.5f, "%.2f m");
        ImGui::SliderFloat("Gravity scale", &config.gravityScale, 0.5f, 4.0f, "%.2f");
        ImGui::SliderFloat("Fall gravity scale", &config.fallGravityScale, 0.5f, 5.0f, "%.2f");
        ImGui::SliderFloat("Coyote time", &config.coyoteTime, 0.0f, 0.4f, "%.3f s");
        ImGui::SliderFloat("Jump buffer", &config.jumpBufferTime, 0.0f, 0.4f, "%.3f s");
    }

    if (ImGui::CollapsingHeader("Capsule"))
    {
        capsuleChanged |= ImGui::SliderFloat("Stand height", &config.standHeight, 1.0f, 2.4f, "%.2f m");
        capsuleChanged |= ImGui::SliderFloat("Crouch height", &config.crouchHeight, 0.6f, 1.6f, "%.2f m");
        capsuleChanged |= ImGui::SliderFloat("Prone height", &config.proneHeight, 0.3f, 1.0f, "%.2f m");
        capsuleChanged |= ImGui::SliderFloat("Radius", &config.radius, 0.15f, 0.6f, "%.2f m");
        ImGui::SliderFloat("Step up", &config.stepHeight, 0.05f, 0.8f, "%.2f m");
        ImGui::SliderFloat("Max slope", &config.maxSlopeAngle, 20.0f, 70.0f, "%.0f deg");
        ImGui::TextUnformatted("Step up and slope apply on respawn.");
    }

    if (ImGui::CollapsingHeader("Camera feel", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("Stand eye", &config.standEyeHeight, 1.0f, 2.0f, "%.2f m");
        ImGui::SliderFloat("Crouch eye", &config.crouchEyeHeight, 0.5f, 1.5f, "%.2f m");
        ImGui::SliderFloat("Prone eye", &config.proneEyeHeight, 0.1f, 1.0f, "%.2f m");
        ImGui::SliderFloat("Eye transition", &config.eyeTransitionSpeed, 1.0f, 30.0f, "%.1f");
        ImGui::SliderFloat("Step smoothing", &config.stepSmoothSpeed, 1.0f, 40.0f, "%.1f");
        ImGui::SliderFloat("Landing dip", &config.landingDipPerSpeed, 0.0f, 0.05f, "%.4f m per m/s");
        ImGui::SliderFloat("Landing dip max", &config.landingDipMax, 0.0f, 0.6f, "%.2f m");
        ImGui::SliderFloat("Landing recovery", &config.landingRecoverSpeed, 1.0f, 25.0f, "%.1f");
        ImGui::SliderFloat("Footfall dip", &config.bobAmount, 0.0f, 0.16f, "%.3f m");
        ImGui::SliderFloat("Walk lower", &config.bobWalkLower, 0.0f, 0.16f, "%.3f m");
        ImGui::SliderFloat("Dip at speed", &config.bobSprintScale, 1.0f, 3.0f, "%.2f x");
        ImGui::SliderFloat("Max pitch", &config.maxPitchDegrees, 45.0f, 89.9f, "%.1f deg");
    }

    if (ImGui::CollapsingHeader("Fall damage"))
    {
        ImGui::Checkbox("Enabled", &config.fallDamageEnabled);
        ImGui::SliderFloat("Free below", &config.fallDamageMinSpeed, 1.0f, 25.0f, "%.1f m/s");
        ImGui::SliderFloat("Lethal at", &config.fallDamageLethalSpeed, 5.0f, 60.0f, "%.1f m/s");
    }

    if (ImGui::CollapsingHeader("Body and gait", ImGuiTreeNodeFlags_DefaultOpen))
    {
        PlayerBody::Config& body = m_body.Tuning();
        bool visible = body.visible;
        if (ImGui::Checkbox("Show body", &visible))
        {
            m_body.SetVisible(m_scene, visible);
        }
        ImGui::SliderFloat("Stride base", &config.strideLengthBase, 0.4f, 2.0f, "%.2f m");
        ImGui::SliderFloat("Stride per m/s", &config.strideLengthPerSpeed, 0.0f, 0.6f, "%.3f m");
        ImGui::SliderFloat("Stance walk", &config.stanceFractionWalk, 0.4f, 0.85f, "%.2f");
        ImGui::SliderFloat("Stance run", &config.stanceFractionRun, 0.3f, 0.7f, "%.2f");
        ImGui::SliderFloat("Foot lift", &body.stepHeight, 0.0f, 0.4f, "%.3f m");
        ImGui::SliderFloat("Step reach", &body.stepReachMargin, 0.7f, 0.99f, "%.2f");
        ImGui::SliderFloat("Hip sway", &body.hipSwayAmount, 0.0f, 0.12f, "%.3f m");
        ImGui::SliderFloat("Hip bob", &body.hipBobAmount, 0.0f, 0.12f, "%.3f m");
        ImGui::SliderFloat("Lean per m/s", &body.leanPerSpeed, 0.0f, 6.0f, "%.2f deg");
        ImGui::SliderFloat("Max lean", &body.maxLean, 0.0f, 40.0f, "%.0f deg");
        ImGui::SliderFloat("Arm swing", &body.armSwingDegrees, 0.0f, 60.0f, "%.0f deg");
        ImGui::SliderFloat("Foot smoothing", &body.footPlantSmoothing, 2.0f, 60.0f, "%.0f");
        ImGui::TextUnformatted("Skeleton overlay: enable the ANIMATION debug category.");
    }

    if (capsuleChanged)
    {
        m_player.ApplyConfigToCharacter();
    }
}

void PredationGame::DrawCondition()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    // What shape the player is in, bottom left. Two bars and no numbers: how hurt you are and how
    // much running is left are things to glance at, and a number invites arithmetic in a game that
    // is meant to be making you panic.
    const float left = viewport->Pos.x + 26.0f;
    const float bottom = viewport->Pos.y + viewport->Size.y - 26.0f;
    constexpr float kWidth = 168.0f;
    constexpr float kHeight = 7.0f;
    constexpr float kGap = 9.0f;

    const auto bar = [&](float y, float fill, ImU32 colour)
    {
        const ImVec2 from{left, y};
        const ImVec2 to{left + kWidth, y + kHeight};
        draw->AddRectFilled(from, to, IM_COL32(14, 16, 20, 150), 2.0f);
        if (fill > 0.0f)
        {
            draw->AddRectFilled(from, {left + kWidth * std::clamp(fill, 0.0f, 1.0f), to.y}, colour, 2.0f);
        }
        draw->AddRect(from, to, IM_COL32(120, 126, 138, 130), 2.0f);
    };

    const PlayerState& state = m_player.State();
    const float health = std::clamp(state.health / 100.0f, 0.0f, 1.0f);
    // Reddens as it empties, so the colour says the same thing as the length.
    const ImU32 healthColour = health > 0.6f   ? IM_COL32(150, 190, 160, 220)
                               : health > 0.3f ? IM_COL32(210, 180, 110, 225)
                                               : IM_COL32(205, 90, 80, 235);
    bar(bottom - kHeight, health, healthColour);

    // Stamina is only worth the space when it is not full, or when it has run out, which is exactly
    // when the player needs to know about it.
    if (state.stamina < 0.999f)
    {
        const ImU32 staminaColour =
            state.winded ? IM_COL32(200, 120, 90, 220) : IM_COL32(140, 165, 195, 200);
        bar(bottom - kHeight * 2.0f - kGap, state.stamina, staminaColour);
    }
}

void PredationGame::DrawHud()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 centre{viewport->Pos.x + viewport->Size.x * 0.5f,
                        viewport->Pos.y + viewport->Size.y * 0.5f};
    constexpr ImGuiWindowFlags kHudFlags = ImGuiWindowFlags_NoDecoration |
                                           ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                           ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground;

    // Reticle. The gap opens with the weapon's current cone, so the crosshair says where rounds can
    // actually go rather than always promising the centre of the screen.
    ImDrawList* draw = ImGui::GetBackgroundDrawList();

    // The microphone, top left, whenever voice could be sending.
    //
    // Everything about a microphone is otherwise invisible until somebody else says whether they
    // could hear you, which is a slow way to find out that Windows never granted the game the
    // device. This says three things at a glance: whether the microphone is open at all, how loud it
    // is hearing you, and whether what it hears is actually going out.
    //
    // Mostly a testing tool, which is why it is plain and in a corner rather than designed.
    //
    // Only with somebody to talk to. Alone it said "nobody to hear you" in the corner of every single
    // player game, which is true and of no use to anybody.
    if (cv_micMeter.Get() && m_screen == Screen::Playing && m_sessionMode != SessionMode::Offline)
    {
        const bool open = m_microphone.Running();
        const float level = std::clamp(m_voiceLevel, 0.0f, 1.0f);

        const ImVec2 origin{viewport->Pos.x + 18.0f, viewport->Pos.y + 18.0f};
        constexpr float kWidth = 150.0f;
        constexpr float kHeight = 9.0f;
        draw->AddRectFilled(origin, {origin.x + kWidth, origin.y + kHeight},
                            IM_COL32(18, 20, 26, 190), 2.0f);

        // Green while it is going out, amber while it is heard but held back by the gate, grey when
        // the microphone is shut. The colour is the state; the length is the loudness.
        const ImU32 colour = !open              ? IM_COL32(90, 94, 104, 200)
                             : m_voiceSending   ? IM_COL32(120, 210, 130, 230)
                                                : IM_COL32(220, 180, 90, 230);
        if (level > 0.001f)
        {
            draw->AddRectFilled(origin, {origin.x + kWidth * level, origin.y + kHeight}, colour, 2.0f);
        }

        // Where the gate opens, so a threshold can be set by watching your own voice cross a line
        // rather than by guessing at a number.
        if (cv_voiceOpenMic.Get())
        {
            const float mark = origin.x + kWidth * std::clamp(cv_voiceThreshold.Get(), 0.0f, 1.0f);
            draw->AddLine({mark, origin.y - 2.0f}, {mark, origin.y + kHeight + 2.0f},
                          IM_COL32(240, 200, 120, 220), 1.5f);
        }

        const std::string talkKey = "hold " + KeyFor(m_app->GetInput(), "voice") + " to talk";
        const std::string what = !cv_voiceEnabled.Get() ? std::string("voice off")
                                 : !open ? (cv_voiceOpenMic.Get() ? std::string("mic not working") : talkKey)
                                 : m_voiceSending ? std::string("talking")
                                                  : std::string("listening");
        draw->AddText({origin.x, origin.y + kHeight + 3.0f}, IM_COL32(200, 205, 215, 200),
                      what.c_str());
    }

    // The version, bottom left, quietly.
    //
    // It is on the title screen too, but by the time somebody is describing a problem they are
    // usually in the game and not looking at the menu. A screenshot of a bug that says which build
    // took it is worth a round of "which version were you running"; this whole session has had two
    // of those.
    {
        const char* version = "v" PRED_VERSION_STRING;
        const ImVec2 size = ImGui::CalcTextSize(version);
        draw->AddText({viewport->Pos.x + 10.0f, viewport->Pos.y + viewport->Size.y - size.y - 8.0f},
                      IM_COL32(180, 190, 200, 90), version);
    }
    const ImU32 reticleColor = IM_COL32(230, 230, 235, 150);
    float gap = 2.0f;
    if (const WeaponDefinition* weapon = EquippedWeapon())
    {
        // Half the vertical field of view maps to half the screen height, so a cone in degrees
        // converts to pixels through the same projection the world is drawn with.
        const float halfFov = glm::radians(cv_fov.Get()) * 0.5f;
        const float aspect = viewport->Size.x / std::max(viewport->Size.y, 1.0f);
        const float verticalHalfFov = std::atan(std::tan(halfFov) / std::max(aspect, 0.01f));
        const float spread = glm::radians(WeaponSim::CurrentSpread(*weapon, m_weapon));
        gap = 2.0f + std::tan(spread) / std::max(std::tan(verticalHalfFov), 1e-3f) * viewport->Size.y * 0.5f;
        gap = std::min(gap, viewport->Size.y * 0.25f);
    }
    const float tick = 5.0f;
    draw->AddLine({centre.x - gap - tick, centre.y}, {centre.x - gap, centre.y}, reticleColor, 1.5f);
    draw->AddLine({centre.x + gap, centre.y}, {centre.x + gap + tick, centre.y}, reticleColor, 1.5f);
    draw->AddLine({centre.x, centre.y - gap - tick}, {centre.x, centre.y - gap}, reticleColor, 1.5f);
    draw->AddLine({centre.x, centre.y + gap}, {centre.x, centre.y + gap + tick}, reticleColor, 1.5f);
    // Interaction prompt, just below the reticle.
    const InteractionSystem::Focus& focus = m_interactions.CurrentFocus();
    const std::string prompt = m_hidingSpot >= 0 ? std::string("Leave Locker") : focus.prompt;
    if (!prompt.empty())
    {
        const std::string line = "[" + KeyFor(m_app->GetInput(), "interact") + "]  " + prompt;
        ImGui::SetNextWindowPos({centre.x, centre.y + 42.0f}, ImGuiCond_Always, {0.5f, 0.0f});
        if (ImGui::Begin("##Prompt", nullptr, kHudFlags))
        {
            ImGui::TextUnformatted(line.c_str());
        }
        ImGui::End();
    }

    // Hotbar: one icon per slot, with the selected one picked out and stack counts in the corner.
    constexpr float kSlotSize = 46.0f;
    constexpr float kSlotGap = 6.0f;

    ImGui::SetNextWindowPos({centre.x, viewport->Pos.y + viewport->Size.y - 16.0f}, ImGuiCond_Always,
                            {0.5f, 1.0f});
    ImGui::SetNextWindowBgAlpha(0.0f);
    if (ImGui::Begin("##Hotbar", nullptr, kHudFlags))
    {
        ImDrawList* list = ImGui::GetWindowDrawList();
        for (int i = 0; i < m_inventory.SlotCount(); ++i)
        {
            if (i > 0)
            {
                ImGui::SameLine(0.0f, kSlotGap);
            }

            const Inventory::Slot& slot = m_inventory.At(i);
            const bool selected = i == m_inventory.SelectedSlot();
            const ImVec2 origin = ImGui::GetCursorScreenPos();

            list->AddRectFilled(origin, {origin.x + kSlotSize, origin.y + kSlotSize},
                                IM_COL32(18, 20, 26, selected ? 190 : 130), 4.0f);
            list->AddRect(origin, {origin.x + kSlotSize, origin.y + kSlotSize},
                          selected ? IM_COL32(235, 205, 130, 235) : IM_COL32(120, 124, 134, 150), 4.0f,
                          0, selected ? 2.0f : 1.0f);

            if (const ItemDefinition* definition = m_items.Get(slot.item); definition != nullptr)
            {
                DrawItemIcon(slot.item, kSlotSize);
                if (slot.count > 1)
                {
                    const std::string count = std::to_string(slot.count);
                    list->AddText({origin.x + kSlotSize - 6.0f - ImGui::CalcTextSize(count.c_str()).x,
                                   origin.y + kSlotSize - 17.0f},
                                  IM_COL32(240, 240, 245, 235), count.c_str());
                }
            }

            const std::string number = std::to_string(i + 1);
            list->AddText({origin.x + 4.0f, origin.y + 2.0f},
                          selected ? IM_COL32(235, 205, 130, 220) : IM_COL32(150, 154, 164, 170),
                          number.c_str());

            ImGui::Dummy({kSlotSize, kSlotSize});
        }
    }
    ImGui::End();

    // How the player is doing. Nothing to do with what is in their hands, which is where this was
    // and why it only appeared when a weapon was out.
    DrawCondition();

    // Ammunition, bottom right, away from the hotbar. Reads magazine over reserve, the way a
    // shooter always has, and says so plainly while the magazine is out.
    if (const WeaponDefinition* weapon = EquippedWeapon())
    {
        ImGui::SetNextWindowPos({viewport->Pos.x + viewport->Size.x - 24.0f,
                                 viewport->Pos.y + viewport->Size.y - 20.0f},
                                ImGuiCond_Always, {1.0f, 1.0f});
        ImGui::SetNextWindowBgAlpha(0.0f);
        if (ImGui::Begin("##Ammo", nullptr, kHudFlags))
        {
            if (m_weapon.IsReloading())
            {
                ImGui::TextColored({0.85f, 0.80f, 0.55f, 1.0f}, "Reloading");
            }
            else
            {
                const ImVec4 colour = m_weapon.rounds == 0 ? ImVec4(0.88f, 0.42f, 0.38f, 1.0f)
                                                           : ImVec4(0.90f, 0.91f, 0.94f, 1.0f);
                ImGui::TextColored(colour, "%d / %d", m_weapon.rounds, m_weapon.reserve);
            }
            ImGui::TextDisabled("%s  %s", weapon->name.c_str(), FireModeName(weapon->mode));
        }
        ImGui::End();
    }

    DrawPlayerList();

    // Whose eyes these are, and how to move to somebody else's. Without it a dead player is looking
    // through a stranger with no way to tell whose view it is or that it can be changed.
    if (!m_player.State().alive && m_spectating >= 0)
    {
        std::string name = "a teammate";
        for (const RemotePlayerView& other : RemotePlayers())
        {
            if (static_cast<int>(other.id) == m_spectating && !other.name.empty())
            {
                name = other.name;
                break;
            }
        }
        ImGui::SetNextWindowPos({centre.x, viewport->Pos.y + viewport->Size.y - 110.0f},
                                ImGuiCond_Always, {0.5f, 1.0f});
        if (ImGui::Begin("##Spectating", nullptr, kHudFlags))
        {
            ImGui::TextColored({0.82f, 0.84f, 0.88f, 1.0f}, "Watching %s", name.c_str());
            ImGui::TextDisabled("[%s]  next", KeyFor(m_app->GetInput(), "interact").c_str());
        }
        ImGui::End();
    }

    if (m_inventoryOpen)
    {
        DrawInventoryPanel();
    }
}

void PredationGame::DrawPlayerList()
{
    // Only in a game with other people in it. On your own it is a list of one, which is a panel
    // asking the player to look at their own name.
    if (m_sessionMode == SessionMode::Offline)
    {
        return;
    }
    const std::vector<RemotePlayerView>& others = RemotePlayers();
    if (others.empty())
    {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->Pos.x + viewport->Size.x - 16.0f, viewport->Pos.y + 16.0f},
                            ImGuiCond_Always, {1.0f, 0.0f});
    ImGui::SetNextWindowBgAlpha(0.35f);
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (!ImGui::Begin("##Players", nullptr, kFlags))
    {
        ImGui::End();
        return;
    }

    // One row per player, this machine first. Health as a short bar rather than a number: from
    // across the screen a bar says "hurt" at a glance and a number has to be read.
    const auto row = [](const char* name, float health, bool alive, int pingMs, bool self,
                        bool spectated)
    {
        ImDrawList* list = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        constexpr float kBarWidth = 54.0f;
        constexpr float kBarHeight = 4.0f;

        const ImVec4 nameColour = !alive     ? ImVec4(0.62f, 0.40f, 0.38f, 1.0f)
                                 : spectated ? ImVec4(0.95f, 0.85f, 0.55f, 1.0f)
                                 : self      ? ImVec4(0.88f, 0.92f, 0.96f, 1.0f)
                                             : ImVec4(0.74f, 0.78f, 0.84f, 1.0f);
        ImGui::TextColored(nameColour, "%s%s", name, self ? " (you)" : "");
        ImGui::SameLine(132.0f);

        if (alive)
        {
            const ImVec2 barAt{origin.x + 132.0f, origin.y + ImGui::GetTextLineHeight() * 0.5f - 1.0f};
            const float fraction = glm::clamp(health / 100.0f, 0.0f, 1.0f);
            list->AddRectFilled(barAt, {barAt.x + kBarWidth, barAt.y + kBarHeight},
                                IM_COL32(60, 64, 72, 180));
            const ImU32 fill = fraction > 0.6f   ? IM_COL32(120, 190, 130, 220)
                               : fraction > 0.3f ? IM_COL32(210, 185, 110, 220)
                                                 : IM_COL32(205, 100, 90, 230);
            list->AddRectFilled(barAt, {barAt.x + kBarWidth * fraction, barAt.y + kBarHeight}, fill);
            ImGui::Dummy({kBarWidth, ImGui::GetTextLineHeight()});
        }
        else
        {
            ImGui::TextColored({0.62f, 0.40f, 0.38f, 1.0f}, "down");
        }

        ImGui::SameLine(200.0f);
        if (self && pingMs < 0)
        {
            // The host is not waiting on anybody, so it has no round trip worth printing.
            ImGui::TextDisabled("host");
        }
        else
        {
            const ImVec4 colour = pingMs < 80    ? ImVec4(0.55f, 0.75f, 0.58f, 1.0f)
                                  : pingMs < 180 ? ImVec4(0.82f, 0.76f, 0.50f, 1.0f)
                                                 : ImVec4(0.85f, 0.50f, 0.45f, 1.0f);
            ImGui::TextColored(colour, "%d ms", pingMs);
        }
    };

    const bool hosting = m_sessionMode == SessionMode::Host;
    row(PlayerName().c_str(), m_player.State().health, m_player.State().alive,
        hosting ? -1 : static_cast<int>(m_client.PingMs()), true, false);
    for (const RemotePlayerView& other : others)
    {
        const bool watched = m_spectating >= 0 && static_cast<int>(other.id) == m_spectating;
        row(other.name.c_str(), other.health, other.alive, static_cast<int>(other.pingMs), false,
            watched);
    }

    ImGui::End();
}

void PredationGame::DrawItemIcon(ItemId item, float boxSize) const
{
    const ItemIcons::Icon* icon = m_itemIcons.Find(item);
    if (icon == nullptr || !m_itemIcons.IsReady())
    {
        return;
    }

    // Inset a little, so the render's own framing does not touch the slot border.
    const float inset = boxSize * 0.06f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    // A pale backing, lighter at the top like a lit shelf, so a dark object has something to stand
    // against. The weapons are near-black metal and polymer, which is right for them and made them
    // black shapes on a black slot: the icon was there, in its real colours, and could not be seen.
    ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
        {origin.x + 2.0f, origin.y + 2.0f}, {origin.x + boxSize - 2.0f, origin.y + boxSize - 2.0f},
        IM_COL32(128, 134, 144, 150), IM_COL32(128, 134, 144, 150), IM_COL32(74, 78, 88, 150),
        IM_COL32(74, 78, 88, 150));
    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(ImGuiLayer::TextureId(m_itemIcons.Texture())),
                                        {origin.x + inset, origin.y + inset},
                                        {origin.x + boxSize - inset, origin.y + boxSize - inset},
                                        {icon->uv0.x, icon->uv0.y}, {icon->uv1.x, icon->uv1.y});
}

void PredationGame::DrawInventoryPanel()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->Pos.x + viewport->Size.x * 0.5f,
                             viewport->Pos.y + viewport->Size.y * 0.5f},
                            ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (ImGui::Begin("Inventory", nullptr,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        constexpr float kSlotSize = 76.0f;
        constexpr int kColumns = 3;

        for (int i = 0; i < m_inventory.SlotCount(); ++i)
        {
            if (i % kColumns != 0)
            {
                ImGui::SameLine(0.0f, 10.0f);
            }
            ImGui::BeginGroup();

            const Inventory::Slot& slot = m_inventory.At(i);
            const bool selected = i == m_inventory.SelectedSlot();
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImDrawList* list = ImGui::GetWindowDrawList();

            list->AddRectFilled(origin, {origin.x + kSlotSize, origin.y + kSlotSize},
                                IM_COL32(26, 28, 36, 220), 4.0f);
            list->AddRect(origin, {origin.x + kSlotSize, origin.y + kSlotSize},
                          selected ? IM_COL32(235, 205, 130, 235) : IM_COL32(110, 114, 124, 160), 4.0f,
                          0, selected ? 2.0f : 1.0f);

            const ItemDefinition* definition = m_items.Get(slot.item);
            if (definition != nullptr)
            {
                DrawItemIcon(slot.item, kSlotSize);
                // The count rides on the icon, as it does on the hotbar, so the caption below only
                // ever has to hold a name.
                if (slot.count > 1)
                {
                    const std::string count = std::to_string(slot.count);
                    list->AddText({origin.x + kSlotSize - 6.0f - ImGui::CalcTextSize(count.c_str()).x,
                                   origin.y + kSlotSize - 17.0f},
                                  IM_COL32(240, 240, 245, 235), count.c_str());
                }
            }

            // An invisible button over the slot makes it clickable for selecting.
            ImGui::InvisibleButton(("##slot" + std::to_string(i)).c_str(), {kSlotSize, kSlotSize});
            if (ImGui::IsItemClicked())
            {
                // Clicking what is already out puts it away, the same as pressing its number, and
                // like the number it does nothing while a climb has the hands.
                if (HandsAreFree())
                {
                    m_inventory.SelectSlot(m_inventory.SelectedSlot() == i ? Inventory::kNoSlot : i);
                }
            }
            if (definition != nullptr && ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", definition->name.c_str());
            }

            // Up to two lines, broken between words, with anything that still will not fit cut short
            // and marked as cut. Clipping one line at the slot edge, which is what this did, reads
            // as a rendering fault rather than as an abbreviation: "Access Keycard" came out as
            // "Access Keyc" with the c half drawn.
            const std::string label = definition != nullptr ? definition->name : std::string("Empty");
            const float lineHeight = ImGui::GetTextLineHeight();
            const ImU32 colour =
                definition != nullptr ? IM_COL32(220, 222, 230, 255) : IM_COL32(115, 118, 128, 255);
            const ImVec2 caption = ImGui::GetCursorScreenPos();

            std::vector<std::string> lines;
            {
                std::string current;
                size_t at = 0;
                while (at <= label.size() && lines.size() < 2)
                {
                    const size_t space = label.find(' ', at);
                    const std::string word = label.substr(at, space - at);
                    const std::string candidate = current.empty() ? word : current + " " + word;
                    if (!current.empty() && ImGui::CalcTextSize(candidate.c_str()).x > kSlotSize)
                    {
                        lines.push_back(current);
                        current = word;
                    }
                    else
                    {
                        current = candidate;
                    }
                    if (space == std::string::npos)
                    {
                        break;
                    }
                    at = space + 1;
                }
                if (lines.size() < 2 && !current.empty())
                {
                    lines.push_back(current);
                }
            }
            for (std::string& line : lines)
            {
                // A single word longer than the slot has nowhere to break, so it is shortened.
                while (line.size() > 1 && ImGui::CalcTextSize((line + ".").c_str()).x > kSlotSize)
                {
                    line.pop_back();
                }
                if (ImGui::CalcTextSize(line.c_str()).x > kSlotSize)
                {
                    line += ".";
                }
            }
            for (size_t line = 0; line < lines.size(); ++line)
            {
                list->AddText({caption.x, caption.y + lineHeight * static_cast<float>(line)}, colour,
                              lines[line].c_str());
            }
            // Always two lines' worth, so a one-word name and a two-word one leave the grid level.
            ImGui::Dummy({kSlotSize, lineHeight * 2.0f});

            ImGui::EndGroup();
        }

        ImGui::Separator();
        ImGui::TextDisabled("Tab closes.  G drops the selected item.");
    }
    ImGui::End();
}

void PredationGame::OnImGui()
{
    // The editor is a tool, not part of the game's own interface, so it is drawn whether or not the
    // debug overlay is showing and it takes the screen to itself.
    if (m_editor.IsOpen())
    {
        m_editor.DrawUi(m_editorScene, m_app->GetMeshes());
        // Beside it, because it answers the one question the editor cannot: where does the hand end
        // up.
        DrawWeaponBench();
        DrawEditorFirstPerson();
        return;
    }

    if (m_screen == Screen::Title)
    {
        DrawTitleScreen();
        return;
    }

    // The HUD is part of the game, not the debug overlay, so it is always drawn.
    if (m_cameraMode != CameraMode::Fly)
    {
        DrawHud();
    }

    if (m_paused)
    {
        DrawPauseMenu();
    }

    if (!m_app->IsOverlayVisible())
    {
        return;
    }

#if !PRED_DEV_TOOLS
    // A player's F3: the frame rate the engine already shows, and the connection under it. The
    // tuning panels below are for building the game, and in a player's hands a slider that moves
    // the sun or the sprint speed is a way to break it rather than a setting.
    DrawConnectionReadout();
#else

    DrawNetworkPanel();
    DrawSoundPanel();

    ImGui::SetNextWindowPos(ImVec2(8.0f, 470.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Player"))
    {
        if (m_cameraMode == CameraMode::Fly)
        {
            ImGui::TextUnformatted("Free camera. P cycles first person, third person and this.");
            ImGui::Text("Camera %.2f %.2f %.2f", m_camera.position.x, m_camera.position.y,
                        m_camera.position.z);
            ImGui::Separator();
        }
        DrawPlayerPanel();

        // What the mixer is doing. Sound is the one subsystem with nothing to look at, so the
        // numbers are the only way to tell a silent bug from a quiet moment.
        ImGui::Separator();
        AudioEngine& audio = m_app->GetAudio();
        const AudioEngine::Stats sound = audio.GetStats();
        ImGui::Text("Audio %s  %d voices, %u played", audio.HasDevice() ? "on" : "OFF (no device)",
                    sound.voices, sound.started);
        if (sound.stolen > 0 || sound.clipped > 0)
        {
            ImGui::TextDisabled("%u cut short, %u samples limited", sound.stolen, sound.clipped);
        }
        float volume = audio.MasterGain();
        if (ImGui::SliderFloat("Volume", &volume, 0.0f, 1.5f, "%.2f"))
        {
            audio.SetMasterGain(volume);
        }
    }
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(420.0f, 470.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("World", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        const SceneRenderer::Stats& stats = m_app->GetSceneRenderer().LastStats();
        ImGui::Text("Entities %zu   submitted %zu   triangles %zu", m_scene.EntityCount(),
                    stats.meshesSubmitted, stats.trianglesSubmitted);
        ImGui::Text("Props %zu", m_props.size());

        float fov = cv_fov.Get();
        if (ImGui::SliderFloat("FOV (horizontal)", &fov, 60.0f, 120.0f, "%.0f"))
        {
            cv_fov.Set(fov);
        }
        float sensitivity = cv_mouseSensitivity.Get();
        if (ImGui::SliderFloat("Sensitivity", &sensitivity, 0.02f, 0.5f, "%.3f"))
        {
            cv_mouseSensitivity.Set(sensitivity);
        }
        bool invert = cv_invertY.Get();
        if (ImGui::Checkbox("Invert Y", &invert))
        {
            cv_invertY.Set(invert);
        }
        bool crouchToggle = cv_crouchToggle.Get();
        if (ImGui::Checkbox("Crouch toggles", &crouchToggle))
        {
            cv_crouchToggle.Set(crouchToggle);
        }
        bool sprintToggle = cv_sprintToggle.Get();
        if (ImGui::Checkbox("Sprint toggles", &sprintToggle))
        {
            cv_sprintToggle.Set(sprintToggle);
        }

        ImGui::Separator();
        Environment& environment = m_scene.GetEnvironment();
        float sunIntensity = cv_sunIntensity.Get();
        if (ImGui::SliderFloat("Sun intensity", &sunIntensity, 0.0f, 8.0f, "%.2f"))
        {
            cv_sunIntensity.Set(sunIntensity);
        }
        ImGui::ColorEdit3("Fog colour", &environment.fogColor.r);
        float fogStart = cv_fogStart.Get();
        if (ImGui::SliderFloat("Fog start", &fogStart, 0.0f, 100.0f, "%.1f m"))
        {
            cv_fogStart.Set(fogStart);
        }
        float fogEnd = cv_fogEnd.Get();
        if (ImGui::SliderFloat("Fog end", &fogEnd, 1.0f, 400.0f, "%.1f m"))
        {
            cv_fogEnd.Set(fogEnd);
        }

        bool grid = cv_showGrid.Get();
        if (ImGui::Checkbox("Reference grid", &grid))
        {
            cv_showGrid.Set(grid);
        }
        ImGui::SameLine();
        bool wireframe = cv_wireframe.Get();
        if (ImGui::Checkbox("Wireframe", &wireframe))
        {
            cv_wireframe.Set(wireframe);
        }
    }
    ImGui::End();
#endif
}

} // namespace pred
