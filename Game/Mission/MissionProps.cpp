#include "Game/Mission/MissionProps.h"

#include "Engine/Render/Material.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Primitives.h"
#include "Game/Interaction/InteractionSystem.h"
#include "Game/World/Lettering.h"
#include "Game/World/ScreenCanvas.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <utility>

namespace pred
{
namespace
{

const Material kTerminalMaterial = Material::Diffuse({0.16f, 0.17f, 0.18f}, 0.6f);
const Material kPanelMaterial = Material::Metal({0.34f, 0.36f, 0.33f}, 0.5f);
const Material kGlassMaterial = Material::Diffuse({0.03f, 0.035f, 0.04f}, 0.25f);
const Material kPlasticMaterial = Material::Diffuse({0.78f, 0.76f, 0.7f}, 0.55f);
const Material kLeverMaterial = Material::Diffuse({0.05f, 0.05f, 0.055f}, 0.45f);
const Material kRedMaterial = Material::Diffuse({0.62f, 0.07f, 0.04f}, 0.45f);
const Material kYellowMaterial = Material::Diffuse({0.78f, 0.62f, 0.08f}, 0.45f);
const Material kSteelMaterial = Material::Metal({0.62f, 0.62f, 0.64f}, 0.3f);
const Material kCaseMaterial = Material::Diffuse({0.2f, 0.23f, 0.15f}, 0.7f); // a field case's olive paint
const Material kBoxMaterial = Material::Metal({0.16f, 0.2f, 0.25f}, 0.45f); // a supply box's blue-grey enamel
const Material kEnterMaterial = Material::Diffuse({0.06f, 0.3f, 0.12f}, 0.45f); // a keypad's enter key

constexpr glm::vec3 kPanelLamp{0.08f, 0.08f, 0.03f};
const glm::quat kNoTurn{1.0f, 0.0f, 0.0f, 0.0f};

// The board (its own frame: its front out from the wall along -z, the viewer's left +x). Its breakers' middles across it.
constexpr float kBreakerPitch = 0.085f;
constexpr float kBreakerRow = -0.02f;
constexpr float kMainRow = 0.24f;
float BreakerX(int i)
{
    return (2.5f - static_cast<float>(i)) * kBreakerPitch;
}
// The terminal's screen, and a station's switches under it.
constexpr glm::vec2 kScreen{0.44f, 0.26f};
constexpr int kScreenWide = 256;
constexpr int kScreenHigh = 152;
constexpr float kSwitchRow = -0.155f;
float ConsoleX(int k)
{
    return 0.14f - 0.14f * static_cast<float>(k);
}
// The transmitter: its case below the lid (its top this high over the middle), and the panel under the lid.
constexpr float kCaseTop = 0.115f;
constexpr glm::vec2 kCasePanel{0.64f, 0.44f};
constexpr glm::vec3 kSenderAt{0.14f, kCaseTop, -0.06f};
constexpr glm::vec3 kPowerAt{-0.14f, kCaseTop, -0.06f};

const char* const kSystemLabels[MissionRules::kConsoleSystems] = {"1 SCRUBBERS", "2 PUMPS", "3 FANS"};

// A secured terminal's keypad, on the bench at the terminal's right hand (in the terminal's frame): its case tilted towards
// whoever stands at it, and its keys on the case's top in a telephone's rows -- 1 2 3 at the back, then clear, 0, enter.
constexpr glm::vec3 kPadAt{-0.43f, -0.168f, -0.1f};
constexpr float kPadTilt = -0.26f;
constexpr glm::vec3 kPadSize{0.16f, 0.04f, 0.2f};
constexpr float kKeyPitchX = 0.044f;
constexpr float kKeyPitchZ = 0.04f;
// Where a key is on the case's top (its frame: tilted with it), and what is printed on it.
glm::vec3 KeyAt(int key)
{
    int row = 3;
    int column = 1;
    if (key >= 1 && key <= 9)
    {
        row = (key - 1) / 3;
        column = (key - 1) % 3;
    }
    else if (key == MissionRules::kKeyClear)
    {
        column = 0;
    }
    else if (key == MissionRules::kKeyEnter)
    {
        column = 2;
    }
    return {(1.0f - static_cast<float>(column)) * kKeyPitchX, kPadSize.y * 0.5f + 0.006f, (1.5f - static_cast<float>(row)) * kKeyPitchZ - 0.012f};
}
const char* KeyLabel(int key)
{
    static const char* const figures[10] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    return key == MissionRules::kKeyClear ? "CLR" : key == MissionRules::kKeyEnter ? "ENT" : figures[key];
}
// A faulted terminal's supply box (its own frame: its front out from the wall along -z, the viewer's left +x). Its cover over the
// top of it, hinged on its left; under the cover the fuse holders on their rail; along the bottom, the isolator.
constexpr glm::vec3 kCoverSize{0.35f, 0.37f, 0.012f};
constexpr float kCoverMiddle = 0.07f; // (its middle, up the box)
constexpr float kCoverFront = -0.056f;
constexpr float kFuseRow = 0.04f;
constexpr float kFusePitch = 0.066f;
float FuseX(int f)
{
    return (1.5f - static_cast<float>(f)) * kFusePitch;
}
const char* const kFuseLabels[MissionSpec::kFuses] = {"SUPPLY", "SCREEN", "STORE", "LINK"};

void SetVisible(Scene& scene, Entity entity, bool visible)
{
    if (MeshRenderer* renderer = scene.GetMeshRenderer(entity))
    {
        renderer->visible = visible;
    }
}

// Lettering (made facing -z) laid on a thing's top, to be read from its -z side.
const glm::mat4 kLaidFlat = glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));

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
        if (interactable->verb != verb)
        {
            interactable->verb = verb;
        }
        if (interactable->name != name)
        {
            interactable->name = name;
        }
    }
}

// Boxes, each its size and its middle, made into one mesh.
MeshData Boxes(std::initializer_list<std::pair<glm::vec3, glm::vec3>> boxes)
{
    MeshData mesh;
    for (const auto& [size, middle] : boxes)
    {
        mesh.Append(Primitives::Box(size), glm::translate(glm::mat4(1.0f), middle));
    }
    return mesh;
}

// A flat picture, `w` by `h`, to be seen from -z (a screen, a plate on a wall): the top of its picture up, its left the viewer's.
MeshData FrontQuad(float w, float h)
{
    MeshData mesh;
    const glm::vec3 normal{0.0f, 0.0f, -1.0f};
    mesh.vertices.push_back(MeshVertex{{w * 0.5f, h * 0.5f, 0.0f}, normal, {0.0f, 0.0f}});
    mesh.vertices.push_back(MeshVertex{{-w * 0.5f, h * 0.5f, 0.0f}, normal, {1.0f, 0.0f}});
    mesh.vertices.push_back(MeshVertex{{-w * 0.5f, -h * 0.5f, 0.0f}, normal, {1.0f, 1.0f}});
    mesh.vertices.push_back(MeshVertex{{w * 0.5f, -h * 0.5f, 0.0f}, normal, {0.0f, 1.0f}});
    mesh.indices = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
    return mesh;
}

// The same lying flat, face up, to be read standing at its -z side: the top of its picture away from the reader.
MeshData TopQuad(float w, float d)
{
    MeshData mesh;
    const glm::vec3 normal{0.0f, 1.0f, 0.0f};
    mesh.vertices.push_back(MeshVertex{{w * 0.5f, 0.0f, d * 0.5f}, normal, {0.0f, 0.0f}});
    mesh.vertices.push_back(MeshVertex{{-w * 0.5f, 0.0f, d * 0.5f}, normal, {1.0f, 0.0f}});
    mesh.vertices.push_back(MeshVertex{{-w * 0.5f, 0.0f, -d * 0.5f}, normal, {1.0f, 1.0f}});
    mesh.vertices.push_back(MeshVertex{{w * 0.5f, 0.0f, -d * 0.5f}, normal, {0.0f, 1.0f}});
    mesh.indices = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
    return mesh;
}

// A picture on something: lit by the room (a label), or glowing as well (a screen: `glow` times its picture).
Material Pictured(TextureHandle texture, float glow)
{
    Material material = Material::Diffuse(glm::vec3(1.0f), 0.4f);
    material.baseColorTexture = texture;
    material.emissive = glm::vec3(glow);
    material.emissiveTextured = true;
    return material;
}

// Text centred on `x`.
void Centred(ScreenCanvas& canvas, int x, int y, const char* text, Rgb colour, int scale)
{
    canvas.Text(x - canvas.TextWidth(text, scale) / 2, y, text, colour, scale);
}

// The board's plate, behind its switches: what each is, as such boards are labelled.
void DrawBoardPlate(ScreenCanvas& canvas, const glm::vec2& size)
{
    const auto u = [&](float x) { return static_cast<int>((size.x * 0.5f - x) / size.x * static_cast<float>(canvas.width)); };
    const auto v = [&](float y) { return static_cast<int>((size.y * 0.5f - y) / size.y * static_cast<float>(canvas.height)); };
    const Rgb ink{232, 232, 224};
    const Rgb faint{150, 154, 150};
    canvas.Clear({62, 66, 64});
    Centred(canvas, canvas.width / 2, 6, "BOARD DB-1", ink, 2);
    canvas.Line(8.0f, 24.0f, static_cast<float>(canvas.width - 8), 24.0f, faint);
    Centred(canvas, u(0.0f), v(kMainRow - 0.1f), "MAIN", ink, 2);
    canvas.Text(u(-0.075f), v(kMainRow + 0.05f), "ON", faint, 1);
    canvas.Text(u(-0.075f), v(kMainRow - 0.04f), "OFF", faint, 1);
    Centred(canvas, u(-0.2f), v(kMainRow - 0.0f), "SUPPLY", faint, 1);
    for (int i = 0; i < MissionSpec::kBoardBreakers; ++i)
    {
        const char digit[2] = {static_cast<char>('1' + i), 0};
        Centred(canvas, u(BreakerX(i)), v(kBreakerRow - 0.085f), digit, ink, 2);
    }
    // The warning along its foot.
    canvas.Fill(0.0f, static_cast<float>(v(-0.27f)), static_cast<float>(canvas.width), static_cast<float>(v(-0.33f)), {214, 170, 28});
    Centred(canvas, canvas.width / 2, v(-0.27f) + 7, "ISOLATE BEFORE WORK", {20, 18, 10}, 1);
}

