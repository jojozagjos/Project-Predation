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
// The map's units for a distance from the star: this times its square root, in astronomical units -- before any planet
// is moved out to make room for the one inside it.
constexpr float kSpread = 7.0f;
// Room left between a planet's moons, and between one planet and its moons and the next.
constexpr float kMoonGap = 0.16f;
constexpr float kPlanetGap = 0.5f;

float Spread(float au)
{
    return kSpread * std::sqrt(au) + SystemMapView::kStarRadius;
}

// A planet's moons drawn one after another outwards from it, innermost first, clear of it, its rings and each other: how
// far out from the planet each is drawn (into `out`, by body index, if given), and how far out from it the furthest of
// them -- or its rings, or itself -- reaches.
float MoonsOut(const StarSystem& system, const Body& planet, std::vector<float>* out)
{
    const float radius = SystemMapView::DrawnRadius(planet);
    std::vector<const Body*> moons;
    for (const Body& body : system.bodies)
    {
        if (body.kind == BodyKind::Moon && body.parent == planet.index)
        {
            moons.push_back(&body);
        }
    }
    std::sort(moons.begin(), moons.end(), [](const Body* a, const Body* b) { return a->orbit < b->orbit; });
    float reach = radius * std::max(1.0f, planet.rings.y);
    float edge = radius * std::max(1.8f, planet.rings.y + 0.45f) + 0.2f;
    for (const Body* moon : moons)
    {
        const float own = SystemMapView::DrawnRadius(*moon);
        const float at = edge + own;
        if (out != nullptr)
        {
            (*out)[static_cast<size_t>(moon->index)] = at;
        }
        edge = at + own + kMoonGap;
        reach = at + own;
    }
    return reach;
}

// Where each planet's orbit is drawn, innermost first: as far out as its distance says, or further, so that it and its
// moons are clear of the star, and of the planet inside it and that one's moons. x = its distance from the star (astronomical
// units), y = drawn there by Spread, z = drawn there in fact.
std::vector<glm::vec3> Stops(const StarSystem& system)
{
    std::vector<glm::vec2> planets; // orbit, reach
    for (const Body& body : system.bodies)
    {
        if (body.kind == BodyKind::Planet)
        {
            planets.push_back({body.orbit, MoonsOut(system, body, nullptr)});
        }
    }
    std::sort(planets.begin(), planets.end(), [](const glm::vec2& a, const glm::vec2& b) { return a.x < b.x; });
    std::vector<glm::vec3> stops;
    // The star as drawn at its largest, and its glow.
    float edge = SystemMapView::kStarRadius * 1.6f + 0.4f;
    for (const glm::vec2& planet : planets)
    {
        const float at = std::max(Spread(planet.x), edge + planet.y);
        stops.push_back({planet.x, Spread(planet.x), at});
        edge = at + planet.y + kPlanetGap;
    }
    return stops;
}

// Place, from the system's Stops.
glm::vec3 PlaceAmong(const std::vector<glm::vec3>& stops, const glm::vec3& au)
{
    const float distance = glm::length(au);
    if (distance < 1.0e-6f)
    {
        return glm::vec3(0.0f);
    }
    // Between two planets' orbits, as far between where they are drawn as it is between where Spread would draw them; inside
    // the first, drawn in proportion to it; outside the last, pushed out as far as it was.
    const float spread = Spread(distance);
    float drawn = spread;
    if (!stops.empty())
    {
        if (spread <= stops.front().y)
        {
            drawn = spread * stops.front().z / stops.front().y;
        }
        else if (spread >= stops.back().y)
        {
            drawn = spread + (stops.back().z - stops.back().y);
        }
        else
        {
            for (size_t i = 0; i + 1 < stops.size(); ++i)
            {
                if (spread <= stops[i + 1].y)
                {
                    const float t = (spread - stops[i].y) / std::max(stops[i + 1].y - stops[i].y, 1.0e-6f);
                    drawn = stops[i].z + (stops[i + 1].z - stops[i].z) * t;
                    break;
                }
            }
        }
    }
    return au / distance * drawn;
}

} // namespace

glm::vec3 SystemMapView::Place(const StarSystem& system, const glm::vec3& au)
{
    return PlaceAmong(Stops(system), au);
}

float SystemMapView::DrawnRadius(const Body& body)
{
    const float scale = body.kind == BodyKind::Moon ? 0.16f : 0.28f;
    return std::clamp(scale * std::sqrt(std::max(body.radius, 0.05f)) + 0.06f, 0.08f, 1.1f);
}

std::vector<SystemMapView::Drawn> SystemMapView::Layout(const StarSystem& system, double clock)
{
    std::vector<Drawn> drawn(system.bodies.size());
    // Planets first, where they are; then each moon round its planet, by where it is in its orbit, as far out as MoonsOut
    // has it.
    std::vector<float> out(system.bodies.size(), 0.0f);
    const std::vector<glm::vec3> stops = Stops(system);
    for (const Body& body : system.bodies)
    {
        Drawn& at = drawn[body.index];
        at.index = body.index;
        at.radius = DrawnRadius(body);
        if (body.kind == BodyKind::Planet)
        {
            at.at = PlaceAmong(stops, system.Position(body.index, clock));
            MoonsOut(system, body, &out);
        }
    }
    for (const Body& body : system.bodies)
    {
        if (body.kind != BodyKind::Moon || body.parent < 0)
        {
            continue;
        }
        const Drawn& planet = drawn[static_cast<size_t>(body.parent)];
        const glm::vec3 offset = system.Position(body.index, clock) - system.Position(body.parent, clock);
        const float length = glm::length(offset);
        const glm::vec3 way = length > 1.0e-9f ? offset / length : glm::vec3(1.0f, 0.0f, 0.0f);
        drawn[body.index].at = planet.at + way * out[static_cast<size_t>(body.index)];
    }
    return drawn;
}

glm::vec3 SystemMapView::ShipAt(const StarSystem& system, const std::vector<Drawn>& drawn, double clock, const glm::vec3& au)
{
    // How far each moon was moved to be drawn, weighed by how near the ship is to it against how far out its orbit is -- and
    // the planet it goes round, which was not moved, weighed the same: so near a moon the ship goes where the moon is drawn,
    // near its planet where the planet is, and between them, between; far from both, nowhere it is not.
    const std::vector<glm::vec3> stops = Stops(system);
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
        moved += (drawn[static_cast<size_t>(body.index)].at - PlaceAmong(stops, moon)) * toMoon;
        weights += toMoon + weight(planet);
    }
    glm::vec3 at = PlaceAmong(stops, au) + moved / weights;
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
        const std::vector<glm::vec3> stops = Stops(system);
        for (int i = 0; i <= points; ++i)
        {
            // The same orbit, the clock run round it.
            const double at = clock + static_cast<double>(found->period) * static_cast<double>(i) / static_cast<double>(points);
            loop.push_back(PlaceAmong(stops, system.Position(body, at)));
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
