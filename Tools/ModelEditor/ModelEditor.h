#pragma once

#include "Engine/Assets/GltfImport.h"
#include "Engine/Assets/ModelAsset.h"
#include "Engine/Render/Camera.h"
#include "Engine/Scene/Scene.h"

#include <string>
#include <utility>
#include <vector>

namespace pred
{

class Application;
class DebugDraw;
class MeshLibrary;

// The in-engine model and animation editor.
//
// It runs inside the game rather than as a separate program, which is the whole point: it draws
// with the same renderer, the same shader and the same lighting the game uses, so what is built
// here is what appears in the world. There is no export step and no second definition of what a
// model is.
//
// What it edits is a ModelAsset: a list of primitive or imported parts, named sockets that tell the
// game where the hands and the muzzle go, and animation clips that move parts about. Toggle it with
// the `editor` console command or start with --editor.
class ModelEditor
{
public:
    void Init(Application& app);
    void Shutdown(Scene& scene);

    bool IsOpen() const { return m_open; }
    void SetOpen(Scene& scene, bool open);

    // Per-frame. Rebuilds the preview only when something actually changed, because uploading a
    // mesh every frame while a slider is dragged is how an editor comes to feel slow.
    void Update(Scene& scene, MeshLibrary& meshes, float dt);
    void DrawUi(Scene& scene, MeshLibrary& meshes);
    void DrawOverlays(DebugDraw& draw) const;

    // Where the editor camera is looking. The game hands this to the renderer while the editor owns
    // the view.
    const FlyCamera& Camera() const { return m_camera; }
    FlyCamera& Camera() { return m_camera; }

    float CameraSpeed() const { return m_cameraSpeed; }

    bool Load(const std::string& modelName);
    bool Save();

    // Selects whatever a ray runs through, and returns true if that changed anything. Clicking
    // nothing clears the selection, which is what clicking nothing means everywhere else.
    bool SelectUnderRay(const glm::vec3& origin, const glm::vec3& direction);
    // Asks for the view to be put on the model. The game owns the camera angles, so the editor can
    // only ask; it returns where to stand and what to look at, or false when nothing has asked.
    bool TakeFrameRequest(glm::vec3& outPosition, glm::vec3& outTarget);
    // Asks for it now.
    void FrameModel() { m_frameRequested = true; }

    // Which part a ray runs through, or -1. The ray comes from the game, which owns the camera and
    // the pointer; the editor owns the parts and is the only thing that can say what was hit.
    int PartUnderRay(const glm::vec3& origin, const glm::vec3& direction) const;
    // Turns, mirrors or rescales everything at once: geometry, part placements and sockets. A
    // download arrives however its author left it, and turning forty parts by hand is not editing.
    void TransformModel(const glm::mat4& transform);

    // What is open, so something outside can hold it.
    const ModelAsset& Model() const { return m_model; }
    // Puts a model in without touching the disk or the scene. Only tests use this: everything the
    // editor does to a model is worth checking, and none of it needs a renderer.
    void SetModelForTesting(ModelAsset model)
    {
        m_model = std::move(model);
        m_selectedPart = m_model.parts.empty() ? -1 : 0;
        m_dirty = true;
        m_previewChanged = true;
    }
    // True once each time the model has changed since this was last asked. The preview rebuilds on
    // it: a weapon visual is meshes and materials, and building one every frame while a slider is
    // dragged is how an editor comes to feel slow.
    bool TakePreviewChanged()
    {
        const bool changed = m_previewChanged;
        m_previewChanged = false;
        return changed;
    }

private:
    void Rebuild(Scene& scene, MeshLibrary& meshes);
    void NewModel();
    void AddPart(const char* name, PartShape shape);
    void ImportMesh(const std::string& file);
    void DrawModelPanel();
    void DrawPartList();
    void DrawPartInspector();
    void DrawSocketPanel();
    void DrawAnimationPanel();
    void DrawFilePanel(Scene& scene, MeshLibrary& meshes);
    AnimationClip* CurrentClip();
    // Writes the current offsets of every part into the selected clip at the playhead, which is how
    // a pose becomes a keyframe.
    void KeyAllParts();
    void DrawTimeline(AnimationClip& clip);
    AnimationTrack& TrackFor(AnimationClip& clip, const std::string& partName);
    AnimationKey* KeyAt(AnimationTrack& track, float time);
    AnimationKey& AddOrGetKey(AnimationTrack& track, float time);
    AnimationTrack* SelectedTrack();

    Application* m_app = nullptr;
    ModelAsset m_model;
    bool m_open = false;
    bool m_dirty = true; // the preview needs rebuilding
    bool m_previewChanged = true; // and whatever is holding it needs rebuilding too
    bool m_frameRequested = true; // put the view on the model at the next opportunity

    // One entity per part, so a part can be hidden or animated on its own.
    std::vector<Entity> m_entities;
    std::vector<Entity> m_socketEntities;

    int m_selectedPart = -1;
    int m_selectedSocket = -1;
    int m_selectedClip = -1;
    int m_selectedTrack = -1;

    float m_playhead = 0.0f;
    bool m_playing = false;

    FlyCamera m_camera;
    float m_cameraSpeed = 1.4f;
    float m_gridSnap = 0.005f;
    bool m_snapEnabled = true;
    bool m_showSockets = true;

    std::string m_status;
    std::string m_saveName = "new_model";
    std::string m_importPath;
    // How a download is turned into something a person can hold: the size it is fitted to, the turn
    // that puts the barrel down +Z, whether the origin is moved to the middle, and whether an
    // import replaces the parts or adds to them. Replacing keeps sockets and clips, because getting
    // the turn right takes a few goes and losing the grip placement each time would be unbearable.
    float m_importSize = 0.6f;
    glm::vec3 m_importRotation{0.0f};
    bool m_importCentre = true;
    bool m_importReplace = true;
    // Offsets being edited at the playhead, before they are committed as keys.
    std::vector<AnimationKey> m_liveOffsets;
};

} // namespace pred
