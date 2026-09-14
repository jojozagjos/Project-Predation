#include "Engine/Audio/Sound.h"

#include "Engine/Core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pred
{
namespace
{

// A small, fast, reproducible noise source.
//
// Not std::mt19937, and not because of speed: a recipe has to give the same samples on every
// machine, and the standard engines are specified well enough for that but the distributions on top
// of them are not. This is nine lines and says exactly what it does.
class Noise
{
public:
    explicit Noise(uint32_t seed) : m_state(seed == 0 ? 1u : seed) {}

    float Next()
    {
        m_state ^= m_state << 13;
        m_state ^= m_state >> 17;
        m_state ^= m_state << 5;
        // The top 24 bits, scaled to [-1, 1]. Taking the low bits of an xorshift gives a much worse
        // sequence than taking the high ones.
        return static_cast<float>(m_state >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }

private:
    uint32_t m_state;
};

float ReadFloat(const nlohmann::json& object, const char* key, float fallback)
{
    const auto found = object.find(key);
    return found != object.end() && found->is_number() ? found->get<float>() : fallback;
}

} // namespace

SoundData Synthesise(const SoundRecipe& recipe, int sampleRate)
{
    SoundData data;
    data.sampleRate = std::max(sampleRate, 1);

    const float seconds = std::clamp(recipe.seconds, 0.005f, 10.0f);
    const int frames = std::max(1, static_cast<int>(seconds * static_cast<float>(data.sampleRate)));
    data.samples.resize(static_cast<size_t>(frames));

    Noise noise(recipe.seed);
    // One-pole low pass, which is the cheapest filter that does anything a person can hear. The
    // coefficient is the recipe's own number: at one the filter is a wire.
    const float cutoff = std::clamp(recipe.lowpass, 0.001f, 1.0f);
    float filtered = 0.0f;
    float phase = 0.0f;

    const float attack = std::max(recipe.attack, 1e-4f);
    const float decay = std::max(recipe.decay, 1e-3f);
    const float step = 1.0f / static_cast<float>(data.sampleRate);

    for (int i = 0; i < frames; ++i)
    {
        const float t = static_cast<float>(i) * step;

        // Up quickly, then down on an exponential, which is what almost everything that is struck
        // or fired does. The tail is cut to zero at the end of the buffer so a sound never stops
        // part way through a swing and clicks.
        const float rise = std::min(t / attack, 1.0f);
        const float fall = std::exp(-t / decay);
        const float fade = std::min((seconds - t) / 0.01f, 1.0f);
        const float envelope = rise * fall * std::max(fade, 0.0f);

        // The note, sliding as it goes. A door falling an octave over a second and a pickup rising
        // a fifth over a tenth of one are the same two numbers with different signs.
        const float hz = recipe.toneHz * std::exp2(-recipe.toneDrop * (t / seconds));
        phase += hz * step;
        if (phase > 1.0f)
        {
            phase -= std::floor(phase);
        }
        const float note = std::sin(phase * 6.283185307f);

        const float hiss = noise.Next();
        filtered += (hiss - filtered) * cutoff;

        // And the click on the front. Two milliseconds of it, which is under the length of anything
        // anybody hears as a pitch, so it reads as impact rather than as a note.
        const float biteEnvelope = t < 0.002f ? (1.0f - t / 0.002f) : 0.0f;

        const float value = filtered * recipe.noise + note * recipe.tone +
                            recipe.bite * biteEnvelope * (hiss * 0.5f + 0.5f);
        data.samples[static_cast<size_t>(i)] = std::clamp(value * envelope, -1.0f, 1.0f);
    }

    return data;
}

namespace
{

// RIFF stores everything little-endian. Reading byte by byte rather than memcpy-ing a struct means
// this behaves the same on a big-endian machine, and costs nothing on the ones we have.
uint16_t ReadU16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t ReadU32(const unsigned char* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

constexpr uint16_t kFormatPcm = 1;
constexpr uint16_t kFormatFloat = 3;
constexpr uint16_t kFormatExtensible = 0xFFFE;

} // namespace

bool LoadWav(const void* bytes, size_t byteCount, SoundData& out, std::string& error)
{
    const auto* data = static_cast<const unsigned char*>(bytes);
    if (data == nullptr || byteCount < 12)
    {
        error = "not long enough to be a wav";
        return false;
    }
    if (std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0)
    {
        error = "no RIFF/WAVE header";
        return false;
    }

    uint16_t format = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint32_t rate = 0;
    const unsigned char* samples = nullptr;
    size_t sampleBytes = 0;

    // Walk the chunks. Each is a four character id, a length, and that many bytes, padded to even.
    size_t at = 12;
    while (at + 8 <= byteCount)
    {
        const unsigned char* id = data + at;
        uint32_t length = ReadU32(data + at + 4);
        const size_t body = at + 8;
        if (length > byteCount - body)
        {
            // A truncated final chunk. Take what is there rather than refusing the whole file: a
            // recording that was cut short is still worth hearing most of.
            length = static_cast<uint32_t>(byteCount - body);
        }
        if (std::memcmp(id, "fmt ", 4) == 0 && length >= 16)
        {
            format = ReadU16(data + body + 0);
            channels = ReadU16(data + body + 2);
            rate = ReadU32(data + body + 4);
            bits = ReadU16(data + body + 14);
            // WAVE_FORMAT_EXTENSIBLE hides the real format in a sub-chunk at the end. Only the
            // first two bytes of that GUID matter and they hold the same numbers as `format`.
            if (format == kFormatExtensible && length >= 26)
            {
                format = ReadU16(data + body + 24);
            }
        }
        else if (std::memcmp(id, "data", 4) == 0)
        {
            samples = data + body;
            sampleBytes = length;
        }
        at = body + length + (length & 1u);
    }

    if (channels == 0 || rate == 0 || bits == 0)
    {
        error = "no format chunk";
        return false;
    }
    if (samples == nullptr)
    {
        error = "no data chunk";
        return false;
    }
    const bool supported = (format == kFormatPcm && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
                           (format == kFormatFloat && bits == 32);
    if (!supported)
    {
        error = "format " + std::to_string(format) + " at " + std::to_string(bits) +
                " bits is not one this reads";
        return false;
    }

    const size_t bytesPerSample = bits / 8u;
    const size_t stride = bytesPerSample * channels;
    const size_t frames = stride > 0 ? sampleBytes / stride : 0;

    out.sampleRate = static_cast<int>(rate);
    out.samples.assign(frames, 0.0f);
    for (size_t frame = 0; frame < frames; ++frame)
    {
        float total = 0.0f;
        for (uint16_t channel = 0; channel < channels; ++channel)
        {
            const unsigned char* p = samples + frame * stride + channel * bytesPerSample;
            float value = 0.0f;
            if (format == kFormatFloat)
            {
                uint32_t raw = ReadU32(p);
                std::memcpy(&value, &raw, sizeof(value));
            }
            else if (bits == 8)
            {
                // Eight bit wav is the odd one out: unsigned, centred on 128.
                value = (static_cast<float>(p[0]) - 128.0f) / 128.0f;
            }
            else if (bits == 16)
            {
                value = static_cast<float>(static_cast<int16_t>(ReadU16(p))) / 32768.0f;
            }
            else if (bits == 24)
            {
                int32_t raw = static_cast<int32_t>(p[0]) | (static_cast<int32_t>(p[1]) << 8) |
                              (static_cast<int32_t>(p[2]) << 16);
                if (raw & 0x00800000) // sign extend
                {
                    raw |= static_cast<int32_t>(0xFF000000u);
                }
                value = static_cast<float>(raw) / 8388608.0f;
            }
            else
            {
                value = static_cast<float>(static_cast<int32_t>(ReadU32(p))) / 2147483648.0f;
            }
            total += value;
        }
        out.samples[frame] = total / static_cast<float>(channels);
    }
    return true;
}

size_t TrimTrailingSilence(SoundData& data, float threshold, float fadeSeconds)
{
    const size_t original = data.samples.size();
    size_t end = original;
    while (end > 0 && std::abs(data.samples[end - 1]) < threshold)
    {
        --end;
    }
    if (end == original || end == 0)
    {
        return 0;
    }
    data.samples.resize(end);

    // Cutting at the first quiet sample still cuts somewhere, and a waveform that stops mid-slope
    // is a click. A few milliseconds of fade is inaudible and removes the question.
    const size_t fade = std::min(end, static_cast<size_t>(std::max(0.0f, fadeSeconds) *
                                                          static_cast<float>(data.sampleRate)));
    for (size_t i = 0; i < fade; ++i)
    {
        const float t = static_cast<float>(i + 1) / static_cast<float>(fade + 1);
        data.samples[end - 1 - i] *= t;
    }
    return original - end;
}

std::vector<SoundLibraryEntry> LoadSoundRecipes(const std::string& jsonText)
{
    std::vector<SoundLibraryEntry> entries;

    nlohmann::json root = nlohmann::json::parse(jsonText, nullptr, false);
    if (root.is_discarded() || !root.is_object())
    {
        PRED_LOG_WARN(Engine, "Sound recipes are not a JSON object; no sounds loaded");
        return entries;
    }

    for (const auto& [name, value] : root.items())
    {
        // A key beginning with an underscore is a note to whoever is editing the file. JSON has no
        // comments, everybody works round that the same way, and warning about it every startup
        // teaches people to ignore the warnings.
        if (!name.empty() && name.front() == '_')
        {
            continue;
        }
        if (!value.is_object())
        {
            PRED_LOG_WARN(Engine, "Sound '{}' is not an object; skipped", name);
            continue;
        }
        SoundLibraryEntry entry;
        entry.name = name;
        SoundRecipe& recipe = entry.recipe;
        recipe.seconds = ReadFloat(value, "seconds", recipe.seconds);
        recipe.attack = ReadFloat(value, "attack", recipe.attack);
        recipe.decay = ReadFloat(value, "decay", recipe.decay);
        recipe.noise = ReadFloat(value, "noise", recipe.noise);
        recipe.tone = ReadFloat(value, "tone", recipe.tone);
        recipe.toneHz = ReadFloat(value, "tone_hz", recipe.toneHz);
        recipe.toneDrop = ReadFloat(value, "tone_drop", recipe.toneDrop);
        recipe.lowpass = ReadFloat(value, "lowpass", recipe.lowpass);
        recipe.bite = ReadFloat(value, "bite", recipe.bite);
        const auto seed = value.find("seed");
        if (seed != value.end() && seed->is_number_unsigned())
        {
            recipe.seed = seed->get<uint32_t>();
        }
        entries.push_back(std::move(entry));
    }

    return entries;
}

} // namespace pred
