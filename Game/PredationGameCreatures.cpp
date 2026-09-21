// The creature, as the game runs it: navigation, spawning, senses, noises, strikes, and the tools for
// looking inside its head.
//
// A separate file from PredationGame.cpp because that file is already the length of a short book,
// and this is one subject with a clear edge: everything here is about the creature and nothing else
// needs to read it.

#include "Game/PredationGame.h"

#include "Engine/Core/CVar.h"
#include "Engine/Core/Log.h"
#include "Engine/Debug/DebugCategories.h"
#include "Engine/Render/DebugDraw.h"

#include <imgui.h>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace pred
{
namespace
{

CVar<int> cv_aiCreatures{"ai.creatures", 1, "How many creatures a new game starts with"};
CVar<int> cv_aiSeed{"ai.seed", 0,
                    "The seed a new game's creature is made from; 0 picks a different one each game"};

// A strike's reach as the game checks it when the blow lands. A little longer than the brain's own
// reach, because the brain decided to swing when somebody was in reach and they get this much room to
// have stepped back during the wind-up before it misses.
constexpr float kStrikeLands = 2.6f;
constexpr float kStrikeDamage = 28.0f;

float BodyHeight(PlayerStance stance)
{
    switch (stance)
    {
    case PlayerStance::Crouching:
        return 1.15f;
    case PlayerStance::Prone:
        return 0.45f;
    case PlayerStance::Standing:
    default:
        return 1.8f;
    }
}

float Horizontal(const glm::vec3& a, const glm::vec3& b)
{
    const glm::vec3 d = b - a;
    return std::sqrt(d.x * d.x + d.z * d.z);
}

} // namespace

void PredationGame::BuildNavigation()
{
    std::string error;
    if (!m_nav.Build(m_app->GetPhysics().StaticTriangles(), NavSettings{}, &error))
    {
        PRED_LOG_ERROR(AI, "No navigation mesh, so no creature can move: {}", error);
    }
}

void PredationGame::ClearCreatures()
{
    m_creatures.clear();
    m_noises.clear();
    m_lastVoiceNoise.clear();
}

bool PredationGame::SpawnCreature(uint32_t seed, const glm::vec3& awayFrom, const glm::vec3* exactly)
{
    if (!m_nav.Valid())
    {
        return false;
    }
    glm::vec3 best{0.0f};
    float bestDistance = -1.0f;
    if (exactly != nullptr)
    {
        if (!m_nav.NearestPoint(*exactly, 4.0f, best))
        {
            return false;
        }
        bestDistance = Horizontal(best, awayFrom);
    }
    else
    {
        // Somewhere far from the players and on the mesh: several candidates, the furthest wins. A
        // creature that starts in sight of the spawn is a creature met in the first second, which is
        // the opposite of the game.
        uint32_t pick = seed * 747796405u + 2891336453u;
        for (int i = 0; i < 24; ++i)
        {
            glm::vec3 candidate;
            if (m_nav.RandomPointNear(awayFrom, 40.0f, pick, candidate))
            {
                const float distance = Horizontal(candidate, awayFrom);
                if (distance > bestDistance)
                {
                    bestDistance = distance;
                    best = candidate;
                }
            }
        }
        if (bestDistance < 0.0f)
        {
            return false;
        }
    }
    if (m_creatures.size() >= kMaxCreatures)
    {
        return false;
    }
    const CreatureTraits traits = CreatureTraits::FromSeed(seed);
    m_creatures.push_back(std::make_unique<Creature>(m_scene, m_app->GetMeshes(), m_app->GetPhysics(),
                                                     &m_nav, traits, best));
    m_creatures.back()->SetNetId(m_nextCreatureId++);
    PRED_LOG_INFO(AI, "Creature spawned {:.0f} m away at {:.1f} {:.1f} {:.1f}: {}", bestDistance, best.x,
                  best.y, best.z, traits.Describe());
    return true;
}

void PredationGame::SpawnCreatures()
{
    ClearCreatures();
    m_creatureClock = 0.0f;
    if (!IsAuthority())
    {
        return;
    }
    const int count = std::clamp(cv_aiCreatures.Get(), 0, static_cast<int>(kMaxCreatures));
    // A fixed seed when one is set, so a strange behaviour can be had again; otherwise the clock, so
    // every game is a different animal.
    uint32_t seed = static_cast<uint32_t>(cv_aiSeed.Get());
    if (seed == 0)
    {
        seed = static_cast<uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count() & 0x7FFFFFFF);
    }
    for (int i = 0; i < count; ++i)
    {
        SpawnCreature(seed + static_cast<uint32_t>(i) * 7919u, m_player.State().position);
    }
}

