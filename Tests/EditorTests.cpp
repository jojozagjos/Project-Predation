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

TEST_CASE("Undo puts back what an edit changed", "[editor][undo]")
{
    // An editor without undo is one nobody dares experiment in, and the fastest way to make an undo
    // stack useless is to fill it with entries that undo nothing.
    ModelEditor editor = MakeEditorWithParts();
    const size_t before = editor.Model().parts.size();

    REQUIRE_FALSE(editor.CanUndo());
    REQUIRE_FALSE(editor.CanRedo());

    editor.PushUndo("a turn");
    editor.TransformModel(glm::translate(glm::mat4(1.0f), {0.0f, 1.0f, 0.0f}));
    CHECK(editor.Model().parts[0].position.y == Catch::Approx(1.0f).margin(0.001));

    // TransformModel records its own, so there are two entries and undoing twice is right.
    REQUIRE(editor.CanUndo());
    REQUIRE(editor.Undo());
    CHECK(editor.Model().parts[0].position.y == Catch::Approx(0.0f).margin(0.001));
    CHECK(editor.Model().parts.size() == before);

    // And forward again.
    REQUIRE(editor.CanRedo());
    REQUIRE(editor.Redo());
    CHECK(editor.Model().parts[0].position.y == Catch::Approx(1.0f).margin(0.001));
}

TEST_CASE("Undo has a floor and a ceiling", "[editor][undo]")
{
    ModelEditor editor = MakeEditorWithParts();

    // Undoing with nothing to undo is refused rather than doing something surprising.
    CHECK_FALSE(editor.Undo());
    CHECK_FALSE(editor.Redo());

    // The stack is bounded, because a copy of a model with imported geometry is megabytes and an
    // editor that grows without limit is one that eventually stops.
    for (int i = 0; i < 60; ++i)
    {
        editor.PushUndo("a change");
    }
    CHECK(editor.UndoDepth() <= 24);

    // And doing something new is what makes the way forward stop existing.
    REQUIRE(editor.Undo());
    REQUIRE(editor.CanRedo());
    editor.PushUndo("something else");
    CHECK_FALSE(editor.CanRedo());
}

TEST_CASE("A socket can be selected and dragged along an axis", "[editor]")
{
    // Moving a grip is the single most common thing anyone does in here, and it could only be done
    // by typing numbers. The handles are what make it a drag.
    ModelEditor editor = MakeEditorWithParts();
    const glm::vec3 socketAt = editor.Model().sockets[0].position;

    // Straight at the socket from in front. Sockets win ties against parts, because they sit on the
    // surface of one and would otherwise be unclickable.
    REQUIRE(editor.SelectUnderRay({socketAt.x, socketAt.y, socketAt.z + 1.0f}, {0.0f, 0.0f, -1.0f}));
    glm::vec3 selected{0.0f};
    REQUIRE(editor.SelectionPosition(selected));
    CHECK(glm::distance(selected, socketAt) < 0.001f);

    // Grab the X handle, which runs from the socket towards +X. Aimed at its middle from above.
    const glm::vec3 grabAt = socketAt + glm::vec3(ModelEditor::kHandleLength * 0.5f, 0.0f, 0.0f);
    REQUIRE(editor.BeginDrag(grabAt + glm::vec3(0.0f, 1.0f, 0.0f), {0.0f, -1.0f, 0.0f}));
    CHECK(editor.Dragging());

    // Move the pointer ray along +X and the socket follows, keeping the grabbed point under it.
    editor.UpdateDrag(grabAt + glm::vec3(0.2f, 1.0f, 0.0f), {0.0f, -1.0f, 0.0f});
    CHECK(editor.Model().sockets[0].position.x == Catch::Approx(socketAt.x + 0.2f).margin(0.005));
    // And only along that axis.
    CHECK(editor.Model().sockets[0].position.y == Catch::Approx(socketAt.y).margin(0.001));
    CHECK(editor.Model().sockets[0].position.z == Catch::Approx(socketAt.z).margin(0.001));

    editor.EndDrag();
    CHECK_FALSE(editor.Dragging());

    // The drag recorded itself, so it can be taken back.
    REQUIRE(editor.CanUndo());
    REQUIRE(editor.Undo());
    CHECK(editor.Model().sockets[0].position.x == Catch::Approx(socketAt.x).margin(0.001));
}

TEST_CASE("A click away from any handle selects instead of dragging", "[editor]")
{
    // The handles sit on top of the thing they move, so a click on one is never a click on it. A
    // click anywhere else has to fall through to selection or nothing can be picked at all.
    ModelEditor editor = MakeEditorWithParts();
    REQUIRE(editor.SelectUnderRay({0.0f, 0.0f, 2.0f}, {0.0f, 0.0f, -1.0f}));

    // Well off to the side of everything.
    CHECK_FALSE(editor.BeginDrag({3.0f, 3.0f, 3.0f}, glm::normalize(glm::vec3(0.0f, -1.0f, 0.0f))));
    CHECK_FALSE(editor.Dragging());
}

TEST_CASE("The selection outline follows the part rather than its nominal size", "[editor]")
{
    // An imported mesh keeps its size at one, which is not its size. Outlining by that drew a metre
    // of box around six centimetres of barrel, and a selection you cannot see the edges of is one
    // that does not tell you what is selected.
    ModelPart imported;
    imported.name = "imported";
    imported.shape = PartShape::Mesh;
    imported.size = glm::vec3(1.0f);
    for (const glm::vec3 corner : {glm::vec3(-0.03f, -0.02f, -0.2f), glm::vec3(0.03f, 0.02f, 0.2f),
                                   glm::vec3(0.0f, 0.0f, 0.0f)})
    {
        MeshVertex vertex;
        vertex.position = corner;
        imported.mesh.vertices.push_back(vertex);
    }
    imported.mesh.indices = {0, 1, 2};

    const AABB bounds = ModelEditor::PartBounds(imported);
    const glm::vec3 extent = bounds.max - bounds.min;
    INFO("outline came out " << extent.x << " by " << extent.y << " by " << extent.z);
    CHECK(extent.x == Catch::Approx(0.06f).margin(0.001));
    CHECK(extent.y == Catch::Approx(0.04f).margin(0.001));
    CHECK(extent.z == Catch::Approx(0.4f).margin(0.001));

    // A primitive still uses its own size, which for a primitive really is its size.
    ModelPart box;
    box.shape = PartShape::Box;
    box.size = {0.2f, 0.3f, 0.4f};
    const AABB boxBounds = ModelEditor::PartBounds(box);
    CHECK((boxBounds.max - boxBounds.min).y == Catch::Approx(0.3f).margin(0.001));
}
