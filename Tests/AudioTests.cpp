#include "Engine/Audio/AudioEngine.h"
#include "Engine/Audio/Sound.h"
#include "Engine/Audio/VoiceCapture.h"
#include "Engine/Audio/VoiceCodec.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace pred;

// Sound is the one subsystem where a fault is hard to see and easy to hear, and "play it and
// listen" is not a test anybody can run twice the same way. So the mixer is a plain function of a
// list of voices and a listener, and everything below runs it with no sound card at all.
namespace
{

constexpr int kRate = 48000;

// A flat buffer of ones, which makes every reading below a gain rather than a waveform.
SoundData Steady(float seconds)
{
    SoundData data;
    data.sampleRate = kRate;
    data.samples.assign(static_cast<size_t>(seconds * kRate), 1.0f);
    return data;
}

// The loudest sample in each channel over a mix.
struct Peaks
{
    float left = 0.0f;
    float right = 0.0f;
};

Peaks MixPeaks(AudioEngine& audio, int frames)
{
    std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
    audio.Mix(out.data(), frames);
    Peaks peaks;
    for (int i = 0; i < frames; ++i)
    {
        peaks.left = std::max(peaks.left, std::abs(out[static_cast<size_t>(i) * 2]));
        peaks.right = std::max(peaks.right, std::abs(out[static_cast<size_t>(i) * 2 + 1]));
    }
    return peaks;
}

// Long enough for the gain smoothing to have arrived wherever it is going.
constexpr int kSettled = 4096;

} // namespace

TEST_CASE("A sound gets quieter as you walk away from it", "[audio]")
{
    AudioEngine audio;
    const SoundId tone = audio.Add("tone", Steady(2.0f));
    audio.SetListener({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});

    const auto loudnessAt = [&](float distance)
    {
        AudioEngine::PlayDesc desc;
        desc.sound = tone;
        desc.position = {0.0f, 0.0f, -distance}; // straight ahead, so both ears get the same
        desc.nearDistance = 3.0f;
        desc.farDistance = 40.0f;
        const VoiceId voice = audio.Play(desc);
        REQUIRE(voice != kInvalidVoice);
        const Peaks peaks = MixPeaks(audio, kSettled);
        audio.Stop(voice);
        MixPeaks(audio, kSettled); // let it go
        return (peaks.left + peaks.right) * 0.5f;
    };

    const float close = loudnessAt(1.0f);
    const float middle = loudnessAt(20.0f);
    const float far = loudnessAt(39.0f);
    const float beyond = loudnessAt(60.0f);

    INFO("1 m " << close << ", 20 m " << middle << ", 39 m " << far << ", 60 m " << beyond);
    CHECK(close > middle);
    CHECK(middle > far);
    // Inside the near distance nothing is taken off by distance at all. A centred sound is 0.707 in
    // each ear rather than 1, which is the same energy in the pair: that is what constant power
    // means and it is what stops a sound dipping as it crosses in front of you.
    CHECK(close == Catch::Approx(0.7071f * AudioEngine::kHeadroom).margin(0.02));
    // And past the far one there is nothing to hear.
    CHECK(beyond == Catch::Approx(0.0f).margin(0.001));
}

TEST_CASE("A sound to your left comes out of the left", "[audio]")
{
    AudioEngine audio;
    const SoundId tone = audio.Add("tone", Steady(2.0f));
    // Facing -Z with +Y up, which is the game's convention, puts +X on the listener's right.
    audio.SetListener({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});

    const auto play = [&](const glm::vec3& where)
    {
        AudioEngine::PlayDesc desc;
        desc.sound = tone;
        desc.position = where;
        desc.nearDistance = 1.0f;
        desc.farDistance = 40.0f;
        const VoiceId voice = audio.Play(desc);
        REQUIRE(voice != kInvalidVoice);
        const Peaks peaks = MixPeaks(audio, kSettled);
        audio.Stop(voice);
        MixPeaks(audio, kSettled);
        return peaks;
    };

    const Peaks left = play({-8.0f, 0.0f, 0.0f});
    INFO("to the left: " << left.left << " / " << left.right);
    CHECK(left.left > left.right * 1.5f);

    const Peaks right = play({8.0f, 0.0f, 0.0f});
    INFO("to the right: " << right.left << " / " << right.right);
    CHECK(right.right > right.left * 1.5f);

    // Straight ahead is even, and no quieter than either side: a sound crossing in front of you
    // must not dip as it passes the middle, which is what panning without constant power does.
    const Peaks ahead = play({0.0f, 0.0f, -8.0f});
    INFO("ahead: " << ahead.left << " / " << ahead.right);
    CHECK(ahead.left == Catch::Approx(ahead.right).margin(0.01));
    CHECK(ahead.left + ahead.right > (left.left + left.right) * 0.9f);
}