// The transmitter's panel, under its lid.
void DrawCasePanel(ScreenCanvas& canvas)
{
    const auto u = [&](float x) { return static_cast<int>((kCasePanel.x * 0.5f - x) / kCasePanel.x * static_cast<float>(canvas.width)); };
    const auto v = [&](float z) { return static_cast<int>((kCasePanel.y * 0.5f - z) / kCasePanel.y * static_cast<float>(canvas.height)); };
    const Rgb ink{226, 226, 214};
    const Rgb faint{140, 144, 136};
    canvas.Clear({36, 39, 34});
    canvas.Box(3.0f, 3.0f, static_cast<float>(canvas.width - 4), static_cast<float>(canvas.height - 4), faint);
    const std::pair<float, const char*> labels[] = {{kSenderAt.x, "TRANSMIT"}, {kPowerAt.x, "POWER"}};
    for (const auto& [x, label] : labels)
    {
        Centred(canvas, u(x), 30, label, ink, 2);
        Centred(canvas, u(x), v(kSenderAt.z + 0.05f) - 9, "ON", faint, 1);
        Centred(canvas, u(x), v(kSenderAt.z - 0.05f) + 3, "OFF", faint, 1);
    }
    Centred(canvas, canvas.width / 2, canvas.height - 18, "SHUT DOWN: TRANSMIT OFF, THEN POWER", {232, 190, 60}, 1);
}

} // namespace

Entity MissionProps::Part(Scene& scene, const char* name, MeshHandle mesh, const Material& material, const glm::vec3& at, const glm::quat& turn,
                          const glm::vec3& local, bool shadow)
{
    Transform transform;
    transform.position = at + turn * local;
    transform.rotation = turn;
    const Entity entity = scene.CreateMeshEntity(name, transform, mesh, material);
    if (!shadow)
    {
        if (MeshRenderer* renderer = scene.GetMeshRenderer(entity))
        {
            renderer->castsShadow = false;
        }
    }
    m_parts.push_back(entity);
    return entity;
}

MissionProps::Moving MissionProps::Mover(Scene& scene, const char* name, MeshHandle mesh, const Material& material, const glm::vec3& at,
                                         const glm::quat& turn, const glm::vec3& pivot, const glm::vec3& axis, float off, float on, float speed)
{
    Moving part;
    part.entity = Part(scene, name, mesh, material, at, turn, pivot);
    part.at = at;
    part.turn = turn;
    part.pivot = pivot;
    part.axis = axis;
    part.off = off;
    part.on = on;
    part.speed = speed;
    return part;
}

void MissionProps::Swing(Scene& scene, Moving& part, bool on, float dt)
{
    SwingTo(scene, part, on ? 1.0f : 0.0f, dt);
}

void MissionProps::SwingTo(Scene& scene, Moving& part, float target, float dt)
{
    if (!part.entity.IsValid())
    {
        return;
    }
    const float was = part.t;
    part.t = part.t < 0.0f ? target : (part.t < target ? std::min(part.t + dt * part.speed, target) : std::max(part.t - dt * part.speed, target));
    if (part.t == was)
    {
        return;
    }
    // Eased at both ends, as a hand moves a thing -- a switch's lever snapping over is quick enough not to show it.
    const float eased = part.t * part.t * (3.0f - 2.0f * part.t);
    if (Transform* transform = scene.GetTransform(part.entity))
    {
        transform->rotation = part.turn * glm::angleAxis(part.off + (part.on - part.off) * eased, part.axis);
        transform->position = part.at + part.turn * part.pivot;
    }
}

void MissionProps::Control(InteractionSystem& interactions, Entity entity, InteractionKind kind, int payload, const glm::vec3& focus, float range,
                           bool closeUp)
{
    Interactable interactable;
    interactable.entity = entity;
    interactable.kind = kind;
    interactable.payload = payload;
    interactable.focusOffset = focus;
    interactable.range = range;
    interactable.enabled = false;
    interactable.closeUp = closeUp;
    interactions.Register(interactable);
    m_controls.push_back(entity);
}

BodyHandle MissionProps::Collider(PhysicsWorld& physics, const glm::vec3& size, const glm::vec3& at, const glm::quat& turn)
{
    Transform transform;
    transform.position = at;
    transform.rotation = turn;
    const BodyHandle body = physics.CreateBox(size * 0.5f, transform, BodyMotion::Static);
    m_colliders.push_back(body);
    return body;
}

