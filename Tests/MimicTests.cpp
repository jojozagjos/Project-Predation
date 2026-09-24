#include "Engine/Net/BitStream.h"
#include "Game/Creature/VoiceMemory.h"
#include "Game/Net/Protocol.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <vector>

using namespace pred;

// What the creatures remember of what people say, and how a creature saying it back travels.

namespace
{

// Somebody talking for `seconds`, one frame every twenty milliseconds, starting at `from`.
float Say(VoiceMemory& memory, uint8_t player, float from, float seconds, uint8_t marker)
{
    float t = from;
    for (int i = 0; i < static_cast<int>(seconds / 0.02f); ++i, t += 0.02f)
    {
        memory.Heard(player, std::vector<uint8_t>{marker, static_cast<uint8_t>(i)}, t);
    }
    return t;
}

} // namespace

TEST_CASE("What somebody says is kept as phrases split at their pauses", "[mimic]")
{
    VoiceMemory memory;
    CHECK_FALSE(memory.Has(1));
    float t = Say(memory, 1, 0.0f, 1.2f, 10);
    t = Say(memory, 1, t + 0.8f, 0.2f, 11); // a cough: too short to keep
    t = Say(memory, 1, t + 0.8f, 2.0f, 12);
    // One more pause closes the last phrase.
    Say(memory, 1, t + 1.0f, 0.1f, 13);
    REQUIRE(memory.Count(1) == 2);
    const VoiceMemory::Phrase* first = memory.Pick(1, 7);
    const VoiceMemory::Phrase* second = memory.Pick(1, 7);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    // Not the same one twice running while there is another.
    CHECK(first != second);
    const std::set<uint8_t> markers{first->frames.front()[0], second->frames.front()[0]};
    CHECK(markers == std::set<uint8_t>{10, 12});
    CHECK(first->player == 1);
}

TEST_CASE("A phrase that goes on is cut, and only so many are kept", "[mimic]")
{
    VoiceMemory memory;
    Say(memory, 2, 0.0f, 10.0f, 1);
    Say(memory, 2, 20.0f, 0.1f, 2);
    // Ten seconds without a pause is several phrases of at most three and a half seconds.
    CHECK(memory.Count(2) >= 2);
    for (uint32_t seed = 0; seed < 8; ++seed)
    {
        const VoiceMemory::Phrase* phrase = memory.Pick(2, seed);
        REQUIRE(phrase != nullptr);
        CHECK(phrase->frames.size() <= VoiceMemory::kLongest);
    }
    float t = 30.0f;
    for (int i = 0; i < 20; ++i)
    {
        t = Say(memory, 3, t + 1.0f, 0.8f, static_cast<uint8_t>(i));
    }
    CHECK(memory.Count(3) <= VoiceMemory::kKept + 1);
}

TEST_CASE("Somebody who takes back their leave, or leaves, is forgotten", "[mimic]")
{
    VoiceMemory memory;
    Say(memory, 4, 0.0f, 1.0f, 1);
    Say(memory, 4, 3.0f, 0.1f, 1);
    REQUIRE(memory.Has(4));
    memory.Forget(4);
    CHECK_FALSE(memory.Has(4));
    CHECK(memory.Pick(4, 1) == nullptr);
}

TEST_CASE("A voice frame says whose it is: a player's or a creature's", "[mimic][net][protocol]")
{
    for (const bool creature : {false, true})
    {
        VoiceMessage sent;
        sent.creature = creature;
        sent.speaker = creature ? 200 : 3;
        sent.sequence = 777;
        sent.frame = {1, 2, 3, 4, 5};
        BitWriter writer;
        WriteMessageHeader(writer, MessageType::Voice);
        WriteVoice(writer, sent);
        const std::vector<uint8_t>& bytes = writer.Finish();
        BitReader reader(bytes.data(), bytes.size());
        MessageType type = MessageType::Count;
        REQUIRE(ReadMessageHeader(reader, type));
        VoiceMessage received;
        REQUIRE(ReadVoice(reader, received));
        CHECK(received.creature == creature);
        CHECK(received.speaker == sent.speaker);
        CHECK(received.sequence == 777);
        CHECK(received.frame == sent.frame);
    }
}
