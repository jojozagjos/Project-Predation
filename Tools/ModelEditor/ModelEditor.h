#pragma once

#include "Engine/Assets/ModelAsset.h"
#include "Engine/Render/Camera.h"
#include "Engine/Scene/Scene.h"

#include <string>
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

    bool Load(const std::string& modelName);
    bool Save();

private:
    void Rebuild(Scene& scene, MeshLibrary& meshes);
    void NewModel();
    void AddPart(const char* name, PartShape shape);
    void ImportMesh(const std::string& file);
    void DrawPartList();
    void DrawPartInspector();
    void DrawSocketPanel();
    void DrawAnimationPanel();
    void DrawFilePanel(Scene& scene, MeshLibrary& meshes);
    AnimationClip* CurrentClip();
    // Writes the current offsets of every part into the selected clip at the playhead, which is how
    // a pose becomes a keyframe.
    void KeyAllParts();

    Application* m_app = nullptr;
    ModelAsset m_model;
    bool m_open = false;
    bool m_dirty = true; // the preview needs rebuilding

    // One entity per part, so a part can be hidden or animated on its own.
    std::vector<Entity> m_entities;
    std::vector<Entity> m_socketEntities;

    int m_selectedPart = -1;
    int m_selectedSocket = -1;
    int m_selectedClip = -1;

    float m_playhead = 0.0f;
    bool m_playing = false;

    FlyCamera m_camera;
    float m_gridSnap = 0.005f;
    bool m_snapEnabled = true;
    bool m_showSockets = true;

    std::string m_status;
    std::string m_saveName = "new_model";
    std::string m_importPath;
    // Offsets being edited at the playhead, before they are committed as keys.
    std::vector<AnimationKey> m_liveOffsets;
};

} // namespace pred
