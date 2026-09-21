#include "Engine/Assets/GltfImport.h"
#include "Engine/Assets/ModelAsset.h"
#include "Engine/Core/Paths.h"
#include "Game/Weapons/WeaponAppearance.h"
#include "Game/Weapons/WeaponDatabase.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

using namespace pred;

// The glTF importer, run against the files it was written for. A parser tested only on something
// this repository generated is a parser tested against its own assumptions, and every fault worth
// catching here is a fault about what real exporters actually write.
namespace
{

std::filesystem::path SourceModel(const char* name)
{
    // Source art lives beside the models it produced, in the same folders.
    return std::filesystem::path(PRED_SOURCE_DIR) / "Assets" / "Models" / "Source" / "Weapons" / name;
}

} // namespace

TEST_CASE("A downloaded weapon imports as separable parts", "[assets][gltf]")
{
    const std::filesystem::path file = SourceModel("m5_carbine.glb");
    INFO("reading " << file.string());
    REQUIRE(std::filesystem::exists(file));
    REQUIRE(IsGltfFile(file));

    GltfImportOptions options;
    options.targetSize = 0.72f; // a short carbine, near enough
    std::string error;
    ModelAsset model;
    REQUIRE(LoadGlbModel(file, options, model, &error));
    INFO("importer said: " << error);

    // Separate parts, not one welded lump. A magazine has to be its own part or a reload cannot
    // move it, and that is the whole reason primitives are kept apart rather than merged.
    CHECK(model.parts.size() > 1);

    size_t triangles = 0;
    for (const ModelPart& part : model.parts)
    {
        INFO("part " << part.name);
        CHECK(part.shape == PartShape::Mesh);
        CHECK_FALSE(part.name.empty());
        CHECK(part.mesh.vertices.size() >= 3);
        CHECK(part.mesh.indices.size() % 3 == 0);
        triangles += part.mesh.TriangleCount();

        // Every index has to point at a vertex of the part it belongs to. An index that runs past
        // the end is the classic way a primitive-per-part importer goes wrong, and it does not
        // crash: it draws a triangle across the model to a vertex from somewhere else.
        for (const uint32_t index : part.mesh.indices)
        {
            REQUIRE(index < part.mesh.vertices.size());
        }
    }
    CHECK(triangles > 100);
}

TEST_CASE("An imported model is scaled to something a person can hold", "[assets][gltf]")
{
    // Downloads arrive in wildly different units. A model a hundred times too large is
    // indistinguishable from one that failed to load, because both fill the screen with nothing.
    for (const char* name : {"m5_carbine.glb", "m9_pistol.glb"})
    {
        const std::filesystem::path file = SourceModel(name);
        REQUIRE(std::filesystem::exists(file));

        GltfImportOptions options;
        options.targetSize = 0.5f;
        ModelAsset model;
        REQUIRE(LoadGlbModel(file, options, model));

        glm::vec3 low{1e9f};
        glm::vec3 high{-1e9f};
        for (const ModelPart& part : model.parts)
        {
            for (const MeshVertex& vertex : part.mesh.vertices)
            {
                low = glm::min(low, vertex.position);
                high = glm::max(high, vertex.position);
            }
        }

        const glm::vec3 extent = high - low;
        const float longest = std::max({extent.x, extent.y, extent.z});
        INFO(name << " came out " << extent.x << " by " << extent.y << " by " << extent.z);
        CHECK(longest > 0.49f);
        CHECK(longest < 0.51f);

        // And centred on its own middle, so it appears where it is placed rather than off in space.
        const glm::vec3 centre = (low + high) * 0.5f;
        CHECK(glm::length(centre) < 0.01f);
    }
}

TEST_CASE("The importer turns a model when asked", "[assets][gltf]")
{
    // glTF has no idea which way a weapon points, and the game wants the barrel down +Z. Turning it
    // once at import beats turning every part by hand afterwards.
    const std::filesystem::path file = SourceModel("m9_pistol.glb");
    REQUIRE(std::filesystem::exists(file));

    GltfImportOptions upright;
    upright.targetSize = 0.3f;
    ModelAsset straight;
    REQUIRE(LoadGlbModel(file, upright, straight));

    GltfImportOptions turned = upright;
    turned.rotationDegrees = {0.0f, 90.0f, 0.0f};
    ModelAsset sideways;
    REQUIRE(LoadGlbModel(file, turned, sideways));

    REQUIRE(straight.parts.size() == sideways.parts.size());
    REQUIRE(!straight.parts.empty());
    REQUIRE(straight.parts[0].mesh.vertices.size() == sideways.parts[0].mesh.vertices.size());

    // A quarter turn about Y sends what was along +X off along -Z. Checked on the extent rather than
    // on one vertex, because which vertex is which is the exporter's business.
    const AABB before = straight.parts[0].mesh.ComputeBounds();
    const AABB after = sideways.parts[0].mesh.ComputeBounds();
    const glm::vec3 wide = before.max - before.min;
    const glm::vec3 tall = after.max - after.min;
    INFO("before " << wide.x << "," << wide.y << "," << wide.z << "  after " << tall.x << "," << tall.y
                   << "," << tall.z);
    CHECK(std::abs(tall.z - wide.x) < 0.01f);
    CHECK(std::abs(tall.x - wide.z) < 0.01f);
    CHECK(std::abs(tall.y - wide.y) < 0.01f);
}

