#include "Game/World/FacilityCondition.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
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
    float Unit()
    {
        uint64_t z = (m_state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return static_cast<float>(z >> 40) / static_cast<float>(1ull << 24);
    }
    float Range(float low, float high) { return low + (high - low) * Unit(); }
    bool Chance(float p) { return Unit() < p; }
    int Int(int low, int high) { return std::min(low + static_cast<int>(Unit() * static_cast<float>(high - low + 1)), high); }

private:
    uint64_t m_state;
};

// A wall's face, inside the building, with nothing through it: its middle at the floor, along it, and out of it into the cell.
struct Face
{
    int floor;
    glm::vec3 middle;
    glm::vec3 along;
    glm::vec3 out;
};

struct Dresser
{
    const FacilityLayout& layout;
    FacilityMap::Blueprint& out;
    Random random;

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

    // Where the floors are clear: open cells (rooms and corridors) with nothing standing in them and no doorway at their edge.
    std::vector<std::pair<int, glm::ivec2>> clear;
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
        // Each doorway's edge, by its floor, low cell and way (as the plan keeps them), and the cells either side of it.
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

    // A point on a clear cell's floor, away from its edges.
    glm::vec3 OnFloor(const std::pair<int, glm::ivec2>& cell)
    {
        return Middle(cell.first, cell.second.x, cell.second.y) + glm::vec3(random.Range(-1.0f, 1.0f), 0.0f, random.Range(-1.0f, 1.0f));
    }

    void Papers(const glm::vec3& at, int count, float spread)
    {
        for (int i = 0; i < count; ++i)
        {
            const glm::vec3 p = at + glm::vec3(random.Range(-spread, spread), 0.0015f + 0.0008f * static_cast<float>(i % 4), random.Range(-spread, spread));
            Add(Form::Box, Kind::Paper, p, {0.21f, 0.002f, 0.297f}, Yaw(random.Range(0.0f, glm::two_pi<float>())));
        }
    }

    void Chunks(const glm::vec3& at, Kind kind, int count, float low, float high, float spread)
    {
        for (int i = 0; i < count; ++i)
        {
            const glm::vec3 size{random.Range(low, high), random.Range(low, high) * 0.6f, random.Range(low, high)};
            const glm::vec3 p = at + glm::vec3(random.Range(-spread, spread), size.y * 0.5f, random.Range(-spread, spread));
            Add(Form::Box, kind, p, size, Yaw(random.Range(0.0f, glm::two_pi<float>())) * glm::angleAxis(random.Range(-0.3f, 0.3f), glm::vec3(1.0f, 0.0f, 0.0f)));
        }
    }

    // A patch on a face (a wall's, or the ceiling's, facing down): so wide and high, its middle at `at`, so far proud.
    void Patch(Kind kind, const glm::vec3& at, const glm::vec3& normal, float wide, float high, float proud)
    {
        Add(Form::Box, kind, at + normal * (proud * 0.5f), {wide, high, proud}, Facing(normal));
    }

