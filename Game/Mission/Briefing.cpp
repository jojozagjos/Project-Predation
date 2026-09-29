#include "Game/Mission/Briefing.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace pred
{

namespace
{

// "POLAR RESEARCH FACILITY" as "Polar Research Facility", for a subtitle.
std::string TitleCase(const std::string& text)
{
    std::string out = text;
    bool start = true;
    for (char& c : out)
    {
        const auto u = static_cast<unsigned char>(c);
        c = static_cast<char>(start ? std::toupper(u) : std::tolower(u));
        start = !std::isalnum(u);
    }
    return out;
}

void WordsOf(const std::string& text, std::vector<std::string>& clips)
{
    std::istringstream in(text);
    std::string word;
    while (in >> word)
    {
        const std::string clip = WordClip(word);
        if (!clip.empty())
        {
            clips.push_back(clip);
        }
    }
}

void Append(std::vector<std::string>& to, const std::vector<std::string>& more)
{
    to.insert(to.end(), more.begin(), more.end());
}

BriefingSegment SegmentFrom(const nlohmann::json& entry)
{
    BriefingSegment segment;
    segment.clip = entry.value("clip", std::string());
    segment.text = entry.value("text", std::string());
    segment.say = entry.value("say", std::string());
    return segment;
}

} // namespace

std::string WordClip(const std::string& word)
{
    std::string slug;
    for (const char c : word)
    {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u))
        {
            slug += static_cast<char>(std::tolower(u));
        }
        else if ((c == ' ' || c == '_') && !slug.empty() && slug.back() != '_')
        {
            slug += '_';
        }
    }
    return slug.empty() ? std::string() : "Briefing/Words/" + slug;
}

std::vector<std::string> NumberClips(int number)
{
    std::vector<std::string> clips;
    const auto clip = [](int n) { return "Briefing/Numbers/" + std::to_string(n); };
    number = std::abs(number);
    if (number >= 100)
    {
        clips.push_back(clip(std::min(number / 100, 9)));
        clips.push_back("Briefing/Numbers/hundred");
        number %= 100;
        if (number == 0)
        {
            return clips;
        }
    }
    if (number < 20)
    {
        clips.push_back(clip(number));
        return clips;
    }
    clips.push_back(clip(number / 10 * 10));
    if (number % 10 != 0)
    {
        clips.push_back(clip(number % 10));
    }
    return clips;
}

