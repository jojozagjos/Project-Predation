#pragma once

#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace pred
{

struct Environment;
class SiteMap;

// The prints left walking over soft ground -- sand, snow -- at a site: a sole's shape pressed into it. Nothing laid over the
// ground: the ground itself is drawn with those near the eye pressed into it (Environment::prints, fs_mesh), its slopes
// shading the dent and its floor darker -- so seen from the side there is no card's edge to see, and lamps, the sun and the
// snow fall on it as on the ground round it. The oldest go when there are too many, and any of them after a few minutes,
// fading as the wind or the snow fills them in.
class Footprints
{
public:
    // A print at `at` (on the ground under a foot), its toe the way `yaw` faces (radians, about up: 0 along -z), the left
    // foot's or the right's.
    // How long it lasts (seconds): kLast in still air, less in falling snow or blowing sand, which fill it in.
    void Leave(const SiteMap& site, const glm::vec3& at, float yaw, bool left, double now, double last = kLast);
    // Takes away those older than they last -- and all of them, the site gone or another in its place.
    void Update(const SiteMap& site, double now);
    // All of them, for another site.
    void Clear();
    // Those nearest the eye, for the ground to be drawn with (Environment::prints): how plain each still is, fading as it
    // fills in.
    void Gather(Environment& environment, const glm::vec3& eye, double now) const;

    size_t Count() const { return m_prints.size(); }

    static constexpr size_t kMost = 160;
    static constexpr double kLast = 150.0;

private:
    struct Print
    {
        glm::vec3 at{0.0f};
        float yaw = 0.0f;
        bool left = false;
        double made = 0.0;
        double last = kLast;
    };
    std::vector<Print> m_prints;
    uint32_t m_site = 0;
};

} // namespace pred
