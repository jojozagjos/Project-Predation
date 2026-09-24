#include "Game/Creature/TacticLearner.h"

#include "Game/Creature/CreatureBrain.h"

#include <algorithm>
#include <cstdio>

namespace pred
{

namespace
{

// How much one outcome moves the value: a fifth of the way towards it. Quick enough that a few
// encounters show, slow enough that one lucky blow does not decide the match.
constexpr float kLearningRate = 0.2f;
// How far the value can tilt a choice either way.
constexpr float kLean = 0.4f;

} // namespace

TacticLearner::Tactic TacticLearner::Of(Behavior behavior)
{
    switch (behavior)
    {
    case Behavior::Hunt:
    case Behavior::Attack:
        return Charge;
    case Behavior::Stalk:
        return Shadow;
    case Behavior::Ambush:
        return LieInWait;
    case Behavior::Flank:
        return GoRound;
    case Behavior::Lure:
        return CallIn;
    default:
        return Count;
    }
}

const char* TacticLearner::Name(Tactic tactic)
{
    switch (tactic)
    {
    case Charge:
        return "charging";
    case Shadow:
        return "stalking";
    case LieInWait:
        return "lying in wait";
    case GoRound:
        return "going round";
    case CallIn:
        return "luring";
    case Count:
        break;
    }
    return "?";
}

void TacticLearner::Reward(Tactic tactic, float amount)
{
    if (tactic >= Count)
    {
        return;
    }
    m_value[tactic] += kLearningRate * (std::clamp(amount, -2.0f, 2.0f) - m_value[tactic]);
    ++m_tries[tactic];
}

float TacticLearner::Weight(Tactic tactic) const
{
    if (tactic >= Count)
    {
        return 1.0f;
    }
    return std::clamp(1.0f + kLean * m_value[tactic], 1.0f - kLean, 1.0f + kLean);
}

void TacticLearner::Reset()
{
    m_value.fill(0.0f);
    m_tries.fill(0);
}

std::string TacticLearner::Describe() const
{
    std::string text;
    for (int i = 0; i < Count; ++i)
    {
        char line[64];
        std::snprintf(line, sizeof(line), "%s%s %+.2f (%d)", text.empty() ? "" : ", ", Name(static_cast<Tactic>(i)),
                      m_value[static_cast<size_t>(i)], m_tries[static_cast<size_t>(i)]);
        text += line;
    }
    return text;
}

} // namespace pred
