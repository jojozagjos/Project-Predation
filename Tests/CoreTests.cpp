#include "Engine/Core/CVar.h"
#include "Engine/Core/Config.h"
#include "Engine/Core/Math.h"
#include "Engine/Core/Time.h"
#include "Engine/Debug/Console.h"
#include "Engine/Platform/Input.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Primitives.h"
#include "Engine/Scene/Scene.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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

TEST_CASE("A rebound key is saved, and only the ones that changed", "[input][bindings]")
{
    // A rebinding that does not survive a restart is not a rebinding. And the file has to hold the
    // difference rather than the whole set: a copy of every default goes stale the first time an
    // action is added to the game, arriving unbound for everybody who had ever touched a key.
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "pred_bindings_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path shipped = dir / "shipped.json";
    const std::filesystem::path mine = dir / "mine.json";

    {
        std::ofstream out(shipped);
        out << R"({"actions":{"jump":["Space"],"fire":["Mouse1"],"crouch":["Left Ctrl","C"]}})";
    }

    Input input;
    REQUIRE(input.LoadBindings(shipped));
    // Rebind jump to J, leave the others alone.
    Input::Binding j;
    REQUIRE(Input::ParseBinding("J", j));
    input.SetAction("jump", {j});

    // Only the changed one is written.
    REQUIRE(input.SaveBindings(mine, {"jump"}));

    {
        std::ifstream in(mine);
        REQUIRE(in.is_open());
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        CHECK(text.find("jump") != std::string::npos);
        // The untouched ones stay out of it, which is the whole point.
        CHECK(text.find("crouch") == std::string::npos);
        CHECK(text.find("fire") == std::string::npos);
    }

    // A fresh session: shipped first, then the overrides on top.
    Input restarted;
    REQUIRE(restarted.LoadBindings(shipped));
    REQUIRE(restarted.MergeBindings(mine));
    CHECK(restarted.ActionHasKey("jump", SDL_SCANCODE_J));
    CHECK_FALSE(restarted.ActionHasKey("jump", SDL_SCANCODE_SPACE));
    // And everything it did not mention is untouched, including the action with two keys on it.
    CHECK(restarted.ActionHasKey("crouch", SDL_SCANCODE_LCTRL));
    CHECK(restarted.ActionHasKey("crouch", SDL_SCANCODE_C));

    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("An action cleared on purpose stays cleared", "[input][bindings]")
{
    // Clearing has to be expressible, or "I do not want this bound to anything" is impossible to
    // say: an empty entry that got erased would fall back to the shipped default on the next load
    // and quietly rebind a key somebody had deliberately taken away.
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "pred_bindings_clear";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path shipped = dir / "shipped.json";
    const std::filesystem::path mine = dir / "mine.json";
    {
        std::ofstream out(shipped);
        out << R"({"actions":{"lean_left":["Q"]}})";
    }

    Input input;
    REQUIRE(input.LoadBindings(shipped));
    CHECK(input.ActionHasKey("lean_left", SDL_SCANCODE_Q));
    input.SetAction("lean_left", {});
    REQUIRE(input.SaveBindings(mine, {"lean_left"}));

    Input restarted;
    REQUIRE(restarted.LoadBindings(shipped));
    REQUIRE(restarted.MergeBindings(mine));
    CHECK_FALSE(restarted.ActionHasKey("lean_left", SDL_SCANCODE_Q));

    std::filesystem::remove_all(dir, ec);
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

TEST_CASE("Scene handles detect stale entities after slot reuse", "[scene]")
{
    Scene scene;
    REQUIRE(scene.EntityCount() == 0);

    const Entity first = scene.Create("first");
    REQUIRE(first.IsValid());
    REQUIRE(scene.IsAlive(first));
    REQUIRE(scene.EntityCount() == 1);
    REQUIRE(scene.Name(first) == "first");

    scene.Destroy(first);
    REQUIRE_FALSE(scene.IsAlive(first));
    REQUIRE(scene.EntityCount() == 0);
    REQUIRE(scene.GetTransform(first) == nullptr);

    // The next entity reuses the slot but must not be reachable through the old handle.
    const Entity second = scene.Create("second");
    REQUIRE(second.index == first.index);
    REQUIRE(second.generation != first.generation);
    REQUIRE(scene.IsAlive(second));
    REQUIRE_FALSE(scene.IsAlive(first));
    REQUIRE(scene.Name(second) == "second");

    // A default-constructed handle is never alive.
    REQUIRE_FALSE(scene.IsAlive(Entity{}));
    scene.Destroy(Entity{}); // must not throw or corrupt state
    REQUIRE(scene.EntityCount() == 1);
}

TEST_CASE("Scene stores transforms and mesh renderers per entity", "[scene]")
{
    Scene scene;
    const Entity entity = scene.Create("box");
    Transform* transform = scene.GetTransform(entity);
    REQUIRE(transform != nullptr);
    transform->position = {1.0f, 2.0f, 3.0f};
    REQUIRE(scene.GetTransform(entity)->position.y == 2.0f);

    REQUIRE(scene.GetMeshRenderer(entity) == nullptr);
    MeshRenderer renderer;
    renderer.mesh = MeshHandle{7};
    renderer.material = Material::Diffuse({1.0f, 0.0f, 0.0f});
    scene.SetMeshRenderer(entity, renderer);

    const MeshRenderer* stored = scene.GetMeshRenderer(entity);
    REQUIRE(stored != nullptr);
    REQUIRE(stored->mesh.index == 7);
    REQUIRE(stored->material.baseColor.r == 1.0f);

    size_t visited = 0;
    scene.ForEachMeshRenderer([&](Entity, const Transform&, const MeshRenderer&) { ++visited; });
    REQUIRE(visited == 1);

    // Invisible renderers are skipped by iteration but still stored.
    scene.GetMeshRenderer(entity)->visible = false;
    visited = 0;
    scene.ForEachMeshRenderer([&](Entity, const Transform&, const MeshRenderer&) { ++visited; });
    REQUIRE(visited == 0);

    scene.RemoveMeshRenderer(entity);
    REQUIRE(scene.GetMeshRenderer(entity) == nullptr);
}

TEST_CASE("Your own head is hidden from your eyes and from nothing else", "[scene]")
{
    // Three sets, not two. `hiddenFromCamera` is the player's head in first person, and it means
    // "not from the camera's own point of view" -- which is a statement about that one viewpoint,
    // not about the head being absent. It has to be in the shadow maps, or the shadow on the ground
    // in front of you is decapitated; and it has to be in a mirror, because a mirror is precisely
    // not the camera's point of view, it is looking back at you from across the room.
    //
    // This is written down as a test because both failures look like a rendering fault rather than
    // like a set membership mistake, and one of them has already happened once.
    Scene scene;
    const Entity body = scene.Create("body");
    const Entity head = scene.Create("head");

    MeshRenderer bodyRenderer;
    bodyRenderer.mesh = MeshHandle{1};
    scene.SetMeshRenderer(body, bodyRenderer);

    MeshRenderer headRenderer;
    headRenderer.mesh = MeshHandle{2};
    headRenderer.hiddenFromCamera = true;
    scene.SetMeshRenderer(head, headRenderer);

    const auto count = [](auto&& visit)
    {
        size_t seen = 0;
        visit([&](Entity, const Transform&, const MeshRenderer&) { ++seen; });
        return seen;
    };

    CHECK(count([&](auto&& fn) { scene.ForEachMeshRenderer(fn); }) == 1);
    CHECK(count([&](auto&& fn) { scene.ForEachShadowCaster(fn); }) == 2);
    CHECK(count([&](auto&& fn) { scene.ForEachReflected(fn); }) == 2);

    // And something that casts no shadow is missing from exactly one of the three.
    scene.GetMeshRenderer(body)->castsShadow = false;
    CHECK(count([&](auto&& fn) { scene.ForEachMeshRenderer(fn); }) == 1);
    CHECK(count([&](auto&& fn) { scene.ForEachShadowCaster(fn); }) == 1);
    CHECK(count([&](auto&& fn) { scene.ForEachReflected(fn); }) == 2);

    // Invisible is invisible everywhere: it is the one flag that means "not in the world".
    scene.GetMeshRenderer(head)->visible = false;
    CHECK(count([&](auto&& fn) { scene.ForEachMeshRenderer(fn); }) == 1);
    CHECK(count([&](auto&& fn) { scene.ForEachShadowCaster(fn); }) == 0);
    CHECK(count([&](auto&& fn) { scene.ForEachReflected(fn); }) == 1);
}

TEST_CASE("Primitive builders produce closed, correctly sized geometry", "[render][primitives]")
{
    SECTION("box")
    {
        const MeshData box = Primitives::Box({2.0f, 4.0f, 6.0f});
        REQUIRE(box.vertices.size() == 24); // 6 faces x 4 unshared vertices
        REQUIRE(box.TriangleCount() == 12);

        const AABB bounds = box.ComputeBounds();
        REQUIRE(bounds.IsValid());
        REQUIRE(bounds.Size().x == Catch::Approx(2.0f));
        REQUIRE(bounds.Size().y == Catch::Approx(4.0f));
        REQUIRE(bounds.Size().z == Catch::Approx(6.0f));
        REQUIRE(glm::length(bounds.Center()) == Catch::Approx(0.0f).margin(1e-5));

        // Every index must address a real vertex.
        for (const uint32_t index : box.indices)
        {
            REQUIRE(index < box.vertices.size());
        }
        // Face normals must be unit length.
        for (const MeshVertex& vertex : box.vertices)
        {
            REQUIRE(glm::length(vertex.normal) == Catch::Approx(1.0f).epsilon(1e-4));
        }
    }

    SECTION("plane")
    {
        const MeshData plane = Primitives::Plane({10.0f, 10.0f}, 4);
        REQUIRE(plane.vertices.size() == 25);
        REQUIRE(plane.TriangleCount() == 32);
        const AABB bounds = plane.ComputeBounds();
        REQUIRE(bounds.Size().x == Catch::Approx(10.0f));
        REQUIRE(bounds.Size().z == Catch::Approx(10.0f));
        REQUIRE(bounds.Size().y == Catch::Approx(0.0f));
    }

    SECTION("sphere")
    {
        const MeshData sphere = Primitives::Sphere(2.0f, 16, 12);
        const AABB bounds = sphere.ComputeBounds();
        REQUIRE(bounds.Size().x == Catch::Approx(4.0f).epsilon(0.02));
        REQUIRE(bounds.Size().y == Catch::Approx(4.0f).epsilon(0.02));
        for (const MeshVertex& vertex : sphere.vertices)
        {
            REQUIRE(glm::length(vertex.position) == Catch::Approx(2.0f).epsilon(1e-4));
        }
    }

    SECTION("ellipsoid, capsule and frustum are the size asked for, with normals facing out")
    {
        const auto outward = [](const MeshData& mesh)
        {
            // Every normal points away from the middle: no inside-out shading.
            for (const MeshVertex& vertex : mesh.vertices)
            {
                if (glm::length(vertex.position) > 1e-4f && glm::dot(vertex.normal, vertex.position) < -1e-4f)
                {
                    return false;
                }
            }
            return true;
        };

        const MeshData ellipsoid = Primitives::Ellipsoid({0.5f, 0.3f, 0.9f});
        const AABB e = ellipsoid.ComputeBounds();
        CHECK(e.Size().x == Catch::Approx(1.0f).epsilon(0.02));
        CHECK(e.Size().y == Catch::Approx(0.6f).epsilon(0.02));
        CHECK(e.Size().z == Catch::Approx(1.8f).epsilon(0.02));
        CHECK(outward(ellipsoid));

        const MeshData capsule = Primitives::Capsule(0.1f, 0.8f);
        const AABB c = capsule.ComputeBounds();
        CHECK(c.Size().y == Catch::Approx(0.8f).epsilon(0.01));
        CHECK(c.Size().x == Catch::Approx(0.2f).epsilon(0.03));
        CHECK(outward(capsule));

        const MeshData cone = Primitives::Frustum(0.2f, 0.0f, 1.0f);
        const AABB f = cone.ComputeBounds();
        CHECK(f.Size().y == Catch::Approx(1.0f));
        CHECK(f.Size().x == Catch::Approx(0.4f).epsilon(0.03));
        CHECK(cone.TriangleCount() > 0);
    }

    SECTION("stairs climb to the expected height")
    {
        const MeshData stairs = Primitives::Stairs(10, 2.0f, 0.2f, 0.3f);
        const AABB bounds = stairs.ComputeBounds();
        REQUIRE(bounds.max.y == Catch::Approx(2.0f));  // 10 steps x 0.2 m
        REQUIRE(bounds.min.y == Catch::Approx(0.0f));
        REQUIRE(bounds.max.z == Catch::Approx(3.0f));  // 10 steps x 0.3 m
        REQUIRE(bounds.Size().x == Catch::Approx(2.0f));
    }

    SECTION("ramp rises along +Z")
    {
        const MeshData ramp = Primitives::Ramp(3.0f, 6.0f, 2.0f);
        const AABB bounds = ramp.ComputeBounds();
        REQUIRE(bounds.min.y == Catch::Approx(0.0f));
        REQUIRE(bounds.max.y == Catch::Approx(2.0f));
        REQUIRE(bounds.max.z == Catch::Approx(6.0f));
        REQUIRE(bounds.Size().x == Catch::Approx(3.0f));
    }

    SECTION("empty geometry produces an invalid bounding box")
    {
        const MeshData empty;
        REQUIRE_FALSE(empty.ComputeBounds().IsValid());
    }
}

namespace
{
// Signed volume of a closed mesh, via the divergence theorem. Positive means the triangles are
// wound so their faces point outwards; negative means the whole solid is inside-out.
double SignedVolume(const MeshData& mesh)
{
    double volume = 0.0;
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
    {
        const glm::vec3& v0 = mesh.vertices[mesh.indices[i]].position;
        const glm::vec3& v1 = mesh.vertices[mesh.indices[i + 1]].position;
        const glm::vec3& v2 = mesh.vertices[mesh.indices[i + 2]].position;
        volume += glm::dot(glm::dvec3(v0), glm::cross(glm::dvec3(v1), glm::dvec3(v2)));
    }
    return volume / 6.0;
}

// Counts triangles whose winding-derived normal disagrees with their stored vertex normals.
int WindingMismatches(const MeshData& mesh)
{
    int mismatches = 0;
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
    {
        const MeshVertex& a = mesh.vertices[mesh.indices[i]];
        const MeshVertex& b = mesh.vertices[mesh.indices[i + 1]];
        const MeshVertex& c = mesh.vertices[mesh.indices[i + 2]];

        const glm::vec3 edge0 = b.position - a.position;
        const glm::vec3 edge1 = c.position - a.position;
        const glm::vec3 geometric = glm::cross(edge0, edge1);
        // Degeneracy has to be judged relative to the triangle's own edge lengths, not against a
        // fixed epsilon, or slivers on a large mesh slip through as if they were real faces.
        const float edgeScale = glm::length(edge0) * glm::length(edge1);
        if (edgeScale < 1e-12f || glm::length(geometric) < edgeScale * 1e-4f)
        {
            continue; // degenerate triangle, nothing meaningful to compare against
        }
        const glm::vec3 stored = a.normal + b.normal + c.normal;
        if (glm::length(stored) < 1e-9f || glm::dot(glm::normalize(geometric), glm::normalize(stored)) <= 0.0f)
        {
            ++mismatches;
        }
    }
    return mismatches;
}
} // namespace

// This invariant is what catches inside-out geometry. Supplying a face's normal separately from its
// winding lets the two disagree, which is how the ramp shipped inverted: it rendered as a hole
// because backface culling discarded the surfaces the player was meant to walk on.
TEST_CASE("Primitive faces wind outwards and agree with their normals", "[render][primitives]")
{
    struct Case
    {
        const char* name;
        MeshData mesh;
        double expectedVolume; // <= 0 means "only require positive"
    };

    std::vector<Case> cases;
    cases.push_back({"box", Primitives::Box({2.0f, 4.0f, 6.0f}), 48.0});
    cases.push_back({"ramp", Primitives::Ramp(3.0f, 6.0f, 2.0f), 0.5 * 3.0 * 6.0 * 2.0});
    cases.push_back({"stairs", Primitives::Stairs(10, 2.0f, 0.2f, 0.3f), 6.6});
    cases.push_back({"sphere", Primitives::Sphere(2.0f, 32, 24), -1.0});
    cases.push_back({"cylinder", Primitives::Cylinder(1.0f, 2.0f, 32), -1.0});
    // Volumes checked against the formulas, to a few percent for the facets.
    cases.push_back({"ellipsoid", Primitives::Ellipsoid({1.0f, 0.5f, 2.0f}, 48, 32),
                     4.0 / 3.0 * 3.14159265 * 1.0 * 0.5 * 2.0});
    cases.push_back({"capsule", Primitives::Capsule(0.5f, 3.0f, 48, 32),
                     3.14159265 * 0.25 * 2.0 + 4.0 / 3.0 * 3.14159265 * 0.125});
    cases.push_back({"frustum", Primitives::Frustum(1.0f, 0.5f, 2.0f, 48),
                     3.14159265 * 2.0 / 3.0 * (1.0 + 0.5 + 0.25)});
    cases.push_back({"cone", Primitives::Frustum(1.0f, 0.0f, 3.0f, 48), 3.14159265 * 3.0 / 3.0});

    for (const Case& testCase : cases)
    {
        INFO("primitive: " << testCase.name);
        REQUIRE(WindingMismatches(testCase.mesh) == 0);

        const double volume = SignedVolume(testCase.mesh);
        REQUIRE(volume > 0.0);
        if (testCase.expectedVolume > 0.0)
        {
            REQUIRE(volume == Catch::Approx(testCase.expectedVolume).epsilon(0.02));
        }
    }
}

TEST_CASE("Ramp rises along +Z with an upward-facing slope", "[render][primitives]")
{
    const MeshData ramp = Primitives::Ramp(3.0f, 6.0f, 2.0f);

    // The sloped surface must face upwards and lean back towards the low end.
    int upwardSlopeVertices = 0;
    for (const MeshVertex& vertex : ramp.vertices)
    {
        if (vertex.normal.y > 0.3f && vertex.normal.z < -0.1f)
        {
            ++upwardSlopeVertices;
        }
    }
    REQUIRE(upwardSlopeVertices >= 4);

    // Exactly one face should point straight down, and one straight back along +Z.
    int downward = 0;
    int backward = 0;
    for (const MeshVertex& vertex : ramp.vertices)
    {
        if (vertex.normal.y < -0.99f)
        {
            ++downward;
        }
        if (vertex.normal.z > 0.99f)
        {
            ++backward;
        }
    }
    REQUIRE(downward == 4);
    REQUIRE(backward == 4);
}

TEST_CASE("MeshData::Append offsets indices and transforms vertices", "[render][primitives]")
{
    MeshData combined = Primitives::Box({1.0f, 1.0f, 1.0f});
    const size_t baseVertices = combined.vertices.size();
    const size_t baseIndices = combined.indices.size();

    const MeshData second = Primitives::Box({1.0f, 1.0f, 1.0f});
    const glm::mat4 offset = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f));
    combined.Append(second, offset);

    REQUIRE(combined.vertices.size() == baseVertices * 2);
    REQUIRE(combined.indices.size() == baseIndices * 2);

    // The appended half must reference only its own vertices, never the originals.
    for (size_t i = baseIndices; i < combined.indices.size(); ++i)
    {
        REQUIRE(combined.indices[i] >= baseVertices);
        REQUIRE(combined.indices[i] < combined.vertices.size());
    }

    const AABB bounds = combined.ComputeBounds();
    REQUIRE(bounds.min.x == Catch::Approx(-0.5f));
    REQUIRE(bounds.max.x == Catch::Approx(10.5f));

    // Normals must survive the transform as unit vectors.
    for (const MeshVertex& vertex : combined.vertices)
    {
        REQUIRE(glm::length(vertex.normal) == Catch::Approx(1.0f).epsilon(1e-4));
    }
}

