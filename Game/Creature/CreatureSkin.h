#pragma once

#include "Engine/Core/Math.h"
#include "Engine/Render/Mesh.h"
#include "Game/Creature/CreatureAnatomy.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace pred
{

// What each bone of a creature is for: how it moves, how it can be hit, and what it falls as.
enum class BoneKind : uint8_t
{
    Pelvis,
    Spine,
    Chest,
    Neck,
    Head,
    Jaw,
    Upper, // hip to knee, or shoulder to elbow
    Lower, // knee to ankle, or elbow to wrist
    End,   // the foot or the hand
    Tail
};

const char* BoneKindName(BoneKind kind);

struct SkinBone
{
    BoneKind kind = BoneKind::Spine;
    int parent = -1;
    int limb = -1;          // which of the rest pose's legs, for Upper, Lower and End
    glm::vec3 head{0.0f};   // where it starts at rest, in the creature's own frame
    glm::vec3 tail{0.0f};   // and where it ends
    float radius = 0.05f;   // how thick it is, for being hit and for falling
    glm::mat4 rest{1.0f};   // its frame at rest: origin at the head, +Y towards the tail
    glm::mat4 restInverse{1.0f};
};

// Up to four bones a vertex follows, and how much of each.
struct SkinWeights
{
    std::array<uint8_t, 4> bone{};
    std::array<float, 4> weight{};
};

// A creature's body as one mesh on a skeleton.
//
// The body is not assembled from parts any more. Its bones, muscles, ribs, skull and joints are
// described as shapes -- rounded cones and ellipsoids, blended smoothly into one another and with
// sockets, nostrils and a mouth carved out of them -- and a single skin is drawn over the lot, the
// way a sculptor's clay covers an armature. Every vertex of that skin is painted (skin, bone showing
// through, gums, sockets, claws) and told which bones it follows, and the skeleton is posed every
// frame to bend it. A knee is where the thigh and the shin are the same surface; a neck is not a tube
// pushed into a head.
//
// Built once per creature from its anatomy, in its own frame: forward -Z, up +Y, the ground under
// the middle of its body at the origin. The same seed builds the same skin on every machine.
struct CreatureSkin
{
    std::vector<SkinBone> bones;
    MeshData mesh;                    // at rest, painted per vertex
    std::vector<SkinWeights> weights; // one per vertex of `mesh`
    // Where the pinpricks of light in its eye sockets sit at rest. They move with the head.
    std::vector<glm::vec3> glints;
    float glintRadius = 0.004f;

    int pelvis = -1;
    int chest = -1;
    int head = -1;
    int jaw = -1;
    std::vector<int> spine; // pelvis to chest, in order
    std::vector<int> neck;  // shoulders to skull, in order
    std::vector<int> tail;  // root to tip
    struct Limb
    {
        int upper = -1;
        int lower = -1;
        int end = -1;
        bool arm = false;
        // Which way the limb bends at rest, square to its plane; the frames of its bones are built
        // round it, at rest and when posed, so the two agree.
        glm::vec3 bend{1.0f, 0.0f, 0.0f};
    };
    std::vector<Limb> limbs; // in the same order as the rest pose's legs

    // How finely it was sampled, in metres: finer for a smaller animal.
    float cell = 0.015f;

    static CreatureSkin Build(const CreatureAnatomy& anatomy);
};

// A bone's frame: origin at `head`, +Y along to `tail`, +Z as near to `up` as it can be while square
// to that. Every bone is framed this way, at rest and when posed.
glm::mat4 BoneFrame(const glm::vec3& head, const glm::vec3& tail, const glm::vec3& up);

// Which way a leg's knee (or an arm's elbow) goes: the pole the limb is solved towards. One answer for
// the rest pose and for every frame after, or the skin would be built on one knee and bent by another.
glm::vec3 LimbPole(const CreatureAnatomy& anatomy, const CreatureAnatomy::Leg& leg);

// The skin bent by a pose: one matrix per bone, taking the bone from its rest frame to where it is
// now, in the creature's own frame. Writes every vertex into `out` and the bounds of the lot.
void SkinVertices(const CreatureSkin& skin, const std::vector<glm::mat4>& skinning, std::vector<MeshVertex>& out,
                  AABB& bounds);

} // namespace pred
