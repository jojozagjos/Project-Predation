#include "Game/Player/Suit.h"

#include <algorithm>
#include <cmath>

namespace pred
{

namespace Suit
{

namespace
{
// A hit's share of a tear: a hard one (a quarter of the body's health) tears it a third.
constexpr float kTearPerDamage = 0.013f;
// It seals itself: a third of a tear in ten seconds or so.
constexpr float kSealing = 0.03f;
// Through a tear, out of air where it cannot be breathed: a third of a tear empties it in about forty seconds.
constexpr float kLeak = 0.075f;
// And the recycler fills it again, from empty in twenty seconds.
constexpr float kRefill = 0.05f;
// How fast the warmth goes where the conditions push it: about a minute to the worst of it.
constexpr float kWarmthRate = 1.0f / 60.0f;
// The dose: a full storm gives the lot in about two minutes outside, a tenth of that sheltered; it fades over minutes.
constexpr float kDoseRate = 1.0f / 120.0f;
constexpr float kDoseFade = 1.0f / 400.0f;

float Towards(float value, float to, float rate, float seconds)
{
    const float step = rate * seconds;
    return to > value ? std::min(value + step, to) : std::max(value - step, to);
}
} // namespace

void Tear(SuitState& suit, float damage)
{
    suit.breach = std::clamp(suit.breach + std::max(damage, 0.0f) * kTearPerDamage, 0.0f, 1.0f);
}

SuitHarm Step(SuitState& suit, const SuitConditions& out, float seconds)
{
    SuitHarm harm;
    const auto worst = [&](float perSecond, const char* cause)
    {
        if (perSecond > harm.perSecond)
        {
            harm.perSecond = perSecond;
            harm.cause = cause;
        }
    };

    // The tear seals; and only matters where the air outside cannot be breathed and nothing round it is pressurised.
    suit.breach = std::max(suit.breach - kSealing * seconds, 0.0f);
    const bool exposed = !out.breathable && !out.sheltered;
    if (exposed && suit.breach > 0.0f)
    {
        suit.air = std::max(suit.air - suit.breach * kLeak * seconds, 0.0f);
        if (out.toxic)
        {
            worst(4.0f * suit.breach, "toxic air");
        }
    }
    else
    {
        suit.air = std::min(suit.air + kRefill * seconds, 1.0f);
    }
    if (exposed && suit.air <= 0.0f)
    {
        worst(7.0f, "no air");
    }

    // Warm or cool as it should be unless the conditions beat it; sheltered, back to as it should be.
    const float pushed = out.sheltered ? 0.0f : std::clamp(out.heat - out.cold, -1.0f, 1.0f);
    suit.warmth = Towards(suit.warmth, pushed, kWarmthRate * (out.sheltered ? 3.0f : 1.0f), seconds);
    if (std::abs(suit.warmth) > 0.8f)
    {
        worst((std::abs(suit.warmth) - 0.8f) * 25.0f, suit.warmth < 0.0f ? "cold" : "heat");
    }

    // The dose: building while out in it (a tenth as fast under cover), fading the rest of the time.
    const float taking = out.radiation * (out.sheltered ? 0.1f : 1.0f);
    suit.dose = std::clamp(suit.dose + (taking * kDoseRate - kDoseFade) * seconds, 0.0f, 1.0f);
    if (suit.dose > 0.6f)
    {
        worst((suit.dose - 0.6f) * 12.0f, "radiation");
    }
    return harm;
}

} // namespace Suit

} // namespace pred