TEST_CASE("Turning round swaps which ear a sound is in", "[audio]")
{
    // The listener's own facing is what decides left and right, not the world's axes.
    AudioEngine audio;
    const SoundId tone = audio.Add("tone", Steady(2.0f));

    AudioEngine::PlayDesc desc;
    desc.sound = tone;
    desc.position = {-8.0f, 0.0f, 0.0f};
    desc.nearDistance = 1.0f;
    desc.farDistance = 40.0f;

    audio.SetListener({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});
    const VoiceId voice = audio.Play(desc);
    REQUIRE(voice != kInvalidVoice);
    const Peaks facingAway = MixPeaks(audio, kSettled);

    audio.SetListener({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f});
    // One buffer for the gains to cross, then measure. Measuring across the crossing would read the
    // loudest moment of the turn rather than where the turn ended up.
    MixPeaks(audio, kSettled);
    const Peaks turnedRound = MixPeaks(audio, kSettled);

    INFO("before " << facingAway.left << " / " << facingAway.right << ", after "
                   << turnedRound.left << " / " << turnedRound.right);
    CHECK(facingAway.left > facingAway.right);
    CHECK(turnedRound.right > turnedRound.left);
}

TEST_CASE("A one-shot ends by itself and a loop does not", "[audio]")
{
    AudioEngine audio;
    const SoundId shot = audio.Add("shot", Steady(0.05f)); // 2400 frames

    AudioEngine::PlayDesc desc;
    desc.sound = shot;
    desc.positioned = false;
    const VoiceId once = audio.Play(desc);
    REQUIRE(audio.IsPlaying(once));
    MixPeaks(audio, 1024);
    CHECK(audio.IsPlaying(once)); // still going, it is 2400 frames long
    MixPeaks(audio, 4096);
    CHECK_FALSE(audio.IsPlaying(once));

    desc.loop = true;
    const VoiceId forever = audio.Play(desc);
    MixPeaks(audio, kRate); // a whole second, twenty times its length
    CHECK(audio.IsPlaying(forever));
    audio.Stop(forever);
    MixPeaks(audio, kSettled);
    CHECK_FALSE(audio.IsPlaying(forever));
}

TEST_CASE("Nothing leaves the mixer louder than a speaker can take", "[audio]")
{
    // Eight sounds at full volume at once really do add up past one, and what a sound card does
    // with a sample past one is not a louder noise, it is a tearing one.
    AudioEngine audio;
    const SoundId tone = audio.Add("tone", Steady(1.0f));

    AudioEngine::PlayDesc desc;
    desc.sound = tone;
    desc.positioned = false;
    for (int i = 0; i < 8; ++i)
    {
        REQUIRE(audio.Play(desc) != kInvalidVoice);
    }

    std::vector<float> out(static_cast<size_t>(kSettled) * 2, 0.0f);
    audio.Mix(out.data(), kSettled);
    float loudest = 0.0f;
    for (const float sample : out)
    {
        loudest = std::max(loudest, std::abs(sample));
    }
    INFO("eight at once peaked at " << loudest);
    CHECK(loudest <= 1.0f);
    CHECK(audio.GetStats().clipped > 0);
}

TEST_CASE("A voice that moves does not click", "[audio]")
{
    // A gain that jumps from one buffer to the next is a click, and a click is the most audible
    // fault a mixer has. What that looks like in the samples is a step: the signal here is flat, so
    // every difference between one sample and the next is the gain moving.
    AudioEngine audio;
    const SoundId tone = audio.Add("tone", Steady(3.0f));
    audio.SetListener({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});

    AudioEngine::PlayDesc desc;
    desc.sound = tone;
    desc.position = {-10.0f, 0.0f, 0.0f};
    desc.nearDistance = 1.0f;
    desc.farDistance = 40.0f;
    const VoiceId voice = audio.Play(desc);
    REQUIRE(voice != kInvalidVoice);

    std::vector<float> out(512 * 2, 0.0f);
    audio.Mix(out.data(), 512);
    float previous = out[0];
    float worst = 0.0f;

    // Flung across the listener, ten metres a buffer, which is far faster than anything in the game
    // moves.
    for (int buffer = 0; buffer < 20; ++buffer)
    {
        audio.SetVoicePosition(voice, {-10.0f + static_cast<float>(buffer), 0.0f, 0.0f});
        audio.Mix(out.data(), 512);
        for (int i = 0; i < 512; ++i)
        {
            const float sample = out[static_cast<size_t>(i) * 2];
            worst = std::max(worst, std::abs(sample - previous));
            previous = sample;
        }
    }

    INFO("the biggest step between two samples was " << worst);
    // The gain is allowed to cross its whole range in about eight milliseconds, which at this rate
    // is a step of about 1/384 a sample. Anything near a tenth is a jump between buffers.
    CHECK(worst < 0.01f);
}