void MissionProps::Build(Scene& scene, MeshLibrary& meshes, TextureLibrary* textures, PhysicsWorld& physics, InteractionSystem& interactions,
                         const SitePlan& site, const MissionPlan& plan, const VehicleProp& shuttle)
{
    Clear(scene, physics, interactions);
    m_kind = plan.kind;
    m_lastTime = -1.0f;

    if (plan.building >= 0) // (a terminal: the download's, or a station's console; a survey and a signal have markers instead)
    {
        BuildTerminal(scene, meshes, textures, physics, interactions, plan);
    }
    m_secured = plan.Secured() && m_terminal.body.IsValid();
    if (m_secured)
    {
        BuildKeypad(scene, meshes, physics, interactions, plan);
    }
    if (plan.Faulted() && m_terminal.body.IsValid())
    {
        BuildFuseBox(scene, meshes, physics, interactions, plan);
    }
    if (plan.feedCut)
    {
        BuildCabinet(scene, meshes, physics, interactions, plan);
    }

    // A breaker board on a wall of every building: the one whose power is out to be worked, the others shut.
    for (size_t b = 0; b < site.buildings.size(); ++b)
    {
        glm::vec3 at;
        float yaw = 0.0f;
        if (!BreakerPanel(site.buildings[b], at, yaw))
        {
            continue;
        }
        if (static_cast<int>(b) == plan.building && plan.powerOut)
        {
            BuildBoard(scene, meshes, textures, physics, interactions, at, yaw);
        }
        else
        {
            BuildPanel(scene, meshes, physics, at, yaw);
        }
    }

    // A survey's markers: each a stake on the ground with an instrument head on it, its lamp showing how its reading stands.
    if (plan.kind == MissionPlan::Kind::Survey)
    {
        const glm::vec3 stake{MissionSpec::kMarkerWidth, MissionSpec::kMarkerHeight, MissionSpec::kMarkerWidth};
        const MeshHandle stakeMesh = meshes.Upload(Primitives::Box(stake), "mission_marker");
        const MeshHandle headMesh = meshes.Upload(Primitives::Box({0.3f, 0.12f, 0.3f}), "mission_marker_head");
        for (size_t m = 0; m < plan.markers.size(); ++m)
        {
            const glm::vec3 at = plan.markers[m] + glm::vec3(0.0f, stake.y * 0.5f - 0.05f, 0.0f);
            const Entity body = Part(scene, "mission_marker", stakeMesh, kPanelMaterial, at, kNoTurn, glm::vec3(0.0f));
            Collider(physics, stake, at, kNoTurn);
            m_markers.push_back(body);
            m_markerLamps.push_back(Part(scene, "mission_marker_face", headMesh, kGlassMaterial, at, kNoTurn, {0.0f, stake.y * 0.5f + 0.06f, 0.0f}, false));
            Control(interactions, body, InteractionKind::Terminal, static_cast<int>(m) * 4, {0.0f, stake.y * 0.3f, 0.0f}, 2.4f);
        }
    }
    // A signal's transmitter.
    if (plan.AtTransmitter() && !plan.markers.empty())
    {
        BuildTransmitter(scene, meshes, textures, physics, interactions, plan.markers.front(), plan.seed);
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

void MissionProps::BuildPanel(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, const glm::vec3& at, float yaw)
{
    // A board that is not the one to work: shut, its lamp green.
    const glm::vec3 size = MissionSpec::kBreakerSize;
    const float front = -size.z * 0.5f;
    const MeshHandle shut = meshes.Upload(Boxes({{{size.x, size.y, size.z - 0.016f}, {0.0f, 0.0f, 0.008f}},
                                                 {{size.x, size.y, 0.016f}, {0.0f, 0.0f, front + 0.008f}},
                                                 {{0.022f, 0.11f, 0.03f}, {-size.x * 0.5f + 0.06f, 0.0f, front - 0.015f}}}),
                                          "mission_board_shut");
    const MeshHandle lampMesh = meshes.Upload(Primitives::Box(kPanelLamp), "mission_breaker_lamp");
    const glm::quat turn = ThingTurn(yaw);
    Part(scene, "mission_breaker", shut, kPanelMaterial, at, turn, glm::vec3(0.0f));
    Collider(physics, size, at, turn);
    Material lamp = kGlassMaterial;
    lamp.emissive = {0.1f, 0.7f, 0.2f};
    Part(scene, "mission_breaker_lamp", lampMesh, lamp, at, turn, {0.0f, size.y * 0.5f - 0.1f, front - 0.016f}, false);
}

void MissionProps::BuildBoard(Scene& scene, MeshLibrary& meshes, TextureLibrary* textures, PhysicsWorld& physics, InteractionSystem& interactions,
                              const glm::vec3& at, float yaw)
{
    const glm::vec3 size = MissionSpec::kBreakerSize;
    const glm::quat turn = ThingTurn(yaw);
    const float front = -size.z * 0.5f;
    const float back = size.z * 0.5f - 0.01f; // the face of its back plate, inside
    // Its box, open at the front: the back plate, its sides, its top and its foot.
    const MeshHandle boxMesh = meshes.Upload(Boxes({{{size.x, size.y, 0.01f}, {0.0f, 0.0f, size.z * 0.5f - 0.005f}},
                                                    {{0.012f, size.y, size.z - 0.01f}, {size.x * 0.5f - 0.006f, 0.0f, -0.005f}},
                                                    {{0.012f, size.y, size.z - 0.01f}, {-size.x * 0.5f + 0.006f, 0.0f, -0.005f}},
                                                    {{size.x - 0.024f, 0.012f, size.z - 0.01f}, {0.0f, size.y * 0.5f - 0.006f, -0.005f}},
                                                    {{size.x - 0.024f, 0.012f, size.z - 0.01f}, {0.0f, -size.y * 0.5f + 0.006f, -0.005f}}}),
                                             "mission_board_box");
    m_board.box = Part(scene, "mission_board", boxMesh, kPanelMaterial, at, turn, glm::vec3(0.0f));
    m_board.at = at;
    m_board.turn = turn;
    Control(interactions, m_board.box, InteractionKind::Device, static_cast<int>(Device::Board), {0.0f, 0.0f, front}, 2.2f);
    Collider(physics, size, at, turn);

    // The plate its switches are on, labelled.
    const glm::vec2 plate{size.x - 0.04f, size.y - 0.04f};
    if (!m_boardTexture.IsValid() && textures != nullptr)
    {
        ScreenCanvas canvas(192, 260);
        DrawBoardPlate(canvas, plate);
        m_boardTexture = textures->CreateDynamic(canvas.width, canvas.height, "mission_board_plate");
        textures->Update(m_boardTexture, canvas.image);
    }
    Material plateMaterial = Pictured(m_boardTexture, 0.0f);
    plateMaterial.emissiveTextured = false;
    Part(scene, "mission_board_plate", meshes.Upload(FrontQuad(plate.x, plate.y), "mission_board_plate"), plateMaterial, at, turn,
         {0.0f, 0.0f, back - 0.002f}, false);

    // The main switch: a red handle on its block, up for on and down for off.
    const glm::vec3 mainBlock{0.12f, 0.15f, 0.04f};
    const MeshHandle mainBlockMesh = meshes.Upload(Primitives::Box(mainBlock), "mission_board_main");
    const MeshHandle mainLeverMesh =
        meshes.Upload(Boxes({{{0.022f, 0.075f, 0.022f}, {0.0f, 0.0375f, 0.0f}}, {{0.07f, 0.024f, 0.03f}, {0.0f, 0.075f, 0.0f}}}), "mission_board_main_lever");
    m_board.mainBase = Part(scene, "mission_board_main", mainBlockMesh, kLeverMaterial, at, turn, {0.0f, kMainRow, back - mainBlock.z * 0.5f});
    m_board.main = Mover(scene, "mission_board_main_lever", mainLeverMesh, kRedMaterial, at, turn, {0.0f, kMainRow, back - mainBlock.z},
                         {1.0f, 0.0f, 0.0f}, -2.69f, -0.45f, 6.0f);
    Control(interactions, m_board.mainBase, InteractionKind::Breaker, MissionRules::kBoardMain, {0.0f, 0.0f, -0.05f}, 1.9f, true);
    const MeshHandle lampMesh = meshes.Upload(Primitives::Box({0.024f, 0.024f, 0.01f}), "mission_board_lamp");
    m_board.mainLamp = Part(scene, "mission_board_lamp", lampMesh, kGlassMaterial, at, turn, {-0.2f, kMainRow + 0.03f, back - 0.006f}, false);

    // The breakers: six in a row, each a lever up for on, and a lamp over it lit while its circuit is live.
    const glm::vec3 breaker{0.06f, 0.13f, 0.045f};
    const MeshHandle breakerMesh = meshes.Upload(Primitives::Box(breaker), "mission_board_breaker");
    const MeshHandle leverMesh = meshes.Upload(Boxes({{{0.02f, 0.038f, 0.016f}, {0.0f, 0.019f, 0.0f}}}), "mission_board_breaker_lever");
    const MeshHandle ledMesh = meshes.Upload(Primitives::Box({0.012f, 0.012f, 0.008f}), "mission_board_led");
    for (int i = 0; i < MissionSpec::kBoardBreakers; ++i)
    {
        const size_t s = static_cast<size_t>(i);
        const float x = BreakerX(i);
        m_board.breakers[s] = Part(scene, "mission_board_breaker", breakerMesh, kPlasticMaterial, at, turn, {x, kBreakerRow, back - breaker.z * 0.5f});
        m_board.levers[s] = Mover(scene, "mission_board_breaker_lever", leverMesh, kLeverMaterial, at, turn, {x, kBreakerRow, back - breaker.z},
                                  {1.0f, 0.0f, 0.0f}, -2.59f, -0.55f, 11.0f);
        m_board.leds[s] = Part(scene, "mission_board_led", ledMesh, kGlassMaterial, at, turn, {x, kBreakerRow + 0.09f, back - 0.004f}, false);
        Control(interactions, m_board.breakers[s], InteractionKind::Breaker, MissionRules::kBoardBreaker + i, {0.0f, 0.0f, -0.03f}, 1.9f, true);
    }

    // Its door, hung on the viewer's left and swung out toward them, its handle at the other edge.
    const MeshHandle doorMesh = meshes.Upload(Boxes({{{size.x, size.y, 0.016f}, {-size.x * 0.5f, 0.0f, 0.0f}},
                                                     {{0.022f, 0.11f, 0.03f}, {-size.x + 0.06f, 0.0f, -0.023f}}}),
                                              "mission_board_door");
    m_board.door = Mover(scene, "mission_board_door", doorMesh, kPanelMaterial, at, turn, {size.x * 0.5f, 0.0f, front - 0.008f},
                         {0.0f, 1.0f, 0.0f}, 0.0f, -1.95f, 1.7f);
    Control(interactions, m_board.door.entity, InteractionKind::Breaker, MissionRules::kBoardDoor, {-size.x * 0.75f, 0.0f, -0.02f}, 2.2f, true);
    m_board.built = true;
}

void MissionProps::BuildTerminal(Scene& scene, MeshLibrary& meshes, TextureLibrary* textures, PhysicsWorld& physics, InteractionSystem& interactions,
                                 const MissionPlan& plan)
{
    const glm::vec3 size = MissionSpec::kTerminalSize;
    const glm::quat turn = ThingTurn(plan.terminalYaw);
    const MeshHandle terminalMesh = meshes.Upload(Primitives::Box(size), "mission_terminal");
    m_terminal.body = Part(scene, "mission_terminal", terminalMesh, kTerminalMaterial, plan.terminal, turn, glm::vec3(0.0f));
    Collider(physics, size, plan.terminal, turn);

    // Its screen, drawn into as the mission goes.
    if (!m_screenTexture.IsValid() && textures != nullptr)
    {
        m_screenTexture = textures->CreateDynamic(kScreenWide, kScreenHigh, "mission_terminal_screen");
    }
    m_terminal.screen = Part(scene, "mission_terminal_screen", meshes.Upload(FrontQuad(kScreen.x, kScreen.y), "mission_terminal_screen_quad"),
                             Pictured(m_screenTexture, 0.0f), plan.terminal, turn, {0.0f, 0.045f, -(size.z * 0.5f + 0.003f)}, false);
    m_terminal.drawn.clear();
    m_terminal.at = plan.terminal;
    m_terminal.turn = turn;

    if (plan.AtConsole())
    {
        // A station's console: three switches under the screen, one a system, in the order they are started.
        const MeshHandle plateMesh = meshes.Upload(Primitives::Box({0.07f, 0.05f, 0.008f}), "mission_console_plate");
        const MeshHandle leverMesh = meshes.Upload(Boxes({{{0.014f, 0.032f, 0.014f}, {0.0f, 0.016f, 0.0f}}, {{0.02f, 0.012f, 0.02f}, {0.0f, 0.033f, 0.0f}}}),
                                                   "mission_console_lever");
        const float face = -size.z * 0.5f;
        Control(interactions, m_terminal.body, InteractionKind::Device, static_cast<int>(Device::Console), {0.0f, 0.0f, face}, 2.2f);
        for (int k = 0; k < MissionRules::kConsoleSystems; ++k)
        {
            const size_t s = static_cast<size_t>(k);
            m_terminal.plates[s] = Part(scene, "mission_console_plate", plateMesh, kSteelMaterial, plan.terminal, turn, {ConsoleX(k), kSwitchRow, face - 0.004f});
            m_terminal.levers[s] = Mover(scene, "mission_console_lever", leverMesh, kSteelMaterial, plan.terminal, turn, {ConsoleX(k), kSwitchRow, face - 0.008f},
                                         {1.0f, 0.0f, 0.0f}, -2.64f, -0.5f, 11.0f);
            Control(interactions, m_terminal.plates[s], InteractionKind::Terminal, k + 1, {0.0f, 0.0f, -0.02f}, 1.9f, true);
        }
    }
    else
    {
        // In the terminal's own frame: the focus is turned with it.
        Control(interactions, m_terminal.body, InteractionKind::Terminal, 0, {0.0f, 0.0f, -size.z * 0.5f}, 2.2f);
    }

    // Down a cave, on a stand of its own: a rugged steel case on the chamber's floor, its lid's seam round it, the terminal on top.
    if (plan.cave >= 0)
    {
        const glm::vec3 stand{MissionSpec::kStandTop.x, MissionSpec::kStandHeight, MissionSpec::kStandTop.z};
        const glm::vec3 seam{stand.x + 0.01f, 0.04f, stand.z + 0.01f};
        const MeshHandle caseMesh = meshes.Upload(Primitives::Box(stand), "mission_station_case");
        const MeshHandle seamMesh = meshes.Upload(Primitives::Box(seam), "mission_station_seam");
        const glm::vec3 at = plan.terminal - glm::vec3(0.0f, size.y * 0.5f + stand.y * 0.5f, 0.0f);
        Part(scene, "mission_station", caseMesh, kPanelMaterial, at, turn, glm::vec3(0.0f));
        Collider(physics, stand, at, turn);
        Part(scene, "mission_station_seam", seamMesh, kGlassMaterial, at, turn, {0.0f, stand.y * 0.5f - 0.12f, 0.0f}, false);
    }
}

void MissionProps::BuildKeypad(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, InteractionSystem& interactions, const MissionPlan& plan)
{
    const glm::quat turn = ThingTurn(plan.terminalYaw);
    const glm::vec3 at = plan.terminal + turn * kPadAt;
    const glm::quat padTurn = turn * glm::angleAxis(kPadTilt, glm::vec3(1.0f, 0.0f, 0.0f));
    m_keypad.body = Part(scene, "mission_keypad", meshes.Upload(Primitives::Box(kPadSize), "mission_keypad"), kTerminalMaterial, at, padTurn, glm::vec3(0.0f));
    Collider(physics, kPadSize, at, padTurn);
    Control(interactions, m_keypad.body, InteractionKind::Device, static_cast<int>(Device::Keypad), {0.0f, kPadSize.y * 0.5f, 0.0f}, 2.2f);

    // Its keys, each pressed by hand; their labels printed on them, all one piece.
    const MeshHandle keyMesh = meshes.Upload(Primitives::Box({0.034f, 0.012f, 0.03f}), "mission_keypad_key");
    MeshData labels;
    for (int k = 0; k < MissionRules::kKeys; ++k)
    {
        const glm::vec3 key = KeyAt(k);
        const Material& material = k == MissionRules::kKeyClear ? kRedMaterial : k == MissionRules::kKeyEnter ? kEnterMaterial : kLeverMaterial;
        m_keypad.keys[static_cast<size_t>(k)] = Part(scene, "mission_keypad_key", keyMesh, material, at, padTurn, key, false);
        Control(interactions, m_keypad.keys[static_cast<size_t>(k)], InteractionKind::Terminal, MissionRules::kKeyPayload + k, glm::vec3(0.0f), 1.9f, true);
        const float height = k >= 10 ? 0.0065f : 0.012f;
        labels.Append(Lettering::Mesh(KeyLabel(k), height), glm::translate(glm::mat4(1.0f), key + glm::vec3(0.0f, 0.0072f, 0.0f)) * kLaidFlat);
    }
    Part(scene, "mission_keypad_labels", meshes.Upload(labels, "mission_keypad_labels"), Material::Diffuse({0.82f, 0.82f, 0.78f}, 0.5f), at, padTurn,
         glm::vec3(0.0f), false);
    // Its lamp, at the back: red while it is locked, green once it has let them in.
    m_keypad.lamp = Part(scene, "mission_keypad_lamp", meshes.Upload(Primitives::Box({0.03f, 0.008f, 0.012f}), "mission_keypad_lamp"), kGlassMaterial, at,
                         padTurn, {0.0f, kPadSize.y * 0.5f + 0.003f, kPadSize.z * 0.5f - 0.018f}, false);

    // The note with the code on it: a sheet of paper on another bench, a little askew, the code written large under what it is.
    m_note.turn = ThingTurn(plan.clueYaw + (static_cast<float>(plan.code % 7u) - 3.0f) * 0.07f);
    m_note.at = plan.clue;
    m_note.paper = Part(scene, "mission_note", meshes.Upload(TopQuad(0.15f, 0.105f), "mission_note"), Material::Diffuse({0.86f, 0.85f, 0.8f}, 0.85f),
                        m_note.at, m_note.turn, glm::vec3(0.0f), false);
    Control(interactions, m_note.paper, InteractionKind::Device, static_cast<int>(Device::Note), glm::vec3(0.0f), 2.0f);
    char code[8];
    std::snprintf(code, sizeof(code), "%04u", static_cast<unsigned>(plan.code));
    MeshData writing;
    writing.Append(Lettering::Mesh("ACCESS CODE", 0.009f), glm::translate(glm::mat4(1.0f), {0.0f, 0.0015f, 0.025f}) * kLaidFlat);
    writing.Append(Lettering::Mesh(code, 0.022f), glm::translate(glm::mat4(1.0f), {0.0f, 0.0015f, -0.012f}) * kLaidFlat);
    Part(scene, "mission_note_writing", meshes.Upload(writing, "mission_note_writing"), Material::Diffuse({0.06f, 0.07f, 0.16f}, 0.6f), m_note.at,
         m_note.turn, glm::vec3(0.0f), false);
}

void MissionProps::BuildFuseBox(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, InteractionSystem& interactions, const MissionPlan& plan)
{
    const glm::vec3 size = MissionSpec::kFuseBoxSize;
    const glm::quat turn = ThingTurn(plan.fuseBoxYaw);
    const glm::vec3 at = plan.fuseBox;
    m_fuseBox.at = at;
    m_fuseBox.turn = turn;
    m_fuseBox.built = true;
    const float wall = 0.008f;
    // The box: a steel tray off the wall -- its back, its four sides -- and the plate along its bottom the isolator is on.
    const float stripTop = kCoverMiddle - kCoverSize.y * 0.5f;
    const float strip = stripTop + size.y * 0.5f;
    const MeshHandle trayMesh = meshes.Upload(Boxes({{{size.x, size.y, wall}, {0.0f, 0.0f, size.z * 0.5f - wall * 0.5f}},
                                                     {{wall, size.y, size.z}, {size.x * 0.5f - wall * 0.5f, 0.0f, 0.0f}},
                                                     {{wall, size.y, size.z}, {-size.x * 0.5f + wall * 0.5f, 0.0f, 0.0f}},
                                                     {{size.x, wall, size.z}, {0.0f, size.y * 0.5f - wall * 0.5f, 0.0f}},
                                                     {{size.x, wall, size.z}, {0.0f, -size.y * 0.5f + wall * 0.5f, 0.0f}},
                                                     {{size.x, strip, wall}, {0.0f, -size.y * 0.5f + strip * 0.5f, -size.z * 0.5f + wall * 0.5f}}}),
                                              "mission_fusebox");
    m_fuseBox.body = Part(scene, "mission_fusebox", trayMesh, kBoxMaterial, at, turn, glm::vec3(0.0f));
    Collider(physics, size, at, turn);
    Control(interactions, m_fuseBox.body, InteractionKind::Device, static_cast<int>(Device::FuseBox), {0.0f, 0.0f, -size.z * 0.5f}, 2.2f);

    // Inside: the rail, the fuse holders on it -- each a base and a carrier that swings down open on its foot -- and their labels.
    Part(scene, "mission_fusebox_rail", meshes.Upload(Primitives::Box({0.3f, 0.035f, 0.008f}), "mission_fusebox_rail"), kSteelMaterial, at, turn,
         {0.0f, kFuseRow, 0.03f});
    const MeshHandle baseMesh = meshes.Upload(Primitives::Box({0.05f, 0.11f, 0.03f}), "mission_fusebox_holder");
    const MeshHandle carrierMesh = meshes.Upload(Boxes({{{0.042f, 0.09f, 0.014f}, {0.0f, 0.045f, -0.007f}}, {{0.016f, 0.012f, 0.01f}, {0.0f, 0.082f, -0.018f}}}),
                                                 "mission_fusebox_carrier");
    MeshData labels;
    for (int f = 0; f < MissionSpec::kFuses; ++f)
    {
        const size_t s = static_cast<size_t>(f);
        Part(scene, "mission_fusebox_holder", baseMesh, kPlasticMaterial, at, turn, {FuseX(f), kFuseRow, 0.012f});
        m_fuseBox.carriers[s] = Mover(scene, "mission_fusebox_carrier", carrierMesh, kLeverMaterial, at, turn, {FuseX(f), kFuseRow - 0.045f, -0.003f},
                                      {1.0f, 0.0f, 0.0f}, -1.15f, 0.0f, 7.0f);
        Control(interactions, m_fuseBox.carriers[s].entity, InteractionKind::Terminal, MissionRules::kBoxPayload + MissionRules::kBoxFuse + f,
                {0.0f, 0.0f, 0.0f}, 1.9f, true);
        char label[16];
        std::snprintf(label, sizeof(label), "F%d", f + 1);
        labels.Append(Lettering::Mesh(label, 0.012f), glm::translate(glm::mat4(1.0f), {FuseX(f), kFuseRow + 0.09f, 0.0275f}));
        labels.Append(Lettering::Mesh(kFuseLabels[f], 0.0072f), glm::translate(glm::mat4(1.0f), {FuseX(f), kFuseRow - 0.075f, 0.0275f}));
    }
    // Along the bottom: what it is, and the isolator -- a red handle on a yellow plate, up for on, across for off.
    labels.Append(Lettering::Mesh("TERMINAL SUPPLY", 0.0085f),
                  glm::translate(glm::mat4(1.0f), {0.055f, -size.y * 0.5f + strip * 0.5f, -size.z * 0.5f - 0.0005f}));
    Part(scene, "mission_fusebox_labels", meshes.Upload(labels, "mission_fusebox_labels"), Material::Diffuse({0.85f, 0.85f, 0.8f}, 0.5f), at, turn,
         glm::vec3(0.0f), false);
    const glm::vec3 isolatorAt{-0.11f, -size.y * 0.5f + strip * 0.5f, -size.z * 0.5f - 0.004f};
    Part(scene, "mission_fusebox_isolator_plate", meshes.Upload(Primitives::Box({0.075f, 0.075f, 0.008f}), "mission_fusebox_isolator_plate"), kYellowMaterial,
         at, turn, isolatorAt, false);
    m_fuseBox.isolator = Mover(scene, "mission_fusebox_isolator",
                               meshes.Upload(Boxes({{{0.03f, 0.03f, 0.014f}, {0.0f, 0.0f, -0.007f}}, {{0.016f, 0.066f, 0.016f}, {0.0f, 0.0f, -0.02f}}}),
                                             "mission_fusebox_isolator"),
                               kRedMaterial, at, turn, isolatorAt + glm::vec3(0.0f, 0.0f, -0.004f), {0.0f, 0.0f, 1.0f}, glm::half_pi<float>(), 0.0f, 6.0f);
    Control(interactions, m_fuseBox.isolator.entity, InteractionKind::Terminal, MissionRules::kBoxPayload + MissionRules::kBoxIsolator, {0.0f, 0.0f, 0.0f}, 1.9f,
            true);

    // The cover, on its hinge at its left (the viewer's), swinging out towards whoever opens it; the spare fuse in a clip inside it,
    // going with it; and its four screws.
    const glm::vec3 hinge{kCoverSize.x * 0.5f, kCoverMiddle, kCoverFront};
    m_fuseBox.cover = Mover(scene, "mission_fusebox_cover",
                            meshes.Upload(Boxes({{kCoverSize, {-kCoverSize.x * 0.5f, 0.0f, 0.0f}}}), "mission_fusebox_cover"), kBoxMaterial, at, turn,
                            hinge, {0.0f, 1.0f, 0.0f}, 0.0f, -1.95f, 2.4f);
    MeshData spare = Primitives::Cylinder(0.0075f, 0.04f, 10);
    MeshData spareFuse;
    spareFuse.Append(spare, glm::translate(glm::mat4(1.0f), {-kCoverSize.x * 0.72f, -0.12f, kCoverSize.z * 0.5f + 0.008f}) *
                                glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f)));
    m_fuseBox.spare = Mover(scene, "mission_fusebox_spare", meshes.Upload(spareFuse, "mission_fusebox_spare"), kPlasticMaterial, at, turn, hinge,
                            {0.0f, 1.0f, 0.0f}, 0.0f, -1.95f, 2.4f);
    const MeshHandle screwMesh = meshes.Upload(Primitives::Box({0.013f, 0.013f, 0.006f}), "mission_fusebox_screw");
    for (int s = 0; s < 4; ++s)
    {
        const glm::vec3 screw{(s % 2 == 0 ? 1.0f : -1.0f) * (kCoverSize.x * 0.5f - 0.02f), kCoverMiddle + (s < 2 ? 1.0f : -1.0f) * (kCoverSize.y * 0.5f - 0.02f),
                              kCoverFront - kCoverSize.z * 0.5f - 0.003f};
        m_fuseBox.screws[static_cast<size_t>(s)] = Part(scene, "mission_fusebox_screw", screwMesh, kSteelMaterial, at, turn, screw, false);
        Control(interactions, m_fuseBox.screws[static_cast<size_t>(s)], InteractionKind::Terminal, MissionRules::kBoxPayload + MissionRules::kBoxScrew + s,
                {0.0f, 0.0f, 0.0f}, 1.9f, true);
    }
}

