#include "Game/Player/SupportDrone.h"

#include "Engine/Render/Material.h"
#include "Engine/Render/Primitives.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

namespace
{

// The body the level stops: the hull between its tracks, about a shoebox and a half.
constexpr glm::vec3 kHalfExtents{0.2f, 0.09f, 0.26f};
constexpr float kMass = 12.0f;
constexpr float kDensity = kMass / (8.0f * kHalfExtents.x * kHalfExtents.y * kHalfExtents.z);

// Where the camera head turns, on top of its mast, and where the lens is on the head.
constexpr glm::vec3 kHeadPivot{0.0f, 0.27f, 0.07f};
constexpr glm::vec3 kLens{0.0f, 0.035f, -0.1f};

// How it drives: a walking pace at best, turning on the spot as tracks do.
constexpr float kTopSpeed = 2.2f;
constexpr float kTurnSpeed = 3.2f;
constexpr float kGrip = 8.0f;
constexpr float kBrake = 10.0f;
// How much it grips: little on its tracks, which roll, and a lot on its hull, which does not.
constexpr float kTrackFriction = 0.1f;
constexpr float kHullFriction = 0.7f;
// How long a blow flings it before its tracks bite again.
constexpr float kShovedSeconds = 0.5f;
// How far the camera tips on its mast.
constexpr float kMostPitch = glm::radians(SupportDrone::kMostPitchDegrees);
// How long on its back or side before it rights itself unasked, and how long between tries.
constexpr float kRightsItselfAfter = 3.0f;
constexpr float kRightingEvery = 1.5f;
constexpr float kRightingSeconds = 0.9f;
// A hop: about a third of a metre up, and not again straight away.
constexpr float kHopSpeed = 3.4f;
constexpr float kHopEvery = 0.9f;
// For how long after a hop it is still driven, through the air: what it was driving at carries it on, up onto the
// step or the ledge it hopped at, rather than straight up the face of it and back down.
constexpr float kHopSteerSeconds = 0.75f;

uint32_t Paint(const glm::vec3& colour, float roughness = 1.0f)
{
    const auto channel = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return channel(colour.r) | (channel(colour.g) << 8) | (channel(colour.b) << 16) | (channel(roughness) << 24);
}

void AddPart(MeshData& into, MeshData part, const glm::mat4& where, const glm::vec3& colour, float roughness = 1.0f)
{
    const uint32_t paint = Paint(colour, roughness);
    for (MeshVertex& vertex : part.vertices)
    {
        vertex.color = paint;
    }
    into.Append(part, where);
}

glm::mat4 At(const glm::vec3& position)
{
    return glm::translate(glm::mat4(1.0f), position);
}

// A tube lying along Z (the way it faces) or along X (across it), from Primitives' upright ones.
glm::mat4 AlongZ(const glm::vec3& position)
{
    return glm::rotate(At(position), glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
}

glm::mat4 AlongX(const glm::vec3& position)
{
    return glm::rotate(At(position), glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
}

// Painted per piece, so the chassis is one mesh and one draw, like the creatures are.
MeshData BuildChassis()
{
    const glm::vec3 paint{0.52f, 0.46f, 0.28f};
    const glm::vec3 paintDark{0.36f, 0.32f, 0.2f};
    const glm::vec3 rubber{0.05f, 0.05f, 0.05f};
    const glm::vec3 steel{0.32f, 0.33f, 0.34f};

    MeshData chassis;
    // The hull, with a lid over it and a bumper across its nose.
    AddPart(chassis, Primitives::Box({0.26f, 0.11f, 0.44f}), At({0.0f, 0.015f, 0.0f}), paint);
    AddPart(chassis, Primitives::Box({0.22f, 0.012f, 0.3f}), At({0.0f, 0.075f, 0.04f}), paintDark);
    AddPart(chassis, Primitives::Box({0.3f, 0.035f, 0.03f}), At({0.0f, -0.02f, -0.245f}), steel, 0.6f);
    AddPart(chassis, Primitives::Box({0.3f, 0.035f, 0.03f}), At({0.0f, -0.02f, 0.245f}), steel, 0.6f);
    // Two tracks, each a belt round a drive wheel at either end and three rollers between.
    for (const float side : {-1.0f, 1.0f})
    {
        const float x = side * 0.165f;
        AddPart(chassis, Primitives::Box({0.07f, 0.1f, 0.4f}), At({x, -0.03f, 0.0f}), rubber);
        AddPart(chassis, Primitives::Cylinder(0.05f, 0.07f, 14), AlongX({x, -0.03f, -0.2f}), rubber);
        AddPart(chassis, Primitives::Cylinder(0.05f, 0.07f, 14), AlongX({x, -0.03f, 0.2f}), rubber);
        for (int roller = -1; roller <= 1; ++roller)
        {
            AddPart(chassis, Primitives::Cylinder(0.026f, 0.074f, 10),
                    AlongX({x, -0.045f, static_cast<float>(roller) * 0.12f}), steel, 0.5f);
        }
        AddPart(chassis, Primitives::Cylinder(0.028f, 0.075f, 10), AlongX({x, -0.03f, -0.2f}), steel, 0.5f);
        AddPart(chassis, Primitives::Cylinder(0.028f, 0.075f, 10), AlongX({x, -0.03f, 0.2f}), steel, 0.5f);
    }
    // The mast the camera sits on, on a collar, and a whip antenna at the back.
    AddPart(chassis, Primitives::Cylinder(0.035f, 0.03f, 12), At({kHeadPivot.x, 0.095f, kHeadPivot.z}), steel, 0.5f);
    AddPart(chassis, Primitives::Cylinder(0.014f, kHeadPivot.y - 0.1f, 8),
            At({kHeadPivot.x, (kHeadPivot.y + 0.1f) * 0.5f, kHeadPivot.z}), steel, 0.5f);
    AddPart(chassis, Primitives::Cylinder(0.004f, 0.28f, 6), At({0.09f, 0.22f, 0.18f}), rubber);
    AddPart(chassis, Primitives::Sphere(0.009f, 8, 6), At({0.09f, 0.36f, 0.18f}), paintDark);
    // Something the arm uses to right it: a short strut folded along its back.
    AddPart(chassis, Primitives::Box({0.03f, 0.02f, 0.18f}), At({-0.08f, 0.09f, 0.12f}), steel, 0.5f);
    return chassis;
}

// The camera head, from its pivot on the mast, looking along -Z.
MeshData BuildHead()
{
    const glm::vec3 housing{0.13f, 0.13f, 0.14f};
    const glm::vec3 paint{0.52f, 0.46f, 0.28f};
    const glm::vec3 glass{0.02f, 0.02f, 0.025f};
    const glm::vec3 lampRim{0.5f, 0.5f, 0.48f};

    MeshData head;
    AddPart(head, Primitives::Box({0.12f, 0.075f, 0.11f}), At({0.0f, 0.035f, -0.025f}), housing);
    AddPart(head, Primitives::Box({0.125f, 0.012f, 0.1f}), At({0.0f, 0.078f, -0.02f}), paint);
    AddPart(head, Primitives::Cylinder(0.03f, 0.03f, 16), AlongZ({kLens.x, kLens.y, -0.085f}), housing);
    AddPart(head, Primitives::Cylinder(0.022f, 0.012f, 16), AlongZ({kLens.x, kLens.y, -0.098f}), glass, 0.1f);
    // The lamp beside the lens.
    AddPart(head, Primitives::Box({0.035f, 0.026f, 0.03f}), At({0.045f, 0.06f, -0.075f}), lampRim, 0.5f);
    // Its pivot: a yoke either side.
    AddPart(head, Primitives::Box({0.015f, 0.05f, 0.04f}), At({-0.068f, 0.01f, 0.0f}), housing);
    AddPart(head, Primitives::Box({0.015f, 0.05f, 0.04f}), At({0.068f, 0.01f, 0.0f}), housing);
    return head;
}

float Wrap(float angle)
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

// Which way a rotation points on the flat, as a yaw the same way the player's look is measured: zero
// looking down -Z, growing towards +X.
float HeadingOf(const glm::quat& rotation)
{
    const glm::vec3 forward = rotation * glm::vec3(0.0f, 0.0f, -1.0f);
    if (forward.x * forward.x + forward.z * forward.z < 1.0e-4f)
    {
        // Nose straight up or down: the flat heading of its back instead.
        const glm::vec3 up = rotation * glm::vec3(0.0f, 1.0f, 0.0f);
        return std::atan2(-up.x * glm::sign(forward.y), up.z * glm::sign(forward.y));
    }
    return std::atan2(forward.x, -forward.z);
}

} // namespace

void SupportDrone::Deploy(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, const glm::vec3& at, float yaw)
{
    Remove(scene, physics);

    Transform transform;
    transform.position = at + glm::vec3(0.0f, kHalfExtents.y + 0.12f, 0.0f);
    transform.rotation = glm::angleAxis(-yaw, glm::vec3(0.0f, 1.0f, 0.0f));
    m_body = physics.CreateBox(kHalfExtents, transform, BodyMotion::Dynamic, kDensity);
    if (!m_body.IsValid())
    {
        return;
    }
    Build(scene, meshes, transform);
    m_proxy = false;
    m_lookYaw = yaw;
}

void SupportDrone::DeployProxy(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, const glm::vec3& at,
                               const glm::quat& rotation)
{
    Remove(scene, physics);

    Transform transform;
    transform.position = at;
    transform.rotation = rotation;
    // Only somewhere for a round to find it: it touches nothing, so nothing here can push it about or be
    // pushed by it -- its owner's machine decides where it goes -- and it is put where it is told rather
    // than driven there.
    m_body = physics.CreateBox(kHalfExtents, transform, BodyMotion::Kinematic, kDensity, PhysicsLayer::Hitbox);
    if (!m_body.IsValid())
    {
        return;
    }
    Build(scene, meshes, transform);
    m_proxy = true;
    m_sentPosition = transform.position;
    m_sentRotation = transform.rotation;
    m_sentHeadYaw = 0.0f;
    m_sentHeadPitch = 0.0f;
}

void SupportDrone::Build(Scene& scene, MeshLibrary& meshes, const Transform& transform)
{
    const MeshHandle chassis = meshes.Upload(BuildChassis(), "support_drone_chassis");
    const MeshHandle head = meshes.Upload(BuildHead(), "support_drone_head");
    const MeshHandle lens = meshes.Upload(Primitives::Sphere(0.013f, 10, 8), "support_drone_lens");
    const MeshHandle lamp = meshes.Upload(Primitives::Box({0.026f, 0.018f, 0.006f}), "support_drone_lamp");

    Material painted = Material::Diffuse(glm::vec3(1.0f), 0.7f);
    m_chassisParts.push_back(scene.CreateMeshEntity("support_drone", transform, chassis, painted));
    m_chassisRest.push_back(glm::vec3(0.0f));

    m_headParts.push_back(scene.CreateMeshEntity("support_drone_head", transform, head, painted));
    m_headRest.push_back(glm::vec3(0.0f));
    m_headParts.push_back(scene.CreateMeshEntity("support_drone_lens", transform, lens,
                                                 Material::Emissive({0.55f, 0.85f, 1.0f}, 1.5f)));
    m_headRest.push_back(kLens + glm::vec3(0.0f, 0.0f, -0.004f));
    m_headParts.push_back(scene.CreateMeshEntity("support_drone_lamp", transform, lamp,
                                                 Material::Diffuse({0.6f, 0.6f, 0.55f}, 0.3f)));
    m_headRest.push_back({0.045f, 0.06f, -0.091f});

    m_active = true;
    m_position = transform.position;
    m_rotation = transform.rotation;
    m_health = kMaxHealth;
    m_rebootLeft = 0.0f;
    m_upendedFor = 0.0f;
    m_rightingCooldown = 0.0f;
    m_trackSpeed = 0.0f;
    m_trackTurn = 0.0f;
    m_shovedFor = 0.0f;
    m_rightingFor = 0.0f;
    m_hopCooldown = 0.0f;
    m_hopFor = 0.0f;
    m_friction = -1.0f;
    m_lookYaw = 0.0f;
    m_lookPitch = 0.0f;
    m_headYaw = 0.0f;
    m_headPitch = 0.0f;
    lightOn = false;
}

void SupportDrone::SetHeadHidden(Scene& scene, bool hidden)
{
    // Its driver looks out of the lens, so the head it is in is not drawn for them -- it still throws its
    // shadow, as a player's own head does.
    for (const Entity part : m_headParts)
    {
        if (MeshRenderer* renderer = scene.GetMeshRenderer(part))
        {
            renderer->hiddenFromCamera = hidden;
        }
    }
}

void SupportDrone::Remove(Scene& scene, PhysicsWorld& physics)
{
    for (const Entity part : m_chassisParts)
    {
        scene.Destroy(part);
    }
    for (const Entity part : m_headParts)
    {
        scene.Destroy(part);
    }
    m_chassisParts.clear();
    m_chassisRest.clear();
    m_headParts.clear();
    m_headRest.clear();
    if (m_body.IsValid() && physics.IsValid(m_body))
    {
        physics.DestroyBody(m_body);
    }
    m_body = BodyHandle{};
    m_active = false;
    m_proxy = false;
}

void SupportDrone::Step(PhysicsWorld& physics, const Controls& controls, float dt)
{
    if (!m_active || m_proxy || !physics.IsValid(m_body))
    {
        return;
    }
    const Transform transform = physics.GetTransform(m_body);
    m_position = transform.position;
    m_rotation = transform.rotation;
    m_lookYaw = controls.lookYaw;
    m_lookPitch = std::clamp(controls.lookPitch, -kMostPitch, kMostPitch);

    if (m_rebootLeft > 0.0f)
    {
        m_rebootLeft -= dt;
        if (m_rebootLeft <= 0.0f)
        {
            m_rebootLeft = 0.0f;
            m_health = kHealthAfterReboot;
        }
    }
    m_rightingCooldown = std::max(m_rightingCooldown - dt, 0.0f);
    m_hopCooldown = std::max(m_hopCooldown - dt, 0.0f);

    const glm::vec3 worldUp{0.0f, 1.0f, 0.0f};
    const glm::vec3 up = m_rotation * worldUp;
    const bool upright = up.y > 0.7f;
    glm::vec3 velocity = physics.GetLinearVelocity(m_body);
    glm::vec3 spin = physics.GetAngularVelocity(m_body);

    // On its tracks and on the ground, which is the only time they do anything.
    const RayHit ground = physics.RayCast(m_position, -worldUp, kHalfExtents.y + 0.08f, m_body);
    const bool grounded = upright && ground.hit;

    // On its back or side, and not moving: it counts, and it gets up by itself if left there, or when
    // its driver asks. The arm kicks it off the floor and rolls it over the nearer way.
    if (!upright && glm::length(velocity) < 0.6f)
    {
        m_upendedFor += dt;
    }
    else if (upright)
    {
        m_upendedFor = 0.0f;
    }
    if (Upended() && !Disabled() && m_rightingCooldown <= 0.0f &&
        (controls.rightItself || m_upendedFor > kRightsItselfAfter))
    {
        // Kicked up off the floor, so it turns over clear of it rather than grinding round on its edge.
        physics.SetLinearVelocity(m_body, velocity + worldUp * 2.0f);
        m_rightingFor = kRightingSeconds;
        m_rightingCooldown = kRightingEvery;
        m_upendedFor = 0.0f;
        return;
    }
    if (m_rightingFor > 0.0f)
    {
        // And rolled over the nearer way, as fast as it is far from upright, so it arrives upright
        // rather than going over the top onto its other side.
        m_rightingFor -= dt;
        const float over = std::acos(std::clamp(up.y, -1.0f, 1.0f));
        glm::vec3 axis = glm::cross(up, worldUp);
        if (glm::dot(axis, axis) < 1.0e-4f)
        {
            // Flat on its back: over its side.
            axis = m_rotation * glm::vec3(0.0f, 0.0f, -1.0f);
        }
        const glm::vec3 wantedTip = glm::normalize(axis) * std::min(over * 8.0f, 9.0f);
        const glm::vec3 about = up * glm::dot(spin, up);
        glm::vec3 tip = spin - about;
        tip += (wantedTip - tip) * (1.0f - std::exp(-20.0f * dt));
        physics.SetAngularVelocity(m_body, about * 0.9f + tip);
        if (over < 0.1f && ground.hit)
        {
            m_rightingFor = 0.0f;
        }
        return;
    }

    // Its tracks do anything only on the ground, and not while it is shut down or still being flung by
    // whatever struck it. Off them it grips like the lump it is; on them it rolls.
    m_shovedFor = std::max(m_shovedFor - dt, 0.0f);
    m_hopFor = std::max(m_hopFor - dt, 0.0f);
    const bool driving = grounded && !Disabled() && m_shovedFor <= 0.0f;
    const bool hopping = !grounded && upright && m_hopFor > 0.0f && !Disabled() && m_shovedFor <= 0.0f;
    const bool driven = driving || hopping;
    // Hopping, no grip at all: driven at a ledge and gripping its face, the face held it up -- the hop was braked to
    // a few centimetres, and it hung there on the wall.
    const float friction = driving ? kTrackFriction : hopping ? 0.0f : kHullFriction;
    if (friction != m_friction)
    {
        m_friction = friction;
        physics.SetFriction(m_body, friction);
    }
    if (!driven)
    {
        m_trackSpeed = 0.0f;
        m_trackTurn = 0.0f;
        m_hopFor = 0.0f;
        return;
    }

    // A hop, pushed off its tracks: up along its own up, keeping what it was already doing.
    if (driving && controls.jump && m_hopCooldown <= 0.0f)
    {
        velocity += up * kHopSpeed;
        m_hopCooldown = kHopEvery;
        m_hopFor = kHopSteerSeconds;
    }

    // Driven the way the camera looks, as a person walks: pushed forward it goes where it is looking,
    // pushed sideways it goes that way. It has tracks, so it turns to face the way it is going and then
    // goes, and pulled back it reverses rather than turning round.
    const float heading = HeadingOf(m_rotation);
    const glm::vec3 lookForward{std::sin(m_lookYaw), 0.0f, -std::cos(m_lookYaw)};
    const glm::vec3 lookRight{std::cos(m_lookYaw), 0.0f, std::sin(m_lookYaw)};
    const glm::vec3 wish = lookRight * controls.move.x + lookForward * controls.move.y;
    const float amount = std::min(glm::length(wish), 1.0f);

    float wantedSpeed = 0.0f;
    float wantedTurn = 0.0f;
    if (amount > 0.05f)
    {
        float wishHeading = std::atan2(wish.x, -wish.z);
        float direction = 1.0f;
        float off = Wrap(wishHeading - heading);
        if (std::abs(off) > glm::radians(115.0f))
        {
            direction = -1.0f;
            wishHeading = Wrap(wishHeading + glm::pi<float>());
            off = Wrap(wishHeading - heading);
        }
        wantedTurn = std::clamp(off * 5.0f, -kTurnSpeed, kTurnSpeed);
        const float aligned = std::clamp((std::cos(off) - 0.4f) / 0.6f, 0.0f, 1.0f);
        wantedSpeed = direction * kTopSpeed * amount * aligned;
    }
    m_trackSpeed += (wantedSpeed - m_trackSpeed) * (1.0f - std::exp(-(amount > 0.05f ? kGrip : kBrake) * dt));
    m_trackTurn += (wantedTurn - m_trackTurn) * (1.0f - std::exp(-12.0f * dt));

    // The tracks say how it moves along the way it points and how it turns about its own up, whatever
    // the floor made of the last tick. Up and down along its own up -- falling, bumping over a step -- is
    // left to the physics, as is any tipping.
    const glm::vec3 forward = m_rotation * glm::vec3(0.0f, 0.0f, -1.0f);
    physics.SetLinearVelocity(m_body, forward * m_trackSpeed + up * glm::dot(velocity, up));
    spin += up * (-m_trackTurn - glm::dot(spin, up));
    physics.SetAngularVelocity(m_body, spin);
}

void SupportDrone::UpdateVisual(Scene& scene, PhysicsWorld& physics, float dt)
{
    if (!m_active || !physics.IsValid(m_body))
    {
        return;
    }
    if (m_proxy)
    {
        // Eased after where its owner last said, a snapshot at a time, so it glides rather than jumps.
        const float ease = 1.0f - std::exp(-14.0f * dt);
        m_position += (m_sentPosition - m_position) * ease;
        if (glm::distance(m_position, m_sentPosition) > 3.0f)
        {
            m_position = m_sentPosition;
        }
        m_rotation = glm::normalize(glm::slerp(m_rotation, m_sentRotation, ease));
        m_headYaw = Wrap(m_headYaw + Wrap(m_sentHeadYaw - m_headYaw) * ease);
        m_headPitch += (m_sentHeadPitch - m_headPitch) * ease;
        physics.SetTransform(m_body, Transform{m_position, glm::normalize(m_rotation), glm::vec3(1.0f)});
    }
    else
    {
        const Transform transform = physics.GetTransform(m_body);
        m_position = transform.position;
        m_rotation = transform.rotation;
    }

    // The camera stays on where its driver looks while the chassis turns under it. Shut down, it sags
    // on its mast and stops following. (A proxy's head is where its owner said, above.)
    if (!m_proxy && Disabled())
    {
        m_headPitch += (-kMostPitch * 0.8f - m_headPitch) * (1.0f - std::exp(-3.0f * dt));
    }
    else if (!m_proxy)
    {
        m_headYaw = Wrap(m_lookYaw - HeadingOf(m_rotation));
        m_headPitch = m_lookPitch;
    }

    for (size_t i = 0; i < m_chassisParts.size(); ++i)
    {
        if (Transform* part = scene.GetTransform(m_chassisParts[i]))
        {
            part->position = m_position + m_rotation * m_chassisRest[i];
            part->rotation = m_rotation;
        }
    }
    const glm::quat headRotation = m_rotation * glm::angleAxis(-m_headYaw, glm::vec3(0.0f, 1.0f, 0.0f)) *
                                   glm::angleAxis(m_headPitch, glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::vec3 pivot = m_position + m_rotation * kHeadPivot;
    for (size_t i = 0; i < m_headParts.size(); ++i)
    {
        if (Transform* part = scene.GetTransform(m_headParts[i]))
        {
            part->position = pivot + headRotation * m_headRest[i];
            part->rotation = headRotation;
        }
    }

    // Its eye glows while it sees, and goes out while it reboots; the lamp shows whether it is on.
    if (m_headParts.size() >= 3)
    {
        if (MeshRenderer* lens = scene.GetMeshRenderer(m_headParts[1]))
        {
            const float blink = Disabled() ? (std::fmod(m_rebootLeft, 1.0f) < 0.15f ? 0.6f : 0.0f) : 1.5f;
            lens->material.emissive = glm::vec3(0.55f, 0.85f, 1.0f) * blink;
        }
        if (MeshRenderer* lamp = scene.GetMeshRenderer(m_headParts[2]))
        {
            lamp->material.emissive = lightOn && !Disabled() ? glm::vec3(1.0f, 0.95f, 0.85f) * 3.0f : glm::vec3(0.0f);
        }
    }
}

void SupportDrone::Hit(PhysicsWorld& physics, const glm::vec3& impulse, float damage)
{
    if (!m_active || !physics.IsValid(m_body))
    {
        return;
    }
    physics.AddImpulse(m_body, impulse);
    m_shovedFor = kShovedSeconds;
    // Struck on one side, not through its middle: it tips, and hard enough it goes over.
    const float strength = glm::length(impulse);
    if (strength > 1.0e-3f)
    {
        const glm::vec3 axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), impulse / strength);
        physics.SetAngularVelocity(m_body, physics.GetAngularVelocity(m_body) +
                                               axis * std::min(strength / kMass * 3.0f, 14.0f));
    }
    if (Disabled() || damage <= 0.0f)
    {
        return;
    }
    m_health -= damage;
    if (m_health <= 0.0f)
    {
        m_health = 0.0f;
        m_rebootLeft = kRebootSeconds;
    }
}

void SupportDrone::SetPose(const glm::vec3& position, const glm::quat& rotation, float headYaw, float headPitch)
{
    m_sentPosition = position;
    m_sentRotation = rotation;
    m_sentHeadYaw = headYaw;
    m_sentHeadPitch = headPitch;
}

void SupportDrone::SetStatus(float health, float rebootLeft)
{
    m_health = health;
    m_rebootLeft = rebootLeft;
}

glm::quat SupportDrone::HeadRotation() const
{
    return m_rotation * glm::angleAxis(-m_headYaw, glm::vec3(0.0f, 1.0f, 0.0f)) *
           glm::angleAxis(m_headPitch, glm::vec3(1.0f, 0.0f, 0.0f));
}

glm::vec3 SupportDrone::Eye() const
{
    return m_position + m_rotation * kHeadPivot + HeadRotation() * kLens;
}

glm::mat4 SupportDrone::ViewMatrix() const
{
    const glm::mat4 world = glm::translate(glm::mat4(1.0f), Eye()) * glm::mat4_cast(HeadRotation());
    return glm::inverse(world);
}

glm::vec3 SupportDrone::LookDirection() const
{
    return HeadRotation() * glm::vec3(0.0f, 0.0f, -1.0f);
}

} // namespace pred
