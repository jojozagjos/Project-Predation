#include "Game/Creature/Creature.h"

#include "Engine/Navigation/NavMesh.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Primitives.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pred
{
namespace
{

// The box rounds hit, in the creature's own frame: forward is -Z, as it is for everything else.
constexpr glm::vec3 kBodyHalfExtents{0.3f, 0.5f, 0.85f};
constexpr glm::vec3 kBodyCentre{0.0f, 0.62f, -0.1f};
constexpr glm::vec3 kEye{0.0f, 1.0f, -1.08f};

// The walk: how many radians of leg swing per metre covered, and how far a leg swings at a run.
constexpr float kStridePerMetre = 3.6f;
constexpr float kLegSwing = 0.55f;
constexpr float kLegLength = 0.75f;
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

// Where each leg hangs from, and when in the cycle it swings: the diagonal pairs move together,
// which is how four legs walk.
constexpr glm::vec3 kHips[4] = {
    {-0.22f, 0.76f, -0.38f}, {0.22f, 0.76f, -0.38f}, {-0.22f, 0.72f, 0.45f}, {0.22f, 0.72f, 0.45f}};
constexpr float kLegPhase[4] = {0.0f, glm::pi<float>(), glm::pi<float>(), 0.0f};

} // namespace

Creature::Creature(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, const NavMesh* nav,
                   const CreatureTraits& traits, const glm::vec3& spawn)
    : m_scene(scene), m_physics(physics), m_nav(nav), m_brain(traits), m_position(spawn)
{
    glm::vec3 onMesh;
    if (m_nav != nullptr && m_nav->NearestPoint(spawn, 3.0f, onMesh))
    {
        m_position = onMesh;
    }
    Transform body;
    body.position = m_position + YawRotation(m_yaw) * kBodyCentre;
    body.rotation = YawRotation(m_yaw);
    m_body = m_physics.CreateBox(kBodyHalfExtents, body, BodyMotion::Kinematic);
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
    return m_position + YawRotation(m_yaw) * kEye;
}

void Creature::BuildVisual(MeshLibrary& meshes)
{
    Material skin = Material::Diffuse({0.40f, 0.42f, 0.38f}, 0.72f);
    Material dark = Material::Diffuse({0.24f, 0.25f, 0.23f}, 0.8f);
    Material eyes = Material::Diffuse({1.0f, 0.3f, 0.2f}, 0.4f);
    // Faint. Enough to find it by in a dark room, not enough to light the room.
    eyes.emissive = {2.4f, 0.5f, 0.35f};

    const auto add = [&](const char* name, const MeshData& data, const glm::vec3& offset,
                         const Material& material, int leg = -1)
    {
        const MeshHandle mesh = meshes.Upload(data, std::string("creature_") + name);
        Piece piece;
        piece.entity = m_scene.CreateMeshEntity(std::string("creature_") + name, Transform{}, mesh, material);
        piece.offset = offset;
        piece.leg = leg;
        piece.glow = material.emissive;
        if (MeshRenderer* renderer = m_scene.GetMeshRenderer(piece.entity))
        {
            renderer->castsShadow = true;
        }
        m_pieces.push_back(piece);
    };

    add("torso", Primitives::Box({0.55f, 0.45f, 1.15f}), {0.0f, 0.86f, 0.05f}, skin);
    add("shoulders", Primitives::Box({0.62f, 0.36f, 0.45f}), {0.0f, 1.06f, -0.35f}, skin);
    add("head", Primitives::Box({0.34f, 0.30f, 0.50f}), {0.0f, 0.97f, -0.82f}, skin);
    add("jaw", Primitives::Box({0.26f, 0.10f, 0.36f}), {0.0f, 0.79f, -0.90f}, dark);
    add("eye_l", Primitives::Sphere(0.035f, 8, 6), {-0.09f, 1.03f, -1.08f}, eyes);
    add("eye_r", Primitives::Sphere(0.035f, 8, 6), {0.09f, 1.03f, -1.08f}, eyes);
    add("tail", Primitives::Box({0.10f, 0.10f, 0.65f}), {0.0f, 0.9f, 0.9f}, dark);
    for (int leg = 0; leg < 4; ++leg)
    {
        add("leg", Primitives::Cylinder(0.07f, kLegLength, 8), kHips[leg], dark, leg);
    }
}

void Creature::TakeDamage(float amount, int byPlayer, const glm::vec3& from, float time)
{
    if (!Alive())
    {
        return;
    }
    m_health = std::max(m_health - amount, 0.0f);
    m_brain.OnDamaged(amount, byPlayer, from, time);
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
    m_stride += Horizontal(m_position, position) * kStridePerMetre;
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
    Move(intent, dt);
    SyncBody(dt);
}

void Creature::Move(const CreatureIntent& intent, float dt)
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
        m_stride += Horizontal(m_position, moved) * kStridePerMetre;
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
    body.position = m_position + facing * (kBodyCentre + glm::vec3(0.0f, -0.27f * m_collapse, 0.0f));
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
    const float pace = std::clamp(m_speed / 5.0f, 0.0f, 1.0f);

    // Going down rolls it onto its side and sinks it to the floor in a little over half a second;
    // getting up takes a little longer. Dead and playing dead are the same fall, deliberately.
    const float lying = Down() ? 1.0f : 0.0f;
    if (lying > 0.5f && m_collapse <= 0.0f)
    {
        // Which way to go over, decided as it starts to: towards whichever side has the more room. A
        // body that fell into the wall or the ramp beside it was drawn inside it, and a creature
        // playing dead that could not be seen was not playing anything.
        const glm::quat turned = YawRotation(m_yaw);
        const glm::vec3 from = m_position + glm::vec3(0.0f, 0.4f, 0.0f);
        const glm::vec3 left = turned * glm::vec3(-1.0f, 0.0f, 0.0f);
        const auto room = [&](const glm::vec3& side)
        {
            const RayHit hit = m_physics.RayCast(from, side, 1.4f, m_body);
            return hit ? hit.distance : 1.4f;
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

    // Stalking, it creeps: legs splayed out front and back and the body let down between them, the
    // head lowest of all. The drop is exactly what the splay takes off the legs' height, so the feet
    // stay on the floor.
    m_crouch += (m_crouchTarget - m_crouch) * (1.0f - std::exp(-6.0f * dt));
    const float splay = 0.6f * m_crouch * (1.0f - collapse);
    const float drop = kLegLength * (1.0f - std::cos(splay));
    const glm::quat roll = glm::angleAxis(collapse * glm::radians(84.0f) * m_fallSide, glm::vec3(0.0f, 0.0f, 1.0f));
    // Rolled about a point at the middle of its body, hip height, not about its feet: rolled about the
    // feet, what had been its back ended up at the height of its ankles and a sink on top of that put it
    // through the floor -- every death, and every body playing dead, was drawn under the ground.
    const glm::vec3 pivot{0.0f, 0.35f, 0.0f};

    // Before a strike it draws back and up, which is the thing a player watching it learns to read.
    const glm::vec3 rear{0.0f, 0.08f * m_windup, 0.22f * m_windup};
    const float bob = std::abs(std::sin(m_stride)) * 0.035f * pace;

    for (const Piece& piece : m_pieces)
    {
        Transform* transform = m_scene.GetTransform(piece.entity);
        if (transform == nullptr)
        {
            continue;
        }
        glm::vec3 local = piece.offset;
        glm::quat localTurn{1.0f, 0.0f, 0.0f, 0.0f};
        if (piece.leg >= 0)
        {
            // Hanging from its hip and swinging about it.
            const float swing =
                std::sin(m_stride + kLegPhase[piece.leg]) * kLegSwing * pace * (1.0f - collapse);
            // Front legs reach forward and back legs back, so a crouch spreads it out rather than
            // folding it up.
            const float spread = piece.offset.z < 0.0f ? splay : -splay;
            localTurn = glm::angleAxis(swing + spread, glm::vec3(1.0f, 0.0f, 0.0f));
            local = piece.offset + glm::vec3(0.0f, -drop, 0.0f) +
                    localTurn * glm::vec3(0.0f, -kLegLength * 0.5f, 0.0f);
        }
        else
        {
            local += glm::vec3(0.0f, bob - drop, 0.0f);
            if (piece.offset.z < -0.3f)
            {
                local += rear; // the front end: shoulders, head, jaw, eyes
                local.y -= 0.06f * m_crouch * (1.0f - collapse);
            }
        }
        const glm::vec3 posed = pivot + roll * (local - pivot);
        // The eyes go out as it goes down, and come back on as it gets up. Dead or pretending, the
        // same: a body lying there with its eyes lit is not dead, and the players should not be able
        // to tell which it is.
        if (piece.glow != glm::vec3(0.0f))
        {
            if (MeshRenderer* renderer = m_scene.GetMeshRenderer(piece.entity))
            {
                renderer->material.emissive = piece.glow * (1.0f - collapse);
            }
        }
        transform->position = m_position + facing * posed;
        transform->rotation = facing * roll * localTurn;
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
    char line[256];
    std::snprintf(line, sizeof(line),
                  "at %.2f %.2f %.2f yaw %.0f deg  %s  fallen %.2f  crouch %.2f  torso drawn at %.2f %.2f %.2f",
                  m_position.x, m_position.y, m_position.z, glm::degrees(m_yaw),
                  !Alive() ? "dead" : (m_down ? "lying still" : "up"), m_collapse, m_crouch, torso.x, torso.y,
                  torso.z);
    return line;
}

} // namespace pred
