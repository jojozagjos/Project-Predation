#pragma once

#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Scene.h"
#include "Game/Player/PlayerTypes.h"
#include "Game/World/FacilityLayout.h"
#include "Game/World/LevelLights.h"
#include "Game/World/WorldObjects.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <string>
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
// The building's outside wall (FacilityMap's shell): half an inside wall's thickness, from the cell's edge to a perimeter
// room's wall face; the shell outward from there, a skin of cladding over it, and a parapet round the roof capped with
// the same. And a way out's doorway.
inline constexpr float kHalfWall = 0.1f;
inline constexpr float kShellThickness = 0.38f;
inline constexpr float kCladding = 0.04f;
inline constexpr float kParapet = 0.9f;
inline constexpr float kCapHeight = 0.08f;
inline constexpr float kDoorWidth = 1.2f;
inline constexpr float kDoorHeight = 2.2f;
// A lift (FacilityLayout::Stairwell::Way::Lift, run by Lifts): its shaft's pit, under the lowest floor; its doorway on each floor,
// as wide as a door and a little taller; and its car, square, a little short of the shaft inside its walls at the back and the
// sides and up to the shaft's wall at the front, so that stepping in is stepping across a hand's width.
inline constexpr float kLiftPit = 0.3f;
inline constexpr float kLiftDoorWidth = 1.2f;
inline constexpr float kLiftDoorHeight = 2.25f;
inline constexpr float kLiftCarWidth = 2.4f;
inline constexpr float kLiftCarFloor = 0.12f;
inline constexpr float kLiftCarHeight = 2.5f;
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
            Fill,    // solid under the top of a flight of stairs
            Cladding, // the building's outside finish
            Sill,     // the threshold plate across a way out
            // And what dresses its outside (Blueprint::dressing), not the building itself:
            Glazing,  // a window's glass
            Trim,     // frames, bands, ribs, corners, canopies: FacilityLayout::Outside::trim
            Accent,   // the accent colour
            Kit,      // plant, vents, masts and dishes on the roof
            Roofing,  // a roof's top, covered (FacilityLayout::Outside::roofing): a slab's, so one of the pieces
            Foundation, // the outside wall's face under the ground, where it goes down past basements: concrete, not cladding
            // A room's furnishing (FacilityFurniture): its solid box, felt and not drawn, and what its detail is made of.
            Furniture,
            Wood,
            Cardboard,
            Fabric,
            Bedding,
            Steel,
            DarkSteel,
            Paint,
            Plastic,
            DarkPlastic,
            Screen,
            Porcelain,
            Mirror,
            Leaf,
            // Signs (FacilityMap's Signage): the letters on them, and a way out's, lit green.
            Lettering,
            ExitSign,
            // What has become of a building (FacilityCondition): litter, soot, frost and rubble.
            Paper,
            Soot,
            Frost,
            Rubble
        };
        Kind kind = Kind::Wall;
        glm::vec3 centre{0.0f};
        glm::vec3 size{1.0f};
        float yaw = 0.0f;

        // The building itself -- floors, ceilings, walls, its shell -- which is meant to join up: a wall reaches a
        // hair into the slabs over and under it, the shell covers every slab's edge. What is in it has to keep clear.
        bool Structural() const
        {
            return kind == Kind::Floor || kind == Kind::Ceiling || kind == Kind::Wall || kind == Kind::Duct || kind == Kind::Fill ||
                   kind == Kind::Cladding || kind == Kind::Sill || kind == Kind::Roofing || kind == Kind::Foundation;
        }
    };

    // Something on the building's outside (FacilityLayout::Outside): a window, a band, a canopy, what stands on its roof. Drawn
    // only -- nothing to collide with -- and kept apart from the pieces, which are the building itself; each a little proud
    // of the cladding or standing on the roof, never into what is inside.
    struct Dressing
    {
        enum class Form : uint8_t
        {
            Box,      // size: as a piece's
            Cylinder, // upright: size.x across, size.y tall
            Cone,     // upright, narrowing: size.x across at its foot, size.z at its top, size.y tall
            Dome,     // half a sphere on its flat side: size.x across, size.y tall
            Dish,     // a shallow bowl opening along its +y: size.x across, size.y deep
            Ball,     // round: size across, tall and deep
            Text,     // `text` in block letters (Lettering) size.y high, flat, facing out of its front
            Bend,     // a pipe's quarter turn (FacilityOutside::PipeBend): size.x its radius, size.y the pipe across; the corner at `centre`
            // A patch laid on a face, ragged-edged (`shape` its seed), flat, facing out of its front: size.x across, size.y high, in
            // `tint` at its middle fading over size.z of the way to none at its edge (0: not at all, a solid shape) -- drawn in the face's
            // own material, it is a stain on it (FacilityCondition)
            Patch
        };
        Form form = Form::Box;
        Piece::Kind kind = Piece::Kind::Kit;
        glm::vec3 centre{0.0f}; // the middle of a box or a cylinder; the middle of a dome's or a dish's rim
        glm::vec3 size{1.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        std::string text; // what a Text says
        glm::vec3 tint{1.0f};  // a Patch's, at its middle
        uint32_t shape = 0;    // and the seed of its edge
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
        // A doorway's share of another lamp (this one's index in the list), with no fitting: see
        // LevelLights::AddSpill. -1 for a lamp of its own.
        int spillOf = -1;
        // The same lamp again, bounded to a corridor that turns off the run it hangs over: -1 for a lamp of
        // its own. Without it the light stopped in a hard line across the mouth of the side corridor.
        int copyOf = -1;
        glm::vec3 direction{0.0f, -1.0f, 0.0f};
    };

    // A lift's shaft (FacilityLayout::Stairwell::Way::Lift), in the world, for Lifts to run: its car, its doors and its buttons
    // are Lifts', what stands still round them the building's.
    struct Lift
    {
        glm::vec3 centre{0.0f};          // the middle of the shaft, at its lowest stop's floor
        glm::vec3 out{0.0f, 0.0f, 1.0f}; // flat and unit length: from the shaft out through its doors
        std::vector<float> stops;        // the floor's level at each stop, the lowest first
        int lowest = 0;                  // the building's floor its lowest stop is on
        int ground = 0;                  // and its ground floor, for naming them
        int building = 0;                // which of a site's buildings (SiteMap's)
        int lamp = -1;                   // the light in its car (LevelLights, SiteMap's), moved with it
    };

    // Everything the facility is, without an engine: for building it, and for testing that it fits
    // together.
    struct Blueprint
    {
        std::vector<Piece> pieces;
        std::vector<Dressing> dressing;
        // And inside, the rooms' furnishing in its detail (FacilityFurniture): drawn as the dressing is.
        std::vector<Dressing> furnishing;
        // What fills each window open through the wall (FacilityOutside::Windows): nothing drawn, only felt -- nobody climbs out of
        // one, or in.
        std::vector<Piece> panes;
        std::vector<Flight> flights;
        // Up through the hatches, in the world: the player's (PlayerController::SetLadders).
        std::vector<Ladder> ladders;
        std::vector<Lift> lifts;
        std::vector<Lamp> lamps;
        WorldObjects::Placements placements;
        glm::vec3 spawn{0.0f};
        float spawnYaw = 0.0f;
    };

    static constexpr int kSteps = 18;
    static constexpr float kStepRun = 0.28f;
    // As wide as the stairwell (a cell) inside its walls: as wide as the way in.
    static constexpr float kStairWidth = FacilityLayout::kCell - 0.22f;

    static Blueprint Draw(const FacilityLayout& layout);

    // Where a point given in cells, on a floor, is in the world, at that floor's level: for the facility in its
    // usual place, or for any building, wherever it stands.
    static glm::vec3 ToWorld(int floor, glm::vec2 cells);
    static glm::vec3 ToWorld(const FacilityLayout& layout, int floor, glm::vec2 cells);
    // Where something the plan put against a wall stands, at the foot of it: `depth` deep, its back a couple of
    // centimetres off the wall's face. It faces its own -z turned by the thing's yaw: (-sin, 0, -cos).
    static glm::vec3 PlacedAgainstWall(const FacilityLayout& layout, const FacilityLayout::Placed& thing, float depth);
    // Just outside one of a building's ways out, `distance` metres beyond its outer wall, at ground level.
    static glm::vec3 ExitOutside(const FacilityLayout& layout, const FacilityLayout::Exit& exit, float distance);

    // Replaces whatever facility was built before with the one planned from `seed`.
    void Build(uint16_t seed, Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, LevelLights* lights);
    // Or with a building already planned -- one of a site's -- its meshes named with `prefix` so that two
    // buildings in one world keep their own. Its walls and floors are one structure (PhysicsWorld::SetOverlapGroup):
    // `structure` if given -- a site's ground, which its buildings are set into -- or a group of its own.
    void Build(FacilityLayout layout, uint32_t seed, Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics,
               LevelLights* lights, const std::string& prefix = {}, uint32_t structure = 0);
    void Clear(Scene& scene, PhysicsWorld& physics, LevelLights* lights);

    bool Built() const { return m_built; }
    uint16_t Seed() const { return m_seed; }
    const FacilityLayout& Layout() const { return m_layout; }
    const WorldObjects::Placements& Placements() const { return m_placements; }
    glm::vec3 Spawn() const { return m_spawn; }
    const std::vector<Ladder>& Ladders() const { return m_ladders; }
    const std::vector<Lift>& Lifts() const { return m_lifts; }
    float SpawnYaw() const { return m_spawnYaw; }

private:
    FacilityLayout m_layout;
    WorldObjects::Placements m_placements;
    glm::vec3 m_spawn{0.0f};
    std::vector<Ladder> m_ladders;
    std::vector<Lift> m_lifts;
    float m_spawnYaw = 0.0f;
    uint16_t m_seed = 0;
    bool m_built = false;
    std::vector<Entity> m_entities;
    // The meshes it uploaded, given back when it is taken away (MeshLibrary::Release), and where they were uploaded to.
    std::vector<MeshHandle> m_meshHandles;
    MeshLibrary* m_meshLibrary = nullptr;
    std::vector<BodyHandle> m_bodies;
    size_t m_firstLight = 0;
    bool m_hasLights = false;
};

} // namespace pred
