#include "Game/World/ShipMap.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/Primitives.h"
#include "Game/Items/ItemDatabase.h"
#include "Game/World/LevelLights.h"
#include "Game/World/MapBuilder.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

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
const Material kDeck = Material::Diffuse({0.16f, 0.17f, 0.18f}, 0.9f);
const Material kCeiling = Material::Diffuse({0.21f, 0.22f, 0.23f}, 0.9f);
const Material kHangarWall = Material::Diffuse({0.36f, 0.37f, 0.36f}, 0.8f);
const Material kHangarDeck = Material::Diffuse({0.20f, 0.21f, 0.21f}, 0.9f);
const Material kFrame = Material::Metal({0.14f, 0.14f, 0.15f}, 0.55f);
const Material kRib = Material::Metal({0.24f, 0.25f, 0.26f}, 0.5f);
const Material kHazardYellow = Material::Diffuse({0.72f, 0.55f, 0.10f}, 0.7f);
const Material kHazardBlack = Material::Diffuse({0.06f, 0.06f, 0.06f}, 0.8f);
const Material kPipe = Material::Metal({0.40f, 0.40f, 0.41f}, 0.45f);
const Material kCrate = Material::Diffuse({0.30f, 0.34f, 0.26f}, 0.85f);
const Material kCrateDark = Material::Diffuse({0.20f, 0.22f, 0.24f}, 0.85f);
const Material kFurniture = Material::Metal({0.28f, 0.29f, 0.31f}, 0.6f);
const Material kTableTop = Material::Diffuse({0.46f, 0.45f, 0.42f}, 0.6f);
const Material kCushion = Material::Diffuse({0.22f, 0.25f, 0.30f}, 0.95f);
const Material kMattress = Material::Diffuse({0.52f, 0.52f, 0.49f}, 0.95f);
const Material kStair = Material::Diffuse({0.26f, 0.27f, 0.28f}, 0.85f);
const Material kHull = Material::Diffuse({0.40f, 0.41f, 0.41f}, 0.7f);
const Material kHullPanel = Material::Diffuse({0.31f, 0.32f, 0.33f}, 0.75f);
const Material kHullDark = Material::Metal({0.17f, 0.18f, 0.19f}, 0.55f);
const Material kNozzle = Material::Metal({0.12f, 0.12f, 0.13f}, 0.4f);

Material Glow(const glm::vec3& colour, float strength)
{
    Material material = Material::Diffuse(colour, 0.4f);
    material.emissive = colour * strength;
    return material;
}

const Material kScreen = Glow({0.06f, 0.16f, 0.26f}, 0.45f);
const Material kScreenWarm = Glow({0.30f, 0.20f, 0.08f}, 0.45f);
const Material kScreenBig = Glow({0.03f, 0.07f, 0.12f}, 0.4f);
const Material kIndicator = Glow({0.2f, 0.9f, 0.4f}, 2.0f);
const Material kWindowGlow = Glow({0.95f, 0.78f, 0.5f}, 1.4f);

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

// A quad with its face outward from `centre`, whichever way its corners were given.
void Quad(MeshData& mesh, const glm::vec3& a, glm::vec3 b, const glm::vec3& c, glm::vec3 d, const glm::vec3& centre)
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
    void Mesh(const char* name, const MeshData& mesh, const Material& material, bool solid = false)
    {
        m_map.AddMesh(name, At(glm::vec3(0.0f)), mesh, material, solid);
    }

    // A wall running across the ship (along x) between z0 and z1, and one running along it (along z) between x0 and x1,
    // with gaps in it: doorways from the floor, windows with a sill.
    void WallX(const char* name, float x0, float x1, float z0, float z1, float y0, float y1, std::vector<Opening> gaps, const Material& material,
               bool solid = true)
    {
        Wall(name, x0, x1, y0, y1, std::move(gaps), material, solid, [&](float a, float b, float lo, float hi)
             { return std::pair{glm::vec3(a, lo, z0), glm::vec3(b, hi, z1)}; });
    }
    void WallZ(const char* name, float z0, float z1, float x0, float x1, float y0, float y1, std::vector<Opening> gaps, const Material& material,
               bool solid = true)
    {
        Wall(name, z0, z1, y0, y1, std::move(gaps), material, solid, [&](float a, float b, float lo, float hi)
             { return std::pair{glm::vec3(x0, lo, a), glm::vec3(x1, hi, b)}; });
    }

