#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <string>
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
    // Three metres: a corridor a little under three wide inside its walls, rooms to match.
    static constexpr float kCell = 3.0f;
    static constexpr float kStorey = 3.6f;     // floor to floor
    static constexpr float kClearHeight = 3.0f; // floor to ceiling
    static constexpr float kSlab = 0.6f;       // floor and roof thickness: see LabMap's kRoof
    static constexpr float kDuctHeight = 1.2f; // crawlspace headroom: crouched or lying down, not standing
    static constexpr float kDuctWidth = 1.2f;  // and across, the same: a square duct, the one size all along it, not a corridor

    enum class Cell : uint8_t
    {
        Solid,    // nothing: behind walls
        Room,
        Corridor,
        Stair,    // the stairwell, on the floor it climbs from and the one it arrives at
        Duct,     // crawlspace
        Outside,  // not part of the building at all: past its walls, beside a wing or over a lower roof
        Void      // the upper half of a hall two storeys high (Room::tall): open down to its floor, walled as the hall is
    };

    enum class RoomKind : uint8_t
    {
        Office,  // small
        Store,   // small, shelves
        Lab,     // medium, benches
        Hall,    // large, pillars
        Plant    // machinery, low light
    };

    // What a room is for (you, 2026-10-26: what a place is for should be what it has in it). Every room is given one by the
    // building's program (Options::program) -- the way in a lobby, the kitchen by the mess, washrooms near the quarters, the
    // plant under the ground where there is a floor down there -- and is furnished as that (Fitted).
    enum class Purpose : uint8_t
    {
        None,
        Lobby,     // where everybody comes in: a reception desk, seats, a screen of notices
        Office,    // desks with their chairs and screens, filing cabinets
        Control,   // consoles along the walls and a big screen: where the place is run from
        Lab,       // benches with things on them, a fume hood, a sink
        Quarters,  // where people sleep: beds or bunks, a desk
        Mess,      // where people eat: tables and benches, a serving counter
        Kitchen,   // counters along the walls, a cooker, a sink, a fridge
        Washroom,  // basins, toilets in their stalls, showers
        Medbay,    // beds, a cabinet, a basin
        Store,     // shelving with boxes on it, a pallet
        Workshop,  // workbenches with vices and tools, drums, shelving
        Plant,     // the machinery that keeps it going: generators, pumps, drums
        Servers,   // racks of computers in rows, a console
        Warehouse, // racking in rows, pallets
        // On the sea's floor (Program::Subsea):
        DiveLocker,    // where everybody comes in, by the moon pool: suits on racks, lockers, a bench
        Decompression, // a chamber to come back up from the deep in, its controls
        PumpRoom,      // what keeps the sea out: pumps, a generator, drums
        WetLab         // specimen tanks along its walls, benches, a sink
    };
    // What a building is for, which rooms it has (SitePlan, by what the place is).
    enum class Program : uint8_t
    {
        Generic,
        Research,
        Industrial,
        Depot,
        Living,
        Comms,
        Observatory,
        Field,
        Subsea // on the sea's floor: its own rooms -- a dive locker by its moon pool, a decompression chamber, a pump room, wet labs
    };
    static const char* PurposeName(Purpose purpose);

    // A room's furnishing, one piece to a cell: what it is, where (cells, fractional; against a wall as Placed is, facing out
    // of it) and which way it faces. Each a solid box the size of what it is (FittingSize) for anything walking or
    // shooting, drawn in its detail (FacilityFurniture).
    enum class Fitting : uint8_t
    {
        Reception,
        Seat,      // a bench seat against a wall
        PlantPot,
        Noticeboard,
        Desk,      // with its chair and a screen on it
        Cabinet,   // filing
        Console,   // a control desk, angled, its screens on it, a chair at it
        Wallscreen,
        LabBench,  // with what is being worked on, on it
        FumeHood,
        Bed,
        Bunk,
        Table,     // with a bench either side
        Counter,
        Cooker,
        Fridge,
        Basin,     // with a mirror over it
        Toilet,    // in its stall
        Shower,
        MedBed,
        Shelving,  // with boxes on it
        Pallet,    // stacked with crates
        Workbench, // with a vice and a board of tools over it
        Drum,
        Generator,
        Pump,
        Rack,      // a server rack
        Racking,   // a bay of warehouse racking, loaded
        Terminal,  // a plain bench, nothing on it: where a mission's terminal stands (Mission)
        Mirror,    // on a wall, over nothing: a quarters' (a washroom's are over its basins)
        // On the sea's floor (Program::Subsea):
        SuitRack,  // diving suits hanging on a rail, their helmets on a shelf over them, fins under
        Chamber,   // a decompression chamber: a steel cylinder lying on its saddles, a round door at one end, a viewport in its side
        Tank       // a specimen tank: glass on a steel stand, lit, with water in it
    };
    struct Fitted
    {
        Fitting kind = Fitting::Desk;
        int floor = 0;
        glm::vec2 at{0.0f};
        float yaw = 0.0f;
        bool wall = true; // stood against a wall (at is the cell's, pushed to it) rather than out in the room
    };
    // How much room one takes: across, deep (from the wall it stands against, if it does) and high, metres.
    static glm::vec3 FittingSize(Fitting kind);

    struct Room
    {
        int floor = 0;
        glm::ivec2 min{0};  // cells, inclusive
        glm::ivec2 max{0};  // cells, inclusive
        RoomKind kind = RoomKind::Office;
        bool dark = false;  // its lights are out
        Purpose purpose = Purpose::None;
        bool tall = false;  // two storeys high, open up through the floor over it (Cell::Void there): a hall to feel the size of
        bool clean = false; // a lab kept clean: an airlock built inside its one door (FacilityMap; Planner::ChooseCleanRoom)
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
        glm::ivec2 min{0};      // a flight's one cell wide, three long; a ladder's one cell
        glm::ivec2 max{0};
        bool alongX = false;    // which way the flight runs
        bool rising = true;     // up towards +axis, or towards -axis
        // A flight of stairs, or a ladder up the back wall of a cell through a hatch in the floor above, its way in on both floors
        // at the low end, where a flight's starts; or a lift, its shaft one cell up through every floor from `floor` to
        // `floor + levels`, its doors at the low end on each.
        enum class Way : uint8_t
        {
            Flight,
            Ladder,
            Lift
        };
        Way way = Way::Flight;
        int levels = 1; // how many floors up from `floor` it goes: one, but for a lift
        int Top() const { return floor + levels; }
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
        Pillar,
        Breaker     // the panel on a wall the building's power is switched at: one a building
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

    // A doorway out of the building, in its outer wall on the ground floor: the cell inside it, and which
    // way out it faces (0 towards -x, 1 +x, 2 -z, 3 +z).
    struct Exit
    {
        glm::ivec2 cell{0};
        int side = 0;
        int room = -1; // the room it opens into, or -1 for a corridor
        // A vehicle bay's (Planner::AddBay): wide and high enough to drive through, a roller shutter in it, into a workshop or a store.
        bool bay = false;
        // Joined by a covered way to another building's (SitePlan::Tube): no canopy over it, the tube built against it.
        bool tubed = false;
    };

    // Its shape from above and how tall each part of it is: a plain block, or wings -- an L, a T, a U, an H, a cross, a
    // ring round a courtyard -- or a block stepping back as it rises, or a tower over a lower hall. Some wings lower than
    // the rest. The rooms and corridors are planned inside whatever shape it is.
    enum class Massing : uint8_t
    {
        Block,
        L,
        T,
        U,
        H,
        Cross,
        Court,
        Stepped,
        Tower,
        Buried // mostly under the ground: its floors there under the whole of it, an entrance block over one side (an earth mound over the rest)
    };

    // How big a building to plan, and how it meets the outside. The defaults are the facility as it
    // always was: one building on its own, entered from inside.
    struct Options
    {
        int width = 24;
        int depth = 24;
        int minFloors = 2;
        int maxFloors = 3;
        // Doorways out of it, and which side the first faces (-1 whichever); the rest are on other sides.
        int exits = 0;
        int exitSide = -1;
        // And where along that side the first is (its cell, counted from the lowest), where a way out can be there; -1 anywhere.
        int exitAlong = -1;
        // The fewest cells across it may be either way (seven at the least: an outpost's smaller buildings); asked for fewer, it is this.
        int least = 10;
        // Floors under the ground (basements), below the others: reached by the stairs, no way out of them but up.
        int basements = 0;
        Massing massing = Massing::Block;
        // A doorway out in its wall facing this way a floor under the ground, into a passage to another building (-1: none).
        int tunnelSide = -1;
        // What it is for, which rooms it has.
        Program program = Program::Generic;
        // A mine's: a doorway out of its lowest floor through its outside wall, into the rock (caves) -- in the wall facing
        // `caveSide` only, where that is given (a building joined to a cave already there), or whichever it can be.
        bool cave = false;
        int caveSide = -1;
        // On the sea's floor (a water world's: you, 2026-11-03): no way out through its walls, but a moon pool in the room everybody
        // comes in by, open to the sea under it (FacilityLayout::moonPool).
        bool moonPool = false;
        // Half fallen in (a building left long ago, or burnt): a corner of it come down, cut out of it before anything in it is
        // planned (FacilityLayout::ruin).
        bool ruined = false;
        glm::vec3 origin{64.0f, 0.0f, -30.0f};
    };

    uint32_t seed = 0;
    // Where the corner of cell (0, 0) is, at ground-floor level: a building can be put anywhere.
    glm::vec3 origin{64.0f, 0.0f, -30.0f};
    int floors = 2;
    // Which of them is at the ground (0, the lowest, unless it has basements under it): the ways out are on it, and `origin`
    // is the lowest floor's level, under the ground by as many storeys as there are basements.
    int ground = 0;
    // Its workshops', stores' and warehouses' doors heavy ones, as well as its machinery's: where the place is worked (an industrial
    // or depot program).
    bool heavyDoors = false;
    // A mine's: the floor (-1: none) that is its workings -- drives and galleries cut in the rock, timbered, rails along them
    // (FacilityMap), not rooms finished as a building's.
    int workings = -1;
    float GroundLevel() const { return origin.y + static_cast<float>(ground) * kStorey; }
    int width = 24; // cells along x
    int depth = 24; // cells along z
    std::vector<Cell> cells; // floors * depth * width
    std::vector<int> roomOf; // the room each cell belongs to, or -1
    std::vector<Room> rooms;
    std::vector<Door> doors;
    std::vector<Stairwell> stairwells;
    std::vector<Duct> ducts;
    // Where a duct on an upper floor runs over a room or a corridor of the floor under it: a grille in that room's ceiling, through
    // the floor between (Drop::cell, on Drop::floor, the duct's) -- dropped through from the duct, and come up into by whatever can
    // climb a wall. A way between the floors that is not the stairs.
    struct Drop
    {
        int floor = 0;          // the duct's floor; the room is on the one under it
        glm::ivec2 cell{0};
    };
    std::vector<Drop> drops;
    std::vector<Lamp> lamps;
    std::vector<Placed> things;
    std::vector<Fitted> fittings;
    std::vector<Exit> exits;
    // Doorways out a floor under the ground (the floor just under `ground`), into a passage to another building (SitePlan).
    std::vector<Exit> tunnels;
    // And on its lowest floor, a mine's (Options::cave), a doorway out through the outside wall into the rock -- an adit to a cave
    // (SitePlan::Cave), nothing of the building past it that way.
    std::vector<Exit> caves;
    int entranceRoom = -1;   // where everybody comes in
    int lairRoom = -1;       // dark and out of the way: where a creature living here has its lair (SiteFauna)
    // On the sea's floor (Options::moonPool): the cell of its lowest floor, in the room everybody comes in by, that is a pool open to
    // the sea under the building -- its way in and out, swum up into and climbed out of by a ladder (none: -1).
    glm::ivec2 moonPool{-1};
    bool HasMoonPool() const { return moonPool.x >= 0; }

    // What has become of it (SitePlan, by its seed): looked after; left long ago -- stripped of much that was in it, its lamps
    // mostly out, rubble and litter about; left in a hurry -- what people had about them dropped where they were; or burnt out,
    // its walls and ceilings sooted. And over that, by the world it stands on (SiteMap): frozen, where it is cold and nobody
    // keeps it warm; overgrown, where things grow and nobody keeps them out. Drawn by FacilityCondition.
    enum class Condition : uint8_t
    {
        Kept,
        Abandoned,
        Hurried,
        Burnt
    };
    Condition condition = Condition::Kept;
    bool frozen = false;
    bool overgrown = false;
    // And flooded, where water got in and nobody pumps it out: every storey from the lowest up to this one under water, standing
    // `floodDepth` metres deep over this one's floor (-1: dry). The tide line it leaves on its walls drawn by FacilityCondition;
    // its water by SiteMap, which says how deep it is where (SiteMap::Flood), to wade and swim in.
    int flooded = -1;
    float floodDepth = 0.0f;

    // How it looks from outside (SitePlan sets it, by what the place is for): the colour of its cladding and what that is
    // made of, its windows, bands, ribs and canopies, and what stands on its roof. Only how it looks -- nothing here
    // changes what is inside or where anybody can go. The default is the plain grey box every building was.
    struct Outside
    {
        enum class Windows : uint8_t
        {
            None,
            Strip,   // a band of glass along each floor
            Punched, // a window to a bay
            Tall     // a tall window to a bay, a bar across it
        };
        glm::vec3 cladding{0.24f, 0.25f, 0.25f};
        std::string surface = "building_cladding";
        glm::vec3 trim{0.16f, 0.17f, 0.18f};
        glm::vec3 accent{0.62f, 0.46f, 0.16f};
        Windows windows = Windows::None;
        int windowEvery = 1;     // a window to every bay, every second... (or every panel between ribs)
        float windowSize = 1.0f; // how big its punched and tall windows are: smaller where it is cold, or hot and bright
        bool bands = false;      // a band round it at every floor
        int ribEvery = 0;        // an upright rib every so many bays (0: none)
        bool stripe = false;     // a band of the accent colour round its top
        bool canopies = false;   // a canopy over every way out
        // Every way out through an airlock (FacilityMap): a steel vestibule built out from the wall, its own door at its far end, the
        // two never open together -- where there is no air outside to breathe. No canopy then: the airlock's roof is over its door.
        bool airlocks = false;
        // What it is called (SitePlan: BLOCK A, BLOCK B...), in letters over its main way out -- lit, where that is how the way in
        // is marked (SitePlan::WayIn::LitEntrance).
        std::string label;
        bool labelLit = false;
        bool roofing = false;    // the roof covered, not bare slab
        int units = 0;           // plant boxes on the roof
        int vents = 0;           // vent pipes on the roof
        int stacks = 0;          // tall stacks on the roof, banded near their tops
        bool solar = false;      // rows of panels over the roof
        bool antenna = false;    // a mast on the roof
        int dishes = 0;          // dishes on the roof, turned up to the sky
        bool dome = false;       // a dome on the roof (an observatory's)
        // Built for the sea's floor (you, 2026-11-08: its own kind, equipped for the water): a pressure hull's painted steel, ring frames
        // round it, viewports; outside, racks of gas bottles strapped to its walls, anodes along its foot, cable trays under its roof;
        // on its roof a hatch, a sonar head, and now and then a viewing dome (FacilityOutside).
        bool subsea = false;
        // Standing on its own, as a site's buildings do: its walls carried up past its roof as a parapet, and its outside dressed. Not
        // inside the shell of another -- an outpost's building, whose parapet and dressing it has -- its walls stopping at its roof
        // and nothing of its outside drawn but the frames of its ways out.
        bool standalone = true;
        // How deep the snow is piled against the walls rising from the ground on each side (0 -x, 1 +x, 2 -z, 3 +z) at most, over
        // its ground floor: a building left to the cold on a frozen world, snowed under to its windows and over (SiteMap). Nought:
        // only what drifts against every building there.
        std::array<float, 4> drifted{};
    };
    Outside outside;

    // Set into a hillside (SiteMap, where its world's land is hilly): the land rising behind it, and earth banked against its wall
    // facing `side` (0 -x, 1 +x, 2 -z, 3 +z) to `height` metres over its ground floor -- and along the two walls either side of that
    // one from it, as high for `hold` metres, then falling away to nothing over `run`. SiteTerrain raises the land and draws the bank;
    // no window under it. None: side -1.
    struct Bank
    {
        int side = -1;
        float height = 0.0f;
        float hold = 0.0f;
        float run = 0.0f;
    };
    Bank bank;
    // Half fallen in (Options::ruined): a corner of it come down to the ground, cut out of it before anything in it was planned --
    // what is left stands whole, walled off where it broke -- and drawn as its ruin (FacilityMap): the stumps of its walls on the line
    // they stood on, its floor broken, the ends of its floors jutting from the wall left standing, slabs fallen, rubble. The cells it
    // covered, corner to corner (inclusive), and how many storeys each had (0: none, it was not part of it).
    struct Ruin
    {
        glm::ivec2 min{0};
        glm::ivec2 max{-1};
        std::vector<int> heights;
        // Where it broke into what still stands: the outside wall of a cell (`cell`, a standing one) facing the ruin (`side`) broken
        // through on a floor -- a ragged hole into the room or corridor behind it (FacilityMap's Shell cuts it, FacilityRuin rags its
        // edges), one at the least on the ground floor to get in by. And floors near it fallen through into the storey under them
        // (Cell::Void there, as a tall hall's upper half is): each its cell and floor, (x, floor, z).
        struct Breach
        {
            glm::ivec2 cell{0};
            int side = 0;
            int floor = 0;
            float low = 0.0f;  // the hole from so high over that floor
            float high = 2.6f; // to so high
            float from = 0.25f; // and along the wall from so far into the cell (from its low end)
            float to = 2.75f;   // to so far
        };
        std::vector<Breach> breaches;
        std::vector<glm::ivec3> fallen;
        // And walls inside near it broken through, where there was no doorway: each on a floor, by the cell on its low side and which way
        // the edge runs (0: to the cell at +x, 1: to the one at +z) -- a ragged hole from the floor to `high` (FacilityMap cuts it,
        // FacilityRuin rags it).
        struct Broken
        {
            int floor = 0;
            glm::ivec2 cell{0};
            int side = 0;
            float high = 2.4f;
            float low = 0.0f; // (what is left standing of it under the hole: 0 broken right down, to walk through)
            float from = 0.45f; // (along the wall, from so far into the cell to so far)
            float to = 2.55f;
        };
        std::vector<Broken> broken;
        bool Has() const { return !heights.empty(); }
        int Height(int x, int z) const
        {
            if (heights.empty() || x < min.x || z < min.y || x > max.x || z > max.y)
            {
                return 0;
            }
            return heights[static_cast<size_t>((z - min.y) * (max.x - min.x + 1) + (x - min.x))];
        }
    };
    Ruin ruin;
    // How high the bank stands against the outside of its walls at a place on the ground there (world x, z), over its ground floor:
    // against the wall it is banked against, and along those either side as far as it reaches (0: none, or the place well inside).
    float BankAt(glm::vec2 at) const;

    // How many storeys each cell has, from the lowest (basements and all) -- `floors` for the tallest part, fewer for a lower
    // wing, 0 for a cell not part of the building: width * depth of them.
    std::vector<int> heights;
    int Height(int x, int z) const;

    // The building's outside wall, in straight runs along the edges of its cells: which way a run faces (0 -x, 1 +x, 2 -z,
    // 3 +z), the edge it stands on (an x for sides 0 and 1, a z for 2 and 3, in cells), the cells along it it runs past, and
    // the storeys it is the outside of -- from `low` (0, the ground under it; more where it rises from a lower roof) to
    // `high`, the top of the part behind it, over which its parapet stands.
    struct OuterWall
    {
        int side = 0;
        int line = 0;
        int from = 0;
        int to = 0;
        int low = 0;
        int high = 0;
    };
    std::vector<OuterWall> OuterWalls() const;
    // And every outside corner of it (where two of those turn round a part standing out): the corner of cells it is at, which
    // way the outside is from it (-1 or 1 on x and on z) and the storeys it is a corner for.
    struct OuterCorner
    {
        glm::ivec2 at{0};
        glm::ivec2 out{0};
        int low = 0;
        int high = 0;
    };
    std::vector<OuterCorner> OuterCorners() const;

    static FacilityLayout Generate(uint32_t seed, const Options& options = {});

    Cell At(int floor, int x, int z) const;
    int RoomAt(int floor, int x, int z) const;
    // The stairwell (its index) a cell is part of on a floor -- a flight's, a ladder's or a lift's shaft -- or -1.
    int WellAt(int floor, int x, int z) const;
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
