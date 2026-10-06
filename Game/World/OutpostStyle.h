#pragma once

#include <map>
#include <string>
#include <vector>

namespace pred
{

// How an outpost's owner has it (OwnerDef, from the universe's data), which with its seed is what the outpost is planned from
// (Outpost::Generate): CIRRA's mark and its standard outpost (its paint, walls and fence the same everywhere), or nobody there
// at all; how many of its lamps work; what its buildings are called; and how often each kind of building or yard goes along its
// street ("block", "shed", "habitat", "tanks", "containers", "comms", "open").
struct OutpostStyle
{
    bool mark = false;
    bool standard = false;
    bool abandoned = false;
    float lit = 1.0f;
    std::string operations = "OPERATIONS";
    std::vector<std::string> blocks{"STORAGE", "WORKSHOP", "CREW QUARTERS", "MAINTENANCE", "POWER"};
    std::vector<std::string> sheds{"HANGAR", "VEHICLE BAY", "WORKSHOP"};
    std::vector<std::string> habitats{"HABITAT", "CREW QUARTERS"};
    std::vector<std::string> tanks{"FUEL", "WATER"};
    std::string comms = "COMMS";
    std::map<std::string, float> uses;
    bool operator==(const OutpostStyle& other) const = default;
};

} // namespace pred
