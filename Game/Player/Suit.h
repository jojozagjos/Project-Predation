#pragma once

namespace pred
{

// The suit's own state against what a world throws at its wearer: its air, whether it is torn, how warm it keeps, and how
// much radiation has got through. Made never to be a chore -- it recycles its own air, it seals its own tears, it keeps its
// wearer warm or cool -- and only a danger where something has beaten it: torn where the air outside cannot be breathed, out
// in a blizzard or by lava too long, out in a radiation storm. Shelter -- a building, the shuttle, the ship -- puts it right.
//
// The same on every machine for the same conditions: the host steps everybody's for the harm it does, each machine its own
// player's for the suit's readings.
struct SuitConditions
{
    bool breathable = true; // the air outside can be breathed
    bool toxic = false;     // the air outside is poisonous: worse through a tear
    bool sheltered = false; // indoors, in the shuttle or the ship: pressurised, warm, shielded
    float cold = 0.0f;      // 0 to 1, how hard the cold pulls (a frozen world's night, a blizzard, icy water)
    float heat = 0.0f;      // 0 to 1, how hard the heat pushes (a scorching world, beside lava)
    float radiation = 0.0f; // 0 to 1, how strong the radiation out there is just now
};

struct SuitState
{
    float air = 1.0f;    // the reserve, 0 to 1
    float breach = 0.0f; // how torn, 0 sealed to 1 badly
    float warmth = 0.0f; // -1 frozen to 1 overheated, 0 as it should be
    float dose = 0.0f;   // the radiation taken, 0 to 1
};

// What the suit's state does to its wearer this step: so much harm a second (0 none), and why.
struct SuitHarm
{
    float perSecond = 0.0f;
    const char* cause = "";
};

namespace Suit
{
// A hit tears it, the harder the more: `damage` as health is counted (out of 100).
void Tear(SuitState& suit, float damage);
// One step of `seconds`: the air leaking or filling, the tear sealing, the warmth drifting, the dose building or fading.
SuitHarm Step(SuitState& suit, const SuitConditions& out, float seconds);
// Back to as it was made (a new body).
inline void Reset(SuitState& suit) { suit = SuitState{}; }
} // namespace Suit

} // namespace pred