Creature* PredationGame::CreatureForBody(BodyHandle body)
{
    if (!body.IsValid())
    {
        return nullptr;
    }
    for (const std::unique_ptr<Creature>& creature : m_creatures)
    {
        if (creature->Body() == body)
        {
            return creature.get();
        }
    }
    return nullptr;
}

void PredationGame::MakeNoise(NoiseKind kind, const glm::vec3& at, float reach, int player)
{
    // Only where a creature is listening. On a client this is a no-op: the host hears everything
    // that happens, including what the client did, because it is the host that resolves it.
    if (!IsAuthority() || m_creatures.empty() || m_screen != Screen::Playing)
    {
        return;
    }
    if (kind == NoiseKind::Voice && player >= 0)
    {
        float& last = m_lastVoiceNoise[player];
        if (m_creatureClock - last < 0.5f)
        {
            return;
        }
        last = m_creatureClock;
    }
    Noise noise;
    noise.kind = kind;
    noise.position = at;
    noise.reach = reach;
    noise.player = player;
    // Bounded: a flood of noises in one tick is a bug somewhere else, and should not become a stall.
    if (m_noises.size() < 256)
    {
        m_noises.push_back(noise);
    }
}

bool PredationGame::OnShotResolved(ShotResult& result, const glm::vec3& origin, int shooter)
{
    // A gunshot is the loudest thing a player can do, and is heard from the shooter's position:
    // where it hit says nothing about where it came from.
    MakeNoise(NoiseKind::Gunshot, origin, NoiseReach::kGunshot, shooter);
    if (!result)
    {
        return false;
    }
    Creature* creature = CreatureForBody(result.body);
    if (creature == nullptr)
    {
        // A round striking a wall is a sound of its own, somewhere else: enough to turn a head
        // towards the wrong place, which is a thing a player can use.
        if (result.surface)
        {
            MakeNoise(NoiseKind::Impact, result.position, NoiseReach::kImpact, -1);
        }
        return false;
    }
    if (IsAuthority())
    {
        creature->TakeDamage(result.damage, shooter, origin, m_creatureClock);
    }
    result.surface = false;
    return true;
}

float PredationGame::LightAt(const glm::vec3& feet, bool torchOn) const
{
    // Under a roof is dark and under the sky is not. Crude, and right for the level there is: the dark
    // room is dark because it has a roof, and nothing else in it pretends otherwise. A torch that is
    // on makes whoever carries it the brightest thing in any room.
    const RayHit roof = m_app->GetPhysics().RayCast(feet + glm::vec3(0.0f, 1.7f, 0.0f),
                                                    glm::vec3(0.0f, 1.0f, 0.0f), 40.0f);
    const float light = roof ? 0.18f : 1.0f;
    return torchOn ? std::max(light, 0.95f) : light;
}

