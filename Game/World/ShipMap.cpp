#include "Game/World/ShipMap.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/Primitives.h"
#include "Game/World/LevelLights.h"
#include "Game/World/MapBuilder.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/vector_relational.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{
namespace
{

using namespace ShipSpec;

// Worn industrial: painted steel, grating underfoot, bare metal where hands and boots have been.
const Material kWall = Material::Diffuse({0.30f, 0.33f, 0.33f}, 0.8f);
const Material kWallDark = Material::Diffuse({0.22f, 0.24f, 0.25f}, 0.85f);
const Material kDeckPlate = Material::Diffuse({0.16f, 0.17f, 0.18f}, 0.9f);
const Material kCeiling = Material::Diffuse({0.21f, 0.22f, 0.23f}, 0.9f);
const Material kBayWall = Material::Diffuse({0.36f, 0.37f, 0.36f}, 0.8f);
const Material kBayDeck = Material::Diffuse({0.20f, 0.21f, 0.21f}, 0.9f);
const Material kFrame = Material::Metal({0.14f, 0.14f, 0.15f}, 0.55f);
const Material kRib = Material::Metal({0.24f, 0.25f, 0.26f}, 0.5f);
const Material kHazardYellow = Material::Diffuse({0.72f, 0.55f, 0.10f}, 0.7f);
const Material kPipe = Material::Metal({0.40f, 0.40f, 0.41f}, 0.45f);
const Material kCrate = Material::Diffuse({0.30f, 0.34f, 0.26f}, 0.85f);
const Material kCrateDark = Material::Diffuse({0.20f, 0.22f, 0.24f}, 0.85f);
const Material kFurniture = Material::Metal({0.28f, 0.29f, 0.31f}, 0.6f);
const Material kTableTop = Material::Diffuse({0.46f, 0.45f, 0.42f}, 0.6f);
const Material kCushion = Material::Diffuse({0.22f, 0.25f, 0.30f}, 0.95f);
const Material kMattress = Material::Diffuse({0.52f, 0.52f, 0.49f}, 0.95f);
const Material kHullDark = Material::Metal({0.17f, 0.18f, 0.19f}, 0.55f);

Material Glow(const glm::vec3& colour, float strength)
{
    Material material = Material::Diffuse(colour, 0.4f);
    material.emissive = colour * strength;
    return material;
}

const Material kScreen = Glow({0.06f, 0.16f, 0.26f}, 0.45f);
const Material kScreenWarm = Glow({0.30f, 0.20f, 0.08f}, 0.45f);
const Material kIndicator = Glow({0.2f, 0.9f, 0.4f}, 2.0f);
const Material kReactorGlow = Glow({0.35f, 0.6f, 1.0f}, 1.6f);

// The ship's frame to the world's.
Transform At(const glm::vec3& local, float yawDegrees = 0.0f)
{
    Transform transform;
    transform.position = kOrigin + local;
    transform.rotation = glm::angleAxis(glm::radians(yawDegrees), glm::vec3(0.0f, 1.0f, 0.0f));
    return transform;
}

CinePose Pose(const glm::vec3& local, float yawDegrees = 0.0f)
{
    return {kOrigin + local, TurnFromDegrees({0.0f, yawDegrees, 0.0f})};
}

// A gap in a wall: from and to along it, and its bottom and top.
struct Opening
{
    float from = 0.0f;
    float to = 0.0f;
    float bottom = 0.0f;
    float top = 0.0f;
};

// Everything the ship is built of, in its own frame, into one structure: its pieces are meant to meet.
class Builder
{
public:
    Builder(MapBuilder& map, PhysicsWorld& physics, uint32_t group, std::vector<BodyHandle>& bodies)
        : m_map(map), m_physics(physics), m_group(group), m_bodies(bodies)
    {
    }

    // Solid and drawn, between two corners.
    void Solid(const char* name, const glm::vec3& lo, const glm::vec3& hi, const Material& material)
    {
        m_map.AddBox(name, At((lo + hi) * 0.5f), glm::abs(hi - lo), material);
    }
    // Drawn only: what is out of reach, or thin on a wall.
    void Shape(const char* name, const glm::vec3& lo, const glm::vec3& hi, const Material& material)
    {
        m_map.AddMesh(name, At((lo + hi) * 0.5f), Primitives::Box(glm::abs(hi - lo)), material, false);
    }
    // Drawn only, and casting no shadow: paint, a screen, a lit panel -- thin things laid on a surface.
    void Decal(const char* name, const glm::vec3& lo, const glm::vec3& hi, const Material& material)
    {
        m_map.SetShadows(false);
        Shape(name, lo, hi, material);
        m_map.SetShadows(true);
    }
    void ShapeTurned(const char* name, const glm::vec3& centre, const glm::vec3& size, float yawDegrees, const Material& material)
    {
        m_map.AddMesh(name, At(centre, yawDegrees), Primitives::Box(size), material, false);
    }
    void SolidTurned(const char* name, const glm::vec3& centre, const glm::vec3& size, float yawDegrees, const Material& material)
    {
        m_map.AddBox(name, At(centre, yawDegrees), size, material);
    }
    // Solid only: the glass in a window, which is not drawn.
    void Barrier(const glm::vec3& lo, const glm::vec3& hi)
    {
        const BodyHandle body = m_physics.CreateBox(glm::abs(hi - lo) * 0.5f, At((lo + hi) * 0.5f), BodyMotion::Static);
        m_physics.SetOverlapGroup(body, m_group);
        m_bodies.push_back(body);
    }

    // A wall running across the ship (along x) between z0 and z1, and one running along it (along z) between x0 and x1,
    // with gaps in it: doorways from the floor, windows with a sill.
    void WallX(const char* name, float x0, float x1, float z0, float z1, float y0, float y1, std::vector<Opening> gaps, const Material& material)
    {
        Wall(name, x0, x1, y0, y1, std::move(gaps), material, [&](float a, float b, float lo, float hi)
             { return std::pair{glm::vec3(a, lo, z0), glm::vec3(b, hi, z1)}; });
    }
    void WallZ(const char* name, float z0, float z1, float x0, float x1, float y0, float y1, std::vector<Opening> gaps, const Material& material)
    {
        Wall(name, z0, z1, y0, y1, std::move(gaps), material, [&](float a, float b, float lo, float hi)
             { return std::pair{glm::vec3(x0, lo, a), glm::vec3(x1, hi, b)}; });
    }

private:
    template <typename Corners>
    void Wall(const char* name, float from, float to, float y0, float y1, std::vector<Opening> gaps, const Material& material, Corners corners)
    {
        std::sort(gaps.begin(), gaps.end(), [](const Opening& a, const Opening& b) { return a.from < b.from; });
        const auto piece = [&](float a, float b, float lo, float hi)
        {
            if (b - a < 0.005f || hi - lo < 0.005f)
            {
                return;
            }
            const auto [low, high] = corners(a, b, lo, hi);
            Solid(name, low, high, material);
        };
        float at = from;
        for (const Opening& gap : gaps)
        {
            piece(at, gap.from, y0, y1);
            piece(gap.from, gap.to, y0, gap.bottom);
            piece(gap.from, gap.to, gap.top, y1);
            at = gap.to;
        }
        piece(at, to, y0, y1);
    }