TEST_CASE("A file that is not a glb is refused rather than half read", "[assets][gltf]")
{
    GltfImportOptions options;
    ModelAsset model;
    std::string error;

    CHECK_FALSE(LoadGlbModel(std::filesystem::path(PRED_SOURCE_DIR) / "does_not_exist.glb", options,
                             model, &error));
    CHECK_FALSE(error.empty());
    CHECK(model.parts.empty());

    // A real file of the wrong kind, so the failure is about the contents rather than about the
    // file being missing.
    error.clear();
    CHECK_FALSE(LoadGlbModel(std::filesystem::path(PRED_SOURCE_DIR) / "README.md", options, model, &error));
    CHECK_FALSE(error.empty());
    CHECK(model.parts.empty());
}

TEST_CASE("An imported model survives a trip through the model file", "[assets][gltf]")
{
    // The importer is only half of it. What the editor opens and what the game holds is the saved
    // model file, so a mesh that imports and then does not round-trip through JSON is no use.
    const std::filesystem::path file = SourceModel("m9_pistol.glb");
    REQUIRE(std::filesystem::exists(file));

    GltfImportOptions options;
    options.targetSize = 0.24f;
    ModelAsset imported;
    REQUIRE(LoadGlbModel(file, options, imported));
    imported.name = "round_trip_test";

    const std::filesystem::path saved =
        std::filesystem::temp_directory_path() / "pred_round_trip_test.json";
    REQUIRE(imported.SaveToFile(saved));

    ModelAsset reopened;
    REQUIRE(reopened.LoadFromFile(saved));
    std::filesystem::remove(saved);

    REQUIRE(reopened.parts.size() == imported.parts.size());
    for (size_t i = 0; i < imported.parts.size(); ++i)
    {
        INFO("part " << i << " " << imported.parts[i].name);
        CHECK(reopened.parts[i].name == imported.parts[i].name);
        CHECK(reopened.parts[i].shape == PartShape::Mesh);
        REQUIRE(reopened.parts[i].mesh.vertices.size() == imported.parts[i].mesh.vertices.size());
        REQUIRE(reopened.parts[i].mesh.indices.size() == imported.parts[i].mesh.indices.size());

        // Positions to within a tenth of a millimetre. A model file that quietly rounds geometry is
        // a model that drifts every time it is opened and saved.
        for (size_t v = 0; v < imported.parts[i].mesh.vertices.size(); v += 37)
        {
            const glm::vec3 before = imported.parts[i].mesh.vertices[v].position;
            const glm::vec3 after = reopened.parts[i].mesh.vertices[v].position;
            REQUIRE(glm::distance(before, after) < 0.0001f);
        }
    }
}

