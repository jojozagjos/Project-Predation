#pragma once

#include "Game/Mission/SiteNames.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace pred
{

// A deployment's briefing, as the briefing room's screen plays it: slides, and a voice-over over them put together out
// of recordings -- a phrase here, then the site's own words and numbers -- so what is said is always what is shown,
// whatever the site. Never synthesised speech: every clip is a recording (a sound folder, as every sound is), and a clip
// not yet recorded is simply not heard, its subtitle and its time on screen kept all the same.
//
// The recordings it asks for:
//   phrases   what the script names (Assets/Data/briefing.json): Briefing/Phrases/<name>
//   words     every word of the site's names and conditions, lower case, spaces as underscores: Briefing/Words/<word>
//   numbers   0 to 19, the tens (20, 30 ... 90), and "hundred": Briefing/Numbers/<n>

// What is known about a site before anybody has been there: what it is called, its weather, and whether there is a map.
struct BriefingFacts
{
    SiteTitle title;
    SiteConditions conditions;
    bool mapGiven = true;
};

struct BriefingSegment
{
    std::string clip; // a phrase: the recording's name
    std::string text; // and what it says
    // Or something of the site's, said in its own words: "planet", "site", "temperature", "wind", "visibility", "map".
    std::string say;
};

struct BriefingSlide
{
    std::string show; // what the screen shows while it plays: "header", "planet", "site", "conditions", "map", "objective", "end"
    float minSeconds = 3.0f;
    std::vector<BriefingSegment> line;
};

class BriefingScript
{
public:
    // The script from the file; without one, the built-in one, which is the same.
    bool LoadFromFile(const std::filesystem::path& file, std::string* error = nullptr);
    static BriefingScript Default();

    std::vector<BriefingSlide> slides;
    // Said for whether the site's map came with the order.
    BriefingSegment mapGiven;
    BriefingSegment mapMissing;
    float gap = 0.15f;            // between one recording and the next
    float secondsPerWord = 0.36f; // a recording not yet made is given this long a word
};

// Something said: its subtitle, and the recordings that say it, in order.
struct Spoken
{
    std::string text;
    std::vector<std::string> clips;
};
Spoken Expand(const BriefingSegment& segment, const BriefingFacts& facts, const BriefingScript& script);
// A number as recordings: "ninety", "one" for 91; "three", "hundred", "twelve" for 312.
std::vector<std::string> NumberClips(int number);
// A word's recording: "Briefing/Words/north" for "NORTH".
std::string WordClip(const std::string& word);

// The briefing laid out in time: when each recording starts, what the screen shows when, and the subtitles.
struct BriefingTimeline
{
    struct Cue
    {
        float at = 0.0f;
        std::string clip;
    };
    struct Caption
    {
        float from = 0.0f;
        float to = 0.0f;
        std::string text;
    };
    struct Part
    {
        float from = 0.0f;
        float to = 0.0f;
        std::string show;
    };
    std::vector<Cue> cues;
    std::vector<Caption> captions;
    std::vector<Part> parts;
    float length = 0.0f;

    const Part* PartAt(float time) const;
    const Caption* CaptionAt(float time) const;
};
// `clipSeconds` says how long a recording is, or less than nought when there is none yet.
BriefingTimeline BuildBriefing(const BriefingScript& script, const BriefingFacts& facts,
                               const std::function<float(const std::string&)>& clipSeconds);

} // namespace pred