    void Abandoned()
    {
        for (const auto& cell : clear)
        {
            const glm::vec3 floor = OnFloor(cell);
            if (random.Chance(0.6f))
            {
                Papers(floor, random.Int(1, 3), 0.5f);
            }
            if (random.Chance(0.25f))
            {
                // A ceiling tile come down.
                Add(Form::Box, Kind::Ceiling, OnFloor(cell) + glm::vec3(0.0f, 0.009f, 0.0f), {0.6f, 0.016f, 0.6f}, Yaw(random.Range(0.0f, 1.6f)));
            }
            if (random.Chance(0.3f))
            {
                Chunks(OnFloor(cell), Kind::Rubble, random.Int(3, 6), 0.04f, 0.14f, 0.35f);
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
                // A chair knocked over onto its back: its seat upright, its back flat on the floor. In the way, as a chair is.
                const glm::vec3 at = OnFloor(cell);
                const float yaw = random.Range(0.0f, glm::two_pi<float>());
                const glm::quat turn = Yaw(yaw);
                Add(Form::Box, Kind::Fabric, at + turn * glm::vec3(0.0f, 0.24f, 0.0f), {0.48f, 0.48f, 0.08f}, turn);
                Add(Form::Box, Kind::Fabric, at + turn * glm::vec3(0.0f, 0.03f, 0.3f), {0.46f, 0.06f, 0.44f}, turn);
                Add(Form::Cylinder, Kind::DarkSteel, at + turn * glm::vec3(0.0f, 0.24f, -0.25f), {0.05f, 0.4f, 0.05f}, turn * glm::angleAxis(glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f)));
                FacilityMap::Piece solid;
                solid.kind = Kind::Furniture;
                solid.centre = at + turn * glm::vec3(0.0f, 0.25f, 0.05f);
                solid.size = {0.5f, 0.5f, 0.9f};
                solid.yaw = yaw;
                out.pieces.push_back(solid);
            }
        }
    }

    void Burnt()
    {
        for (const Face& face : faces)
        {
            if (!random.Chance(0.6f))
            {
                continue;
            }
            // Soot up from where it burnt, under the ceiling, wider at the top.
            const float wide = random.Range(1.2f, 2.6f);
            const float high = random.Range(0.6f, 1.4f);
            const glm::vec3 top = face.middle + face.along * random.Range(-0.6f, 0.6f) + glm::vec3(0.0f, kClear - 0.03f, 0.0f);
            Patch(Kind::Soot, top - glm::vec3(0.0f, high * 0.5f, 0.0f), face.out, wide, high, random.Range(0.003f, 0.006f));
            Patch(Kind::Soot, top - glm::vec3(0.0f, high + 0.3f, 0.0f) + face.along * random.Range(-0.3f, 0.3f), face.out, wide * 0.45f, 0.6f,
                  random.Range(0.002f, 0.003f));
        }
        for (int f = 0; f < layout.floors; ++f)
        {
            for (int z = 0; z < layout.depth; ++z)
            {
                for (int x = 0; x < layout.width; ++x)
                {
                    const Cell cell = layout.At(f, x, z);
                    if ((cell == Cell::Room || cell == Cell::Corridor) && random.Chance(0.5f))
                    {
                        const glm::vec3 at = Middle(f, x, z) + glm::vec3(random.Range(-0.4f, 0.4f), kClear - random.Range(0.003f, 0.006f) * 0.5f, random.Range(-0.4f, 0.4f));
                        Add(Form::Box, Kind::Soot, at, {random.Range(1.5f, 2.6f), random.Range(0.003f, 0.006f), random.Range(1.5f, 2.6f)}, Yaw(random.Range(0.0f, 1.6f)));
                    }
                }
            }
        }
        for (const auto& cell : clear)
        {
            if (random.Chance(0.5f))
            {
                Chunks(OnFloor(cell), Kind::Soot, random.Int(3, 6), 0.05f, 0.3f, 0.4f);
            }
        }
    }

    void Frozen()
    {
        for (const Face& face : faces)
        {
            if (!random.Chance(0.7f))
            {
                continue;
            }
            // Rime up the foot of the wall, and ice along the floor at it.
            const float high = random.Range(0.15f, 0.5f);
            const float wide = random.Range(1.6f, 2.6f);
            const glm::vec3 foot = face.middle + face.along * random.Range(-0.3f, 0.3f);
            Patch(Kind::Frost, foot + glm::vec3(0.0f, high * 0.5f, 0.0f), face.out, wide, high, random.Range(0.003f, 0.005f));
            const float deep = random.Range(0.25f, 0.5f);
            Add(Form::Box, Kind::Frost, foot + face.out * (deep * 0.5f + 0.006f) + glm::vec3(0.0f, 0.002f, 0.0f), {wide, 0.004f, deep}, Facing(face.out));
        }
        for (int f = 0; f < layout.floors; ++f)
        {
            for (int z = 0; z < layout.depth; ++z)
            {
                for (int x = 0; x < layout.width; ++x)
                {
                    const Cell cell = layout.At(f, x, z);
                    if ((cell != Cell::Room && cell != Cell::Corridor) || !random.Chance(0.4f))
                    {
                        continue;
                    }
                    // Icicles from the ceiling, a few together.
                    const glm::vec3 at = Middle(f, x, z) + glm::vec3(random.Range(-1.1f, 1.1f), kClear, random.Range(-1.1f, 1.1f));
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
    }

    void Overgrown()
    {
        for (const Face& face : faces)
        {
            if (!random.Chance(0.5f))
            {
                continue;
            }
            // Vines down from the ceiling, wandering a little as they go.
            for (int v = random.Int(3, 7); v > 0; --v)
            {
                glm::vec3 at = face.middle + face.along * random.Range(-1.2f, 1.2f) + glm::vec3(0.0f, kClear, 0.0f);
                const float wide = random.Range(0.04f, 0.1f);
                const float proud = random.Range(0.008f, 0.015f);
                for (int s = random.Int(2, 4); s > 0 && at.y - face.middle.y > 0.3f; --s)
                {
                    const float length = random.Range(0.4f, 0.8f);
                    const glm::vec3 to = at + face.along * random.Range(-0.12f, 0.12f) - glm::vec3(0.0f, length, 0.0f);
                    const glm::vec3 mid = (at + to) * 0.5f;
                    const glm::vec3 run = to - at;
                    // Its length along the run, its thickness out of the wall, its width across both.
                    const glm::vec3 up = glm::normalize(-run);
                    const glm::vec3 across = glm::normalize(glm::cross(up, -face.out));
                    const glm::quat lie = glm::quat_cast(glm::mat3(across, up, -face.out));
                    Add(Form::Box, Kind::Leaf, mid + face.out * (proud * 0.5f), {wide, glm::length(run), proud}, lie);
                    at = to;
                }
            }
        }
        for (const auto& cell : clear)
        {
            if (random.Chance(0.5f))
            {
                // Moss over the floor, and leaves blown in.
                const float wide = random.Range(0.4f, 1.2f);
                Add(Form::Box, Kind::Leaf, OnFloor(cell) + glm::vec3(0.0f, 0.003f, 0.0f), {wide, 0.005f, wide * random.Range(0.6f, 1.0f)},
                    Yaw(random.Range(0.0f, glm::two_pi<float>())));
            }
            const glm::vec3 at = OnFloor(cell);
            for (int i = random.Int(0, 8); i > 0; --i)
            {
                Add(Form::Box, Kind::Leaf, at + glm::vec3(random.Range(-0.6f, 0.6f), 0.007f + 0.001f * static_cast<float>(i % 3), random.Range(-0.6f, 0.6f)),
                    {0.07f, 0.002f, 0.1f}, Yaw(random.Range(0.0f, glm::two_pi<float>())));
            }
        }
    }
};

} // namespace

void Dress(const FacilityLayout& layout, FacilityMap::Blueprint& out)
{
    if (layout.condition == Condition::Kept && !layout.frozen && !layout.overgrown)
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
}

} // namespace FacilityCondition

} // namespace pred
