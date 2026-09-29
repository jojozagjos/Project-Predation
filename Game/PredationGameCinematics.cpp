// Cinematics as the game plays them: which there are (Assets/Cinematics), what they are measured from here (the site's
// pad, the mission's building, the ship), the picture while one plays and while it is handed back to the player's own
// eyes, what is drawn over it -- the bars, the black, a title card typed out -- and the game's side of what happens in
// one: its sounds, its markers, its puffs of exhaust. The cinematic itself is Game/Cinematic.

#include "Game/PredationGame.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Engine/Render/DebugDraw.h"
#include "Engine/Render/Primitives.h"
#include "Game/World/TestMap.h"
#include "Game/World/Vehicles.h"

#include <imgui.h>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>

namespace pred
{

namespace
{

// Far enough to see a planet from orbit, near enough that nothing close fights over its depth.
constexpr float kCinematicFar = 20000.0f;
constexpr float kCinematicNear = 0.25f;

glm::mat4 ViewOf(const CameraState& camera)
{
    return glm::inverse(glm::translate(glm::mat4(1.0f), camera.position) * glm::mat4_cast(camera.rotation));
}

CameraState CameraOf(const glm::mat4& view, float fov)
{
    const glm::mat4 world = glm::inverse(view);
    CameraState camera;
    camera.position = glm::vec3(world[3]);
    camera.rotation = glm::normalize(glm::quat_cast(glm::mat3(world)));
    camera.fov = fov;
    return camera;
}

// A shake that never repeats in any way the eye can find: a few sines of unrelated speeds on each axis.
glm::vec3 Shaking(float time, float speed)
{
    const float t = time * speed;
    return {std::sin(t * 1.00f + 0.3f) * 0.55f + std::sin(t * 2.31f + 1.7f) * 0.30f + std::sin(t * 5.13f) * 0.15f,
            std::sin(t * 0.87f + 2.1f) * 0.55f + std::sin(t * 2.07f + 0.4f) * 0.30f + std::sin(t * 4.71f + 2.9f) * 0.15f,
            std::sin(t * 0.63f + 4.2f) * 0.60f + std::sin(t * 1.91f + 3.3f) * 0.40f};
}

std::string TwoFigures(int value)
{
    char text[8];
    std::snprintf(text, sizeof(text), "%02d", value);
    return text;
}

} // namespace

void PredationGame::LoadCinematics()
{
    m_cinematics.clear();
    const std::filesystem::path folder = Paths::AssetsRoot() / "Cinematics";
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".json")
        {
            continue;
        }
        Cinematic cinematic;
        std::string error;
        if (!cinematic.LoadFromFile(entry.path(), &error))
        {
            PRED_LOG_WARN(Gameplay, "Cinematic not read: {}", error);
            continue;
        }
        m_cinematics[cinematic.name] = std::move(cinematic);
    }
    PRED_LOG_INFO(Gameplay, "{} cinematic(s) in {}", m_cinematics.size(), folder.string());
}

CinematicBindings PredationGame::CinematicBindingsNow() const
{
    CinematicBindings bindings;
    // The ship: itself, its hangar, its cockpit, its briefing room, its engines.
    m_ship.Anchors(bindings.anchors);
    // This player's own eyes, for a shot that begins or ends in them -- theirs even while the editor's camera has the
    // picture.
    const glm::vec3 eye = m_cineEditor.IsOpen() ? m_player.View().eyePosition : m_renderEye;
    bindings.anchors["player"] = {eye, TurnFromDegrees({glm::degrees(m_lookPitch), glm::degrees(m_lookYaw), 0.0f})};

    if (!m_facility.Built())
    {
        return bindings;
    }
    const SitePlan& site = m_facility.Plan();
    // The pad, facing the way the shuttle's ramp does: into the site.
    bindings.anchors["pad"] = {site.ShuttleBase(), TurnFromDegrees({0.0f, glm::degrees(site.landingYaw), 0.0f})};
    bindings.anchors["site"] = {site.origin + glm::vec3(site.size * 0.5f, 0.0f, site.size * 0.5f), {}};
    if (m_missionPlan.Valid())
    {
        const FacilityLayout& building = site.buildings[static_cast<size_t>(m_missionPlan.building)];
        glm::vec2 lo;
        glm::vec2 hi;
        SitePlan::Footprint(building, 0.0f, lo, hi);
        const glm::vec2 middle = (lo + hi) * 0.5f;
        bindings.anchors["facility"] = {{middle.x, building.origin.y, middle.y}, {}};
        bindings.anchors["terminal"] = {m_missionPlan.terminal, glm::angleAxis(m_missionPlan.terminalYaw, glm::vec3(0.0f, 1.0f, 0.0f))};
        if (!building.exits.empty())
        {
            // A few metres out from its first way in, facing it.
            const FacilityLayout::Exit& exit = building.exits.front();
            const glm::vec3 outside = FacilityMap::ExitOutside(building, exit, 6.0f);
            const glm::vec3 door = FacilityMap::ExitOutside(building, exit, 0.0f);
            const glm::vec3 in = door - outside;
            bindings.anchors["facility_entrance"] = {outside, TurnFromDegrees({0.0f, glm::degrees(std::atan2(in.x, -in.z)), 0.0f})};
            // From just off the foot of the shuttle's ramp to the way in: the route a vehicle would take.
            const glm::vec3 ramp = site.ShuttleBase() + glm::vec3(std::sin(site.landingYaw), 0.0f, -std::cos(site.landingYaw)) * 8.0f;
            bindings.paths["pad_to_facility"] = {ramp, outside + (ramp - outside) * 0.5f, outside};

            // Somewhere to see the building from, arriving: out from its way in, up high, with nothing between there and the
            // door -- tried at a few angles, distances and heights, and the first clear one taken. A shot measured from here
            // shows the place whatever else the site has put about it.
            const glm::vec3 out = glm::normalize(glm::vec3(outside.x - door.x, 0.0f, outside.z - door.z));
            const glm::vec3 target = door + glm::vec3(0.0f, 2.0f, 0.0f);
            const PhysicsWorld& physics = m_app->GetPhysics();
            glm::vec3 chosen = outside + out * 50.0f + glm::vec3(0.0f, 16.0f, 0.0f);
            bool found = false;
            for (const float distance : {55.0f, 45.0f, 65.0f, 38.0f})
            {
                for (const float degrees : {25.0f, -25.0f, 40.0f, -40.0f, 10.0f, -10.0f, 55.0f, -55.0f})
                {
                    for (const float height : {16.0f, 12.0f, 22.0f})
                    {
                        if (found)
                        {
                            break;
                        }
                        const glm::quat turn = glm::angleAxis(glm::radians(degrees), glm::vec3(0.0f, 1.0f, 0.0f));
                        const glm::vec3 p = door + (turn * out) * distance + glm::vec3(0.0f, height, 0.0f);
                        const glm::vec3 to = target - p;
                        const float length = glm::length(to);
                        // On the site, over open ground, and a clear line to the door.
                        const RayHit ground = physics.RayCastStatic(p, {0.0f, -1.0f, 0.0f}, height + 2.0f);
                        if (!m_facility.Contains(p) || !ground || ground.distance < height - 1.5f ||
                            physics.RayCastStatic(p, to / length, length - 2.0f).hit)
                        {
                            continue;
                        }
                        chosen = p;
                        found = true;
                    }
                }
            }
            const glm::vec3 look = target - chosen;
            bindings.anchors["reveal_point"] = {chosen, TurnFromDegrees({glm::degrees(std::asin(std::clamp(look.y / glm::length(look), -1.0f, 1.0f))),
                                                                         glm::degrees(std::atan2(look.x, -look.z)), 0.0f})};
        }
    }
    // Where the shuttle rests.
    bindings.anchors["shuttle_home"] = m_facility.ShuttleHome();
    // What a title card fills in.
    const SiteTitle title = m_siteNames.For(m_facility.Seed());
    bindings.words["site"] = title.site;
    bindings.words["planet"] = title.planet;
    const size_t comma = title.site.find(", ");
    bindings.words["region"] = comma == std::string::npos ? std::string() : title.site.substr(comma + 2);
    const uint32_t seed = m_facility.Seed();
    bindings.words["local_time"] = "LOCAL " + TwoFigures(static_cast<int>((seed * 7u) % 24u)) + ":" + TwoFigures(static_cast<int>((seed * 13u) % 60u));
    const SiteConditions weather = ConditionsFor(seed, site.sky.fogEnd);
    bindings.words["conditions"] = std::to_string(weather.temperature) + " C  WIND " + std::to_string(weather.wind) + " M/S  VISIBILITY " + weather.visibility;
    return bindings;
}

