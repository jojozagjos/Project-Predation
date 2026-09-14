#include "Engine/Audio/AudioEngine.h"
#include "Engine/Audio/Sound.h"

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
    CHECK(close == Catch::Approx(0.7071f).margin(0.02));
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
