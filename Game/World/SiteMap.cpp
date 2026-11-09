#include "Game/World/SiteMap.h"

#include "Game/World/FacilityOutside.h"
#include "Game/World/Lettering.h"

#include "Engine/Core/Loading.h"
#include "Engine/Core/Log.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/MeshSimplify.h"
#include "Engine/Render/Primitives.h"
#include "Engine/Render/SurfaceNets.h"
#include "Game/World/Cargo.h"
#include "Game/World/Foliage.h"
#include "Game/World/MapBuilder.h"
#include "Game/World/ShipMap.h"
#include "Game/World/SiteScatter.h"
#include "Game/World/SiteStructures.h"
#include "Game/World/Surfaces.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <future>
#include <map>
#include <set>
#include <string>

namespace pred
{
namespace
{

using Kind = SitePlan::BlockKind;

// The ground, the rock round it and the boulders on it take the world's own colours (SiteMap::Look); the ground has the snow
// set laid on it, its grain, tinted to that colour.
const Material kHullDebrisPlain = Material::Metal({0.27f, 0.28f, 0.3f}, 0.55f);
const Material kHullDebrisLightPlain = Material::Diffuse({0.56f, 0.56f, 0.54f}, 0.7f);
const Material kPadPlain = Material::Diffuse({0.3f, 0.3f, 0.29f}, 0.85f);
const Material kPipePlain = Material::Metal({0.34f, 0.35f, 0.36f}, 0.5f);
const Material kSupportPlain = Material::Metal({0.22f, 0.22f, 0.23f}, 0.6f);
const Material kTankPlain = Material::Diffuse({0.6f, 0.6f, 0.56f}, 0.55f);
const Material kPolePlain = Material::Metal({0.25f, 0.25f, 0.26f}, 0.55f);
// Freight containers come in the colours they always do, faded.
const glm::vec3 kContainerColours[] = {{0.32f, 0.12f, 0.08f}, {0.1f, 0.18f, 0.27f}, {0.23f, 0.25f, 0.14f}, {0.36f, 0.35f, 0.33f}};

// Open ground is drawn in big pieces: few lamps reach it, and there is a great deal of it.
constexpr float kOutdoorTile = 24.0f;
// On the sea's floor: how high every building stands on its legs over it (room to swim in under it, up into its moon pool), and how
// far the deck of the pad floating on the sea over it stands out of the water.
constexpr float kSeabedLift = SitePlan::kSeabedLift;
constexpr float kPontoonDeck = 6.0f; // the pad's deck over the sea, on its legs: over the highest a storm raises the waves (Waves::Raised, three metres or so)

const char* NameOf(Kind kind)
{
    switch (kind)
    {
    case Kind::Rock: return "site_rock";
    case Kind::Container: return "site_container";
    case Kind::Generator: return "site_generator";
    case Kind::Pad: return "site_pad";
    case Kind::PipeX:
    case Kind::PipeZ: return "site_pipe";
    case Kind::Support: return "site_support";
    case Kind::Elbow:
    case Kind::Collar: return "site_pipe_fitting";
    case Kind::Tank: return "site_tank";
    case Kind::Mast: return "site_mast";
    case Kind::Debris: return "site_debris";
    default: break; // (what SiteStructures builds names itself)
    }
    return "site";
}

LightMood MoodOf(FacilityLayout::LampMood mood)
{
    switch (mood)
    {
    case FacilityLayout::LampMood::Flicker: return LightMood::Flicker;
    case FacilityLayout::LampMood::Failing: return LightMood::Failing;
    case FacilityLayout::LampMood::Dead: return LightMood::Dead;
    case FacilityLayout::LampMood::Emergency: return LightMood::Pulse;
    case FacilityLayout::LampMood::Steady: break;
    }
    return LightMood::Steady;
}

uint32_t Mix(uint32_t a, uint32_t b)
{
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u);
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

// A colour for a vertex (MeshVertex::color: linear, opaque).
uint32_t PaintVertex(const glm::vec3& c)
{
    const auto channel = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return channel(c.r) | (channel(c.g) << 8) | (channel(c.b) << 16) | (255u << 24);
}

// 0 to 1 from a few numbers, the same everywhere.
float Hash01(uint32_t a, uint32_t b, uint32_t c)
{
    return static_cast<float>(Mix(Mix(a, b), c) & 0xFFFFFFu) / static_cast<float>(0x1000000u);
}

// A pebble about a unit across: a squashed, uneven double pyramid of six points, faceted (each face its own corners, so it
// is lit as a chipped stone, not a smooth one), a third of it under its middle.
MeshData Pebble(uint32_t seed)
{
    const auto r = [&](uint32_t n, float low, float high) { return low + (high - low) * Hash01(seed, n, 0x9EBBu); };
    const glm::vec3 points[6] = {{r(1, 0.38f, 0.55f), r(2, -0.08f, 0.08f), 0.0f},
                                 {-r(3, 0.38f, 0.55f), r(4, -0.08f, 0.08f), r(5, -0.1f, 0.1f)},
                                 {r(6, -0.1f, 0.1f), r(7, -0.08f, 0.08f), r(8, 0.32f, 0.5f)},
                                 {r(9, -0.1f, 0.1f), r(10, -0.08f, 0.08f), -r(11, 0.32f, 0.5f)},
                                 {r(12, -0.12f, 0.12f), r(13, 0.22f, 0.36f), r(14, -0.12f, 0.12f)},
                                 {0.0f, -r(15, 0.12f, 0.18f), 0.0f}};
    const int faces[8][3] = {{0, 4, 2}, {2, 4, 1}, {1, 4, 3}, {3, 4, 0}, {2, 5, 0}, {1, 5, 2}, {3, 5, 1}, {0, 5, 3}};
    MeshData mesh;
    for (const auto& face : faces)
    {
        const glm::vec3 a = points[face[0]];
        const glm::vec3 b = points[face[1]];
        const glm::vec3 c = points[face[2]];
        glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
        // Wound to face out, whichever way it came.
        const bool flip = glm::dot(n, a + b + c) < 0.0f;
        n = flip ? -n : n;
        const auto base = static_cast<uint32_t>(mesh.vertices.size());
        for (const glm::vec3& at : {a, flip ? c : b, flip ? b : c})
        {
            MeshVertex vertex;
            vertex.position = at;
            vertex.normal = n;
            vertex.uv = {at.x, at.z};
            mesh.vertices.push_back(vertex);
        }
        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2});
    }
    return mesh;
}

void Append(WorldObjects::Placements& into, const WorldObjects::Placements& from)
{
    into.doors.insert(into.doors.end(), from.doors.begin(), from.doors.end());
    into.lockers.insert(into.lockers.end(), from.lockers.begin(), from.lockers.end());
    into.ammoCrates.insert(into.ammoCrates.end(), from.ammoCrates.begin(), from.ammoCrates.end());
    into.items.insert(into.items.end(), from.items.begin(), from.items.end());
}

// A passage joining two buildings under the ground (SitePlan::Tunnel), built as their basements are: a floor and a roof along
// each straight stretch of it, walls down both sides in runs -- those along x reaching round the corners, those along z
// stopping at them, and both stopping at the cladding of the building at either end, where its doorway is -- and lamps along
// it. Where they are and what they light, for lamps to be put up there.
struct TunnelLamp
{
    glm::vec3 at{0.0f};
    glm::vec3 low{0.0f};
    glm::vec3 high{0.0f};
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    bool wall = false; // a caged lamp over a doorway, rather than one in the roof
};

// Whether a world grows grass, and the colour of its ground grown over far off (SiteMap::BuildFrom: the ground's colouring, and what
// is laid to look like it).
bool Grassy(const SiteMap::Look& look)
{
    return look.biome == "jungle" || look.biome == "temperate" || look.biome == "oceanic";
}
glm::vec3 Growing(const SiteMap::Look& look)
{
    return glm::mix(look.ground, look.biome == "jungle" ? glm::vec3(0.05f, 0.13f, 0.03f) : glm::vec3(0.08f, 0.16f, 0.04f), 0.65f);
}

// The snow lying on a building's roofs on a frozen world, `deep` metres or so: over each, inside the parapet round it and up to the
// walls of any part standing over it, drifted deeper against them, uneven -- what stands on the roof standing up out of it. Half a
// metre fine, its height the same function of where it is in every cell, so where two cells meet it does too.
MeshData RoofSnow(const FacilityLayout& building, float deep, uint32_t seed)
{
    MeshData mesh;
    constexpr float kCell = FacilityLayout::kCell;
    constexpr int kSteps = 6;
    const glm::vec2 origin{building.origin.x, building.origin.z};
    const auto roof = [&](int x, int z) { return building.Height(x, z); };
    // How far into a cell its roof's snow stops short of an edge of it with a wall along it: the parapet's inner face round its own
    // roof (its neighbour lower), the cladding of a part standing over it (higher); none where the roof runs on.
    const auto margin = [&](int h, int beside)
    {
        return beside == h ? 0.0f : beside < h ? FacilitySpec::kHalfWall + 0.01f : FacilitySpec::kShellThickness + FacilitySpec::kCladding - FacilitySpec::kHalfWall + 0.01f;
    };
    const glm::ivec2 steps[4] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    // How far a place on a roof is from the nearest wall round it (to two metres).
    const auto fromWall = [&](glm::vec2 p, int h)
    {
        const glm::vec2 local = (p - origin) / kCell;
        const int cx = static_cast<int>(std::floor(local.x));
        const int cz = static_cast<int>(std::floor(local.y));
        float nearest = 2.0f;
        for (int dz = -1; dz <= 1; ++dz)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                const int x = cx + dx;
                const int z = cz + dz;
                if (roof(x, z) != h)
                {
                    continue;
                }
                const glm::vec2 lo = origin + glm::vec2(static_cast<float>(x), static_cast<float>(z)) * kCell;
                for (int e = 0; e < 4; ++e)
                {
                    const int beside = roof(x + steps[e].x, z + steps[e].y);
                    if (beside == h)
                    {
                        continue;
                    }
                    const float line = e == 0 ? lo.x : e == 1 ? lo.x + kCell : e == 2 ? lo.y : lo.y + kCell;
                    const float along = e < 2 ? p.y : p.x;
                    const float a = e < 2 ? lo.y : lo.x;
                    const float off = std::max(std::max(a - along, along - (a + kCell)), 0.0f);
                    const float across = std::abs((e < 2 ? p.x : p.y) - line) - margin(h, beside);
                    nearest = std::min(nearest, std::sqrt(off * off + std::max(across, 0.0f) * std::max(across, 0.0f)));
                }
            }
        }
        return nearest;
    };
    // Uneven: a value every metre and a half, eased between.
    const auto uneven = [&](glm::vec2 p)
    {
        const glm::vec2 q = p / 1.5f + glm::vec2(4096.0f);
        const auto ix = static_cast<uint32_t>(q.x);
        const auto iz = static_cast<uint32_t>(q.y);
        const float fx = glm::smoothstep(0.0f, 1.0f, q.x - std::floor(q.x));
        const float fz = glm::smoothstep(0.0f, 1.0f, q.y - std::floor(q.y));
        const auto at = [&](uint32_t i, uint32_t k) { return Hash01(seed, i, k); };
        return glm::mix(glm::mix(at(ix, iz), at(ix + 1u, iz), fx), glm::mix(at(ix, iz + 1u), at(ix + 1u, iz + 1u), fx), fz);
    };
    const auto height = [&](glm::vec2 p, int h)
    {
        const float drift = 1.0f - glm::smoothstep(0.0f, 2.0f, fromWall(p, h));
        // (Never over the parapet: it stands kParapet over the roof.)
        return std::min(deep * (0.55f + 0.45f * uneven(p)) + deep * 1.1f * drift * drift, FacilitySpec::kParapet - 0.15f);
    };
    for (int z = 0; z < building.depth; ++z)
    {
        for (int x = 0; x < building.width; ++x)
        {
            const int h = roof(x, z);
            // (Not a buried part's roof: the ground is over that.)
            if (h <= building.ground)
            {
                continue;
            }
            const float top = building.origin.y + static_cast<float>(h) * FacilityLayout::kStorey;
            const glm::vec2 lo = origin + glm::vec2(static_cast<float>(x), static_cast<float>(z)) * kCell +
                                 glm::vec2(margin(h, roof(x - 1, z)), margin(h, roof(x, z - 1)));
            const glm::vec2 hi = origin + glm::vec2(static_cast<float>(x + 1), static_cast<float>(z + 1)) * kCell -
                                 glm::vec2(margin(h, roof(x + 1, z)), margin(h, roof(x, z + 1)));
            const auto base = static_cast<uint32_t>(mesh.vertices.size());
            for (int k = 0; k <= kSteps; ++k)
            {
                for (int i = 0; i <= kSteps; ++i)
                {
                    const glm::vec2 p = glm::mix(lo, hi, glm::vec2(static_cast<float>(i), static_cast<float>(k)) / static_cast<float>(kSteps));
                    MeshVertex vertex;
                    vertex.position = {p.x, top + height(p, h), p.y};
                    const float e = 0.2f;
                    const float dx = height(p + glm::vec2(e, 0.0f), h) - height(p - glm::vec2(e, 0.0f), h);
                    const float dz = height(p + glm::vec2(0.0f, e), h) - height(p - glm::vec2(0.0f, e), h);
                    vertex.normal = glm::normalize(glm::vec3(-dx, 2.0f * e, -dz));
                    vertex.uv = p;
                    mesh.vertices.push_back(vertex);
                }
            }
            for (int k = 0; k < kSteps; ++k)
            {
                for (int i = 0; i < kSteps; ++i)
                {
                    const uint32_t a = base + static_cast<uint32_t>(k * (kSteps + 1) + i);
                    const uint32_t c = a + static_cast<uint32_t>(kSteps + 1);
                    mesh.indices.insert(mesh.indices.end(), {a, c, a + 1, a + 1, c, c + 1});
                }
            }
        }
    }
    return mesh;
}

// Sets a building into a hillside if it can be (FacilityLayout::Bank): the sides tried in turn from one its roll picks, and the first
// that will take the bank taken -- the whole of it standing to the ground along that wall and as far along the walls either side as
// the bank goes; no way out on it, nor on those beside it nearer than the bank comes and a couple of metres more; and nothing else of
// the site where the land rises behind it and beside it (another building, anything standing about, the landing). Whether it was.
bool SetIntoHillside(SitePlan& plan, size_t index, uint32_t roll)
{
    FacilityLayout& building = plan.buildings[index];
    glm::vec2 min;
    glm::vec2 max;
    SitePlan::Footprint(building, 0.0f, min, max);
    constexpr float kCell = FacilityLayout::kCell;
    for (int k = 0; k < 4; ++k)
    {
        const int side = static_cast<int>(((roll >> 8) + static_cast<uint32_t>(k)) % 4u);
        // Its cells by how far along the wall (`along`) and how many in from it (`in`).
        const int across = side < 2 ? building.depth : building.width;
        const int deep = side < 2 ? building.width : building.depth;
        const auto cell = [&](int along, int in)
        {
            return side == 0 ? glm::ivec2(in, along) : side == 1 ? glm::ivec2(building.width - 1 - in, along) : side == 2 ? glm::ivec2(along, in)
                                                                                                                     : glm::ivec2(along, building.depth - 1 - in);
        };
        int lowest = building.floors;
        for (int i = 0; i < across; ++i)
        {
            const glm::ivec2 c = cell(i, 0);
            lowest = std::min(lowest, building.Height(c.x, c.y));
        }
        if (lowest <= building.ground)
        {
            continue;
        }
        FacilityLayout::Bank bank;
        bank.side = side;
        // Over the ground floor's back, and up its first floor's foot where there is one -- under the sills of its windows; where it
        // is one storey, a little under its roof.
        bank.height = lowest - building.ground >= 2 ? FacilityLayout::kStorey + 0.35f : FacilityLayout::kStorey - 0.55f;
        bank.run = bank.height * (2.0f + 0.15f * static_cast<float>((roll >> 16) % 4u));
        bank.hold = static_cast<float>(deep) * kCell * (0.2f + 0.05f * static_cast<float>((roll >> 20) % 4u));
        bool open = true;
        for (const FacilityLayout::Exit& exit : building.exits)
        {
            if (exit.side == side)
            {
                open = false;
                break;
            }
            if (exit.side / 2 != side / 2)
            {
                // How far forward of the wall its middle is.
                const glm::vec2 mid = glm::vec2(building.origin.x, building.origin.z) + (glm::vec2(exit.cell) + 0.5f) * kCell;
                const float forward = side == 0 ? mid.x - min.x : side == 1 ? max.x - mid.x : side == 2 ? mid.y - min.y : max.y - mid.y;
                bank.hold = std::min(bank.hold, forward - bank.run - 2.0f);
            }
        }
        if (!open || bank.hold < 0.0f)
        {
            continue;
        }
        for (int in = 0; open && static_cast<float>(in) * kCell < bank.hold + bank.run; ++in)
        {
            for (const int along : {0, across - 1})
            {
                const glm::ivec2 c = cell(along, in);
                open = open && building.Height(c.x, c.y) > building.ground;
            }
        }
        // Where the land rises: thirty metres behind, twelve out either side, from there forward as far as the bank goes and a little.
        glm::vec2 lo = min - glm::vec2(12.0f);
        glm::vec2 hi = max + glm::vec2(12.0f);
        const float reach = bank.hold + bank.run + 4.0f;
        switch (side)
        {
        case 0: lo.x = min.x - 30.0f; hi.x = min.x + reach; break;
        case 1: hi.x = max.x + 30.0f; lo.x = max.x - reach; break;
        case 2: lo.y = min.y - 30.0f; hi.y = min.y + reach; break;
        default: hi.y = max.y + 30.0f; lo.y = max.y - reach; break;
        }
        const auto in = [&](glm::vec2 a, glm::vec2 b) { return a.x < hi.x && b.x > lo.x && a.y < hi.y && b.y > lo.y; };
        for (size_t b = 0; open && b < plan.buildings.size(); ++b)
        {
            glm::vec2 a0;
            glm::vec2 a1;
            SitePlan::Footprint(plan.buildings[b], 2.0f, a0, a1);
            open = b == index || !in(a0, a1);
        }
        for (const SitePlan::Block& block : plan.blocks)
        {
            const float half = std::max(block.size.x, block.size.z) * 0.5f;
            const glm::vec2 at{block.centre.x, block.centre.z};
            open = open && (block.outlying || !in(at - glm::vec2(half), at + glm::vec2(half)));
        }
        for (const glm::vec3& at : {plan.landing, plan.rampFoot.position})
        {
            open = open && !in(glm::vec2(at.x, at.z) - glm::vec2(8.0f), glm::vec2(at.x, at.z) + glm::vec2(8.0f));
        }
        if (open)
        {
            building.bank = bank;
            return true;
        }
    }
    return false;
}

// A building raised on stilts (SiteMap::m_stilts): steel piles under it, every other corner of its cells from a little into the ground
// up to its underside; and at each of its ways out a landing in front of the door on two posts, steel stairs down from it to the
// ground, railed both sides. Felt where anybody stands or walks into them.
// The pad of a site on the sea's floor (you, 2026-11-08: better than a floating slab, and a way back onto it): a landing platform
// standing in the sea on four legs, laid along the way the shuttle's ramp comes down (`along`, flat, from the shuttle to its ramp's foot) -- a steel
// deck 25 m across well over the waves (kPontoonDeck: you, 2026-11-09, the waves came up through it), braced between its legs; yellow and black along its edges and a landing
// square on it; a rail round it, but for the gaps where three ladders come up its sides out of the water (climbed back onto it from the
// sea: `ladders`); a red beacon at each corner on a short post and two floodlights over its back corners on poles (`lamps`: where
// each light is and which way it shines; red ones pulse). Felt all of it.
struct PontoonLamp
{
    glm::vec3 at;
    glm::vec3 direction;
    bool beacon;
};
void BuildPontoon(const glm::vec3& middle, const glm::vec2& along, float deckTop, float level, float bottom, MapBuilder& builder, const Material& deck,
                  const Material& floats, const Material& steel, const Material& hazard, std::vector<Ladder>& ladders, std::vector<PontoonLamp>& lamps,
                  std::vector<glm::vec3>& posts)
{
    constexpr float kHalf = 12.5f;
    const glm::vec3 fwd = glm::normalize(glm::vec3(along.x, 0.0f, along.y));
    const glm::vec3 right{-fwd.z, 0.0f, fwd.x};
    const glm::quat turn = glm::angleAxis(std::atan2(fwd.x, fwd.z), glm::vec3(0.0f, 1.0f, 0.0f));
    // A point in its own terms: across it (x), up from the deck's top (y), along it (z).
    const auto at = [&](float x, float y, float z) { return glm::vec3(middle.x, deckTop, middle.z) + right * x + fwd * z + glm::vec3(0.0f, y, 0.0f); };
    const auto box = [&](const char* name, glm::vec3 lo, glm::vec3 hi, const Material& material, bool felt)
    {
        Transform where;
        where.position = at((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f);
        where.rotation = turn;
        const glm::vec3 size = glm::abs(hi - lo);
        if (felt)
        {
            builder.AddBox(name, where, size, material);
        }
        else
        {
            builder.AddMesh(name, where, Primitives::Box(size), material, false);
        }
    };
    // The deck: plate over a frame of beams.
    box("site_pontoon_deck", {-kHalf, -0.25f, -kHalf}, {kHalf, 0.0f, kHalf}, deck, true);
    for (const float z : {-kHalf + 0.3f, -4.0f, 4.0f, kHalf - 0.3f})
    {
        box("site_pontoon_beam", {-kHalf + 0.2f, -0.75f, z - 0.15f}, {kHalf - 0.2f, -0.25f, z + 0.15f}, steel, false);
    }
    // Yellow and black along its edges, and a landing square where the shuttle stands -- painted on, a hair over the plate.
    for (int side = 0; side < 4; ++side)
    {
        for (float s = -kHalf; s < kHalf - 0.01f; s += 1.0f)
        {
            if (static_cast<int>(std::floor(s)) % 2 != 0)
            {
                continue;
            }
            const float e0 = kHalf - 0.45f;
            const glm::vec3 lo = side < 2 ? glm::vec3((side == 0 ? -1.0f : 1.0f) * e0, 0.0f, s) : glm::vec3(s, 0.0f, (side == 2 ? -1.0f : 1.0f) * e0);
            const glm::vec3 hi = side < 2 ? glm::vec3((side == 0 ? -1.0f : 1.0f) * kHalf, 0.006f, s + 1.0f) : glm::vec3(s + 1.0f, 0.006f, (side == 2 ? -1.0f : 1.0f) * kHalf);
            box("site_pontoon_stripe", lo, hi, hazard, false);
        }
    }
    for (const glm::vec4& strip : {glm::vec4(-8.0f, -8.0f, 8.0f, -7.75f), glm::vec4(-8.0f, 7.75f, 8.0f, 8.0f), glm::vec4(-8.0f, -7.75f, -7.75f, 7.75f),
                                   glm::vec4(7.75f, -7.75f, 8.0f, 7.75f)})
    {
        box("site_pontoon_mark", {strip.x, 0.0f, strip.y}, {strip.z, 0.005f, strip.w}, hazard, false);
    }
    // Its legs: four steel columns from well into the sea's floor (`bottom`) up under its corners, wider at the water where the sea
    // works at them; braced across between them over the water, at the water and under it -- a fixed platform, its deck well over
    // the highest a storm's crests reach (kPontoonDeck), not a float the waves ran over.
    for (const float x : {-9.0f, 9.0f})
    {
        for (const float z : {-9.0f, 9.0f})
        {
            const float tall = deckTop - 0.75f - bottom;
            Transform leg;
            leg.position = at(x, (bottom - deckTop - 0.75f) * 0.5f, z);
            builder.AddMesh("site_pontoon_float", leg, Primitives::Cylinder(0.75f, tall, 20), floats, true);
            // (A wider collar where the sea rises and falls on it.)
            Transform collar;
            collar.position = at(x, level - deckTop, z);
            builder.AddMesh("site_pontoon_float", collar, Primitives::Cylinder(0.9f, 3.2f, 20), floats, false);
            posts.emplace_back(collar.position.x, collar.position.z, 0.9f);
        }
    }
    for (const float y : {-1.2f, level + 1.6f - deckTop, level - 4.0f - deckTop})
    {
        if (y < bottom - deckTop + 0.5f)
        {
            continue;
        }
        for (const float s : {-9.0f, 9.0f})
        {
            box("site_pontoon_brace", {-9.0f, y - 0.2f, s - 0.2f}, {9.0f, y + 0.2f, s + 0.2f}, steel, y > level - deckTop);
            box("site_pontoon_brace", {s - 0.2f, y - 0.2f, -9.0f}, {s + 0.2f, y + 0.2f, 9.0f}, steel, y > level - deckTop);
        }
    }
    // The ladders up out of the water: in the middle of either side, and of its back (the ramp's way is its front).
    struct Way
    {
        glm::vec3 edge; // the middle of its foot on the deck's edge, in its own terms
        glm::vec3 out;  // outwards, in its own terms
    };
    const Way ways[3] = {{{-kHalf, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}}, {{kHalf, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}, {{0.0f, 0.0f, -kHalf}, {0.0f, 0.0f, -1.0f}}};
    constexpr float kGap = 0.75f;
    constexpr float kStile = 0.25f;
    const float down = deckTop - (level - 2.2f);
    for (const Way& way : ways)
    {
        const glm::vec3 across{-way.out.z, 0.0f, way.out.x};
        const glm::vec3 foot = at(way.edge.x, -down, way.edge.z);
        const glm::vec3 outWorld = right * way.out.x + fwd * way.out.z;
        const glm::vec3 acrossWorld = right * across.x + fwd * across.z;
        for (const float s : {-kStile, kStile})
        {
            const glm::vec3 a = foot + acrossWorld * s + outWorld * Ladder::kRungOut;
            Transform stile;
            stile.position = a + glm::vec3(0.0f, (down + 1.0f) * 0.5f, 0.0f);
            builder.AddMesh("site_pontoon_ladder", stile, Primitives::Cylinder(0.03f, down + 1.0f, 8), steel, false);
            // Bent over the edge at the top, down onto the deck, to hold climbing over.
            Transform over;
            over.position = a + glm::vec3(0.0f, down + 1.0f, 0.0f) - outWorld * 0.2f;
            over.rotation = glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), outWorld);
            builder.AddMesh("site_pontoon_ladder", over, Primitives::Cylinder(0.03f, 0.45f, 8), steel, false);
        }
        for (float y = Ladder::kRungSpacing; y < down; y += Ladder::kRungSpacing)
        {
            Transform rung;
            rung.position = foot + outWorld * Ladder::kRungOut + glm::vec3(0.0f, y, 0.0f);
            rung.rotation = glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), acrossWorld);
            builder.AddMesh("site_pontoon_ladder", rung, Primitives::Cylinder(0.018f, kStile * 2.0f, 8), steel, false);
        }
        Ladder ladder;
        ladder.foot = foot;
        ladder.out = outWorld;
        ladder.height = down;
        ladder.width = kStile * 2.0f + 0.05f;
        ladders.push_back(ladder);
    }
    // The rail round it: posts, a top rail and a knee rail, felt as a low wall -- but for the ladders' gaps.
    const auto railRun = [&](glm::vec3 a, glm::vec3 b)
    {
        const glm::vec3 run = b - a;
        const float length = glm::length(run);
        if (length < 0.3f)
        {
            return;
        }
        const glm::vec3 lo = glm::min(a, b) - glm::vec3(0.03f, 0.0f, 0.03f);
        const glm::vec3 hi = glm::max(a, b) + glm::vec3(0.03f, 1.1f, 0.03f);
        box("site_pontoon_rail", lo + glm::vec3(0.0f, 1.05f, 0.0f), hi, steel, false);
        box("site_pontoon_rail", lo + glm::vec3(0.0f, 0.52f, 0.0f), hi - glm::vec3(0.0f, 0.53f, 0.0f), steel, false);
        {
            Transform felt;
            felt.position = at((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f);
            felt.rotation = turn;
            builder.AddSolid(felt, glm::abs(hi - lo)); // (felt and not seen: the rails are drawn as rails)
        }
        const int posts = std::max(1, static_cast<int>(std::ceil(length / 2.4f)));
        for (int k = 0; k <= posts; ++k)
        {
            const glm::vec3 p = a + run * (static_cast<float>(k) / static_cast<float>(posts));
            box("site_pontoon_rail", p - glm::vec3(0.04f, 0.0f, 0.04f), p + glm::vec3(0.04f, 1.1f, 0.04f), steel, false);
        }
    };
    constexpr float kEdge = kHalf - 0.12f;
    railRun({-kEdge, 0.0f, -kEdge}, {-kEdge, 0.0f, -kGap});
    railRun({-kEdge, 0.0f, kGap}, {-kEdge, 0.0f, kEdge});
    railRun({kEdge, 0.0f, -kEdge}, {kEdge, 0.0f, -kGap});
    railRun({kEdge, 0.0f, kGap}, {kEdge, 0.0f, kEdge});
    railRun({-kEdge, 0.0f, -kEdge}, {-kGap, 0.0f, -kEdge});
    railRun({kGap, 0.0f, -kEdge}, {kEdge, 0.0f, -kEdge});
    railRun({-kEdge, 0.0f, kEdge}, {kEdge, 0.0f, kEdge});
    // Its lights: a red beacon on a short post at each corner; a floodlight on a pole over each back corner, looking in over the deck.
    for (const float x : {-kHalf + 0.5f, kHalf - 0.5f})
    {
        for (const float z : {-kHalf + 0.5f, kHalf - 0.5f})
        {
            box("site_pontoon_post", {x - 0.06f, 0.0f, z - 0.06f}, {x + 0.06f, 0.9f, z + 0.06f}, steel, true);
            lamps.push_back({at(x, 0.98f, z), {0.0f, 1.0f, 0.0f}, true});
        }
        const float z = -kHalf + 0.6f;
        box("site_pontoon_pole", {x - 0.08f, 0.0f, z - 0.08f}, {x + 0.08f, 6.0f, z + 0.08f}, steel, true);
        // (Its lamp on an arm out over the deck from the pole's top, a finned body turned down at the middle of the deck.)
        box("site_pontoon_pole", {std::min(x, x * 0.93f) - 0.04f, 5.94f, z - 0.04f}, {std::max(x, x * 0.93f) + 0.04f, 6.02f, z + 0.54f}, steel, false);
        box("site_pontoon_lamp", {x * 0.93f - 0.22f, 5.8f, z + 0.32f}, {x * 0.93f + 0.22f, 6.0f, z + 0.68f}, steel, false);
        const glm::vec3 head = at(x * 0.93f, 5.78f, z + 0.5f);
        lamps.push_back({head, glm::normalize(at(0.0f, 0.0f, 0.0f) - head), false});
    }
}

