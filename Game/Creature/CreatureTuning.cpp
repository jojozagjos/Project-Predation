#include "Game/Creature/CreatureTuning.h"

#include "Engine/Core/JsonText.h"
#include "Engine/Core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace pred
{
namespace
{

CreatureTuning g_tuning;

} // namespace

const std::array<const char*, 12>& QuirkKeys()
{
    static const std::array<const char*, 12> keys{"ceiling_dweller", "knocker", "shrieker", "watcher", "hit_and_run", "light_chaser", "light_shy", "faker", "pacer", "clicker", "silent", "baiter"};
    return keys;
}

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
                            "stalk_patience_range", "ambush_patience", "ambush_patience_range", "lure_every", "director", "quirks"}))
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
    if (json.contains("director") && json["director"].is_object())
    {
        const nlohmann::json& d = json["director"];
        for (const std::string& key :
             UnknownKeys(d, {"quiet_seconds", "nudge_every", "nudge_every_range", "nudge_distance", "close_range", "build_close",
                             "build_busy", "drain", "per_blow", "after_easing", "withdraw_seconds", "withdraw_seconds_range"}))
        {
            PRED_LOG_WARN(AI, "creatures.json: a director key nothing reads, '{}' -- a typo?", key);
            if (warnings != nullptr)
            {
                warnings->push_back("director." + key);
            }
        }
        CreatureTuning::Director& o = out.director;
        o.quietSeconds = d.value("quiet_seconds", o.quietSeconds);
        o.nudgeEvery = d.value("nudge_every", o.nudgeEvery);
        o.nudgeEveryRange = d.value("nudge_every_range", o.nudgeEveryRange);
        o.nudgeDistance = d.value("nudge_distance", o.nudgeDistance);
        o.closeRange = d.value("close_range", o.closeRange);
        o.buildClose = d.value("build_close", o.buildClose);
        o.buildBusy = d.value("build_busy", o.buildBusy);
        o.drain = d.value("drain", o.drain);
        o.perBlow = d.value("per_blow", o.perBlow);
        o.afterEasing = d.value("after_easing", o.afterEasing);
        o.withdrawSeconds = d.value("withdraw_seconds", o.withdrawSeconds);
        o.withdrawSecondsRange = d.value("withdraw_seconds_range", o.withdrawSecondsRange);
    }
    if (json.contains("quirks") && json["quirks"].is_object())
    {
        const nlohmann::json& q = json["quirks"];
        out.quirksPerCreature = std::clamp(q.value("per_creature", out.quirksPerCreature), 0, 4);
        if (q.contains("weights") && q["weights"].is_object())
        {
            const nlohmann::json& w = q["weights"];
            for (const auto& [key, value] : w.items())
            {
                (void)value;
                if (key.rfind("_", 0) == 0 ||
                    std::any_of(QuirkKeys().begin(), QuirkKeys().end(), [&](const char* known) { return key == known; }))
                {
                    continue;
                }
                PRED_LOG_WARN(AI, "creatures.json: a habit nothing has, '{}' -- a typo?", key);
                if (warnings != nullptr)
                {
                    warnings->push_back("quirks.weights." + key);
                }
            }
            for (size_t i = 0; i < QuirkKeys().size(); ++i)
            {
                out.quirkWeights[i] = std::max(w.value(QuirkKeys()[i], out.quirkWeights[i]), 0.0f);
            }
        }
    }
    return true;
}

} // namespace pred
