#include "Engine/Scene/Scene.h"

#include "Engine/Core/Log.h"

namespace pred
{
namespace
{
const std::string kEmptyName;
}

Entity Scene::Create(std::string name)
{
    uint32_t index;
    if (!m_freeList.empty())
    {
        index = m_freeList.back();
        m_freeList.pop_back();
    }
    else
    {
        index = static_cast<uint32_t>(m_slots.size());
        m_slots.emplace_back();
        m_transforms.emplace_back();
        m_meshRenderers.emplace_back();
        m_names.emplace_back();
    }

    Slot& slot = m_slots[index];
    // Generation 0 is reserved for "invalid", so a recycled slot never lands back on it.
    ++slot.generation;
    if (slot.generation == 0)
    {
        slot.generation = 1;
    }
    slot.alive = true;

    m_transforms[index] = Transform{};
    m_meshRenderers[index].reset();
    m_names[index] = std::move(name);
    ++m_aliveCount;

    return Entity{index, slot.generation};
}

void Scene::Destroy(Entity entity)
{
    uint32_t index;
    if (!ResolveIndex(entity, index))
    {
        return;
    }
    m_slots[index].alive = false;
    m_meshRenderers[index].reset();
    m_names[index].clear();
    m_freeList.push_back(index);
    --m_aliveCount;
}

void Scene::Clear()
{
    m_slots.clear();
    m_freeList.clear();
    m_transforms.clear();
    m_meshRenderers.clear();
    m_names.clear();
    m_aliveCount = 0;
}

bool Scene::ResolveIndex(Entity entity, uint32_t& outIndex) const
{
    if (!entity.IsValid() || entity.index >= m_slots.size())
    {
        return false;
    }
    const Slot& slot = m_slots[entity.index];
    if (!slot.alive || slot.generation != entity.generation)
    {
        return false;
    }
    outIndex = entity.index;
    return true;
}

bool Scene::IsAlive(Entity entity) const
{
    uint32_t index;
    return ResolveIndex(entity, index);
}

const std::string& Scene::Name(Entity entity) const
{
    uint32_t index;
    return ResolveIndex(entity, index) ? m_names[index] : kEmptyName;
}

void Scene::SetName(Entity entity, std::string name)
{
    uint32_t index;
    if (ResolveIndex(entity, index))
    {
        m_names[index] = std::move(name);
    }
}

Transform* Scene::GetTransform(Entity entity)
{
    uint32_t index;
    return ResolveIndex(entity, index) ? &m_transforms[index] : nullptr;
}

const Transform* Scene::GetTransform(Entity entity) const
{
    uint32_t index;
    return ResolveIndex(entity, index) ? &m_transforms[index] : nullptr;
}

void Scene::SetMeshRenderer(Entity entity, const MeshRenderer& renderer)
{
    uint32_t index;
    if (!ResolveIndex(entity, index))
    {
        PRED_LOG_WARN(Gameplay, "SetMeshRenderer on a dead or invalid entity");
        return;
    }
    m_meshRenderers[index] = renderer;
}

MeshRenderer* Scene::GetMeshRenderer(Entity entity)
{
    uint32_t index;
    if (!ResolveIndex(entity, index) || !m_meshRenderers[index].has_value())
    {
        return nullptr;
    }
    return &(*m_meshRenderers[index]);
}

const MeshRenderer* Scene::GetMeshRenderer(Entity entity) const
{
    uint32_t index;
    if (!ResolveIndex(entity, index) || !m_meshRenderers[index].has_value())
    {
        return nullptr;
    }
    return &(*m_meshRenderers[index]);
}

void Scene::RemoveMeshRenderer(Entity entity)
{
    uint32_t index;
    if (ResolveIndex(entity, index))
    {
        m_meshRenderers[index].reset();
    }
}

Entity Scene::CreateMeshEntity(std::string name, const Transform& transform, MeshHandle mesh,
                               const Material& material)
{
    const Entity entity = Create(std::move(name));
    *GetTransform(entity) = transform;
    MeshRenderer renderer;
    renderer.mesh = mesh;
    renderer.material = material;
    SetMeshRenderer(entity, renderer);
    return entity;
}

} // namespace pred
