#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace pred
{

enum class Behavior : uint8_t;

// What the brood learns, over a match, about how to get at these particular people.
//
// Each way of going about somebody -- straight at them, shadowing them from cover, lying in wait,
// going round, calling them in a friend's voice -- has a running value, learnt from what happened the
// times it was tried: a blow or a grab that landed is a reward; being shot while at it, or killed, is
// a cost. It is reinforcement learning of the simplest kind that works (a multi-armed bandit with an
// exponentially weighted average per arm), shared by the whole brood, so what one of them finds out
// the others use. Against people who shoot everything that charges, charging stops being chosen;
// against people who never look behind them, stalking wins more and more.
//
// It only tilts the choice. Everything else a creature weighs still decides; a tactic the brood has
// learnt to distrust is less likely, never impossible, and one it has never tried is not written off.
class TacticLearner
{
public:
    enum Tactic : uint8_t
    {
        Charge,
        Shadow,
        LieInWait,
        GoRound,
        CallIn,
        Count
    };

    // Which tactic a behaviour is, or Count for one that is not a way of getting at somebody.
    static Tactic Of(Behavior behavior);
    static const char* Name(Tactic tactic);

    // Something happened to a creature that was, just now, going about somebody this way.
    void Reward(Tactic tactic, float amount);

    // How much to lean towards this tactic: 1 knows nothing, above 1 has worked, below has cost.
    float Weight(Tactic tactic) const;
    float Value(Tactic tactic) const { return m_value[tactic]; }
    int Tries(Tactic tactic) const { return m_tries[tactic]; }

    void Reset();
    std::string Describe() const;

private:
    std::array<float, Count> m_value{};
    std::array<int, Count> m_tries{};
};

} // namespace pred