void MissionProps::ShowFuseBox(Scene& scene, InteractionSystem& interactions, const MissionState& state, float dt)
{
    const bool open = MissionRules::BoxOpen(state);
    const bool workable = Workable(Device::FuseBox, state);
    Swing(scene, m_fuseBox.cover, open, dt);
    Swing(scene, m_fuseBox.spare, open, dt);
    SetVisible(scene, m_fuseBox.spare.entity, !state.boxSpared);
    for (int s = 0; s < 4; ++s)
    {
        const bool out = ((state.boxScrews >> s) & 1u) != 0u;
        SetVisible(scene, m_fuseBox.screws[static_cast<size_t>(s)], !out);
        interactions.SetEnabled(m_fuseBox.screws[static_cast<size_t>(s)], workable && !out);
    }
    Swing(scene, m_fuseBox.isolator, state.boxIsolator, dt);
    interactions.SetEnabled(m_fuseBox.isolator.entity, workable);
    for (int f = 0; f < MissionSpec::kFuses; ++f)
    {
        const size_t s = static_cast<size_t>(f);
        Swing(scene, m_fuseBox.carriers[s], ((state.boxFuses >> f) & 1u) != 0u, dt);
        interactions.SetEnabled(m_fuseBox.carriers[s].entity, workable && open);
    }
    Offer(interactions, m_fuseBox.body, workable, "Use", "supply box");
}

