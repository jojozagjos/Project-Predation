#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <vector>

namespace pred
{

// What the creatures have heard people say, kept so that some of them can say it back.
//
// The host keeps the last few things each player said -- each a phrase, the run of voice frames between
// two pauses -- as the encoded frames that already arrived, so saying one again is only sending the same
// frames from somewhere else. Only from players who have allowed it (the "may be mimicked" bit their own
// game puts on every frame they send), only in memory, and only for the match: nothing is ever written
// anywhere, and a player who leaves takes their phrases with them.
class VoiceMemory
{
public:
    struct Phrase
    {
        uint8_t player = 0;
        std::vector<std::vector<uint8_t>> frames;
        float heardAt = 0.0f;
    };

    // Frames this long apart or more are two phrases, not one: a breath, a pause between sentences.
    static constexpr float kPause = 0.35f;
    // Shorter than this is a cough or a click, not something worth saying back; longer is cut off here.
    static constexpr size_t kShortest = 25; // half a second, at twenty milliseconds a frame
    static constexpr size_t kLongest = 175; // three and a half seconds
    // How many phrases are kept for each player, the newest.
    static constexpr size_t kKept = 12;

    // A frame of somebody's voice, heard at `now` (seconds).
    void Heard(uint8_t player, const std::vector<uint8_t>& frame, float now);
    // Everything of theirs, gone: they left, or asked not to be mimicked.
    void Forget(uint8_t player);
    void Clear();

    // Whether there is anything of theirs to say.
    bool Has(uint8_t player) const;
    // One of their phrases, chosen by `seed`, preferring ones that have not just been used. Null when
    // there are none.
    const Phrase* Pick(uint8_t player, uint32_t seed);
    size_t Count(uint8_t player) const;

private:
    struct Voice
    {
        std::deque<Phrase> phrases;
        Phrase current;
        float lastFrameAt = -1.0e9f;
        std::vector<uint32_t> used;
    };
    void Close(Voice& voice);
    std::map<uint8_t, Voice> m_voices;
};

} // namespace pred
