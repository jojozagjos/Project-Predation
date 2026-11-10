#include "Game/Campaign/Campaign.h"
#include "Game/Campaign/Contracts.h"
#include "Game/Campaign/Difficulty.h"
#include "Game/Campaign/ShipTerminal.h"
#include "Game/Campaign/Universe.h"
#include "Engine/Net/BitStream.h"
#include "Game/Net/Protocol.h"

#include <catch2/catch_test_macros.hpp>


#include <filesystem>
#include <set>

using namespace pred;

// Phase 4: contracts offered at the ship's terminal, taken on and paid; the debrief; the terminal's screen; and the orbit scan's
// categories and reads.

namespace
{

const UniverseData& Data()
{
    static UniverseData data = []
    {
        UniverseData loaded;
        std::string error;
        const bool ok = loaded.LoadFromFile(std::filesystem::path(PRED_SOURCE_DIR) / "Assets" / "Data" / "universe.json", &error);
        INFO(error);
        REQUIRE(ok);
        return loaded;
    }();
    return data;
}

Contracts::Rules ShippedRules()
{
    Contracts::Rules rules;
    std::string error;
    const bool ok = rules.LoadFromFile(std::filesystem::path(PRED_SOURCE_DIR) / "Assets" / "Data" / "contracts.json", &error);
    INFO(error);
    REQUIRE(ok);
    return rules;
}

ShipTerminal::Words TestWords()
{
    ShipTerminal::Words words;
    words.area = [](uint64_t, int body, int region) { return "AREA " + std::to_string(body) + "-" + std::to_string(region); };
    words.world = [](uint64_t, int body) { return "WORLD " + std::to_string(body); };
    words.wanted = [](const std::string& kind) { return "Do the " + kind + "."; };
    words.read = [](uint64_t, int, int) { return std::string("Open ground."); };
    words.result = [](const std::string&, bool success) { return std::string(success ? "COMPLETE" : "FAILED"); };
    words.player = [](int id) { return "Crew " + std::to_string(id + 1); };
    words.plotted = [](uint64_t, int, int) { return false; };
    return words;
}

} // namespace

TEST_CASE("The set contracts are areas of the home system, each with a mission; every kind has words and a reward", "[contracts]")
{
    const Contracts::Rules rules = ShippedRules();
    REQUIRE(rules.set.size() >= 3);
    Universe universe;
    universe.Reset(7, &Data());
    const StarSystem* home = universe.System(universe.Home());
    REQUIRE(home != nullptr);
    // (The home system is the same in every campaign, so the set ones are too.)
    Universe other;
    other.Reset(1234, &Data());
    const StarSystem* otherHome = other.System(other.Home());
    REQUIRE(otherHome != nullptr);
    for (const Contracts::Rules::Set& set : rules.set)
    {
        INFO(set.area);
        int found = 0;
        for (const StarSystem* system : {home, otherHome})
        {
            for (const Body& body : system->bodies)
            {
                for (const LandingRegion& region : body.regions)
                {
                    if (region.designation == set.area)
                    {
                        ++found;
                        // Somewhere with work: not an outpost, not a wreck.
                        CHECK(region.kind != "hub");
                        CHECK(region.kind != "wreck");
                        CHECK(body.Landable());
                    }
                }
            }
        }
        CHECK(found == 2);
        CHECK(set.reward > 0);
    }
    for (const char* kind : {"download_data", "restore", "survey", "signal"})
    {
        INFO(kind);
        CHECK_FALSE(rules.Wanted(kind).empty());
        CHECK(rules.Reward(kind, 0.0f) > 0);
        // Further out pays more, to the nearest ten.
        CHECK(rules.Reward(kind, 20.0f) > rules.Reward(kind, 0.0f));
        CHECK(rules.Reward(kind, 13.0f) % 10 == 0);
        const std::string note = rules.LeadNote(kind, "SURVEY SITE 02", "LV-789");
        CHECK(note.find("SURVEY SITE 02") != std::string::npos);
        CHECK(note.find("{place}") == std::string::npos);
    }
    CHECK(rules.maxOffered >= 1);
    CHECK(rules.offerEvery[0] <= rules.offerEvery[1]);
}