void MissionProps::BuildCabinet(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, InteractionSystem& interactions, const MissionPlan& plan)
{
    // Its own frame: its foot on the ground, its front out along -z, the viewer's left +x. A concrete plinth; on it a steel
    // cabinet -- its back, sides, top (a little over) and bottom -- and inside, the panel its switches are on.
    const glm::vec3 size = MissionSpec::kCabinetSize;
    const float plinth = MissionSpec::kCabinetPlinth;
    const glm::quat turn = ThingTurn(plan.cabinetYaw);
    const glm::vec3 at = plan.cabinet;
    m_cabinet.at = at;
    m_cabinet.turn = turn;
    m_cabinet.built = true;
    const Material enamel = Material::Metal({0.16f, 0.25f, 0.19f}, 0.55f); // (a feeder pillar's green)
    const float wall = 0.012f;
    const float middle = plinth + size.y * 0.5f;
    Part(scene, "mission_cabinet_plinth", meshes.Upload(Primitives::Box({size.x + 0.1f, plinth + 0.2f, size.z + 0.1f}), "mission_cabinet_plinth"),
         Material::Diffuse({0.42f, 0.41f, 0.39f}, 0.9f), at, turn, {0.0f, plinth * 0.5f - 0.1f, 0.0f});
    m_cabinet.body = Part(scene, "mission_cabinet",
                          meshes.Upload(Boxes({{{size.x, size.y, wall}, {0.0f, middle, size.z * 0.5f - wall * 0.5f}},
                                               {{wall, size.y, size.z}, {size.x * 0.5f - wall * 0.5f, middle, 0.0f}},
                                               {{wall, size.y, size.z}, {-size.x * 0.5f + wall * 0.5f, middle, 0.0f}},
                                               {{size.x + 0.04f, 0.03f, size.z + 0.05f}, {0.0f, plinth + size.y + 0.015f, -0.01f}},
                                               {{size.x, wall, size.z}, {0.0f, plinth + wall * 0.5f, 0.0f}}}),
                                        "mission_cabinet"),
                          enamel, at, turn, glm::vec3(0.0f));
    const float panel = size.z * 0.5f - 0.06f; // (the panel's face, inside)
    // Solid from its back to just behind its panel, so its switches are not behind its own collision.
    const float solid = size.z * 0.5f - panel;
    Collider(physics, {size.x, size.y + plinth, solid}, at + turn * glm::vec3(0.0f, (size.y + plinth) * 0.5f, size.z * 0.5f - solid * 0.5f), turn);
    Control(interactions, m_cabinet.body, InteractionKind::Device, static_cast<int>(Device::Cabinet), {0.0f, middle, -size.z * 0.5f}, 2.4f);
    Part(scene, "mission_cabinet_panel", meshes.Upload(Primitives::Box({size.x - 0.06f, size.y - 0.1f, 0.01f}), "mission_cabinet_panel"), kPanelMaterial, at,
         turn, {0.0f, middle, panel + 0.005f});

    // The switches: each feed's on its own yellow plate, a red handle up for on and across for off; the changeover between them,
    // a black handle on a white plate turned from the main (A, to the viewer's left) through off (0, up) to the standby (B).
    const float row = middle + 0.12f;
    const MeshHandle plateMesh = meshes.Upload(Primitives::Box({0.12f, 0.12f, 0.01f}), "mission_cabinet_plate");
    const MeshHandle handleMesh =
        meshes.Upload(Boxes({{{0.04f, 0.04f, 0.02f}, {0.0f, 0.0f, -0.01f}}, {{0.024f, 0.1f, 0.024f}, {0.0f, 0.0f, -0.03f}}}), "mission_cabinet_handle");
    const glm::vec3 mainAt{0.25f, row, panel - 0.005f};
    const glm::vec3 standbyAt{-0.25f, row, panel - 0.005f};
    const glm::vec3 changeAt{0.0f, row, panel - 0.005f};
    Part(scene, "mission_cabinet_plate", plateMesh, kYellowMaterial, at, turn, mainAt, false);
    Part(scene, "mission_cabinet_plate", plateMesh, kYellowMaterial, at, turn, standbyAt, false);
    Part(scene, "mission_cabinet_plate", meshes.Upload(Primitives::Box({0.16f, 0.16f, 0.01f}), "mission_cabinet_change_plate"), kPlasticMaterial, at, turn,
         changeAt, false);
    m_cabinet.main = Mover(scene, "mission_cabinet_main", handleMesh, kRedMaterial, at, turn, mainAt + glm::vec3(0.0f, 0.0f, -0.005f), {0.0f, 0.0f, 1.0f},
                           glm::half_pi<float>(), 0.0f, 6.0f);
    m_cabinet.standby = Mover(scene, "mission_cabinet_standby", handleMesh, kRedMaterial, at, turn, standbyAt + glm::vec3(0.0f, 0.0f, -0.005f),
                              {0.0f, 0.0f, 1.0f}, glm::half_pi<float>(), 0.0f, 6.0f);
    m_cabinet.changeover = Mover(scene, "mission_cabinet_changeover",
                                 meshes.Upload(Boxes({{{0.05f, 0.05f, 0.022f}, {0.0f, 0.0f, -0.011f}}, {{0.026f, 0.075f, 0.026f}, {0.0f, 0.03f, -0.035f}}}),
                                               "mission_cabinet_changeover"),
                                 kLeverMaterial, at, turn, changeAt + glm::vec3(0.0f, 0.0f, -0.005f), {0.0f, 0.0f, 1.0f}, -0.9f, 0.9f, 4.0f);
    Control(interactions, m_cabinet.main.entity, InteractionKind::Terminal, MissionRules::kCabinetPayload + MissionRules::kCabinetMain, glm::vec3(0.0f), 2.0f,
            true);
    Control(interactions, m_cabinet.standby.entity, InteractionKind::Terminal, MissionRules::kCabinetPayload + MissionRules::kCabinetStandby, glm::vec3(0.0f),
            2.0f, true);
    Control(interactions, m_cabinet.changeover.entity, InteractionKind::Terminal, MissionRules::kCabinetPayload + MissionRules::kCabinetChangeover,
            glm::vec3(0.0f), 2.0f, true);
    // Each feed's lamp over its switch, and the building's over the changeover.
    const MeshHandle lampMesh = meshes.Upload(Primitives::Box({0.03f, 0.03f, 0.012f}), "mission_cabinet_lamp");
    m_cabinet.mainLamp = Part(scene, "mission_cabinet_lamp", lampMesh, kGlassMaterial, at, turn, mainAt + glm::vec3(0.0f, 0.13f, -0.004f), false);
    m_cabinet.standbyLamp = Part(scene, "mission_cabinet_lamp", lampMesh, kGlassMaterial, at, turn, standbyAt + glm::vec3(0.0f, 0.13f, -0.004f), false);
    m_cabinet.busLamp = Part(scene, "mission_cabinet_lamp", lampMesh, kGlassMaterial, at, turn, changeAt + glm::vec3(0.0f, 0.15f, -0.004f), false);
    // What each is.
    MeshData words;
    const auto word = [&](const char* text, float height, const glm::vec3& where)
    { words.Append(Lettering::Mesh(text, height), glm::translate(glm::mat4(1.0f), where)); };
    word("A  MAIN", 0.022f, mainAt + glm::vec3(0.0f, -0.11f, -0.001f));
    word("B  STANDBY", 0.022f, standbyAt + glm::vec3(0.0f, -0.11f, -0.001f));
    word("SUPPLY", 0.022f, changeAt + glm::vec3(0.0f, -0.13f, -0.001f));
    word("A", 0.018f, changeAt + glm::vec3(0.062f, 0.052f, -0.0065f));
    word("0", 0.018f, changeAt + glm::vec3(0.0f, 0.066f, -0.0065f));
    word("B", 0.018f, changeAt + glm::vec3(-0.062f, 0.052f, -0.0065f));
    Part(scene, "mission_cabinet_words", meshes.Upload(words, "mission_cabinet_words"), kLeverMaterial, at, turn, glm::vec3(0.0f), false);

    // The door, on its hinge at its left (the viewer's), with its warning plate on it.
    const glm::vec3 hinge{size.x * 0.5f, middle, -size.z * 0.5f - 0.01f};
    const glm::vec3 doorSize{size.x - 0.01f, size.y - 0.02f, 0.02f};
    m_cabinet.door = Mover(scene, "mission_cabinet_door",
                           meshes.Upload(Boxes({{doorSize, {-doorSize.x * 0.5f, 0.0f, 0.0f}}, {{0.03f, 0.12f, 0.03f}, {-doorSize.x + 0.06f, 0.0f, -0.02f}}}),
                                         "mission_cabinet_door"),
                           enamel, at, turn, hinge, {0.0f, 1.0f, 0.0f}, 0.0f, -1.9f, 2.0f);
    m_cabinet.doorPlate = Mover(scene, "mission_cabinet_door_plate", meshes.Upload(Boxes({{{0.32f, 0.14f, 0.004f}, {-doorSize.x * 0.5f, 0.32f, -0.012f}}}),
                                                                                        "mission_cabinet_door_plate"),
                                kYellowMaterial, at, turn, hinge, {0.0f, 1.0f, 0.0f}, 0.0f, -1.9f, 2.0f);
    MeshData danger;
    danger.Append(Lettering::Mesh("DANGER", 0.04f), glm::translate(glm::mat4(1.0f), {-doorSize.x * 0.5f, 0.345f, -0.0145f}));
    danger.Append(Lettering::Mesh("400 VOLTS", 0.03f), glm::translate(glm::mat4(1.0f), {-doorSize.x * 0.5f, 0.285f, -0.0145f}));
    m_cabinet.doorWords = Mover(scene, "mission_cabinet_door_words", meshes.Upload(danger, "mission_cabinet_door_words"), kLeverMaterial, at, turn, hinge,
                                {0.0f, 1.0f, 0.0f}, 0.0f, -1.9f, 2.0f);
    Control(interactions, m_cabinet.door.entity, InteractionKind::Terminal, MissionRules::kCabinetPayload + MissionRules::kCabinetDoor,
            {-doorSize.x + 0.06f, 0.0f, -0.02f}, 2.0f, true);
}

