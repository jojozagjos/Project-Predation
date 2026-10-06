#pragma once

#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Scene.h"
#include "Game/World/FacilityMap.h"
#include "Game/World/LevelLights.h"
#include "Game/World/Vehicles.h"
#include "Game/World/SitePlan.h"
#include "Game/World/SiteTerrain.h"
#include "Game/World/WorldObjects.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pred
{

class MeshLibrary;

// A mission site, built: SitePlan's buildings (a FacilityMap each), the ground they stand on and the rock that rises
// round it (SiteTerrain, shaped as its world's ground is), the cover and the lamps outside, and the doors, lockers and items of every building together
// for WorldObjects to put in.
//
// Rebuilt in place from a new seed, like a facility was: only the seed is ever sent.
class SiteMap
{
public:
    // How the place's outside looks: its own world's ground and rock and the shape of its ground, and whether snow lies
    // there and falls. Set before building, the same on every machine (it comes from the world the site is on).
    struct Look
    {
        glm::vec3 ground{0.72f, 0.74f, 0.78f};
        glm::vec3 rock{0.21f, 0.2f, 0.18f};
        bool snow = true;
        SiteTerrain::Shape terrain = SiteTerrain::Shape::Flat;
        bool dunes = false;
        // The texture set its ground is laid with (universe.json, a biome's site surface; Assets/Textures), tinted to its
        // colour: none, a plain colour, until the set is there.
        std::string surface = "snow_02";
        // And where it is steep (empty: the ground set, darkened to the rock's colour).
        std::string steep = "rock_cliff";
    };
    void SetLook(const Look& look) { m_look = look; }
    const Look& GetLook() const { return m_look; }

    void Build(uint16_t seed, Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, LevelLights* lights);
    void Clear(Scene& scene, PhysicsWorld& physics, LevelLights* lights);

    bool Built() const { return m_built; }
    uint16_t Seed() const { return m_seed; }
    const SitePlan& Plan() const { return m_plan; }
    const SiteTerrain& Terrain() const { return m_terrain; }
    const WorldObjects::Placements& Placements() const { return m_placements; }
    // Where somebody put on the site with nothing to bring them stands: off the foot of the shuttle's ramp, facing in.
    glm::vec3 Spawn() const { return m_plan.rampFoot.position + glm::vec3(0.0f, 0.5f, 0.0f); }
    float SpawnYaw() const { return m_plan.rampFoot.yaw; }
    // The shuttle on the pad: where it rests, its back to the site and its ramp down.
    VehicleProp& Shuttle() { return m_shuttle; }
    const VehicleProp& Shuttle() const { return m_shuttle; }
    CinePose ShuttleHome() const;
    // All of it, rock included: for anything that only needs the part of the world the site is in.
    void Bounds(glm::vec3& min, glm::vec3& max) const;
    // Whether a point is on the site.
    bool Contains(const glm::vec3& at) const;
    // Whether anything could walk there: the open ground and the foot of the rock round it, not the heights of the rock
    // (too steep for anybody), which the navigation is not built over.
    bool InReach(const glm::vec3& at) const;

private:
    Look m_look;
    SitePlan m_plan;
    SiteTerrain m_terrain;
    std::vector<std::unique_ptr<FacilityMap>> m_buildings;
    VehicleProp m_shuttle;
    WorldObjects::Placements m_placements;
    std::vector<Entity> m_entities;
    std::vector<BodyHandle> m_bodies;
    size_t m_firstLight = 0;
    bool m_hasLights = false;
    bool m_built = false;
    uint16_t m_seed = 0;
};

} // namespace pred