private:
    template <typename Corners>
    void Wall(const char* name, float from, float to, float y0, float y1, std::vector<Opening> gaps, const Material& material, bool solid,
              Corners corners)
    {
        std::sort(gaps.begin(), gaps.end(), [](const Opening& a, const Opening& b) { return a.from < b.from; });
        const auto piece = [&](float a, float b, float lo, float hi)
        {
            if (b - a < 0.005f || hi - lo < 0.005f)
            {
                return;
            }
            const auto [low, high] = corners(a, b, lo, hi);
            solid ? Solid(name, low, high, material) : Shape(name, low, high, material);
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

// A block narrowing from its back face to its front one, both upright rectangles across x, centred on it: the bow.
MeshData Tapered(float backZ, float backHalf, float backLow, float backHigh, float frontZ, float frontHalf, float frontLow, float frontHigh)
{
    const glm::vec3 b0{-backHalf, backLow, backZ}, b1{backHalf, backLow, backZ}, b2{backHalf, backHigh, backZ}, b3{-backHalf, backHigh, backZ};
    const glm::vec3 f0{-frontHalf, frontLow, frontZ}, f1{frontHalf, frontLow, frontZ}, f2{frontHalf, frontHigh, frontZ},
        f3{-frontHalf, frontHigh, frontZ};
    const glm::vec3 centre = (b0 + b1 + b2 + b3 + f0 + f1 + f2 + f3) / 8.0f;
    MeshData mesh;
    Quad(mesh, b0, b1, b2, b3, centre);
    Quad(mesh, f0, f1, f2, f3, centre);
    Quad(mesh, b0, b1, f1, f0, centre);
    Quad(mesh, b3, b2, f2, f3, centre);
    Quad(mesh, b0, b3, f3, f0, centre);
    Quad(mesh, b1, b2, f2, f1, centre);
    return mesh;
}

// The decks, in the ship's frame.
constexpr float kLower = kLowerDeck;          // lower deck floor
constexpr float kLowerTop = 3.0f;             // its ceiling
constexpr float kUpper = kUpperDeck;          // upper deck floor
constexpr float kUpperTop = 6.6f;             // its ceiling
constexpr float kRoof = 7.2f;                 // the top of the roof over it
constexpr float kHangarTop = 8.0f;            // the hangar's ceiling
constexpr float kHangarRoof = 8.6f;
constexpr float kSlab = 0.6f;
constexpr float kDoorTop = 2.3f;              // a doorway's height
// The bay in the hangar floor the shuttle drops through.
constexpr float kBayHalfX = 4.5f;
constexpr float kBayFront = 13.0f;
constexpr float kBayBack = 27.0f;
// The stairs: up to starboard from the corridor, climbing towards +x.
constexpr float kStairFoot = 3.06f;
constexpr float kStairTop = 8.1f;
constexpr float kStairZ0 = 1.3f;
constexpr float kStairZ1 = 3.7f;

void BuildStructure(Builder& b)
{
    // --- Floors, ceilings, roofs -------------------------------------------------------------------------------
    b.Solid("ship_deck", {-12.3f, kLower - kSlab, -22.3f}, {12.3f, kLower, 5.85f}, kDeck);
    // The hangar's, round the bay.
    b.Solid("ship_hangar_deck", {-12.3f, kLower - kSlab, 5.85f}, {-kBayHalfX, kLower, 34.3f}, kHangarDeck);
    b.Solid("ship_hangar_deck", {kBayHalfX, kLower - kSlab, 5.85f}, {12.3f, kLower, 34.3f}, kHangarDeck);
    b.Solid("ship_hangar_deck", {-kBayHalfX, kLower - kSlab, 5.85f}, {kBayHalfX, kLower, kBayFront}, kHangarDeck);
    b.Solid("ship_hangar_deck", {-kBayHalfX, kLower - kSlab, kBayBack}, {kBayHalfX, kLower, 34.3f}, kHangarDeck);
    // Between the decks: the lower's ceiling and the upper's floor, open over the stairs.
    b.Solid("ship_between", {-12.3f, kLowerTop, -22.3f}, {12.3f, kUpper, kStairZ0}, kDeck);
    b.Solid("ship_between", {-12.3f, kLowerTop, kStairZ1}, {12.3f, kUpper, 5.85f}, kDeck);
    b.Solid("ship_between", {-12.3f, kLowerTop, kStairZ0}, {1.8f, kUpper, kStairZ1}, kDeck);
    b.Solid("ship_between", {kStairTop, kLowerTop, kStairZ0}, {12.3f, kUpper, kStairZ1}, kDeck);
    b.Solid("ship_between", {-8.3f, kLowerTop, -40.3f}, {8.3f, kUpper, -22.3f}, kDeck);
    b.Solid("ship_roof", {-12.3f, kUpperTop, -22.3f}, {12.3f, kRoof, 5.85f}, kCeiling);
    b.Solid("ship_roof", {-8.3f, kUpperTop, -40.3f}, {8.3f, kRoof, -22.3f}, kCeiling);
    b.Solid("ship_hangar_roof", {-12.3f, kHangarTop, 5.85f}, {12.3f, kHangarRoof, 34.3f}, kCeiling);

    // --- The lower deck ------------------------------------------------------------------------------------
    b.Solid("ship_wall", {-12.3f, kLower, -22.3f}, {-12.0f, kLowerTop, 5.85f}, kWall);
    b.Solid("ship_wall", {12.0f, kLower, -22.3f}, {12.3f, kLowerTop, 5.85f}, kWall);
    b.Solid("ship_wall", {-12.0f, kLower, -22.3f}, {12.0f, kLowerTop, -22.0f}, kWall);
    // The corridor: the gear room and the quarters off it to port, the stairs and the mess to starboard.
    b.WallZ("ship_wall", -22.0f, 5.85f, -1.8f, -1.5f, kLower, kLowerTop,
            {{-0.8f, 0.8f, kLower, kDoorTop}, {-14.8f, -13.2f, kLower, kDoorTop}}, kWall);
    b.WallZ("ship_wall", -22.0f, 5.85f, 1.5f, 1.8f, kLower, kLowerTop,
            {{1.5f, 3.5f, kLower, kDoorTop}, {-12.8f, -11.2f, kLower, kDoorTop}}, kWall);
    b.Solid("ship_wall", {-12.0f, kLower, -6.15f}, {-1.8f, kLowerTop, -5.85f}, kWall);
    b.Solid("ship_wall", {1.8f, kLower, -1.15f}, {12.0f, kLowerTop, -0.85f}, kWall);

    // The stairs, up to the upper deck's landing, and rails round the well at the top.
    b.Mesh("ship_stairs", [] {
        MeshData stairs;
        const glm::mat4 turn = glm::translate(glm::mat4(1.0f), glm::vec3(kStairFoot, kLower, 2.5f)) *
                               glm::mat4_cast(glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f)));
        stairs.Append(Primitives::Stairs(18, 2.0f, 0.2f, 0.28f), turn);
        return stairs;
    }(), kStair, true);
    const float rail = kUpper + 1.05f;
    for (const float z : {kStairZ0 - 0.05f, kStairZ1 + 0.05f})
    {
        b.Shape("ship_rail", {1.8f, rail - 0.05f, z - 0.025f}, {kStairTop - 0.2f, rail, z + 0.025f}, kPipe);
        b.Shape("ship_rail", {1.8f, kUpper + 0.5f, z - 0.02f}, {kStairTop - 0.2f, kUpper + 0.54f, z + 0.02f}, kPipe);
        for (float x = 1.85f; x < kStairTop - 0.1f; x += 1.25f)
        {
            b.Shape("ship_rail_post", {x - 0.025f, kUpper, z - 0.025f}, {x + 0.025f, rail, z + 0.025f}, kPipe);
        }
        b.Barrier({1.8f, kUpper, z - 0.03f}, {kStairTop - 0.2f, rail, z + 0.03f});
    }
    b.Shape("ship_rail", {1.74f, rail - 0.05f, kStairZ0}, {1.79f, rail, kStairZ1}, kPipe);
    b.Barrier({1.74f, kUpper, kStairZ0}, {1.8f, rail, kStairZ1});

    // --- The hangar: as tall as both decks, the bay in its floor ------------------------------------------------
    // Its forward wall, with the way in from the corridor and the gallery's window high up on the port side.
    b.WallX("ship_hangar_wall", -12.3f, 12.3f, 5.85f, 6.15f, kLower, kHangarTop,
            {{-1.2f, 1.2f, kLower, 2.4f}, {-10.0f, -3.0f, kUpper + 1.0f, kUpper + 2.6f}}, kHangarWall);
    b.Barrier({-10.0f, kUpper + 1.0f, 5.95f}, {-3.0f, kUpper + 2.6f, 6.05f});
    b.Solid("ship_hangar_wall", {-12.3f, kLower, 6.15f}, {-12.0f, kHangarTop, 34.0f}, kHangarWall);
    b.Solid("ship_hangar_wall", {12.0f, kLower, 6.15f}, {12.3f, kHangarTop, 34.0f}, kHangarWall);
    b.Solid("ship_hangar_wall", {-12.3f, kLower, 34.0f}, {12.3f, kHangarTop, 34.3f}, kHangarWall);
    // Ribs down its walls, and girders across under its roof.
    for (float z = 9.0f; z < 33.0f; z += 4.0f)
    {
        b.Solid("ship_hangar_rib", {-12.0f, kLower, z - 0.2f}, {-11.75f, kHangarTop, z + 0.2f}, kRib);
        b.Solid("ship_hangar_rib", {11.75f, kLower, z - 0.2f}, {12.0f, kHangarTop, z + 0.2f}, kRib);
        b.Shape("ship_hangar_girder", {-11.75f, kHangarTop - 0.5f, z - 0.15f}, {11.75f, kHangarTop, z + 0.15f}, kRib);
    }
    // Yellow and black round the bay, for where not to stand.
    {
        int stripe = 0;
        const auto band = [&](float x0, float z0, float x1, float z1)
        {
            b.Shape("ship_hazard", {x0, kLower, z0}, {x1, kLower + 0.006f, z1}, (stripe++ % 2 == 0) ? kHazardYellow : kHazardBlack);
        };
        for (float x = -kBayHalfX - 0.4f; x < kBayHalfX + 0.4f - 0.01f; x += 0.7f)
        {
            const float to = std::min(x + 0.7f, kBayHalfX + 0.4f);
            band(x, kBayFront - 0.4f, to, kBayFront);
            band(x, kBayBack, to, kBayBack + 0.4f);
        }
        for (float z = kBayFront; z < kBayBack - 0.01f; z += 0.7f)
        {
            const float to = std::min(z + 0.7f, kBayBack);
            band(-kBayHalfX - 0.4f, z, -kBayHalfX, to);
            band(kBayHalfX, z, kBayHalfX + 0.4f, to);
        }
    }
    // The bay's shaft through the hull, lined.
    b.Shape("ship_bay_shaft", {-kBayHalfX - 0.05f, -1.4f, kBayFront}, {-kBayHalfX, kLower - kSlab, kBayBack}, kHullDark);
    b.Shape("ship_bay_shaft", {kBayHalfX, -1.4f, kBayFront}, {kBayHalfX + 0.05f, kLower - kSlab, kBayBack}, kHullDark);
    b.Shape("ship_bay_shaft", {-kBayHalfX, -1.4f, kBayFront - 0.05f}, {kBayHalfX, kLower - kSlab, kBayFront}, kHullDark);
    b.Shape("ship_bay_shaft", {-kBayHalfX, -1.4f, kBayBack}, {kBayHalfX, kLower - kSlab, kBayBack + 0.05f}, kHullDark);

    // --- The upper deck --------------------------------------------------------------------------------------
    b.Solid("ship_wall", {-12.3f, kUpper, -22.3f}, {-12.0f, kUpperTop, 5.85f}, kWall);
    b.Solid("ship_wall", {12.0f, kUpper, -22.3f}, {12.3f, kUpperTop, 5.85f}, kWall);
    // The gallery to port of the corridor, the landing open to starboard.
    b.WallZ("ship_wall", -3.85f, 5.85f, -1.8f, -1.5f, kUpper, kUpperTop, {{-0.8f, 0.8f, kUpper, kUpper + kDoorTop}}, kWall);
    // Into the briefing room, and out of it forward.
    b.WallX("ship_wall", -12.0f, 12.0f, -4.15f, -3.85f, kUpper, kUpperTop, {{-1.0f, 1.0f, kUpper, kUpper + kDoorTop}}, kWall);
    b.WallX("ship_wall", -12.0f, 12.0f, -22.3f, -22.0f, kUpper, kUpperTop, {{-1.0f, 1.0f, kUpper, kUpper + kDoorTop}}, kWall);
    // The forward corridor, and the cockpit at the end of it: windows ahead and to either side.
    b.Solid("ship_wall", {-1.8f, kUpper, -30.0f}, {-1.5f, kUpperTop, -22.3f}, kWall);
    b.Solid("ship_wall", {1.5f, kUpper, -30.0f}, {1.8f, kUpperTop, -22.3f}, kWall);
    b.WallX("ship_wall", -8.0f, 8.0f, -30.3f, -30.0f, kUpper, kUpperTop, {{-1.0f, 1.0f, kUpper, kUpper + kDoorTop}}, kWall);
    const Opening side{-38.5f, -31.5f, kUpper + 0.9f, kUpper + 2.5f};
    b.WallZ("ship_wall", -40.3f, -22.3f, -8.3f, -8.0f, kUpper, kUpperTop, {side}, kWall);
    b.WallZ("ship_wall", -40.3f, -22.3f, 8.0f, 8.3f, kUpper, kUpperTop, {side}, kWall);
    const Opening ahead{-6.8f, 6.8f, kUpper + 0.7f, kUpper + 2.7f};
    b.WallX("ship_wall", -8.3f, 8.3f, -40.3f, -40.0f, kUpper, kUpperTop, {ahead}, kWall);
    b.Barrier({-8.3f, side.bottom, side.from}, {-8.0f, side.top, side.to});
    b.Barrier({8.0f, side.bottom, side.from}, {8.3f, side.top, side.to});
    b.Barrier({ahead.from, ahead.bottom, -40.3f}, {ahead.to, ahead.top, -40.0f});
    // The window frames: a mullion or two, so it reads as a window rather than a hole.
    for (const float x : {-2.3f, 2.3f})
    {
        b.Shape("ship_mullion", {x - 0.06f, ahead.bottom, -40.3f}, {x + 0.06f, ahead.top, -40.0f}, kFrame);
    }
    for (const float x : {-8.3f, 8.0f})
    {
        b.Shape("ship_mullion", {x, side.bottom, -35.06f}, {x + 0.3f, side.top, -34.94f}, kFrame);
    }
}

