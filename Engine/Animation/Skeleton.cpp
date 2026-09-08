#include "Engine/Animation/Skeleton.h"

#include "Engine/Core/Log.h"

#include <glm/gtc/matrix_inverse.hpp>

namespace pred
{

BoneIndex Skeleton::AddBone(std::string name, BoneIndex parent, const Transform& bindLocal)
{
    if (parent != kInvalidBone && (parent < 0 || parent >= BoneCount()))
    {
        PRED_LOG_ERROR(Animation, "Bone '{}' names parent {} which does not exist yet", name, parent);
        parent = kInvalidBone;
    }
    const auto index = static_cast<BoneIndex>(m_bones.size());
    m_bones.push_back(Bone{std::move(name), parent, bindLocal});
    return index;
}

BoneIndex Skeleton::Find(std::string_view name) const
{
    for (size_t i = 0; i < m_bones.size(); ++i)
    {
        if (m_bones[i].name == name)
        {
            return static_cast<BoneIndex>(i);
        }
    }
    return kInvalidBone;
}

bool Skeleton::ValidateOrdering() const
{
    for (size_t i = 0; i < m_bones.size(); ++i)
    {
        const BoneIndex parent = m_bones[i].parent;
        if (parent != kInvalidBone && parent >= static_cast<BoneIndex>(i))
        {
            PRED_LOG_ERROR(Animation, "Bone '{}' at {} has parent {} which does not precede it",
                           m_bones[i].name, i, parent);
            return false;
        }
    }
    return true;
}

// -----------------------------------------------------------------------------------------------

void Pose::ResetToBind(const Skeleton& skeleton)
{
    const size_t count = static_cast<size_t>(skeleton.BoneCount());
    m_local.resize(count);
    m_global.assign(count, glm::mat4(1.0f));
    for (size_t i = 0; i < count; ++i)
    {
        m_local[i] = skeleton.GetBone(static_cast<BoneIndex>(i)).bindLocal;
    }
}

void Pose::ComputeGlobals(const Skeleton& skeleton, const glm::mat4& root)
{
    const size_t count = static_cast<size_t>(skeleton.BoneCount());
    if (m_local.size() != count)
    {
        ResetToBind(skeleton);
    }
    m_global.resize(count);

    // Single forward pass: parents always precede their children, so a parent's global transform is
    // already final by the time any child reads it.
    for (size_t i = 0; i < count; ++i)
    {
        const BoneIndex parent = skeleton.GetBone(static_cast<BoneIndex>(i)).parent;
        const glm::mat4 local = m_local[i].Matrix();
        m_global[i] = (parent == kInvalidBone) ? root * local
                                               : m_global[static_cast<size_t>(parent)] * local;
    }
}

void Pose::SetGlobal(const Skeleton& skeleton, BoneIndex index, const glm::mat4& global)
{
    if (!skeleton.IsValid(index) || m_global.empty())
    {
        return;
    }
    m_global[static_cast<size_t>(index)] = global;

    // Rebuild everything after this bone. Children always follow their parent in the array, so a
    // forward sweep from here is enough and costs nothing to reason about.
    for (size_t i = static_cast<size_t>(index) + 1; i < m_global.size(); ++i)
    {
        const BoneIndex parent = skeleton.GetBone(static_cast<BoneIndex>(i)).parent;
        if (parent != kInvalidBone)
        {
            m_global[i] = m_global[static_cast<size_t>(parent)] * m_local[i].Matrix();
        }
    }
}

} // namespace pred
