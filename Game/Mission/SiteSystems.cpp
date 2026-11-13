#include "Game/Mission/SiteSystems.h"

#include <algorithm>
#include <cmath>

namespace pred
{

SiteSystems SiteSystems::Survey(const SitePlan& site)
{
    SiteSystems systems;
    systems.generator = site.generator;
    for (size_t b = 0; b < site.buildings.size(); ++b)
    {
        const FacilityLayout& building = site.buildings[b];
        const int index = static_cast<int>(b);
        const std::vector<bool> reach = building.Reachable(false);
        const auto spotOf = [&](int floor, glm::vec2 at, float yaw)
        {
            Spot spot;
            spot.building = index;
            spot.floor = floor;
            spot.room = building.RoomAt(floor, static_cast<int>(std::floor(at.x)), static_cast<int>(std::floor(at.y)));
            spot.at = at;
            spot.yaw = yaw;
            return spot;
        };

        // Its terminals' benches.
        for (size_t t = 0; t < building.fittings.size(); ++t)
        {
            const FacilityLayout::Fitted& fitted = building.fittings[t];
            if (fitted.kind != FacilityLayout::Fitting::Terminal)
            {
                continue;
            }
            Terminal terminal;
            terminal.spot = spotOf(fitted.floor, fitted.at, fitted.yaw);
            terminal.fitting = t;
            const glm::ivec2 cell{static_cast<int>(std::floor(fitted.at.x)), static_cast<int>(std::floor(fitted.at.y))};
            terminal.open = terminal.spot.room >= 0 && terminal.spot.room != building.lairRoom && reach[building.Index(fitted.floor, cell.x, cell.y)];
            terminal.purpose = terminal.spot.room >= 0 ? building.rooms[static_cast<size_t>(terminal.spot.room)].purpose : FacilityLayout::Purpose::None;
            systems.terminals.push_back(terminal);
        }

        // Its power.
        Power power;
        power.building = index;
        for (const FacilityLayout::Lamp& lamp : building.lamps)
        {
            if (std::find(power.circuits.begin(), power.circuits.end(), lamp.circuit) == power.circuits.end())
            {
                power.circuits.push_back(lamp.circuit);
            }
        }

        // What is placed in it.
        for (const FacilityLayout::Placed& thing : building.things)
        {
            const Spot spot = spotOf(thing.floor, thing.at, thing.yaw);
            switch (thing.thing)
            {
            case FacilityLayout::Thing::Breaker:
                if (!power.breaker)
                {
                    power.breaker = true;
                    power.panel = spot;
                }
                break;
            case FacilityLayout::Thing::Keycard: systems.keycards.push_back(spot); break;
            case FacilityLayout::Thing::Supplies: systems.supplies.push_back(spot); break;
            case FacilityLayout::Thing::Locker: systems.lockers.push_back(spot); break;
            case FacilityLayout::Thing::AmmoCrate: systems.ammo.push_back(spot); break;
            default: break;
            }
        }
        systems.power.push_back(power);

        // Its locks, its ways in, its rooms.
        for (size_t d = 0; d < building.doors.size(); ++d)
        {
            if (building.doors[d].locked)
            {
                systems.locks.push_back({index, d, building.doors[d].room});
            }
        }
        for (size_t e = 0; e < building.exits.size(); ++e)
        {
            systems.ways.push_back({index, e, building.exits[e].bay, building.outside.airlocks});
        }
        for (const FacilityLayout::Room& room : building.rooms)
        {
            Room entry;
            entry.spot = spotOf(room.floor, glm::vec2(room.min + room.max + glm::ivec2(1)) * 0.5f, 0.0f);
            entry.purpose = room.purpose;
            entry.clean = room.clean;
            entry.tall = room.tall;
            systems.rooms.push_back(entry);
        }
    }
    for (const SitePlan::Cave& cave : site.caves)
    {
        Cave entry;
        entry.building = cave.building;
        entry.flooded = cave.water > cave.bottom;
        for (const glm::vec4& chamber : cave.chambers)
        {
            const float floor = chamber.y - chamber.w * 0.45f;
            if (floor > cave.water + 0.3f)
            {
                entry.chambers.push_back({chamber.x, floor, chamber.z});
            }
        }
        systems.caves.push_back(entry);
    }
    return systems;
}

std::vector<const SiteSystems::Terminal*> SiteSystems::OpenTerminals() const
{
    std::vector<const Terminal*> open;
    for (const Terminal& terminal : terminals)
    {
        if (terminal.open)
        {
            open.push_back(&terminal);
        }
    }
    return open;
}

const SiteSystems::Power* SiteSystems::PowerOf(int building) const
{
    for (const Power& entry : power)
    {
        if (entry.building == building)
        {
            return &entry;
        }
    }
    return nullptr;
}

std::vector<const SiteSystems::Room*> SiteSystems::RoomsFor(FacilityLayout::Purpose purpose) const
{
    std::vector<const Room*> found;
    for (const Room& room : rooms)
    {
        if (room.purpose == purpose)
        {
            found.push_back(&room);
        }
    }
    return found;
}

} // namespace pred