// Conduit along a wall at a height, from one end to the other along z or x.
void Conduit(Builder& b, const glm::vec3& from, const glm::vec3& to, float thickness)
{
    const glm::vec3 lo = glm::min(from, to) - glm::vec3(thickness * 0.5f);
    const glm::vec3 hi = glm::max(from, to) + glm::vec3(thickness * 0.5f);
    b.Shape("ship_conduit", lo, hi, kPipe);
}

// A chair facing `yaw` (degrees; 0 faces -z), on a floor.
void Chair(Builder& b, float x, float floor, float z, float yaw)
{
    b.SolidTurned("ship_chair_seat", {x, floor + 0.23f, z}, {0.46f, 0.46f, 0.46f}, yaw, kFurniture);
    const float r = glm::radians(yaw);
    const glm::vec3 back{std::sin(r) * 0.21f, 0.0f, std::cos(r) * 0.21f};
    b.ShapeTurned("ship_chair_back", glm::vec3(x, floor + 0.72f, z) + back, {0.46f, 0.52f, 0.05f}, yaw, kCushion);
    b.ShapeTurned("ship_chair_cushion", {x, floor + 0.475f, z}, {0.42f, 0.03f, 0.42f}, yaw, kCushion);
}

// A table: a top and a solid underneath, which nobody wants to crawl under anyway.
void Table(Builder& b, float x0, float x1, float z0, float z1, float floor, float top)
{
    b.Solid("ship_table", {x0 + 0.1f, floor, z0 + 0.1f}, {x1 - 0.1f, floor + top - 0.04f, z1 - 0.1f}, kFrame);
    b.Solid("ship_table_top", {x0, floor + top - 0.04f, z0}, {x1, floor + top, z1}, kTableTop);
}

