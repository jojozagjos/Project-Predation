#include "Game/Creature/Creature.h"

#include "Engine/Core/Log.h"
#include "Engine/Navigation/NavMesh.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Primitives.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>

namespace pred
{
namespace
{

constexpr float kTurnRate = 5.0f;

glm::quat YawRotation(float yaw)
{
    // Forward is -Z at yaw zero and yaw increases turning right, so the turn about +Y is negative.
    return glm::angleAxis(-yaw, glm::vec3(0.0f, 1.0f, 0.0f));
}

float WrapAngle(float angle)
{
    while (angle > glm::pi<float>())
    {
        angle -= glm::two_pi<float>();
    }
    while (angle < -glm::pi<float>())
    {
        angle += glm::two_pi<float>();
    }
    return angle;
}

float Horizontal(const glm::vec3& a, const glm::vec3& b)
{
    const glm::vec3 d = b - a;
    return std::sqrt(d.x * d.x + d.z * d.z);
}

glm::mat4 RootMatrix(const glm::vec3& position, float yaw)
{
    return glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(YawRotation(yaw));
}

glm::mat4 Unscaled(const glm::mat4& m)
{
    glm::mat4 out = m;
    for (int c = 0; c < 3; ++c)
    {
        const float length = glm::length(glm::vec3(m[c]));
        if (length > 1e-6f)
        {
            out[c] = m[c] / length;
        }
    }
    return out;
}

// Skins by seed. Building one is the most expensive thing a creature does -- tens of milliseconds -- and
// the same seed always builds the same skin, so a creature made again, on this machine or in the next
// game, takes the one already made. A handful are kept; beyond that the oldest go.
std::shared_ptr<const CreatureSkin> SkinFor(const CreatureAnatomy& anatomy)
{
    static std::mutex lock;
    static std::map<uint32_t, std::shared_ptr<const CreatureSkin>> made;
    static std::vector<uint32_t> order;
    {
        const std::lock_guard<std::mutex> guard(lock);
        if (const auto found = made.find(anatomy.seed); found != made.end())
        {
            return found->second;
        }
    }
    const auto start = std::chrono::steady_clock::now();
    auto skin = std::make_shared<const CreatureSkin>(CreatureSkin::Build(anatomy));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    PRED_LOG_INFO(AI, "Creature {} skin: {} vertices, {} triangles, {} bones, {:.0f} ms", anatomy.seed,
                  skin->mesh.vertices.size(), skin->mesh.TriangleCount(), skin->bones.size(), ms);
    const std::lock_guard<std::mutex> guard(lock);
    made[anatomy.seed] = skin;
    order.push_back(anatomy.seed);
    while (order.size() > 24)
    {
        made.erase(order.front());
        order.erase(order.begin());
    }
    return skin;
}

// How much a round counts for, by what it hit.
float ScaleFor(BoneKind kind)
{
    switch (kind)
    {
    case BoneKind::Head:
    case BoneKind::Jaw:
        return 2.2f;
    case BoneKind::Neck:
        return 1.5f;
    case BoneKind::Chest:
    case BoneKind::Spine:
    case BoneKind::Pelvis:
        return 1.0f;
    case BoneKind::Upper:
        return 0.75f;
    case BoneKind::Lower:
        return 0.6f;
    case BoneKind::End:
    case BoneKind::Tail:
        return 0.45f;
    }
    return 1.0f;
}

} // namespace

CreatureTraits Creature::WithBody(CreatureTraits traits, const CreatureCapabilities& caps)
{
    // The temperament is the seed's; everything physical is the body's. A heavy thing does not run as
    // fast as a light one because it has decided not to.
    traits.runSpeed = caps.runSpeed;
    traits.walkSpeed = caps.walkSpeed;
    traits.sight = caps.sight;
    traits.hearing = caps.hearing;
    traits.strikeReach = caps.strikeReach;
    traits.eyeHeight = caps.eye.y;
    traits.bodyMiddle = caps.bodyCentre.y;
    traits.fitsVents = caps.fitsVents;
    traits.climbs = caps.climbs;
    return traits;
}

Creature::Creature(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, const NavMesh* nav,
                   const CreatureTraits& traits, const glm::vec3& spawn, float healthScale)
    : m_scene(scene), m_physics(physics), m_meshes(&meshes), m_nav(nav), m_anatomy(CreatureAnatomy::FromSeed(traits.seed)),
      m_caps(CreatureCapabilities::From(m_anatomy)), m_rest(m_anatomy.Rest()),
      m_brain(WithBody(traits, m_caps)), m_position(spawn)
{
    m_maxHealth = std::max(m_caps.health * std::max(healthScale, 0.01f), 1.0f);
    m_health = m_maxHealth;

    glm::vec3 onMesh;
    if (m_nav != nullptr && m_nav->NearestPoint(spawn, 3.0f, onMesh, m_caps.fitsVents ? NavMesh::kCrawl : 0))
    {
        m_position = onMesh;
    }

    m_anchor = m_position;

    // What the players bump into: the torso, and no further. Legs and the head stand out of it, and
    // rounds find those through their own capsules.
    m_bodyHalfExtents = {std::max(m_anatomy.width * 0.38f, 0.12f), std::max(m_anatomy.depth * 0.42f, 0.12f),
                         std::max(m_anatomy.length * 0.42f, 0.2f)};
    m_bodyCentre = {0.0f, m_anatomy.hipHeight + m_anatomy.depth * 0.5f + m_anatomy.hunch * 0.5f, 0.0f};
    Transform body;
    body.position = m_position + YawRotation(m_yaw) * m_bodyCentre;
    body.rotation = YawRotation(m_yaw);
    m_body = m_physics.CreateBox(m_bodyHalfExtents, body, BodyMotion::Kinematic);
    BuildVisual(meshes);
    UpdateVisual(0.0f);
}

Creature::~Creature()
{
    Destroy();
}

void Creature::Destroy()
{
    m_ragdoll.Stop(m_physics);
    for (const Hitbox& hitbox : m_hitboxes)
    {
        m_physics.DestroyBody(hitbox.body);
    }
    m_hitboxes.clear();
    if (m_skinEntity.IsValid())
    {
        m_scene.Destroy(m_skinEntity);
        m_skinEntity = Entity{};
    }
    if (m_glintEntity.IsValid())
    {
        m_scene.Destroy(m_glintEntity);
        m_glintEntity = Entity{};
    }
    if (m_meshes != nullptr)
    {
        m_meshes->Release(m_skinMesh);
        m_meshes->Release(m_glintMesh);
        m_skinMesh = MeshHandle{};
        m_glintMesh = MeshHandle{};
    }
    if (m_body.IsValid())
    {
        m_physics.DestroyBody(m_body);
        m_body = BodyHandle{};
    }
}

glm::vec3 Creature::Forward() const
{
    return {std::sin(m_yaw), 0.0f, -std::cos(m_yaw)};
}

glm::vec3 Creature::Eye() const
{
    return m_position + Orientation() * m_caps.eye;
}

float Creature::SurfaceYaw() const
{
    // Up a wall it faces up it; otherwise the way it is heading, laid onto whatever it is on.
    const glm::vec3 forward = m_cling == Cling::Wall ? m_wallHeading : Forward();
    const glm::vec3 local = glm::inverse(m_surface) * forward;
    if (glm::length(glm::vec2(local.x, local.z)) > 1e-3f)
    {
        m_lastSurfaceYaw = std::atan2(local.x, -local.z);
    }
    return m_lastSurfaceYaw;
}

glm::quat Creature::Orientation() const
{
    return m_surface * YawRotation(SurfaceYaw());
}

void Creature::SetShownCling(Cling cling, float wallYaw)
{
    m_cling = cling;
    m_wallNormal = {std::sin(wallYaw), 0.0f, -std::cos(wallYaw)};
    m_surfaceUp = cling == Cling::Wall      ? m_wallNormal
                  : cling == Cling::Ceiling ? glm::vec3(0.0f, -1.0f, 0.0f)
                                            : glm::vec3(0.0f, 1.0f, 0.0f);
}

float Creature::CeilingAbove(const glm::vec3& floor) const
{
    const RayHit roof = m_physics.RayCastStatic(floor + glm::vec3(0.0f, 0.3f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), 6.0f);
    return roof ? roof.distance + 0.3f : 0.0f;
}

bool Creature::FindWall()
{
    // Round it at the height of its body, the nearest wall with floor at its foot, a ceiling it can
    // hang from over it, and wall all the way up to that ceiling.
    const glm::vec3 from = m_position + glm::vec3(0.0f, 0.7f, 0.0f);
    float nearest = 1.0e9f;
    bool found = false;
    for (int i = 0; i < 16; ++i)
    {
        const float angle = static_cast<float>(i) * (glm::two_pi<float>() / 16.0f);
        const glm::vec3 direction{std::cos(angle), 0.0f, std::sin(angle)};
        const RayHit hit = m_physics.RayCastStatic(from, direction, 3.0f);
        if (!hit || std::abs(hit.normal.y) > 0.3f || hit.distance >= nearest)
        {
            continue;
        }
        const glm::vec3 normal = glm::normalize(glm::vec3(hit.normal.x, 0.0f, hit.normal.z));
        glm::vec3 foot = hit.position + normal * 0.45f;
        foot.y = m_position.y;
        glm::vec3 onMesh;
        if (m_nav == nullptr || !m_nav->NearestPoint(foot, 0.8f, onMesh) || Horizontal(onMesh, foot) > 0.7f)
        {
            continue;
        }
        const float top = CeilingAbove(onMesh);
        const float depth = glm::dot(onMesh - hit.position, normal);
        if (top < 2.2f || top > 5.0f || depth < 0.05f || depth > 1.2f)
        {
            continue;
        }
        if (!m_physics.RayCastStatic(onMesh + glm::vec3(0.0f, top - 0.35f, 0.0f), -normal, depth + 0.5f))
        {
            continue; // no wall up by the ceiling: it would climb into thin air
        }
        nearest = hit.distance;
        found = true;
        m_wallFoot = onMesh;
        m_wallNormal = normal;
        m_wallDepth = depth;
        m_wallTop = top;
    }
    return found;
}

void Creature::StartDrop(const glm::vec3& onto)
{
    glm::vec3 landing = m_anchor;
    if (m_nav != nullptr)
    {
        // Onto them, or as near as the floor allows: beside them, not on top of them, and no further
        // than a fall can carry it.
        glm::vec3 target{onto.x, m_anchor.y, onto.z};
        glm::vec3 back{m_anchor.x - onto.x, 0.0f, m_anchor.z - onto.z};
        if (glm::length(back) > 0.7f)
        {
            target += glm::normalize(back) * 0.7f;
        }
        if (Horizontal(target, m_anchor) > 3.5f)
        {
            target = m_anchor + glm::normalize(glm::vec3(target.x - m_anchor.x, 0.0f, target.z - m_anchor.z)) * 3.5f;
        }
        glm::vec3 floor;
        if (m_nav->NearestPoint(target, 1.5f, floor))
        {
            landing = floor;
        }
    }
    m_dropFrom = m_position;
    m_dropTo = landing;
    m_climbT = 0.0f;
    m_dropDuration = std::clamp(std::sqrt(2.0f * std::max(m_dropFrom.y - landing.y, 0.3f) / 9.81f) + 0.1f, 0.3f, 0.9f);
    m_cling = Cling::Dropping;
    m_route.clear();
    m_routeJumps.clear();
    const glm::vec3 across{landing.x - m_dropFrom.x, 0.0f, landing.z - m_dropFrom.z};
    if (glm::length(across) > 0.3f)
    {
        m_yaw = std::atan2(across.x, -across.z);
    }
}

void Creature::MoveClinging(const CreatureIntent& intent, float dt)
{
    constexpr float kClimbSpeed = 2.6f;
    switch (m_cling)
    {
    case Cling::Wall:
    {
        m_surfaceUp = m_wallNormal;
        if (intent.drop)
        {
            StartDrop(intent.dropAt);
            return;
        }
        // Across the wall as well as up it: on a slant towards wherever it is going, as far along as the
        // wall goes, up to the ceiling -- or, wanting the floor, down it head first.
        const glm::vec3 across = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), m_wallNormal));
        const float highest = std::max(m_wallTop - 0.25f, 0.3f);
        const bool down = !intent.climb;
        float wantAlong = m_wallBack ? 0.0f : m_wallAlong;
        if (intent.move && !m_wallBack)
        {
            wantAlong = std::clamp(glm::dot(intent.destination - m_wallFoot, across), -5.0f, 5.0f);
        }
        const float wantHeight = down ? 0.0f : highest;
        const glm::vec2 now{m_wallAlong, m_climbT * highest};
        // Sideways only where there is still wall under its feet a little further along, and floor under
        // that to come down to. Where there is not, it goes straight up or down instead.
        if (std::abs(wantAlong - now.x) > 0.05f)
        {
            const float further = now.x + (wantAlong > now.x ? 0.3f : -0.3f);
            const glm::vec3 probe = m_wallFoot + across * further + glm::vec3(0.0f, now.y + 0.3f, 0.0f);
            const RayHit still = m_physics.RayCastStatic(probe, -m_wallNormal, m_wallDepth + 0.4f);
            glm::vec3 floor;
            if (!still || glm::dot(still.normal, m_wallNormal) < 0.9f || m_nav == nullptr ||
                !m_nav->NearestPoint(m_wallFoot + across * further, 0.6f, floor))
            {
                wantAlong = now.x;
            }
        }
        const glm::vec2 goal{wantAlong, wantHeight};
        const glm::vec2 gap = goal - now;
        const float length = glm::length(gap);
        const float stride = kClimbSpeed * (down ? 0.8f : 1.0f) * dt;
        const glm::vec2 next = length > stride ? now + gap / length * stride : goal;
        const glm::vec2 went = next - now;
        if (glm::length(went) > 1e-4f)
        {
            m_wallHeading = glm::normalize(across * went.x + glm::vec3(0.0f, went.y, 0.0f));
        }
        m_wallAlong = next.x;
        m_climbT = std::clamp(next.y / highest, 0.0f, 1.0f);
        const glm::vec3 wall = m_wallFoot - m_wallNormal * (m_wallDepth - 0.03f);
        m_position = wall + across * m_wallAlong + glm::vec3(0.0f, next.y, 0.0f);
        glm::vec3 below = m_wallFoot + across * m_wallAlong;
        if (m_nav != nullptr)
        {
            m_nav->NearestPoint(below, 0.6f, below);
        }
        m_anchor = below;
        m_speed = glm::length(went) / std::max(dt, 1e-4f);
        if (down && next.y <= 0.01f)
        {
            // Down: off the wall and onto its feet.
            m_cling = Cling::Floor;
            m_position = m_anchor;
            m_surfaceUp = glm::vec3(0.0f, 1.0f, 0.0f);
            m_yaw = std::atan2(m_wallNormal.x, -m_wallNormal.z);
            m_yawRate = 0.0f;
            m_routeAge = 1.0e9f;
            m_wallBack = false;
            break;
        }
        // At the top, and either where it meant to be along the wall or unable to get any further along it.
        const bool alongDone = std::abs(next.x - wantAlong) < 0.05f || std::abs(went.x) < 1e-4f;
        if (!down && next.y >= highest - 0.01f && alongDone)
        {
            // Over onto the ceiling, just out from the wall, heading away from it -- where there is one.
            const float roof = CeilingAbove(m_anchor);
            if (roof < 2.0f || roof > 5.2f)
            {
                m_wallBack = true;
                break;
            }
            m_wallBack = false;
            m_cling = Cling::Ceiling;
            m_ceilingHeight = roof;
            m_position = m_anchor + glm::vec3(0.0f, m_ceilingHeight, 0.0f);
            m_surfaceUp = glm::vec3(0.0f, -1.0f, 0.0f);
            m_yaw = std::atan2(m_wallNormal.x, -m_wallNormal.z);
            m_speed = 0.0f;
        }
        break;
    }

    case Cling::Ceiling:
    {
        m_surfaceUp = glm::vec3(0.0f, -1.0f, 0.0f);
        if (intent.drop)
        {
            StartDrop(intent.dropAt);
            return;
        }
        if (!intent.climb)
        {
            // Wanting the floor with nobody to fall on, it comes down the nearest wall rather than
            // letting go -- over the edge of the ceiling and down it, the way it went up.
            const glm::vec3 up = m_anchor + glm::vec3(0.0f, std::max(m_ceilingHeight - 0.4f, 0.5f), 0.0f);
            RayHit nearest;
            for (int i = 0; i < 12; ++i)
            {
                const float angle = static_cast<float>(i) * (glm::two_pi<float>() / 12.0f);
                const RayHit hit = m_physics.RayCastStatic(up, {std::cos(angle), 0.0f, std::sin(angle)}, 1.6f);
                if (hit && std::abs(hit.normal.y) < 0.3f && (!nearest || hit.distance < nearest.distance))
                {
                    nearest = hit;
                }
            }
            glm::vec3 foot;
            if (nearest && m_nav != nullptr)
            {
                const glm::vec3 normal = glm::normalize(glm::vec3(nearest.normal.x, 0.0f, nearest.normal.z));
                glm::vec3 wanted = nearest.position + normal * 0.45f;
                wanted.y = m_anchor.y;
                if (m_nav->NearestPoint(wanted, 0.7f, foot))
                {
                    m_cling = Cling::Wall;
                    m_wallNormal = normal;
                    m_wallFoot = foot;
                    m_wallDepth = std::max(glm::dot(foot - nearest.position, normal), 0.1f);
                    m_wallTop = m_ceilingHeight;
                    m_wallAlong = 0.0f;
                    m_wallBack = false;
                    m_climbT = 1.0f;
                    m_wallHeading = glm::vec3(0.0f, -1.0f, 0.0f);
                    return;
                }
            }
            StartDrop(m_anchor);
            return;
        }
        // Across the ceiling over the floor it would walk: the route is the floor's, with no jumps in it.
        glm::vec3 heading{0.0f};
        float wanted = 0.0f;
        if (intent.move && m_nav != nullptr)
        {
            if (m_route.empty() || m_routeAge > 0.6f || Horizontal(m_routeGoal, intent.destination) > 0.8f)
            {
                m_nav->FindPath(m_anchor, intent.destination, m_route, &m_routeReached, &m_routeJumps, 0);
                m_routeAge = 0.0f;
                m_routeGoal = intent.destination;
            }
            while (!m_route.empty() && Horizontal(m_route.front(), m_anchor) < 0.35f)
            {
                m_route.erase(m_route.begin());
                if (!m_routeJumps.empty())
                {
                    m_routeJumps.erase(m_routeJumps.begin());
                }
            }
            const glm::vec3 next = m_route.empty() ? intent.destination : m_route.front();
            const glm::vec3 toward{next.x - m_anchor.x, 0.0f, next.z - m_anchor.z};
            if (glm::length(toward) > 0.05f)
            {
                heading = glm::normalize(toward);
                wanted = std::min(intent.speed, m_caps.walkSpeed * 1.4f);
            }
        }
        m_speed += std::clamp(wanted - m_speed, -8.0f * dt, 8.0f * dt);
        glm::vec3 look = heading;
        if (intent.face)
        {
            const glm::vec3 toFace{intent.facePoint.x - m_anchor.x, 0.0f, intent.facePoint.z - m_anchor.z};
            if (glm::length(toFace) > 0.6f)
            {
                look = toFace;
            }
        }
        if (glm::length(look) > 0.05f)
        {
            look = glm::normalize(look);
            const float target = std::atan2(look.x, -look.z);
            m_yaw = WrapAngle(m_yaw + std::clamp(WrapAngle(target - m_yaw), -3.0f * dt, 3.0f * dt));
        }
        if (m_speed > 0.01f && glm::length(heading) > 0.5f && m_nav != nullptr)
        {
            glm::vec3 moved;
            if (m_nav->MoveAlongSurface(m_anchor, m_anchor + heading * m_speed * dt, moved))
            {
                // Only as far as there is a ceiling to hang from. Past that it lets go.
                const float above = CeilingAbove(moved);
                if (above < 2.0f || above > 5.2f)
                {
                    StartDrop(m_anchor);
                    return;
                }
                m_anchor = moved;
                m_ceilingHeight = above;
            }
        }
        m_ceilingCheck -= dt;
        if (m_ceilingCheck <= 0.0f)
        {
            m_ceilingCheck = 0.25f;
            const float above = CeilingAbove(m_anchor);
            if (above < 2.0f || above > 5.2f)
            {
                StartDrop(m_anchor);
                return;
            }
            m_ceilingHeight = above;
        }
        m_position = m_anchor + glm::vec3(0.0f, m_ceilingHeight, 0.0f);
        break;
    }

    case Cling::Dropping:
    {
        // Falling, faster as it goes, turning over on the way down to land on its feet.
        m_surfaceUp = glm::vec3(0.0f, 1.0f, 0.0f);
        m_climbT = std::min(m_climbT + dt / std::max(m_dropDuration, 0.1f), 1.0f);
        const float t = m_climbT;
        glm::vec3 at = glm::mix(m_dropFrom, m_dropTo, t);
        at.y = m_dropFrom.y + (m_dropTo.y - m_dropFrom.y) * t * t;
        m_position = at;
        m_airborne = t < 1.0f ? std::max(t, 0.01f) : 0.0f;
        m_speed = 0.0f;
        if (t >= 1.0f)
        {
            m_cling = Cling::Floor;
            m_position = m_dropTo;
            m_anchor = m_dropTo;
            m_airborne = 0.0f;
            m_routeAge = 1.0e9f;
        }
        break;
    }

    case Cling::Floor:
        break;
    }
}

