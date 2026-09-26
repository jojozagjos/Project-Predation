#include "Game/Creature/CreatureSkin.h"

#include "Engine/Animation/IK.h"
#include "Engine/Core/ParallelFor.h"
#include "Engine/Render/MeshSimplify.h"
#include "Engine/Render/Primitives.h"
#include "Engine/Render/Sdf.h"
#include "Engine/Render/SurfaceNets.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <unordered_map>

namespace pred
{
namespace
{

using namespace Sdf;

// What a part of the surface is made of, for painting it.
enum class Zone : uint8_t
{
    Skin,
    Bone,   // skull and knuckles, where the skin is thin enough to show it
    Hollow, // eye sockets and nostrils
    Gums,   // the inside of the mouth
    Teeth,
    Claw,
    Plate,
    Membrane,
    Count
};

// --- The sculpture ------------------------------------------------------------------------------

struct Shape
{
    bool cone = true;
    glm::vec3 a{0.0f};
    glm::vec3 b{0.0f};
    float ra = 0.0f;
    float rb = 0.0f;
    glm::vec3 centre{0.0f};
    glm::vec3 radii{0.1f};
    glm::mat3 toLocal{1.0f};
    float blend = 0.02f; // how far it blends into what is already there
    int bone = 0;
    Zone zone = Zone::Skin;
    bool cut = false; // carved out rather than added
    glm::vec3 lo{0.0f};
    glm::vec3 hi{0.0f};

    float Distance(const glm::vec3& p) const
    {
        return cone ? RoundCone(p, a, b, ra, rb) : Ellipsoid(toLocal * (p - centre), radii);
    }
};

class Sculpture
{
public:
    std::vector<Shape> shapes;
    uint32_t seed = 0;
    float lumps = 0.0f; // how lumpy the skin is, in metres

    void Finish(float cell)
    {
        glm::vec3 lo(std::numeric_limits<float>::max());
        glm::vec3 hi(-std::numeric_limits<float>::max());
        for (Shape& shape : shapes)
        {
            glm::vec3 slo;
            glm::vec3 shi;
            if (shape.cone)
            {
                slo = glm::min(shape.a - glm::vec3(shape.ra), shape.b - glm::vec3(shape.rb));
                shi = glm::max(shape.a + glm::vec3(shape.ra), shape.b + glm::vec3(shape.rb));
            }
            else
            {
                const float r = std::max({shape.radii.x, shape.radii.y, shape.radii.z});
                slo = shape.centre - glm::vec3(r);
                shi = shape.centre + glm::vec3(r);
            }
            // Out as far as its blend reaches, and a little more for the lumps and the band the grid is
            // sampled in.
            const float margin = shape.blend + cell * 3.0f + lumps;
            shape.lo = slo - glm::vec3(margin);
            shape.hi = shi + glm::vec3(margin);
            lo = glm::min(lo, shape.lo);
            hi = glm::max(hi, shape.hi);
        }
        m_lo = lo;
        m_hi = hi;
        m_bin = cell * 8.0f;
        const glm::vec3 extent = hi - lo;
        m_nx = std::max(1, static_cast<int>(std::ceil(extent.x / m_bin)));
        m_ny = std::max(1, static_cast<int>(std::ceil(extent.y / m_bin)));
        m_nz = std::max(1, static_cast<int>(std::ceil(extent.z / m_bin)));
        m_bins.assign(static_cast<size_t>(m_nx) * static_cast<size_t>(m_ny) * static_cast<size_t>(m_nz), {});
        for (size_t i = 0; i < shapes.size(); ++i)
        {
            const glm::ivec3 from = BinOf(shapes[i].lo);
            const glm::ivec3 to = BinOf(shapes[i].hi);
            for (int z = from.z; z <= to.z; ++z)
            {
                for (int y = from.y; y <= to.y; ++y)
                {
                    for (int x = from.x; x <= to.x; ++x)
                    {
                        m_bins[BinIndex(x, y, z)].push_back(static_cast<uint16_t>(i));
                    }
                }
            }
        }
    }

    glm::vec3 Lo() const { return m_lo; }
    glm::vec3 Hi() const { return m_hi; }

    const std::vector<uint16_t>* Near(const glm::vec3& p) const
    {
        if (p.x < m_lo.x || p.y < m_lo.y || p.z < m_lo.z || p.x >= m_hi.x || p.y >= m_hi.y || p.z >= m_hi.z)
        {
            return nullptr;
        }
        const glm::ivec3 bin = BinOf(p);
        return &m_bins[BinIndex(bin.x, bin.y, bin.z)];
    }

    float Distance(const glm::vec3& p) const
    {
        const std::vector<uint16_t>* near = Near(p);
        if (near == nullptr || near->empty())
        {
            return 0.25f;
        }
        float d = 0.25f;
        bool any = false;
        for (const uint16_t index : *near)
        {
            const Shape& shape = shapes[index];
            if (shape.cut || p.x < shape.lo.x || p.y < shape.lo.y || p.z < shape.lo.z || p.x > shape.hi.x ||
                p.y > shape.hi.y || p.z > shape.hi.z)
            {
                continue;
            }
            const float here = shape.Distance(p);
            d = any ? SmoothUnion(d, here, shape.blend) : here;
            any = true;
        }
        for (const uint16_t index : *near)
        {
            const Shape& shape = shapes[index];
            if (shape.cut)
            {
                d = SmoothCut(d, shape.Distance(p), shape.blend);
            }
        }
        // Skin is not smooth: a little lumpiness near the surface, sinew and scar and knuckle, too
        // small to change the shape and big enough to catch the light.
        if (lumps > 0.0f && std::abs(d) < lumps * 6.0f)
        {
            d += (Fbm(p * 22.0f, seed, 3) - 0.5f) * lumps;
        }
        return d;
    }

    glm::vec3 Gradient(const glm::vec3& p, float step) const
    {
        // Four samples on a tetrahedron rather than six on the axes.
        const glm::vec3 k0{1.0f, -1.0f, -1.0f};
        const glm::vec3 k1{-1.0f, -1.0f, 1.0f};
        const glm::vec3 k2{-1.0f, 1.0f, -1.0f};
        const glm::vec3 k3{1.0f, 1.0f, 1.0f};
        const glm::vec3 g = k0 * Distance(p + k0 * step) + k1 * Distance(p + k1 * step) +
                            k2 * Distance(p + k2 * step) + k3 * Distance(p + k3 * step);
        const float length = glm::length(g);
        return length > 1e-8f ? g / length : glm::vec3(0.0f, 1.0f, 0.0f);
    }

private:
    glm::ivec3 BinOf(const glm::vec3& p) const
    {
        const glm::vec3 f = (p - m_lo) / m_bin;
        return {std::clamp(static_cast<int>(f.x), 0, m_nx - 1), std::clamp(static_cast<int>(f.y), 0, m_ny - 1),
                std::clamp(static_cast<int>(f.z), 0, m_nz - 1)};
    }
    size_t BinIndex(int x, int y, int z) const
    {
        return static_cast<size_t>(x) + static_cast<size_t>(m_nx) * (static_cast<size_t>(y) + static_cast<size_t>(m_ny) * static_cast<size_t>(z));
    }

