#pragma once

#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Entity.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace pred
{

class MeshLibrary;
class Scene;
class SiteMap;

// The prints left walking over soft ground -- sand, snow, mud -- at a site: a sole's shape pressed into it, the ground's
// own surface and colour over it, darker where it is pressed down and shaded by the dent's slopes (a few millimetres over
// the ground, as a bullet hole is: sunk into it, the ground would hide it). The oldest go when there are too many, and
// any of them after a few minutes, as the wind or the snow fills them in.
class Footprints
{
public:
    // A print at `at` (on the ground under a foot), its toe the way `yaw` faces (radians, about up: 0 along -z), the left
    // foot's or the right's.
    void Leave(Scene& scene, MeshLibrary& meshes, const SiteMap& site, const glm::vec3& at, float yaw, bool left, double now);
    // Takes away those older than they last -- and all of them, the site gone or another in its place.
    void Update(Scene& scene, const SiteMap& site, double now);
    // All of them, for another site.
    void Clear(Scene& scene);

    static constexpr size_t kMost = 160;
    static constexpr double kLast = 150.0;

private:
    struct Print
    {
        Entity entity;
        MeshHandle mesh;
        double made = 0.0;
        bool shown = false;
    };
    std::vector<Print> m_prints;
    size_t m_next = 0;
    uint32_t m_site = 0;
};

} // namespace pred
