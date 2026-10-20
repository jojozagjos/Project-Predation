#include "Game/World/Footprints.h"

#include "Engine/Scene/Scene.h"
#include "Game/World/SiteMap.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

void Footprints::Leave(const SiteMap& site, const glm::vec3& at, float yaw, bool left, double now, double last)
{
    if (site.Seed() != m_site)
    {
        Clear();
        m_site = site.Seed();
    }
    // The oldest goes for it when there are too many.
    if (m_prints.size() >= kMost)
    {
        m_prints.erase(m_prints.begin());
    }
    // A foot's width to the side of where the body went, on the ground there.
    const glm::vec3 right{std::cos(yaw), 0.0f, std::sin(yaw)};
    Print print;
    print.at = at + right * (left ? -0.09f : 0.09f);
    print.at.y = site.Terrain().Height(print.at.x, print.at.z);
    print.yaw = yaw;
    print.left = left;
    print.made = now;
    print.last = last;
    m_prints.push_back(print);
}

void Footprints::Update(const SiteMap& site, double now)
{
    if (!m_prints.empty() && (!site.Built() || site.Seed() != m_site))
    {
        Clear();
        return;
    }
    m_prints.erase(std::remove_if(m_prints.begin(), m_prints.end(), [now](const Print& print) { return now - print.made > print.last; }),
                   m_prints.end());
}

void Footprints::Clear()
{
    m_prints.clear();
}

void Footprints::Gather(Environment& environment, const glm::vec3& eye, double now) const
{
    environment.printCount = 0;
    // Past this the dent is a pixel or two, and the shader lets it go.
    constexpr float kSeen = 30.0f;
    std::vector<std::pair<float, const Print*>> near;
    near.reserve(m_prints.size());
    for (const Print& print : m_prints)
    {
        const glm::vec3 apart = print.at - eye;
        const float distance = glm::dot(apart, apart);
        if (distance < kSeen * kSeen && now - print.made <= print.last)
        {
            near.emplace_back(distance, &print);
        }
    }
    const size_t count = std::min(near.size(), static_cast<size_t>(Environment::kPrints));
    std::partial_sort(near.begin(), near.begin() + static_cast<std::ptrdiff_t>(count), near.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 0; i < count; ++i)
    {
        const Print& print = *near[i].second;
        // Plain at first, filling in over the last third of the time it lasts.
        const float age = static_cast<float>((now - print.made) / std::max(print.last, 1.0));
        const float plain = 1.0f - glm::smoothstep(0.66f, 1.0f, age);
        environment.prints[i * 2] = glm::vec4(print.at, plain);
        environment.prints[i * 2 + 1] = glm::vec4(std::sin(print.yaw), -std::cos(print.yaw), print.left ? 1.0f : -1.0f, 0.0f);
    }
    environment.printCount = static_cast<int>(count);
}

} // namespace pred