void BuildRooms(Builder& b)
{
    // --- The gear room: lockers, the bench the kit is laid on, a rack, cabinets --------------------------------
    b.Solid("ship_bench", {-10.8f, kLower, -5.8f}, {-3.2f, kLower + 0.9f, -5.1f}, kFurniture);
    b.Solid("ship_bench_top", {-10.85f, kLower + 0.9f, -5.82f}, {-3.15f, kLower + 0.92f, -5.08f}, kTableTop);
    b.Solid("ship_rack", {-10.0f, kLower + 0.9f, 5.6f}, {-4.0f, kLower + 2.3f, 5.85f}, kWallDark);
    for (float x = -9.7f; x < -4.1f; x += 0.6f)
    {
        b.Shape("ship_rack_slot", {x - 0.03f, kLower + 1.0f, 5.5f}, {x + 0.03f, kLower + 2.2f, 5.6f}, kFrame);
    }
    for (float z = -4.6f; z < 0.7f; z += 1.8f)
    {
        b.Solid("ship_cabinet", {-11.97f, kLower, z}, {-11.45f, kLower + 2.0f, z + 1.75f}, kFurniture);
        b.Shape("ship_cabinet_line", {-11.46f, kLower + 0.1f, z + 0.86f}, {-11.44f, kLower + 1.9f, z + 0.89f}, kFrame);
    }
    b.Solid("ship_seat", {-8.5f, kLower, 0.8f}, {-5.5f, kLower + 0.45f, 1.2f}, kFurniture);
    Conduit(b, {-11.85f, 2.75f, -5.7f}, {-11.85f, 2.75f, 5.7f}, 0.14f);

    // --- The crew quarters: bunks down the hull side, lockers opposite, a table in the middle -------------------
    {
        int bunk = 0;
        for (float z = -6.5f; z > -20.6f; z -= 2.4f, ++bunk)
        {
            const float z0 = z - 2.0f;
            const float z1 = z;
            for (const float zp : {z0 + 0.03f, z1 - 0.03f})
            {
                b.Shape("ship_bunk_post", {-11.95f, kLower, zp - 0.03f}, {-11.89f, kLower + 2.3f, zp + 0.03f}, kFrame);
                b.Shape("ship_bunk_post", {-11.06f, kLower, zp - 0.03f}, {-11.0f, kLower + 2.3f, zp + 0.03f}, kFrame);
            }
            for (const float level : {0.35f, 1.5f})
            {
                b.Solid("ship_bunk", {-11.97f, kLower + level, z0}, {-11.0f, kLower + level + 0.12f, z1}, kFrame);
                b.Shape("ship_mattress", {-11.9f, kLower + level + 0.12f, z0 + 0.05f}, {-11.05f, kLower + level + 0.28f, z1 - 0.05f}, kMattress);
                // A blanket over most of it, each its own colour, and the pillow at the far end.
                const float hue = static_cast<float>((bunk * 3 + static_cast<int>(level * 2.0f)) % 5) / 5.0f;
                const Material blanket = Material::Diffuse({0.22f + 0.2f * hue, 0.26f + 0.06f * (1.0f - hue), 0.32f - 0.12f * hue}, 0.95f);
                b.Shape("ship_blanket", {-11.92f, kLower + level + 0.28f, z0 + 0.1f}, {-11.03f, kLower + level + 0.33f, z1 - 0.55f}, blanket);
                b.Shape("ship_pillow", {-11.8f, kLower + level + 0.28f, z1 - 0.5f}, {-11.15f, kLower + level + 0.4f, z1 - 0.12f}, kMattress);
            }
            // Something stowed under the bottom one.
            b.Shape("ship_bag", {-11.85f, kLower, z0 + 0.3f + static_cast<float>(bunk % 3) * 0.3f},
                    {-11.3f, kLower + 0.3f, z0 + 0.8f + static_cast<float>(bunk % 3) * 0.3f}, kCrateDark);
        }
        // Personal lockers on the corridor side, clear of the door.
        const float lockerZ[] = {-6.5f, -8.3f, -10.1f, -15.3f, -17.1f, -18.9f};
        int n = 0;
        for (const float z : lockerZ)
        {
            const float hue = static_cast<float>(n++ % 3);
            const Material paint = Material::Diffuse({0.28f + 0.03f * hue, 0.30f, 0.31f - 0.02f * hue}, 0.8f);
            b.Solid("ship_locker", {-2.4f, kLower, z - 1.7f}, {-1.83f, kLower + 2.0f, z - 0.05f}, paint);
            b.Shape("ship_locker_line", {-2.42f, kLower + 0.1f, z - 0.9f}, {-2.39f, kLower + 1.9f, z - 0.86f}, kFrame);
        }
        Table(b, -7.6f, -6.0f, -15.0f, -13.0f, kLower, 0.76f);
        Chair(b, -8.2f, kLower, -14.0f, -90.0f);
        Chair(b, -5.4f, kLower, -14.0f, 90.0f);
        Chair(b, -6.8f, kLower, -15.6f, 180.0f);
        // What people leave on a table.
        b.Shape("ship_mug", {-7.3f, kLower + 0.76f, -14.4f}, {-7.2f, kLower + 0.86f, -14.3f}, kPipe);
        b.Shape("ship_mug", {-6.5f, kLower + 0.76f, -13.5f}, {-6.4f, kLower + 0.86f, -13.4f}, kHazardYellow);
        b.Shape("ship_cards", {-7.0f, kLower + 0.76f, -14.0f}, {-6.85f, kLower + 0.78f, -13.8f}, kMattress);
        // A plant somebody keeps alive.
        b.Shape("ship_pot", {-2.9f, kLower, -21.9f}, {-2.5f, kLower + 0.35f, -21.5f}, kCrate);
        b.Shape("ship_plant", {-2.95f, kLower + 0.35f, -21.95f}, {-2.45f, kLower + 0.8f, -21.45f},
                Material::Diffuse({0.16f, 0.32f, 0.14f}, 0.9f));
        Conduit(b, {-11.85f, 2.8f, -21.8f}, {-11.85f, 2.8f, -6.3f}, 0.12f);
    }

    // --- The mess: the galley along the hull, tables either side of an aisle from the door -----------------------
    {
        b.Solid("ship_counter", {10.95f, kLower, -19.5f}, {11.97f, kLower + 0.9f, -6.0f}, kFurniture);
        b.Solid("ship_counter_top", {10.9f, kLower + 0.9f, -19.55f}, {11.97f, kLower + 0.95f, -5.95f}, kTableTop);
        b.Solid("ship_cupboard", {11.5f, kLower + 1.6f, -19.5f}, {11.97f, kLower + 2.4f, -6.0f}, kFurniture);
        b.Solid("ship_fridge", {10.9f, kLower, -21.9f}, {11.97f, kLower + 2.1f, -20.1f}, Material::Metal({0.55f, 0.56f, 0.57f}, 0.35f));
        b.Shape("ship_coffee", {11.3f, kLower + 0.95f, -8.4f}, {11.8f, kLower + 1.45f, -8.0f}, kFrame);
        b.Shape("ship_coffee_light", {11.28f, kLower + 1.3f, -8.3f}, {11.3f, kLower + 1.34f, -8.26f}, kIndicator);
        b.Shape("ship_sink", {11.1f, kLower + 0.951f, -13.5f}, {11.8f, kLower + 0.955f, -12.5f}, kFrame);
        for (const float x : {4.4f, 8.0f})
        {
            for (const auto& [z0, z1] : {std::pair{-19.0f, -13.2f}, std::pair{-10.8f, -5.0f}})
            {
                Table(b, x, x + 1.0f, z0, z1, kLower, 0.76f);
                b.Solid("ship_mess_bench", {x - 0.85f, kLower, z0}, {x - 0.45f, kLower + 0.45f, z1}, kFurniture);
                b.Solid("ship_mess_bench", {x + 1.45f, kLower, z0}, {x + 1.85f, kLower + 0.45f, z1}, kFurniture);
            }
        }
        b.Shape("ship_tray", {4.6f, kLower + 0.76f, -17.5f}, {5.0f, kLower + 0.78f, -17.2f}, kPipe);
        b.Shape("ship_tray", {8.2f, kLower + 0.76f, -8.0f}, {8.6f, kLower + 0.78f, -7.7f}, kPipe);
        b.Shape("ship_mug", {5.1f, kLower + 0.76f, -7.0f}, {5.2f, kLower + 0.86f, -6.9f}, kPipe);
        b.Shape("ship_mess_screen", {1.82f, kLower + 1.2f, -20.5f}, {1.84f, kLower + 2.1f, -19.0f}, kScreenWarm);
    }

    // --- The stair hall: crates under the landing --------------------------------------------------------------
    b.Solid("ship_crate", {9.4f, kLower, 3.9f}, {11.9f, kLower + 1.2f, 5.7f}, kCrate);
    b.Solid("ship_crate", {10.2f, kLower + 1.2f, 4.3f}, {11.6f, kLower + 2.0f, 5.5f}, kCrateDark);

    // --- The gallery over the hangar: a console under its window, chairs at it ------------------------------------
    b.Solid("ship_gallery_desk", {-9.5f, kUpper, 4.9f}, {-3.5f, kUpper + 0.85f, 5.8f}, kFurniture);
    b.Shape("ship_gallery_screen", {-9.2f, kUpper + 0.851f, 5.1f}, {-6.8f, kUpper + 0.86f, 5.6f}, kScreen);
    b.Shape("ship_gallery_screen", {-6.2f, kUpper + 0.851f, 5.1f}, {-3.8f, kUpper + 0.86f, 5.6f}, kScreen);
    Chair(b, -8.0f, kUpper, 4.2f, 180.0f);
    Chair(b, -5.0f, kUpper, 4.2f, 180.0f);

    // --- The briefing room: the screen ahead, the console before it, the table --------------------------------
    b.Shape("ship_briefing_frame", {-4.7f, kUpper + 0.45f, -22.0f}, {4.7f, kUpper + 2.8f, -21.93f}, kFrame);
    b.Shape("ship_briefing_screen", {-4.5f, kUpper + 0.6f, -21.93f}, {4.5f, kUpper + 2.65f, -21.9f}, kScreenBig);
    Table(b, -3.5f, 3.5f, -13.5f, -9.5f, kUpper, 0.78f);
    for (const float x : {-2.4f, -0.8f, 0.8f, 2.4f})
    {
        Chair(b, x, kUpper, -14.15f, 180.0f);
        Chair(b, x, kUpper, -8.85f, 0.0f);
    }
    for (const float x : {-11.97f, 11.9f})
    {
        b.Shape("ship_wall_screen", {x, kUpper + 0.9f, -18.0f}, {x + 0.07f, kUpper + 2.2f, -13.5f}, kScreen);
    }
    Conduit(b, {-11.8f, kUpperTop - 0.25f, -21.9f}, {-11.8f, kUpperTop - 0.25f, -4.3f}, 0.16f);
    Conduit(b, {11.8f, kUpperTop - 0.25f, -21.9f}, {11.8f, kUpperTop - 0.25f, -4.3f}, 0.16f);

    // --- The cockpit: a console under the windows, the two seats, the side stations -----------------------------
    b.Solid("ship_helm", {-6.5f, kUpper, -39.95f}, {6.5f, kUpper + 0.65f, -39.0f}, kFurniture);
    for (float x = -6.1f; x < 6.0f; x += 1.55f)
    {
        b.Shape("ship_helm_screen", {x, kUpper + 0.651f, -39.8f}, {x + 1.3f, kUpper + 0.66f, -39.2f}, (static_cast<int>(x + 7.0f) % 3 == 0) ? kScreenWarm : kScreen);
    }
    for (const float x : {-1.6f, 1.6f})
    {
        Chair(b, x, kUpper, -37.3f, 0.0f);
        b.Shape("ship_seat_arm", {x - 0.3f, kUpper + 0.45f, -37.55f}, {x - 0.24f, kUpper + 0.7f, -37.05f}, kFrame);
        b.Shape("ship_seat_arm", {x + 0.24f, kUpper + 0.45f, -37.55f}, {x + 0.3f, kUpper + 0.7f, -37.05f}, kFrame);
    }
    for (const float x : {-7.97f, 6.95f})
    {
        b.Solid("ship_station", {x, kUpper, -31.3f}, {x + 1.02f, kUpper + 0.85f, -30.35f}, kFurniture);
        b.Shape("ship_station_screen", {x + 0.1f, kUpper + 0.851f, -31.2f}, {x + 0.92f, kUpper + 0.86f, -30.45f}, kScreen);
    }
    b.Shape("ship_overhead", {-2.0f, kUpperTop - 0.2f, -38.6f}, {2.0f, kUpperTop, -36.0f}, kFrame);
    for (float x = -1.7f; x < 1.8f; x += 0.5f)
    {
        b.Shape("ship_overhead_light", {x, kUpperTop - 0.205f, -37.5f}, {x + 0.06f, kUpperTop - 0.2f, -37.44f}, kIndicator);
    }

    // --- The hangar: crates, a hose reel, pipes along the starboard wall ----------------------------------------
    b.Solid("ship_crate", {-11.7f, kLower, 7.0f}, {-9.6f, kLower + 1.6f, 9.4f}, kCrate);
    b.Solid("ship_crate", {-11.7f, kLower + 1.6f, 7.3f}, {-10.2f, kLower + 2.6f, 8.8f}, kCrateDark);
    b.Solid("ship_crate", {-11.7f, kLower, 29.0f}, {-9.2f, kLower + 1.4f, 32.0f}, kCrateDark);
    b.Solid("ship_crate", {9.4f, kLower, 30.0f}, {11.7f, kLower + 1.8f, 33.7f}, kCrate);
    b.Solid("ship_reel", {10.6f, kLower, 9.0f}, {11.7f, kLower + 1.3f, 10.6f}, kHazardYellow);
    Conduit(b, {11.65f, 1.2f, 11.0f}, {11.65f, 1.2f, 33.8f}, 0.2f);
    Conduit(b, {11.65f, 1.5f, 11.0f}, {11.65f, 1.5f, 33.8f}, 0.12f);
    b.Shape("ship_hangar_panel", {-11.74f, 1.2f, 16.0f}, {-11.7f, 2.4f, 18.0f}, kScreenWarm);
}