void BuildStilts(const FacilityLayout& building, float lift, MapBuilder& builder, const Material& steel, const Material& tread, bool landings = true)
{
    const float floor = building.origin.y;           // its ground floor, raised
    const float ground = floor - lift;                // the ground under it
    const float under = floor - FacilityLayout::kSlab; // its underside
    const auto block = [&](const char* name, const glm::vec3& lo, const glm::vec3& hi, const Material& material, bool solid)
    {
        Transform where;
        where.position = (lo + hi) * 0.5f;
        const glm::vec3 size = glm::abs(hi - lo);
        if (solid)
        {
            builder.AddBox(name, where, size, material);
        }
        else
        {
            builder.AddMesh(name, where, Primitives::Box(size), material, false);
        }
    };
    constexpr float kPile = 0.18f; // half a pile across
    for (int z = 0; z <= building.depth; z += 2)
    {
        for (int x = 0; x <= building.width; x += 2)
        {
            if (building.Height(x, z) <= 0 && building.Height(x - 1, z) <= 0 && building.Height(x, z - 1) <= 0 && building.Height(x - 1, z - 1) <= 0)
            {
                continue;
            }
            const glm::vec3 at = FacilityMap::ToWorld(building, 0, glm::vec2(static_cast<float>(x), static_cast<float>(z)));
            block("site_stilt", {at.x - kPile, ground - 0.4f, at.z - kPile}, {at.x + kPile, under, at.z + kPile}, steel, true);
        }
    }
    constexpr float kLanding = 1.6f; // out from the wall
    constexpr float kHalf = 0.95f;   // half the landing across, and the stairs a little narrower
    constexpr float kRun = 0.3f;
    // (None on the sea's floor: its ways out are lockouts into the water, swum to.)
    for (const FacilityLayout::Exit& exit : landings ? building.exits : std::vector<FacilityLayout::Exit>{})
    {
        const glm::vec2 out2 = exit.side == 0 ? glm::vec2(-1.0f, 0.0f) : exit.side == 1 ? glm::vec2(1.0f, 0.0f) : exit.side == 2 ? glm::vec2(0.0f, -1.0f) : glm::vec2(0.0f, 1.0f);
        const glm::vec3 out{out2.x, 0.0f, out2.y};
        const glm::vec3 along{std::abs(out.z), 0.0f, std::abs(out.x)};
        const glm::vec3 face = FacilityMap::ToWorld(building, building.ground, glm::vec2(exit.cell) + glm::vec2(0.5f) + out2 * 0.5f) +
                               out * (FacilitySpec::kShellThickness + FacilitySpec::kCladding - FacilitySpec::kHalfWall);
        const auto span = [&](float o0, float o1, float a0, float a1, float y0, float y1)
        {
            const glm::vec3 p = face + out * o0 + along * a0;
            const glm::vec3 q = face + out * o1 + along * a1;
            return std::make_pair(glm::vec3(std::min(p.x, q.x), y0, std::min(p.z, q.z)), glm::vec3(std::max(p.x, q.x), y1, std::max(p.z, q.z)));
        };
        // The landing, a hair off the wall, on its two posts.
        {
            const auto [lo, hi] = span(0.01f, kLanding, -kHalf, kHalf, floor - 0.12f, floor);
            block("site_stilt_landing", lo, hi, tread, true);
        }
        for (const float s : {-1.0f, 1.0f})
        {
            const auto [lo, hi] = span(kLanding - 0.14f, kLanding - 0.02f, s * kHalf - 0.06f, s * kHalf + 0.06f, ground - 0.3f, floor - 0.12f);
            block("site_stilt", lo, hi, steel, true);
        }
        // The stairs down from it, tread by tread, a stringer either side.
        const int steps = std::max(2, static_cast<int>(std::ceil(lift / 0.19f)));
        const float rise = lift / static_cast<float>(steps);
        for (int k = 1; k < steps; ++k)
        {
            const float o = kLanding + kRun * static_cast<float>(k - 1);
            const auto [lo, hi] = span(o, o + kRun, -kHalf + 0.15f, kHalf - 0.15f, floor - rise * static_cast<float>(k) - 0.05f, floor - rise * static_cast<float>(k));
            block("site_stilt_tread", lo, hi, tread, true);
        }
        const float foot = kLanding + kRun * static_cast<float>(steps - 1);
        for (const float s : {-1.0f, 1.0f})
        {
            // (A stringer down each side under the treads' ends, and a rail over it at a hand's height -- drawn as blocks stepping
            // down with the treads, felt as the rail is.)
            for (int k = 1; k < steps; ++k)
            {
                const float o = kLanding + kRun * static_cast<float>(k - 1);
                const float top = floor - rise * static_cast<float>(k);
                const auto [lo, hi] = span(o, o + kRun, s * (kHalf - 0.15f) - (s > 0.0f ? 0.0f : 0.06f), s * (kHalf - 0.15f) + (s > 0.0f ? 0.06f : 0.0f),
                                           top - 0.22f, top - 0.051f);
                block("site_stilt_stringer", lo, hi, steel, false);
            }
            const float a = s * (kHalf - 0.12f);
            for (const float o : {kLanding, foot})
            {
                const float base = o == kLanding ? floor : ground + rise;
                const auto [lo, hi] = span(o - 0.03f, o + 0.03f, a - 0.03f, a + 0.03f, base, base + 1.0f);
                block("site_stilt_rail", lo, hi, steel, false);
            }
            // And the rail down the stairs between them.
            {
                const glm::vec3 p0 = face + out * kLanding + along * a + glm::vec3(0.0f, 1.0f, 0.0f);
                const glm::vec3 p1 = face + out * foot + along * a + glm::vec3(0.0f, ground + rise + 1.0f - floor, 0.0f);
                Transform where;
                where.position = (p0 + p1) * 0.5f;
                where.rotation = glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), glm::normalize(p1 - p0));
                builder.AddMesh("site_stilt_rail", where, Primitives::Box({0.05f, glm::length(p1 - p0), 0.05f}), steel, false);
            }
            // The landing's own rail along its side, from the wall to the stairs.
            const auto [lo, hi] = span(0.05f, kLanding, s * kHalf - 0.03f, s * kHalf + 0.03f, floor + 0.97f, floor + 1.03f);
            block("site_stilt_rail", lo, hi, steel, false);
            for (const float o : {0.05f, kLanding * 0.5f})
            {
                const auto [plo, phi] = span(o - 0.03f, o + 0.03f, s * kHalf - 0.03f, s * kHalf + 0.03f, floor, floor + 0.97f);
                block("site_stilt_rail", plo, phi, steel, false);
            }
        }
    }
}

// What was left down a cave by whoever worked it (SiteMap::BuildFrom): now and then in a chamber, out of any water standing there, a
// cache on its floor -- a few things to light the way and patch yourself up with -- and with some of them an ammunition crate; the
// most at the far end. The same everywhere, by the cave's seed.
void CaveCaches(const SitePlan::Cave& cave, WorldObjects::Placements& placements)
{
    for (size_t c = 0; c < cave.chambers.size(); ++c)
    {
        const glm::vec4& chamber = cave.chambers[c];
        const float floor = chamber.y - chamber.w * 0.45f;
        const uint32_t roll = Mix(cave.seed, 0xCAC4Eu + static_cast<uint32_t>(c));
        const bool last = c + 1 == cave.chambers.size();
        if (floor < cave.water + 0.3f || (!last && roll % 100u >= 55u))
        {
            continue;
        }
        // Against the chamber's side, a little in from where its floor meets the wall, on a side of its own -- each thing on the floor
        // found under it (the cave's floor runs from one part's to the next, and is not quite any one's).
        const float angle = static_cast<float>(roll % 360u) * glm::pi<float>() / 180.0f;
        const glm::vec3 out{std::cos(angle), 0.0f, std::sin(angle)};
        const glm::vec3 at = glm::vec3(chamber.x, floor, chamber.z) + out * (chamber.w * 0.38f); // (its floor reaches about 0.57 of it out)
        const glm::vec3 across{-out.z, 0.0f, out.x};
        const auto onFloor = [&cave](glm::vec3 p)
        {
            p.y = cave.FloorUnder(p);
            return p;
        };
        const int count = last ? 4 : 2 + static_cast<int>((roll >> 8) % 2u);
        for (int k = 0; k < count; ++k)
        {
            const uint32_t pick = (Mix(roll, static_cast<uint32_t>(k)) % 100u);
            const char* key = pick < 40 ? "battery" : pick < 75 ? "flare" : "medkit";
            placements.items.push_back({key, 1, onFloor(at + across * (static_cast<float>(k) - static_cast<float>(count - 1) * 0.5f) * 0.45f) + glm::vec3(0.0f, 0.02f, 0.0f)});
        }
        if (last || (roll >> 12) % 3u == 0u)
        {
            placements.ammoCrates.push_back({onFloor(at - out * 0.6f), std::atan2(-out.x, -out.z)});
        }
    }
}

// A cave (SitePlan::Cave): the adit out to it from the building's doorway -- rock walls, floor and roof, timbered every metre or
// so as the workings' drives are -- and the cave itself, its face where the open space of it meets the rock (SurfaceNets, faced
// into the open space), the rock's own, felt. Its triangle count kept down; how long it took, logged.
void BuildCave(const SitePlan::Cave& cave, const SiteTerrain& terrain, MapBuilder& builder, const Material& rock, const Material& timber)
{
    // Each adit's boards and timbers; its rock is the cave's own (below), so the one runs into the other. (Built of boxes, the cave's
    // open space came round the outside of their walls where the two met, the rock against one side and a gap down the other.)
    std::vector<SitePlan::Cave::Adit> adits{cave.First()};
    adits.insert(adits.end(), cave.joins.begin(), cave.joins.end());
    constexpr float kHalf = SitePlan::Cave::kAditHalf;
    constexpr float kHigh = SitePlan::Cave::kAditHigh;
    for (const SitePlan::Cave::Adit& adit : adits)
    {
        const glm::vec3 across{-adit.out.z, 0.0f, adit.out.x};
        const glm::vec3 alongAbs = glm::abs(adit.out);
        const glm::vec3 acrossAbs = glm::abs(across);
        // A box from `a0` to `a1` out along the adit, `s0` to `s1` across it, `y0` to `y1` up from its floor.
        const auto box = [&](const char* name, float a0, float a1, float s0, float s1, float y0, float y1, const Material& material, bool solid)
        {
            Transform where;
            where.position = adit.mouth + adit.out * ((a0 + a1) * 0.5f) + across * ((s0 + s1) * 0.5f) + glm::vec3(0.0f, (y0 + y1) * 0.5f, 0.0f);
            const glm::vec3 size = alongAbs * (a1 - a0) + acrossAbs * (s1 - s0) + glm::vec3(0.0f, y1 - y0, 0.0f);
            if (solid)
            {
                builder.AddBox(name, where, size, material);
            }
            else
            {
                builder.AddMesh(name, where, Primitives::Box(size), material, false);
            }
        };
        box("site_adit", 0.0f, adit.length, -kHalf + 0.05f, kHalf - 0.05f, -0.3f, 0.03f, timber, true); // (its boards a step over the rock)
        for (float a = 0.6f; a < adit.length - 0.1f; a += 1.1f)
        {
            for (const float s : {-1.0f, 1.0f})
            {
                box("site_adit_timber", a - 0.09f, a + 0.09f, s * (kHalf - 0.2f) - 0.09f, s * (kHalf - 0.2f) + 0.09f, 0.0f, kHigh - 0.24f, timber, false);
            }
            box("site_adit_timber", a - 0.11f, a + 0.11f, -kHalf + 0.01f, kHalf - 0.01f, kHigh - 0.24f, kHigh - 0.02f, timber, false);
        }
    }

    // The cave: the open space of it outside "the shape", so its face looks into the open space -- and never within two and a half
    // metres of the ground over it, wherever the land dips (its roof flat there, under it).
    const auto started = std::chrono::steady_clock::now();
    const auto field = [&](const glm::vec3& at) { return -std::max(cave.Distance(at), at.y - (terrain.Height(at.x, at.z) - 2.5f)); };
    constexpr float kCellSize = 0.7f;
    const DistanceGrid grid = SampleDistance(cave.min, cave.max, kCellSize, field);
    MeshData surface = SurfaceNets(grid);
    for (MeshVertex& vertex : surface.vertices)
    {
        const auto gradient = [&](const glm::vec3& at)
        {
            const float e = kCellSize * 0.35f;
            const glm::vec3 g{field(at + glm::vec3(e, 0.0f, 0.0f)) - field(at - glm::vec3(e, 0.0f, 0.0f)), field(at + glm::vec3(0.0f, e, 0.0f)) - field(at - glm::vec3(0.0f, e, 0.0f)),
                              field(at + glm::vec3(0.0f, 0.0f, e)) - field(at - glm::vec3(0.0f, 0.0f, e))};
            const float length = glm::length(g);
            return length > 1.0e-8f ? g / length : glm::vec3(0.0f, 1.0f, 0.0f);
        };
        glm::vec3 p = vertex.position;
        p -= gradient(p) * field(p);
        vertex.position = p;
        vertex.normal = gradient(p);
    }
    // Open at each adit's mouth, flush with the building's wall, where its doorway is: the rock behind that (in the wall, and the
    // drive's end past it, into the building) cut away -- a triangle wholly behind it left out, the corners of one across it brought
    // back to it.
    {
        const auto behind = [&](const glm::vec3& p, glm::vec3& onto)
        {
            for (const SitePlan::Cave::Adit& adit : adits)
            {
                const glm::vec3 local = p - adit.mouth;
                const float along = glm::dot(local, adit.out);
                const float side = std::abs(local.x * adit.out.z - local.z * adit.out.x);
                if (along < 0.0f && along > -2.5f && side < kHalf + 1.6f && local.y > -1.0f && local.y < kHigh + 1.2f)
                {
                    onto = p - adit.out * along;
                    return true;
                }
            }
            return false;
        };
        std::vector<uint8_t> back(surface.vertices.size(), 0);
        std::vector<glm::vec3> onto(surface.vertices.size());
        for (size_t v = 0; v < surface.vertices.size(); ++v)
        {
            back[v] = behind(surface.vertices[v].position, onto[v]) ? 1 : 0;
        }
        std::vector<uint32_t> kept;
        kept.reserve(surface.indices.size());
        for (size_t t = 0; t + 2 < surface.indices.size(); t += 3)
        {
            const uint32_t a = surface.indices[t];
            const uint32_t b = surface.indices[t + 1];
            const uint32_t c = surface.indices[t + 2];
            if (back[a] != 0 && back[b] != 0 && back[c] != 0)
            {
                continue;
            }
            kept.insert(kept.end(), {a, b, c});
        }
        for (size_t v = 0; v < surface.vertices.size(); ++v)
        {
            if (back[v] != 0)
            {
                surface.vertices[v].position = onto[v];
            }
        }
        surface.indices = std::move(kept);
    }
    constexpr size_t kMostTriangles = 140000;
    if (surface.indices.size() / 3 > kMostTriangles)
    {
        std::vector<uint32_t> kept;
        surface = SimplifyMesh(surface, kMostTriangles, 0.05f, kept);
    }
    builder.AddMesh("site_cave", Transform{}, surface, rock, true);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    PRED_LOG_INFO(Gameplay, "Site cave: from {:.0f} {:.0f} {:.0f}, {} passages, {} chambers, {} triangles, in {} ms", cave.mouth.x, cave.mouth.y, cave.mouth.z,
                  cave.passages.size(), cave.chambers.size(), surface.indices.size() / 3, took);
}

// How high a passage under the ground is inside, floor to roof (BuildTunnel).
constexpr float kTunnelClear = 2.7f;

std::vector<TunnelLamp> BuildTunnel(const SitePlan& plan, const SitePlan::Tunnel& tunnel, MapBuilder& builder, const Material& wall,
                                    const Material& floor, const Material& roof)
{
    constexpr float kCell = FacilityLayout::kCell;
    constexpr float kClear = kTunnelClear;
    constexpr float kRoof = 0.3f;
    constexpr float kHalf = 0.1f;
    // How far a building's cladding stands out past the edge of its cells, into the passage's end.
    constexpr float kSkin = FacilitySpec::kShellThickness + FacilitySpec::kCladding - FacilitySpec::kHalfWall;
    const glm::vec2 origin{plan.origin.x, plan.origin.z};
    const float base = tunnel.floor;
    const auto X = [&](int i) { return origin.x + static_cast<float>(i) * kCell; };
    const auto Z = [&](int j) { return origin.y + static_cast<float>(j) * kCell; };
    const auto in = [&](glm::ivec2 c) { return std::find(tunnel.cells.begin(), tunnel.cells.end(), c) != tunnel.cells.end(); };
    // Which way each end's building is from it: the one cell beside it that is part of that building.
    const auto towards = [&](glm::ivec2 cell, int index)
    {
        const FacilityLayout& building = plan.buildings[static_cast<size_t>(index)];
        const glm::ivec2 corner = glm::ivec2(glm::round((glm::vec2(building.origin.x, building.origin.z) - origin) / kCell));
        for (const glm::ivec2 step : {glm::ivec2(1, 0), glm::ivec2(-1, 0), glm::ivec2(0, 1), glm::ivec2(0, -1)})
        {
            const glm::ivec2 local = cell + step - corner;
            if (building.Height(local.x, local.y) > 0)
            {
                return step;
            }
        }
        return glm::ivec2(0);
    };
    struct End
    {
        glm::ivec2 cell;
        glm::ivec2 building;
    };
    const End ends[2] = {{tunnel.cells.front(), towards(tunnel.cells.front(), tunnel.from)}, {tunnel.cells.back(), towards(tunnel.cells.back(), tunnel.to)}};
    // A box between two corners, in the world, its x and z from a cell's sides less what a building at an end takes of it.
    const auto box = [&](const std::string& name, const Material& material, glm::vec3 lo, glm::vec3 hi)
    {
        Transform transform;
        transform.position = (lo + hi) * 0.5f;
        builder.AddBox(name, transform, hi - lo, material);
    };

    // Floor and roof, a straight stretch at a time (a corner's cell the stretch before it's).
    std::vector<std::pair<size_t, size_t>> stretches;
    size_t start = 0;
    for (size_t i = 2; i < tunnel.cells.size(); ++i)
    {
        if (tunnel.cells[i] - tunnel.cells[i - 1] != tunnel.cells[i - 1] - tunnel.cells[i - 2])
        {
            stretches.push_back({start, i});
            start = i;
        }
    }
    stretches.push_back({start, tunnel.cells.size()});
    for (const auto& [first, last] : stretches)
    {
        glm::vec2 lo{1.0e9f};
        glm::vec2 hi{-1.0e9f};
        for (size_t k = first; k < last; ++k)
        {
            const glm::ivec2 c = tunnel.cells[k];
            glm::vec2 cellLo{X(c.x), Z(c.y)};
            glm::vec2 cellHi{X(c.x + 1), Z(c.y + 1)};
            for (const End& end : ends)
            {
                if (end.cell == c)
                {
                    cellLo += glm::max(-glm::vec2(end.building), glm::vec2(0.0f)) * kSkin;
                    cellHi -= glm::max(glm::vec2(end.building), glm::vec2(0.0f)) * kSkin;
                }
            }
            lo = glm::min(lo, cellLo);
            hi = glm::max(hi, cellHi);
        }
        box("tunnel_floor", floor, {lo.x, base - FacilityLayout::kSlab, lo.y}, {hi.x, base, hi.y});
        box("tunnel_roof", roof, {lo.x, base + kClear, lo.y}, {hi.x, base + kClear + kRoof, hi.y});
    }

    // The walls: every side of a cell of it with no more of it beyond, but the doorways at its ends -- in runs along each line.
    std::map<int, std::vector<int>> rowsX;
    std::map<int, std::vector<int>> rowsZ;
    for (const glm::ivec2 c : tunnel.cells)
    {
        for (const glm::ivec2 step : {glm::ivec2(1, 0), glm::ivec2(-1, 0), glm::ivec2(0, 1), glm::ivec2(0, -1)})
        {
            const bool door = std::any_of(std::begin(ends), std::end(ends), [&](const End& end) { return end.cell == c && end.building == step; });
            if (in(c + step) || door)
            {
                continue;
            }
            if (step.y != 0)
            {
                rowsX[step.y > 0 ? c.y + 1 : c.y].push_back(c.x);
            }
            else
            {
                rowsZ[step.x > 0 ? c.x + 1 : c.x].push_back(c.y);
            }
        }
    }
    // A run's end at a building's doorway: at its cladding, not into it.
    const auto atBuilding = [&](bool runAlongX, int line, int lattice, int outward)
    {
        for (const End& end : ends)
        {
            const int along = runAlongX ? end.building.x : end.building.y;
            const glm::ivec2 c = end.cell;
            const int sideLine0 = runAlongX ? c.y : c.x;
            const int cellAlong = runAlongX ? c.x : c.y;
            if (along == outward && (line == sideLine0 || line == sideLine0 + 1) && lattice == (outward < 0 ? cellAlong : cellAlong + 1))
            {
                return true;
            }
        }
        return false;
    };
    // The lattice points the walls along x pass through, ends and all: the walls along z stop at them.
    std::set<std::pair<int, int>> crossed;
    const float y0 = base - 0.02f;
    const float y1 = base + kClear + 0.02f;
    for (auto& [line, xs] : rowsX)
    {
        std::sort(xs.begin(), xs.end());
        size_t k = 0;
        while (k < xs.size())
        {
            size_t e = k;
            while (e + 1 < xs.size() && xs[e + 1] == xs[e] + 1)
            {
                ++e;
            }
            const int a = xs[k];
            const int b = xs[e] + 1;
            const float from = atBuilding(true, line, a, -1) ? X(a) + kSkin : X(a) - kHalf;
            const float to = atBuilding(true, line, b, 1) ? X(b) - kSkin : X(b) + kHalf;
            box("tunnel_wall", wall, {from, y0, Z(line) - kHalf}, {to, y1, Z(line) + kHalf});
            for (int p = a; p <= b; ++p)
            {
                crossed.insert({p, line});
            }
            k = e + 1;
        }
    }
    for (auto& [line, zs] : rowsZ)
    {
        std::sort(zs.begin(), zs.end());
        size_t k = 0;
        while (k < zs.size())
        {
            size_t e = k;
            while (e + 1 < zs.size() && zs[e + 1] == zs[e] + 1)
            {
                ++e;
            }
            const int a = zs[k];
            const int b = zs[e] + 1;
            const float from = atBuilding(false, line, a, -1) ? Z(a) + kSkin : crossed.count({line, a}) != 0 ? Z(a) + kHalf : Z(a) - kHalf;
            const float to = atBuilding(false, line, b, 1) ? Z(b) - kSkin : crossed.count({line, b}) != 0 ? Z(b) - kHalf : Z(b) + kHalf;
            box("tunnel_wall", wall, {X(line) - kHalf, y0, from}, {X(line) + kHalf, y1, to});
            k = e + 1;
        }
    }

    // A lamp to every third cell, lighting the cells either side of it.
    std::vector<TunnelLamp> lamps;
    for (size_t i = 1; i < tunnel.cells.size(); i += 3)
    {
        const glm::ivec2 c = tunnel.cells[i];
        TunnelLamp lamp;
        lamp.at = {X(c.x) + kCell * 0.5f, base + kClear - 0.05f, Z(c.y) + kCell * 0.5f};
        glm::vec2 lo{1.0e9f};
        glm::vec2 hi{-1.0e9f};
        for (size_t k = i > 0 ? i - 1 : 0; k <= std::min(i + 1, tunnel.cells.size() - 1); ++k)
        {
            lo = glm::min(lo, glm::vec2(X(tunnel.cells[k].x), Z(tunnel.cells[k].y)) + glm::vec2(kHalf));
            hi = glm::max(hi, glm::vec2(X(tunnel.cells[k].x + 1), Z(tunnel.cells[k].y + 1)) - glm::vec2(kHalf));
        }
        lamp.low = {lo.x, base - 0.02f, lo.y};
        lamp.high = {hi.x, base + kClear + 0.02f, hi.y};
        lamps.push_back(lamp);
    }
    // And at each end, a caged lamp on the wall over the building's door, shining down the passage.
    for (const End& end : ends)
    {
        const glm::vec2 cellMiddle = glm::vec2(X(end.cell.x), Z(end.cell.y)) + glm::vec2(kCell * 0.5f);
        const glm::vec2 face = cellMiddle + glm::vec2(end.building) * (kCell * 0.5f - kSkin - 0.15f);
        TunnelLamp lamp;
        lamp.wall = true;
        lamp.at = {face.x, base + FacilitySpec::kDoorHeight + 0.45f, face.y};
        lamp.direction = glm::normalize(glm::vec3(-static_cast<float>(end.building.x), -1.2f, -static_cast<float>(end.building.y)));
        lamp.low = {X(end.cell.x) + kHalf, base - 0.02f, Z(end.cell.y) + kHalf};
        lamp.high = {X(end.cell.x + 1) - kHalf, base + kClear + 0.02f, Z(end.cell.y + 1) - kHalf};
        lamps.push_back(lamp);
    }
    return lamps;
}
} // namespace

