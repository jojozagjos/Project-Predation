#include "Engine/Core/CVar.h"
#include "Engine/Core/Config.h"
#include "Engine/Core/Time.h"
#include "Engine/Debug/Console.h"
#include "Engine/Platform/Input.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

using namespace pred;

TEST_CASE("CVar registers, parses and formats values", "[cvar]")
{
    CVar<int> intVar{"test.int", 5, "int"};
    CVar<float> floatVar{"test.float", 1.5f, "float"};
    CVar<bool> boolVar{"test.bool", false, "bool"};
    CVar<std::string> stringVar{"test.string", "hello", "string"};

    REQUIRE(CVarRegistry::Instance().Find("test.int") == &intVar);
    REQUIRE(intVar.Type() == CVarType::Int);

    REQUIRE(intVar.SetFromString("42"));
    REQUIRE(intVar.Get() == 42);
    REQUIRE_FALSE(intVar.SetFromString("abc"));
    REQUIRE(intVar.Get() == 42);
    REQUIRE(intVar.SetFromString(" 7 "));
    REQUIRE(intVar.Get() == 7);

    REQUIRE(floatVar.SetFromString("2.25"));
    REQUIRE(floatVar.Get() == 2.25f);
    REQUIRE(floatVar.GetString() == "2.25");

    REQUIRE(boolVar.SetFromString("on"));
    REQUIRE(boolVar.Get());
    REQUIRE(boolVar.SetFromString("0"));
    REQUIRE_FALSE(boolVar.Get());
    REQUIRE_FALSE(boolVar.SetFromString("maybe"));

    REQUIRE(stringVar.SetFromString("world"));
    REQUIRE(stringVar.Get() == "world");
    REQUIRE_FALSE(stringVar.IsDefault());
    stringVar.ResetToDefault();
    REQUIRE(stringVar.IsDefault());
    REQUIRE(stringVar.Get() == "hello");
}

TEST_CASE("CVar registry applies pending values when a cvar registers late", "[cvar]")
{
    CVarRegistry& registry = CVarRegistry::Instance();
    REQUIRE(registry.Set("test.late", "99") == CVarRegistry::SetResult::Pending);

    CVar<int> late{"test.late", 1, "registered after config"};
    REQUIRE(late.Get() == 99);

    REQUIRE(registry.Set("test.late", "nope") == CVarRegistry::SetResult::ParseError);
    REQUIRE(late.Get() == 99);
    REQUIRE(registry.Set("test.late", "5") == CVarRegistry::SetResult::Applied);
    REQUIRE(late.Get() == 5);
}

TEST_CASE("CVar unregisters on destruction", "[cvar]")
{
    {
        CVar<int> scoped{"test.scoped", 1, ""};
        REQUIRE(CVarRegistry::Instance().Find("test.scoped") != nullptr);
    }
    REQUIRE(CVarRegistry::Instance().Find("test.scoped") == nullptr);
}

TEST_CASE("CVar change callbacks fire on real changes only", "[cvar]")
{
    CVar<int> var{"test.callback", 1, ""};
    int calls = 0;
    var.OnChange([&](CVarBase&) { ++calls; });

    var.Set(1);
    REQUIRE(calls == 0);
    var.Set(2);
    REQUIRE(calls == 1);
    var.SetFromString("2");
    REQUIRE(calls == 1);
    var.SetFromString("3");
    REQUIRE(calls == 2);
}

TEST_CASE("Read-only cvars reject registry writes", "[cvar]")
{
    CVar<int> readOnly{"test.readonly", 3, "", CVarFlags::ReadOnly};
    REQUIRE(CVarRegistry::Instance().Set("test.readonly", "4") == CVarRegistry::SetResult::ReadOnly);
    REQUIRE(readOnly.Get() == 3);
    REQUIRE(CVarRegistry::Instance().Set("test.readonly", "4", true) == CVarRegistry::SetResult::Applied);
    REQUIRE(readOnly.Get() == 4);
}