void BuildHull(Builder& b, std::vector<Entity>& glowOut, std::vector<glm::vec3>& colourOut, Scene& scene, MeshLibrary& meshes)
{
    // The skin, outside everything inside: the main body, taller over the hangar.
    const float out = 13.0f;
    for (const float side : {-1.0f, 1.0f})
    {
        const float x0 = side < 0.0f ? -out : 12.3f;
        const float x1 = side < 0.0f ? -12.3f : out;
        b.Shape("ship_skin", {x0, -1.4f, -22.3f}, {x1, 7.6f, 5.85f}, kHull);
        b.Shape("ship_skin", {x0, -1.4f, 5.85f}, {x1, 9.0f, 34.6f}, kHull);
        // The hangar's sides stand out from the rest: sponsons, dark, with the bay's machinery in them.
        b.Shape("ship_sponson", {side < 0.0f ? -out - 1.2f : out, -1.0f, 8.0f}, {side < 0.0f ? -out : out + 1.2f, 6.5f, 32.0f}, kHullDark);
        b.Shape("ship_sponson_cap", {side < 0.0f ? -out - 1.3f : out, 6.5f, 7.5f}, {side < 0.0f ? -out : out + 1.3f, 6.9f, 32.5f}, kHullPanel);
        // A darker belt along it at the deck line, and panels proud of it here and there.
        b.Shape("ship_belt", {side < 0.0f ? -out - 0.05f : out, 2.8f, -22.3f}, {side < 0.0f ? -out : out + 0.05f, 3.4f, 34.6f}, kHullDark);
        for (float z = -20.0f; z < 33.0f; z += 7.0f)
        {
            b.Shape("ship_panel", {side < 0.0f ? -out - 0.04f : out, 4.2f, z}, {side < 0.0f ? -out : out + 0.04f, 6.8f, z + 5.5f}, kHullPanel);
            b.Shape("ship_panel", {side < 0.0f ? -out - 0.04f : out, -0.8f, z + 1.0f}, {side < 0.0f ? -out : out + 0.04f, 2.2f, z + 4.0f}, kHullPanel);
        }
        // Lit windows along it: people aboard.
        for (float z = -19.0f; z < 4.0f; z += 3.2f)
        {
            b.Shape("ship_porthole", {side < 0.0f ? -out - 0.06f : out, 5.0f, z}, {side < 0.0f ? -out : out + 0.06f, 5.5f, z + 0.9f}, kWindowGlow);
        }
        // Radiators up off the hangar roof.
        for (const float x : {7.0f, 10.0f})
        {
            b.Shape("ship_radiator", {side * x - 0.06f, 9.0f, 10.0f}, {side * x + 0.06f, 10.8f, 30.0f}, kHullDark);
        }
    }
    b.Shape("ship_skin", {-out, 7.2f, -22.3f}, {out, 7.6f, 5.85f}, kHull);
    // A spine down the middle of the roof, and plating on the roof either side of it.
    b.Shape("ship_spine", {-2.2f, 7.6f, -30.0f}, {2.2f, 8.4f, 5.55f}, kHullPanel);
    b.Shape("ship_spine", {-2.2f, 9.0f, 5.85f}, {2.2f, 9.6f, 33.0f}, kHullPanel);
    for (float z = -20.0f; z < 4.0f; z += 6.0f)
    {
        b.Shape("ship_roof_plate", {-11.5f, 7.6f, z}, {-3.0f, 7.68f, z + 4.8f}, kHullPanel);
        b.Shape("ship_roof_plate", {3.0f, 7.6f, z + 1.0f}, {11.5f, 7.68f, z + 5.8f}, kHullPanel);
    }
    b.Shape("ship_skin", {-out, 8.6f, 5.55f}, {out, 9.0f, 34.6f}, kHull);
    b.Shape("ship_skin", {-out, 7.6f, 5.55f}, {out, 8.6f, 5.85f}, kHull);
    b.Shape("ship_skin", {-out, -1.4f, 34.3f}, {out, 9.0f, 34.6f}, kHullDark);
    // The belly, open under the bay.
    b.Shape("ship_belly", {-out, -1.4f, -22.3f}, {out, -0.6f, kBayFront}, kHull);
    b.Shape("ship_belly", {-out, -1.4f, kBayBack}, {out, -0.6f, 34.6f}, kHull);
    b.Shape("ship_belly", {-out, -1.4f, kBayFront}, {-kBayHalfX - 0.05f, -0.6f, kBayBack}, kHull);
    b.Shape("ship_belly", {kBayHalfX + 0.05f, -1.4f, kBayFront}, {out, -0.6f, kBayBack}, kHull);
    // The step down from the main body to the cockpit block.
    for (const float side : {-1.0f, 1.0f})
    {
        b.Shape("ship_skin", {side < 0.0f ? -out : 8.3f, 3.0f, -22.6f}, {side < 0.0f ? -8.3f : out, 7.6f, -22.3f}, kHull);
    }
    // The cockpit block, its windows through it.
    const float cockpit = 9.0f;
    const Opening side{-38.5f, -31.5f, kUpper + 0.9f, kUpper + 2.5f};
    b.WallZ("ship_skin", -40.6f, -22.6f, -cockpit, -8.3f, 3.0f, 7.6f, {side}, kHull, false);
    b.WallZ("ship_skin", -40.6f, -22.6f, 8.3f, cockpit, 3.0f, 7.6f, {side}, kHull, false);
    b.Shape("ship_skin", {-cockpit, 7.2f, -40.6f}, {cockpit, 7.6f, -22.6f}, kHull);
    b.WallX("ship_skin", -cockpit, cockpit, -40.6f, -40.3f, 3.0f, 7.6f, {{-6.8f, 6.8f, kUpper + 0.7f, kUpper + 2.7f}}, kHull, false);
    b.Shape("ship_visor", {-7.4f, kUpper + 2.7f, -40.9f}, {7.4f, kUpper + 3.0f, -40.3f}, kHullDark);
    // The bow under it, narrowing to a blunt nose ahead of the windows.
    b.Mesh("ship_bow", Tapered(-22.3f, out, -1.4f, 3.0f, -48.0f, 3.5f, 1.2f, 3.0f), kHull);
    b.Mesh("ship_bow_deck", Tapered(-22.3f, 8.3f, 3.0f, 3.2f, -46.0f, 3.2f, 3.0f, 3.1f), kHullPanel);
    // A mast and a dish on the roof.
    b.Shape("ship_mast", {-0.12f, 7.6f, -12.12f}, {0.12f, 11.8f, -11.88f}, kHullDark);
    b.Mesh("ship_dish", [] {
        MeshData dish;
        dish.Append(Primitives::Cylinder(1.4f, 0.18f, 20),
                    glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 11.0f, -12.0f)) *
                        glm::mat4_cast(glm::angleAxis(glm::radians(60.0f), glm::vec3(1.0f, 0.0f, 0.0f))));
        return dish;
    }(), kHullPanel);

    // The engines aft: three, their nozzles dark rings round a glow that is turned up for a burn.
    struct Engine
    {
        float x;
        float y;
        float half;
        float length;
    };
    for (const Engine& engine : {Engine{0.0f, 3.8f, 2.8f, 10.0f}, Engine{-9.2f, 3.0f, 2.2f, 8.0f}, Engine{9.2f, 3.0f, 2.2f, 8.0f}})
    {
        const float z0 = 34.6f;
        const float z1 = z0 + engine.length;
        b.Shape("ship_engine", {engine.x - engine.half, engine.y - engine.half, z0}, {engine.x + engine.half, engine.y + engine.half, z1}, kHullPanel);
        b.Shape("ship_engine_band", {engine.x - engine.half - 0.05f, engine.y - engine.half - 0.05f, z0 + 2.0f},
                {engine.x + engine.half + 0.05f, engine.y + engine.half + 0.05f, z0 + 2.6f}, kHullDark);
        b.Mesh("ship_nozzle", [&] {
            MeshData nozzle;
            nozzle.Append(Primitives::Cylinder(engine.half * 0.95f, 1.6f, 24),
                          glm::translate(glm::mat4(1.0f), glm::vec3(engine.x, engine.y, z1 + 0.8f)) *
                              glm::mat4_cast(glm::angleAxis(glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f))));
            return nozzle;
        }(), kNozzle);
        // The glow, just inside the nozzle's mouth: its own thing, so its brightness can be changed.
        const glm::vec3 colour{0.55f, 0.75f, 1.0f};
        Material glow = Material::Diffuse(colour, 0.4f);
        glow.emissive = colour * 0.3f;
        MeshData disc;
        disc.Append(Primitives::Cylinder(engine.half * 0.8f, 0.05f, 24),
                    glm::mat4_cast(glm::angleAxis(glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f))));
        Transform at = At({engine.x, engine.y, z1 + 1.4f});
        const Entity entity = scene.CreateMeshEntity("ship_engine_glow", at, meshes.Upload(disc, "ship_engine_glow_" + std::to_string(glowOut.size())), glow);
        if (MeshRenderer* renderer = scene.GetMeshRenderer(entity))
        {
            renderer->castsShadow = false;
        }
        glowOut.push_back(entity);
        colourOut.push_back(colour);
    }

    // Its lights: red to port, green to starboard, white at the tail.
    b.Shape("ship_nav_port", {-out - 0.2f, 3.0f, -22.0f}, {-out, 3.2f, -21.8f}, Glow({1.0f, 0.1f, 0.08f}, 4.0f));
    b.Shape("ship_nav_starboard", {out, 3.0f, -22.0f}, {out + 0.2f, 3.2f, -21.8f}, Glow({0.1f, 1.0f, 0.2f}, 4.0f));
    b.Shape("ship_nav_tail", {-0.1f, 9.0f, 34.2f}, {0.1f, 9.2f, 34.4f}, Glow({1.0f, 1.0f, 1.0f}, 4.0f));
}

