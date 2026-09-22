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
#include <functional>

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

// A ring of a lofted tube: where its middle is, and how far it reaches to the side, over the top and
// underneath. Different above and below is what lets a ribcage be deep and a back be flat.
struct Ring
{
    glm::vec3 centre{0.0f};
    float side = 0.1f;
    float over = 0.1f;
    float under = 0.1f;
};

// One continuous skin through a row of rings, closed at both ends: a torso, a tail, a tendril. One
// surface rather than a row of overlapping shapes is what stops a body looking assembled. `swell`
// pushes the surface out or in at a ring and an angle round it (0 the top, a quarter turn its right
// side, a half turn underneath), for ribs and a spine showing through.
MeshData Loft(const std::vector<Ring>& rings, int around, const std::function<float(size_t, float)>& swell = {})
{
    MeshData mesh;
    const size_t count = rings.size();
    if (count < 2)
    {
        return mesh;
    }
    for (size_t i = 0; i < count; ++i)
    {
        const glm::vec3 ahead = rings[std::min(i + 1, count - 1)].centre - rings[i > 0 ? i - 1 : 0].centre;
        const glm::vec3 along = glm::length(ahead) > 1e-6f ? glm::normalize(ahead) : glm::vec3(0.0f, 0.0f, -1.0f);
        glm::vec3 side = glm::cross(along, glm::vec3(0.0f, 1.0f, 0.0f));
        side = glm::length(side) > 1e-4f ? glm::normalize(side) : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 up = glm::cross(side, along);
        for (int k = 0; k < around; ++k)
        {
            const float angle = static_cast<float>(k) / static_cast<float>(around) * glm::two_pi<float>();
            const float across = std::sin(angle);
            const float vertical = std::cos(angle);
            const float grow = swell ? swell(i, angle) : 1.0f;
            const glm::vec3 offset =
                side * across * rings[i].side + up * vertical * (vertical > 0.0f ? rings[i].over : rings[i].under);
            mesh.vertices.push_back({rings[i].centre + offset * grow, glm::vec3(0.0f),
                                     {static_cast<float>(k) / static_cast<float>(around),
                                      static_cast<float>(i) / static_cast<float>(count - 1)}});
        }
    }
    const auto at = [&](size_t ring, int k)
    { return static_cast<uint32_t>(ring * static_cast<size_t>(around) + static_cast<size_t>((k + around) % around)); };
    for (size_t i = 0; i + 1 < count; ++i)
    {
        for (int k = 0; k < around; ++k)
        {
            mesh.indices.insert(mesh.indices.end(),
                                {at(i, k), at(i, k + 1), at(i + 1, k + 1), at(i, k), at(i + 1, k + 1), at(i + 1, k)});
        }
    }
    // Closed at each end on a point just past the last ring.
    const auto start = static_cast<uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back({rings.front().centre + (rings[0].centre - rings[1].centre) * 0.4f, glm::vec3(0.0f), {0.5f, 0.0f}});
    const auto end = static_cast<uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back({rings.back().centre + (rings[count - 1].centre - rings[count - 2].centre) * 0.4f,
                             glm::vec3(0.0f), {0.5f, 1.0f}});
    for (int k = 0; k < around; ++k)
    {
        mesh.indices.insert(mesh.indices.end(), {start, at(0, k + 1), at(0, k)});
        mesh.indices.insert(mesh.indices.end(), {end, at(count - 1, k), at(count - 1, k + 1)});
    }
    mesh.RecalculateNormals();
    return mesh;
}

// A limb segment from `r0` at one end to `r1` at the other, `length` long along Y and centred, with a
// ball at the far end -- and at the near end too when asked. The segment beyond puts its own near end
// inside that ball, so at any angle the two meet in one rounded joint instead of two ends showing.
// A ball a little wider than the limb is a knuckle or a knee: bony, which suits something starved.
MeshData LimbSegment(float r0, float r1, float length, float endBall, float startBall = 0.0f, int segments = 12)
{
    MeshData mesh = Primitives::Frustum(r0, r1, length, segments);
    const glm::mat4 identity(1.0f);
    mesh.Append(Primitives::Sphere(endBall, segments, 8),
                glm::translate(identity, glm::vec3(0.0f, length * 0.5f, 0.0f)));
    if (startBall > 0.0f)
    {
        mesh.Append(Primitives::Sphere(startBall, segments, 8),
                    glm::translate(identity, glm::vec3(0.0f, -length * 0.5f, 0.0f)));
    }
    return mesh;
}

