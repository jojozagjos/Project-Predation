#include "Game/Mission/MissionProps.h"

#include "Engine/Render/Material.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Primitives.h"
#include "Game/Interaction/InteractionSystem.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace pred
{
namespace
{

const Material kTerminalMaterial = Material::Diffuse({0.16f, 0.17f, 0.18f}, 0.6f);
const Material kPanelMaterial = Material::Metal({0.34f, 0.36f, 0.33f}, 0.5f);
const Material kGlassMaterial = Material::Diffuse({0.03f, 0.035f, 0.04f}, 0.25f);

constexpr glm::vec3 kScreen{0.44f, 0.28f, 0.01f};
constexpr glm::vec3 kPanelLamp{0.08f, 0.08f, 0.03f};

glm::quat ThingTurn(float yaw)
{
    // As a thing is turned: its own -z out to (-sin, 0, -cos).
    return glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
}

void SetGlow(Scene& scene, Entity entity, const glm::vec3& glow)
{
    if (MeshRenderer* renderer = scene.GetMeshRenderer(entity))
    {
        renderer->material.emissive = glow;
    }
}

void Offer(InteractionSystem& interactions, Entity entity, bool enabled, const char* verb, const char* name)
{
    if (Interactable* interactable = interactions.Find(entity))
    {
        interactable->enabled = enabled;
        interactable->verb = verb;
        interactable->name = name;
    }
}

} // namespace

MissionProps::Prop MissionProps::Make(Scene& scene, PhysicsWorld& physics, const char* name, MeshHandle bodyMesh,
                                      const Material& bodyMaterial, const glm::vec3& size, const glm::vec3& at, const glm::quat& turn,
                                      MeshHandle faceMesh, const glm::vec3& faceAt)
{
    Prop prop;
    Transform transform;
    transform.position = at;
    transform.rotation = turn;
    prop.body = scene.CreateMeshEntity(name, transform, bodyMesh, bodyMaterial);
    prop.collider = physics.CreateBox(size * 0.5f, transform, BodyMotion::Static);
    Transform face = transform;
    face.position = at + turn * faceAt;
    prop.face = scene.CreateMeshEntity(std::string(name) + "_face", face, faceMesh, kGlassMaterial);
    if (MeshRenderer* renderer = scene.GetMeshRenderer(prop.face))
    {
        renderer->castsShadow = false;
    }
    return prop;
}

void MissionProps::Build(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, InteractionSystem& interactions, const SitePlan& site,
                         const MissionPlan& plan, const VehicleProp& shuttle)
{
    Clear(scene, physics, interactions);
    m_building = plan.building;
    const MeshHandle terminalMesh = meshes.Upload(Primitives::Box(MissionSpec::kTerminalSize), "mission_terminal");
    const MeshHandle screenMesh = meshes.Upload(Primitives::Box(kScreen), "mission_terminal_screen");
    const MeshHandle panelMesh = meshes.Upload(Primitives::Box(MissionSpec::kBreakerSize), "mission_breaker");
    const MeshHandle lampMesh = meshes.Upload(Primitives::Box(kPanelLamp), "mission_breaker_lamp");

    if (plan.Valid())
    {
        const glm::vec3 size = MissionSpec::kTerminalSize;
        m_terminal = Make(scene, physics, "mission_terminal", terminalMesh, kTerminalMaterial, size, plan.terminal, ThingTurn(plan.terminalYaw),
                          screenMesh, {0.0f, 0.03f, -(size.z * 0.5f + 0.006f)});
        Interactable interactable;
        interactable.entity = m_terminal.body;
        interactable.kind = InteractionKind::Terminal;
        // In the terminal's own frame: the focus is turned with it. Turned here as well, it was turned twice, and a
        // terminal facing the other way had its focus on its back -- behind its own front, which hid it: no prompt.
        interactable.focusOffset = glm::vec3(0.0f, 0.0f, -size.z * 0.5f);
        interactable.range = 2.2f;
        interactions.Register(interactable);
    }

    for (size_t b = 0; b < site.buildings.size(); ++b)
    {
        glm::vec3 at;
        float yaw = 0.0f;
        if (!BreakerPanel(site.buildings[b], at, yaw))
        {
            continue;
        }
        const glm::vec3 size = MissionSpec::kBreakerSize;
        Prop panel = Make(scene, physics, "mission_breaker", panelMesh, kPanelMaterial, size, at, ThingTurn(yaw), lampMesh,
                          {0.0f, size.y * 0.5f - 0.1f, -(size.z * 0.5f + 0.016f)});
        panel.building = static_cast<int>(b);
        Interactable interactable;
        interactable.entity = panel.body;
        interactable.kind = InteractionKind::Breaker;
        interactable.payload = static_cast<int>(b);
        interactable.focusOffset = glm::vec3(0.0f, 0.0f, -size.z * 0.5f);
        interactable.range = 2.2f;
        interactions.Register(interactable);
        m_breakers.push_back(panel);
    }

    // The shuttle's console, at the front of its cabin, to press.
    m_console = shuttle.Part("console");
    m_consoleScreen = shuttle.Part("fx_console_screen");
    if (plan.Valid() && m_console.IsValid())
    {
        Interactable interactable;
        interactable.entity = m_console;
        interactable.kind = InteractionKind::Launch;
        interactable.focusOffset = glm::vec3(0.0f, 0.475f, 0.0f);
        interactable.range = 2.2f;
        interactions.Register(interactable);
    }
    else
    {
        m_console = Entity{};
    }
}

void MissionProps::Clear(Scene& scene, PhysicsWorld& physics, InteractionSystem& interactions)
{
    const auto remove = [&](Prop& prop)
    {
        if (prop.body.IsValid())
        {
            interactions.Unregister(prop.body);
            scene.Destroy(prop.body);
        }
        if (prop.face.IsValid())
        {
            scene.Destroy(prop.face);
        }
        if (prop.collider.IsValid())
        {
            physics.DestroyBody(prop.collider);
        }
        prop = Prop{};
    };
    remove(m_terminal);
    for (Prop& panel : m_breakers)
    {
        remove(panel);
    }
    m_breakers.clear();
    if (m_console.IsValid())
    {
        interactions.Unregister(m_console);
    }
    m_console = Entity{};
    m_consoleScreen = Entity{};
    m_building = -1;
}

void MissionProps::Show(Scene& scene, InteractionSystem& interactions, const MissionState& state, float time)
{
    using Stage = MissionState::Stage;
    const bool live = state.stage != Stage::None && state.stage != Stage::Over;

    // The terminal: dark without power; lit and waiting; working, with a flicker of activity while it copies and a
    // dimmer, stalled look while nobody is with it; and done.
    if (m_terminal.body.IsValid())
    {
        glm::vec3 glow{0.0f};
        if (state.powered)
        {
            switch (state.stage)
            {
            case Stage::Find: glow = {0.1f, 0.35f, 0.22f}; break;
            case Stage::Downloading:
                glow = state.attended ? glm::vec3(0.55f, 0.42f, 0.12f) * (0.8f + 0.2f * std::sin(time * 17.0f))
                                      : glm::vec3(0.3f, 0.22f, 0.06f);
                break;
            case Stage::Carry:
            case Stage::Over: glow = {0.12f, 0.5f, 0.3f}; break;
            case Stage::None: break;
            }
        }
        SetGlow(scene, m_terminal.face, glow);
        if (!state.powered)
        {
            Offer(interactions, m_terminal.body, live, "Use", "terminal (no power)");
        }
        else
        {
            Offer(interactions, m_terminal.body, state.stage == Stage::Find, "Download", "the data");
        }
    }

    // Every building's panel: green with power, red without. Only the one that is out has anything to do.
    for (const Prop& panel : m_breakers)
    {
        const bool out = panel.building == m_building && !state.powered;
        SetGlow(scene, panel.face, out ? glm::vec3(0.9f, 0.08f, 0.05f) * (0.75f + 0.25f * std::sin(time * 5.0f)) : glm::vec3(0.1f, 0.7f, 0.2f));
        Offer(interactions, panel.body, out && live, "Reset", "breaker");
    }

    // The shuttle's console: steady, or counting down in red.
    if (m_console.IsValid())
    {
        const glm::vec3 glow = state.Launching() ? glm::vec3(0.8f, 0.1f, 0.06f) * (0.6f + 0.4f * std::abs(std::sin(time * 3.2f)))
                                                 : glm::vec3(0.12f, 0.25f, 0.4f);
        SetGlow(scene, m_consoleScreen, live ? glow : glm::vec3(0.02f));
        Offer(interactions, m_console, live, state.Launching() ? "Hold" : "Leave", state.Launching() ? "the departure" : "the site");
    }
}

} // namespace pred