void SiteMap::Build(uint16_t seed, Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, LevelLights* lights)
{
    BuildFrom(SitePlan::Generate(seed, m_look.water == SiteTerrain::Water::Seabed), seed, false, scene, meshes, physics, lights);
}

void SiteMap::BuildFrom(const SitePlan& plan, uint16_t seed, bool groundOnly, Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, LevelLights* lights)
{
    Clear(scene, physics, lights);
    m_seed = seed;
    // (How long each part of it takes, in the log at debug level: a site is made in one go, and that time is a held frame.)
    auto previous = std::chrono::steady_clock::now();
    std::string timings;
    const auto mark = [&](const char* what)
    {
        const auto now = std::chrono::steady_clock::now();
        timings += fmt::format(" {} {:.0f}", what, std::chrono::duration<double, std::milli>(now - previous).count());
        LoadingPulse();
        previous = now;
    };
    m_plan = plan;
    m_groundOnly = groundOnly;
    // (Its meshes by names of its own: the library shares meshes by name, and the same name again for other geometry takes the
    // first one's place -- the ground round the ship's outpost, made while a site stood, became the site's far land and all.)
    m_tag = groundOnly ? "outpost_ground" : "site";
    // On stilts (Phase 3: outsides that sit in the land): on a frozen world -- the ground there frozen and heaving, nothing built
    // straight on it -- and on a wet one, a jungle's or an ocean world's, most buildings with nothing under the ground and no vehicle
    // bay stand raised on piles, a metre and more over the ground, stairs up to their doors (BuildStilts). Where there is no air,
    // never: its ways out through airlocks, on the ground. On a draw of their own; the floodlights over their doors raised with them.
    m_stilts.assign(m_plan.buildings.size(), 0.0f);
    // (On the sea's floor, every one of them: high enough to swim in under it and up into its moon pool -- FacilityLayout::moonPool.)
    const bool submerged = !groundOnly && m_look.water == SiteTerrain::Water::Seabed;
    const bool stilted = !groundOnly && !m_look.airless && m_look.biome != "toxic" &&
                         (m_look.biome == "frozen" || m_look.biome == "jungle" || m_look.biome == "oceanic" || submerged);
    for (size_t b = 0; stilted && b < m_plan.buildings.size(); ++b)
    {
        FacilityLayout& building = m_plan.buildings[b];
        const bool bay = std::any_of(building.exits.begin(), building.exits.end(), [](const FacilityLayout::Exit& exit) { return exit.bay; });
        const uint32_t roll = Mix(seed, 0x57117u + static_cast<uint32_t>(b));
        if (!submerged && (building.ground > 0 || bay || building.ruin.Has() || roll % 100u >= 70u))
        {
            continue;
        }
        const float lift = submerged ? kSeabedLift : 1.2f + static_cast<float>((roll >> 8) % 7u) * 0.1f;
        glm::vec2 min;
        glm::vec2 max;
        SitePlan::Footprint(building, 3.0f, min, max);
        for (SitePlan::Lamp& lamp : m_plan.lamps)
        {
            if (lamp.kind == SitePlan::LampKind::Flood && lamp.position.x > min.x && lamp.position.x < max.x && lamp.position.z > min.y && lamp.position.z < max.y)
            {
                lamp.position.y += lift;
            }
        }
        building.origin.y += lift;
        m_stilts[b] = lift;
    }
    // Set into a hillside (Phase 3: outsides that sit in the land): where the world's land is hilly, now and then a building dug into
    // the slope -- the land rising behind it, earth banked against that wall and partway along the walls either side of it, no
    // window under it (SetIntoHillside; SiteTerrain raises the land and draws the bank). Two at most to a site; none on stilts.
    {
        using Shape = SiteTerrain::Shape;
        const Shape shape = m_look.terrain;
        const bool hilly = shape == Shape::Rolling || shape == Shape::Mountainous || shape == Shape::Canyons || shape == Shape::Glacial ||
                           shape == Shape::Karst || shape == Shape::Highlands || shape == Shape::Mesas || shape == Shape::Pillars ||
                           shape == Shape::Cratered;
        int banked = 0;
        for (size_t b = 0; hilly && !groundOnly && b < m_plan.buildings.size() && banked < 2; ++b)
        {
            const uint32_t roll = Mix(seed, 0xBA4Cu + static_cast<uint32_t>(b));
            if (m_stilts[b] <= 0.0f && roll % 100u < 45u && SetIntoHillside(m_plan, b, roll))
            {
                const FacilityLayout& building = m_plan.buildings[b];
                PRED_LOG_INFO(Gameplay, "Site: building {} set into a hillside on its side {} ({:.1f} m high), its corner at {:.0f} {:.0f}, {}x{} cells", b,
                              building.bank.side, building.bank.height, building.origin.x, building.origin.z, building.width, building.depth);
                ++banked;
            }
        }
    }
    // Its world's own sets begun reading at once, side by side, while its ground is worked out (Surfaces::Prefetch).
    for (const std::string& set : {m_look.surface, m_look.steep, m_look.blend, std::string("rock_cliff"), std::string("ice_cliff"), std::string("basalt"),
                                   std::string("soil_cliff")})
    {
        if (!set.empty())
        {
            Surfaces::Prefetch(set);
        }
    }
    // A stream across it, where the world's water runs over the ground: where things grow, frozen over on a frozen world, and
    // wherever there are lakes -- at a site, not round an outpost (what wades, splashes and swims there is the site's).
    const bool wet = m_look.liquid == Look::Liquid::Water || m_look.liquid == Look::Liquid::Ice;
    const bool stream = !groundOnly && wet && (m_look.water == SiteTerrain::Water::Lakes || m_look.biome == "temperate" || m_look.biome == "jungle" ||
                                                    m_look.biome == "oceanic" || m_look.biome == "frozen");
    m_terrain.Plan(m_plan, m_look.terrain, m_look.dunes, m_look.water, stream && !submerged);
    // The field generator and the relay mast out in their valleys (SitePlan's outlying blocks), stood where the land lets them: out of
    // the water, the lava or the acid lying in the bowl's low ground, on ground level enough to stand them on -- where they were put, if
    // it is; else the nearest place round about that is, inside how far anybody may go. (Stood wherever the bowl's middle was, they
    // stood in a lava pool, a lake, or half sunk into its side.)
    for (SitePlan::Block& block : m_plan.blocks)
    {
        if (!block.outlying || groundOnly)
        {
            continue;
        }
        const glm::vec2 across{std::cos(block.yaw), -std::sin(block.yaw)};
        const glm::vec2 along{std::sin(block.yaw), std::cos(block.yaw)};
        // How bad a place is to stand it: in anything lying on the ground, far worse than any slope; else how much the ground under it
        // and a metre round it rises from one side to the other.
        const auto badness = [&](glm::vec2 at)
        {
            float low = 1.0e9f;
            float high = -1.0e9f;
            bool wet = false;
            for (const float a : {-1.0f, 0.0f, 1.0f})
            {
                for (const float b : {-1.0f, 0.0f, 1.0f})
                {
                    const glm::vec2 p = at + across * (a * (block.size.x * 0.5f + 1.0f)) + along * (b * (block.size.z * 0.5f + 1.0f));
                    const float h = m_terrain.Height(p.x, p.y);
                    low = std::min(low, h);
                    high = std::max(high, h);
                    wet = wet || (m_terrain.HasWater() && m_terrain.WaterDepth(p.x, p.y) > -0.4f) || m_terrain.BedAt(p.x, p.y) > 0.2f;
                }
            }
            return wet ? 1.0e6f : high - low;
        };
        const glm::vec2 was{block.centre.x, block.centre.z};
        float best = badness(was);
        glm::vec2 chosen = was;
        if (best > 0.8f)
        {
            for (float r = 4.0f; r <= 48.0f; r += 4.0f)
            {
                for (int k = 0; k < 16; ++k)
                {
                    const float angle = static_cast<float>(k) * glm::two_pi<float>() / 16.0f;
                    const glm::vec2 at = was + glm::vec2(std::cos(angle), std::sin(angle)) * r;
                    if (m_terrain.PastRoam(at.x, at.y) > -6.0f)
                    {
                        continue;
                    }
                    const float score = badness(at) + r * 0.004f;
                    if (score < best)
                    {
                        best = score;
                        chosen = at;
                    }
                }
            }
        }
        if (chosen != was)
        {
            PRED_LOG_INFO(Gameplay, "Site: {} moved from {:.0f} {:.0f} to {:.0f} {:.0f}, out of what lay there or off its slope", NameOf(block.kind), was.x, was.y,
                          chosen.x, chosen.y);
            block.centre.x = chosen.x;
            block.centre.z = chosen.y;
        }
    }
    // On the sea's floor, the pad stands in the sea on its legs, its deck well over the waves -- the shuttle set down there, its ramp onto the deck (BuildFrom's Pad).
    if (submerged)
    {
        const float deck = m_terrain.WaterLevel() + kPontoonDeck;
        m_plan.landing.y = deck + 0.5f;
        m_plan.rampFoot.position.y = deck;
    }
    for (const SiteTerrain::Stream& each : m_terrain.Streams())
    {
        const size_t mid = each.line.size() / 2;
        // (And where it falls furthest in one length: a fall over the edge's cliff, coming in.)
        size_t fall = 0;
        for (size_t k = 1; k < each.line.size(); ++k)
        {
            fall = each.surface[k - 1] - each.surface[k] > each.surface[fall > 0 ? fall - 1 : 0] - each.surface[fall] ? k : fall;
        }
        PRED_LOG_INFO(Gameplay, "Site {}: from {:.0f} {:.0f} to {:.0f} {:.0f}{}, {} lengths; its middle at {:.0f} {:.1f} {:.0f}; falling {:.1f} m at {:.0f} {:.1f} {:.0f}",
                      each.water > 1.0f ? "river" : "stream",
                      each.line.front().x, each.line.front().y, each.line.back().x, each.line.back().y, each.downstream.empty() ? " (into the water there)" : "",
                      each.line.size(), each.line[mid].x, each.surface[mid],
                      each.line[mid].y, fall > 0 ? each.surface[fall - 1] - each.surface[fall] : 0.0f, each.line[fall].x, each.surface[fall], each.line[fall].y);
    }
    if (lights != nullptr)
    {
        m_firstLight = lights->Count();
        m_hasLights = true;
    }

    mark("plan");
    // The ground, the rock that closes it in and the buildings set into it are one structure, built to reach into
    // one another: the rock is a ragged wall of blocks run together and sunk into the ground, and so is every
    // building's outer wall. The level check has nothing to say of that (PhysicsWorld::SetOverlapGroup).
    const uint32_t ground = physics.NewOverlapGroup();
    const Material rockMaterial = Surfaces::Apply(Material::Diffuse(m_look.rock, 0.95f), "rock_cliff", 4.0f, 0.35f, true);
    // What people left: wreckage, pipework, tanks, containers, lamp poles, each with what it is made of.
    const Material hullDebris = Surfaces::Apply(kHullDebrisPlain, "hull_weathered", 2.0f, 0.3f);
    const Material hullDebrisLight = Surfaces::Apply(kHullDebrisLightPlain, "cut_metal", 2.0f, 0.3f);
    const Material padMaterial = Surfaces::Apply(kPadPlain, "concrete_slab", 6.0f, 0.3f);
    const Material pipeMaterial = Surfaces::Apply(kPipePlain, "pipe_steel", 1.0f, 0.3f);
    const Material supportMaterial = Surfaces::Apply(kSupportPlain, "pipe_steel", 1.0f, 0.3f);
    // Painted steel, the paint worn through here and there (a tank's, and the panels on a mast).
    const Material tankMaterial = Surfaces::ApplyWorn(kTankPlain, "painted_metal", 2.0f, 0.35f);
    const Material poleMaterial = Surfaces::Apply(kPolePlain, "pipe_steel", 1.0f, 0.3f);
    // The field generator's housing, painted a dull green and worn, and the dark grilles let into it.
    const Material gensetMaterial = Surfaces::ApplyWorn(Material::Diffuse({0.22f, 0.28f, 0.22f}, 0.55f), "painted_metal", 1.5f, 0.35f);
    const Material grilleMaterial = Material::Diffuse({0.05f, 0.05f, 0.055f}, 0.6f);
    // What stands round the buildings by what the place is (SiteStructures): a crane's yellow, a flame.
    const Material hazardMaterial = Surfaces::ApplyWorn(Material::Diffuse({0.78f, 0.6f, 0.12f}, 0.5f), "painted_metal", 2.0f, 0.35f);
    const Material flameMaterial = [] {
        Material flame = Material::Diffuse({1.0f, 0.55f, 0.15f}, 1.0f);
        flame.emissive = {6.0f, 2.6f, 0.6f};
        return flame;
    }();
    const SiteStructures::Materials structureMaterials{padMaterial, supportMaterial, tankMaterial, hazardMaterial, pipeMaterial, rockMaterial,
                                                       grilleMaterial, flameMaterial};
    // Where something stands on the ground: how far its foot is moved from the plan's flat ground to the terrain's, at the
    // lowest of its middle, its corners and halfway out to them every way round, so nothing on a slope or over a dip stands
    // on air.
    const auto lift = [&](glm::vec2 at, float reach)
    {
        float low = m_terrain.Height(at.x, at.y);
        for (int k = 0; k < 16; ++k)
        {
            const float a = glm::two_pi<float>() * static_cast<float>(k) / 8.0f;
            const float out = reach * (k % 2 == 1 ? 1.4142f : 1.0f) * (k < 8 ? 1.0f : 0.5f); // (its edges and corners, then halfway)
            low = std::min(low, m_terrain.Height(at.x + std::cos(a) * out, at.y + std::sin(a) * out));
        }
        return low - m_plan.origin.y;
    };

    mark("props");
    // The buildings, each with its own meshes, and everything in them for WorldObjects.
    PRED_LOG_INFO(Gameplay, "Site type: {}, {} buildings, the pad at {:.0f} {:.0f}", SitePlan::TypeName(m_plan.type), m_plan.buildings.size(), m_plan.landing.x,
                  m_plan.landing.z);
    // How high the water stands in each building (FloodTops), settled for them all before any is built: a passage under the ground
    // lets it through from one to another.
    const bool coldSite = m_look.biome == "frozen" || m_look.weather == "snow" || m_look.liquid == Look::Liquid::Ice || m_look.airless;
    std::vector<float> floodTops = FloodTops(seed, coldSite);
    // A cave joined to a building is one with it, water and all: where the building's water stands over the floor of the adit out of
    // it, it has run on into the cave and stands there at the same height -- not stopped at the adit's mouth as if walled off. And a
    // cave flooded over that floor has run back into the building. (One height for both, the higher; never up onto its ground floor.)
    for (SitePlan::Cave& cave : m_plan.caves)
    {
        std::vector<std::pair<int, float>> ways{{cave.building, cave.mouth.y}};
        for (const SitePlan::Cave::Adit& join : cave.joins)
        {
            ways.emplace_back(join.building, join.mouth.y);
        }
        for (const auto& [b, sill] : ways)
        {
            if (b < 0 || static_cast<size_t>(b) >= floodTops.size())
            {
                continue;
            }
            const float top = floodTops[static_cast<size_t>(b)];
            if (top > sill + 0.02f && top > cave.water)
            {
                cave.water = top;
            }
        }
        for (const auto& [b, sill] : ways)
        {
            if (b >= 0 && static_cast<size_t>(b) < floodTops.size() && cave.water > sill + 0.02f)
            {
                const FacilityLayout& building = m_plan.buildings[static_cast<size_t>(b)];
                floodTops[static_cast<size_t>(b)] = std::max(floodTops[static_cast<size_t>(b)], std::min(cave.water, building.GroundLevel() - 0.02f));
            }
        }
    }
    // (A bunker's earth over its roof drawn as the ground round it is: FacilityMap::SetEarth.)
    // (Tinted as the ground is drawn about the middle of the site, as near the colour of the land round it as one colour goes.)
    const glm::vec2 siteMiddle = glm::vec2(m_plan.origin.x, m_plan.origin.z) + glm::vec2(m_plan.size * 0.5f);
    const glm::vec3 grown = Growing(m_look) * 0.78f;
    const glm::vec3 tint = m_terrain.DrawnColour(siteMiddle.x, siteMiddle.y, m_look.ground, m_look.rock * 0.88f, Grassy(m_look) ? &grown : nullptr);
    const Material earth = Surfaces::Apply(Material::Diffuse(tint, 1.0f), m_look.surface, 2.5f, 0.6f, true);
    for (size_t b = 0; b < m_plan.buildings.size(); ++b)
    {
        auto building = std::make_unique<FacilityMap>();
        building->SetEarth(earth);
        // Its windows as the world it stands on would have them: where it is cold -- a frozen world, snow falling, its water
        // ice -- or there is no air to breathe, fewer and smaller, set in the wall one by one (no strips of glass, no tall
        // windows letting the heat out or standing against a vacuum); on a desert, smaller, out of the glare.
        FacilityLayout made = m_plan.buildings[b];
        const bool cold = m_look.biome == "frozen" || m_look.weather == "snow" || m_look.liquid == Look::Liquid::Ice || m_look.airless;
        if (cold || m_look.biome == "desert")
        {
            FacilityLayout::Outside& look = made.outside;
            if (look.windows != FacilityLayout::Outside::Windows::None)
            {
                look.windows = FacilityLayout::Outside::Windows::Punched;
                look.windowEvery = cold ? std::max(look.windowEvery, 2) : look.windowEvery;
                look.windowSize = cold ? 0.7f : 0.8f;
            }
        }
        // And what the world has done to it, where nobody looks after it (FacilityLayout::Condition): frozen where it is cold and
        // there is air to freeze; overgrown where things grow.
        const bool neglected = made.condition != FacilityLayout::Condition::Kept && made.condition != FacilityLayout::Condition::Hurried;
        made.frozen = neglected && cold && !m_look.airless;
        m_plan.buildings[b].frozen = made.frozen;
        // Snowed under, left to the cold on a frozen world (Phase 3: snow-buried outsides): the snow drifted up its walls to its windows
        // and over, deepest on the side the wind blows away from, where it drops what it carries (the drifts, below). Not one on stilts,
        // the snow blowing through under it.
        if (made.frozen && m_look.biome == "frozen" && m_stilts[b] <= 0.0f)
        {
            const float windAngle = WindAngle(seed);
            const glm::vec2 wind{std::cos(windAngle), std::sin(windAngle)};
            const glm::vec2 outs[4] = {{-1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, -1.0f}, {0.0f, 1.0f}};
            for (size_t side = 0; side < 4; ++side)
            {
                made.outside.drifted[side] = 3.0f * glm::mix(0.5f, 1.0f, glm::smoothstep(-0.3f, 0.8f, glm::dot(outs[side], wind)));
            }
            m_plan.buildings[b].outside.drifted = made.outside.drifted;
            PRED_LOG_INFO(Gameplay, "Site: building {} snowed under, its corner at {:.0f} {:.0f}, {}x{} cells", b, made.origin.x, made.origin.z, made.width,
                          made.depth);
        }
        // Its ways out through airlocks, where there is no air outside to breathe, or none fit to (its own kept full of it).
        // (And on the sea's floor, where a way out is a lockout into the sea.)
        made.outside.airlocks = m_look.airless || m_look.biome == "toxic" || m_terrain.Submerged();
        made.outside.canopies = made.outside.canopies && !made.outside.airlocks;
        m_plan.buildings[b].outside.airlocks = made.outside.airlocks; // (kept with the plan: what is outside its doors goes by it)
        m_plan.buildings[b].outside.canopies = made.outside.canopies;
        // (Not sealed on the sea's floor: nothing grows in the dark in there.)
        made.overgrown = neglected && !cold && !m_terrain.Submerged() && (m_look.biome == "temperate" || m_look.biome == "jungle" || m_look.biome == "oceanic");
        // And flooded, where the water stands in it (FloodTops): the storey its surface is in and how deep over that storey's floor,
        // every storey under it full. The lamps under the water out.
        if (b < floodTops.size() && floodTops[b] > made.origin.y + 0.02f)
        {
            // (Under the ground floor, in its basements -- or on the sea's floor, over its lowest floor, which has no ground floor over it.)
            made.flooded = std::clamp(static_cast<int>((floodTops[b] - made.origin.y) / FacilityLayout::kStorey), 0, std::max(made.ground - 1, 0));
            made.floodDepth = std::min(floodTops[b] - (made.origin.y + static_cast<float>(made.flooded) * FacilityLayout::kStorey),
                                       FacilityLayout::kClearHeight - 0.08f);
            m_plan.buildings[b].flooded = made.flooded; // (kept with the plan too, for what asks where the water is)
            m_plan.buildings[b].floodDepth = made.floodDepth;
            for (FacilityLayout::Lamp& lamp : made.lamps)
            {
                // (A lamp at the ceiling: under it too where the water is up to the ceiling.)
                if (lamp.floor < made.flooded || (lamp.floor == made.flooded && made.floodDepth > FacilityLayout::kClearHeight - 0.3f))
                {
                    lamp.mood = FacilityLayout::LampMood::Dead;
                }
            }
        }
        m_floods.emplace_back(made.flooded, made.floodDepth);
        building->Build(made, Mix(seed, static_cast<uint32_t>(b) + 1u), scene, meshes, physics, lights,
                        "site" + std::to_string(b) + "_", ground);
        const size_t doorsBefore = m_placements.doors.size();
        Append(m_placements, building->Placements());
        // Its airlocks; on the sea's floor, lockouts: the sea let into them and out of them as they cycle (WaterAt).
        for (const FacilityMap::Blueprint::Vestibule& room : building->Vestibules())
        {
            Lockout lockout;
            lockout.min = room.min;
            lockout.max = room.max;
            lockout.inner = doorsBefore + room.inner;
            lockout.outer = doorsBefore + room.outer;
            lockout.inward = room.inward;
            m_airlocks.push_back(lockout);
            if (m_terrain.Submerged())
            {
                m_lockouts.push_back(lockout);
            }
        }
        for (const FacilityLayout::Exit& exit : m_plan.buildings[b].exits)
        {
            const glm::vec3 at = FacilityMap::ExitOutside(m_plan.buildings[b], exit, 2.0f);
            PRED_LOG_INFO(Gameplay, "Site building {} ({:.0f} {:.0f} to {:.0f} {:.0f}, {} floors): a way in at {:.1f} {:.1f} {:.1f}, facing {}", b,
                          m_plan.buildings[b].origin.x, m_plan.buildings[b].origin.z, m_plan.buildings[b].origin.x + m_plan.buildings[b].width * FacilityLayout::kCell,
                          m_plan.buildings[b].origin.z + m_plan.buildings[b].depth * FacilityLayout::kCell, m_plan.buildings[b].floors, at.x, at.y, at.z,
                          exit.side == 0 ? "west" : exit.side == 1 ? "east" : exit.side == 2 ? "north" : "south");
        }
        m_ladders.insert(m_ladders.end(), building->Ladders().begin(), building->Ladders().end());
        for (FacilityMap::Lift shaft : building->Lifts())
        {
            shaft.building = static_cast<int>(b);
            // The light in its car: warm, thrown down and round its walls, on its own battery (on whatever the building's power),
            // kept to the car by its box -- moved with it by whoever runs it (Lifts).
            if (lights != nullptr)
            {
                const glm::vec3 car = shaft.centre + glm::vec3(0.0f, FacilitySpec::kLiftCarHeight - 0.05f, 0.0f);
                shaft.lamp = lights->AddLamp(car, {0.0f, -1.0f, 0.0f}, {1.0f, 0.95f, 0.86f}, 2.5f, 4.0f, 70.0f, 160.0f, 0);
                const float inside = FacilityLayout::kCell * 0.5f - FacilitySpec::kHalfWall; // the shaft, inside its walls
                const glm::vec3 reach{inside, 0.0f, inside};
                lights->Bound(shaft.lamp, shaft.centre - reach - glm::vec3(0.0f, 0.05f, 0.0f),
                              shaft.centre + reach + glm::vec3(0.0f, FacilitySpec::kLiftCarHeight + 0.1f, 0.0f));
            }
            m_lifts.push_back(shaft);
        }
        m_buildings.push_back(std::move(building));
    }

    mark("buildings");
    // Outside.
    MapBuilder builder(scene, meshes, &physics, m_tag + "_");
    m_meshLibrary = &meshes;
    builder.Track(&m_entities, &m_bodies, &m_meshHandles);
    // The ground, in pieces each drawn on its own (so what is out of sight is not drawn): the world's ground where it is
    // gentle, its rock showing through as it steepens. Part of the one structure the buildings are set into.
    builder.SetStructure(ground);
    // The passages joining buildings under the ground, if there are any: walled, floored and roofed as their basements are,
    // and lit, on the first one's circuits.
    if (!m_plan.tunnels.empty())
    {
        const Material wall = Surfaces::Apply(Material::Diffuse({0.42f, 0.44f, 0.42f}, 0.9f), "facility_wall", 3.0f, 0.3f);
        const Material floor = Surfaces::Apply(Material::Diffuse({0.22f, 0.23f, 0.24f}, 0.9f), "facility_floor", 3.0f, 0.3f);
        const Material roof = Surfaces::Apply(Material::Diffuse({0.30f, 0.30f, 0.31f}, 0.95f), "facility_ceiling", 3.0f, 0.3f);
        for (size_t t = 0; t < m_plan.tunnels.size(); ++t)
        {
            const SitePlan::Tunnel& tunnel = m_plan.tunnels[t];
            const std::vector<TunnelLamp> lamps = BuildTunnel(m_plan, tunnel, builder, wall, floor, roof);
            // The water standing in it, where the buildings at its ends stand in water over its floor (FloodTops).
            const float high = std::max(tunnel.from >= 0 && static_cast<size_t>(tunnel.from) < floodTops.size() ? floodTops[static_cast<size_t>(tunnel.from)] : -1.0e9f,
                                        tunnel.to >= 0 && static_cast<size_t>(tunnel.to) < floodTops.size() ? floodTops[static_cast<size_t>(tunnel.to)] : -1.0e9f);
            m_tunnelFloods.push_back(high > tunnel.floor + 0.02f ? high : -1.0e9f);
            PRED_LOG_INFO(Gameplay, "Site tunnel: building {} to {}, {} cells, from {} {} to {} {}", tunnel.from, tunnel.to, tunnel.cells.size(), tunnel.cells.front().x,
                          tunnel.cells.front().y, tunnel.cells.back().x, tunnel.cells.back().y);
            if (lights == nullptr)
            {
                continue;
            }
            const int circuit = 100 * (tunnel.from + 1);
            for (size_t i = 0; i < lamps.size(); ++i)
            {
                const uint32_t roll = Mix(seed, 0x7A44u + static_cast<uint32_t>(t * 64 + i));
                // (Out, under the water standing in it.)
                const bool drowned = m_tunnelFloods[t] > lamps[i].at.y - 0.1f;
                const LightMood mood = drowned ? LightMood::Dead : roll % 7u == 0u ? LightMood::Flicker : LightMood::Steady;
                const int lamp = lamps[i].wall ? lights->Add(scene, meshes, LightKind::Wall, mood, lamps[i].at, lamps[i].direction, circuit, roll | 1u, 6.0f)
                                               : lights->Add(scene, meshes, LightKind::Ceiling, mood, lamps[i].at, glm::vec3(0.0f, -1.0f, 0.0f), circuit, roll | 1u, 5.0f);
                lights->Bound(lamp, lamps[i].low, lamps[i].high);
            }
        }
    }
    // The piles under any building raised on stilts, and the stairs up to its doors (BuildStilts).
    {
        const Material grate = Surfaces::Apply(Material::Diffuse({0.38f, 0.39f, 0.4f}, 0.6f), "metal_grate", 1.0f, 0.4f);
        for (size_t b = 0; b < m_plan.buildings.size() && b < m_stilts.size(); ++b)
        {
            if (m_stilts[b] > 0.0f)
            {
                BuildStilts(m_plan.buildings[b], m_stilts[b], builder, supportMaterial, grate, !m_terrain.Submerged());
            }
        }
    }
    // The caves broken into from a mine's workings (SitePlan::Cave): their adits and the caves themselves.
    if (!m_plan.caves.empty())
    {
        const Material timber = Surfaces::Apply(Material::Diffuse({0.42f, 0.32f, 0.22f}, 0.85f), "tabletop", 1.2f, 0.45f);
        for (const SitePlan::Cave& cave : m_plan.caves)
        {
            BuildCave(cave, m_terrain, builder, rockMaterial, timber);
            CaveCaches(cave, m_placements);
        }
    }
    const glm::vec3 cliffColour = m_look.rock * 0.88f;
    Material terrainMaterial = Surfaces::Apply(Material::Diffuse(SiteTerrain::Paint(m_look.ground, cliffColour), 1.0f), m_look.surface, 2.5f, 0.6f, true);
    terrainMaterial = Surfaces::ApplySteep(terrainMaterial, m_look.steep, 11.0f);
    if (!m_look.blend.empty())
    {
        terrainMaterial = Surfaces::ApplyBlend(terrainMaterial, m_look.blend, 3.0f, m_look.blendColour, m_look.blendShare,
                                               m_terrain.HasWater() ? m_look.blendShore : 0.0f);
    }
    m_groundMaterial = terrainMaterial;
    m_cliffColour = cliffColour;
    // Made side by side, a row of pieces a thread -- the shapes to stand on are most of the time a site takes to build -- and
    // put into the world here, in order.
    struct Piece
    {
        MeshData mesh;
        PhysicsWorld::PreparedMesh shape;
    };
    const int chunks = m_terrain.Chunks();
    // Where the world grows grass, its ground grown over far off as well as the tufts drawn near (UpdateCover): their colour.
    const bool grassy = Grassy(m_look);
    const glm::vec3 growing = Growing(m_look);
    const glm::vec3 grassColour = glm::mix(growing, m_look.ground, 0.3f) * 1.05f;
    // (The ground under it a deeper green: seen far off, the grass is mostly its own shade.)
    const glm::vec3 grownOver = growing * 0.78f;
    m_grownOver = grownOver;
    m_grassy = grassy;
    std::vector<Piece> pieces(static_cast<size_t>(chunks * chunks));
    {
        std::vector<std::future<void>> rows;
        for (int cz = 0; cz < chunks; ++cz)
        {
            rows.push_back(std::async(std::launch::async, [&, cz]
                                      {
                                          for (int cx = 0; cx < chunks; ++cx)
                                          {
                                              Piece& piece = pieces[static_cast<size_t>(cz * chunks + cx)];
                                              piece.mesh = m_terrain.Mesh(cx, cz, m_look.ground, cliffColour, grassy ? &grownOver : nullptr);
                                              if (!piece.mesh.indices.empty() && m_terrain.InReach(cx, cz))
                                              {
                                                  piece.shape = PhysicsWorld::PrepareMesh(piece.mesh);
                                              }
                                          }
                                      }));
        }
        for (std::future<void>& row : rows)
        {
            row.get();
        }
    }
    for (const Piece& piece : pieces)
    {
        if (!piece.mesh.indices.empty())
        {
            builder.AddMesh("site_ground", Transform{}, piece.mesh, terrainMaterial, piece.shape);
        }
    }
    // The earth banked against any building set into a hillside, drawn as the ground is (SiteTerrain::Bank).
    for (const FacilityLayout& building : m_plan.buildings)
    {
        const MeshData bank = m_terrain.Bank(building, m_look.ground, cliffColour, grassy ? &grownOver : nullptr);
        if (!bank.indices.empty())
        {
            builder.AddMesh("site_bank", Transform{}, bank, terrainMaterial, true);
        }
    }
    mark("ground");
    // And the land round it to the horizon (SiteTerrain::FarMesh): drawn however far off, nothing to stand on, no shadow worth
    // its cost at that distance.
    if (!groundOnly)
    {
        const MeshData far = m_terrain.FarMesh(m_look.ground, cliffColour, grassy ? &grownOver : nullptr);
        if (!far.indices.empty())
        {
            const MeshHandle mesh = meshes.Upload(far, m_tag + "_far_land");
            m_meshHandles.push_back(mesh);
            const Entity entity = scene.CreateMeshEntity("site_far_land", Transform{}, mesh, terrainMaterial);
            if (MeshRenderer* renderer = scene.GetMeshRenderer(entity))
            {
                renderer->castsShadow = false;
                renderer->blocksSky = false;
                renderer->farVisible = true;
            }
            m_entities.push_back(entity);
            m_farLand = entity;
        }
    }
    mark("far land");
    // The water standing in its flooded buildings (FacilityLayout::flooded): a still sheet at its level over every cell of the
    // flooded storey that is not solid -- rooms, corridors, stairwells, crawlspaces -- how deep it is there laid on it as the
    // site's water's is, for its colour.
    m_indoorWater = MeshHandle{};
    {
        MeshData indoor;
        // And the sea standing in each moon pool of a building on the sea's floor (FacilityLayout::moonPool), a little under its floor:
        // the sea's own, under a roof.
        m_pools.clear();
        for (size_t b = 0; m_terrain.Submerged() && b < m_plan.buildings.size(); ++b)
        {
            const FacilityLayout& building = m_plan.buildings[b];
            if (!building.HasMoonPool())
            {
                continue;
            }
            const glm::vec3 middle = FacilityMap::ToWorld(building, 0, glm::vec2(building.moonPool) + glm::vec2(0.5f));
            Pool pool;
            pool.min = glm::vec2(middle.x, middle.z) - glm::vec2(FacilityMap::kPoolHalf);
            pool.max = glm::vec2(middle.x, middle.z) + glm::vec2(FacilityMap::kPoolHalf);
            pool.surface = building.origin.y - FacilityMap::kPoolWater;
            pool.building = static_cast<int>(b);
            // (Where the building has lost its air and the sea has come up through its pool over its lowest floor -- FloodTops -- the
            // water in the pool is the flood's, and drawn with it.)
            const bool flooded = b < m_floods.size() && m_floods[b].first == 0;
            if (flooded)
            {
                pool.surface = building.origin.y + m_floods[b].second;
            }
            m_pools.push_back(pool);
            if (flooded)
            {
                continue;
            }
            const auto first = static_cast<uint32_t>(indoor.vertices.size());
            const float deep = pool.surface - m_terrain.Height(middle.x, middle.z);
            for (const glm::vec2 corner : {pool.min, glm::vec2(pool.max.x, pool.min.y), glm::vec2(pool.min.x, pool.max.y), pool.max})
            {
                MeshVertex vertex;
                vertex.position = {corner.x, pool.surface, corner.y};
                vertex.normal = {0.0f, 1.0f, 0.0f};
                vertex.uv = {deep, 0.0f};
                indoor.vertices.push_back(vertex);
            }
            indoor.indices.insert(indoor.indices.end(), {first, first + 2, first + 1, first + 1, first + 2, first + 3});
            glm::vec2 lo;
            glm::vec2 hi;
            SitePlan::Footprint(building, 0.0f, lo, hi);
            PRED_LOG_INFO(Gameplay, "Site building {} (from {:.0f} {:.0f} to {:.0f} {:.0f}): its moon pool at {:.1f} {:.1f} {:.1f}, {:.1f} m over the floor of the sea, the sea {:.1f} m over that", b, lo.x, lo.y, hi.x, hi.y, middle.x, pool.surface, middle.z, deep, m_terrain.WaterLevel() - pool.surface);
        }
        for (size_t b = 0; b < m_plan.buildings.size() && b < m_floods.size(); ++b)
        {
            const auto [storey, depth] = m_floods[b];
            if (storey < 0)
            {
                continue;
            }
            const FacilityLayout& building = m_plan.buildings[b];
            const float level = FacilityMap::ToWorld(building, storey, glm::vec2(0.0f)).y + depth;
            const float deep = std::min(level - FacilityMap::ToWorld(building, 0, glm::vec2(0.0f)).y, 80.0f);
            for (int z = 0; z < building.depth; ++z)
            {
                for (int x = 0; x < building.width; ++x)
                {
                    const FacilityLayout::Cell cell = building.At(storey, x, z);
                    if (cell == FacilityLayout::Cell::Solid || cell == FacilityLayout::Cell::Outside)
                    {
                        continue;
                    }
                    const glm::vec3 lo = FacilityMap::ToWorld(building, storey, glm::vec2(static_cast<float>(x), static_cast<float>(z)));
                    const glm::vec3 hi = FacilityMap::ToWorld(building, storey, glm::vec2(static_cast<float>(x + 1), static_cast<float>(z + 1)));
                    const auto first = static_cast<uint32_t>(indoor.vertices.size());
                    for (const glm::vec2 corner : {glm::vec2(lo.x, lo.z), glm::vec2(hi.x, lo.z), glm::vec2(lo.x, hi.z), glm::vec2(hi.x, hi.z)})
                    {
                        MeshVertex vertex;
                        vertex.position = {corner.x, level, corner.y};
                        vertex.normal = {0.0f, 1.0f, 0.0f};
                        vertex.uv = {deep, 0.0f};
                        indoor.vertices.push_back(vertex);
                    }
                    // (Wound as the site's water is: SiteTerrain::WaterMesh.)
                    indoor.indices.insert(indoor.indices.end(), {first, first + 2, first + 1, first + 1, first + 2, first + 3});
                }
            }
            PRED_LOG_INFO(Gameplay, "Site building {} flooded: storey {} under {:.2f} m, its surface at {:.2f}", b, storey, depth, level);
        }
        // And in the passages it has run through (FloodTops): over every cell of each, at the one height, up to its roof at most.
        for (size_t t = 0; t < m_plan.tunnels.size() && t < m_tunnelFloods.size(); ++t)
        {
            const SitePlan::Tunnel& tunnel = m_plan.tunnels[t];
            if (m_tunnelFloods[t] <= tunnel.floor)
            {
                continue;
            }
            const float level = std::min(m_tunnelFloods[t], tunnel.floor + kTunnelClear - 0.05f);
            for (const glm::ivec2 c : tunnel.cells)
            {
                const glm::vec2 lo = glm::vec2(m_plan.origin.x, m_plan.origin.z) + glm::vec2(c) * FacilityLayout::kCell;
                const glm::vec2 hi = lo + glm::vec2(FacilityLayout::kCell);
                const auto first = static_cast<uint32_t>(indoor.vertices.size());
                for (const glm::vec2 corner : {glm::vec2(lo.x, lo.y), glm::vec2(hi.x, lo.y), glm::vec2(lo.x, hi.y), glm::vec2(hi.x, hi.y)})
                {
                    MeshVertex vertex;
                    vertex.position = {corner.x, level, corner.y};
                    vertex.normal = {0.0f, 1.0f, 0.0f};
                    vertex.uv = {level - tunnel.floor, 0.0f};
                    indoor.vertices.push_back(vertex);
                }
                indoor.indices.insert(indoor.indices.end(), {first, first + 2, first + 1, first + 1, first + 2, first + 3});
            }
            PRED_LOG_INFO(Gameplay, "Site tunnel {} flooded: its surface at {:.2f}, {:.2f} over its floor", t, level, level - tunnel.floor);
        }
        // And in its flooded caves (SitePlan::Cave::water): over every half-metre square where the cave is open at the water's
        // level, any corner of it (what reaches on into the rock is hidden behind the rock's face).
        for (size_t c = 0; c < m_plan.caves.size(); ++c)
        {
            const SitePlan::Cave& cave = m_plan.caves[c];
            if (cave.water < cave.bottom)
            {
                continue;
            }
            constexpr float kStep = 0.5f;
            const auto open = [&](float x, float z) { return cave.Distance({x, cave.water - 0.02f, z}) < 0.0f; };
            size_t squares = 0;
            for (float z = cave.min.z; z < cave.max.z; z += kStep)
            {
                for (float x = cave.min.x; x < cave.max.x; x += kStep)
                {
                    if (!open(x, z) && !open(x + kStep, z) && !open(x, z + kStep) && !open(x + kStep, z + kStep))
                    {
                        continue;
                    }
                    const auto first = static_cast<uint32_t>(indoor.vertices.size());
                    for (const glm::vec2 corner : {glm::vec2(x, z), glm::vec2(x + kStep, z), glm::vec2(x, z + kStep), glm::vec2(x + kStep, z + kStep)})
                    {
                        MeshVertex vertex;
                        vertex.position = {corner.x, cave.water, corner.y};
                        vertex.normal = {0.0f, 1.0f, 0.0f};
                        vertex.uv = {cave.water - cave.bottom, 0.0f};
                        indoor.vertices.push_back(vertex);
                    }
                    indoor.indices.insert(indoor.indices.end(), {first, first + 2, first + 1, first + 1, first + 2, first + 3});
                    ++squares;
                }
            }
            PRED_LOG_INFO(Gameplay, "Site cave {} flooded: its surface at {:.2f}, {:.2f} over its lowest floor, {} squares", c, cave.water,
                          cave.water - cave.bottom, squares);
        }
        if (!indoor.indices.empty())
        {
            m_indoorWater = meshes.Upload(indoor, m_tag + "_indoor_water");
            m_meshHandles.push_back(m_indoorWater);
        }
        // And each lockout's own surface: a sheet across its floor, raised to as full of the sea as it is (the game, by Lockout::level).
        for (size_t k = 0; k < m_lockouts.size(); ++k)
        {
            Lockout& lockout = m_lockouts[k];
            MeshData sheet;
            for (const glm::vec2 corner : {glm::vec2(lockout.min.x, lockout.min.z), glm::vec2(lockout.max.x, lockout.min.z), glm::vec2(lockout.min.x, lockout.max.z),
                                           glm::vec2(lockout.max.x, lockout.max.z)})
            {
                MeshVertex vertex;
                vertex.position = {corner.x, lockout.min.y, corner.y};
                vertex.normal = {0.0f, 1.0f, 0.0f};
                vertex.uv = {2.0f, 0.0f};
                sheet.vertices.push_back(vertex);
            }
            sheet.indices = {0, 2, 1, 1, 2, 3};
            lockout.water = meshes.Upload(sheet, m_tag + "_lockout_water_" + std::to_string(k));
            m_meshHandles.push_back(lockout.water);
            PRED_LOG_INFO(Gameplay, "Site lockout {}: from {:.1f} {:.1f} {:.1f} to {:.1f} {:.1f} {:.1f}, its doors {} and {}", k, lockout.min.x, lockout.min.y, lockout.min.z,
                          lockout.max.x, lockout.max.y, lockout.max.z, lockout.inner, lockout.outer);
        }
    }
    // Its water, if it has any (SiteTerrain::WaterMesh), drawn over the picture by the game (WaterRenderer).
    m_waterMesh = MeshHandle{};
    if (m_terrain.HasWater() || m_terrain.HasStreams())
    {
        const MeshData water = m_terrain.WaterMesh();
        if (!water.indices.empty())
        {
            m_waterMesh = meshes.Upload(water, m_tag + "_water");
            m_meshHandles.push_back(m_waterMesh);
            PRED_LOG_INFO(Gameplay, "Site water: {} at {:.1f}", m_look.liquid == Look::Liquid::Lava ? "lava" : m_look.liquid == Look::Liquid::Acid ? "acid" : m_look.liquid == Look::Liquid::Ice ? "ice" : "water",
                          m_terrain.WaterLevel());
            // Frozen, it is ground: walked on, and sounds of ice underfoot.
            if (m_look.liquid == Look::Liquid::Ice)
            {
                const BodyHandle ice = physics.CreateMeshBody(water, Transform{});
                physics.SetTag(ice, Surfaces::StepOfSet("ice_cliff", false));
                m_bodies.push_back(ice);
            }
        }
        // Lava lights what is round it: an orange glow over its edges, where there is ground for it to light -- every stretch
        // of shore over the ground that can be walked and a margin round it, the nearest the site first (taken from one
        // corner, the few there were all lit one bank of one pool).
        if (m_look.liquid == Look::Liquid::Lava && lights != nullptr)
        {
            constexpr float kCell = 14.0f;
            constexpr size_t kMostGlows = 56;
            const glm::vec2 from = m_terrain.Min() - glm::vec2(40.0f);
            const int cells = static_cast<int>((m_terrain.Extent() + 80.0f) / kCell);
            std::vector<glm::vec2> shores;
            for (int j = 0; j < cells; ++j)
            {
                for (int i = 0; i < cells; ++i)
                {
                    const glm::vec2 at = from + (glm::vec2(static_cast<float>(i), static_cast<float>(j)) + 0.5f) * kCell;
                    if (m_terrain.WaterDepth(at.x, at.y) < 0.2f)
                    {
                        continue;
                    }
                    // Dry ground somewhere within a cell of it.
                    bool shore = false;
                    for (int k = 0; k < 8 && !shore; ++k)
                    {
                        const float angle = static_cast<float>(k) * 0.785398f;
                        const glm::vec2 out = at + glm::vec2(std::cos(angle), std::sin(angle)) * kCell;
                        shore = m_terrain.WaterDepth(out.x, out.y) < 0.0f;
                    }
                    if (shore)
                    {
                        shores.push_back(at);
                    }
                }
            }
            const glm::vec2 middle{m_plan.origin.x + m_plan.size * 0.5f, m_plan.origin.z + m_plan.size * 0.5f};
            std::sort(shores.begin(), shores.end(), [&](const glm::vec2& a, const glm::vec2& b) { return glm::distance(a, middle) < glm::distance(b, middle); });
            shores.resize(std::min(shores.size(), kMostGlows));
            for (const glm::vec2& at : shores)
            {
                lights->AddLamp({at.x, m_terrain.WaterLevel() + 1.5f, at.y}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.4f, 0.09f}, 90.0f, 24.0f, 180.0f, 180.0f);
            }
            if (!shores.empty())
            {
                PRED_LOG_INFO(Gameplay, "Site lava: {} glows along its shores, the nearest at {:.0f} {:.0f} {:.0f}", shores.size(), shores[0].x,
                              m_terrain.WaterLevel(), shores[0].y);
            }
        }
    }
    builder.BeginBatching(24.0f);
    int containers = 0;
    uint32_t rocks = 0;
    for (const SitePlan::Block& block : m_plan.blocks)
    {
        // (An outpost's own things are its own: only its ground here.)
        if (groundOnly)
        {
            break;
        }
        // (No boulder or wreckage in the way of what stands out in a valley -- the field generator, the relay mast -- where it was put or
        // moved to: room round it to get to it.)
        if (block.kind == Kind::Rock || block.kind == Kind::Debris)
        {
            const bool inTheWay = std::any_of(m_plan.blocks.begin(), m_plan.blocks.end(), [&](const SitePlan::Block& other)
            {
                return other.outlying && glm::distance(glm::vec2(other.centre.x, other.centre.z), glm::vec2(block.centre.x, block.centre.z)) <
                                             std::max(other.size.x, other.size.z) * 0.5f + std::max(block.size.x, block.size.z) * 0.5f + 3.0f;
            });
            if (inTheWay)
            {
                continue;
            }
        }
        Transform transform;
        transform.position = block.centre;
        // Boulders, wreckage and heaped spoil lie where the ground is, and what stands out in the land; everything else stands on levelled
        // ground.
        if (block.kind == Kind::Rock || block.kind == Kind::Debris || block.kind == Kind::SpoilHeap || block.outlying)
        {
            // (From the lowest of the ground under its corners, however it is turned.)
            transform.position.y += lift({block.centre.x, block.centre.z}, std::max(block.size.x, block.size.z) * 0.5f);
        }
        transform.rotation = glm::angleAxis(block.yaw, glm::vec3(0.0f, 1.0f, 0.0f)) * glm::angleAxis(block.tip, glm::vec3(1.0f, 0.0f, 0.0f));
        // The pipework too, which runs into the side of each building it joins; and debris, half in the ground.
        const bool terrain = block.kind == Kind::Rock || block.kind == Kind::Pad ||
                             block.kind == Kind::PipeX || block.kind == Kind::PipeZ || block.kind == Kind::Support || block.kind == Kind::Elbow ||
                             block.kind == Kind::Collar || block.kind == Kind::Debris ||
                             block.outlying;
        builder.SetStructure(terrain ? ground : 0);
        // What stands round the buildings by what the place is: drawn as it is built (SiteStructures).
        if (SiteStructures::Is(block.kind))
        {
            // (Set into the ground as a boulder is -- its footings, its heap's foot -- and its own parts meeting, as a leg on its footing.)
            builder.SetStructure(ground);
            SiteStructures::Build(block, transform.position - glm::vec3(0.0f, block.size.y * 0.5f, 0.0f), builder, structureMaterials);
            PRED_LOG_INFO(Gameplay, "Site structure {} at {:.0f} {:.0f} {:.0f}, {:.0f} m high", static_cast<int>(block.kind), block.centre.x, block.centre.y - block.size.y * 0.5f, block.centre.z, block.size.y);
            builder.SetStructure(0);
            continue;
        }
        switch (block.kind)
        {
        case Kind::Rock:
        {
            // A boulder (SiteScatter's), not a box: a lump as big as the block is every way, each one its own, its foot a
            // little into the ground. Solid as its shape.
            MeshData rock = SiteScatter::Shape(SiteScatter::Kind::Boulder, static_cast<int>(rocks % 8u), Mix(seed, 0xB0u + rocks));
            ++rocks;
            const AABB bounds = rock.ComputeBounds();
            const glm::vec3 fit = block.size / glm::max(bounds.max - bounds.min, glm::vec3(0.01f));
            const glm::vec3 middle{(bounds.min.x + bounds.max.x) * 0.5f, bounds.min.y, (bounds.min.z + bounds.max.z) * 0.5f};
            for (MeshVertex& vertex : rock.vertices)
            {
                vertex.position = (vertex.position - middle) * fit;
                vertex.normal = glm::normalize(vertex.normal / fit);
            }
            Transform foot = transform;
            foot.position.y -= block.size.y * 0.5f + 0.1f;
            builder.AddMesh(NameOf(block.kind), foot, rock, rockMaterial);
            break;
        }
        case Kind::Generator:
        {
            // The field generator as one is built: a long steel housing on a concrete plinth, louvred down both sides for the
            // air, a radiator grille across one end, a silencer on the roof and its exhaust stack up off it, and the cables
            // out of its other end into the ground through a conduit; the housing's own doors on the side it faces.
            const glm::vec3 foot = transform.position - glm::vec3(0.0f, block.size.y * 0.5f, 0.0f);
            const glm::quat turn = transform.rotation;
            const auto part = [&](const glm::vec3& local, const MeshData& mesh, const Material& material, const glm::quat& lie = glm::quat(1.0f, 0.0f, 0.0f, 0.0f))
            {
                Transform where;
                where.position = foot + turn * local;
                where.rotation = turn * lie;
                builder.AddMesh(NameOf(block.kind), where, mesh, material, false);
            };
            const auto box = [&](const glm::vec3& lo, const glm::vec3& hi, const Material& material)
            {
                part((lo + hi) * 0.5f, Primitives::Box(hi - lo), material);
            };
            const float half = block.size.x * 0.5f;
            const float side = block.size.z * 0.5f;
            const float plinth = 0.25f;
            const float roof = block.size.y;
            PRED_LOG_INFO(Gameplay, "Site field generator standing at {:.1f} {:.1f} {:.1f}, facing {:.0f} degrees", foot.x, foot.y, foot.z, glm::degrees(block.yaw));
            // (Into the ground at its foot by as much as the ground falls away under it.)
            box({-half - 0.25f, -0.6f, -side - 0.25f}, {half + 0.25f, plinth, side + 0.25f}, padMaterial);
            box({-half, plinth, -side}, {half, roof, side}, gensetMaterial);
            // The louvres: a dark frame let into each side, slats across it standing a little proud.
            for (const float s : {-1.0f, 1.0f})
            {
                for (const float x : {-half * 0.62f, 0.0f, half * 0.62f})
                {
                    // (Not across the middle of the side the breaker is on: its doors are there.)
                    if (s < 0.0f && std::abs(x) < 0.1f)
                    {
                        continue;
                    }
                    const float face = s * side;
                    box({x - 0.6f, plinth + 0.9f, face - 0.012f}, {x + 0.6f, plinth + 1.9f, face + 0.012f}, grilleMaterial);
                    for (float y = plinth + 0.98f; y < plinth + 1.85f; y += 0.11f)
                    {
                        box({x - 0.56f, y, face + s * 0.012f}, {x + 0.56f, y + 0.045f, face + s * 0.04f}, gensetMaterial);
                    }
                }
            }
            // Its doors, either side of the breaker: their seams and handles.
            for (const float x : {-0.75f, 0.75f})
            {
                box({x - 0.6f, plinth + 0.15f, -side - 0.006f}, {x - 0.58f, roof - 0.25f, -side + 0.006f}, grilleMaterial);
                box({x + 0.58f, plinth + 0.15f, -side - 0.006f}, {x + 0.6f, roof - 0.25f, -side + 0.006f}, grilleMaterial);
                box({x - 0.6f, roof - 0.27f, -side - 0.006f}, {x + 0.6f, roof - 0.25f, -side + 0.006f}, grilleMaterial);
                const float handle = x < 0.0f ? x + 0.5f : x - 0.5f;
                box({handle - 0.02f, plinth + 0.95f, -side - 0.05f}, {handle + 0.02f, plinth + 1.25f, -side}, supportMaterial);
            }
            // The radiator's grille across its -x end.
            box({-half - 0.012f, plinth + 0.3f, -side + 0.25f}, {-half + 0.012f, roof - 0.3f, side - 0.25f}, grilleMaterial);
            for (float z = -side + 0.35f; z < side - 0.3f; z += 0.12f)
            {
                box({-half - 0.04f, plinth + 0.35f, z}, {-half - 0.012f, roof - 0.35f, z + 0.04f}, gensetMaterial);
            }
            // The silencer lying on the roof along it, and the stack up off its end with a rain cap.
            const glm::quat lying = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
            part({-half * 0.35f, roof + 0.32f, 0.0f}, Primitives::Cylinder(0.26f, 1.6f, 16), pipeMaterial, lying);
            box({-half * 0.35f - 0.5f, roof, -0.12f}, {-half * 0.35f - 0.4f, roof + 0.1f, 0.12f}, supportMaterial);
            box({-half * 0.35f + 0.4f, roof, -0.12f}, {-half * 0.35f + 0.5f, roof + 0.1f, 0.12f}, supportMaterial);
            part({-half * 0.35f + 0.8f, roof + 0.32f + 0.7f, 0.0f}, Primitives::Cylinder(0.11f, 1.4f, 12), pipeMaterial);
            part({-half * 0.35f + 0.8f, roof + 0.32f + 1.45f, 0.0f}, Primitives::Cylinder(0.17f, 0.04f, 12), pipeMaterial);
            // Its cables: out of the +x end low down, into a conduit bent down into the ground.
            box({half, plinth + 0.2f, -0.35f}, {half + 0.25f, plinth + 0.7f, 0.35f}, grilleMaterial);
            part({half + 0.4f, plinth - 0.1f, 0.0f}, Primitives::Cylinder(0.08f, 1.0f, 10), pipeMaterial);
            m_bodies.push_back(physics.CreateBox(block.size * 0.5f, transform, BodyMotion::Static));
            physics.SetOverlapGroup(m_bodies.back(), ground);
            break;
        }
        case Kind::Container:
        {
            // A freight container (Cargo) the size of the block, its length along the block's longer side: drawn as one and
            // solid as the block.
            const bool alongZ = block.size.z >= block.size.x;
            const glm::vec3 colour = kContainerColours[static_cast<size_t>(Mix(seed, static_cast<uint32_t>(containers++)) % std::size(kContainerColours))];
            Transform foot = transform;
            foot.position.y -= block.size.y * 0.5f;
            foot.rotation = transform.rotation * glm::angleAxis(alongZ ? 0.0f : glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
            for (const auto& [material, mesh] : Cargo::Drawn(Cargo::ShippingContainer(alongZ ? block.size.z : block.size.x, colour)))
            {
                builder.AddMesh(NameOf(block.kind), foot, mesh, material, false);
            }
            m_bodies.push_back(physics.CreateBox(block.size * 0.5f, transform, BodyMotion::Static));
            // (Out in the land, set into the ground as it lies there: one structure with it, as a boulder is.)
            if (block.outlying)
            {
                physics.SetOverlapGroup(m_bodies.back(), ground);
            }
            break;
        }
        case Kind::Pad:
            if (m_terrain.Submerged())
            {
                // Floating on the sea over a site on its floor: a landing platform (BuildPontoon), its deck where the shuttle sets down
                // (m_plan.landing raised to it), along the way its ramp comes down, ladders up its sides out of the water.
                const glm::vec2 along = glm::vec2(m_plan.rampFoot.position.x - m_plan.landing.x, m_plan.rampFoot.position.z - m_plan.landing.z);
                const Material deckMaterial = Surfaces::ApplyWorn(Material::Diffuse({0.3f, 0.31f, 0.32f}, 0.55f), "deck_plate", 2.0f, 0.4f);
                const Material floatMaterial = Surfaces::ApplyWorn(Material::Diffuse({0.72f, 0.36f, 0.08f}, 0.5f), "painted_metal", 2.0f, 0.4f);
                std::vector<PontoonLamp> padLamps;
                builder.SetStructure(ground);
                BuildPontoon(m_plan.landing, glm::length(along) > 0.1f ? along : glm::vec2(0.0f, 1.0f), m_plan.landing.y - 0.5f, m_terrain.WaterLevel(),
                             m_terrain.Height(m_plan.landing.x, m_plan.landing.z) - 1.5f, builder,
                             deckMaterial, floatMaterial, supportMaterial, hazardMaterial, m_ladders, padLamps, m_seaPosts);
                builder.SetStructure(0);
                for (size_t k = 0; lights != nullptr && k < padLamps.size(); ++k)
                {
                    const PontoonLamp& lamp = padLamps[k];
                    if (lamp.beacon)
                    {
                        lights->Add(scene, meshes, LightKind::Emergency, LightMood::Pulse, lamp.at, lamp.direction, 0, Mix(seed, 0x9A0D0u + static_cast<uint32_t>(k)) | 1u, 6.0f);
                    }
                    else
                    {
                        lights->Add(scene, meshes, LightKind::Area, LightMood::Steady, lamp.at, lamp.direction, 0, Mix(seed, 0x9A0D1u + static_cast<uint32_t>(k)) | 1u, 22.0f);
                    }
                }
                break;
            }
            builder.AddBox(NameOf(block.kind), transform, block.size, padMaterial, kOutdoorTile);
            break;
        case Kind::Support:
        {
            // A post on a foot plate, and a saddle across its top the pipe lies in, with a brace up from the post to each end
            // of it. The block is the post's height and the saddle's width (size.z, across the pipe).
            const float high = block.size.y;
            const float across = block.size.z;
            const glm::vec3 foot = transform.position - glm::vec3(0.0f, high * 0.5f, 0.0f);
            Transform part = transform;
            // (Standing on its plate, not through it: the two bottoms would be one face, drawn twice.)
            part.position = foot + glm::vec3(0.0f, (high + 0.06f) * 0.5f, 0.0f);
            builder.AddBox(NameOf(block.kind), part, {0.22f, high - 0.06f, 0.22f}, supportMaterial);
            part.position = foot + glm::vec3(0.0f, 0.03f, 0.0f);
            builder.AddBox(NameOf(block.kind), part, {0.5f, 0.06f, 0.5f}, supportMaterial);
            // The saddle a little up round the pipe's underside, so it lies in it rather than touching it along one line, sky
            // showing between them either side.
            part.position = foot + glm::vec3(0.0f, high + 0.09f, 0.0f);
            builder.AddBox(NameOf(block.kind), part, {0.24f, 0.18f, across}, supportMaterial);
            // And a cheek up either side of the pipe from it, the pipe held between them (a clamp round its top: SitePlan's collar).
            for (const float s : {-1.0f, 1.0f})
            {
                part.position = foot + glm::vec3(0.0f, high + 0.275f, 0.0f) + transform.rotation * glm::vec3(0.0f, 0.0f, s * (across * 0.5f - 0.05f));
                builder.AddBox(NameOf(block.kind), part, {0.2f, 0.45f, 0.05f}, supportMaterial);
            }
            for (const float s : {-1.0f, 1.0f})
            {
                // From partway up the post out to under the saddle's end, in the plane across the pipe.
                const glm::vec3 low = foot + glm::vec3(0.0f, high - 0.7f, 0.0f);
                // (Up into the saddle, not short of it: its end is cut square across a slope, and a sliver of sky showed.)
                const glm::vec3 up = foot + glm::vec3(0.0f, high + 0.08f, 0.0f) + transform.rotation * glm::vec3(0.0f, 0.0f, s * (across * 0.5f - 0.1f));
                const glm::vec3 run = up - low;
                Transform brace;
                brace.position = (low + up) * 0.5f;
                brace.rotation = glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), glm::normalize(run));
                builder.AddMesh(NameOf(block.kind), brace, Primitives::Box({0.08f, glm::length(run), 0.08f}), supportMaterial, false);
            }
            break;
        }
        case Kind::PipeX:
        case Kind::PipeZ:
        {
            // A cylinder stands along y: laid down along the pipe's run -- with a flanged joint every six metres or so along it.
            const bool alongX = block.kind == Kind::PipeX;
            const float length = alongX ? block.size.x : block.size.z;
            const float radius = block.size.y * 0.5f;
            transform.rotation = alongX ? glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f))
                                        : glm::angleAxis(glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
            builder.AddMesh(NameOf(block.kind), transform, Primitives::Cylinder(radius, length, 16), pipeMaterial);
            const int joints = static_cast<int>(length / 6.0f);
            for (int k = 1; k <= joints; ++k)
            {
                const float along = -length * 0.5f + length * static_cast<float>(k) / static_cast<float>(joints + 1);
                Transform joint = transform;
                joint.position = block.centre + (alongX ? glm::vec3(along, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, along));
                builder.AddMesh(NameOf(block.kind), joint, Primitives::Cylinder(radius + 0.05f, 0.1f, 16), pipeMaterial, false);
            }
            break;
        }
        case Kind::Elbow:
            // A bend, not a ball: the block at the corner the pipe turns at, its yaw the way it comes in along, `tip` which way
            // round it goes (SitePlan), size.x round its middle and size.y across the pipe.
            transform.rotation = glm::angleAxis(block.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
            builder.AddMesh(NameOf(block.kind), transform, FacilityOutside::PipeBend(block.size.x, block.size.y, block.tip), pipeMaterial);
            PRED_LOG_INFO(Gameplay, "Site pipe bends at {:.0f} {:.1f} {:.0f}", block.centre.x, block.centre.y, block.centre.z);
            break;
        case Kind::Collar:
        {
            // A ring round the pipe along the block's x: a cylinder laid down that way.
            Transform ring = transform;
            ring.rotation = transform.rotation * glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
            builder.AddMesh(NameOf(block.kind), ring, Primitives::Cylinder(block.size.y * 0.5f, block.size.x, 16), supportMaterial, false);
            break;
        }
        case Kind::Tank:
        {
            // A storage tank as one is built: on a ring of concrete, its shell welded up in courses, a low cone of roof with
            // a vent and a rim, a ladder up its side, and a pipe out at its foot with a valve -- the ladder and the pipe
            // on the side its yaw faces. Painted, the paint worn.
            const float r = block.size.x * 0.5f;
            const float high = block.size.y;
            const glm::vec3 foot = transform.position - glm::vec3(0.0f, high * 0.5f, 0.0f);
            const glm::quat turn = glm::angleAxis(block.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
            const auto part = [&](const glm::vec3& local, const MeshData& mesh, const Material& material, bool solid = false)
            {
                Transform where;
                where.position = foot + turn * local;
                where.rotation = turn;
                builder.AddMesh(NameOf(block.kind), where, mesh, material, solid);
            };
            part({0.0f, 0.05f, 0.0f}, Primitives::Cylinder(r + 0.3f, 0.3f, 24), padMaterial, true);
            const float shell = high - 0.2f - r * 0.24f;
            part({0.0f, 0.2f + shell * 0.5f, 0.0f}, Primitives::Cylinder(r, shell, 28), tankMaterial, true);
            for (float y = 0.2f + 1.5f; y < 0.2f + shell - 0.3f; y += 1.5f)
            {
                part({0.0f, y, 0.0f}, Primitives::Cylinder(r + 0.012f, 0.035f, 28), tankMaterial);
            }
            const float roofFoot = 0.2f + shell;
            part({0.0f, roofFoot + 0.06f, 0.0f}, Primitives::Cylinder(r + 0.04f, 0.12f, 28), tankMaterial);
            part({0.0f, roofFoot + r * 0.12f, 0.0f}, Primitives::Frustum(r, r * 0.12f, r * 0.24f, 28), tankMaterial, true);
            part({0.0f, roofFoot + r * 0.24f + 0.2f, 0.0f}, Primitives::Cylinder(0.12f, 0.4f, 12), tankMaterial);
            part({0.0f, roofFoot + r * 0.24f + 0.43f, 0.0f}, Primitives::Frustum(0.22f, 0.02f, 0.12f, 12), tankMaterial);
            // The ladder: two rails stood off the shell on brackets, and a rung every 0.3 m.
            const float out = r + 0.22f;
            const float ladderTop = roofFoot + 0.9f;
            for (const float s : {-0.22f, 0.22f})
            {
                part({s, ladderTop * 0.5f, out}, Primitives::Box({0.05f, ladderTop, 0.05f}), supportMaterial);
                // Stood off the shell on brackets.
                for (float y = 1.0f; y < ladderTop - 0.5f; y += 2.0f)
                {
                    part({s, y, (r + out) * 0.5f}, Primitives::Box({0.04f, 0.04f, out - r + 0.04f}), supportMaterial);
                }
            }
            for (float y = 0.2f + Ladder::kRungSpacing; y < ladderTop - 0.1f; y += Ladder::kRungSpacing)
            {
                part({0.0f, y, out}, Primitives::Box({0.44f, 0.03f, 0.03f}), supportMaterial);
            }
            // And climbed (PlayerController's ladders, the same list on every machine): from the concrete ring up to a little
            // over the roof's edge, then over onto the roof, as a ledge is climbed -- the rails standing on past it to hold. Its
            // foot taken on the shell so its rungs stand where a climber's hands and feet go on them (Ladder::kRungOut).
            Ladder climb;
            climb.out = turn * glm::vec3(0.0f, 0.0f, 1.0f);
            climb.foot = foot + turn * glm::vec3(0.0f, 0.2f, out - Ladder::kRungOut);
            climb.height = roofFoot + 0.3f - 0.2f;
            climb.width = 0.44f;
            m_ladders.push_back(climb);
            PRED_LOG_INFO(Gameplay, "Site tank at {:.0f} {:.0f}, its ladder site_ladder {}", foot.x, foot.z, m_ladders.size() - 1);
            // The outlet at its foot: out of the shell, a flange, and a valve with its wheel -- off to one side of the ladder.
            const glm::quat sideways = glm::angleAxis(glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
            const float pipeOut = 0.9f;
            Transform pipe;
            pipe.rotation = turn * sideways;
            pipe.position = foot + turn * glm::vec3(r * 0.55f, 0.55f, r * 0.83f + pipeOut * 0.5f);
            builder.AddMesh(NameOf(block.kind), pipe, Primitives::Cylinder(0.11f, pipeOut, 12), pipeMaterial, false);
            pipe.position = foot + turn * glm::vec3(r * 0.55f, 0.55f, r * 0.83f + pipeOut * 0.6f);
            builder.AddMesh(NameOf(block.kind), pipe, Primitives::Cylinder(0.17f, 0.06f, 12), pipeMaterial, false);
            part({r * 0.55f, 0.85f, r * 0.83f + pipeOut * 0.6f}, Primitives::Cylinder(0.025f, 0.45f, 8), supportMaterial);
            part({r * 0.55f, 1.08f, r * 0.83f + pipeOut * 0.6f}, Primitives::Cylinder(0.16f, 0.03f, 12), supportMaterial);
            break;
        }
        case Kind::Mast:
        {
            // A radio mast as one is built: three legs in a triangle narrowing as it rises, braced every panel by struts across
            // and a zig-zag up each face, guyed at two heights to concrete anchors out round it; antennas at its top and the
            // red light that warns aircraft.
            const float high = block.size.y;
            const glm::vec3 base = transform.position - glm::vec3(0.0f, high * 0.5f, 0.0f);
            const glm::quat turn = glm::angleAxis(block.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
            const float wide = block.size.x * 0.7f;
            PRED_LOG_INFO(Gameplay, "Site mast {:.0f} m at {:.0f} {:.0f} {:.0f}", high, base.x, base.y, base.z);
            const auto leg = [&](int k, float y)
            {
                const float a = static_cast<float>(k) * glm::two_pi<float>() / 3.0f;
                const float spread = glm::mix(wide, wide * 0.55f, y / high);
                return base + turn * glm::vec3(std::cos(a) * spread, y, std::sin(a) * spread);
            };
            const auto rod = [&](const glm::vec3& from, const glm::vec3& to, float radius, const Material& material, bool solid)
            {
                const glm::vec3 run = to - from;
                Transform where;
                where.position = (from + to) * 0.5f;
                where.rotation = glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), glm::normalize(run));
                builder.AddMesh(NameOf(block.kind), where, Primitives::Cylinder(radius, glm::length(run), 8), material, solid);
            };
            // A block set into the ground where it stands, however the ground lies under it: from a little under the lowest of
            // the ground beneath its corners to `proud` over the highest -- so it neither floats over a dip nor is lost in a rise.
            const auto footing = [&](const glm::vec3& at, float half, float proud, const glm::quat& turned)
            {
                float lowest = 1.0e9f;
                float highest = -1.0e9f;
                for (const glm::vec2 corner : {glm::vec2(0.0f), glm::vec2(-1.0f, -1.0f), glm::vec2(1.0f, -1.0f), glm::vec2(-1.0f, 1.0f), glm::vec2(1.0f, 1.0f)})
                {
                    const glm::vec3 c = turned * glm::vec3(corner.x * half, 0.0f, corner.y * half);
                    const float ground = m_terrain.Height(at.x + c.x, at.z + c.z);
                    lowest = std::min(lowest, ground);
                    highest = std::max(highest, ground);
                }
                const float bottom = std::min(lowest, at.y) - 0.3f;
                const float top = std::max(highest, at.y) + proud;
                Transform where;
                where.position = glm::vec3(at.x, (bottom + top) * 0.5f, at.z);
                where.rotation = turned;
                builder.AddMesh(NameOf(block.kind), where, Primitives::Box({half * 2.0f, top - bottom, half * 2.0f}), padMaterial);
                return top;
            };
            const int panels = std::max(4, static_cast<int>(high / 1.6f));
            const float step = high / static_cast<float>(panels);
            for (int k = 0; k < 3; ++k)
            {
                for (int p = 0; p < panels; ++p)
                {
                    const float y0 = static_cast<float>(p) * step;
                    const float y1 = y0 + step;
                    rod(leg(k, y0), leg(k, y1), 0.055f, supportMaterial, true);
                    rod(leg(k, y1), leg((k + 1) % 3, y1), 0.025f, supportMaterial, false);
                    // The zig-zag: up across the face one way this panel, the other the next.
                    if (p % 2 == 0)
                    {
                        rod(leg(k, y0), leg((k + 1) % 3, y1), 0.02f, supportMaterial, false);
                    }
                    else
                    {
                        rod(leg((k + 1) % 3, y0), leg(k, y1), 0.02f, supportMaterial, false);
                    }
                }
                // A footing under each leg.
                footing(leg(k, 0.0f), 0.225f, 0.3f, turn);
            }
            // Guyed from two heights to three anchors, out between its legs.
            for (int k = 0; k < 3; ++k)
            {
                const float a = (static_cast<float>(k) + 0.5f) * glm::two_pi<float>() / 3.0f;
                glm::vec3 anchor = base + turn * glm::vec3(std::cos(a), 0.0f, std::sin(a)) * (high * 0.55f);
                anchor.y = m_terrain.Height(anchor.x, anchor.z);
                anchor.y = footing(anchor, 0.4f, 0.25f, turn);
                for (const float at : {0.45f, 0.85f})
                {
                    const glm::vec3 top = (leg(k, high * at) + leg((k + 1) % 3, high * at)) * 0.5f;
                    rod(top, anchor - glm::vec3(0.0f, 0.05f, 0.0f), 0.012f, poleMaterial, false);
                }
            }
            // At its top: a whip antenna, two panel antennas on its legs, and the light.
            const glm::vec3 crown = base + glm::vec3(0.0f, high, 0.0f);
            // A plate across the legs' tops, the whip stood on it, and the warning light on the whip's tip -- the highest point of all,
            // where a light to warn aircraft has to be.
            {
                Transform plate;
                plate.position = crown + glm::vec3(0.0f, 0.04f, 0.0f);
                plate.rotation = turn;
                const float span = wide * 0.55f * 2.0f;
                builder.AddMesh(NameOf(block.kind), plate, Primitives::Cylinder(span * 0.5f + 0.1f, 0.08f, 14), supportMaterial, false);
            }
            rod(crown + glm::vec3(0.0f, 0.08f, 0.0f), crown + glm::vec3(0.0f, 3.2f, 0.0f), 0.035f, supportMaterial, false);
            for (int k = 0; k < 2; ++k)
            {
                const glm::vec3 at = leg(k, high - 1.2f);
                const glm::vec3 out = glm::normalize(at - glm::vec3(crown.x, at.y, crown.z));
                Transform panel;
                panel.position = at + out * 0.15f;
                panel.rotation = glm::angleAxis(std::atan2(out.x, out.z), glm::vec3(0.0f, 1.0f, 0.0f));
                builder.AddMesh(NameOf(block.kind), panel, Primitives::Box({0.3f, 1.3f, 0.12f}), tankMaterial, false);
            }
            Material beacon = Material::Diffuse({0.3f, 0.02f, 0.02f}, 0.4f);
            beacon.emissive = {3.0f, 0.15f, 0.1f};
            Transform top;
            top.position = crown + glm::vec3(0.0f, 3.2f + 0.12f, 0.0f);
            builder.AddMesh("site_mast_light", top, Primitives::Sphere(0.16f, 12, 8), beacon, false);
            break;
        }
        case Kind::Debris:
            builder.AddBox(NameOf(block.kind), transform, block.size, Mix(seed, static_cast<uint32_t>(block.centre.x * 7.0f)) % 3u == 0 ? hullDebrisLight : hullDebris);
            break;
        default:
            break;
        }
    }

    // (Over open ground, in larger cells and not split by height: what lies about over hundreds of metres in cells as small
    // as a building's would be thousands of meshes -- more than there are buffers for.)
    builder.BeginBatching(96.0f, true);
    mark("water etc");
    // What lies about on the ground, by the world it is on: in clusters on its own ground, never on what was levelled
    // (SiteScatter). Each shape made once and placed many times, turned and sized in its vertices (a collision mesh is not
    // scaled with its transform), batched with the rest.
    {
        // The landmarks first, and the organism's signs, so nothing grows or lies where they stand (a boulder lay across the
        // roots of one, a tree stood out of its knot).
        m_landmarks = groundOnly ? std::vector<Landmark::Placed>{} : Landmark::Scatter(seed, m_look.biome, m_look.terrain, m_plan, m_terrain);
        // Without air nothing is carved by the weather: no arches, spires, buttes or balanced rocks.
        if (m_look.airless)
        {
            m_landmarks.erase(std::remove_if(m_landmarks.begin(), m_landmarks.end(),
                                             [](const Landmark::Placed& placed)
                                             {
                                                 return placed.kind == Landmark::Kind::Arch || placed.kind == Landmark::Kind::Spire ||
                                                        placed.kind == Landmark::Kind::Butte || placed.kind == Landmark::Kind::BalancedRock;
                                             }),
                              m_landmarks.end());
        }
        std::vector<glm::vec3> clearings;
        // (Round what stands out in the land -- the field generator, the relay mast -- room to stand and get round it.)
        for (const SitePlan::Block& block : m_plan.blocks)
        {
            if (block.outlying)
            {
                clearings.emplace_back(block.centre.x, block.centre.z, std::max(block.size.x, block.size.z) * 0.5f + 5.0f);
                PRED_LOG_INFO(Gameplay, "Site {} out in a valley at {:.0f} {:.0f}", block.kind == SitePlan::BlockKind::Mast ? "relay mast" : "field generator", block.centre.x, block.centre.z);
            }
        }
        // (And round what stands round the buildings by what the place is -- a headframe and its winder house behind it, a tank
        // farm, a fenced array -- nothing growing in it.)
        for (const SitePlan::Block& block : m_plan.blocks)
        {
            if (!block.outlying && SiteStructures::Is(block.kind))
            {
                const float back = block.kind == SitePlan::BlockKind::Headframe ? block.size.y * 0.75f + 4.0f : 0.0f;
                const glm::vec2 at = glm::vec2(block.centre.x, block.centre.z) - glm::vec2(std::sin(block.yaw), std::cos(block.yaw)) * (back * 0.5f);
                clearings.emplace_back(at.x, at.y, std::max(block.size.x, block.size.z) * 0.6f + back * 0.5f + 3.0f);
            }
        }
        for (const Landmark::Placed& landmark : m_landmarks)
        {
            clearings.emplace_back(landmark.at.x, landmark.at.z, landmark.radius);
        }
        for (const Signs::Formation& formation : Signs::Make(seed, m_look.signs, m_plan, m_terrain))
        {
            clearings.emplace_back(formation.at.x, formation.at.z, formation.size * 1.1f);
        }
        std::vector<SiteScatter::Item> scattered = SiteScatter::Plan(seed, m_look.biome, m_plan, m_terrain, clearings);
        // And on airless ground, a third as much lying about, and no spires: bare regolith and craters, a boulder here and there.
        if (m_look.airless)
        {
            size_t kept = 0;
            for (size_t i = 0; i < scattered.size(); ++i)
            {
                if (scattered[i].kind != SiteScatter::Kind::Spire && Hash01(seed ^ 0xA1B1E55u, static_cast<uint32_t>(i), 7u) < 0.33f)
                {
                    scattered[kept++] = scattered[i];
                }
            }
            scattered.resize(kept);
        }
        const bool lush = m_look.biome == "jungle";
        const glm::vec3 growth = glm::mix(m_look.ground, lush ? glm::vec3(0.12f, 0.3f, 0.08f) : glm::vec3(0.2f, 0.34f, 0.12f), 0.65f);
        mark("plan scatter");
        // Whether things grow here, so moss does on its rocks and leaves lie under its trees.
        const bool mossy = m_look.biome == "jungle" || m_look.biome == "temperate" || m_look.biome == "oceanic";
        Material plain = Material::Diffuse(glm::vec3(1.0f), 0.85f);
        Material ice = Surfaces::Apply(Material::Diffuse({0.7f, 0.8f, 0.9f}, 0.35f), "ice_cliff", 3.0f, 0.4f, true);
        // (A frozen fall glossier and paler, its ice laid on larger: up close its set's repeat ran down it in a grid.)
        Material icefall = Surfaces::Apply(Material::Diffuse({0.78f, 0.87f, 0.96f}, 0.18f), "ice_cliff", 9.0f, 0.25f, true);
        // Crystal catches the light -- glossy, the sun's highlight sharp on it -- but gives none of its own: a glow in the dark is the
        // organisms' own sign, not the ground's.
        Material crystal = Material::Diffuse({0.62f, 0.72f, 0.95f}, 0.08f);
        crystal.metallic = 0.1f;
        Material basalt = Surfaces::Apply(Material::Diffuse(m_look.rock * 0.55f, 0.9f), "basalt", 3.0f, 0.3f, true);
        Material dead = Material::Diffuse({0.3f, 0.26f, 0.21f}, 0.9f);
        // Bark, painted on each trunk's vertices; leaves cut out of the foliage atlas.
        Material wood = Surfaces::Apply(Material::Diffuse({0.75f, 0.7f, 0.65f}, 0.9f), "soil_cliff", 1.2f, 0.25f);
        Material leaves = Material::Diffuse(glm::vec3(1.0f), 0.7f);
        leaves.baseColorTexture = Surfaces::FoliageAtlas();
        leaves.cutout = true;
        const auto materialOf = [&](SiteScatter::Kind kind) -> const Material&
        {
            switch (kind)
            {
            case SiteScatter::Kind::Boulder:
            case SiteScatter::Kind::Spire: return rockMaterial;
            case SiteScatter::Kind::IceShard: return ice;
            case SiteScatter::Kind::Crystal: return crystal;
            case SiteScatter::Kind::Basalt: return basalt;
            case SiteScatter::Kind::DeadTree: return dead;
            case SiteScatter::Kind::Tree: return wood;
            default: return plain;
            }
        };
        std::vector<MeshData> shapes(static_cast<size_t>(SiteScatter::Kind::Count) * SiteScatter::kVariants);
        std::vector<MeshData> foliage(shapes.size());
        std::vector<bool> made(shapes.size(), false);
        // The shapes of each kind and variant there are first (a few dozen), then the things themselves in batches: each
        // batch's turning and sizing worked out side by side on threads, then put into the world in order (the same world as
        // one after another; thousands of trees made one after another were most of the time a forest took).
        for (const SiteScatter::Item& item : scattered)
        {
            const size_t which = static_cast<size_t>(item.kind) * SiteScatter::kVariants + static_cast<size_t>(item.variant);
            if (!made[which])
            {
                shapes[which] = SiteScatter::Shape(item.kind, item.variant, seed);
                foliage[which] = SiteScatter::Leaves(item.kind, item.variant, seed, growth);
                made[which] = true;
            }
        }
        struct Ready
        {
            bool skip = false;
            MeshData placed;
            MeshData leafy;
        };
        const auto prepare = [&](const SiteScatter::Item& item, Ready& ready)
        {
            ready = Ready{};
            // Nothing that grows standing in the water (rocks may lie in it).
            const bool grows = item.kind == SiteScatter::Kind::Tree || item.kind == SiteScatter::Kind::Shrub || item.kind == SiteScatter::Kind::Grass ||
                               item.kind == SiteScatter::Kind::DeadTree;
            if (grows && m_terrain.WaterDepth(item.at.x, item.at.z) > -0.3f)
            {
                ready.skip = true;
                return;
            }
            const size_t which = static_cast<size_t>(item.kind) * SiteScatter::kVariants + static_cast<size_t>(item.variant);
            const glm::quat turn = glm::angleAxis(item.yaw, glm::vec3(0.0f, 1.0f, 0.0f)) * glm::angleAxis(item.tip, glm::vec3(1.0f, 0.0f, 0.0f));
            ready.placed = shapes[which];
            for (MeshVertex& vertex : ready.placed.vertices)
            {
                vertex.position = turn * (vertex.position * item.scale);
                vertex.normal = glm::normalize(turn * (vertex.normal / item.scale));
                // Moss on the tops of the rocks where things grow, as it lies on them -- not on their sides.
                if (mossy && (item.kind == SiteScatter::Kind::Boulder || item.kind == SiteScatter::Kind::Spire))
                {
                    const float moss = glm::smoothstep(0.35f, 0.85f, vertex.normal.y) * 0.85f;
                    vertex.color = PaintVertex(glm::mix(glm::vec3(1.0f), growth * 2.2f, moss));
                }
            }
            ready.leafy = foliage[which];
            for (MeshVertex& vertex : ready.leafy.vertices)
            {
                vertex.position = turn * (vertex.position * item.scale);
                vertex.normal = glm::normalize(turn * (vertex.normal / item.scale));
            }
        };
        constexpr size_t kBatch = 512;
        constexpr size_t kSlices = 8;
        std::vector<Ready> ready(kBatch);
        for (size_t first = 0; first < scattered.size(); first += kBatch)
        {
            const size_t count = std::min(kBatch, scattered.size() - first);
            std::vector<std::future<void>> work;
            for (size_t slice = 0; slice < kSlices; ++slice)
            {
                work.push_back(std::async(std::launch::async, [&, slice, first, count]
                {
                    for (size_t i = slice; i < count; i += kSlices)
                    {
                        prepare(scattered[first + i], ready[i]);
                    }
                }));
            }
            for (std::future<void>& done : work)
            {
                done.get();
            }
            for (size_t i = 0; i < count; ++i)
            {
                const SiteScatter::Item& item = scattered[first + i];
                if (ready[i].skip)
                {
                    continue;
                }
                // Small things cast no shadow worth its cost.
                const bool small = item.kind == SiteScatter::Kind::Grass || item.kind == SiteScatter::Kind::Shrub ||
                                   (item.kind == SiteScatter::Kind::Boulder && item.scale.y < 0.6f);
                // (And none past the height field, where they are too far off to need one.)
                const bool shaded = m_terrain.OnField(item.at.x, item.at.z);
                builder.SetShadows(!small && shaded);
                if (!ready[i].placed.vertices.empty())
                {
                    // (Solid only where anybody could walk into it.)
                    const bool reachable = WithinRoam(item.at) == item.at;
                    builder.AddMesh("site_scatter", Transform{item.at}, ready[i].placed, materialOf(item.kind),
                                    reachable && SiteScatter::Solid(item.kind, item.scale));
                }
                if (!ready[i].leafy.vertices.empty())
                {
                    builder.SetShadows(item.kind == SiteScatter::Kind::Tree && shaded);
                    builder.AddMesh("site_foliage", Transform{item.at}, ready[i].leafy, leaves, false);
                }
            }
        }
        mark("near scatter");
        // And the trees of the land round it, past the height field, to the horizon (SiteScatter::FarTrees): drawn however far
        // off, nothing to walk into, no shadow worth its cost at that distance.
        if (!groundOnly)
        {
            const MeshData far = SiteScatter::FarTrees(seed, m_look.biome, m_plan, m_terrain, growth * 0.5f, clearings);
            if (!far.indices.empty())
            {
                const MeshHandle mesh = meshes.Upload(far, m_tag + "_far_trees");
                m_meshHandles.push_back(mesh);
                const Entity entity = scene.CreateMeshEntity("site_far_trees", Transform{}, mesh, Material::Diffuse(glm::vec3(1.0f), 0.85f));
                if (MeshRenderer* renderer = scene.GetMeshRenderer(entity))
                {
                    renderer->castsShadow = false;
                    renderer->blocksSky = false;
                    renderer->farVisible = true;
                }
                m_entities.push_back(entity);
                m_farTrees = entity;
                PRED_LOG_INFO(Gameplay, "Site trees: {} near, {} far vertices", std::count_if(scattered.begin(), scattered.end(), [](const SiteScatter::Item& item) { return item.kind == SiteScatter::Kind::Tree; }), far.vertices.size());
            }
        }
        mark("far trees");
        // And its rocks there, the same way (SiteScatter::FarScatter).
        if (!groundOnly)
        {
            const MeshData far = SiteScatter::FarScatter(seed, m_look.biome, m_plan, m_terrain, m_look.rock, clearings, m_look.airless);
            if (!far.indices.empty())
            {
                const MeshHandle mesh = meshes.Upload(far, m_tag + "_far_scatter");
                m_meshHandles.push_back(mesh);
                const Entity entity = scene.CreateMeshEntity("site_far_scatter", Transform{}, mesh, Material::Diffuse(glm::vec3(1.0f), 0.9f));
                if (MeshRenderer* renderer = scene.GetMeshRenderer(entity))
                {
                    renderer->castsShadow = false;
                    renderer->blocksSky = false;
                    renderer->farVisible = true;
                }
                m_entities.push_back(entity);
                m_farScatter = entity;
                PRED_LOG_INFO(Gameplay, "Site far scatter: {} vertices", far.vertices.size());
            }
        }
        mark("far scatter");
        // And the landmarks: rock in the world's own rock, ice and crystal in theirs, a giant tree from a tree's own shapes, each
        // its own shape. Drawn however far off -- throwing their shadows where they are near enough for it to show -- and
        // nothing to walk into, out past where anybody may go.
        if (!groundOnly)
        {
            const glm::vec2 middle{m_plan.origin.x + m_plan.size * 0.5f, m_plan.origin.z + m_plan.size * 0.5f};
            for (const Landmark::Placed& landmark : m_landmarks)
            {
                builder.SetShadows(glm::distance(glm::vec2(landmark.at.x, landmark.at.z), middle) < 1300.0f);
                if (landmark.kind == Landmark::Kind::GreatTree)
                {
                    const glm::vec3 scale(landmark.size);
                    const glm::quat turn = glm::angleAxis(landmark.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
                    const int variant = static_cast<int>(landmark.seed % 2u) * 2;
                    MeshData trunk = SiteScatter::Shape(SiteScatter::Kind::Tree, variant, landmark.seed);
                    MeshData crown = SiteScatter::Leaves(SiteScatter::Kind::Tree, variant, landmark.seed, growth);
                    for (MeshData* part : {&trunk, &crown})
                    {
                        for (MeshVertex& vertex : part->vertices)
                        {
                            vertex.position = turn * (vertex.position * scale);
                            vertex.normal = glm::normalize(turn * vertex.normal);
                        }
                    }
                    builder.AddMesh("site_landmark", Transform{landmark.at - glm::vec3(0.0f, 0.5f, 0.0f)}, trunk, wood, false);
                    builder.AddMesh("site_landmark_leaves", Transform{landmark.at - glm::vec3(0.0f, 0.5f, 0.0f)}, crown, leaves, false);
                }
                else
                {
                    const Material& stone = landmark.kind == Landmark::Kind::Icefall    ? icefall
                                           : landmark.kind == Landmark::Kind::IceTower ? ice
                                           : landmark.kind == Landmark::Kind::Crystals ? crystal
                                           : landmark.kind == Landmark::Kind::Mast ? hullDebris
                                                                                   : rockMaterial;
                    builder.AddMesh("site_landmark", Transform{}, Landmark::Shape(landmark, &m_terrain), stone, false);
                }
                PRED_LOG_INFO(Gameplay, "Site landmark: {} {:.0f} m at {:.0f} {:.0f} {:.0f}", Landmark::Name(landmark.kind), landmark.size, landmark.at.x,
                              landmark.at.y, landmark.at.z);
            }
        }
        mark("landmarks");
        // And its vents, where the world is still alive under it: a crusted mound round each mouth (its plume is the game's to
        // draw, by the clock: Vents::Plumes).
        m_vents = Vents::Plan(seed, m_look.biome, m_plan, m_terrain);
        // And the organism's signs, where this place has any (Signs): its rock the world's own where faint, darker and wet with a
        // sheen where plain -- each its own thing in the scene, not batched, so the game can give it the rhythm's faint glow.
        m_signs.clear();
        if (m_look.signs > 0.0f)
        {
            const float clear = glm::smoothstep(0.15f, 0.9f, m_look.signs);
            Material stone = rockMaterial;
            // (The rock material's colour is over its set's average: the tissue colour put over the same.)
            stone.baseColor = glm::mix(rockMaterial.baseColor, rockMaterial.baseColor * m_look.signTissue / glm::max(m_look.rock, glm::vec3(0.02f)), clear * 0.85f);
            stone.roughness = glm::mix(rockMaterial.roughness, 0.3f, clear);
            // Alive under the rock: a slow swell running out along its branches from the knot (Material::organic, given its beat
            // each frame by the game), never dying back.
            stone.organic = {1.0f, 1.0e6f, -1.0e6f, 0.0f};
            for (const Signs::Formation& formation : Signs::Make(seed, m_look.signs, m_plan, m_terrain))
            {
                const MeshData data = Signs::Rock(formation, m_look.signs, m_terrain);
                PRED_LOG_INFO(Gameplay, "Site sign at {:.0f} {:.0f} {:.0f}, {:.0f} m across", formation.at.x, formation.at.y, formation.at.z, formation.size);
                if (data.indices.empty())
                {
                    continue;
                }
                // (Each its own name: the library shares meshes by name.)
                const MeshHandle mesh = meshes.Upload(data, m_tag + "_sign_rock#" + std::to_string(m_signs.size()));
                m_meshHandles.push_back(mesh);
                const Entity entity = scene.CreateMeshEntity("site_sign_rock", Transform{}, mesh, stone);
                m_entities.push_back(entity);
                m_signs.push_back(entity);
            }
            PRED_LOG_INFO(Gameplay, "Site signs: {:.2f}, {} pieces", m_look.signs, m_signs.size());
        }
        mark("signs");
        // And on a storm world, glass where lightning has struck: dark, glossy, fused into the ground.
        {
            Material glass = Material::Diffuse({0.05f, 0.05f, 0.06f}, 0.08f);
            glass.metallic = 0.15f;
            const std::vector<glm::vec3> strikes = Landmark::Strikes(seed, m_look.biome, m_plan, m_terrain);
            if (!strikes.empty())
            {
                PRED_LOG_INFO(Gameplay, "Site glass: {} lightning strikes, the first at {:.0f} {:.0f} {:.0f}", strikes.size(), strikes[0].x, strikes[0].y, strikes[0].z);
            }
            for (const glm::vec3& strike : strikes)
            {
                builder.AddMesh("site_strike", Transform{}, Landmark::Strike(strike, seed, m_terrain), glass, false);
            }
        }
        PRED_LOG_INFO(Gameplay, "Site vents: {}{}", m_vents.size(), m_vents.empty() ? std::string() : fmt::format(", the first at {:.0f} {:.0f} {:.0f}", m_vents[0].at.x, m_vents[0].at.y, m_vents[0].at.z));
        builder.SetShadows(false);
        for (const Vents::Vent& vent : m_vents)
        {
            builder.AddMesh("site_vent", Transform{}, Vents::Mound(vent, m_look.biome, seed), rockMaterial, false);
        }
        mark("glass");
        // Leaves fallen under the trees, lying flat on the ground round each trunk as far out as its crown, dried brown and
        // yellow -- under a conifer (its odd shapes, SiteScatter::Leaves) its needles, rust-brown: cards of the foliage picture, as
        // its leaves are.
        builder.SetShadows(false);
        {
            MeshData litter;
            uint32_t n = 0;
            for (const SiteScatter::Item& item : scattered)
            {
                if (item.kind != SiteScatter::Kind::Tree)
                {
                    continue;
                }
                const float crown = std::max(item.scale.x, item.scale.z) * 0.9f;
                const bool conifer = item.variant % 2 != 0;
                const glm::vec4 cell = Foliage::Cell(conifer ? Foliage::kNeedles : Foliage::kBroad);
                const int count = 6 + static_cast<int>(Hash01(seed, n, 0x11u) * 8.0f);
                for (int i = 0; i < count; ++i, ++n)
                {
                    const float angle = Hash01(seed, n, 0x21u) * glm::two_pi<float>();
                    const float out = crown * std::sqrt(Hash01(seed, n, 0x22u));
                    const glm::vec2 at = glm::vec2(item.at.x, item.at.z) + glm::vec2(std::cos(angle), std::sin(angle)) * out;
                    if (m_plan.InBuilding(at, 0.5f) || m_terrain.WaterDepth(at.x, at.y) > 0.0f)
                    {
                        continue;
                    }
                    const float size = 0.35f + 0.35f * Hash01(seed, n, 0x23u);
                    const float yaw = Hash01(seed, n, 0x24u) * glm::two_pi<float>();
                    const glm::vec3 up = m_terrain.GroundNormal(at.x, at.y);
                    const glm::vec3 across = glm::normalize(glm::cross(up, glm::vec3(std::sin(yaw), 0.0f, std::cos(yaw)))) * (size * 0.5f);
                    const glm::vec3 along = glm::normalize(glm::cross(across, up)) * (size * 0.5f);
                    const glm::vec3 middle{at.x, m_terrain.GroundHeight(at.x, at.y) + 0.012f + 0.004f * static_cast<float>(i % 3), at.y};
                    const glm::vec3 dried = conifer ? glm::mix(glm::vec3(0.4f, 0.22f, 0.1f), glm::vec3(0.5f, 0.32f, 0.15f), Hash01(seed, n, 0x25u))
                                                    : glm::mix(glm::vec3(0.42f, 0.3f, 0.14f), glm::vec3(0.55f, 0.46f, 0.18f), Hash01(seed, n, 0x25u));
                    const uint32_t colour = PaintVertex(dried *
                                                        glm::mix(0.6f, 1.0f, Hash01(seed, n, 0x26u)));
                    const auto base = static_cast<uint32_t>(litter.vertices.size());
                    const glm::vec3 corners[4] = {middle - across - along, middle + across - along, middle + across + along, middle - across + along};
                    const glm::vec2 uvs[4] = {{cell.x, cell.w}, {cell.z, cell.w}, {cell.z, cell.y}, {cell.x, cell.y}};
                    for (int c = 0; c < 4; ++c)
                    {
                        MeshVertex vertex;
                        vertex.position = corners[c];
                        vertex.normal = up;
                        vertex.uv = uvs[c];
                        vertex.color = colour;
                        litter.vertices.push_back(vertex);
                    }
                    litter.indices.insert(litter.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
                }
            }
            if (!litter.vertices.empty())
            {
                builder.AddMesh("site_litter", Transform{}, litter, leaves, false);
            }
        }
        mark("leaf litter");
        // And pebbles: small stones lying all over the ground, a handful round every step -- the world's rock, a few shapes
        // turned and sized, sunk a little into the ground, too small to stand on or to throw a shadow worth drawing. In blocks
        // of ground, so what is far off can be let go of.
        {
            std::array<MeshData, 6> stones;
            for (size_t i = 0; i < stones.size(); ++i)
            {
                stones[i] = Pebble(Mix(seed, static_cast<uint32_t>(i) + 0x5EB1u));
            }
            constexpr float kCell = 1.5f;
            constexpr int kBlockCells = 24;
            // On a frozen world most are under the snow: a few, sunk half into it -- a scatter of dark chips over the white read
            // as dirt on it.
            const bool snowedOver = m_look.biome == "frozen";
            const float lying = snowedOver ? 0.06f : 0.38f;
            const float sunk = snowedOver ? 0.15f : 0.05f;
            // (Over all of the field, but only where anybody may go and a little round it: PastRoam.)
            constexpr float kOut = SiteTerrain::kMargin - 10.0f;
            const int cells = static_cast<int>((m_plan.size + kOut * 2.0f) / kCell);
            // Each row of blocks made on a thread of its own (they are independent of each other), then put into the world in
            // order: tens of thousands of stones, made one after another, were most of a second of the site being made.
            const int rows = (cells + kBlockCells - 1) / kBlockCells;
            std::vector<std::vector<MeshData>> blocks(static_cast<size_t>(rows));
            std::vector<std::future<void>> work;
            for (int row = 0; row < rows; ++row)
            {
                work.push_back(std::async(std::launch::async, [&, row]
                {
                    const int bz = row * kBlockCells;
                    for (int bx = 0; bx < cells; bx += kBlockCells)
                    {
                        MeshData block;
                        for (int cz = bz; cz < std::min(bz + kBlockCells, cells); ++cz)
                        {
                            for (int cx = bx; cx < std::min(bx + kBlockCells, cells); ++cx)
                            {
                                const auto ux = static_cast<uint32_t>(cx);
                                const auto uz = static_cast<uint32_t>(cz);
                                if (Hash01(seed ^ 0x9EBB1Eu, ux, uz) > lying)
                                {
                                    continue;
                                }
                                const int count = 1 + static_cast<int>(Hash01(seed, ux * 7u + 1u, uz) * 3.0f);
                                for (int p = 0; p < count; ++p)
                                {
                                    const auto k = static_cast<uint32_t>(p) * 31u;
                                    const float x = m_plan.origin.x - kOut + (static_cast<float>(cx) + Hash01(seed, ux + k, uz * 3u + 1u)) * kCell;
                                    const float z = m_plan.origin.z - kOut + (static_cast<float>(cz) + Hash01(seed, ux + k, uz * 3u + 2u)) * kCell;
                                    if (m_terrain.PastRoam(x, z) > 6.0f || m_terrain.Normal(x, z).y < 0.8f || m_plan.InBuilding({x, z}, 0.5f))
                                    {
                                        continue;
                                    }
                                    // Mostly small, now and then a stone a hand across.
                                    const float pick = Hash01(seed, ux + k, uz * 3u + 3u);
                                    const float size = pick > 0.9f ? 0.2f + 0.15f * Hash01(seed, ux + k, uz * 5u) : 0.06f + 0.12f * pick;
                                    const float yaw = Hash01(seed, ux + k, uz * 5u + 1u) * glm::two_pi<float>();
                                    const glm::quat turn = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
                                    const glm::vec3 foot{x, m_terrain.Height(x, z) - size * sunk, z};
                                    const MeshData& stone = stones[static_cast<size_t>(ux * 5u + uz * 3u + static_cast<uint32_t>(p)) % stones.size()];
                                    const uint32_t shade = PaintVertex(glm::vec3(0.7f + 0.5f * Hash01(seed, ux + k, uz * 5u + 2u)));
                                    const auto base = static_cast<uint32_t>(block.vertices.size());
                                    for (MeshVertex vertex : stone.vertices)
                                    {
                                        vertex.position = foot + turn * (vertex.position * glm::vec3(size, size * 0.8f, size));
                                        vertex.normal = turn * vertex.normal;
                                        vertex.color = shade;
                                        block.vertices.push_back(vertex);
                                    }
                                    for (const uint32_t index : stone.indices)
                                    {
                                        block.indices.push_back(base + index);
                                    }
                                }
                            }
                        }
                        blocks[static_cast<size_t>(row)].push_back(std::move(block));
                    }
                }));
            }
            for (std::future<void>& done : work)
            {
                done.get();
            }
            for (const std::vector<MeshData>& row : blocks)
            {
                for (const MeshData& block : row)
                {
                    if (!block.vertices.empty())
                    {
                        builder.AddMesh("site_pebbles", Transform{}, block, rockMaterial, false);
                    }
                }
            }
        }
        mark("pebbles");
        // Drifts against the buildings on a snow or sand world: the wind piling it against the walls -- deepest on the side it
        // blows away from, where it eddies and drops what it carries, less round the others -- rising to the wall and running
        // out over a couple of metres, uneven along it, round the corners and stopping short of the doors. The ground's own
        // surface, walked through, not over -- but for a building snowed under (FacilityLayout::Outside::drifted): drifted up its
        // walls to its windows and over, out over metres, to be walked up, a way kept down to each of its doors.
        if (m_look.biome == "frozen" || m_look.surface == "sand")
        {
            const float deepest = m_look.biome == "frozen" ? 0.9f : 0.55f;
            const float windAngle = WindAngle(seed);
            const glm::vec2 wind{std::cos(windAngle), std::sin(windAngle)};
            MeshData drifts;
            MeshData buried;
            // What stands on the ground about the buildings, and how far round it reaches: the drifts of one snowed under keep clear.
            std::vector<std::pair<glm::vec2, float>> standing;
            for (const SitePlan::Block& block : m_plan.blocks)
            {
                if (!block.outlying)
                {
                    standing.emplace_back(glm::vec2(block.centre.x, block.centre.z), std::max(block.size.x, block.size.z) * 0.5f);
                }
            }
            for (const SitePlan::Lamp& lamp : m_plan.lamps)
            {
                if (lamp.kind != SitePlan::LampKind::Flood)
                {
                    standing.emplace_back(glm::vec2(lamp.position.x, lamp.position.z), 0.3f);
                }
            }
            for (const SitePlan::Signpost& sign : m_plan.signposts)
            {
                standing.emplace_back(glm::vec2(sign.foot.x, sign.foot.z), 0.6f);
            }
            for (size_t b = 0; b < m_plan.buildings.size(); ++b)
            {
                const FacilityLayout& building = m_plan.buildings[b];
                const bool under = building.outside.drifted[0] > 0.0f;
                MeshData& into = under ? buried : drifts;
                std::vector<glm::vec2> doors;
                for (const FacilityLayout::Exit& exit : building.exits)
                {
                    const glm::vec3 at = FacilityMap::ExitOutside(building, exit, 0.5f);
                    doors.emplace_back(at.x, at.z);
                }
                // How deep at most against a side, and how far out from the wall it runs for a depth; how much of it is left near a
                // door (none at it, a way down to it wider the deeper it is).
                const auto deepAt = [&](int side, glm::vec2 out)
                {
                    return under ? building.outside.drifted[static_cast<size_t>(side)]
                                 : deepest * glm::mix(0.3f, 1.0f, glm::smoothstep(-0.3f, 0.8f, glm::dot(out, wind)));
                };
                const auto reachOf = [&](float h) { return 0.4f + h * (under ? 3.0f : 2.4f); };
                const auto byDoors = [&](glm::vec2 at, float deep)
                {
                    float keep = 1.0f;
                    for (const glm::vec2& door : doors)
                    {
                        keep *= glm::smoothstep(1.4f, 1.4f + std::max(1.2f, deep * 1.2f), glm::distance(door, at));
                    }
                    return keep;
                };
                // And, snowed under, scooped out round anything standing in it -- a lamp's post, a container, a sign -- as the wind
                // scours round a post (and the drift, stood on, never through it).
                const auto byThings = [&](glm::vec2 p)
                {
                    float keep = 1.0f;
                    for (const auto& [at, radius] : standing)
                    {
                        keep *= under ? glm::smoothstep(radius + 1.5f, radius + 3.5f, glm::distance(at, p)) : 1.0f;
                    }
                    return keep;
                };
                // Its outside corners on the ground, and how deep the snow is in each: as deep as the shallower side either side of it,
                // less a little -- the drifts along those sides coming down to it at their ends, and round the corner between them.
                struct Corner
                {
                    glm::vec2 at;
                    glm::vec2 out;
                    float deep;
                };
                std::vector<Corner> corners;
                const glm::vec2 origin{building.origin.x, building.origin.z};
                for (const FacilityLayout::OuterCorner& corner : building.OuterCorners())
                {
                    // (Nor round a corner onto a ruin.)
                    const glm::ivec2 cell = corner.at + glm::ivec2(corner.out.x > 0 ? -1 : 0, corner.out.y > 0 ? -1 : 0);
                    const FacilityLayout::Ruin& ruin = building.ruin;
                    if (corner.low != 0 || ruin.Height(cell.x + corner.out.x, cell.y) > 0 || ruin.Height(cell.x, cell.y + corner.out.y) > 0 ||
                        ruin.Height(cell.x + corner.out.x, cell.y + corner.out.y) > 0)
                    {
                        continue;
                    }
                    const glm::vec2 out{static_cast<float>(corner.out.x), static_cast<float>(corner.out.y)};
                    const float deep = 0.8f * std::min(deepAt(out.x < 0.0f ? 0 : 1, {out.x, 0.0f}), deepAt(out.y < 0.0f ? 2 : 3, {0.0f, out.y}));
                    corners.push_back({origin + glm::vec2(corner.at) * FacilityLayout::kCell, out, deep});
                }
                const auto cornerAt = [&](glm::vec2 at)
                {
                    for (const Corner& corner : corners)
                    {
                        if (glm::distance(corner.at, at) < 0.01f)
                        {
                            return corner.deep;
                        }
                    }
                    return -1.0f;
                };
                constexpr int kAcross = 6;
                // Each stretch of its walls standing on the ground (FacilityLayout::OuterWalls), whatever its shape: where it runs
                // from and to -- round the building the same way every side -- and which way is out from it.
                for (const FacilityLayout::OuterWall& wall : building.OuterWalls())
                {
                    // (Not the wall left standing where a building half fallen in broke: its ruin is past it -- FacilityLayout::Ruin.)
                    const int past = wall.side == 0 || wall.side == 2 ? wall.line - 1 : wall.line;
                    const int mid = (wall.from + wall.to) / 2;
                    if (wall.low != 0 || (wall.side < 2 ? building.ruin.Height(past, mid) : building.ruin.Height(mid, past)) > 0)
                    {
                        continue;
                    }
                    const float line = static_cast<float>(wall.line) * FacilityLayout::kCell;
                    const float a = static_cast<float>(wall.from) * FacilityLayout::kCell;
                    const float c = static_cast<float>(wall.to) * FacilityLayout::kCell;
                    glm::vec2 from;
                    glm::vec2 to;
                    glm::vec2 out;
                    switch (wall.side)
                    {
                    case 0: from = origin + glm::vec2(line, a); to = origin + glm::vec2(line, c); out = {-1.0f, 0.0f}; break;
                    case 1: from = origin + glm::vec2(line, c); to = origin + glm::vec2(line, a); out = {1.0f, 0.0f}; break;
                    case 2: from = origin + glm::vec2(c, line); to = origin + glm::vec2(a, line); out = {0.0f, -1.0f}; break;
                    default: from = origin + glm::vec2(a, line); to = origin + glm::vec2(c, line); out = {0.0f, 1.0f}; break;
                    }
                    const float height = deepAt(wall.side, out);
                    // At each end: the corner's depth where it is an outside one, nothing where it is an inside one.
                    const float ends[2] = {cornerAt(from), cornerAt(to)};
                    const float taper = 2.5f + (under ? height * 0.8f : 0.0f);
                    const float length = glm::distance(from, to);
                    const int along = std::max(static_cast<int>(length / 0.5f), 2);
                    const auto base = static_cast<uint32_t>(into.vertices.size());
                    for (int i = 0; i <= along; ++i)
                    {
                        const float t = static_cast<float>(i) / static_cast<float>(along);
                        const glm::vec2 at = glm::mix(from, to, t) + out * 0.03f;
                        // Uneven along it, smoothly: a value every couple of metres, eased between.
                        const float knot = t * length / 2.0f;
                        const auto k0 = static_cast<uint32_t>(knot);
                        const float f = glm::smoothstep(0.0f, 1.0f, knot - static_cast<float>(k0));
                        const float uneven = glm::mix(Hash01(seed, static_cast<uint32_t>(b * 64u + static_cast<size_t>(wall.side) * 16u + static_cast<size_t>(wall.line)), k0),
                                                      Hash01(seed, static_cast<uint32_t>(b * 64u + static_cast<size_t>(wall.side) * 16u + static_cast<size_t>(wall.line)), k0 + 1u), f);
                        float h = height * (0.6f + 0.4f * uneven);
                        const float end = t < 0.5f ? ends[0] : ends[1];
                        h = glm::mix(std::max(end, 0.0f), h, glm::smoothstep(0.0f, taper, std::min(t, 1.0f - t) * length));
                        h *= byDoors(at, height);
                        const float reach = reachOf(h);
                        for (int k = 0; k <= kAcross; ++k)
                        {
                            const float u = static_cast<float>(k) / static_cast<float>(kAcross);
                            const glm::vec2 p = at + out * (reach * u);
                            MeshVertex vertex;
                            // Highest at the wall, falling away in a curve to nothing.
                            vertex.position = {p.x, m_terrain.Height(p.x, p.y) + h * std::pow(1.0f - u, 1.6f) * byThings(p) - 0.02f, p.y};
                            vertex.uv = p;
                            into.vertices.push_back(vertex);
                        }
                    }
                    for (int i = 0; i < along; ++i)
                    {
                        for (int k = 0; k < kAcross; ++k)
                        {
                            const auto a0 = base + static_cast<uint32_t>(i * (kAcross + 1) + k);
                            const auto c0 = a0 + static_cast<uint32_t>(kAcross + 1);
                            // Wound to face up (sides round the building one way, `out` turned from `along` the same way each).
                            into.indices.insert(into.indices.end(), {a0, a0 + 1, c0, a0 + 1, c0 + 1, c0});
                        }
                    }
                }
                // Round each outside corner, from the drift along one side to the one along the other: a quarter round, as deep as
                // they are where they meet it.
                // (Each way round from a hair off the corner along it, as the drift along a side starts a hair off its wall: the first and
                // the last the same points as that drift's end, so no seam shows between them -- a corner set off diagonally left a
                // sliver of ground open along each, dark lines out from the corner.)
                for (const Corner& corner : corners)
                {
                    constexpr int kArc = 6;
                    const auto base = static_cast<uint32_t>(into.vertices.size());
                    // (From the side along x's way out round to the side along z's, or the other way, so it faces up either way.)
                    const bool turn = corner.out.x * corner.out.y > 0.0f;
                    for (int i = 0; i <= kArc; ++i)
                    {
                        const float angle = glm::half_pi<float>() * static_cast<float>(turn ? kArc - i : i) / static_cast<float>(kArc);
                        const glm::vec2 dir{corner.out.x * std::cos(angle), corner.out.y * std::sin(angle)};
                        const glm::vec2 at = corner.at + dir * 0.03f;
                        const float h = corner.deep * byDoors(at, corner.deep);
                        const float reach = reachOf(h);
                        for (int k = 0; k <= kAcross; ++k)
                        {
                            const float u = static_cast<float>(k) / static_cast<float>(kAcross);
                            const glm::vec2 p = at + dir * (reach * u);
                            MeshVertex vertex;
                            vertex.position = {p.x, m_terrain.Height(p.x, p.y) + h * std::pow(1.0f - u, 1.6f) * byThings(p) - 0.02f, p.y};
                            vertex.uv = p;
                            into.vertices.push_back(vertex);
                        }
                    }
                    for (int i = 0; i < kArc; ++i)
                    {
                        for (int k = 0; k < kAcross; ++k)
                        {
                            const auto a0 = base + static_cast<uint32_t>(i * (kAcross + 1) + k);
                            const auto c0 = a0 + static_cast<uint32_t>(kAcross + 1);
                            into.indices.insert(into.indices.end(), {a0, a0 + 1, c0, a0 + 1, c0 + 1, c0});
                        }
                    }
                }
            }
            // Lit by its own shape: each corner's normal from the faces round it.
            for (MeshData* mesh : {&drifts, &buried})
            {
                std::vector<glm::vec3> normals(mesh->vertices.size(), glm::vec3(0.0f));
                for (size_t i = 0; i + 2 < mesh->indices.size(); i += 3)
                {
                    const uint32_t a = mesh->indices[i];
                    const uint32_t b = mesh->indices[i + 1];
                    const uint32_t c = mesh->indices[i + 2];
                    const glm::vec3 n = glm::cross(mesh->vertices[b].position - mesh->vertices[a].position, mesh->vertices[c].position - mesh->vertices[a].position);
                    normals[a] += n;
                    normals[b] += n;
                    normals[c] += n;
                }
                for (size_t i = 0; i < normals.size(); ++i)
                {
                    mesh->vertices[i].normal = glm::length(normals[i]) > 1.0e-9f ? glm::normalize(normals[i]) : glm::vec3(0.0f, 1.0f, 0.0f);
                }
            }
            if (!drifts.vertices.empty())
            {
                builder.AddMesh("site_drifts", Transform{}, drifts, m_groundMaterial, false);
            }
            // (A building snowed under: its drifts stood on, walked up.)
            if (!buried.vertices.empty())
            {
                builder.AddMesh("site_drifts", Transform{}, buried, m_groundMaterial, true);
            }
            // And on a frozen world the snow on their roofs (RoofSnow): deeper on one snowed under.
            if (m_look.biome == "frozen")
            {
                MeshData roofs;
                for (size_t b = 0; b < m_plan.buildings.size(); ++b)
                {
                    const FacilityLayout& building = m_plan.buildings[b];
                    const MeshData snow = RoofSnow(building, building.outside.drifted[0] > 0.0f ? 0.45f : 0.15f, Mix(seed, 0x5A0Fu + static_cast<uint32_t>(b)));
                    const auto base = static_cast<uint32_t>(roofs.vertices.size());
                    roofs.vertices.insert(roofs.vertices.end(), snow.vertices.begin(), snow.vertices.end());
                    for (const uint32_t index : snow.indices)
                    {
                        roofs.indices.push_back(base + index);
                    }
                }
                if (!roofs.indices.empty())
                {
                    builder.AddMesh("site_roof_snow", Transform{}, roofs, m_groundMaterial, false);
                }
            }
        }
        builder.SetShadows(true);
        mark("drifts");
        // And the grass all over where the world grows it (UpdateCover): how thick, by the world, and the tufts it is made of.
        m_coverDensity = m_look.biome == "jungle" ? 6.5f : m_look.biome == "temperate" ? 5.2f : m_look.biome == "oceanic" ? 3.8f : 0.0f;
        m_tufts.clear();
        for (int variant = 0; variant < SiteScatter::kVariants; ++variant)
        {
            // A shade nearer the ground's own colour than the clumps are, so the ground reads as grown over, not dotted.
            m_tufts.push_back(SiteScatter::Leaves(SiteScatter::Kind::Grass, variant, seed, grassColour));
        }
        m_coverMaterial = leaves;
        m_coverMaterial.cover = true;
        // Its colour on the material, the vertices' being wanted for other things (Material::cover); how much lighter or darker
        // each card was made than it kept in them still.
        m_coverMaterial.baseColor = grassColour * 2.4f;
        m_coverAt = glm::vec2(-1.0e9f);
        PRED_LOG_INFO(Gameplay, "Site scatter: {} things on the ground ({})", scattered.size(), m_look.biome);
    }

    builder.SetStructure(0);

    mark("scatter");
    // The shuttle on the pad, standing on it and part of the site's structure, a lamp lit in its cabin.
    if (!groundOnly)
    {
        m_shuttle.Build(scene, meshes, &physics, Vehicles::Load("shuttle"), ShuttleHome(), ground, "site_shuttle_");
        m_shuttleLamp = -1;
        CinePose cabinLamp;
        if (lights != nullptr && m_shuttle.Socket("lamp", cabinLamp))
        {
            const int lamp = ShipSpec::AddShuttleLamp(*lights, cabinLamp.position);
            // Kept to the cabin by its box, not a shadow, which let some of it through the roof (see ShipMap).
            lights->Place(scene, lamp, cabinLamp.position, glm::vec3(0.0f, -1.0f, 0.0f));
            glm::vec3 low;
            glm::vec3 high;
            ShipSpec::ShuttleCabinBounds(cabinLamp, low, high);
            lights->Bound(lamp, low, high);
            m_shuttleLamp = lamp;
        }
    }

    mark("shuttle");
    // The lamps outside, as lamps are: on poles over the ground -- a concrete plinth, a round pole narrowing up it, an arm
    // out from its top and a drop to the lamp's head; over the doors, the same head on a bracket from the wall; and at the
    // pad's edges, a short post with its red light sat on top. Each head a dark housing with its lens under it, the lens the
    // light's own fitting (LightKind::Area) and the only part that glows, turned the way it shines and everything else built
    // to meet it there.
    const Material housingMaterial = Surfaces::ApplyWorn(Material::Diffuse({0.16f, 0.17f, 0.18f}, 0.5f), "painted_metal", 1.0f, 0.3f);
    const auto between = [&](const std::string& name, glm::vec3 from, glm::vec3 to, float radius, const Material& material)
    {
        // A rod from one point to another: a cylinder turned onto the line between them.
        const glm::vec3 run = to - from;
        Transform rod;
        rod.position = (from + to) * 0.5f;
        rod.rotation = glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), glm::normalize(run));
        builder.AddMesh(name, rod, Primitives::Cylinder(radius, glm::length(run), 12), material);
    };
    // A lamp's head round its lens at `at`: turned as the fitting is (its -y the way it shines); where its top is, to hang it from.
    const auto head = [&](const glm::vec3& at, const glm::vec3& direction)
    {
        const glm::vec3 down{0.0f, -1.0f, 0.0f};
        const glm::vec3 shine = glm::normalize(direction);
        const glm::quat turn = glm::dot(down, shine) > 0.999f ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f) : glm::rotation(down, shine);
        Transform housing;
        housing.rotation = turn;
        housing.position = at + turn * glm::vec3(0.0f, 0.085f, 0.0f);
        builder.AddMesh("site_lamp_head", housing, Primitives::Box({0.56f, 0.15f, 0.32f}), housingMaterial, false);
        // A fin along its back, as a lamp's cooling has.
        housing.position = at + turn * glm::vec3(0.0f, 0.18f, 0.0f);
        builder.AddMesh("site_lamp_head", housing, Primitives::Box({0.4f, 0.05f, 0.12f}), housingMaterial, false);
        return at + turn * glm::vec3(0.0f, 0.2f, 0.0f);
    };
    // The lamp over a way out that goes through an airlock (FacilityLayout::Outside::airlocks): moved out onto the airlock's far wall,
    // over its door, as it stood over the building's -- planned where the airlock now is.
    for (SitePlan::Lamp& lamp : m_plan.lamps)
    {
        if (lamp.kind != SitePlan::LampKind::Flood)
        {
            continue;
        }
        for (const FacilityLayout& building : m_plan.buildings)
        {
            if (!building.outside.airlocks)
            {
                continue;
            }
            for (const FacilityLayout::Exit& exit : building.exits)
            {
                const glm::vec3 door = FacilityMap::ExitOutside(building, exit, 0.0f);
                const glm::vec2 out = exit.side == 0 ? glm::vec2(-1.0f, 0.0f) : exit.side == 1 ? glm::vec2(1.0f, 0.0f)
                                    : exit.side == 2 ? glm::vec2(0.0f, -1.0f) : glm::vec2(0.0f, 1.0f);
                if (glm::distance(glm::vec2(lamp.position.x, lamp.position.z), glm::vec2(door.x, door.z) + out * 0.75f) < 1.0f)
                {
                    // (On the edge of its roof, over the AIRLOCK lettered over its door.)
                    const float past = FacilitySpec::kAirlockDepth + FacilitySpec::kAirlockWall;
                    const glm::vec2 at = glm::vec2(door.x, door.z) + out * (0.75f + past);
                    lamp.position = glm::vec3(at.x, lamp.position.y, at.y);
                    lamp.position.y = building.GroundLevel() + FacilitySpec::kAirlockHeight + (exit.bay ? 0.4f : 0.0f) + 0.12f;
                }
            }
        }
    }
    for (size_t i = 0; i < m_plan.lamps.size(); ++i)
    {
        const SitePlan::Lamp& lamp = m_plan.lamps[i];
        const float raise = lift({lamp.position.x, lamp.position.z}, lamp.kind == SitePlan::LampKind::Beacon ? 0.3f : 0.5f);
        const glm::vec3 at = lamp.position + glm::vec3(0.0f, lamp.kind == SitePlan::LampKind::Flood ? 0.0f : raise, 0.0f);
        glm::vec3 back{-lamp.direction.x, 0.0f, -lamp.direction.z};
        back = glm::length(back) > 0.05f ? glm::normalize(back) : glm::vec3(1.0f, 0.0f, 0.0f);
        builder.SetStructure(physics.NewOverlapGroup());
        if (lamp.kind == SitePlan::LampKind::Pole)
        {
            // Set back from over where it shines, holding its head out on the arm.
            const glm::vec3 foot = glm::vec3(lamp.position.x, m_plan.origin.y + raise, lamp.position.z) + back * 0.9f;
            const glm::vec3 hang = head(at, lamp.direction);
            const float top = hang.y + 0.35f;
            Transform plinth;
            plinth.position = foot + glm::vec3(0.0f, 0.1f, 0.0f);
            builder.AddMesh("site_pole_plinth", plinth, Primitives::Cylinder(0.26f, 0.5f, 16), padMaterial);
            Transform pole;
            pole.position = glm::vec3(foot.x, (foot.y + 0.35f + top) * 0.5f, foot.z);
            builder.AddMesh("site_pole", pole, Primitives::Frustum(0.09f, 0.06f, top - foot.y - 0.35f, 12), poleMaterial);
            // The arm out level from just under its top to over the head, and the drop down to it.
            const glm::vec3 elbow{hang.x, top - 0.08f, hang.z};
            between("site_pole_arm", glm::vec3(foot.x, top - 0.08f, foot.z) + back * 0.05f, elbow + back * 0.04f, 0.045f, poleMaterial);
            between("site_pole_arm", elbow + glm::vec3(0.0f, 0.04f, 0.0f), hang - glm::vec3(0.0f, 0.02f, 0.0f), 0.04f, poleMaterial);
        }
        else if (lamp.kind == SitePlan::LampKind::Flood)
        {
            // Over a door: on a bracket out from the wall behind it, a plate on the cladding where it is fixed.
            const glm::vec3 hang = head(at, lamp.direction);
            const float toWall = 0.75f - (FacilitySpec::kShellThickness + FacilitySpec::kCladding - FacilitySpec::kHalfWall) - glm::dot(hang - at, back);
            const glm::vec3 wall = hang + back * toWall;
            between("site_lamp_bracket", hang - glm::vec3(0.0f, 0.02f, 0.0f) + glm::vec3(0.0f, 0.06f, 0.0f), wall + glm::vec3(0.0f, 0.04f, 0.0f), 0.035f, poleMaterial);
            Transform plate;
            plate.position = wall + glm::vec3(0.0f, 0.04f, 0.0f) + back * 0.005f;
            plate.rotation = glm::angleAxis(std::atan2(back.x, back.z), glm::vec3(0.0f, 1.0f, 0.0f));
            builder.AddMesh("site_lamp_bracket", plate, Primitives::Box({0.22f, 0.3f, 0.04f}), poleMaterial, false);
        }
        else if (lamp.kind == SitePlan::LampKind::Mast)
        {
            // The way in's beacon: a steel mast on a concrete footing sunk in the ground, narrowing up to its light.
            const glm::vec3 foot{lamp.position.x, m_plan.origin.y + raise, lamp.position.z};
            Transform footing;
            footing.position = foot + glm::vec3(0.0f, 0.05f, 0.0f);
            builder.AddMesh("site_mast_footing", footing, Primitives::Box({0.9f, 0.6f, 0.9f}), padMaterial);
            Transform mast;
            mast.position = glm::vec3(foot.x, (foot.y + 0.35f + at.y - 0.08f) * 0.5f, foot.z);
            builder.AddMesh("site_mast", mast, Primitives::Frustum(0.12f, 0.05f, at.y - 0.08f - foot.y - 0.35f, 12), poleMaterial);
        }
        else
        {
            // A beacon: a short round post on a base plate, its red light sat on the top of it (SitePlan puts it there).
            const glm::vec3 foot{lamp.position.x, m_plan.origin.y + raise, lamp.position.z};
            Transform base;
            base.position = foot + glm::vec3(0.0f, 0.03f, 0.0f);
            builder.AddMesh("site_beacon_post", base, Primitives::Cylinder(0.16f, 0.06f, 14), poleMaterial);
            Transform post;
            post.position = foot + glm::vec3(0.0f, 0.06f + 0.42f, 0.0f);
            builder.AddMesh("site_beacon_post", post, Primitives::Cylinder(0.06f, 0.84f, 12), poleMaterial);
        }
        builder.SetStructure(0);
        if (lights != nullptr)
        {
            if (lamp.kind == SitePlan::LampKind::Beacon || lamp.kind == SitePlan::LampKind::Mast)
            {
                // (Red, pulsing slowly, on its own: no circuit to lose.)
                lights->Add(scene, meshes, LightKind::Emergency, LightMood::Pulse, at, glm::vec3(0.0f, 1.0f, 0.0f), 0, Mix(seed, 0xBEAC0u + static_cast<uint32_t>(i)) | 1u, lamp.range);
                continue;
            }
            lights->Add(scene, meshes, LightKind::Area, MoodOf(lamp.mood), at, lamp.direction, 0, Mix(seed, 0x1A3Bu + static_cast<uint32_t>(i)) | 1u,
                        lamp.range);
        }
    }
    // The way in, where it is marked by signposts: each a post with a board on it saying where the way goes, readable either
    // side; and where it is a path, a worn strip of the ground's own colour from the pad to the door, laid over it.
    {
        const Material boardMaterial = Surfaces::ApplyWorn(Material::Diffuse({0.07f, 0.22f, 0.14f}, 0.6f), "painted_metal", 1.5f, 0.3f);
        const Material letterMaterial = Material::Diffuse({0.9f, 0.9f, 0.85f}, 0.6f);
        for (const SitePlan::Signpost& sign : m_plan.signposts)
        {
            const float raise = lift({sign.foot.x, sign.foot.z}, 0.15f);
            const glm::vec3 foot{sign.foot.x, m_plan.origin.y + raise, sign.foot.z};
            const glm::quat turn = glm::angleAxis(sign.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
            builder.SetStructure(physics.NewOverlapGroup());
            Transform post;
            post.position = foot + glm::vec3(0.0f, 0.95f, 0.0f);
            post.rotation = turn;
            builder.AddBox("site_signpost", post, {0.08f, 2.3f, 0.08f}, poleMaterial);
            builder.SetStructure(0);
            Transform board;
            board.position = foot + glm::vec3(0.0f, 1.8f, 0.0f) + turn * glm::vec3(0.0f, 0.0f, -0.06f);
            board.rotation = turn;
            builder.AddMesh("site_signboard", board, Primitives::Box({1.3f, 0.34f, 0.04f}), boardMaterial, false);
            for (const float side : {-1.0f, 1.0f})
            {
                Transform words;
                words.position = board.position + turn * glm::vec3(0.0f, 0.0f, side * 0.0215f);
                words.rotation = side < 0.0f ? turn : turn * glm::angleAxis(glm::pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
                builder.AddMesh("site_signboard_words", words, Lettering::Mesh(sign.text, 0.16f), letterMaterial, false);
            }
        }
    }
    physics.OptimizeBroadPhase();
    // The windsock by the pad: its pole, and the sock -- four bands, orange and white, narrowing -- which the game turns and
    // fills with the wind (PoseWindsock).
    m_windsock = Entity{};
    if (glm::length(m_plan.windsock) > 0.0f)
    {
        const float raise = lift({m_plan.windsock.x, m_plan.windsock.z}, 0.3f);
        const glm::vec3 foot{m_plan.windsock.x, m_plan.origin.y + raise, m_plan.windsock.z};
        Transform pole;
        pole.position = foot + glm::vec3(0.0f, 2.75f, 0.0f);
        builder.SetStructure(physics.NewOverlapGroup());
        builder.AddBox("site_windsock_pole", pole, {0.1f, 5.5f, 0.1f}, poleMaterial);
        builder.SetStructure(0);
        MeshData sock;
        constexpr int kBands = 4;
        for (int band = 0; band < kBands; ++band)
        {
            const float t0 = static_cast<float>(band) / kBands;
            const float t1 = static_cast<float>(band + 1) / kBands;
            MeshData piece = Primitives::Frustum(glm::mix(0.32f, 0.12f, t0), glm::mix(0.32f, 0.12f, t1), 2.0f / kBands, 12);
            const uint32_t colour = PaintVertex(band % 2 == 0 ? glm::vec3(0.95f, 0.38f, 0.08f) : glm::vec3(0.92f));
            for (MeshVertex& vertex : piece.vertices)
            {
                vertex.position.y += -1.0f + (static_cast<float>(band) + 0.5f) * (2.0f / kBands);
                vertex.color = colour;
            }
            sock.Append(piece, glm::mat4(1.0f));
        }
        const MeshHandle mesh = meshes.Upload(sock, m_tag + "_windsock");
        m_meshHandles.push_back(mesh);
        m_windsock = scene.CreateMeshEntity("site_windsock", Transform{}, mesh, Material::Diffuse(glm::vec3(1.0f), 0.8f));
        m_entities.push_back(m_windsock);
        m_windsockPivot = foot + glm::vec3(0.0f, 5.35f, 0.0f);
    }
    // What stands out on the land round it (an outpost's): each stood on the lowest of the ground under it -- not levelled for it
    // -- all of it drawn as one, solid part by part.
    if (!m_plan.outlying.empty())
    {
        ModelAsset solid;
        solid.name = "site_outlying";
        ModelAsset dressing;
        dressing.name = "site_outlying_dressing";
        for (const SitePlan::Outlying& piece : m_plan.outlying)
        {
            const glm::vec3 foot{piece.at.x, m_plan.origin.y + lift(piece.at, piece.reach), piece.at.y};
            for (ModelPart part : piece.solid)
            {
                part.position += foot;
                solid.parts.push_back(std::move(part));
            }
            for (ModelPart part : piece.dressing)
            {
                part.position += foot;
                dressing.parts.push_back(std::move(part));
            }
        }
        const CinePose home{glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)};
        m_outlying.SetMerged(true);
        m_outlyingDressing.SetMerged(true);
        m_outlying.Build(scene, meshes, &physics, std::make_shared<ModelAsset>(std::move(solid)), home, ground, m_tag + "_outlying_");
        m_outlyingDressing.Build(scene, meshes, nullptr, std::make_shared<ModelAsset>(std::move(dressing)), home, 0, m_tag + "_outlying_dressing_");
    }
    mark("lamps");

    m_built = true;
    PRED_LOG_INFO(Gameplay, "Site {}: {} buildings, {} pieces outside, {} lamps outside, {} doors in all", seed,
                  m_plan.buildings.size(), m_plan.blocks.size(), m_plan.lamps.size(), m_placements.doors.size());
    PRED_LOG_INFO(Gameplay, "Site {} made, in ms:{}", seed, timings);
}

