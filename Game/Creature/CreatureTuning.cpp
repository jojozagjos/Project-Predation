#include "Game/Creature/CreatureTuning.h"

#include "Engine/Core/JsonText.h"
#include "Engine/Core/Log.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace pred
{
namespace
{

CreatureTuning g_tuning;

} // namespace

const CreatureTuning& Tuning()
{
    return g_tuning;
}

void SetTuning(const CreatureTuning& tuning)
{
    g_tuning = tuning;
}

bool LoadCreatureTuning(const std::filesystem::path& file, CreatureTuning& out, std::vector<std::string>* warnings)
{
    std::ifstream stream(file);
    if (!stream.is_open())
    {
        return false;
    }
    nlohmann::json json;
    try
    {
        stream >> json;
    }
    catch (const std::exception& error)
    {
        PRED_LOG_ERROR(AI, "{} is not valid JSON: {}", file.string(), error.what());
        return false;
    }
    for (const std::string& key :
         UnknownKeys(json, {"sight_range", "half_field_degrees", "edge_of_view", "close_sense", "exposure_gain",
                            "exposure_decay", "suspicion", "through_walls", "commitment", "stalk_patience",
                            "stalk_patience_range", "ambush_patience", "ambush_patience_range", "lure_every"}))
    {
        PRED_LOG_WARN(AI, "creatures.json: a key nothing reads, '{}' -- a typo?", key);
        if (warnings != nullptr)
        {
            warnings->push_back(key);
        }
    }
    out.sightRange = json.value("sight_range", out.sightRange);
    out.halfFieldDegrees = json.value("half_field_degrees", out.halfFieldDegrees);
    out.edgeOfView = json.value("edge_of_view", out.edgeOfView);
    out.closeSense = json.value("close_sense", out.closeSense);
    out.exposureGain = json.value("exposure_gain", out.exposureGain);
    out.exposureDecay = json.value("exposure_decay", out.exposureDecay);
    out.suspicion = json.value("suspicion", out.suspicion);
    out.throughWalls = json.value("through_walls", out.throughWalls);
    out.commitment = json.value("commitment", out.commitment);
    out.stalkPatience = json.value("stalk_patience", out.stalkPatience);
    out.stalkPatienceRange = json.value("stalk_patience_range", out.stalkPatienceRange);
    out.ambushPatience = json.value("ambush_patience", out.ambushPatience);
    out.ambushPatienceRange = json.value("ambush_patience_range", out.ambushPatienceRange);
    out.lureEvery = json.value("lure_every", out.lureEvery);
    return true;
}

} // namespace pred