void Creature::BuildVisual(MeshLibrary& meshes)
{
    m_skin = SkinFor(m_anatomy);
    const CreatureSkin& skin = *m_skin;

    // The skin: one mesh, painted per vertex, rewritten every frame as the skeleton bends it. White and
    // a little glossy as a material, because the colour is all in the vertices.
    const std::string prefix = "creature_" + std::to_string(m_anatomy.seed) + "_" + std::to_string(reinterpret_cast<uintptr_t>(this));
    m_skinMesh = meshes.CreateDynamic(skin.mesh, prefix + "_skin");
    Material flesh = Material::Diffuse(glm::vec3(1.0f), m_anatomy.roughness);
    m_skinEntity = m_scene.CreateMeshEntity(prefix + "_skin", Transform{}, m_skinMesh, flesh);
    if (MeshRenderer* renderer = m_scene.GetMeshRenderer(m_skinEntity))
    {
        renderer->castsShadow = true;
    }

    // The pinpricks deep in its sockets, which go out when it goes down. Faint: enough to find it by in a
    // dark room, not enough to light the room.
    if (!skin.glints.empty())
    {
        MeshData glints;
        for (const glm::vec3& at : skin.glints)
        {
            glints.Append(Primitives::Sphere(skin.glintRadius, 8, 6), glm::translate(glm::mat4(1.0f), at));
        }
        m_glintMesh = meshes.Upload(glints, prefix + "_glints");
        Material glint = Material::Diffuse(glm::vec3(0.0f), 0.4f);
        glint.emissive = m_anatomy.eyeGlow * 0.55f;
        m_glow = m_anatomy.eyeGlow * 0.55f;
        m_glintEntity = m_scene.CreateMeshEntity(prefix + "_glints", Transform{}, m_glintMesh, glint);
        if (MeshRenderer* renderer = m_scene.GetMeshRenderer(m_glintEntity))
        {
            renderer->castsShadow = false;
        }
    }

    m_rig.Init(m_anatomy, skin, m_position, m_yaw);

    // A capsule round every bone with any thickness to it, which is what rounds find.
    for (size_t b = 0; b < skin.bones.size(); ++b)
    {
        const SkinBone& bone = skin.bones[b];
        if (bone.kind == BoneKind::Jaw)
        {
            continue;
        }
        const float length = glm::distance(bone.head, bone.tail);
        const float radius = std::clamp(bone.radius * 1.1f, 0.02f, std::max(length * 0.7f, 0.03f));
        Hitbox hitbox;
        hitbox.bone = static_cast<int>(b);
        hitbox.halfLength = length * 0.5f;
        hitbox.body = m_physics.CreateCapsule(std::max(length * 0.5f - radius * 0.5f, 0.005f), radius, Transform{},
                                              BodyMotion::Kinematic, 1000.0f, PhysicsLayer::Hitbox);
        if (hitbox.body.IsValid())
        {
            m_hitboxes.push_back(hitbox);
        }
    }
}

