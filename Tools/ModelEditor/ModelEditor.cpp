#include "Tools/ModelEditor/ModelEditor.h"

#include "Engine/Application.h"
#include "Engine/Core/Log.h"
#include "Engine/Render/DebugDraw.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Assets/MeshImport.h"
#include "Engine/Render/Primitives.h"

#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>

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
    AddPart("body", PartShape::Box);
}

void ModelEditor::AddPart(const char* name, PartShape shape)
{
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
    m_dirty = true;
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
    m_selectedPart = m_model.parts.empty() ? -1 : 0;
    m_selectedSocket = -1;
    m_selectedClip = m_model.clips.empty() ? -1 : 0;
    m_dirty = true;
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

    if (m_selectedPart >= 0 && m_selectedPart < static_cast<int>(m_model.parts.size()))
    {
        const ModelPart& part = m_model.parts[static_cast<size_t>(m_selectedPart)];
        draw.BoxOriented(part.LocalMatrix(), part.size * 0.5f, Color::kYellow);
    }
    for (const ModelSocket& socket : m_model.sockets)
    {
        draw.Axes(glm::translate(glm::mat4(1.0f), socket.position), 0.05f);
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
    ImGui::TextDisabled("OBJ files. Paste a path, including one from a Sketchfab download.");

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
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (m_selectedPart >= 0 && m_selectedPart < static_cast<int>(m_model.parts.size()))
    {
        if (ImGui::Button("Duplicate"))
        {
            ModelPart copy = m_model.parts[static_cast<size_t>(m_selectedPart)];
            copy.name += "_copy";
            m_model.parts.push_back(std::move(copy));
            m_selectedPart = static_cast<int>(m_model.parts.size()) - 1;
            m_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete"))
        {
            m_model.parts.erase(m_model.parts.begin() + m_selectedPart);
            m_selectedPart = std::min(m_selectedPart, static_cast<int>(m_model.parts.size()) - 1);
            m_dirty = true;
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
    ImGui::TextDisabled("Sockets are what the game asks for by name: grip, support, muzzle, magazine.");
    if (ImGui::Button("Add socket"))
    {
        ModelSocket socket;
        socket.name = "socket_" + std::to_string(m_model.sockets.size() + 1);
        m_model.sockets.push_back(socket);
        m_selectedSocket = static_cast<int>(m_model.sockets.size()) - 1;
        m_dirty = true;
    }

    for (int i = 0; i < static_cast<int>(m_model.sockets.size()); ++i)
    {
        ModelSocket& socket = m_model.sockets[static_cast<size_t>(i)];
        ImGui::PushID(1000 + i);
        char nameBuffer[64];
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", socket.name.c_str());
        if (ImGui::InputText("##socketname", nameBuffer, sizeof(nameBuffer)))
        {
            socket.name = nameBuffer;
        }
        DragVec3("##socketpos", socket.position, 0.005f);
        ImGui::SameLine();
        if (ImGui::Button("x"))
        {
            m_model.sockets.erase(m_model.sockets.begin() + i);
            m_dirty = true;
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
    if (ImGui::Button("New clip"))
    {
        AnimationClip clip;
        clip.name = "clip_" + std::to_string(m_model.clips.size() + 1);
        clip.duration = 1.5f;
        m_model.clips.push_back(std::move(clip));
        m_selectedClip = static_cast<int>(m_model.clips.size()) - 1;
        m_selectedTrack = -1;
        m_playhead = 0.0f;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Name one reload or equip and the game plays it at the right moment.");

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
    MeshData imported;
    if (!LoadObjMesh(file, imported))
    {
        m_status = "Could not import " + file;
        return;
    }

    ModelPart part;
    part.name = std::filesystem::path(file).stem().string();
    part.shape = PartShape::Mesh;
    part.mesh = std::move(imported);
    part.sourceFile = file;
    part.size = glm::vec3(1.0f);
    m_model.parts.push_back(std::move(part));
    m_selectedPart = static_cast<int>(m_model.parts.size()) - 1;
    m_dirty = true;
    m_status = "Imported " + part.sourceFile;
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
            }
            ImGui::TextDisabled("Right mouse to look, WASD to move, Space and Ctrl for up and down.");
        }
    }
    ImGui::End();
}

} // namespace pred
