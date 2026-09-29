#pragma once

#include "Engine/Scene/Scene.h"

#include <functional>
#include <vector>

#include <glm/vec3.hpp>

namespace pred
{

class MeshLibrary;

// Snow falling round whoever is looking: a box of flakes that goes with the eye, each coming round again at the other
// side as it leaves -- so it seems to fill the sky wherever anybody goes -- drifting with the wind and swaying as it
// falls, and not indoors.
class Snowfall
{
public:
    // `wind`: metres a second of drift, sideways; `ground`: how high the ground is under the eye, for what blows along it.
    void Update(Scene& scene, MeshLibrary& meshes, const glm::vec3& eye, float ground, float dt, bool falling, const glm::vec3& wind,
                const std::function<bool(const glm::vec3&)>& indoors);
    void Clear(Scene& scene);

private:
    struct Flake
    {
        Entity entity;
        glm::vec3 at{0.0f};
        float fall = 1.0f;
        float sway = 0.0f;
        float size = 0.03f;
        glm::vec3 spin{0.0f, 1.0f, 0.0f};
        float spinRate = 2.0f;
        float height = 0.2f; // spindrift: how far over the ground
        bool drift = false;
        bool shown = false;
    };
    std::vector<Flake> m_flakes;
    float m_clock = 0.0f;
    uint32_t m_seed = 0x5A0Fu;
    bool m_falling = false;
};

} // namespace pred
