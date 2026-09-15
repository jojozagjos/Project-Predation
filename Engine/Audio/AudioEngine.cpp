#include "Engine/Audio/AudioEngine.h"

#include "Engine/Core/Log.h"

#include <SDL3/SDL.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace pred
{
namespace
{

// How fast a voice's channel gains are allowed to change, per frame of output, when a source moves
// or the listener turns. A gain that jumps from one buffer to the next is a click, and a click is
// the single most audible fault a mixer has. At 48 kHz this crosses the whole range in about eight
// milliseconds, which is faster than anything anybody can hear moving and slow enough to be silent.
constexpr float kGainStep = 1.0f / 384.0f;

void SDLCALL FeedStream(void* userdata, SDL_AudioStream* stream, int additional, int /*total*/)
{
    auto* engine = static_cast<AudioEngine*>(userdata);
    if (engine == nullptr || additional <= 0)
    {
        return;
    }

    // Asked for bytes, thought about in frames. Interleaved stereo floats, so one frame is eight
    // bytes; a request that is not a whole number of them is rounded down rather than trusted.
    constexpr int kBytesPerFrame = static_cast<int>(sizeof(float)) * 2;
    int framesWanted = additional / kBytesPerFrame;

    // In blocks, so a device asking for a quarter of a second at a time does not put a quarter of a
    // second of buffer on the stack.
    constexpr int kBlockFrames = 512;
    float block[kBlockFrames * 2];
    while (framesWanted > 0)
    {
        const int frames = std::min(framesWanted, kBlockFrames);
        engine->Mix(block, frames);
        SDL_PutAudioStreamData(stream, block, frames * kBytesPerFrame);
        framesWanted -= frames;
    }
}

} // namespace

AudioEngine::~AudioEngine()
{
    Shutdown();
}

bool AudioEngine::Init(const Settings& settings)
{
    Shutdown();
    m_settings = settings;
    m_settings.sampleRate = std::clamp(settings.sampleRate, 8000, 192000);
    m_settings.maxVoices = std::clamp(settings.maxVoices, 1, 256);

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        // No sound card, no driver, or a machine with audio switched off. The game is playable
        // without it, so this is said once and then forgotten about.
        PRED_LOG_WARN(Engine, "No audio: {}", SDL_GetError());
        return false;
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = m_settings.sampleRate;

    m_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, FeedStream, this);
    if (m_stream == nullptr)
    {
        PRED_LOG_WARN(Engine, "No audio device: {}", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }

    // Opened paused, which is the one thing about this API that catches everybody.
    SDL_ResumeAudioStreamDevice(m_stream);
    PRED_LOG_INFO(Engine, "Audio open: {} Hz stereo, up to {} voices", m_settings.sampleRate,
                  m_settings.maxVoices);
    return true;
}

void AudioEngine::Shutdown()
{
    if (m_stream != nullptr)
    {
        // Destroying the stream stops the callback, so nothing is mixing by the time the voices go.
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    std::lock_guard lock(m_mutex);
    m_voices.clear();
    m_streams.clear();
}

SoundId AudioEngine::Add(const std::string& name, SoundData data)
{
    std::lock_guard lock(m_mutex);
    const auto existing = m_byName.find(name);
    if (existing != m_byName.end())
    {
        m_sounds[existing->second] = std::move(data);
        return existing->second;
    }
    if (m_sounds.size() >= kInvalidSound)
    {
        return kInvalidSound;
    }
    const auto id = static_cast<SoundId>(m_sounds.size());
    m_sounds.push_back(std::move(data));
    m_byName.emplace(name, id);
    return id;
}

SoundId AudioEngine::Find(const std::string& name) const
{
    std::lock_guard lock(m_mutex);
    const auto found = m_byName.find(name);
    return found == m_byName.end() ? kInvalidSound : found->second;
}

int AudioEngine::AddRecipes(const std::string& jsonText)
{
    const std::vector<SoundLibraryEntry> entries = LoadSoundRecipes(jsonText);
    for (const SoundLibraryEntry& entry : entries)
    {
        Add(entry.name, Synthesise(entry.recipe, m_settings.sampleRate));
    }
    return static_cast<int>(entries.size());
}

VoiceId AudioEngine::Play(const PlayDesc& desc)
{
    std::lock_guard lock(m_mutex);
    if (desc.stream != kInvalidStream)
    {
        // A stream is allowed to be empty: that is the ordinary state of one at the moment somebody
        // starts talking, and refusing it here would mean the first syllable is the one that opens
        // the voice and therefore the one that is lost.
        if (FindStream(desc.stream) == nullptr)
        {
            return kInvalidVoice;
        }
    }
    else if (desc.sound >= m_sounds.size() || m_sounds[desc.sound].samples.empty())
    {
        return kInvalidVoice;
    }

    if (static_cast<int>(m_voices.size()) >= m_settings.maxVoices)
    {
        // Everything is busy. The quietest thing sounding gives way, which for a full mix is
        // whatever is furthest off: a footstep across the map losing to a gunshot beside you is the
        // right way round, and dropping the oldest instead would take the gunshot.
        auto quietest = m_voices.end();
        float worst = std::numeric_limits<float>::max();
        for (auto it = m_voices.begin(); it != m_voices.end(); ++it)
        {
            const float distance = it->positioned ? glm::distance(it->position, m_listenerPosition)
                                                  : 0.0f;
            const float loudness = it->gain / std::max(distance, 0.1f);
            if (!it->loop && loudness < worst)
            {
                worst = loudness;
                quietest = it;
            }
        }
        if (quietest == m_voices.end())
        {
            return kInvalidVoice;
        }
        m_voices.erase(quietest);
        ++m_stats.stolen;
    }

    Voice voice;
    voice.id = m_nextVoice++;
    if (m_nextVoice == kInvalidVoice)
    {
        ++m_nextVoice;
    }
    voice.sound = desc.sound;
    voice.stream = desc.stream;
    voice.position = desc.position;
    voice.positioned = desc.positioned;
    voice.loop = desc.loop;
    voice.gain = std::max(desc.gain, 0.0f);
    voice.pitch = std::clamp(desc.pitch, 0.05f, 8.0f);
    voice.nearDistance = std::max(desc.nearDistance, 0.01f);
    voice.farDistance = std::max(desc.farDistance, voice.nearDistance + 0.01f);
    m_voices.push_back(voice);
    ++m_stats.started;
    return voice.id;
}

AudioEngine::Stream* AudioEngine::FindStream(StreamId id)
{
    const auto found = std::find_if(m_streams.begin(), m_streams.end(),
                                    [&](const Stream& candidate) { return candidate.id == id; });
    return found == m_streams.end() ? nullptr : &*found;
}

const AudioEngine::Stream* AudioEngine::FindStream(StreamId id) const
{
    const auto found = std::find_if(m_streams.begin(), m_streams.end(),
                                    [&](const Stream& candidate) { return candidate.id == id; });
    return found == m_streams.end() ? nullptr : &*found;
}

StreamId AudioEngine::OpenStream(int sampleRate)
{
    std::lock_guard lock(m_mutex);
    if (m_streams.size() >= kInvalidStream)
    {
        return kInvalidStream;
    }
    Stream stream;
    stream.id = m_nextStream++;
    if (m_nextStream == kInvalidStream)
    {
        m_nextStream = 0;
    }
    stream.sampleRate = std::clamp(sampleRate, 8000, 192000);
    m_streams.push_back(std::move(stream));
    return m_streams.back().id;
}

void AudioEngine::PushStream(StreamId stream, const float* samples, size_t count)
{
    if (samples == nullptr || count == 0)
    {
        return;
    }
    std::lock_guard lock(m_mutex);
    Stream* target = FindStream(stream);
    if (target == nullptr || !target->open)
    {
        return;
    }
    target->pending.insert(target->pending.end(), samples, samples + count);

    // Past the cap, the oldest go.
    //
    // The alternative is to let the queue grow, and what that sounds like is somebody talking to you
    // from further and further in the past: a listener whose machine stalled for a second is then a
    // second behind the conversation for the rest of it and has no way to catch up. Dropping audio
    // is audible and being permanently late is worse.
    const auto cap = static_cast<size_t>(kMaxQueuedSeconds * static_cast<float>(target->sampleRate));
    if (target->pending.size() > cap)
    {
        const size_t drop = target->pending.size() - cap;
        target->pending.erase(target->pending.begin(),
                              target->pending.begin() + static_cast<ptrdiff_t>(drop));
        target->consumed += drop;
    }
}

size_t AudioEngine::StreamQueued(StreamId stream) const
{
    std::lock_guard lock(m_mutex);
    const Stream* target = FindStream(stream);
    return target == nullptr ? 0 : target->pending.size();
}

void AudioEngine::CloseStream(StreamId stream)
{
    std::lock_guard lock(m_mutex);
    Stream* target = FindStream(stream);
    if (target == nullptr)
    {
        return;
    }
    // Marked rather than removed. Whatever has already arrived is still played out, and the voice
    // reading it ends when it reaches the end rather than being cut off mid-word.
    target->open = false;
}

VoiceId AudioEngine::PlayAt(SoundId sound, const glm::vec3& position, float gain, float pitch)
{
    PlayDesc desc;
    desc.sound = sound;
    desc.position = position;
    desc.gain = gain;
    desc.pitch = pitch;
    return Play(desc);
}

void AudioEngine::Stop(VoiceId voice)
{
    std::lock_guard lock(m_mutex);
    const auto found = std::find_if(m_voices.begin(), m_voices.end(),
                                    [&](const Voice& candidate) { return candidate.id == voice; });
    if (found != m_voices.end())
    {
        // Marked rather than removed, so the mixer can take its gain down over a few hundred frames.
        // Cutting a sounding voice at a buffer boundary is a click.
        found->stopping = true;
        found->loop = false;
    }
}

void AudioEngine::StopAll()
{
    std::lock_guard lock(m_mutex);
    for (Voice& voice : m_voices)
    {
        voice.stopping = true;
        voice.loop = false;
    }
}

void AudioEngine::SetVoicePosition(VoiceId voice, const glm::vec3& position)
{
    std::lock_guard lock(m_mutex);
    if (Voice* found = FindVoice(voice); found != nullptr)
    {
        found->position = position;
    }
}

void AudioEngine::SetVoiceGain(VoiceId voice, float gain)
{
    std::lock_guard lock(m_mutex);
    if (Voice* found = FindVoice(voice); found != nullptr)
    {
        found->gain = std::max(gain, 0.0f);
    }
}

bool AudioEngine::IsPlaying(VoiceId voice) const
{
    std::lock_guard lock(m_mutex);
    return FindVoice(voice) != nullptr;
}

void AudioEngine::SetListener(const glm::vec3& position, const glm::vec3& forward,
                             const glm::vec3& up)
{
    std::lock_guard lock(m_mutex);
    m_listenerPosition = position;
    const float length = glm::length(forward);
    m_listenerForward = length > 1e-5f ? forward / length : glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::vec3 right = glm::cross(m_listenerForward, up);
    const float rightLength = glm::length(right);
    // Looking straight up or down, forward and up line up and the cross product collapses. Keeping
    // the last good answer is right: which way your ears face has not changed.
    if (rightLength > 1e-4f)
    {
        m_listenerRight = right / rightLength;
    }
}

void AudioEngine::SetMasterGain(float gain)
{
    std::lock_guard lock(m_mutex);
    m_masterGain = std::clamp(gain, 0.0f, 4.0f);
}

float AudioEngine::MasterGain() const
{
    std::lock_guard lock(m_mutex);
    return m_masterGain;
}

AudioEngine::Stats AudioEngine::GetStats() const
{
    std::lock_guard lock(m_mutex);
    Stats stats = m_stats;
    stats.voices = static_cast<int>(m_voices.size());
    return stats;
}

AudioEngine::Voice* AudioEngine::FindVoice(VoiceId id)
{
    const auto found = std::find_if(m_voices.begin(), m_voices.end(),
                                    [&](const Voice& candidate) { return candidate.id == id; });
    return found == m_voices.end() ? nullptr : &*found;
}

const AudioEngine::Voice* AudioEngine::FindVoice(VoiceId id) const
{
    const auto found = std::find_if(m_voices.begin(), m_voices.end(),
                                    [&](const Voice& candidate) { return candidate.id == id; });
    return found == m_voices.end() ? nullptr : &*found;
}

void AudioEngine::Mix(float* out, int frames)
{
    if (out == nullptr || frames <= 0)
    {
        return;
    }
    std::lock_guard lock(m_mutex);
    MixLocked(out, frames);
}

void AudioEngine::MixLocked(float* out, int frames)
{
    std::fill(out, out + static_cast<size_t>(frames) * 2, 0.0f);

    for (size_t index = 0; index < m_voices.size();)
    {
        Voice& voice = m_voices[index];
        // Either a finished buffer or one that is still arriving. The difference is only where the
        // samples come from and what running out of them means: a sound that ends is over, a stream
        // that runs dry is waiting.
        Stream* stream = voice.stream != kInvalidStream ? FindStream(voice.stream) : nullptr;
        const SoundData* sound = stream == nullptr ? &m_sounds[voice.sound] : nullptr;
        if (stream == nullptr && sound == nullptr)
        {
            m_voices.erase(m_voices.begin() + static_cast<ptrdiff_t>(index));
            continue;
        }
        const int sourceRate = stream != nullptr ? stream->sampleRate : sound->sampleRate;
        const double rate = static_cast<double>(sourceRate) /
                            static_cast<double>(m_settings.sampleRate) * voice.pitch;

        // Where this voice sits, as two channel gains.
        //
        // Volume falls off between the near and far distances rather than by an inverse square.
        // Inverse square is what sound actually does and it is the wrong thing here: it is either
        // deafening at two metres or inaudible at twenty, and a game needs the same footstep to be
        // audible across a room and not overwhelming beside you. Two distances and a straight line
        // between them is a thing a person can tune by walking away from a sound.
        float left = voice.gain * m_masterGain;
        float right = left;
        if (voice.positioned)
        {
            const glm::vec3 toSource = voice.position - m_listenerPosition;
            const float distance = glm::length(toSource);
            const float falloff =
                distance <= voice.nearDistance
                    ? 1.0f
                    : std::max(1.0f - (distance - voice.nearDistance) /
                                          (voice.farDistance - voice.nearDistance),
                               0.0f);
            // Which ear. The sideways part of the direction to the source, with the pan pulled in
            // as the source gets close: something a hand's width away is not in one ear only, it is
            // all around you, and panning it hard is the most obvious way a mix sounds wrong.
            const float sideways =
                distance > 1e-4f ? glm::dot(toSource / distance, m_listenerRight) : 0.0f;
            const float closeness = std::min(distance / std::max(voice.nearDistance, 0.01f), 1.0f);
            const float pan = sideways * closeness * 0.85f;
            // Constant power, so a sound crossing in front of you does not dip in the middle: the
            // two channels together hold the same energy at every angle, which is what the ear
            // hears rather than the amplitude in either one.
            //
            // And no louder than one at the extremes. Scaling this up so that a hard-panned sound
            // reaches full amplitude in its own channel is the obvious mistake and makes every
            // sound beside the listener clip on its own, before anything else is mixed in.
            const float angle = (pan + 1.0f) * 0.25f * 3.14159265f;
            left *= std::cos(angle) * falloff;
            right *= std::sin(angle) * falloff;
        }
        if (voice.stopping)
        {
            left = 0.0f;
            right = 0.0f;
        }

        // Started where it is meant to be rather than swept up from silence, or every sound would
        // begin with a fade nobody asked for.
        if (voice.mixedLeft < 0.0f)
        {
            voice.mixedLeft = left;
            voice.mixedRight = right;
        }

        bool finished = false;
        const double length =
            stream != nullptr ? 0.0 : static_cast<double>(sound->samples.size());
        for (int frame = 0; frame < frames; ++frame)
        {
            float value = 0.0f;
            if (stream != nullptr)
            {
                // The cursor counts samples since the stream opened, not samples into the buffer,
                // because the buffer keeps having its front thrown away as it is consumed.
                const uint64_t arrived = stream->consumed + stream->pending.size();
                const double position = voice.cursor - static_cast<double>(stream->consumed);
                if (voice.cursor + 1.0 >= static_cast<double>(arrived))
                {
                    // Nothing has arrived yet for this frame. Silence, and the cursor stays where
                    // it is: a hole in the network is a gap in the sound and not a reason to start
                    // reading the next words early. A closed stream that has run dry really is over.
                    if (!stream->open)
                    {
                        finished = true;
                        break;
                    }
                    voice.mixedLeft += std::clamp(left - voice.mixedLeft, -kGainStep, kGainStep);
                    voice.mixedRight += std::clamp(right - voice.mixedRight, -kGainStep, kGainStep);
                    continue;
                }
                const auto whole = static_cast<size_t>(position);
                const float fraction = static_cast<float>(position - static_cast<double>(whole));
                const float a = stream->pending[whole];
                const float b = whole + 1 < stream->pending.size() ? stream->pending[whole + 1] : a;
                value = a + (b - a) * fraction;
            }
            else
            {
                if (voice.cursor >= length)
                {
                    if (!voice.loop)
                    {
                        finished = true;
                        break;
                    }
                    voice.cursor = std::fmod(voice.cursor, length);
                }

                // Linear interpolation between the two nearest samples. At a pitch of one this is
                // exact and costs nothing; away from one it is what stops a resampled sound
                // sounding gritty.
                const auto whole = static_cast<size_t>(voice.cursor);
                const float fraction = static_cast<float>(voice.cursor - static_cast<double>(whole));
                const float a = sound->samples[whole];
                const float b = whole + 1 < sound->samples.size()
                                    ? sound->samples[whole + 1]
                                    : (voice.loop ? sound->samples[0] : 0.0f);
                value = a + (b - a) * fraction;
            }

            voice.mixedLeft += std::clamp(left - voice.mixedLeft, -kGainStep, kGainStep);
            voice.mixedRight += std::clamp(right - voice.mixedRight, -kGainStep, kGainStep);

            out[static_cast<size_t>(frame) * 2] += value * voice.mixedLeft;
            out[static_cast<size_t>(frame) * 2 + 1] += value * voice.mixedRight;
            voice.cursor += rate;
        }

        // Everything this voice has read can go. The front of the queue is thrown away rather than
        // kept, because a conversation that ran for an hour would otherwise be an hour of audio
        // held in memory. One voice reads one stream, which is what makes this safe to do here.
        if (stream != nullptr && voice.cursor > static_cast<double>(stream->consumed))
        {
            const auto used = static_cast<size_t>(voice.cursor - static_cast<double>(stream->consumed));
            const size_t drop = std::min(used, stream->pending.size());
            if (drop > 0)
            {
                stream->pending.erase(stream->pending.begin(),
                                      stream->pending.begin() + static_cast<ptrdiff_t>(drop));
                stream->consumed += drop;
            }
        }

        // A stopping voice goes when it has faded out, which the gain step above guarantees within
        // a few hundred frames.
        if (voice.stopping && voice.mixedLeft <= 0.0f && voice.mixedRight <= 0.0f)
        {
            finished = true;
        }

        if (finished)
        {
            m_voices.erase(m_voices.begin() + static_cast<ptrdiff_t>(index));
        }
        else
        {
            ++index;
        }
    }

    // And nothing leaves here outside what a speaker can take.
    //
    // Squashed rather than chopped. Four gunshots at once really do add up past one, and what a
    // sound card does with a sample past one is not a louder gunshot -- but clamping is not much
    // better: it flattens the tops of the waveform into straight lines, which is a square wave, and
    // a square wave is harmonics that were never in the sound. A whole mix run through that comes
    // back sounding coarse and grainy, and it does not recover when the loud thing stops, because
    // every sample above the line is still being flattened. It was reported as everything sounding
    // bit crushed after somebody used the voice chat.
    //
    // Below the knee nothing is touched at all, so quiet material passes through exactly as it was.
    // Above it the curve bends over and approaches one without reaching it, so a loud moment loses
    // some of its peak instead of gaining a buzz.
    constexpr float kKnee = 0.70f;
    for (int i = 0; i < frames * 2; ++i)
    {
        const float sample = out[i];
        const float magnitude = std::abs(sample);
        if (magnitude <= kKnee)
        {
            continue;
        }
        const float over = (magnitude - kKnee) / (1.0f - kKnee);
        const float squashed = kKnee + (1.0f - kKnee) * std::tanh(over);
        out[i] = std::copysign(squashed, sample);
        ++m_stats.clipped;
    }
}

} // namespace pred
