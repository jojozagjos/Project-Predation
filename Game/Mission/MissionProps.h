#pragma once

#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/TextureLibrary.h"
#include "Engine/Scene/Scene.h"
#include "Game/Interaction/Interactable.h"
#include "Game/Mission/Mission.h"
#include "Game/World/Vehicles.h"

#include <array>
#include <string>
#include <vector>

namespace pred
{

class InteractionSystem;
class MeshLibrary;

// The mission's things in the world: the terminal on its bench, a breaker board on a wall of every building, a survey's
// markers or a signal's transmitter, and the launch console in the shuttle on the pad -- which the team comes in on and
// leaves on.
//
// Worked by hand (FIX 303-305): the board of the building whose power is out has a door that swings open on its hinge,
// a main switch and six breakers that are thrown one at a time; a station's terminal has its three life-support
// switches under its screen, and its screen shows how the restart stands; a transmitter's lid lifts on its hinge to
// its transmit and power switches. Every moving part is swung to where the mission's state says it is, a little at a
// time, so everybody sees the same thing move.
//
// Built with the site and taken away with it; Show is told how the mission stands every frame, on every machine.
class MissionProps
{
public:
    // `shuttle` is the site's, on the pad: its console is the one the launch is worked from.
    void Build(Scene& scene, MeshLibrary& meshes, TextureLibrary* textures, PhysicsWorld& physics, InteractionSystem& interactions,
               const SitePlan& site, const MissionPlan& plan, const VehicleProp& shuttle);
    void Clear(Scene& scene, PhysicsWorld& physics, InteractionSystem& interactions);
    void Show(Scene& scene, InteractionSystem& interactions, TextureLibrary* textures, const MissionState& state, float time);
    // Every frame: the shuttle's console worked only from aboard it -- never reached through its hull from outside.
    void Reach(InteractionSystem& interactions, bool aboard);

    // The things worked up close (FIX 306): pressing one (InteractionKind::Device, its payload which) brings the camera up to
    // it, and its door or lid and its switches are worked with the pointer -- offered to nothing else (Interactable::closeUp).
    enum class Device : uint8_t
    {
        None,
        Board,      // the breaker board of the building whose power is out
        Console,    // a station's life-support switches, under its terminal's screen
        Transmitter, // a signal's transmitter, its lid and its switches
        ShipTerminal, // (not a site's: the ship's terminal, PredationGame's own -- none of the calls here know it)
        Keypad,     // a secured terminal's keypad, on the bench beside it (Phase 5, codes)
        Note,       // the note its code is written on, on a bench in another room: looked at close, nothing on it to work
        FuseBox     // a faulted terminal's supply box, on the wall over its bench (Phase 5, repair)
    };
    // Where the camera stands to work it, and the point it looks at. False when there is none here.
    bool View(Device device, glm::vec3& eye, glm::vec3& target) const;
    // Which device something pressed is part of: one of its controls (None: none).
    Device DeviceOf(Entity entity) const;
    // Whether there is anything to do at it now; once there is not, whoever is at it steps back.
    bool Workable(Device device, const MissionState& state) const;
    // The control under the pointer, lit a little (none: Entity{}).
    void Hover(Scene& scene, Entity entity);

private:
    // A part that turns about a pivot -- a door on its hinge, a switch's lever, a lid -- between its angle off and its
    // angle on. Its mesh is made about the pivot, so turning it is all there is to it.
    struct Moving
    {
        Entity entity;
        glm::vec3 at{0.0f};          // the thing it is on: its middle
        glm::quat turn{1.0f, 0.0f, 0.0f, 0.0f}; // and how that is turned
        glm::vec3 pivot{0.0f};       // in the thing's frame
        glm::vec3 axis{1.0f, 0.0f, 0.0f};
        float off = 0.0f;            // its angle, off and on (radians)
        float on = 0.0f;
        float speed = 9.0f;          // how fast it goes over, a whole swing a second
        float t = -1.0f;             // where it is between them (below nought: not yet placed)
    };

    Entity Part(Scene& scene, const char* name, MeshHandle mesh, const Material& material, const glm::vec3& at, const glm::quat& turn,
                const glm::vec3& local, bool shadow = true);
    Moving Mover(Scene& scene, const char* name, MeshHandle mesh, const Material& material, const glm::vec3& at, const glm::quat& turn,
                 const glm::vec3& pivot, const glm::vec3& axis, float off, float on, float speed);
    void Swing(Scene& scene, Moving& part, bool on, float dt);
    void Control(InteractionSystem& interactions, Entity entity, InteractionKind kind, int payload, const glm::vec3& focus, float range,
                 bool closeUp = false);
    BodyHandle Collider(PhysicsWorld& physics, const glm::vec3& size, const glm::vec3& at, const glm::quat& turn);