TEST_CASE("Config flattens nested JSON into cvars and later layers win", "[config]")
{
    CVar<int> width{"cfgtest.r.width", 100, ""};
    CVar<bool> vsync{"cfgtest.r.vsync", false, ""};
    CVar<std::string> name{"cfgtest.name", "default", ""};
    CVar<float> scale{"cfgtest.scale", 1.0f, ""};

    const auto defaults = nlohmann::json::parse(
        R"({"cfgtest": {"r": {"width": 640, "vsync": true}, "name": "from-defaults", "scale": 2.5}})");
    Config::LoadResult result = Config::ApplyJson(defaults, "defaults");
    REQUIRE(result.applied == 4);
    REQUIRE(result.errors == 0);
    REQUIRE(width.Get() == 640);
    REQUIRE(vsync.Get());
    REQUIRE(name.Get() == "from-defaults");
    REQUIRE(scale.Get() == 2.5f);

    const auto user = nlohmann::json::parse(R"({"cfgtest": {"r": {"width": 1920}}})");
    result = Config::ApplyJson(user, "user");
    REQUIRE(result.applied == 1);
    REQUIRE(width.Get() == 1920);
    REQUIRE(vsync.Get());

    const auto bad = nlohmann::json::parse(R"({"cfgtest": {"r": {"width": "wide"}, "list": [1, 2]}})");
    result = Config::ApplyJson(bad, "bad");
    REQUIRE(result.errors == 2);
    REQUIRE(width.Get() == 1920);
}

TEST_CASE("Config archive round-trips through a file", "[config]")
{
    CVar<int> archived{"archtest.value", 1, "", CVarFlags::Archive};
    CVar<int> transient{"archtest.transient", 1, ""};
    archived.Set(77);
    transient.Set(88);

    const nlohmann::json json = Config::ArchiveToJson();
    REQUIRE(json.contains("archtest"));
    REQUIRE(json["archtest"]["value"] == 77);
    REQUIRE_FALSE(json["archtest"].contains("transient"));

    const std::filesystem::path file = std::filesystem::temp_directory_path() / "predation_test_settings.json";
    REQUIRE(Config::SaveArchive(file));
    archived.Set(1);
    const Config::LoadResult load = Config::LoadFile(file);
    REQUIRE(load.fileFound);
    REQUIRE(archived.Get() == 77);
    std::filesystem::remove(file);
}

TEST_CASE("Input binding names parse to scancodes and mouse buttons", "[input]")
{
    Input::Binding binding;
    REQUIRE(Input::ParseBinding("F3", binding));
    REQUIRE(binding.kind == Input::Binding::Kind::Key);
    REQUIRE(binding.code == SDL_SCANCODE_F3);

    REQUIRE(Input::ParseBinding("W", binding));
    REQUIRE(binding.code == SDL_SCANCODE_W);

    REQUIRE(Input::ParseBinding("Grave", binding));
    REQUIRE(binding.code == SDL_SCANCODE_GRAVE);

    REQUIRE(Input::ParseBinding("Left Shift", binding));
    REQUIRE(binding.code == SDL_SCANCODE_LSHIFT);

    REQUIRE(Input::ParseBinding("Mouse2", binding));
    REQUIRE(binding.kind == Input::Binding::Kind::Mouse);
    REQUIRE(binding.code == static_cast<int>(MouseButton::Right));

    REQUIRE(Input::ParseBinding("lmb", binding));
    REQUIRE(binding.code == static_cast<int>(MouseButton::Left));

    REQUIRE_FALSE(Input::ParseBinding("NotAKey", binding));
    REQUIRE_FALSE(Input::ParseBinding("", binding));
}

TEST_CASE("Input actions can be bound and queried by key", "[input]")
{
    Input input;
    Input::Binding binding;
    REQUIRE(Input::ParseBinding("F3", binding));
    input.BindAction("debug_overlay", binding);
    REQUIRE(input.ActionHasKey("debug_overlay", SDL_SCANCODE_F3));
    REQUIRE_FALSE(input.ActionHasKey("debug_overlay", SDL_SCANCODE_F4));
    REQUIRE_FALSE(input.ActionHasKey("missing", SDL_SCANCODE_F3));
}

TEST_CASE("Fixed step accumulator produces stable step counts and drops runaway time", "[time]")
{
    FixedStepAccumulator accumulator(1.0 / 60.0, 5);
    int steps = 0;
    for (int i = 0; i < 60; ++i)
    {
        steps += accumulator.Accumulate(1.0 / 60.0);
    }
    REQUIRE(steps >= 59);
    REQUIRE(steps <= 61);

    REQUIRE(accumulator.Accumulate(1.0) == 5);
    REQUIRE(accumulator.DroppedTimeLastFrame());
    REQUIRE(accumulator.Alpha() >= 0.0);
    REQUIRE(accumulator.Alpha() < 1.0);
}

TEST_CASE("Console tokenizer handles quotes and whitespace", "[console]")
{
    const std::vector<std::string> tokens = Console::Tokenize("  spawn_creature  \"north wing\" 42 ");
    REQUIRE(tokens.size() == 3);
    REQUIRE(tokens[0] == "spawn_creature");
    REQUIRE(tokens[1] == "north wing");
    REQUIRE(tokens[2] == "42");
    REQUIRE(Console::Tokenize("").empty());
    REQUIRE(Console::Tokenize("\"\"").size() == 1);
}
