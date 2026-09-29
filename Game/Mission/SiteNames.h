#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace pred
{

// What a site is called on its title card: the planet, as a survey catalogue lists it ("KEPLER-91 IV"), and the site,
// as the Company designates it ("POLAR RESEARCH FACILITY 06, NORTH CRYOSPHERE"). Made from the site's seed out of the
// word lists in Assets/Data/sites.json, so every machine calls a site the same thing, and the same site the same thing
// every time.
struct SiteTitle
{
    std::string planet;
    std::string site;
    // And the parts they are made of, for saying them word by word.
    std::string catalogue;
    int catalogueNumber = 0;
    std::string numeral; // which planet of its star, as a Roman numeral; empty for none
    std::string qualifier;
    std::string kind;
    int siteNumber = 0;
    std::string region;
};

// A Roman numeral's value (0 for something that is not one).
int NumeralValue(const std::string& numeral);

// A site's weather, from its seed and how far can be seen there: the same on every machine.
struct SiteConditions
{
    int temperature = -20; // degrees Celsius
    int wind = 8;          // metres a second
    std::string visibility = "FAIR"; // POOR, LOW or FAIR
};
SiteConditions ConditionsFor(uint32_t seed, float fogEnd);

class SiteNames
{
public:
    // The lists from the file; whatever it leaves out keeps what is here, the examples agreed so far.
    bool LoadFromFile(const std::filesystem::path& file, std::string* error = nullptr);
    SiteTitle For(uint32_t seed) const;

    std::vector<std::string> catalogues{"KEPLER"};
    int catalogueLow = 10;
    int catalogueHigh = 999;
    std::vector<std::string> planets{"I", "II", "III", "IV", "V", "VI"};
    std::vector<std::string> qualifiers{"POLAR"};
    std::vector<std::string> kinds{"RESEARCH FACILITY"};
    int siteLow = 1;
    int siteHigh = 24;
    std::vector<std::string> regions{"NORTH CRYOSPHERE"};
};

// What the ship's intercom says, and when: lines by the moment they are for, each a recording (a sound folder, as every
// sound is) and the subtitle shown while it plays. Read from Assets/Data/intercom.json. The lines are to be written and
// recorded; with none for a moment, nothing is said.
struct IntercomLine
{
    std::string sound;    // e.g. "Intercom/arrival_01": Assets/Audio/Intercom/arrival_01/
    std::string subtitle;
    float seconds = 0.0f; // how long the subtitle stays; 0 works it out from its length
};

class IntercomLines
{
public:
    // The moments a line can be for.
    static const std::vector<std::string>& Moments();

    bool LoadFromFile(const std::filesystem::path& file, std::string* error = nullptr);
    // One of the lines for a moment, the same one on every machine for the same seed; null when there are none.
    const IntercomLine* Pick(const std::string& moment, uint32_t seed) const;
    size_t Count() const;
    // How long a line's subtitle stays up.
    static float SecondsFor(const IntercomLine& line);

private:
    std::map<std::string, std::vector<IntercomLine>> m_lines;
};

} // namespace pred
