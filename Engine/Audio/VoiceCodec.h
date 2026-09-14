#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct OpusEncoder;
struct OpusDecoder;

namespace pred
{

// Voice, small enough to send.
//
// A twenty millisecond frame at 48 kHz is 960 samples, which as floats is 3840 bytes: three times
// what fits in a datagram before fragmentation, for a twentieth of a second of one person talking.
// Speech has to be compressed before it can be sent at all, so this is not an optimisation.
//
// Opus, because voice codecs are not a thing to write yourself. It is the one every voice
// application uses, it is designed for exactly this — short frames, packets that go missing, one
// person speaking — and at twenty-four kilobits a second it turns that 3840 bytes into about sixty.
//
// The decoder is deliberately told when a frame was lost rather than being given silence. Opus can
// fill a gap from what came before it far better than silence does: a dropped packet becomes a
// smudge rather than a click, and a click in the middle of a word is the most noticeable thing a
// voice connection can do.
class VoiceCodec
{
public:
    struct Settings
    {
        int sampleRate = 48000;
        // Matches VoiceCapture. Opus only accepts certain frame lengths and twenty milliseconds is
        // the usual one: short enough not to add noticeable delay, long enough to compress well.
        float frameSeconds = 0.020f;
        // How much bandwidth one speaker may use. Twenty-four kilobits is comfortably transparent
        // for speech; the game's own traffic is about twenty-one, so one person talking roughly
        // doubles what a connection carries.
        int bitsPerSecond = 24000;
    };

    VoiceCodec() = default;
    ~VoiceCodec();
    VoiceCodec(const VoiceCodec&) = delete;
    VoiceCodec& operator=(const VoiceCodec&) = delete;

    bool Init(const Settings& settings);
    void Shutdown();
    bool Ready() const { return m_encoder != nullptr && m_decoder != nullptr; }
    const std::string& Message() const { return m_message; }

    size_t FrameSamples() const;

    // One frame in, one packet out. False when the frame is the wrong length or the codec refused
    // it, which is a bug rather than a thing that happens.
    bool Encode(const float* samples, size_t count, std::vector<uint8_t>& out);
    // And back. Pass an empty packet to say a frame was lost; the decoder then invents something
    // plausible from what it has already heard, which is what keeps a dropped packet from clicking.
    bool Decode(const uint8_t* packet, size_t bytes, std::vector<float>& out);

private:
    Settings m_settings;
    OpusEncoder* m_encoder = nullptr;
    OpusDecoder* m_decoder = nullptr;
    std::string m_message;
};

} // namespace pred
