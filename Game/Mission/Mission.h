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
inline constexpr glm::vec3 kBreakerSize{0.6f, 0.8f, 0.14f};
inline constexpr float kBreakerHeight = 1.45f;
// How near the terminal somebody has to be for the download to go on: in the room with it, more or less.
inline constexpr float kAttendReach = 4.5f;
// From somebody pressing launch to the shuttle leaving.
inline constexpr float kLaunchSeconds = 20.0f;
// How long the result is shown before everybody is back aboard the ship.
inline constexpr float kResultSeconds = 8.0f;
} // namespace MissionSpec

struct MissionPlan
{
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
    float downloadSeconds = 45.0f;

    // The same mission from the same site and seed, on every machine.
    static MissionPlan Generate(const SitePlan& site, uint32_t seed);
    bool Valid() const { return building >= 0; }
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
    bool Launching() const { return launchIn >= 0.0f; }
};

namespace MissionRules
{

MissionState Start(const MissionPlan& plan);
bool CanDownload(const MissionState& state);
bool CanRestorePower(const MissionState& state);
// Somebody at the terminal pressing it: the download starts, if it can. True when it did.
bool StartDownload(MissionState& state);
bool RestorePower(MissionState& state);
// Somebody aboard pressing the console: launched if it was not, held if it was. False once it is over.
bool ToggleLaunch(MissionState& state);

// What happened in a tick.
struct Ticked
{
    bool downloaded = false; // the drive is ready: put it on the bench
    bool launched = false;   // the shuttle has gone: see who and what is aboard, then Finish
};
// A tick of the mission, on the host: the download goes on while somebody is at the terminal, and the launch counts
// down.
Ticked Tick(MissionState& state, const MissionPlan& plan, float dt, bool attended);
// The shuttle has gone: with the drive aboard or not, and with whom.
void Finish(MissionState& state, bool driveAboard, uint8_t aboard);

} // namespace MissionRules

} // namespace pred
