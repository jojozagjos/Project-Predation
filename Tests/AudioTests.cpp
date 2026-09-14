#include "Engine/Audio/AudioEngine.h"
#include "Engine/Audio/Sound.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
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
