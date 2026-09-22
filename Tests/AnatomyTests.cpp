#include "Game/Creature/CreatureAnatomy.h"
#include "Game/Creature/CreatureTraits.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

#include <array>
#include <cmath>

using namespace pred;

// Milestone 10: bodies from seeds, and what those bodies can do.

TEST_CASE("The same seed makes the same body, and different seeds different ones", "[anatomy]")
{
    const CreatureAnatomy a = CreatureAnatomy::FromSeed(1234);
    const CreatureAnatomy b = CreatureAnatomy::FromSeed(1234);
    CHECK(a.plan == b.plan);
    CHECK(a.length == b.length);
    CHECK(a.hipHeight == b.hipHeight);
    CHECK(a.eyes == b.eyes);
    CHECK(a.legs.size() == b.legs.size());
    CHECK(a.legs.front().upper == b.legs.front().upper);
    CHECK(a.skin == b.skin);

    int differ = 0;
    for (uint32_t seed = 1; seed < 50; ++seed)
    {
        const CreatureAnatomy x = CreatureAnatomy::FromSeed(seed);
        const CreatureAnatomy y = CreatureAnatomy::FromSeed(seed + 1);
        differ += (x.length != y.length || x.plan != y.plan || x.eyes != y.eyes) ? 1 : 0;
    }
    CHECK(differ == 49);
}

TEST_CASE("A body does not change the temperament a seed already had", "[anatomy]")
{
    // Seed 14 as it was before it had a body: the stalking tests were written against this animal.
    const CreatureTraits traits = CreatureTraits::FromSeed(14);
    CHECK(traits.aggression == Catch::Approx(0.54f).margin(0.005));
    CHECK(traits.fear == Catch::Approx(0.25f).margin(0.005));
    CHECK(traits.curiosity == Catch::Approx(0.31f).margin(0.005));
    CHECK(traits.stealth == Catch::Approx(0.97f).margin(0.005));
}

TEST_CASE("Across many seeds, every kind of body turns up", "[anatomy]")
{
    std::array<int, static_cast<size_t>(BodyPlan::Count)> plans{};
    std::array<int, static_cast<size_t>(SizeClass::Count)> sizes{};
    std::array<int, 7> eyes{};
    int plated = 0;
    int tailless = 0;
    constexpr int kSeeds = 2000;
    for (uint32_t seed = 0; seed < kSeeds; ++seed)
    {
        const CreatureAnatomy a = CreatureAnatomy::FromSeed(seed);
        const CreatureCapabilities c = CreatureCapabilities::From(a);
        ++plans[static_cast<size_t>(a.plan)];
        ++sizes[static_cast<size_t>(c.size)];
        ++eyes[static_cast<size_t>(a.eyes)];
        plated += a.plates > 0 ? 1 : 0;
        tailless += a.tailLength < 0.05f ? 1 : 0;
    }
    INFO("four " << plans[0] << ", six " << plans[1] << ", two " << plans[2] << "; small " << sizes[0]
                 << ", medium " << sizes[1] << ", large " << sizes[2] << "; eyeless " << eyes[0]);
    // Roughly the shares it is drawn with, and every kind present in a useful number.
    CHECK(plans[0] > kSeeds * 0.4);
    CHECK(plans[1] > kSeeds * 0.18);
    CHECK(plans[2] > kSeeds * 0.18);
    for (const int count : sizes)
    {
        CHECK(count > kSeeds / 20);
    }
    CHECK(eyes[0] > kSeeds / 20); // blind ones that hunt by sound
    CHECK(eyes[2] > 0);
    CHECK(eyes[4] > 0);
    CHECK(eyes[6] > 0);
    CHECK(eyes[1] + eyes[3] + eyes[5] == 0); // eyes come in pairs
    CHECK(plated > 0);
    CHECK(tailless > 0);
}

