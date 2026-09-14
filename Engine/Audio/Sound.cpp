#include "Engine/Audio/Sound.h"

#include "Engine/Core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

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