CinePose SiteMap::ShuttleHome() const
{
    // Its nose away from the site, so its back and its ramp face into it.
    return {m_plan.ShuttleBase(), TurnFromDegrees({0.0f, glm::degrees(m_plan.landingYaw) + 180.0f, 0.0f})};
}

void SiteMap::Clear(Scene& scene, PhysicsWorld& physics, LevelLights* lights)
{
    // The grass being made on another thread reads the ground: done with first.
    if (m_coverBuild.valid())
    {
        m_coverBuild.wait();
        m_coverBuild = {};
    }
    m_shuttle.Clear(scene, &physics);
    m_shuttleLamp = -1;
    m_outlying.Clear(scene, &physics);
    m_outlyingDressing.Clear(scene, nullptr);
    if (m_cover.IsValid())
    {
        scene.Destroy(m_cover);
        m_cover = Entity{};
    }
    m_coverDensity = 0.0f;
    for (const Entity entity : m_entities)
    {
        scene.Destroy(entity);
    }
    if (m_meshLibrary != nullptr)
    {
        for (const MeshHandle mesh : m_meshHandles)
        {
            m_meshLibrary->Release(mesh);
        }
    }
    m_meshHandles.clear();
    for (const BodyHandle body : m_bodies)
    {
        physics.DestroyBody(body);
    }
    m_entities.clear();
    m_bodies.clear();
    // The buildings' own, without their lights: the site takes every light it added away at once.
    for (auto it = m_buildings.rbegin(); it != m_buildings.rend(); ++it)
    {
        (*it)->Clear(scene, physics, nullptr);
    }
    m_buildings.clear();
    m_ladders.clear();
    m_floods.clear();
    m_pools.clear();
    m_lockouts.clear();
    m_airlocks.clear();
    m_seaPosts.clear();
    m_tunnelFloods.clear();
    m_stilts.clear();
    m_indoorWater = MeshHandle{};
    m_lifts.clear();
    if (lights != nullptr && m_hasLights)
    {
        lights->RemoveFrom(scene, m_firstLight);
    }
    m_hasLights = false;
    m_placements = {};
    m_built = false;
}

