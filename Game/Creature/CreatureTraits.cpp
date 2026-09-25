#include "Game/Creature/CreatureTraits.h"
#include "Game/Creature/CreatureTuning.h"

#include "Game/Creature/Noise.h"

#include <cstdio>

namespace pred
{

const char* NoiseKindName(NoiseKind kind)
{
    switch (kind)
    {
    case NoiseKind::Footstep:
        return "footstep";
    case NoiseKind::Gunshot:
        return "gunshot";
    case NoiseKind::Impact:
        return "impact";
    case NoiseKind::Door:
        return "door";
    case NoiseKind::Item:
        return "item";
    case NoiseKind::Voice:
        return "voice";
    case NoiseKind::Call:
        return "call";
    }
    return "noise";
}

uint64_t SeededRandom::Next()
{
    m_state += 0x9E3779B97F4A7C15ull;
    uint64_t z = m_state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

float SeededRandom::Unit()
{
    // The top 24 bits, which a float holds exactly.
    return static_cast<float>(Next() >> 40) / static_cast<float>(1u << 24);
}

CreatureTraits CreatureTraits::FromSeed(uint32_t seed)
{
    // Drawn in a fixed order, so adding a trait at the end later does not change any creature that
    // already exists -- a seed somebody wrote down keeps meaning the same animal.
    SeededRandom random(seed);
    CreatureTraits traits;
    traits.seed = seed;
    traits.aggression = random.Range(0.25f, 0.95f);
    traits.fear = random.Range(0.2f, 0.9f);
    traits.curiosity = random.Range(0.3f, 1.0f);
    traits.persistence = random.Range(8.0f, 24.0f);
    traits.perception = random.Range(0.8f, 1.25f);
    traits.runSpeed = random.Range(4.4f, 6.0f);
    traits.walkSpeed = random.Range(1.3f, 2.0f);
    // Milestone 9. After everything above, so each creature seeded before keeps its temperament and
    // only gains these.
    traits.patience = random.Range(0.1f, 1.0f);
    traits.stealth = random.Range(0.1f, 1.0f);
    traits.isolationPreference = random.Range(0.2f, 1.0f);
    // Drawn last, so every seed keeps the temperament it had: how much of a nest-builder it is. Most are
    // not; the ones that are take what they catch home.
    traits.nesting = random.Range(0.0f, 1.0f);
    // And last of all, how it takes to people. Weighted by what it already is, so the timid ones are
    // the fearful, unaggressive ones and the curious ones the curious, unaggressive ones -- about six in
    // ten hunt, two hold ground, and the rest keep away or watch.
    {
        const float weights[] = {
            0.55f * (0.5f + traits.aggression),
            0.22f * (1.5f - traits.stealth),
            0.20f * (0.1f + 2.2f * traits.fear * traits.fear) * (1.3f - traits.aggression),
            0.12f * (0.3f + 1.4f * traits.curiosity) * (1.3f - traits.aggression),
        };
        float total = 0.0f;
        for (const float weight : weights)
        {
            total += weight;
        }
        float roll = random.Unit() * total;
        traits.temperament = Temperament::Curious;
        for (int i = 0; i < 4; ++i)
        {
            if (roll < weights[i])
            {
                traits.temperament = static_cast<Temperament>(i);
                break;
            }
            roll -= weights[i];
        }
    }
    // Its habits, from a stream of their own so nothing above changes: as many as creatures.json says,
    // each as likely as it says, never two that contradict each other.
    {
        SeededRandom habits(static_cast<uint64_t>(seed) * 0x9E3779B97F4A7C15ull + 0x51ED270Bull);
        const int count = static_cast<int>(Quirk::Count);
        const auto clashes = [&](int a, int b)
        {
            const auto is = [&](Quirk x, Quirk y)
            { return (a == static_cast<int>(x) && b == static_cast<int>(y)) || (a == static_cast<int>(y) && b == static_cast<int>(x)); };
            return is(Quirk::LightChaser, Quirk::LightShy) || is(Quirk::Silent, Quirk::Clicker) ||
                   is(Quirk::Silent, Quirk::Knocker) || is(Quirk::Silent, Quirk::Shrieker);
        };
        const auto& weights = Tuning().quirkWeights;
        for (int pick = 0; pick < Tuning().quirksPerCreature; ++pick)
        {
            float total = 0.0f;
            for (int q = 0; q < count; ++q)
            {
                bool allowed = (traits.quirks & (1u << q)) == 0;
                for (int other = 0; other < count && allowed; ++other)
                {
                    allowed = !((traits.quirks & (1u << other)) != 0 && clashes(q, other));
                }
                total += allowed ? weights[static_cast<size_t>(q)] : 0.0f;
            }
            if (total <= 0.0f)
            {
                break;
            }
            float roll = habits.Unit() * total;
            for (int q = 0; q < count; ++q)
            {
                bool allowed = (traits.quirks & (1u << q)) == 0;
                for (int other = 0; other < count && allowed; ++other)
                {
                    allowed = !((traits.quirks & (1u << other)) != 0 && clashes(q, other));
                }
                const float weight = allowed ? weights[static_cast<size_t>(q)] : 0.0f;
                if (weight > 0.0f && roll < weight)
                {
                    traits.quirks = static_cast<uint16_t>(traits.quirks | (1u << q));
                    break;
                }
                roll -= weight;
            }
        }
    }
    return traits;
}

const char* QuirkName(Quirk quirk)
{
    switch (quirk)
    {
    case Quirk::CeilingDweller: return "goes about on the ceiling";
    case Quirk::Knocker: return "knocks on the walls";
    case Quirk::Shrieker: return "shrieks when it sees you";
    case Quirk::Watcher: return "a patient watcher";
    case Quirk::HitAndRun: return "hits and runs";
    case Quirk::LightChaser: return "goes for lights";
    case Quirk::LightShy: return "afraid of lights";
    case Quirk::Faker: return "plays dead";
    case Quirk::Pacer: return "paces";
    case Quirk::Clicker: return "clicks in the dark";
    case Quirk::Silent: return "silent";
    case Quirk::Baiter: return "waits by the dead";
    case Quirk::Count: break;
    }
    return "?";
}

const char* TemperamentName(Temperament temperament)
{
    switch (temperament)
    {
    case Temperament::Predator: return "predator";
    case Temperament::Territorial: return "territorial";
    case Temperament::Timid: return "timid";
    case Temperament::Curious: return "curious";
    case Temperament::Count: break;
    }
    return "?";
}

std::string CreatureTraits::Describe() const
{
    char line[360];
    std::snprintf(line, sizeof(line),
                  "seed %u  %s  aggression %.2f  fear %.2f  curiosity %.2f  persistence %.0fs  "
                  "senses x%.2f  run %.1f m/s  patience %.2f  stealth %.2f  prefers loners %.2f%s%s",
                  seed, TemperamentName(temperament), aggression, fear, curiosity, persistence, perception, runSpeed,
                  patience, stealth, isolationPreference, Captures() ? "  captures" : "", Nests() ? "  nests" : "");
    std::string text = line;
    for (int q = 0; q < static_cast<int>(Quirk::Count); ++q)
    {
        if (Has(static_cast<Quirk>(q)))
        {
            text += std::string("; ") + QuirkName(static_cast<Quirk>(q));
        }
    }
    return text;
}

} // namespace pred

namespace pred
{

float CreatureTraits::StalkPatienceSeconds() const
{
    return (Tuning().stalkPatience + Tuning().stalkPatienceRange * patience) * (Has(Quirk::Watcher) ? 2.0f : 1.0f);
}

} // namespace pred