bool Creature::Owns(BodyHandle body) const
{
    if (!body.IsValid())
    {
        return false;
    }
    return body == m_body || BoneOf(body) >= 0;
}

int Creature::BoneOf(BodyHandle body) const
{
    for (const Hitbox& hitbox : m_hitboxes)
    {
        if (hitbox.body == body)
        {
            return hitbox.bone;
        }
    }
    return m_ragdoll.BoneOf(body);
}

float Creature::DamageScale(BodyHandle body) const
{
    const int bone = BoneOf(body);
    if (bone < 0 || !m_skin)
    {
        return 1.0f;
    }
    return ScaleFor(m_skin->bones[static_cast<size_t>(bone)].kind);
}

glm::mat4 Creature::BoneWorld(int bone) const
{
    if (bone < 0 || static_cast<size_t>(bone) >= m_pose.size())
    {
        return RootMatrix(m_position, m_yaw);
    }
    if (m_ragdoll.Active())
    {
        return m_ragdoll.World()[static_cast<size_t>(bone)];
    }
    return m_rig.Root() * m_pose[static_cast<size_t>(bone)];
}

void Creature::TakeDamage(float amount, int byPlayer, const glm::vec3& from, float time, BodyHandle hit)
{
    if (!Alive())
    {
        // Rounds into a body push it about, even so.
        const int bone = BoneOf(hit);
        if (bone >= 0 && m_ragdoll.Active())
        {
            glm::vec3 push = BoneWorld(bone)[3];
            push = glm::length(push - from) > 1e-3f ? glm::normalize(push - from) : Forward();
            m_ragdoll.Push(m_physics, hit, push * 8.0f);
        }
        return;
    }
    const int bone = BoneOf(hit);
    if (bone >= 0)
    {
        amount *= ScaleFor(m_skin->bones[static_cast<size_t>(bone)].kind);
    }
    // Plates stop part of every round.
    amount *= 1.0f - m_caps.armour;
    m_health = std::max(m_health - amount, 0.0f);
    m_brain.OnDamaged(amount, byPlayer, from, time, m_maxHealth);

    // It flinches away from the hit, and remembers the push for which way it falls.
    const glm::vec3 at = bone >= 0 ? glm::vec3(BoneWorld(bone)[3]) : m_position + glm::vec3(0.0f, m_bodyCentre.y, 0.0f);
    glm::vec3 direction = at - from;
    direction = glm::length(direction) > 1e-3f ? glm::normalize(direction) : Forward();
    m_rig.Flinch(bone, direction, std::clamp(amount / std::max(m_maxHealth * 0.05f, 1.0f), 0.05f, 0.6f));
    m_lastHitBone = bone >= 0 ? bone : (m_skin ? m_skin->chest : -1);
    m_lastHitPush = direction * (12.0f + 30.0f * std::clamp(amount / std::max(m_maxHealth * 0.1f, 1.0f), 0.0f, 1.0f));

    if (!Alive())
    {
        // Dead for real. The mind stops with it -- nothing after this perceives or decides -- which
        // is what makes lying still as a trick a different thing from this.
        m_brain.OnDied(time);
        m_speed = 0.0f;
        m_windup = 0.0f;
        m_crouchTarget = 0.0f;
        m_action = Action{};
    }
}