TEST_CASE("A contract is offered, taken on, and paid once its area's mission is done; the set ones move on", "[contracts]")
{
    CampaignState campaign;
    campaign.credits = 100;
    campaign.clock = 50.0;
    const CampaignState::Contract& first = Contracts::Offer(campaign, 5, 2, 1, "survey", 250, 0);
    const uint32_t id = first.id;
    CHECK(Contracts::Offered(campaign) == 1);
    CHECK(Contracts::OpenAt(campaign, 5, 2, 1) != nullptr);
    CHECK(Contracts::OpenAt(campaign, 5, 2, 0) == nullptr);
    // Taken on once; not twice.
    CHECK(Contracts::Accept(campaign, id));
    CHECK_FALSE(Contracts::Accept(campaign, id));
    CHECK_FALSE(Contracts::Accept(campaign, 999));
    CHECK(Contracts::Offered(campaign) == 0);
    // Coming back from somewhere else pays nothing.
    CHECK(Contracts::Complete(campaign, 5, 2, 0) == nullptr);
    CHECK(campaign.credits == 100);
    // From its area, done: paid, and the set ones on to the next.
    campaign.clock = 90.0;
    const CampaignState::Contract* done = Contracts::Complete(campaign, 5, 2, 1);
    REQUIRE(done != nullptr);
    CHECK(done->status == CampaignState::Contract::Status::Done);
    CHECK(done->done == 90.0);
    CHECK(campaign.credits == 350);
    CHECK(campaign.setContractsDone == 1);
    // Not paid twice.
    CHECK(Contracts::Complete(campaign, 5, 2, 1) == nullptr);
    CHECK(campaign.credits == 350);
    // One only offered is paid too, the work being done; one taken on comes before one only offered at the same area.
    Contracts::Offer(campaign, 5, 3, 0, "signal", 500, -1);
    const uint32_t taken = Contracts::Offer(campaign, 5, 3, 0, "signal", 600, -1).id;
    REQUIRE(Contracts::Accept(campaign, taken));
    CHECK(Contracts::OpenAt(campaign, 5, 3, 0)->id == taken);
    CHECK(Contracts::Complete(campaign, 5, 3, 0)->reward == 600);
    CHECK(campaign.setContractsDone == 1);
}

TEST_CASE("Contracts and the debrief are saved and read back as they were", "[contracts][campaign]")
{
    CampaignState campaign;
    Contracts::Offer(campaign, 77, 4, 2, "download_data", 640, 1);
    REQUIRE(Contracts::Accept(campaign, campaign.contracts.front().id));
    Contracts::Offer(campaign, 78, 0, 0, "restore", 820, -1);
    campaign.setContractsDone = 1;
    campaign.nextOfferAt = 1234.5;
    campaign.debrief.any = true;
    campaign.debrief.system = 77;
    campaign.debrief.body = 4;
    campaign.debrief.region = 2;
    campaign.debrief.kind = "survey";
    campaign.debrief.success = true;
    campaign.debrief.crew = 0b1011;
    campaign.debrief.aboard = 0b0011;
    campaign.debrief.paid = 250;
    campaign.debrief.contract = 1;
    campaign.debrief.entries = {3, 4, 9};
    campaign.debrief.time = 600.0;

    CampaignState read;
    std::string error;
    REQUIRE(CampaignState::FromJson(campaign.ToJson(), read, &error));
    REQUIRE(read.contracts.size() == 2);
    CHECK(read.contracts[0].system == 77);
    CHECK(read.contracts[0].body == 4);
    CHECK(read.contracts[0].region == 2);
    CHECK(read.contracts[0].kind == "download_data");
    CHECK(read.contracts[0].reward == 640);
    CHECK(read.contracts[0].status == CampaignState::Contract::Status::Accepted);
    CHECK(read.contracts[0].set == 1);
    CHECK(read.contracts[1].status == CampaignState::Contract::Status::Offered);
    CHECK(read.nextContractId == campaign.nextContractId);
    CHECK(read.setContractsDone == 1);
    CHECK(read.nextOfferAt == 1234.5);
    CHECK(read.debrief.any);
    CHECK(read.debrief.kind == "survey");
    CHECK(read.debrief.success);
    CHECK(read.debrief.crew == 0b1011);
    CHECK(read.debrief.aboard == 0b0011);
    CHECK(read.debrief.paid == 250);
    CHECK(read.debrief.entries == std::vector<uint32_t>{3, 4, 9});
    // (Nothing of it left over as a key it does not know.)
    CHECK(read.extra.empty());
}

