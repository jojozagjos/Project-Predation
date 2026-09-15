#include "Engine/Render/ShadowMap.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

using namespace pred;

namespace
{
// Where a world point lands in a fitted map, in texels.
glm::vec2 TexelOf(const ShadowFit& fit, const glm::vec3& world, float resolution)
{
    const glm::vec4 clip = fit.projection * fit.view * glm::vec4(world, 1.0f);
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    return (ndc * 0.5f + 0.5f) * resolution;
}
} // namespace

TEST_CASE("A shadow map moves in whole texels as the player walks", "[render][shadow]")
{
    // The one thing about a shadow map that cannot be seen in a still picture.
    //
    // The map is fitted around the player, so it moves when they do. If it moves smoothly, the grid
    // it samples the world on slides under every surface, and the filtered value for a fixed point
    // in the world changes a little every frame: shadow edges crawl and every partially shaded patch
    // shimmers. It has to move in whole texels instead, so a given point keeps landing on the same
    // texel until the map jumps a whole one.
    //
    // This is written as a test because the bug it guards against is silent. The first version
    // looked exactly like this one and did nothing at all: it built the view from the centre and
    // then asked where the centre landed in it, which is the view-space origin by construction,
    // every time. The rounding had nothing to round. Screenshots could not show it and it was
    // reported as flickering four separate times before anybody went looking here.
    constexpr uint16_t kResolution = 1024;
    constexpr float kRadius = 20.0f;
    constexpr float kDepth = 220.0f;
    const glm::vec3 sun = glm::normalize(glm::vec3(-0.35f, -0.85f, -0.4f));
    const float texel = (kRadius * 2.0f) / static_cast<float>(kResolution);

    // A fixed thing in the world, and a player creeping past it in steps far smaller than a texel.
    const glm::vec3 landmark{3.0f, 1.0f, -2.0f};
    const ShadowFit first = FitShadowMap({0.0f, 1.7f, 0.0f}, sun, kRadius, kDepth, kResolution, false);
    const glm::vec2 firstTexel = TexelOf(first, landmark, kResolution);

    int moves = 0;
    for (int step = 1; step <= 8; ++step)
    {
        const glm::vec3 centre{static_cast<float>(step) * texel * 0.1f, 1.7f, 0.0f};
        const ShadowFit fit = FitShadowMap(centre, sun, kRadius, kDepth, kResolution, false);
        const glm::vec2 now = TexelOf(fit, landmark, kResolution);
        const float drift = glm::length(now - firstTexel);
        INFO("after " << step << " tenths of a texel the landmark moved " << drift << " texels");
        // Either it has not moved at all, or the map has jumped a whole texel and it has moved by
        // one. What it must never do is drift by a tenth of a texel at a time.
        CHECK((drift < 0.01f || std::abs(drift - std::round(drift)) < 0.01f));
        if (drift > 0.01f)
        {
            ++moves;
        }
    }

    // And it does eventually move, or "never drifts" would be satisfied by a map that ignores the
    // player altogether.
    const ShadowFit far = FitShadowMap({40.0f, 1.7f, 0.0f}, sun, kRadius, kDepth, kResolution, false);
    CHECK(glm::length(TexelOf(far, landmark, kResolution) - firstTexel) > 1.0f);
    INFO("the map jumped on " << moves << " of the eight sub-texel steps");
}

TEST_CASE("A shadow map still covers what it was aimed at", "[render][shadow]")
{
    // Snapping moves the map by up to half a texel, which must not move it off the thing it was
    // fitted around.
    constexpr uint16_t kResolution = 1024;
    constexpr float kRadius = 20.0f;
    const glm::vec3 sun = glm::normalize(glm::vec3(-0.35f, -0.85f, -0.4f));
    const glm::vec3 centre{12.0f, 1.7f, -7.0f};
    const ShadowFit fit = FitShadowMap(centre, sun, kRadius, 220.0f, kResolution, false);

    const glm::vec2 middle = TexelOf(fit, centre, kResolution);
    INFO("the centre landed at texel " << middle.x << ", " << middle.y << " of " << kResolution);
    // Within half a texel of the middle of the map.
    CHECK(std::abs(middle.x - kResolution * 0.5f) < 1.0f);
    CHECK(std::abs(middle.y - kResolution * 0.5f) < 1.0f);
}