void Creature::SetShownState(const glm::vec3& position, float yaw, float speed, float windup, bool alive,
                             bool down, float crouch)
{
    m_position = position;
    m_yaw = yaw;
    m_speed = speed;
    m_windup = windup;
    m_down = down;
    m_crouchTarget = crouch;
    if (!alive && Alive())
    {
        m_health = 0.0f;
    }
    SyncBody(0.0f);
}

void Creature::Receive(const glm::vec3& position, float yaw, float speed, float windup, float healthFraction,
                       bool alive, bool down, float crouch)
{
    m_received = {position, yaw, speed, windup, alive, down, crouch};
    m_receivedAge = 0.0f;
    m_hasReceived = true;
    // Health follows the host's, for anything that shows it. Not when it has died: dying is left to
    // SetShownState, which only notices a death while the body is still alive to have one.
    if (alive)
    {
        m_health = std::max(healthFraction * m_maxHealth, 0.01f);
    }
}

void Creature::FollowReceived(float dt)
{
    if (!m_hasReceived)
    {
        return;
    }
    m_receivedAge += dt;
    // Guessed forward by what it was doing, for no longer than a few packets: past that a guess is
    // worse than waiting to be told.
    // Not up a wall or falling, where its heading is not the way it is going.
    const bool guessable = m_cling == Cling::Floor || m_cling == Cling::Ceiling;
    const float lead = m_received.alive && guessable ? std::min(m_receivedAge, 0.1f) : 0.0f;
    const glm::vec3 heading{std::sin(m_received.yaw), 0.0f, -std::cos(m_received.yaw)};
    const glm::vec3 goal = m_received.position + heading * (m_received.speed * lead);

    // Eased rather than set, or thirty updates a second would draw as thirty small jumps. Too far
    // off to ease -- the first packet, or a creature that has been somewhere else entirely -- it is
    // simply put there.
    const float blend = 1.0f - std::exp(-15.0f * dt);
    glm::vec3 position = goal;
    float yaw = m_received.yaw;
    if (m_followedOnce && glm::distance(m_position, goal) < 3.0f)
    {
        position = m_position + (goal - m_position) * blend;
        yaw = m_yaw + std::remainder(m_received.yaw - m_yaw, glm::two_pi<float>()) * blend;
    }
    m_followedOnce = true;
    SetShownState(position, yaw, m_received.speed, m_received.windup, m_received.alive, m_received.down,
                  m_received.crouch);
}