bool PredationGame::PlayCinematic(const std::string& name, float from)
{
    const auto found = m_cinematics.find(name);
    if (found == m_cinematics.end())
    {
        m_app->GetConsole().PrintError("No cinematic called '" + name + "' (cine_list says which there are).");
        return false;
    }
    // One taking over from another finds everything where it rests, not where the last left it.
    ParkVehicles();
    // And nothing is left open over it: the pause menu, the map, the briefing, the inventory.
    CloseOverlays();
    m_cine.Play(found->second, CinematicBindingsNow(), m_scene, m_app->GetMeshes(), *this, from);
    m_cineHandBack = 0.0f;
    if (m_sessionMode == SessionMode::Host)
    {
        WorldEventMessage event;
        event.kind = WorldEventKind::Cinematic;
        event.item = SoundKey(name);
        event.flag = true;
        event.amount = from;
        m_host.Broadcast(event);
    }
    return true;
}

void PredationGame::StopCinematic(bool handBack)
{
    if (!m_cine.Active())
    {
        return;
    }
    if (handBack)
    {
        m_cineHandBackFrom = m_cine.Picture();
        m_cineHandBackTotal = std::max(m_cine.Playing().blendOut, 0.0f);
        m_cineHandBack = m_cineHandBackTotal;
    }
    // Ending in black, the picture comes back up out of it rather than appearing all at once.
    if (m_cine.Sampler().Fade(m_cine.Time()) > 0.5f)
    {
        m_cineFadeIn = 1.0f;
    }
    m_cine.Stop(m_scene);
    m_cineHolds = false;
    ParkVehicles();
    // Stopped here, stopped everywhere: one skipped on the host otherwise went on playing for everybody else.
    if (m_sessionMode == SessionMode::Host)
    {
        WorldEventMessage event;
        event.kind = WorldEventKind::Cinematic;
        event.flag = false;
        m_host.Broadcast(event);
    }
}

void PredationGame::CloseOverlays()
{
    m_paused = false;
    m_settingsOpen = false;
    m_inventoryOpen = false;
    m_wantMouseCaptured = !m_cineEditor.IsOpen();
}

void PredationGame::ParkVehicles()
{
    if (m_facility.Shuttle().Built())
    {
        m_facility.Shuttle().GoHome(m_scene);
    }
    if (m_ship.Built())
    {
        m_ship.Shuttle().GoHome(m_scene);
        m_ship.BayDoors().GoHome(m_scene);
        m_ship.Hull().GoHome(m_scene);
        m_ship.StageHull().GoHome(m_scene);
        m_ship.SetEngines(m_scene, 0.0f);
    }
    UpdateVehicleLamps();
}

bool PredationGame::CinematicHoldsPlayers() const
{
    // Editing one, nobody moves: the keys are the editor's.
    return m_cineHolds || m_cineEditor.IsOpen();
}

bool PredationGame::CinematicHoldsWorld() const
{
    return m_cineHolds || m_cineEditor.IsOpen();
}

