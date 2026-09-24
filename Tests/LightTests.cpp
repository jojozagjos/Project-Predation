#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Game/World/LevelLights.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace pred;

TEST_CASE("A lamp's mood is the same on every machine at the same moment", "[lights]")
{
    for (const LightMood mood : {LightMood::Steady, LightMood::Flicker, LightMood::Failing, LightMood::Dead,
                                 LightMood::Pulse})
    {
        for (float t = 0.0f; t < 30.0f; t += 0.37f)
        {
            const float a = LevelLights::MoodLevel(mood, 1234u, t);
            const float b = LevelLights::MoodLevel(mood, 1234u, t);
            INFO(LightMoodName(mood) << " at " << t);
            REQUIRE(a == b);
            REQUIRE(a >= 0.0f);
            REQUIRE(a <= 1.0f);
        }
    }
}

TEST_CASE("Each mood behaves as its name says", "[lights]")
{
    // Sampled over two minutes at sixty a second.
    const auto sample = [](LightMood mood, uint32_t seed)
    {
        std::vector<float> levels;
        for (int i = 0; i < 60 * 120; ++i)
        {
            levels.push_back(LevelLights::MoodLevel(mood, seed, static_cast<float>(i) / 60.0f));
        }
        return levels;
    };
    const auto fractionBelow = [](const std::vector<float>& levels, float line)
    {
        int count = 0;
        for (const float level : levels)
        {
            count += level < line ? 1 : 0;
        }
        return static_cast<float>(count) / static_cast<float>(levels.size());
    };

    // Steady is always on, dead always off.
    CHECK(fractionBelow(sample(LightMood::Steady, 1u), 1.0f) == 0.0f);
    CHECK(fractionBelow(sample(LightMood::Dead, 1u), 0.001f) == 1.0f);
    // A flickering lamp is mostly on, and does dip.
    const std::vector<float> flicker = sample(LightMood::Flicker, 7u);
    CHECK(fractionBelow(flicker, 0.6f) > 0.01f);
    CHECK(fractionBelow(flicker, 0.6f) < 0.12f);
    // A failing one is mostly off, and does come on.
    const std::vector<float> failing = sample(LightMood::Failing, 7u);
    CHECK(fractionBelow(failing, 0.05f) > 0.6f);
    CHECK(fractionBelow(failing, 0.05f) < 0.97f);
    // A pulse never goes out and never stays still.
    const std::vector<float> pulse = sample(LightMood::Pulse, 7u);
    CHECK(fractionBelow(pulse, 0.25f) == 0.0f);
    CHECK(fractionBelow(pulse, 0.6f) > 0.2f);
}

TEST_CASE("A circuit without power puts its lamps out, and emergency lamps stay lit", "[lights]")
{
    Scene scene;
    MeshLibrary meshes;
    meshes.SetHeadless(true);
    LevelLights lights;
    lights.Add(scene, meshes, LightKind::Ceiling, LightMood::Steady, {0.0f, 3.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 1);
    lights.Add(scene, meshes, LightKind::Wall, LightMood::Steady, {4.0f, 2.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 1);
    lights.Add(scene, meshes, LightKind::Emergency, LightMood::Steady, {8.0f, 2.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, 1);
    lights.Add(scene, meshes, LightKind::Ceiling, LightMood::Dead, {0.0f, 3.0f, 8.0f}, {0.0f, -1.0f, 0.0f}, 2);

    std::vector<PunctualLight> lit;
    lights.Update(scene, 1.0f);
    lights.Gather(lit);
    CHECK(lit.size() == 3); // the dead one gives no light

    lights.SetPowered(1, false);
    CHECK_FALSE(lights.Powered(1));
    lights.Update(scene, 2.0f);
    lit.clear();
    lights.Gather(lit);
    REQUIRE(lit.size() == 1);
    CHECK(lit.front().color.r > lit.front().color.g * 3.0f); // what is left is the red one

    // And a dark lamp's fitting is dark: nothing glowing where no light comes from.
    const MeshRenderer* ceiling = scene.GetMeshRenderer(lights.Lights()[0].fitting);
    REQUIRE(ceiling != nullptr);
    CHECK(ceiling->material.emissive.r == 0.0f);

    lights.SetPowered(1, true);
    lights.Update(scene, 3.0f);
    lit.clear();
    lights.Gather(lit);
    CHECK(lit.size() == 3);
}
