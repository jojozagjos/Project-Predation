#pragma once

#include "Engine/Audio/Sound.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct SDL_AudioStream;

namespace pred
{

using SoundId = uint16_t;
using VoiceId = uint32_t;
using StreamId = uint16_t;
inline constexpr SoundId kInvalidSound = 0xFFFF;
inline constexpr VoiceId kInvalidVoice = 0;
inline constexpr StreamId kInvalidStream = 0xFFFF;

// Everything this game makes a noise with.
//
// The shape of it is one decision, and the rest follows from it: the mixer is a plain function of a
// list of voices and a listener, so it can be run without a sound card and asserted on. Sound is
// the one subsystem where a bug is hard to see and easy to hear, and "play it and listen" is not a
// test anybody can run twice the same way. Attenuation, panning and when a voice ends are all
// arithmetic, and arithmetic can be checked.
//
// The device is therefore the outer layer rather than the middle of it. With no sound card, or with
// audio switched off, everything above still runs and Mix is simply never called: a headless smoke
// run makes no noise and takes no special path to do it.
class AudioEngine
{
public:
    struct Settings
    {
        int sampleRate = 48000;
        // How many voices may sound at once. Past this the quietest is taken rather than the oldest:
        // a footstep across the map losing to a gunshot beside you is the right way round.
        int maxVoices = 48;
    };

    // How a sound is placed in the world. The defaults describe something a person would make.
    struct PlayDesc
    {
        SoundId sound = kInvalidSound;
        // A stream instead of a sound, for audio that does not exist yet when it starts playing.
        //
        // Every other sound in this game is a buffer that is complete before anybody hears it. A
        // voice coming down a wire is not: it arrives while it is being played, in fragments, with
        // gaps. When this is set, `sound` is ignored and the voice reads whatever has arrived.
        StreamId stream = kInvalidStream;
        glm::vec3 position{0.0f};
        // False plays it in both ears at full volume, wherever the listener is. That is right for
        // your own weapon and for anything on the interface, and wrong for everything else.
        bool positioned = true;
        float gain = 1.0f;
        float pitch = 1.0f;
        bool loop = false;
        // Full volume within this, then falling away, and silent past the far one. Two numbers
        // rather than a curve, because a curve is a thing nobody can tune by ear.
        float nearDistance = 3.0f;
        float farDistance = 55.0f;
    };

    struct Stats
    {
        int voices = 0;       // sounding now
        uint32_t started = 0; // since the engine came up
        uint32_t stolen = 0;  // cut short because everything was busy
        uint32_t clipped = 0; // mixed frames that had to be limited
    };

    AudioEngine() = default;
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Opens the device. Returns false when there is not one, which is not an error: the game runs
    // silently and everything else about this class still works.
    bool Init(const Settings& settings);
    void Shutdown();
    bool HasDevice() const { return m_stream != nullptr; }
    int SampleRate() const { return m_settings.sampleRate; }

    // --- Sounds ---------------------------------------------------------------------------------
    SoundId Add(const std::string& name, SoundData data);
    SoundId Find(const std::string& name) const;
    // The name a sound was added under, for logs. Walks the table, so not for anything per sample.
    std::string NameOf(SoundId id) const;
    // Builds every recipe in the text and adds it under its own name. Returns how many.
    int AddRecipes(const std::string& jsonText);

    // --- Streams --------------------------------------------------------------------------------
    //
    // Sound that is still arriving. One per person talking: the packets turn up late, out of order
    // and with holes in them, and the mixer has to keep playing through all of that rather than
    // deciding the voice has ended every time the network hiccups.
    //
    // A stream that runs dry plays silence and stays open. That is the whole difference between
    // this and a sound: a sound that reaches its end is finished, and a stream that reaches its end
    // is merely waiting.
    StreamId OpenStream(int sampleRate);
    // Appends samples. Mono, in the stream's own rate; the mixer resamples like anything else.
    // Beyond `maxQueuedSeconds` the oldest are dropped, because a listener who falls behind must
    // not accumulate a growing delay: in conversation, late audio is worse than missing audio.
    void PushStream(StreamId stream, const float* samples, size_t count);
    // How much is waiting, in samples. Useful for deciding whether to start playing yet.
    size_t StreamQueued(StreamId stream) const;
    void CloseStream(StreamId stream);

    // --- Playing --------------------------------------------------------------------------------
    VoiceId Play(const PlayDesc& desc);
    // Convenience for the common case: a one-shot somewhere in the world.
    VoiceId PlayAt(SoundId sound, const glm::vec3& position, float gain = 1.0f, float pitch = 1.0f);
    void Stop(VoiceId voice);
    void StopAll();
    void SetVoicePosition(VoiceId voice, const glm::vec3& position);
    void SetVoiceGain(VoiceId voice, float gain);
    bool IsPlaying(VoiceId voice) const;

    void SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up);
    void SetMasterGain(float gain);
    float MasterGain() const;

    // Mixes `frames` of interleaved stereo into `out`, which is overwritten rather than added to.
    // Called by the audio thread, and directly by tests, which is the point.
    void Mix(float* out, int frames);

    Stats GetStats() const;

private:
    // Sound that is still arriving. See OpenStream.
    struct Stream
    {
        StreamId id = kInvalidStream;
        int sampleRate = 48000;
        std::vector<float> pending;
        // Kept so a voice reading this stream can hold its place between buffers without the whole
        // queue having to survive: the cursor is in samples since the stream opened, and this is
        // how many have already been thrown away off the front.
        uint64_t consumed = 0;
        bool open = true;
    };

    struct Voice
    {
        VoiceId id = kInvalidVoice;
        SoundId sound = kInvalidSound;
        StreamId stream = kInvalidStream;
        double cursor = 0.0; // in source samples, fractional because pitch is not always one
        glm::vec3 position{0.0f};
        bool positioned = true;
        bool loop = false;
        float gain = 1.0f;
        float pitch = 1.0f;
        float nearDistance = 3.0f;
        float farDistance = 55.0f;
        bool stopping = false;
        // Where the last mix left this voice's two channel gains, so a voice that moves or a
        // listener that turns does not step from one gain to another between buffers and click.
        float mixedLeft = -1.0f;
        float mixedRight = -1.0f;
    };

    void MixLocked(float* out, int frames);
    Voice* FindVoice(VoiceId id);
    const Voice* FindVoice(VoiceId id) const;

    Stream* FindStream(StreamId id);
    const Stream* FindStream(StreamId id) const;

    Settings m_settings;
    SDL_AudioStream* m_stream = nullptr;
    // How much a stream may hold before the oldest is thrown away. Half a second is long enough to
    // ride out a normal network wobble and short enough that a listener never ends up a second
    // behind a conversation without noticing.
    static constexpr float kMaxQueuedSeconds = 0.5f;
    std::vector<Stream> m_streams;
    StreamId m_nextStream = 0;

    // One lock over the voices and the listener. The game thread starts and moves voices at the
    // frame rate and the audio thread reads them a few hundred times a second, so the contention is
    // nothing and a lock-free ring buffer here would be machinery bought for no reason.
    mutable std::mutex m_mutex;
    std::vector<Voice> m_voices;
    std::vector<SoundData> m_sounds;
    std::unordered_map<std::string, SoundId> m_byName;

    glm::vec3 m_listenerPosition{0.0f};
    glm::vec3 m_listenerForward{0.0f, 0.0f, -1.0f};
    glm::vec3 m_listenerRight{1.0f, 0.0f, 0.0f};
    float m_masterGain = 1.0f;
    VoiceId m_nextVoice = 1;
    Stats m_stats;
};

} // namespace pred