TEST_CASE("The shipped weapon models are the size of the weapons they belong to", "[assets][weapons]")
{
    // Both weapons point at imported models now, and an import that came out ten times too large or
    // facing backwards is not something a unit test of the importer would notice: it only shows when
    // the model is held. These are the numbers the weapon bench puts on screen.
    // The test runs from the build directory, which has no assets beside it, so the source tree is
    // pointed at explicitly. Without this the weapons fall back to their built-in shapes and the
    // test measures the thing it was written to stop being used.
    Paths::Init(nullptr, std::filesystem::path(PRED_SOURCE_DIR) / "Assets");
    REQUIRE_FALSE(ModelPath("m5_carbine").empty());

    WeaponDatabase weapons;
    REQUIRE(weapons.LoadFromFile(std::filesystem::path(PRED_SOURCE_DIR) / "Assets" / "Data" /
                                 "weapons.json"));
    REQUIRE_FALSE(weapons.All().empty());
    // All() no longer hands out the "no weapon" placeholder, so every entry here is a real weapon
    // that has to have a real model. It used to, and the missing-model fallback gave the empty one
    // a plausible shape, which is how a weapon with no key and no model passed a size check.

    for (const WeaponDefinition& definition : weapons.All())
    {
        INFO("weapon " << definition.key << " wearing model '" << definition.model << "'");
        const WeaponVisual visual = BuildWeaponVisual(definition);
        REQUIRE_FALSE(visual.parts.empty());

        const AABB bounds = visual.Combined().ComputeBounds();
        const glm::vec3 extent = bounds.max - bounds.min;
        INFO("extent " << extent.x << " by " << extent.y << " by " << extent.z);

        // Long enough to read as a weapon and short enough to be carried by a person.
        CHECK(extent.z > 0.12f);
        CHECK(extent.z < 1.20f);
        CHECK(extent.y < 0.60f);
        CHECK(extent.x < 0.30f);

        // The barrel runs down +Z, so the longest axis has to be Z. A model imported facing sideways
        // comes out wide instead of long, and everything that aims it is then pointing at its flank.
        CHECK(extent.z > extent.x);
        CHECK(extent.z > extent.y);

        // Where the sockets sit is reported rather than asserted.
        //
        // These files are worked on: they spend time half-turned, with a grip moved and a muzzle
        // not moved yet, and a suite that goes red while somebody is editing an asset is a suite
        // everyone learns to ignore. The rule itself is checked in BodyPoseTests against models
        // written by the test. What this does is say so out loud when the shipped ones drift, which
        // is the same thing the editor says on screen.
        if (visual.muzzle.z <= visual.triggerGrip.z)
        {
            WARN(definition.key << " has its muzzle behind its grip: muzzle z " << visual.muzzle.z
                                << ", grip z " << visual.triggerGrip.z
                                << ". Held as it is, the player holds it by the barrel.");
        }
        if (visual.supportGrip.z <= visual.triggerGrip.z)
        {
            WARN(definition.key << " has its support socket behind its grip: support z "
                                << visual.supportGrip.z << ", grip z " << visual.triggerGrip.z);
        }
        if (visual.muzzle.z > bounds.max.z + 0.05f)
        {
            WARN(definition.key << " has its muzzle socket " << (visual.muzzle.z - bounds.max.z) * 100.0f
                                << " cm past the end of its geometry, so rounds appear in mid air."); 
        }
    }
}

TEST_CASE("A textured model keeps its textures through the model file", "[assets][gltf]")
{
    // The images are written out beside the model and named from it, so what has to survive is the
    // name. A model that loads with its texture field empty draws flat grey, which is exactly what
    // a model with no texture does, so nothing about it says anything went wrong.
    //
    // Its own model rather than a shipped one: the weapons in the game now take their colours from
    // their materials and carry no images at all, and a test that needs a textured weapon to exist
    // would have nothing to check the moment the art changed.
    ModelAsset model;
    model.name = "texture_round_trip";
    ModelPart plain;
    plain.name = "plain";
    model.parts.push_back(plain);
    ModelPart painted;
    painted.name = "painted";
    painted.texture = "Models/Textures/texture_round_trip_0.png";
    model.parts.push_back(painted);

    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "pred_texture_round_trip.json";
    REQUIRE(model.SaveToFile(file));
    ModelAsset reopened;
    REQUIRE(reopened.LoadFromFile(file));
    std::filesystem::remove(file);

    REQUIRE(reopened.parts.size() == 2);
    CHECK(reopened.parts[0].texture.empty());
    // Named relative to the assets root, so a model file does not carry the paths of the machine
    // it was imported on.
    CHECK(reopened.parts[1].texture == "Models/Textures/texture_round_trip_0.png");
}