void PredationGame::UpdateCreatures(float dt)
{
    // Paused alone, it waits too. The pause menu does not stop the world -- with other people in it,
    // it cannot -- but a game played alone is nobody else's, and being eaten in the settings menu is
    // not a thing that should happen to anybody.
    const bool pausedAlone = m_paused && m_sessionMode == SessionMode::Offline;
    if (!IsAuthority() || m_screen != Screen::Playing || m_creatures.empty() || pausedAlone)
    {
        m_noises.clear();
        return;
    }
    m_creatureClock += dt;

    // Everybody it could see or hear: this machine's player, and, when hosting, everybody else.
    std::vector<SensedPlayer> players;
    {
        const PlayerState& local = m_player.State();
        SensedPlayer me;
        me.id = LocalPlayerId();
        me.name = PlayerName();
        me.feet = local.position;
        me.velocity = local.velocity;
        me.height = BodyHeight(local.stance);
        me.alive = local.alive;
        me.hidden = m_hidingSpot >= 0;
        me.light = LightAt(local.position, m_torchOn);
        players.push_back(me);
    }
    if (m_sessionMode == SessionMode::Host)
    {
        for (const RemotePlayerView& remote : m_host.Remotes())
        {
            if (remote.id == LocalPlayerId())
            {
                continue;
            }
            SensedPlayer other;
            other.id = remote.id;
            other.name = remote.name;
            other.feet = remote.position;
            other.velocity = remote.velocity;
            other.height = BodyHeight(remote.stance);
            other.alive = remote.alive;
            other.light = LightAt(remote.position, remote.torchOn);
            for (const WorldObjects::HidingSpot& spot : m_world.HidingSpots())
            {
                other.hidden = other.hidden || (spot.occupied && spot.occupant == remote.id);
            }
            players.push_back(other);
        }
    }

    PhysicsWorld& physics = m_app->GetPhysics();
    for (const std::unique_ptr<Creature>& creature : m_creatures)
    {
        CreatureSenses senses;
        senses.players = players;
        senses.noises = m_noises;
        const BodyHandle self = creature->Body();
        // Whether anything solid is between two points, ignoring the creature's own body -- its eyes
        // are inside it -- and stopping a little short of the far end, which is usually somebody
        // with a shape of their own.
        senses.clearLine = [&physics, self](const glm::vec3& from, const glm::vec3& to)
        {
            const glm::vec3 along = to - from;
            const float length = glm::length(along);
            if (length < 0.4f)
            {
                return true;
            }
            return !physics.RayCast(from, along / length, length - 0.35f, self);
        };
        creature->Update(std::move(senses), m_creatureClock, dt);

        // A strike landing. Checked again here rather than trusted: somebody who stepped back
        // during the wind-up is out of reach now, and that is the whole reason there is one.
        const int target = creature->Brain().Intent().strikeTarget;
        if (target < 0)
        {
            continue;
        }
        for (const SensedPlayer& player : players)
        {
            if (player.id != target || !player.alive)
            {
                continue;
            }
            const float reach = Horizontal(creature->Position(), player.feet);
            const float rise = std::abs(creature->Position().y - player.feet.y);
            if (reach > kStrikeLands || rise > 1.5f)
            {
                PRED_LOG_INFO(AI, "Strike at {} (player {}) missed: {:.1f} m away", player.name, player.id, reach);
                break;
            }
            // Through the same door as a bullet, so everything that follows from being hurt follows
            // from this too: the victim is told (their health bar, their hurt sound), and a blow that
            // kills them kills them properly -- ragdoll, dropped kit, respawn. Taking health off
            // directly did none of that, and a client struck by it never saw their health fall.
            glm::vec3 blow = player.feet - creature->Position();
            blow.y = 0.0f;
            blow = glm::length(blow) > 1e-3f ? glm::normalize(blow) : creature->Forward();
            ApplyPlayerDamage(static_cast<uint8_t>(player.id), kStrikeDamage, kNoKiller, blow, "creature");
            PlaySound(m_sounds.hurt.Pick(), player.feet + glm::vec3(0.0f, 1.2f, 0.0f), 0.9f, 0.85f);
            PRED_LOG_INFO(AI, "Strike at {} (player {}) landed", player.name, player.id);
            break;
        }
    }
    m_noises.clear();
}

void PredationGame::UpdateCreatureVisuals(float dt)
{
    // A client's creatures have no mind here. They are eased towards what the host last said.
    const bool shownOnly = !IsAuthority();
    for (const std::unique_ptr<Creature>& creature : m_creatures)
    {
        if (shownOnly)
        {
            creature->FollowReceived(dt);
        }
        creature->UpdateVisual(dt);
    }
}

void PredationGame::SendCreatureState()
{
    CreatureStateMessage state;
    state.sequence = ++m_creatureSequence;
    for (const std::unique_ptr<Creature>& creature : m_creatures)
    {
        if (state.count >= kMaxCreatures)
        {
            break;
        }
        CreatureSnapshot& entry = state.creatures[state.count++];
        entry.id = creature->NetId();
        entry.seed = creature->Brain().Traits().seed;
        entry.position = creature->Position();
        entry.yaw = creature->Yaw();
        entry.speed = creature->Speed();
        entry.windup = creature->Brain().Intent().windup;
        entry.health = creature->Health() / creature->MaxHealth();
        entry.alive = creature->Alive();
    }
    m_host.SendCreatureState(state);
}