void SiteMap::Bounds(glm::vec3& min, glm::vec3& max) const
{
    // The ground and the rock reach some way out past the open ground (SiteTerrain::kMargin), down into hollows and up.
    constexpr float kRock = SiteTerrain::kMargin;
    min = m_plan.origin - glm::vec3(kRock, 20.0f, kRock);
    max = m_plan.origin + glm::vec3(m_plan.size + kRock, 150.0f, m_plan.size + kRock);
}

bool SiteMap::InReach(const glm::vec3& at) const
{
    constexpr float kFoot = 14.0f;
    const glm::vec3 local = at - m_plan.origin;
    return local.x > -kFoot && local.z > -kFoot && local.x < m_plan.size + kFoot && local.z < m_plan.size + kFoot;
}

void SiteMap::ShowFarLand(Scene& scene, const glm::vec3& eye)
{
    // (And its rocks out there: left on, from the ship kilometres off they were a line of flickering specks across the
    // picture, seen edge on.)
    const glm::vec2 middle{m_plan.origin.x + m_plan.size * 0.5f, m_plan.origin.z + m_plan.size * 0.5f};
    const bool over = m_built && glm::distance(glm::vec2(eye.x, eye.z), middle) < 2000.0f;
    for (const Entity far : {m_farLand, m_farTrees, m_farScatter})
    {
        if (MeshRenderer* renderer = scene.GetMeshRenderer(far))
        {
            renderer->visible = over;
        }
    }
}

