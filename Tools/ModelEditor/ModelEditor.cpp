#include "Tools/ModelEditor/ModelEditor.h"

#include "Engine/Application.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Engine/Render/DebugDraw.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Assets/MeshImport.h"
#include "Engine/Render/Primitives.h"

#include <imgui.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <utility>
#include <cstdio>
#include <filesystem>
#include <limits>

namespace pred
{
namespace
{

// Small helper so every vector field in the editor looks and behaves the same.
bool DragVec3(const char* label, glm::vec3& value, float speed, const char* format = "%.3f")
{
    return ImGui::DragFloat3(label, glm::value_ptr(value), speed, 0.0f, 0.0f, format);
}

float SnapTo(float value, float step)
{
    return step > 1e-6f ? std::round(value / step) * step : value;
}

} // namespace

void ModelEditor::Init(Application& app)
{
    m_app = &app;
    m_camera.position = {0.55f, 0.35f, 0.75f};
    m_camera.yaw = glm::radians(-36.0f);
    m_camera.pitch = glm::radians(-18.0f);
    m_camera.moveSpeed = 1.4f;
    NewModel();
}

void ModelEditor::Shutdown(Scene& scene)
{
    for (Entity entity : m_entities)
    {
        scene.Destroy(entity);
    }
    for (Entity entity : m_socketEntities)
    {
        scene.Destroy(entity);
    }
    m_entities.clear();
    m_socketEntities.clear();
}

void ModelEditor::SetOpen(Scene& scene, bool open)
{
    if (open == m_open)
    {
        return;
    }
    m_open = open;
    if (open)
    {
        m_dirty = true;
    m_previewChanged = true;
    m_geometryChanged = true;
    }
    else
    {
        Shutdown(scene);
    }
}

void ModelEditor::NewModel()
{
    m_model = ModelAsset{};
    m_model.name = "new_model";
    m_saveName = "new_model";
    m_selectedPart = -1;
    m_selectedSocket = -1;
    m_selectedClip = -1;
    m_dirty = true;
    m_previewChanged = true;
    m_geometryChanged = true;
    AddPart("body", PartShape::Box);
}

void ModelEditor::AddPart(const char* name, PartShape shape)
{
    PushUndo("a new part");
    ModelPart part;
    part.name = name;
    // Names have to be unique, because animation tracks address parts by name.
    int suffix = 1;
    while (std::any_of(m_model.parts.begin(), m_model.parts.end(),
                       [&](const ModelPart& other) { return other.name == part.name; }))
    {
        part.name = std::string(name) + "_" + std::to_string(++suffix);
    }
    part.shape = shape;
    // Sitting on the grid rather than half buried in it. A new box centred on the origin has half
    // its height below the floor, which is a poor first impression of a modelling tool.
    part.position.y = part.size.y * 0.5f;
    m_model.parts.push_back(std::move(part));
    m_selectedPart = static_cast<int>(m_model.parts.size()) - 1;
    m_pick = Pick::Part;
    m_dirty = true;
    m_previewChanged = true;
    m_geometryChanged = true;
}

bool ModelEditor::Load(const std::string& modelName)
{
    ModelAsset loaded;
    const std::filesystem::path file = ModelPath(modelName);
    if (file.empty() || !loaded.LoadFromFile(file))
    {
        m_status = "Could not load " + modelName;
        return false;
    }
    m_model = std::move(loaded);
    m_saveName = modelName;
    m_frameRequested = true;
    m_selectedPart = m_model.parts.empty() ? -1 : 0;
    m_pick = m_model.parts.empty() ? Pick::None : Pick::Part;
    m_selectedSocket = -1;
    m_selectedClip = m_model.clips.empty() ? -1 : 0;
    m_dirty = true;
    m_previewChanged = true;
    m_geometryChanged = true;
    m_status = "Loaded " + modelName;
    return true;
}

bool ModelEditor::Save()
{
    m_model.name = m_saveName;
    // Rewritten where it already is, or put in the folder the editor is filing new models under.
    if (!m_model.SaveToFile(ModelPathFor(m_saveName, m_folder)))
    {
        m_status = "Could not save " + m_saveName;
        return false;
    }
    // So a model saved under a new name is findable by name at once, without a restart.
    RescanModels();
    m_status = "Saved " + m_saveName;
    return true;
}

AnimationClip* ModelEditor::CurrentClip()
{
    if (m_selectedClip < 0 || m_selectedClip >= static_cast<int>(m_model.clips.size()))
    {
        return nullptr;
    }
    return &m_model.clips[static_cast<size_t>(m_selectedClip)];
}

void ModelEditor::Rebuild(Scene& scene, MeshLibrary& meshes)
{
    for (Entity entity : m_entities)
    {
        scene.Destroy(entity);
    }
    for (Entity entity : m_socketEntities)
    {
        scene.Destroy(entity);
    }
    m_entities.clear();
    m_socketEntities.clear();

    // One entity per part. Baking the whole model into a single mesh would be cheaper to draw, but
    // then a part could not be hidden, animated or picked out on its own.
    for (const ModelPart& part : m_model.parts)
    {
        Material material;
        material.baseColor = part.color;
        material.roughness = part.roughness;
        material.metallic = part.metallic;
        material.emissive = part.color * part.emissive;
        // The image the part names, if it has one. Read through the same library the game uses, so
        // what the editor shows is what the game draws rather than a second interpretation of it.
        if (m_app != nullptr && !part.texture.empty())
        {
            if (const auto resolved = Paths::Resolve(part.texture))
            {
                material.baseColorTexture =
                    m_app->GetTextures().LoadFromFile(resolved->string(), part.texture);
            }
        }
        const MeshHandle mesh =
            meshes.Upload(m_model.BuildPartMesh(part), "editor_" + m_model.name + "_" + part.name);
        m_entities.push_back(scene.CreateMeshEntity("editor_part", Transform{}, mesh, material));
    }

    if (m_showSockets)
    {
        const MeshHandle socketMesh = meshes.Upload(Primitives::Sphere(0.012f, 10, 8), "editor_socket");
        const Material socketMaterial = Material::Emissive({0.95f, 0.72f, 0.25f}, 2.4f);
        for (size_t i = 0; i < m_model.sockets.size(); ++i)
        {
            m_socketEntities.push_back(
                scene.CreateMeshEntity("editor_socket", Transform{}, socketMesh, socketMaterial));
        }
    }
    m_dirty = false;
}

void ModelEditor::Update(Scene& scene, MeshLibrary& meshes, float dt)
{
    if (!m_open)
    {
        return;
    }

    // One undo entry per gesture. A widget only says it changed something after changing it, so the
    // copy is taken when any widget is grabbed and kept until it is let go; if nothing about the
    // model differs by then, the gesture is discarded rather than filling the stack with entries
    // that undo nothing.
    {
        const bool holding = ImGui::IsAnyItemActive();
        if (holding && !m_gestureActive)
        {
            m_gestureStart = m_model;
            m_gestureFingerprint = Fingerprint();
        }
        else if (!holding && m_gestureActive)
        {
            if (Fingerprint() != m_gestureFingerprint)
            {
                PushSnapshot(std::move(m_gestureStart), "a change");
            }
            m_gestureStart = ModelAsset{};
        }
        m_gestureActive = holding;
    }

    if (m_dirty)
    {
        Rebuild(scene, meshes);
    }

    // Anything about the model changed this frame -- a key typed into, a key dragged -- and whatever is
    // holding it in the first-person preview is told, so it plays what is on the timeline now. Cheap:
    // the fingerprint reads numbers, not meshes.
    if (const size_t fingerprint = Fingerprint(); fingerprint != m_lastFingerprint)
    {
        m_lastFingerprint = fingerprint;
        m_previewChanged = true;
    }

    const AnimationClip* clip = m_selectedClip >= 0 && m_selectedClip < static_cast<int>(m_model.clips.size())
                                    ? &m_model.clips[static_cast<size_t>(m_selectedClip)]
                                    : nullptr;
    if (m_playing && clip != nullptr)
    {
        m_playhead += dt;
        if (m_playhead > clip->duration)
        {
            m_playhead = clip->loop ? 0.0f : clip->duration;
            m_playing = clip->loop;
        }
    }

    // Parts are placed from the clip when one is selected, so scrubbing the timeline moves the model
    // exactly as it will move in the game.
    for (size_t i = 0; i < m_model.parts.size() && i < m_entities.size(); ++i)
    {
        const ModelPart& part = m_model.parts[i];
        float visibility = 1.0f;
        const glm::mat4 local = m_model.PartMatrixAt(part, clip, m_playhead, &visibility);
        if (Transform* transform = scene.GetTransform(m_entities[i]))
        {
            transform->position = glm::vec3(local[3]);
            transform->rotation = glm::quat_cast(glm::mat3(local));
            transform->scale = glm::vec3(visibility > 0.5f ? 1.0f : 0.0f);
        }
    }

    for (size_t i = 0; i < m_model.sockets.size() && i < m_socketEntities.size(); ++i)
    {
        if (Transform* transform = scene.GetTransform(m_socketEntities[i]))
        {
            transform->position = m_model.sockets[i].position;
        }
    }
}

void ModelEditor::DrawOverlays(DebugDraw& draw) const
{
    if (!m_open)
    {
        return;
    }
    // A ten centimetre grid on the ground plane and the model's own axes, so sizes can be judged
    // against something rather than guessed.
    draw.Grid(0.5f, 0.05f, 0.0f);
    draw.Axes(glm::mat4(1.0f), 0.25f);

    // Around what the part actually occupies. An imported mesh keeps its size at one, which is not
    // its size: outlining by that drew a metre of box around six centimetres of barrel.
    if (m_pick == Pick::Part && m_selectedPart >= 0 &&
        m_selectedPart < static_cast<int>(m_model.parts.size()))
    {
        const ModelPart& part = m_model.parts[static_cast<size_t>(m_selectedPart)];
        const AABB bounds = PartBounds(part);
        const glm::vec3 centre = (bounds.min + bounds.max) * 0.5f;
        const glm::vec3 half = glm::max((bounds.max - bounds.min) * 0.5f, glm::vec3(0.002f));
        // Where it is at the playhead when a clip is open, so the outline stays on the part as it moves.
        const AnimationClip* playing = SelectedClip();
        const glm::mat4 placed = playing != nullptr ? m_model.PartMatrixAt(part, playing, m_playhead) : part.LocalMatrix();
        draw.BoxOriented(placed * glm::translate(glm::mat4(1.0f), centre), half, Color::kYellow);
    }

    for (size_t i = 0; i < m_model.sockets.size(); ++i)
    {
        const ModelSocket& socket = m_model.sockets[i];
        const bool selected = m_pick == Pick::Socket && static_cast<int>(i) == m_selectedSocket;
        draw.Axes(glm::translate(glm::mat4(1.0f), socket.position), selected ? 0.09f : 0.045f);
        if (selected)
        {
            draw.Sphere(socket.position, 0.022f, Color::kYellow, 10);
        }
    }

    // The hands, where the clip puts them at the playhead: a box for each palm and its axes, so a
    // reload can be posed against the weapon without switching to the first-person view. Left is
    // blue and right is orange, and a hand is only drawn when the clip moves it.
    if (const AnimationClip* clip = SelectedClip())
    {
        for (int side = 0; side < 2; ++side)
        {
            glm::vec3 offset{0.0f};
            glm::quat turn{1.0f, 0.0f, 0.0f, 0.0f};
            if (!m_model.HandAt(clip, side, m_playhead, offset, turn))
            {
                continue;
            }
            const glm::mat4 hand = m_model.HandFrameAt(clip, side, m_playhead, m_model.HandRest(side));
            const uint32_t colour = side == 0 ? Color::RGBA(110, 170, 240, 255) : Color::RGBA(240, 160, 90, 255);
            draw.BoxOriented(hand, glm::vec3(0.022f, 0.045f, 0.05f), colour);
            draw.Axes(hand, 0.05f);
            // And a line back to the socket it rests on, so where it has gone from is never a guess.
            draw.Line(m_model.HandRest(side), glm::vec3(hand[3]), colour);
        }
    }

    // The move handles. Three lines from whatever is selected, coloured the way axes are coloured
    // everywhere, with the one being dragged lit. Drawn last so they are on top of what they move.
    glm::vec3 at{0.0f};
    if (SelectionPosition(at))
    {
        const uint32_t colours[3] = {Color::kRed, Color::kGreen, Color::kBlue};
        for (int axis = 0; axis < 3; ++axis)
        {
            glm::vec3 unit{0.0f};
            unit[axis] = 1.0f;
            const uint32_t colour = axis == m_dragAxis ? Color::kYellow : colours[axis];
            draw.Line(at, at + unit * kHandleLength, colour);
            // A head on the end, so there is something with size to aim at rather than a line.
            draw.Sphere(at + unit * kHandleLength, 0.009f, colour, 8);
        }
        // And the turning rings, one round each axis in the same colours.
        for (int axis = 0; axis < 3; ++axis)
        {
            const uint32_t colour = axis + 3 == m_dragAxis ? Color::kYellow : colours[axis];
            glm::vec3 u{0.0f};
            glm::vec3 v{0.0f};
            u[(axis + 1) % 3] = 1.0f;
            v[(axis + 2) % 3] = 1.0f;
            constexpr int kSegments = 40;
            glm::vec3 last = at + u * kRingRadius;
            for (int i = 1; i <= kSegments; ++i)
            {
                const float angle = glm::two_pi<float>() * static_cast<float>(i) / static_cast<float>(kSegments);
                const glm::vec3 next = at + (u * std::cos(angle) + v * std::sin(angle)) * kRingRadius;
                draw.Line(last, next, colour);
                last = next;
            }
        }
    }
}

void ModelEditor::DrawFilePanel(Scene& scene, MeshLibrary& meshes)
{
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%s", m_saveName.c_str());
    if (ImGui::InputText("Name", buffer, sizeof(buffer)))
    {
        m_saveName = buffer;
    }

    // Which folder a new model is filed under. One that already exists is rewritten where it is,
    // so this only decides where new ones land -- moving somebody's file because they saved it is
    // the kind of helpfulness nobody asked for.
    char folderBuffer[64];
    std::snprintf(folderBuffer, sizeof(folderBuffer), "%s", m_folder.c_str());
    if (ImGui::InputText("Folder", folderBuffer, sizeof(folderBuffer)))
    {
        m_folder = folderBuffer;
    }
    ImGui::SetItemTooltip("%s", "Under Assets/Models. A model is found by its name whatever folder "
                                "it is in, so this is only about keeping the tree tidy.");

    // Undo where it can be seen, as well as on the keys. A tool whose undo is invisible is one
    // people do not trust enough to experiment in, which is most of what an editor is for.
    ImGui::BeginDisabled(!CanUndo());
    if (ImGui::Button("Undo"))
    {
        Undo();
    }
    ImGui::EndDisabled();
    if (CanUndo() && ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Ctrl+Z: %s", UndoName());
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!CanRedo());
    if (ImGui::Button("Redo"))
    {
        Redo();
    }
    ImGui::EndDisabled();
    if (CanRedo() && ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Ctrl+Y: %s", RedoName());
    }
    ImGui::SameLine();
    ImGui::TextDisabled(UndoDepth() == 1 ? "1 step back" : "%zu steps back", UndoDepth());

    // What the keys and the mouse do, on screen. An editor whose controls have to be guessed at is
    // one where the first ten minutes are spent finding out it can do anything at all.
    if (ImGui::TreeNode("Controls"))
    {
        // Aligned columns, so this one block does not wrap.
        ImGui::PushTextWrapPos(-1.0f);
        ImGui::TextDisabled("WASD           move the view    Q and E  down and up");
        ImGui::TextDisabled("Right mouse    look around");
        ImGui::TextDisabled("Left click     select a part, or a socket if one is under it");
        ImGui::TextDisabled("Drag a handle  move the selected thing along that axis");
        ImGui::TextDisabled("Ctrl+Z         undo      Ctrl+Y  redo");
        ImGui::TextDisabled("F              put the view back on the model");
        ImGui::TextDisabled("Escape         back to the menu");
        ImGui::PopTextWrapPos();
        ImGui::TreePop();
    }

    if (ImGui::Button("Save"))
    {
        Save();
    }
    ImGui::SameLine();
    if (ImGui::Button("New"))
    {
        NewModel();
        Rebuild(scene, meshes);
    }

    if (ImGui::BeginCombo("Open", "choose a model"))
    {
        for (const std::string& modelName : ListModels())
        {
            if (ImGui::Selectable(modelName.c_str()))
            {
                Load(modelName);
                Rebuild(scene, meshes);
            }
        }
        ImGui::EndCombo();
    }

    // Dropping the file on the window, which is the whole of importing for the common case.
    // Pasting a path still works and is now the fallback rather than the only way.
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.85f, 0.95f, 1.0f));
    ImGui::TextUnformatted("Drag an OBJ or GLB onto this window to import it.");
    ImGui::PopStyleColor();
    ImGui::TextDisabled("The file is copied into Assets/Models/Source, so the import can be "
                        "repeated without going to find the download again.");
    if (!m_lastImportSource.empty())
    {
        ImGui::TextDisabled("Kept at %s", m_lastImportSource.c_str());
    }

    if (ImGui::TreeNode("Import from a path instead"))
    {
        char importBuffer[512];
        std::snprintf(importBuffer, sizeof(importBuffer), "%s", m_importPath.c_str());
        if (ImGui::InputText("Path", importBuffer, sizeof(importBuffer)))
        {
            m_importPath = importBuffer;
        }
        ImGui::SameLine();
        if (ImGui::Button("Import"))
        {
            ImportMesh(m_importPath);
        }
        ImGui::TreePop();
    }

    // How a download is turned into something the game can hold. All four are wanted before the
    // first look rather than after it: a model that arrives a hundred times too large and facing
    // sideways is indistinguishable from one that failed to load.
    ImGui::DragFloat("Fit to (m)", &m_importSize, 0.01f, 0.05f, 4.0f, "%.2f");
    ImGui::DragFloat3("Turn (deg)", &m_importRotation.x, 1.0f, -180.0f, 180.0f, "%.0f");
    ImGui::Checkbox("Centre on import", &m_importCentre);
    ImGui::SameLine();
    ImGui::Checkbox("Replace parts", &m_importReplace);
    ImGui::TextDisabled("The game wants the barrel down +Z and the grip at the origin. Replacing "
                        "keeps sockets and clips, so turning it right takes a few goes.");

    if (!m_status.empty())
    {
        ImGui::TextColored({0.75f, 0.82f, 0.68f, 1.0f}, "%s", m_status.c_str());
    }
}

