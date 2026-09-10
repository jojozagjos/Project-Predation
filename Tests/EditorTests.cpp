#include "Engine/Assets/ModelAsset.h"
#include "Tools/ModelEditor/ModelEditor.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

using namespace pred;

// The parts of the editor that are arithmetic rather than interface. Picking and the whole-model
// transform are both things that are wrong in ways nobody notices by looking: a pick that is off by
// a part selects the wrong thing, and a transform that misses the sockets moves the model out from
// under its own grip.
namespace
{

ModelEditor MakeEditorWithParts()
{
    ModelEditor editor;
    ModelAsset model;
    model.name = "picking";

    // Three boxes in a row down +Z, a quarter of a metre apart.
    for (int i = 0; i < 3; ++i)
    {
        ModelPart part;
        part.name = "part" + std::to_string(i);
        part.shape = PartShape::Box;
        part.size = glm::vec3(0.1f);
        part.position = {0.0f, 0.0f, static_cast<float>(i) * 0.25f};
        model.parts.push_back(part);
    }

    ModelSocket grip;
    grip.name = "grip";
    grip.position = {0.0f, 0.0f, 0.25f};
    model.sockets.push_back(grip);

    editor.SetModelForTesting(std::move(model));
    return editor;
}

} // namespace

TEST_CASE("Clicking a part in the viewport selects that part", "[editor]")
{
    ModelEditor editor = MakeEditorWithParts();

    // Straight down the row from in front. The nearest box is the one that gets picked, because
    // picking the front-most thing is what clicking means.
    CHECK(editor.PartUnderRay({0.0f, 0.0f, 2.0f}, {0.0f, 0.0f, -1.0f}) == 2);

    // From behind, the far end of the row is nearest, so the other end is picked.
    CHECK(editor.PartUnderRay({0.0f, 0.0f, -2.0f}, {0.0f, 0.0f, 1.0f}) == 0);

    // From the side, aimed at the middle box only.
    CHECK(editor.PartUnderRay({2.0f, 0.0f, 0.25f}, {-1.0f, 0.0f, 0.0f}) == 1);

    // Past everything.
    CHECK(editor.PartUnderRay({2.0f, 0.0f, 5.0f}, {-1.0f, 0.0f, 0.0f}) == -1);

    // And a ray pointing away from the model finds nothing, rather than finding it behind the eye.
    CHECK(editor.PartUnderRay({0.0f, 0.0f, 2.0f}, {0.0f, 0.0f, 1.0f}) == -1);
}

TEST_CASE("A hidden part cannot be clicked", "[editor]")
{
    // You cannot click what you cannot see, and a hidden part is usually hidden because it is in
    // the way.
    ModelEditor editor = MakeEditorWithParts();
    REQUIRE(editor.PartUnderRay({0.0f, 0.0f, 2.0f}, {0.0f, 0.0f, -1.0f}) == 2);

    ModelAsset model = editor.Model();
    model.parts[2].visible = false;
    editor.SetModelForTesting(std::move(model));

    CHECK(editor.PartUnderRay({0.0f, 0.0f, 2.0f}, {0.0f, 0.0f, -1.0f}) == 1);
}

TEST_CASE("Turning the model takes the sockets with it", "[editor]")
{
    // A transform that misses the sockets moves the model out from under its own grip, and the
    // hand then holds a point in the air beside it. This is the whole reason the operation exists
    // at all: a download arrives facing the wrong way and everything has to turn together.
    ModelEditor editor = MakeEditorWithParts();

    editor.TransformModel(glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), {0.0f, 1.0f, 0.0f}));

    const ModelAsset& turned = editor.Model();
    REQUIRE(turned.sockets.size() == 1);
    // A quarter turn left about Y sends +Z off along +X.
    CHECK(turned.sockets[0].position.x == Catch::Approx(0.25f).margin(0.001));
    CHECK(std::abs(turned.sockets[0].position.z) < 0.001f);

    // And the parts went with it, so the socket is still on the part it was on.
    CHECK(turned.parts[1].position.x == Catch::Approx(0.25f).margin(0.001));

    // Picking follows, because the parts really moved rather than being drawn somewhere else.
    CHECK(editor.PartUnderRay({2.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}) == 2);
}

TEST_CASE("Rescaling the model rescales what is on it", "[editor]")
{
    ModelEditor editor = MakeEditorWithParts();
    editor.TransformModel(glm::scale(glm::mat4(1.0f), glm::vec3(2.0f)));

    const ModelAsset& scaled = editor.Model();
    CHECK(scaled.sockets[0].position.z == Catch::Approx(0.5f).margin(0.001));
    CHECK(scaled.parts[2].position.z == Catch::Approx(1.0f).margin(0.001));
}