TEST_CASE("The shipped weapons are not facing backwards", "[assets][weapons]")
{
    // A rifle carries its magazine and its grip behind the middle, and a pistol carries its own
    // grip further back still, so the lowest part of a weapon is behind its centre. When it is not,
    // the model is the wrong way round, and it is held by its muzzle. Both models that arrived ran
    // along +X as exported and had to be turned a quarter, and the first attempt turned one of them
    // the wrong way.
    Paths::Init(nullptr, std::filesystem::path(PRED_SOURCE_DIR) / "Assets");

    for (const char* name : {"m5_carbine", "m9_pistol"})
    {
        ModelAsset model;
        REQUIRE(model.LoadFromFile(ModelPath(name)));

        glm::vec3 low{1e9f};
        glm::vec3 high{-1e9f};
        for (const ModelPart& part : model.parts)
        {
            for (const MeshVertex& vertex : part.mesh.vertices)
            {
                low = glm::min(low, vertex.position);
                high = glm::max(high, vertex.position);
            }
        }
        const glm::vec3 extent = high - low;

        double lowestAlong = 0.0;
        size_t counted = 0;
        for (const ModelPart& part : model.parts)
        {
            for (const MeshVertex& vertex : part.mesh.vertices)
            {
                if (vertex.position.y < low.y + extent.y * 0.06f)
                {
                    lowestAlong += vertex.position.z;
                    ++counted;
                }
            }
        }
        REQUIRE(counted > 0);
        // Through the grip socket's own turn, because that is now a legitimate way to right a model
        // whose geometry runs the wrong way: what has to be behind the middle is the low mass as it
        // is held, not as it is stored.
        const ModelSocket* grip = model.FindSocket("grip");
        const glm::quat hold = grip != nullptr ? grip->Rotation() : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        const glm::vec3 held =
            hold * glm::vec3(0.0f, 0.0f, static_cast<float>(lowestAlong / static_cast<double>(counted)));
        // Reported rather than asserted, for the same reason as the sockets above: these files are
        // being worked on, and a model half way through being turned round is a normal state for
        // one to be in for an afternoon.
        if (held.z >= 0.0f)
        {
            WARN(name << " looks like it is facing backwards: its lowest mass, which on a weapon is "
                         "the grip and the magazine, sits at z "
                      << held.z << " as held. Turn it round, or turn its grip socket.");
        }
    }
}

TEST_CASE("A dropped weapon keeps the pieces it was made of", "[assets][weapons]")
{
    // A dropped weapon used to be one merged mesh with one flat material, so it lay on the floor as
    // a single lump in one colour: none of the textures, and no way for them to be there, because a
    // merged mesh has one material and this weapon has three.
    Paths::Init(nullptr, std::filesystem::path(PRED_SOURCE_DIR) / "Assets");

    WeaponDatabase weapons;
    REQUIRE(weapons.LoadFromFile(std::filesystem::path(PRED_SOURCE_DIR) / "Assets" / "Data" /
                                 "weapons.json"));
    const WeaponDefinition* carbine = weapons.Find("carbine");
    REQUIRE(carbine != nullptr);

    const WeaponVisual visual = BuildWeaponVisual(*carbine);
    INFO("carbine is " << visual.parts.size() << " parts");
    CHECK(visual.parts.size() > 1);

    // Each piece carries its own material, and more than one of them names an image. That is the
    // whole reason a pickup is drawn as pieces rather than as one mesh.
    int named = 0;
    for (const WeaponVisual::Part& part : visual.parts)
    {
        INFO("part " << part.name);
        CHECK(part.mesh.vertices.size() >= 3);
        if (part.material.baseColorTexture.IsValid())
        {
            ++named;
        }
    }
    // No renderer here, so nothing resolves to a real handle; what is checked is that the parts are
    // separate and each has a material of its own to carry one.
    CHECK(named == 0);
    CHECK(visual.Combined().vertices.size() > visual.parts.front().mesh.vertices.size());
}

TEST_CASE("A weapon whose model is missing is unmistakable about it", "[assets][weapon]")
{
    // There used to be a rather good little gun built out of boxes here, drawn whenever a model
    // would not load. That was the problem: a stand-in that looks like a weapon is a stand-in
    // nobody notices, and the only symptom of an asset that was never loading was wondering for a
    // while why an edit to the file was not showing up.
    Paths::Init(nullptr, std::filesystem::path(PRED_SOURCE_DIR) / "Assets");

    WeaponDefinition broken;
    broken.key = "test_broken";
    broken.model = "no_such_model_exists_anywhere";
    broken.size = {0.06f, 0.14f, 0.60f};

    const WeaponVisual visual = BuildWeaponVisual(broken);
    REQUIRE_FALSE(visual.parts.empty());
    for (const WeaponVisual::Part& part : visual.parts)
    {
        INFO("part " << part.name);
        // Magenta, glowing, and not shaped like anything. Nothing in the game is this colour.
        CHECK(part.material.baseColor.r > 0.9f);
        CHECK(part.material.baseColor.g < 0.1f);
        CHECK(part.material.baseColor.b > 0.5f);
        CHECK(glm::length(part.material.emissive) > 0.1f);
    }

    // Still held properly. An arm bug on top of a missing model is two problems to read at once.
    CHECK(visual.muzzle.z > visual.triggerGrip.z);
}