    void BuildBoard(Scene& scene, MeshLibrary& meshes, TextureLibrary* textures, PhysicsWorld& physics, InteractionSystem& interactions,
                    const glm::vec3& at, float yaw);
    void BuildPanel(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, const glm::vec3& at, float yaw);
    void BuildTerminal(Scene& scene, MeshLibrary& meshes, TextureLibrary* textures, PhysicsWorld& physics, InteractionSystem& interactions,
                       const MissionPlan& plan);
    void BuildTransmitter(Scene& scene, MeshLibrary& meshes, TextureLibrary* textures, PhysicsWorld& physics, InteractionSystem& interactions,
                          const glm::vec3& foot, uint32_t seed);
    void BuildKeypad(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, InteractionSystem& interactions, const MissionPlan& plan);
    void BuildFuseBox(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, InteractionSystem& interactions, const MissionPlan& plan);
    void ShowFuseBox(Scene& scene, InteractionSystem& interactions, const MissionState& state, float dt);
    void ShowBoard(Scene& scene, InteractionSystem& interactions, const MissionState& state, float time, float dt);
    void ShowTerminal(Scene& scene, InteractionSystem& interactions, TextureLibrary* textures, const MissionState& state, float time, float dt);
    void ShowTransmitter(Scene& scene, InteractionSystem& interactions, const MissionState& state, float time, float dt);
    void DrawScreen(TextureLibrary* textures, const MissionState& state, float time);

    // Everything built, to be taken away again; and the things pressed, to be unregistered.
    std::vector<Entity> m_parts;
    std::vector<BodyHandle> m_colliders;
    std::vector<Entity> m_controls;

    // The breaker board of the terminal's building, where its power is out (and only then): its door, main switch and
    // breakers, and the lamps on it.
    struct Board
    {
        bool built = false;
        Moving door;
        Moving main;
        std::array<Moving, MissionSpec::kBoardBreakers> levers;
        std::array<Entity, MissionSpec::kBoardBreakers> breakers; // (what is pressed: the breaker, which does not move)
        std::array<Entity, MissionSpec::kBoardBreakers> leds;
        Entity box; // (what is pressed to come up to it)
        glm::vec3 at{0.0f};
        glm::quat turn{1.0f, 0.0f, 0.0f, 0.0f};
        Entity mainBase;
        Entity mainLamp;
    } m_board;

    // The terminal: its body, its screen, and a station's three switches under the screen.
    struct Terminal
    {
        Entity body;
        Entity screen;
        std::array<Moving, MissionRules::kConsoleSystems> levers;
        std::array<Entity, MissionRules::kConsoleSystems> plates;
        std::string drawn; // what the screen last had drawn on it
        glm::vec3 at{0.0f};
        glm::quat turn{1.0f, 0.0f, 0.0f, 0.0f};
    } m_terminal;

    // A secured terminal's keypad (its keys, in MissionRules' order: the figures, clear, enter) and its lamp; and the note with
    // its code on it.
    struct Keypad
    {
        Entity body; // (what is pressed to come up to it)
        std::array<Entity, MissionRules::kKeys> keys;
        Entity lamp;
    } m_keypad;
    struct Note
    {
        Entity paper;
        glm::vec3 at{0.0f};
        glm::quat turn{1.0f, 0.0f, 0.0f, 0.0f};
    } m_note;
    bool m_secured = false;

    // A faulted terminal's supply box: its cover (on its hinge) and the cover's screws, the spare fuse clipped inside the cover,
    // its isolator's handle, and its four fuse holders, each a carrier that swings down open.
    struct FuseBox
    {
        bool built = false;
        Entity body; // (what is pressed to come up to it)
        Moving cover;
        Moving spare;
        std::array<Entity, 4> screws;
        Moving isolator;
        std::array<Moving, MissionSpec::kFuses> carriers;
        glm::vec3 at{0.0f};
        glm::quat turn{1.0f, 0.0f, 0.0f, 0.0f};
    } m_fuseBox;

    // A survey's markers (each a stake: what is pressed, and its lamp).
    std::vector<Entity> m_markers;
    std::vector<Entity> m_markerLamps;

    // A signal's transmitter: its case, its lid, its two switches and its lamps.
    struct Transmitter
    {
        bool built = false;
        Moving lid;
        Moving sender;
        Moving power;
        Entity senderBase;
        Entity powerBase;
        Entity beacon;
        Entity senderLed;
        Entity powerLed;
        Entity body; // (what is pressed to come up to it)
        glm::vec3 at{0.0f};
        glm::quat turn{1.0f, 0.0f, 0.0f, 0.0f};
    } m_transmitter;

    // The shuttle's console, and its screen.
    Entity m_console;
    Entity m_consoleScreen;
    bool m_consoleLive = false; // (what Show last made of it)

    TextureHandle m_screenTexture; // the terminal's screen: made once, drawn into whenever what it shows changes
    TextureHandle m_boardTexture;  // the board's plate, its labels
    TextureHandle m_caseTexture;   // the transmitter's panel, its labels
    MissionPlan::Kind m_kind = MissionPlan::Kind::Download;
    float m_lastTime = -1.0f;
    Entity m_hovered; // (lit under the pointer)
};

} // namespace pred