void Creature::Update(CreatureSenses senses, float time, float dt)
{
    m_time = time;
    if (!Alive())
    {
        return;
    }
    senses.time = time;
    senses.position = m_cling == Cling::Floor ? m_position : m_anchor;
    senses.onCeiling = m_cling != Cling::Floor;
    senses.eye = Eye();
    senses.forward = Forward();
    senses.healthFraction = m_health / m_maxHealth;
    senses.nav = m_nav;
    senses.crawl = m_caps.fitsVents ? NavMesh::kCrawl : 0;
    senses.routeReached = m_routeReached;
    senses.verticalReach = m_caps.verticalReach;

    m_brain.Update(senses, dt);
    const CreatureIntent& intent = m_brain.Intent();
    m_windup = intent.windup;
    m_down = intent.down;
    m_crouchTarget = intent.crouch;
    m_look = intent.look;
    m_lookAt = intent.lookAt;

    // What the body shows of what it is doing, beyond walking.
    // A call to the others and a threat display at somebody are the same rearing-up to look at.
    if (intent.roar || intent.display)
    {
        m_roarUntil = time + 1.3f;
    }
    if (intent.bashing && m_bashStarted < 0.0f)
    {
        m_bashStarted = time;
    }
    else if (!intent.bashing)
    {
        m_bashStarted = -1.0f;
    }
    Action action;
    switch (intent.attack)
    {
    case AttackKind::Swipe:
        action.kind = RigAction::Swipe;
        break;
    case AttackKind::Bite:
        action.kind = RigAction::Bite;
        break;
    case AttackKind::Lunge:
        action.kind = RigAction::Lunge;
        break;
    case AttackKind::Grab:
        action.kind = RigAction::Grab;
        break;
    case AttackKind::None:
        break;
    }
    if (action.kind != RigAction::None)
    {
        action.phase = intent.attackPhase;
        action.side = intent.attackSide;
        action.target = intent.attackAt;
    }
    else if (intent.holding >= 0)
    {
        action.kind = RigAction::Carry;
        action.phase = 0.5f;
        action.target = m_position + Forward() * (m_anatomy.length * 0.6f) + glm::vec3(0.0f, m_anatomy.hipHeight, 0.0f);
    }
    else if (m_bashStarted >= 0.0f)
    {
        action.kind = RigAction::Bash;
        action.phase = std::fmod((time - m_bashStarted) / 0.9f, 1.0f);
        action.target = m_position + Forward();
    }
    else if (time < m_roarUntil)
    {
        action.kind = RigAction::Roar;
        action.phase = 1.0f - (m_roarUntil - time) / 1.3f;
        action.target = m_position + Forward();
    }
    m_action = action;

    Move(intent, senses.others, dt);
    if (m_cling == Cling::Floor)
    {
        m_anchor = m_position;
    }
    SyncBody(dt);
}