void SiteMap::PoseWindsock(Scene& scene, float angle, float strength, float seconds)
{
    Transform* placed = m_windsock.IsValid() ? scene.GetTransform(m_windsock) : nullptr;
    if (placed == nullptr)
    {
        return;
    }
    // Out downwind, drooping the less there is of it -- hanging nearly straight down in a calm, straight out in a stiff
    // breeze -- and twitching as it gusts.
    const float filled = std::clamp(strength, 0.0f, 1.0f);
    const float turn = angle + 0.08f * filled * std::sin(seconds * 5.3f) + 0.05f * std::sin(seconds * 1.7f);
    const float droop = (1.0f - filled) * 1.35f + 0.04f * filled * std::sin(seconds * 7.1f);
    const glm::vec3 way{std::cos(turn) * std::cos(droop), -std::sin(droop), std::sin(turn) * std::cos(droop)};
    placed->rotation = glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), way);
    // Its narrow end (the frustum's +y) out along the way it points, its wide end at the pole.
    placed->position = m_windsockPivot + way * 1.0f;
}

glm::vec3 SiteMap::WithinRoam(const glm::vec3& at) const
{
    // Back inside where anybody may go (SiteTerrain::PastRoam), straight back across its edge; just as it was when inside.
    glm::vec2 p{at.x, at.z};
    for (int step = 0; step < 4; ++step)
    {
        const float past = m_terrain.PastRoam(p.x, p.y);
        if (past <= 0.0f)
        {
            break;
        }
        constexpr float e = 0.25f;
        const glm::vec2 slope{m_terrain.PastRoam(p.x + e, p.y) - m_terrain.PastRoam(p.x - e, p.y),
                              m_terrain.PastRoam(p.x, p.y + e) - m_terrain.PastRoam(p.x, p.y - e)};
        if (glm::length(slope) < 1.0e-5f)
        {
            break;
        }
        p -= glm::normalize(slope) * (past + 0.01f);
    }
    return {p.x, at.y, p.y};
}

