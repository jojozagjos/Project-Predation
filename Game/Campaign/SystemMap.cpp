#include "Game/Campaign/SystemMap.h"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace pred
{

namespace
{

constexpr float kTau = 6.28318530718f;
// The map's units for a distance from the star: this times its square root, in astronomical units.
constexpr float kSpread = 7.0f;

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
        // One after another outwards, well clear of the planet and of its rings as drawn.
        const float clear = std::max(1.8f, system.bodies[static_cast<size_t>(body.parent)].rings.y + 0.45f);
        drawn[body.index].at = planet.at + way * (planet.radius * clear + 0.35f + 0.38f * static_cast<float>(nth));
    }
    return drawn;
}

glm::vec3 SystemMapView::ShipAt(const StarSystem& system, const std::vector<Drawn>& drawn, double clock, const glm::vec3& au)
{
    // How far each moon was moved to be drawn, weighed by how near the ship is to it against how far out its orbit is -- and
    // the planet it goes round, which was not moved, weighed the same: so near a moon the ship goes where the moon is drawn,
    // near its planet where the planet is, and between them, between; far from both, nowhere it is not.
    glm::vec3 moved{0.0f};
    float weights = 0.05f;
    for (const Body& body : system.bodies)
    {
        if (body.kind != BodyKind::Moon || body.parent < 0 || body.index >= static_cast<int>(drawn.size()))
        {
            continue;
        }
        const glm::vec3 moon = system.Position(body.index, clock);
        const glm::vec3 planet = system.Position(body.parent, clock);
        const float reach = std::max(glm::length(moon - planet), 1.0e-6f);
        const auto weight = [&](const glm::vec3& at)
        {
            const float apart = glm::length(au - at) / reach;
            return std::exp(-2.0f * apart * apart);
        };
        const float toMoon = weight(moon);
        moved += (drawn[static_cast<size_t>(body.index)].at - Place(moon)) * toMoon;
        weights += toMoon + weight(planet);
    }
    glm::vec3 at = Place(au) + moved / weights;
    // Outside every disc: the drawn bodies are far larger than life.
    for (const Drawn& body : drawn)
    {
        const float clear = body.radius * 1.35f;
        const glm::vec3 from = at - body.at;
        if (body.index < 0 || glm::length(from) >= clear)
        {
            continue;
        }
        glm::vec3 way = au - system.Position(body.index, clock);
        way = glm::length(from) > 1.0e-4f ? from : glm::length(way) > 1.0e-9f ? way : glm::vec3(0.0f, 1.0f, 0.0f);
        at = body.at + glm::normalize(way) * clear;
    }
    return at;
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
    m_distanceWanted = std::clamp(distance, m_nearest, m_furthest);
}

void SystemMapView::SetWanted(const Wanted& wanted)
{
    m_focusWanted = wanted.focus;
    m_distanceWanted = std::clamp(wanted.distance, m_nearest, m_furthest);
    // The nearest way round to it.
    m_yawWanted = m_yaw + std::remainder(wanted.yaw - m_yaw, kTau);
    m_pitchWanted = std::clamp(wanted.pitch, -1.45f, 1.45f);
}

void SystemMapView::Jump(const glm::vec3& at, float distance)
{
    Focus(at, distance);
    m_focus = m_focusWanted;
    m_distance = m_distanceWanted;
}

void SystemMapView::Pan(float right, float up)
{
    m_focusWanted += (Right() * right + Up() * up) * m_distance;
}

void SystemMapView::Slide(float right, float away)
{
    const glm::vec3 flatRight{std::cos(m_yaw), 0.0f, -std::sin(m_yaw)};
    const glm::vec3 flatAway{-std::sin(m_yaw), 0.0f, -std::cos(m_yaw)};
    const glm::vec3 move = flatRight * right + flatAway * away;
    m_focus += move;
    m_focusWanted += move;
}

void SystemMapView::FaceFrom(const glm::vec3& direction)
{
    if (glm::length(direction) < 1.0e-6f)
    {
        return;
    }
    const glm::vec3 way = glm::normalize(direction);
    m_pitchWanted = std::clamp(std::asin(std::clamp(way.y, -1.0f, 1.0f)), -1.45f, 1.45f);
    // The nearest way round to it.
    const float yaw = std::atan2(way.x, way.z);
    m_yawWanted = m_yaw + std::remainder(yaw - m_yaw, kTau);
}

float SystemMapView::PixelSize(float height) const
{
    return 2.0f * m_distance * std::tan(kFieldOfView * 0.5f) / std::max(height, 1.0f);
}

void SystemMapView::SetLimits(float nearest, float furthest)
{
    m_nearest = std::max(nearest, 0.01f);
    m_furthest = std::max(furthest, m_nearest);
    m_distanceWanted = std::clamp(m_distanceWanted, m_nearest, m_furthest);
}

int SystemMapView::TakePastLimit()
{
    return std::exchange(m_pastLimit, 0);
}

void SystemMapView::Turn(float yaw, float pitch)
{
    m_yaw += yaw;
    m_pitch = std::clamp(m_pitch + pitch, -1.45f, 1.45f);
    m_yawWanted = m_yaw;
    m_pitchWanted = m_pitch;
}

void SystemMapView::Zoom(float factor)
{
    const float wanted = m_distanceWanted * factor;
    // Pushing on past a limit, already at it, is the way to the next scale of map.
    if (wanted < m_nearest && m_distanceWanted <= m_nearest * 1.02f)
    {
        m_pastLimit = -1;
    }
    else if (wanted > m_furthest && m_distanceWanted >= m_furthest * 0.98f)
    {
        m_pastLimit = 1;
    }
    m_distanceWanted = std::clamp(wanted, m_nearest, m_furthest);
}

void SystemMapView::Update(float dt)
{
    const float ease = 1.0f - std::exp(-dt * 6.0f);
    m_focus += (m_focusWanted - m_focus) * ease;
    m_distance += (m_distanceWanted - m_distance) * ease;
    m_yaw += (m_yawWanted - m_yaw) * ease;
    m_pitch += (m_pitchWanted - m_pitch) * ease;
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
    const float nearPlane = std::max(m_distance * 0.01f, 0.005f);
    const float farPlane = std::max(4000.0f, m_distance * 20.0f);
    return homogeneousDepth ? glm::perspectiveRH_NO(kFieldOfView, aspect, nearPlane, farPlane) : glm::perspectiveRH_ZO(kFieldOfView, aspect, nearPlane, farPlane);
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
        const float across = body.radius / std::max(depth * std::tan(kFieldOfView * 0.5f), 1.0e-4f) * 0.5f;
        // A small one can be clicked from a little way off: never less than about twenty pixels on a picture 900 high.
        const float reach = std::max(across * 1.3f, 0.024f);
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
