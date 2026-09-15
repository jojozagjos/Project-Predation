#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>
#include <vector>

struct SDL_AudioStream;

namespace pred
{

// A microphone, read in frames.
//
// The counterpart of the mixer's streams: that plays sound that is still arriving, and this is
// where sound that is still being made comes from. Neither knows about the other or about the
// network, which is what lets both be tested without a sound card or a second machine.
//
// Mono at a fixed rate, because everything downstream of it wants exactly that: a voice is one
// source in one place and the mixer positions it, so a stereo microphone has already decided
// something that is not its business.
//
// The device is opened when somebody starts talking and closed when they stop, rather than held
// open for the session. That costs the first few tens of milliseconds of the first word, and it
// buys the operating system's microphone indicator meaning what it says: a game that holds the
// microphone open all match and promises it is not listening is asking to be taken on trust, and
// this way there is nothing to take on trust.
class VoiceCapture
{
public:
    struct Settings
    {
        int sampleRate = 48000;
        // How long a frame is. Twenty milliseconds is what voice codecs work in, so choosing it
        // here means nothing has to be re-cut later.
        float frameSeconds = 0.020f;
        // How much unread audio may pile up before the oldest is dropped. A game that stalls must
        // not then transmit the backlog: what arrives late in a conversation is worse than useless,
        // because everybody else has moved on.
        float maxQueuedSeconds = 0.40f;
        // Which microphone, or zero for whatever the system calls the default.
        //
        // Worth choosing, because "the default" on a machine with a webcam, a headset and a monitor
        // with a built-in array is a coin toss, and the one it lands on is usually the one pointing
        // at the fans. An id rather than a name: names are not unique and change when a device is
        // replugged, and a stale one should fall back to the default rather than fail.
        uint32_t deviceId = 0;
    };

    // The recording devices this machine has, for a settings screen to offer.
    struct Device
    {
        uint32_t id = 0;
        std::string name;
    };
    static std::vector<Device> Devices();

    VoiceCapture() = default;
    ~VoiceCapture();
    VoiceCapture(const VoiceCapture&) = delete;
    VoiceCapture& operator=(const VoiceCapture&) = delete;

    // Applies the settings without touching a device. Start does this first; a test does only this,
    // so that framing and backlog can be checked deterministically rather than against whatever a
    // build machine has plugged into it.
    void Configure(const Settings& settings);
    // Opens the microphone. False means there is not one, or it is refused, which is not an error:
    // the game plays without a voice and says so once.
    bool Start(const Settings& settings);
    void Stop();
    bool Running() const { return m_stream != nullptr; }
    const std::string& Message() const { return m_message; }

    int SampleRate() const { return m_settings.sampleRate; }
    size_t FrameSamples() const;

    // The next whole frame, or false when one has not been recorded yet. Frames rather than
    // whatever happens to be there, so what goes on the wire is the same size every time.
    bool ReadFrame(std::vector<float>& out);

    // How loud the last frame read was, 0 to 1, as the peak sample. For a level meter, and for
    // deciding whether anybody is actually speaking.
    float LastLevel() const;

    // Whether an open microphone should be transmitting, given how loud the last frame was.
    //
    // Hysteresis, and a hold after the last loud frame, because neither on its own is enough. One
    // threshold alone chatters on and off through every pause between two words, and a gap in the
    // middle of a sentence is more noticeable than a little room tone at the end of one. So it opens
    // at the threshold, stays open until well below it, and then stays open a moment longer.
    //
    // Separate from the capture itself so it can be tested without a microphone: the whole of it is
    // a decision about a number.
    bool ShouldTransmit(float level, float threshold, float dt);
    // What that decision is now, without advancing it.
    bool Transmitting() const { return m_transmitting; }

    // Called by SDL from its own thread. Public because SDL needs a plain function to reach it.
    void OnRecorded(const float* samples, size_t count);

private:
    Settings m_settings;
    SDL_AudioStream* m_stream = nullptr;
    std::string m_message;

    mutable std::mutex m_mutex;
    std::vector<float> m_pending;
    float m_lastLevel = 0.0f;
    // Open-mic state: whether it is currently open, and how long since it last heard anything above
    // the threshold.
    bool m_transmitting = false;
    float m_quietFor = 0.0f;
};

} // namespace pred