void Creature::Move(const CreatureIntent& asked, const std::vector<glm::vec3>& others, float dt)
{
    CreatureIntent intent = asked;
    if (m_climbOverride >= 0)
    {
        intent.climb = m_climbOverride == 1;
        intent.drop = false;
    }
    m_routeAge += dt;
    if (m_jumps == 0)
    {
        // The jumps its body can make, as the navigation mesh names them. A drop it can always take.
        m_jumps = NavMesh::kDrop;
        if (m_caps.jump >= 0.9f)
        {
            m_jumps |= NavMesh::kJumpLow;
        }
        if (m_caps.jump >= 1.75f)
        {
            m_jumps |= NavMesh::kJumpMid;
        }
        if (m_caps.jump >= 2.6f)
        {
            m_jumps |= NavMesh::kJumpHigh;
        }
        // And along crawlspaces, when it is small enough to get into one.
        if (m_caps.fitsVents)
        {
            m_jumps |= NavMesh::kCrawl;
        }
    }
    const uint16_t crawl = m_jumps & NavMesh::kCrawl;

    if (m_cling != Cling::Floor)
    {
        MoveClinging(intent, dt);
        m_brain.Route() = m_route;
        return;
    }
    // Wanting the ceiling, one that climbs goes to the foot of the nearest wall that has one over it, and
    // up. Not from a crawlspace, not lying down, and not in the middle of a jump.
    if (intent.climb && m_caps.climbs && m_squeeze < 0.3f && !m_down && !m_jumping)
    {
        if (!m_haveWall && m_time >= m_wallSearchAt)
        {
            m_haveWall = FindWall();
            m_wallSearchAt = m_time + 1.5f;
        }
        if (m_haveWall)
        {
            if (Horizontal(m_position, m_wallFoot) > 0.4f)
            {
                intent.move = true;
                intent.destination = m_wallFoot;
                intent.speed = std::max(intent.speed, m_caps.walkSpeed * 1.3f);
                intent.face = false;
            }
            else
            {
                m_cling = Cling::Wall;
                m_climbT = 0.0f;
                m_wallAlong = 0.0f;
                m_wallBack = false;
                m_wallHeading = glm::vec3(0.0f, 1.0f, 0.0f);
                m_anchor = m_wallFoot;
                m_haveWall = false;
                m_route.clear();
                m_routeJumps.clear();
                // Facing the wall, to go up it.
                m_yaw = std::atan2(-m_wallNormal.x, m_wallNormal.z);
                m_brain.Route() = m_route;
                return;
            }
        }
    }
    else
    {
        m_haveWall = false;
    }

    // In the air: carried along the arc from where it left the ground to where it lands, the feet
    // tucked up, and nothing else to decide until it is down.
    if (m_jumping)
    {
        m_jumpTime += dt;
        const float t = std::clamp(m_jumpTime / m_jumpDuration, 0.0f, 1.0f);
        const float rise = std::max(m_jumpTo.y - m_jumpFrom.y, 0.0f);
        const float arc = 0.35f + rise * 0.35f;
        m_position = glm::mix(m_jumpFrom, m_jumpTo, t) + glm::vec3(0.0f, arc * 4.0f * t * (1.0f - t), 0.0f);
        const glm::vec3 along{m_jumpTo.x - m_jumpFrom.x, 0.0f, m_jumpTo.z - m_jumpFrom.z};
        if (glm::length(along) > 0.05f)
        {
            const float target = std::atan2(along.x, -along.z);
            m_yaw = WrapAngle(m_yaw + std::clamp(WrapAngle(target - m_yaw), -8.0f * dt, 8.0f * dt));
        }
        m_airborne = t < 1.0f ? std::max(t, 0.01f) : 0.0f;
        if (t >= 1.0f)
        {
            m_jumping = false;
            m_position = m_jumpTo;
            m_routeAge = 1.0e9f; // a fresh route from the new floor
        }
        m_brain.Route() = m_route;
        return;
    }
    m_airborne = 0.0f;

    glm::vec3 heading{0.0f};
    float wanted = 0.0f;
    if (intent.move && m_nav != nullptr)
    {
        if (m_route.empty() || m_routeAge > 0.6f || Horizontal(m_routeGoal, intent.destination) > 0.8f)
        {
            m_nav->FindPath(m_position, intent.destination, m_route, &m_routeReached, &m_routeJumps, m_jumps);
            m_routeAge = 0.0f;
            m_routeGoal = intent.destination;
        }
        // Corners already reached are behind it, including the first, which is where it stands. A corner
        // that is the take-off of a jump is not passed but jumped from.
        while (!m_route.empty() && Horizontal(m_route.front(), m_position) < 0.35f)
        {
            const bool takeOff = !m_routeJumps.empty() && m_routeJumps.front() != 0 && m_route.size() >= 2;
            if (takeOff)
            {
                m_jumping = true;
                m_jumpFrom = m_position;
                m_jumpTo = m_route[1];
                m_jumpTime = 0.0f;
                const float gap = glm::distance(m_jumpFrom, m_jumpTo);
                m_jumpDuration = std::clamp(0.3f + gap * 0.12f + std::max(m_jumpTo.y - m_jumpFrom.y, 0.0f) * 0.08f, 0.35f, 0.9f);
                m_route.erase(m_route.begin(), m_route.begin() + 2);
                m_routeJumps.erase(m_routeJumps.begin(), m_routeJumps.begin() + std::min<size_t>(2, m_routeJumps.size()));
                m_brain.Route() = m_route;
                return;
            }
            m_route.erase(m_route.begin());
            if (!m_routeJumps.empty())
            {
                m_routeJumps.erase(m_routeJumps.begin());
            }
        }
        const glm::vec3 next = m_route.empty() ? intent.destination : m_route.front();
        const glm::vec3 toward{next.x - m_position.x, 0.0f, next.z - m_position.z};
        if (glm::length(toward) > 0.05f)
        {
            heading = glm::normalize(toward);
            wanted = intent.speed;
            // Coming up to a corner it starts round it early, the faster the earlier, so the route is
            // run as a curve and not as a string of straight lines with a pivot at each end. Not a corner
            // it jumps from: that it has to reach.
            const bool takeOffNext = !m_routeJumps.empty() && m_routeJumps.front() != 0;
            if (m_route.size() >= 2 && !takeOffNext)
            {
                const float ahead = 0.6f + m_speed * 0.35f;
                const float left = glm::length(toward);
                const glm::vec3 after{m_route[1].x - next.x, 0.0f, m_route[1].z - next.z};
                if (left < ahead && glm::length(after) > 0.05f)
                {
                    const float blend = (1.0f - left / ahead) * 0.6f;
                    const glm::vec3 rounded = glm::mix(heading, glm::normalize(after), blend);
                    if (glm::length(rounded) > 0.05f)
                    {
                        heading = glm::normalize(rounded);
                    }
                }
            }
        }
    }
    else
    {
        m_route.clear();
        m_routeJumps.clear();
    }
    m_brain.Route() = m_route;

    // Getting up to speed and stopping take a moment, both ways. Faster to stop than to start,
    // because what it stops for is usually something in front of it.
    const float rate = wanted > m_speed ? 10.0f : 14.0f;
    m_speed += std::clamp(wanted - m_speed, -rate * dt, rate * dt);

    // Turning: towards whatever it is looking at, or else the way it is going.
    //
    // Not towards anything right on top of it -- the direction to a point under its own nose swings all
    // the way round with every centimetre either of them moves, and it spun on the spot chasing it -- and,
    // standing still, not for a small turn at all: its head does that. Turning on the spot is also slower
    // than turning on the run, the way a body has to step itself round.
    glm::vec3 lookAlong = heading;
    bool faceTarget = false;
    if (intent.face)
    {
        const glm::vec3 toFace{intent.facePoint.x - m_position.x, 0.0f, intent.facePoint.z - m_position.z};
        if (glm::length(toFace) > 0.6f)
        {
            lookAlong = toFace;
            faceTarget = true;
        }
    }
    if (glm::length(lookAlong) > 0.05f)
    {
        lookAlong = glm::normalize(lookAlong);
        const float target = std::atan2(lookAlong.x, -lookAlong.z);
        const float turn = WrapAngle(target - m_yaw);
        const bool standing = m_speed < 0.3f;
        // How fast it would like to be turning: in proportion to how far it has to go, up to what its
        // body manages -- less for a big one, less standing than on the move. How fast it gets there is
        // its weight: it swings into a turn and out of it.
        const float bulk = std::clamp(1.35f - 0.3f * m_anatomy.length, 0.55f, 1.2f);
        const float most = (standing ? 2.4f : kTurnRate * 0.8f) * bulk;
        float desired = std::clamp(turn * 3.5f, -most, most);
        if (standing && faceTarget && std::abs(turn) < 0.3f)
        {
            desired = 0.0f; // a small turn, standing, is for the head to make
        }
        const float swing = 9.0f * bulk * dt;
        m_yawRate += std::clamp(desired - m_yawRate, -swing, swing);
    }
    else
    {
        m_yawRate += std::clamp(-m_yawRate, -9.0f * dt, 9.0f * dt);
    }
    m_yaw = WrapAngle(m_yaw + m_yawRate * dt);

    if (m_speed > 0.01f && glm::length(heading) > 0.5f)
    {
        // Slower while it is still turning to face the way it is going, so it turns and then runs
        // rather than running sideways.
        const float aligned = std::clamp(glm::dot(Forward(), heading), 0.3f, 1.0f);
        // On its belly it goes no faster than a crawl.
        const float crawling = m_squeeze > 0.5f ? std::min(m_speed, m_caps.walkSpeed * 1.2f) : m_speed;
        // It goes a little the way it faces as well as the way it wants: a body on the move carries
        // round a turn, it does not slide sideways into the new direction.
        glm::vec3 going = heading;
        if (glm::dot(Forward(), heading) > 0.2f)
        {
            going = glm::normalize(heading * 0.7f + Forward() * 0.3f);
        }
        const glm::vec3 step = going * crawling * aligned * dt;
        glm::vec3 moved = m_position + step;
        if (m_nav == nullptr || !m_nav->MoveAlongSurface(m_position, m_position + step, moved, crawl))
        {
            moved = m_position + step;
        }
        m_position = moved;
    }

    // Room for each other. Two creatures on the same errand take the same route and would end up
    // standing inside one another, because nothing but this keeps them apart: their bodies are moved,
    // not pushed, and the navigation mesh knows nothing of either. So each eases away from any other
    // closer than its own length, along the walkable surface, and a pack stays a pack of bodies.
    constexpr float kPersonalSpace = 1.8f;
    glm::vec3 push{0.0f};
    for (const glm::vec3& other : others)
    {
        glm::vec3 apart{m_position.x - other.x, 0.0f, m_position.z - other.z};
        const float distance = glm::length(apart);
        if (distance >= kPersonalSpace)
        {
            continue;
        }
        // Exactly on top of each other -- made at one spot -- there is no "away"; each takes a side of
        // its own, by its number, so the two do not both step the same way.
        const float angle = static_cast<float>(m_netId) * 2.39996f;
        apart = distance > 1e-3f ? apart / distance : glm::vec3(std::cos(angle), 0.0f, std::sin(angle));
        push += apart * (kPersonalSpace - distance);
    }
    if (glm::length(push) > 1e-4f)
    {
        const glm::vec3 step = push * std::min(4.0f * dt, 1.0f);
        glm::vec3 moved = m_position + step;
        if (m_nav == nullptr || !m_nav->MoveAlongSurface(m_position, m_position + step, moved, crawl))
        {
            moved = m_position + step;
        }
        m_position = moved;
    }
}