// A smooth 0..1 blend between numbered keys, for how thick a body is along its length.
struct ProfileKey
{
    float t;
    float side;
    float over;
    float under;
};
glm::vec3 Profile(const std::vector<ProfileKey>& keys, float t)
{
    if (t <= keys.front().t)
    {
        return {keys.front().side, keys.front().over, keys.front().under};
    }
    for (size_t i = 0; i + 1 < keys.size(); ++i)
    {
        if (t <= keys[i + 1].t)
        {
            float u = (t - keys[i].t) / std::max(keys[i + 1].t - keys[i].t, 1e-5f);
            u = u * u * (3.0f - 2.0f * u);
            const glm::vec3 a{keys[i].side, keys[i].over, keys[i].under};
            const glm::vec3 b{keys[i + 1].side, keys[i + 1].over, keys[i + 1].under};
            return a + (b - a) * u;
        }
    }
    return {keys.back().side, keys.back().over, keys.back().under};
}

// A number from 0 to 1 for the n-th of something on this creature: a tooth's length, a finger's
// splay. The same every time, so a creature's teeth do not change between two looks at it.
float Detail(uint32_t seed, uint32_t n)
{
    uint32_t h = seed * 747796405u + n * 2891336453u + 0x9E3779B9u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    return static_cast<float>(h & 0xFFFFu) / 65535.0f;
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
    const bool crawler = a.plan == BodyPlan::Crawler;
    const Material skin = Material::Diffuse(a.skin, a.roughness);
    const Material under = Material::Diffuse(a.underside, std::min(a.roughness + 0.1f, 1.0f));
    const Material plate = Material::Diffuse(a.skin * 0.75f, 0.45f);
    // Bone: the skull, the teeth and the spurs of the spine, yellowed and a little glossy, whatever
    // colour the skin is.
    const Material bone = Material::Diffuse(glm::mix(a.skin, glm::vec3(0.74f, 0.70f, 0.60f), 0.6f), 0.42f);
    const Material claw = Material::Diffuse({0.09f, 0.08f, 0.07f}, 0.3f);
    // The sockets, the nose and the throat: nearly black, so a face is mostly holes.
    const Material hollow = Material::Diffuse({0.012f, 0.008f, 0.008f}, 0.95f);
    const Material throat = Material::Diffuse({0.07f, 0.012f, 0.012f}, 0.6f);
    Material glint = Material::Diffuse({0.0f, 0.0f, 0.0f}, 0.4f);
    // Faint. Enough to find it by in a dark room, not enough to light the room.
    glint.emissive = a.eyeGlow;

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
    const glm::mat4 identity(1.0f);

    // The body: one skin from behind the rump to in front of the shoulders, bowed up in the middle.
    // A crawler's is a person's gone wrong -- a pelvis, a waist sucked in to nothing, a deep ribcage
    // and wide shoulder blades -- and everything else's a long barrel narrowing to the back.
    const float halfWidth = a.width * 0.5f;
    const float halfDepth = a.depth * 0.5f;
    const glm::vec3 back = glm::normalize(pose.rump - pose.shoulders);
    const glm::vec3 rumpEnd = pose.rump + back * (a.length * (crawler ? 0.1f : 0.14f));
    const glm::vec3 shoulderEnd = pose.shoulders - back * (a.length * (crawler ? 0.1f : 0.08f));
    const float arch = a.depth * (crawler ? 0.28f : 0.1f);
    const auto spineAt = [&](float t)
    { return rumpEnd + (shoulderEnd - rumpEnd) * t + glm::vec3(0.0f, arch * std::sin(glm::pi<float>() * t), 0.0f); };
    std::vector<ProfileKey> keys;
    if (crawler)
    {
        keys = {{0.0f, 0.18f, 0.2f, 0.2f},    {0.06f, 0.5f, 0.62f, 0.62f}, {0.15f, 0.64f, 0.8f, 0.86f},
                {0.28f, 0.5f, 0.72f, 0.6f},   {0.4f, 0.4f, 0.68f, 0.46f},  {0.55f, 0.56f, 0.82f, 0.86f},
                {0.72f, 0.64f, 0.92f, 1.0f},  {0.86f, 0.82f, 0.9f, 0.86f}, {0.94f, 0.94f, 0.8f, 0.62f},
                {1.0f, 0.5f, 0.45f, 0.36f}};
    }
    else
    {
        const float rear = 1.0f - a.taper;
        keys = {{0.0f, 0.25f * rear, 0.25f, 0.25f}, {0.08f, 0.8f * rear, 0.78f, 0.78f},
                {0.3f, 0.92f * rear + 0.08f, 0.88f, 0.9f}, {0.7f, 1.0f, 0.98f, 1.0f},
                {0.93f, 0.95f, 0.92f, 0.9f},        {1.0f, 0.45f, 0.45f, 0.42f}};
    }
    constexpr size_t kTorsoRings = 40;
    std::vector<Ring> torso;
    for (size_t i = 0; i < kTorsoRings; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(kTorsoRings - 1);
        const glm::vec3 size = Profile(keys, t);
        torso.push_back({spineAt(t), halfWidth * size.x, halfDepth * size.y, halfDepth * size.z});
    }
    const float ribs = a.ribs;
    const float vertebrae = crawler ? 13.0f : 9.0f + 6.0f * a.ribs;
    const auto torsoSwell = [&](size_t i, float angle)
    {
        const float t = static_cast<float>(i) / static_cast<float>(kTorsoRings - 1);
        const float wrapped = angle > glm::pi<float>() ? angle - glm::two_pi<float>() : angle;
        const float top = std::max(std::cos(angle), 0.0f);
        float grow = 1.0f;
        // Ribs on the sides and underneath, as ridges with hollows between, over the chest.
        const float chestFrom = crawler ? 0.48f : 0.35f;
        const float chestTo = crawler ? 0.9f : 0.92f;
        if (t > chestFrom && t < chestTo)
        {
            const float u = (t - chestFrom) / (chestTo - chestFrom);
            const float ridge = std::pow(std::abs(std::cos(u * (crawler ? 6.0f : 8.0f) * glm::pi<float>())), 6.0f);
            const float sides = std::clamp(1.0f - top * 1.4f, 0.0f, 1.0f);
            grow += (ridge - 0.35f) * 0.13f * ribs * std::sin(u * glm::pi<float>()) * sides;
        }
        // The spine down the middle of the back, a knob for every vertebra.
        const float knobs = 0.55f + 0.45f * std::pow(std::abs(std::sin(t * vertebrae * glm::pi<float>())), 2.0f);
        grow += (0.06f + 0.16f * ribs) * knobs * std::pow(top, 14.0f) * std::sin(glm::pi<float>() * t);
        if (crawler)
        {
            // Shoulder blades standing up either side of the spine, and hip bones either side of it.
            const auto bump = [&](float centreT, float widthT, float centreAngle, float widthAngle)
            {
                const float dt = (t - centreT) / widthT;
                const float da = (std::abs(wrapped) - centreAngle) / widthAngle;
                return std::exp(-dt * dt - da * da);
            };
            grow += 0.16f * bump(0.85f, 0.07f, 0.75f, 0.35f);
            grow += 0.1f * bump(0.15f, 0.05f, 1.05f, 0.3f);
        }
        return grow;
    };
    add(Loft(torso, 28, torsoSwell), skin, PieceKind::Body, identity);

    // Where the top of the back is at a point along it, for anything that stands on the ridge.
    const auto ridgeAt = [&](float t)
    {
        const glm::vec3 size = Profile(keys, t);
        return spineAt(t) + glm::vec3(0.0f, halfDepth * size.y, 0.0f);
    };
    const glm::quat alongSpine = TurnBetween(glm::vec3(0.0f, 0.0f, 1.0f), back);

    // Plates along the top of the back, and spines or bony spurs along the ridge.
    for (int p = 0; p < a.plates; ++p)
    {
        const float t = 0.2f + 0.65f * (static_cast<float>(p) + 0.5f) / static_cast<float>(a.plates);
        const glm::vec3 radii{a.width * 0.36f, 0.035f, a.length / static_cast<float>(a.plates) * 0.5f};
        add(Primitives::Ellipsoid(radii, 12, 6), plate, PieceKind::Body,
            glm::translate(identity, ridgeAt(t) - glm::vec3(0.0f, 0.01f, 0.0f)) * glm::mat4_cast(alongSpine));
    }
    if (a.spines > 0)
    {
        MeshData spurs;
        for (int s = 0; s < a.spines; ++s)
        {
            const float t = 0.12f + 0.8f * (static_cast<float>(s) + 0.5f) / static_cast<float>(a.spines);
            const glm::vec3 base = ridgeAt(t) - glm::vec3(0.0f, a.spineLength * 0.15f, 0.0f);
            // Raked back, the way a spine on an animal that moves forwards is.
            const float length = a.spineLength * (0.7f + 0.6f * Detail(a.seed, 100u + static_cast<uint32_t>(s)));
            const glm::vec3 tip = base + glm::normalize(glm::vec3(0.0f, 1.0f, 0.5f)) * length;
            spurs.Append(Primitives::Frustum(length * 0.18f, 0.0f, glm::distance(base, tip), 8), Between(base, tip));
        }
        add(spurs, bone, PieceKind::Body, identity);
    }

    // The tail, one skin drooping more the further it goes.
    if (a.tailLength > 0.05f)
    {
        std::vector<Ring> tail;
        constexpr int kTailRings = 16;
        glm::vec3 at = pose.tailBase - pose.tailDirection * (a.tailThickness * 1.5f);
        glm::vec3 direction = pose.tailDirection;
        const float step = (a.tailLength + a.tailThickness * 1.5f) / static_cast<float>(kTailRings - 1);
        for (int r = 0; r < kTailRings; ++r)
        {
            const float u = static_cast<float>(r) / static_cast<float>(kTailRings - 1);
            const float radius = a.tailThickness * (1.0f - 0.88f * u);
            tail.push_back({at, radius, radius, radius});
            at += direction * step;
            direction = glm::normalize(direction + glm::vec3(0.0f, -0.03f, 0.0f));
        }
        const float tailKnobs = 4.0f + static_cast<float>(a.tailSegments) * 2.0f;
        add(Loft(tail, 14,
                 [&](size_t i, float angle)
                 {
                     const float u = static_cast<float>(i) / static_cast<float>(kTailRings - 1);
                     const float top = std::pow(std::max(std::cos(angle), 0.0f), 10.0f);
                     return 1.0f + 0.12f * ribs * top * std::abs(std::sin(u * tailKnobs * glm::pi<float>()));
                 }),
            skin, PieceKind::Body, identity);
    }

    // The neck: from inside the shoulders to under the back of the skull, narrowing.
    const float hl = a.headLength;
    const float hw = a.headWidth;
    const float hd = a.headDepth;
    const glm::vec3 headBack = pose.head + glm::vec3(0.0f, -hd * 0.12f, hl * 0.28f);
    {
        const float length = glm::distance(pose.neckBase, headBack);
        // Never so thin the head looks balanced on a stalk.
        const float r0 = std::max(a.neckThickness * 0.55f, hw * 0.32f);
        const float r1 = std::max(a.neckThickness * 0.42f, hw * 0.27f);
        add(LimbSegment(r0, r1, length, r1, r0), skin, PieceKind::Front, Between(pose.neckBase, headBack));
    }

    // The head: a skull. A cranium swelling at the back, a heavy brow, cheekbones, a face that juts as
    // far as its snout does, and a mouth full of teeth. Built round the middle of the head, facing -Z.
    const float snout = a.snout;
    const glm::vec3 muzzleCentre{0.0f, -hd * 0.17f, -hl * 0.3f * snout};
    const glm::vec3 muzzleRadii{hw * 0.3f, hd * 0.2f, hl * 0.3f * snout};
    const glm::vec3 craniumRadii{hw * 0.46f * a.cranium, hd * 0.44f * a.cranium, hl * 0.48f * a.cranium};
    const glm::vec3 craniumCentre{0.0f, hd * 0.1f, hl * 0.1f + hl * 0.12f * (a.cranium - 1.0f)};
    // The point on the skull in the direction of `wanted`, and which way the skull faces there: where
    // the sockets and the brow go, however many eyes it has and however its skull is shaped.
    struct Seat
    {
        glm::vec3 at;
        glm::vec3 normal;
    };
    const auto onSkull = [&](const glm::vec3& wanted)
    {
        const glm::vec3 from = wanted - craniumCentre;
        const float scale = std::max(glm::length(from / craniumRadii), 1e-4f);
        const glm::vec3 surface = craniumCentre + from / scale;
        return Seat{surface, glm::normalize((surface - craniumCentre) / (craniumRadii * craniumRadii))};
    };
    {
        MeshData skull = Primitives::Ellipsoid(craniumRadii, 18, 12);
        MeshData moved;
        moved.Append(skull, glm::translate(identity, craniumCentre));
        for (const float side : {-1.0f, 1.0f})
        {
            // A ridge over each eye, low at the nose and high at the temple, so the face is always frowning.
            // Over the first pair of sockets where there are any, where eyes would be where there are not.
            const Seat over = onSkull(pose.eyes.size() >= 2 ? pose.eyes[side > 0.0f ? 1 : 0] - pose.head
                                                            : glm::vec3(side * hw * 0.21f, hd * 0.1f, -hl * 0.3f));
            const float socket = hw * 0.13f;
            moved.Append(Primitives::Ellipsoid({hw * 0.2f, hd * (0.035f + 0.045f * a.brow), hl * 0.08f}, 12, 6),
                         glm::translate(identity, over.at + glm::vec3(0.0f, socket * 1.05f, 0.0f) - over.normal * (socket * 0.1f)) *
                             glm::mat4_cast(glm::angleAxis(side * glm::radians(16.0f), glm::vec3(0.0f, 0.0f, 1.0f))));
            moved.Append(Primitives::Ellipsoid({hw * 0.09f, hd * 0.07f, hl * 0.16f}, 10, 6),
                         glm::translate(identity, glm::vec3(side * hw * 0.3f, -hd * 0.06f, -hl * 0.22f)));
        }
        moved.Append(Primitives::Ellipsoid(muzzleRadii, 14, 9), glm::translate(identity, muzzleCentre));
        // Upper teeth round the edge of the face, pointing down: longest a little way back from the
        // front, where a dog's fangs are.
        for (int t = 0; t < a.teeth; ++t)
        {
            const float u = a.teeth > 1 ? static_cast<float>(t) / static_cast<float>(a.teeth - 1) : 0.5f;
            const float around = (u - 0.5f) * 2.5f;
            const float fang = std::exp(-std::pow((std::abs(around) - 0.75f) / 0.3f, 2.0f));
            const float length = hd * a.toothLength * (0.09f + 0.09f * Detail(a.seed, static_cast<uint32_t>(t)) + 0.12f * fang);
            const glm::vec3 root{muzzleRadii.x * 0.85f * std::sin(around), muzzleCentre.y - muzzleRadii.y * 0.62f,
                                 muzzleCentre.z - muzzleRadii.z * 0.85f * std::cos(around)};
            moved.Append(Primitives::Frustum(0.0f, hw * 0.065f, length, 6),
                         glm::translate(identity, root - glm::vec3(0.0f, length * 0.5f, 0.0f)));
        }
        add(moved, bone, PieceKind::Front, glm::translate(identity, pose.head));
    }
    {
        // What is not bone: the sockets, the hole where a nose was, and the throat behind the teeth.
        MeshData holes;
        for (size_t e = 0; e < pose.eyes.size(); ++e)
        {
            const float row = static_cast<float>(e / 2);
            // Oval and slanted down towards the nose, which is what reads as a glare rather than a stare.
            const float socket = hw * 0.13f * (1.0f - 0.22f * row);
            const float side = pose.eyes[e].x > pose.head.x ? 1.0f : -1.0f;
            // Sunk into the skull where the eye would be, facing out of it, so only a hollow shows.
            const Seat seat = onSkull(pose.eyes[e] - pose.head);
            holes.Append(Primitives::Ellipsoid({socket * 1.1f, socket * 0.8f, socket}, 12, 8),
                         glm::translate(identity, seat.at - seat.normal * (socket * 0.5f)) *
                             glm::mat4_cast(TurnBetween(glm::vec3(0.0f, 0.0f, -1.0f), seat.normal)) *
                             glm::mat4_cast(glm::angleAxis(side * glm::radians(18.0f), glm::vec3(0.0f, 0.0f, 1.0f))));
        }
        holes.Append(Primitives::Ellipsoid({hw * 0.07f, hd * 0.07f, hl * 0.05f}, 10, 6),
                     glm::translate(identity, glm::vec3(0.0f, muzzleCentre.y + hd * 0.11f,
                                                        muzzleCentre.z - muzzleRadii.z * 0.78f)));
        add(holes, hollow, PieceKind::Front, glm::translate(identity, pose.head));
        add(Primitives::Ellipsoid({hw * 0.25f, hd * 0.13f, hl * 0.3f * snout}, 12, 8), throat, PieceKind::Front,
            glm::translate(identity, pose.head + glm::vec3(0.0f, -hd * 0.3f, -hl * 0.22f * snout)));
    }
    if (!pose.eyes.empty())
    {
        // A pinprick deep in each socket. They go out when it goes down.
        MeshData points;
        for (size_t e = 0; e < pose.eyes.size(); ++e)
        {
            const float row = static_cast<float>(e / 2);
            const float socket = hw * 0.13f * (1.0f - 0.22f * row);
            const Seat seat = onSkull(pose.eyes[e] - pose.head);
            points.Append(Primitives::Sphere(socket * 0.15f, 8, 6),
                          glm::translate(identity, seat.at + seat.normal * (socket * 0.4f) - glm::vec3(0.0f, socket * 0.1f, 0.0f)));
        }
        add(points, glint, PieceKind::Front, glm::translate(identity, pose.head));
    }
    if (a.frills > 0.05f && !crawler)
    {
        for (const float side : {-1.0f, 1.0f})
        {
            const glm::vec3 at = pose.head + glm::vec3(side * hw * 0.55f, hd * 0.1f, hl * 0.15f);
            const glm::vec3 radii{0.012f, 0.03f + hd * 0.7f * a.frills, 0.03f + hl * 0.5f * a.frills};
            add(Primitives::Ellipsoid(radii, 10, 6), under, PieceKind::Front,
                glm::translate(identity, at) *
                    glm::mat4_cast(glm::angleAxis(side * glm::radians(-25.0f), glm::vec3(0.0f, 0.0f, 1.0f))));
        }
    }

    // The lower jaw, hinged under the back of the skull, with its own row of teeth pointing up. Drawn
    // about the hinge so it can hang open.
    {
        const float jaw = a.jawLength * std::max(snout, 0.75f);
        MeshData lower = Primitives::Ellipsoid({hw * 0.27f, hd * 0.11f, jaw * 0.46f}, 14, 8);
        MeshData moved;
        moved.Append(lower, glm::translate(identity, glm::vec3(0.0f, -hd * 0.1f, -jaw * 0.46f)));
        const int teeth = std::max(a.teeth - 2, 3);
        for (int t = 0; t < teeth; ++t)
        {
            const float u = static_cast<float>(t) / static_cast<float>(teeth - 1);
            const float around = (u - 0.5f) * 2.2f;
            const float length = hd * a.toothLength * (0.07f + 0.1f * Detail(a.seed, 50u + static_cast<uint32_t>(t)));
            const glm::vec3 root{hw * 0.21f * std::sin(around), -hd * 0.05f, -jaw * 0.46f - jaw * 0.4f * std::cos(around)};
            moved.Append(Primitives::Frustum(hw * 0.06f, 0.0f, length, 6),
                         glm::translate(identity, root + glm::vec3(0.0f, length * 0.5f, 0.0f)));
        }
        add(moved, bone, PieceKind::Jaw, glm::translate(identity, pose.head + glm::vec3(0.0f, -hd * 0.2f, hl * 0.08f)));
    }

    // Limbs: three segments each, placed every frame by solving the limb to where its foot should be,
    // and meeting in knobbly joints so a limb is one limb. A crawler's arms are thin and end in long
    // fingers; its legs and everything else's end in toes and claws.
    for (size_t i = 0; i < pose.legs.size(); ++i)
    {
        const LegPair& pair = *pose.legs[i].pair;
        const int leg = static_cast<int>(i);
        const float t = pair.thickness;
        const glm::vec4 radius = pair.arm ? glm::vec4(0.95f, 0.52f, 0.4f, 0.32f) * t
                                 : crawler ? glm::vec4(1.0f, 0.6f, 0.42f, 0.32f) * t
                                           : glm::vec4(1.0f, 0.72f, 0.55f, 0.42f) * t;
        const float knob = crawler ? 1.2f : 1.08f;
        add(LimbSegment(radius.x, radius.y, pair.upper, radius.y * knob, radius.x), skin, PieceKind::LegUpper, identity, leg);
        add(LimbSegment(radius.y, radius.z, pair.lower, radius.z * knob), skin, PieceKind::LegLower, identity, leg);
        add(LimbSegment(radius.z, radius.w, pair.foot, radius.w * 1.05f), skin, PieceKind::LegFoot, identity, leg);

        // Fingers or toes, built about the toe with forward -Z, and the claws on the ends of them.
        MeshData fingers;
        MeshData claws;
        const int digits = std::max(a.fingers - (pair.arm ? 0 : 1), 2);
        const float groundBelow = t * 0.5f;
        const float fingerLength = pair.arm ? pair.foot * 0.75f : (crawler ? pair.foot * 0.4f : pair.foot * 0.15f);
        const float clawLength = pair.foot * (pair.arm ? 0.35f : 0.3f) * a.clawLength;
        const float fingerRadius = radius.w * (pair.arm ? 0.42f : 0.5f);
        for (int f = 0; f < digits; ++f)
        {
            const float u = static_cast<float>(f) / static_cast<float>(digits - 1) - 0.5f;
            const float splay = u * (pair.arm ? 1.1f : 0.8f) + (Detail(a.seed, 200u + static_cast<uint32_t>(i * 8 + static_cast<size_t>(f))) - 0.5f) * 0.2f;
            const glm::vec3 flat{std::sin(splay), 0.0f, -std::cos(splay)};
            const glm::vec3 base = flat * radius.w * 0.5f;
            const float length = fingerLength * (1.0f - 0.3f * std::abs(u) * 2.0f);
            const glm::vec3 knuckle = base + glm::normalize(flat + glm::vec3(0.0f, -0.12f, 0.0f)) * length;
            if (length > 0.005f)
            {
                fingers.Append(LimbSegment(fingerRadius, fingerRadius * 0.8f, glm::distance(base, knuckle), fingerRadius * 0.85f, 0.0f, 8),
                               Between(base, knuckle));
            }
            // The claw hooks down to the floor from the last knuckle.
            const float drop = std::clamp((-groundBelow * 0.95f - knuckle.y) / std::max(clawLength, 1e-3f), -0.95f, 0.3f);
            const glm::vec3 tip = knuckle + (flat * std::sqrt(1.0f - drop * drop) + glm::vec3(0.0f, drop, 0.0f)) * clawLength;
            claws.Append(Primitives::Frustum(fingerRadius * 0.8f, 0.0f, glm::distance(knuckle, tip), 6), Between(knuckle, tip));
        }
        if (!fingers.vertices.empty())
        {
            add(fingers, skin, PieceKind::Hand, identity, leg);
        }
        add(claws, claw, PieceKind::Hand, identity, leg);
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
        const glm::vec3 step = push * std::min(4.0f * dt, 1.0f);
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
        else if (m_anatomy.plan == BodyPlan::Crawler)
        {
            // A crawler's elbows stand up and out above its back, like a spider's knees; its knees point
            // forwards and out, the way a person's do on all fours.
            pole = pair.arm ? glm::vec3(leg.side * 0.55f, 0.8f, 0.6f) : glm::vec3(leg.side * 0.45f, 0.2f, -1.0f);
        }
        const TwoBoneIKResult ik = SolveTwoBoneIK(joints.hip, ankleTarget, glm::normalize(pole), pair.upper, pair.lower);
        joints.knee = ik.jointPosition;
        joints.ankle = ik.endPosition;
        joints.toe = joints.ankle + glm::vec3(0.0f, -pair.thickness * 0.5f, -pair.foot);
    }

    // Dead, the jaw falls slack; alive, it breathes.
    const float breathing = std::sin(m_shownTime * 1.9f + static_cast<float>(m_anatomy.seed % 97u)) * 3.0f;
    // A long jaw swings through more space for the same angle, so it opens less far.
    const float jawLength = m_anatomy.jawLength * std::max(m_anatomy.snout, 0.75f);
    const float jawReach = std::clamp(0.3f / std::max(jawLength, 0.05f), 0.35f, 1.0f);
    const float gape = (6.0f + 26.0f * m_anatomy.gape + 30.0f * m_windup + (Alive() ? breathing : 8.0f)) * jawReach;

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
        case PieceKind::Hand:
            local = glm::translate(glm::mat4(1.0f), legs[static_cast<size_t>(piece.leg)].toe);
            break;
        case PieceKind::Jaw:
            // Hanging open as far as this one's hangs, working slowly as it breathes, and gaping wide
            // as it gathers itself to strike.
            local = glm::translate(glm::mat4(1.0f), lift + rear + glm::vec3(0.0f, -0.06f * m_crouch * standing, 0.0f)) *
                    local *
                    glm::mat4_cast(glm::angleAxis(-glm::radians(gape), glm::vec3(1.0f, 0.0f, 0.0f)));
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
