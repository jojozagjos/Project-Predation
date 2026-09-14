#include "Engine/Audio/VoiceCodec.h"

#include "Engine/Core/Log.h"

#include <opus.h>

#include <algorithm>

namespace pred
{
namespace
{

// The most one encoded frame can be. Opus will not exceed about a quarter of this at speech
// bitrates; the buffer exists so the encoder has somewhere to write without the caller guessing.
constexpr size_t kMaxEncodedBytes = 512;

} // namespace

VoiceCodec::~VoiceCodec()
{
    Shutdown();
}

bool VoiceCodec::Init(const Settings& settings)
{
    Shutdown();
    m_settings = settings;
    m_message.clear();

    int error = OPUS_OK;
    // VOIP rather than AUDIO: it optimises for speech intelligibility over musical fidelity, which
    // is the right trade when the thing being carried is somebody whispering that they can hear it.
    m_encoder = opus_encoder_create(m_settings.sampleRate, 1, OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK || m_encoder == nullptr)
    {
        m_message = opus_strerror(error);
        PRED_LOG_WARN(Engine, "Voice encoder failed: {}", m_message);
        Shutdown();
        return false;
    }
    opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(m_settings.bitsPerSecond));
    // In-band forward error correction: each packet carries a coarse copy of the one before it, so
    // a single lost packet can be reconstructed from the next one rather than guessed at. It costs
    // a little bandwidth and it is the difference between a dropped packet being inaudible and
    // being a hole in a sentence.
    opus_encoder_ctl(m_encoder, OPUS_SET_INBAND_FEC(1));
    // What loss to encode for. Telling it to expect some makes it spend bits on being robust; this
    // is a guess at a typical connection rather than a measurement, and measuring it is a later job.
    opus_encoder_ctl(m_encoder, OPUS_SET_PACKET_LOSS_PERC(10));

    m_decoder = opus_decoder_create(m_settings.sampleRate, 1, &error);
    if (error != OPUS_OK || m_decoder == nullptr)
    {
        m_message = opus_strerror(error);
        PRED_LOG_WARN(Engine, "Voice decoder failed: {}", m_message);
        Shutdown();
        return false;
    }

    PRED_LOG_INFO(Engine, "Voice codec: {} Hz, {:.0f} ms frames, {} kbit/s", m_settings.sampleRate,
                  m_settings.frameSeconds * 1000.0f, m_settings.bitsPerSecond / 1000);
    return true;
}

void VoiceCodec::Shutdown()
{
    if (m_encoder != nullptr)
    {
        opus_encoder_destroy(m_encoder);
        m_encoder = nullptr;
    }
    if (m_decoder != nullptr)
    {
        opus_decoder_destroy(m_decoder);
        m_decoder = nullptr;
    }
}

size_t VoiceCodec::FrameSamples() const
{
    return static_cast<size_t>(m_settings.frameSeconds * static_cast<float>(m_settings.sampleRate));
}

bool VoiceCodec::Encode(const float* samples, size_t count, std::vector<uint8_t>& out)
{
    const size_t frame = FrameSamples();
    if (m_encoder == nullptr || samples == nullptr || count != frame)
    {
        return false;
    }
    out.resize(kMaxEncodedBytes);
    const int written = opus_encode_float(m_encoder, samples, static_cast<int>(frame), out.data(),
                                          static_cast<opus_int32>(out.size()));
    if (written < 0)
    {
        m_message = opus_strerror(written);
        out.clear();
        return false;
    }
    out.resize(static_cast<size_t>(written));
    return true;
}

bool VoiceCodec::Decode(const uint8_t* packet, size_t bytes, std::vector<float>& out)
{
    const size_t frame = FrameSamples();
    if (m_decoder == nullptr || frame == 0)
    {
        return false;
    }
    out.resize(frame);
    // A null packet tells Opus the frame was lost, and it fills the gap from what it has already
    // decoded. Handing it silence instead would be a hole in the middle of a word, which is the
    // most noticeable thing a voice connection can do.
    const bool lost = packet == nullptr || bytes == 0;
    const int decoded = opus_decode_float(m_decoder, lost ? nullptr : packet,
                                          lost ? 0 : static_cast<opus_int32>(bytes), out.data(),
                                          static_cast<int>(frame), lost ? 1 : 0);
    if (decoded < 0)
    {
        m_message = opus_strerror(decoded);
        out.clear();
        return false;
    }
    out.resize(static_cast<size_t>(decoded));
    return true;
}

} // namespace pred
