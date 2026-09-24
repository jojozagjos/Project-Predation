#include "Engine/Audio/SoundDesign.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace pred;

namespace
{

std::vector<SoundPatch> TheLibrary()
{
    const std::filesystem::path path = std::filesystem::path(PRED_SOURCE_DIR) / "Assets" / "Data" / "sound_design.json";
    std::ifstream file(path);
    REQUIRE(file);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::string> problems;
    std::vector<SoundPatch> patches = LoadSoundPatches(text, &problems);
    INFO((problems.empty() ? std::string() : problems.front()));
    CHECK(problems.empty());
    return patches;
}

// How loud, on average, a stretch is: a sound that is all click and no body is almost all silence.
float Rms(const std::vector<float>& samples, size_t from, size_t count)
{
    double sum = 0.0;
    const size_t to = std::min(samples.size(), from + count);
    for (size_t i = from; i < to; ++i)
    {
        sum += static_cast<double>(samples[i]) * samples[i];
    }
    return to > from ? static_cast<float>(std::sqrt(sum / static_cast<double>(to - from))) : 0.0f;
}

} // namespace

TEST_CASE("Every designed sound renders to something usable", "[audio][design]")
{
    const std::vector<SoundPatch> patches = TheLibrary();
    REQUIRE(patches.size() >= 50);
    for (const SoundPatch& patch : patches)
    {
        for (int take = 0; take < patch.variants; ++take)
        {
            const SoundData data = RenderPatch(patch, take, 48000);
            INFO(patch.name << " take " << take + 1);
            REQUIRE(!data.samples.empty());
            CHECK(data.Seconds() == Catch::Approx(patch.seconds).margin(0.02));
            bool finite = true;
            float peak = 0.0f;
            for (const float sample : data.samples)
            {
                finite = finite && std::isfinite(sample);
                peak = std::max(peak, std::abs(sample));
            }
            CHECK(finite);
            // Brought to its level, and never past full scale.
            CHECK(peak == Catch::Approx(patch.gain).margin(0.01));
            CHECK(peak <= 1.0f);
            // Something there: not one sample and silence.
            CHECK(Rms(data.samples, 0, data.samples.size()) > patch.gain * 0.01f);
        }
    }
}

TEST_CASE("A take is the same every time, and different from the next take", "[audio][design]")
{
    const std::vector<SoundPatch> patches = TheLibrary();
    for (const SoundPatch& patch : patches)
    {
        if (patch.variants < 2)
        {
            continue;
        }
        const SoundData a = RenderPatch(patch, 0, 48000);
        const SoundData b = RenderPatch(patch, 0, 48000);
        const SoundData c = RenderPatch(patch, 1, 48000);
        INFO(patch.name);
        const bool repeatable = a.samples == b.samples;
        const bool varied = a.samples != c.samples;
        CHECK(repeatable);
        CHECK(varied);
    }
}

TEST_CASE("A looping sound runs back into its own start without a step", "[audio][design]")
{
    const std::vector<SoundPatch> patches = TheLibrary();
    int loops = 0;
    for (const SoundPatch& patch : patches)
    {
        if (!patch.loop)
        {
            continue;
        }
        ++loops;
        const SoundData data = RenderPatch(patch, 0, 48000);
        INFO(patch.name);
        // The last sample and the first are neighbours when it loops: the jump between them has to be
        // no bigger than the jumps the sound makes between any two of its own samples.
        float biggestStep = 0.0f;
        for (size_t i = 1; i < data.samples.size(); ++i)
        {
            biggestStep = std::max(biggestStep, std::abs(data.samples[i] - data.samples[i - 1]));
        }
        const float seam = std::abs(data.samples.front() - data.samples.back());
        CHECK(seam <= biggestStep * 1.5f + 1e-4f);
        // And it is as loud at its start as through its middle: it does not fade up each time round.
        const size_t tenth = data.samples.size() / 10;
        CHECK(Rms(data.samples, 0, tenth) > Rms(data.samples, tenth * 4, tenth) * 0.5f);
    }
    CHECK(loops >= 3);
}