TEST_CASE("The same recipe always makes the same sound", "[audio]")
{
    // Two machines in a game must not disagree about what a gunshot sounds like, and a test that
    // asserts on a waveform is only worth writing if the waveform is the same every time.
    SoundRecipe recipe;
    recipe.seconds = 0.3f;
    recipe.decay = 0.08f;
    recipe.bite = 0.8f;
    recipe.seed = 12345;

    const SoundData first = Synthesise(recipe, kRate);
    const SoundData second = Synthesise(recipe, kRate);
    REQUIRE(first.samples.size() == second.samples.size());
    CHECK(first.samples == second.samples);

    // And it is a sound rather than silence or a constant.
    float loudest = 0.0f;
    for (const float sample : first.samples)
    {
        loudest = std::max(loudest, std::abs(sample));
    }
    INFO("peak " << loudest << " over " << first.Seconds() << " s");
    CHECK(loudest > 0.2f);
    // It decays: the end is far quieter than the start, which is what makes a bang a bang.
    const size_t quarter = first.samples.size() / 4;
    float early = 0.0f;
    float late = 0.0f;
    for (size_t i = 0; i < quarter; ++i)
    {
        early = std::max(early, std::abs(first.samples[i]));
        late = std::max(late, std::abs(first.samples[first.samples.size() - 1 - i]));
    }
    INFO("first quarter peaks at " << early << ", last quarter at " << late);
    CHECK(early > late * 4.0f);
}

TEST_CASE("A recipe file is read, and a broken one does not take the rest with it", "[audio]")
{
    const std::string text = R"({
        "gunshot": { "seconds": 0.3, "decay": 0.06, "bite": 0.9, "lowpass": 0.7 },
        "door":    { "seconds": 1.2, "tone": 0.8, "noise": 0.2, "tone_hz": 90, "tone_drop": 1.0 },
        "broken":  7,
        "pickup":  { "seconds": 0.18, "tone": 1.0, "noise": 0.0, "tone_hz": 520, "tone_drop": -0.6 }
    })";

    const std::vector<SoundLibraryEntry> entries = LoadSoundRecipes(text);
    REQUIRE(entries.size() == 3);

    const auto named = [&](const char* name)
    {
        return std::find_if(entries.begin(), entries.end(),
                            [&](const SoundLibraryEntry& entry) { return entry.name == name; });
    };
    REQUIRE(named("gunshot") != entries.end());
    REQUIRE(named("door") != entries.end());
    REQUIRE(named("pickup") != entries.end());
    CHECK(named("broken") == entries.end());

    CHECK(named("door")->recipe.toneHz == Catch::Approx(90.0f));
    CHECK(named("pickup")->recipe.toneDrop == Catch::Approx(-0.6f));

    // A key beginning with an underscore is a note to whoever is editing the file, not a sound, and
    // warning about one every startup teaches people to ignore the warnings.
    CHECK(LoadSoundRecipes(R"({"_note": "a comment", "bang": {"seconds": 0.2}})").size() == 1);

    // Nonsense in gives nothing out rather than taking the process down.
    CHECK(LoadSoundRecipes("not json at all").empty());
    CHECK(LoadSoundRecipes("[1, 2, 3]").empty());
}

TEST_CASE("Everything still works with no sound card", "[audio]")
{
    // A headless run makes no noise and takes no special path to do it: the device is the outer
    // layer, and everything above it runs whether or not there is one.
    AudioEngine audio;
    CHECK_FALSE(audio.HasDevice());

    const SoundId tone = audio.Add("tone", Steady(0.5f));
    CHECK(audio.Find("tone") == tone);
    CHECK(audio.Find("nothing called this") == kInvalidSound);

    AudioEngine::PlayDesc desc;
    desc.sound = tone;
    CHECK(audio.Play(desc) != kInvalidVoice);
    CHECK(audio.GetStats().voices == 1);

    // Playing something that does not exist is refused rather than crashing on it.
    desc.sound = kInvalidSound;
    CHECK(audio.Play(desc) == kInvalidVoice);
}

