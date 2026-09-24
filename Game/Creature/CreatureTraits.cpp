#include "Game/Creature/CreatureTraits.h"

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
    return traits;
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
    return line;
}

} // namespace pred
