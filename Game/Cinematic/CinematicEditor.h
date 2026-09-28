#pragma once

#include "Engine/Render/Camera.h"
#include "Game/Cinematic/Cinematic.h"
#include "Game/Cinematic/CinematicPlayer.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace pred
{

class Scene;

// The cinematic editor: the cinematic playing, laid out on a timeline and edited while it plays in the world -- the
// real site, the real vehicles, the real light -- so what is seen while editing is what is seen in the game.
//
// Everything a cinematic has is on the timeline, a row for each thing: the shots, each camera, each actor (its keys, the
// paths it follows, its clips), the sounds, the markers, the words, the particles, the lights, and the shake, fade,
// bars, fog and light. Keys are dragged along it, snapped if asked, duplicated, deleted, copied and pasted; what is
// selected is changed in the inspector beside it, eased by a curve that can be shaped by hand. It is watched through its
// own cameras, or looked round with a free one -- and a camera is placed by flying there and keying it from the view,
// measured from whatever that camera is anchored to, so the key fits every site the cinematic plays at.
//
// Saved to Assets/Cinematics as the file the game plays. Nothing a mission generates writes to it: what a mission
// supplies is only the anchors, so hand edits are never overwritten.
//
// Only in a development build: nobody playing the game edits, skips or scrubs a cinematic.
class CinematicEditor
{
public:
    // What it works through, handed to it each frame by the game.
    struct Context
    {
        CinematicPlayer* player = nullptr;
        Scene* scene = nullptr;
        CinematicPlayer::Host* host = nullptr;
        std::map<std::string, Cinematic>* library = nullptr; // every cinematic there is, as last read or saved
        std::filesystem::path folder;                        // where they are saved
        std::vector<std::string> sounds;                     // every sound there is, to choose from
        // Start one playing to edit it, from a moment, measured from wherever the game is now.
        std::function<void(const Cinematic& cinematic, float from)> replay;
        std::function<void(const std::string& sound)> hear;
        // The point on the level the free camera is looking at.
        std::function<bool(glm::vec3& at)> pick;
        // The clips an actor's model has, bound or brought.
        std::function<std::vector<std::string>(const std::string& actor)> clipsOf;
    };

    void Open() { m_open = true; }
    void Close();
    bool IsOpen() const { return m_open; }
    // The free camera, for looking round while editing; the game turns and moves it.
    FlyCamera& View() { return m_view; }
    const FlyCamera& View() const { return m_view; }
    // Whether the picture is the cinematic's own camera, or the free one.
    bool ThroughCinematic() const { return m_throughCinematic; }
    // Taking hold of the view: looking round freely from `from`, if it was not already.
    void LookFreely(const CameraState& from);
    // Whether it may be closed now: with changes unsaved, only when asked twice.
    bool MayClose();
    bool Unsaved() const { return m_pending || m_committed != m_saved; }
    // Where the picture is shown while editing: the part of the screen its windows leave, at the screen's shape, in
    // the interface's coordinates. False while the editor is hidden, when the picture has the whole screen.
    bool Preview(glm::vec2& min, glm::vec2& max) const;
    // The camera selected, to draw its path brighter; empty when none is.
    std::string SelectedCamera(const Cinematic& cinematic) const;
    // Played to its end: round again, when looping.
    void AtEnd(Context& context);
    // The editor's windows and keys, every frame it is open.
    void Draw(Context& context);

private:
    enum class Pick : uint8_t
    {
        None,
        Camera,
        CameraKey,
        Shot,
        Actor,
        ActorKey,
        Path,
        Clip,
        Sound,
        Marker,
        Text,
        Particle,
        Light,
        LightKey,
        Float
    };
    // What is selected: a track (`track`), or something on one (`item`, and `track` when it takes one).
    struct Selection
    {
        Pick kind = Pick::None;
        int track = -1;
        int item = -1;
        bool operator==(const Selection&) const = default;
    };
    // A row of the timeline: what it shows, and which of them.
    struct Row
    {
        std::string label;
        Pick kind = Pick::None; // Shot, Camera, Actor, Sound, Marker, Text, Particle, Light or Float
        int track = -1;
    };
    // Something drawn on a row: a moment, or a span when `end` is not below zero.
    struct Item
    {
        float time = 0.0f;
        float end = -1.0f;
        Selection select;
        bool key = false; // drawn as a key rather than a flag
        uint32_t colour = 0;
        std::string label;
    };
    using Copied = std::variant<std::monostate, CameraKey, TransformKey, FloatKey, Shot, PathFollow, ClipEvent, SoundEvent, pred::Marker, TextItem,
                                ParticleEvent>;

    Cinematic& Edited(Context& context) { return context.player->Edited(); }
    // After a change: the list it is in back in order, and the world shown as it now is.
    void Changed(Context& context);
    // A change finished with: kept, for undoing.
    void Commit(Context& context);
    // A different cinematic in hand: its history starts again.
    void Adopt(Context& context);
    void Apply(Context& context, const std::string& text);
    void Undo(Context& context);
    void Redo(Context& context);
    void Replay(Context& context, const Cinematic& cinematic, float from);
    void Seek(Context& context, float time);
    void PlayPause(Context& context);
    float Snapped(float time, float playhead) const;

    void DrawToolbar(Context& context);
    void DrawTimeline(Context& context);
    void DrawRowMenu(Context& context);
    void DrawInspector(Context& context);
    void DrawSettings(Context& context);
    void DrawAddMenu(Context& context);
    bool DrawEase(Ease& ease, const char* id);
    bool DrawChoice(const char* label, std::string& value, const std::vector<std::string>& choices, bool allowOther);
    void Shortcuts(Context& context);

    std::vector<Row> Rows(const Cinematic& cinematic) const;
    std::vector<Item> ItemsOf(const Row& row, const Cinematic& cinematic) const;
    bool Valid(const Cinematic& cinematic, const Selection& select) const;
    float TimeOf(const Cinematic& cinematic, const Selection& select) const;
    void SetTime(Cinematic& cinematic, const Selection& select, float time);
    // Puts the list the selection is in back in time order, keeping the selection on the same thing.
    void Resort(Cinematic& cinematic);
    void Delete(Context& context);
    void Duplicate(Context& context, float at);
    void Copy(const Cinematic& cinematic);
    void Paste(Context& context, float at);
    // A new key or event on a row at a moment: a key takes the value there already, so nothing moves until it is changed.
    void AddAt(Context& context, const Row& row, float time);
    void KeyFromView(Context& context, CameraKey& key, const std::string& anchor, float time) const;
    void ViewFrom(const CameraState& camera);
    std::vector<std::string> Anchors(Context& context) const;
    std::vector<std::string> Unbound(Context& context) const;

    bool m_open = false;
    bool m_throughCinematic = true;
    FlyCamera m_view;
    Selection m_selected;
    // The timeline: how wide a second is, and the moment at its left edge.
    float m_pixelsPerSecond = 40.0f;
    float m_scroll = 0.0f;
    bool m_fitted = false;
    bool m_snap = true;
    float m_snapStep = 0.1f;
    bool m_loop = false;
    bool m_follow = true;
    // A drag under way, and how far into the thing dragged the pointer took hold of it.
    bool m_dragging = false;
    float m_grab = 0.0f;
    bool m_scrubbing = false;
    // Where the right button was pressed, for the menu it opens.
    int m_menuRow = -1;
    float m_menuTime = 0.0f;
    Copied m_copied;
    Selection m_copiedFrom;
    // History: the cinematic as each change left it, as its file has it.
    std::string m_editing;
    std::string m_committed;
    std::string m_saved;
    std::vector<std::string> m_undo;
    std::vector<std::string> m_redo;
    bool m_pending = false;
    bool m_needsReplay = false;
    bool m_closeArmed = false;
    bool m_menuOpen = false;
    // Out of the way, for the picture on the whole screen; and where its windows leave the picture.
    bool m_hidden = false;
    float m_timelineTop = 0.0f;
    float m_inspectorLeft = 0.0f;
    char m_newName[96] = {};
    std::string m_status;
    float m_statusAge = 0.0f;
};

} // namespace pred
