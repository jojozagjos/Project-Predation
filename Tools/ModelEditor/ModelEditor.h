#pragma once

#include "Engine/Assets/GltfImport.h"
#include "Engine/Assets/ModelAsset.h"
#include "Engine/Render/Camera.h"
#include "Engine/Scene/Scene.h"

#include <filesystem>
#include <string>
#include <functional>
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
    // The clip open in the timeline and where the playhead is, so the first-person preview can play
    // exactly what is being edited, at exactly that moment.
    const AnimationClip* SelectedClip() const;
    float Playhead() const { return m_playhead; }
    void SetPlayhead(float seconds) { m_playhead = seconds; m_playing = false; }
    const std::string& Status() const { return m_status; }

    // Starting points for a reload: a clip with the hands and the magazine already moving, shaped by
    // hand from there rather than started from nothing.
    enum class ReloadTemplate : uint8_t
    {
        Tactical,       // magazine out, a fresh one from the belt, in
        Empty,          // the same, then the bolt released
        DoubleMagazine  // two magazines taped side by side: out, roll the pair over, in
    };
    void MakeReloadTemplate(ReloadTemplate kind);

    const FlyCamera& Camera() const { return m_camera; }
    FlyCamera& Camera() { return m_camera; }

    float CameraSpeed() const { return m_cameraSpeed; }

    bool Load(const std::string& modelName);
    bool Save();

    // --- Dropping a file on the window ----------------------------------------------------------
    //
    // The whole of adding an asset, for the common case. Importing used to mean finding the file in
    // Explorer, copying its path, switching to the game, pasting it into a box and pressing a
    // button, and that is five steps too many for something a window can simply be given.
    //
    // A model file dropped on the window is imported; a model file dropped when the editor is not
    // open opens the editor on it. The source file is copied into the assets tree beside the model
    // it produced, so the import can be repeated later without going looking for the download.
    static bool IsImportableModel(const std::filesystem::path& file);
    // Returns false when the file is not one of ours, so the caller can say so rather than
    // swallowing it.
    bool DropFile(const std::string& path);
    // Where the source art was copied to, empty when nothing was copied. Reported so a person can
    // see their download is now part of the project rather than wondering.
    const std::string& LastImportSource() const { return m_lastImportSource; }

    // What is selected. One thing at a time, part or socket, because everything that acts on a
    // selection acts on one thing and a list of one is a list.
    enum class Pick : uint8_t
    {
        None,
        Part,
        Socket,
        // The key under the playhead on the selected track: the handles move that key, so a hand or a
        // part can be posed by dragging it where it should be at that moment.
        Key
    };

    // Selects whatever a ray runs through, and returns true if that changed anything. Clicking
    // nothing clears the selection, which is what clicking nothing means everywhere else.
    bool SelectUnderRay(const glm::vec3& origin, const glm::vec3& direction);
    // Where the selected thing is, and whether there is one.
    bool SelectionPosition(glm::vec3& out) const;
    void MoveSelection(const glm::vec3& delta);

    // --- Dragging -------------------------------------------------------------------------------
    // The three axis handles on the selected thing. The game feeds a ray from the pointer, because
    // it owns the camera and the pointer; the editor decides what that ray grabbed and where it
    // moved to, because it owns the model.
    //
    // Returns true when a handle was grabbed, which is also how the game knows not to treat the
    // same click as a selection.
    bool BeginDrag(const glm::vec3& origin, const glm::vec3& direction);
    void UpdateDrag(const glm::vec3& origin, const glm::vec3& direction);
    void EndDrag() { m_dragAxis = -1; }
    bool Dragging() const { return m_dragAxis >= 0; }
    // How long the handles are drawn, in metres. Also how far out they can be grabbed.
    static constexpr float kHandleLength = 0.11f;

    // --- Undo -----------------------------------------------------------------------------------
    // Records the state before a change, so it can be gone back to. Called by everything that edits
    // the model; repeated calls inside one drag collapse into one entry, which is what makes
    // dragging a slider one undo rather than four hundred.
    void PushUndo(const char* what);
    // Pushes a copy taken earlier, for a change already made. A gesture snapshots when it starts,
    // because by the time a widget says it changed something, it has.
    void PushSnapshot(ModelAsset before, const char* what);
    // A cheap summary of everything a person can change by hand, used to tell whether a gesture
    // altered anything at all. Geometry is left out: it only changes on an import, which records
    // its own undo.
    size_t Fingerprint() const;
    bool Undo();
    bool Redo();
    bool CanUndo() const { return !m_undo.empty(); }
    bool CanRedo() const { return !m_redo.empty(); }
    const char* UndoName() const { return m_undo.empty() ? "" : m_undo.back().what; }
    const char* RedoName() const { return m_redo.empty() ? "" : m_redo.back().what; }
    size_t UndoDepth() const { return m_undo.size(); }
    // Asks for the view to be put on the model. The game owns the camera angles, so the editor can
    // only ask; it returns where to stand and what to look at, or false when nothing has asked.
    bool TakeFrameRequest(glm::vec3& outPosition, glm::vec3& outTarget);
    // Asks for it now.
    void FrameModel() { m_frameRequested = true; }

    // Which part a ray runs through, or -1. The ray comes from the game, which owns the camera and
    // the pointer; the editor owns the parts and is the only thing that can say what was hit.
    int PartUnderRay(const glm::vec3& origin, const glm::vec3& direction) const;
    // The box a part actually occupies, in its own frame. An imported mesh keeps its size at one,
    // which is not its size at all.
    static AABB PartBounds(const ModelPart& part);
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
    // And whether what changed was geometry rather than a socket or a clip.
    //
    // Moving a socket changes nothing that is drawn: it changes where a hand goes. Rebuilding the
    // meshes for it copied a quarter of a megabyte per part, destroyed and remade every entity, and
    // did it on every frame of every drag, which is why the editor fell to single-figure frame
    // rates after a while of moving grips about.
    bool TakeGeometryChanged()
    {
        const bool changed = m_geometryChanged;
        m_geometryChanged = false;
        return changed;
    }

