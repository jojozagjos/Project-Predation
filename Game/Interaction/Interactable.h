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
    // The mission's: the terminal the data is on, a building's breaker panel (the payload is which building), and the
    // shuttle's launch console.
    Terminal,
    Breaker,
    Launch,
    // The deployment console aboard the ship: where the next site is chosen.
    Deploy,
    // The gear room's loadout locker: where each person chooses what to take down. Opens a screen on their own machine;
    // what is drawn is told to the host, which keeps count of it.
    Loadout,
    // The ship's helm, in the cockpit: setting out on the course plotted on the map.
    Helm,
    // The ship's boarding door's controls: opening it on the ground, shutting it.
    Airlock,
    // A lift's button: inside its car or the call button at a stop (Lifts::Payload: which lift, which stop, or open).
    Lift,
    // A mission device, to be brought up close and worked by hand (MissionProps::Device: which). Never asked of the host.
    Device,
    // A seat in a shuttle (payload: which, four a shuttle -- the hangar's first, then the site's): sat in, or got up from.
    Seat,
    // A deposit on a site's ground (payload: which): a sample taken out of it (Resources).
    Deposit
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

    // Worked only up close (a mission device's switches, FIX 306): never offered to the crosshair, only to the pointer with
    // the camera brought up to the thing it is on (InteractionSystem::UpdateFocus's closeUp).
    bool closeUp = false;
};

} // namespace pred
