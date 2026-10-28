#include "Game/World/FacilityCondition.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/mat3x3.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>
#include <vector>

namespace pred
{

namespace FacilityCondition
{

namespace
{

using Kind = FacilityMap::Piece::Kind;
using Form = FacilityMap::Dressing::Form;
using Cell = FacilityLayout::Cell;
using Condition = FacilityLayout::Condition;
constexpr float kCell = FacilityLayout::kCell;
constexpr float kHalfWall = FacilitySpec::kHalfWall;
constexpr float kClear = FacilityLayout::kClearHeight;

// SplitMix64, as the plan is made with.
class Random
{
public:
    explicit Random(uint64_t seed) : m_state(seed) {}
    uint32_t Next()
    {
        uint64_t z = (m_state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return static_cast<uint32_t>((z ^ (z >> 31)) >> 16);
    }
    float Unit() { return static_cast<float>(Next() & 0xFFFFFFu) / static_cast<float>(1u << 24); }
    float Range(float low, float high) { return low + (high - low) * Unit(); }
    bool Chance(float p) { return Unit() < p; }
    int Int(int low, int high) { return std::min(low + static_cast<int>(Unit() * static_cast<float>(high - low + 1)), high); }

private:
    uint64_t m_state;
};

uint32_t Paint(const glm::vec3& colour)
{
    const auto byte = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return byte(colour.r) | (byte(colour.g) << 8) | (byte(colour.b) << 16) | 0xFF000000u;
}

// A wall's face, inside the building, with nothing through it: the middle of its foot, along it, and out of it into the cell.
struct Face
{
    int floor;
    glm::vec3 middle;
    glm::vec3 along;
    glm::vec3 out;
};

// How far along a cell's face anything laid on it may reach either way of its middle: short of the walls at its ends.
constexpr float kFaceHalf = kCell * 0.5f - kHalfWall - 0.06f;

struct Dresser
{
    const FacilityLayout& layout;
    FacilityMap::Blueprint& out;
    Random random;
    int stains = 0; // laid so far, for the layer each lies in

