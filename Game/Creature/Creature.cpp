#include "Game/Creature/Creature.h"

#include "Engine/Animation/IK.h"
#include "Engine/Navigation/NavMesh.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Primitives.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

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

// The shortest turn that points `from` along `to`, both unit length.
glm::quat TurnBetween(const glm::vec3& from, const glm::vec3& to)
{
    const float cosine = glm::dot(from, to);
    if (cosine < -0.9999f)
    {
        // Exactly opposite: any half turn about something square to it.
        const glm::vec3 axis = std::abs(from.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
        return glm::angleAxis(glm::pi<float>(), glm::normalize(glm::cross(from, axis)));
    }
    const glm::vec3 axis = glm::cross(from, to);
    return glm::normalize(glm::quat(1.0f + cosine, axis.x, axis.y, axis.z));
}

// A piece that runs from `a` to `b`, drawn from a mesh that runs along Y and is centred on its middle.
glm::mat4 Between(const glm::vec3& a, const glm::vec3& b)
{
    const glm::vec3 along = b - a;
    const float length = glm::length(along);
    const glm::quat turn = length > 1e-5f ? TurnBetween(glm::vec3(0.0f, 1.0f, 0.0f), along / length)
                                          : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    return glm::translate(glm::mat4(1.0f), (a + b) * 0.5f) * glm::mat4_cast(turn);
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
    return traits;
}

Creature::Creature(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, const NavMesh* nav,
                   const CreatureTraits& traits, const glm::vec3& spawn, float healthScale)
    : m_scene(scene), m_physics(physics), m_nav(nav), m_anatomy(CreatureAnatomy::FromSeed(traits.seed)),
      m_caps(CreatureCapabilities::From(m_anatomy)), m_rest(m_anatomy.Rest()),
      m_brain(WithBody(traits, m_caps)), m_position(spawn)
{
    m_maxHealth = std::max(m_caps.health * std::max(healthScale, 0.01f), 1.0f);
    m_health = m_maxHealth;

    // The legs' swing: half a stride is about a third of the leg's length, and a foot on the ground
    // moves backwards exactly as fast as the body moves forwards, which is what stops it sliding. A
    // stride of amplitude A needs 1/A radians of the cycle per metre for that to be true.
    float legLength = 0.0f;
    for (const LegPair& pair : m_anatomy.legs)
    {
        legLength += pair.upper + pair.lower;
    }
    legLength /= static_cast<float>(std::max<size_t>(m_anatomy.legs.size(), 1));
    m_strideRate = 1.0f / std::max(legLength * 0.32f, 0.05f);

    glm::vec3 onMesh;
    if (m_nav != nullptr && m_nav->NearestPoint(spawn, 3.0f, onMesh))
    {
        m_position = onMesh;
    }
    Transform body;
    body.position = m_position + YawRotation(m_yaw) * m_caps.bodyCentre;
    body.rotation = YawRotation(m_yaw);
    m_body = m_physics.CreateBox(m_caps.bodyHalfExtents, body, BodyMotion::Kinematic);
    BuildVisual(meshes);
    UpdateVisual(0.0f);
}

Creature::~Creature()
{
    Destroy();
}

void Creature::Destroy()
{
    for (const Piece& piece : m_pieces)
    {
        m_scene.Destroy(piece.entity);
    }
    m_pieces.clear();
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
    return m_position + YawRotation(m_yaw) * m_caps.eye;
}

void Creature::BuildVisual(MeshLibrary& meshes)
{
    const CreatureAnatomy& a = m_anatomy;
    const CreatureAnatomy::RestPose& pose = m_rest;
    const Material skin = Material::Diffuse(a.skin, a.roughness);
    const Material under = Material::Diffuse(a.underside, std::min(a.roughness + 0.1f, 1.0f));
    Material plate = Material::Diffuse(a.skin * 0.75f, 0.45f);
    Material eyes = Material::Diffuse({1.0f, 0.3f, 0.2f}, 0.4f);
    // Faint. Enough to find it by in a dark room, not enough to light the room.
    eyes.emissive = a.eyeGlow;

    // Every mesh is named for this creature's seed: two creatures are two different bodies, and a
    // shared name would hand the second the first one's legs.
    const std::string prefix = "creature_" + std::to_string(a.seed) + "_";
    int count = 0;
    const auto add = [&](const MeshData& data, const Material& material, PieceKind kind, const glm::mat4& local,
                         int leg = -1)
    {
        const std::string name = prefix + std::to_string(count++);
        const MeshHandle mesh = meshes.Upload(data, name);
        Piece piece;
        piece.entity = m_scene.CreateMeshEntity(name, Transform{}, mesh, material);
        piece.kind = kind;
        piece.local = local;
        piece.leg = leg;
        piece.glow = material.emissive;
        if (MeshRenderer* renderer = m_scene.GetMeshRenderer(piece.entity))
        {
            renderer->castsShadow = true;
        }
        m_pieces.push_back(piece);
    };

    // The body: a row of overlapping ellipsoids from shoulders to rump, narrowing towards the back, laid
    // along the line between them so a hunched back slopes.
    const glm::vec3 spineDirection = glm::normalize(pose.rump - pose.shoulders);
    const glm::quat alongSpine = TurnBetween(glm::vec3(0.0f, 0.0f, 1.0f), spineDirection);
    const float segmentLength = a.length / static_cast<float>(std::max(a.segments, 1));
    for (size_t i = 0; i < pose.spine.size(); ++i)
    {
        const float t = pose.spine.size() > 1 ? static_cast<float>(i) / static_cast<float>(pose.spine.size() - 1) : 0.0f;
        const glm::vec3 radii{pose.spineRadius[i], a.depth * 0.5f * (1.0f - 0.18f * t), segmentLength * 0.8f};
        add(Primitives::Ellipsoid(radii, 14, 9), skin, PieceKind::Body,
            glm::translate(glm::mat4(1.0f), pose.spine[i]) * glm::mat4_cast(alongSpine));
    }

    // Plates along the top of the back, and spines along the ridge.
    for (int p = 0; p < a.plates; ++p)
    {
        const float t = 0.15f + 0.7f * (static_cast<float>(p) + 0.5f) / static_cast<float>(a.plates);
        const glm::vec3 at = pose.shoulders + (pose.rump - pose.shoulders) * t + glm::vec3(0.0f, a.depth * 0.42f, 0.0f);
        const glm::vec3 radii{a.width * 0.36f, 0.035f, a.length / static_cast<float>(a.plates) * 0.5f};
        add(Primitives::Ellipsoid(radii, 12, 6), plate, PieceKind::Body,
            glm::translate(glm::mat4(1.0f), at) * glm::mat4_cast(alongSpine));
    }
    for (int s = 0; s < a.spines; ++s)
    {
        const float t = 0.05f + 0.9f * (static_cast<float>(s) + 0.5f) / static_cast<float>(a.spines);
        const glm::vec3 base = pose.shoulders + (pose.rump - pose.shoulders) * t + glm::vec3(0.0f, a.depth * 0.45f, 0.0f);
        // Raked back, the way a spine on an animal that moves forwards is.
        const glm::vec3 tip = base + glm::normalize(glm::vec3(0.0f, 1.0f, 0.45f)) * a.spineLength;
        add(Primitives::Frustum(a.spineLength * 0.16f, 0.0f, a.spineLength, 8), plate, PieceKind::Body,
            Between(base, tip));
    }

    // The neck, the head and its jaw, the eyes and the frills: the front end, which rears back before
    // it strikes.
    const glm::vec3 headBack = pose.head + glm::vec3(0.0f, 0.0f, a.headLength * 0.35f);
    add(Primitives::Capsule(a.neckThickness * 0.5f, glm::distance(pose.neckBase, headBack) + a.neckThickness, 10, 6),
        skin, PieceKind::Front, Between(pose.neckBase, headBack));
    add(Primitives::Ellipsoid({a.headWidth * 0.5f, a.headDepth * 0.5f, a.headLength * 0.5f}, 14, 9), skin,
        PieceKind::Front, glm::translate(glm::mat4(1.0f), pose.head));
    add(Primitives::Ellipsoid({a.headWidth * 0.38f, a.headDepth * 0.2f, a.jawLength * 0.5f}, 12, 7), under,
        PieceKind::Front, glm::translate(glm::mat4(1.0f), pose.jaw));
    for (const glm::vec3& eye : pose.eyes)
    {
        // A little proud of the head, so the head does not swallow them.
        const glm::vec3 out = pose.head + (eye - pose.head) * 1.05f;
        add(Primitives::Sphere(a.eyeSize * 0.8f, 8, 6), eyes, PieceKind::Front, glm::translate(glm::mat4(1.0f), out));
    }
    if (a.frills > 0.05f)
    {
        for (const float side : {-1.0f, 1.0f})
        {
            const glm::vec3 at = pose.head + glm::vec3(side * a.headWidth * 0.55f, a.headDepth * 0.1f, a.headLength * 0.15f);
            const glm::vec3 radii{0.012f, 0.03f + a.headDepth * 0.7f * a.frills, 0.03f + a.headLength * 0.5f * a.frills};
            add(Primitives::Ellipsoid(radii, 10, 6), under, PieceKind::Front,
                glm::translate(glm::mat4(1.0f), at) *
                    glm::mat4_cast(glm::angleAxis(side * glm::radians(-25.0f), glm::vec3(0.0f, 0.0f, 1.0f))));
        }
    }

    // The tail, in tapering sections, drooping more the further it goes.
    if (a.tailLength > 0.05f)
    {
        const float section = a.tailLength / static_cast<float>(std::max(a.tailSegments, 1));
        glm::vec3 from = pose.tailBase;
        glm::vec3 direction = pose.tailDirection;
        for (int s = 0; s < a.tailSegments; ++s)
        {
            const float t0 = static_cast<float>(s) / static_cast<float>(a.tailSegments);
            const float t1 = static_cast<float>(s + 1) / static_cast<float>(a.tailSegments);
            const glm::vec3 to = from + direction * section;
            add(Primitives::Frustum(a.tailThickness * (1.0f - 0.8f * t0), a.tailThickness * (1.0f - 0.8f * t1), section * 1.08f, 10),
                skin, PieceKind::Body, Between(from, to));
            from = to;
            direction = glm::normalize(direction + glm::vec3(0.0f, -0.07f, 0.0f));
        }
    }

    // Legs: three pieces each, placed every frame by solving the leg to where its foot should be.
    for (size_t i = 0; i < pose.legs.size(); ++i)
    {
        const LegPair& pair = *pose.legs[i].pair;
        const int leg = static_cast<int>(i);
        add(Primitives::Capsule(pair.thickness, pair.upper + pair.thickness, 10, 6), skin, PieceKind::LegUpper,
            glm::mat4(1.0f), leg);
        add(Primitives::Capsule(pair.thickness * 0.8f, pair.lower + pair.thickness * 0.8f, 10, 6), under,
            PieceKind::LegLower, glm::mat4(1.0f), leg);
        add(Primitives::Capsule(pair.thickness * 0.7f, pair.foot + pair.thickness * 0.7f, 8, 5), under,
            PieceKind::LegFoot, glm::mat4(1.0f), leg);
    }
}

void Creature::TakeDamage(float amount, int byPlayer, const glm::vec3& from, float time)
{
    if (!Alive())
    {
        return;
    }
    // Plates stop part of every round.
    amount *= 1.0f - m_caps.armour;
    m_health = std::max(m_health - amount, 0.0f);
    m_brain.OnDamaged(amount, byPlayer, from, time, m_maxHealth);
    if (!Alive())
    {
        // Dead for real. The mind stops with it -- nothing after this perceives or decides -- which
        // is what makes lying still as a trick a different thing from this.
        m_brain.OnDied(time);
        m_speed = 0.0f;
        m_windup = 0.0f;
        m_crouchTarget = 0.0f;
    }
}

void Creature::SetShownState(const glm::vec3& position, float yaw, float speed, float windup, bool alive,
                             bool down, float crouch)
{
    m_stride += Horizontal(m_position, position) * m_strideRate;
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
    const float lead = m_received.alive ? std::min(m_receivedAge, 0.1f) : 0.0f;
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
    senses.position = m_position;
    senses.eye = Eye();
    senses.forward = Forward();
    senses.healthFraction = m_health / m_maxHealth;
    senses.nav = m_nav;

    m_brain.Update(senses, dt);
    const CreatureIntent& intent = m_brain.Intent();
    m_windup = intent.windup;
    m_down = intent.down;
    m_crouchTarget = intent.crouch;
    Move(intent, senses.others, dt);
    SyncBody(dt);
}

void Creature::Move(const CreatureIntent& intent, const std::vector<glm::vec3>& others, float dt)
{
    m_routeAge += dt;
    glm::vec3 heading{0.0f};
    float wanted = 0.0f;

    if (intent.move && m_nav != nullptr)
    {
        if (m_route.empty() || m_routeAge > 0.6f || Horizontal(m_routeGoal, intent.destination) > 0.8f)
        {
            m_nav->FindPath(m_position, intent.destination, m_route);
            m_routeAge = 0.0f;
            m_routeGoal = intent.destination;
        }
        // Corners already reached are behind it, including the first, which is where it stands.
        while (!m_route.empty() && Horizontal(m_route.front(), m_position) < 0.35f)
        {
            m_route.erase(m_route.begin());
        }
        const glm::vec3 next = m_route.empty() ? intent.destination : m_route.front();
        const glm::vec3 toward{next.x - m_position.x, 0.0f, next.z - m_position.z};
        if (glm::length(toward) > 0.05f)
        {
            heading = glm::normalize(toward);
            wanted = intent.speed;
        }
    }
    else
    {
        m_route.clear();
    }
    m_brain.Route() = m_route;

    // Getting up to speed and stopping take a moment, both ways. Faster to stop than to start,
    // because what it stops for is usually something in front of it.
    const float rate = wanted > m_speed ? 10.0f : 14.0f;
    m_speed += std::clamp(wanted - m_speed, -rate * dt, rate * dt);

    // Turning: towards whatever it is looking at, or else the way it is going.
    glm::vec3 lookAlong = heading;
    if (intent.face)
    {
        lookAlong = glm::vec3(intent.facePoint.x - m_position.x, 0.0f, intent.facePoint.z - m_position.z);
    }
    if (glm::length(lookAlong) > 0.05f)
    {
        lookAlong = glm::normalize(lookAlong);
        const float target = std::atan2(lookAlong.x, -lookAlong.z);
        const float turn = WrapAngle(target - m_yaw);
        m_yaw = WrapAngle(m_yaw + std::clamp(turn, -kTurnRate * dt, kTurnRate * dt));
    }

    if (m_speed > 0.01f && glm::length(heading) > 0.5f)
    {
        // Slower while it is still turning to face the way it is going, so it turns and then runs
        // rather than running sideways.
        const float aligned = std::clamp(glm::dot(Forward(), heading), 0.3f, 1.0f);
        const glm::vec3 step = heading * m_speed * aligned * dt;
        glm::vec3 moved = m_position + step;
        if (m_nav == nullptr || !m_nav->MoveAlongSurface(m_position, m_position + step, moved))
        {
            moved = m_position + step;
        }
        m_stride += Horizontal(m_position, moved) * m_strideRate;
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
        const glm::vec3 step = push * std::min(3.0f * dt, 1.0f);
        glm::vec3 moved = m_position + step;
        if (m_nav == nullptr || !m_nav->MoveAlongSurface(m_position, m_position + step, moved))
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
    // Lying down with the drawing, so a round aimed at a body on the floor finds it there -- and
    // a creature only playing dead can be shot where it lies.
    const glm::quat facing = YawRotation(m_yaw);
    Transform body;
    body.rotation = facing * glm::angleAxis(m_collapse * glm::radians(84.0f) * m_fallSide, glm::vec3(0.0f, 0.0f, 1.0f));
    const float sink = m_caps.bodyHalfExtents.y * 0.5f * m_collapse;
    body.position = m_position + facing * (m_caps.bodyCentre + glm::vec3(0.0f, -sink, 0.0f));
    if (dt > 0.0f)
    {
        m_physics.MoveKinematic(m_body, body, dt);
    }
    else
    {
        m_physics.SetTransform(m_body, body);
    }
}

void Creature::UpdateVisual(float dt)
{
    m_shownTime += dt;
    const glm::quat facing = YawRotation(m_yaw);
    // How much it is striding: nothing standing still, all of it at a walk and above.
    const float pace = std::clamp(m_speed / std::max(m_caps.walkSpeed, 0.1f), 0.0f, 1.0f);

    // Going down rolls it onto its side and lets it down to the floor in a little over half a second;
    // getting up takes a little longer. Dead and playing dead are the same fall, deliberately.
    const float lying = Down() ? 1.0f : 0.0f;
    if (lying > 0.5f && m_collapse <= 0.0f)
    {
        // Which way to go over, decided as it starts to: towards whichever side has the more room. A
        // body that fell into the wall or the ramp beside it was drawn inside it, and a creature
        // playing dead that could not be seen was not playing anything.
        const glm::quat turned = YawRotation(m_yaw);
        const glm::vec3 from = m_position + glm::vec3(0.0f, m_anatomy.hipHeight * 0.6f, 0.0f);
        const glm::vec3 left = turned * glm::vec3(-1.0f, 0.0f, 0.0f);
        const float reach = std::max(m_anatomy.hipHeight + m_anatomy.depth, 0.8f) * 1.4f;
        const auto room = [&](const glm::vec3& side)
        {
            const RayHit hit = m_physics.RayCast(from, side, reach, m_body);
            return hit ? hit.distance : reach;
        };
        // A positive roll takes it over onto its left side.
        m_fallSide = room(left) >= room(-left) ? 1.0f : -1.0f;
    }
    const float wasLying = m_collapse;
    const float rate = lying > m_collapse ? 1.0f / 0.55f : 1.0f / 0.8f;
    m_collapse += std::clamp(lying - m_collapse, -rate * dt, rate * dt);
    if (m_collapse != wasLying)
    {
        SyncBody(0.0f);
    }
    // Eased so the fall starts slowly and lands hard, which is how a weight goes over.
    const float collapse = m_collapse * m_collapse * (3.0f - 2.0f * m_collapse);
    const float standing = 1.0f - collapse;

    // Stalking, it creeps: the body let down towards the floor, which the legs take up by bending --
    // they are solved to the ground below, so lowering the body is the whole of a crouch.
    m_crouch += (m_crouchTarget - m_crouch) * (1.0f - std::exp(-6.0f * dt));
    const float drop = m_anatomy.hipHeight * 0.35f * m_crouch * standing;
    // A step lifts the body a little, twice a stride.
    const float bob = std::abs(std::sin(m_stride)) * 0.025f * m_anatomy.hipHeight * pace * standing;
    const glm::vec3 lift{0.0f, bob - drop, 0.0f};
    // Before a strike it draws back and up, which is the thing a player watching it learns to read.
    const glm::vec3 rear = glm::vec3(0.0f, 0.1f, 0.25f) * m_windup * std::max(m_anatomy.headLength * 2.0f, 0.6f);

    // Rolled about a point in the middle of its body, not about its feet: rolled about the feet, what
    // had been its back ended up at ankle height and the body went through the floor.
    const glm::vec3 pivot{0.0f, m_anatomy.hipHeight * 0.55f, 0.0f};
    const glm::quat roll = glm::angleAxis(collapse * glm::radians(84.0f) * m_fallSide, glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::mat4 toWorld = glm::translate(glm::mat4(1.0f), m_position) * glm::mat4_cast(facing) *
                              glm::translate(glm::mat4(1.0f), pivot) * glm::mat4_cast(roll) *
                              glm::translate(glm::mat4(1.0f), -pivot);

    // Where each leg's joints are, in the body's frame. The feet stay on the ground while the body is
    // up; lying down, they hang where they rest, and the whole thing goes over together.
    struct Joints
    {
        glm::vec3 hip{0.0f};
        glm::vec3 knee{0.0f};
        glm::vec3 ankle{0.0f};
        glm::vec3 toe{0.0f};
    };
    std::vector<Joints> legs(m_rest.legs.size());
    for (size_t i = 0; i < m_rest.legs.size(); ++i)
    {
        const CreatureAnatomy::Leg& leg = m_rest.legs[i];
        const LegPair& pair = *leg.pair;
        // Which half of the cycle this leg is in. Every body walks the same rule: a pair's two legs are
        // opposite, and each pair is opposite the one in front of it -- a four-legged walk is diagonal,
        // six legs make two tripods, two legs alternate.
        const int pairIndex = static_cast<int>(i / 2);
        const int sideIndex = leg.side > 0.0f ? 1 : 0;
        const float phase = m_stride + static_cast<float>((pairIndex + sideIndex) % 2) * glm::pi<float>();
        // Half a stride either way of where the foot rests, backwards while it is down and forwards
        // while it is lifted.
        const float amplitude = standing * pace / m_strideRate;
        const float raise = std::max(0.0f, -std::cos(phase)) * (pair.upper + pair.lower) * 0.14f * pace * standing;

        Joints& joints = legs[i];
        joints.hip = leg.hip + lift;
        const glm::vec3 foot = leg.foot + glm::vec3(0.0f, raise, amplitude * std::sin(phase));
        const glm::vec3 ankleTarget = foot + glm::vec3(0.0f, pair.thickness, 0.0f);

        // Which way the knee goes: forwards like a person's, backwards like a dog's hind leg, or -- six
        // legs splayed wide -- up and out.
        glm::vec3 pole = pair.backwardKnee ? glm::vec3(0.0f, 0.2f, 1.0f) : glm::vec3(0.0f, 0.2f, -1.0f);
        if (m_anatomy.plan == BodyPlan::Hexapod)
        {
            pole = glm::vec3(leg.side, 1.0f, 0.0f);
        }
        const TwoBoneIKResult ik = SolveTwoBoneIK(joints.hip, ankleTarget, glm::normalize(pole), pair.upper, pair.lower);
        joints.knee = ik.jointPosition;
        joints.ankle = ik.endPosition;
        joints.toe = joints.ankle + glm::vec3(0.0f, -pair.thickness * 0.5f, -pair.foot);
    }

    for (const Piece& piece : m_pieces)
    {
        Transform* transform = m_scene.GetTransform(piece.entity);
        if (transform == nullptr)
        {
            continue;
        }
        glm::mat4 local = piece.local;
        switch (piece.kind)
        {
        case PieceKind::Body:
            local = glm::translate(glm::mat4(1.0f), lift) * local;
            break;
        case PieceKind::Front:
            // The head end hangs lower still while it creeps.
            local = glm::translate(glm::mat4(1.0f), lift + rear +
                                                        glm::vec3(0.0f, -0.06f * m_crouch * standing, 0.0f)) *
                    local;
            break;
        case PieceKind::LegUpper:
            local = Between(legs[static_cast<size_t>(piece.leg)].hip, legs[static_cast<size_t>(piece.leg)].knee);
            break;
        case PieceKind::LegLower:
            local = Between(legs[static_cast<size_t>(piece.leg)].knee, legs[static_cast<size_t>(piece.leg)].ankle);
            break;
        case PieceKind::LegFoot:
            local = Between(legs[static_cast<size_t>(piece.leg)].ankle, legs[static_cast<size_t>(piece.leg)].toe);
            break;
        }
        const glm::mat4 world = toWorld * local;
        // The eyes go out as it goes down, and come back on as it gets up. Dead or pretending, the
        // same: a body lying there with its eyes lit is not dead, and the players should not be able
        // to tell which it is.
        if (piece.glow != glm::vec3(0.0f))
        {
            if (MeshRenderer* renderer = m_scene.GetMeshRenderer(piece.entity))
            {
                renderer->material.emissive = piece.glow * standing;
            }
        }
        transform->position = glm::vec3(world[3]);
        transform->rotation = glm::quat_cast(glm::mat3(world));
    }
}

std::string Creature::DescribePose() const
{
    glm::vec3 torso{0.0f};
    if (!m_pieces.empty())
    {
        if (const Transform* transform = m_scene.GetTransform(m_pieces.front().entity))
        {
            torso = transform->position;
        }
    }
    char line[320];
    std::snprintf(line, sizeof(line),
                  "at %.2f %.2f %.2f yaw %.0f deg  %s  fallen %.2f  crouch %.2f  torso drawn at %.2f %.2f %.2f  "
                  "(%s, %.0f of %.0f health)",
                  m_position.x, m_position.y, m_position.z, glm::degrees(m_yaw),
                  !Alive() ? "dead" : (m_down ? "lying still" : "up"), m_collapse, m_crouch, torso.x, torso.y,
                  torso.z, BodyPlanName(m_anatomy.plan), m_health, m_maxHealth);
    return line;
}

} // namespace pred
