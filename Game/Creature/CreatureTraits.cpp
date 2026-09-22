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
    return traits;
}

std::string CreatureTraits::Describe() const
{
    char line[320];
    std::snprintf(line, sizeof(line),
                  "seed %u  aggression %.2f  fear %.2f  curiosity %.2f  persistence %.0fs  "
                  "senses x%.2f  run %.1f m/s  patience %.2f  stealth %.2f  prefers loners %.2f%s",
                  seed, aggression, fear, curiosity, persistence, perception, runSpeed, patience, stealth,
                  isolationPreference, Nests() ? "  nests" : "");
    return line;
}

} // namespace pred