private:
    void Rebuild(Scene& scene, MeshLibrary& meshes);
    void NewModel();
    void AddPart(const char* name, PartShape shape);
    void ImportMesh(const std::string& file);
    // Copies a dropped or pasted source file into the assets tree beside the model it produced,
    // and returns where it went. Empty when the file was already ours or could not be copied.
    std::string KeepSource(const std::filesystem::path& file);
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
    // Changes what holds a part from a key on, keeping the part exactly where it is on the screen.
    void SetHolder(AnimationClip& clip, AnimationTrack& track, AnimationKey& key, PartHolder holder);
    // Where the selected track is at the playhead, in the model's frame, and how a move there turns
    // into a change to its key.
    bool KeyPosition(glm::vec3& out) const;
    void MoveKey(const glm::vec3& delta);

    Application* m_app = nullptr;
    ModelAsset m_model;
    bool m_open = false;
    bool m_dirty = true; // the preview needs rebuilding
    bool m_previewChanged = true; // and whatever is holding it needs rebuilding too
    // Whether the change was to geometry. A socket or a clip does not need the meshes rebuilt.
    bool m_geometryChanged = true;
    bool m_frameRequested = true; // put the view on the model at the next opportunity

    // One entity per part, so a part can be hidden or animated on its own.
    std::vector<Entity> m_entities;
    std::vector<Entity> m_socketEntities;

    int m_selectedPart = -1;
    int m_selectedSocket = -1;
    Pick m_pick = Pick::None;

    // Whole copies of the model, which is the only kind of undo worth having in an editor: every
    // operation is then undoable without each one having to describe its own inverse, and getting
    // one of those inverses wrong is how an undo stack quietly corrupts what it is protecting.
    // Imported geometry makes a copy expensive, so the depth is small and a copy is only taken when
    // something is actually about to change.
    struct Snapshot
    {
        ModelAsset model;
        const char* what = "edit";
    };
    static constexpr size_t kUndoDepth = 24;
    std::vector<Snapshot> m_undo;
    std::vector<Snapshot> m_redo;
    // Collapses a drag into one entry: the same name arriving again within a moment is the same
    // gesture continuing rather than a new one starting.
    // The model as it was when the current gesture began, and what it looked like then. A widget
    // only reports a change after making it, so the copy has to be taken when the widget is grabbed
    // rather than when it reports. Watching for any widget at all being grabbed covers every one of
    // them without each having to say so.
    ModelAsset m_gestureStart;
    size_t m_gestureFingerprint = 0;
    bool m_gestureActive = false;

    // Which axis handle is being dragged, or -1. The grab offset is kept so the thing does not jump
    // to the pointer the instant it is grabbed, which is the difference between dragging something
    // and throwing it.
    int m_dragAxis = -1;
    float m_dragGrab = 0.0f;
    int m_selectedClip = -1;
    int m_selectedTrack = -1;
    // The key being dragged along its lane, if any. A key is a moment in time and dragging it is
    // how a moment is moved; the values on it are edited in the inspector below the timeline.
    int m_dragTrack = -1;
    int m_dragKey = -1;

    float m_playhead = 0.0f;
    bool m_playing = false;

    FlyCamera m_camera;
    float m_cameraSpeed = 1.4f;
    float m_gridSnap = 0.005f;
    bool m_snapEnabled = true;
    bool m_showSockets = true;

    std::string m_status;
    std::string m_saveName = "new_model";
    // Which folder under Models a newly named model is filed in. A model that already exists is
    // rewritten where it is, so this only decides where new ones land.
    std::string m_folder = "Weapons";
    std::string m_importPath;
    // Where the last import copied its source file to, for the status line.
    std::string m_lastImportSource;
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
    // The model as it was last frame, to notice any change however it was made.
    size_t m_lastFingerprint = 0;
};

} // namespace pred
