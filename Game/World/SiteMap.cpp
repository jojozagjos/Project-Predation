#include "Game/World/SiteMap.h"

#include "Engine/Core/Loading.h"
#include "Engine/Core/Log.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Primitives.h"
#include "Game/World/Cargo.h"
#include "Game/World/Foliage.h"
#include "Game/World/MapBuilder.h"
#include "Game/World/ShipMap.h"
#include "Game/World/SiteScatter.h"
#include "Game/World/Surfaces.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <future>
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
const Material kTankPlain = Material::Diffuse({0.5f, 0.5f, 0.47f}, 0.7f);
const Material kPolePlain = Material::Metal({0.25f, 0.25f, 0.26f}, 0.55f);
// Freight containers come in the colours they always do, faded.
const glm::vec3 kContainerColours[] = {{0.32f, 0.12f, 0.08f}, {0.1f, 0.18f, 0.27f}, {0.23f, 0.25f, 0.14f}, {0.36f, 0.35f, 0.33f}};

// Open ground is drawn in big pieces: few lamps reach it, and there is a great deal of it.
constexpr float kOutdoorTile = 24.0f;

const char* NameOf(Kind kind)
{
    switch (kind)
    {
    case Kind::Rock: return "site_rock";
    case Kind::Container: return "site_container";
    case Kind::Pad: return "site_pad";
    case Kind::PipeX:
    case Kind::PipeZ: return "site_pipe";
    case Kind::Support: return "site_support";
    case Kind::Tank: return "site_tank";
    case Kind::Mast: return "site_mast";
    case Kind::Debris: return "site_debris";
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

} // namespace

void SiteMap::Build(uint16_t seed, Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, LevelLights* lights)
{
    BuildFrom(SitePlan::Generate(seed), seed, false, scene, meshes, physics, lights);
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
    m_terrain.Plan(m_plan, m_look.terrain, m_look.dunes, m_look.water, stream);
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
    const Material tankMaterial = Surfaces::Apply(kTankPlain, "painted_metal", 3.0f, 0.2f);
    const Material poleMaterial = Surfaces::Apply(kPolePlain, "pipe_steel", 1.0f, 0.3f);
    // Where something stands on the ground: how far its foot is moved from the plan's flat ground to the terrain's, at the
    // lowest of its middle and corners, so nothing on a slope stands on air.
    const auto lift = [&](glm::vec2 at, float reach)
    {
        float low = m_terrain.Height(at.x, at.y);
        for (const glm::vec2 corner : {glm::vec2(-1.0f, -1.0f), glm::vec2(1.0f, -1.0f), glm::vec2(-1.0f, 1.0f), glm::vec2(1.0f, 1.0f)})
        {
            low = std::min(low, m_terrain.Height(at.x + corner.x * reach, at.y + corner.y * reach));
        }
        return low - m_plan.origin.y;
    };

    mark("props");
    // The buildings, each with its own meshes, and everything in them for WorldObjects.
    for (size_t b = 0; b < m_plan.buildings.size(); ++b)
    {
        auto building = std::make_unique<FacilityMap>();
        building->Build(m_plan.buildings[b], Mix(seed, static_cast<uint32_t>(b) + 1u), scene, meshes, physics, lights,
                        "site" + std::to_string(b) + "_", ground);
        Append(m_placements, building->Placements());
        for (const FacilityLayout::Exit& exit : m_plan.buildings[b].exits)
        {
            const glm::vec3 at = FacilityMap::ExitOutside(m_plan.buildings[b], exit, 2.0f);
            PRED_LOG_INFO(Gameplay, "Site building {}: a way in at {:.1f} {:.1f} {:.1f}, facing {}", b, at.x, at.y, at.z,
                          exit.side == 0 ? "west" : exit.side == 1 ? "east" : exit.side == 2 ? "north" : "south");
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
    const bool grassy = m_look.biome == "jungle" || m_look.biome == "temperate" || m_look.biome == "oceanic";
    const glm::vec3 growing = glm::mix(m_look.ground, m_look.biome == "jungle" ? glm::vec3(0.05f, 0.13f, 0.03f) : glm::vec3(0.08f, 0.16f, 0.04f), 0.65f);
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
    for (const SitePlan::Block& block : m_plan.blocks)
    {
        // (An outpost's own things are its own: only its ground here.)
        if (groundOnly)
        {
            break;
        }
        Transform transform;
        transform.position = block.centre;
        // Boulders and wreckage lie where the ground is, and what stands out in the land; everything else stands on levelled
        // ground.
        if (block.kind == Kind::Rock || block.kind == Kind::Debris || block.outlying)
        {
            // (From the lowest of the ground under its corners, however it is turned.)
            transform.position.y += lift({block.centre.x, block.centre.z}, std::max(block.size.x, block.size.z) * 0.5f);
        }
        transform.rotation = glm::angleAxis(block.yaw, glm::vec3(0.0f, 1.0f, 0.0f)) * glm::angleAxis(block.tip, glm::vec3(1.0f, 0.0f, 0.0f));
        // The pipework too, which runs into the side of each building it joins; and debris, half in the ground.
        const bool terrain = block.kind == Kind::Rock || block.kind == Kind::Pad ||
                             block.kind == Kind::PipeX || block.kind == Kind::PipeZ || block.kind == Kind::Support || block.kind == Kind::Debris ||
                             block.outlying;
        builder.SetStructure(terrain ? ground : 0);
        switch (block.kind)
        {
        case Kind::Rock:
            builder.AddBox(NameOf(block.kind), transform, block.size, rockMaterial);
            break;
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
            builder.AddBox(NameOf(block.kind), transform, block.size, padMaterial, kOutdoorTile);
            break;
        case Kind::Support:
            builder.AddBox(NameOf(block.kind), transform, block.size, supportMaterial);
            break;
        case Kind::PipeX:
        case Kind::PipeZ:
        {
            // A cylinder stands along y: laid down along the pipe's run.
            const bool alongX = block.kind == Kind::PipeX;
            const float length = alongX ? block.size.x : block.size.z;
            const float radius = block.size.y * 0.5f;
            transform.rotation = alongX ? glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f))
                                        : glm::angleAxis(glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
            builder.AddMesh(NameOf(block.kind), transform, Primitives::Cylinder(radius, length, 14), pipeMaterial);
            break;
        }
        case Kind::Tank:
            builder.AddMesh(NameOf(block.kind), transform, Primitives::Cylinder(block.size.x * 0.5f, block.size.y, 20), tankMaterial);
            break;
        case Kind::Mast:
        {
            // A lattice: four legs, braced every few metres, a red light at the top.
            const float half = block.size.x * 0.5f;
            const float foot = transform.position.y - block.size.y * 0.5f;
            const glm::quat turn = glm::angleAxis(block.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
            for (const glm::vec2 leg : {glm::vec2(-1.0f, -1.0f), glm::vec2(1.0f, -1.0f), glm::vec2(-1.0f, 1.0f), glm::vec2(1.0f, 1.0f)})
            {
                Transform post;
                post.position = transform.position + turn * glm::vec3(leg.x * half, 0.0f, leg.y * half);
                post.rotation = turn;
                builder.AddBox(NameOf(block.kind), post, {0.16f, block.size.y, 0.16f}, supportMaterial);
            }
            for (float y = foot + 3.0f; y < foot + block.size.y - 0.5f; y += 3.5f)
            {
                for (int side = 0; side < 4; ++side)
                {
                    const glm::vec3 out = side == 0 ? glm::vec3(0.0f, 0.0f, -half) : side == 1 ? glm::vec3(0.0f, 0.0f, half) : side == 2 ? glm::vec3(-half, 0.0f, 0.0f)
                                                                                                                       : glm::vec3(half, 0.0f, 0.0f);
                    Transform brace;
                    brace.position = glm::vec3(transform.position.x, y, transform.position.z) + turn * out;
                    brace.rotation = turn;
                    const bool alongX = side < 2;
                    builder.AddMesh(NameOf(block.kind), brace, Primitives::Box(alongX ? glm::vec3(block.size.x, 0.1f, 0.1f) : glm::vec3(0.1f, 0.1f, block.size.x)),
                                    supportMaterial);
                }
            }
            Material beacon = Material::Diffuse({0.3f, 0.02f, 0.02f}, 0.4f);
            beacon.emissive = {3.0f, 0.15f, 0.1f};
            Transform top;
            top.position = glm::vec3(transform.position.x, foot + block.size.y + 0.25f, transform.position.z);
            builder.AddMesh("site_mast_light", top, Primitives::Box({0.5f, 0.5f, 0.5f}), beacon);
            break;
        }
        case Kind::Debris:
            builder.AddBox(NameOf(block.kind), transform, block.size, Mix(seed, static_cast<uint32_t>(block.centre.x * 7.0f)) % 3u == 0 ? hullDebrisLight : hullDebris);
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
                shapes[which] = SiteScatter::Shape(item.kind, item.variant, seed, growth);
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
                    MeshData trunk = SiteScatter::Shape(SiteScatter::Kind::Tree, variant, landmark.seed, growth);
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
        // out over a couple of metres, uneven along it, tapering at the corners and stopping short of the doors. The ground's
        // own surface, walked through, not over.
        if (m_look.biome == "frozen" || m_look.surface == "sand")
        {
            const float deepest = m_look.biome == "frozen" ? 0.9f : 0.55f;
            const float windAngle = WindAngle(seed);
            const glm::vec2 wind{std::cos(windAngle), std::sin(windAngle)};
            MeshData drifts;
            for (size_t b = 0; b < m_plan.buildings.size(); ++b)
            {
                const FacilityLayout& building = m_plan.buildings[b];
                glm::vec2 low;
                glm::vec2 high;
                SitePlan::Footprint(building, 0.0f, low, high);
                std::vector<glm::vec2> doors;
                for (const FacilityLayout::Exit& exit : building.exits)
                {
                    const glm::vec3 at = FacilityMap::ExitOutside(building, exit, 0.5f);
                    doors.emplace_back(at.x, at.z);
                }
                // Each side: where it runs from and to, and which way is out from it.
                const struct
                {
                    glm::vec2 from;
                    glm::vec2 to;
                    glm::vec2 out;
                } sides[4] = {{{low.x, low.y}, {low.x, high.y}, {-1.0f, 0.0f}},
                              {{high.x, high.y}, {high.x, low.y}, {1.0f, 0.0f}},
                              {{high.x, low.y}, {low.x, low.y}, {0.0f, -1.0f}},
                              {{low.x, high.y}, {high.x, high.y}, {0.0f, 1.0f}}};
                for (int s = 0; s < 4; ++s)
                {
                    const float lee = glm::smoothstep(-0.3f, 0.8f, glm::dot(sides[s].out, wind));
                    const float height = deepest * glm::mix(0.3f, 1.0f, lee);
                    const float length = glm::distance(sides[s].from, sides[s].to);
                    const int along = std::max(static_cast<int>(length / 0.5f), 2);
                    constexpr int kAcross = 6;
                    const auto base = static_cast<uint32_t>(drifts.vertices.size());
                    for (int i = 0; i <= along; ++i)
                    {
                        const float t = static_cast<float>(i) / static_cast<float>(along);
                        const glm::vec2 wall = glm::mix(sides[s].from, sides[s].to, t) + sides[s].out * 0.03f;
                        const float fromEnd = std::min(t, 1.0f - t) * length;
                        // Uneven along it, smoothly: a value every couple of metres, eased between.
                        const float knot = t * length / 2.0f;
                        const auto k0 = static_cast<uint32_t>(knot);
                        const float f = glm::smoothstep(0.0f, 1.0f, knot - static_cast<float>(k0));
                        const float uneven = glm::mix(Hash01(seed, static_cast<uint32_t>(b * 4 + s), k0), Hash01(seed, static_cast<uint32_t>(b * 4 + s), k0 + 1u), f);
                        float h = height * (0.6f + 0.4f * uneven) *
                                  glm::smoothstep(0.0f, 2.5f, fromEnd);
                        for (const glm::vec2& door : doors)
                        {
                            h *= glm::smoothstep(1.4f, 2.6f, glm::distance(door, wall));
                        }
                        const float reach = 0.4f + h * 2.4f;
                        for (int k = 0; k <= kAcross; ++k)
                        {
                            const float u = static_cast<float>(k) / static_cast<float>(kAcross);
                            const glm::vec2 at = wall + sides[s].out * (reach * u);
                            MeshVertex vertex;
                            // Highest at the wall, falling away in a curve to nothing.
                            vertex.position = {at.x, m_terrain.Height(at.x, at.y) + h * std::pow(1.0f - u, 1.6f) - 0.02f, at.y};
                            vertex.uv = at;
                            drifts.vertices.push_back(vertex);
                        }
                    }
                    for (int i = 0; i < along; ++i)
                    {
                        for (int k = 0; k < kAcross; ++k)
                        {
                            const auto a = base + static_cast<uint32_t>(i * (kAcross + 1) + k);
                            const auto c = a + static_cast<uint32_t>(kAcross + 1);
                            // Wound to face up (sides round the building one way, `out` turned from `along` the same way each).
                            drifts.indices.insert(drifts.indices.end(), {a, a + 1, c, a + 1, c + 1, c});
                        }
                    }
                }
            }
            // Lit by its own shape: each corner's normal from the faces round it.
            std::vector<glm::vec3> normals(drifts.vertices.size(), glm::vec3(0.0f));
            for (size_t i = 0; i + 2 < drifts.indices.size(); i += 3)
            {
                const uint32_t a = drifts.indices[i];
                const uint32_t b = drifts.indices[i + 1];
                const uint32_t c = drifts.indices[i + 2];
                const glm::vec3 n = glm::cross(drifts.vertices[b].position - drifts.vertices[a].position, drifts.vertices[c].position - drifts.vertices[a].position);
                normals[a] += n;
                normals[b] += n;
                normals[c] += n;
            }
            for (size_t i = 0; i < normals.size(); ++i)
            {
                drifts.vertices[i].normal = glm::length(normals[i]) > 1.0e-9f ? glm::normalize(normals[i]) : glm::vec3(0.0f, 1.0f, 0.0f);
            }
            if (!drifts.vertices.empty())
            {
                builder.AddMesh("site_drifts", Transform{}, drifts, m_groundMaterial, false);
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
            const int lamp = lights->Add(scene, meshes, LightKind::Ceiling, LightMood::Steady, cabinLamp.position, glm::vec3(0.0f, -1.0f, 0.0f), 0,
                                         Mix(seed, 0x5A77u) | 1u, 6.0f);
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
    // The lamps outside: floodlights over the doors, and on poles over the ground.
    for (size_t i = 0; i < m_plan.lamps.size(); ++i)
    {
        const SitePlan::Lamp& lamp = m_plan.lamps[i];
        if (lamp.kind == SitePlan::LampKind::Pole)
        {
            // Beside the lamp rather than under it, holding it out on an arm: a lamp inside the top of its own
            // pole is inside the pole as far as its shadow is concerned, and lights nothing.
            glm::vec3 back{-lamp.direction.x, 0.0f, -lamp.direction.z};
            back = glm::length(back) > 0.05f ? glm::normalize(back) : glm::vec3(1.0f, 0.0f, 0.0f);
            const float raise = lift({lamp.position.x, lamp.position.z}, 0.5f);
            const glm::vec3 foot = glm::vec3(lamp.position.x, m_plan.origin.y + raise, lamp.position.z) + back * 0.6f;
            const float height = lamp.position.y - m_plan.origin.y + 0.15f;
            Transform pole;
            pole.position = foot + glm::vec3(0.0f, height * 0.5f, 0.0f);
            // The arm is set into the pole: one thing.
            builder.SetStructure(physics.NewOverlapGroup());
            builder.AddBox("site_pole", pole, {0.18f, height, 0.18f}, poleMaterial);
            Transform arm;
            arm.position = (foot + glm::vec3(lamp.position.x, 0.0f, lamp.position.z) - glm::vec3(0.0f, foot.y, 0.0f)) * 0.5f;
            arm.position.y = lamp.position.y + raise + 0.12f;
            arm.rotation = glm::angleAxis(std::atan2(-back.z, back.x), glm::vec3(0.0f, 1.0f, 0.0f));
            builder.AddBox("site_pole_arm", arm, {0.62f, 0.08f, 0.08f}, poleMaterial);
            builder.SetStructure(0);
        }
        else if (lamp.kind == SitePlan::LampKind::Beacon)
        {
            // A short post at the pad's edge, its light on top.
            const float raise = lift({lamp.position.x, lamp.position.z}, 0.3f);
            Transform post;
            post.position = glm::vec3(lamp.position.x, m_plan.origin.y + raise + 0.45f, lamp.position.z);
            builder.SetStructure(physics.NewOverlapGroup());
            builder.AddBox("site_beacon_post", post, {0.14f, 0.9f, 0.14f}, poleMaterial);
            builder.SetStructure(0);
        }
        if (lights != nullptr)
        {
            const bool standing = lamp.kind == SitePlan::LampKind::Pole || lamp.kind == SitePlan::LampKind::Beacon;
            const glm::vec3 at = lamp.position + glm::vec3(0.0f, standing ? lift({lamp.position.x, lamp.position.z}, lamp.kind == SitePlan::LampKind::Beacon ? 0.3f : 0.5f) : 0.0f, 0.0f);
            if (lamp.kind == SitePlan::LampKind::Beacon)
            {
                // (Red, pulsing slowly, on its own: no circuit to lose.)
                lights->Add(scene, meshes, LightKind::Emergency, LightMood::Pulse, at, glm::vec3(0.0f, 1.0f, 0.0f), 0, Mix(seed, 0xBEAC0u + static_cast<uint32_t>(i)) | 1u, lamp.range);
                continue;
            }
            lights->Add(scene, meshes, LightKind::Flood, MoodOf(lamp.mood), at, lamp.direction, 0,
                        Mix(seed, 0x1A3Bu + static_cast<uint32_t>(i)) | 1u, lamp.range);
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

bool SiteMap::GeneratorSide(glm::vec3& ground, float& yaw) const
{
    if (m_plan.generator < 0 || static_cast<size_t>(m_plan.generator) >= m_plan.blocks.size() || !m_terrain.Planned())
    {
        return false;
    }
    const SitePlan::Block& block = m_plan.blocks[static_cast<size_t>(m_plan.generator)];
    // Where Build stands it: on the lowest of the ground round it.
    const float reach = std::max(block.size.x, block.size.z) * 0.35f;
    float low = m_terrain.Height(block.centre.x, block.centre.z);
    for (const glm::vec2 corner : {glm::vec2(-1.0f, -1.0f), glm::vec2(1.0f, -1.0f), glm::vec2(-1.0f, 1.0f), glm::vec2(1.0f, 1.0f)})
    {
        low = std::min(low, m_terrain.Height(block.centre.x + corner.x * reach, block.centre.z + corner.y * reach));
    }
    // Its long side across the valley: the side facing back down it is out along the block's own -z, turned.
    const glm::vec2 facing{-std::sin(block.yaw), -std::cos(block.yaw)};
    ground = {block.centre.x + facing.x * (block.size.z * 0.5f + 0.08f), low, block.centre.z + facing.y * (block.size.z * 0.5f + 0.08f)};
    yaw = block.yaw;
    return true;
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

} // namespace pred
