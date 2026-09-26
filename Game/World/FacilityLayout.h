#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace pred
{

// A facility, planned from a seed: which cells of each floor are rooms, which are corridor, which are
// solid; where the doors are and which are locked; where the stairs join the floors; which rooms a
// crawlspace duct joins; and where the lamps, the lockers, the supplies and the way in go.
//
// Only a plan: no meshes, no bodies, nothing that needs the engine. FacilityMap builds it. Every
// machine plans the same facility from the same seed, so only the seed is ever sent.
//
// The shape of it, by design:
// - non-linear: rooms joined by corridors that loop, so there is always more than one way round and
//   something coming one way can be avoided by going the other;
// - several floors, joined by more than one stairwell;
// - crawlspace ducts between some rooms, a person's-height-and-a-half below the ceiling -- somewhere
//   to hide, and somewhere small creatures travel;
// - locked doors only where locking them cuts nothing off, and a keycard somewhere it can be reached.
struct FacilityLayout
{
    // How big a cell is, and how tall a floor.
    static constexpr float kCell = 2.5f;
    static constexpr float kStorey = 3.6f;     // floor to floor
    static constexpr float kClearHeight = 3.0f; // floor to ceiling
    static constexpr float kSlab = 0.6f;       // floor and roof thickness: see LabMap's kRoof
    static constexpr float kDuctHeight = 1.2f; // crawlspace headroom: crouched or lying down, not standing
    static constexpr float kDuctWidth = 1.4f;  // and across: a shaft, not a corridor

    enum class Cell : uint8_t
    {
        Solid,    // nothing: behind walls
        Room,
        Corridor,
        Stair,    // the stairwell, on the floor it climbs from and the one it arrives at
        Duct      // crawlspace
    };

    enum class RoomKind : uint8_t
    {
        Office,  // small
        Store,   // small, shelves
        Lab,     // medium, benches
        Hall,    // large, pillars
        Plant    // machinery, low light
    };

    struct Room
    {
        int floor = 0;
        glm::ivec2 min{0};  // cells, inclusive
        glm::ivec2 max{0};  // cells, inclusive
        RoomKind kind = RoomKind::Office;
        bool dark = false;  // its lights are out
    };

    // A doorway on the edge between two cells: `cell` and the one next to it in `side` (0 +x, 1 +z).
    struct Door
    {
        int floor = 0;
        glm::ivec2 cell{0};
        int side = 0;
        bool hasDoor = true; // otherwise an open archway
        bool locked = false;
        int room = -1;       // the room it opens into
    };

    struct Stairwell
    {
        int floor = 0;          // the floor it climbs from; it arrives at floor + 1
        glm::ivec2 min{0};      // two cells wide, three long
        glm::ivec2 max{0};
        bool alongX = false;    // which way the flight runs
        bool rising = true;     // up towards +axis, or towards -axis
    };

    // A run of crawlspace, as the cells it goes through. Ducts join rooms to rooms, rooms to corridors,
    // and each other, so together they are a network: somewhere to hide, a way round, and the way small
    // creatures get about unseen. Where one opens into a room or a corridor is a mouth -- a low opening
    // at the foot of the wall -- given as the duct cell and the open cell beside it. Duct cells next to
    // each other are always joined.
    struct Mouth
    {
        glm::ivec2 duct{0};
        glm::ivec2 open{0};
    };
    struct Duct
    {
        int floor = 0;
        std::vector<glm::ivec2> cells;
        std::vector<Mouth> mouths;
        int fromRoom = -1;
        int toRoom = -1; // -1 when it runs to a corridor or into another duct
    };

    enum class LampMood : uint8_t
    {
        Steady,
        Flicker,
        Failing,
        Dead,
        Emergency
    };

    struct Lamp
    {
        int floor = 0;
        glm::vec2 at{0.0f}; // cells, fractional
        LampMood mood = LampMood::Steady;
        int circuit = 0;
    };

    enum class Thing : uint8_t
    {
        Locker,
        Supplies,   // a place a few items are left
        AmmoCrate,
        Keycard,
        Crate,      // cover: a box on the floor
        Shelf,      // a tall shelf against a wall
        Bench,      // a waist-high bench
        Pillar
    };

    struct Placed
    {
        Thing thing = Thing::Crate;
        int floor = 0;
        glm::vec2 at{0.0f}; // cells, fractional
        float yaw = 0.0f;   // radians; for something against a wall, facing out of it
        glm::vec2 size{1.0f}; // metres across and deep, for cover
        float height = 1.0f;
    };

    uint32_t seed = 0;
    int floors = 2;
    int width = 24; // cells along x
    int depth = 24; // cells along z
    std::vector<Cell> cells; // floors * depth * width
    std::vector<int> roomOf; // the room each cell belongs to, or -1
    std::vector<Room> rooms;
    std::vector<Door> doors;
    std::vector<Stairwell> stairwells;
    std::vector<Duct> ducts;
    std::vector<Lamp> lamps;
    std::vector<Placed> things;
    int entranceRoom = -1;   // where everybody comes in
    int nestRoom = -1;       // dark and out of the way: somewhere a creature would build

    static FacilityLayout Generate(uint32_t seed);

    Cell At(int floor, int x, int z) const;
    int RoomAt(int floor, int x, int z) const;
    bool Open(int floor, int x, int z) const; // anywhere a person can stand, not counting ducts
    size_t Index(int floor, int x, int z) const
    {
        return (static_cast<size_t>(floor) * static_cast<size_t>(depth) + static_cast<size_t>(z)) * static_cast<size_t>(width) +
               static_cast<size_t>(x);
    }

    // Everywhere a person can reach from the entrance, walking and climbing stairs, through the doors
    // that are not locked (or through all of them); as a set of cells. For checking a plan.
    std::vector<bool> Reachable(bool throughLocked) const;
};

} // namespace pred