void PredationGame::AttachVehicleLamps()
{
    // Put up again only when the site's lights were: the same lamps are moved to where their vehicles now rest.
    if (m_vehicleLampsOf != m_levelLights.Generation() || m_vehicleLamps.empty())
    {
        m_vehicleLamps.clear();
        const auto add = [&](const std::string& vehicle, const VehicleProp& prop, LightKind kind, float range)
        {
            if (prop.Model() == nullptr)
            {
                return;
            }
            for (const ModelSocket& socket : prop.Model()->sockets)
            {
                const bool cabin = socket.name == "lamp";
                if (!cabin && socket.name.rfind("light_", 0) != 0)
                {
                    continue;
                }
                CinePose at;
                prop.Socket(socket.name, at);
                const glm::vec3 direction = cabin ? glm::vec3(0.0f, -1.0f, 0.0f) : at.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
                const int light = m_levelLights.Add(m_scene, m_app->GetMeshes(), cabin ? LightKind::Ceiling : kind, LightMood::Steady, at.position,
                                                    direction, 0, 0, cabin ? 5.0f : range);
                m_vehicleLamps.push_back({vehicle, socket.name, light});
            }
        };
        // The shuttle's cabin lamp is the site's own; its landing light is one of these.
        if (m_facility.Shuttle().Built())
        {
            for (const ModelSocket& socket : m_facility.Shuttle().Model()->sockets)
            {
                if (socket.name.rfind("light_", 0) == 0)
                {
                    CinePose at;
                    m_facility.Shuttle().Socket(socket.name, at);
                    const int light = m_levelLights.Add(m_scene, m_app->GetMeshes(), LightKind::Flood, LightMood::Steady, at.position,
                                                        at.rotation * glm::vec3(0.0f, 0.0f, -1.0f), 0, 0, 24.0f);
                    m_vehicleLamps.push_back({"site_shuttle", socket.name, light});
                }
            }
        }
        m_vehicleLampsOf = m_levelLights.Generation();
    }
    UpdateVehicleLamps();
}

void PredationGame::UpdateVehicleLamps()
{
    for (const VehicleLamp& lamp : m_vehicleLamps)
    {
        const VehicleProp* vehicle = &m_facility.Shuttle();
        CinePose at;
        if (vehicle->Built() && vehicle->SocketShown(lamp.socket, at))
        {
            const glm::vec3 direction = lamp.socket == "lamp" ? glm::vec3(0.0f, -1.0f, 0.0f) : at.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
            m_levelLights.Place(m_scene, lamp.light, at.position, direction);
        }
    }
}

void PredationGame::UpdateCinematic(float dt)
{
    UpdateCinematicParticles(dt);
    m_cineFadeIn = std::max(m_cineFadeIn - dt / 1.2f, 0.0f);
    // Nobody's body is out there while a cinematic has them: they are aboard.
    if (m_cineHolds != m_cineHidBodies)
    {
        m_cineHidBodies = m_cineHolds;
        m_body.SetVisible(m_scene, !m_cineHidBodies);
        for (const std::unique_ptr<RemoteAvatar>& avatar : m_avatars)
        {
            if (avatar != nullptr && avatar->built)
            {
                avatar->body.SetVisible(m_scene, !m_cineHidBodies);
            }
        }
    }
    if (!m_cine.Active())
    {
        m_cineHolds = false;
        return;
    }
    m_cine.Update(dt, m_scene, *this);
    if (m_cineGoTo.has_value())
    {
        const MapChoice map = *m_cineGoTo;
        m_cineGoTo.reset();
        GoToMap(map);
        m_dockingReturn = false;
        // And what is seen of getting there.
        if (!m_cineThen.empty())
        {
            const std::string next = m_cineThen;
            m_cineThen.clear();
            if (HasCinematic(next))
            {
                PlayCinematic(next);
            }
        }
        if (!m_cine.Active())
        {
            return;
        }
    }
    UpdateVehicleLamps();
    m_cineHolds = m_cine.Playing().pausesGameplay;
    if (m_cine.GetState() == CinematicPlayer::State::Finished)
    {
        if (m_cineEditor.IsOpen())
        {
            CinematicEditor::Context context = CinematicEditorContext();
            m_cineEditor.AtEnd(context);
        }
        else
        {
            StopCinematic(true);
        }
    }
}

bool PredationGame::CinematicCamera(glm::mat4& view, glm::vec3& eye, float dt, float gameplayFov)
{
    m_cineFov = 0.0f;
    m_cineFar = 0.0f;
    // Editing, looking round freely.
    if (m_cineEditor.IsOpen() && (!m_cineEditor.ThroughCinematic() || !m_cine.Active()))
    {
        const FlyCamera& free = m_cineEditor.View();
        view = free.View();
        eye = free.position;
        m_cineFov = free.fovDegrees;
        m_cineFar = kCinematicFar;
        return true;
    }
    if (m_cine.Active())
    {
        CameraState picture = m_cine.Picture();
        const CinematicSampler sampler = m_cine.Sampler();
        const float shake = sampler.Shake(m_cine.Time());
        if (shake > 0.0f)
        {
            const glm::vec3 jolt = Shaking(m_cine.Time(), sampler.ShakeSpeed(m_cine.Time())) * shake;
            picture.rotation = picture.rotation * TurnFromDegrees(jolt);
            picture.position += picture.rotation * (jolt * 0.01f);
        }
        view = ViewOf(picture);
        eye = picture.position;
        m_cineFov = picture.fov;
        m_cineFar = kCinematicFar;
        return true;
    }
    // Eyes somewhere else altogether -- taken aboard the ship as it ended -- are cut to, not swept across the world to.
    if (m_cineHandBack > 0.0f && glm::distance(m_cineHandBackFrom.position, CameraOf(view, gameplayFov).position) > 40.0f)
    {
        m_cineHandBack = 0.0f;
    }
    if (m_cineHandBack > 0.0f)
    {
        // From the last picture back to the player's eyes, which have been where they are all along.
        m_cineHandBack = std::max(m_cineHandBack - dt, 0.0f);
        Ease ease;
        const float t = ease.Apply(1.0f - m_cineHandBack / std::max(m_cineHandBackTotal, 1.0e-3f));
        const CameraState blended = LerpCamera(m_cineHandBackFrom, CameraOf(view, gameplayFov), t);
        view = ViewOf(blended);
        eye = blended.position;
        m_cineFov = blended.fov;
        m_cineFar = m_cineHandBack > 0.0f ? kCinematicFar : 0.0f;
        return true;
    }
    return false;
}

// --- What happens in one -------------------------------------------------------------------------------

namespace
{

// The vehicles a cinematic can bind to by name.
VehicleProp* Bound(const std::string& bind, SiteMap& site, MissionProps& props, ShipMap& ship)
{
    if (bind == "ship_shuttle")
    {
        return ship.Shuttle().Built() ? &ship.Shuttle() : nullptr;
    }
    if (bind == "ship_bay_doors")
    {
        return ship.BayDoors().Built() ? &ship.BayDoors() : nullptr;
    }
    if (bind == "ship_hull")
    {
        return ship.Hull().Built() ? &ship.Hull() : nullptr;
    }
    if (bind == "ship_stage")
    {
        return ship.StageHull().Built() ? &ship.StageHull() : nullptr;
    }
    if (bind == "site_shuttle")
    {
        return site.Shuttle().Built() ? &site.Shuttle() : nullptr;
    }
    return nullptr;
}

} // namespace

