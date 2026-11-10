#include "Game/Campaign/Difficulty.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace pred::Difficulty
{

Read Rate(const Inputs& inputs)
{
    // Each thing adds to the score, and says why; the rating is the score rounded, one to five.
    std::vector<std::pair<float, std::string>> parts;
    switch (inputs.kind)
    {
    case SitePlan::SiteKind::Survey: parts.push_back({1.0f, "Survey: readings out in the open"}); break;
    case SitePlan::SiteKind::Signal: parts.push_back({1.4f, "Signal: a transmitter to find and shut down"}); break;
    case SitePlan::SiteKind::Station: parts.push_back({2.2f, "Station: power and life support to restore inside"}); break;
    case SitePlan::SiteKind::Wreck: parts.push_back({2.0f, "Wreck: unstable ground and a hull to search"}); break;
    case SitePlan::SiteKind::Facility: parts.push_back({1.8f, "Facility: data to recover from inside"}); break;
    }
    if (!inputs.charted)
    {
        parts.push_back({0.7f, "Uncharted: little known of the place"});
    }
    if (!inputs.threatKnown)
    {
        parts.push_back({0.6f, "Threat unknown: the sensors cannot read it"});
    }
    else if (inputs.signs >= 0.65f)
    {
        parts.push_back({1.6f, "Threat high: plain signs of the organism"});
    }
    else if (inputs.signs >= 0.35f)
    {
        parts.push_back({0.8f, "Threat moderate"});
    }
    if (inputs.weather >= 0.7f)
    {
        parts.push_back({0.9f, "Severe weather"});
    }
    else if (inputs.weather >= 0.35f)
    {
        parts.push_back({0.4f, "Rough weather"});
    }
    if (!inputs.air)
    {
        parts.push_back({0.5f, "No air: suits sealed throughout"});
    }
    if (inputs.temperature <= -60.0f || inputs.temperature >= 60.0f)
    {
        parts.push_back({0.4f, inputs.temperature < 0.0f ? "Extreme cold" : "Extreme heat"});
    }
    if (inputs.night)
    {
        parts.push_back({0.4f, "Night there now"});
    }

    float score = 0.0f;
    for (const auto& part : parts)
    {
        score += part.first;
    }
    Read read;
    read.rating = std::clamp(static_cast<int>(std::lround(score)), 1, 5);
    static const char* const kWords[] = {"EASY", "MODERATE", "HARD", "SEVERE", "EXTREME"};
    read.word = kWords[read.rating - 1];
    // Worst first, the kind of work last (it is always there).
    std::stable_sort(parts.begin() + 1, parts.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = 1; i < parts.size(); ++i)
    {
        read.why.push_back(parts[i].second);
    }
    read.why.push_back(parts.front().second);
    return read;
}

std::string Line(const Read& read)
{
    return read.word + " (" + std::to_string(read.rating) + "/5)";
}

} // namespace pred::Difficulty
