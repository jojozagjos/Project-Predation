#pragma once

#include "Game/World/SitePlan.h"

#include <glm/vec3.hpp>

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
// How near the terminal somebody has to be for the download to go on: in the room with it, more or less.
inline constexpr float kAttendReach = 4.5f;
// From somebody pressing launch to the shuttle leaving.
inline constexpr float kLaunchSeconds = 20.0f;
// How long the result is shown before everybody is back aboard the ship.
inline constexpr float kResultSeconds = 8.0f;
// A survey (MissionPlan::Kind::Survey): its markers -- a stake with an instrument head, this tall and this wide -- and how long a
// reading takes, somebody standing at the marker (within kReadingReach).
inline constexpr float kMarkerHeight = 1.4f;
inline constexpr float kMarkerWidth = 0.16f;
inline constexpr float kReadingSeconds = 8.0f;
inline constexpr float kReadingReach = 2.5f;
// A signal's transmitter (MissionPlan::Kind::Signal): a field case on the ground, and the whip aerial up from it.
inline constexpr glm::vec3 kTransmitterSize{0.7f, 0.45f, 0.5f};
inline constexpr float kAerialHeight = 2.2f;
// A breaker board's breakers, under its main switch; and all of them on, a bit each.
inline constexpr int kBoardBreakers = 6;
inline constexpr uint8_t kBoardAll = (1u << kBoardBreakers) - 1u;
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
        Signal    // a transmitter somewhere on the site, found and shut down (markers: its one), then back to the shuttle
    };
    Kind kind = Kind::Download;
    // A survey's markers, each where it stands (its foot, on the ground: the game sets its height from the ground there); or a signal's
    // transmitter, its one.
    std::vector<glm::vec3> markers;
    // Worked at markers (a survey's, a signal's) rather than at a terminal.
    bool AtMarkers() const { return kind == Kind::Survey || kind == Kind::Signal; }
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

    // The same mission from the same site and seed, on every machine.
    static MissionPlan Generate(const SitePlan& site, uint32_t seed);
    bool Valid() const { return building >= 0 || !markers.empty(); }
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
    // A survey's: the readings taken (a bit a marker), and the one being taken now (-1 none: Stage::Downloading is its reading).
    uint8_t readings = 0;
    int reading = -1;
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
// Somebody at a survey's marker pressing it: its reading starts, if it can. True when it did.
bool StartReading(MissionState& state, const MissionPlan& plan, int marker);
// Every reading of the survey taken.
bool AllReadings(const MissionState& state, const MissionPlan& plan);
// Somebody aboard pressing the console: launched if it was not, held if it was. False once it is over.
bool ToggleLaunch(MissionState& state);

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
// The shuttle has gone: with the drive aboard or not, and with whom.
void Finish(MissionState& state, bool driveAboard, uint8_t aboard);

} // namespace MissionRules

} // namespace pred