bool PredationGame::CineFindBound(const std::string& bind, CinePose& where)
{
    const VehicleProp* vehicle = Bound(bind, m_facility, m_missionProps, m_ship);
    if (vehicle == nullptr)
    {
        return false;
    }
    where = vehicle->Home();
    return true;
}

void PredationGame::CineMoveBound(const std::string& bind, const CinePose& pose)
{
    if (VehicleProp* vehicle = Bound(bind, m_facility, m_missionProps, m_ship))
    {
        vehicle->Show(m_scene, pose);
    }
}

void PredationGame::CinePoseBound(const std::string& bind, const std::string& clip, float clipTime)
{
    if (VehicleProp* vehicle = Bound(bind, m_facility, m_missionProps, m_ship))
    {
        vehicle->ShowClip(m_scene, clip, clipTime);
    }
}

void PredationGame::CineSound(const SoundEvent& sound, const glm::vec3* at)
{
    if (at != nullptr && !sound.music)
    {
        PlayNamed(sound.sound, *at, sound.volume, 1.0f, true);
    }
    else
    {
        PlayNamed(sound.sound, m_renderEye, sound.volume, 1.0f, false);
    }
}

void PredationGame::CineMarker(const pred::Marker& marker)
{
    PRED_LOG_INFO(Gameplay, "Cinematic marker '{}'{}", marker.name, marker.value.empty() ? "" : " " + marker.value);
    if (m_cineEditor.IsOpen() && marker.name != "say")
    {
        return;
    }
    if (marker.name == "hold")
    {
        m_cineHolds = true;
    }
    else if (marker.name == "release")
    {
        m_cineHolds = false;
    }
    else if (marker.name == "say")
    {
        // "arrival" is whichever arrival there is: with a map or without one.
        Say(marker.value == "arrival" && !m_missionPlan.mapGiven ? "arrival_no_map" : marker.value);
    }
    else if (marker.name == "go_to_ship")
    {
        // Everybody back aboard, for the debrief: the host takes them, and the result is up from now.
        m_missionOverFor = 0.0f;
        if (IsAuthority() && m_screen == Screen::Playing && m_map == MapChoice::Facility)
        {
            // The ones who made it aboard come back in the shuttle; the rest are waiting in the briefing room.
            m_cineGoTo = MapChoice::Ship;
            m_cineThen = "ship_docking";
            m_dockingReturn = true;
        }
    }
    else if (marker.name == "docked")
    {
        // Back aboard: the result is up from now, when everybody can see it.
        m_missionOverFor = 0.0f;
    }
    else if (marker.name == "arrive")
    {
        // The burn is over: the ship is over the site's planet, and the shuttle is waiting.
        ArriveOverSite();
    }
    else if (marker.name == "go_to_site")
    {
        // The shuttle has dropped out of the hangar: everybody is on their way down.
        m_shipReady = false;
        if (IsAuthority() && m_screen == Screen::Playing && m_map == MapChoice::Ship)
        {
            m_cineGoTo = MapChoice::Facility;
        }
    }
}

void PredationGame::CineParticle(const ParticleEvent& particle, const glm::vec3& at)
{
    // A handful of puffs, by the kind of effect: exhaust glows and rises fast, snow and dust are pale and hang.
    const bool exhaust = particle.effect.find("exhaust") != std::string::npos || particle.effect.find("thrust") != std::string::npos;
    Material material = Material::Diffuse(exhaust ? glm::vec3(0.9f, 0.55f, 0.25f) : glm::vec3(0.85f, 0.88f, 0.92f), 0.9f);
    // Snow catches whatever light there is: a little of its own, so it shows against the dark.
    material.emissive = exhaust ? glm::vec3(2.4f, 1.2f, 0.45f) : glm::vec3(0.12f, 0.13f, 0.14f);
    const MeshHandle mesh = m_app->GetMeshes().Upload(Primitives::Sphere(0.5f, 10, 8), "cine_puff");
    // Many small bits rather than a few big ones: flakes thrown up, sparks blown down.
    const int count = exhaust ? 36 : 60;
    for (int i = 0; i < count; ++i)
    {
        const float a = static_cast<float>(i) * 2.399963f; // golden angle: spread without clumping
        const float r = 0.25f + 0.75f * static_cast<float>((i * 7) % 11) / 10.0f;
        CinePuff puff;
        Transform transform;
        transform.position = at + glm::vec3(std::cos(a) * r * 0.6f, 0.05f * static_cast<float>(i % 4), std::sin(a) * r * 0.6f);
        transform.scale = glm::vec3(0.02f);
        puff.entity = m_scene.CreateMeshEntity("cine_puff", transform, mesh, material);
        if (MeshRenderer* renderer = m_scene.GetMeshRenderer(puff.entity))
        {
            renderer->castsShadow = false;
        }
        puff.velocity = exhaust ? glm::vec3(std::cos(a) * r * 1.5f, -4.0f - r * 3.0f, std::sin(a) * r * 1.5f)
                                : glm::vec3(std::cos(a) * r * 4.0f, 1.0f + r * 2.2f, std::sin(a) * r * 4.0f);
        puff.life = std::max(particle.duration, 0.2f) * (0.6f + 0.4f * r);
        puff.from = exhaust ? 0.06f : 0.05f;
        puff.to = exhaust ? 0.18f : 0.14f;
        m_cinePuffs.push_back(puff);
    }
}

void PredationGame::UpdateCinematicParticles(float dt)
{
    for (CinePuff& puff : m_cinePuffs)
    {
        puff.age += dt;
        if (Transform* transform = m_scene.GetTransform(puff.entity))
        {
            puff.velocity *= std::exp(-1.2f * dt);
            puff.velocity.y -= 2.5f * dt; // and settles
            transform->position += puff.velocity * dt;
            const float t = std::clamp(puff.age / puff.life, 0.0f, 1.0f);
            // Grows, and shrinks away at the end rather than blinking out.
            const float size = (puff.from + (puff.to - puff.from) * t) * (t > 0.7f ? 1.0f - (t - 0.7f) / 0.3f : 1.0f);
            transform->scale = glm::vec3(std::max(size, 0.001f));
        }
    }
    std::erase_if(m_cinePuffs, [&](const CinePuff& puff)
    {
        if (puff.age < puff.life)
        {
            return false;
        }
        m_scene.Destroy(puff.entity);
        return true;
    });
}