TEST_CASE("A full mixer drops the quietest voice, not the newest", "[audio]")
{
    // A footstep across the map losing to a gunshot beside you is the right way round.
    AudioEngine::Settings settings;
    settings.maxVoices = 4;
    AudioEngine mixer;
    // Opened and closed again: the settings stick, and the mixer is then driven by this test alone
    // rather than by a sound card asking for buffers underneath it.
    mixer.Init(settings);
    mixer.Shutdown();
    const SoundId tone = mixer.Add("tone", Steady(2.0f));
    mixer.SetListener({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});

    AudioEngine::PlayDesc desc;
    desc.sound = tone;
    desc.nearDistance = 1.0f;
    desc.farDistance = 200.0f;

    // Four voices, the first of them a long way off.
    desc.position = {0.0f, 0.0f, -150.0f};
    const VoiceId distant = mixer.Play(desc);
    for (int i = 0; i < 3; ++i)
    {
        desc.position = {0.0f, 0.0f, -2.0f - static_cast<float>(i)};
        REQUIRE(mixer.Play(desc) != kInvalidVoice);
    }
    REQUIRE(mixer.IsPlaying(distant));

    // A fifth, right beside the listener.
    desc.position = {0.0f, 0.0f, -0.5f};
    const VoiceId close = mixer.Play(desc);
    CHECK(close != kInvalidVoice);
    CHECK(mixer.IsPlaying(close));
    CHECK_FALSE(mixer.IsPlaying(distant));
    CHECK(mixer.GetStats().stolen == 1);
}

namespace
{

// Builds a wav in memory. `junk` puts a JUNK chunk before the format, which is what the footstep
// pack this was written for actually does and what a reader that seeks to a fixed offset gets wrong.
std::vector<unsigned char> MakeWav(const std::vector<std::vector<int16_t>>& channels, int rate,
                                   bool junk)
{
    const size_t frames = channels.empty() ? 0 : channels.front().size();
    const size_t channelCount = channels.size();
    const size_t dataBytes = frames * channelCount * 2;

    std::vector<unsigned char> out;
    const auto u32 = [&out](uint32_t v)
    {
        out.push_back(static_cast<unsigned char>(v & 0xFF));
        out.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
        out.push_back(static_cast<unsigned char>((v >> 16) & 0xFF));
        out.push_back(static_cast<unsigned char>((v >> 24) & 0xFF));
    };
    const auto u16 = [&out](uint16_t v)
    {
        out.push_back(static_cast<unsigned char>(v & 0xFF));
        out.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
    };
    const auto tag = [&out](const char* s)
    { for (int i = 0; i < 4; ++i) { out.push_back(static_cast<unsigned char>(s[i])); } };

    tag("RIFF");
    u32(0); // patched at the end
    tag("WAVE");
    if (junk)
    {
        tag("JUNK");
        u32(4);
        u32(0);
    }
    tag("fmt ");
    u32(16);
    u16(1);                                        // PCM
    u16(static_cast<uint16_t>(channelCount));
    u32(static_cast<uint32_t>(rate));
    u32(static_cast<uint32_t>(rate * channelCount * 2)); // bytes per second
    u16(static_cast<uint16_t>(channelCount * 2));        // block align
    u16(16);                                             // bits
    tag("data");
    u32(static_cast<uint32_t>(dataBytes));
    for (size_t frame = 0; frame < frames; ++frame)
    {
        for (size_t channel = 0; channel < channelCount; ++channel)
        {
            u16(static_cast<uint16_t>(channels[channel][frame]));
        }
    }
    const uint32_t riffSize = static_cast<uint32_t>(out.size() - 8);
    out[4] = static_cast<unsigned char>(riffSize & 0xFF);
    out[5] = static_cast<unsigned char>((riffSize >> 8) & 0xFF);
    out[6] = static_cast<unsigned char>((riffSize >> 16) & 0xFF);
    out[7] = static_cast<unsigned char>((riffSize >> 24) & 0xFF);
    return out;
}

} // namespace

TEST_CASE("A wav is read past whatever chunks come before the format")
{
    const std::vector<int16_t> ramp{0, 16384, -16384, 32767, -32768};
    const std::vector<unsigned char> file = MakeWav({ramp}, 44100, true);

    SoundData data;
    std::string error;
    REQUIRE(LoadWav(file.data(), file.size(), data, error));
    CHECK(error.empty());
    CHECK(data.sampleRate == 44100);
    REQUIRE(data.samples.size() == ramp.size());
    CHECK(data.samples[0] == Catch::Approx(0.0f));
    CHECK(data.samples[1] == Catch::Approx(0.5f));
    CHECK(data.samples[2] == Catch::Approx(-0.5f));
    CHECK(data.samples[4] == Catch::Approx(-1.0f));
}

TEST_CASE("A stereo wav is averaged down to mono")
{
    // The mixer places sounds itself, so a source that has already decided which ear it is in is
    // fighting the positioning. Left and right that cancel must come out silent, not one-sided.
    const std::vector<int16_t> left{16384, 16384};
    const std::vector<int16_t> right{-16384, 16384};
    const std::vector<unsigned char> file = MakeWav({left, right}, 48000, false);

    SoundData data;
    std::string error;
    REQUIRE(LoadWav(file.data(), file.size(), data, error));
    REQUIRE(data.samples.size() == 2);
    CHECK(data.samples[0] == Catch::Approx(0.0f).margin(1e-6));
    CHECK(data.samples[1] == Catch::Approx(0.5f));
}

