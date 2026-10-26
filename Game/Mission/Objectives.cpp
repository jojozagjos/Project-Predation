#include "Game/Mission/Objectives.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace pred
{

namespace Objectives
{

namespace
{

bool KindOf(const std::string& name, Kind& kind)
{
    if (name == "restore_power")
    {
        kind = Kind::RestorePower;
        return true;
    }
    if (name == "download")
    {
        kind = Kind::Download;
        return true;
    }
    if (name == "carry")
    {
        kind = Kind::Carry;
        return true;
    }
    return false;
}

std::string Text(const nlohmann::json& entry, const char* key)
{
    return entry.contains(key) && entry[key].is_string() ? entry[key].get<std::string>() : std::string();
}

} // namespace

bool Library::LoadFromString(const std::string& text, std::string* error)
{
    const nlohmann::json json = nlohmann::json::parse(text, nullptr, false, true);
    const auto fail = [&](const std::string& why)
    {
        if (error != nullptr)
        {
            *error = why;
        }
        return false;
    };
    if (json.is_discarded() || !json.is_object() || !json.contains("templates") || !json["templates"].is_array())
    {
        return fail("no \"templates\" list");
    }
    std::vector<Template> read;
    for (const nlohmann::json& entry : json["templates"])
    {
        Template made;
        made.id = Text(entry, "id");
        made.name = Text(entry, "name");
        if (made.id.empty() || !entry.contains("steps") || !entry["steps"].is_array())
        {
            return fail("a template without an id or steps");
        }
        for (const nlohmann::json& stepEntry : entry["steps"])
        {
            StepDef step;
            step.id = Text(stepEntry, "id");
            if (step.id.empty() || !KindOf(Text(stepEntry, "kind"), step.kind))
            {
                return fail("template \"" + made.id + "\": a step without an id or a kind it knows");
            }
            if (stepEntry.contains("after") && stepEntry["after"].is_array())
            {
                for (const nlohmann::json& before : stepEntry["after"])
                {
                    if (before.is_string())
                    {
                        step.after.push_back(before.get<std::string>());
                    }
                }
            }
            step.text = Text(stepEntry, "text");
            step.textOutside = Text(stepEntry, "text_outside");
            step.hint = Text(stepEntry, "hint");
            step.hintNoMap = Text(stepEntry, "hint_no_map");
            step.onlyWhenPowerOut = stepEntry.value("only_when_power_out", false);
            step.learntAtTerminal = stepEntry.value("learnt_at_terminal", false);
            made.steps.push_back(std::move(step));
        }
        read.push_back(std::move(made));
    }
    templates = std::move(read);
    return true;
}

bool Library::LoadFromFile(const std::filesystem::path& file, std::string* error)
{
    std::ifstream stream(file);
    if (!stream.is_open())
    {
        if (error != nullptr)
        {
            *error = "not found: " + file.string();
        }
        return false;
    }
    std::stringstream text;
    text << stream.rdbuf();
    return LoadFromString(text.str(), error);
}

const Template* Library::Find(const std::string& id) const
{
    for (const Template& each : templates)
    {
        if (each.id == id)
        {
            return &each;
        }
    }
    return nullptr;
}

std::vector<int> Graph::Open() const
{
    std::vector<int> open;
    for (size_t i = 0; i < steps.size(); ++i)
    {
        const Step& step = steps[i];
        if (step.done || !step.known)
        {
            continue;
        }
        // (One the crew does not know of yet does not hold up what comes after it: they go on as they know it.)
        const bool ready = std::all_of(step.after.begin(), step.after.end(), [&](int before)
                                       { return steps[static_cast<size_t>(before)].done || !steps[static_cast<size_t>(before)].known; });
        if (ready)
        {
            open.push_back(static_cast<int>(i));
        }
    }
    return open;
}

bool Graph::Complete() const
{
    return std::all_of(steps.begin(), steps.end(), [](const Step& step) { return step.done; });
}

Graph Lay(const Template& source, const MissionPlan& plan)
{
    Graph graph;
    graph.source = &source;
    std::vector<int> laidAs(source.steps.size(), -1);
    for (size_t i = 0; i < source.steps.size(); ++i)
    {
        const StepDef& def = source.steps[i];
        if (def.onlyWhenPowerOut && !plan.powerOut)
        {
            continue;
        }
        laidAs[i] = static_cast<int>(graph.steps.size());
        Step step;
        step.def = &def;
        graph.steps.push_back(step);
    }
    // What each waits on, by the index it was laid at: those not laid for this mission left out.
    for (size_t i = 0; i < source.steps.size(); ++i)
    {
        if (laidAs[i] < 0)
        {
            continue;
        }
        for (const std::string& before : source.steps[i].after)
        {
            for (size_t j = 0; j < source.steps.size(); ++j)
            {
                if (source.steps[j].id == before && laidAs[j] >= 0)
                {
                    graph.steps[static_cast<size_t>(laidAs[i])].after.push_back(laidAs[j]);
                }
            }
        }
    }
    return graph;
}

void Update(Graph& graph, const MissionState& state, bool foundNoPower)
{
    using Stage = MissionState::Stage;
    for (Step& step : graph.steps)
    {
        switch (step.def->kind)
        {
        case Kind::RestorePower: step.done = state.powered; break;
        case Kind::Download: step.done = state.stage == Stage::Carry || state.stage == Stage::Over; break;
        case Kind::Carry: step.done = state.stage == Stage::Over && state.recovered; break;
        }
        // Not known until learnt -- but once done, done whether the crew knew of it or not.
        step.known = !step.def->learntAtTerminal || foundNoPower;
    }
}

std::string TextOf(const Step& step, const MissionPlan& plan)
{
    if (step.def->kind == Kind::RestorePower && plan.breakerOutside && !step.def->textOutside.empty())
    {
        return step.def->textOutside;
    }
    return step.def->text;
}

std::string HintOf(const Step& step, const MissionPlan& plan)
{
    if (!plan.mapGiven && !step.def->hintNoMap.empty())
    {
        return step.def->hintNoMap;
    }
    return step.def->hint;
}

} // namespace Objectives

} // namespace pred