    MapBuilder& m_map;
    PhysicsWorld& m_physics;
    uint32_t m_group = 0;
    std::vector<BodyHandle>& m_bodies;
};

// --- The layout, in the ship's frame ------------------------------------------------------------------------------
//
// Inside faces of walls, along the spine (z, bow at -z) and across it (x). Walls are kWall thick, outside these faces.
constexpr float kTop = 2.7f;      // the crew sections' ceilings
constexpr float kRoof = 3.0f;     // and the top of the roof over them
constexpr float kBayTop = 5.6f;   // the bay's ceiling
constexpr float kBayRoof = 5.95f;
constexpr float kSlab = 0.5f;     // the floor's thickness, under y = 0
constexpr float kWallT = 0.25f;
constexpr float kDoorTop = 2.2f;
constexpr float kDoorHalf = 0.7f;
constexpr float kHalf = 3.4f;     // the crew sections, inside, either side of the middle
constexpr float kBayHalf = 5.4f;  // the bay
constexpr float kNose = -17.0f;      // the cockpit's front wall
constexpr float kOpsFront = -11.5f;  // cockpit | ops room
constexpr float kCrewFront = -4.0f;  // ops room | crew section
constexpr float kBayFront = 3.5f;    // crew section | bay
constexpr float kBayBack = 19.5f;    // bay | engine room
constexpr float kStern = 24.5f;      // the engine room's back wall
// The corridor down the crew section, between the bunks and the gear room.
constexpr float kCorridor = 0.8f;
constexpr float kPartition = 0.1f;
constexpr float kSideDoorFrom = -1.6f;
constexpr float kSideDoorTo = -0.4f;
// The bay doors in the bay's floor (BayDoorsModel: nine metres across, fourteen long).
constexpr float kDoorsHalfX = 4.5f;
constexpr float kDoorsFront = 4.5f;
constexpr float kDoorsBack = 18.5f;
// The cockpit's windows: ahead, and either side.
constexpr Opening kWindscreen{-2.4f, 2.4f, 0.95f, 2.15f};
constexpr Opening kSideWindow{-16.3f, -13.0f, 1.0f, 2.1f};
// The two screens on the ops room's forward wall, either side of the door.
constexpr float kScreenX = 2.05f;
constexpr float kScreenWidth = 2.4f;
constexpr float kScreenY = 1.55f;

void BuildStructure(Builder& b)
{
    const float outer = kHalf + kWallT;
    const float bayOuter = kBayHalf + kWallT;
    // --- Floors and roofs ---------------------------------------------------------------------------------------
    b.Solid("ship_deck", {-outer, -kSlab, kNose - kWallT}, {outer, 0.0f, kBayFront - kWallT}, kDeckPlate);
    b.Solid("ship_deck", {-bayOuter, -kSlab, kBayFront - kWallT}, {bayOuter, 0.0f, kDoorsFront}, kBayDeck);
    b.Solid("ship_deck", {-bayOuter, -kSlab, kDoorsBack}, {bayOuter, 0.0f, kBayBack + kWallT}, kBayDeck);
    b.Solid("ship_deck", {-bayOuter, -kSlab, kDoorsFront}, {-kDoorsHalfX, 0.0f, kDoorsBack}, kBayDeck);
    b.Solid("ship_deck", {kDoorsHalfX, -kSlab, kDoorsFront}, {bayOuter, 0.0f, kDoorsBack}, kBayDeck);
    b.Solid("ship_deck", {-outer, -kSlab, kBayBack + kWallT}, {outer, 0.0f, kStern + kWallT}, kDeckPlate);
    b.Solid("ship_roof", {-outer, kTop, kNose - kWallT}, {outer, kRoof, kBayFront - kWallT}, kCeiling);
    b.Solid("ship_roof", {-bayOuter, kBayTop, kBayFront - kWallT}, {bayOuter, kBayRoof, kBayBack + kWallT}, kCeiling);
    b.Solid("ship_roof", {-outer, kTop, kBayBack + kWallT}, {outer, kRoof, kStern + kWallT}, kCeiling);

    // --- The outside walls of the crew sections and the engine room -------------------------------------------------
    b.WallX("ship_wall", -outer, outer, kNose - kWallT, kNose, 0.0f, kTop, {kWindscreen}, kWall);
    b.Barrier({kWindscreen.from, kWindscreen.bottom, kNose - kWallT}, {kWindscreen.to, kWindscreen.top, kNose});
    for (const float side : {-1.0f, 1.0f})
    {
        const float x0 = side < 0.0f ? -outer : kHalf;
        const float x1 = side < 0.0f ? -kHalf : outer;
        b.WallZ("ship_wall", kNose, kBayFront - kWallT, x0, x1, 0.0f, kTop, {kSideWindow}, kWall);
        b.Barrier({x0, kSideWindow.bottom, kSideWindow.from}, {x1, kSideWindow.top, kSideWindow.to});
        b.Solid("ship_wall", {x0, 0.0f, kBayBack + kWallT}, {x1, kTop, kStern}, kWall);
    }
    b.Solid("ship_wall", {-outer, 0.0f, kStern}, {outer, kTop, kStern + kWallT}, kWall);
    // The window frames: mullions, so it reads as a window rather than a hole.
    for (const float x : {-1.2f, 1.2f})
    {
        b.Shape("ship_mullion", {x - 0.05f, kWindscreen.bottom, kNose - kWallT}, {x + 0.05f, kWindscreen.top, kNose}, kFrame);
    }
    for (const float x : {-outer, kHalf})
    {
        b.Shape("ship_mullion", {x, kSideWindow.bottom, -14.7f}, {x + kWallT, kSideWindow.top, -14.6f}, kFrame);
    }

    // --- Bulkheads between the sections, a door through each in the middle --------------------------------------
    const Opening door{-kDoorHalf, kDoorHalf, 0.0f, kDoorTop};
    b.WallX("ship_bulkhead", -kHalf, kHalf, kOpsFront - kWallT * 0.5f, kOpsFront + kWallT * 0.5f, 0.0f, kTop, {door}, kWallDark);
    b.WallX("ship_bulkhead", -kHalf, kHalf, kCrewFront - kWallT * 0.5f, kCrewFront + kWallT * 0.5f, 0.0f, kTop, {door}, kWallDark);
    b.WallX("ship_bulkhead", -bayOuter, bayOuter, kBayFront - kWallT, kBayFront, 0.0f, kBayTop, {door}, kBayWall);
    b.WallX("ship_bulkhead", -bayOuter, bayOuter, kBayBack, kBayBack + kWallT, 0.0f, kBayTop, {door}, kBayWall);

    // --- The crew section's corridor, between the bunks and the gear room -------------------------------------------
    const Opening sideDoor{kSideDoorFrom, kSideDoorTo, 0.0f, kDoorTop};
    b.WallZ("ship_partition", kCrewFront + kWallT * 0.5f, kBayFront - kWallT, -kCorridor - kPartition, -kCorridor, 0.0f, kTop, {sideDoor}, kWall);
    b.WallZ("ship_partition", kCrewFront + kWallT * 0.5f, kBayFront - kWallT, kCorridor, kCorridor + kPartition, 0.0f, kTop, {sideDoor}, kWall);

    // --- The bay: its walls, ribs down them and girders over it ---------------------------------------------------
    for (const float side : {-1.0f, 1.0f})
    {
        const float x0 = side < 0.0f ? -bayOuter : kBayHalf;
        const float x1 = side < 0.0f ? -kBayHalf : bayOuter;
        b.Solid("ship_bay_wall", {x0, 0.0f, kBayFront}, {x1, kBayTop, kBayBack}, kBayWall);
        for (float z = kBayFront + 2.0f; z < kBayBack - 1.0f; z += 3.5f)
        {
            const float r0 = side < 0.0f ? -kBayHalf : kBayHalf - 0.2f;
            b.Solid("ship_bay_rib", {r0, 0.0f, z - 0.15f}, {r0 + 0.2f, kBayTop, z + 0.15f}, kRib);
            if (side < 0.0f)
            {
                b.Shape("ship_bay_girder", {-kBayHalf + 0.2f, kBayTop - 0.4f, z - 0.12f}, {kBayHalf - 0.2f, kBayTop, z + 0.12f}, kRib);
            }
        }
    }
    // Yellow round the doors, for where not to stand.
    for (float x = -kDoorsHalfX - 0.35f; x < kDoorsHalfX + 0.35f - 0.01f; x += 0.7f)
    {
        const float to = std::min(x + 0.7f, kDoorsHalfX + 0.35f);
        b.Decal("ship_hazard", {x, 0.0f, kDoorsFront - 0.35f}, {to, 0.012f, kDoorsFront}, kHazardYellow);
        b.Decal("ship_hazard", {x, 0.0f, kDoorsBack}, {to, 0.012f, kDoorsBack + 0.35f}, kHazardYellow);
    }
    for (float z = kDoorsFront; z < kDoorsBack - 0.01f; z += 0.7f)
    {
        const float to = std::min(z + 0.7f, kDoorsBack);
        b.Decal("ship_hazard", {-kDoorsHalfX - 0.35f, 0.0f, z}, {-kDoorsHalfX, 0.012f, to}, kHazardYellow);
        b.Decal("ship_hazard", {kDoorsHalfX, 0.0f, z}, {kDoorsHalfX + 0.35f, 0.012f, to}, kHazardYellow);
    }
    // The shaft through the belly under the doors, lined.
    b.Shape("ship_bay_shaft", {-kDoorsHalfX - 0.05f, -0.9f, kDoorsFront}, {-kDoorsHalfX, -kSlab, kDoorsBack}, kHullDark);
    b.Shape("ship_bay_shaft", {kDoorsHalfX, -0.9f, kDoorsFront}, {kDoorsHalfX + 0.05f, -kSlab, kDoorsBack}, kHullDark);
}

// Conduit along a wall at a height, from one end to the other.
void Conduit(Builder& b, const glm::vec3& from, const glm::vec3& to, float thickness)
{
    const glm::vec3 lo = glm::min(from, to) - glm::vec3(thickness * 0.5f);
    const glm::vec3 hi = glm::max(from, to) + glm::vec3(thickness * 0.5f);
    b.Shape("ship_conduit", lo, hi, kPipe);
}

// A chair facing `yaw` (degrees; 0 faces -z), on the floor.
void Chair(Builder& b, float x, float z, float yaw)
{
    b.SolidTurned("ship_chair_seat", {x, 0.23f, z}, {0.46f, 0.46f, 0.46f}, yaw, kFurniture);
    const float r = glm::radians(yaw);
    const glm::vec3 back{std::sin(r) * 0.21f, 0.0f, std::cos(r) * 0.21f};
    b.ShapeTurned("ship_chair_back", glm::vec3(x, 0.72f, z) + back, {0.46f, 0.52f, 0.05f}, yaw, kCushion);
    b.ShapeTurned("ship_chair_cushion", {x, 0.475f, z}, {0.42f, 0.03f, 0.42f}, yaw, kCushion);
}

void BuildRooms(Builder& b)
{
    // --- The cockpit: the helm under the windscreen, two seats at it, the overhead panel --------------------------
    b.Solid("ship_helm", {-2.9f, 0.0f, kNose}, {2.9f, 0.75f, kNose + 0.8f}, kFurniture);
    for (float x = -2.7f; x < 2.6f; x += 1.1f)
    {
        b.Decal("ship_helm_screen", {x, 0.751f, kNose + 0.1f}, {x + 0.95f, 0.76f, kNose + 0.7f},
                static_cast<int>((x + 3.0f) / 1.1f) % 3 == 1 ? kScreenWarm : kScreen);
    }
    for (const float x : {-1.1f, 1.1f})
    {
        Chair(b, x, kNose + 1.9f, 0.0f);
    }
    b.Shape("ship_overhead", {-1.6f, kTop - 0.18f, kNose + 0.6f}, {1.6f, kTop, kNose + 2.4f}, kFrame);
    for (float x = -1.4f; x < 1.5f; x += 0.4f)
    {
        b.Decal("ship_overhead_light", {x, kTop - 0.185f, kNose + 1.4f}, {x + 0.05f, kTop - 0.18f, kNose + 1.45f}, kIndicator);
    }
    // A station either side behind the seats.
    for (const float side : {-1.0f, 1.0f})
    {
        const float x0 = side < 0.0f ? -kHalf : kHalf - 0.6f;
        b.Solid("ship_station", {x0, 0.0f, -12.9f}, {x0 + 0.6f, 0.85f, -11.75f}, kFurniture);
        b.Decal("ship_station_screen", {x0 + 0.08f, 0.851f, -12.8f}, {x0 + 0.52f, 0.86f, -11.85f}, kScreen);
    }

    // --- The ops room: the navigation table in the middle (the game's console), the screens ahead, a galley and a bench
    for (const float side : {-1.0f, 1.0f})
    {
        const float centre = side * kScreenX;
        b.Shape("ship_screen_frame", {centre - kScreenWidth * 0.5f - 0.08f, kScreenY - 0.5f, kOpsFront + kWallT * 0.5f},
                {centre + kScreenWidth * 0.5f + 0.08f, kScreenY + 0.5f, kOpsFront + kWallT * 0.5f + 0.04f}, kFrame);
    }
    b.Solid("ship_counter", {-kHalf, 0.0f, -11.0f}, {-kHalf + 0.65f, 0.9f, -8.6f}, kFurniture);
    b.Solid("ship_counter_top", {-kHalf, 0.9f, -11.05f}, {-kHalf + 0.7f, 0.94f, -8.55f}, kTableTop);
    b.Solid("ship_cupboard", {-kHalf, 1.55f, -11.0f}, {-kHalf + 0.4f, 2.3f, -8.6f}, kFurniture);
    b.Shape("ship_coffee", {-kHalf + 0.1f, 0.94f, -10.6f}, {-kHalf + 0.5f, 1.38f, -10.25f}, kFrame);
    b.Decal("ship_coffee_light", {-kHalf + 0.5f, 1.25f, -10.5f}, {-kHalf + 0.52f, 1.28f, -10.46f}, kIndicator);
    b.Solid("ship_bench", {kHalf - 0.45f, 0.0f, -6.9f}, {kHalf, 0.45f, -4.4f}, kFurniture);
    b.Solid("ship_table", {kHalf - 1.2f, 0.0f, -6.4f}, {kHalf - 0.6f, 0.72f, -4.9f}, kFrame);
    b.Solid("ship_table_top", {kHalf - 1.25f, 0.72f, -6.45f}, {kHalf - 0.55f, 0.76f, -4.85f}, kTableTop);
    b.Decal("ship_mug", {kHalf - 1.0f, 0.76f, -5.5f}, {kHalf - 0.9f, 0.86f, -5.4f}, kHazardYellow);
    b.Decal("ship_wall_screen", {-kHalf, 0.95f, -7.6f}, {-kHalf + 0.03f, 1.9f, -5.4f}, kScreen);
    Conduit(b, {kHalf - 0.1f, kTop - 0.2f, -11.3f}, {kHalf - 0.1f, kTop - 0.2f, -4.2f}, 0.12f);

    // --- The bunks, to port of the corridor -------------------------------------------------------------------------
    {
        int bunk = 0;
        for (const auto& [z0, z1] : {std::pair{-3.75f, -1.75f}, std::pair{1.1f, 3.1f}})
        {
            for (const float zp : {z0 + 0.03f, z1 - 0.03f})
            {
                b.Shape("ship_bunk_post", {-kHalf + 0.02f, 0.0f, zp - 0.03f}, {-kHalf + 0.08f, 2.3f, zp + 0.03f}, kFrame);
                b.Shape("ship_bunk_post", {-kHalf + 0.9f, 0.0f, zp - 0.03f}, {-kHalf + 0.96f, 2.3f, zp + 0.03f}, kFrame);
            }
            for (const float level : {0.35f, 1.45f})
            {
                b.Solid("ship_bunk", {-kHalf, level, z0}, {-kHalf + 0.97f, level + 0.12f, z1}, kFrame);
                b.Shape("ship_mattress", {-kHalf + 0.07f, level + 0.12f, z0 + 0.05f}, {-kHalf + 0.92f, level + 0.28f, z1 - 0.05f}, kMattress);
                const float hue = static_cast<float>((bunk * 3 + static_cast<int>(level * 2.0f)) % 5) / 5.0f;
                const Material blanket = Material::Diffuse({0.22f + 0.2f * hue, 0.26f + 0.06f * (1.0f - hue), 0.32f - 0.12f * hue}, 0.95f);
                b.Shape("ship_blanket", {-kHalf + 0.05f, level + 0.28f, z0 + 0.1f}, {-kHalf + 0.94f, level + 0.33f, z1 - 0.55f}, blanket);
                b.Shape("ship_pillow", {-kHalf + 0.17f, level + 0.28f, z1 - 0.5f}, {-kHalf + 0.82f, level + 0.4f, z1 - 0.12f}, kMattress);
            }
            b.Shape("ship_bag", {-kHalf + 0.1f, 0.0f, z0 + 0.3f + static_cast<float>(bunk % 2) * 0.4f},
                    {-kHalf + 0.6f, 0.3f, z0 + 0.8f + static_cast<float>(bunk % 2) * 0.4f}, kCrateDark);
            ++bunk;
        }
        // A footlocker under the door's far side, and the light switch's panel.
        b.Solid("ship_footlocker", {-2.0f, 0.0f, 2.5f}, {-1.0f, 0.45f, 3.2f}, kCrate);
        Conduit(b, {-kHalf + 0.06f, kTop - 0.15f, -3.8f}, {-kHalf + 0.06f, kTop - 0.15f, 3.2f}, 0.1f);
    }

    // --- The gear room, to starboard: the loadout locker straight ahead through its door, a bench, lockers ------------
    // The loadout locker, taller than the lockers beside it, with its screen (the game's: LoadoutLocker) and a hazard line
    // on the floor in front of it. The one thing in the room that looks like it is for something.
    b.Solid("ship_loadout", {kHalf - 0.55f, 0.0f, -1.85f}, {kHalf, 2.4f, -0.15f}, kFurniture);
    b.Decal("ship_loadout_shelf", {kHalf - 0.75f, 0.95f, -1.7f}, {kHalf - 0.55f, 1.0f, -0.3f}, kFrame);
    for (const float z : {-1.9f, -0.1f})
    {
        b.Decal("ship_loadout_line", {kHalf - 1.6f, 0.0f, z - 0.04f}, {kHalf - 0.55f, 0.004f, z + 0.04f}, kHazardYellow);
    }
    b.Decal("ship_loadout_line", {kHalf - 1.68f, 0.0f, -1.9f}, {kHalf - 1.6f, 0.004f, -0.1f}, kHazardYellow);
    b.Solid("ship_bench", {kCorridor + kPartition + 0.2f, 0.0f, kCrewFront + kWallT * 0.5f}, {kHalf - 0.1f, 0.9f, kCrewFront + 0.7f}, kFurniture);
    b.Solid("ship_bench_top", {kCorridor + kPartition + 0.15f, 0.9f, kCrewFront + kWallT * 0.5f}, {kHalf - 0.05f, 0.92f, kCrewFront + 0.75f},
            kTableTop);
    b.Solid("ship_rack", {kCorridor + kPartition + 0.4f, 1.3f, kCrewFront + kWallT * 0.5f}, {kHalf - 0.4f, 2.2f, kCrewFront + 0.3f}, kWallDark);
    Conduit(b, {kHalf - 0.06f, kTop - 0.15f, -3.8f}, {kHalf - 0.06f, kTop - 0.15f, 3.2f}, 0.12f);

    // --- The bay: crates against the walls aft, out of the way of the doors, and a panel ---------------------------------
    b.Solid("ship_crate", {-kBayHalf, 0.0f, kBayBack - 2.0f}, {-kDoorsHalfX - 0.1f, 1.2f, kBayBack}, kCrate);
    b.Solid("ship_crate", {-kBayHalf, 1.2f, kBayBack - 1.6f}, {-kDoorsHalfX - 0.2f, 1.9f, kBayBack - 0.2f}, kCrateDark);
    b.Solid("ship_crate", {kDoorsHalfX + 0.1f, 0.0f, kBayBack - 1.8f}, {kBayHalf, 1.0f, kBayBack}, kCrateDark);
    b.Decal("ship_bay_panel", {-kBayHalf, 1.2f, 8.0f}, {-kBayHalf + 0.03f, 2.3f, 9.6f}, kScreenWarm);
    Conduit(b, {kBayHalf - 0.25f, 1.2f, kBayFront + 0.3f}, {kBayHalf - 0.25f, 1.2f, kBayBack - 2.0f}, 0.18f);

    // --- The engine room: the reactor in the middle, its glow through a band round it, and the machinery round it ------
    b.Solid("ship_reactor", {-0.8f, 0.0f, 21.2f}, {0.8f, 2.4f, 22.8f}, kRib);
    b.Decal("ship_reactor_band", {-0.82f, 1.1f, 21.18f}, {0.82f, 1.4f, 22.82f}, kReactorGlow);
    for (const float side : {-1.0f, 1.0f})
    {
        const float x0 = side < 0.0f ? -kHalf : kHalf - 0.7f;
        b.Solid("ship_machinery", {x0, 0.0f, 20.3f}, {x0 + 0.7f, 1.6f, 23.9f}, kFurniture);
        b.Decal("ship_machinery_light", {x0 + (side < 0.0f ? 0.7f : -0.01f), 1.2f, 21.5f}, {x0 + (side < 0.0f ? 0.71f : 0.0f), 1.3f, 21.6f},
                kIndicator);
        Conduit(b, {side * (kHalf - 0.1f), kTop - 0.2f, kBayBack + 0.4f}, {side * (kHalf - 0.1f), kTop - 0.2f, kStern - 0.2f}, 0.16f);
    }
    Conduit(b, {0.0f, 2.4f, 22.0f}, {0.0f, kTop - 0.01f, 22.0f}, 0.3f);
}

void BuildLamps(Scene& scene, MeshLibrary& meshes, LevelLights& lights)
{
    const glm::vec3 down{0.0f, -1.0f, 0.0f};
    const auto lamp = [&](LightKind kind, const glm::vec3& at, const glm::vec3& a, const glm::vec3& b, float range,
                          const glm::vec3& direction = glm::vec3(0.0f, -1.0f, 0.0f))
    {
        const int index = lights.Add(scene, meshes, kind, LightMood::Steady, kOrigin + at, direction, 0, 0, range);
        lights.Bound(index, kOrigin + a, kOrigin + b);
    };
    const float ceiling = kTop - 0.04f;
    lamp(LightKind::Wall, {-1.8f, ceiling - 0.1f, -14.0f}, {-kHalf, -0.05f, kNose}, {kHalf, kTop, kOpsFront}, 6.0f, down);
    lamp(LightKind::Wall, {1.8f, ceiling - 0.1f, -14.0f}, {-kHalf, -0.05f, kNose}, {kHalf, kTop, kOpsFront}, 6.0f, down);
    for (const float z : {-9.5f, -6.0f})
    {
        lamp(LightKind::Ceiling, {0.0f, ceiling, z}, {-kHalf, -0.05f, kOpsFront}, {kHalf, kTop, kCrewFront}, 8.0f);
    }
    lamp(LightKind::Ceiling, {0.0f, ceiling, 0.0f}, {-kCorridor, -0.05f, kCrewFront}, {kCorridor, kTop, kBayFront}, 7.0f);
    lamp(LightKind::Wall, {-2.1f, ceiling - 0.1f, -0.2f}, {-kHalf, -0.05f, kCrewFront}, {-kCorridor, kTop, kBayFront}, 6.0f, down);
    lamp(LightKind::Ceiling, {2.1f, ceiling, -0.5f}, {kCorridor, -0.05f, kCrewFront}, {kHalf, kTop, kBayFront}, 7.0f);
    for (const float x : {-3.2f, 3.2f})
    {
        for (const float z : {7.5f, 15.5f})
        {
            lamp(LightKind::Flood, {x, kBayTop - 0.1f, z}, {-kBayHalf, -0.05f, kBayFront}, {kBayHalf, kBayTop, kBayBack}, 14.0f, down);
        }
    }
    lamp(LightKind::Ceiling, {0.0f, ceiling, 20.6f}, {-kHalf, -0.05f, kBayBack}, {kHalf, kTop, kStern}, 7.0f);
    lamp(LightKind::Wall, {0.0f, ceiling - 0.1f, 23.9f}, {-kHalf, -0.05f, kBayBack}, {kHalf, kTop, kStern}, 6.0f, down);
}

// --- The outside --------------------------------------------------------------------------------------------------

// A part of the hull model: a box between two corners.
ModelPart& HullBox(ModelAsset& model, const std::string& name, const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& colour,
                   float roughness = 0.7f, float metallic = 0.0f, float emissive = 0.0f)
{
    ModelPart part;
    part.name = name;
    part.shape = PartShape::Box;
    part.position = (lo + hi) * 0.5f;
    part.size = glm::abs(hi - lo);
    part.color = colour;
    part.roughness = roughness;
    part.metallic = metallic;
    part.emissive = emissive;
    model.parts.push_back(part);
    return model.parts.back();
}

ModelPart& HullCylinder(ModelAsset& model, const std::string& name, const glm::vec3& centre, float diameter, float length, const glm::vec3& rotation,
                        const glm::vec3& colour, float metallic = 0.0f, float emissive = 0.0f)
{
    ModelPart& part = HullBox(model, name, centre - glm::vec3(diameter * 0.5f, length * 0.5f, diameter * 0.5f),
                              centre + glm::vec3(diameter * 0.5f, length * 0.5f, diameter * 0.5f), colour, 0.45f, metallic, emissive);
    part.shape = PartShape::Cylinder;
    part.rotation = rotation;
    return part;
}

// A quad facing out from a centre, and a block narrowing from one upright rectangle across x to another.
void OutwardQuad(MeshData& mesh, const glm::vec3& a, glm::vec3 b, const glm::vec3& c, glm::vec3 d, const glm::vec3& centre)
{
    glm::vec3 normal = glm::normalize(glm::cross(b - a, c - a));
    if (glm::dot(normal, (a + b + c + d) * 0.25f - centre) < 0.0f)
    {
        std::swap(b, d);
        normal = -normal;
    }
    const auto base = static_cast<uint32_t>(mesh.vertices.size());
    const glm::vec3 corners[4] = {a, b, c, d};
    const glm::vec2 uvs[4] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    for (int i = 0; i < 4; ++i)
    {
        mesh.vertices.push_back(MeshVertex{corners[i], normal, uvs[i]});
    }
    mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

MeshData Tapered(float backZ, float backHalf, float backLow, float backHigh, float frontZ, float frontHalf, float frontLow, float frontHigh)
{
    const glm::vec3 b0{-backHalf, backLow, backZ}, b1{backHalf, backLow, backZ}, b2{backHalf, backHigh, backZ}, b3{-backHalf, backHigh, backZ};
    const glm::vec3 f0{-frontHalf, frontLow, frontZ}, f1{frontHalf, frontLow, frontZ}, f2{frontHalf, frontHigh, frontZ},
        f3{-frontHalf, frontHigh, frontZ};
    const glm::vec3 centre = (b0 + b1 + b2 + b3 + f0 + f1 + f2 + f3) / 8.0f;
    MeshData mesh;
    OutwardQuad(mesh, b0, b1, b2, b3, centre);
    OutwardQuad(mesh, f0, f1, f2, f3, centre);
    OutwardQuad(mesh, b0, b1, f1, f0, centre);
    OutwardQuad(mesh, b3, b2, f2, f3, centre);
    OutwardQuad(mesh, b0, b3, f3, f0, centre);
    OutwardQuad(mesh, b1, b2, f2, f1, centre);
    return mesh;
}

// A section of hull: an outline across the ship (x, y; convex, either way round) drawn along it from z0 to z1. Open, the
// outline's last point is not joined back to its first (the bay, open underneath); its ends are closed either way.
MeshData Extruded(const std::vector<glm::vec2>& outline, float z0, float z1, bool closed)
{
    MeshData mesh;
    glm::vec2 centre{0.0f};
    for (const glm::vec2& point : outline)
    {
        centre += point;
    }
    centre /= static_cast<float>(std::max<size_t>(outline.size(), 1));
    const glm::vec3 middle{centre, (z0 + z1) * 0.5f};
    const size_t edges = closed ? outline.size() : outline.size() - 1;
    for (size_t i = 0; i < edges; ++i)
    {
        const glm::vec2 a = outline[i];
        const glm::vec2 b = outline[(i + 1) % outline.size()];
        OutwardQuad(mesh, {a, z0}, {b, z0}, {b, z1}, {a, z1}, middle);
    }
    // The ends, a fan from the outline's middle, wound to face out of the end each closes.
    for (const float z : {z0, z1})
    {
        const glm::vec3 normal{0.0f, 0.0f, z < middle.z ? -1.0f : 1.0f};
        const auto base = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back(MeshVertex{{centre, z}, normal, {0.5f, 0.5f}});
        for (const glm::vec2& point : outline)
        {
            mesh.vertices.push_back(MeshVertex{{point, z}, normal, {0.0f, 0.0f}});
        }
        for (size_t i = 0; i < outline.size(); ++i)
        {
            const auto first = base + 1 + static_cast<uint32_t>(i);
            const auto second = base + 1 + static_cast<uint32_t>((i + 1) % outline.size());
            const glm::vec3 pa = mesh.vertices[first].position - mesh.vertices[base].position;
            const glm::vec3 pb = mesh.vertices[second].position - mesh.vertices[base].position;
            if (glm::dot(glm::cross(pa, pb), normal) >= 0.0f)
            {
                mesh.indices.insert(mesh.indices.end(), {base, first, second});
            }
            else
            {
                mesh.indices.insert(mesh.indices.end(), {base, second, first});
            }
        }
    }
    return mesh;
}

void HullMesh(ModelAsset& model, const std::string& name, MeshData mesh, const glm::vec3& colour)
{
    ModelPart part;
    part.name = name;
    part.shape = PartShape::Mesh;
    part.size = glm::vec3(1.0f);
    part.mesh = std::move(mesh);
    part.color = colour;
    part.roughness = 0.7f;
    model.parts.push_back(part);
}

} // namespace

ModelAsset ShipMap::HullModel(const ShipHullLook& look)
{
    // In the ship's frame, round its rooms: the skin a few centimetres outside every outer wall and roof, so it covers them
    // seen from outside and never shares a face with them; the windows' gaps through it where theirs are.
    ModelAsset model;
    model.name = "ship_hull";
    int count = 0;
    const auto name = [&](const char* stem) { return std::string(stem) + "_" + std::to_string(count++); };
    const glm::vec3 primary = look.primary;
    const glm::vec3 secondary = look.secondary;
    const glm::vec3 accent = look.accent;
    const glm::vec3 dark{0.12f, 0.12f, 0.13f};
    const glm::vec3 glass{0.95f, 0.78f, 0.5f};
    const float skin = 0.06f;
    const float outer = kHalf + kWallT;      // the crew sections' outer walls
    const float bayOuter = kBayHalf + kWallT;
    const float side = outer + skin;
    const float belly = -kSlab - 0.4f;
    const float top = kRoof + 0.06f;

    // --- The cockpit: its own section ahead of the body, narrower and lower, skin round its windows --------------------
    const float front = kNose - kWallT;
    for (const float s : {-1.0f, 1.0f})
    {
        const float x0 = s < 0.0f ? -side : outer;
        const float x1 = s < 0.0f ? -outer : side;
        HullBox(model, name("skin"), {x0, belly, front}, {x1, top, kSideWindow.from}, primary);
        HullBox(model, name("skin"), {x0, belly, kSideWindow.to}, {x1, top, kOpsFront}, primary);
        HullBox(model, name("skin"), {x0, belly, kSideWindow.from}, {x1, kSideWindow.bottom, kSideWindow.to}, primary);
        HullBox(model, name("skin"), {x0, kSideWindow.top, kSideWindow.from}, {x1, top, kSideWindow.to}, primary);
    }
    HullBox(model, name("skin"), {-outer, kRoof, front}, {outer, top, kOpsFront}, primary);
    HullBox(model, name("skin"), {-outer, belly, front}, {outer, -kSlab, kOpsFront}, primary);
    const float face = front - skin;
    HullBox(model, name("skin"), {-side, belly, face}, {-kWindscreen.to, top, front}, primary);
    HullBox(model, name("skin"), {kWindscreen.to, belly, face}, {side, top, front}, primary);
    HullBox(model, name("skin"), {-kWindscreen.to, belly, face}, {kWindscreen.to, kWindscreen.bottom, front}, primary);
    HullBox(model, name("skin"), {-kWindscreen.to, kWindscreen.top, face}, {kWindscreen.to, top, front}, primary);
    HullMesh(model, name("chin"), Tapered(face, side, belly, kWindscreen.bottom - 0.05f, face - 2.8f, 1.6f, -0.2f, 0.5f), secondary);
    HullMesh(model, name("brow"), Tapered(face, side, kWindscreen.top + 0.05f, top, face - 1.0f, 2.6f, kWindscreen.top + 0.15f, top - 0.35f), primary);
    HullBox(model, name("visor"), {-2.6f, kWindscreen.top + 0.02f, face - 0.55f}, {2.6f, kWindscreen.top + 0.12f, face - 0.05f}, dark, 0.5f, 0.8f);
    HullMesh(model, name("nose_stripe"), Tapered(face - 1.2f, 2.45f, -0.05f, 0.25f, face - 1.6f, 2.15f, -0.05f, 0.25f), accent);

    // --- The body: sections drawn along the spine from outlines across it, their long edges bevelled ----------------------
    // The crew sections from the ops room aft to the bay, wider and taller than the cockpit ahead of them; the bay, widest
    // and tallest, open underneath where its doors are; and the engine room aft of it. Each a little outside the rooms in it,
    // so nothing of the rooms shows through, even at the bevels.
    const float bodySide = outer + 0.25f;
    const float bodyTop = kRoof + 0.35f;
    const float bodyLow = belly - 0.1f;
    const float bayWide = bayOuter + 0.3f;
    const float bayHigh = kBayRoof + 0.25f;
    const float bayLow = belly - 0.05f;
    const auto bevelled = [](float half, float low, float high, float topCut, float bottomCut)
    {
        return std::vector<glm::vec2>{{-half, low + bottomCut}, {-half, high - topCut}, {-half + topCut, high}, {half - topCut, high},
                                      {half, high - topCut},     {half, low + bottomCut}, {half - bottomCut, low}, {-half + bottomCut, low}};
    };
    const float bayFront = kBayFront - kWallT - skin;
    const float bayBack = kBayBack + kWallT + skin;
    const float tail = kStern + kWallT + skin + 0.1f;
    HullMesh(model, name("body"), Extruded(bevelled(bodySide, bodyLow, bodyTop, 0.5f, 0.45f), kOpsFront, bayFront, true), primary);
    HullMesh(model, name("engine_room"), Extruded(bevelled(bodySide, bodyLow, bodyTop, 0.5f, 0.45f), bayBack, tail, true), secondary);
    const std::vector<glm::vec2> bayOutline{{-bayWide, bayLow},        {-bayWide, bayHigh - 0.4f}, {-bayWide + 0.4f, bayHigh},
                                            {bayWide - 0.4f, bayHigh}, {bayWide, bayHigh - 0.4f},  {bayWide, bayLow}};
    HullMesh(model, name("bay"), Extruded(bayOutline, bayFront, bayBack, false), secondary);
    // Its belly round the doors' shaft, just inside the bay's sides.
    const float inside = bayWide - 0.02f;
    HullBox(model, name("bay_belly"), {-inside, bayLow, bayFront}, {inside, -kSlab, kDoorsFront}, secondary, 0.6f, 0.4f);
    HullBox(model, name("bay_belly"), {-inside, bayLow, kDoorsBack}, {inside, -kSlab, bayBack}, secondary, 0.6f, 0.4f);
    HullBox(model, name("bay_belly"), {-inside, bayLow, kDoorsFront}, {-kDoorsHalfX - 0.05f, -kSlab, kDoorsBack}, secondary, 0.6f, 0.4f);
    HullBox(model, name("bay_belly"), {kDoorsHalfX + 0.05f, bayLow, kDoorsFront}, {inside, -kSlab, kDoorsBack}, secondary, 0.6f, 0.4f);

    // --- What is on the body: a belt of the accent, plates, lit portholes, the spine and plating on the roof -------------
    for (const float s : {-1.0f, 1.0f})
    {
        const float b0 = s < 0.0f ? -bodySide - 0.04f : bodySide;
        const float b1 = s < 0.0f ? -bodySide : bodySide + 0.04f;
        HullBox(model, name("belt"), {b0, 0.15f, kOpsFront + 0.3f}, {b1, 0.55f, bayFront - 0.3f}, accent);
        HullBox(model, name("plate"), {b0, -0.45f, -10.5f}, {b1, -0.05f, -5.0f}, secondary, 0.6f, 0.4f);
        HullBox(model, name("plate"), {b0, -0.45f, -3.5f}, {b1, -0.05f, 2.5f}, secondary, 0.6f, 0.4f);
        for (float z = -9.8f; z < 2.6f; z += 2.4f)
        {
            HullBox(model, name("fx_porthole"), {b0, 1.55f, z}, {b1, 1.9f, z + 0.5f}, glass, 0.4f, 0.0f, 1.3f);
        }
        // On the bay's sides: raised panels between ribs, and a stripe of the accent along it.
        // Each sunk a little way into the side, by a different amount, so no two share a face there.
        const float p0 = s < 0.0f ? -bayWide - 0.05f : bayWide - 0.01f;
        const float p1 = s < 0.0f ? -bayWide + 0.01f : bayWide + 0.05f;
        for (float z = kBayFront; z < kBayBack - 1.0f; z += 4.0f)
        {
            HullBox(model, name("bay_panel"), {p0, 0.4f, z + 0.4f}, {p1, 3.9f, z + 3.4f}, primary);
            HullBox(model, name("bay_rib"), {p0 - (s < 0.0f ? 0.04f : 0.02f), -0.6f, z + 3.6f}, {p1 + (s < 0.0f ? 0.02f : 0.04f), 5.6f, z + 3.85f}, dark,
                    0.5f, 0.8f);
        }
        HullBox(model, name("bay_stripe"), {p0 - (s < 0.0f ? 0.03f : 0.01f), 4.4f, bayFront + 0.3f}, {p1 + (s < 0.0f ? 0.01f : 0.03f), 4.9f, bayBack - 0.3f},
                accent);
    }
    HullBox(model, name("spine"), {-0.8f, bodyTop, -10.8f}, {0.8f, bodyTop + 0.35f, bayFront}, secondary, 0.6f, 0.4f);
    for (float z = -10.5f; z < 2.0f; z += 4.0f)
    {
        HullBox(model, name("roof_plate"), {-3.2f, bodyTop, z}, {-1.2f, bodyTop + 0.06f, z + 3.0f}, secondary, 0.6f, 0.4f);
        HullBox(model, name("roof_plate"), {1.2f, bodyTop, z + 0.5f}, {3.2f, bodyTop + 0.06f, z + 3.5f}, secondary, 0.6f, 0.4f);
    }
    for (float z = bayFront + 1.0f; z < bayBack - 2.0f; z += 5.0f)
    {
        HullBox(model, name("bay_roof_plate"), {-4.6f, bayHigh, z}, {4.6f, bayHigh + 0.06f, z + 3.6f}, primary);
    }

    // --- Sensors on the roof: a mast; a dish with the first upgrade; arrays either side with the third ------------------
    HullBox(model, name("mast"), {-0.08f, bodyTop + 0.35f, -8.08f}, {0.08f, bodyTop + 2.2f, -7.92f}, dark, 0.5f, 0.8f);
    HullBox(model, name("fx_mast_light"), {-0.1f, bodyTop + 2.2f, -8.1f}, {0.1f, bodyTop + 2.35f, -7.9f}, {1.0f, 0.15f, 0.1f}, 0.4f, 0.0f, 4.0f);
    if (look.sensors >= 1)
    {
        HullBox(model, name("dish_mount"), {-0.25f, bayHigh + 0.06f, 9.6f}, {0.25f, bayHigh + 0.95f, 10.1f}, dark, 0.5f, 0.8f);
        HullCylinder(model, name("dish"), {0.0f, bayHigh + 1.45f, 9.85f}, 2.2f + 0.4f * static_cast<float>(std::min(look.sensors, 4)), 0.15f,
                     {55.0f, 0.0f, 0.0f}, secondary);
    }
    if (look.sensors >= 3)
    {
        for (const float s : {-1.0f, 1.0f})
        {
            HullBox(model, name("array_boom"), {s < 0.0f ? -bayWide - 2.6f : bayWide - 0.04f, 3.6f, 14.0f}, {s < 0.0f ? -bayWide + 0.04f : bayWide + 2.6f, 3.75f, 14.2f},
                    dark, 0.5f, 0.8f);
            HullBox(model, name("array"), {s < 0.0f ? -bayWide - 2.7f : bayWide + 2.4f, 2.4f, 13.4f},
                    {s < 0.0f ? -bayWide - 2.4f : bayWide + 2.7f, 5.0f, 14.8f}, secondary, 0.6f, 0.4f);
        }
    }

    // --- The engines aft: a main drive and, as the drive is upgraded, nacelles either side and larger nozzles ------------
    const float aft = tail;
    const float tier = static_cast<float>(std::clamp(look.drive, 0, 5));
    struct Engine
    {
        float x;
        float y;
        float half;
        float length;
    };
    std::vector<Engine> engines{{0.0f, 1.25f, 1.6f + 0.15f * tier, 3.0f + 0.4f * tier}};
    if (look.drive >= 2)
    {
        // Clear of the bay's side, a pylon's width out.
        const float half = 1.0f + 0.12f * tier;
        engines.push_back({-(bayWide + 0.6f + half), 2.5f, half, 6.0f + 0.5f * tier});
        engines.push_back({bayWide + 0.6f + half, 2.5f, half, 6.0f + 0.5f * tier});
    }
    else
    {
        engines.push_back({-2.55f, 0.9f, 0.6f, 2.2f});
        engines.push_back({2.55f, 0.9f, 0.6f, 2.2f});
    }
    int engineIndex = 0;
    for (const Engine& engine : engines)
    {
        const bool nacelle = std::abs(engine.x) > outer;
        // A nacelle is carried on a pylon from the bay's side, along it from the bay's after end.
        const float z0 = nacelle ? kBayBack - 3.0f : aft;
        const float z1 = z0 + engine.length;
        HullBox(model, name("engine"), {engine.x - engine.half, engine.y - engine.half, z0}, {engine.x + engine.half, engine.y + engine.half, z1},
                secondary, 0.6f, 0.4f);
        HullBox(model, name("engine_band"), {engine.x - engine.half - 0.05f, engine.y - engine.half - 0.05f, z0 + 0.6f},
                {engine.x + engine.half + 0.05f, engine.y + engine.half + 0.05f, z0 + 1.0f}, accent);
        if (nacelle)
        {
            const float inner = engine.x < 0.0f ? -bayWide + 0.05f : bayWide - 0.05f;
            const float out = engine.x < 0.0f ? engine.x + engine.half : engine.x - engine.half;
            HullBox(model, name("pylon"), {std::min(inner, out), engine.y - 0.2f, z0 + 1.2f}, {std::max(inner, out), engine.y + 0.2f, z0 + 3.2f}, dark,
                    0.5f, 0.8f);
        }
        HullCylinder(model, name("nozzle"), {engine.x, engine.y, z1 + 0.5f}, engine.half * 1.8f, 1.0f, {90.0f, 0.0f, 0.0f}, dark, 1.0f);
        // In the nozzle's mouth and proud of it, so no face of it lies close to one of the nozzle's. Dark until a burn lights it.
        HullCylinder(model, "fx_engine_glow_" + std::to_string(engineIndex++), {engine.x, engine.y, z1 + 1.05f}, engine.half * 1.5f, 0.3f,
                     {90.0f, 0.0f, 0.0f}, {0.55f, 0.75f, 1.0f});
    }

    // Its lights: red to port, green to starboard, white at the tail.
    HullBox(model, "fx_nav_port", {-bayWide - 0.2f, 3.0f, kBayFront + 0.2f}, {-bayWide, 3.2f, kBayFront + 0.4f}, {1.0f, 0.1f, 0.08f}, 0.4f, 0.0f, 4.0f);
    HullBox(model, "fx_nav_starboard", {bayWide, 3.0f, kBayFront + 0.2f}, {bayWide + 0.2f, 3.2f, kBayFront + 0.4f}, {0.1f, 1.0f, 0.2f}, 0.4f, 0.0f,
            4.0f);
    HullBox(model, "fx_nav_tail", {-0.1f, bayHigh, kBayBack - 0.4f}, {0.1f, bayHigh + 0.2f, kBayBack - 0.2f}, {1.0f, 1.0f, 1.0f}, 0.4f, 0.0f, 4.0f);
    return model;
}

void ShipMap::Build(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, LevelLights* lights)
{
    if (m_built)
    {
        return;
    }
    const uint32_t group = physics.NewOverlapGroup();
    {
        MapBuilder map(scene, meshes, &physics, "ship_");
        map.BeginBatching();
        map.Track(&m_entities, &m_bodies);
        map.SetStructure(group);
        Builder b(map, physics, group, m_bodies);
        BuildStructure(b);
        BuildRooms(b);
    }
    // Its outside, drawn round the rooms and never solid -- nobody is out there -- and the same again out on the stage.
    const auto hull = std::make_shared<ModelAsset>(HullModel(m_look));
    m_hull.Build(scene, meshes, nullptr, hull, Pose(glm::vec3(0.0f)), 0, "ship_hull_");
    m_stageHull.Build(scene, meshes, nullptr, hull, {kStage, {1.0f, 0.0f, 0.0f, 0.0f}}, 0, "ship_stage_");
    if (lights != nullptr)
    {
        BuildLamps(scene, meshes, *lights);
    }

    // The bay doors in its floor, and the shuttle standing on them nose forward, its ramp down aft.
    m_bayDoors.Build(scene, meshes, &physics, Vehicles::Load("hangar_doors"), Pose({0.0f, kDeck, (kDoorsFront + kDoorsBack) * 0.5f}), group,
                     "ship_bay_");
    m_shuttle.Build(scene, meshes, &physics, Vehicles::Load("shuttle"), Pose(kShuttleHome), group, "ship_shuttle_");
    // And a lamp lit in its cabin, which goes with it when it flies (the game moves it: ShuttleLamp).
    CinePose cabin;
    if (lights != nullptr && m_shuttle.Socket("lamp", cabin))
    {
        m_shuttleLamp = lights->Add(scene, meshes, LightKind::Ceiling, LightMood::Steady, cabin.position, glm::vec3(0.0f, -1.0f, 0.0f), 0, 0x5A77u,
                                    6.0f);
        lights->Place(scene, m_shuttleLamp, cabin.position, glm::vec3(0.0f, -1.0f, 0.0f));
        glm::vec3 low;
        glm::vec3 high;
        ShuttleCabinBounds(cabin, low, high);
        lights->Bound(m_shuttleLamp, low, high);
    }

    // What WorldObjects puts aboard: lockers down the gear room's hull side and ammunition by its bench. The kit is drawn
    // at the loadout locker.
    for (const float z : {0.45f, 1.57f, 2.69f})
    {
        m_placements.lockers.push_back({ToWorld({kHalf - 0.47f, kDeck, z}), glm::half_pi<float>()});
    }
    m_placements.ammoCrates.push_back({ToWorld({1.6f, kDeck, -2.75f}), 0.0f});
    m_placements.ammoCrates.push_back({ToWorld({2.6f, kDeck, -2.75f}), 0.0f});
    m_built = true;
    PRED_LOG_INFO(Gameplay, "The ship: {} bodies, {} entities", m_bodies.size(), m_entities.size());
}

void ShipMap::SetLook(Scene& scene, MeshLibrary& meshes, const ShipHullLook& look)
{
    if (look == m_look && m_hull.Built())
    {
        return;
    }
    m_look = look;
    if (!m_built)
    {
        return;
    }
    // Both outsides made again for it, where they are now.
    const CinePose home = m_hull.Home();
    const CinePose stage = m_stageHull.Home();
    const auto hull = std::make_shared<ModelAsset>(HullModel(m_look));
    m_hull.Clear(scene, nullptr);
    m_stageHull.Clear(scene, nullptr);
    m_hull.Build(scene, meshes, nullptr, hull, home, 0, "ship_hull_");
    m_stageHull.Build(scene, meshes, nullptr, hull, stage, 0, "ship_stage_");
    m_hullsFar = false;
    m_showSet = false;
    SetEngines(scene, m_burn);
}

std::vector<glm::vec4> ShipMap::DeckPlan(int deck)
{
    if (deck != 0)
    {
        return {};
    }
    return {{-kHalf, kNose, kHalf, kOpsFront},           {-kHalf, kOpsFront, kHalf, kCrewFront}, {-kCorridor, kCrewFront, kCorridor, kBayFront},
            {-kHalf, kCrewFront, -kCorridor, kBayFront}, {kCorridor, kCrewFront, kHalf, kBayFront}, {-kBayHalf, kBayFront, kBayHalf, kBayBack},
            {-kHalf, kBayBack, kHalf, kStern}};
}

bool ShipMap::Contains(const glm::vec3& point) const
{
    const glm::vec2 d{point.x - kOrigin.x, point.z - kOrigin.z};
    const glm::vec2 s{point.x - kStage.x, point.z - kStage.z};
    return m_built && (glm::dot(d, d) < kReach * kReach || glm::dot(s, s) < kStageReach * kStageReach);
}

bool ShipMap::InHangar(const glm::vec3& point) const
{
    const glm::vec3 local = point - kOrigin;
    return m_built && local.x > -kBayHalf && local.x < kBayHalf && local.z > kBayFront && local.z < kBayBack && local.y > kDeck - 0.5f &&
           local.y < kBayTop;
}

glm::vec3 ShipMap::Spawn(uint8_t player) const
{
    // In the ops room, aft of the navigation table, two rows of four, looking forward at the screens.
    const float x = -1.5f + static_cast<float>(player % 4) * 1.0f;
    const float z = -5.6f + static_cast<float>((player / 4) % 2) * 0.9f;
    return ToWorld({x, kDeck + 0.1f, z});
}

float ShipMap::SpawnYaw() const
{
    return 0.0f;
}

CinePose ShipMap::LoadoutLocker() const
{
    // The middle of its screen, on its face, turned to look out into the gear room (-x), across from the room's door.
    return Pose({kHalf - 0.565f, kDeck + 1.5f, -1.0f}, 90.0f);
}

CinePose ShipMap::BriefingConsole() const
{
    // The navigation table in the middle of the ops room, its front aft, towards where people come in and stand.
    return Pose({0.0f, kDeck, -8.6f}, 180.0f);
}

CinePose ShipMap::BriefingScreen(int index, float& width) const
{
    width = kScreenWidth;
    return Pose({index == 0 ? -kScreenX : kScreenX, kScreenY, kOpsFront + kWallT * 0.5f + 0.05f});
}

void ShipMap::ShowFor(Scene& scene, const glm::vec3& eye)
{
    if (!m_built)
    {
        return;
    }
    // Both outsides are to be seen however far off they go -- flying away into the distance.
    if (!m_hullsFar)
    {
        m_hullsFar = true;
        for (VehicleProp* hull : {&m_hull, &m_stageHull})
        {
            for (const ModelPart& part : hull->Model()->parts)
            {
                if (MeshRenderer* renderer = scene.GetMeshRenderer(hull->Part(part.name)))
                {
                    renderer->farVisible = true;
                }
            }
        }
    }
    const bool stage = glm::distance(eye, kStage) < glm::distance(eye, kOrigin);
    if (m_showSet && stage == m_showingStage)
    {
        return;
    }
    m_showSet = true;
    m_showingStage = stage;
    // Looking at the stage: its ship, and nothing of the one everybody is standing in -- rooms, bay, shuttle and all.
    m_stageHull.SetHidden(scene, !stage);
    m_hull.SetHidden(scene, stage);
    m_shuttle.SetHidden(scene, stage);
    m_bayDoors.SetHidden(scene, stage);
    for (const Entity entity : m_entities)
    {
        if (MeshRenderer* renderer = scene.GetMeshRenderer(entity))
        {
            renderer->visible = !stage;
        }
    }
    for (Speck& speck : m_dust)
    {
        if (MeshRenderer* renderer = scene.GetMeshRenderer(speck.entity))
        {
            renderer->visible = m_dustShown && !stage;
        }
    }
}

void ShipMap::UpdateDust(Scene& scene, MeshLibrary& meshes, float speed, float dt)
{
    if (!m_built)
    {
        return;
    }
    // Round the ship, never inside it: its outside's box, and a little more, is kept clear.
    AABB hull;
    hull.min = glm::vec3(-8.0f, -4.0f, -24.0f);
    hull.max = glm::vec3(8.0f, 9.0f, 34.0f);
    if (m_hull.Built())
    {
        bool any = false;
        for (const ModelPart& part : m_hull.Model()->parts)
        {
            if (part.shape == PartShape::Mesh)
            {
                continue;
            }
            const glm::vec3 half = part.size * 0.5f;
            const glm::vec3 lo = part.position - half;
            const glm::vec3 hi = part.position + half;
            hull.min = any ? glm::min(hull.min, lo) : lo;
            hull.max = any ? glm::max(hull.max, hi) : hi;
            any = true;
        }
    }
    const glm::vec3 clearLo = hull.min - glm::vec3(4.0f);
    const glm::vec3 clearHi = hull.max + glm::vec3(4.0f);
    // A frame along the ship, bow first: dust is laid out across it and moves back down it.
    const glm::vec3 along{0.0f, 0.0f, -1.0f};
    const glm::vec3 sideways = glm::normalize(glm::cross(along, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 up = glm::cross(sideways, along);
    const float reach = std::max(glm::length(hull.min), glm::length(hull.max)) + 220.0f; // how far ahead and behind
    const float across = std::max(std::abs(clearLo.x), std::abs(clearHi.x)) + 60.0f;
    const float over = std::max(std::abs(clearLo.y), std::abs(clearHi.y)) + 40.0f;
    const auto random = [this]()
    {
        m_dustSeed = m_dustSeed * 1664525u + 1013904223u;
        return static_cast<float>(m_dustSeed >> 8) / static_cast<float>(1u << 24);
    };
    const auto place = [&](Speck& speck, float distance)
    {
        for (int tries = 0; tries < 16; ++tries)
        {
            speck.at = sideways * glm::mix(-across, across, random()) + up * glm::mix(-over, over, random()) + along * distance;
            // Across the ship only: it travels the ship's whole length, so a lane that crosses the hull's outline anywhere
            // goes straight through the rooms.
            const bool inside = speck.at.x > clearLo.x && speck.at.x < clearHi.x && speck.at.y > clearLo.y && speck.at.y < clearHi.y;
            if (!inside)
            {
                return;
            }
        }
        speck.at += sideways * (across * 2.0f);
    };
    if (m_dust.empty())
    {
        const MeshHandle mesh = meshes.Upload(Primitives::Box({0.05f, 0.05f, 3.2f}), "ship_dust");
        const Material glow = Material::Emissive({0.75f, 0.82f, 1.0f}, 2.6f);
        for (int i = 0; i < 700; ++i)
        {
            Speck speck;
            speck.entity = scene.CreateMeshEntity("ship_dust", Transform{}, mesh, glow);
            if (MeshRenderer* renderer = scene.GetMeshRenderer(speck.entity))
            {
                renderer->castsShadow = false;
                renderer->blocksSky = false;
                renderer->visible = false;
            }
            place(speck, glm::mix(-reach, reach, random()));
            m_dust.push_back(speck);
        }
    }
    const bool show = speed > 0.0f;
    if (show != m_dustShown)
    {
        m_dustShown = show;
        for (Speck& speck : m_dust)
        {
            if (MeshRenderer* renderer = scene.GetMeshRenderer(speck.entity))
            {
                renderer->visible = show && !m_showingStage;
            }
        }
    }
    if (!show)
    {
        return;
    }
    // The ship goes forward: what it passes goes back past it, and comes round again ahead of it.
    const glm::quat lie = glm::rotation(glm::vec3(0.0f, 0.0f, -1.0f), along);
    for (Speck& speck : m_dust)
    {
        speck.at -= along * (speed * dt);
        const float distance = glm::dot(speck.at, along);
        if (distance < -reach)
        {
            place(speck, reach + (distance + reach));
        }
        if (Transform* transform = scene.GetTransform(speck.entity))
        {
            transform->position = kOrigin + speck.at;
            transform->rotation = lie;
        }
    }
}

void ShipMap::SetEngines(Scene& scene, float burn)
{
    m_burn = std::clamp(burn, 0.0f, 1.0f);
    for (VehicleProp* hull : {&m_hull, &m_stageHull})
    {
        if (!hull->Built())
        {
            continue;
        }
        for (const ModelPart& part : hull->Model()->parts)
        {
            if (part.name.rfind("fx_engine_glow", 0) != 0)
            {
                continue;
            }
            if (MeshRenderer* renderer = scene.GetMeshRenderer(hull->Part(part.name)))
            {
                renderer->material.emissive = part.color * (part.emissive + m_burn * 7.0f);
            }
        }
    }
}

void ShipMap::Anchors(std::map<std::string, CinePose>& anchors) const
{
    if (!m_built)
    {
        return;
    }
    anchors["ship"] = Pose(glm::vec3(0.0f));
    anchors["hangar"] = Pose({0.0f, kDeck, (kDoorsFront + kDoorsBack) * 0.5f});
    anchors["ship_shuttle_home"] = m_shuttle.Home();
    anchors["cockpit"] = Pose({0.0f, kDeck, -14.5f});
    anchors["briefing"] = Pose({0.0f, kDeck, -8.0f});
    anchors["engines"] = Pose({0.0f, 1.3f, kStern + 6.0f});
    anchors["stage"] = {kStage, {1.0f, 0.0f, 0.0f, 0.0f}};
}

} // namespace pred
