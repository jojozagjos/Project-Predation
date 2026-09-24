#include "Engine/Audio/SoundDesign.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{
namespace
{

constexpr float kTwoPi = 6.283185307f;
constexpr float kPi = 3.14159265f;

// SplitMix64: the same numbers from the same seed on every machine, which is what makes a take
// reproducible.
struct Random
{
    uint64_t state;
    explicit Random(uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}
    uint64_t Next()
    {
        state += 0x9E3779B97F4A7C15ull;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float Unit() { return static_cast<float>(Next() >> 40) / static_cast<float>(1u << 24); }
    float Signed() { return Unit() * 2.0f - 1.0f; }
};

// Hiss in three colours. White is every frequency alike, which is harsh; pink falls off towards the top
// the way most natural noise does; brown falls off faster still, which is rumble.
struct NoiseSource
{
    Random random;
    float b[7] = {};
    float brown = 0.0f;
    explicit NoiseSource(uint64_t seed) : random(seed) {}
    float Next(int color)
    {
        const float white = random.Signed();
        if (color == 1)
        {
            // Paul Kellet's filter: six one-poles summed, close to pink across the audible range.
            b[0] = 0.99886f * b[0] + white * 0.0555179f;
            b[1] = 0.99332f * b[1] + white * 0.0750759f;
            b[2] = 0.96900f * b[2] + white * 0.1538520f;
            b[3] = 0.86650f * b[3] + white * 0.3104856f;
            b[4] = 0.55000f * b[4] + white * 0.5329522f;
            b[5] = -0.7616f * b[5] - white * 0.0168980f;
            const float pink = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + white * 0.5362f;
            b[6] = white * 0.115926f;
            return pink * 0.11f;
        }
        if (color == 2)
        {
            brown = (brown + white * 0.02f) * 0.998f;
            return brown * 3.5f;
        }
        return white;
    }
};

// A state-variable filter in the form that stays stable while its cutoff moves every sample, which
// every sweep here does. Band pass is normalised to unity at its centre, so a narrow one rings rather
// than shouting.
struct Svf
{
    float ic1 = 0.0f;
    float ic2 = 0.0f;
    void Process(float x, float cutoffHz, float q, int rate, float& low, float& band, float& high)
    {
        const float fc = std::clamp(cutoffHz, 10.0f, static_cast<float>(rate) * 0.45f);
        const float g = std::tan(kPi * fc / static_cast<float>(rate));
        const float k = 1.0f / std::max(q, 0.05f);
        const float a1 = 1.0f / (1.0f + g * (g + k));
        const float a2 = g * a1;
        const float a3 = g * a2;
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        low = v2;
        band = v1 * k;
        high = x - k * v1 - v2;
    }
};

// Takes the corner off a saw or square at each jump, so a high note does not fold back as a whine.
float PolyBlep(float t, float dt)
{
    if (t < dt)
    {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt)
    {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

float Glide(float from, float to, float along)
{
    // Pitch and cutoff move by ratio rather than by difference: an octave is an octave wherever it is.
    return from * std::pow(std::max(to, 1e-3f) / std::max(from, 1e-3f), along);
}

void RenderLayer(const SoundLayer& layer, std::vector<float>& out, int rate, int at, float pitch, float gain,
                 uint64_t seed)
{
    const int frames = std::max(1, static_cast<int>(layer.length * static_cast<float>(rate)));
    NoiseSource noise(seed);
    Random rough(seed ^ 0xA5A5F00Dull);
    Svf filter;
    Svf formant[3];
    float phase = 0.0f;
    float fmPhase = 0.0f;
    float wobble = 0.0f;
    float wobbleTarget = 0.0f;
    int wobbleCount = 0;
    const float hzEnd = layer.hzEnd < 0.0f ? layer.hz : layer.hzEnd;
    const float cutoffEnd = layer.cutoffEnd < 0.0f ? layer.cutoff : layer.cutoffEnd;
    const float fmEnd = layer.fmIndexEnd < 0.0f ? layer.fmIndex : layer.fmIndexEnd;
    const float step = 1.0f / static_cast<float>(rate);
    const float driveAmount = 1.0f + layer.drive * 8.0f;
    const float driveNorm = std::tanh(driveAmount);

    for (int i = 0; i < frames; ++i)
    {
        const int index = at + i;
        if (index < 0)
        {
            continue;
        }
        if (index >= static_cast<int>(out.size()))
        {
            break;
        }
        const float t = static_cast<float>(i) * step;
        const float along = frames > 1 ? static_cast<float>(i) / static_cast<float>(frames - 1) : 0.0f;
        const float shaped = std::pow(along, std::max(layer.curve, 0.05f));

        float envelope = 0.0f;
        if (t < layer.attack)
        {
            envelope = t / std::max(layer.attack, 1e-5f);
        }
        else if (t < layer.attack + layer.hold)
        {
            envelope = 1.0f;
        }
        else
        {
            const float falling = t - layer.attack - layer.hold;
            envelope = layer.sustain + (1.0f - layer.sustain) * std::exp(-falling / std::max(layer.decay, 1e-4f));
        }
        envelope *= std::clamp((layer.length - t) / 0.006f, 0.0f, 1.0f);

        float hz = Glide(layer.hz, hzEnd, shaped) * pitch;
        if (layer.vibratoDepth > 0.0f)
        {
            hz *= std::exp2(layer.vibratoDepth / 12.0f * std::sin(kTwoPi * layer.vibratoHz * t));
        }
        if (layer.jitter > 0.0f)
        {
            // A new direction for the pitch every eight milliseconds or so, eased towards: the
            // unsteadiness of a throat under strain rather than the buzz of random numbers.
            if (wobbleCount-- <= 0)
            {
                wobbleTarget = rough.Signed();
                wobbleCount = rate / 120;
            }
            wobble += (wobbleTarget - wobble) * 0.02f;
            hz *= std::exp2(layer.jitter * 0.6f * wobble);
        }
        const float dt = std::min(hz * step, 0.49f);

        float value = 0.0f;
        switch (layer.source)
        {
        case SoundLayer::Source::Noise:
            value = noise.Next(layer.color);
            break;
        case SoundLayer::Source::Sine:
            value = std::sin(kTwoPi * phase);
            break;
        case SoundLayer::Source::Saw:
            value = 2.0f * phase - 1.0f - PolyBlep(phase, dt);
            break;
        case SoundLayer::Source::Square:
            value = phase < 0.5f ? 1.0f : -1.0f;
            value += PolyBlep(phase, dt);
            value -= PolyBlep(std::fmod(phase + 0.5f, 1.0f), dt);
            break;
        case SoundLayer::Source::Triangle:
            value = 1.0f - 4.0f * std::abs(phase - 0.5f);
            break;
        case SoundLayer::Source::Fm:
        {
            const float depth = fmEnd == layer.fmIndex ? layer.fmIndex : layer.fmIndex + (fmEnd - layer.fmIndex) * shaped;
            value = std::sin(kTwoPi * phase + depth * std::sin(kTwoPi * fmPhase));
            fmPhase += hz * layer.fmRatio * step;
            fmPhase -= std::floor(fmPhase);
            break;
        }
        case SoundLayer::Source::Voice:
        {
            // A glottal pulse, part note and part air, through the resonances of a throat.
            float pulse = 2.0f * phase - 1.0f - PolyBlep(phase, dt);
            pulse = pulse * (1.0f - layer.breath) + noise.Next(1) * layer.breath * 3.0f;
            float sum = 0.0f;
            bool shaped3 = false;
            for (int f = 0; f < 3; ++f)
            {
                if (layer.formants[f] <= 0.0f)
                {
                    continue;
                }
                float low = 0.0f;
                float band = 0.0f;
                float high = 0.0f;
                const float q = layer.formants[f] / std::max(layer.formantWidth, 10.0f);
                formant[f].Process(pulse, layer.formants[f] * std::sqrt(pitch), q, rate, low, band, high);
                sum += band * (f == 0 ? 1.0f : 0.8f / static_cast<float>(f));
                shaped3 = true;
            }
            value = shaped3 ? sum : pulse;
            break;
        }
        case SoundLayer::Source::Click:
            value = i == 0 ? 1.0f : (i == 1 ? -0.6f : 0.0f);
            break;
        }
        phase += dt;
        phase -= std::floor(phase);

        if (layer.filter != 0)
        {
            float low = 0.0f;
            float band = 0.0f;
            float high = 0.0f;
            // The filter moves with the take as the pitch does: a click-only sound is nothing but its filter, and
            // left alone every take of one came out identical.
            filter.Process(value, Glide(layer.cutoff, cutoffEnd, shaped) * pitch, layer.q, rate, low, band, high);
            value = layer.filter == 1 ? low : layer.filter == 2 ? high : band;
        }
        if (layer.amDepth > 0.0f)
        {
            value *= 1.0f - layer.amDepth * 0.5f * (1.0f - std::cos(kTwoPi * layer.amHz * t));
        }
        if (layer.drive > 0.0f)
        {
            value = std::tanh(value * driveAmount) / driveNorm;
        }
        // The click is let through its own first sample, which the envelope would otherwise zero:
        // the impulse is the whole of the source, and what the filter does with it is the sound.
        const float shape = layer.source == SoundLayer::Source::Click ? std::max(envelope, i < 2 ? 1.0f : 0.0f) : envelope;
        out[static_cast<size_t>(index)] += value * shape * gain * layer.gain;
    }
}

// A room, cheaply: eight combs and four all-passes in the manner of Freeverb. Enough to put a sound in
// a concrete corridor rather than in the listener's skull.
void AddRoom(std::vector<float>& samples, int rate, float mix, float size)
{
    if (mix <= 0.0f)
    {
        return;
    }
    const float scale = static_cast<float>(rate) / 44100.0f * (0.5f + size);
    const int combLengths[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    const int allpassLengths[4] = {556, 441, 341, 225};
    const float feedback = 0.70f + 0.26f * std::clamp(size, 0.0f, 1.0f);
    const float damping = 0.35f;

    struct Comb
    {
        std::vector<float> buffer;
        size_t at = 0;
        float stored = 0.0f;
    };
    std::vector<Comb> combs(8);
    for (int i = 0; i < 8; ++i)
    {
        combs[static_cast<size_t>(i)].buffer.assign(std::max<size_t>(1, static_cast<size_t>(combLengths[i] * scale)), 0.0f);
    }
    std::vector<std::vector<float>> allpass(4);
    std::vector<size_t> allpassAt(4, 0);
    for (int i = 0; i < 4; ++i)
    {
        allpass[static_cast<size_t>(i)].assign(std::max<size_t>(1, static_cast<size_t>(allpassLengths[i] * scale)), 0.0f);
    }

    for (float& sample : samples)
    {
        const float input = sample * 0.015f;
        float wet = 0.0f;
        for (Comb& comb : combs)
        {
            const float out = comb.buffer[comb.at];
            comb.stored = out * (1.0f - damping) + comb.stored * damping;
            comb.buffer[comb.at] = input + comb.stored * feedback;
            comb.at = (comb.at + 1) % comb.buffer.size();
            wet += out;
        }
        for (size_t i = 0; i < 4; ++i)
        {
            std::vector<float>& buffer = allpass[i];
            const float delayed = buffer[allpassAt[i]];
            const float out = -wet + delayed;
            buffer[allpassAt[i]] = wet + delayed * 0.5f;
            allpassAt[i] = (allpassAt[i] + 1) % buffer.size();
            wet = out;
        }
        sample = sample + wet * mix * 2.5f;
    }
}

SoundLayer::Source SourceFrom(const std::string& name)
{
    if (name == "sine") return SoundLayer::Source::Sine;
    if (name == "saw") return SoundLayer::Source::Saw;
    if (name == "square") return SoundLayer::Source::Square;
    if (name == "triangle") return SoundLayer::Source::Triangle;
    if (name == "fm") return SoundLayer::Source::Fm;
    if (name == "voice") return SoundLayer::Source::Voice;
    if (name == "click") return SoundLayer::Source::Click;
    return SoundLayer::Source::Noise;
}

int FilterFrom(const nlohmann::json& value)
{
    if (value.is_number_integer())
    {
        return std::clamp(value.get<int>(), 0, 3);
    }
    const std::string name = value.is_string() ? value.get<std::string>() : "";
    if (name == "lowpass") return 1;
    if (name == "highpass") return 2;
    if (name == "bandpass") return 3;
    return 0;
}

int ColorFrom(const nlohmann::json& value)
{
    if (value.is_number_integer())
    {
        return std::clamp(value.get<int>(), 0, 2);
    }
    const std::string name = value.is_string() ? value.get<std::string>() : "";
    if (name == "pink") return 1;
    if (name == "brown") return 2;
    return 0;
}

} // namespace

std::vector<SoundPatch> LoadSoundPatches(const std::string& jsonText, std::vector<std::string>* errors)
{
    std::vector<SoundPatch> patches;
    const nlohmann::json root = nlohmann::json::parse(jsonText, nullptr, false, true);
    if (!root.is_object())
    {
        if (errors != nullptr)
        {
            errors->push_back("not a JSON object");
        }
        return patches;
    }
    for (auto it = root.begin(); it != root.end(); ++it)
    {
        if (it.key().empty() || it.key()[0] == '_' || !it.value().is_object())
        {
            continue;
        }
        const nlohmann::json& p = it.value();
        SoundPatch patch;
        patch.name = it.key();
        patch.seconds = p.value("seconds", patch.seconds);
        patch.variants = std::clamp(p.value("variants", patch.variants), 1, 12);
        patch.vary = p.value("vary", patch.vary);
        patch.gain = p.value("gain", patch.gain);
        patch.loop = p.value("loop", patch.loop);
        patch.room = p.value("room", patch.room);
        patch.roomSize = p.value("room_size", patch.roomSize);
        patch.seed = p.value("seed", patch.seed);
        if (!p.contains("layers") || !p["layers"].is_array() || p["layers"].empty())
        {
            if (errors != nullptr)
            {
                errors->push_back(patch.name + ": no layers");
            }
            continue;
        }
        for (const nlohmann::json& l : p["layers"])
        {
            SoundLayer layer;
            layer.source = SourceFrom(l.value("source", std::string("noise")));
            layer.start = l.value("start", layer.start);
            layer.length = l.value("length", patch.seconds - layer.start);
            layer.gain = l.value("gain", layer.gain);
            layer.attack = l.value("attack", layer.attack);
            layer.hold = l.value("hold", layer.hold);
            layer.decay = l.value("decay", layer.decay);
            layer.sustain = l.value("sustain", layer.sustain);
            layer.hz = l.value("hz", layer.hz);
            layer.hzEnd = l.value("hz_end", layer.hzEnd);
            layer.curve = l.value("curve", layer.curve);
            layer.vibratoHz = l.value("vibrato_hz", layer.vibratoHz);
            layer.vibratoDepth = l.value("vibrato_depth", layer.vibratoDepth);
            layer.jitter = l.value("jitter", layer.jitter);
            if (l.contains("color"))
            {
                layer.color = ColorFrom(l["color"]);
            }
            if (l.contains("filter"))
            {
                layer.filter = FilterFrom(l["filter"]);
            }
            layer.cutoff = l.value("cutoff", layer.cutoff);
            layer.cutoffEnd = l.value("cutoff_end", layer.cutoffEnd);
            layer.q = l.value("q", layer.q);
            layer.fmRatio = l.value("fm_ratio", layer.fmRatio);
            layer.fmIndex = l.value("fm_index", layer.fmIndex);
            layer.fmIndexEnd = l.value("fm_index_end", layer.fmIndexEnd);
            if (l.contains("formants") && l["formants"].is_array())
            {
                for (size_t f = 0; f < 3 && f < l["formants"].size(); ++f)
                {
                    layer.formants[f] = l["formants"][f].get<float>();
                }
            }
            layer.formantWidth = l.value("formant_width", layer.formantWidth);
            layer.breath = l.value("breath", layer.breath);
            layer.amHz = l.value("am_hz", layer.amHz);
            layer.amDepth = l.value("am_depth", layer.amDepth);
            layer.drive = l.value("drive", layer.drive);
            layer.repeats = std::clamp(l.value("repeats", layer.repeats), 1, 200);
            layer.interval = l.value("interval", layer.interval);
            layer.intervalJitter = l.value("interval_jitter", layer.intervalJitter);
            layer.repeatGain = l.value("repeat_gain", layer.repeatGain);
            layer.repeatPitch = l.value("repeat_pitch", layer.repeatPitch);
            patch.layers.push_back(layer);
        }
        patches.push_back(std::move(patch));
    }
    return patches;
}

SoundData RenderPatch(const SoundPatch& patch, int take, int sampleRate)
{
    SoundData data;
    data.sampleRate = std::max(sampleRate, 8000);
    const int rate = data.sampleRate;

    // Each take its own pitch and pace, a little either side, so three of them are three sounds.
    Random vary(static_cast<uint64_t>(patch.seed) * 7919u + static_cast<uint64_t>(take) * 104729u + 17u);
    const float pitch = 1.0f + patch.vary * vary.Signed();
    const float pace = 1.0f + patch.vary * 0.5f * vary.Signed();

    const float seconds = std::clamp(patch.seconds, 0.01f, 30.0f);
    // A loop is rendered long and folded: the overlap is crossfaded back over its own beginning.
    const float overlap = patch.loop ? std::min(0.5f, seconds * 0.25f) : 0.0f;
    const int loopFrames = static_cast<int>(seconds * static_cast<float>(rate));
    const int overlapFrames = static_cast<int>(overlap * static_cast<float>(rate));
    std::vector<float> mix(static_cast<size_t>(loopFrames + overlapFrames), 0.0f);

    for (size_t l = 0; l < patch.layers.size(); ++l)
    {
        const SoundLayer& layer = patch.layers[l];
        Random repeatRandom(static_cast<uint64_t>(patch.seed) * 131u + l * 977u + static_cast<uint64_t>(take) * 7919u);
        float at = layer.start * pace;
        float gain = 1.0f;
        for (int r = 0; r < layer.repeats; ++r)
        {
            const float semitones = layer.repeatPitch * (r == 0 ? 0.0f : repeatRandom.Signed());
            const uint64_t seed = static_cast<uint64_t>(patch.seed) * 1000003u + l * 9973u +
                                  static_cast<uint64_t>(take) * 7919u + static_cast<uint64_t>(r) * 31u;
            RenderLayer(layer, mix, rate, static_cast<int>(at * static_cast<float>(rate)),
                        pitch * std::exp2(semitones / 12.0f), gain, seed);
            at += layer.interval * pace * (1.0f + layer.intervalJitter * repeatRandom.Signed());
            gain *= layer.repeatGain;
        }
    }

    AddRoom(mix, rate, patch.room, patch.roomSize);

    // No offset left in it: a sound sitting off zero thumps when it starts and when it stops.
    float previousIn = 0.0f;
    float previousOut = 0.0f;
    for (float& sample : mix)
    {
        const float out = sample - previousIn + 0.995f * previousOut;
        previousIn = sample;
        previousOut = out;
        sample = out;
    }

    if (patch.loop && overlapFrames > 0)
    {
        for (int i = 0; i < overlapFrames; ++i)
        {
            const float w = std::sin(0.5f * kPi * static_cast<float>(i) / static_cast<float>(overlapFrames));
            const float head = mix[static_cast<size_t>(i)];
            const float tail = mix[static_cast<size_t>(loopFrames + i)];
            mix[static_cast<size_t>(i)] = head * w + tail * std::sqrt(std::max(1.0f - w * w, 0.0f));
        }
        mix.resize(static_cast<size_t>(loopFrames));
    }

    float peak = 0.0f;
    for (const float sample : mix)
    {
        peak = std::max(peak, std::abs(sample));
    }
    const float scale = peak > 1e-6f ? patch.gain / peak : 0.0f;
    for (float& sample : mix)
    {
        sample *= scale;
    }

    // Not a loop: a few milliseconds of fade at the very end, whatever the layers did.
    if (!patch.loop)
    {
        const int fade = std::min(static_cast<int>(0.004f * static_cast<float>(rate)), static_cast<int>(mix.size()));
        for (int i = 0; i < fade; ++i)
        {
            mix[mix.size() - 1 - static_cast<size_t>(i)] *= static_cast<float>(i) / static_cast<float>(std::max(fade, 1));
        }
    }
    data.samples = std::move(mix);
    return data;
}

} // namespace pred
