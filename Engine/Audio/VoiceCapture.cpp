#include "Engine/Audio/VoiceCapture.h"

#include "Engine/Core/Log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace pred
{
namespace
{

void SDLCALL Recorded(void* userdata, SDL_AudioStream* stream, int additional, int /*total*/)
{
    auto* capture = static_cast<VoiceCapture*>(userdata);
    if (capture == nullptr || additional <= 0)
    {
        return;
    }
    // Everything the device has, in blocks, so a driver handing over a large buffer does not put a
    // large buffer on the stack.
    constexpr int kBlockSamples = 1024;
    float block[kBlockSamples];
    int left = additional;
    while (left > 0)
    {
        const int wanted = std::min(left, static_cast<int>(sizeof(block)));
        const int got = SDL_GetAudioStreamData(stream, block, wanted);
        if (got <= 0)
        {
            break;
        }
        capture->OnRecorded(block, static_cast<size_t>(got) / sizeof(float));
        left -= got;
    }
}

} // namespace

VoiceCapture::~VoiceCapture()
{
    Stop();
}

void VoiceCapture::Configure(const Settings& settings)
{
    m_settings = settings;
    m_settings.sampleRate = std::clamp(settings.sampleRate, 8000, 48000);
    m_settings.frameSeconds = std::clamp(settings.frameSeconds, 0.005f, 0.100f);
    m_settings.maxQueuedSeconds = std::clamp(settings.maxQueuedSeconds, 0.05f, 5.0f);
}

bool VoiceCapture::Start(const Settings& settings)
{
    Stop();
    Configure(settings);
    m_message.clear();

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        m_message = SDL_GetError();
        PRED_LOG_WARN(Engine, "No audio subsystem for the microphone: {}", m_message);
        return false;
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = 1;
    spec.freq = m_settings.sampleRate;

    m_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, Recorded, this);
    if (m_stream == nullptr)
    {
        m_message = SDL_GetError();
        // No microphone, none permitted, or none chosen. All three are ordinary and none of them
        // stops the game, so this is said once and then left alone.
        PRED_LOG_INFO(Engine, "No microphone: {}", m_message);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }

    SDL_ResumeAudioStreamDevice(m_stream);
    PRED_LOG_INFO(Engine, "Microphone open: {} Hz mono, {:.0f} ms frames", m_settings.sampleRate,
                  m_settings.frameSeconds * 1000.0f);
    return true;
}

void VoiceCapture::Stop()
{
    if (m_stream != nullptr)
    {
        // Destroying the stream stops the callback, so nothing is writing by the time the queue is
        // cleared below.
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending.clear();
    m_lastLevel = 0.0f;
}

size_t VoiceCapture::FrameSamples() const
{
    return static_cast<size_t>(m_settings.frameSeconds * static_cast<float>(m_settings.sampleRate));
}

void VoiceCapture::OnRecorded(const float* samples, size_t count)
{
    if (samples == nullptr || count == 0)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending.insert(m_pending.end(), samples, samples + count);

    // Past the cap the oldest go, for the same reason the mixer's streams drop theirs: a game that
    // stalled must not then send the backlog. Everybody else has already moved on, and audio that
    // arrives late in a conversation is worse than audio that never arrives.
    const auto cap =
        static_cast<size_t>(m_settings.maxQueuedSeconds * static_cast<float>(m_settings.sampleRate));
    if (m_pending.size() > cap)
    {
        m_pending.erase(m_pending.begin(),
                        m_pending.begin() + static_cast<ptrdiff_t>(m_pending.size() - cap));
    }
}

bool VoiceCapture::ReadFrame(std::vector<float>& out)
{
    const size_t frame = FrameSamples();
    if (frame == 0)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pending.size() < frame)
    {
        return false;
    }
    out.assign(m_pending.begin(), m_pending.begin() + static_cast<ptrdiff_t>(frame));
    m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<ptrdiff_t>(frame));

    float peak = 0.0f;
    for (const float sample : out)
    {
        peak = std::max(peak, std::abs(sample));
    }
    m_lastLevel = std::min(peak, 1.0f);
    return true;
}

float VoiceCapture::LastLevel() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastLevel;
}

} // namespace pred
