#pragma once

#include "Game/World/SitePlan.h"

#include <string>
#include <vector>

namespace pred::Difficulty
{

// How hard a place to go down to looks from the ship, before anybody has been: from what kind of work it is, how much is known
// of it, how plain the organism's signs are there (when the sensors can read them), and what the place itself throws at the
// crew -- its weather, no air, its cold or heat, the dark. Shown on the map and on the ship's terminal.
struct Inputs
{
    SitePlan::SiteKind kind = SitePlan::SiteKind::Facility;
    bool charted = false;     // on the charts: its layout known (a map of it likely), not a place only found
    bool threatKnown = false; // the sensors read the threat
    float signs = 0.0f;       // how plain the organism's signs are, 0 to 1 (only with threatKnown)
    bool air = true;          // air to breathe outside
    float weather = 0.0f;     // how rough its weather is, 0 calm to 1 the worst
    float temperature = 15.0f;
    bool night = false;       // dark there now
};

struct Read
{
    int rating = 1;              // 1 to 5
    std::string word;            // EASY, MODERATE, HARD, SEVERE, EXTREME
    std::vector<std::string> why; // the reasons, plainly, worst first
};

Read Rate(const Inputs& inputs);

// "HARD (3/5)".
std::string Line(const Read& read);

} // namespace pred::Difficulty
