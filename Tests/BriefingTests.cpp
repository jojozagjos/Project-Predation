#include "Game/Mission/Briefing.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace pred;

namespace
{

BriefingFacts Facts()
{
    BriefingFacts facts;
    facts.title.catalogue = "KEPLER";
    facts.title.catalogueNumber = 91;
    facts.title.numeral = "IV";
    facts.title.qualifier = "POLAR";
    facts.title.kind = "RESEARCH FACILITY";
    facts.title.siteNumber = 6;
    facts.title.region = "NORTH CRYOSPHERE";
    facts.conditions.temperature = -23;
    facts.conditions.wind = 12;
    facts.conditions.visibility = "POOR";
    facts.mapGiven = false;
    return facts;
}

} // namespace

TEST_CASE("A briefing says a site's names in recordings of their own words and numbers", "[briefing]")
{
    const BriefingScript script = BriefingScript::Default();
    const BriefingFacts facts = Facts();

    CHECK(NumberClips(91) == std::vector<std::string>{"Briefing/Numbers/90", "Briefing/Numbers/1"});
    CHECK(NumberClips(312) == std::vector<std::string>{"Briefing/Numbers/3", "Briefing/Numbers/hundred", "Briefing/Numbers/12"});
    CHECK(NumberClips(40) == std::vector<std::string>{"Briefing/Numbers/40"});
    CHECK(NumeralValue("IV") == 4);
    CHECK(NumeralValue("IX") == 9);
    CHECK(WordClip("CRYOSPHERE") == "Briefing/Words/cryosphere");

    const Spoken planet = Expand({"", "", "planet"}, facts, script);
    CHECK(planet.text == "Kepler-91 IV");
    CHECK(planet.clips == std::vector<std::string>{"Briefing/Words/kepler", "Briefing/Numbers/90", "Briefing/Numbers/1", "Briefing/Numbers/4"});

    // The site's number a figure at a time, as a designation is said.
    const Spoken site = Expand({"", "", "site"}, facts, script);
    CHECK(site.text == "Polar Research Facility 06, North Cryosphere");
    CHECK(site.clips == std::vector<std::string>{"Briefing/Words/polar", "Briefing/Words/research", "Briefing/Words/facility",
                                                 "Briefing/Numbers/0", "Briefing/Numbers/6", "Briefing/Words/north", "Briefing/Words/cryosphere"});

    const Spoken cold = Expand({"", "", "temperature"}, facts, script);
    CHECK(cold.text == "minus 23 degrees");
    CHECK(cold.clips.front() == "Briefing/Words/minus");
    CHECK(Expand({"", "", "map"}, facts, script).clips == std::vector<std::string>{script.mapMissing.clip});
}

TEST_CASE("A briefing is laid out as long as what is said takes, recorded yet or not", "[briefing]")
{
    const BriefingScript script = BriefingScript::Default();
    const BriefingFacts facts = Facts();

    // Nothing recorded: every slide still has its time, its subtitle and its cues, one after another.
    const BriefingTimeline silent = BuildBriefing(script, facts, [](const std::string&) { return -1.0f; });
    REQUIRE(silent.parts.size() == script.slides.size());
    for (size_t i = 0; i < silent.parts.size(); ++i)
    {
        CHECK(silent.parts[i].to - silent.parts[i].from >= script.slides[i].minSeconds - 1e-4f);
        if (i > 0)
        {
            CHECK(silent.parts[i].from == Catch::Approx(silent.parts[i - 1].to));
        }
    }
    CHECK(silent.length == Catch::Approx(silent.parts.back().to));
    for (size_t i = 1; i < silent.cues.size(); ++i)
    {
        CHECK(silent.cues[i].at > silent.cues[i - 1].at);
    }
    REQUIRE(silent.PartAt(0.1f) != nullptr);
    CHECK(silent.PartAt(0.1f)->show == "header");
    CHECK(silent.CaptionAt(silent.captions[1].from + 0.1f)->text.find("Kepler-91 IV") != std::string::npos);

    // Long recordings stretch the slide they are in, and only that one.
    const BriefingTimeline slow = BuildBriefing(script, facts, [](const std::string& clip) { return clip == "Briefing/Words/kepler" ? 9.0f : -1.0f; });
    CHECK(slow.parts[1].to - slow.parts[1].from > 9.0f);
    CHECK(slow.parts[0].to - slow.parts[0].from == Catch::Approx(silent.parts[0].to - silent.parts[0].from));
}