    void Add(Form form, Kind kind, const glm::vec3& centre, const glm::vec3& size, const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f))
    {
        FacilityMap::Dressing dressing;
        dressing.form = form;
        dressing.kind = kind;
        dressing.centre = centre;
        dressing.size = size;
        dressing.rotation = rotation;
        out.furnishing.push_back(dressing);
    }
    glm::quat Yaw(float yaw) const { return glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)); }
    // Turned to lie on a face: its own -z out of it.
    glm::quat Facing(const glm::vec3& n) const { return Yaw(std::atan2(-n.x, -n.z)); }
    glm::quat Down() const { return glm::angleAxis(-glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f)); }
    glm::quat Up() const { return glm::angleAxis(glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f)); }

    // A stain on a face (FacilityMap::Dressing::Form::Patch), in that face's material, a few millimetres proud of it: each in turn a
    // millimetre further out than the last, over four layers, so two that overlap never lie in the same place and flicker.
    void Stain(Kind face, const glm::vec3& at, const glm::quat& lie, float wide, float high, const glm::vec3& tint, float fade)
    {
        FacilityMap::Dressing patch;
        patch.form = Form::Patch;
        patch.kind = face;
        patch.centre = at + lie * glm::vec3(0.0f, 0.0f, -(0.002f + 0.001f * static_cast<float>(stains++ % 4)));
        patch.size = {wide, high, fade};
        patch.rotation = lie;
        patch.tint = tint;
        patch.shape = random.Next();
        out.furnishing.push_back(patch);
    }
    // On a wall's face: so wide (kept on the face), from one height to another.
    void OnWall(const Face& face, Kind kind, float wide, float bottom, float top, const glm::vec3& tint, float fade)
    {
        wide = std::min(wide, kFaceHalf * 2.0f);
        const float along = random.Range(-(kFaceHalf - wide * 0.5f), kFaceHalf - wide * 0.5f);
        const glm::vec3 at = face.middle + face.along * along + glm::vec3(0.0f, (bottom + top) * 0.5f, 0.0f);
        Stain(kind, at, Facing(face.out), wide, top - bottom, tint, fade);
    }

    glm::vec3 Middle(int f, int x, int z) const
    {
        return FacilityMap::ToWorld(layout, f, {static_cast<float>(x) + 0.5f, static_cast<float>(z) + 0.5f});
    }
    // Which space a cell is part of (a room, the corridors, a stairwell), or none.
    int Space(int f, int x, int z) const
    {
        if (x < 0 || z < 0 || x >= layout.width || z >= layout.depth)
        {
            return -9;
        }
        const Cell cell = layout.At(f, x, z);
        return cell == Cell::Room || cell == Cell::Stair ? layout.roomOf[layout.Index(f, x, z)] : cell == Cell::Corridor ? -2 : cell == Cell::Duct ? -3 : -9;
    }

    // Where the floors are clear (rooms and corridors with nothing standing in them and no doorway at their edge), every room and
    // corridor cell, and every inside face of a wall with no opening in it.
    std::vector<std::pair<int, glm::ivec2>> clear;
    std::vector<std::pair<int, glm::ivec2>> open;
    std::vector<Face> faces;

    void Survey()
    {
        std::set<std::tuple<int, int, int>> taken;
        for (const FacilityLayout::Fitted& fitted : layout.fittings)
        {
            taken.insert({fitted.floor, static_cast<int>(std::floor(fitted.at.x)), static_cast<int>(std::floor(fitted.at.y))});
        }
        for (const FacilityLayout::Placed& thing : layout.things)
        {
            taken.insert({thing.floor, static_cast<int>(std::floor(thing.at.x)), static_cast<int>(std::floor(thing.at.y))});
        }
        std::set<std::tuple<int, int, int, int>> holes;
        const auto hole = [&](int f, glm::ivec2 a, glm::ivec2 b)
        {
            const glm::ivec2 low = glm::min(a, b);
            holes.insert({f, low.x, low.y, a.x != b.x ? 0 : 1});
            taken.insert({f, a.x, a.y});
            taken.insert({f, b.x, b.y});
        };
        for (const FacilityLayout::Door& door : layout.doors)
        {
            hole(door.floor, door.cell, door.cell + (door.side == 0 ? glm::ivec2(1, 0) : glm::ivec2(0, 1)));
        }
        for (const FacilityLayout::Duct& duct : layout.ducts)
        {
            for (const FacilityLayout::Mouth& mouth : duct.mouths)
            {
                hole(duct.floor, mouth.duct, mouth.open);
            }
        }
        for (const FacilityLayout::Exit& exit : layout.exits)
        {
            taken.insert({layout.ground, exit.cell.x, exit.cell.y});
        }
        for (const FacilityLayout::Exit& tunnel : layout.tunnels)
        {
            taken.insert({layout.ground - 1, tunnel.cell.x, tunnel.cell.y});
        }
        const glm::ivec2 kWays[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (int f = 0; f < layout.floors; ++f)
        {
            for (int z = 0; z < layout.depth; ++z)
            {
                for (int x = 0; x < layout.width; ++x)
                {
                    const Cell cell = layout.At(f, x, z);
                    if (cell != Cell::Room && cell != Cell::Corridor)
                    {
                        continue;
                    }
                    open.push_back({f, {x, z}});
                    if (taken.count({f, x, z}) == 0)
                    {
                        clear.push_back({f, {x, z}});
                    }
                    const int here = Space(f, x, z);
                    for (const glm::ivec2 way : kWays)
                    {
                        const int there = Space(f, x + way.x, z + way.y);
                        // An inside wall (not the outside, where the windows are), with no doorway or vent through it.
                        if (there == here || there == -9)
                        {
                            continue;
                        }
                        const glm::ivec2 low = glm::min(glm::ivec2(x, z), glm::ivec2(x, z) + way);
                        if (holes.count({f, low.x, low.y, way.x != 0 ? 0 : 1}) != 0)
                        {
                            continue;
                        }
                        const glm::vec3 w{static_cast<float>(way.x), 0.0f, static_cast<float>(way.y)};
                        faces.push_back({f, Middle(f, x, z) + w * (kCell * 0.5f - kHalfWall), glm::vec3(w.z, 0.0f, -w.x), -w});
                    }
                }
            }
        }
    }

    // A point on a cell's floor, so far off its middle at most.
    glm::vec3 OnFloor(const std::pair<int, glm::ivec2>& cell, float keep = 1.0f)
    {
        keep = std::max(keep, 0.0f);
        return Middle(cell.first, cell.second.x, cell.second.y) + glm::vec3(random.Range(-keep, keep), 0.0f, random.Range(-keep, keep));
    }
    glm::vec3 OnCeiling(const std::pair<int, glm::ivec2>& cell, float keep) { return OnFloor(cell, keep) + glm::vec3(0.0f, kClear, 0.0f); }

    void Papers(const glm::vec3& at, int count, float spread)
    {
        for (int i = 0; i < count; ++i)
        {
            const glm::vec3 p = at + glm::vec3(random.Range(-spread, spread), 0.0015f + 0.0008f * static_cast<float>(i % 4), random.Range(-spread, spread));
            Add(Form::Box, Kind::Paper, p, {0.21f, 0.002f, 0.297f}, Yaw(random.Range(0.0f, glm::two_pi<float>())));
        }
    }

    // Lumps of what came down or what burnt, lying where they fell: each its own size, turned, a little tipped.
    void Lumps(const glm::vec3& at, Kind kind, int count, float low, float high, float spread, float flat)
    {
        for (int i = 0; i < count; ++i)
        {
            const glm::vec3 size{random.Range(low, high), random.Range(low, high) * flat, random.Range(low, high)};
            const glm::vec3 p = at + glm::vec3(random.Range(-spread, spread), size.y * 0.45f, random.Range(-spread, spread));
            Add(Form::Box, kind, p, size, Yaw(random.Range(0.0f, glm::two_pi<float>())) * glm::angleAxis(random.Range(-0.25f, 0.25f), glm::vec3(1.0f, 0.0f, 0.0f)));
        }
    }

    void Abandoned()
    {
        // Water come in where it could: stains run down from the ceiling, and dried on the ceilings and floors under them.
        for (const Face& face : faces)
        {
            if (random.Chance(0.35f))
            {
                OnWall(face, Kind::Wall, random.Range(0.5f, 1.3f), random.Range(0.6f, 1.6f), kClear - 0.01f, {0.62f, 0.55f, 0.42f}, 0.85f);
            }
        }
        for (const auto& cell : open)
        {
            if (random.Chance(0.3f))
            {
                const float wide = random.Range(1.0f, 2.2f);
                Stain(Kind::Ceiling, OnCeiling(cell, 1.4f - wide * 0.5f), Down(), wide, wide * random.Range(0.6f, 1.0f), {0.6f, 0.52f, 0.38f}, 0.85f);
            }
            if (random.Chance(0.25f))
            {
                const float wide = random.Range(0.8f, 1.8f);
                Stain(Kind::Floor, OnFloor(cell, 1.4f - wide * 0.5f), Up(), wide, wide * random.Range(0.6f, 1.0f), {0.62f, 0.6f, 0.56f}, 0.9f);
            }
        }
        for (const auto& cell : clear)
        {
            if (random.Chance(0.6f))
            {
                Papers(OnFloor(cell), random.Int(1, 3), 0.5f);
            }
            // The ceiling's skin come away over a spot, and what fell under it.
            if (random.Chance(0.2f))
            {
                const glm::vec3 at = OnFloor(cell, 0.8f);
                Stain(Kind::Ceiling, at + glm::vec3(0.0f, kClear, 0.0f), Down(), random.Range(0.6f, 1.1f), random.Range(0.5f, 0.9f), {0.42f, 0.41f, 0.4f}, 0.3f);
                Lumps(at, Kind::Rubble, random.Int(4, 8), 0.04f, 0.16f, 0.4f, 0.55f);
            }
        }
    }

    void Hurried()
    {
        for (const auto& cell : clear)
        {
            if (random.Chance(0.7f))
            {
                Papers(OnFloor(cell), random.Int(2, 6), 0.6f);
            }
            if (random.Chance(0.15f))
            {
                // A bag, dropped.
                const glm::vec3 at = OnFloor(cell);
                Add(Form::Box, Kind::Fabric, at + glm::vec3(0.0f, 0.13f, 0.0f), {0.38f, 0.26f, 0.24f}, Yaw(random.Range(0.0f, glm::two_pi<float>())));
            }
            if (random.Chance(0.08f))
            {
                // A chair knocked over onto its back: its seat upright, its back flat on the floor (low: stepped over, not felt).
                const glm::vec3 at = OnFloor(cell);
                const glm::quat turn = Yaw(random.Range(0.0f, glm::two_pi<float>()));
                Add(Form::Box, Kind::Fabric, at + turn * glm::vec3(0.0f, 0.24f, 0.0f), {0.48f, 0.48f, 0.08f}, turn);
                Add(Form::Box, Kind::Fabric, at + turn * glm::vec3(0.0f, 0.03f, 0.3f), {0.46f, 0.06f, 0.44f}, turn);
                Add(Form::Cylinder, Kind::DarkSteel, at + turn * glm::vec3(0.0f, 0.24f, -0.25f), {0.05f, 0.4f, 0.05f},
                    turn * glm::angleAxis(glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f)));
            }
        }
    }

    void Burnt()
    {
        // Soot: up the walls from where it burnt, thickest under the ceiling; across the ceilings; scorched into the floors.
        for (const Face& face : faces)
        {
            if (random.Chance(0.55f))
            {
                OnWall(face, Kind::Wall, random.Range(1.2f, 2.6f), random.Range(0.8f, 1.8f), kClear - 0.01f, glm::vec3(0.05f), 0.7f);
            }
            if (random.Chance(0.2f))
            {
                // Where it burnt against the wall: a tongue of it up from the floor.
                OnWall(face, Kind::Wall, random.Range(0.6f, 1.1f), -0.05f, random.Range(1.5f, 2.6f), glm::vec3(0.07f), 0.55f);
            }
        }
        for (const auto& cell : open)
        {
            if (random.Chance(0.55f))
            {
                const float wide = random.Range(1.6f, 2.6f);
                Stain(Kind::Ceiling, OnCeiling(cell, 1.4f - wide * 0.5f), Down(), wide, wide * random.Range(0.7f, 1.0f), glm::vec3(0.05f), 0.7f);
            }
        }
        for (const auto& cell : clear)
        {
            if (random.Chance(0.4f))
            {
                const float wide = random.Range(0.8f, 1.8f);
                const glm::vec3 at = OnFloor(cell, 1.4f - wide * 0.5f);
                Stain(Kind::Floor, at, Up(), wide, wide * random.Range(0.6f, 1.0f), glm::vec3(0.12f), 0.6f);
                // What burnt there: charred planks and lumps.
                Lumps(at, Kind::Soot, random.Int(2, 5), 0.06f, 0.5f, wide * 0.3f, 0.12f);
            }
        }
    }

    void Frozen()
    {
        // Rime up the foot of the walls and over the floor along them, ice on the ceilings in places, icicles hanging from them.
        for (const Face& face : faces)
        {
            if (random.Chance(0.6f))
            {
                OnWall(face, Kind::Frost, random.Range(1.0f, 2.4f), -0.08f, random.Range(0.2f, 0.65f), glm::vec3(1.0f), 0.0f);
                const float wide = random.Range(0.9f, 2.0f);
                const float deep = random.Range(0.3f, 0.7f);
                const float along = random.Range(-(kFaceHalf - wide * 0.5f), kFaceHalf - wide * 0.5f);
                // Laid flat along the wall, its far edge under the wall's foot.
                const glm::vec3 at = face.middle + face.along * along + face.out * (deep * 0.5f - 0.06f);
                Stain(Kind::Frost, at, Facing(face.out) * Up(), wide, deep, glm::vec3(1.0f), 0.0f);
            }
        }
        for (const auto& cell : open)
        {
            if (random.Chance(0.2f))
            {
                const float wide = random.Range(0.8f, 1.6f);
                Stain(Kind::Frost, OnCeiling(cell, 1.4f - wide * 0.5f), Down(), wide, wide * 0.7f, glm::vec3(1.0f), 0.0f);
            }
            if (random.Chance(0.35f))
            {
                const glm::vec3 at = OnCeiling(cell, 1.1f);
                for (int i = random.Int(2, 5); i > 0; --i)
                {
                    const float length = random.Range(0.1f, 0.4f);
                    const float foot = random.Range(0.03f, 0.08f);
                    const glm::vec3 p = at + glm::vec3(random.Range(-0.2f, 0.2f), -length * 0.5f + 0.004f, random.Range(-0.2f, 0.2f));
                    Add(Form::Cone, Kind::Frost, p, {foot, length, 0.004f}, glm::angleAxis(glm::pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f)));
                }
            }
        }
    }

    // Flooded: a tide line all round the walls where the water stands, a band of silt and scum darkest at the waterline and
    // fading up the wall, ragged at its top -- where it has stood a long time.
    void Flooded()
    {
        for (const Face& face : faces)
        {
            if (face.floor != layout.flooded)
            {
                continue;
            }
            const float line = layout.floodDepth;
            OnWall(face, Kind::Wall, kFaceHalf * 2.0f, line - 0.05f, line + random.Range(0.15f, 0.35f), glm::vec3(0.34f, 0.29f, 0.21f), 0.35f);
        }
    }

    // A leaf, flat-ish, at a point, turned and tipped as it grows.
    void Leaf(const glm::vec3& at, float size, const glm::quat& turn) { Add(Form::Ball, Kind::Leaf, at, {size, size * 0.12f, size * 0.62f}, turn); }

    void Overgrown()
    {
        for (const Face& face : faces)
        {
            // Damp up the foot of the wall, green.
            if (random.Chance(0.45f))
            {
                OnWall(face, Kind::Wall, random.Range(1.0f, 2.4f), -0.05f, random.Range(0.4f, 1.1f), {0.42f, 0.5f, 0.32f}, 0.8f);
            }
            // Vines down from the ceiling: a stem wandering down the wall, a leaf off it either side every hand's width.
            if (random.Chance(0.4f))
            {
                for (int v = random.Int(2, 5); v > 0; --v)
                {
                    glm::vec3 at = face.middle + face.along * random.Range(-kFaceHalf + 0.15f, kFaceHalf - 0.15f) + face.out * 0.018f + glm::vec3(0.0f, kClear, 0.0f);
                    const float reach = random.Range(0.8f, 2.4f);
                    float down = 0.0f;
                    int leaf = 0;
                    while (down < reach)
                    {
                        const float step = random.Range(0.18f, 0.3f);
                        glm::vec3 to = at + face.along * random.Range(-0.06f, 0.06f) - glm::vec3(0.0f, step, 0.0f);
                        // (Kept on its wall's face.)
                        const float along = glm::dot(to - face.middle, face.along);
                        to += face.along * (std::clamp(along, -kFaceHalf, kFaceHalf) - along);
                        const glm::vec3 run = to - at;
                        Add(Form::Cylinder, Kind::Leaf, (at + to) * 0.5f, {0.014f, glm::length(run), 0.014f},
                            glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), glm::normalize(run)));
                        for (float t = 0.2f; t < 1.0f; t += 0.45f)
                        {
                            const float side = (leaf++ % 2 == 0) ? 1.0f : -1.0f;
                            const glm::vec3 p = glm::mix(at, to, t) + face.along * (side * 0.035f) + face.out * 0.01f;
                            Leaf(p, random.Range(0.06f, 0.1f),
                                 Facing(face.out) * glm::angleAxis(side * random.Range(0.5f, 1.0f), glm::vec3(0.0f, 0.0f, 1.0f)) *
                                     glm::angleAxis(glm::half_pi<float>() - 0.3f, glm::vec3(1.0f, 0.0f, 0.0f)));
                        }
                        at = to;
                        down += step;
                    }
                }
            }
            // And growing up out of the corner of the floor and the wall: a clump of leaves.
            if (random.Chance(0.25f))
            {
                const glm::vec3 root = face.middle + face.along * random.Range(-kFaceHalf + 0.3f, kFaceHalf - 0.3f) + face.out * 0.12f;
                for (int l = random.Int(6, 11); l > 0; --l)
                {
                    const float a = random.Range(0.0f, glm::two_pi<float>());
                    const float lean = random.Range(0.3f, 1.0f);
                    const float size = random.Range(0.12f, 0.22f);
                    const glm::quat turn = Yaw(a) * glm::angleAxis(lean, glm::vec3(1.0f, 0.0f, 0.0f));
                    Leaf(root + turn * glm::vec3(0.0f, size * 0.15f, -size * 0.45f) + glm::vec3(0.0f, size * 0.25f, 0.0f), size, turn);
                }
            }
        }
        for (const auto& cell : clear)
        {
            if (random.Chance(0.5f))
            {
                // Moss over the floor.
                const float wide = random.Range(0.8f, 2.2f);
                Stain(Kind::Floor, OnFloor(cell, 1.4f - wide * 0.5f), Up(), wide, wide * random.Range(0.6f, 1.0f), {0.22f, 0.36f, 0.12f}, 0.6f);
            }
            // Leaves blown in.
            const glm::vec3 at = OnFloor(cell);
            for (int i = random.Int(0, 8); i > 0; --i)
            {
                const float size = random.Range(0.06f, 0.11f);
                Leaf(at + glm::vec3(random.Range(-0.6f, 0.6f), size * 0.06f + 0.002f, random.Range(-0.6f, 0.6f)), size,
                     Yaw(random.Range(0.0f, glm::two_pi<float>())) * glm::angleAxis(random.Range(-0.15f, 0.15f), glm::vec3(1.0f, 0.0f, 0.0f)));
            }
        }
    }
};

} // namespace

