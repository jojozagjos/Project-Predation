#include "Game/Creature/CreatureBrain.h"
#include "Game/Creature/TacticLearner.h"

#include <catch2/catch_test_macros.hpp>

using namespace pred;

// What the brood learns over a match: what worked leans the choice towards it, what cost leans it away,
// never all the way.

TEST_CASE("The brood leans towards what has worked and away from what has cost it", "[creature][learning]")
{
    TacticLearner learned;
    CHECK(learned.Weight(TacticLearner::Charge) == 1.0f);
    CHECK(learned.Weight(TacticLearner::Of(Behavior::Roam)) == 1.0f);

    // Charging gets them shot; stalking lands blows.
    for (int i = 0; i < 6; ++i)
    {
        learned.Reward(TacticLearner::Of(Behavior::Hunt), -1.0f);
        learned.Reward(TacticLearner::Of(Behavior::Stalk), 1.0f);
    }
    CHECK(learned.Weight(TacticLearner::Charge) < 0.85f);
    CHECK(learned.Weight(TacticLearner::Shadow) > 1.15f);
    // Never written off, never certain.
    CHECK(learned.Weight(TacticLearner::Charge) >= 0.6f);
    CHECK(learned.Weight(TacticLearner::Shadow) <= 1.4f);
    // What was never tried is left alone.
    CHECK(learned.Weight(TacticLearner::LieInWait) == 1.0f);
    CHECK(learned.Tries(TacticLearner::Shadow) == 6);

    // And it changes its mind when the players do: charging starts working.
    for (int i = 0; i < 12; ++i)
    {
        learned.Reward(TacticLearner::Charge, 1.0f);
    }
    CHECK(learned.Weight(TacticLearner::Charge) > 1.1f);

    learned.Reset();
    CHECK(learned.Weight(TacticLearner::Charge) == 1.0f);
}