bool ModelEditor::IsInside(const std::string& part, const std::string& group) const
{
    // Whether `part` is `group` or moves with it, however many steps down: what may not become its
    // parent, or the two would each move with the other.
    std::string at = part;
    for (int depth = 0; depth < 16 && !at.empty(); ++depth)
    {
        if (at == group)
        {
            return true;
        }
        const ModelPart* found = m_model.FindPart(at);
        at = found != nullptr ? found->parent : std::string();
    }
    return false;
}

void ModelEditor::RenamePart(ModelPart& part, const std::string& name)
{
    // Everything that names it by name follows the new one: the parts that move with it, and its
    // tracks in every clip. A rename used to leave the animation behind on the old name.
    const std::string old = part.name;
    part.name = name;
    for (ModelPart& other : m_model.parts)
    {
        if (other.parent == old)
        {
            other.parent = name;
        }
    }
    for (AnimationClip& clip : m_model.clips)
    {
        for (AnimationTrack& track : clip.tracks)
        {
            if (track.part == old)
            {
                track.part = name;
            }
        }
    }
}

void ModelEditor::JoinParts(int into, const std::vector<int>& others)
{
    if (into < 0 || into >= static_cast<int>(m_model.parts.size()) || others.empty())
    {
        return;
    }
    PushUndo("joining parts");
    // One mesh, in the frame of the part everything is joined into, at the size it is drawn at.
    ModelPart& target = m_model.parts[static_cast<size_t>(into)];
    MeshData joined = m_model.BuildPartMesh(target);
    const glm::mat4 intoTarget = glm::inverse(target.LocalMatrix());
    std::vector<std::string> gone;
    for (const int index : others)
    {
        if (index == into || index < 0 || index >= static_cast<int>(m_model.parts.size()))
        {
            continue;
        }
        const ModelPart& part = m_model.parts[static_cast<size_t>(index)];
        joined.Append(m_model.BuildPartMesh(part), intoTarget * part.LocalMatrix());
        gone.push_back(part.name);
    }
    target.shape = PartShape::Mesh;
    target.mesh = std::move(joined);
    target.size = glm::vec3(1.0f);
    const std::string targetName = target.name;

    // The joined parts go, and anything that moved with one of them moves with the joined part now.
    m_model.parts.erase(std::remove_if(m_model.parts.begin(), m_model.parts.end(),
                                       [&](const ModelPart& part)
                                       { return std::find(gone.begin(), gone.end(), part.name) != gone.end(); }),
                        m_model.parts.end());
    for (ModelPart& part : m_model.parts)
    {
        if (std::find(gone.begin(), gone.end(), part.parent) != gone.end())
        {
            part.parent = targetName;
        }
    }
    for (int i = 0; i < static_cast<int>(m_model.parts.size()); ++i)
    {
        if (m_model.parts[static_cast<size_t>(i)].name == targetName)
        {
            m_selectedPart = i;
        }
    }
    m_status = "Joined " + std::to_string(gone.size()) + " part(s) into " + targetName +
               ". It takes its colour and texture; the others' are gone with them.";
    m_dirty = true;
    m_previewChanged = true;
    m_geometryChanged = true;
}