    glm::vec3 m_lo{0.0f};
    glm::vec3 m_hi{0.0f};
    float m_bin = 0.1f;
    int m_nx = 1;
    int m_ny = 1;
    int m_nz = 1;
    std::vector<std::vector<uint16_t>> m_bins;
};

// The shortest turn from one direction to another.
glm::quat TurnBetween(const glm::vec3& from, const glm::vec3& to)
{
    const float cosine = glm::dot(from, to);
    if (cosine < -0.9999f)
    {
        const glm::vec3 axis = std::abs(from.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
        return glm::angleAxis(glm::pi<float>(), glm::normalize(glm::cross(from, axis)));
    }
    const glm::vec3 axis = glm::cross(from, to);
    return glm::normalize(glm::quat(1.0f + cosine, axis.x, axis.y, axis.z));
}

// A piece of fixed geometry, such as a tooth, placed from a mesh built along Y and centred.
// One tooth, built where it sits: from its root, sunk in the gum, out to its point, bent along the way
// towards `hook` -- back towards the throat, a little inwards -- as a predator's are, so it holds what it
// bites. Thickest a little way out from the root, flattened side to side into a blade, and its uv.y runs
// from 0 at the root to 1 at the point, for the stain at its base.
MeshData CurvedTooth(const glm::vec3& root, const glm::vec3& tip, const glm::vec3& hook, float radius, float flat)
{
    constexpr int kSides = 10;
    constexpr int kRings = 7;
    MeshData mesh;
    const float length = glm::distance(root, tip);
    const glm::vec3 bend = glm::mix(root, tip, 0.55f) + hook * (length * 0.28f);
    const auto at = [&](float t)
    { return (1.0f - t) * (1.0f - t) * root + 2.0f * (1.0f - t) * t * bend + t * t * tip; };
    const auto along = [&](float t)
    {
        const glm::vec3 d = 2.0f * (1.0f - t) * (bend - root) + 2.0f * t * (tip - bend);
        return glm::length(d) > 1e-6f ? glm::normalize(d) : glm::vec3(0.0f, -1.0f, 0.0f);
    };
    // Its frame across: the blade faces the way it hooks.
    const glm::vec3 first = along(0.0f);
    glm::vec3 side = glm::cross(first, hook);
    side = glm::length(side) > 1e-4f ? glm::normalize(side) : glm::normalize(glm::cross(first, glm::vec3(1.0f, 0.0f, 0.0f)));
    for (int r = 0; r <= kRings; ++r)
    {
        const float t = static_cast<float>(r) / static_cast<float>(kRings);
        const glm::vec3 centre = at(t);
        const glm::vec3 forward = along(t);
        const glm::vec3 x = glm::normalize(side - forward * glm::dot(side, forward));
        const glm::vec3 y = glm::cross(forward, x);
        const float swell = t < 0.2f ? 0.85f + 0.75f * t : 1.0f - (t - 0.2f) / 0.8f;
        const float here = radius * std::pow(std::max(swell, 0.0f), 0.85f);
        for (int k = 0; k < kSides; ++k)
        {
            const float angle = static_cast<float>(k) / static_cast<float>(kSides) * glm::two_pi<float>();
            const glm::vec3 out = x * std::cos(angle) * flat + y * std::sin(angle);
            MeshVertex vertex;
            vertex.position = centre + out * here;
            vertex.normal = glm::normalize(x * std::cos(angle) / std::max(flat, 0.2f) + y * std::sin(angle) + forward * 0.25f);
            vertex.uv = {static_cast<float>(k) / static_cast<float>(kSides), t};
            mesh.vertices.push_back(vertex);
        }
    }
    for (int r = 0; r < kRings; ++r)
    {
        for (int k = 0; k < kSides; ++k)
        {
            const uint32_t a = static_cast<uint32_t>(r * kSides + k);
            const uint32_t b = static_cast<uint32_t>(r * kSides + (k + 1) % kSides);
            const uint32_t c = a + kSides;
            const uint32_t d = b + kSides;
            mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
        }
    }
    return mesh;
}

glm::mat4 Between(const glm::vec3& a, const glm::vec3& b)
{
    const glm::vec3 along = b - a;
    const float length = glm::length(along);
    const glm::quat turn = length > 1e-5f ? TurnBetween(glm::vec3(0.0f, 1.0f, 0.0f), along / length)
                                          : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    return glm::translate(glm::mat4(1.0f), (a + b) * 0.5f) * glm::mat4_cast(turn);
}

float Detail(uint32_t seed, uint32_t n)
{
    uint32_t h = seed * 747796405u + n * 2891336453u + 0x9E3779B9u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    return static_cast<float>(h & 0xFFFFu) / 65535.0f;
}

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

uint32_t PackColour(const glm::vec3& colour, float roughnessScale)
{
    const auto channel = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    // Little-endian RGBA8: red in the lowest byte, as the vertex layout reads it.
    return channel(colour.r) | (channel(colour.g) << 8) | (channel(colour.b) << 16) | (channel(roughnessScale) << 24);
}

// Fixed geometry that is not part of the sculpted surface -- teeth, claws, fingers, spurs -- with the
// bone it follows and what it is painted as.
struct Attached
{
    MeshData mesh;
    int bone = 0;
    Zone zone = Zone::Skin;
};

} // namespace

const char* BoneKindName(BoneKind kind)
{
    switch (kind)
    {
    case BoneKind::Pelvis:
        return "pelvis";
    case BoneKind::Spine:
        return "spine";
    case BoneKind::Chest:
        return "chest";
    case BoneKind::Neck:
        return "neck";
    case BoneKind::Head:
        return "head";
    case BoneKind::Jaw:
        return "jaw";
    case BoneKind::Upper:
        return "upper limb";
    case BoneKind::Lower:
        return "lower limb";
    case BoneKind::End:
        return "hand or foot";
    case BoneKind::Tail:
        return "tail";
    }
    return "?";
}

glm::mat4 BoneFrame(const glm::vec3& head, const glm::vec3& tail, const glm::vec3& up)
{
    const glm::vec3 along = tail - head;
    const glm::vec3 y = glm::length(along) > 1e-6f ? glm::normalize(along) : glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 z = up - y * glm::dot(up, y);
    if (glm::length(z) < 1e-4f)
    {
        const glm::vec3 other = std::abs(y.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
        z = other - y * glm::dot(other, y);
    }
    z = glm::normalize(z);
    const glm::vec3 x = glm::cross(y, z);
    glm::mat4 frame(1.0f);
    frame[0] = glm::vec4(x, 0.0f);
    frame[1] = glm::vec4(y, 0.0f);
    frame[2] = glm::vec4(z, 0.0f);
    frame[3] = glm::vec4(head, 1.0f);
    return frame;
}

glm::vec3 LimbPole(const CreatureAnatomy& anatomy, const CreatureAnatomy::Leg& leg)
{
    const LegPair& pair = *leg.pair;
    if (anatomy.plan == BodyPlan::Hexapod)
    {
        return glm::normalize(glm::vec3(leg.side, 1.0f, 0.0f));
    }
    if (anatomy.plan == BodyPlan::Crawler)
    {
        // Elbows up and out above the back, like a spider's knees; knees forwards and out, the way a
        // person's go on all fours.
        return glm::normalize(pair.arm ? glm::vec3(leg.side * 0.55f, 0.8f, 0.6f) : glm::vec3(leg.side * 0.45f, 0.2f, -1.0f));
    }
    return glm::normalize(pair.backwardKnee ? glm::vec3(0.0f, 0.2f, 1.0f) : glm::vec3(0.0f, 0.2f, -1.0f));
}

CreatureSkin CreatureSkin::Build(const CreatureAnatomy& a)
{
    CreatureSkin skin;
    const CreatureAnatomy::RestPose pose = a.Rest();
    const bool crawler = a.plan == BodyPlan::Crawler;
    const glm::mat4 identity(1.0f);
    const glm::vec3 up{0.0f, 1.0f, 0.0f};

    // How big the animal is, for everything whose size is not already a measurement of it.
    const float scale = std::clamp(std::cbrt(a.length * a.width * a.depth / (0.72f * 0.44f * 0.26f)), 0.6f, 2.2f);
    // Fine enough for a face: at two centimetres the whole of one was a dozen samples across, and the
    // brow, the cheek and the line of the mouth all melted into one smooth lump.
    skin.cell = std::clamp(a.OverallLength() * 0.008f, 0.009f, 0.018f);

    // --- The skeleton -----------------------------------------------------------------------------
    const auto addBone = [&](BoneKind kind, int parent, const glm::vec3& from, const glm::vec3& to, const glm::vec3& boneUp,
                             float radius, int limb = -1)
    {
        SkinBone bone;
        bone.kind = kind;
        bone.parent = parent;
        bone.limb = limb;
        bone.head = from;
        bone.tail = to;
        bone.radius = radius;
        bone.rest = BoneFrame(from, to, boneUp);
        bone.restInverse = glm::inverse(bone.rest);
        skin.bones.push_back(bone);
        return static_cast<int>(skin.bones.size()) - 1;
    };

    // The spine: from behind the rump to in front of the shoulders, bowed up in the middle.
    const float halfWidth = a.width * 0.5f;
    const float halfDepth = a.depth * 0.5f;
    const glm::vec3 back = glm::normalize(pose.rump - pose.shoulders);
    const glm::vec3 rumpEnd = pose.rump + back * (a.length * (crawler ? 0.1f : 0.14f));
    const glm::vec3 shoulderEnd = pose.shoulders - back * (a.length * (crawler ? 0.1f : 0.08f));
    const float arch = a.depth * (crawler ? 0.28f : 0.1f);
    const auto spineAt = [&](float t)
    { return rumpEnd + (shoulderEnd - rumpEnd) * t + glm::vec3(0.0f, arch * std::sin(glm::pi<float>() * t), 0.0f); };
    const std::array<float, 4> spineT{0.1f, 0.42f, 0.72f, 0.97f};
    skin.pelvis = addBone(BoneKind::Pelvis, -1, spineAt(spineT[0]), spineAt(spineT[1]), up, halfDepth);
    const int midSpine = addBone(BoneKind::Spine, skin.pelvis, spineAt(spineT[1]), spineAt(spineT[2]), up, halfDepth * 0.8f);
    skin.chest = addBone(BoneKind::Chest, midSpine, spineAt(spineT[2]), spineAt(spineT[3]), up, halfDepth);
    skin.spine = {skin.pelvis, midSpine, skin.chest};
    const auto spineBoneAt = [&](float t) { return t < 0.42f ? skin.pelvis : (t < 0.72f ? midSpine : skin.chest); };

    // The neck, the skull and the jaw.
    const float hl = a.headLength;
    const float hw = a.headWidth;
    const float hd = a.headDepth;
    const float snout = a.snout;
    const glm::vec3 headBack = pose.head + glm::vec3(0.0f, -hd * 0.12f, hl * 0.28f);
    const glm::vec3 neckMid = (pose.neckBase + headBack) * 0.5f + glm::vec3(0.0f, a.neckLength * 0.05f, 0.0f);
    const float neckRoot = std::max(a.neckThickness * 0.55f, hw * 0.32f);
    const float neckTop = std::max(a.neckThickness * 0.42f, hw * 0.27f);
    const int neckLow = addBone(BoneKind::Neck, skin.chest, pose.neckBase, neckMid, up, neckRoot);
    const int neckHigh = addBone(BoneKind::Neck, neckLow, neckMid, headBack, up, neckTop);
    skin.neck = {neckLow, neckHigh};
    const glm::vec3 headFront = pose.head + glm::vec3(0.0f, -hd * 0.1f, -hl * 0.5f * std::max(snout, 0.8f));
    skin.head = addBone(BoneKind::Head, neckHigh, headBack, headFront, up, std::max(hw, hd) * 0.5f);
    // The face, measured from the middle of the head: how far forward its tip is, where the edge of the
    // upper jaw runs, and a lower jaw hinged below and behind the eyes that reaches as far as the upper one
    // does, less a little. Sculpted nearly shut -- a jaw that hangs open at rest reads as broken, and the
    // rig opens it -- except a maw, which hangs.
    const float faceLength = hl * 0.55f * snout;
    const float faceTipZ = -faceLength;
    const float mouthCornerZ = hl * 0.02f;
    const float gumLine = -hd * 0.3f;
    const float jawGap = hd * 0.015f;
    const glm::vec3 hinge = pose.head + glm::vec3(0.0f, gumLine + hd * 0.02f, hl * 0.1f);
    const float jawLength = hinge.z - (pose.head.z + faceTipZ + hl * 0.04f);
    const float restGape = glm::radians(a.headShape == HeadShape::Maw ? 14.0f : 4.0f);
    skin.restGape = restGape;
    const glm::mat4 jawTurn = glm::translate(identity, hinge) *
                              glm::mat4_cast(glm::angleAxis(-restGape, glm::vec3(1.0f, 0.0f, 0.0f))) *
                              glm::translate(identity, -hinge);
    const auto onJaw = [&](const glm::vec3& p) { return glm::vec3(jawTurn * glm::vec4(p, 1.0f)); };
    const glm::vec3 chin = onJaw(hinge + glm::vec3(0.0f, -hd * 0.1f, -jawLength));
    skin.jaw = addBone(BoneKind::Jaw, skin.head, hinge, chin, up, hw * 0.25f);

    // Both jaws as two tapering rods side by side, from the corner of the mouth to the tip: rounded, wider
    // than deep, and with an underside (or, for the lower jaw, a top) that runs in a straight line, so the
    // edge of the mouth is straight. At `t` from the tip (0) to the corner (1): how far out each rod runs,
    // and how thick it is. The lower jaw is narrower all the way along, so the upper teeth close outside it.
    struct JawRod
    {
        float out = 0.0f;
        float radius = 0.0f;
    };
    // Narrower than it is deep, a muzzle and not a bill: spread wide and flat, the face read as squashed.
    const auto upperRod = [&](float t) { return JawRod{hw * (0.045f + 0.085f * t), hd * (0.1f + 0.1f * t)}; };
    const auto lowerRod = [&](float t) { return JawRod{hw * (0.065f * t), hd * (0.05f + 0.06f * t)}; };
    const auto alongFace = [&](float t) { return faceTipZ + (mouthCornerZ - faceTipZ) * t; };
    const float lowerGum = gumLine - jawGap;

    // The tail: a chain drooping more the further it goes.
    std::vector<glm::vec3> tailPoints;
    if (a.tailLength > 0.05f)
    {
        glm::vec3 at = pose.tailBase - pose.tailDirection * (a.tailThickness * 1.2f);
        glm::vec3 direction = pose.tailDirection;
        const int count = std::max(a.tailSegments, 3);
        const float step = (a.tailLength + a.tailThickness * 1.2f) / static_cast<float>(count);
        tailPoints.push_back(at);
        int parent = skin.pelvis;
        for (int i = 0; i < count; ++i)
        {
            const glm::vec3 next = at + direction * step;
            const float u = static_cast<float>(i) / static_cast<float>(count);
            parent = addBone(BoneKind::Tail, parent, at, next, up, a.tailThickness * (1.0f - 0.8f * u));
            skin.tail.push_back(parent);
            tailPoints.push_back(next);
            at = next;
            direction = glm::normalize(direction + glm::vec3(0.0f, -0.12f, 0.0f));
        }
    }

    // The limbs, solved to stand exactly as they will on the first frame.
    struct LimbShape
    {
        glm::vec3 hip, knee, ankle, toe;
        glm::vec4 radius;
    };
    std::vector<LimbShape> limbShapes;
    for (size_t i = 0; i < pose.legs.size(); ++i)
    {
        const CreatureAnatomy::Leg& leg = pose.legs[i];
        const LegPair& pair = *leg.pair;
        const glm::vec3 ankleTarget = leg.foot + glm::vec3(0.0f, pair.thickness, 0.0f);
        const TwoBoneIKResult ik = SolveTwoBoneIK(leg.hip, ankleTarget, LimbPole(a, leg), pair.upper, pair.lower);
        LimbShape shape;
        shape.hip = leg.hip;
        shape.knee = ik.jointPosition;
        shape.ankle = ik.endPosition;
        shape.toe = shape.ankle + glm::vec3(0.0f, -pair.thickness * 0.5f, -pair.foot);
        const float t = pair.thickness;
        // Thick where the muscle is, at the root, and down to sinew and knuckle at the far end: a limb is
        // not a pipe. These are the bones' own radii; the muscle is sculpted over them below.
        shape.radius = pair.arm ? glm::vec4(1.05f, 0.5f, 0.36f, 0.3f) * t
                       : crawler ? glm::vec4(1.15f, 0.56f, 0.38f, 0.3f) * t
                                 : glm::vec4(1.2f, 0.66f, 0.46f, 0.38f) * t;
        // Never thinner than the skin can be drawn at: a limb finer than two samples across comes out
        // in pieces.
        shape.radius = glm::max(shape.radius, glm::vec4(skin.cell * 1.05f));
        limbShapes.push_back(shape);

        glm::vec3 bend = glm::cross(shape.knee - shape.hip, shape.ankle - shape.knee);
        bend = glm::length(bend) > 1e-6f ? glm::normalize(bend) : glm::vec3(leg.side, 0.0f, 0.0f);
        const int parent = pair.arm || pair.along < 0.34f ? skin.chest : (pair.along > 0.66f ? skin.pelvis : midSpine);
        Limb limb;
        limb.arm = pair.arm;
        limb.bend = bend;
        const int limbIndex = static_cast<int>(i);
        limb.upper = addBone(BoneKind::Upper, parent, shape.hip, shape.knee, bend, shape.radius.x, limbIndex);
        limb.lower = addBone(BoneKind::Lower, limb.upper, shape.knee, shape.ankle, bend, shape.radius.y, limbIndex);
        limb.end = addBone(BoneKind::End, limb.lower, shape.ankle, shape.toe, bend, shape.radius.z, limbIndex);
        skin.limbs.push_back(limb);
    }

    // --- The sculpture ----------------------------------------------------------------------------
    Sculpture sculpture;
    sculpture.seed = a.seed;
    sculpture.lumps = skin.cell * 0.35f;
    const auto cone = [&](const glm::vec3& from, const glm::vec3& to, float r0, float r1, int bone, float blend,
                          Zone zone = Zone::Skin)
    {
        Shape shape;
        shape.cone = true;
        shape.a = from;
        shape.b = to;
        shape.ra = r0;
        shape.rb = r1;
        shape.bone = bone;
        shape.blend = blend;
        shape.zone = zone;
        sculpture.shapes.push_back(shape);
    };
    const auto blob = [&](const glm::vec3& centre, const glm::vec3& radii, const glm::quat& turn, int bone, float blend,
                          Zone zone = Zone::Skin, bool cut = false)
    {
        Shape shape;
        shape.cone = false;
        shape.centre = centre;
        shape.radii = glm::max(radii, glm::vec3(1e-3f));
        shape.toLocal = glm::transpose(glm::mat3_cast(turn));
        shape.bone = bone;
        shape.blend = blend;
        shape.zone = zone;
        shape.cut = cut;
        sculpture.shapes.push_back(shape);
    };
    const glm::quat noTurn(1.0f, 0.0f, 0.0f, 0.0f);

    // The torso: a row of ellipsoids along the spine, each as thick as the body is there, blended into
    // one. A crawler's is a person's gone wrong: a pelvis, a waist sucked in to nothing, a deep ribcage,
    // wide shoulders.
    std::vector<ProfileKey> keys;
    if (crawler)
    {
        keys = {{0.0f, 0.3f, 0.35f, 0.35f},  {0.06f, 0.55f, 0.66f, 0.66f}, {0.15f, 0.66f, 0.8f, 0.86f},
                {0.28f, 0.5f, 0.72f, 0.6f},  {0.4f, 0.4f, 0.66f, 0.44f},   {0.55f, 0.56f, 0.82f, 0.86f},
                {0.72f, 0.64f, 0.9f, 1.0f},  {0.86f, 0.8f, 0.88f, 0.86f},  {0.94f, 0.9f, 0.78f, 0.62f},
                {1.0f, 0.55f, 0.5f, 0.42f}};
    }
    else
    {
        const float rear = 1.0f - a.taper;
        keys = {{0.0f, 0.35f * rear, 0.35f, 0.35f}, {0.08f, 0.8f * rear, 0.78f, 0.78f},
                {0.3f, 0.92f * rear + 0.08f, 0.88f, 0.9f}, {0.7f, 1.0f, 0.98f, 1.0f},
                {0.93f, 0.95f, 0.92f, 0.9f}, {1.0f, 0.5f, 0.48f, 0.45f}};
    }
    const float torsoBlend = 0.05f * scale;
    const int torsoCount = std::max(9, static_cast<int>(a.length / (0.07f * scale)));
    for (int i = 0; i < torsoCount; ++i)
    {
        const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(torsoCount);
        const glm::vec3 size = Profile(keys, t);
        const glm::vec3 here = spineAt(t);
        const glm::vec3 ahead = spineAt(std::min(t + 0.02f, 1.0f)) - spineAt(std::max(t - 0.02f, 0.0f));
        const glm::quat turn = TurnBetween(glm::vec3(0.0f, 0.0f, -1.0f), glm::normalize(ahead));
        const float over = halfDepth * size.y;
        const float under = halfDepth * size.z;
        const float spacing = glm::length(shoulderEnd - rumpEnd) / static_cast<float>(torsoCount);
        blob(here + turn * glm::vec3(0.0f, (over - under) * 0.5f, 0.0f),
             {halfWidth * size.x, (over + under) * 0.5f, spacing * 1.25f}, turn, spineBoneAt(t), torsoBlend);
    }
    // Where the surface of the torso is at a point along it, a given angle round (0 the top, a quarter
    // turn its right side), for everything that sits on or just under the skin.
    const auto onTorso = [&](float t, float angle, float outwards)
    {
        const glm::vec3 size = Profile(keys, t);
        const glm::vec3 ahead = glm::normalize(spineAt(std::min(t + 0.02f, 1.0f)) - spineAt(std::max(t - 0.02f, 0.0f)));
        const glm::quat turn = TurnBetween(glm::vec3(0.0f, 0.0f, -1.0f), ahead);
        const float vertical = std::cos(angle);
        const glm::vec3 local{std::sin(angle) * halfWidth * size.x,
                              vertical * halfDepth * (vertical > 0.0f ? size.y : size.z), 0.0f};
        return spineAt(t) + turn * (local * outwards);
    };

    // The spine showing down the back, a knob for every vertebra.
    const float ribs = a.ribs;
    const float vertebra = skin.cell * (1.2f + 1.3f * ribs) * std::sqrt(scale);
    const int vertebrae = std::max(8, static_cast<int>(glm::length(shoulderEnd - rumpEnd) / (vertebra * 2.4f)));
    for (int i = 0; i < vertebrae; ++i)
    {
        const float t = 0.04f + 0.92f * (static_cast<float>(i) + 0.5f) / static_cast<float>(vertebrae);
        blob(onTorso(t, 0.0f, 0.97f), glm::vec3(vertebra * 0.8f, vertebra, vertebra * 0.7f), noTurn, spineBoneAt(t),
             vertebra * 0.9f, Zone::Skin);
    }
    // Ribs standing out of the chest: arcs from the spine down each side, sloping back as they go.
    const int ribCount = crawler ? 7 : 4 + static_cast<int>(ribs * 5.0f);
    const float ribFrom = crawler ? 0.5f : 0.4f;
    const float ribTo = crawler ? 0.88f : 0.88f;
    const float ribRadius = skin.cell * (0.8f + 0.9f * ribs) * std::sqrt(scale);
    if (ribs > 0.15f)
    {
        for (int r = 0; r < ribCount; ++r)
        {
            const float t = ribFrom + (ribTo - ribFrom) * (static_cast<float>(r) + 0.5f) / static_cast<float>(ribCount);
            for (const float side : {-1.0f, 1.0f})
            {
                glm::vec3 previous{0.0f};
                constexpr int kPieces = 4;
                for (int k = 0; k <= kPieces; ++k)
                {
                    const float angle = side * glm::radians(20.0f + 125.0f * static_cast<float>(k) / static_cast<float>(kPieces));
                    // Sloping back towards the pelvis as they come round.
                    const float slope = -0.05f * static_cast<float>(k) / static_cast<float>(kPieces);
                    const glm::vec3 point = onTorso(std::max(t + slope, 0.0f), angle, 0.99f);
                    if (k > 0)
                    {
                        cone(previous, point, ribRadius, ribRadius, skin.chest, ribRadius * 1.2f);
                    }
                    previous = point;
                }
            }
        }
    }
    // Shoulder blades standing up either side of the spine, and hip bones: on anything, but sharpest on
    // something starving.
    for (const float side : {-1.0f, 1.0f})
    {
        const float bony = crawler ? 1.0f : 0.5f + 0.5f * ribs;
        const glm::vec3 blade = onTorso(crawler ? 0.84f : 0.8f, side * 0.75f, 0.92f);
        blob(blade, glm::vec3(0.075f * (crawler ? 1.0f : a.width / 0.5f), (0.015f + 0.012f * ribs) * bony, 0.09f * (crawler ? 1.0f : a.length / 1.3f)) * scale,
             glm::angleAxis(side * 0.6f, glm::vec3(0.0f, 0.0f, 1.0f)), skin.chest, 0.035f * scale);
        const glm::vec3 hipBone = onTorso(0.14f, side * 1.1f, 0.95f);
        blob(hipBone, glm::vec3(0.03f, 0.05f, 0.06f) * scale * (0.6f + 0.4f * bony), noTurn, skin.pelvis, 0.03f * scale);
    }
    if (crawler)
    {
        // A collarbone from the top of the breastbone out to each shoulder.
        const glm::vec3 sternum = onTorso(0.93f, glm::pi<float>() * 0.82f, 0.95f);
        for (size_t i = 0; i < pose.legs.size(); ++i)
        {
            if (pose.legs[i].pair->arm && pose.legs[i].pair->along < 0.1f)
            {
                cone(sternum, limbShapes[i].hip, skin.cell * 1.0f, skin.cell * 1.1f, skin.chest, skin.cell * 1.6f);
            }
        }
    }
    // Armour plates along the back.
    for (int p = 0; p < a.plates; ++p)
    {
        const float t = 0.2f + 0.65f * (static_cast<float>(p) + 0.5f) / static_cast<float>(a.plates);
        const glm::vec3 ahead = glm::normalize(spineAt(std::min(t + 0.02f, 1.0f)) - spineAt(std::max(t - 0.02f, 0.0f)));
        blob(onTorso(t, 0.0f, 0.98f), {a.width * 0.36f, 0.03f, a.length / static_cast<float>(a.plates) * 0.5f},
             TurnBetween(glm::vec3(0.0f, 0.0f, -1.0f), ahead), spineBoneAt(t), skin.cell * 1.2f, Zone::Plate);
    }

    // The neck, and on a crawler the cords either side of it that stand out on somebody starving.
    cone(pose.neckBase, neckMid, neckRoot, (neckRoot + neckTop) * 0.5f, neckLow, 0.05f * scale);
    cone(neckMid, headBack, (neckRoot + neckTop) * 0.5f, neckTop, neckHigh, 0.03f * scale);
    if (crawler)
    {
        for (const float side : {-1.0f, 1.0f})
        {
            const glm::vec3 behindJaw = pose.head + glm::vec3(side * hw * 0.25f, -hd * 0.28f, hl * 0.18f);
            const glm::vec3 collar = pose.neckBase + glm::vec3(side * neckRoot * 0.35f, -neckRoot * 0.55f, -neckRoot * 0.45f);
            cone(behindJaw, collar, skin.cell * 1.05f, skin.cell * 1.25f, neckHigh, skin.cell * 1.5f);
        }
    }

    // The skull: a long cranium, an upper jaw tapering from under the eyes to the tip of the face with a
    // bridge along the top of it, and a lower jaw as long, hinged below the eyes. Nothing stands out from
    // the sides or the back of it: the flaps, knobs and cheek muscles that used to read as ears.
    // As tall as it is wide, near enough: a skull flattened to a wedge was what read as squashed.
    const glm::vec3 craniumRadii{hw * 0.42f * a.cranium, hd * 0.47f * a.cranium, hl * 0.46f * a.cranium};
    const glm::vec3 craniumCentre = pose.head + glm::vec3(0.0f, hd * 0.16f, hl * 0.12f + hl * 0.1f * (a.cranium - 1.0f));
    // Joined with a small blend, so the joins show as creases: a blend wider than the jaw itself, as it
    // was, melted the jaws into the skull and the face into a bean.
    const float skullBlend = 0.014f * std::sqrt(scale);
    blob(craniumCentre, craniumRadii, noTurn, skin.head, skullBlend, Zone::Skin);
    for (const float side : {-1.0f, 1.0f})
    {
        const JawRod rear = upperRod(1.0f);
        const JawRod tip = upperRod(0.0f);
        cone(pose.head + glm::vec3(side * rear.out, gumLine + rear.radius, mouthCornerZ),
             pose.head + glm::vec3(side * tip.out, gumLine + tip.radius, faceTipZ + tip.radius), rear.radius, tip.radius, skin.head,
             skullBlend, Zone::Skin);
    }
    // The bridge of the face, from the brow down to the tip.
    cone(pose.head + glm::vec3(0.0f, hd * 0.14f, -hl * 0.14f),
         pose.head + glm::vec3(0.0f, gumLine + upperRod(0.0f).radius * 1.6f, faceTipZ + upperRod(0.0f).radius * 1.4f),
         hw * 0.17f, upperRod(0.0f).radius * 0.8f, skin.head, skullBlend * 1.2f, Zone::Skin);
    // Cheekbones: a bar of bone either side from under the eye back to above the hinge of the jaw, low and
    // close in along the side of the face -- the plane of the cheek under it, and a line the light catches.
    for (const float side : {-1.0f, 1.0f})
    {
        const JawRod mid = upperRod(0.7f);
        cone(pose.head + glm::vec3(side * (mid.out + mid.radius * 0.9f), gumLine + mid.radius * 2.1f, alongFace(0.62f)),
             pose.head + glm::vec3(side * hw * 0.34f, gumLine + hd * 0.2f, hinge.z - pose.head.z - hl * 0.04f), hw * 0.045f, hw * 0.06f,
             skin.head, skin.cell * 1.2f, Zone::Skin);
    }
    // A low crest down the middle of the skull from the brow back, where the jaw muscles meet over it.
    cone(craniumCentre + glm::vec3(0.0f, craniumRadii.y * 0.9f, -craniumRadii.z * 0.35f),
         craniumCentre + glm::vec3(0.0f, craniumRadii.y * 0.72f, craniumRadii.z * 0.85f), hw * 0.035f, hw * 0.05f, skin.head,
         skin.cell * 1.5f, Zone::Skin);
    // The lower jaw: two rods meeting at the chin, a V from below, with a little muscle where it hinges,
    // kept low and inside the line of the head.
    for (const float side : {-1.0f, 1.0f})
    {
        const JawRod rear = lowerRod(1.0f);
        const JawRod tip = lowerRod(0.0f);
        cone(onJaw(pose.head + glm::vec3(side * rear.out, lowerGum - rear.radius, hinge.z - pose.head.z - hl * 0.02f)),
             onJaw(pose.head + glm::vec3(side * tip.out, lowerGum - tip.radius, faceTipZ + hl * 0.04f + tip.radius)), rear.radius,
             tip.radius, skin.jaw, skullBlend, Zone::Skin);
        blob(onJaw(hinge + glm::vec3(side * (rear.out + rear.radius * 0.6f), -hd * 0.08f, -hl * 0.06f)),
             {hw * 0.05f, hd * 0.07f, hl * 0.08f}, noTurn, skin.jaw, skin.cell * 2.0f);
    }
    // The inside of the mouth: the palate hollowed out of the upper jaw between its rows of teeth, and the
    // floor of the mouth out of the lower one, so the teeth have somewhere to close into.
    blob(pose.head + glm::vec3(0.0f, gumLine, alongFace(0.58f)),
         {upperRod(0.5f).out * 0.9f, hd * 0.05f, (mouthCornerZ - faceTipZ) * 0.38f}, noTurn, skin.head, skin.cell * 0.8f, Zone::Gums, true);
    blob(onJaw(pose.head + glm::vec3(0.0f, lowerGum, alongFace(0.6f) + hl * 0.03f)),
         {std::max(lowerRod(0.5f).out, skin.cell * 1.2f), hd * 0.03f, (mouthCornerZ - faceTipZ) * 0.35f}, noTurn, skin.jaw, skin.cell * 0.8f,
         Zone::Gums, true);
    // The line of the mouth along each side, from the corner forward, cut in between the jaws: shut, the
    // jaws are still two things, not one lump with teeth stuck on.
    for (const float side : {-1.0f, 1.0f})
    {
        const JawRod mid = upperRod(0.55f);
        blob(pose.head + glm::vec3(side * (mid.out + mid.radius * 0.8f), (gumLine + lowerGum) * 0.5f, alongFace(0.55f)),
             {mid.radius * 0.7f, std::max(hd * 0.012f, skin.cell * 0.45f), (mouthCornerZ - faceTipZ) * 0.5f}, noTurn, skin.head,
             skin.cell * 0.5f, Zone::Hollow, true);
    }
    // Gums: a ridge of wet flesh along the edge of each jaw, which the teeth come out of.
    for (const float side : {-1.0f, 1.0f})
    {
        const auto upperEdge = [&](float t)
        {
            const JawRod rod = upperRod(t);
            return pose.head + glm::vec3(side * (rod.out + rod.radius * 0.6f), gumLine + rod.radius * 0.25f, alongFace(t));
        };
        const auto lowerEdge = [&](float t)
        {
            const JawRod rod = lowerRod(t);
            return onJaw(pose.head + glm::vec3(side * (rod.out + rod.radius * 0.3f), lowerGum - rod.radius * 0.1f, alongFace(t) + hl * 0.03f));
        };
        // Stopping short of the tip: carried round the front they read as lips, or as a nose.
        cone(upperEdge(0.95f), upperEdge(0.22f), std::max(upperRod(0.95f).radius * 0.22f, skin.cell * 0.8f),
             std::max(upperRod(0.22f).radius * 0.22f, skin.cell * 0.7f), skin.head, skin.cell * 0.6f, Zone::Gums);
        cone(lowerEdge(0.95f), lowerEdge(0.25f), std::max(lowerRod(0.95f).radius * 0.25f, skin.cell * 0.8f),
             std::max(lowerRod(0.25f).radius * 0.25f, skin.cell * 0.7f), skin.jaw, skin.cell * 0.6f, Zone::Gums);
    }

    // The point on the cranium in the direction of `wanted`, and which way the skull faces there.
    struct Seat
    {
        glm::vec3 at;
        glm::vec3 normal;
    };
    const auto onSkull = [&](const glm::vec3& wanted)
    {
        const glm::vec3 from = wanted - craniumCentre;
        const float s = std::max(glm::length(from / craniumRadii), 1e-4f);
        const glm::vec3 surface = craniumCentre + from / s;
        return Seat{surface, glm::normalize((surface - craniumCentre) / (craniumRadii * craniumRadii))};
    };
    const float socket = hw * 0.14f;
    // A brow over each eye, low and close to the skull: heavy enough to shade the socket, never a horn.
    if (!pose.eyes.empty())
    {
        for (const float side : {-1.0f, 1.0f})
        {
            const Seat over = onSkull(pose.eyes.size() >= 2 ? pose.eyes[side > 0.0f ? 1 : 0]
                                                            : pose.head + glm::vec3(side * hw * 0.21f, hd * 0.1f, -hl * 0.3f));
            blob(over.at + glm::vec3(0.0f, socket * 0.8f, 0.0f) - over.normal * (socket * 0.3f),
                 {hw * 0.14f, hd * (0.025f + 0.03f * a.brow), hl * 0.07f},
                 glm::angleAxis(side * glm::radians(10.0f), glm::vec3(0.0f, 0.0f, 1.0f)), skin.head, skin.cell * 1.5f, Zone::Skin);
        }
    }
    // An eyeless, pitted face: rows of small deep pits down either side of the snout and across the
    // brow where eyes would be.
    if (a.headShape == HeadShape::Pitted)
    {
        for (const float side : {-1.0f, 1.0f})
        {
            for (int row = 0; row < 3; ++row)
            {
                for (int k = 0; k < 4; ++k)
                {
                    const float along = static_cast<float>(k) / 3.0f;
                    const glm::vec3 wanted = pose.head + glm::vec3(side * hw * (0.16f + 0.07f * static_cast<float>(row)),
                                                                   hd * (0.12f - 0.1f * static_cast<float>(row)),
                                                                   -hl * (0.12f + 0.3f * along * snout));
                    const Seat seat = onSkull(wanted);
                    const float size = hw * (0.035f - 0.006f * static_cast<float>(row)) * (1.0f - 0.25f * along);
                    blob(seat.at - seat.normal * (size * 0.2f), glm::vec3(std::max(size, skin.cell * 0.9f)), noTurn, skin.head,
                         skin.cell * 0.6f, Zone::Hollow, true);
                }
            }
        }
    }
    // The eyes' sockets.
    for (size_t e = 0; e < pose.eyes.size(); ++e)
    {
        const float row = static_cast<float>(e / 2);
        const float size = socket * (1.0f - 0.22f * row);
        const float side = pose.eyes[e].x > pose.head.x ? 1.0f : -1.0f;
        const Seat seat = onSkull(pose.eyes[e]);
        const glm::quat facing = TurnBetween(glm::vec3(0.0f, 0.0f, -1.0f), seat.normal) *
                                 glm::angleAxis(side * glm::radians(18.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        blob(seat.at - seat.normal * (size * 0.35f), {size * 1.1f, size * 0.8f, size}, facing, skin.head, skin.cell * 0.8f,
             Zone::Hollow, true);
        skin.glints.push_back(seat.at - seat.normal * (size * 1.05f));
    }
    skin.glintRadius = socket * 0.2f;
    if (pose.eyes.empty() && a.headShape != HeadShape::Pitted)
    {
        // No eyes at all: the skin grown over where they would be, in two shallow dents.
        for (const float side : {-1.0f, 1.0f})
        {
            const Seat seat = onSkull(pose.head + glm::vec3(side * hw * 0.21f, hd * 0.1f, -hl * 0.3f));
            blob(seat.at + seat.normal * (socket * 0.3f), glm::vec3(socket * 0.9f), noTurn, skin.head, skin.cell * 1.5f,
                 Zone::Skin, true);
        }
    }
    // Two slits for nostrils on top of the tip of the face.
    {
        const JawRod tip = upperRod(0.1f);
        for (const float side : {-1.0f, 1.0f})
        {
            blob(pose.head + glm::vec3(side * hw * 0.04f, gumLine + tip.radius * 1.9f, alongFace(0.1f)),
                 {hw * 0.02f, hd * 0.03f, hl * 0.05f}, glm::angleAxis(side * glm::radians(-15.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
                 skin.head, skin.cell * 0.6f, Zone::Hollow, true);
        }
    }

    // The limbs. Each segment is a bone with muscle over it: two bellies on the upper segment, front and
    // back, swelling out of the body at the root; a joint that is two knuckles side by side with a point
    // on the outside of the bend, where the kneecap or the elbow is; a calf or forearm belly high on the
    // lower segment that thins to tendons, so the ankle and the wrist are cords over bone; and the root of
    // the limb grown out of the torso through a mass of shoulder or haunch, not pushed into its side.
    for (size_t i = 0; i < limbShapes.size(); ++i)
    {
        const LimbShape& shape = limbShapes[i];
        const Limb& limb = skin.limbs[i];
        const bool arm = pose.legs[i].pair->arm;
        const float t = pose.legs[i].pair->thickness;
        // How much muscle: a starving one is sinew, but even sinew is shaped.
        const float meat = std::clamp(crawler ? (arm ? 0.45f : 0.6f) : 1.05f - 0.55f * ribs, 0.35f, 1.05f);
        const glm::vec3 upperAlong = glm::normalize(shape.knee - shape.hip);
        const glm::vec3 lowerAlong = glm::normalize(shape.ankle - shape.knee);
        const float upperLength = glm::distance(shape.hip, shape.knee);
        const float lowerLength = glm::distance(shape.knee, shape.ankle);
        // The outside of the bend, where the kneecap or the point of the elbow is, and the axis the joint
        // hinges on.
        glm::vec3 outside = upperAlong - lowerAlong;
        outside = glm::length(outside) > 1e-4f ? glm::normalize(outside) : glm::normalize(glm::cross(limb.bend, upperAlong));
        const glm::vec3 jointAxis = limb.bend;

        // The bones.
        cone(shape.hip, shape.knee, shape.radius.x * 0.8f, shape.radius.y, limb.upper, 0.04f * scale);
        cone(shape.knee, shape.ankle, shape.radius.y, shape.radius.z, limb.lower, skin.cell * 1.5f);
        cone(shape.ankle, shape.toe, shape.radius.z, shape.radius.w, limb.end, skin.cell * 1.5f);

        // Shoulder or haunch: a mass from the body over the top of the limb, so the limb comes out of
        // the torso rather than being stuck onto it.
        {
            const glm::vec3 root = shape.hip + upperAlong * (upperLength * 0.12f);
            const glm::vec3 inwards = glm::normalize(glm::vec3(-shape.hip.x, 0.0f, 0.0f) + glm::vec3(0.0f, 0.35f, 0.0f));
            const glm::quat along = TurnBetween(glm::vec3(0.0f, 1.0f, 0.0f), upperAlong);
            blob(root + inwards * (t * 0.35f), glm::vec3(t * (1.35f + 0.25f * meat), upperLength * 0.26f, t * (1.25f + 0.2f * meat)),
                 along, limb.upper, 0.05f * scale);
        }
        // Upper segment: two bellies, the front one fuller, tapering into the joint.
        const glm::quat upperTurn = TurnBetween(glm::vec3(0.0f, 1.0f, 0.0f), upperAlong);
        for (const float face : {1.0f, -1.0f})
        {
            const float at = face > 0.0f ? 0.36f : 0.44f;
            const glm::vec3 centre = glm::mix(shape.hip, shape.knee, at) + outside * (face * t * 0.32f);
            const float girth = t * (face > 0.0f ? 0.78f : 0.66f) * (0.75f + 0.3f * meat);
            blob(centre, {girth, upperLength * (face > 0.0f ? 0.3f : 0.26f), girth * 0.9f}, upperTurn, limb.upper, skin.cell * 2.2f);
        }
        // The joint.
        for (const float side : {-1.0f, 1.0f})
        {
            blob(shape.knee + jointAxis * (side * shape.radius.y * 0.55f) - upperAlong * (shape.radius.y * 0.2f),
                 glm::vec3(shape.radius.y * 0.62f), noTurn, limb.lower, skin.cell * 1.3f, Zone::Bone);
        }
        blob(shape.knee + outside * (shape.radius.y * 0.7f), glm::vec3(shape.radius.y * 0.5f, shape.radius.y * 0.6f, shape.radius.y * 0.5f),
             upperTurn, limb.lower, skin.cell * 1.2f, Zone::Bone);
        // Lower segment: a belly high on the inside of the bend, thinning to two tendons at the far joint.
        const glm::quat lowerTurn = TurnBetween(glm::vec3(0.0f, 1.0f, 0.0f), lowerAlong);
        {
            const glm::vec3 belly = glm::mix(shape.knee, shape.ankle, 0.3f) - outside * (t * 0.22f);
            const float girth = t * 0.6f * (0.7f + 0.35f * meat);
            blob(belly, {girth, lowerLength * 0.24f, girth * 0.95f}, lowerTurn, limb.lower, skin.cell * 2.2f);
        }
        for (const float side : {-1.0f, 1.0f})
        {
            const glm::vec3 from = glm::mix(shape.knee, shape.ankle, 0.45f) + jointAxis * (side * t * 0.2f) - outside * (t * 0.12f);
            const glm::vec3 to = shape.ankle + jointAxis * (side * shape.radius.z * 0.55f) - outside * (shape.radius.z * 0.3f);
            cone(from, to, std::max(t * 0.14f, skin.cell * 0.9f), std::max(t * 0.1f, skin.cell * 0.85f), limb.lower, skin.cell * 1.1f);
        }
        // The far joint: a knuckle of bone either side.
        for (const float side : {-1.0f, 1.0f})
        {
            blob(shape.ankle + jointAxis * (side * shape.radius.z * 0.6f), glm::vec3(shape.radius.z * 0.55f), noTurn, limb.end,
                 skin.cell * 1.1f, Zone::Bone);
        }
        // The palm or the sole: flat, wider than the wrist, the bones of it showing on the back.
        const glm::vec3 palm = glm::mix(shape.ankle, shape.toe, 0.7f) + glm::vec3(0.0f, -shape.radius.w * 0.2f, 0.0f);
        blob(palm, {std::max(shape.radius.w * 2.1f, skin.cell * 1.4f), std::max(shape.radius.w * 0.8f, skin.cell * 1.05f),
                    glm::distance(shape.ankle, shape.toe) * 0.4f},
             noTurn, limb.end, skin.cell * 1.5f);
        const glm::vec3 toward = glm::normalize(shape.toe - shape.ankle);
        for (int k = -1; k <= 1; ++k)
        {
            const glm::vec3 across = glm::normalize(glm::cross(toward, glm::vec3(0.0f, 1.0f, 0.0f)));
            const glm::vec3 base = shape.ankle + across * (static_cast<float>(k) * shape.radius.w * 0.7f) + glm::vec3(0.0f, shape.radius.w * 0.35f, 0.0f);
            cone(base, base + toward * glm::distance(shape.ankle, shape.toe) * 0.8f, std::max(shape.radius.w * 0.28f, skin.cell * 0.85f),
                 std::max(shape.radius.w * 0.22f, skin.cell * 0.8f), limb.end, skin.cell * 0.9f, Zone::Bone);
        }
        (void)arm;
    }

    // Across the top of the shoulders on a crawler: the muscle from the neck out to each arm, which is
    // what makes the arms part of the same body as the head.
    if (crawler)
    {
        for (size_t i = 0; i < pose.legs.size(); ++i)
        {
            if (pose.legs[i].pair->arm && pose.legs[i].pair->along < 0.1f)
            {
                const glm::vec3 shoulder = limbShapes[i].hip + glm::vec3(0.0f, pose.legs[i].pair->thickness * 0.6f, 0.0f);
                cone(pose.neckBase + glm::vec3(0.0f, neckRoot * 0.2f, neckRoot * 0.3f), shoulder, neckRoot * 0.55f,
                     pose.legs[i].pair->thickness * 0.9f, skin.chest, 0.05f * scale);
            }
        }
    }

    // Growths: lumps under the skin, clustered, on the body and the tops of the limbs. Some are big.
    if (a.growths > 0.0f)
    {
        const int count = 4 + static_cast<int>(a.growths * 16.0f);
        for (int g = 0; g < count; ++g)
        {
            const float pick = Detail(a.seed, 900u + static_cast<uint32_t>(g) * 4u);
            const float size = (0.018f + 0.05f * Detail(a.seed, 901u + static_cast<uint32_t>(g) * 4u) * Detail(a.seed, 903u + static_cast<uint32_t>(g) * 4u)) *
                               scale * (0.6f + 0.6f * a.growths);
            if (pick < 0.7f || limbShapes.empty())
            {
                const float along = 0.12f + 0.8f * Detail(a.seed, 902u + static_cast<uint32_t>(g) * 4u);
                const float round = (Detail(a.seed, 905u + static_cast<uint32_t>(g) * 4u) - 0.5f) * glm::pi<float>() * 1.6f;
                blob(onTorso(along, round, 0.97f), glm::vec3(size, size * 0.8f, size), noTurn, spineBoneAt(along), size * 0.6f);
            }
            else
            {
                const size_t limb = static_cast<size_t>(Detail(a.seed, 906u + static_cast<uint32_t>(g) * 4u) * static_cast<float>(limbShapes.size())) %
                                    limbShapes.size();
                const LimbShape& shape = limbShapes[limb];
                const glm::vec3 at = glm::mix(shape.hip, shape.knee, 0.2f + 0.5f * Detail(a.seed, 907u + static_cast<uint32_t>(g) * 4u));
                blob(at + glm::vec3(0.0f, shape.radius.x * 0.7f, 0.0f), glm::vec3(size * 0.8f), noTurn, skin.limbs[limb].upper, size * 0.5f);
            }
        }
    }
    // A hammer head's two ends, swept out either side of the skull to where the eyes are.
    if (a.headShape == HeadShape::Hammer)
    {
        for (const float side : {-1.0f, 1.0f})
        {
            blob(pose.head + glm::vec3(side * hw * 0.34f, hd * 0.05f, -hl * 0.15f), {hw * 0.18f, hd * 0.26f, hl * 0.2f}, noTurn, skin.head,
                 skin.cell * 2.0f);
        }
    }

    // The tail.
    for (size_t i = 0; i < skin.tail.size(); ++i)
    {
        const float u0 = static_cast<float>(i) / static_cast<float>(skin.tail.size());
        const float u1 = static_cast<float>(i + 1) / static_cast<float>(skin.tail.size());
        cone(tailPoints[i], tailPoints[i + 1], std::max(a.tailThickness * (1.0f - 0.85f * u0), skin.cell * 1.05f),
             std::max(a.tailThickness * (1.0f - 0.85f * u1), skin.cell * 1.05f), skin.tail[i],
             i == 0 ? 0.05f * scale : skin.cell * 1.5f);
    }

    sculpture.Finish(skin.cell);

    // --- The skin -----------------------------------------------------------------------------------
    const DistanceGrid grid =
        SampleDistance(sculpture.Lo(), sculpture.Hi(), skin.cell, [&](const glm::vec3& p) { return sculpture.Distance(p); });
    MeshData surface = SurfaceNets(grid);

    // The colours it is painted in, before shading.
    const glm::vec3 skinColour = a.skin;
    const glm::vec3 boneColour = glm::mix(a.skin, glm::vec3(0.72f, 0.68f, 0.58f), 0.3f + 0.25f * ribs);
    std::array<glm::vec3, static_cast<size_t>(Zone::Count)> palette{};
    palette[static_cast<size_t>(Zone::Skin)] = skinColour;
    palette[static_cast<size_t>(Zone::Bone)] = boneColour;
    palette[static_cast<size_t>(Zone::Hollow)] = glm::vec3(0.01f, 0.008f, 0.008f);
    palette[static_cast<size_t>(Zone::Gums)] = glm::vec3(0.16f, 0.025f, 0.03f);
    palette[static_cast<size_t>(Zone::Teeth)] = glm::vec3(0.66f, 0.6f, 0.46f);
    palette[static_cast<size_t>(Zone::Claw)] = glm::vec3(0.07f, 0.06f, 0.055f);
    palette[static_cast<size_t>(Zone::Plate)] = a.skin * 0.62f;
    palette[static_cast<size_t>(Zone::Membrane)] = glm::mix(a.skin, glm::vec3(0.35f, 0.12f, 0.12f), 0.35f) * 0.8f;
    std::array<float, static_cast<size_t>(Zone::Count)> wetness{};
    wetness.fill(1.0f);
    wetness[static_cast<size_t>(Zone::Gums)] = 0.45f;
    wetness[static_cast<size_t>(Zone::Teeth)] = 0.6f;
    wetness[static_cast<size_t>(Zone::Claw)] = 0.45f;
    wetness[static_cast<size_t>(Zone::Hollow)] = 0.7f;
    wetness[static_cast<size_t>(Zone::Bone)] = 0.85f;

    const size_t boneCount = skin.bones.size();
    const float weightSpread = std::max(0.018f * scale, skin.cell * 1.3f);
    const float colourSpread = skin.cell * 0.9f;

    skin.weights.resize(surface.vertices.size());
    ParallelFor(surface.vertices.size(), [&](size_t first, size_t last) {
    std::vector<float> boneDistance(boneCount);
    for (size_t v = first; v < last; ++v)
    {
        MeshVertex& vertex = surface.vertices[v];
        // Onto the surface itself, from where the grid put it, and its normal from the shape.
        glm::vec3 p = vertex.position;
        glm::vec3 normal = sculpture.Gradient(p, skin.cell * 0.5f);
        p -= normal * sculpture.Distance(p);
        normal = sculpture.Gradient(p, skin.cell * 0.5f);
        vertex.position = p;
        vertex.normal = normal;

        // Which bones it follows and what it is painted as, from the shapes nearest it.
        std::fill(boneDistance.begin(), boneDistance.end(), std::numeric_limits<float>::max());
        std::array<float, static_cast<size_t>(Zone::Count)> zoneWeight{};
        float nearest = std::numeric_limits<float>::max();
        const std::vector<uint16_t>* near = sculpture.Near(p);
        std::vector<std::pair<float, Zone>> zones;
        if (near != nullptr)
        {
            for (const uint16_t index : *near)
            {
                const Shape& shape = sculpture.shapes[index];
                const float d = shape.Distance(p);
                if (shape.cut)
                {
                    // On the wall of something carved out, it is painted as what was carved: the
                    // inside of a socket is dark however much bone it went through.
                    const float onWall = 1.0f - std::abs(d) / (skin.cell * 1.6f);
                    if (onWall > 0.0f)
                    {
                        zoneWeight[static_cast<size_t>(shape.zone)] += onWall * 4.0f;
                    }
                    continue;
                }
                boneDistance[static_cast<size_t>(shape.bone)] = std::min(boneDistance[static_cast<size_t>(shape.bone)], d);
                nearest = std::min(nearest, d);
                zones.emplace_back(d, shape.zone);
            }
        }
        for (const auto& [d, zone] : zones)
        {
            zoneWeight[static_cast<size_t>(zone)] += std::exp(-(d - nearest) / colourSpread);
        }

        // The four nearest bones, weighted by how near.
        std::array<std::pair<float, int>, 4> best{};
        best.fill({std::numeric_limits<float>::max(), 0});
        for (size_t b = 0; b < boneCount; ++b)
        {
            const float d = boneDistance[b];
            if (d >= best[3].first)
            {
                continue;
            }
            best[3] = {d, static_cast<int>(b)};
            std::sort(best.begin(), best.end());
        }
        SkinWeights& weights = skin.weights[v];
        float total = 0.0f;
        for (size_t k = 0; k < 4; ++k)
        {
            const float w = best[k].first < std::numeric_limits<float>::max()
                                ? std::exp(-(best[k].first - best[0].first) / weightSpread)
                                : 0.0f;
            weights.bone[k] = static_cast<uint8_t>(best[k].second);
            weights.weight[k] = w < 0.02f ? 0.0f : w;
            total += weights.weight[k];
        }
        if (total <= 0.0f)
        {
            weights.bone = {static_cast<uint8_t>(skin.pelvis), 0, 0, 0};
            weights.weight = {1.0f, 0.0f, 0.0f, 0.0f};
        }
        else
        {
            for (float& w : weights.weight)
            {
                w /= total;
            }
        }

        // The paint.
        glm::vec3 colour{0.0f};
        float wet = 0.0f;
        float zoneTotal = 0.0f;
        for (size_t z = 0; z < zoneWeight.size(); ++z)
        {
            colour += palette[z] * zoneWeight[z];
            wet += wetness[z] * zoneWeight[z];
            zoneTotal += zoneWeight[z];
        }
        if (zoneTotal > 0.0f)
        {
            colour /= zoneTotal;
            wet /= zoneTotal;
        }
        else
        {
            colour = skinColour;
            wet = 1.0f;
        }
        // One flat colour for each part and nothing painted on top: the surface detail is for textures,
        // which will come from outside the code. Mottling, spots, stripes and veins used to be worked out
        // here, and would only fight whatever is laid over them.
        // And darker in the creases -- between the ribs, in the armpits, deep in the sockets -- from how
        // much of the body is close around it.
        float occlusion = 0.0f;
        float weight = 1.0f;
        for (const float step : {skin.cell * 1.5f, skin.cell * 3.5f, skin.cell * 7.0f})
        {
            occlusion += weight * std::max(0.0f, step - sculpture.Distance(p + normal * step)) / step;
            weight *= 0.5f;
        }
        colour *= std::clamp(1.0f - occlusion * 0.75f, 0.3f, 1.0f);
        vertex.color = PackColour(colour, wet);
        vertex.uv = {0.0f, 0.0f};
    }
    }, 128);

    // Far fewer triangles for nearly the same shape: the grid puts as many on a flat back as on a
    // knuckle, and a body bent on the processor every frame should not carry fifty thousand.
    {
        const size_t target = static_cast<size_t>(std::clamp(9000.0f * scale, 6500.0f, 16000.0f));
        std::vector<uint32_t> kept;
        MeshData simple = SimplifyMesh(surface, target, 0.004f, kept);
        std::vector<SkinWeights> weights(kept.size());
        for (size_t v = 0; v < kept.size(); ++v)
        {
            weights[v] = skin.weights[kept[v]];
        }
        skin.weights = std::move(weights);
        surface = std::move(simple);
    }
    skin.mesh = std::move(surface);

    // --- Things fixed to the bones --------------------------------------------------------------------
    std::vector<Attached> attached;
    const auto attach = [&](MeshData mesh, int bone, Zone zone) { attached.push_back({std::move(mesh), bone, zone}); };

    // Teeth: no two alike, and every one sized to where it closes. An upper row along each side of the
    // mouth, pointing down, out past the lower jaw and never lower than its chin; a longer fang a little
    // back from the tip, where a dog's is; smaller at the back. A lower row inside it, pointing up into the
    // palate, short enough to meet it and no more. Worked out for the jaw shut, so nothing passes through
    // anything whichever way the rig turns it.
    {
        const int perSide = std::clamp(a.teeth / 2 + 2, 4, 9);
        MeshData upper;
        MeshData lower;
        for (const float side : {-1.0f, 1.0f})
        {
            for (int k = 0; k < perSide; ++k)
            {
                const uint32_t id = static_cast<uint32_t>(k) + (side > 0.0f ? 100u : 0u);
                // From just behind the tip (0) back towards the corner of the mouth.
                const float t = 0.06f + 0.84f * (static_cast<float>(k) + 0.5f * Detail(a.seed, 400u + id)) / static_cast<float>(perSide);
                const float z = alongFace(t);
                const float fang = std::exp(-std::pow((t - 0.2f) / 0.08f, 2.0f));
                const JawRod upperJaw = upperRod(t);
                const JawRod lowerJaw = lowerRod(std::clamp((z - hl * 0.03f - faceTipZ) / (mouthCornerZ - faceTipZ), 0.0f, 1.0f));

                // Upper: rooted in the gum on the underside of the rod, as far out as it can be.
                const float radius = std::max(hw * (0.018f + 0.02f * fang) * (1.0f - 0.3f * t), 0.0025f);
                const float x = upperJaw.out + upperJaw.radius * 0.72f;
                const float rootY = gumLine + upperJaw.radius * (1.0f - std::sqrt(1.0f - 0.72f * 0.72f));
                // The lower jaw beside it, shut: how far down the tooth can go before it would touch it.
                const float clear = x - radius - lowerJaw.out;
                float reach = lowerJaw.radius * 1.7f; // past it altogether, to just above the chin
                if (clear < lowerJaw.radius)
                {
                    reach = lowerJaw.radius - std::sqrt(std::max(lowerJaw.radius * lowerJaw.radius - clear * clear, 0.0f));
                }
                float length = hd * a.toothLength * (0.075f + 0.04f * Detail(a.seed, id) + 0.1f * fang);
                length = std::min(length, (rootY - lowerGum) + reach);
                if (Detail(a.seed, 320u + id) < 0.1f)
                {
                    length *= 0.55f; // broken
                }
                if (length > radius * 1.5f)
                {
                    const glm::vec3 root = pose.head + glm::vec3(side * x, rootY + radius, z);
                    const glm::vec3 tip = pose.head + glm::vec3(side * (x - radius * 0.3f), rootY - length, z + length * 0.12f);
                    upper.Append(CurvedTooth(root, tip, glm::normalize(glm::vec3(-side * 0.6f, 0.0f, 0.5f)), radius, 0.7f), identity);
                }

                // Lower: between the upper ones, inside them, up into the palate.
                if (k == perSide - 1)
                {
                    continue;
                }
                const float lowerRadius = std::max(radius * 0.8f, 0.0022f);
                const float lx = lowerJaw.out + lowerJaw.radius * 0.35f;
                const float palate = hd * 0.05f * 0.8f;
                float lowerLength = hd * a.toothLength * (0.035f + 0.025f * Detail(a.seed, 50u + id));
                lowerLength = std::min(lowerLength, jawGap + palate);
                if (lowerLength > lowerRadius * 1.5f && lx + lowerRadius < x - radius)
                {
                    const float lz = alongFace(t + 0.42f / static_cast<float>(perSide)) + hl * 0.03f;
                    const glm::vec3 root = onJaw(pose.head + glm::vec3(side * lx, lowerGum - lowerRadius * 1.2f, lz));
                    const glm::vec3 tip = onJaw(pose.head + glm::vec3(side * lx, lowerGum + lowerLength, lz - lowerLength * 0.1f));
                    lower.Append(CurvedTooth(root, tip, glm::normalize(glm::vec3(-side * 0.5f, 0.0f, 0.5f)), lowerRadius, 0.7f), identity);
                }
            }
        }
        attach(std::move(upper), skin.head, Zone::Teeth);
        if (!lower.vertices.empty())
        {
            attach(std::move(lower), skin.jaw, Zone::Teeth);
        }
    }

    // Fingers and toes, and the claws on them: too fine to sculpt, so built as they were and fixed to
    // the hand. The fingers are painted as skin and sink into the palm, so they read as part of it.
    for (size_t i = 0; i < limbShapes.size(); ++i)
    {
        const LimbShape& shape = limbShapes[i];
        const LegPair& pair = *pose.legs[i].pair;
        const int bone = skin.limbs[i].end;
        MeshData fingers;
        MeshData claws;
        const int digits = std::max(a.fingers - (pair.arm ? 0 : 1), 2);
        const float groundBelow = shape.toe.y;
        // Long enough on a foot to read as toes: at a sixth of the foot they were lost in the sole, and
        // every foot looked like a club.
        const float fingerLength = pair.arm ? pair.foot * 0.75f : (crawler ? pair.foot * 0.4f : pair.foot * 0.32f);
        const float clawLength = pair.foot * (pair.arm ? 0.35f : 0.3f) * a.clawLength;
        const float fingerRadius = std::max(shape.radius.w * (pair.arm ? 0.42f : 0.5f), 0.005f);
        for (int f = 0; f < digits; ++f)
        {
            const float u = static_cast<float>(f) / static_cast<float>(digits - 1) - 0.5f;
            const float splay = u * (pair.arm ? 1.1f : 0.8f) +
                                (Detail(a.seed, 200u + static_cast<uint32_t>(i * 8 + static_cast<size_t>(f))) - 0.5f) * 0.2f;
            const glm::vec3 flat{std::sin(splay), 0.0f, -std::cos(splay)};
            const glm::vec3 base = shape.toe + flat * shape.radius.w * 0.4f;
            const float length = fingerLength * (1.0f - 0.3f * std::abs(u) * 2.0f);
            glm::vec3 knuckle = base;
            if (length > 0.005f)
            {
                // Two joints, the second curling down to the floor.
                const glm::vec3 middle = base + glm::normalize(flat + glm::vec3(0.0f, 0.05f, 0.0f)) * (length * 0.55f);
                knuckle = middle + glm::normalize(flat + glm::vec3(0.0f, -0.35f, 0.0f)) * (length * 0.45f);
                const auto segment = [&](const glm::vec3& from, const glm::vec3& to, float r0, float r1)
                {
                    MeshData piece = Primitives::Frustum(r0, r1, glm::distance(from, to), 8);
                    piece.Append(Primitives::Sphere(r1 * 1.15f, 8, 6), glm::translate(identity, glm::vec3(0.0f, glm::distance(from, to) * 0.5f, 0.0f)));
                    fingers.Append(piece, Between(from, to));
                };
                segment(base, middle, fingerRadius, fingerRadius * 0.85f);
                segment(middle, knuckle, fingerRadius * 0.85f, fingerRadius * 0.7f);
            }
            const float drop = std::clamp((-groundBelow * 0.95f - (knuckle.y - shape.toe.y)) / std::max(clawLength, 1e-3f), -0.95f, 0.3f);
            const glm::vec3 tip = knuckle + (flat * std::sqrt(1.0f - drop * drop) + glm::vec3(0.0f, drop, 0.0f)) * clawLength;
            claws.Append(Primitives::Frustum(fingerRadius * 0.75f, 0.0f, glm::distance(knuckle, tip), 7), Between(knuckle, tip));
        }
        if (!fingers.vertices.empty())
        {
            attach(std::move(fingers), bone, Zone::Skin);
        }
        attach(std::move(claws), bone, Zone::Claw);
    }

    // Spurs along the spine, and the frills that catch sound.
    if (a.spines > 0)
    {
        for (int s = 0; s < a.spines; ++s)
        {
            const float t = 0.12f + 0.8f * (static_cast<float>(s) + 0.5f) / static_cast<float>(a.spines);
            const glm::vec3 base = onTorso(t, 0.0f, 0.95f);
            const float length = a.spineLength * (0.7f + 0.6f * Detail(a.seed, 100u + static_cast<uint32_t>(s)));
            const glm::vec3 tip = base + glm::normalize(glm::vec3(0.0f, 1.0f, 0.5f)) * length;
            MeshData spur = Primitives::Frustum(length * 0.18f, 0.0f, glm::distance(base, tip), 8);
            MeshData placed;
            placed.Append(spur, Between(base, tip));
            attach(std::move(placed), spineBoneAt(t), Zone::Teeth);
        }
    }

    for (Attached& piece : attached)
    {
        const size_t first = skin.mesh.vertices.size();
        const glm::vec3 base = palette[static_cast<size_t>(piece.zone)];
        const float wet = wetness[static_cast<size_t>(piece.zone)];
        for (MeshVertex& vertex : piece.mesh.vertices)
        {
            const glm::vec3 colour = piece.zone == Zone::Skin ? skinColour : base;
            vertex.color = PackColour(colour, wet);
        }
        skin.mesh.Append(piece.mesh, identity);
        for (size_t v = first; v < skin.mesh.vertices.size(); ++v)
        {
            SkinWeights weights;
            weights.bone = {static_cast<uint8_t>(piece.bone), 0, 0, 0};
            weights.weight = {1.0f, 0.0f, 0.0f, 0.0f};
            skin.weights.push_back(weights);
        }
    }
    return skin;
}

void SkinVertices(const CreatureSkin& skin, const std::vector<glm::mat4>& skinning, std::vector<MeshVertex>& out,
                  AABB& bounds)
{
    const size_t count = skin.mesh.vertices.size();
    out.resize(count);
    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(-std::numeric_limits<float>::max());
    for (size_t v = 0; v < count; ++v)
    {
        const MeshVertex& source = skin.mesh.vertices[v];
        const SkinWeights& weights = skin.weights[v];
        glm::mat4 blended = skinning[weights.bone[0]] * weights.weight[0];
        for (size_t k = 1; k < 4; ++k)
        {
            if (weights.weight[k] > 0.0f)
            {
                blended += skinning[weights.bone[k]] * weights.weight[k];
            }
        }
        MeshVertex& vertex = out[v];
        vertex.position = glm::vec3(blended * glm::vec4(source.position, 1.0f));
        const glm::vec3 normal = glm::mat3(blended) * source.normal;
        const float length = glm::length(normal);
        vertex.normal = length > 1e-8f ? normal / length : source.normal;
        vertex.uv = source.uv;
        vertex.color = source.color;
        lo = glm::min(lo, vertex.position);
        hi = glm::max(hi, vertex.position);
    }
    bounds.min = lo;
    bounds.max = hi;
}

} // namespace pred
