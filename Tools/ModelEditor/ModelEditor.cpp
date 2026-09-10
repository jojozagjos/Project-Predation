#include "Tools/ModelEditor/ModelEditor.h"

#include "Engine/Application.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Engine/Render/DebugDraw.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Assets/MeshImport.h"
#include "Engine/Render/Primitives.h"

#include <imgui.h>

#include <glm/gtc/matrix_inverse.hpp>
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
}

bool ModelEditor::Load(const std::string& modelName)
{
    ModelAsset loaded;
    if (!loaded.LoadFromFile(ModelDirectory() / (modelName + ".json")))
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
    m_status = "Loaded " + modelName;
    return true;
}

bool ModelEditor::Save()
{
    m_model.name = m_saveName;
    if (!m_model.SaveToFile(ModelDirectory() / (m_saveName + ".json")))
    {
        m_status = "Could not save " + m_saveName;
        return false;
    }
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
        draw.BoxOriented(part.LocalMatrix() * glm::translate(glm::mat4(1.0f), centre), half,
                         Color::kYellow);
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
        ImGui::TextDisabled("Right mouse    look around, WASD to move while held");
        ImGui::TextDisabled("Left click     select a part, or a socket if one is under it");
        ImGui::TextDisabled("Drag a handle  move the selected thing along that axis");
        ImGui::TextDisabled("Ctrl+Z         undo      Ctrl+Y  redo");
        ImGui::TextDisabled("F              put the view back on the model");
        ImGui::TextDisabled("Escape         back to the menu");
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

    // Importing is by path rather than a file dialog: the engine has no native dialog yet, and a
    // path pasted from a download folder is what actually happens in practice.
    char importBuffer[512];
    std::snprintf(importBuffer, sizeof(importBuffer), "%s", m_importPath.c_str());
    if (ImGui::InputText("Import path", importBuffer, sizeof(importBuffer)))
    {
        m_importPath = importBuffer;
    }
    ImGui::SameLine();
    if (ImGui::Button("Import"))
    {
        ImportMesh(m_importPath);
    }
    ImGui::TextDisabled("OBJ or GLB. Paste a path, including one from a Sketchfab download.");

    // How a download is turned into something the game can hold. All four are wanted before the
    // first look rather than after it: a model that arrives a hundred times too large and facing
    // sideways is indistinguishable from one that failed to load.
    ImGui::DragFloat("Fit to (m)", &m_importSize, 0.01f, 0.05f, 4.0f, "%.2f");
    ImGui::DragFloat3("Turn (deg)", &m_importRotation.x, 1.0f, -180.0f, 180.0f, "%.0f");
    ImGui::Checkbox("Centre on import", &m_importCentre);
    ImGui::SameLine();
    ImGui::Checkbox("Replace parts", &m_importReplace);
    ImGui::TextDisabled("The game wants the barrel down +Z and the grip at the origin.");
    ImGui::TextDisabled("Replacing keeps sockets and clips, so turning it right takes a few goes.");

    if (!m_status.empty())
    {
        ImGui::TextColored({0.75f, 0.82f, 0.68f, 1.0f}, "%s", m_status.c_str());
    }
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

    ImGui::BeginChild("##parts", {0.0f, 150.0f}, ImGuiChildFlags_Borders);
    for (int i = 0; i < static_cast<int>(m_model.parts.size()); ++i)
    {
        ModelPart& part = m_model.parts[static_cast<size_t>(i)];
        ImGui::PushID(i);
        bool visible = part.visible;
        if (ImGui::Checkbox("##visible", &visible))
        {
            part.visible = visible;
        }
        ImGui::SameLine();
        if (ImGui::Selectable(part.name.c_str(), m_selectedPart == i))
        {
            m_selectedPart = i;
            m_pick = Pick::Part;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (m_selectedPart >= 0 && m_selectedPart < static_cast<int>(m_model.parts.size()))
    {
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
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete"))
        {
            PushUndo("a delete");
            m_model.parts.erase(m_model.parts.begin() + m_selectedPart);
            m_selectedPart = std::min(m_selectedPart, static_cast<int>(m_model.parts.size()) - 1);
            m_dirty = true;
    m_previewChanged = true;
        }
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
        part.name = nameBuffer;
    }

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
    ImGui::TextDisabled("Sockets are what the game asks for by name:");
    ImGui::TextDisabled("grip and support are where the hands close, muzzle is where");
    ImGui::TextDisabled("rounds appear, magazine is where the magazine seats, sight is");
    ImGui::TextDisabled("the line aiming puts on the view axis.");
    ImGui::TextDisabled("Click one in the viewport to select it, then drag a handle.");
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
    // A new key starts from where the track already is at this moment, so adding one never moves
    // anything: it pins down what is already on screen and then you change it.
    AnimationKey key;
    key.time = time;
    if (!track.keys.empty())
    {
        const ModelPart* part = nullptr;
        for (const ModelPart& candidate : m_model.parts)
        {
            if (candidate.name == track.part)
            {
                part = &candidate;
                break;
            }
        }
        if (part != nullptr && m_selectedClip >= 0)
        {
            const AnimationClip& clip = m_model.clips[static_cast<size_t>(m_selectedClip)];
            const glm::mat4 local = m_model.PartMatrixAt(*part, &clip, time, &key.visible);
            key.position = glm::vec3(local[3]) - part->position;
        }
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
    m_status = "Keyed every part";
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
        if (ImGui::IsItemActive())
        {
            const float local = (ImGui::GetIO().MousePos.x - origin.x) / width;
            m_playhead = std::clamp(local, 0.0f, 1.0f) * duration;
            m_selectedTrack = static_cast<int>(t);
            m_playing = false;
        }

        for (const AnimationKey& key : track.keys)
        {
            const float x = origin.x + width * std::clamp(key.time / duration, 0.0f, 1.0f);
            const float y = origin.y + kRowHeight * 0.5f;
            const bool onPlayhead = std::abs(key.time - m_playhead) < 1e-3f;
            draw->AddCircleFilled({x, y}, 5.0f, onPlayhead && isSelected ? keySelected : keyColor, 8);
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
    ImGui::TextDisabled("The game plays reload, equip and fire at the right moments. Anything");
    ImGui::TextDisabled("else is yours to play from the Hold it panel. A clip replaces the");
    ImGui::TextDisabled("built-in movement of the same name rather than adding to it.");

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
    if (ImGui::BeginCombo("Animate part", "add a track"))
    {
        // "root" moves the whole weapon rather than one part, which is what an equip needs.
        if (ImGui::Selectable("root (the whole model)"))
        {
            TrackFor(*clip, "root");
            m_selectedTrack = static_cast<int>(clip->tracks.size()) - 1;
        }
        for (const ModelPart& part : m_model.parts)
        {
            if (ImGui::Selectable(part.name.c_str()))
            {
                TrackFor(*clip, part.name);
                m_selectedTrack = static_cast<int>(clip->tracks.size()) - 1;
            }
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
    ImGui::SliderFloat("Visible", &key->visible, 0.0f, 1.0f);
    ImGui::TextDisabled("Offsets are from the part's rest pose, so editing the model keeps the clip.");
}

void ModelEditor::ImportMesh(const std::string& file)
{
    if (file.empty())
    {
        m_status = "Nothing to import: paste a path first";
        return;
    }

    const std::filesystem::path path = file;

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
    m_status = "Imported " + file;
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
    m_dirty = true;
    m_previewChanged = true;
    return true;
}

// --- Selection ----------------------------------------------------------------------------------

bool ModelEditor::SelectionPosition(glm::vec3& out) const
{
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
    if (m_pick == Pick::Part && m_selectedPart >= 0 &&
        m_selectedPart < static_cast<int>(m_model.parts.size()))
    {
        m_model.parts[static_cast<size_t>(m_selectedPart)].position += delta;
    }
    else if (m_pick == Pick::Socket && m_selectedSocket >= 0 &&
             m_selectedSocket < static_cast<int>(m_model.sockets.size()))
    {
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

    if (best < 0)
    {
        return false;
    }

    PushUndo("a move");
    m_dragAxis = best;
    m_dragGrab = grabbed;
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

    ImGui::SetNextWindowSize({430.0f, 700.0f}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Model editor"))
    {
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
            ImGui::TextDisabled("Right mouse to look, WASD to move, Space and Ctrl for up and down.");
        }
    }
    ImGui::End();
}

} // namespace pred
