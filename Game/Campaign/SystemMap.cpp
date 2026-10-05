#include "Game/Campaign/SystemMap.h"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

namespace
{

constexpr float kTau = 6.28318530718f;
// The map's units for a distance from the star: this times its square root, in astronomical units.
constexpr float kSpread = 7.0f;
constexpr float kFov = 0.75f; // radians, up and down

} // namespace

glm::vec3 SystemMapView::Place(const glm::vec3& au)
{
    const float distance = glm::length(au);
    if (distance < 1.0e-6f)
    {
        return glm::vec3(0.0f);
    }
    return au / distance * (kSpread * std::sqrt(distance) + kStarRadius);
}

float SystemMapView::DrawnRadius(const Body& body)
{
    const float scale = body.kind == BodyKind::Moon ? 0.16f : 0.28f;
    return std::clamp(scale * std::sqrt(std::max(body.radius, 0.05f)) + 0.06f, 0.08f, 1.1f);
}

std::vector<SystemMapView::Drawn> SystemMapView::Layout(const StarSystem& system, double clock)
{
    std::vector<Drawn> drawn(system.bodies.size());
    // Planets first, where they are; then each moon round its planet, by where it is in its orbit, well clear of it.
    std::vector<int> moonsSoFar(system.bodies.size(), 0);
    for (const Body& body : system.bodies)
    {
        Drawn& out = drawn[body.index];
        out.index = body.index;
        out.radius = DrawnRadius(body);
        if (body.kind == BodyKind::Planet)
        {
            out.at = Place(system.Position(body.index, clock));
        }
    }
    for (const Body& body : system.bodies)
    {
        if (body.kind != BodyKind::Moon || body.parent < 0)
        {
            continue;
        }
        const Drawn& planet = drawn[static_cast<size_t>(body.parent)];
        const int nth = moonsSoFar[static_cast<size_t>(body.parent)]++;
        const glm::vec3 offset = system.Position(body.index, clock) - system.Position(body.parent, clock);
        const float length = glm::length(offset);
        const glm::vec3 way = length > 1.0e-9f ? offset / length : glm::vec3(1.0f, 0.0f, 0.0f);
        drawn[body.index].at = planet.at + way * (planet.radius * 1.8f + 0.35f + 0.38f * static_cast<float>(nth));
    }
    return drawn;
}

glm::vec3 SystemMapView::ShipAt(const std::vector<Drawn>& drawn, int body, const glm::vec3& au)
{
    if (body >= 0 && body < static_cast<int>(drawn.size()))
    {
        // Just off its sunward side, so it reads as being at that body rather than inside it.
        const Drawn& at = drawn[static_cast<size_t>(body)];
        const glm::vec3 sunward = glm::length(at.at) > 1.0e-4f ? -glm::normalize(at.at) : glm::vec3(1.0f, 0.0f, 0.0f);
        return at.at + (sunward + glm::vec3(0.0f, 0.6f, 0.0f)) * (at.radius + 0.25f);
    }
    return Place(au);
}

std::vector<glm::vec3> SystemMapView::Orbit(const StarSystem& system, int body, const std::vector<Drawn>& drawn, double clock, int points)
{
    std::vector<glm::vec3> loop;
    const Body* found = system.Find(body);
    if (found == nullptr || points < 3)
    {
        return loop;
    }
    if (found->kind == BodyKind::Planet)
    {
        for (int i = 0; i <= points; ++i)
        {
            // The same orbit, the clock run round it.
            const double at = clock + static_cast<double>(found->period) * static_cast<double>(i) / static_cast<double>(points);
            loop.push_back(Place(system.Position(body, at)));
        }
        return loop;
    }
    // A moon: a circle round where its planet is drawn, through where it is drawn.
    const glm::vec3 centre = drawn[static_cast<size_t>(found->parent)].at;
    const float reach = glm::length(drawn[static_cast<size_t>(body)].at - centre);
    for (int i = 0; i <= points; ++i)
    {
        const float angle = kTau * static_cast<float>(i) / static_cast<float>(points);
        loop.push_back(centre + glm::vec3(std::cos(angle), 0.0f, std::sin(angle)) * reach);
    }
    return loop;
}

void SystemMapView::Focus(const glm::vec3& at, float distance)
{
    m_focusWanted = at;
    m_distanceWanted = std::clamp(distance, 2.0f, 400.0f);
}

void SystemMapView::Turn(float yaw, float pitch)
{
    m_yaw += yaw;
    m_pitch = std::clamp(m_pitch + pitch, -1.45f, 1.45f);
}

void SystemMapView::Zoom(float factor)
{
    m_distanceWanted = std::clamp(m_distanceWanted * factor, 2.0f, 400.0f);
}

void SystemMapView::Update(float dt)
{
    const float ease = 1.0f - std::exp(-dt * 6.0f);
    m_focus += (m_focusWanted - m_focus) * ease;
    m_distance += (m_distanceWanted - m_distance) * ease;
}

glm::vec3 SystemMapView::Eye() const
{
    const glm::vec3 back{std::cos(m_pitch) * std::sin(m_yaw), std::sin(m_pitch), std::cos(m_pitch) * std::cos(m_yaw)};
    return m_focus + back * m_distance;
}

glm::mat4 SystemMapView::View() const
{
    return glm::lookAtRH(Eye(), m_focus, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 SystemMapView::Projection(float aspect, bool homogeneousDepth) const
{
    const float nearPlane = std::max(m_distance * 0.01f, 0.02f);
    const float farPlane = 2000.0f;
    return homogeneousDepth ? glm::perspectiveRH_NO(kFov, aspect, nearPlane, farPlane) : glm::perspectiveRH_ZO(kFov, aspect, nearPlane, farPlane);
}

glm::vec3 SystemMapView::Right() const
{
    const glm::mat4 view = View();
    return {view[0][0], view[1][0], view[2][0]};
}

glm::vec3 SystemMapView::Up() const
{
    const glm::mat4 view = View();
    return {view[0][1], view[1][1], view[2][1]};
}

bool SystemMapView::OnScreen(const glm::vec3& at, float aspect, bool homogeneousDepth, glm::vec2& out) const
{
    const glm::vec4 clip = Projection(aspect, homogeneousDepth) * View() * glm::vec4(at, 1.0f);
    if (clip.w <= 1.0e-4f)
    {
        return false;
    }
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    out = {ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f};
    return true;
}

int SystemMapView::Pick(const glm::vec2& point, const std::vector<Drawn>& drawn, float aspect, bool homogeneousDepth) const
{
    int best = -1;
    float bestDepth = 1.0e9f;
    const glm::vec3 eye = Eye();
    for (const Drawn& body : drawn)
    {
        glm::vec2 centre;
        if (body.index < 0 || !OnScreen(body.at, aspect, homogeneousDepth, centre))
        {
            continue;
        }
        // How big it is on the picture, with a little to spare so a small one can be clicked.
        const float depth = glm::length(body.at - eye);
        const float across = body.radius / std::max(depth * std::tan(kFov * 0.5f), 1.0e-4f) * 0.5f;
        const float reach = std::max(across, 0.012f) * 1.4f;
        glm::vec2 off = point - centre;
        off.x *= aspect;
        if (glm::length(off) <= reach && depth < bestDepth)
        {
            best = body.index;
            bestDepth = depth;
        }
    }
    return best;
}

} // namespace pred
