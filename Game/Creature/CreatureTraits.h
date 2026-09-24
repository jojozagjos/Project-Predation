#pragma once

#include <cstdint>
#include <string>

namespace pred
{

// How it takes to people at all.
//
// These are animals, not monsters: dangerous is not the same as malicious, and not every one of them
// wants anything to do with a person. Most hunt. Some hold ground and warn before they fight for it;
// some keep away and fight only when there is no way out; some just watch. What they all share is that
// one that has been hurt by somebody is dangerous to that somebody.
enum class Temperament : uint8_t
{
    Predator,    // hunts people
    Territorial, // keeps its ground: warns anybody who comes onto it, and fights if they stay
    Timid,       // keeps away; lashes out only when cornered or hurt at close quarters
    Curious,     // watches and follows; turns only on somebody who hurts it
    Count
};

const char* TemperamentName(Temperament temperament);

// What kind of animal this one is. Fixed for its life, and made entirely from its seed.
//
// The same seed always makes the same creature: the same temperament, the same senses, the same
// wandering. What it does in a match still changes -- it remembers, it gets hurt, it learns where
// people hide -- but where it starts is reproducible, which is what lets a strange behaviour be
// looked at again rather than described.
//
// Every value is 0 to 1 unless it says otherwise, and each one is a dial on a response rather than
// a switch: an aggressive creature does not always attack, it is more willing to.
struct CreatureTraits
{
    uint32_t seed = 0;

    // How readily it goes for somebody it has found.
    float aggression = 0.5f;
    // How quickly pain turns into getting away.
    float fear = 0.5f;
    // How strongly a noise it cannot explain pulls it towards it.
    float curiosity = 0.5f;
    // How long it keeps looking for somebody it has lost, in seconds.
    float persistence = 14.0f;
    // How sharp its senses are: a multiplier on how far it sees and hears.
    float perception = 1.0f;
    // How fast it runs and walks, in metres per second.
    float runSpeed = 5.2f;
    float walkSpeed = 1.6f;
    // How long something new holds its interest before it has seen enough, in seconds.
    float BoredomSeconds() const { return 15.0f + 45.0f * curiosity; }
    // How long it will shadow somebody before it stops waiting for a better moment. Scales a wait of
    // between about ten seconds and forty.
    float patience = 0.5f;
    // How much it prefers not to be seen: a stealthy one stalks from cover and waits for an opening,
    // a brazen one simply comes.
    float stealth = 0.5f;
    // How much more it wants somebody on their own than somebody with company.
    float isolationPreference = 0.5f;
    // How much of a nest-builder it is. Past two thirds it makes one somewhere dark and out of the way,
    // takes what it catches there, and goes back to it to heal; the rest never build anything.
    float nesting = 0.0f;
    // How it takes to people, drawn from the seed and weighted by the rest of its temperament: a timid
    // one is one that was already fearful and not very aggressive.
    Temperament temperament = Temperament::Predator;
    // Whether it takes people at all -- a grab, carried off -- rather than only fighting them. Only
    // some do, and only ones that fight at all. Those that build a nest take their catches there.
    bool Captures() const
    {
        return (temperament == Temperament::Predator || temperament == Temperament::Territorial) && nesting > 0.5f;
    }
    bool Nests() const { return Captures() && nesting > 0.66f; }
    // Whether it says back what it has heard people say, to draw them to it: the cunning ones among the
    // hunters and the watchers -- stealthy, and curious enough to have listened.
    bool Mimics() const
    {
        return (temperament == Temperament::Predator || temperament == Temperament::Curious) && stealth > 0.5f &&
               curiosity > 0.45f;
    }

    // Seconds it will stalk one person before patience runs out.
    float StalkPatienceSeconds() const { return 10.0f + 30.0f * patience; }

    // Set by its body rather than drawn here (see CreatureAnatomy): how well its eyes and ears work, on
    // top of how sharp it is, and how far a blow reaches. Sight 0 is a creature with no eyes at all.
    float sight = 1.0f;
    float hearing = 1.0f;
    float strikeReach = 2.3f;
    // How high its eyes are and the middle of its body, above its feet: where it looks from when it
    // picks a spot to peek from, and what a player would have to see to see it.
    float eyeHeight = 1.0f;
    float bodyMiddle = 0.7f;
    // Whether its body goes where a person has to crawl, and up walls and across ceilings.
    bool fitsVents = false;
    bool climbs = false;

    static CreatureTraits FromSeed(uint32_t seed);

    // One line, for the inspector and the log.
    std::string Describe() const;
};

// A small, fully specified random sequence.
//
// Not the standard library's generators: their output for a given seed is not the same on every
// compiler, and a creature that is reproducible only on the machine that made it is not
// reproducible. This is SplitMix64, which is a dozen lines and the same everywhere.
class SeededRandom
{
public:
    explicit SeededRandom(uint64_t seed) : m_state(seed) {}
    uint64_t Next();
    // [0, 1)
    float Unit();
    float Range(float low, float high) { return low + (high - low) * Unit(); }

private:
    uint64_t m_state;
};

} // namespace pred