void Creature::SyncBody(float dt)
{
    if (!m_body.IsValid())
    {
        return;
    }
    Transform body;
    if (m_ragdoll.Active())
    {
        // Lying as a ragdoll, its pieces are what rounds find and nothing is left standing to bump into.
        body.position = m_position + glm::vec3(0.0f, -500.0f, 0.0f);
        m_physics.SetTransform(m_body, body);
        return;
    }
    const glm::quat facing = Orientation();
    body.rotation = facing;
    body.position = m_position + facing * m_bodyCentre;
    if (dt > 0.0f)
    {
        m_physics.MoveKinematic(m_body, body, dt);
    }
    else
    {
        m_physics.SetTransform(m_body, body);
    }
}

void Creature::SetShownAction(const Action& action, float airborne, bool look, const glm::vec3& lookAt)
{
    m_action = action;
    m_airborne = airborne;
    m_look = look;
    m_lookAt = lookAt;
}

void Creature::PlaceHitboxes(const std::vector<glm::mat4>& local, bool away)
{
    const glm::mat4 root = m_rig.Root();
    for (size_t i = 0; i < m_hitboxes.size(); ++i)
    {
        const Hitbox& hitbox = m_hitboxes[i];
        Transform transform;
        if (away || static_cast<size_t>(hitbox.bone) >= local.size())
        {
            transform.position = m_position + glm::vec3(static_cast<float>(i), -500.0f, 0.0f);
        }
        else
        {
            const glm::mat4 world = Unscaled(root * local[static_cast<size_t>(hitbox.bone)]);
            transform.position = glm::vec3(world * glm::vec4(0.0f, hitbox.halfLength, 0.0f, 1.0f));
            transform.rotation = glm::quat_cast(glm::mat3(world));
        }
        m_physics.SetTransform(hitbox.body, transform);
    }
}

void Creature::GoLimp()
{
    if (!m_skin)
    {
        return;
    }
    const size_t count = m_skin->bones.size();
    std::vector<glm::mat4> world(count);
    for (size_t b = 0; b < count; ++b)
    {
        world[b] = m_rig.Root() * (b < m_pose.size() ? m_pose[b] : m_skin->bones[b].rest);
    }
    // Falling the way it was going, and the way the last round pushed it.
    m_ragdoll.Start(m_physics, *m_skin, world, m_velocity * 0.8f, m_lastHitBone, m_lastHitPush);
    m_fallen.clear();
    m_getUp = 0.0f;
    PlaceHitboxes(m_pose, true);
    SyncBody(0.0f);
}

void Creature::GetUp()
{
    // Out of the heap: where it lies becomes where it stands, and every bone eases from where it fell to
    // where it stands over the next second, rather than the body snapping upright.
    m_ragdoll.Update(m_physics);
    m_fallen = m_ragdoll.World();
    const glm::vec3 centre = m_skin ? glm::vec3(m_fallen[static_cast<size_t>(m_skin->pelvis)][3]) : m_position;
    if (!m_hasReceived)
    {
        glm::vec3 onMesh;
        if (m_nav != nullptr && m_nav->NearestPoint(centre, 1.5f, onMesh))
        {
            m_position = onMesh;
        }
        // Facing the way its spine lies.
        if (m_skin)
        {
            const glm::vec3 along = glm::vec3(m_fallen[static_cast<size_t>(m_skin->pelvis)][1]);
            if (glm::length(glm::vec2(along.x, along.z)) > 0.1f)
            {
                m_yaw = std::atan2(along.x, -along.z);
            }
        }
    }
    m_ragdoll.Stop(m_physics);
    m_getUp = 1.0f;
    m_rig.Init(m_anatomy, *m_skin, m_position, m_yaw);
    SyncBody(0.0f);
}