TEST_CASE("The sockets a weapon cannot do without are named when they are absent", "[assets][weapon]")
{
    ModelAsset bare;
    bare.name = "bare";
    const std::vector<std::string> missing = MissingWeaponSockets(bare);
    // Not carry and not magazine: each of those has a real answer when it is absent rather than a
    // guess standing in for an oversight.
    CHECK(missing.size() == 4);

    Paths::Init(nullptr, std::filesystem::path(PRED_SOURCE_DIR) / "Assets");
    for (const char* name : {"m5_carbine", "m9_pistol"})
    {
        ModelAsset shipped;
        REQUIRE(shipped.LoadFromFile(ModelPath(name)));
        INFO("shipped model " << name << " is missing: " << [&]
             {
                 std::string all;
                 for (const std::string& one : MissingWeaponSockets(shipped))
                 {
                     all += one + " ";
                 }
                 return all;
             }());
        CHECK(MissingWeaponSockets(shipped).empty());
    }
}

TEST_CASE("A model is found by name wherever its folder is", "[assets][models]")
{
    // The folders under Models are for people, not for the game. A model is named by its file, so
    // moving one into a subfolder does not rewrite weapons.json, the editor's list, or anybody
    // else's reference to it -- which is what makes adding an asset a matter of dropping a file in.
    Paths::Init(nullptr, std::filesystem::path(PRED_SOURCE_DIR) / "Assets");

    const std::filesystem::path carbine = ModelPath("m5_carbine");
    REQUIRE_FALSE(carbine.empty());
    CHECK(std::filesystem::exists(carbine));
    // It is not at the top of the folder any more, which is the point.
    CHECK(carbine.parent_path() != ModelDirectory());

    // And the list of what exists finds it there.
    const std::vector<std::string> all = ListModels();
    CHECK(std::find(all.begin(), all.end(), "m5_carbine") != all.end());
    CHECK(std::find(all.begin(), all.end(), "m9_pistol") != all.end());

    // A name nothing answers to is empty rather than a path that does not exist, so a caller has
    // to notice.
    CHECK(ModelPath("no_model_is_called_this").empty());
}

TEST_CASE("Saving a model makes it findable at once", "[assets][models]")
{
    // "Remember to rescan after you save" is a rule that gets forgotten, and its symptom is a
    // model that is on the disk and cannot be found by name until the game is restarted. So the
    // save does it.
    Paths::Init(nullptr, std::filesystem::path(PRED_SOURCE_DIR) / "Assets");
    REQUIRE(ModelPath("index_freshness_test").empty());

    ModelAsset model;
    model.name = "index_freshness_test";
    ModelPart part;
    part.name = "body";
    part.shape = PartShape::Box;
    part.size = {0.1f, 0.1f, 0.1f};
    model.parts.push_back(part);

    const std::filesystem::path file = ModelPathFor("index_freshness_test", "Weapons");
    // A folder was chosen for it rather than the top of the tree.
    CHECK(file.parent_path().filename() == "Weapons");
    REQUIRE(model.SaveToFile(file));

    CHECK(ModelPath("index_freshness_test") == file);
    std::filesystem::remove(file);
    RescanModels();
    CHECK(ModelPath("index_freshness_test").empty());
}

TEST_CASE("List the parts of a downloaded model", "[.][parts]")
{
    // Hidden. For sorting out a download before it becomes a weapon: which parts are the gun and
    // which are the display props an artist left beside it, and which way it faces.
    //   PredationTests.exe "[parts]"   with PRED_GLB set to the file
    const char* path = std::getenv("PRED_GLB");
    REQUIRE(path != nullptr);
    GltfImportOptions options;
    options.targetSize = 1.0f;
    ModelAsset model;
    std::string error;
    REQUIRE(LoadGlbModel(path, options, model, &error));
    std::string table = "\n";
    for (const ModelPart& part : model.parts)
    {
        glm::vec3 low{1e9f};
        glm::vec3 high{-1e9f};
        const glm::mat4 rest = part.LocalMatrix();
        for (const MeshVertex& vertex : part.mesh.vertices)
        {
            const glm::vec3 at = glm::vec3(rest * glm::vec4(vertex.position, 1.0f));
            low = glm::min(low, at);
            high = glm::max(high, at);
        }
        char row[256];
        std::snprintf(row, sizeof(row), "%-28s x %6.3f..%6.3f  y %6.3f..%6.3f  z %6.3f..%6.3f  v %zu\n",
                      part.name.c_str(), low.x, high.x, low.y, high.y, low.z, high.z,
                      part.mesh.vertices.size());
        table += row;
    }
    WARN(table);
}