void MissionProps::ShowCabinet(Scene& scene, InteractionSystem& interactions, const MissionState& state, float dt)
{
    const bool workable = Workable(Device::Cabinet, state);
    Swing(scene, m_cabinet.door, state.cabinetOpen, dt);
    Swing(scene, m_cabinet.doorPlate, state.cabinetOpen, dt);
    Swing(scene, m_cabinet.doorWords, state.cabinetOpen, dt);
    Swing(scene, m_cabinet.main, state.feedMain, dt);
    Swing(scene, m_cabinet.standby, state.feedStandby, dt);
    SwingTo(scene, m_cabinet.changeover, static_cast<float>(state.changeover) * 0.5f, dt);
    // The main feed's lamp dark (its cable cut), the standby's lit; the building's lit once it is fed.
    SetGlow(scene, m_cabinet.mainLamp, glm::vec3(0.0f));
    SetGlow(scene, m_cabinet.standbyLamp, glm::vec3(0.75f, 0.5f, 0.08f));
    SetGlow(scene, m_cabinet.busLamp, state.fed ? glm::vec3(0.1f, 0.85f, 0.25f) : glm::vec3(0.0f));
    interactions.SetEnabled(m_cabinet.door.entity, workable && !state.cabinetOpen);
    interactions.SetEnabled(m_cabinet.main.entity, workable && state.cabinetOpen);
    interactions.SetEnabled(m_cabinet.standby.entity, workable && state.cabinetOpen);
    interactions.SetEnabled(m_cabinet.changeover.entity, workable && state.cabinetOpen);
    Offer(interactions, m_cabinet.body, workable, "Use", "feeder cabinet");
}

void MissionProps::BuildTransmitter(Scene& scene, MeshLibrary& meshes, TextureLibrary* textures, PhysicsWorld& physics, InteractionSystem& interactions,
                                    const glm::vec3& foot, uint32_t seed)
{
    const glm::vec3 box = MissionSpec::kTransmitterSize;
    // (Turned by its seed, so it does not always sit square to the world.)
    const glm::quat turn = ThingTurn(static_cast<float>((seed * 2654435761u) >> 20) * 0.0015f);
    const glm::vec3 at = foot + glm::vec3(0.0f, box.y * 0.5f - 0.04f, 0.0f);
    const float bottom = -box.y * 0.5f;
    const glm::vec3 body{box.x, kCaseTop - bottom, box.z};
    const glm::vec3 bodyMiddle{0.0f, (kCaseTop + bottom) * 0.5f, 0.0f};
    m_transmitter.body = Part(scene, "mission_transmitter", meshes.Upload(Primitives::Box(body), "mission_transmitter_case"), kCaseMaterial, at, turn, bodyMiddle);
    m_transmitter.at = at;
    m_transmitter.turn = turn;
    Control(interactions, m_transmitter.body, InteractionKind::Device, static_cast<int>(Device::Transmitter), {0.0f, body.y * 0.5f, 0.0f}, 2.4f);
    Collider(physics, body, at + turn * bodyMiddle, turn);

    // Its panel, under the lid: two switches and their lamps.
    if (!m_caseTexture.IsValid() && textures != nullptr)
    {
        ScreenCanvas canvas(256, 176);
        DrawCasePanel(canvas);
        m_caseTexture = textures->CreateDynamic(canvas.width, canvas.height, "mission_transmitter_panel");
        textures->Update(m_caseTexture, canvas.image);
    }
    Material panel = Pictured(m_caseTexture, 0.0f);
    panel.emissiveTextured = false;
    Part(scene, "mission_transmitter_panel", meshes.Upload(TopQuad(kCasePanel.x, kCasePanel.y), "mission_transmitter_panel"), panel, at, turn,
         {0.0f, kCaseTop + 0.001f, 0.0f}, false);
    const MeshHandle baseMesh = meshes.Upload(Primitives::Box({0.08f, 0.012f, 0.08f}), "mission_transmitter_switch");
    const MeshHandle leverMesh = meshes.Upload(Boxes({{{0.018f, 0.065f, 0.018f}, {0.0f, 0.0325f, 0.0f}}, {{0.03f, 0.02f, 0.03f}, {0.0f, 0.065f, 0.0f}}}),
                                               "mission_transmitter_lever");
    const MeshHandle ledMesh = meshes.Upload(Primitives::Box({0.022f, 0.01f, 0.022f}), "mission_transmitter_led");
    m_transmitter.senderBase = Part(scene, "mission_transmitter_switch", baseMesh, kSteelMaterial, at, turn, kSenderAt + glm::vec3(0.0f, 0.006f, 0.0f));
    m_transmitter.powerBase = Part(scene, "mission_transmitter_switch", baseMesh, kSteelMaterial, at, turn, kPowerAt + glm::vec3(0.0f, 0.006f, 0.0f));
    // (Thrown away from whoever stands at it for on, toward them for off.)
    m_transmitter.sender = Mover(scene, "mission_transmitter_lever", leverMesh, kYellowMaterial, at, turn, kSenderAt + glm::vec3(0.0f, 0.012f, 0.0f),
                                 {1.0f, 0.0f, 0.0f}, -0.5f, 0.5f, 9.0f);
    m_transmitter.power = Mover(scene, "mission_transmitter_lever", leverMesh, kRedMaterial, at, turn, kPowerAt + glm::vec3(0.0f, 0.012f, 0.0f),
                                {1.0f, 0.0f, 0.0f}, -0.5f, 0.5f, 9.0f);
    m_transmitter.senderLed = Part(scene, "mission_transmitter_led", ledMesh, kGlassMaterial, at, turn, {kSenderAt.x, kCaseTop + 0.004f, 0.065f}, false);
    m_transmitter.powerLed = Part(scene, "mission_transmitter_led", ledMesh, kGlassMaterial, at, turn, {kPowerAt.x, kCaseTop + 0.004f, 0.065f}, false);
    Control(interactions, m_transmitter.senderBase, InteractionKind::Terminal, MissionRules::kCaseSender, {0.0f, 0.04f, 0.0f}, 1.9f, true);
    Control(interactions, m_transmitter.powerBase, InteractionKind::Terminal, MissionRules::kCasePower, {0.0f, 0.04f, 0.0f}, 1.9f, true);

    // Its lid, hung along the back and lifted over, its latches at the front.
    const float lidHigh = box.y * 0.5f - kCaseTop;
    const MeshHandle lidMesh = meshes.Upload(Boxes({{{box.x, lidHigh, box.z}, {0.0f, lidHigh * 0.5f, -box.z * 0.5f}},
                                                    {{0.06f, 0.05f, 0.02f}, {box.x * 0.31f, 0.01f, -box.z - 0.01f}},
                                                    {{0.06f, 0.05f, 0.02f}, {-box.x * 0.31f, 0.01f, -box.z - 0.01f}}}),
                                             "mission_transmitter_lid");
    m_transmitter.lid = Mover(scene, "mission_transmitter_lid", lidMesh, kCaseMaterial, at, turn, {0.0f, kCaseTop, box.z * 0.5f}, {1.0f, 0.0f, 0.0f},
                              0.0f, 1.9f, 1.4f);
    Control(interactions, m_transmitter.lid.entity, InteractionKind::Terminal, MissionRules::kCaseLid, {0.0f, lidHigh, -box.z * 0.85f}, 2.4f, true);

    // The whip aerial up from a mount on its side, and the beacon lamp round its foot.
    const glm::vec3 mount{-box.x * 0.5f - 0.03f, 0.0f, box.z * 0.3f};
    Part(scene, "mission_transmitter_mount", meshes.Upload(Primitives::Box({0.06f, 0.1f, 0.06f}), "mission_transmitter_mount"), kSteelMaterial, at, turn, mount);
    Part(scene, "mission_transmitter_aerial", meshes.Upload(Primitives::Box({0.025f, MissionSpec::kAerialHeight, 0.025f}), "mission_transmitter_aerial"),
         kSteelMaterial, at, turn, mount + glm::vec3(0.0f, 0.05f + MissionSpec::kAerialHeight * 0.5f, 0.0f));
    m_transmitter.beacon = Part(scene, "mission_transmitter_lamp", meshes.Upload(Primitives::Box({0.07f, 0.06f, 0.07f}), "mission_transmitter_lamp"),
                                kGlassMaterial, at, turn, mount + glm::vec3(0.0f, 0.08f, 0.0f), false);
    m_transmitter.built = true;
}

void MissionProps::Clear(Scene& scene, PhysicsWorld& physics, InteractionSystem& interactions)
{
    for (const Entity entity : m_controls)
    {
        interactions.Unregister(entity);
    }
    m_controls.clear();
    for (const Entity entity : m_parts)
    {
        scene.Destroy(entity);
    }
    m_parts.clear();
    for (const BodyHandle body : m_colliders)
    {
        physics.DestroyBody(body);
    }
    m_colliders.clear();
    m_board = Board{};
    m_terminal = Terminal{};
    m_transmitter = Transmitter{};
    m_keypad = Keypad{};
    m_note = Note{};
    m_fuseBox = FuseBox{};
    m_cabinet = Cabinet{};
    m_secured = false;
    m_hovered = Entity{};
    m_markers.clear();
    m_markerLamps.clear();
    if (m_console.IsValid())
    {
        interactions.Unregister(m_console);
    }
    m_console = Entity{};
    m_consoleScreen = Entity{};
}