MeshData PatchMesh(float wide, float high, const glm::vec3& tint, float fade, uint32_t shape)
{
    constexpr int kRings = 5;
    constexpr int kAround = 28;
    Random random(shape);
    // Its edge: in and out round it, at a few sizes at once.
    float phase[4];
    for (float& p : phase)
    {
        p = random.Range(0.0f, glm::two_pi<float>());
    }
    const auto edge = [&](float a)
    {
        return 1.0f - 0.16f * (0.5f + 0.5f * std::sin(2.0f * a + phase[0])) - 0.1f * (0.5f + 0.5f * std::sin(3.0f * a + phase[1])) -
               0.07f * (0.5f + 0.5f * std::sin(5.0f * a + phase[2])) - 0.04f * (0.5f + 0.5f * std::sin(9.0f * a + phase[3]));
    };
    MeshData mesh;
    const glm::vec3 normal{0.0f, 0.0f, -1.0f};
    const auto colour = [&](float out)
    {
        // The tint at its middle and over all of it but the fading part, there giving way to none; a little uneven all over.
        const float strength = fade <= 0.0f ? 1.0f : 1.0f - glm::smoothstep(1.0f - fade, 1.0f, out);
        const float uneven = fade <= 0.0f ? 1.0f : 0.8f + 0.2f * random.Unit();
        return Paint(glm::mix(glm::vec3(1.0f), tint, std::clamp(strength * uneven, 0.0f, 1.0f)));
    };
    mesh.vertices.push_back(MeshVertex{{0.0f, 0.0f, 0.0f}, normal, {0.5f, 0.5f}, colour(0.0f)});
    for (int r = 1; r <= kRings; ++r)
    {
        const float out = static_cast<float>(r) / static_cast<float>(kRings);
        for (int k = 0; k < kAround; ++k)
        {
            const float a = glm::two_pi<float>() * static_cast<float>(k) / static_cast<float>(kAround);
            const float reach = out * edge(a) * (r < kRings ? random.Range(0.92f, 1.04f) : 1.0f);
            const glm::vec3 p{std::cos(a) * reach * wide * 0.5f, std::sin(a) * reach * high * 0.5f, 0.0f};
            mesh.vertices.push_back(MeshVertex{p, normal, {0.5f + p.x, 0.5f + p.y}, colour(out)});
        }
    }
    // Facing -z: wound back the other way round to the way round it goes.
    for (int k = 0; k < kAround; ++k)
    {
        const auto a = static_cast<uint32_t>(1 + k);
        const auto b = static_cast<uint32_t>(1 + (k + 1) % kAround);
        mesh.indices.insert(mesh.indices.end(), {0u, b, a});
    }
    for (int r = 1; r < kRings; ++r)
    {
        const auto inner = static_cast<uint32_t>(1 + (r - 1) * kAround);
        const auto outer = static_cast<uint32_t>(1 + r * kAround);
        for (int k = 0; k < kAround; ++k)
        {
            const auto k1 = static_cast<uint32_t>((k + 1) % kAround);
            const uint32_t i0 = inner + static_cast<uint32_t>(k);
            const uint32_t i1 = inner + k1;
            const uint32_t o0 = outer + static_cast<uint32_t>(k);
            const uint32_t o1 = outer + k1;
            mesh.indices.insert(mesh.indices.end(), {i0, i1, o1, i0, o1, o0});
        }
    }
    return mesh;
}

void Dress(const FacilityLayout& layout, FacilityMap::Blueprint& out)
{
    if (layout.condition == Condition::Kept && !layout.frozen && !layout.overgrown && layout.flooded < 0)
    {
        return;
    }
    Dresser dresser{layout, out, Random((static_cast<uint64_t>(layout.seed) << 20) ^ 0xC0D171ull)};
    dresser.Survey();
    switch (layout.condition)
    {
    case Condition::Abandoned: dresser.Abandoned(); break;
    case Condition::Hurried: dresser.Hurried(); break;
    case Condition::Burnt: dresser.Burnt(); break;
    case Condition::Kept: break;
    }
    if (layout.frozen)
    {
        dresser.Frozen();
    }
    if (layout.overgrown)
    {
        dresser.Overgrown();
    }
    if (layout.flooded >= 0)
    {
        dresser.Flooded();
    }
}

} // namespace FacilityCondition

} // namespace pred
