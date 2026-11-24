#pragma once

#include "Game/World/SitePlan.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace pred
{

// What a deployment to a site is for: the data on a terminal in one of its buildings, downloaded onto a drive, and the
// drive carried back to the shuttle and taken off the site in it.
//
// Sometimes harder: the power in the terminal's building is out until somebody finds its breaker panel and resets it,
// and sometimes the briefing comes without a map of the site. The shuttle leaves when somebody aboard launches it, a
// short while after, with whoever is aboard; anybody else is left behind.
//
// Only plans and rules here: nothing that needs the engine, so all of it can be tested. MissionProps puts the terminal,
// the panels and the launch console in the world, and PredationGame runs it (PredationGameMission.cpp) -- the host,
// that is, which decides everything and tells everybody how it stands.

namespace MissionSpec
{
// The terminal standing on its bench, the breaker panel on its wall, and how high the panel's middle is.
inline constexpr glm::vec3 kTerminalSize{0.56f, 0.42f, 0.36f};
// A survey station's stand, down a cave (MissionPlan::cave): a steel field table, its top this high and this big.
inline constexpr float kStandHeight = 0.9f;
inline constexpr glm::vec3 kStandTop{1.1f, 0.05f, 0.7f};
inline constexpr glm::vec3 kBreakerSize{0.6f, 0.8f, 0.14f};
inline constexpr float kBreakerHeight = 1.2f; // its middle, up the wall: a board at a hand's height, as fuse boards are
// From somebody pressing launch to the shuttle leaving.
inline constexpr float kLaunchSeconds = 20.0f;
// How long the result is shown before everybody is back aboard the ship.
inline constexpr float kResultSeconds = 8.0f;
// A survey (MissionPlan::Kind::Survey), set up by hand (Phase 6): its markers come in a crate in the shuttle, and each is carried
// out to its point, planted, its three legs kicked out, levelled with its bubble (each leg's length one of kLegSteps) and switched
// on. Readings are taken while at least kMarkersNeeded are working at once (they triangulate): each working marker's in this long.
inline constexpr int kMaxMarkers = 6;
inline constexpr int kMarkerLegs = 3;
inline constexpr int kLegSteps = 4;
inline constexpr int kMarkersNeeded = 3;
inline constexpr float kMarkerReadSeconds = 40.0f;
// A server rack's drive bay (Phase 6, data got out by hand): on the rack's face at chest height, this wide, high and deep; and how
// long an archive takes to copy onto the copy unit.
inline constexpr glm::vec3 kBaySize{0.48f, 0.18f, 0.16f};
inline constexpr float kBayHeight = 1.25f;
inline constexpr float kArchiveSeconds = 90.0f;
// A plant got running as a chain (Phase 6): its generator's filler on a wall (a tank with its cap), its hand pump (strokes of its lever
// to prime it) and its valves (four handwheels on a manifold), each on the wall over a bench in a room of their own; and the fuel can
// brought to the filler.
inline constexpr glm::vec3 kFillerSize{0.5f, 0.36f, 0.22f};
inline constexpr glm::vec3 kPumpSize{0.22f, 0.42f, 0.16f};
inline constexpr glm::vec3 kManifoldSize{0.9f, 0.3f, 0.12f};
inline constexpr int kPrimeStrokes = 8;
inline constexpr int kPlantValves = 4;
// A beacon (Phase 6): its parts -- mast, dish, power cell -- fitted in that order at its place; its dish turned in steps of 15
// degrees round and 10 up, to the bearing on its plate. And samples kept cold: so many cores, each so many turns of the drill, and how
// long the cold case keeps them cold away from the shuttle (back aboard it is chilled again, four times as fast).
inline constexpr int kBeaconParts = 3;
inline constexpr int kAimSteps = 24;
inline constexpr int kTiltSteps = 6;
inline constexpr int kSampleSites = 3;
inline constexpr int kDrillTurns = 4;
inline constexpr float kColdSeconds = 300.0f;
// The kinds of part for the ship's upgrades that are found on sites (Phase 8; armoury.json's "parts"), and how often the optional find
// is one of them rather than a records drive.
inline constexpr int kPartKinds = 3;
inline constexpr float kPartChance = 0.35f;
// A signal's transmitter (MissionPlan::Kind::Signal): a field case on the ground, and the whip aerial up from it.
inline constexpr glm::vec3 kTransmitterSize{0.7f, 0.45f, 0.5f};
inline constexpr float kAerialHeight = 2.2f;
// A breaker board's breakers, under its main switch; and all of them on, a bit each.
inline constexpr int kBoardBreakers = 6;
inline constexpr uint8_t kBoardAll = (1u << kBoardBreakers) - 1u;
// The terminal's supply box (Phase 5, repair): on the wall over its bench, its middle this far out from the wall; its fuses.
inline constexpr glm::vec3 kFuseBoxSize{0.36f, 0.52f, 0.1f};
inline constexpr int kFuses = 4;
inline constexpr uint8_t kFusesAll = (1u << kFuses) - 1u;
// A building's feeder cabinet (Phase 5, power routing): outside it, by a door, standing on the ground.
inline constexpr glm::vec3 kCabinetSize{0.8f, 1.25f, 0.38f};
inline constexpr float kCabinetPlinth = 0.15f;
} // namespace MissionSpec

struct MissionPlan
{
    // What it is (Phase 4's mission kinds): the data off a terminal and back to the shuttle; or a survey -- readings taken at markers
    // set out across the site (a survey site's), and back to the shuttle with them.
    enum class Kind : uint8_t
    {
        Download, // the data off a terminal, carried to the shuttle on a drive
        Restore,  // a station's: its power back and its life support restarted at its control console, then back to the shuttle
        Survey,   // readings at markers across the site
        Signal,   // a transmitter somewhere on the site, found and shut down (markers: its one), then back to the shuttle
        Activate, // a facility's: its power back if it is out, and its systems started at its control console, then back to the shuttle
        Revive,   // a signal gone dead: its transmitter found and brought back on the air (markers: its one), then back to the shuttle
        Explore,  // no work there (you, 2026-11-13: missions only on contracts): the place looked round, what it has gathered, and back
        Beacon,   // survey ground's, now and then (Phase 6): a beacon carried up to its highest ground in parts, put together and aimed
        Samples   // survey ground's, now and then (Phase 6): cores drilled at three outcrops into a cold case, back before it warms
    };
    // A place gone down to with no contract and no finding there: nothing to do but look round it, gather what it has, and go.
    static MissionPlan Exploring(uint32_t seed);
    Kind kind = Kind::Download;
    // Which the mission at a site is, from the site's own seed (the same before the site is made -- a contract's -- as after): a
    // station restored, survey ground read; a facility's data recovered or its systems got running; a signal shut down or
    // brought back. (About half of facilities and signals each way.)
    static Kind KindFor(uint32_t siteSeed);
    // Worked at a control console, its systems started in turn (a station's, a facility's).
    static bool AtConsole(Kind kind) { return kind == Kind::Restore || kind == Kind::Activate; }
    bool AtConsole() const { return AtConsole(kind); }
    // Worked at a transmitter out on the site (shutting it down, or bringing it back).
    static bool AtTransmitter(Kind kind) { return kind == Kind::Signal || kind == Kind::Revive; }
    bool AtTransmitter() const { return AtTransmitter(kind); }
    // A survey's markers, each where it stands (its foot, on the ground: the game sets its height from the ground there); or a signal's
    // transmitter, its one.
    std::vector<glm::vec3> markers;
    // Where the data is in a server rack rather than on a terminal's bench (Phase 6: data got out by hand) -- its drive bay on the
    // rack's face (`terminal` its middle, `terminalYaw` its way out) -- and how it comes out: pulled from the bay; its drive damaged,
    // fitted first with a controller board out of another rack (`donor`, that rack's service hatch; `donorYaw`); or an archive too
    // big for a drive, copied onto the copy unit brought from the shuttle and carried back in that.
    enum class Data : uint8_t
    {
        Terminal,
        Pull,
        Damaged,
        Archive
    };
    Data data = Data::Terminal;
    bool AtRack() const { return data != Data::Terminal; }
    glm::vec3 donor{0.0f};
    float donorYaw = 0.0f;
    // A plant to get running (Activate) as a chain rather than a switch (Phase 6): fuel brought from where it is kept (`fuelCan`, on a
    // bench) and poured into its generator's tank at its filler (`filler`), its pump primed by hand (`pump`), its valves set to the
    // line-up on their plate (`valves`; `lineUp`, which are to be open, a bit each; `valvesFound`, how they are found) -- each on the
    // wall over a bench of a room of its own, in any order -- and then the start at its console.
    bool chain = false;
    glm::vec3 fuelCan{0.0f};
    glm::vec3 filler{0.0f};
    float fillerYaw = 0.0f;
    glm::vec3 pump{0.0f};
    float pumpYaw = 0.0f;
    glm::vec3 valves{0.0f};
    float valvesYaw = 0.0f;
    uint8_t lineUp = 0;
    uint8_t valvesFound = 0;
    // A beacon's aim (Beacon): its dish's bearing, in steps of MissionSpec::kAimStep from north, and its elevation, in steps of
    // kTiltStep up from level -- what is on its plate. (Its place, markers[0], is the site's highest ground: the game finds it.)
    uint8_t aimBearing = 0;
    uint8_t aimTilt = 0;
    // Worked at markers (a survey's, a signal's) rather than at a terminal.
    bool AtMarkers() const { return kind == Kind::Survey || AtTransmitter(); }
    // Which markers (a bit each) already stand at their height -- on a room's floor indoors -- rather than out on the ground, which
    // the game stands the rest on.
    uint8_t heightSet = 0;
    uint32_t seed = 0;
    int building = -1; // which of the site's buildings the terminal is in
    int room = -1;     // and which of its rooms
    int floor = 0;
    glm::vec3 terminal{0.0f}; // the middle of the terminal, on its bench
    float terminalYaw = 0.0f; // which way its screen faces, as a thing is turned: (-sin, 0, -cos)
    // Where the drive comes out: on the bench, in front of the terminal.
    glm::vec3 driveAt{0.0f};
    bool powerOut = false;      // the terminal's building has no power to begin with
    std::vector<int> circuits;  // that building's lamps' circuits, which are out with it
    bool mapGiven = true;       // the briefing came with a map of the site
    // The breaker board, where the power is out: which of its breakers are found tripped (a bit each), and the one whose circuit
    // has a fault, to be found and left off (-1: none has).
    uint8_t boardTripped = 0;
    int boardFault = -1;
    float downloadSeconds = 45.0f;
    // Down a cave (SitePlan::caves; -1: not): the terminal a survey station on its own stand on a chamber's floor, on the power of
    // the mine the cave was broken into from (`building`).
    int cave = -1;
    // The terminal secured (Phase 5, codes): it asks for an access code -- four figures -- before it gives anything up, and the code
    // is on a note left on a bench in another room of the same building (`clue`, the note's middle on the bench top; `clueYaw`,
    // the bench's way). Nought: not secured.
    uint16_t code = 0;
    glm::vec3 clue{0.0f};
    float clueYaw = 0.0f;
    int clueRoom = -1; // (its room, and which floor: what the objective says once the terminal is found locked)
    int clueFloor = 0;
    int clueBuilding = -1; // (and which building: another than the terminal's where the site has one -- FIX 738)
    int fuelBuilding = -1; // (a chain's fuel can or fresh cartridge: in which building, where not the plant's own -- FIX 738)
    bool Secured() const { return code != 0; }
    // The terminal's supply faulted (Phase 5, repair and diagnosis): one of the four fuses in its supply box blown, so the terminal
    // is dead though its building has power -- until the box on the wall over its bench (`fuseBox`, its middle; `fuseBoxYaw`, its
    // way) is opened, its cover's screws out (a multitool), and the blown fuse found and the spare in its cover put in its place.
    // A blown fuse looks like any other: a circuit tester tells which. -1: not faulted.
    int blownFuse = -1;
    glm::vec3 fuseBox{0.0f};
    float fuseBoxYaw = 0.0f;
    bool Faulted() const { return blownFuse >= 0; }
    // What to take from the loadout locker for it (Phase 8: the briefing says it and the locker marks it), by item key: the tracker
    // always, the map where there is one, and the tools its faults want -- a multitool for a supply box's screws or a rack's board, a
    // circuit tester for a blown fuse or a faulted breaker.
    std::vector<std::string> KitWanted() const;
    // Where the power is out, now and then its feed cut as well (Phase 5, power routing): no supply coming into the building at
    // all, so its board brings nothing back until it is switched over to the standby feed at its feeder cabinet outside, by a door
    // (`cabinet`, its foot on the ground; `cabinetYaw`, its way) -- the cut main feed isolated first, or the changeover trips.
    bool feedCut = false;
    glm::vec3 cabinet{0.0f};
    float cabinetYaw = 0.0f;
    // An optional goal (Phase 5), now and then: a records drive left on a bench in another of the site's buildings (`extraBuilding`;
    // `extra`, where it lies on the bench top) -- brought back aboard, it pays; never needed to finish. -1: none.
    int extraBuilding = -1;
    glm::vec3 extra{0.0f};
    bool HasExtra() const { return extraBuilding >= 0; }
    // And now and then (Phase 8) what lies there is not a records drive but a part the ship's upgrades need (one of
    // MissionSpec::kPartKinds, armoury.json's "parts" in order): brought back, it is the ship's. -1: a records drive.
    int extraPart = -1;

    // The same mission from the same site and seed, on every machine.
    static MissionPlan Generate(const SitePlan& site, uint32_t seed);
    bool Valid() const { return building >= 0 || !markers.empty() || kind == Kind::Explore; }
};

// Where a building's breaker panel is (the middle of it, on its wall) and which way it faces, as a thing is turned.
// False for a building that has none.
bool BreakerPanel(const FacilityLayout& building, glm::vec3& at, float& yaw);

// How the mission stands. The host keeps it and sends it to everybody whenever it changes; it is all anybody needs to
// show the terminal, the panel, the console and the objective as they are.
struct MissionState
{
    enum class Stage : uint8_t
    {
        None,        // no mission: not at a site
        Find,        // the data is still on the terminal
        Downloading, // being copied onto a drive
        Carry,       // on a drive, to be taken to the shuttle
        Over         // the shuttle has gone
    };
    Stage stage = Stage::None;
    bool powered = true;     // the terminal's building has power
    float progress = 0.0f;   // of the download, 0 to 1
    bool attended = false;   // somebody is at the terminal, so it is copying
    float launchIn = -1.0f;  // counting down to the shuttle leaving; below 0 when nobody has launched it
    bool recovered = false;  // once it is over: whether the drive left with it
    uint8_t aboard = 0;      // once it is over: who was aboard, a bit a player
    // A survey's: the readings taken (a bit a marker).
    uint8_t readings = 0;
    // And its markers (Phase 6), how many, and each as it stands: `setup` (kPlanted, kLegOut a bit a leg, kOn, kFallen, kPacked);
    // each leg's length (two bits a leg, 0 to kLegSteps - 1) and how far down the ground under each foot is (two bits a leg, 0 to
    // 2: dealt as it is planted, and again whenever it is stood back up); and how far its reading has got, 0 to 1.
    struct Marker
    {
        uint8_t setup = 0;
        uint8_t legs = 0;
        uint8_t ground = 0;
        float read = 0.0f;
    };
    std::array<Marker, MissionSpec::kMaxMarkers> survey{};
    uint8_t markers = 0;
    static constexpr uint8_t kPlanted = 1;
    static constexpr uint8_t kLegOut = 2; // (shifted by which leg)
    static constexpr uint8_t kOn = 16;
    static constexpr uint8_t kFallen = 32;
    static constexpr uint8_t kPacked = 64;
    // A rack's drive bay (MissionPlan::AtRack): its cover open, a damaged drive's sled drawn out to its board, its board good, the copy
    // unit connected to its port; and the other rack's service hatch open and its board taken.
    uint8_t rack = 0;
    static constexpr uint8_t kRackCover = 1;
    static constexpr uint8_t kRackSled = 2;
    static constexpr uint8_t kRackBoard = 4;
    static constexpr uint8_t kRackUnit = 8;
    static constexpr uint8_t kDonorHatch = 16;
    static constexpr uint8_t kDonorTaken = 32;
    // A plant's chain (MissionPlan::chain): its filler's cap open, its tank fuelled, its valves set right (kept with them: the
    // objective's); how many strokes its pump has had (to MissionSpec::kPrimeStrokes); its valves, a bit each open.
    uint8_t plant = 0;
    uint8_t primed = 0;
    uint8_t valves = 0;
    static constexpr uint8_t kFillerOpen = 1;
    static constexpr uint8_t kFueled = 2;
    static constexpr uint8_t kLinedUp = 4;
    // (A station's scrubber, where the filler would be -- FIX 724: its spent cartridge pulled out, the fresh one not yet in.)
    static constexpr uint8_t kSpentOut = 8;
    // A beacon (Beacon): its parts fitted (a bit each: mast, dish, cell), its switch; its dish's bearing and tilt, in steps.
    uint8_t beacon = 0;
    uint8_t bearing = 0;
    uint8_t tilt = 0;
    static constexpr uint8_t kBeaconOn = 8;
    // Samples (Samples): the cores taken (a bit a site), each site's drill turns so far (two bits each), and how long the cold case
    // keeps them cold yet.
    uint8_t cored = 0;
    uint8_t drilled = 0;
    float cold = MissionSpec::kColdSeconds;
    // The breaker board of the terminal's building: its door open, its main switch on, and which of its breakers are on (a bit
    // each).
    bool boardOpen = false;
    bool boardMain = true;
    uint8_t boardOn = MissionSpec::kBoardAll;
    // What the mission is worked at by hand. A station's console: which of its systems are switched on (a bit each, in their
    // order). A signal's transmitter: its lid open (kLidOpen), its sender off (kSenderOff), its power off (kPowerOff).
    uint8_t device = 0;
    // Where there was no map: the terminal found -- somebody has been in the room with it -- so the tracker may point at it.
    bool found = false;
    // A secured terminal's keypad: the figures typed so far (as a number), how many, and whether it has let them in (true
    // where it was never secured).
    uint16_t typed = 0;
    uint8_t typedCount = 0;
    bool unlocked = true;
    // The terminal's supply (MissionPlan::blownFuse): whether it has one (true where it never faulted); and its box -- its cover's
    // screws taken out (a bit each), its isolator on, which of its fuse holders are shut on a fuse (a bit each), and whether the
    // spare has gone in where the blown fuse was.
    bool supplied = true;
    uint8_t boxScrews = 0;
    bool boxIsolator = true;
    uint8_t boxFuses = MissionSpec::kFusesAll;
    bool boxSpared = false;
    // The building's feed (MissionPlan::feedCut): whether supply is coming into it (true where it was never cut); and its feeder
    // cabinet -- its door open, its main and standby feeds' switches on, and its changeover's position (0 on the main feed, 1 off,
    // 2 on the standby), and whether it last came off the standby (so its next turn is back towards the main).
    bool fed = true;
    bool cabinetOpen = false;
    bool feedMain = true;
    bool feedStandby = false;
    uint8_t changeover = 0;
    bool changeoverBack = false;
    // Once it is over: whether the optional goal's records drive (MissionPlan::extra) left with the shuttle.
    bool extra = false;
    static constexpr uint8_t kLidOpen = 1;
    static constexpr uint8_t kSenderOff = 2;
    static constexpr uint8_t kPowerOff = 4;
    // (A console that has tripped, until a system is next started: said on its screen.)
    static constexpr uint8_t kConsoleTripped = 8;
    bool Launching() const { return launchIn >= 0.0f; }
};

namespace MissionRules
{

MissionState Start(const MissionPlan& plan);
bool CanDownload(const MissionState& state);
bool CanRestorePower(const MissionState& state);
// Somebody at the terminal pressing it: the download starts, if it can. True when it did.
bool StartDownload(MissionState& state);

// What working something by hand came to.
enum class Worked : uint8_t
{
    Nothing,  // not to be worked now, or not that way
    Opened,   // a door or a lid swung open
    Switched, // a switch thrown, and nothing more
    Tripped,  // thrown wrongly: it has gone off with a bang, heard a long way
    Done      // and that was the last of it: the power is back, the transmitter is down
};
// The breaker board (FIX 303), worked by hand. `control`: its door (kBoardDoor), its main switch (kBoardMain), or one of its
// breakers (kBoardBreaker + which). The door first; then the breakers found tripped switched back on, and the main switch
// thrown. Where a circuit has a fault, the main switch goes off again with a bang whenever it and that circuit's breaker are
// both on -- so the faulted one is found by switching them on one at a time with the main on, and left off. The power is back
// once the main and every sound circuit's breaker are on; the board is not worked after.
inline constexpr int kBoardDoor = 0;
inline constexpr int kBoardMain = 1;
inline constexpr int kBoardBreaker = 2;
Worked WorkBoard(MissionState& state, const MissionPlan& plan, int control);
// A breaker's circuit live: the breaker on, and the main switch.
bool BreakerLive(const MissionState& state, int breaker);
// Which breaker one of the building's lamp circuits (MissionPlan::circuits: which of them) comes through.
int BreakerOf(const MissionPlan& plan, size_t circuit);
// A station's life support, restarted at its console by hand (FIX 304): its three systems (`system` 0 to 2: scrubbers, pumps,
// fans) switched on in their order, each only once the one before is up to pressure -- which it comes to while somebody stays
// in the room (Tick). One switched on out of its turn trips the lot: all off, the pressure gone, to be begun again. One switched
// off stops those after it too.
inline constexpr int kConsoleSystems = 3;
Worked WorkConsole(MissionState& state, const MissionPlan& plan, int system);
// How far a restart may get with what is switched on: a third a system.
float ConsoleCap(const MissionState& state);
// A signal's transmitter, shut down by hand (FIX 305). `control`: its case's lid (kCaseLid), its sender's switch (kCaseSender),
// its power switch (kCasePower). The lid first; then the sender off, and then the power -- the power thrown while it is still
// sending goes off with a bang, and changes nothing.
inline constexpr int kCaseLid = 0;
inline constexpr int kCaseSender = 1;
inline constexpr int kCasePower = 2;
Worked WorkTransmitter(MissionState& state, const MissionPlan& plan, int control);
// A secured terminal's keypad, worked by hand (Phase 5): `key` a figure (0 to 9), clear (kKeyClear) or enter (kKeyEnter). Up
// to four figures typed, then enter: the right code lets them in (Done); a wrong one is refused (Tripped) and cleared.
inline constexpr int kKeyClear = 10;
inline constexpr int kKeyEnter = 11;
inline constexpr int kKeys = 12;
// (A key pressed comes to the host as a terminal's control, its payload this plus which.)
inline constexpr int kKeyPayload = 100;
Worked WorkKeypad(MissionState& state, const MissionPlan& plan, int key);
// A faulted terminal's supply box, worked by hand (Phase 5): `control` one of its cover's four screws (kBoxScrew + which: out with
// a multitool, `tool`, and not without), its isolator (kBoxIsolator: on and off), or one of its fuse holders (kBoxFuse + which:
// opened, or shut again -- the spare going in where the blown fuse was). A holder opened or shut with the isolator on draws an
// arc (Tripped: heard a long way). The terminal has its supply once every holder is shut on a good fuse and the isolator is on
// (Done); the box is not worked after.
inline constexpr int kBoxScrew = 0;
inline constexpr int kBoxIsolator = 4;
inline constexpr int kBoxFuse = 5;
inline constexpr int kBoxControls = kBoxFuse + MissionSpec::kFuses;
// (Comes to the host as a terminal's control, its payload this plus which.)
inline constexpr int kBoxPayload = 120;
Worked WorkBox(MissionState& state, const MissionPlan& plan, int control, bool tool);
// The cover off: all four screws out.
inline bool BoxOpen(const MissionState& state) { return state.boxScrews == 0xFu; }
// A cut feed's cabinet, worked by hand (Phase 5): `control` its door (kCabinetDoor), its main feed's switch (kCabinetMain), its
// standby feed's (kCabinetStandby), or its changeover (kCabinetChangeover: a turn, from the main through off to the standby and
// back). Turned onto the standby with the cut main feed still switched in, it trips straight back to off (Tripped: a bang); the
// main switched in with the changeover on the standby trips likewise. The building is fed once the changeover is on the standby,
// its switch on and the main's off (Done, and its power back if its board is set too); the cabinet is not worked after.
inline constexpr int kCabinetDoor = 0;
inline constexpr int kCabinetMain = 1;
inline constexpr int kCabinetStandby = 2;
inline constexpr int kCabinetChangeover = 3;
inline constexpr int kCabinetControls = 4;
// (Comes to the host as a terminal's control, its payload this plus which.)
inline constexpr int kCabinetPayload = 140;
Worked WorkCabinet(MissionState& state, const MissionPlan& plan, int control);
// A survey's markers, set up by hand (Phase 6). A marker's controls (its payload: kMarkerControls a marker, from nought): its stake
// -- planted at its point from a marker carried there (PlantMarker); stood back up when it has been knocked over; and, once its
// reading is taken, its switch off and its legs in, pulled up and packed (Done) -- its three legs, kicked out or folded in; each
// leg's clamp, the leg lengthened a step (the longest back to the shortest); and its switch.
inline constexpr int kMarkerStake = 0;
inline constexpr int kMarkerLeg = 1;
inline constexpr int kMarkerClamp = 4;
inline constexpr int kMarkerSwitch = 7;
inline constexpr int kMarkerControls = 8;
// A server rack's drive bay, worked by hand (Phase 6): `control` its cover (kRackCover: opened and shut), its drive's sled (kRackSled:
// pulled -- Done, the drive out -- or, damaged, drawn out to its board), the board's slot (kRackSlot: a board carried fitted in its
// place), its port (kRackPort: the copy unit carried connected, and the copy started), the copy unit (kRackUnit: once the copy is
// done, disconnected -- Done, to be carried), and the other rack's service hatch (kDonorHatch) and its board (kDonorBoard: taken out
// with a multitool -- Done). `carrying`: whoever works it has what that control wants (the board, the copy unit, the multitool).
inline constexpr int kRackCover = 0;
inline constexpr int kRackSled = 1;
inline constexpr int kRackSlot = 2;
inline constexpr int kRackPort = 3;
inline constexpr int kRackUnit = 4;
inline constexpr int kDonorHatch = 5;
inline constexpr int kDonorBoard = 6;
inline constexpr int kRackControls = 7;
// (Comes to the host as a terminal's control, its payload this plus which.)
inline constexpr int kRackPayload = 160;
Worked WorkRack(MissionState& state, const MissionPlan& plan, int control, bool carrying);
// A plant's chain, worked by hand (Phase 6): `control` its filler's cap (kFillerCap: opened and shut), its mouth (kFillerPour: the fuel
// carried poured in -- Done), its pump's lever (kPumpLever: a stroke; Done on the one that primes it) or one of its valves (kValve +
// which: opened or shut; turned while the plant starts, it trips). `carrying`: the fuel can, for the pour.
// A station's chain (FIX 724: its life support got running by hand, not one switch) is the same three on a station's own: its CO2
// scrubber where the filler is (kFillerCap its door; kFillerPour its cartridge slot -- the spent one pulled out with nothing carried, then
// the fresh one, carried, slid in: Done), its coolant pump primed, its air valves lined up.
inline constexpr int kFillerCap = 0;
inline constexpr int kFillerPour = 1;
inline constexpr int kPumpLever = 2;
inline constexpr int kValve = 3;
inline constexpr int kPlantControls = kValve + MissionSpec::kPlantValves;
// (Comes to the host as a terminal's control, its payload this plus which.)
inline constexpr int kPlantPayload = 180;
Worked WorkPlant(MissionState& state, const MissionPlan& plan, int control, bool carrying);
// Fuelled, primed and lined up -- or no chain to it: the console can start it.
bool PlantReady(const MissionState& state, const MissionPlan& plan);
// A beacon, put together and aimed by hand (Phase 6): `control` its place (kBeaconFit: the next of its parts, carried, fitted -- mast,
// then dish, then cell), its bearing knob (kBeaconBearing: a step round), its tilt knob (kBeaconTilt: a step up, the highest back to
// level) and its switch (kBeaconSwitch). Done once it is on and aimed as its plate says. `carrying`: the part it wants next.
inline constexpr int kBeaconFit = 0;
inline constexpr int kBeaconBearing = 1;
inline constexpr int kBeaconTilt = 2;
inline constexpr int kBeaconSwitch = 3;
inline constexpr int kBeaconControls = 4;
inline constexpr int kBeaconPayload = 200;
Worked WorkBeacon(MissionState& state, const MissionPlan& plan, int control, bool carrying);
// Which of its parts it wants next (0 mast, 1 dish, 2 cell; kBeaconParts: none).
int NextBeaconPart(const MissionState& state);
bool BeaconAimed(const MissionState& state, const MissionPlan& plan);
// A core drilled at a sample site (Phase 6): a turn of the drill, the cold case carried to take it; Done on the last turn (and all of
// them: back to the shuttle). Spoiled: the cold case warmed through -- every core lost, to be drilled again.
inline constexpr int kSamplePayload = 210;
Worked DrillSample(MissionState& state, const MissionPlan& plan, int site, bool carrying);
void SpoilSamples(MissionState& state);
// Planted at its point: `ground` how far down the ground under each foot is (two bits a leg, 0 to 2).
Worked PlantMarker(MissionState& state, const MissionPlan& plan, int marker, uint8_t ground);
// One of its controls worked; `ground` dealt afresh for it stood back up.
Worked WorkMarker(MissionState& state, const MissionPlan& plan, int marker, int control, uint8_t ground);
// Knocked over (something walked into it): it stops reading until it is stood up and levelled again. True when it was standing.
bool KnockMarker(MissionState& state, int marker);
int LegLength(const MissionState::Marker& marker, int leg);
int GroundUnder(const MissionState::Marker& marker, int leg);
// Its feet all bearing alike: each leg's length less the ground's fall under its foot the same for all three.
bool MarkerLevel(const MissionState::Marker& marker);
// Planted, standing, its legs out, level and switched on: reading, when enough others are.
bool MarkerWorking(const MissionState::Marker& marker);
int MarkersWorking(const MissionState& state);
// Its reading taken, its switch off, its legs in, standing: to be pulled up and packed.
bool MarkerPackable(const MissionState& state, int marker);
// Which way its head leans, in its own frame (x, z), so many steps of leg -- nought when level. Leg `leg` points along LegWay.
glm::vec2 MarkerLean(const MissionState::Marker& marker);
glm::vec2 LegWay(int leg);
// Every reading of the survey taken.
bool AllReadings(const MissionState& state, const MissionPlan& plan);
// Somebody aboard pressing the console: launched if it was not, held if it was. False once it is over.
// `seconds` is the countdown (the Shuttle upgrade shortens it: Phase 8).
bool ToggleLaunch(MissionState& state, float seconds = MissionSpec::kLaunchSeconds);

// What happened in a tick.
struct Ticked
{
    bool downloaded = false; // the drive is ready: put it on the bench
    bool read = false;       // a survey's reading is taken
    bool launched = false;   // the shuttle has gone: see who and what is aboard, then Finish
};
// A tick of the mission, on the host: the download goes on while somebody is at the terminal, and the launch counts
// down.
Ticked Tick(MissionState& state, const MissionPlan& plan, float dt, bool attended);
// The shuttle has gone: with the drive aboard or not, with whom, and with the optional goal's records drive or not.
void Finish(MissionState& state, bool driveAboard, uint8_t aboard, bool extraAboard = false);

} // namespace MissionRules

} // namespace pred