TEST_CASE("The ship's terminal: its tabs, its rows and its buttons answer clicks where they are drawn", "[contracts][terminal]")
{
    CampaignState campaign;
    campaign.credits = 1500;
    for (int i = 0; i < 12; ++i)
    {
        Contracts::Offer(campaign, 1, i, 0, "survey", 100 + i, -1);
    }
    campaign.AddLog("lead", "1:3:2", "SIGNAL SOURCE 15", "The transmitter was relaying from here.");
    ShipTerminal terminal;
    ScreenCanvas canvas(ShipTerminal::kWide, ShipTerminal::kHigh);
    ShipTerminal::View view;
    terminal.Draw(canvas, campaign, view, TestWords());
    // The tabs along the top, a page each.
    for (int page = 0; page < 4; ++page)
    {
        const ShipTerminal::Press press = terminal.Hit(14 + page * 186 + 89, 54);
        CHECK(press.kind == ShipTerminal::Press::Kind::Page);
        CHECK(press.value == page);
    }
    // The list: a row picked by a click on it; more than fit, so it scrolls.
    const ShipTerminal::Press row = terminal.Hit(150, 80 + 4 + 2 * 40 + 10);
    CHECK(row.kind == ShipTerminal::Press::Kind::Select);
    CHECK(row.value == 2);
    CHECK(terminal.Hit(60, ShipTerminal::kHigh - 20).kind == ShipTerminal::Press::Kind::Scroll);
    // The picked one, on offer: taking it on, its id.
    const ShipTerminal::Press take = terminal.Hit(530, ShipTerminal::kHigh - 12 - 30);
    CHECK(take.kind == ShipTerminal::Press::Kind::Accept);
    CHECK(campaign.FindContract(static_cast<uint32_t>(take.value)) != nullptr);
    // Nothing where nothing is.
    CHECK(terminal.Hit(5, 5).kind == ShipTerminal::Press::Kind::None);
    // A view past its rows is brought back inside them.
    view.selected = 40;
    view.scroll = 40;
    const ShipTerminal::View kept = ShipTerminal::Clamped(campaign, view);
    CHECK(kept.selected == 11);
    CHECK(kept.scroll == 12 - ShipTerminal::kRows);
    // The findings page: its one finding, and plotting a course to it.
    view = {ShipTerminal::Page::Leads, 0, 0};
    terminal.Draw(canvas, campaign, view, TestWords());
    const ShipTerminal::Press plot = terminal.Hit(530, ShipTerminal::kHigh - 12 - 30);
    CHECK(plot.kind == ShipTerminal::Press::Kind::Plot);
    CHECK(plot.lead);
    // The debrief draws with nothing yet, and with one.
    view = {ShipTerminal::Page::Debrief, 0, 0};
    terminal.Draw(canvas, campaign, view, TestWords());
    campaign.debrief.any = true;
    campaign.debrief.crew = 0b11;
    campaign.debrief.aboard = 0b01;
    terminal.Draw(canvas, campaign, view, TestWords());
    CHECK(terminal.Hit(530, ShipTerminal::kHigh - 42).kind == ShipTerminal::Press::Kind::None);
}

TEST_CASE("The terminal's page crosses the wire unchanged", "[contracts][net][protocol]")
{
    WorldEventMessage sent;
    sent.kind = WorldEventKind::Terminal;
    sent.index = 2;
    sent.item = 37;
    sent.rounds = 29;
    BitWriter writer;
    WriteWorldEvent(writer, sent);
    const std::vector<uint8_t>& bytes = writer.Finish();
    BitReader reader(bytes.data(), bytes.size());
    WorldEventMessage received;
    REQUIRE(ReadWorldEvent(reader, received));
    CHECK(received.kind == WorldEventKind::Terminal);
    CHECK(received.index == 2);
    CHECK(received.item == 37);
    CHECK(received.rounds == 29);
}

TEST_CASE("Every kind of landing area has a scan category; all but an outpost a one-line read", "[contracts][campaign]")
{
    const std::set<std::string> categories{"outpost", "structure", "wreck", "signal", "survey", "anomaly"};
    for (const RegionKindDef& kind : Data().regionKinds)
    {
        INFO(kind.id);
        CHECK(categories.count(kind.category) == 1);
        CHECK(kind.read.empty() == (kind.category == "outpost"));
    }
}

TEST_CASE("A place's difficulty rises with the work, what is unknown, the threat and the place itself", "[contracts][difficulty]")
{
    Difficulty::Inputs easy;
    easy.kind = SitePlan::SiteKind::Survey;
    easy.charted = true;
    easy.threatKnown = true;
    easy.signs = 0.1f;
    const Difficulty::Read calm = Difficulty::Rate(easy);
    CHECK(calm.rating == 1);
    CHECK(calm.word == "EASY");
    REQUIRE_FALSE(calm.why.empty());

    Difficulty::Inputs hard = easy;
    hard.kind = SitePlan::SiteKind::Station;
    hard.charted = false;
    hard.signs = 0.9f;
    hard.weather = 0.9f;
    hard.night = true;
    hard.air = false;
    const Difficulty::Read rough = Difficulty::Rate(hard);
    CHECK(rough.rating >= 4);
    // The worst reason first, the kind of work last.
    CHECK(rough.why.front().find("Threat high") != std::string::npos);
    CHECK(rough.why.back().find("Station") != std::string::npos);

    // Not reading the threat is less than a high threat, more than none.
    Difficulty::Inputs unknown = easy;
    unknown.threatKnown = false;
    CHECK(Difficulty::Rate(unknown).rating >= calm.rating);
    CHECK(Difficulty::Line(rough).find("/5)") != std::string::npos);
}
