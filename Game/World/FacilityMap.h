#pragma once

#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Scene.h"
#include "Game/World/FacilityLayout.h"
#include "Game/World/LevelLights.h"
#include "Game/World/WorldObjects.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace pred
{

class MeshLibrary;

namespace FacilitySpec
{
// The corner of cell (0, 0) on the ground floor. Between the test map, which ends at x = 45, and the
// lab, which begins at 138: the world is one navigation mesh, and in the gap the facility makes it no
// bigger.
inline constexpr glm::vec3 kOrigin{64.0f, 0.0f, -30.0f};
// The facility every machine builds until the host says otherwise.
inline constexpr uint16_t kDefaultSeed = 1;
} // namespace FacilitySpec

// A generated facility, built: FacilityLayout's plan turned into floors, walls, doorways, crawlspace
// ducts, stairs, furniture and lamps, and a list of the doors, lockers, crates and items for
// WorldObjects to put in it.
//
// Built into the same world as the test map and the lab, and rebuilt in place from a new seed. Only
// the seed is ever sent: every machine builds the same facility from it.
class FacilityMap
{
public:
    // One solid box of the building, in world space. Yaw turns it about its own centre.
    struct Piece
    {
        enum class Kind : uint8_t
        {
            Floor,   // the top of a slab: what is walked on
            Ceiling, // the rest of a slab: what is seen overhead
            Wall,
            Duct,    // a crawlspace's roof
            Pillar,
            Shelf,
            Bench,
            Crate,
            Cabinet, // where supplies are left
            Fill     // solid under the top of a flight of stairs
        };
        Kind kind = Kind::Wall;
        glm::vec3 centre{0.0f};
        glm::vec3 size{1.0f};
        float yaw = 0.0f;
    };

    // A flight of stairs: where the front of its bottom step is, at floor level, and which way it climbs
    // (radians about y; 0 climbs towards +z).
    struct Flight
    {
        glm::vec3 foot{0.0f};
        float yaw = 0.0f;
    };

    struct Lamp
    {
        glm::vec3 position{0.0f};
        LightKind kind = LightKind::Ceiling;
        LightMood mood = LightMood::Steady;
        int circuit = 0;
        float range = 0.0f;
        // The room it is in, or the straight run of corridor: nothing outside is lit by it.
        glm::vec3 boundsMin{0.0f};
        glm::vec3 boundsMax{0.0f};
    };

    // Everything the facility is, without an engine: for building it, and for testing that it fits
    // together.
    struct Blueprint
    {
        std::vector<Piece> pieces;
        std::vector<Flight> flights;
        std::vector<Lamp> lamps;
        WorldObjects::Placements placements;
        glm::vec3 spawn{0.0f};
        float spawnYaw = 0.0f;
    };

    static constexpr int kSteps = 18;
    static constexpr float kStepRun = 0.28f;
    static constexpr float kStairWidth = 4.78f;

    static Blueprint Draw(const FacilityLayout& layout);

    // Where a point given in cells, on a floor, is in the world, at that floor's level.
    static glm::vec3 ToWorld(int floor, glm::vec2 cells);

    // Replaces whatever facility was built before with the one planned from `seed`.
    void Build(uint16_t seed, Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, LevelLights* lights);
    void Clear(Scene& scene, PhysicsWorld& physics, LevelLights* lights);

    bool Built() const { return m_built; }
    uint16_t Seed() const { return m_seed; }
    const FacilityLayout& Layout() const { return m_layout; }
    const WorldObjects::Placements& Placements() const { return m_placements; }
    glm::vec3 Spawn() const { return m_spawn; }
    float SpawnYaw() const { return m_spawnYaw; }

private:
    FacilityLayout m_layout;
    WorldObjects::Placements m_placements;
    glm::vec3 m_spawn{0.0f};
    float m_spawnYaw = 0.0f;
    uint16_t m_seed = 0;
    bool m_built = false;
    std::vector<Entity> m_entities;
    std::vector<BodyHandle> m_bodies;
    size_t m_firstLight = 0;
    bool m_hasLights = false;
};

} // namespace pred
