#pragma once

#include "Game/Mission/Mission.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pred
{

// What a deployment is for, as data (Assets/Data/missions.json): a template of steps -- each a thing to be done at the site
// (the power restored at a breaker, the data downloaded at a terminal, what was got carried to the shuttle) and which steps
// must be done before it -- with what the crew is told for each. Laid over one site's mission (MissionPlan) and stood
// against how that is going (MissionState), its steps are open, done or not yet, and known to the crew or not. All of that
// is worked out from the mission's own state on every machine, so nothing more is sent.
//
// The download mission is the first template; more kinds are more templates (and, where they need it, more kinds of step).
// Only data and rules here: nothing that needs the engine, so all of it can be tested.
namespace Objectives
{

enum class Kind : uint8_t
{
    RestorePower, // reset the breaker the terminal's power comes through
    Download,     // work the terminal while somebody stays at it: the data copied onto a drive, or a station's systems restarted
    Carry,        // back to the shuttle with it (the drive, a survey's readings) or with it done
    Readings,     // work each of the mission's markers: a survey's readings, a signal's transmitter shut down
    Code          // a secured terminal's access code found and entered at its keypad
};

struct StepDef
{
    std::string id;
    Kind kind = Kind::Download;
    std::vector<std::string> after; // the steps done before it opens (any not laid for this mission, left out)
    std::string text;               // what the crew is told to do
    std::string hint;               // a line under it while it is open
    std::string hintNoMap;          // ... and one when the briefing came without a map of the site
    bool onlyWhenPowerOut = false;  // only part of the mission when the power is out
    bool onlyWhenSecured = false;   // only part of the mission when the terminal is secured (MissionPlan::code)
    bool learntAtTerminal = false;  // not known to the crew until somebody has found the terminal dead
    // While it is under way (a download, a restart, a reading): what is shown with how far it has got, and the line under it
    // with somebody at it and with nobody.
    std::string working;
    std::string stay;
    std::string paused;
};

struct Template
{
    std::string id;
    std::string name;
    std::vector<StepDef> steps;
    // What the briefing's screen lists as the objective, a line a step; and how it ended, said over the result.
    std::vector<std::string> brief;
    std::string done;
    std::string failed;
};

struct Library
{
    std::vector<Template> templates;

    bool LoadFromFile(const std::filesystem::path& file, std::string* error = nullptr);
    bool LoadFromString(const std::string& text, std::string* error = nullptr);
    const Template* Find(const std::string& id) const;
};

// A template laid over one mission: its steps, by index, and how each stands.
struct Step
{
    const StepDef* def = nullptr;
    std::vector<int> after;
    bool done = false;
    bool known = true;
};

struct Graph
{
    const Template* source = nullptr;
    std::vector<Step> steps;

    // The steps open now, in the template's order: known, not done, and everything before them done (or not known of).
    std::vector<int> Open() const;
    bool Complete() const;
};

// Its steps for this mission (one only there when the power is out left out when it is not).
Graph Lay(const Template& source, const MissionPlan& plan);
// How it stands, from how the mission does (and whether somebody has found the terminal without power).
void Update(Graph& graph, const MissionState& state, bool foundNoPower);
// What the crew is told for a step, and the line under it (empty: none).
std::string TextOf(const Step& step);
std::string HintOf(const Step& step, const MissionPlan& plan);
// The template a mission of this kind is laid from (its id in missions.json).
const char* TemplateFor(MissionPlan::Kind kind);

} // namespace Objectives

} // namespace pred
