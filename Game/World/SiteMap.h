#pragma once

#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Scene.h"
#include "Game/World/FacilityMap.h"
#include "Game/World/LevelLights.h"
#include "Game/World/Vehicles.h"
#include "Game/World/SitePlan.h"
#include "Game/World/WorldObjects.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace pred
{

class MeshLibrary;

// A mission site, built: SitePlan's buildings (a FacilityMap each), the ground they stand on, the rock
// round it, the cover and the lamps outside, and the doors, lockers and items of every building together
// for WorldObjects to put in.
//
// Rebuilt in place from a new seed, like a facility was: only the seed is ever sent.
class SiteMap
{
public:
    void Build(uint16_t seed, Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, LevelLights* lights);
    void Clear(Scene& scene, PhysicsWorld& physics, LevelLights* lights);

    bool Built() const { return m_built; }
    uint16_t Seed() const { return m_seed; }
    const SitePlan& Plan() const { return m_plan; }
    const WorldObjects::Placements& Placements() const { return m_placements; }
    // Where somebody put on the site with nothing to bring them stands: by the pad, where the crawler waits, facing in.
    glm::vec3 Spawn() const { return m_plan.crawlerStart.position + glm::vec3(0.0f, 0.5f, 0.0f); }
    float SpawnYaw() const { return m_plan.crawlerStart.yaw; }
    // The shuttle on the pad: where it rests, its back to the site and its ramp down.
    VehicleProp& Shuttle() { return m_shuttle; }
    const VehicleProp& Shuttle() const { return m_shuttle; }
    CinePose ShuttleHome() const;
    // All of it, rock included: for anything that only needs the part of the world the site is in.
    void Bounds(glm::vec3& min, glm::vec3& max) const;
    // Whether a point is on the site.
    bool Contains(const glm::vec3& at) const;

private:
    SitePlan m_plan;
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