TEST_CASE("Every body can stand: legs reach the ground with the knee bent, and there are no wings", "[anatomy]")
{
    for (uint32_t seed = 0; seed < 2000; ++seed)
    {
        const CreatureAnatomy a = CreatureAnatomy::FromSeed(seed);
        const CreatureAnatomy::RestPose pose = a.Rest();
        INFO("seed " << seed << ": " << a.Describe());
        const size_t pairs = a.plan == BodyPlan::Hexapod ? 3 : (a.plan == BodyPlan::Quadruped ? 2 : 1);
        REQUIRE(a.legs.size() == pairs);
        REQUIRE(pose.legs.size() == pairs * 2);
        for (const CreatureAnatomy::Leg& leg : pose.legs)
        {
            // Every limb there is is a leg, and every leg stands on the ground. There is no such thing
            // as a limb that does not, which is how there are no wings.
            CHECK(leg.foot.y == 0.0f);
            CHECK(leg.hip.y > 0.15f);
            // Long enough to reach its foot, and not so long it could not help but stand straight.
            const float reach = glm::distance(leg.hip, leg.foot + glm::vec3(0.0f, leg.pair->thickness, 0.0f));
            const float total = leg.pair->upper + leg.pair->lower;
            CHECK(total > reach * 1.05f);
            CHECK(total < reach * 1.4f);
        }
        // The body above the legs, the head in front of the body, the tail behind it.
        CHECK(pose.shoulders.y > a.hipHeight);
        CHECK(pose.head.z < pose.shoulders.z);
        CHECK(pose.tailBase.z > 0.0f);
        CHECK(static_cast<int>(pose.eyes.size()) == a.eyes);
        CHECK(pose.head.y - a.headDepth * 0.5f > 0.0f); // head clear of the ground
    }
}

TEST_CASE("What a body can do follows from the body", "[anatomy]")
{
    float lightHealth = 0.0f;
    float heavyHealth = 0.0f;
    int light = 0;
    int heavy = 0;
    for (uint32_t seed = 0; seed < 2000; ++seed)
    {
        const CreatureAnatomy a = CreatureAnatomy::FromSeed(seed);
        const CreatureCapabilities c = CreatureCapabilities::From(a);
        INFO("seed " << seed << ": " << a.Describe() << " / " << c.Describe());
        // In range, every one.
        CHECK(c.health >= 700.0f);
        CHECK(c.health <= 4000.0f);
        CHECK(c.runSpeed >= 3.6f);
        CHECK(c.runSpeed <= 6.8f);
        CHECK(c.walkSpeed < c.runSpeed);
        CHECK(c.strikeDamage >= 18.0f);
        CHECK(c.strikeDamage <= 45.0f);
        CHECK(c.strikeReach >= 1.8f);
        CHECK(c.strikeReach <= 3.0f);
        // Its eyes decide its sight, and a creature with none makes up for it in hearing.
        if (a.eyes == 0)
        {
            CHECK(c.sight == 0.0f);
            CHECK(c.hearing > 1.5f);
        }
        else
        {
            CHECK(c.sight > 0.8f);
            CHECK(c.hearing < 1.3f);
        }
        // Plates stop rounds; no plates, nothing stopped.
        CHECK((c.armour > 0.0f) == (a.plates > 0));
        CHECK(c.armour <= 0.3f);
        // Only a small body fits a vent.
        CHECK(c.fitsVents == (c.size == SizeClass::Small));

        // The box rounds hit holds the whole body: head, rump and the top of its back.
        const CreatureAnatomy::RestPose pose = a.Rest();
        const glm::vec3 low = c.bodyCentre - c.bodyHalfExtents - glm::vec3(0.01f);
        const glm::vec3 high = c.bodyCentre + c.bodyHalfExtents + glm::vec3(0.01f);
        const auto inside = [&](const glm::vec3& p)
        { return p.x >= low.x && p.y >= low.y && p.z >= low.z && p.x <= high.x && p.y <= high.y && p.z <= high.z; };
        CHECK(inside(pose.head));
        CHECK(inside(pose.rump));
        CHECK(inside(pose.shoulders));
        CHECK(low.y <= 0.02f);

        if (c.mass < 120.0f)
        {
            lightHealth += c.health;
            ++light;
        }
        else if (c.mass > 300.0f)
        {
            heavyHealth += c.health;
            ++heavy;
        }
    }
    REQUIRE(light > 0);
    REQUIRE(heavy > 0);
    // The heavy ones take a great deal more killing.
    CHECK(heavyHealth / static_cast<float>(heavy) > lightHealth / static_cast<float>(light) * 1.5f);
}