void MissionProps::Show(Scene& scene, InteractionSystem& interactions, TextureLibrary* textures, const MissionState& state, float time)
{
    using Stage = MissionState::Stage;
    const float dt = m_lastTime < 0.0f ? 0.0f : std::clamp(time - m_lastTime, 0.0f, 0.1f);
    m_lastTime = time;
    const bool live = state.stage != Stage::None && state.stage != Stage::Over;

    if (m_board.built)
    {
        ShowBoard(scene, interactions, state, time, dt);
    }
    if (m_terminal.body.IsValid())
    {
        ShowTerminal(scene, interactions, textures, state, time, dt);
    }
    if (m_transmitter.built)
    {
        ShowTransmitter(scene, interactions, state, time, dt);
    }
    if (m_fuseBox.built)
    {
        ShowFuseBox(scene, interactions, state, dt);
    }
    if (m_cabinet.built)
    {
        ShowCabinet(scene, interactions, state, dt);
    }

    // A survey's markers: blue and steady waiting for a reading, amber and flickering while one is taken (dimmer while nobody is at
    // it), green once taken. Each offered only while its reading is still to be taken and nothing else is being read.
    for (size_t m = 0; m < m_markers.size(); ++m)
    {
        const bool taken = ((state.readings >> m) & 1u) != 0u;
        const bool reading = state.reading == static_cast<int>(m) && state.stage == Stage::Downloading;
        const glm::vec3 glow = taken     ? glm::vec3(0.1f, 0.7f, 0.25f)
                               : reading ? glm::vec3(0.8f, 0.55f, 0.1f) * (state.attended ? 0.75f + 0.25f * std::sin(time * 14.0f) : 0.4f)
                                         : glm::vec3(0.12f, 0.35f, 0.75f) * (0.75f + 0.25f * std::sin(time * 2.0f + static_cast<float>(m)));
        SetGlow(scene, m_markerLamps[m], live || taken ? glow : glm::vec3(0.0f));
        Offer(interactions, m_markers[m], !taken && state.stage == Stage::Find, "Take reading", "");
    }

    // The shuttle's console: steady, or counting down in red.
    if (m_console.IsValid())
    {
        const glm::vec3 glow = state.Launching() ? glm::vec3(0.8f, 0.1f, 0.06f) * (0.6f + 0.4f * std::abs(std::sin(time * 3.2f)))
                                                 : glm::vec3(0.05f, 0.1f, 0.16f);
        SetGlow(scene, m_consoleScreen, live ? glow : glm::vec3(0.02f));
        m_consoleLive = live;
        Offer(interactions, m_console, live, state.Launching() ? "Hold departure" : "Leave site", "");
    }
}

void MissionProps::ShowBoard(Scene& scene, InteractionSystem& interactions, const MissionState& state, float time, float dt)
{
    // Worked until the power is back; then left as it was left -- its door open, a faulted circuit's breaker off.
    const bool workable = MissionRules::CanRestorePower(state);
    Swing(scene, m_board.door, state.boardOpen, dt);
    Swing(scene, m_board.main, state.boardMain, dt);
    for (int i = 0; i < MissionSpec::kBoardBreakers; ++i)
    {
        const size_t s = static_cast<size_t>(i);
        const bool on = ((state.boardOn >> i) & 1u) != 0u;
        Swing(scene, m_board.levers[s], on, dt);
        SetGlow(scene, m_board.leds[s], MissionRules::BreakerLive(state, i) ? glm::vec3(0.1f, 0.9f, 0.25f) : glm::vec3(0.0f));
        interactions.SetEnabled(m_board.breakers[s], workable && state.boardOpen);
    }
    // The supply lamp: green with the main switch on, red without -- slowly pulsing.
    SetGlow(scene, m_board.mainLamp,
            state.boardMain ? glm::vec3(0.1f, 0.85f, 0.25f) : glm::vec3(0.95f, 0.08f, 0.05f) * (0.6f + 0.4f * std::sin(time * 4.0f)));
    interactions.SetEnabled(m_board.mainBase, workable && state.boardOpen);
    interactions.SetEnabled(m_board.door.entity, workable && !state.boardOpen);
    Offer(interactions, m_board.box, workable, "Use", "breaker board");
}

void MissionProps::ShowTerminal(Scene& scene, InteractionSystem& interactions, TextureLibrary* textures, const MissionState& state, float time,
                                float dt)
{
    using Stage = MissionState::Stage;
    const bool live = state.stage != Stage::None && state.stage != Stage::Over;
    // The screen: dark without power, lit with it, and drawn into whenever what it shows changes.
    const bool lit = state.powered && state.supplied && state.stage != Stage::None;
    if (MissionPlan::AtConsole(m_kind))
    {
        const bool workable = state.stage == Stage::Find || state.stage == Stage::Downloading;
        for (int k = 0; k < MissionRules::kConsoleSystems; ++k)
        {
            const size_t s = static_cast<size_t>(k);
            const bool on = ((state.device >> k) & 1u) != 0u;
            Swing(scene, m_terminal.levers[s], on, dt);
            interactions.SetEnabled(m_terminal.plates[s], workable);
        }
        Offer(interactions, m_terminal.body, workable, "Use", "console");
    }
    else if (!state.powered || !state.supplied)
    {
        Offer(interactions, m_terminal.body, live, "Use", "(no power)");
    }
    else if (m_secured && !state.unlocked)
    {
        Offer(interactions, m_terminal.body, state.stage == Stage::Find, "Use", "terminal (secured)");
    }
    else
    {
        Offer(interactions, m_terminal.body, state.stage == Stage::Find, "Download", "");
    }
    if (m_secured)
    {
        // Locked and lit, pressing the terminal is coming up to its keypad; otherwise it is the terminal's own.
        const bool locked = state.powered && state.supplied && !state.unlocked;
        if (Interactable* interactable = interactions.Find(m_terminal.body))
        {
            interactable->kind = locked ? InteractionKind::Device : InteractionKind::Terminal;
            interactable->payload = locked ? static_cast<int>(Device::Keypad) : 0;
        }
        const bool keys = Workable(Device::Keypad, state);
        for (const Entity key : m_keypad.keys)
        {
            interactions.SetEnabled(key, keys);
        }
        Offer(interactions, m_keypad.body, keys, "Use", "keypad");
        SetGlow(scene, m_keypad.lamp, !lit ? glm::vec3(0.0f)
                                      : state.unlocked ? glm::vec3(0.1f, 0.85f, 0.25f)
                                                       : glm::vec3(0.9f, 0.08f, 0.05f) * (0.65f + 0.35f * std::sin(time * 3.0f)));
        Offer(interactions, m_note.paper, live, "Look at", "note");
    }
    SetGlow(scene, m_terminal.screen, glm::vec3(lit ? 1.15f : 0.0f));
    DrawScreen(textures, state, time);
}

void MissionProps::DrawScreen(TextureLibrary* textures, const MissionState& state, float time)
{
    using Stage = MissionState::Stage;
    if (textures == nullptr || !m_screenTexture.IsValid())
    {
        return;
    }
    const bool restore = MissionPlan::AtConsole(m_kind);
    const bool lit = state.powered && state.supplied && state.stage != Stage::None;
    const bool tripped = restore && (state.device & MissionState::kConsoleTripped) != 0;
    const bool lost = !restore && state.stage == Stage::Downloading && !state.attended;
    const int blink = (tripped || lost) && std::fmod(time, 1.0f) < 0.5f ? 1 : 0;
    const int amount = static_cast<int>(state.progress * (restore ? 300.0f : 100.0f));
    char key[96];
    std::snprintf(key, sizeof(key), "%d %d %d %d %d %d %d %d", lit ? 1 : 0, static_cast<int>(state.stage), state.attended ? 1 : 0, amount,
                  static_cast<int>(state.device), blink, static_cast<int>(state.typedCount), state.unlocked ? 1 : 0);
    if (m_terminal.drawn == key)
    {
        return;
    }
    m_terminal.drawn = key;

    const Rgb green{96, 236, 150};
    const Rgb dim{36, 92, 60};
    const Rgb amber{255, 176, 64};
    const Rgb red{255, 72, 52};
    ScreenCanvas canvas(kScreenWide, kScreenHigh);
    if (!lit)
    {
        canvas.Clear({3, 4, 4});
        textures->Update(m_screenTexture, canvas.image);
        return;
    }
    canvas.Clear({5, 13, 10});
    const int w = kScreenWide;
    const auto bar = [&](int y, float fill, Rgb colour)
    {
        canvas.Box(10.0f, static_cast<float>(y), static_cast<float>(w - 10), static_cast<float>(y + 9), dim);
        canvas.Fill(12.0f, static_cast<float>(y + 2), 12.0f + (static_cast<float>(w) - 24.0f) * std::clamp(fill, 0.0f, 1.0f), static_cast<float>(y + 7),
                    colour);
    };
    if (restore)
    {
        canvas.Text(10, 8, "LIFE SUPPORT", green, 2);
        canvas.Line(8.0f, 26.0f, static_cast<float>(w - 8), 26.0f, dim);
        for (int k = 0; k < MissionRules::kConsoleSystems; ++k)
        {
            const int y = 33 + k * 30;
            const bool on = ((state.device >> k) & 1u) != 0u;
            const float pressure = std::clamp(state.progress * 3.0f - static_cast<float>(k), 0.0f, 1.0f);
            canvas.Text(10, y, kSystemLabels[k], on ? green : dim, 2);
            const char* status = !on ? "OFF" : pressure >= 1.0f ? "UP" : "RUN";
            canvas.Text(w - 10 - canvas.TextWidth(status, 2), y, status, !on ? dim : pressure >= 1.0f ? green : amber, 2);
            bar(y + 16, pressure, pressure >= 1.0f ? green : amber);
        }
        const unsigned systems = state.device & 7u;
        const char* line = "START 1, 2, 3 IN TURN";
        Rgb colour = dim;
        if (state.stage == Stage::Carry || state.stage == Stage::Over)
        {
            line = "LIFE SUPPORT ONLINE";
            colour = green;
        }
        else if (tripped)
        {
            line = blink != 0 ? "TRIPPED - START AGAIN" : "";
            colour = red;
        }
        else if (systems != 0 && !state.attended)
        {
            line = "STAND BY THE CONSOLE";
            colour = amber;
        }
        else if (systems != 0 && state.progress * 3.0f >= static_cast<float>(std::popcount(systems)) - 1e-3f)
        {
            line = "AT PRESSURE - NEXT";
            colour = green;
        }
        else if (systems != 0)
        {
            line = "PRESSURISING";
            colour = amber;
        }
        Centred(canvas, w / 2, kScreenHigh - 16, line, colour, 1);
    }
    else
    {
        canvas.Text(10, 8, "DATA TERMINAL", green, 2);
        canvas.Line(8.0f, 26.0f, static_cast<float>(w - 8), 26.0f, dim);
        switch (state.stage)
        {
        case Stage::Find:
            if (m_secured && !state.unlocked)
            {
                // Secured: a box for each figure of the code, filled as they are typed.
                Centred(canvas, w / 2, 36, "SECURED", amber, 3);
                Centred(canvas, w / 2, 66, "ENTER ACCESS CODE", dim, 1);
                for (int f = 0; f < 4; ++f)
                {
                    const float x = static_cast<float>(w / 2 - 70 + f * 36);
                    canvas.Box(x, 86.0f, x + 32.0f, 122.0f, f < state.typedCount ? amber : dim);
                    if (f < state.typedCount)
                    {
                        canvas.Fill(x + 10.0f, 98.0f, x + 22.0f, 110.0f, amber);
                    }
                }
                Centred(canvas, w / 2, 134, "THEN ENT", dim, 1);
                break;
            }
            Centred(canvas, w / 2, 50, "READY", green, 4);
            Centred(canvas, w / 2, 100, "PRESS TO COPY", dim, 2);
            break;
        case Stage::Downloading:
        {
            char percent[16];
            std::snprintf(percent, sizeof(percent), "%d%%", std::min(amount, 99));
            Centred(canvas, w / 2, 36, lost ? "LINK LOST" : "COPYING", lost ? red : amber, 3);
            Centred(canvas, w / 2, 70, percent, lost ? dim : amber, 3);
            bar(102, state.progress, lost ? dim : amber);
            if (lost && blink != 0)
            {
                Centred(canvas, w / 2, 124, "STAY AT THE TERMINAL", red, 1);
            }
            break;
        }
        case Stage::Carry:
        case Stage::Over:
            Centred(canvas, w / 2, 44, "COPY", green, 3);
            Centred(canvas, w / 2, 72, "COMPLETE", green, 3);
            Centred(canvas, w / 2, 112, "TAKE THE DRIVE", dim, 2);
            break;
        case Stage::None: break;
        }
    }
    canvas.Lines();
    textures->Update(m_screenTexture, canvas.image);
}