bool SiteMap::Contains(const glm::vec3& at) const
{
    glm::vec3 min;
    glm::vec3 max;
    Bounds(min, max);
    return at.x >= min.x && at.y >= min.y && at.z >= min.z && at.x <= max.x && at.y <= max.y && at.z <= max.z;
}

void SiteMap::UpdateCover(Scene& scene, MeshLibrary& meshes, const glm::vec3& eye, float detail)
{
    if (!m_built || m_coverDensity <= 0.0f || m_tufts.empty())
    {
        return;
    }
    // Made on another thread, so walking on never stops for it: drawn when it is ready.
    if (m_coverBuild.valid() && m_coverBuild.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        const MeshData mesh = m_coverBuild.get();
        // None at all (the setting at nought, or bare ground all round): what was drawn put out of sight.
        if (mesh.vertices.empty())
        {
            if (MeshRenderer* renderer = scene.GetMeshRenderer(m_cover))
            {
                renderer->visible = false;
            }
        }
        else
        {
            m_coverMesh = m_coverMesh.IsValid() ? meshes.Replace(m_coverMesh, mesh) : meshes.Upload(mesh, m_tag + "_grass_cover");
            if (!m_cover.IsValid())
            {
                m_cover = scene.CreateMeshEntity("site_grass_cover", Transform{}, m_coverMesh, m_coverMaterial);
            }
            if (MeshRenderer* renderer = scene.GetMeshRenderer(m_cover))
            {
                renderer->castsShadow = false;
                renderer->blocksSky = false;
                renderer->visible = true;
            }
        }
    }
    // Made again only when the eye is some metres from where it was last made (and not while one is being made). (By the
    // distance, not by which square of ground the eye is on: standing on the line between two, the eye's own little
    // movements took it back and forth across it, and the grass was made again every few frames.)
    constexpr float kStep = 5.0f;
    const glm::vec2 here{eye.x, eye.z};
    if ((glm::distance(here, m_coverAt) < kStep && detail == m_coverDetail) || m_coverBuild.valid())
    {
        return;
    }
    m_coverAt = here;
    m_coverDetail = detail;
    m_coverBuild = std::async(std::launch::async, [this, eye, detail] { return CoverAround(eye, detail); });
}