void PredationGame::ApplyCreatureState(const CreatureStateMessage& state)
{
    // Whatever the host no longer has goes. Matched on the seed as well as the number, because the
    // host numbers creatures from nought again each game: the same number with a different seed is a
    // different animal, and has to be built as one.
    std::erase_if(m_creatures,
                  [&](const std::unique_ptr<Creature>& creature)
                  {
                      for (uint8_t i = 0; i < state.count; ++i)
                      {
                          if (state.creatures[i].id == creature->NetId() &&
                              state.creatures[i].seed == creature->Brain().Traits().seed)
                          {
                              return false;
                          }
                      }
                      return true;
                  });

    for (uint8_t i = 0; i < state.count; ++i)
    {
        const CreatureSnapshot& shown = state.creatures[i];
        Creature* creature = nullptr;
        for (const std::unique_ptr<Creature>& existing : m_creatures)
        {
            if (existing->NetId() == shown.id)
            {
                creature = existing.get();
                break;
            }
        }
        if (creature == nullptr)
        {
            // Built from the same seed, so it is the same animal the host has -- which matters more
            // once its body is generated from that seed rather than from boxes.
            m_creatures.push_back(std::make_unique<Creature>(m_scene, m_app->GetMeshes(), m_app->GetPhysics(),
                                                             &m_nav, CreatureTraits::FromSeed(shown.seed),
                                                             shown.position));
            creature = m_creatures.back().get();
            creature->SetNetId(shown.id);
            PRED_LOG_INFO(AI, "Shown the host's creature {} (seed {})", shown.id, shown.seed);
        }
        creature->Receive(shown.position, shown.yaw, shown.speed, shown.windup, shown.health, shown.alive);
    }
}

void PredationGame::DrawCreatureOverlays(DebugDraw& draw)
{
    if (DebugCategories::IsEnabled(DebugCategory::Navigation))
    {
        m_nav.Draw(draw);
    }
    const bool mind = DebugCategories::IsEnabled(DebugCategory::AI) || m_showBrain;
    const bool senses = DebugCategories::IsEnabled(DebugCategory::Perception) || m_showBrain;
    if (!mind && !senses)
    {
        return;
    }
    // A marker is left out when the camera is inside or right beside it. Most of them are about the
    // player -- where it heard them, where it thinks they are -- so in first person they sit on your
    // own head, and a wire sphere drawn round the camera is lines across the whole screen that say
    // nothing.
    // Where the picture is taken from, worked out the way the renderer does it.
    const glm::vec3 viewer =
        m_cameraMode == CameraMode::Fly ? m_camera.position : m_player.View().eyePosition;
    const auto marker = [&](const glm::vec3& centre, float radius, uint32_t colour, int segments)
    {
        if (glm::distance(centre, viewer) > radius + 1.5f)
        {
            draw.Sphere(centre, radius, colour, segments);
        }
    };

    for (const std::unique_ptr<Creature>& creature : m_creatures)
    {
        const CreatureBrain& brain = creature->Brain();
        const glm::vec3 eye = creature->Eye();
        if (senses)
        {
            // The field of view: its two edges and an arc at the range it sees to, level with its eye.
            const float range = 26.0f * brain.Traits().perception;
            const float half = glm::radians(65.0f);
            const float yaw = creature->Yaw();
            const auto along = [&](float angle)
            { return glm::vec3(std::sin(yaw + angle), 0.0f, -std::cos(yaw + angle)); };
            const uint32_t cone = Color::RGBA(240, 200, 90, 150);
            draw.Line(eye, eye + along(-half) * range, cone);
            draw.Line(eye, eye + along(half) * range, cone);
            glm::vec3 previous = eye + along(-half) * range;
            for (int i = 1; i <= 16; ++i)
            {
                const glm::vec3 next = eye + along(-half + 2.0f * half * static_cast<float>(i) / 16.0f) * range;
                draw.Line(previous, next, cone);
                previous = next;
            }

            // Each player it knows of: a line coloured by how much of a sighting it has, and a marker
            // where it thinks they are.
            for (const CreatureBrain::Track& track : brain.Tracks())
            {
                const float e = track.exposure;
                const uint32_t colour = track.visible ? Color::kRed
                                                      : Color::RGBA(static_cast<uint8_t>(90 + 150 * e),
                                                                    static_cast<uint8_t>(200 - 120 * e), 90, 200);
                const glm::vec3 end = track.lastKnown + glm::vec3(0.0f, 1.1f, 0.0f);
                // Not when it ends on the viewer: a line into the camera is a streak across the view.
                if ((e > 0.01f || track.visible) && glm::distance(end, viewer) > 2.0f)
                {
                    draw.Line(eye, end, colour);
                }
                // Only once they are out of sight, which is when "where it thinks they are" and
                // "where they are" can differ. While it can see them the marker is on their feet --
                // and drawn at your own feet it slices through the whole first-person view.
                if (!track.visible && track.confidence > 0.05f)
                {
                    marker(track.lastKnown + glm::vec3(0.0f, 0.1f, 0.0f), 0.15f + 0.2f * track.confidence,
                                Color::kMagenta, 10);
                }
            }

            // What it has heard, as a ring where the sound was, fading.
            for (const CreatureBrain::Heard& heard : brain.RecentNoises())
            {
                const float age = std::clamp((m_creatureClock - heard.time) / 12.0f, 0.0f, 1.0f);
                const auto alpha = static_cast<uint8_t>(220.0f * (1.0f - age));
                marker(heard.noise.position, 0.25f + heard.strength * 0.6f, Color::RGBA(90, 180, 255, alpha), 12);
            }
        }
        if (mind)
        {
            // Where it is going, and why: the route, and what it is interested in.
            const std::vector<glm::vec3>& route = brain.Route();
            glm::vec3 from = creature->Position() + glm::vec3(0.0f, 0.1f, 0.0f);
            for (const glm::vec3& corner : route)
            {
                const glm::vec3 to = corner + glm::vec3(0.0f, 0.1f, 0.0f);
                draw.Line(from, to, Color::kGreen);
                from = to;
            }
            if (!brain.Interest().resolved)
            {
                marker(brain.Interest().position + glm::vec3(0.0f, 0.3f, 0.0f), 0.3f, Color::kCyan, 10);
            }
        }
    }
}