void ModelEditor::DrawPartList()
{
    if (ImGui::Button("Box"))
    {
        AddPart("part", PartShape::Box);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cylinder"))
    {
        AddPart("part", PartShape::Cylinder);
    }
    ImGui::SameLine();
    if (ImGui::Button("Sphere"))
    {
        AddPart("part", PartShape::Sphere);
    }

    // Parts that move with another are listed under it, indented, so a magazine made of six pieces
    // reads as one magazine.
    ImGui::BeginChild("##parts", {0.0f, 150.0f}, ImGuiChildFlags_Borders);
    const auto row = [&](int i, int depth, const auto& self) -> void
    {
        ModelPart& part = m_model.parts[static_cast<size_t>(i)];
        ImGui::PushID(i);
        bool visible = part.visible;
        if (ImGui::Checkbox("##visible", &visible))
        {
            part.visible = visible;
        }
        ImGui::SameLine(0.0f, 4.0f + 16.0f * static_cast<float>(depth));
        if (ImGui::Selectable(part.name.empty() ? "(unnamed part)" : part.name.c_str(), m_selectedPart == i))
        {
            m_selectedPart = i;
            m_pick = Pick::Part;
        }
        ImGui::PopID();
        const std::string name = part.name;
        for (int j = 0; j < static_cast<int>(m_model.parts.size()) && depth < 8; ++j)
        {
            if (j != i && m_model.parts[static_cast<size_t>(j)].parent == name && !name.empty())
            {
                self(j, depth + 1, self);
            }
        }
    };
    for (int i = 0; i < static_cast<int>(m_model.parts.size()); ++i)
    {
        const ModelPart& part = m_model.parts[static_cast<size_t>(i)];
        // Top-level ones here; the rest are drawn under their parent. A part whose parent has gone
        // is top-level again rather than lost from the list.
        if (part.parent.empty() || m_model.FindPart(part.parent) == nullptr || part.parent == part.name)
        {
            row(i, 0, row);
        }
    }
    ImGui::EndChild();

    if (m_selectedPart < 0 || m_selectedPart >= static_cast<int>(m_model.parts.size()))
    {
        return;
    }
    if (ImGui::Button("Duplicate"))
    {
        PushUndo("a duplicate");
        ModelPart copy = m_model.parts[static_cast<size_t>(m_selectedPart)];
        copy.name += "_copy";
        m_model.parts.push_back(std::move(copy));
        m_selectedPart = static_cast<int>(m_model.parts.size()) - 1;
        m_pick = Pick::Part;
        m_dirty = true;
        m_previewChanged = true;
        m_geometryChanged = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete"))
    {
        PushUndo("a delete");
        const std::string gone = m_model.parts[static_cast<size_t>(m_selectedPart)].name;
        m_model.parts.erase(m_model.parts.begin() + m_selectedPart);
        // Anything that moved with it moves with what it moved with, rather than with nothing.
        for (ModelPart& part : m_model.parts)
        {
            if (part.parent == gone)
            {
                part.parent.clear();
            }
        }
        m_selectedPart = std::min(m_selectedPart, static_cast<int>(m_model.parts.size()) - 1);
        m_dirty = true;
        m_previewChanged = true;
        m_geometryChanged = true;
        return;
    }

    // Grouping: tick the parts that move with this one. Nothing moves when you tick one -- a part in a
    // group rests where it rests -- but from then on, whatever animates this part carries them too.
    ImGui::SameLine();
    if (ImGui::Button("Group..."))
    {
        ImGui::OpenPopup("##group");
    }
    ImGui::SetItemTooltip("%s", "Tick the parts that move with this one: the pieces of a magazine, a bolt "
                                "and its handle. Animate this part and they come with it.");
    if (ImGui::BeginPopup("##group"))
    {
        const std::string group = m_model.parts[static_cast<size_t>(m_selectedPart)].name;
        ImGui::TextDisabled("Moves with %s:", group.c_str());
        ImGui::BeginChild("##groupparts", {280.0f, 220.0f}, ImGuiChildFlags_Borders);
        for (int i = 0; i < static_cast<int>(m_model.parts.size()); ++i)
        {
            ModelPart& part = m_model.parts[static_cast<size_t>(i)];
            // Not itself, and not anything this part already moves with: that would be a loop.
            if (i == m_selectedPart || IsInside(group, part.name))
            {
                continue;
            }
            ImGui::PushID(i);
            bool inGroup = part.parent == group;
            if (ImGui::Checkbox(part.name.empty() ? "(unnamed part)" : part.name.c_str(), &inGroup))
            {
                PushUndo("grouping");
                part.parent = inGroup ? group : std::string();
                m_dirty = true;
                m_dirty = true;
                m_previewChanged = true;
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }

    // Joining: several parts become this one, for good. For pieces that never move apart, a downloaded
    // model split into twenty meshes being the usual case.
    ImGui::SameLine();
    if (ImGui::Button("Join..."))
    {
        m_joinPicks.assign(m_model.parts.size(), false);
        ImGui::OpenPopup("##join");
    }
    ImGui::SetItemTooltip("%s", "Merge other parts into this one, as one piece. For pieces that never move "
                                "apart. Undo brings them back.");
    if (ImGui::BeginPopup("##join"))
    {
        m_joinPicks.resize(m_model.parts.size(), false);
        ImGui::TextDisabled("Join into %s:", m_model.parts[static_cast<size_t>(m_selectedPart)].name.c_str());
        ImGui::BeginChild("##joinparts", {280.0f, 220.0f}, ImGuiChildFlags_Borders);
        for (int i = 0; i < static_cast<int>(m_model.parts.size()); ++i)
        {
            if (i == m_selectedPart)
            {
                continue;
            }
            ImGui::PushID(i);
            bool picked = m_joinPicks[static_cast<size_t>(i)];
            const std::string& name = m_model.parts[static_cast<size_t>(i)].name;
            if (ImGui::Checkbox(name.empty() ? "(unnamed part)" : name.c_str(), &picked))
            {
                m_joinPicks[static_cast<size_t>(i)] = picked;
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (ImGui::Button("Join them"))
        {
            std::vector<int> picked;
            for (int i = 0; i < static_cast<int>(m_joinPicks.size()); ++i)
            {
                if (m_joinPicks[static_cast<size_t>(i)])
                {
                    picked.push_back(i);
                }
            }
            JoinParts(m_selectedPart, picked);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void ModelEditor::DrawPartInspector()
{
    if (m_selectedPart < 0 || m_selectedPart >= static_cast<int>(m_model.parts.size()))
    {
        ImGui::TextDisabled("Select a part.");
        return;
    }
    ModelPart& part = m_model.parts[static_cast<size_t>(m_selectedPart)];

    char nameBuffer[96];
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", part.name.c_str());
    if (ImGui::InputText("Part name", nameBuffer, sizeof(nameBuffer)))
    {
        RenamePart(part, nameBuffer);
        m_dirty = true;
    }

    // Which part this one is carried by when that part is animated.
    const char* movesWith = part.parent.empty() ? "(nothing)" : part.parent.c_str();
    if (ImGui::BeginCombo("Moves with", movesWith))
    {
        if (ImGui::Selectable("(nothing)", part.parent.empty()))
        {
            PushUndo("grouping");
            part.parent.clear();
            m_previewChanged = true;
            m_dirty = true;
            m_dirty = true;
        }
        for (const ModelPart& other : m_model.parts)
        {
            // Not itself, and not a part that already moves with this one.
            if (&other == &part || other.name.empty() || IsInside(other.name, part.name))
            {
                continue;
            }
            if (ImGui::Selectable(other.name.c_str(), other.name == part.parent))
            {
                PushUndo("grouping");
                part.parent = other.name;
                m_previewChanged = true;
                m_dirty = true;
                m_dirty = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s", "When that part is animated, this one goes with it. A part that is held in "
                                "a hand follows the hand instead.");

    ImGui::TextDisabled("Shape: %s", PartShapeName(part.shape));

    bool changed = false;
    changed |= DragVec3("Position", part.position, 0.005f);
    changed |= DragVec3("Rotation", part.rotation, 0.5f, "%.1f deg");
    changed |= DragVec3("Size", part.size, 0.005f);

    if (m_snapEnabled && changed)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            part.position[axis] = SnapTo(part.position[axis], m_gridSnap);
            part.size[axis] = std::max(SnapTo(part.size[axis], m_gridSnap), m_gridSnap);
        }
    }

    changed |= ImGui::ColorEdit3("Colour", glm::value_ptr(part.color));
    changed |= ImGui::SliderFloat("Roughness", &part.roughness, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Metallic", &part.metallic, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Emissive", &part.emissive, 0.0f, 4.0f);

    if (changed)
    {
        m_dirty = true;
    m_previewChanged = true;
    m_geometryChanged = true;
    }

    if (part.shape == PartShape::Mesh)
    {
        ImGui::TextDisabled("Imported from %s", part.sourceFile.c_str());
        ImGui::TextDisabled("%zu vertices, %zu triangles", part.mesh.vertices.size(),
                            part.mesh.TriangleCount());
    }
}

void ModelEditor::DrawSocketPanel()
{
    ImGui::TextDisabled("Sockets are what the game asks for by name. Carry is where the weapon "
                        "sits, so moving it moves the gun on the screen; grip and support are "
                        "where the hands close, so moving those moves the hands along the gun. "
                        "Muzzle is where rounds appear, magazine is where the magazine seats, and "
                        "sight is the line aiming puts on the view axis. Click one in the viewport "
                        "to select it, then drag a handle.");

    // A model written before the carry socket existed has none, and falls back to being carried by
    // its grip, which is the behaviour that could not tell the two apart. Offering to add one where
    // the grip is changes nothing until it is moved.
    if (m_model.FindSocket("carry") == nullptr && m_model.FindSocket("grip") != nullptr)
    {
        ImGui::TextColored({0.90f, 0.80f, 0.55f, 1.0f},
                           "This model has no carry socket, so the gun is carried by its grip and "
                           "moving the grip moves both.");
        if (ImGui::Button("Add a carry socket on the grip"))
        {
            PushUndo("a carry socket");
            ModelSocket carry;
            carry.name = "carry";
            carry.position = m_model.FindSocket("grip")->position;
            m_model.sockets.push_back(carry);
            m_selectedSocket = static_cast<int>(m_model.sockets.size()) - 1;
            m_pick = Pick::Socket;
            m_dirty = true;
            m_previewChanged = true;
        }
    }
    if (ImGui::Button("Add socket"))
    {
        PushUndo("a new socket");
        ModelSocket socket;
        socket.name = "socket_" + std::to_string(m_model.sockets.size() + 1);
        m_model.sockets.push_back(socket);
        m_selectedSocket = static_cast<int>(m_model.sockets.size()) - 1;
        m_dirty = true;
    m_previewChanged = true;
    }

    for (int i = 0; i < static_cast<int>(m_model.sockets.size()); ++i)
    {
        ModelSocket& socket = m_model.sockets[static_cast<size_t>(i)];
        ImGui::PushID(1000 + i);
        char nameBuffer[64];
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", socket.name.c_str());
        // Clicking the name selects it, so the list and the viewport agree about what is being
        // worked on and the handles appear on whichever one was picked either way.
        const bool selected = m_pick == Pick::Socket && i == m_selectedSocket;
        if (ImGui::RadioButton("##socketpick", selected))
        {
            m_pick = Pick::Socket;
            m_selectedSocket = i;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(96.0f);
        if (ImGui::InputText("##socketname", nameBuffer, sizeof(nameBuffer)))
        {
            socket.name = nameBuffer;
            // The name is the whole contract with the game: renaming one is the difference
            // between a hand holding the grip and a hand holding nothing.
            m_dirty = true;
            m_previewChanged = true;
        }
        ImGui::SameLine();
        if (DragVec3("##socketpos", socket.position, 0.005f))
        {
            // Without this a socket moved by typing changed the data and nothing else: the hands
            // went on holding where it used to be, and moving a grip appeared to do nothing at all.
            m_dirty = true;
            m_previewChanged = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("x"))
        {
            PushUndo("a socket delete");
            m_model.sockets.erase(m_model.sockets.begin() + i);
            m_dirty = true;
            m_previewChanged = true;
            ImGui::PopID();
            break;
        }

        // The turn, under the selected socket only, because it is three more fields and only one
        // socket is being worked on at a time. On `grip` this is how the weapon sits in the hand,
        // which is the only way to right a model that was exported on its side: turning the
        // geometry instead would drag the sockets, the clips and the animation along with it.
        if (selected)
        {
            ImGui::Indent();
            if (DragVec3("Turn", socket.rotation, 0.5f, "%.1f deg"))
            {
                m_dirty = true;
                m_previewChanged = true;
            }
            const auto quarter = [&](const char* label, const glm::vec3& about)
            {
                if (ImGui::SmallButton(label))
                {
                    PushUndo("a socket turn");
                    socket.rotation += about;
                    m_dirty = true;
                    m_previewChanged = true;
                }
            };
            quarter("X+90", {90.0f, 0.0f, 0.0f});
            ImGui::SameLine();
            quarter("Y+90", {0.0f, 90.0f, 0.0f});
            ImGui::SameLine();
            quarter("Z+90", {0.0f, 0.0f, 90.0f});
            ImGui::SameLine();
            if (ImGui::SmallButton("Square"))
            {
                PushUndo("a socket turn");
                socket.rotation = glm::vec3(0.0f);
                m_dirty = true;
                m_previewChanged = true;
            }
            if (socket.name == "grip")
            {
                ImGui::TextDisabled("This turns the whole weapon in the hand. The grip stays where "
                                    "it is; the model spins about it.");
                // Which way the position fields move things is worth saying outright, because it is
                // the reverse of what it looks like: this socket is the point the hand holds, so
                // the game places the weapon by subtracting it. Raising it lowers the gun.
                ImGui::TextDisabled("The position above is where the hand grips. The game hangs the "
                                    "weapon off it, so this is also how the weapon is moved in the "
                                    "hand, backwards: raise this socket and the gun sits lower on "
                                    "the screen, move it forward and the gun comes back.");
            }
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
}

AnimationTrack* ModelEditor::SelectedTrack()
{
    AnimationClip* clip = CurrentClip();
    if (clip == nullptr || m_selectedTrack < 0 || m_selectedTrack >= static_cast<int>(clip->tracks.size()))
    {
        return nullptr;
    }
    return &clip->tracks[static_cast<size_t>(m_selectedTrack)];
}

AnimationTrack& ModelEditor::TrackFor(AnimationClip& clip, const std::string& partName)
{
    const auto found = std::find_if(clip.tracks.begin(), clip.tracks.end(),
                                    [&](const AnimationTrack& track) { return track.part == partName; });
    if (found != clip.tracks.end())
    {
        return *found;
    }
    AnimationTrack fresh;
    fresh.part = partName;
    clip.tracks.push_back(std::move(fresh));
    return clip.tracks.back();
}

AnimationKey* ModelEditor::KeyAt(AnimationTrack& track, float time)
{
    const auto found = std::find_if(track.keys.begin(), track.keys.end(), [&](const AnimationKey& key)
                                    { return std::abs(key.time - time) < 1e-3f; });
    return found == track.keys.end() ? nullptr : &*found;
}

AnimationKey& ModelEditor::AddOrGetKey(AnimationTrack& track, float time)
{
    if (AnimationKey* existing = KeyAt(track, time))
    {
        return *existing;
    }
    // A new key starts from where the track already is at this moment -- held by the same thing, at
    // the same place, turned the same way -- so adding one never moves anything: it pins down what is
    // already on screen and then you change it.
    AnimationKey key;
    key.time = time;
    if (!track.keys.empty())
    {
        const TrackSample sample = SampleTrack(track, time);
        key.position = sample.position;
        key.rotation = sample.euler;
        key.visible = sample.visible;
        key.holder = sample.holder;
    }
    track.keys.push_back(key);
    std::sort(track.keys.begin(), track.keys.end(),
              [](const AnimationKey& a, const AnimationKey& b) { return a.time < b.time; });
    return *KeyAt(track, time);
}

void ModelEditor::KeyAllParts()
{
    AnimationClip* clip = CurrentClip();
    if (clip == nullptr)
    {
        return;
    }
    for (const ModelPart& part : m_model.parts)
    {
        AnimationKey& key = AddOrGetKey(TrackFor(*clip, part.name), m_playhead);
        key.visible = part.visible ? 1.0f : 0.0f;
    }
    // And the hands, when the clip moves them: a pose is the parts and the hands together.
    for (AnimationTrack& track : clip->tracks)
    {
        if (track.part == kLeftHandTrack || track.part == kRightHandTrack)
        {
            AddOrGetKey(track, m_playhead);
        }
    }
    m_status = "Keyed every part";
}

const AnimationClip* ModelEditor::SelectedClip() const
{
    if (m_selectedClip < 0 || m_selectedClip >= static_cast<int>(m_model.clips.size()))
    {
        return nullptr;
    }
    return &m_model.clips[static_cast<size_t>(m_selectedClip)];
}

void ModelEditor::SetHolder(AnimationClip& clip, AnimationTrack& track, AnimationKey& key, PartHolder holder)
{
    if (key.holder == holder)
    {
        return;
    }
    const ModelPart* part = m_model.FindPart(track.part);
    if (part == nullptr)
    {
        key.holder = holder;
        return;
    }
    // Where the part is at this key before anything changes, and then the numbers that put it in the
    // same place in its new holder's frame. Handing a magazine to a hand is then a change of who is
    // carrying it and nothing else: it does not jump.
    const glm::mat4 now = m_model.PartMatrixAt(*part, &clip, key.time);
    key.holder = holder;
    if (holder == PartHolder::Weapon)
    {
        key.position = glm::vec3(now[3]) - part->position;
        key.rotation = EulerDegreesFromMatrix(now) - part->rotation;
    }
    else
    {
        const int side = holder == PartHolder::LeftHand ? 0 : 1;
        const glm::mat4 inHand =
            glm::inverse(m_model.HandFrameAt(&clip, side, key.time, m_model.HandRest(side))) * now;
        key.position = glm::vec3(inHand[3]);
        key.rotation = EulerDegreesFromMatrix(inHand);
    }
    m_dirty = true;
    m_previewChanged = true;
}

bool ModelEditor::KeyPosition(glm::vec3& out) const
{
    const AnimationClip* clip = SelectedClip();
    if (clip == nullptr || m_selectedTrack < 0 || m_selectedTrack >= static_cast<int>(clip->tracks.size()))
    {
        return false;
    }
    const AnimationTrack& track = clip->tracks[static_cast<size_t>(m_selectedTrack)];
    const bool keyed = std::any_of(track.keys.begin(), track.keys.end(), [&](const AnimationKey& key)
                                   { return std::abs(key.time - m_playhead) < 1e-3f; });
    if (!keyed)
    {
        return false;
    }
    if (track.part == kLeftHandTrack || track.part == kRightHandTrack)
    {
        const int side = track.part == kLeftHandTrack ? 0 : 1;
        out = glm::vec3(m_model.HandFrameAt(clip, side, m_playhead, m_model.HandRest(side))[3]);
        return true;
    }
    const ModelPart* part = m_model.FindPart(track.part);
    if (part == nullptr)
    {
        return false;
    }
    out = glm::vec3(m_model.PartMatrixAt(*part, clip, m_playhead)[3]);
    return true;
}

void ModelEditor::MoveKey(const glm::vec3& delta)
{
    AnimationClip* clip = CurrentClip();
    AnimationTrack* track = SelectedTrack();
    AnimationKey* key = track != nullptr ? KeyAt(*track, m_playhead) : nullptr;
    if (clip == nullptr || key == nullptr)
    {
        return;
    }
    if (key->holder == PartHolder::Weapon || track->part == kLeftHandTrack || track->part == kRightHandTrack)
    {
        // An offset in the weapon's frame, which is the frame the handles are drawn in.
        key->position += delta;
    }
    else
    {
        // A place in a hand, whose frame may be turned: the drag is turned into it.
        const int side = key->holder == PartHolder::LeftHand ? 0 : 1;
        const glm::mat3 hand = glm::mat3(m_model.HandFrameAt(clip, side, m_playhead, m_model.HandRest(side)));
        key->position += glm::transpose(hand) * delta;
    }
    m_dirty = true;
    m_previewChanged = true;
}

void ModelEditor::MakeReloadTemplate(ReloadTemplate kind)
{
    // Everything is measured from the model: where the support hand rests, where the magazine sits and
    // how tall it is. The numbers only have to be a good start -- the point of a template is that it
    // already moves, so shaping it is changing keys rather than inventing them.
    if (m_model.FindPart("magazine") == nullptr)
    {
        m_status = "A reload template needs a part called 'magazine'. Rename the part that is one.";
        return;
    }
    PushUndo("a reload template");

    if (kind == ReloadTemplate::DoubleMagazine && m_model.FindPart("magazine_2") == nullptr)
    {
        // The second magazine, taped alongside the first and upside down: the first turned half over
        // about the barrel's axis, around a point beside it. That shape is the same after being rolled
        // over as before, which is what lets the reload end with the model exactly as it started.
        ModelPart second = *m_model.FindPart("magazine");
        const AABB bounds = PartBounds(second);
        const float spacing = std::max(bounds.max.x - bounds.min.x, 0.02f) + 0.004f;
        second.name = "magazine_2";
        second.position.x += spacing;
        second.rotation.z += 180.0f;
        m_model.parts.push_back(std::move(second));
        m_dirty = true;
        m_geometryChanged = true;
    }

    const char* name = kind == ReloadTemplate::Empty ? "reload_empty" : "reload";
    const float duration =
        kind == ReloadTemplate::Tactical ? 2.2f : (kind == ReloadTemplate::Empty ? 2.6f : 2.4f);
    const auto existing = std::find_if(m_model.clips.begin(), m_model.clips.end(),
                                       [&](const AnimationClip& candidate) { return candidate.name == name; });
    if (existing != m_model.clips.end())
    {
        m_model.clips.erase(existing);
    }
    AnimationClip fresh;
    fresh.name = name;
    fresh.duration = duration;
    m_model.clips.push_back(std::move(fresh));
    AnimationClip& clip = m_model.clips.back();

    const ModelPart magazine = *m_model.FindPart("magazine");
    const AABB magazineBounds = PartBounds(magazine);
    const float magazineHeight = std::max(magazineBounds.max.y - magazineBounds.min.y, 0.05f);
    const glm::vec3 support = m_model.HandRest(0);
    // Where the palm closes on the magazine: a little up from its bottom end.
    const glm::vec3 grab = magazine.position + glm::vec3(0.0f, -magazineHeight * 0.3f, 0.0f);
    const glm::vec3 toMagazine = grab - support;
    // A pouch on the belt, from the support hand: down, out to the left, and back towards the body.
    const glm::vec3 toPouch{-0.10f, -0.32f, -0.28f};
    const glm::vec3 pulled = toMagazine + glm::vec3(0.0f, -0.16f, 0.0f);

    const auto addKey = [&](const std::string& track, float time, const glm::vec3& offset, const glm::vec3& turn)
    {
        AnimationKey key;
        key.time = time;
        key.position = offset;
        key.rotation = turn;
        TrackFor(clip, track).keys.push_back(key);
    };
    const auto handKey = [&](float time, const glm::vec3& offset, const glm::vec3& turn = glm::vec3(0.0f))
    { addKey(kLeftHandTrack, time, offset, turn); };
    const auto rootKey = [&](float time, const glm::vec3& turn, const glm::vec3& offset = glm::vec3(0.0f))
    { addKey("root", time, offset, turn); };
    const auto sortAll = [&]()
    {
        for (AnimationTrack& track : clip.tracks)
        {
            std::sort(track.keys.begin(), track.keys.end(),
                      [](const AnimationKey& a, const AnimationKey& b) { return a.time < b.time; });
        }
    };
    // A key that puts a part in the left hand exactly where it rests, worked out once the hand's own
    // keys are in place.
    const auto inHand = [&](const std::string& part, float time)
    {
        const ModelPart* found = m_model.FindPart(part);
        const glm::mat4 rest = found != nullptr ? found->LocalMatrix() : glm::mat4(1.0f);
        const glm::mat4 local = glm::inverse(m_model.HandFrameAt(&clip, 0, time, support)) * rest;
        AnimationKey key;
        key.time = time;
        key.position = glm::vec3(local[3]);
        key.rotation = EulerDegreesFromMatrix(local);
        key.holder = PartHolder::LeftHand;
        return key;
    };
    const auto inWeapon = [](float time)
    {
        AnimationKey key;
        key.time = time;
        return key;
    };

    if (kind == ReloadTemplate::DoubleMagazine)
    {
        // Out, roll the pair over, in. The pair is rolled about its own middle rather than about the
        // hand, so the hand travels while it turns: its offsets at the half and the whole turn are
        // worked out to keep the middle of the pair where it is.
        rootKey(0.0f, glm::vec3(0.0f));
        rootKey(0.3f, glm::vec3(8.0f, 0.0f, 22.0f), glm::vec3(-0.03f, 0.03f, 0.0f));
        rootKey(2.0f, glm::vec3(8.0f, 0.0f, 22.0f), glm::vec3(-0.03f, 0.03f, 0.0f));
        rootKey(2.4f, glm::vec3(0.0f));
        handKey(0.0f, glm::vec3(0.0f));
        handKey(0.35f, toMagazine);
        handKey(0.45f, toMagazine);
        handKey(0.75f, pulled);

        const ModelPart second = *m_model.FindPart("magazine_2");
        const glm::vec3 middle = (magazine.position + second.position) * 0.5f + glm::vec3(0.0f, -0.16f, 0.0f);
        const glm::vec3 middleInHand = middle - (support + pulled);
        const auto rolled = [&](float degrees)
        {
            const glm::mat3 turn = glm::mat3(EulerDegreesMatrix(glm::vec3(0.0f, 0.0f, degrees)));
            return middle - support - turn * middleInHand;
        };
        handKey(1.0f, rolled(90.0f), glm::vec3(0.0f, 0.0f, 90.0f));
        handKey(1.25f, rolled(180.0f), glm::vec3(0.0f, 0.0f, 180.0f));
        handKey(1.6f, rolled(180.0f) + glm::vec3(0.0f, 0.16f, 0.0f), glm::vec3(0.0f, 0.0f, 180.0f));
        handKey(1.75f, rolled(180.0f) + glm::vec3(0.0f, 0.16f, 0.0f), glm::vec3(0.0f, 0.0f, 180.0f));
        handKey(2.05f, glm::vec3(-0.04f, -0.05f, 0.0f), glm::vec3(0.0f, 0.0f, 90.0f));
        handKey(2.4f, glm::vec3(0.0f));
        sortAll();

        for (const char* part : {"magazine", "magazine_2"})
        {
            AnimationTrack& track = TrackFor(clip, part);
            track.keys.push_back(inWeapon(0.0f));
            track.keys.push_back(inHand(part, 0.45f));
            // Seated again, the other way up. The pair looks exactly as it did before, so each part is
            // simply put back where it rests.
            track.keys.push_back(inWeapon(1.6f));
        }
        m_status = "Made a double-magazine reload: out, roll the pair over, in. Shape it from here.";
    }
    else
    {
        const bool empty = kind == ReloadTemplate::Empty;
        rootKey(0.0f, glm::vec3(0.0f));
        // Up a little, towards the middle, and rolled so the magazine well faces the eyes: a reload is
        // looked at, and a weapon held as it is for shooting hides everything below it.
        const glm::vec3 lift{-0.03f, 0.03f, 0.0f};
        const glm::vec3 roll{8.0f, 0.0f, 22.0f};
        rootKey(0.3f, roll, lift);
        rootKey(empty ? 2.0f : 1.85f, roll, lift);
        handKey(0.0f, glm::vec3(0.0f));
        handKey(0.35f, toMagazine);
        handKey(0.45f, toMagazine);
        handKey(0.75f, pulled);
        handKey(1.05f, toPouch, glm::vec3(0.0f, 0.0f, -25.0f));
        handKey(1.25f, toPouch, glm::vec3(0.0f, 0.0f, -25.0f));
        handKey(1.6f, toMagazine + glm::vec3(0.0f, -0.08f, 0.0f));
        handKey(1.75f, toMagazine);
        if (empty)
        {
            // Then the bolt catch, on the side of the receiver above the magazine, and the weapon
            // jolting as the bolt goes home.
            const glm::vec3 boltCatch = toMagazine + glm::vec3(-0.035f, magazineHeight * 0.55f, 0.03f);
            handKey(2.0f, boltCatch);
            handKey(2.1f, boltCatch + glm::vec3(0.012f, 0.0f, 0.0f));
            rootKey(2.1f, roll + glm::vec3(-4.0f, 0.0f, -6.0f), lift + glm::vec3(0.0f, 0.0f, -0.01f));
            rootKey(2.3f, roll * 0.5f, lift * 0.5f);
            rootKey(2.6f, glm::vec3(0.0f));
            handKey(2.6f, glm::vec3(0.0f));
        }
        else
        {
            rootKey(2.2f, glm::vec3(0.0f));
            handKey(2.2f, glm::vec3(0.0f));
        }
        sortAll();

        AnimationTrack& track = TrackFor(clip, "magazine");
        track.keys.push_back(inWeapon(0.0f));
        AnimationKey held = inHand("magazine", 0.45f);
        track.keys.push_back(held);
        // Into the pouch, and a full one out of it: the same part, gone for a moment.
        held.time = 1.05f;
        held.visible = 0.0f;
        track.keys.push_back(held);
        held.time = 1.25f;
        held.visible = 1.0f;
        track.keys.push_back(held);
        // Seated the moment the hand is back at the magazine well, which is where the hand put it.
        track.keys.push_back(inWeapon(1.75f));

        // A bolt, if the model has one: locked back while the magazine is empty, forward when the
        // catch is hit.
        if (empty && m_model.FindPart("bolt") != nullptr)
        {
            AnimationKey back = inWeapon(0.0f);
            back.position = glm::vec3(0.0f, 0.0f, -0.04f);
            back.ease = KeyEase::Step;
            TrackFor(clip, "bolt").keys.push_back(back);
            back.time = 2.1f;
            TrackFor(clip, "bolt").keys.push_back(back);
            TrackFor(clip, "bolt").keys.push_back(inWeapon(2.12f));
        }
        m_status = empty ? "Made a reload from empty, ending with the bolt released. Shape it from here."
                         : "Made a reload: magazine out, a fresh one from the belt, in. Shape it from here.";
    }
    sortAll();

    m_selectedClip = static_cast<int>(m_model.clips.size()) - 1;
    m_selectedTrack = 0;
    m_playhead = 0.0f;
    m_dirty = true;
    m_previewChanged = true;
}

void ModelEditor::DrawTimeline(AnimationClip& clip)
{
    // A row per part with its keys drawn on it, and a playhead across the whole thing. Clicking the
    // strip moves the playhead; clicking a key selects it; the buttons act on the selection. The
    // old panel was a list of numbers with no sense of when anything happened, which is most of
    // what an animation editor is for.
    constexpr float kRowHeight = 22.0f;
    constexpr float kLabelWidth = 110.0f;
    const float width = std::max(ImGui::GetContentRegionAvail().x - kLabelWidth - 12.0f, 120.0f);
    const float duration = std::max(clip.duration, 0.05f);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 laneColor = IM_COL32(38, 41, 50, 255);
    const ImU32 gridColor = IM_COL32(60, 64, 76, 255);
    const ImU32 keyColor = IM_COL32(210, 190, 120, 255);
    const ImU32 keySelected = IM_COL32(255, 235, 170, 255);
    const ImU32 headColor = IM_COL32(230, 120, 110, 255);

    for (size_t t = 0; t < clip.tracks.size(); ++t)
    {
        AnimationTrack& track = clip.tracks[t];
        ImGui::PushID(static_cast<int>(t));

        const bool isSelected = m_selectedTrack == static_cast<int>(t);
        if (ImGui::Selectable(track.part.c_str(), isSelected, 0, {kLabelWidth, kRowHeight}))
        {
            m_selectedTrack = static_cast<int>(t);
            m_pick = Pick::Key;
        }
        ImGui::SameLine(kLabelWidth + 8.0f);

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        draw->AddRectFilled(origin, {origin.x + width, origin.y + kRowHeight}, laneColor, 3.0f);
        for (int mark = 0; mark <= 4; ++mark)
        {
            const float x = origin.x + width * static_cast<float>(mark) * 0.25f;
            draw->AddLine({x, origin.y}, {x, origin.y + kRowHeight}, gridColor);
        }

        ImGui::InvisibleButton("##lane", {width, kRowHeight});
        const bool hovered = ImGui::IsItemHovered();
        const float mouseTime =
            std::clamp((ImGui::GetIO().MousePos.x - origin.x) / width, 0.0f, 1.0f) * duration;

        // Which key the pointer is over, in pixels rather than in seconds. Landing on a key used to
        // mean putting the playhead within a thousandth of a second of it, which at this width is a
        // fraction of a pixel: keys were selectable only by accident.
        constexpr float kGrabPixels = 7.0f;
        const float grabSeconds = duration * (kGrabPixels / std::max(width, 1.0f));
        int nearest = -1;
        float nearestGap = grabSeconds;
        for (size_t k = 0; k < track.keys.size(); ++k)
        {
            const float gap = std::abs(track.keys[k].time - mouseTime);
            if (gap <= nearestGap)
            {
                nearestGap = gap;
                nearest = static_cast<int>(k);
            }
        }

        if (ImGui::IsItemActivated())
        {
            m_selectedTrack = static_cast<int>(t);
            m_pick = Pick::Key;
            m_playing = false;
            // Grabbing a key takes hold of it; grabbing anywhere else moves the playhead. Either
            // way the playhead ends up on the key, so the inspector below is showing the one being
            // dragged rather than whatever happened to be under the last click.
            m_dragTrack = nearest >= 0 ? static_cast<int>(t) : -1;
            m_dragKey = nearest;
            if (nearest >= 0)
            {
                PushUndo("moving a key");
                m_playhead = track.keys[static_cast<size_t>(nearest)].time;
            }
            else
            {
                m_playhead = mouseTime;
            }
        }
        else if (ImGui::IsItemActive())
        {
            if (m_dragTrack == static_cast<int>(t) && m_dragKey >= 0 &&
                m_dragKey < static_cast<int>(track.keys.size()))
            {
                // Dragged in time only. A key is a moment, and the values on it are edited in the
                // inspector; letting a drag change both at once means never being sure which moved.
                track.keys[static_cast<size_t>(m_dragKey)].time = mouseTime;
                std::sort(track.keys.begin(), track.keys.end(),
                          [](const AnimationKey& a, const AnimationKey& b) { return a.time < b.time; });
                // The sort may have moved it, so find it again by the time it now has.
                for (size_t k = 0; k < track.keys.size(); ++k)
                {
                    if (std::abs(track.keys[k].time - mouseTime) < 1e-6f)
                    {
                        m_dragKey = static_cast<int>(k);
                        break;
                    }
                }
                m_playhead = mouseTime;
                m_dirty = true;
                m_previewChanged = true;
            }
            else
            {
                m_playhead = mouseTime;
            }
        }
        else if (ImGui::IsItemDeactivated())
        {
            m_dragTrack = -1;
            m_dragKey = -1;
        }

        for (size_t k = 0; k < track.keys.size(); ++k)
        {
            const AnimationKey& key = track.keys[k];
            const float x = origin.x + width * std::clamp(key.time / duration, 0.0f, 1.0f);
            const float y = origin.y + kRowHeight * 0.5f;
            const bool onPlayhead = std::abs(key.time - m_playhead) < 1e-3f;
            const bool underPointer = hovered && nearest == static_cast<int>(k);
            // Bigger under the pointer, so it is clear what a click is about to take hold of.
            const float radius = underPointer ? 7.0f : 5.0f;
            draw->AddCircleFilled({x, y}, radius,
                                  (onPlayhead && isSelected) || underPointer ? keySelected : keyColor,
                                  10);
            if (underPointer)
            {
                draw->AddCircle({x, y}, radius + 2.0f, headColor, 10, 1.5f);
            }
        }

        const float headX = origin.x + width * std::clamp(m_playhead / duration, 0.0f, 1.0f);
        draw->AddLine({headX, origin.y}, {headX, origin.y + kRowHeight}, headColor, 1.5f);
        ImGui::PopID();
    }
}

void ModelEditor::DrawAnimationPanel()
{
    // The three names the game looks for, offered rather than left to be guessed. A clip named
    // anything else is still playable in here, but the game will never reach for it.
    const auto newClip = [&](const char* name, float duration)
    {
        PushUndo("a new clip");
        AnimationClip clip;
        clip.name = name;
        clip.duration = duration;
        m_model.clips.push_back(std::move(clip));
        m_selectedClip = static_cast<int>(m_model.clips.size()) - 1;
        m_selectedTrack = -1;
        m_playhead = 0.0f;
        m_dirty = true;
        m_previewChanged = true;
    };

    if (ImGui::Button("Reload clip"))
    {
        newClip("reload", 2.2f);
    }
    ImGui::SameLine();
    if (ImGui::Button("Equip clip"))
    {
        newClip("equip", 0.6f);
    }
    ImGui::SameLine();
    if (ImGui::Button("Fire clip"))
    {
        newClip("fire", 0.18f);
    }
    ImGui::SameLine();
    if (ImGui::Button("Other"))
    {
        newClip(("clip_" + std::to_string(m_model.clips.size() + 1)).c_str(), 1.5f);
    }
    ImGui::SameLine();
    if (ImGui::Button("Empty reload clip"))
    {
        newClip("reload_empty", 2.6f);
    }
    ImGui::TextDisabled("The game plays reload, reload_empty (when the magazine ran dry), equip and fire "
                        "at the right moments, and a reload takes exactly as long as its clip. Anything "
                        "else is yours to play from the Hold it panel.");

    // Reloads that already move, to shape rather than build from nothing: the hands, the magazine
    // held and handed over, and the weapon tilting to meet them.
    ImGui::TextUnformatted("Start a reload from a template:");
    if (ImGui::Button("Reload"))
    {
        MakeReloadTemplate(ReloadTemplate::Tactical);
    }
    ImGui::SetItemTooltip("%s", "Magazine out, a fresh one from the belt, in. Replaces the clip called reload.");
    ImGui::SameLine();
    if (ImGui::Button("Reload from empty"))
    {
        MakeReloadTemplate(ReloadTemplate::Empty);
    }
    ImGui::SetItemTooltip("%s", "The same, then the hand hits the bolt catch. Replaces the clip called "
                                "reload_empty, and moves a part called bolt if there is one.");
    ImGui::SameLine();
    if (ImGui::Button("Double magazine"))
    {
        MakeReloadTemplate(ReloadTemplate::DoubleMagazine);
    }
    ImGui::SetItemTooltip("%s", "Two magazines taped side by side, one upside down: out, roll the pair "
                                "over, in. Adds magazine_2 beside the magazine if there is none. "
                                "Replaces the clip called reload.");

    for (int i = 0; i < static_cast<int>(m_model.clips.size()); ++i)
    {
        ImGui::PushID(2000 + i);
        if (ImGui::RadioButton(m_model.clips[static_cast<size_t>(i)].name.c_str(), m_selectedClip == i))
        {
            m_selectedClip = i;
            m_selectedTrack = -1;
            m_playhead = 0.0f;
        }
        ImGui::PopID();
        if (i + 1 < static_cast<int>(m_model.clips.size()))
        {
            ImGui::SameLine();
        }
    }

    AnimationClip* clip = CurrentClip();
    if (clip == nullptr)
    {
        ImGui::TextDisabled("No clip selected.");
        return;
    }

    char nameBuffer[64];
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", clip->name.c_str());
    if (ImGui::InputText("Clip name", nameBuffer, sizeof(nameBuffer)))
    {
        clip->name = nameBuffer;
    }
    ImGui::DragFloat("Duration", &clip->duration, 0.05f, 0.1f, 20.0f, "%.2f s");
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &clip->loop);
    ImGui::SameLine();
    if (ImGui::Button("Delete clip"))
    {
        m_model.clips.erase(m_model.clips.begin() + m_selectedClip);
        m_selectedClip = m_model.clips.empty() ? -1 : 0;
        m_selectedTrack = -1;
        return;
    }

    // --- Transport ---------------------------------------------------------------------------
    if (ImGui::Button(m_playing ? "Pause" : "Play"))
    {
        m_playing = !m_playing;
    }
    ImGui::SameLine();
    if (ImGui::Button("|<"))
    {
        m_playhead = 0.0f;
        m_playing = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(">|"))
    {
        m_playhead = clip->duration;
        m_playing = false;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("##time", &m_playhead, 0.0f, clip->duration, "%.2f s"))
    {
        m_playing = false;
    }

    // --- Adding tracks and keys --------------------------------------------------------------
    // Adds the track if it is new, and selects it either way.
    const auto selectTrack = [&](const std::string& name)
    {
        TrackFor(*clip, name);
        for (size_t t = 0; t < clip->tracks.size(); ++t)
        {
            if (clip->tracks[t].part == name)
            {
                m_selectedTrack = static_cast<int>(t);
            }
        }
    };
    if (ImGui::BeginCombo("Animate part", "add a track"))
    {
        // "root" moves the whole weapon rather than one part, which is what an equip needs.
        if (ImGui::Selectable("root (the whole model)"))
        {
            selectTrack("root");
        }
        // The hands. A clip with one of these takes that hand over while it plays: where it goes and
        // how it turns are all in the keys.
        if (ImGui::Selectable("hand_left (the left hand)"))
        {
            selectTrack(kLeftHandTrack);
            m_pick = Pick::Key;
        }
        if (ImGui::Selectable("hand_right (the right hand)"))
        {
            selectTrack(kRightHandTrack);
            m_pick = Pick::Key;
        }
        // Each row carries its own id. A part whose name has been cleared would otherwise be a row
        // with an empty id at the root of a popup, which ImGui refuses by aborting the process.
        for (size_t p = 0; p < m_model.parts.size(); ++p)
        {
            const ModelPart& part = m_model.parts[p];
            ImGui::PushID(static_cast<int>(p));
            if (ImGui::Selectable(part.name.empty() ? "(unnamed part)" : part.name.c_str()))
            {
                selectTrack(part.name);
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    if (ImGui::Button("Key here"))
    {
        if (AnimationTrack* track = SelectedTrack())
        {
            AddOrGetKey(*track, m_playhead);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Key all parts"))
    {
        KeyAllParts();
    }
    ImGui::SameLine();
    if (ImGui::Button("Mirror hands"))
    {
        MirrorHands();
    }
    ImGui::SetItemTooltip("%s", "Swap the hands in this clip, reflected across the weapon: what the left hand "
                                "did the right does, and every part a hand held goes to the other.");
    ImGui::SameLine();
    if (ImGui::Button("Delete key"))
    {
        if (AnimationTrack* track = SelectedTrack())
        {
            const auto found = std::find_if(track->keys.begin(), track->keys.end(),
                                            [&](const AnimationKey& key)
                                            { return std::abs(key.time - m_playhead) < 1e-3f; });
            if (found != track->keys.end())
            {
                track->keys.erase(found);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete track"))
    {
        if (m_selectedTrack >= 0 && m_selectedTrack < static_cast<int>(clip->tracks.size()))
        {
            clip->tracks.erase(clip->tracks.begin() + m_selectedTrack);
            m_selectedTrack = -1;
            return;
        }
    }

    ImGui::Separator();
    DrawTimeline(*clip);
    ImGui::Separator();

    // --- The key under the playhead ------------------------------------------------------------
    AnimationTrack* track = SelectedTrack();
    if (track == nullptr)
    {
        ImGui::TextDisabled("Select a track above, then move the playhead and press Key here.");
        return;
    }
    AnimationKey* key = KeyAt(*track, m_playhead);
    if (key == nullptr)
    {
        ImGui::TextDisabled("No key on '%s' at %.2f s. Press Key here to make one.", track->part.c_str(),
                            static_cast<double>(m_playhead));
        return;
    }

    ImGui::Text("%s at %.2f s", track->part.c_str(), static_cast<double>(key->time));
    ImGui::DragFloat("Key time", &key->time, 0.01f, 0.0f, clip->duration, "%.2f s");
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        std::sort(track->keys.begin(), track->keys.end(),
                  [](const AnimationKey& a, const AnimationKey& b) { return a.time < b.time; });
        m_playhead = key->time;
    }
    DragVec3("Move", key->position, 0.005f);
    DragVec3("Turn", key->rotation, 0.5f, "%.1f deg");

    // How this key hands over to the next one. It belongs to the key being left rather than the one
    // being arrived at, because that is the one whose departure it describes.
    int ease = static_cast<int>(key->ease);
    if (ImGui::Combo("Leaves", &ease, "Holding\0Straight\0Eased\0"))
    {
        key->ease = static_cast<KeyEase>(std::clamp(ease, 0, 2));
        m_dirty = true;
        m_previewChanged = true;
    }
    ImGui::SetItemTooltip(
        "%s", "Holding keeps this value until the next key, for anything that does not slide: a "
              "magazine is in the weapon or gone. Straight runs at a constant speed. Eased starts "
              "and stops, which is what most of a reload is and is the reason to prefer it.");

    // A boolean, drawn as one. It was a slider from zero to one and it is not a fade: the renderer
    // treats anything above a hundredth as present, and the sampler holds a key's value until the
    // next one, so every value in between meant the same as one of the ends.
    const bool isHand = track->part == kLeftHandTrack || track->part == kRightHandTrack;
    if (isHand)
    {
        ImGui::TextDisabled("How far the hand has moved from its socket (%s), in the weapon's frame, and "
                            "how the wrist is turned. Zero is the hand on the gun, so start and end there. "
                            "Drag the handles in the view to move it.",
                            track->part == kLeftHandTrack ? "support" : "grip");
        return;
    }
    if (track->part == "root")
    {
        ImGui::TextDisabled("Moves the whole weapon in the hands: a tilt towards the magazine, a jolt as "
                            "the bolt goes home.");
        return;
    }

    bool visible = key->visible > 0.5f;
    if (ImGui::Checkbox("Part is there", &visible))
    {
        key->visible = visible ? 1.0f : 0.0f;
        m_dirty = true;
        m_previewChanged = true;
    }

    // Who is carrying the part from this key on. Changing it keeps the part exactly where it is, so a
    // magazine is taken by the hand without jumping, and put back in the weapon the same way.
    int holder = static_cast<int>(key->holder);
    if (ImGui::Combo("Held by", &holder, "The weapon\0The left hand\0The right hand\0"))
    {
        SetHolder(*clip, *track, *key, static_cast<PartHolder>(std::clamp(holder, 0, 2)));
    }
    ImGui::SetItemTooltip(
        "%s", "In a hand, the part goes wherever that hand goes until a later key gives it back. Hand it "
              "over at the moment the hand is on it, and back when the hand has put it where it rests.");
    ImGui::TextDisabled(key->holder == PartHolder::Weapon
                            ? "Offsets are from the part's rest pose, so editing the model keeps the clip."
                            : "Where the part sits in the hand. It moves with the hand from here.");
}

bool ModelEditor::IsImportableModel(const std::filesystem::path& file)
{
    std::string extension = file.extension().string();
    for (char& c : extension)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return extension == ".glb" || extension == ".gltf" || extension == ".obj";
}

bool ModelEditor::DropFile(const std::string& path)
{
    const std::filesystem::path file = path;
    if (!IsImportableModel(file))
    {
        return false;
    }
    // A dropped file with nothing open yet names the model, because that is nearly always what
    // somebody meant: they dragged m5_carbine.glb in to make m5_carbine.
    if (m_model.parts.empty() && m_saveName == "new_model")
    {
        m_saveName = file.stem().string();
    }
    ImportMesh(path);
    return true;
}

// Puts a download inside the project, beside the model it produced.
//
// Otherwise an imported model's only tie to the art it came from is a path into somebody's
// Downloads folder, which is not in the repository, is not on anybody else's machine, and will be
// cleared out one day. Copying it costs a megabyte and means the import can be repeated when the
// importer improves or when the original needs re-cutting.
std::string ModelEditor::KeepSource(const std::filesystem::path& file)
{
    std::error_code ec;
    const std::filesystem::path sourceRoot = ModelDirectory() / "Source" /
                                             (m_folder.empty() ? std::string("Weapons") : m_folder);

    // Already ours. Re-importing from inside the assets tree must not copy a file onto itself,
    // which on Windows truncates it.
    const std::filesystem::path canonicalFile = std::filesystem::weakly_canonical(file, ec);
    const std::filesystem::path canonicalRoot =
        std::filesystem::weakly_canonical(ModelDirectory(), ec);
    if (!canonicalRoot.empty() &&
        canonicalFile.string().rfind(canonicalRoot.string(), 0) == 0)
    {
        return {};
    }

    std::filesystem::create_directories(sourceRoot, ec);
    // Named after the model rather than after the download, because a download is called
    // "low-poly_colt_m5_scw.glb" and the thing it became is called something else entirely.
    const std::filesystem::path kept =
        sourceRoot / (m_saveName + file.extension().string());
    std::filesystem::copy_file(file, kept, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        PRED_LOG_WARN(Asset, "Could not keep a copy of {}: {}", file.string(), ec.message());
        return {};
    }
    PRED_LOG_INFO(Asset, "Kept the source art at {}", kept.string());
    return kept.string();
}

void ModelEditor::ImportMesh(const std::string& file)
{
    if (file.empty())
    {
        m_status = "Nothing to import: drop a file on the window, or paste a path";
        return;
    }

    const std::filesystem::path path = file;
    m_lastImportSource.clear();

    // A glTF file describes a whole model rather than one lump of geometry, so it arrives as a set
    // of parts. That is the difference that matters for a weapon: a magazine has to be a part of
    // its own or no reload can move it, and merging the file down to one mesh throws that away.
    if (IsGltfFile(path))
    {
        PushUndo("an import");
        GltfImportOptions options;
        options.targetSize = m_importSize;
        options.rotationDegrees = m_importRotation;
        options.centre = m_importCentre;

        ModelAsset imported;
        std::string error;
        if (!LoadGlbModel(path, options, imported, &error))
        {
            m_status = "Could not import: " + error;
            return;
        }

        if (m_importReplace)
        {
            // Sockets and clips survive a reimport. Turning a model the right way round usually
            // takes two or three goes, and losing the grip placement each time is what would make
            // that unbearable.
            m_model.parts = std::move(imported.parts);
            if (m_model.name.empty())
            {
                m_model.name = imported.name;
            }
        }
        else
        {
            for (ModelPart& part : imported.parts)
            {
                m_model.parts.push_back(std::move(part));
            }
        }

        m_selectedPart = m_model.parts.empty() ? -1 : 0;
        m_frameRequested = true;
        m_dirty = true;
        m_previewChanged = true;
        m_lastImportSource = KeepSource(path);
        m_status = "Imported " + std::to_string(imported.parts.size()) + " parts from " +
                   path.filename().string();
        return;
    }

    MeshData imported;
    if (!LoadObjMesh(path, imported))
    {
        m_status = "Could not import " + file;
        return;
    }

    ModelPart part;
    part.name = path.stem().string();
    part.shape = PartShape::Mesh;
    part.mesh = std::move(imported);
    part.sourceFile = file;
    part.size = glm::vec3(1.0f);
    NormalizeMeshScale(part.mesh, m_importSize);
    m_model.parts.push_back(std::move(part));
    m_selectedPart = static_cast<int>(m_model.parts.size()) - 1;
    m_dirty = true;
    m_previewChanged = true;
    m_geometryChanged = true;
    m_lastImportSource = KeepSource(path);
    m_status = "Imported " + path.filename().string();
}


// Turns, mirrors or rescales the whole model at once.
//
// A download arrives however its author left it: the two that turned up first ran along +X and the
// game wants the barrel down +Z. Turning forty parts by hand is not editing, and doing it at import
// only helps if the guess was right the first time.
//
// Sockets move with the geometry, or the grip ends up on the other side of the weapon from the hand.
// Which part a ray runs through, or -1.
//
// Tested against each part's box rather than against its triangles. A weapon has a few thousand
// triangles and a click has to answer instantly; boxes overlap a little, so the nearest hit wins,
// which is what picking the front-most thing means. Anything finer than this is only wanted for
// picking one blade of grass out of a field, which is not what an editor is for.
// --- Undo ---------------------------------------------------------------------------------------
//
// Whole copies of the model, which is the only kind of undo worth having in an editor this size.
// Every operation becomes undoable without each one having to describe its own inverse, and getting
// one of those inverses wrong is how an undo stack quietly corrupts the thing it is protecting.
//
// Imported geometry makes a copy expensive, so the depth is small and a copy is only taken when
// something is about to change. Dragging a slider is one entry rather than four hundred: the same
// operation arriving again within a moment is the same gesture continuing.
void ModelEditor::PushUndo(const char* what)
{
    PushSnapshot(m_model, what);
}

void ModelEditor::PushSnapshot(ModelAsset before, const char* what)
{
    m_undo.push_back({std::move(before), what});
    if (m_undo.size() > kUndoDepth)
    {
        m_undo.erase(m_undo.begin());
    }
    // Doing something new is what makes the way forward stop existing.
    m_redo.clear();
}

// A cheap summary of everything a person can change by hand: placements, colours, names, sockets and
// keyframes. Geometry is left out because it only changes on an import, which records its own undo.
//
// Used to tell whether a gesture actually altered anything. Grabbing a slider and letting go without
// moving it must not fill the undo stack with entries that undo nothing, which is the fastest way to
// make an undo stack useless.
size_t ModelEditor::Fingerprint() const
{
    size_t hash = 1469598103934665603ull;
    const auto mix = [&hash](size_t value)
    {
        hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    };
    const auto mixFloat = [&mix](float value)
    {
        // Quantised, so a value that merely redisplays itself does not read as a change.
        mix(static_cast<size_t>(static_cast<long long>(value * 100000.0f)));
    };

    mix(m_model.parts.size());
    mix(m_model.sockets.size());
    mix(m_model.clips.size());
    for (const ModelPart& part : m_model.parts)
    {
        mix(std::hash<std::string>{}(part.name));
        mix(static_cast<size_t>(part.shape));
        mix(part.visible ? 1u : 0u);
        mix(std::hash<std::string>{}(part.texture));
        for (int axis = 0; axis < 3; ++axis)
        {
            mixFloat(part.position[axis]);
            mixFloat(part.rotation[axis]);
            mixFloat(part.size[axis]);
            mixFloat(part.color[axis]);
        }
        mixFloat(part.roughness);
        mixFloat(part.metallic);
        mixFloat(part.emissive);
    }
    for (const ModelSocket& socket : m_model.sockets)
    {
        mix(std::hash<std::string>{}(socket.name));
        for (int axis = 0; axis < 3; ++axis)
        {
            mixFloat(socket.position[axis]);
            mixFloat(socket.rotation[axis]);
        }
    }
    for (const AnimationClip& clip : m_model.clips)
    {
        mix(std::hash<std::string>{}(clip.name));
        mixFloat(clip.duration);
        mix(clip.loop ? 1u : 0u);
        for (const AnimationTrack& track : clip.tracks)
        {
            mix(std::hash<std::string>{}(track.part));
            for (const AnimationKey& key : track.keys)
            {
                mixFloat(key.time);
                mixFloat(key.visible);
                mix(static_cast<size_t>(key.holder));
                mix(static_cast<size_t>(key.ease));
                for (int axis = 0; axis < 3; ++axis)
                {
                    mixFloat(key.position[axis]);
                    mixFloat(key.rotation[axis]);
                }
            }
        }
    }
    return hash;
}

bool ModelEditor::Undo()
{
    if (m_undo.empty())
    {
        return false;
    }
    m_redo.push_back({m_model, m_undo.back().what});
    m_status = std::string("Undid ") + m_undo.back().what;
    m_model = std::move(m_undo.back().model);
    m_undo.pop_back();

    // Whatever was selected may not exist any more.
    m_selectedPart = std::min(m_selectedPart, static_cast<int>(m_model.parts.size()) - 1);
    m_selectedSocket = std::min(m_selectedSocket, static_cast<int>(m_model.sockets.size()) - 1);
    m_selectedClip = std::min(m_selectedClip, static_cast<int>(m_model.clips.size()) - 1);
    m_dirty = true;
    m_geometryChanged = true;
    m_previewChanged = true;
    return true;
}

bool ModelEditor::Redo()
{
    if (m_redo.empty())
    {
        return false;
    }
    m_undo.push_back({m_model, m_redo.back().what});
    m_status = std::string("Redid ") + m_redo.back().what;
    m_model = std::move(m_redo.back().model);
    m_redo.pop_back();

    m_selectedPart = std::min(m_selectedPart, static_cast<int>(m_model.parts.size()) - 1);
    m_selectedSocket = std::min(m_selectedSocket, static_cast<int>(m_model.sockets.size()) - 1);
    m_selectedClip = std::min(m_selectedClip, static_cast<int>(m_model.clips.size()) - 1);
    m_geometryChanged = true;
    m_dirty = true;
    m_previewChanged = true;
    return true;
}

// --- Selection ----------------------------------------------------------------------------------

bool ModelEditor::SelectionPosition(glm::vec3& out) const
{
    if (m_pick == Pick::Key)
    {
        return KeyPosition(out);
    }
    if (m_pick == Pick::Part && m_selectedPart >= 0 &&
        m_selectedPart < static_cast<int>(m_model.parts.size()))
    {
        out = m_model.parts[static_cast<size_t>(m_selectedPart)].position;
        return true;
    }
    if (m_pick == Pick::Socket && m_selectedSocket >= 0 &&
        m_selectedSocket < static_cast<int>(m_model.sockets.size()))
    {
        out = m_model.sockets[static_cast<size_t>(m_selectedSocket)].position;
        return true;
    }
    return false;
}

void ModelEditor::MoveSelection(const glm::vec3& delta)
{
    if (m_pick == Pick::Key)
    {
        MoveKey(delta);
        return;
    }
    if (m_pick == Pick::Part && m_selectedPart >= 0 &&
        m_selectedPart < static_cast<int>(m_model.parts.size()))
    {
        m_model.parts[static_cast<size_t>(m_selectedPart)].position += delta;
        m_geometryChanged = true;
    }
    else if (m_pick == Pick::Socket && m_selectedSocket >= 0 &&
             m_selectedSocket < static_cast<int>(m_model.sockets.size()))
    {
        // Not geometry. A socket says where a hand goes; nothing about the model is drawn
        // differently for it, and rebuilding every mesh on every frame of the drag is what took the
        // frame rate down to single figures.
        m_model.sockets[static_cast<size_t>(m_selectedSocket)].position += delta;
    }
    else
    {
        return;
    }
    m_dirty = true;
    m_previewChanged = true;
}

// --- Dragging -----------------------------------------------------------------------------------
//
// Where a ray comes closest to running along an axis through a point, as a distance along that axis
// and how far the ray missed by. The whole of a move gizmo is this: grab the axis whose handle the
// pointer is nearest, then keep the grabbed point of it under the pointer.
namespace
{

struct AxisHit
{
    float along = 0.0f;
    float miss = std::numeric_limits<float>::max();
};

AxisHit ClosestOnAxis(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& point,
                      const glm::vec3& axis)
{
    AxisHit hit;
    const glm::vec3 toPoint = point - origin;
    const float axisDotRay = glm::dot(axis, direction);
    const float denominator = 1.0f - axisDotRay * axisDotRay;
    if (std::abs(denominator) < 1e-5f)
    {
        // The ray runs along the axis, so every point on it is equally close and dragging it means
        // nothing. Refused rather than answered with a number that will be enormous.
        return hit;
    }
    const float alongRay = glm::dot(toPoint, direction);
    const float alongAxis = glm::dot(toPoint, axis);
    hit.along = (alongAxis - axisDotRay * alongRay) / -denominator;
    const glm::vec3 onAxis = point + axis * hit.along;
    const float onRay = std::max(glm::dot(onAxis - origin, direction), 0.0f);
    hit.miss = glm::distance(onAxis, origin + direction * onRay);
    return hit;
}

} // namespace

bool ModelEditor::BeginDrag(const glm::vec3& origin, const glm::vec3& direction)
{
    glm::vec3 at{0.0f};
    if (!SelectionPosition(at))
    {
        return false;
    }

    // Whichever handle the pointer is nearest, so long as it is near one at all and the point
    // grabbed is on the handle rather than out along the line beyond it.
    int best = -1;
    float nearest = 0.018f; // how close counts as grabbing, in metres
    float grabbed = 0.0f;
    for (int axis = 0; axis < 3; ++axis)
    {
        glm::vec3 unit{0.0f};
        unit[axis] = 1.0f;
        const AxisHit hit = ClosestOnAxis(origin, direction, at, unit);
        if (hit.along < -0.01f || hit.along > kHandleLength || hit.miss >= nearest)
        {
            continue;
        }
        nearest = hit.miss;
        best = axis;
        grabbed = hit.along;
    }

    if (best >= 0)
    {
        PushUndo("a move");
        m_dragAxis = best;
        m_dragGrab = grabbed;
        return true;
    }

    // Or a ring: where the pointer's ray crosses each ring's plane, and how near that is to the ring.
    float nearestRing = 0.012f;
    for (int axis = 0; axis < 3; ++axis)
    {
        glm::vec3 normal{0.0f};
        normal[axis] = 1.0f;
        const float facing = glm::dot(direction, normal);
        if (std::abs(facing) < 1e-3f)
        {
            continue;
        }
        const float t = glm::dot(at - origin, normal) / facing;
        if (t <= 0.0f)
        {
            continue;
        }
        const glm::vec3 hit = origin + direction * t;
        const float off = std::abs(glm::distance(hit, at) - kRingRadius);
        if (off < nearestRing)
        {
            nearestRing = off;
            best = axis;
        }
    }
    if (best < 0)
    {
        return false;
    }
    PushUndo("a turn");
    m_dragAxis = best + 3;
    m_ringTurned = 0.0f;
    m_ringApplied = 0.0f;
    m_ringAngle = std::numeric_limits<float>::max();
    UpdateDrag(origin, direction);
    return true;
}

void ModelEditor::RotateSelection(const glm::vec3& axis, float radians)
{
    glm::vec3* euler = nullptr;
    glm::vec3 about = axis;
    if (m_pick == Pick::Key)
    {
        AnimationClip* clip = CurrentClip();
        AnimationTrack* track = SelectedTrack();
        AnimationKey* key = track != nullptr ? KeyAt(*track, m_playhead) : nullptr;
        if (clip == nullptr || key == nullptr)
        {
            return;
        }
        if (key->holder != PartHolder::Weapon && track->part != kLeftHandTrack && track->part != kRightHandTrack)
        {
            // Held in a hand whose frame may be turned: the axis turned into it, as a move is.
            const int side = key->holder == PartHolder::LeftHand ? 0 : 1;
            const glm::mat3 hand = glm::mat3(m_model.HandFrameAt(clip, side, m_playhead, m_model.HandRest(side)));
            about = glm::transpose(hand) * axis;
        }
        euler = &key->rotation;
    }
    else if (m_pick == Pick::Part && m_selectedPart >= 0 && m_selectedPart < static_cast<int>(m_model.parts.size()))
    {
        euler = &m_model.parts[static_cast<size_t>(m_selectedPart)].rotation;
        m_geometryChanged = true;
    }
    else if (m_pick == Pick::Socket && m_selectedSocket >= 0 &&
             m_selectedSocket < static_cast<int>(m_model.sockets.size()))
    {
        euler = &m_model.sockets[static_cast<size_t>(m_selectedSocket)].rotation;
    }
    if (euler == nullptr)
    {
        return;
    }
    // The same convention every rotation in a model is kept in: X, then Y, then Z.
    const glm::quat turned = glm::angleAxis(radians, glm::normalize(about)) * glm::quat(glm::radians(*euler));
    *euler = glm::degrees(glm::eulerAngles(turned));
    m_dirty = true;
    m_previewChanged = true;
}

bool ModelEditor::MirrorHands()
{
    AnimationClip* clip = CurrentClip();
    if (clip == nullptr)
    {
        return false;
    }
    PushUndo("mirroring the hands");
    // Reflected across the weapon's own middle, the plane its barrel and its up lie in: across x.
    const auto mirror = [](AnimationKey& key)
    {
        key.position.x = -key.position.x;
        key.rotation.y = -key.rotation.y;
        key.rotation.z = -key.rotation.z;
    };
    for (AnimationTrack& track : clip->tracks)
    {
        const bool left = track.part == kLeftHandTrack;
        const bool right = track.part == kRightHandTrack;
        if (left || right)
        {
            track.part = left ? kRightHandTrack : kLeftHandTrack;
            for (AnimationKey& key : track.keys)
            {
                mirror(key);
            }
            continue;
        }
        for (AnimationKey& key : track.keys)
        {
            if (key.holder == PartHolder::LeftHand || key.holder == PartHolder::RightHand)
            {
                key.holder = key.holder == PartHolder::LeftHand ? PartHolder::RightHand : PartHolder::LeftHand;
                mirror(key);
            }
        }
    }
    m_dirty = true;
    m_previewChanged = true;
    m_status = "Mirrored the hands in '" + clip->name + "'.";
    return true;
}

void ModelEditor::UpdateDrag(const glm::vec3& origin, const glm::vec3& direction)
{
    if (m_dragAxis < 0)
    {
        return;
    }
    glm::vec3 at{0.0f};
    if (!SelectionPosition(at))
    {
        EndDrag();
        return;
    }

    if (m_dragAxis >= 3)
    {
        // Round a ring: the angle of where the pointer crosses the ring's plane, and how far it has come
        // round since last time. Whole turns are followed, because the difference is taken each frame.
        const int axis = m_dragAxis - 3;
        glm::vec3 normal{0.0f};
        normal[axis] = 1.0f;
        const float facing = glm::dot(direction, normal);
        if (std::abs(facing) < 1e-3f)
        {
            return;
        }
        const glm::vec3 hit = origin + direction * (glm::dot(at - origin, normal) / facing);
        glm::vec3 u{0.0f};
        glm::vec3 v{0.0f};
        u[(axis + 1) % 3] = 1.0f;
        v[(axis + 2) % 3] = 1.0f;
        const glm::vec3 across = hit - at;
        const float angle = std::atan2(glm::dot(across, v), glm::dot(across, u));
        if (m_ringAngle == std::numeric_limits<float>::max())
        {
            m_ringAngle = angle;
            return;
        }
        m_ringTurned += std::remainder(angle - m_ringAngle, glm::two_pi<float>());
        m_ringAngle = angle;
        const float step = glm::radians(5.0f);
        const float wanted = m_snapEnabled ? std::round(m_ringTurned / step) * step : m_ringTurned;
        if (wanted != m_ringApplied)
        {
            RotateSelection(normal, wanted - m_ringApplied);
            m_ringApplied = wanted;
        }
        return;
    }

    glm::vec3 unit{0.0f};
    unit[m_dragAxis] = 1.0f;
    const AxisHit hit = ClosestOnAxis(origin, direction, at, unit);
    if (hit.miss >= std::numeric_limits<float>::max())
    {
        return; // looking straight down the axis, so there is nothing to follow
    }

    // The grabbed point stays under the pointer, which is the difference between dragging something
    // and throwing it: without this the thing jumps so its origin is where the pointer is.
    float wanted = at[m_dragAxis] + (hit.along - m_dragGrab);
    if (m_snapEnabled)
    {
        // Snapped on the result rather than on the movement, or a drag that starts off the grid
        // stays off it forever and the snapping does nothing anyone can see.
        wanted = SnapTo(wanted, m_gridSnap);
    }

    glm::vec3 delta{0.0f};
    delta[m_dragAxis] = wanted - at[m_dragAxis];
    MoveSelection(delta);
}

// Where to stand to see the whole model, and what to look at.
//
// Asked for rather than done, because the game owns the camera: the editor knows how big the model
// is and nothing else does. Requested whenever a model is opened or imported, because a model that
// arrives off screen or a hundred times too large looks exactly like one that failed to load, and
// the first thing anyone does either way is hunt for it.
bool ModelEditor::TakeFrameRequest(glm::vec3& outPosition, glm::vec3& outTarget)
{
    if (!m_frameRequested)
    {
        return false;
    }
    m_frameRequested = false;

    AABB bounds;
    bool any = false;
    for (const ModelPart& part : m_model.parts)
    {
        if (!part.visible)
        {
            continue;
        }
        AABB partBounds;
        if (part.shape == PartShape::Mesh)
        {
            if (part.mesh.vertices.empty())
            {
                continue;
            }
            partBounds = part.mesh.ComputeBounds();
        }
        else
        {
            partBounds.min = part.size * -0.5f;
            partBounds.max = part.size * 0.5f;
        }
        const glm::mat4 local = part.LocalMatrix();
        // The eight corners, because a turned box's bounds are not its bounds turned.
        for (int corner = 0; corner < 8; ++corner)
        {
            const glm::vec3 point{(corner & 1) ? partBounds.max.x : partBounds.min.x,
                                  (corner & 2) ? partBounds.max.y : partBounds.min.y,
                                  (corner & 4) ? partBounds.max.z : partBounds.min.z};
            const glm::vec3 world = glm::vec3(local * glm::vec4(point, 1.0f));
            bounds.min = any ? glm::min(bounds.min, world) : world;
            bounds.max = any ? glm::max(bounds.max, world) : world;
            any = true;
        }
    }

    if (!any)
    {
        outTarget = glm::vec3(0.0f, 0.3f, 0.0f);
        outPosition = outTarget + glm::vec3(0.35f, 0.25f, 0.8f);
        return true;
    }

    outTarget = (bounds.min + bounds.max) * 0.5f;
    const glm::vec3 extent = bounds.max - bounds.min;
    const float size = std::max({extent.x, extent.y, extent.z, 0.05f});
    // Far enough back that the longest side fits comfortably across the view, from three quarters
    // on, which is the angle anything is easiest to judge from.
    const float distance = size * 1.6f;
    outPosition = outTarget + glm::normalize(glm::vec3(0.55f, 0.42f, 1.0f)) * distance;
    return true;
}

int ModelEditor::PartUnderRay(const glm::vec3& origin, const glm::vec3& direction) const
{
    int best = -1;
    float nearest = std::numeric_limits<float>::max();

    for (size_t i = 0; i < m_model.parts.size(); ++i)
    {
        const ModelPart& part = m_model.parts[i];
        if (!part.visible)
        {
            continue; // you cannot click what you cannot see
        }

        // The ray is put into the part's own frame rather than the box into the world's, so a part
        // turned on its side is still tested against the box it actually occupies.
        const glm::mat4 inverse = glm::inverse(part.LocalMatrix());
        const glm::vec3 from = glm::vec3(inverse * glm::vec4(origin, 1.0f));
        const glm::vec3 along = glm::vec3(inverse * glm::vec4(direction, 0.0f));

        glm::vec3 low{-0.5f};
        glm::vec3 high{0.5f};
        if (part.shape == PartShape::Mesh)
        {
            if (part.mesh.vertices.empty())
            {
                continue;
            }
            const AABB bounds = part.mesh.ComputeBounds();
            low = bounds.min;
            high = bounds.max;
        }
        else
        {
            low = part.size * -0.5f;
            high = part.size * 0.5f;
        }

        // Slab test. A tiny thickness is added, or a part modelled flat is unclickable.
        constexpr float kMinimumThickness = 0.002f;
        low -= glm::vec3(kMinimumThickness);
        high += glm::vec3(kMinimumThickness);

        float enter = 0.0f;
        float exit = std::numeric_limits<float>::max();
        bool missed = false;
        for (int axis = 0; axis < 3 && !missed; ++axis)
        {
            if (std::abs(along[axis]) < 1e-8f)
            {
                missed = from[axis] < low[axis] || from[axis] > high[axis];
                continue;
            }
            const float inverseAlong = 1.0f / along[axis];
            float first = (low[axis] - from[axis]) * inverseAlong;
            float second = (high[axis] - from[axis]) * inverseAlong;
            if (first > second)
            {
                std::swap(first, second);
            }
            enter = std::max(enter, first);
            exit = std::min(exit, second);
            missed = enter > exit;
        }
        if (missed || exit < 0.0f)
        {
            continue;
        }
        if (enter < nearest)
        {
            nearest = enter;
            best = static_cast<int>(i);
        }
    }
    return best;
}

bool ModelEditor::SelectUnderRay(const glm::vec3& origin, const glm::vec3& direction)
{
    // Sockets first, and generously. They are the smallest things on a model and the ones most
    // often wanted, and a socket sitting on the surface of a part loses every tie against it.
    int bestSocket = -1;
    float nearestSocket = std::numeric_limits<float>::max();
    constexpr float kSocketRadius = 0.025f;
    for (size_t i = 0; i < m_model.sockets.size(); ++i)
    {
        const glm::vec3 toSocket = m_model.sockets[i].position - origin;
        const float along = glm::dot(toSocket, direction);
        if (along < 0.0f)
        {
            continue; // behind the eye
        }
        const float miss = glm::length(toSocket - direction * along);
        if (miss < kSocketRadius && along < nearestSocket)
        {
            nearestSocket = along;
            bestSocket = static_cast<int>(i);
        }
    }

    if (bestSocket >= 0)
    {
        const bool changed = m_pick != Pick::Socket || m_selectedSocket != bestSocket;
        m_pick = Pick::Socket;
        m_selectedSocket = bestSocket;
        m_status = "Selected the " + m_model.sockets[static_cast<size_t>(bestSocket)].name + " socket";
        return changed;
    }

    const int hit = PartUnderRay(origin, direction);
    const bool changed = m_pick != (hit >= 0 ? Pick::Part : Pick::None) || m_selectedPart != hit;
    m_selectedPart = hit;
    m_pick = hit >= 0 ? Pick::Part : Pick::None;
    m_status = hit >= 0 ? "Selected " + m_model.parts[static_cast<size_t>(hit)].name
                        : std::string("Nothing selected");
    return changed;
}

// The box a part actually occupies, in its own frame. An imported mesh keeps its size at one, which
// is not its size at all: highlighting by that drew a metre of outline around six centimetres of
// barrel and made the selection useless for seeing what was selected.
AABB ModelEditor::PartBounds(const ModelPart& part)
{
    AABB bounds;
    if (part.shape == PartShape::Mesh)
    {
        if (!part.mesh.vertices.empty())
        {
            bounds = part.mesh.ComputeBounds();
        }
        return bounds;
    }
    bounds.min = part.size * -0.5f;
    bounds.max = part.size * 0.5f;
    return bounds;
}

void ModelEditor::TransformModel(const glm::mat4& transform)
{
    PushUndo("turning the model");
    const glm::mat3 normalMatrix = glm::inverseTranspose(glm::mat3(transform));
    // A mirror turns triangles inside out, so their winding has to be turned back.
    const bool mirrors = glm::determinant(glm::mat3(transform)) < 0.0f;

    for (ModelPart& part : m_model.parts)
    {
        part.position = glm::vec3(transform * glm::vec4(part.position, 1.0f));
        for (MeshVertex& vertex : part.mesh.vertices)
        {
            vertex.position = glm::vec3(transform * glm::vec4(vertex.position, 1.0f));
            const glm::vec3 normal = normalMatrix * vertex.normal;
            vertex.normal =
                glm::length(normal) > 1e-6f ? glm::normalize(normal) : glm::vec3(0.0f, 1.0f, 0.0f);
        }
        if (mirrors)
        {
            for (size_t i = 0; i + 2 < part.mesh.indices.size(); i += 3)
            {
                std::swap(part.mesh.indices[i + 1], part.mesh.indices[i + 2]);
            }
        }
    }
    for (ModelSocket& socket : m_model.sockets)
    {
        socket.position = glm::vec3(transform * glm::vec4(socket.position, 1.0f));
    }
    m_geometryChanged = true;

    m_dirty = true;
    m_previewChanged = true;
}

void ModelEditor::DrawModelPanel()
{
    if (ImGui::Button("Frame it"))
    {
        FrameModel();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Everything below is all parts and sockets at once.");

    // Which way round the weapon is, judged by the only thing that decides it: the game holds a
    // weapon by its grip and sends rounds out of its muzzle, so the muzzle has to be in front of the
    // grip down +Z. Everything else about the model can be however its author left it.
    {
        const ModelSocket* grip = m_model.FindSocket("grip");
        const ModelSocket* muzzle = m_model.FindSocket("muzzle");
        if (grip != nullptr && muzzle != nullptr && muzzle->position.z <= grip->position.z)
        {
            ImGui::TextColored({0.95f, 0.55f, 0.45f, 1.0f},
                               "This model faces backwards: its muzzle socket is behind its grip, so "
                               "the game will hold it by the barrel. Turn everything round, which "
                               "moves the geometry and the sockets together:");
            if (ImGui::Button("Turn it round"))
            {
                PushUndo("turning the model round");
                TransformModel(glm::rotate(glm::mat4(1.0f), glm::pi<float>(), {0.0f, 1.0f, 0.0f}));
                m_status = "Turned the model round";
            }
            ImGui::SameLine();
            ImGui::TextDisabled("or set the grip socket's Turn to 0, 180, 0 to leave the geometry "
                                "alone and turn it in the hand instead.");
        }
    }

    // Quarter turns, because that is what a wrongly exported model needs. Anything else is a job for
    // the part inspector.
    const auto turn = [&](const char* label, const glm::vec3& axis, float degrees)
    {
        if (ImGui::Button(label))
        {
            TransformModel(glm::rotate(glm::mat4(1.0f), glm::radians(degrees), axis));
            m_status = "Turned the model";
        }
    };
    turn("Turn left", {0.0f, 1.0f, 0.0f}, 90.0f);
    ImGui::SameLine();
    turn("Turn right", {0.0f, 1.0f, 0.0f}, -90.0f);
    ImGui::SameLine();
    turn("Turn around", {0.0f, 1.0f, 0.0f}, 180.0f);

    turn("Tip forward", {1.0f, 0.0f, 0.0f}, -90.0f);
    ImGui::SameLine();
    turn("Tip back", {1.0f, 0.0f, 0.0f}, 90.0f);
    ImGui::SameLine();
    turn("Roll", {0.0f, 0.0f, 1.0f}, 90.0f);

    if (ImGui::Button("Mirror left to right"))
    {
        TransformModel(glm::scale(glm::mat4(1.0f), {-1.0f, 1.0f, 1.0f}));
        m_status = "Mirrored the model";
    }

    ImGui::Separator();

    // Where it sits and how big it is. Both are measured from what is there rather than typed in
    // blind, because the useful operations are "put the origin at the grip" and "make it this long".
    AABB bounds;
    bool first = true;
    for (const ModelPart& part : m_model.parts)
    {
        if (part.mesh.vertices.empty())
        {
            continue;
        }
        const AABB partBounds = part.mesh.ComputeBounds();
        bounds.min = first ? partBounds.min : glm::min(bounds.min, partBounds.min);
        bounds.max = first ? partBounds.max : glm::max(bounds.max, partBounds.max);
        first = false;
    }
    if (first)
    {
        ImGui::TextDisabled("Nothing with geometry in this model yet.");
        return;
    }

    const glm::vec3 extent = bounds.max - bounds.min;
    ImGui::Text("Size: %.3f wide, %.3f tall, %.3f long", extent.x, extent.y, extent.z);
    ImGui::Text("Origin sits %.3f, %.3f, %.3f from the middle", -(bounds.min.x + bounds.max.x) * 0.5f,
                -(bounds.min.y + bounds.max.y) * 0.5f, -(bounds.min.z + bounds.max.z) * 0.5f);

    ImGui::SetNextItemWidth(90.0f);
    ImGui::DragFloat("##fit", &m_importSize, 0.01f, 0.05f, 4.0f, "%.2f");
    ImGui::SameLine();
    if (ImGui::Button("Fit longest side to this"))
    {
        const float longest = std::max({extent.x, extent.y, extent.z});
        if (longest > 1e-5f)
        {
            TransformModel(glm::scale(glm::mat4(1.0f), glm::vec3(m_importSize / longest)));
            m_status = "Rescaled the model";
        }
    }

    if (ImGui::Button("Move the origin to the middle"))
    {
        TransformModel(glm::translate(glm::mat4(1.0f), -(bounds.min + bounds.max) * 0.5f));
        m_status = "Recentred the model";
    }
    ImGui::SameLine();
    if (ImGui::Button("Sit it on the floor"))
    {
        TransformModel(glm::translate(glm::mat4(1.0f), {0.0f, -bounds.min.y, 0.0f}));
        m_status = "Dropped the model onto the floor";
    }

    if (m_selectedPart >= 0 && m_selectedPart < static_cast<int>(m_model.parts.size()))
    {
        if (ImGui::Button("Move the origin to the selected part"))
        {
            TransformModel(
                glm::translate(glm::mat4(1.0f), -m_model.parts[static_cast<size_t>(m_selectedPart)].position));
            m_status = "Moved the origin";
        }
    }
}

void ModelEditor::DrawUi(Scene& scene, MeshLibrary& meshes)
{
    if (!m_open)
    {
        return;
    }

    ImGui::SetNextWindowSize({470.0f, 720.0f}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Model editor"))
    {
        // Everything in this window wraps to its width. The panel is mostly explanation and a
        // sentence cut off at the right edge teaches nothing; the Controls table opts back out
        // because it is aligned in columns that wrapping would scramble.
        ImGui::PushTextWrapPos(0.0f);
        if (ImGui::CollapsingHeader("File", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawFilePanel(scene, meshes);
        }
        if (ImGui::CollapsingHeader("Model"))
        {
            DrawModelPanel();
        }
        if (ImGui::CollapsingHeader("Parts", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawPartList();
            ImGui::Separator();
            DrawPartInspector();
        }
        if (ImGui::CollapsingHeader("Sockets"))
        {
            DrawSocketPanel();
        }
        if (ImGui::CollapsingHeader("Animation"))
        {
            DrawAnimationPanel();
        }
        if (ImGui::CollapsingHeader("View"))
        {
            ImGui::Checkbox("Snap to grid", &m_snapEnabled);
            ImGui::DragFloat("Snap step", &m_gridSnap, 0.001f, 0.001f, 0.1f, "%.3f m");
            if (ImGui::Checkbox("Show sockets", &m_showSockets))
            {
                m_dirty = true;
                m_previewChanged = true;
            }
            ImGui::TextDisabled("WASD to move, Q and E for down and up, right mouse to look.");
        }
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
}

} // namespace pred
