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
    // The ship, for now the testing area: where everybody gathers.
    bindings.anchors["ship"] = {{0.0f, 0.0f, TestMapSpec::kSpawnZ}, {}};
    // This player's own eyes, for a shot that begins or ends in them.
    bindings.anchors["player"] = {m_renderEye, TurnFromDegrees({glm::degrees(m_lookPitch), glm::degrees(m_lookYaw), 0.0f})};

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
        }
    }
    // What a title card fills in.
    const SiteTitle title = m_siteNames.For(m_facility.Seed());
    bindings.words["site"] = title.site;
    bindings.words["planet"] = title.planet;
    const size_t comma = title.site.find(", ");
    bindings.words["region"] = comma == std::string::npos ? std::string() : title.site.substr(comma + 2);
    const uint32_t seed = m_facility.Seed();
    bindings.words["local_time"] = "LOCAL " + TwoFigures(static_cast<int>((seed * 7u) % 24u)) + ":" + TwoFigures(static_cast<int>((seed * 13u) % 60u));
    bindings.words["conditions"] = "-" + std::to_string(18 + static_cast<int>(seed % 23u)) + " C  WIND " + std::to_string(4 + static_cast<int>((seed / 3u) % 19u)) +
                                   " M/S  VISIBILITY " + (site.sky.fogEnd < 45.0f ? "POOR" : site.sky.fogEnd < 70.0f ? "LOW" : "FAIR");
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
    m_cine.Stop(m_scene);
    m_cineHolds = false;
}

bool PredationGame::CinematicHoldsPlayers() const
{
    return m_cineHolds;
}

bool PredationGame::CinematicHoldsWorld() const
{
    return m_cineHolds;
}

void PredationGame::UpdateCinematic(float dt)
{
    UpdateCinematicParticles(dt);
    if (!m_cine.Active())
    {
        m_cineHolds = false;
        return;
    }
    m_cine.Update(dt, m_scene, *this);
    m_cineHolds = m_cine.Playing().pausesGameplay;
    if (m_cine.GetState() == CinematicPlayer::State::Finished)
    {
        StopCinematic(true);
    }
}

bool PredationGame::CinematicCamera(glm::mat4& view, glm::vec3& eye, float dt, float gameplayFov)
{
    m_cineFov = 0.0f;
    m_cineFar = 0.0f;
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

bool PredationGame::CineFindBound(const std::string& bind, CinePose& where)
{
    // Nothing the game has can be moved by a cinematic yet: the site's vehicles come with the insertion.
    (void)bind;
    (void)where;
    return false;
}

void PredationGame::CineMoveBound(const std::string& bind, const CinePose& pose)
{
    (void)bind;
    (void)pose;
}

void PredationGame::CinePoseBound(const std::string& bind, const std::string& clip, float clipTime)
{
    (void)bind;
    (void)clip;
    (void)clipTime;
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
        Say(marker.value);
    }
}

void PredationGame::CineParticle(const ParticleEvent& particle, const glm::vec3& at)
{
    // A handful of puffs, by the kind of effect: exhaust glows and rises fast, snow and dust are pale and hang.
    const bool exhaust = particle.effect.find("exhaust") != std::string::npos || particle.effect.find("thrust") != std::string::npos;
    Material material = Material::Diffuse(exhaust ? glm::vec3(0.9f, 0.55f, 0.25f) : glm::vec3(0.75f, 0.78f, 0.8f), 0.9f);
    material.emissive = exhaust ? glm::vec3(1.6f, 0.8f, 0.3f) : glm::vec3(0.0f);
    const MeshHandle mesh = m_app->GetMeshes().Upload(Primitives::Sphere(0.5f, 10, 8), "cine_puff");
    const int count = exhaust ? 10 : 16;
    for (int i = 0; i < count; ++i)
    {
        const float a = static_cast<float>(i) * 2.399963f; // golden angle: spread without clumping
        const float r = 0.4f + 0.6f * static_cast<float>(i % 5) / 4.0f;
        CinePuff puff;
        Transform transform;
        transform.position = at + glm::vec3(std::cos(a) * r * 0.4f, 0.0f, std::sin(a) * r * 0.4f);
        transform.scale = glm::vec3(0.1f);
        puff.entity = m_scene.CreateMeshEntity("cine_puff", transform, mesh, material);
        if (MeshRenderer* renderer = m_scene.GetMeshRenderer(puff.entity))
        {
            renderer->castsShadow = false;
        }
        puff.velocity = exhaust ? glm::vec3(std::cos(a) * r, -3.0f - r * 2.0f, std::sin(a) * r)
                                : glm::vec3(std::cos(a) * r * 2.5f, 0.8f + r, std::sin(a) * r * 2.5f);
        puff.life = std::max(particle.duration, 0.2f) * (0.7f + 0.3f * r);
        puff.from = exhaust ? 0.3f : 0.2f;
        puff.to = exhaust ? 1.4f : 2.2f;
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
    // The cabin lamps of what a cinematic brings with it are its models' own glow for now; the world's lamps by name
    // come with the ship.
    (void)light;
    (void)intensity;
}

// --- Over the picture ---------------------------------------------------------------------------------

void PredationGame::DrawCinematicOverlay()
{
    if (!m_cine.Active() || m_screen != Screen::Playing)
    {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImVec2 min = viewport->Pos;
    const ImVec2 max{viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y};
    const CinematicSampler sampler = m_cine.Sampler();
    const float time = m_cine.Time();

    // The bars, top and bottom.
    const float bars = sampler.Letterbox(time) * viewport->Size.y * 0.11f;
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
            const ImVec2 size = ImGui::CalcTextSize(text.c_str());
            const ImVec2 at{(min.x + max.x - size.x) * 0.5f, max.y - bars - size.y - 36.0f};
            draw->AddText(at, IM_COL32(230, 232, 236, static_cast<int>(255 * alpha)), text.c_str());
            continue;
        }
        ImFont* font = ImGui::GetFont();
        float y = max.y - bars - 60.0f - static_cast<float>(item.lines.size()) * 26.0f;
        const float x = min.x + viewport->Size.x * 0.06f;
        bool typing = false;
        for (size_t i = 0; i < item.lines.size(); ++i)
        {
            const std::string full = sampler.Fill(item.lines[i]);
            const std::string shown = sampler.Text(item, i, time);
            const float scale = i == 0 ? 1.45f : 1.0f;
            const float fontSize = ImGui::GetFontSize() * scale;
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
            y += fontSize + 8.0f;
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
    if (!m_cineDebug || !m_cine.Active())
    {
        return;
    }
    const Cinematic& cinematic = m_cine.Playing();
    const CinematicSampler sampler = m_cine.Sampler();
    const float step = std::max(cinematic.duration / 160.0f, 0.05f);
    // Every camera's path, and where it is now; the one in the shot brighter.
    std::string shot;
    m_cine.Picture(&shot);
    for (const CameraTrack& camera : cinematic.cameras)
    {
        const uint32_t colour = camera.name == shot ? Color::kYellow : Color::kGrey;
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
}

} // namespace pred
