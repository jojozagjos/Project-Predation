#include "Game/World/SiteMap.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Primitives.h"
#include "Game/World/MapBuilder.h"
#include "Game/World/ShipMap.h"
#include "Game/World/Surfaces.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
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
const Material kHullDebris = Material::Metal({0.27f, 0.28f, 0.3f}, 0.55f);
const Material kHullDebrisLight = Material::Diffuse({0.56f, 0.56f, 0.54f}, 0.7f);
const Material kPadMaterial = Material::Diffuse({0.3f, 0.3f, 0.29f}, 0.85f);
const Material kPipeMaterial = Material::Metal({0.34f, 0.35f, 0.36f}, 0.5f);
const Material kSupportMaterial = Material::Metal({0.22f, 0.22f, 0.23f}, 0.6f);
const Material kTankMaterial = Material::Diffuse({0.5f, 0.5f, 0.47f}, 0.7f);
const Material kPoleMaterial = Material::Metal({0.25f, 0.25f, 0.26f}, 0.55f);
// Freight containers come in the colours they always do, faded.
const Material kContainerMaterials[] = {
    Material::Diffuse({0.32f, 0.12f, 0.08f}, 0.8f),
    Material::Diffuse({0.1f, 0.18f, 0.27f}, 0.8f),
    Material::Diffuse({0.23f, 0.25f, 0.14f}, 0.8f),
    Material::Diffuse({0.36f, 0.35f, 0.33f}, 0.8f),
};

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
    Clear(scene, physics, lights);
    m_seed = seed;
    m_plan = SitePlan::Generate(seed);
    m_terrain.Plan(m_plan, m_look.terrain, m_look.dunes);
    if (lights != nullptr)
    {
        m_firstLight = lights->Count();
        m_hasLights = true;
    }

    // The ground, the rock that closes it in and the buildings set into it are one structure, built to reach into
    // one another: the rock is a ragged wall of blocks run together and sunk into the ground, and so is every
    // building's outer wall. The level check has nothing to say of that (PhysicsWorld::SetOverlapGroup).
    const uint32_t ground = physics.NewOverlapGroup();
    const Material rockMaterial = Material::Diffuse(m_look.rock, 0.95f);
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

    // Outside.
    MapBuilder builder(scene, meshes, &physics, "site_");
    builder.Track(&m_entities, &m_bodies);
    // The ground, in pieces each drawn on its own (so what is out of sight is not drawn): the world's ground where it is
    // gentle, its rock showing through as it steepens. Part of the one structure the buildings are set into.
    builder.SetStructure(ground);
    const glm::vec3 cliffColour = m_look.rock * 0.88f;
    Material terrainMaterial = Surfaces::Apply(Material::Diffuse(SiteTerrain::Paint(m_look.ground, cliffColour), 1.0f), m_look.surface, 2.5f);
    terrainMaterial = Surfaces::ApplySteep(terrainMaterial, m_look.steep, 11.0f);
    // Made side by side, a row of pieces a thread -- the shapes to stand on are most of the time a site takes to build -- and
    // put into the world here, in order.
    struct Piece
    {
        MeshData mesh;
        PhysicsWorld::PreparedMesh shape;
    };
    const int chunks = m_terrain.Chunks();
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
                                              piece.mesh = m_terrain.Mesh(cx, cz, m_look.ground, cliffColour);
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
    builder.BeginBatching(24.0f);
    int containers = 0;
    for (const SitePlan::Block& block : m_plan.blocks)
    {
        Transform transform;
        transform.position = block.centre;
        // Boulders and wreckage lie where the ground is; everything else stands on levelled ground.
        if (block.kind == Kind::Rock || block.kind == Kind::Debris)
        {
            transform.position.y += lift({block.centre.x, block.centre.z}, std::max(block.size.x, block.size.z) * 0.35f);
        }
        transform.rotation = glm::angleAxis(block.yaw, glm::vec3(0.0f, 1.0f, 0.0f)) * glm::angleAxis(block.tip, glm::vec3(1.0f, 0.0f, 0.0f));
        // The pipework too, which runs into the side of each building it joins; and debris, half in the ground.
        const bool terrain = block.kind == Kind::Rock || block.kind == Kind::Pad ||
                             block.kind == Kind::PipeX || block.kind == Kind::PipeZ || block.kind == Kind::Support || block.kind == Kind::Debris;
        builder.SetStructure(terrain ? ground : 0);
        switch (block.kind)
        {
        case Kind::Rock:
            builder.AddBox(NameOf(block.kind), transform, block.size, rockMaterial);
            break;
        case Kind::Container:
            builder.AddBox(NameOf(block.kind), transform, block.size,
                           kContainerMaterials[static_cast<size_t>(Mix(seed, static_cast<uint32_t>(containers++)) % std::size(kContainerMaterials))]);
            break;
        case Kind::Pad:
            builder.AddBox(NameOf(block.kind), transform, block.size, kPadMaterial, kOutdoorTile);
            break;
        case Kind::Support:
            builder.AddBox(NameOf(block.kind), transform, block.size, kSupportMaterial);
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
            builder.AddMesh(NameOf(block.kind), transform, Primitives::Cylinder(radius, length, 14), kPipeMaterial);
            break;
        }
        case Kind::Tank:
            builder.AddMesh(NameOf(block.kind), transform, Primitives::Cylinder(block.size.x * 0.5f, block.size.y, 20), kTankMaterial);
            break;
        case Kind::Mast:
        {
            // A lattice: four legs, braced every few metres, a red light at the top.
            const float half = block.size.x * 0.5f;
            const float foot = block.centre.y - block.size.y * 0.5f;
            const glm::quat turn = glm::angleAxis(block.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
            for (const glm::vec2 leg : {glm::vec2(-1.0f, -1.0f), glm::vec2(1.0f, -1.0f), glm::vec2(-1.0f, 1.0f), glm::vec2(1.0f, 1.0f)})
            {
                Transform post;
                post.position = block.centre + turn * glm::vec3(leg.x * half, 0.0f, leg.y * half);
                post.rotation = turn;
                builder.AddBox(NameOf(block.kind), post, {0.16f, block.size.y, 0.16f}, kSupportMaterial);
            }
            for (float y = foot + 3.0f; y < foot + block.size.y - 0.5f; y += 3.5f)
            {
                for (int side = 0; side < 4; ++side)
                {
                    const glm::vec3 out = side == 0 ? glm::vec3(0.0f, 0.0f, -half) : side == 1 ? glm::vec3(0.0f, 0.0f, half) : side == 2 ? glm::vec3(-half, 0.0f, 0.0f)
                                                                                                                       : glm::vec3(half, 0.0f, 0.0f);
                    Transform brace;
                    brace.position = glm::vec3(block.centre.x, y, block.centre.z) + turn * out;
                    brace.rotation = turn;
                    const bool alongX = side < 2;
                    builder.AddMesh(NameOf(block.kind), brace, Primitives::Box(alongX ? glm::vec3(block.size.x, 0.1f, 0.1f) : glm::vec3(0.1f, 0.1f, block.size.x)),
                                    kSupportMaterial);
                }
            }
            Material beacon = Material::Diffuse({0.3f, 0.02f, 0.02f}, 0.4f);
            beacon.emissive = {3.0f, 0.15f, 0.1f};
            Transform top;
            top.position = glm::vec3(block.centre.x, foot + block.size.y + 0.25f, block.centre.z);
            builder.AddMesh("site_mast_light", top, Primitives::Box({0.5f, 0.5f, 0.5f}), beacon);
            break;
        }
        case Kind::Debris:
            builder.AddBox(NameOf(block.kind), transform, block.size, Mix(seed, static_cast<uint32_t>(block.centre.x * 7.0f)) % 3u == 0 ? kHullDebrisLight : kHullDebris);
            break;
        }
    }

    builder.SetStructure(0);

    // The shuttle on the pad, standing on it and part of the site's structure, a lamp lit in its cabin.
    m_shuttle.Build(scene, meshes, &physics, Vehicles::Load("shuttle"), ShuttleHome(), ground, "site_shuttle_");
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
    }

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
            builder.AddBox("site_pole", pole, {0.18f, height, 0.18f}, kPoleMaterial);
            Transform arm;
            arm.position = (foot + glm::vec3(lamp.position.x, 0.0f, lamp.position.z) - glm::vec3(0.0f, foot.y, 0.0f)) * 0.5f;
            arm.position.y = lamp.position.y + raise + 0.12f;
            arm.rotation = glm::angleAxis(std::atan2(-back.z, back.x), glm::vec3(0.0f, 1.0f, 0.0f));
            builder.AddBox("site_pole_arm", arm, {0.62f, 0.08f, 0.08f}, kPoleMaterial);
            builder.SetStructure(0);
        }
        if (lights != nullptr)
        {
            const glm::vec3 at = lamp.position + glm::vec3(0.0f, lamp.kind == SitePlan::LampKind::Pole ? lift({lamp.position.x, lamp.position.z}, 0.5f) : 0.0f, 0.0f);
            lights->Add(scene, meshes, LightKind::Flood, MoodOf(lamp.mood), at, lamp.direction, 0,
                        Mix(seed, 0x1A3Bu + static_cast<uint32_t>(i)) | 1u, lamp.range);
        }
    }
    physics.OptimizeBroadPhase();

    m_built = true;
    PRED_LOG_INFO(Gameplay, "Site {}: {} buildings, {} pieces outside, {} lamps outside, {} doors in all", seed,
                  m_plan.buildings.size(), m_plan.blocks.size(), m_plan.lamps.size(), m_placements.doors.size());
}

CinePose SiteMap::ShuttleHome() const
{
    // Its nose away from the site, so its back and its ramp face into it.
    return {m_plan.ShuttleBase(), TurnFromDegrees({0.0f, glm::degrees(m_plan.landingYaw) + 180.0f, 0.0f})};
}

void SiteMap::Clear(Scene& scene, PhysicsWorld& physics, LevelLights* lights)
{
    m_shuttle.Clear(scene, &physics);
    for (const Entity entity : m_entities)
    {
        scene.Destroy(entity);
    }
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

bool SiteMap::Contains(const glm::vec3& at) const
{
    glm::vec3 min;
    glm::vec3 max;
    Bounds(min, max);
    return at.x >= min.x && at.y >= min.y && at.z >= min.z && at.x <= max.x && at.y <= max.y && at.z <= max.z;
}

} // namespace pred