void PredationGame::CineLight(const std::string& light, float intensity)
{
    // The ship's engines, burning; the cabin lamps of what a cinematic brings with it are its models' own glow for now.
    if (light == "ship_engines")
    {
        m_ship.SetEngines(m_scene, intensity);
    }
}

// --- Over the picture ---------------------------------------------------------------------------------

void PredationGame::DrawCinematicOverlay()
{
    if (m_cineFadeIn > 0.0f && !m_cine.Active())
    {
        const ImGuiViewport* view = ImGui::GetMainViewport();
        ImGui::GetForegroundDrawList()->AddRectFilled(view->Pos, {view->Pos.x + view->Size.x, view->Pos.y + view->Size.y},
                                                       IM_COL32(0, 0, 0, static_cast<int>(255 * m_cineFadeIn)));
    }
    if (!m_cine.Active() || m_screen != Screen::Playing)
    {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // Where the picture is: the screen, or the editor's preview of it, and how much smaller that is.
    ImVec2 min = viewport->Pos;
    ImVec2 max{viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y};
    glm::vec2 previewMin;
    glm::vec2 previewMax;
    if (m_cineEditor.Preview(previewMin, previewMax))
    {
        min = {previewMin.x, previewMin.y};
        max = {previewMax.x, previewMax.y};
    }
    const float scale = (max.x - min.x) / std::max(viewport->Size.x, 1.0f);
    if (m_cineEditor.IsOpen() && !m_cineEditor.ThroughCinematic())
    {
        ImGui::GetForegroundDrawList()->AddText({min.x + 10.0f, min.y + 8.0f}, IM_COL32(255, 210, 120, 255), "FREE CAMERA  (C: through the cinematic's)");
        return;
    }
    ImDrawList* draw = m_cineEditor.IsOpen() ? ImGui::GetBackgroundDrawList() : ImGui::GetForegroundDrawList();
    const CinematicSampler sampler = m_cine.Sampler();
    const float time = m_cine.Time();

    // The bars, top and bottom.
    const float bars = sampler.Letterbox(time) * (max.y - min.y) * 0.11f;
    if (bars > 0.5f)
    {
        draw->AddRectFilled(min, {max.x, min.y + bars}, IM_COL32(0, 0, 0, 255));
        draw->AddRectFilled({min.x, max.y - bars}, max, IM_COL32(0, 0, 0, 255));
    }

    // Words: a title card typed out low on the left like a terminal's, a caption across the bottom.
    for (const TextItem& item : m_cine.Playing().texts)
    {
        if (time < item.time || time > item.time + item.duration)
        {
            continue;
        }
        const float left = item.time + item.duration - time;
        const float alpha = std::clamp(left / 0.8f, 0.0f, 1.0f);
        if (item.style == "caption")
        {
            std::string text;
            for (size_t i = 0; i < item.lines.size(); ++i)
            {
                text += (i > 0 ? "\n" : "") + sampler.Text(item, i, time);
            }
            const float fontSize = ImGui::GetFontSize() * scale;
            const ImVec2 size = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str());
            const ImVec2 at{(min.x + max.x - size.x) * 0.5f, max.y - bars - size.y - 36.0f * scale};
            draw->AddText(ImGui::GetFont(), fontSize, at, IM_COL32(230, 232, 236, static_cast<int>(255 * alpha)), text.c_str());
            continue;
        }
        ImFont* font = ImGui::GetFont();
        float y = max.y - bars - (60.0f + static_cast<float>(item.lines.size()) * 26.0f) * scale;
        const float x = min.x + (max.x - min.x) * 0.06f;
        bool typing = false;
        for (size_t i = 0; i < item.lines.size(); ++i)
        {
            const std::string full = sampler.Fill(item.lines[i]);
            const std::string shown = sampler.Text(item, i, time);
            const float fontSize = ImGui::GetFontSize() * (i == 0 ? 1.45f : 1.0f) * scale;
            const ImU32 colour = i == 0 ? IM_COL32(226, 230, 234, static_cast<int>(255 * alpha)) : IM_COL32(150, 158, 166, static_cast<int>(255 * alpha));
            draw->AddText(font, fontSize, {x, y}, colour, shown.c_str());
            // The cursor, at the end of the line being typed, blinking.
            if (!typing && shown.size() < full.size() && !shown.empty())
            {
                typing = true;
                const float width = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, shown.c_str()).x;
                if (std::fmod(time, 0.5f) < 0.3f)
                {
                    draw->AddRectFilled({x + width + 2.0f, y + 2.0f}, {x + width + 2.0f + fontSize * 0.5f, y + fontSize}, colour);
                }
            }
            y += fontSize + 8.0f * scale;
        }
    }

    // Black over all of it last.
    const float fade = sampler.Fade(time);
    if (fade > 0.001f)
    {
        draw->AddRectFilled(min, max, IM_COL32(0, 0, 0, static_cast<int>(255 * fade)));
    }
}

