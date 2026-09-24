#include "Game/Creature/VoiceMemory.h"

#include <algorithm>

namespace pred
{

void VoiceMemory::Close(Voice& voice)
{
    // A phrase finished: kept if it is long enough to be a phrase at all.
    if (voice.current.frames.size() >= kShortest)
    {
        voice.phrases.push_back(std::move(voice.current));
        while (voice.phrases.size() > kKept)
        {
            voice.phrases.pop_front();
        }
        voice.used.clear();
    }
    voice.current = Phrase{};
}

void VoiceMemory::Heard(uint8_t player, const std::vector<uint8_t>& frame, float now)
{
    if (frame.empty())
    {
        return;
    }
    Voice& voice = m_voices[player];
    if (now - voice.lastFrameAt > kPause || voice.current.frames.size() >= kLongest)
    {
        Close(voice);
        voice.current.player = player;
        voice.current.heardAt = now;
    }
    voice.current.frames.push_back(frame);
    voice.lastFrameAt = now;
}

void VoiceMemory::Forget(uint8_t player)
{
    m_voices.erase(player);
}

void VoiceMemory::Clear()
{
    m_voices.clear();
}

bool VoiceMemory::Has(uint8_t player) const
{
    return Count(player) > 0;
}

size_t VoiceMemory::Count(uint8_t player) const
{
    const auto found = m_voices.find(player);
    if (found == m_voices.end())
    {
        return 0;
    }
    // A phrase still being spoken counts once it is long enough to be one.
    return found->second.phrases.size() + (found->second.current.frames.size() >= kShortest ? 1 : 0);
}

const VoiceMemory::Phrase* VoiceMemory::Pick(uint8_t player, uint32_t seed)
{
    const auto found = m_voices.find(player);
    if (found == m_voices.end())
    {
        return nullptr;
    }
    Voice& voice = found->second;
    // One they have stopped saying, if there is one: a phrase still coming in is not finished yet.
    if (voice.phrases.empty())
    {
        return nullptr;
    }
    // Not the same thing twice running while there is anything else: a voice saying one sentence over
    // and over is a recording, and everybody knows it.
    std::vector<uint32_t> fresh;
    for (uint32_t i = 0; i < voice.phrases.size(); ++i)
    {
        if (std::find(voice.used.begin(), voice.used.end(), i) == voice.used.end())
        {
            fresh.push_back(i);
        }
    }
    if (fresh.empty())
    {
        voice.used.clear();
        for (uint32_t i = 0; i < voice.phrases.size(); ++i)
        {
            fresh.push_back(i);
        }
    }
    const uint32_t chosen = fresh[(seed * 2654435761u >> 7) % fresh.size()];
    voice.used.push_back(chosen);
    return &voice.phrases[chosen];
}

} // namespace pred