void MissionProps::ShowTransmitter(Scene& scene, InteractionSystem& interactions, const MissionState& state, float time, float dt)
{
    using Stage = MissionState::Stage;
    const bool open = (state.device & MissionState::kLidOpen) != 0;
    const bool powered = (state.device & MissionState::kPowerOff) == 0 && state.stage != Stage::None;
    // (Sending once it is on the air again too, brought back: MissionPlan::Kind::Revive.)
    const bool sending = powered && (state.device & MissionState::kSenderOff) == 0 && state.stage != Stage::None;
    Swing(scene, m_transmitter.lid, open, dt);
    Swing(scene, m_transmitter.sender, (state.device & MissionState::kSenderOff) == 0, dt);
    Swing(scene, m_transmitter.power, (state.device & MissionState::kPowerOff) == 0, dt);
    // The beacon: a double flash of red while it sends, as a beacon's; steady amber with only its power on; dark once off.
    const float beat = std::fmod(time, 1.6f);
    const glm::vec3 beacon = sending ? glm::vec3(0.95f, 0.1f, 0.06f) * (beat < 0.12f || (beat > 0.26f && beat < 0.38f) ? 1.0f : 0.06f)
                             : powered ? glm::vec3(0.7f, 0.42f, 0.08f)
                                       : glm::vec3(0.0f);
    SetGlow(scene, m_transmitter.beacon, beacon);
    SetGlow(scene, m_transmitter.senderLed, sending ? glm::vec3(0.9f, 0.55f, 0.08f) * (0.6f + 0.4f * std::sin(time * 23.0f)) : glm::vec3(0.0f));
    SetGlow(scene, m_transmitter.powerLed, powered ? glm::vec3(0.1f, 0.85f, 0.25f) : glm::vec3(0.0f));
    const bool workable = state.stage == Stage::Find;
    interactions.SetEnabled(m_transmitter.lid.entity, workable && !open);
    interactions.SetEnabled(m_transmitter.senderBase, workable && open);
    interactions.SetEnabled(m_transmitter.powerBase, workable && open);
    Offer(interactions, m_transmitter.body, workable, "Use", "transmitter");
}

bool MissionProps::View(Device device, glm::vec3& eye, glm::vec3& target) const
{
    // Each from where somebody working it would have their head: square on to the board, a little over the console's switches,
    // and over the case looking down into it.
    switch (device)
    {
    case Device::Board:
        if (!m_board.box.IsValid())
        {
            return false;
        }
        target = m_board.at + m_board.turn * glm::vec3(0.0f, 0.0f, 0.0f);
        eye = m_board.at + m_board.turn * glm::vec3(0.0f, 0.02f, -0.82f);
        return true;
    case Device::Console:
        if (!m_terminal.body.IsValid() || !MissionPlan::AtConsole(m_kind))
        {
            return false;
        }
        target = m_terminal.at + m_terminal.turn * glm::vec3(0.0f, -0.04f, -0.18f);
        eye = m_terminal.at + m_terminal.turn * glm::vec3(0.0f, 0.2f, -0.78f);
        return true;
    case Device::Transmitter:
        if (!m_transmitter.body.IsValid())
        {
            return false;
        }
        target = m_transmitter.at + m_transmitter.turn * glm::vec3(0.0f, kCaseTop, -0.02f);
        eye = m_transmitter.at + m_transmitter.turn * glm::vec3(0.0f, 0.72f, -0.5f);
        return true;
    case Device::Keypad:
        if (!m_keypad.body.IsValid())
        {
            return false;
        }
        // Over the keypad, the terminal's screen in sight beside it.
        target = m_terminal.at + m_terminal.turn * glm::vec3(-0.35f, -0.12f, -0.12f);
        eye = m_terminal.at + m_terminal.turn * glm::vec3(-0.36f, 0.17f, -0.5f);
        return true;
    case Device::Cabinet:
        if (!m_cabinet.built)
        {
            return false;
        }
        // Standing in front of it, looking in at its switches.
        target = m_cabinet.at + m_cabinet.turn * glm::vec3(0.0f, MissionSpec::kCabinetPlinth + MissionSpec::kCabinetSize.y * 0.56f, 0.0f);
        eye = m_cabinet.at + m_cabinet.turn * glm::vec3(0.0f, 1.45f, -0.95f);
        return true;
    case Device::FuseBox:
        if (!m_fuseBox.built)
        {
            return false;
        }
        // Square on to it, its isolator and its holders both in sight.
        target = m_fuseBox.at + m_fuseBox.turn * glm::vec3(0.0f, -0.03f, 0.0f);
        eye = m_fuseBox.at + m_fuseBox.turn * glm::vec3(-0.03f, 0.05f, -0.55f);
        return true;
    case Device::Note:
        if (!m_note.paper.IsValid())
        {
            return false;
        }
        target = m_note.at;
        eye = m_note.at + m_note.turn * glm::vec3(0.0f, 0.36f, -0.16f);
        return true;
    case Device::None:
    case Device::ShipTerminal: break;
    }
    return false;
}

MissionProps::Device MissionProps::DeviceOf(Entity entity) const
{
    if (!entity.IsValid())
    {
        return Device::None;
    }
    if (m_board.box.IsValid() &&
        (entity == m_board.door.entity || entity == m_board.mainBase ||
         std::find(m_board.breakers.begin(), m_board.breakers.end(), entity) != m_board.breakers.end()))
    {
        return Device::Board;
    }
    if (m_terminal.body.IsValid() && std::find(m_terminal.plates.begin(), m_terminal.plates.end(), entity) != m_terminal.plates.end())
    {
        return Device::Console;
    }
    if (m_transmitter.body.IsValid() &&
        (entity == m_transmitter.lid.entity || entity == m_transmitter.senderBase || entity == m_transmitter.powerBase))
    {
        return Device::Transmitter;
    }
    if (m_keypad.body.IsValid() && std::find(m_keypad.keys.begin(), m_keypad.keys.end(), entity) != m_keypad.keys.end())
    {
        return Device::Keypad;
    }
    if (m_cabinet.built && (entity == m_cabinet.door.entity || entity == m_cabinet.main.entity || entity == m_cabinet.standby.entity ||
                            entity == m_cabinet.changeover.entity))
    {
        return Device::Cabinet;
    }
    if (m_fuseBox.built)
    {
        const auto carrier = [&](const Moving& part) { return part.entity == entity; };
        if (entity == m_fuseBox.isolator.entity || std::find(m_fuseBox.screws.begin(), m_fuseBox.screws.end(), entity) != m_fuseBox.screws.end() ||
            std::any_of(m_fuseBox.carriers.begin(), m_fuseBox.carriers.end(), carrier))
        {
            return Device::FuseBox;
        }
    }
    return Device::None;
}

bool MissionProps::Workable(Device device, const MissionState& state) const
{
    switch (device)
    {
    case Device::Board: return m_board.box.IsValid() && MissionRules::CanRestorePower(state);
    case Device::Console:
        return MissionPlan::AtConsole(m_kind) && state.supplied && (state.stage == MissionState::Stage::Find || state.stage == MissionState::Stage::Downloading);
    case Device::Transmitter: return m_transmitter.body.IsValid() && state.stage == MissionState::Stage::Find;
    case Device::Keypad: return m_secured && !state.unlocked && state.powered && state.supplied && state.stage == MissionState::Stage::Find;
    case Device::FuseBox: return m_fuseBox.built && !state.supplied && state.stage == MissionState::Stage::Find;
    case Device::Cabinet:
        return m_cabinet.built && !state.fed && state.stage != MissionState::Stage::None && state.stage != MissionState::Stage::Over;
    case Device::Note: return m_note.paper.IsValid() && state.stage != MissionState::Stage::None && state.stage != MissionState::Stage::Over;
    case Device::None:
    case Device::ShipTerminal: break;
    }
    return false;
}

void MissionProps::Hover(Scene& scene, Entity entity)
{
    if (entity == m_hovered)
    {
        return;
    }
    if (m_hovered.IsValid())
    {
        SetGlow(scene, m_hovered, glm::vec3(0.0f));
    }
    m_hovered = entity;
    if (m_hovered.IsValid())
    {
        SetGlow(scene, m_hovered, glm::vec3(0.16f, 0.12f, 0.04f));
    }
}

void MissionProps::Reach(InteractionSystem& interactions, bool aboard)
{
    if (m_console.IsValid())
    {
        if (Interactable* interactable = interactions.Find(m_console))
        {
            interactable->enabled = m_consoleLive && aboard;
        }
    }
}

} // namespace pred
