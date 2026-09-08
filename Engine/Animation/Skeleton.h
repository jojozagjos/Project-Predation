#pragma once

#include "Engine/Core/Math.h"

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pred
{

// Index into a Skeleton's bone array. Kept as a plain index rather than a handle because a pose is
// always evaluated against the skeleton it was built for.
using BoneIndex = int;
inline constexpr BoneIndex kInvalidBone = -1;

struct Bone
{
    std::string name;
    BoneIndex parent = kInvalidBone;
    Transform bindLocal; // rest pose, relative to the parent
};

// A bone hierarchy.
//
// Bones are stored so that every parent appears before its children. That single invariant means
// global transforms can be computed in one forward pass with no recursion and no sorting, which is
// what keeps pose evaluation cheap enough to run for several creatures per frame.
class Skeleton
{
public:
    BoneIndex AddBone(std::string name, BoneIndex parent, const Transform& bindLocal);

    int BoneCount() const { return static_cast<int>(m_bones.size()); }
    const Bone& GetBone(BoneIndex index) const { return m_bones[static_cast<size_t>(index)]; }
    const std::vector<Bone>& Bones() const { return m_bones; }

    BoneIndex Find(std::string_view name) const;
    bool IsValid(BoneIndex index) const { return index >= 0 && index < BoneCount(); }

    // True when every bone's parent precedes it, which pose evaluation relies on.
    bool ValidateOrdering() const;

private:
    std::vector<Bone> m_bones;
};

// Local-space transforms for every bone in a skeleton, plus their evaluated global transforms.
class Pose
{
public:
    void ResetToBind(const Skeleton& skeleton);

    Transform& Local(BoneIndex index) { return m_local[static_cast<size_t>(index)]; }
    const Transform& Local(BoneIndex index) const { return m_local[static_cast<size_t>(index)]; }

    const glm::mat4& Global(BoneIndex index) const { return m_global[static_cast<size_t>(index)]; }
    glm::vec3 GlobalPosition(BoneIndex index) const { return glm::vec3(Global(index)[3]); }

    // Recomputes every global transform. `root` places the whole skeleton in the world.
    void ComputeGlobals(const Skeleton& skeleton, const glm::mat4& root = glm::mat4(1.0f));

    // Writes a global transform directly, then rebuilds that bone's descendants. Used by IK, which
    // naturally produces global orientations.
    void SetGlobal(const Skeleton& skeleton, BoneIndex index, const glm::mat4& global);

    int Size() const { return static_cast<int>(m_local.size()); }
    bool IsEmpty() const { return m_local.empty(); }

private:
    std::vector<Transform> m_local;
    std::vector<glm::mat4> m_global;
};

} // namespace pred