TEST_CASE("Transform builds a matrix with translation, rotation and scale", "[math]")
{
    Transform transform;
    transform.position = {1.0f, 2.0f, 3.0f};
    transform.scale = {2.0f, 2.0f, 2.0f};

    const glm::mat4 matrix = transform.Matrix();
    const glm::vec3 origin = glm::vec3(matrix * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    REQUIRE(origin.x == Catch::Approx(1.0f));
    REQUIRE(origin.y == Catch::Approx(2.0f));
    REQUIRE(origin.z == Catch::Approx(3.0f));

    const glm::vec3 scaled = glm::vec3(matrix * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
    REQUIRE(scaled.x == Catch::Approx(3.0f)); // 1 unit scaled by 2, offset by 1

    // A quarter turn about Y maps +X onto -Z.
    transform.position = glm::vec3(0.0f);
    transform.scale = glm::vec3(1.0f);
    transform.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec3 rotated = glm::vec3(transform.Matrix() * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
    REQUIRE(rotated.x == Catch::Approx(0.0f).margin(1e-5));
    REQUIRE(rotated.z == Catch::Approx(-1.0f).margin(1e-5));
}

TEST_CASE("AABB expansion and metrics", "[math]")
{
    AABB bounds;
    REQUIRE_FALSE(bounds.IsValid());
    REQUIRE(bounds.BoundingRadius() == Catch::Approx(0.0f));

    bounds.Expand(glm::vec3(-1.0f, -2.0f, -3.0f));
    bounds.Expand(glm::vec3(1.0f, 2.0f, 3.0f));
    REQUIRE(bounds.IsValid());
    REQUIRE(bounds.Size().y == Catch::Approx(4.0f));
    REQUIRE(glm::length(bounds.Center()) == Catch::Approx(0.0f).margin(1e-5));

    AABB other;
    other.Expand(glm::vec3(5.0f));
    bounds.Expand(other);
    REQUIRE(bounds.max.x == Catch::Approx(5.0f));
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


TEST_CASE("Two boxes of different sizes do not end up as the same mesh", "[render][map]")
{
    // Meshes are shared by name, which is what stopped the editor exhausting the renderer by
    // uploading a new one on every frame of a drag. It also means that anything uploading several
    // different shapes under one name gets whichever was uploaded last, for all of them: the
    // corridor's doorways narrow as they go and every lintel was called "gap_lintel", so they all
    // came out the width of the last and the tops of the wide ones stopped reaching their walls.
    //
    // Checked on the fingerprint rather than through the renderer, because there is no renderer in
    // a test: the fingerprint is what Upload compares, so two shapes that disagree here are two
    // shapes it will keep apart.
    const MeshData wide = Primitives::Box({1.6f, 0.9f, 0.3f});
    const MeshData narrow = Primitives::Box({0.7f, 0.9f, 0.3f});
    CHECK(MeshFingerprintForTesting(wide) != MeshFingerprintForTesting(narrow));

    // And the same shape twice really is the same, or the sharing does nothing.
    const MeshData again = Primitives::Box({1.6f, 0.9f, 0.3f});
    CHECK(MeshFingerprintForTesting(wide) == MeshFingerprintForTesting(again));
}