TEST_CASE("A sound written out reads back as the same sound", "[audio][wav]")
{
    // Every sound in the game is now a wav on disk, and they were produced by this writer. A writer
    // that is subtly wrong -- a header field off by four, a sample scaled by 32768 so the loudest
    // one wraps to silence -- would put twelve broken files in the repository and the fault would
    // look like the mixer.
    SoundData original;
    original.sampleRate = 48000;
    original.samples.reserve(512);
    for (size_t i = 0; i < 512; ++i)
    {
        // A sweep that reaches both rails, because the interesting mistakes are all at the extremes.
        const float t = static_cast<float>(i) / 511.0f;
        original.samples.push_back(std::sin(t * 37.0f) * (0.02f + 0.98f * t));
    }
    original.samples.push_back(1.0f);
    original.samples.push_back(-1.0f);

    const std::vector<uint8_t> bytes = SaveWav(original);
    REQUIRE(bytes.size() == 44 + original.samples.size() * 2);

    SoundData read;
    std::string error;
    REQUIRE(LoadWav(bytes.data(), bytes.size(), read, error));
    CHECK(read.sampleRate == original.sampleRate);
    REQUIRE(read.samples.size() == original.samples.size());

    // Two least significant bits, which is all that is available to lose.
    //
    // One of them is the rounding to sixteen bits. The other is that the writer scales by 32767 so
    // that full scale lands exactly on the largest value a signed short holds, while the reader
    // divides by 32768 so that the largest *negative* value maps to exactly -1. Both conventions are
    // right on their own end of the range and they differ by one part in 32768; picking either one
    // for both ends makes one rail slightly wrong instead. Under a thousandth of a decibel either
    // way, and worth a sentence here rather than a bug report later.
    for (size_t i = 0; i < original.samples.size(); ++i)
    {
        CHECK(std::abs(read.samples[i] - original.samples[i]) < 2.0f / 32767.0f);
    }
    // And the rails survive, rather than wrapping to the opposite one.
    CHECK(read.samples[read.samples.size() - 2] > 0.99f);
    CHECK(read.samples[read.samples.size() - 1] < -0.99f);
}

TEST_CASE("Anything above full scale is held at it rather than wrapping", "[audio][wav]")
{
    // A sample above one truncated into a signed short wraps to a large negative number, which is
    // the loudest noise a sound card can make. Clamped before scaling, not after.
    SoundData loud;
    loud.sampleRate = 48000;
    loud.samples = {4.0f, -4.0f, 1.5f, -1.5f};

    SoundData read;
    std::string error;
    const std::vector<uint8_t> bytes = SaveWav(loud);
    REQUIRE(LoadWav(bytes.data(), bytes.size(), read, error));
    REQUIRE(read.samples.size() == 4);
    CHECK(read.samples[0] > 0.99f);
    CHECK(read.samples[1] < -0.99f);
    CHECK(read.samples[2] > 0.99f);
    CHECK(read.samples[3] < -0.99f);
}

