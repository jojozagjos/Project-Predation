#pragma once

#include "Game/Interaction/Interactable.h"

#include <glm/vec3.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace pred
{

class Scene;
class PhysicsWorld;
class DebugDraw;

// Decides what the player is currently able to use.
//
// Selection is by aim rather than by proximity: the thing nearest the centre of the screen wins,
// not the thing nearest the player's feet. Standing between a door and a dropped item and having
// the game pick for you is the classic way this feels wrong.
class InteractionSystem
{
public:
    struct Focus
    {
        bool valid = false;
        Entity entity;
        InteractionKind kind = InteractionKind::Generic;
        std::string prompt; // ready to display, e.g. "Open Door"
        int payload = 0;
        float distance = 0.0f;
    };

    void Register(const Interactable& interactable);
    void Unregister(Entity entity);
    void Clear();

    Interactable* Find(Entity entity);
    const Interactable* Find(Entity entity) const;
    // Looks one up by what it is and which one it is, which is how a machine that did not register
    // it refers to it: both ends build the same map, so the pair is a name both agree on.
    const Interactable* FindByPayload(InteractionKind kind, int payload) const;
    void SetEnabled(Entity entity, bool enabled);
    void SetVerb(Entity entity, std::string verb);

    size_t Count() const { return m_interactables.size(); }

    // `maxAngleDegrees` is the half-angle of the cone the player can select within.
    const Focus& UpdateFocus(const Scene& scene, const PhysicsWorld& physics, const glm::vec3& eye,
                             const glm::vec3& forward, float maxAngleDegrees = 28.0f);
    const Focus& CurrentFocus() const { return m_focus; }
    void ClearFocus();

    void DebugDraw(class DebugDraw& draw, const Scene& scene) const;

private:
    glm::vec3 FocusPoint(const Scene& scene, const Interactable& interactable) const;

    std::unordered_map<uint64_t, Interactable> m_interactables; // keyed by Entity::Key()
    Focus m_focus;
};

} // namespace pred