void BuildLamps(Scene& scene, MeshLibrary& meshes, LevelLights& lights)
{
    const glm::vec3 down{0.0f, -1.0f, 0.0f};
    struct Room
    {
        glm::vec3 a;
        glm::vec3 b;
    };
    const auto lamp = [&](LightKind kind, const glm::vec3& at, const Room& room, float range = 0.0f, const glm::vec3& direction = glm::vec3(0.0f, -1.0f, 0.0f))
    {
        const int index = lights.Add(scene, meshes, kind, LightMood::Steady, kOrigin + at, direction, 0, 0, range);
        lights.Bound(index, kOrigin + room.a, kOrigin + room.b);
    };
    const float low = kLowerTop - 0.04f;
    const float high = kUpperTop - 0.04f;
    const Room corridor{{-1.5f, -0.05f, -22.0f}, {1.5f, kLowerTop, 5.85f}};
    for (const float z : {2.5f, -6.0f, -15.0f})
    {
        lamp(LightKind::Ceiling, {0.0f, low, z}, corridor, 10.0f);
    }
    const Room gear{{-12.0f, -0.05f, -5.85f}, {-1.8f, kLowerTop, 5.85f}};
    lamp(LightKind::Ceiling, {-7.0f, low, -2.5f}, gear, 11.0f);
    lamp(LightKind::Ceiling, {-7.0f, low, 3.0f}, gear, 11.0f);
    // The quarters and the mess warmer than the rest: a caged bulb over each bunk and table as well as the strips.
    const Room quarters{{-12.0f, -0.05f, -22.0f}, {-1.8f, kLowerTop, -6.15f}};
    for (const float z : {-9.0f, -18.0f})
    {
        lamp(LightKind::Ceiling, {-6.5f, low, z}, quarters, 11.0f);
    }
    for (const float z : {-7.5f, -12.3f, -17.1f})
    {
        lamp(LightKind::Wall, {-10.2f, low - 0.1f, z}, quarters, 8.0f, down);
    }
    lamp(LightKind::Wall, {-6.8f, low - 0.1f, -14.0f}, quarters, 7.0f, down);
    const Room mess{{1.8f, -0.05f, -22.0f}, {12.0f, kLowerTop, -1.15f}};
    for (const float z : {-5.0f, -12.0f, -19.0f})
    {
        lamp(LightKind::Ceiling, {6.4f, low, z}, mess, 11.0f);
    }
    lamp(LightKind::Wall, {10.6f, low - 0.1f, -9.0f}, mess, 7.0f, down);
    lamp(LightKind::Wall, {10.6f, low - 0.1f, -16.0f}, mess, 7.0f, down);
    // The stairs and the landing at the top of them are one space.
    const Room stairs{{1.8f, -0.05f, -0.85f}, {12.0f, kUpperTop, 5.85f}};
    lamp(LightKind::Ceiling, {10.6f, low, 4.8f}, stairs, 10.0f);
    const Room landing{{-1.5f, kUpper - 0.05f, -3.85f}, {12.0f, kUpperTop, 5.85f}};
    lamp(LightKind::Ceiling, {0.0f, high, -1.5f}, landing);
    lamp(LightKind::Ceiling, {10.0f, high, -1.8f}, landing);
    const Room gallery{{-12.0f, kUpper - 0.05f, -3.85f}, {-1.8f, kUpperTop, 5.85f}};
    lamp(LightKind::Ceiling, {-7.0f, high, 0.5f}, gallery, 9.0f);
    const Room briefing{{-12.0f, kUpper - 0.05f, -22.0f}, {12.0f, kUpperTop, -4.15f}};
    for (const float x : {-7.0f, 0.0f, 7.0f})
    {
        for (const float z : {-8.0f, -16.0f})
        {
            lamp(LightKind::Ceiling, {x, high, z}, briefing, 11.0f);
        }
    }
    const Room forward{{-1.5f, kUpper - 0.05f, -30.0f}, {1.5f, kUpperTop, -22.3f}};
    lamp(LightKind::Ceiling, {0.0f, high, -26.0f}, forward);
    const Room cockpit{{-8.0f, kUpper - 0.05f, -40.0f}, {8.0f, kUpperTop, -30.0f}};
    lamp(LightKind::Wall, {-3.0f, high - 0.1f, -32.0f}, cockpit, 7.0f, down);
    lamp(LightKind::Wall, {3.0f, high - 0.1f, -32.0f}, cockpit, 7.0f, down);
    // The hangar: floods from its roof, and caged lamps down its walls.
    const Room hangar{{-12.0f, -0.05f, 6.15f}, {12.0f, kHangarTop, 34.0f}};
    for (const float x : {-7.0f, 7.0f})
    {
        for (const float z : {12.0f, 27.0f})
        {
            lamp(LightKind::Flood, {x, kHangarTop - 0.1f, z}, hangar, 22.0f, down);
        }
    }
    lamp(LightKind::Wall, {-11.6f, 4.5f, 20.0f}, hangar, 10.0f, glm::normalize(glm::vec3(1.0f, -0.4f, 0.0f)));
    lamp(LightKind::Wall, {11.6f, 4.5f, 20.0f}, hangar, 10.0f, glm::normalize(glm::vec3(-1.0f, -0.4f, 0.0f)));
}

} // namespace

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
        BuildHull(b, m_engineGlow, m_engineGlowColour, scene, meshes);
    }
    if (lights != nullptr)
    {
        BuildLamps(scene, meshes, *lights);
    }

    // The bay doors in the hangar floor, and the shuttle standing on them, its ramp down towards the way in.
    m_bayDoors.Build(scene, meshes, &physics, Vehicles::Load("hangar_doors"), Pose({0.0f, kLower, (kBayFront + kBayBack) * 0.5f}), group,
                     "ship_bay_");
    m_shuttle.Build(scene, meshes, &physics, Vehicles::Load("shuttle"), Pose(kShuttleHome, 180.0f), group, "ship_shuttle_");

    // What WorldObjects puts aboard: lockers down the gear room's hull side and ammunition by its bench (and the kit on
    // the bench: LayOutKit).
    for (const float z : {4.6f, 3.48f, 2.36f})
    {
        m_placements.lockers.push_back({ToWorld({-11.53f, kLower, z}), -glm::half_pi<float>()});
    }
    m_placements.ammoCrates.push_back({ToWorld({-9.5f, kLower, -4.2f}), 0.0f});
    m_placements.ammoCrates.push_back({ToWorld({-4.5f, kLower, -4.2f}), 0.0f});
    m_built = true;
    PRED_LOG_INFO(Gameplay, "The ship: {} bodies, {} entities", m_bodies.size(), m_entities.size());
}

