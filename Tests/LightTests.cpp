#include "Engine/Render/Mesh.h"
#include "Engine/Render/SceneRenderer.h"
#include "Engine/Scene/Scene.h"
#include "Game/World/LevelLights.h"

#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
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

TEST_CASE("A lamp is put in the cells of the view its light reaches, and in none it does not", "[light][cluster]")
{
    // Looking down -z from the origin, with a 90 degree view.
    const glm::mat4 view = glm::lookAtRH(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(90.0f), 16.0f / 9.0f, 0.1f, 300.0f);
    constexpr int kAcross = SceneRenderer::kClusterAcross;
    constexpr int kUp = SceneRenderer::kClusterUp;
    const auto column = [&](int cell) { return cell % kAcross; };
    const auto row = [&](int cell) { return (cell / kAcross) % kUp; };
    const auto slice = [&](int cell) { return cell / (kAcross * kUp); };
    std::vector<int> cells;

    // A small lamp dead ahead, ten metres off: the middle cells, and the slices round ten metres.
    SceneRenderer::ClusterCells(view, projection, {0.0f, 0.0f, -10.0f}, 0.5f, false, {}, {}, cells);
    REQUIRE_FALSE(cells.empty());
    const float perLog = static_cast<float>(SceneRenderer::kClusterSlices) / std::log(SceneRenderer::kClusterFar / SceneRenderer::kClusterNear);
    const int tenMetres = static_cast<int>(std::floor(std::log(10.0f / SceneRenderer::kClusterNear) * perLog));
    for (const int cell : cells)
    {
        CHECK(column(cell) >= kAcross / 2 - 1);
        CHECK(column(cell) <= kAcross / 2);
        CHECK(std::abs(slice(cell) - tenMetres) <= 1);
    }

    // Off to the right: the right-hand cells only.
    SceneRenderer::ClusterCells(view, projection, {6.5f, 0.0f, -8.0f}, 0.5f, false, {}, {}, cells);
    REQUIRE_FALSE(cells.empty());
    for (const int cell : cells)
    {
        CHECK(column(cell) > kAcross / 2);
    }

    // Wholly behind the camera: nowhere.
    SceneRenderer::ClusterCells(view, projection, {0.0f, 0.0f, 6.0f}, 1.0f, false, {}, {}, cells);
    CHECK(cells.empty());

    // Round the camera itself: the nearest slice everywhere across and up.
    SceneRenderer::ClusterCells(view, projection, glm::vec3(0.0f), 3.0f, false, {}, {}, cells);
    int nearest = 0;
    for (const int cell : cells)
    {
        nearest += slice(cell) == 0 ? 1 : 0;
    }
    CHECK(nearest == kAcross * kUp);

    // Beside the camera, a lamp reaches the near cells on its own side and not the far side of the picture:
    // round, not a box. This is what used to put a lamp in every cell there was.
    SceneRenderer::ClusterCells(view, projection, {4.0f, 0.0f, -1.0f}, 4.5f, false, {}, {}, cells);
    REQUIRE_FALSE(cells.empty());
    int farLeft = 0;
    for (const int cell : cells)
    {
        farLeft += (column(cell) == 0 && slice(cell) > 12) ? 1 : 0;
    }
    CHECK(farLeft == 0);
    CHECK(cells.size() < static_cast<size_t>(kAcross * kUp * SceneRenderer::kClusterSlices / 2));

    // Kept in a box that ends before the view: nowhere.
    SceneRenderer::ClusterCells(view, projection, {0.0f, 0.0f, -5.0f}, 5.0f, true, {-1.0f, -1.0f, 1.0f}, {1.0f, 1.0f, 3.0f}, cells);
    CHECK(cells.empty());
}