TEST_CASE("Nonsense is refused with a reason rather than crashing")
{
    SoundData data;
    std::string error;
    const char* notAWav = "this is not a wav file at all";
    CHECK_FALSE(LoadWav(notAWav, std::strlen(notAWav), data, error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(LoadWav(nullptr, 0, data, error));
}

TEST_CASE("Trailing silence is trimmed and the cut is faded")
{
    // A sound pack pads its clips. That padding holds a voice open for its whole length, and at a
    // sprint the next step starts before the last file has finished being silent at us.
    SoundData data;
    data.sampleRate = 1000;
    data.samples.assign(1000, 0.0f);
    for (int i = 0; i < 100; ++i)
    {
        data.samples[static_cast<size_t>(i)] = 0.8f;
    }

    const size_t removed = TrimTrailingSilence(data, 0.0025f, 0.005f);
    CHECK(removed == 900);
    REQUIRE(data.samples.size() == 100);
    // The last sample is on its way down rather than landing on full scale, which is the click.
    CHECK(data.samples.back() < 0.8f);
    CHECK(data.samples.back() > 0.0f);
    CHECK(data.samples[0] == Catch::Approx(0.8f));

    // Nothing to trim leaves the buffer exactly as it was.
    SoundData loud;
    loud.sampleRate = 1000;
    loud.samples.assign(10, 0.5f);
    CHECK(TrimTrailingSilence(loud) == 0);
    CHECK(loud.samples.size() == 10);
}

TEST_CASE("A stream plays what has arrived and waits for the rest", "[audio][stream][voice]")
{
    // Every other sound in this game is a complete buffer before anybody hears it. A voice coming
    // down a wire is not: it arrives while it is being played, in fragments, with holes. The mixer
    // has to keep going through all of that rather than deciding the voice ended at the first gap.
    AudioEngine mixer;
    AudioEngine::Settings settings;
    settings.sampleRate = 48000;
    mixer.Init(settings);
    mixer.Shutdown();

    const StreamId stream = mixer.OpenStream(48000);
    REQUIRE(stream != kInvalidStream);

    AudioEngine::PlayDesc desc;
    desc.stream = stream;
    desc.positioned = false;
    const VoiceId voice = mixer.Play(desc);
    REQUIRE(voice != kInvalidVoice);
    // Opened empty on purpose: a voice that refused an empty stream would lose the first syllable,
    // because the first syllable is what opens it.
    CHECK(mixer.IsPlaying(voice));

    std::vector<float> out(64 * 2, 0.0f);

    // Nothing has arrived. Silence, and the voice is still there.
    mixer.Mix(out.data(), 64);
    for (float sample : out)
    {
        CHECK(sample == Catch::Approx(0.0f));
    }
    CHECK(mixer.IsPlaying(voice));

    // Now some does.
    std::vector<float> speech(32, 0.5f);
    mixer.PushStream(stream, speech.data(), speech.size());
    CHECK(mixer.StreamQueued(stream) == 32);

    std::fill(out.begin(), out.end(), 0.0f);
    mixer.Mix(out.data(), 64);
    // The first samples are the ones pushed; the rest is the gap after them.
    CHECK(out[0] > 0.0f);
    CHECK(out[2 * 20] > 0.0f);
    CHECK(out[2 * 50] == Catch::Approx(0.0f));
    CHECK(mixer.IsPlaying(voice));

    // What was played is not kept: an hour of conversation must not be an hour of audio in memory.
    CHECK(mixer.StreamQueued(stream) < 32);

    // More arrives after the gap and is heard, rather than the voice having given up.
    mixer.PushStream(stream, speech.data(), speech.size());
    std::fill(out.begin(), out.end(), 0.0f);
    mixer.Mix(out.data(), 64);
    CHECK(out[0] > 0.0f);
    CHECK(mixer.IsPlaying(voice));
}

TEST_CASE("A closed stream finishes what it has and then ends", "[audio][stream][voice]")
{
    AudioEngine mixer;
    AudioEngine::Settings settings;
    settings.sampleRate = 48000;
    mixer.Init(settings);
    mixer.Shutdown();

    const StreamId stream = mixer.OpenStream(48000);
    AudioEngine::PlayDesc desc;
    desc.stream = stream;
    desc.positioned = false;
    const VoiceId voice = mixer.Play(desc);

    std::vector<float> speech(16, 0.25f);
    mixer.PushStream(stream, speech.data(), speech.size());
    mixer.CloseStream(stream);
    // Closing does not cut anybody off mid-word: what arrived is still played.
    CHECK(mixer.IsPlaying(voice));

    std::vector<float> out(64 * 2, 0.0f);
    mixer.Mix(out.data(), 64);
    CHECK(out[0] > 0.0f);
    // And then it really is over, rather than sitting open forever waiting for a speaker who left.
    CHECK_FALSE(mixer.IsPlaying(voice));
}

TEST_CASE("A stream that falls behind drops the oldest, not the newest", "[audio][stream][voice]")
{
    // A listener whose machine stalled would otherwise be permanently behind the conversation, with
    // no way to catch up: everything they hear from then on is late by however long the stall was.
    // Dropping audio is audible; being a second late for the rest of the game is worse.
    AudioEngine mixer;
    AudioEngine::Settings settings;
    settings.sampleRate = 48000;
    mixer.Init(settings);
    mixer.Shutdown();

    const StreamId stream = mixer.OpenStream(48000);
    // Two seconds of it, against a half-second cap.
    std::vector<float> flood(48000 * 2, 0.1f);
    mixer.PushStream(stream, flood.data(), flood.size());

    const size_t queued = mixer.StreamQueued(stream);
    INFO("queued " << queued << " samples of " << flood.size() << " pushed");
    CHECK(queued <= 48000 / 2 + 1);
    CHECK(queued > 48000 / 4);
}

TEST_CASE("Captured audio comes out in whole frames", "[audio][voice][capture]")
{
    // The device hands over whatever it has, in whatever size it likes. What goes on the wire has
    // to be the same size every time, so the queue is cut into frames here and a part frame waits.
    //
    // Configured rather than started, and driven through OnRecorded rather than through a
    // microphone. Opening a real device in a test would mix whatever the build machine can hear
    // into the thing being measured, which is not a test.
    VoiceCapture capture;
    VoiceCapture::Settings settings;
    settings.sampleRate = 48000;
    settings.frameSeconds = 0.020f; // 960 samples
    capture.Configure(settings);
    REQUIRE(capture.FrameSamples() == 960);

    std::vector<float> frame;
    CHECK_FALSE(capture.ReadFrame(frame));

    // One and a half frames in: one comes out, the half waits rather than being sent short.
    std::vector<float> recorded(1440, 0.3f);
    capture.OnRecorded(recorded.data(), recorded.size());
    REQUIRE(capture.ReadFrame(frame));
    CHECK(frame.size() == 960);
    CHECK_FALSE(capture.ReadFrame(frame));

    // The rest of it arrives and completes the second frame.
    capture.OnRecorded(recorded.data(), 480);
    REQUIRE(capture.ReadFrame(frame));
    CHECK(frame.size() == 960);
}

TEST_CASE("The microphone queue drops the oldest when nobody reads it", "[audio][voice][capture]")
{
    // A game that stalls must not then transmit the backlog. Everybody else has moved on, and
    // audio that arrives late in a conversation is worse than audio that never arrives.
    VoiceCapture capture;
    VoiceCapture::Settings settings;
    settings.sampleRate = 8000;
    settings.frameSeconds = 0.020f;    // 160 samples
    settings.maxQueuedSeconds = 0.10f; // 800 samples
    capture.Configure(settings);

    std::vector<float> loud(16000, 0.0f);
    for (size_t i = 0; i < loud.size(); ++i)
    {
        // A ramp, so which part survived can be told from the values.
        loud[i] = static_cast<float>(i) / static_cast<float>(loud.size());
    }
    capture.OnRecorded(loud.data(), loud.size());

    std::vector<float> frame;
    REQUIRE(capture.ReadFrame(frame));
    REQUIRE(frame.size() == 160);
    // The samples that survived are from the end of what was recorded, not the beginning.
    INFO("first surviving sample is " << frame.front());
    CHECK(frame.front() > 0.85f);

    // And only the cap's worth is there: five frames of 160, minus the one just taken.
    int remaining = 0;
    while (capture.ReadFrame(frame))
    {
        ++remaining;
    }
    INFO(remaining << " further frames were queued");
    CHECK(remaining == 4);
}

TEST_CASE("The level meter follows the loudest sample in the frame", "[audio][voice][capture]")
{
    VoiceCapture capture;
    VoiceCapture::Settings settings;
    settings.sampleRate = 8000;
    settings.frameSeconds = 0.020f;
    capture.Configure(settings);

    std::vector<float> quiet(160, 0.02f);
    capture.OnRecorded(quiet.data(), quiet.size());
    std::vector<float> frame;
    REQUIRE(capture.ReadFrame(frame));
    CHECK(capture.LastLevel() == Catch::Approx(0.02f));

    std::vector<float> shout(160, 0.0f);
    shout[80] = -0.9f; // negative, because loudness is not a sign
    capture.OnRecorded(shout.data(), shout.size());
    REQUIRE(capture.ReadFrame(frame));
    CHECK(capture.LastLevel() == Catch::Approx(0.9f));
}

TEST_CASE("Voice compresses to something that fits in a datagram", "[audio][voice][codec]")
{
    // A twenty millisecond frame at 48 kHz is 960 floats, which is 3840 bytes: three times what
    // fits in a datagram, for a twentieth of a second of one person talking. Compression is not an
    // optimisation here, it is the difference between voice being possible and not.
    VoiceCodec codec;
    VoiceCodec::Settings settings;
    REQUIRE(codec.Init(settings));
    REQUIRE(codec.FrameSamples() == 960);

    // Something speech-shaped rather than silence: a couple of tones in the range a voice occupies,
    // because an encoder handed nothing at all produces a packet of nothing at all and proves less.
    std::vector<float> frame(codec.FrameSamples());
    for (size_t i = 0; i < frame.size(); ++i)
    {
        const float t = static_cast<float>(i) / 48000.0f;
        frame[i] = 0.35f * std::sin(6.2831853f * 180.0f * t) +
                   0.15f * std::sin(6.2831853f * 900.0f * t);
    }

    std::vector<uint8_t> packet;
    REQUIRE(codec.Encode(frame.data(), frame.size(), packet));
    INFO("960 samples, " << frame.size() * sizeof(float) << " bytes raw, became " << packet.size());
    CHECK(packet.size() > 0);
    CHECK(packet.size() < 200); // comfortably inside a datagram, with the game's own traffic too

    std::vector<float> back;
    REQUIRE(codec.Decode(packet.data(), packet.size(), back));
    CHECK(back.size() == frame.size());

    // Lossy, so the samples do not come back identical and asserting that they do would be wrong.
    // What must survive is the shape: roughly the same energy, and not silence.
    const auto energy = [](const std::vector<float>& samples)
    {
        double sum = 0.0;
        for (const float sample : samples)
        {
            sum += static_cast<double>(sample) * sample;
        }
        return std::sqrt(sum / std::max<size_t>(samples.size(), 1));
    };
    const double before = energy(frame);
    const double after = energy(back);
    INFO("energy " << before << " in, " << after << " out");
    CHECK(after > before * 0.4);
    CHECK(after < before * 2.0);
}

TEST_CASE("A lost voice frame is filled in rather than left as a hole", "[audio][voice][codec]")
{
    // Packets go missing. Handing the decoder silence for a missing one puts a hole in the middle
    // of a word, and a hole is the most noticeable thing a voice connection can do. Opus can invent
    // something plausible from what it has already heard, but only if it is told the frame was lost
    // rather than being given zeroes.
    VoiceCodec codec;
    REQUIRE(codec.Init(VoiceCodec::Settings{}));

    std::vector<float> frame(codec.FrameSamples());
    for (size_t i = 0; i < frame.size(); ++i)
    {
        const float t = static_cast<float>(i) / 48000.0f;
        frame[i] = 0.4f * std::sin(6.2831853f * 220.0f * t);
    }

    // Several frames of speech, so the decoder has something to work from.
    std::vector<uint8_t> packet;
    std::vector<float> back;
    for (int i = 0; i < 5; ++i)
    {
        REQUIRE(codec.Encode(frame.data(), frame.size(), packet));
        REQUIRE(codec.Decode(packet.data(), packet.size(), back));
    }

    // Now one goes missing.
    std::vector<float> filled;
    REQUIRE(codec.Decode(nullptr, 0, filled));
    REQUIRE(filled.size() == frame.size());

    double loudest = 0.0;
    for (const float sample : filled)
    {
        loudest = std::max(loudest, std::abs(static_cast<double>(sample)));
    }
    INFO("the invented frame peaked at " << loudest);
    // Not silence. It does not have to be right, it has to be something.
    CHECK(loudest > 0.01);
}

TEST_CASE("Open mic opens on speech and does not chatter", "[audio][voice][gate]")
{
    // One threshold opens and closes on the same number, so a voice sitting near it flickers on and
    // off, and flicker is worse than either state. This opens at the threshold, stays open until
    // well below it, and then holds on a moment longer so the pause between two words does not cut
    // the channel.
    //
    // Tested without a microphone because the whole of it is a decision about a number.
    VoiceCapture mic;
    constexpr float kThreshold = 0.04f;
    constexpr float kFrame = 0.020f;

    // Silence says nothing.
    CHECK_FALSE(mic.ShouldTransmit(0.001f, kThreshold, kFrame));
    CHECK_FALSE(mic.Transmitting());

    // Speech opens it at once: the first syllable is the one that must not be lost.
    CHECK(mic.ShouldTransmit(0.20f, kThreshold, kFrame));
    CHECK(mic.Transmitting());

    // Just under the threshold keeps it open rather than closing on the quiet part of a word.
    CHECK(mic.ShouldTransmit(kThreshold * 0.8f, kThreshold, kFrame));

    // A short gap between words does not close it.
    for (int i = 0; i < 10; ++i) // 0.2 s
    {
        CHECK(mic.ShouldTransmit(0.0f, kThreshold, kFrame));
    }

    // A long enough silence does.
    bool closed = false;
    for (int i = 0; i < 60 && !closed; ++i) // up to 1.2 s more
    {
        closed = !mic.ShouldTransmit(0.0f, kThreshold, kFrame);
    }
    CHECK(closed);
    CHECK_FALSE(mic.Transmitting());

    // And it opens again for the next thing said.
    CHECK(mic.ShouldTransmit(0.30f, kThreshold, kFrame));
}

TEST_CASE("A voice hovering at the threshold does not flicker", "[audio][voice][gate]")
{
    // The failure this guards against is not silence being sent, it is the channel opening and
    // closing several times a second, which sounds like somebody's connection failing.
    VoiceCapture mic;
    constexpr float kThreshold = 0.05f;
    constexpr float kFrame = 0.020f;

    int changes = 0;
    bool previous = false;
    // Two seconds of somebody speaking quietly, wavering either side of the threshold.
    for (int i = 0; i < 100; ++i)
    {
        const float level = kThreshold * (i % 2 == 0 ? 1.1f : 0.85f);
        const bool now = mic.ShouldTransmit(level, kThreshold, kFrame);
        if (now != previous)
        {
            ++changes;
            previous = now;
        }
    }
    INFO("the channel changed state " << changes << " times in two seconds");
    // Once: open, and stay open.
    CHECK(changes == 1);
    CHECK(previous);
}