void ShipMap::LayOutKit(const ItemDatabase& items)
{
    m_placements.items.clear();
    int laid = 0;
    for (const ItemDefinition& definition : items.All())
    {
        laid += definition.id != kInvalidItem && definition.benchCount > 0 ? 1 : 0;
    }
    const float spacing = std::min(0.62f, 7.2f / static_cast<float>(std::max(laid, 1)));
    float x = -7.0f - spacing * static_cast<float>(std::max(laid - 1, 0)) * 0.5f;
    for (const ItemDefinition& definition : items.All())
    {
        if (definition.id == kInvalidItem || definition.benchCount <= 0)
        {
            continue;
        }
        m_placements.items.push_back({definition.key, definition.benchCount, ToWorld({x, kLower + 0.92f, -5.45f})});
        x += spacing;
    }
}

bool ShipMap::Contains(const glm::vec3& point) const
{
    const glm::vec2 d{point.x - kOrigin.x, point.z - kOrigin.z};
    return m_built && glm::dot(d, d) < kReach * kReach;
}

bool ShipMap::InHangar(const glm::vec3& point) const
{
    const glm::vec3 local = point - kOrigin;
    return m_built && local.x > -12.0f && local.x < 12.0f && local.z > 6.15f && local.z < 34.0f && local.y > kLower - 0.5f && local.y < kHangarTop;
}