void Creature::UpdateVisual(float dt)
{
    if (!m_skin)
    {
        return;
    }
    const CreatureSkin& skin = *m_skin;
    m_shownTime += dt;
    if (!m_shownOnce)
    {
        m_lastShown = m_position;
        m_shownOnce = true;
    }
    if (dt > 0.0f)
    {
        glm::vec3 moved = (m_position - m_lastShown) / dt;
        // A jump across the map is being put somewhere, not moving there.
        if (glm::length(moved) > 25.0f)
        {
            moved = glm::vec3(0.0f);
        }
        m_velocity += (moved - m_velocity) * (1.0f - std::exp(-12.0f * dt));
    }
    m_lastShown = m_position;
    // Clinging to a wall or a ceiling it keeps its body flat to it, legs splayed, the way anything that
    // climbs does: hanging a whole hip's height off a ceiling on straight legs looked like a puppet.
    const float crouchWanted = m_cling == Cling::Wall || m_cling == Cling::Ceiling ? std::max(m_crouchTarget, 0.85f) : m_crouchTarget;
    m_crouch += (crouchWanted - m_crouch) * (1.0f - std::exp(-6.0f * dt));

    const bool lying = Down();
    if (lying && !m_ragdoll.Active())
    {
        GoLimp();
    }
    else if (!lying && m_ragdoll.Active())
    {
        GetUp();
    }

    const size_t count = skin.bones.size();
    std::vector<glm::mat4> skinning(count);
    glm::mat4 placed(1.0f);
    if (m_ragdoll.Active())
    {
        m_ragdoll.Update(m_physics);
        const std::vector<glm::mat4>& world = m_ragdoll.World();
        const glm::mat4 rootInverse = glm::inverse(m_rig.Root());
        m_pose.resize(count);
        for (size_t b = 0; b < count; ++b)
        {
            skinning[b] = world[b] * skin.bones[b].restInverse;
            m_pose[b] = rootInverse * world[b];
        }
    }
    else
    {
        RigInput input;
        // Its body turned towards the up of whatever it is on, a quarter turn at a time rather than all
        // at once: up a wall, over onto the ceiling, and over again as it falls.
        const glm::vec3 current = m_surface * glm::vec3(0.0f, 1.0f, 0.0f);
        const float agree = glm::dot(current, m_surfaceUp);
        if (agree < 0.9999f && dt > 0.0f)
        {
            const glm::quat turn = agree < -0.999f ? glm::angleAxis(glm::pi<float>(), Forward())
                                                   : glm::rotation(current, m_surfaceUp);
            const float rate = m_cling == Cling::Dropping ? 10.0f : 7.0f;
            m_surface = glm::normalize(glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), turn, std::min(rate * dt, 1.0f)) * m_surface);
        }
        input.position = m_position;
        input.yaw = SurfaceYaw();
        input.surface = m_surface;
        input.probe = [this](const glm::vec3& from, const glm::vec3& direction, float reach, glm::vec3& hit)
        {
            const RayHit found = m_physics.RayCastStatic(from, direction, reach);
            if (found)
            {
                hit = found.position;
            }
            return static_cast<bool>(found);
        };
        input.velocity = m_velocity;
        // In a crawlspace: flat to its belly while it is in one, eased in and out at either end. Worked
        // out here, where it is drawn, so a machine that is only shown it lays it down in there too.
        m_crawlCheck -= dt;
        if (m_caps.fitsVents && m_nav != nullptr && m_crawlCheck <= 0.0f)
        {
            m_crawlCheck = 0.2f;
            m_inCrawlspace = m_nav->InCrawlspace(m_position);
        }
        m_squeeze += ((m_inCrawlspace ? 1.0f : 0.0f) - m_squeeze) * std::min(6.0f * dt, 1.0f);
        input.crouch = m_crouch;
        input.squeeze = m_squeeze;
        input.windup = m_windup;
        input.action = m_action.kind;
        input.actionPhase = m_action.phase;
        input.actionSide = m_action.side;
        input.actionTarget = m_action.target;
        input.look = m_look;
        input.lookAt = m_lookAt;
        input.airborne = m_airborne;
        input.time = m_shownTime;
        input.dt = dt;
        input.ground = [this](const glm::vec3& from, float drop, float& height)
        {
            const RayHit hit = m_physics.RayCastStatic(from, glm::vec3(0.0f, -1.0f, 0.0f), drop);
            if (!hit)
            {
                return false;
            }
            height = hit.position.y;
            return true;
        };
        m_rig.Update(input);
        m_pose = m_rig.Bones();
        placed = m_rig.Root();

        if (m_getUp > 0.0f && m_fallen.size() == count)
        {
            // Easing out of where it fell.
            const float t = 1.0f - m_getUp;
            const float s = t * t * (3.0f - 2.0f * t);
            const glm::mat4 rootInverse = glm::inverse(placed);
            for (size_t b = 0; b < count; ++b)
            {
                const glm::mat4 from = rootInverse * m_fallen[b];
                const glm::mat4& to = m_pose[b];
                const glm::quat qa = glm::quat_cast(glm::mat3(Unscaled(from)));
                const glm::quat qb = glm::quat_cast(glm::mat3(Unscaled(to)));
                glm::mat4 blended = glm::mat4_cast(glm::slerp(qa, qb, s));
                blended[3] = glm::vec4(glm::mix(glm::vec3(from[3]), glm::vec3(to[3]), s), 1.0f);
                m_pose[b] = blended;
            }
            m_getUp = std::max(m_getUp - dt / 1.1f, 0.0f);
        }
        for (size_t b = 0; b < count; ++b)
        {
            skinning[b] = m_pose[b] * skin.bones[b].restInverse;
        }
    }

    AABB bounds;
    SkinVertices(skin, skinning, m_skinned, bounds);
    if (m_meshes != nullptr)
    {
        m_meshes->UpdateDynamic(m_skinMesh, m_skinned, bounds);
    }
    if (Transform* transform = m_scene.GetTransform(m_skinEntity))
    {
        transform->position = glm::vec3(placed[3]);
        transform->rotation = glm::quat_cast(glm::mat3(placed));
    }
    if (m_glintEntity.IsValid())
    {
        // Carried by the head, and out as it goes down. Dead or pretending, the same: a body lying there
        // with its eyes lit is not dead, and the players should not be able to tell which it is.
        const glm::mat4 head = placed * skinning[static_cast<size_t>(skin.head)];
        if (Transform* transform = m_scene.GetTransform(m_glintEntity))
        {
            transform->position = glm::vec3(head[3]);
            transform->rotation = glm::quat_cast(glm::mat3(Unscaled(head)));
        }
        if (MeshRenderer* renderer = m_scene.GetMeshRenderer(m_glintEntity))
        {
            renderer->material.emissive = lying ? glm::vec3(0.0f) : m_glow * (1.0f - m_getUp);
        }
    }
    PlaceHitboxes(m_pose, m_ragdoll.Active());
    SyncBody(0.0f);
}

std::string Creature::DescribePose() const
{
    glm::vec3 torso = m_position;
    if (m_skin)
    {
        torso = glm::vec3(BoneWorld(m_skin->chest)[3]);
    }
    char line[360];
    std::snprintf(line, sizeof(line),
                  "at %.2f %.2f %.2f yaw %.0f deg  %s%s  crouch %.2f  %s %.2f  torso drawn at %.2f %.2f %.2f  "
                  "(%s, %.0f of %.0f health)",
                  m_position.x, m_position.y, m_position.z, glm::degrees(m_yaw),
                  !Alive() ? "dead" : (m_down ? "lying still" : "up"), m_ragdoll.Active() ? " (limp)" : "", m_crouch,
                  RigActionName(m_action.kind), m_action.phase, torso.x, torso.y, torso.z, BodyPlanName(m_anatomy.plan),
                  m_health, m_maxHealth);
    return line;
}

} // namespace pred
