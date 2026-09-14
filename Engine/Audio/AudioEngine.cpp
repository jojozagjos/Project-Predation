#include "Engine/Audio/AudioEngine.h"

#include "Engine/Core/Log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

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
    if (desc.sound >= m_sounds.size() || m_sounds[desc.sound].samples.empty())
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
        const SoundData& sound = m_sounds[voice.sound];
        const double rate = static_cast<double>(sound.sampleRate) /
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
            // Constant power, so a sound crossing in front of you does not dip in the middle.
            const float angle = (pan + 1.0f) * 0.25f * 3.14159265f;
            left *= std::cos(angle) * 1.41421356f * falloff;
            right *= std::sin(angle) * 1.41421356f * falloff;
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
        const double length = static_cast<double>(sound.samples.size());
        for (int frame = 0; frame < frames; ++frame)
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

            // Linear interpolation between the two nearest samples. At a pitch of one this is exact
            // and costs nothing; away from one it is what stops a resampled sound sounding gritty.
            const auto whole = static_cast<size_t>(voice.cursor);
            const float fraction = static_cast<float>(voice.cursor - static_cast<double>(whole));
            const float a = sound.samples[whole];
            const float b = whole + 1 < sound.samples.size()
                                ? sound.samples[whole + 1]
                                : (voice.loop ? sound.samples[0] : 0.0f);
            const float value = a + (b - a) * fraction;

            voice.mixedLeft += std::clamp(left - voice.mixedLeft, -kGainStep, kGainStep);
            voice.mixedRight += std::clamp(right - voice.mixedRight, -kGainStep, kGainStep);

            out[static_cast<size_t>(frame) * 2] += value * voice.mixedLeft;
            out[static_cast<size_t>(frame) * 2 + 1] += value * voice.mixedRight;
            voice.cursor += rate;
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

    // And nothing leaves here outside what a speaker can take. Four gunshots at once really do add
    // up past one, and what a sound card does with a sample past one is not a louder gunshot, it is
    // a tearing noise.
    for (int i = 0; i < frames * 2; ++i)
    {
        if (out[i] > 1.0f || out[i] < -1.0f)
        {
            out[i] = std::clamp(out[i], -1.0f, 1.0f);
            ++m_stats.clipped;
        }
    }
}

} // namespace pred
