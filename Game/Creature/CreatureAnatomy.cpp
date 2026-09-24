#include "Game/Creature/CreatureAnatomy.h"

#include "Game/Creature/CreatureTraits.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pred
{
namespace
{

// The anatomy's own random stream, apart from the temperament's, so adding a body did not change any
// creature's temperament and changing how bodies are drawn never will.
uint64_t AnatomyStream(uint32_t seed)
{
    return (static_cast<uint64_t>(seed) * 0x9E3779B97F4A7C15ull) ^ 0x00A7A70B0D1E5ull;
}

int RangeInt(SeededRandom& random, int low, int high)
{
    return low + static_cast<int>(random.Unit() * static_cast<float>(high - low + 1) * 0.9999f);
}

// Skins: pale, sickly and wet-looking, or nearly black. Things that live in the dark have lost their
// colour, and the ones that look like something that used to be a person are the worst to see.
constexpr glm::vec3 kSkins[] = {
    {0.62f, 0.60f, 0.55f}, // corpse white
    {0.46f, 0.46f, 0.44f}, // ash grey
    {0.56f, 0.52f, 0.40f}, // jaundiced
    {0.42f, 0.36f, 0.39f}, // bruised
    {0.47f, 0.50f, 0.44f}, // grave-mould green-grey
    {0.15f, 0.15f, 0.16f}, // charcoal
};
// Dim: pinpricks deep in the sockets, not lamps. Enough to catch in a torch beam, not to light a room.
constexpr glm::vec3 kEyeGlows[] = {
    {1.6f, 0.30f, 0.20f}, // red
    {1.5f, 0.90f, 0.20f}, // amber
    {0.9f, 1.20f, 0.60f}, // sickly yellow-green
    {1.2f, 1.25f, 1.20f}, // dead white
};

} // namespace

const char* BodyPlanName(BodyPlan plan)
{
    switch (plan)
    {
    case BodyPlan::Quadruped:
        return "four-legged";
    case BodyPlan::Hexapod:
        return "six-legged";
    case BodyPlan::Biped:
        return "two-legged";
    case BodyPlan::Crawler:
        return "crawling";
    case BodyPlan::Count:
        break;
    }
    return "?";
}

const char* SizeClassName(SizeClass size)
{
    switch (size)
    {
    case SizeClass::Small:
        return "small";
    case SizeClass::Medium:
        return "medium";
    case SizeClass::Large:
        return "large";
    case SizeClass::Count:
        break;
    }
    return "?";
}

CreatureAnatomy CreatureAnatomy::FromSeed(uint32_t seed)
{
    // Drawn in a fixed order; anything added later is drawn after, so a body somebody has seen keeps
    // looking the way it did. (The crawler, when it arrived, changed every body once, on purpose.)
    SeededRandom random(AnatomyStream(seed));
    CreatureAnatomy a;
    a.seed = seed;

    // Every kind turns up about as often as any other, so the next one through the door could be
    // anything: something shaped too much like a person on its hands and feet, a long low four-legged
    // thing, a six-legged one splayed across the floor, or one that stands up on two.
    const float plan = random.Unit();
    a.plan = plan < 0.3f    ? BodyPlan::Crawler
             : plan < 0.56f ? BodyPlan::Quadruped
             : plan < 0.78f ? BodyPlan::Hexapod
                            : BodyPlan::Biped;
    const bool crawler = a.plan == BodyPlan::Crawler;
    // Overall scale, weighted towards the middle and away from the largest: a big one is an event.
    const float s = 0.75f + 0.6f * std::pow(random.Unit(), 1.3f);

    switch (a.plan)
    {
    case BodyPlan::Quadruped:
        a.length = random.Range(1.1f, 1.7f) * s;
        a.width = random.Range(0.36f, 0.6f) * s;
        a.depth = random.Range(0.32f, 0.5f) * s;
        a.hipHeight = random.Range(0.45f, 0.8f) * s;
        a.hunch = random.Range(0.0f, 0.18f) * s;
        break;
    case BodyPlan::Hexapod:
        a.length = random.Range(1.2f, 1.9f) * s;
        a.width = random.Range(0.4f, 0.65f) * s;
        a.depth = random.Range(0.26f, 0.4f) * s;
        a.hipHeight = random.Range(0.3f, 0.55f) * s;
        a.hunch = random.Range(0.0f, 0.06f) * s;
        break;
    case BodyPlan::Crawler:
        // Shoulders to pelvis, as wide as a person's shoulders and thin enough to count the ribs. Its
        // arms are longer than its legs, so the shoulders ride high and the back slopes down to the hips.
        a.length = random.Range(0.62f, 0.85f) * s;
        a.width = random.Range(0.38f, 0.5f) * s;
        a.depth = random.Range(0.22f, 0.3f) * s;
        a.hipHeight = random.Range(0.5f, 0.72f) * s;
        a.hunch = random.Range(0.18f, 0.4f) * s;
        break;
    case BodyPlan::Biped:
    case BodyPlan::Count:
        a.length = random.Range(0.9f, 1.3f) * s;
        a.width = random.Range(0.36f, 0.55f) * s;
        a.depth = random.Range(0.4f, 0.6f) * s;
        a.hipHeight = random.Range(0.8f, 1.2f) * s;
        a.hunch = random.Range(0.0f, 0.1f) * s;
        break;
    }
    a.segments = RangeInt(random, 3, 6);
    a.taper = random.Range(0.0f, 0.4f);

    // Legs. Each is made long enough to reach the ground from its hip with the knee a little bent,
    // which is how anything stands: a straight leg is a leg with nowhere left to go.
    // Now and then a crawler has a second, smaller pair of arms under the first.
    const bool extraArms = crawler && random.Unit() < 0.25f;
    const int pairs = a.plan == BodyPlan::Hexapod ? 3 : (a.plan == BodyPlan::Biped ? 1 : (extraArms ? 3 : 2));
    for (int p = 0; p < pairs; ++p)
    {
        LegPair leg;
        leg.along = pairs == 1 ? 0.55f : (0.1f + 0.8f * static_cast<float>(p) / static_cast<float>(pairs - 1));
        leg.hipSpread = a.width * 0.5f * random.Range(0.6f, 0.9f);
        leg.footSpread = a.plan == BodyPlan::Hexapod ? a.width * 0.5f + random.Range(0.4f, 0.75f) * s
                                                     : leg.hipSpread + random.Range(0.0f, 0.08f) * s;
        leg.thickness = (a.plan == BodyPlan::Hexapod   ? random.Range(0.035f, 0.06f)
                         : a.plan == BodyPlan::Biped   ? random.Range(0.08f, 0.12f)
                                                       : random.Range(0.05f, 0.09f)) * s;
        leg.foot = random.Range(0.1f, 0.2f) * s;
        float bend = random.Range(1.12f, 1.3f);
        if (crawler)
        {
            // Arms at the very front, from the ends of the shoulders, hands planted out ahead and wide;
            // legs at the very back, from the pelvis, feet a little behind. The arms are thin and long
            // and folded, the elbows standing up above the back like a spider's.
            leg.arm = p < pairs - 1;
            leg.along = !leg.arm ? 1.0f : (p == 0 ? 0.0f : 0.3f);
            leg.hipSpread = a.width * 0.5f * (leg.arm ? random.Range(0.85f, 1.0f) : random.Range(0.45f, 0.62f));
            leg.footSpread = leg.hipSpread + random.Range(0.03f, 0.14f) * s;
            leg.footForward = leg.arm ? -random.Range(0.14f, 0.32f) * s : random.Range(0.0f, 0.1f) * s;
            leg.thickness = (leg.arm ? random.Range(0.048f, 0.064f) : random.Range(0.055f, 0.075f)) * s;
            leg.foot = (leg.arm ? random.Range(0.13f, 0.18f) : random.Range(0.14f, 0.19f)) * s;
            bend = leg.arm ? random.Range(1.15f, 1.3f) : random.Range(1.14f, 1.3f);
            if (leg.arm && p > 0)
            {
                leg.hipSpread *= 0.75f;
                leg.thickness *= 0.8f;
                leg.footForward *= 0.5f;
            }
        }
        const float drop = a.HipY(leg.along) - leg.thickness;
        const float out = leg.footSpread - leg.hipSpread;
        const float reach = std::sqrt(drop * drop + out * out + leg.footForward * leg.footForward);
        const float total = reach * bend;
        leg.upper = total * random.Range(0.42f, 0.55f);
        leg.lower = total - leg.upper;
        // Hind legs of a four-legged one, and both of a two-legged one, bend backwards, as a dog's
        // and a bird's do. Six-legged ones bend upwards, and a crawler's elbows up and out, which the
        // body works out for itself.
        leg.backwardKnee = a.plan == BodyPlan::Biped || (a.plan == BodyPlan::Quadruped && p == pairs - 1);
        a.legs.push_back(leg);
    }

    a.neckLength = (a.plan == BodyPlan::Hexapod   ? random.Range(0.05f, 0.15f)
                    : a.plan == BodyPlan::Biped   ? random.Range(0.25f, 0.5f)
                    : crawler                     ? random.Range(0.1f, 0.22f)
                                                  : random.Range(0.15f, 0.45f)) * s;
    a.neckThickness = std::min(a.width, a.depth) * random.Range(0.35f, 0.5f);
    a.headLength = random.Range(0.3f, 0.6f) * s;
    a.headWidth = random.Range(0.22f, 0.38f) * s;
    a.headDepth = random.Range(0.2f, 0.32f) * s;
    a.jawLength = a.headLength * random.Range(0.6f, 1.05f);
    if (crawler)
    {
        // A head about a person's size, which is the worst of it: a skull, not a snout.
        a.headLength = random.Range(0.22f, 0.29f) * s;
        a.headWidth = random.Range(0.15f, 0.2f) * s;
        a.headDepth = random.Range(0.19f, 0.25f) * s;
        a.jawLength = a.headLength * random.Range(0.72f, 0.95f);
        a.neckThickness = std::min(a.width, a.depth) * random.Range(0.32f, 0.4f);
    }

    const float eyes = random.Unit();
    a.eyes = crawler ? (eyes < 0.16f ? 0 : (eyes < 0.84f ? 2 : 4))
                     : (eyes < 0.12f ? 0 : (eyes < 0.57f ? 2 : (eyes < 0.85f ? 4 : 6)));
    a.eyeSize = random.Range(0.022f, 0.05f) * s;
    a.eyeGlow = kEyeGlows[RangeInt(random, 0, static_cast<int>(std::size(kEyeGlows)) - 1)];
    const float frills = random.Unit();
    a.frills = a.eyes == 0 ? random.Range(0.6f, 1.0f) : (frills < 0.35f ? random.Range(0.3f, 1.0f) : 0.0f);
    if (crawler)
    {
        // Frills on a skull would be a costume. A blind one listens with the whole head instead.
        a.frills = a.eyes == 0 ? 0.8f : 0.0f;
    }

    const float tail = random.Unit();
    a.tailLength = (a.plan == BodyPlan::Biped      ? random.Range(1.1f, 1.8f)
                    : a.plan == BodyPlan::Hexapod  ? (tail < 0.4f ? 0.0f : random.Range(0.2f, 0.6f))
                    : crawler                      ? 0.0f
                                                   : random.Range(0.3f, 1.3f)) * s;
    a.tailSegments = RangeInt(random, 3, 6);
    a.tailThickness = a.depth * random.Range(0.18f, 0.3f);

    a.plates = random.Unit() < 0.35f && !crawler ? RangeInt(random, 2, 6) : 0;
    a.spines = random.Unit() < 0.4f ? RangeInt(random, 3, 9) : 0;
    a.spineLength = random.Range(0.08f, 0.3f) * s;
    if (crawler)
    {
        // Short bony spurs where the vertebrae push through, rather than a crest.
        a.spineLength *= 0.45f;
    }

    const glm::vec3 skin = kSkins[RangeInt(random, 0, static_cast<int>(std::size(kSkins)) - 1)];
    const glm::vec3 shade{random.Range(0.92f, 1.08f), random.Range(0.92f, 1.08f), random.Range(0.92f, 1.08f)};
    a.skin = glm::clamp(skin * shade, glm::vec3(0.05f), glm::vec3(0.8f));
    a.underside = a.skin * random.Range(0.5f, 0.75f);
    // Slick rather than dry: a little shine is what makes pale skin look wet.
    a.roughness = random.Range(0.38f, 0.62f);

    // The details, drawn last. A crawler's face is flatter and its fingers longer; an animal's face
    // juts. Otherwise anything goes: starved or merely lean, a mouth hanging open or shut, a few long
    // fangs or rows of short ones.
    a.snout = crawler ? random.Range(0.6f, 1.1f) : random.Range(0.9f, 1.8f);
    a.gape = random.Range(0.1f, 1.0f);
    a.teeth = RangeInt(random, 5, 14);
    a.toothLength = random.Range(0.6f, 1.7f) * (a.teeth < 8 ? 1.3f : 1.0f);
    a.ribs = crawler ? random.Range(0.55f, 1.0f) : random.Range(0.0f, 0.8f);
    a.fingers = crawler ? RangeInt(random, 3, 5) : RangeInt(random, 2, 4);
    a.clawLength = random.Range(0.6f, 1.6f);
    a.brow = random.Range(0.2f, 1.0f);
    a.cranium = random.Range(0.85f, 1.25f);
    return a;
}
CreatureAnatomy::RestPose CreatureAnatomy::Rest() const
{
    RestPose pose;
    const float shoulderY = hipHeight + depth * 0.5f + hunch;
    const float rumpY = hipHeight + depth * 0.5f;
    pose.shoulders = {0.0f, shoulderY, -length * 0.5f};
    pose.rump = {0.0f, rumpY, length * 0.5f};
    for (int i = 0; i < segments; ++i)
    {
        const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(segments);
        pose.spine.push_back(pose.shoulders + (pose.rump - pose.shoulders) * t);
        pose.spineRadius.push_back(width * 0.5f * (1.0f - taper * t));
    }

    // Neck forward and a little down; a two-legged one carries its head up, over the body, and a
    // crawler thrusts its head straight out in front, level with its shoulders.
    pose.neckBase = pose.shoulders + glm::vec3(0.0f, depth * 0.1f, -0.05f);
    const glm::vec3 neckDirection = glm::normalize(plan == BodyPlan::Biped     ? glm::vec3(0.0f, 0.35f, -1.0f)
                                                   : plan == BodyPlan::Crawler ? glm::vec3(0.0f, 0.08f, -1.0f)
                                                                               : glm::vec3(0.0f, -0.15f, -1.0f));
    pose.head = pose.neckBase + neckDirection * neckLength + glm::vec3(0.0f, 0.0f, -headLength * 0.5f);
    pose.jaw = pose.head + glm::vec3(0.0f, -headDepth * 0.3f, -headLength * 0.1f);
    // The eyes are sockets in the face of the skull, set close together under the brow; a second and
    // third pair sit above and behind the first.
    for (int k = 0; k < eyes / 2; ++k)
    {
        const float row = static_cast<float>(k);
        for (const float side : {-1.0f, 1.0f})
        {
            pose.eyes.push_back(pose.head + glm::vec3(side * headWidth * (0.21f + 0.05f * row),
                                                      headDepth * (0.1f + 0.13f * row),
                                                      -headLength * (0.3f - 0.1f * row)));
        }
    }

    for (const LegPair& pair : legs)
    {
        const float z = -length * 0.5f + length * pair.along;
        const float y = HipY(pair.along);
        for (const float side : {-1.0f, 1.0f})
        {
            Leg leg;
            leg.hip = {side * pair.hipSpread, y, z};
            leg.foot = {side * pair.footSpread, 0.0f, z + pair.footForward};
            leg.pair = &pair;
            leg.side = side;
            pose.legs.push_back(leg);
        }
    }

    pose.tailBase = pose.rump;
    pose.tailDirection = glm::normalize(plan == BodyPlan::Biped ? glm::vec3(0.0f, 0.05f, 1.0f)
                                                                : glm::vec3(0.0f, -0.25f, 1.0f));
    return pose;
}

float CreatureAnatomy::HipY(float along) const
{
    // Just under the body, and higher towards the front by however much the shoulders are raised.
    return hipHeight + depth * 0.15f + hunch * (1.0f - along);
}

float CreatureAnatomy::TopHeight() const
{
    const RestPose pose = Rest();
    const float back = pose.shoulders.y + depth * 0.5f + (spines > 0 ? spineLength * 0.7f : 0.0f);
    return std::max(back, pose.head.y + headDepth * 0.5f);
}

float CreatureAnatomy::OverallLength() const
{
    return length + neckLength + headLength + tailLength;
}

std::string CreatureAnatomy::Describe() const
{
    char line[320];
    std::snprintf(line, sizeof(line),
                  "%s, %.1f m long overall, %.2f m tall, %d eyes%s, %s%s%s",
                  BodyPlanName(plan), OverallLength(), TopHeight(), eyes, eyes == 0 ? " (hunts by sound)" : "",
                  tailLength > 0.05f ? "a tail" : "no tail", plates > 0 ? ", plated" : "",
                  spines > 0 ? ", spined" : "");
    return line;
}

CreatureCapabilities CreatureCapabilities::From(const CreatureAnatomy& a)
{
    CreatureCapabilities c;
    const CreatureAnatomy::RestPose pose = a.Rest();

    // Bulk: the body as an ellipsoid, a quarter again for legs, head and tail, at about the density of
    // flesh; and whatever the plates weigh on top.
    const float body = glm::pi<float>() / 6.0f * a.length * a.width * a.depth;
    // A crawler's long limbs and head weigh more than a quarter of its thin body: about as much again.
    const float limbs = a.plan == BodyPlan::Crawler ? 2.4f : 1.25f;
    c.mass = body * limbs * 950.0f + static_cast<float>(a.plates) * 8.0f;
    // A lot of it. Something that hunts a team of armed people and only dies to two or three magazines
    // is a predator; one that drops to a burst is a target. A medium body, about 170 kg, takes about
    // 1600 -- some seventy-five carbine rounds -- and the biggest take far more.
    c.health = std::clamp(400.0f + c.mass * 7.0f, 700.0f, 4000.0f);
    c.armour = std::min(static_cast<float>(a.plates) * 0.05f, 0.3f);

    // Speed from its legs, and a little less for the weight they carry.
    float legLength = 0.0f;
    for (const LegPair& leg : a.legs)
    {
        legLength += leg.upper + leg.lower;
    }
    legLength /= static_cast<float>(std::max<size_t>(a.legs.size(), 1));
    const float planPace = a.plan == BodyPlan::Biped ? 1.08f : (a.plan == BodyPlan::Hexapod ? 0.95f : 1.0f);
    c.runSpeed = std::clamp((3.0f + 2.8f * legLength) * planPace * (1.05f - c.mass / 2500.0f), 3.6f, 6.8f);
    c.walkSpeed = c.runSpeed * 0.32f;

    // Senses. No eyes is no sight -- it knows only what it touches -- and the best hearing of any.
    c.sight = a.eyes == 0 ? 0.0f
                          : (a.eyes == 2 ? 1.0f : (a.eyes == 4 ? 1.1f : 1.18f)) *
                                (0.9f + std::min(a.eyeSize / 0.05f, 1.0f) * 0.2f);
    // Ordinary ears hear as well as every creature did before bodies; frills that catch sound do better.
    c.hearing = a.eyes == 0 ? 1.6f : 1.0f + 0.25f * a.frills;

    // The blow: weight behind it and jaws to do it with, reaching as far as neck and head let it.
    c.strikeDamage = std::clamp(14.0f + c.mass * 0.06f + a.jawLength * 18.0f, 18.0f, 45.0f);
    c.strikeReach = std::clamp(1.3f + a.neckLength + a.headLength * 0.8f + (a.plan == BodyPlan::Biped ? 0.2f : 0.0f) +
                                   (a.plan == BodyPlan::Crawler ? 0.35f : 0.0f),
                               1.8f, 3.0f);

    const float height = a.TopHeight();
    float footSpan = a.width;
    for (const LegPair& leg : a.legs)
    {
        footSpan = std::max(footSpan, leg.footSpread * 2.0f);
    }
    if (height < 1.0f && a.width < 0.5f && footSpan < 1.0f)
    {
        c.size = SizeClass::Small;
    }
    else if (height > 1.55f || a.width > 0.75f || footSpan > 1.8f || a.OverallLength() > 3.6f)
    {
        c.size = SizeClass::Large;
    }
    else
    {
        c.size = SizeClass::Medium;
    }
    // Squeezed down on its belly a body loses about two thirds of its hips' height: anything small, and
    // the lower of the middling ones, gets through a crawlspace a person has to crawl along.
    c.fitsVents = c.size == SizeClass::Small || (c.size == SizeClass::Medium && height < 1.35f);
    c.climbs = a.plan == BodyPlan::Hexapod || c.mass < 100.0f;
    // Anything that climbs gets up nearly three metres; the rest jump as high as their legs throw them.
    c.jump = c.climbs ? 2.7f : std::clamp(0.5f + legLength * 0.95f, 0.8f, 1.95f);
    c.verticalReach = std::clamp(height * 0.75f + 0.45f + (a.plan == BodyPlan::Crawler ? 0.35f : 0.0f), 1.2f, 2.8f);

    // The box rounds hit: from the ground to the top of its back, as wide as the body, from the tip of
    // its head to its rump. The legs are inside it, so a round at a leg is a round that hits.
    const float front = -(pose.head.z - a.headLength * 0.5f);
    const float back = pose.rump.z;
    c.bodyHalfExtents = {std::max(a.width, 0.3f) * 0.5f + 0.05f, height * 0.5f, (front + back) * 0.5f};
    c.bodyCentre = {0.0f, height * 0.5f, (back - front) * 0.5f};

    // Sees from between its eyes, or where they would be.
    c.eye = pose.eyes.empty() ? pose.head + glm::vec3(0.0f, a.headDepth * 0.2f, -a.headLength * 0.3f)
                              : pose.eyes.front() * glm::vec3(0.0f, 1.0f, 1.0f);
    return c;
}

std::string CreatureCapabilities::Describe() const
{
    char line[320];
    std::snprintf(line, sizeof(line),
                  "%s, %.0f kg, %.0f health%s, runs %.1f m/s, sight x%.2f, hearing x%.2f, strikes %.0f at %.1f m%s%s",
                  SizeClassName(size), mass, health,
                  armour > 0.0f ? (" (plates stop " + std::to_string(static_cast<int>(armour * 100.0f)) + "%)").c_str() : "",
                  runSpeed, sight, hearing, strikeDamage, strikeReach, fitsVents ? ", fits vents" : "",
                  climbs ? ", climbs" : "");
    return line;
}

} // namespace pred
