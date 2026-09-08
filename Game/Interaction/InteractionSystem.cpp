#include "Game/Interaction/InteractionSystem.h"

#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/DebugDraw.h"
#include "Engine/Scene/Scene.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

const char* InteractionKindName(InteractionKind kind)
{
    switch (kind)
    {
    case InteractionKind::Door:
        return "door";
    case InteractionKind::Pickup:
        return "pickup";
    case InteractionKind::HidingSpot:
        return "hiding spot";
    case InteractionKind::Generic:
    default:
        return "generic";
    }
}

void InteractionSystem::Register(const Interactable& interactable)
{
    m_interactables[interactable.entity.Key()] = interactable;
}

void InteractionSystem::Unregister(Entity entity)
{
    m_interactables.erase(entity.Key());
    if (m_focus.valid && m_focus.entity == entity)
    {
        ClearFocus();
    }
}

void InteractionSystem::Clear()
{
    m_interactables.clear();
    ClearFocus();
}

Interactable* InteractionSystem::Find(Entity entity)
{
    const auto it = m_interactables.find(entity.Key());
    return it == m_interactables.end() ? nullptr : &it->second;
}

const Interactable* InteractionSystem::Find(Entity entity) const
{
    const auto it = m_interactables.find(entity.Key());
    return it == m_interactables.end() ? nullptr : &it->second;
}

void InteractionSystem::SetEnabled(Entity entity, bool enabled)
{
    if (Interactable* interactable = Find(entity))
    {
        interactable->enabled = enabled;
    }
}

void InteractionSystem::SetVerb(Entity entity, std::string verb)
{
    if (Interactable* interactable = Find(entity))
    {
        interactable->verb = std::move(verb);
    }
}

void InteractionSystem::ClearFocus()
{
    m_focus = Focus{};
}

glm::vec3 InteractionSystem::FocusPoint(const Scene& scene, const Interactable& interactable) const
{
    const Transform* transform = scene.GetTransform(interactable.entity);
    if (transform == nullptr)
    {
        return glm::vec3(0.0f);
    }
    return transform->position + transform->rotation * interactable.focusOffset;
}

const InteractionSystem::Focus& InteractionSystem::UpdateFocus(const Scene& scene,
                                                               const PhysicsWorld& physics,
                                                               const glm::vec3& eye,
                                                               const glm::vec3& forward,
                                                               float maxAngleDegrees)
{
    ClearFocus();

    const float minimumDot = std::cos(glm::radians(std::clamp(maxAngleDegrees, 1.0f, 89.0f)));
    const glm::vec3 aim = glm::normalize(forward);

    float bestDot = minimumDot;
    const Interactable* best = nullptr;
    float bestDistance = 0.0f;

    for (const auto& [key, interactable] : m_interactables)
    {
        if (!interactable.enabled || !scene.IsAlive(interactable.entity))
        {
            continue;
        }

        const glm::vec3 point = FocusPoint(scene, interactable);
        const glm::vec3 toPoint = point - eye;
        const float distance = glm::length(toPoint);
        if (distance > interactable.range || distance < 1e-3f)
        {
            continue;
        }

        const float dot = glm::dot(toPoint / distance, aim);
        if (dot <= bestDot)
        {
            continue;
        }

        // Do not offer something through a wall. The trace stops slightly short so the object's own
        // collision does not count as blocking it.
        const RayHit blocked = physics.RayCast(eye, toPoint / distance, distance - 0.15f);
        if (blocked)
        {
            continue;
        }

        bestDot = dot;
        best = &interactable;
        bestDistance = distance;
    }

    if (best != nullptr)
    {
        m_focus.valid = true;
        m_focus.entity = best->entity;
        m_focus.kind = best->kind;
        m_focus.payload = best->payload;
        m_focus.distance = bestDistance;
        m_focus.prompt = best->name.empty() ? best->verb : best->verb + " " + best->name;
    }
    return m_focus;
}

void InteractionSystem::DebugDraw(class DebugDraw& draw, const Scene& scene) const
{
    for (const auto& [key, interactable] : m_interactables)
    {
        if (!scene.IsAlive(interactable.entity))
        {
            continue;
        }
        const glm::vec3 point = FocusPoint(scene, interactable);
        const bool focused = m_focus.valid && m_focus.entity == interactable.entity;
        const uint32_t color = !interactable.enabled ? Color::kDarkGrey
                               : focused             ? Color::kYellow
                                                     : Color::kCyan;
        draw.Sphere(point, focused ? 0.09f : 0.06f, color, 10);
    }
}

} // namespace pred
