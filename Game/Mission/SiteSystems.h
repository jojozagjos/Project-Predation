#pragma once

#include "Game/World/FacilityLayout.h"
#include "Game/World/SitePlan.h"

#include <glm/vec2.hpp>

#include <cstddef>
#include <vector>

namespace pred
{

// What a site has in it that a mission can use, laid out so a mission asks for "a terminal" or "a breaker" and is told which
// and where (the objective graph, Phase 4): its terminals and whether each can be reached without the keycard; each building's
// power -- its circuits and its breaker -- and the field generator out in the land; its locked doors and where the keycard is;
// its ways in (a vehicle bay's, an airlock's); what is left to find in it; and its rooms, by what they are for. Read from the
// site's plan, so the same on every machine.
struct SiteSystems
{
    // Somewhere in a building: which, its floor and room (-1: none), where (cells, fractional) and which way it faces.
    struct Spot
    {
        int building = -1;
        int floor = 0;
        int room = -1;
        glm::vec2 at{0.0f};
        float yaw = 0.0f;
    };
    // A bench a terminal can stand on (FacilityLayout::Fitting::Terminal): which of its building's fittings, whether it can be
    // reached without the keycard and is not where a creature would build (open), and what its room is for.
    struct Terminal
    {
        Spot spot;
        size_t fitting = 0;
        bool open = false;
        FacilityLayout::Purpose purpose = FacilityLayout::Purpose::None;
    };
    // A building's power: its lamps' circuits, and its breaker panel when it has one.
    struct Power
    {
        int building = -1;
        std::vector<int> circuits;
        bool breaker = false;
        Spot panel;
    };
    // A door kept locked: which of its building's doorways, and the room behind it.
    struct Lock
    {
        int building = -1;
        size_t door = 0;
        int room = -1;
    };
    // A way in: which of its building's ways out, whether it is a vehicle bay's, and whether it is through an airlock.
    struct Way
    {
        int building = -1;
        size_t exit = 0;
        bool bay = false;
        bool airlock = false;
    };
    // A room: where it is, what it is for, and whether it is kept clean (an airlock inside its door) or two storeys high.
    struct Room
    {
        Spot spot;
        FacilityLayout::Purpose purpose = FacilityLayout::Purpose::None;
        bool clean = false;
        bool tall = false;
    };

    std::vector<Terminal> terminals;
    std::vector<Power> power;
    int generator = -1; // the field generator's block (SitePlan::generator), or -1
    std::vector<Lock> locks;
    std::vector<Spot> keycards;
    std::vector<Way> ways;
    std::vector<Spot> supplies;
    std::vector<Spot> lockers;
    std::vector<Spot> ammo;
    std::vector<Room> rooms;

    static SiteSystems Survey(const SitePlan& site);

    // The terminals a mission may put its business on: open ones, in the order they were found (building by building).
    std::vector<const Terminal*> OpenTerminals() const;
    // A building's power (none: null).
    const Power* PowerOf(int building) const;
    // The rooms for something, all buildings.
    std::vector<const Room*> RoomsFor(FacilityLayout::Purpose purpose) const;
};

} // namespace pred
