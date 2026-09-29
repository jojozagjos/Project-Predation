#pragma once

#include "Engine/Scene/Entity.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <string>

namespace pred
{

enum class InteractionKind : uint8_t
{
    Generic,
    Door,
    Pickup,
    HidingSpot,
    AmmoCrate,
    // Somebody wrapped up at a creature nest. The payload is who.
    Cocoon,
    // The mission's: the terminal the data is on, a building's breaker panel (the payload is which building), and the
    // shuttle's launch console.
    Terminal,
    Breaker,
    Launch,
    // The deployment console aboard the ship: where the next site is chosen.
    Deploy,
    // The ship's shuttle's controls, in its cabin in the hangar: down to the site, once everybody is aboard.
    Board,
    // The gear room's loadout locker: where each person chooses what to take down. Opens a screen on their own machine;
    // what is drawn is told to the host, which keeps count of it.
    Loadout
};

const char* InteractionKindName(InteractionKind kind);

// Something in the world the player can look at and use.
//
// Interactables are held in the interaction system's own registry rather than as a Scene component,
// because they are a gameplay concept and the engine has no business knowing about doors. If
// several more gameplay component types appear, that is the point to add a general component store
// rather than a registry each.
struct Interactable
{
    Entity entity;
    InteractionKind kind = InteractionKind::Generic;

    // Shown in the prompt, as "<verb> <name>", e.g. "Open Door".
    std::string verb = "Use";
    std::string name;

    // Offset from the entity's origin to the point the player actually aims at. A door's origin is
    // its hinge, which is a poor thing to aim for.
    glm::vec3 focusOffset{0.0f};

    float range = 2.4f;
    bool enabled = true;

    // Meaning depends on the kind: a door index, an item id, a hiding spot index.
    int payload = 0;
};

} // namespace pred