void PredationGame::DrawBrainInspector()
{
#if PRED_DEV_TOOLS
    // Open when asked for with ai_brain, or with the AI category on while the overlay is up.
    const bool byCategory = DebugCategories::IsEnabled(DebugCategory::AI) && m_app->IsOverlayVisible();
    if (!m_showBrain && !byCategory)
    {
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkSize.x - 470.0f, 60.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(460.0f, 640.0f), ImGuiCond_FirstUseEver);
    bool open = true;
    const bool shown = ImGui::Begin("Creature brain", &open);
    if (!open)
    {
        m_showBrain = false;
        DebugCategories::SetEnabled(DebugCategory::AI, false);
    }
    if (!shown)
    {
        ImGui::End();
        return;
    }
    if (!IsAuthority())
    {
        // The copies here have a brain object, but it never runs: showing it would be showing a mind
        // that is not thinking. What this machine does know is what it is being shown.
        ImGui::TextWrapped("The host runs the creatures; their minds are on the host's machine. This one "
                           "is only shown where they are.");
        for (const std::unique_ptr<Creature>& creature : m_creatures)
        {
            ImGui::Text("#%u  seed %u  %s  %.1f m/s", creature->NetId(), creature->Brain().Traits().seed,
                        creature->Alive() ? "alive" : "dead", creature->Speed());
            ImGui::ProgressBar(creature->Health() / creature->MaxHealth(), ImVec2(-1.0f, 0.0f), "health");
        }
        ImGui::End();
        return;
    }
    if (m_creatures.empty())
    {
        ImGui::TextDisabled("No creature. spawn_creature [seed] makes one.");
        ImGui::End();
        return;
    }
    m_inspectedCreature = std::clamp(m_inspectedCreature, 0, static_cast<int>(m_creatures.size()) - 1);
    if (m_creatures.size() > 1)
    {
        ImGui::SliderInt("Creature", &m_inspectedCreature, 0, static_cast<int>(m_creatures.size()) - 1);
    }
    const Creature& creature = *m_creatures[static_cast<size_t>(m_inspectedCreature)];
    const CreatureBrain& brain = creature.Brain();

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", brain.Traits().Describe().c_str());
    ImGui::PopTextWrapPos();
    ImGui::ProgressBar(creature.Health() / creature.MaxHealth(), ImVec2(-1.0f, 0.0f),
                       creature.Alive() ? "health" : "dead");

    ImGui::SeparatorText("Now");
    ImGui::Text("%s", BehaviorName(brain.Current()));
    ImGui::SameLine();
    ImGui::TextDisabled("-- %s", brain.CurrentGoal().c_str());
    const CreatureBrain::State& feelings = brain.Feelings();
    ImGui::ProgressBar(std::min(feelings.pain, 1.0f), ImVec2(140.0f, 0.0f), "pain");
    ImGui::SameLine();
    ImGui::ProgressBar(feelings.fear, ImVec2(140.0f, 0.0f), "fear");
    ImGui::SameLine();
    ImGui::ProgressBar(feelings.arousal, ImVec2(140.0f, 0.0f), "arousal");

    ImGui::SeparatorText("What it weighed");
    // Best first, with every consideration behind each score, because the number alone never says
    // why.
    for (const CreatureBrain::Option& option : brain.Options())
    {
        const bool chosen = option.behavior == brain.Current() && option.target == brain.CurrentTarget();
        char header[160];
        std::snprintf(header, sizeof(header), "%-28s %.2f###%s", option.label.c_str(), option.score,
                      option.label.c_str());
        if (chosen)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.95f, 0.6f, 1.0f));
        }
        const bool expanded = ImGui::TreeNode(header);
        if (chosen)
        {
            ImGui::PopStyleColor();
        }
        if (expanded)
        {
            for (const CreatureBrain::Consideration& c : option.considerations)
            {
                ImGui::Text("%-16s %.2f", c.name, c.value);
            }
            ImGui::TreePop();
        }
    }

    ImGui::SeparatorText("Who it knows about");
    for (const CreatureBrain::Track& track : brain.Tracks())
    {
        ImGui::Text("%s%s", track.name.c_str(), track.visible ? "  (in sight)" : "");
        ImGui::ProgressBar(track.exposure, ImVec2(150.0f, 0.0f), "seen");
        ImGui::SameLine();
        ImGui::ProgressBar(track.confidence, ImVec2(150.0f, 0.0f), "sure where");
        if (track.harm > 0.0f)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1.0f), "hurt it %.0f", track.harm * 100.0f);
        }
    }
    if (!brain.Interest().resolved)
    {
        ImGui::Text("Interested in %s, %.0f%%", brain.Interest().what.c_str(), brain.Interest().strength * 100.0f);
    }

    ImGui::SeparatorText("Timeline");
    // Newest first, with how long ago.
    ImGui::BeginChild("##timeline", ImVec2(0.0f, 180.0f), true);
    const auto& timeline = brain.Timeline();
    for (auto it = timeline.rbegin(); it != timeline.rend(); ++it)
    {
        ImGui::TextDisabled("%5.1fs ago", m_creatureClock - it->time);
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(it->what.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Overlays: the AI and Perception debug categories, or this window open.");
    ImGui::End();
#endif
}

void PredationGame::RegisterCreatureCommands()
{
#if PRED_DEV_TOOLS
    Console& console = m_app->GetConsole();
    console.RegisterCommand(
        "spawn_creature", "Make a creature, far from you, or a few metres in front with 'ahead'",
        [this](const std::vector<std::string>& args)
        {
            if (!IsAuthority())
            {
                m_app->GetConsole().PrintError("Only the host can make a creature.");
                return;
            }
            const uint32_t seed = args.size() >= 2
                                      ? static_cast<uint32_t>(std::strtoul(args[1].c_str(), nullptr, 10))
                                      : static_cast<uint32_t>(std::rand());
            const bool ahead = args.size() >= 3 && args[2] == "ahead";
            const PlayerView& view = m_player.View();
            const glm::vec3 flat = glm::normalize(glm::vec3(view.Forward().x, 0.0f, view.Forward().z) +
                                                  glm::vec3(0.0f, 0.0f, 1e-4f));
            const glm::vec3 inFront = m_player.State().position + flat * 7.0f;
            if (SpawnCreature(seed, m_player.State().position, ahead ? &inFront : nullptr))
            {
                // The one just made is the one somebody wants to watch.
                m_inspectedCreature = static_cast<int>(m_creatures.size()) - 1;
                m_app->GetConsole().Print("Creature " + std::to_string(seed) + ": " +
                                          m_creatures.back()->Brain().Traits().Describe());
            }
            else
            {
                m_app->GetConsole().PrintError("Could not place one: no walkable ground there, or there are already " +
                                               std::to_string(kMaxCreatures) + ".");
            }
        },
        "spawn_creature [seed] [ahead]");
    console.RegisterCommand("creature_clear", "Remove every creature",
                            [this](const std::vector<std::string>&) { ClearCreatures(); });
    console.RegisterCommand("ai_brain", "Show or hide the creature brain inspector",
                            [this](const std::vector<std::string>&) { m_showBrain = !m_showBrain; });
    console.RegisterCommand(
        "creature_hurt", "Hurt the first creature as though you had shot it: creature_hurt [amount]",
        [this](const std::vector<std::string>& args)
        {
            if (m_creatures.empty())
            {
                return;
            }
            const float amount = args.size() >= 2 ? std::strtof(args[1].c_str(), nullptr) : 30.0f;
            m_creatures.front()->TakeDamage(amount, LocalPlayerId(),
                                            m_player.View().eyePosition, m_creatureClock);
        },
        "creature_hurt [amount]");
#endif
}

} // namespace pred
