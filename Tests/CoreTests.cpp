#include "Engine/Core/CVar.h"
#include "Engine/Core/Config.h"
#include "Engine/Core/Math.h"
#include "Engine/Core/Time.h"
#include "Engine/Debug/Console.h"
#include "Engine/Platform/Input.h"
#include "Engine/Render/Primitives.h"
#include "Engine/Scene/Scene.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
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