void PredationGame::DrawCinematicDebug()
{
    if (!m_cineDebug)
    {
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(12.0f, 120.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420.0f, 460.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Cinematic", &m_cineDebug))
    {
        ImGui::End();
        return;
    }
    if (!m_cine.Active())
    {
        ImGui::TextDisabled("None playing.%s", m_cineHandBack > 0.0f ? " Handing the picture back." : "");
        ImGui::TextDisabled("%zu in Assets/Cinematics", m_cinematics.size());
        for (const auto& [name, cinematic] : m_cinematics)
        {
            if (ImGui::SmallButton(("Play##" + name).c_str()))
            {
                PlayCinematic(name);
            }
            ImGui::SameLine();
            ImGui::Text("%s  (%.1f s)", name.c_str(), cinematic.duration);
        }
        ImGui::End();
        return;
    }
    const Cinematic& cinematic = m_cine.Playing();
    static const char* const kStates[] = {"stopped", "playing", "paused", "finished"};
    ImGui::Text("%s -- %s", cinematic.name.c_str(), kStates[static_cast<int>(m_cine.GetState())]);
    float time = m_cine.Time();
    if (ImGui::SliderFloat("time", &time, 0.0f, cinematic.duration, "%.2f s"))
    {
        m_cine.Seek(time, m_scene, *this);
    }
    std::string shot;
    float blend = 1.0f;
    const CameraState picture = m_cine.Picture(&shot, &blend);
    ImGui::Text("shot %s%s", shot.c_str(), blend < 1.0f ? (" (blending " + std::to_string(static_cast<int>(blend * 100.0f)) + "%)").c_str() : "");
    const glm::vec3 degrees = DegreesFromTurn(picture.rotation);
    ImGui::Text("camera %.1f %.1f %.1f  turn %.0f %.0f %.0f  fov %.0f", picture.position.x, picture.position.y, picture.position.z, degrees.x, degrees.y,
                degrees.z, picture.fov);
    const CinematicSampler sampler = m_cine.Sampler();
    ImGui::Text("shake %.2f  fade %.2f  bars %.2f  holds %s", sampler.Shake(time), sampler.Fade(time), sampler.Letterbox(time), m_cineHolds ? "yes" : "no");
    if (ImGui::CollapsingHeader("Actors", ImGuiTreeNodeFlags_DefaultOpen))
    {
        for (const ActorDef& actor : cinematic.actors)
        {
            const CinePose pose = sampler.Actor(actor.name, time);
            ImGui::Text("%s (%s)  %.1f %.1f %.1f", actor.name.c_str(), actor.bind.empty() ? actor.model.c_str() : ("bound " + actor.bind).c_str(),
                        pose.position.x, pose.position.y, pose.position.z);
        }
    }
    if (ImGui::CollapsingHeader("Tracks"))
    {
        ImGui::Text("%zu camera(s), %zu shot(s), %zu actor track(s), %zu path(s)", cinematic.cameras.size(), cinematic.shots.size(),
                    cinematic.actorTracks.size(), cinematic.paths.size());
        ImGui::Text("%zu sound(s), %zu marker(s), %zu text(s), %zu clip(s), %zu particle(s)", cinematic.sounds.size(), cinematic.markers.size(),
                    cinematic.texts.size(), cinematic.clips.size(), cinematic.particles.size());
    }
    if (ImGui::CollapsingHeader("Coming", ImGuiTreeNodeFlags_DefaultOpen))
    {
        int shown = 0;
        for (const CinematicHappening& happening : HappeningsBetween(cinematic, time, cinematic.duration))
        {
            if (shown++ >= 8)
            {
                break;
            }
            const char* kinds[] = {"marker", "sound", "clip", "particles", "text"};
            ImGui::TextDisabled("%6.2f  %s", happening.time, kinds[static_cast<int>(happening.kind)]);
        }
    }
    if (ImGui::CollapsingHeader("Happened", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const auto& history = m_cine.History();
        for (size_t i = history.size() > 10 ? history.size() - 10 : 0; i < history.size(); ++i)
        {
            ImGui::TextDisabled("%6.2f  %s", history[i].first, history[i].second.c_str());
        }
    }
    if (ImGui::CollapsingHeader("Anchors"))
    {
        for (const auto& [name, pose] : m_cine.Bindings().anchors)
        {
            ImGui::TextDisabled("%s  %.1f %.1f %.1f", name.c_str(), pose.position.x, pose.position.y, pose.position.z);
        }
    }
    ImGui::End();
}

void PredationGame::DrawCinematicPaths(DebugDraw& draw)
{
    const bool editing = m_cineEditor.IsOpen() && !m_cineEditor.ThroughCinematic();
    if ((!m_cineDebug && !editing) || !m_cine.Active())
    {
        return;
    }
    const Cinematic& cinematic = m_cine.Playing();
    const CinematicSampler sampler = m_cine.Sampler();
    const float step = std::max(cinematic.duration / 160.0f, 0.05f);
    // Every camera's path, and where it is now; the one in the shot brighter, the one selected in the editor brightest,
    // with its keys and the edges of what it sees.
    std::string shot;
    m_cine.Picture(&shot);
    const std::string selected = m_cineEditor.IsOpen() ? m_cineEditor.SelectedCamera(cinematic) : std::string();
    for (const CameraTrack& camera : cinematic.cameras)
    {
        const bool picked = camera.name == selected;
        const uint32_t colour = picked ? Color::kWhite : camera.name == shot ? Color::kYellow : Color::kGrey;
        glm::vec3 last = sampler.Camera(camera, 0.0f).position;
        for (float t = step; t <= cinematic.duration; t += step)
        {
            const glm::vec3 now = sampler.Camera(camera, t).position;
            draw.Line(last, now, colour);
            last = now;
        }
        const CameraState state = sampler.Camera(camera, m_cine.Time());
        draw.Sphere(state.position, 0.4f, colour);
        draw.Line(state.position, state.position + state.rotation * glm::vec3(0.0f, 0.0f, -3.0f), colour);
        if (picked)
        {
            for (const CameraKey& key : camera.keys)
            {
                draw.Sphere(sampler.Camera(camera, key.time).position, 0.25f, Color::kYellow);
            }
            // What it sees, four metres out, at the picture's shape.
            const float half = std::tan(glm::radians(state.fov) * 0.5f) * 4.0f;
            const float aspect = static_cast<float>(std::max<int>(m_app->GetRenderer().Width(), 1)) /
                                 static_cast<float>(std::max<int>(m_app->GetRenderer().Height(), 1));
            glm::vec3 corners[4];
            for (int i = 0; i < 4; ++i)
            {
                const glm::vec3 local{(i == 0 || i == 3 ? -half : half), (i < 2 ? half : -half) / aspect, -4.0f};
                corners[i] = state.position + state.rotation * local;
                draw.Line(state.position, corners[i], Color::kWhite);
            }
            for (int i = 0; i < 4; ++i)
            {
                draw.Line(corners[i], corners[(i + 1) % 4], Color::kWhite);
            }
        }
    }
    // Every actor's, and the paths the game supplied.
    for (const ActorDef& actor : cinematic.actors)
    {
        glm::vec3 last = sampler.Actor(actor.name, 0.0f).position;
        for (float t = step; t <= cinematic.duration; t += step)
        {
            const glm::vec3 now = sampler.Actor(actor.name, t).position;
            draw.Line(last, now, Color::kCyan);
            last = now;
        }
    }
    for (const auto& [name, path] : m_cine.Bindings().paths)
    {
        for (size_t i = 1; i < path.size(); ++i)
        {
            draw.Line(path[i - 1] + glm::vec3(0.0f, 0.2f, 0.0f), path[i] + glm::vec3(0.0f, 0.2f, 0.0f), Color::kMagenta);
        }
    }
    for (const auto& [name, pose] : m_cine.Bindings().anchors)
    {
        draw.Line(pose.position, pose.position + glm::vec3(0.0f, 2.0f, 0.0f), Color::kGreen);
        draw.Line(pose.position + glm::vec3(0.0f, 2.0f, 0.0f), pose.position + glm::vec3(0.0f, 2.0f, 0.0f) + pose.rotation * glm::vec3(0.0f, 0.0f, -1.5f),
                  Color::kGreen);
    }
}

// --- The editor --------------------------------------------------------------------------------------

void PredationGame::PlayForEditing(const Cinematic& cinematic, float from)
{
    ParkVehicles();
    m_cine.Play(cinematic, CinematicBindingsNow(), m_scene, m_app->GetMeshes(), *this, from);
    m_cineHandBack = 0.0f;
    UpdateVehicleLamps();
}

CinematicEditor::Context PredationGame::CinematicEditorContext()
{
    CinematicEditor::Context context;
    context.player = &m_cine;
    context.scene = &m_scene;
    context.host = this;
    context.library = &m_cinematics;
    context.folder = Paths::AssetsRoot() / "Cinematics";
    for (const auto& [name, variants] : m_soundBank)
    {
        context.sounds.push_back(name);
    }
    context.replay = [this](const Cinematic& cinematic, float from) { PlayForEditing(cinematic, from); };
    context.hear = [this](const std::string& sound) { PlayNamed(sound, m_renderEye, 1.0f, 1.0f, false); };
    context.pick = [this](glm::vec3& at)
    {
        const FlyCamera& view = m_cineEditor.View();
        const RayHit hit = m_app->GetPhysics().RayCastStatic(view.position, view.Forward(), 1000.0f);
        if (hit)
        {
            at = hit.position;
        }
        return hit.hit;
    };
    context.clipsOf = [this](const std::string& actor)
    {
        std::vector<std::string> names;
        const ActorDef* def = m_cine.Playing().FindActor(actor);
        if (def == nullptr)
        {
            return names;
        }
        const ModelAsset* model = nullptr;
        if (const VehicleProp* vehicle = Bound(def->bind, m_facility, m_missionProps, m_ship))
        {
            model = vehicle->Model();
        }
        else if (!def->model.empty())
        {
            // Read once, then remembered.
            const auto known = m_cineClipNames.find(def->model);
            if (known != m_cineClipNames.end())
            {
                return known->second;
            }
            ModelAsset loaded;
            if (loaded.LoadFromFile(ModelPath(def->model)))
            {
                for (const AnimationClip& clip : loaded.clips)
                {
                    names.push_back(clip.name);
                }
            }
            m_cineClipNames[def->model] = names;
            return names;
        }
        if (model != nullptr)
        {
            for (const AnimationClip& clip : model->clips)
            {
                names.push_back(clip.name);
            }
        }
        return names;
    };
    return context;
}

void PredationGame::OpenCinematicEditor(const std::string& name)
{
    if (m_screen != Screen::Playing)
    {
        m_app->GetConsole().PrintError("The cinematic editor works in the world: be somewhere first.");
        return;
    }
    // The one named, or the one playing, from where it has got to; with neither, the editor asks which.
    const std::string which = !name.empty() ? name : m_cine.Active() ? m_cine.Playing().name : std::string();
    if (!which.empty())
    {
        const auto found = m_cinematics.find(which);
        if (found == m_cinematics.end())
        {
            m_app->GetConsole().PrintError("No cinematic called '" + which + "' (cine_list says which there are).");
            return;
        }
        const float from = m_cine.Active() && m_cine.Playing().name == which ? m_cine.Time() : 0.0f;
        PlayForEditing(found->second, from);
        m_cine.SetPaused(true);
    }
    // The free camera starts where the eyes are.
    FlyCamera& view = m_cineEditor.View();
    view.position = m_renderEye;
    view.yaw = m_lookYaw;
    view.pitch = m_lookPitch;
    view.fovDegrees = 60.0f;
    m_cineEditor.Open();
    m_cineEditorLooking = false;
    m_wantMouseCaptured = false;
    UpdateMouseCapture();
}

void PredationGame::CloseCinematicEditor()
{
    if (!m_cineEditor.IsOpen())
    {
        return;
    }
    if (m_cineEditor.Unsaved())
    {
        m_app->GetConsole().Print("Cinematic editor closed with changes not saved: they are gone.");
    }
    m_cineEditor.Close();
    m_cineEditorLooking = false;
    StopCinematic(false);
    m_wantMouseCaptured = true;
    UpdateMouseCapture();
}

void PredationGame::UpdateCinematicEditor(float dt)
{
    if (!m_cineEditor.IsOpen())
    {
        return;
    }
    if (m_screen != Screen::Playing)
    {
        m_cineEditor.Close();
        return;
    }
    Input& input = m_app->GetInput();
    const ImGuiIO& io = ImGui::GetIO();
    if (!m_app->IsConsoleOpen() && !io.WantTextInput && !m_cineEditorLooking && input.WasActionPressed("quit_capture") && m_cineEditor.MayClose())
    {
        CloseCinematicEditor();
        return;
    }
    // As in the model editor: the pointer is free, and holding the right button over the picture takes it to look round
    // -- taking the view out of the cinematic's camera into a free one where it was.
    const bool wantLook = input.IsMouseDown(MouseButton::Right) && (m_cineEditorLooking || !m_app->IsUiCapturingMouse());
    if (wantLook != m_cineEditorLooking)
    {
        m_cineEditorLooking = wantLook;
        m_wantMouseCaptured = wantLook;
        UpdateMouseCapture();
        if (wantLook && m_cine.Active())
        {
            m_cineEditor.LookFreely(m_cine.Picture());
        }
    }
    FlyCamera& view = m_cineEditor.View();
    if (m_cineEditorLooking && !m_discardNextMouseDelta)
    {
        const glm::vec2 delta = input.MouseDelta();
        const float sensitivity = glm::radians(view.mouseSensitivity);
        view.yaw += delta.x * sensitivity;
        view.pitch = std::clamp(view.pitch - delta.y * sensitivity, glm::radians(-89.0f), glm::radians(89.0f));
    }
    m_discardNextMouseDelta = false;
    // Flying: WASD, Q and E down and up, Shift faster. Not while typing, and not with Ctrl, which is the editor's.
    if (!m_cineEditor.ThroughCinematic() && !io.WantTextInput && !io.KeyCtrl)
    {
        const float speed = 12.0f * (io.KeyShift ? 5.0f : 1.0f) * (io.KeyAlt ? 0.2f : 1.0f);
        glm::vec3 move{0.0f};
        move += view.Forward() * ((input.IsActionDown("move_forward") ? 1.0f : 0.0f) - (input.IsActionDown("move_back") ? 1.0f : 0.0f));
        move += view.Right() * ((input.IsActionDown("move_right") ? 1.0f : 0.0f) - (input.IsActionDown("move_left") ? 1.0f : 0.0f));
        move.y += (input.IsActionDown("lean_right") ? 1.0f : 0.0f) - (input.IsActionDown("lean_left") ? 1.0f : 0.0f);
        view.position += move * speed * dt;
    }
}

void PredationGame::DrawCinematicEditor()
{
    if (!m_cineEditor.IsOpen())
    {
        return;
    }
    CinematicEditor::Context context = CinematicEditorContext();
    m_cineEditor.Draw(context);
    // Black round the preview, under the windows.
    glm::vec2 min;
    glm::vec2 max;
    if (m_cineEditor.Preview(min, max))
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        const ImVec2 a = viewport->Pos;
        const ImVec2 b{viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y};
        const ImU32 black = IM_COL32(8, 9, 11, 255);
        draw->AddRectFilled(a, {b.x, min.y}, black);
        draw->AddRectFilled({a.x, max.y}, b, black);
        draw->AddRectFilled({a.x, min.y}, {min.x, max.y}, black);
        draw->AddRectFilled({max.x, min.y}, {b.x, max.y}, black);
        draw->AddRect({min.x - 1.0f, min.y - 1.0f}, {max.x + 1.0f, max.y + 1.0f}, IM_COL32(70, 74, 82, 255));
    }
}

// --- Commands --------------------------------------------------------------------------------------

void PredationGame::RegisterCinematicCommands()
{
    Console& console = m_app->GetConsole();
    console.RegisterCommand("cine_list", "List the cinematics in Assets/Cinematics", [this](const std::vector<std::string>&)
                            {
                                for (const auto& [name, cinematic] : m_cinematics)
                                {
                                    char line[160];
                                    std::snprintf(line, sizeof(line), "%s  %.1f s, %zu camera(s), %zu actor(s)", name.c_str(), cinematic.duration,
                                                  cinematic.cameras.size(), cinematic.actors.size());
                                    m_app->GetConsole().Print(line);
                                }
                            });
#if PRED_DEV_TOOLS
    // Nobody playing the game can skip, pause, hurry or start a cinematic: these are for making them, in a development
    // build. In the game one plays to its end, the same for everybody.
    console.RegisterCommand("cine_play", "Play a cinematic: cine_play <name> [from seconds]", [this](const std::vector<std::string>& args)
                            {
                                if (args.size() < 2)
                                {
                                    m_app->GetConsole().PrintError("usage: cine_play <name> [from seconds]");
                                    return;
                                }
                                PlayCinematic(args[1], args.size() >= 3 ? std::strtof(args[2].c_str(), nullptr) : 0.0f);
                            });
    console.RegisterCommand("cine_restart", "Play the cinematic playing again from its start", [this](const std::vector<std::string>&)
                            {
                                if (m_cine.Active())
                                {
                                    PlayCinematic(m_cine.Playing().name);
                                }
                            });
    console.RegisterCommand("cine_skip", "Skip to the end of the cinematic playing, as if it had played", [this](const std::vector<std::string>&)
                            {
                                if (m_cine.Active())
                                {
                                    // Everything it would have made happen on the way still happens: a skipped insertion still lands.
                                    const float end = m_cine.Playing().duration;
                                    m_cine.SetPaused(false);
                                    m_cine.Update(end - m_cine.Time() + 1.0e-3f, m_scene, *this);
                                    StopCinematic(false);
                                }
                            });
    console.RegisterCommand("cine_seek", "Go to a moment of the cinematic playing: cine_seek <seconds>", [this](const std::vector<std::string>& args)
                            {
                                if (m_cine.Active() && args.size() >= 2)
                                {
                                    m_cine.Seek(std::strtof(args[1].c_str(), nullptr), m_scene, *this);
                                }
                            });
    console.RegisterCommand("cine_pause", "Pause the cinematic playing", [this](const std::vector<std::string>&) { m_cine.SetPaused(true); });
    console.RegisterCommand("cine_resume", "Carry on with the cinematic paused", [this](const std::vector<std::string>&) { m_cine.SetPaused(false); });
    console.RegisterCommand("cine_speed", "Play cinematics faster or slower: cine_speed <times>", [this](const std::vector<std::string>& args)
                            {
                                if (args.size() >= 2)
                                {
                                    m_cine.SetSpeed(std::clamp(std::strtof(args[1].c_str(), nullptr), 0.05f, 8.0f));
                                }
                            });
    console.RegisterCommand("cine_stop", "Stop the cinematic playing, handing the picture back", [this](const std::vector<std::string>&)
                            { StopCinematic(true); });
    console.RegisterCommand("cine_reload", "Read Assets/Cinematics again", [this](const std::vector<std::string>&)
                            {
                                LoadCinematics();
                                m_app->GetConsole().Print(std::to_string(m_cinematics.size()) + " cinematic(s)");
                            });
    console.RegisterCommand("cine_debug", "Show or hide the cinematic panel and paths", [this](const std::vector<std::string>&)
                            { m_cineDebug = !m_cineDebug; });
    console.RegisterCommand("cine_edit", "Open the cinematic editor: cine_edit [name]; again with no name closes it",
                            [this](const std::vector<std::string>& args)
                            {
                                if (m_cineEditor.IsOpen() && args.size() < 2)
                                {
                                    CloseCinematicEditor();
                                    return;
                                }
                                OpenCinematicEditor(args.size() >= 2 ? args[1] : std::string());
                            });
#endif
}

} // namespace pred