MeshData SiteMap::CoverAround(const glm::vec3& eye, float detail) const
{
    // Square metres of ground in patches of two, out to a hundred and twenty metres (thinning as the square of the distance past
    // thirty, so where they come in is far enough off not to be seen coming). Each patch's tufts are where its own numbers put
    // them, the same every time; how many of them show falls with how far off the patch is -- each coming up from nothing as
    // the eye nears, not all at once.
    constexpr float kPatch = 2.0f;
    constexpr float kReach = 128.0f;
    const auto hash = [](int x, int z, uint32_t n)
    {
        uint32_t h = static_cast<uint32_t>(x) * 0x8DA6B343u ^ static_cast<uint32_t>(z) * 0xD8163841u ^ n * 0xCB1AB31Fu;
        h ^= h >> 13;
        h *= 0x5BD1E995u;
        h ^= h >> 15;
        return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0x1000000u);
    };
    MeshData mesh;
    const int reach = static_cast<int>(kReach / kPatch);
    const int cx = static_cast<int>(std::floor(eye.x / kPatch));
    const int cz = static_cast<int>(std::floor(eye.z / kPatch));
    const float density = m_coverDensity * detail;
    const int most = static_cast<int>(std::ceil(density * kPatch * kPatch)) + 1;
    for (int pz = cz - reach; pz <= cz + reach; ++pz)
    {
        for (int px = cx - reach; px <= cx + reach; ++px)
        {
            const glm::vec2 middle{(static_cast<float>(px) + 0.5f) * kPatch, (static_cast<float>(pz) + 0.5f) * kPatch};
            const float distance = glm::length(middle - glm::vec2(eye.x, eye.z));
            if (distance > kReach)
            {
                continue;
            }
            // How many of its tufts could be up by the time this is made again -- the eye some metres nearer by then -- as
            // the shader has them (vs_mesh, Material::cover): which of those are, and how far grown, it works out each frame.
            const float near = std::max(distance - 9.0f, 0.0f);
            const float share = near > 122.0f ? 0.0f : std::clamp(900.0f / std::max(near * near, 1.0f), 0.0f, 1.0f);
            const float all = density * kPatch * kPatch;
            for (int i = 0; i < most; ++i)
            {
                // Which go first: the last of a patch's, the first of them last.
                const float rank = (static_cast<float>(i) + 0.5f) / std::max(all, 1.0f);
                if (rank >= share || static_cast<float>(i) >= all)
                {
                    break;
                }
                const float x = (static_cast<float>(px) + hash(px, pz, static_cast<uint32_t>(i * 4))) * kPatch;
                const float z = (static_cast<float>(pz) + hash(px, pz, static_cast<uint32_t>(i * 4 + 1))) * kPatch;
                // On the site, on ground it could grow on (not too steep), thinning out towards what was levelled for buildings,
                // pads and the ways between them, and none right up against them.
                if (m_terrain.WaterDepth(x, z) > -0.1f || m_terrain.BedAt(x, z) > 0.4f)
                {
                    continue; // (none growing in the water)
                }
                const float wild = m_terrain.Wildness(x, z);
                const float cut = 0.15f + 0.45f * hash(px, pz, static_cast<uint32_t>(i * 4 + 900));
                // (Nor on a trail worn bare.)
                if (!m_terrain.OnField(x, z) || wild < cut || m_terrain.Normal(x, z).y < 0.85f || m_terrain.Worn(x, z) > cut + 0.25f)
                {
                    continue;
                }
                // Thinner deep in the woods, where little light reaches the ground (SiteScatter::Woodland); none on a second
                // ground that is not grass -- a beach, bare earth (Surfaces::BlendAt).
                if (hash(px, pz, static_cast<uint32_t>(i * 4 + 901)) < SiteScatter::Woodland(m_seed, m_look.biome, x, z) * 0.6f)
                {
                    continue;
                }
                if (!m_look.blend.empty() && Surfaces::StepName(Surfaces::StepOfSet(m_look.blend, false)) != std::string_view("grass") &&
                    Surfaces::BlendAt(x, z, m_terrain.Height(x, z), m_look.blendShare, m_terrain.WaterLevel(),
                                      m_terrain.HasWater() ? m_look.blendShore : 0.0f))
                {
                    continue;
                }
                // Shorter towards where it stops, so it thins out rather than ending in a line.
                // Wider the further off, and a little taller, standing in for the ones left out there: fewer, but still covering
                // the ground (the share left falls as the square of the distance; this makes up some of it).
                const float spread = std::clamp(std::sqrt(std::max(distance, 30.0f) / 30.0f), 1.0f, 2.0f);
                const float rise = std::sqrt(spread);
                const float size = (0.3f + 0.35f * hash(px, pz, static_cast<uint32_t>(i * 4 + 2))) * glm::mix(0.4f, 1.0f, glm::smoothstep(cut, cut + 0.3f, wild));
                const float yaw = hash(px, pz, static_cast<uint32_t>(i * 4 + 3)) * 6.2831853f;
                // (None standing into a building's wall: clear of every one by as far as it reaches out.)
                if (m_plan.Indoors(glm::vec3(x, m_terrain.Height(x, z) + 0.3f, z), size * 1.15f * spread * 0.5f + 0.1f))
                {
                    continue;
                }
                const MeshData& tuft = m_tufts[static_cast<size_t>(px * 7 + pz * 13 + i) % m_tufts.size()];
                const glm::quat turn = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
                const glm::vec3 foot{x, m_terrain.Height(x, z) - 0.04f, z};
                const auto base = static_cast<uint32_t>(mesh.vertices.size());
                const auto channel = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
                for (MeshVertex vertex : tuft.vertices)
                {
                    const float over = std::max(vertex.position.y * size * 0.9f * rise + 0.04f, 0.0f);
                    vertex.position = foot + turn * (vertex.position * glm::vec3(size * 1.15f * spread, size * 0.9f * rise, size * 1.15f * spread));
                    // Lit mostly as the ground under it is, so the grass reads as the ground grown up rather than cards on it.
                    vertex.normal = glm::normalize(glm::mix(turn * vertex.normal, glm::vec3(0.0f, 1.0f, 0.0f), 0.5f));
                    const float shade = static_cast<float>((vertex.color >> 8) & 0xFFu) / 255.0f / std::max(m_coverMaterial.baseColor.g, 0.01f);
                    vertex.color = channel(rank) | (channel(over / 1.5f) << 8) | (channel((shade - 0.6f) / 0.8f) << 16) | (255u << 24);
                    mesh.vertices.push_back(vertex);
                }
                for (const uint32_t index : tuft.indices)
                {
                    mesh.indices.push_back(base + index);
                }
            }
        }
    }
    return mesh;
}

float SiteMap::WindAngle(uint32_t seed)
{
    return static_cast<float>((seed * 2654435761u) >> 20) / 4096.0f * 6.2831853f;
}

std::vector<float> SiteMap::FloodTops(uint32_t seed, bool cold) const
{
    std::vector<float> tops(m_plan.buildings.size(), -1.0e9f);
    // Flooded, now and then, where it was left and there is water about that does not freeze (nor where there is no air) -- under the
    // ground: only a building with basements, the water standing in them and never up on the ground floor, where its doors would let
    // it out. One surface all through it, as water finds: in the lowest basement only, at the first step, round the ankles, the
    // knees, the waist, the chest, or up to its ceiling; or over that, every basement under it full and the one it stands in so deep.
    // Or as a test asked (Look::flood): the first building with a basement, whatever else -- its water so high, if it said.
    // On the sea's floor (you, 2026-11-03: air inside, some parts flooded), a building that has lost its air: the sea come up through its
    // moon pool and standing over its lowest floor -- the pool's water at that level -- its floors over that dry. Half of those left,
    // a few of those kept.
    const bool watery = m_look.liquid == Look::Liquid::Water && !cold && !m_look.airless && m_look.water != SiteTerrain::Water::None;
    const bool seabed = m_look.water == SiteTerrain::Water::Seabed;
    bool forcedYet = false;
    for (size_t b = 0; b < m_plan.buildings.size(); ++b)
    {
        const FacilityLayout& building = m_plan.buildings[b];
        const bool pooled = seabed && building.HasMoonPool();
        if (building.ground <= 0 && !pooled)
        {
            continue;
        }
        const bool forced = m_look.flood && !forcedYet;
        forcedYet = forcedYet || forced;
        const bool neglected = building.condition != FacilityLayout::Condition::Kept && building.condition != FacilityLayout::Condition::Hurried;
        const uint32_t chance = pooled ? (neglected ? 50u : 15u) : 12u; // (a basement: one in eight of those left -- at one in four, most were)
        if (!forced && !((neglected || pooled) && watery && Mix(seed, 0xF100Du + static_cast<uint32_t>(b)) % 100u < chance))
        {
            continue;
        }
        const uint32_t pick = Mix(seed, 0xF100Eu + static_cast<uint32_t>(b));
        constexpr float kDepths[] = {0.18f, 0.32f, 0.55f, 1.0f, 1.4f, FacilityLayout::kClearHeight - 0.08f};
        const int storey = pooled ? 0 : static_cast<int>(pick % static_cast<uint32_t>(building.ground));
        tops[b] = forced && m_look.floodLevel >= 0.0f ? building.origin.y + m_look.floodLevel
                                                      : building.origin.y + static_cast<float>(storey) * FacilityLayout::kStorey + kDepths[(pick / 7u) % 6u];
    }
    // Through the passages joining buildings under the ground: water standing over a passage's floor at one end runs through it into
    // the other, until it stands at the one height in both and in the passage -- as high as the higher, but never up onto either's
    // ground floor. (Round again for a building joined to two.)
    // (On the sea's floor, no doors to let it out: as high as its lowest floor's ceiling.)
    const auto cap = [&](size_t b)
    {
        const FacilityLayout& building = m_plan.buildings[b];
        return seabed && building.HasMoonPool() ? building.origin.y + FacilityLayout::kClearHeight - 0.08f : building.GroundLevel() - 0.02f;
    };
    for (size_t b = 0; b < tops.size(); ++b)
    {
        tops[b] = std::min(tops[b], cap(b));
    }
    for (int round = 0; round < 4; ++round)
    {
        for (const SitePlan::Tunnel& tunnel : m_plan.tunnels)
        {
            const size_t a = static_cast<size_t>(tunnel.from);
            const size_t c = static_cast<size_t>(tunnel.to);
            if (a >= tops.size() || c >= tops.size())
            {
                continue;
            }
            const float high = std::max(tops[a], tops[c]);
            if (high <= tunnel.floor + 0.02f)
            {
                continue;
            }
            tops[a] = std::max(tops[a], std::min(high, cap(a)));
            tops[c] = std::max(tops[c], std::min(high, cap(c)));
        }
    }
    // Never in one plane with a step's top (the stairs rise from every floor in steps of a storey's eighteenth, the floors among them):
    // a surface there fought with the step all along its edge. A few centimetres over it instead.
    const float rise = FacilityLayout::kStorey / static_cast<float>(FacilityMap::kSteps);
    for (size_t b = 0; b < tops.size(); ++b)
    {
        if (tops[b] < -1.0e8f)
        {
            continue; // (none)
        }
        const float over = tops[b] - m_plan.buildings[b].origin.y;
        const float plane = tops[b] - (over - std::round(over / rise) * rise);
        if (std::abs(tops[b] - plane) < 0.02f)
        {
            // (Under it instead where over it would be up on the ground floor.)
            tops[b] = plane + 0.04f <= cap(b) ? plane + 0.04f : plane - 0.04f;
        }
    }
    return tops;
}

bool SiteMap::UnderBuilding(float x, float z) const
{
    if (!m_built || !m_terrain.Submerged())
    {
        return false;
    }
    for (const FacilityLayout& building : m_plan.buildings)
    {
        glm::vec2 min;
        glm::vec2 max;
        SitePlan::Footprint(building, 0.0f, min, max);
        if (x > min.x && x < max.x && z > min.y && z < max.y)
        {
            return true;
        }
    }
    return false;
}

bool SiteMap::Sealed(const glm::vec3& at) const
{
    if (!m_built || !m_terrain.Submerged())
    {
        return false;
    }
    for (const FacilityLayout& building : m_plan.buildings)
    {
        glm::vec2 min;
        glm::vec2 max;
        SitePlan::Footprint(building, 0.0f, min, max);
        // From the underside of its lowest floor to over its roof, inside its walls -- where it stands there (not in a courtyard).
        if (at.x <= min.x || at.x >= max.x || at.z <= min.y || at.z >= max.y || at.y < building.origin.y - FacilityLayout::kSlab - 0.02f)
        {
            continue;
        }
        const glm::ivec2 cell{static_cast<int>(std::floor((at.x - building.origin.x) / FacilityLayout::kCell)),
                              static_cast<int>(std::floor((at.z - building.origin.z) / FacilityLayout::kCell))};
        const int high = building.Height(cell.x, cell.y);
        if (high > 0 && at.y < building.origin.y + static_cast<float>(high) * FacilityLayout::kStorey + 0.3f)
        {
            return true;
        }
    }
    return false;
}

float SiteMap::WaterAt(const glm::vec3& at, float& surface, bool* standing) const
{
    if (standing != nullptr)
    {
        *standing = false;
    }
    if (!m_built)
    {
        return -1.0f;
    }
    // A building on the sea's floor is dry inside, the sea round it and under it -- but for the column of its moon pool, the sea
    // standing up into it, from the floor of the sea under the building to the pool's surface.
    if (m_terrain.Submerged())
    {
        // In a lockout, as full of the sea as it is now (Lockout::level): the sea let in or out of it as it cycles.
        for (const Lockout& lockout : m_lockouts)
        {
            // (Through its inner doorway too, to the wall's inside face: in neither it nor the building, the doorway was
            // counted as the open sea, and anybody stepping through it from a drained lockout swam a stride.)
            const glm::vec2 lo = glm::vec2(lockout.min.x, lockout.min.z) + glm::min(lockout.inward, glm::vec2(0.0f)) * 0.45f;
            const glm::vec2 hi = glm::vec2(lockout.max.x, lockout.max.z) + glm::max(lockout.inward, glm::vec2(0.0f)) * 0.45f;
            if (at.x > lo.x && at.x < hi.x && at.z > lo.y && at.z < hi.y && at.y > lockout.min.y - 0.3f && at.y < lockout.max.y + 0.2f)
            {
                const float top = lockout.min.y + lockout.level * (lockout.max.y - lockout.min.y);
                if (lockout.level < 0.01f)
                {
                    return -1.0f;
                }
                surface = top;
                if (standing != nullptr)
                {
                    *standing = true;
                }
                return top - lockout.min.y;
            }
        }
        for (const Pool& pool : m_pools)
        {
            if (at.x > pool.min.x && at.x < pool.max.x && at.z > pool.min.y && at.z < pool.max.y && at.y < pool.surface + 2.5f)
            {
                surface = pool.surface;
                if (standing != nullptr)
                {
                    *standing = true;
                }
                return pool.surface - m_terrain.Height(at.x, at.z);
            }
        }
        if (Sealed(at))
        {
            // (Dry, but where the sea has come up into it over its lowest floor.)
            const float flood = Flood(at.x, at.z, surface);
            if (flood > 0.0f && standing != nullptr)
            {
                *standing = true;
            }
            return flood > 0.0f ? flood : -1.0f;
        }
        // Anywhere else, the sea: under a building as round it (what stands in one is only in it).
        surface = m_terrain.WaterSurface(at.x, at.z);
        return m_terrain.WaterDepth(at.x, at.z);
    }
    const float flood = Flood(at.x, at.z, surface);
    if (flood > 0.0f)
    {
        if (standing != nullptr)
        {
            *standing = true;
        }
        return flood;
    }
    if (!m_terrain.HasWater() && !m_terrain.HasStreams())
    {
        return -1.0f;
    }
    surface = m_terrain.WaterSurface(at.x, at.z);
    return m_terrain.WaterDepth(at.x, at.z);
}

float SiteMap::Flood(float x, float z, float& surface) const
{
    for (size_t b = 0; b < m_plan.buildings.size() && b < m_floods.size(); ++b)
    {
        const auto [storey, depth] = m_floods[b];
        if (storey < 0)
        {
            continue;
        }
        // Anywhere the building stands: the water stands at the one level all through it, over every storey under that one --
        // whatever is over a basement at the ground (filler behind walls, as often as not) -- and up there, over that storey's floor.
        const FacilityLayout& building = m_plan.buildings[b];
        const glm::vec3 low = FacilityMap::ToWorld(building, 0, glm::vec2(0.0f));
        const int cx = static_cast<int>(std::floor((x - low.x) / FacilityLayout::kCell));
        const int cz = static_cast<int>(std::floor((z - low.z) / FacilityLayout::kCell));
        if (cx < 0 || cz < 0 || cx >= building.width || cz >= building.depth || building.Height(cx, cz) <= 0)
        {
            continue;
        }
        surface = FacilityMap::ToWorld(building, storey, glm::vec2(0.0f)).y + depth;
        return surface - low.y;
    }
    // Or in a passage under the ground it has run through (FloodTops), over its floor.
    const glm::ivec2 cell{static_cast<int>(std::floor((x - m_plan.origin.x) / FacilityLayout::kCell)),
                          static_cast<int>(std::floor((z - m_plan.origin.z) / FacilityLayout::kCell))};
    for (size_t t = 0; t < m_plan.tunnels.size() && t < m_tunnelFloods.size(); ++t)
    {
        const SitePlan::Tunnel& tunnel = m_plan.tunnels[t];
        if (m_tunnelFloods[t] > tunnel.floor && std::find(tunnel.cells.begin(), tunnel.cells.end(), cell) != tunnel.cells.end())
        {
            surface = std::min(m_tunnelFloods[t], tunnel.floor + kTunnelClear - 0.05f);
            return surface - tunnel.floor;
        }
    }
    // Or in a flooded cave (SitePlan::Cave::water), where it is open at the water's level -- but not under a lake or the sea, whose
    // water is the one anybody over it is in.
    for (const SitePlan::Cave& cave : m_plan.caves)
    {
        if (cave.water > cave.bottom && x > cave.min.x && x < cave.max.x && z > cave.min.z && z < cave.max.z && m_terrain.WaterDepth(x, z) <= 0.0f &&
            cave.Distance({x, cave.water - 0.02f, z}) < 0.0f)
        {
            surface = cave.water;
            return cave.water - cave.bottom;
        }
    }
    return -1.0f;
}

} // namespace pred