glm::vec3 ShipMap::Spawn(uint8_t player) const
{
    // In the briefing room, a row just inside its door, looking at the screen.
    const float x = -3.5f + static_cast<float>(player % 8) * 1.0f;
    return ToWorld({x, kUpper + 0.1f, -6.5f});
}

float ShipMap::SpawnYaw() const
{
    return 0.0f;
}

CinePose ShipMap::BriefingConsole() const
{
    // Before the screen, its front towards the room.
    return Pose({0.0f, kUpper, -18.8f}, 180.0f);
}

void ShipMap::SetEngines(Scene& scene, float burn)
{
    m_burn = std::clamp(burn, 0.0f, 1.0f);
    for (size_t i = 0; i < m_engineGlow.size(); ++i)
    {
        if (MeshRenderer* renderer = scene.GetMeshRenderer(m_engineGlow[i]))
        {
            renderer->material.emissive = m_engineGlowColour[i] * (0.3f + m_burn * 7.0f);
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
    anchors["hangar"] = Pose({0.0f, kLower, (kBayFront + kBayBack) * 0.5f});
    anchors["ship_shuttle_home"] = m_shuttle.Home();
    anchors["cockpit"] = Pose({0.0f, kUpper, -35.0f});
    anchors["briefing"] = Pose({0.0f, kUpper, -13.0f});
    anchors["engines"] = Pose({0.0f, 3.8f, 46.0f});
}

} // namespace pred
