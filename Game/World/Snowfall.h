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
    // `wind`: metres a second of drift, sideways.
    void Update(Scene& scene, MeshLibrary& meshes, const glm::vec3& eye, float dt, bool falling, const glm::vec3& wind,
                const std::function<bool(const glm::vec3&)>& indoors);
    void Clear(Scene& scene);

private:
    struct Flake
    {
        Entity entity;
        glm::vec3 at{0.0f};
        float fall = 1.0f;
        float sway = 0.0f;
        bool shown = false;
    };
    std::vector<Flake> m_flakes;
    float m_clock = 0.0f;
    uint32_t m_seed = 0x5A0Fu;
    bool m_falling = false;
};

} // namespace pred
