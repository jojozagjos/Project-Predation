#include "Engine/Audio/SoundDesign.h"
#include "Engine/Core/JsonText.h"
#include "Game/Creature/CreatureTuning.h"
#include "Game/Items/ItemDatabase.h"
#include "Game/Weapons/WeaponDatabase.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace pred;

// The data files in Assets are edited by hand. A key spelled wrong is read by nothing, and whatever it
// was meant to set keeps its default without a word -- so every loader names the keys it does not know,
// and the files the game ships with must have none.

namespace
{

std::filesystem::path DataFile(const char* name)
{
    return std::filesystem::path(PRED_SOURCE_DIR) / "Assets" / "Data" / name;
}

std::string Joined(const std::vector<std::string>& lines)
{
    std::string out;
    for (const std::string& line : lines)
    {
        out += "\n  " + line;
    }
    return out;
}

} // namespace

TEST_CASE("The shipped data files have no keys that nothing reads", "[data]")
{
    ItemDatabase items;
    REQUIRE(items.LoadFromFile(DataFile("items.json")));
    INFO("items.json:" << Joined(items.Warnings()));
    CHECK(items.Warnings().empty());

    WeaponDatabase weapons;
    REQUIRE(weapons.LoadFromFile(DataFile("weapons.json")));
    INFO("weapons.json:" << Joined(weapons.Warnings()));
    CHECK(weapons.Warnings().empty());

    std::vector<std::string> errors;
    const std::vector<SoundPatch> patches = LoadSoundLibrary(DataFile("Sounds").string(), &errors);
    INFO("Sounds:" << Joined(errors));
    CHECK(errors.empty());
    CHECK(patches.size() > 60);
}

TEST_CASE("A misspelled key is named rather than ignored", "[data]")
{
    const nlohmann::json entry = {{"key", "medkit"}, {"max_stak", 2}, {"_comment", "fine"}, {"name", "Medical Kit"}};
    const std::vector<std::string> unknown = UnknownKeys(entry, {"key", "name", "max_stack"});
    REQUIRE(unknown.size() == 1);
    CHECK(unknown[0] == "max_stak");

    const std::filesystem::path file = std::filesystem::temp_directory_path() / "predation_typo_items.json";
    {
        std::ofstream out(file);
        out << R"({"items":[{"key":"medkit","name":"Medical Kit","max_stak":2,"use":{"kind":"heal","secnds":2}}]})";
    }
    ItemDatabase items;
    REQUIRE(items.LoadFromFile(file));
    std::filesystem::remove(file);
    REQUIRE(items.Warnings().size() == 2);
    CHECK(items.Warnings()[0] == "medkit: max_stak");
    CHECK(items.Warnings()[1] == "medkit.use: secnds");
}

TEST_CASE("creatures.json reads cleanly, and says what the code would have anyway", "[data][creature]")
{
    CreatureTuning read;
    std::vector<std::string> warnings;
    REQUIRE(LoadCreatureTuning(DataFile("creatures.json"), read, &warnings));
    INFO("creatures.json:" << Joined(warnings));
    CHECK(warnings.empty());
    // The shipped file holds the numbers the tests were written against; changing it is tuning, and a
    // test here says so rather than letting forty others fail for reasons that look like bugs.
    const CreatureTuning defaults;
    CHECK(read.sightRange == defaults.sightRange);
    CHECK(read.exposureGain == defaults.exposureGain);
    CHECK(read.commitment == defaults.commitment);
    CHECK(read.ambushPatienceRange == defaults.ambushPatienceRange);
    CHECK(read.lureEvery == defaults.lureEvery);
}