Spoken Expand(const BriefingSegment& segment, const BriefingFacts& facts, const BriefingScript& script)
{
    Spoken spoken;
    const SiteTitle& title = facts.title;
    if (segment.say.empty())
    {
        spoken.text = segment.text;
        if (!segment.clip.empty())
        {
            spoken.clips.push_back(segment.clip);
        }
        return spoken;
    }
    if (segment.say == "planet")
    {
        // "KEPLER-91 IV": the catalogue, its number, and which planet -- the numeral said as a number.
        spoken.text = TitleCase(title.catalogue) + "-" + std::to_string(title.catalogueNumber) + (title.numeral.empty() ? "" : " " + title.numeral);
        WordsOf(title.catalogue, spoken.clips);
        Append(spoken.clips, NumberClips(title.catalogueNumber));
        if (const int which = NumeralValue(title.numeral); which > 0)
        {
            Append(spoken.clips, NumberClips(which));
        }
    }
    else if (segment.say == "site")
    {
        // "POLAR RESEARCH FACILITY 06, NORTH CRYOSPHERE": its number said a figure at a time, as a designation is.
        char number[8];
        std::snprintf(number, sizeof(number), "%02d", title.siteNumber);
        spoken.text = TitleCase((title.qualifier.empty() ? "" : title.qualifier + " ") + title.kind) + " " + number +
                      (title.region.empty() ? "" : ", " + TitleCase(title.region));
        WordsOf(title.qualifier, spoken.clips);
        WordsOf(title.kind, spoken.clips);
        for (const char* c = number; *c != '\0'; ++c)
        {
            spoken.clips.push_back("Briefing/Numbers/" + std::string(1, *c));
        }
        WordsOf(title.region, spoken.clips);
    }
    else if (segment.say == "temperature")
    {
        const int degrees = facts.conditions.temperature;
        spoken.text = (degrees < 0 ? "minus " : "") + std::to_string(std::abs(degrees)) + " degrees";
        if (degrees < 0)
        {
            spoken.clips.push_back(WordClip("minus"));
        }
        Append(spoken.clips, NumberClips(degrees));
        spoken.clips.push_back(WordClip("degrees"));
    }
    else if (segment.say == "wind")
    {
        spoken.text = "wind " + std::to_string(facts.conditions.wind) + " metres per second";
        spoken.clips.push_back(WordClip("wind"));
        Append(spoken.clips, NumberClips(facts.conditions.wind));
        WordsOf("metres per second", spoken.clips);
    }
    else if (segment.say == "visibility")
    {
        std::string visibility = facts.conditions.visibility;
        std::transform(visibility.begin(), visibility.end(), visibility.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        spoken.text = "visibility " + visibility;
        spoken.clips.push_back(WordClip("visibility"));
        spoken.clips.push_back(WordClip(visibility));
    }
    else if (segment.say == "map")
    {
        const BriefingSegment& said = facts.mapGiven ? script.mapGiven : script.mapMissing;
        spoken.text = said.text;
        if (!said.clip.empty())
        {
            spoken.clips.push_back(said.clip);
        }
    }
    return spoken;
}

BriefingScript BriefingScript::Default()
{
    // Plain and functional, to be rewritten and recorded: Assets/Data/briefing.json is the one the game reads.
    BriefingScript script;
    const auto phrase = [](const char* clip, const char* text) { return BriefingSegment{clip, text, ""}; };
    const auto say = [](const char* what) { return BriefingSegment{"", "", what}; };
    script.slides = {
        {"header", 3.0f, {phrase("Briefing/Phrases/begin", "Deployment briefing.")}},
        {"planet", 5.0f, {phrase("Briefing/Phrases/destination", "Destination:"), say("planet")}},
        {"site", 6.0f, {phrase("Briefing/Phrases/site", "Site:"), say("site")}},
        {"conditions", 6.0f, {phrase("Briefing/Phrases/conditions", "Surface conditions:"), say("temperature"), say("wind"), say("visibility")}},
        {"map", 6.0f, {say("map")}},
        {"objective", 8.0f,
         {phrase("Briefing/Phrases/objective", "Objective: locate the terminal, download the data, and bring the drive back to the shuttle.")}},
        {"end", 4.0f, {phrase("Briefing/Phrases/end", "Deploy when ready.")}},
    };
    script.mapGiven = phrase("Briefing/Phrases/map_given", "A map of the site is on file.");
    script.mapMissing = phrase("Briefing/Phrases/map_missing", "There is no map of the site on file.");
    return script;
}

bool BriefingScript::LoadFromFile(const std::filesystem::path& file, std::string* error)
{
    *this = Default();
    std::ifstream in(file);
    if (!in)
    {
        if (error != nullptr)
        {
            *error = "cannot open " + file.string();
        }
        return false;
    }
    nlohmann::json json;
    try
    {
        in >> json;
    }
    catch (const std::exception& problem)
    {
        if (error != nullptr)
        {
            *error = problem.what();
        }
        return false;
    }
    gap = json.value("gap", gap);
    secondsPerWord = json.value("seconds_per_word", secondsPerWord);
    if (json.contains("map_given") && json["map_given"].is_object())
    {
        mapGiven = SegmentFrom(json["map_given"]);
    }
    if (json.contains("map_missing") && json["map_missing"].is_object())
    {
        mapMissing = SegmentFrom(json["map_missing"]);
    }
    if (json.contains("slides") && json["slides"].is_array())
    {
        slides.clear();
        for (const nlohmann::json& entry : json["slides"])
        {
            if (!entry.is_object())
            {
                continue;
            }
            BriefingSlide slide;
            slide.show = entry.value("show", std::string("header"));
            slide.minSeconds = entry.value("min_seconds", slide.minSeconds);
            if (entry.contains("line") && entry["line"].is_array())
            {
                for (const nlohmann::json& segment : entry["line"])
                {
                    if (segment.is_object())
                    {
                        slide.line.push_back(SegmentFrom(segment));
                    }
                }
            }
            slides.push_back(std::move(slide));
        }
    }
    return true;
}

BriefingTimeline BuildBriefing(const BriefingScript& script, const BriefingFacts& facts,
                               const std::function<float(const std::string&)>& clipSeconds)
{
    BriefingTimeline timeline;
    // A recording not made yet is given as long as its words would take: a word clip one word, a phrase as many as it
    // has in its subtitle.
    const auto lengthOf = [&](const std::string& clip, const std::string& text, size_t clipsInSegment)
    {
        const float recorded = clipSeconds ? clipSeconds(clip) : -1.0f;
        if (recorded >= 0.0f)
        {
            return recorded;
        }
        if (clipsInSegment == 1 && !text.empty())
        {
            const auto words = static_cast<float>(std::count(text.begin(), text.end(), ' ') + 1);
            return words * script.secondsPerWord;
        }
        return script.secondsPerWord;
    };
    float at = 0.0f;
    for (const BriefingSlide& slide : script.slides)
    {
        BriefingTimeline::Part part;
        part.from = at;
        part.show = slide.show;
        float voice = at + 0.6f; // the picture first, then the voice
        std::string caption;
        for (const BriefingSegment& segment : slide.line)
        {
            const Spoken spoken = Expand(segment, facts, script);
            if (!spoken.text.empty())
            {
                caption += (caption.empty() ? "" : " ") + spoken.text;
            }
            for (const std::string& clip : spoken.clips)
            {
                timeline.cues.push_back({voice, clip});
                voice += lengthOf(clip, spoken.text, spoken.clips.size()) + script.gap;
            }
        }
        const float spoken = voice - (at + 0.6f);
        part.to = at + std::max(slide.minSeconds, spoken + 1.4f);
        if (!caption.empty())
        {
            timeline.captions.push_back({at + 0.6f, std::max(voice + 0.6f, at + 0.6f + 1.5f), caption});
        }
        timeline.parts.push_back(part);
        at = part.to;
    }
    timeline.length = at;
    return timeline;
}

const BriefingTimeline::Part* BriefingTimeline::PartAt(float time) const
{
    for (const Part& part : parts)
    {
        if (time >= part.from && time < part.to)
        {
            return &part;
        }
    }
    return nullptr;
}

const BriefingTimeline::Caption* BriefingTimeline::CaptionAt(float time) const
{
    for (const Caption& caption : captions)
    {
        if (time >= caption.from && time < caption.to)
        {
            return &caption;
        }
    }
    return nullptr;
}

} // namespace pred
