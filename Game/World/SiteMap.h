#pragma once

#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Material.h"
#include "Engine/Scene/Scene.h"
#include "Game/World/FacilityMap.h"
#include "Game/World/LevelLights.h"
#include "Game/World/Vehicles.h"
#include "Game/World/SitePlan.h"
#include "Game/World/SiteTerrain.h"
#include "Game/World/WorldObjects.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <future>
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
    // How the place's outside looks: its own world's ground and rock and the shape of its ground, and what falls from its
    // sky. Set before building, the same on every machine (it comes from the world the site is on).
    struct Look
    {
        glm::vec3 ground{0.72f, 0.74f, 0.78f};
        glm::vec3 rock{0.21f, 0.2f, 0.18f};
        // What falls (a weather's particles: "snow", "rain", "dust", "ash", or "none") and how much of it, 0 to 1.
        std::string weather = "snow";
        float weatherAmount = 0.6f;
        SiteTerrain::Shape terrain = SiteTerrain::Shape::Flat;
        bool dunes = false;
        // The texture set its ground is laid with (universe.json, a biome's site surface; Assets/Textures), tinted to its
        // colour: none, a plain colour, until the set is there.
        std::string surface = "snow_02";
        // And where it is steep (empty: the ground set, darkened to the rock's colour).
        std::string steep = "rock_cliff";
        // Its world's biome (universe.json), for what lies about on the ground (SiteScatter).
        std::string biome = "frozen";
    };
    void SetLook(const Look& look) { m_look = look; }
    // How much of its weather is coming down just now (Look::weatherAmount): its spells come and go.
    void SetWeatherAmount(float amount) { m_look.weatherAmount = amount; }
    const Look& GetLook() const { return m_look; }

    void Build(uint16_t seed, Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, LevelLights* lights);
    void Clear(Scene& scene, PhysicsWorld& physics, LevelLights* lights);

    bool Built() const { return m_built; }
    uint16_t Seed() const { return m_seed; }
    const SitePlan& Plan() const { return m_plan; }
    const SiteTerrain& Terrain() const { return m_terrain; }
    // What its ground is drawn in, and the colour painted on it at a point: for what is laid on the ground to match it (a
    // footprint).
    const Material& GroundMaterial() const { return m_groundMaterial; }
    // Which way the wind blows at a site (radians about up, from +x towards +z), from its seed: the same for its weather, its
    // plants and the drifts against its walls, on every machine.
    static float WindAngle(uint32_t seed);
    glm::vec3 GroundTint(float x, float z) const
    {
        return m_terrain.Colour(x, z, m_terrain.Normal(x, z), m_look.ground, m_cliffColour, m_grassy ? &m_grownOver : nullptr);
    }
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
    // A place on the ground kept within how far round the site anybody may go (SiteTerrain::kRoam): itself, inside it.
    glm::vec3 WithinRoam(const glm::vec3& at) const;
    // The land round the site to the horizon shown only to an eye over it (within a couple of kilometres, at any height: a
    // shuttle coming down), never from anywhere else.
    void ShowFarLand(Scene& scene, const glm::vec3& eye);
    // The grass round the eye, where the world grows it: tufts thick close by and thinner further off, out to some tens of
    // metres, made again as the eye moves on (each tuft where its patch of ground puts it, so they stay put).
    // `detail` scales how thick it is drawn (the r.grass setting: 0 none).
    void UpdateCover(Scene& scene, MeshLibrary& meshes, const glm::vec3& eye, float detail);

private:
    Look m_look;
    SitePlan m_plan;
    SiteTerrain m_terrain;
    std::vector<std::unique_ptr<FacilityMap>> m_buildings;
    VehicleProp m_shuttle;
    WorldObjects::Placements m_placements;
    std::vector<Entity> m_entities;
    // The meshes it uploaded, given back when it is taken away (MeshLibrary::Release), and where they were uploaded to.
    std::vector<MeshHandle> m_meshHandles;
    MeshLibrary* m_meshLibrary = nullptr;
    Entity m_farLand;
    std::vector<BodyHandle> m_bodies;
    size_t m_firstLight = 0;
    bool m_hasLights = false;
    bool m_built = false;
    uint16_t m_seed = 0;
    // The grass round the eye (UpdateCover): tufts a square metre, its tufts' shapes and material, what it is drawn as, and
    // the patch of ground the eye was on when it was last made.
    float m_coverDensity = 0.0f;
    std::vector<MeshData> m_tufts;
    Material m_coverMaterial;
    Entity m_cover;
    MeshHandle m_coverMesh;
    glm::ivec2 m_coverAt{-100000, -100000};
    // The next one, being made on another thread.
    std::future<MeshData> m_coverBuild;
    MeshData CoverAround(const glm::vec3& eye, float detail) const;
    float m_coverDetail = -1.0f;
    Material m_groundMaterial;
    glm::vec3 m_cliffColour{0.3f};
    glm::vec3 m_grownOver{0.3f};
    bool m_grassy = false;
};

} // namespace pred
