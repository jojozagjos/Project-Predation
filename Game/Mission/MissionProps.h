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
        FuseBox,    // a faulted terminal's supply box, on the wall over its bench (Phase 5, repair)
        Cabinet,    // a cut feed's feeder cabinet, outside the building by a door (Phase 5, power routing)
        Marker,     // a survey's marker, planted at its point (Phase 6): its legs, their clamps, its switch, its stake (OpenMarker: which)
        Rack,       // a server rack's drive bay, where the data is in one (Phase 6): its cover, its drive's sled and board, its port
        Donor,      // another rack's service hatch, a damaged drive's spare board in it (Phase 6)
        Filler,     // a plant's generator filler on a wall, its cap (Phase 6, the chain)
        Pump,       // its hand pump, its lever and its gauge
        Valves      // its valves on their manifold, and the plate with their line-up
    };
    // A survey marker's head pressed (InteractionKind::Device): its payload this plus which marker.
    static constexpr int kMarkerDevices = 32;
    // Which survey marker is up close (Device::Marker); -1 none.
    void OpenMarker(int marker) { m_openMarker = marker; }
    int OpenedMarker() const { return m_openMarker; }
    // A survey's crate in the shuttle (Phase 6): where its markers lie in it (their middles) and how they lie (the shuttle's turn:
    // along its cabin), for the game to put them there. Empty with no crate.
    const std::vector<glm::vec3>& CrateSlots() const { return m_crateSlots; }
    const glm::quat& CrateTurn() const { return m_crateTurn; }
    // On the shuttle cabin's right-hand bench, where something brought for the work is left (the copy unit), and how it lies.
    const glm::vec3& BenchSpot() const { return m_benchSpot; }
    const glm::quat& BenchTurn() const { return m_benchTurn; }
    // Where the copy unit stands connected to a rack (its middle), and where a donor rack's board comes out.
    glm::vec3 UnitAt() const;
    glm::vec3 DonorAt() const { return m_donor.at; }
    // A survey point's flag: what is pressed to plant a marker there (Entity{} none).
    Entity PointFlag(int marker) const;
    // Where the camera stands to work it, and the point it looks at. False when there is none here.
    bool View(Device device, glm::vec3& eye, glm::vec3& target) const;
    // Which device something pressed is part of: one of its controls (None: none).
    Device DeviceOf(Entity entity) const;
    // Whether there is anything to do at it now; once there is not, whoever is at it steps back.
    bool Workable(Device device, const MissionState& state) const;
    // The control under the pointer, lit a little (none: Entity{}).
    void Hover(Scene& scene, Entity entity);
    // Colour-blind safe lamps (the setting, ui.colour_safe): what is right lit blue rather than green and what is wrong orange
    // rather than red -- told apart by those who cannot tell red from green. (Every lamp's state shows in how it lights as well:
    // steady, pulsing or flickering, lit or dark.)
    void SetColourSafe(bool safe) { m_colourSafe = safe; }

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
    // The same to anywhere between (0 off, 1 on): a switch with a middle position.
    void SwingTo(Scene& scene, Moving& part, float target, float dt);
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
    void BuildCabinet(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, InteractionSystem& interactions, const MissionPlan& plan);
    void ShowCabinet(Scene& scene, InteractionSystem& interactions, const MissionState& state, float dt);
    void ShowBoard(Scene& scene, InteractionSystem& interactions, const MissionState& state, float time, float dt);
    void ShowTerminal(Scene& scene, InteractionSystem& interactions, TextureLibrary* textures, const MissionState& state, float time, float dt);
    void ShowTransmitter(Scene& scene, InteractionSystem& interactions, const MissionState& state, float time, float dt);
    void BuildMarkers(Scene& scene, MeshLibrary& meshes, InteractionSystem& interactions, const MissionPlan& plan);
    void BuildRack(Scene& scene, MeshLibrary& meshes, InteractionSystem& interactions, const MissionPlan& plan);
    void ShowRack(Scene& scene, InteractionSystem& interactions, const MissionState& state, float time, float dt);
    void BuildPlant(Scene& scene, MeshLibrary& meshes, TextureLibrary* textures, PhysicsWorld& physics, InteractionSystem& interactions, const MissionPlan& plan);
    void ShowPlant(Scene& scene, InteractionSystem& interactions, const MissionState& state, float dt);
    void BuildCrate(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, const VehicleProp& shuttle, size_t markers);
    void ShowMarkers(Scene& scene, InteractionSystem& interactions, const MissionState& state, float time, float dt);
    // A part of a marker put where it is: in the marker's frame (its foot, turned and leaning or fallen), at `local` turned `spin`.
    void PlaceOn(Scene& scene, Entity entity, const glm::vec3& foot, const glm::quat& frame, const glm::vec3& local, const glm::quat& spin) const;
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

    // A cut feed's cabinet: its door (with the plate on it), its two feeds' switches, its changeover, and its lamps -- each feed's,
    // lit where there is supply on it, and the building's.
    struct Cabinet
    {
        bool built = false;
        Entity body; // (what is pressed to come up to it)
        Moving door;
        Moving doorPlate;
        Moving doorWords;
        Moving main;
        Moving standby;
        Moving changeover;
        Entity mainLamp;
        Entity standbyLamp;
        Entity busLamp;
        glm::vec3 at{0.0f}; // (its foot)
        glm::quat turn{1.0f, 0.0f, 0.0f, 0.0f};
    } m_cabinet;

    // A survey's markers (Phase 6): each its point's flag on its pin, and the marker planted there -- its stake, the collar its three
    // legs hang from, each leg's sleeve, clamp and sliding foot, and its head: the case, its lamp, its switch and the bubble's vial
    // on top (its ring, and the bubble). What is shown of each as it moves: how far it is pushed in, its legs out and their feet slid
    // out, its switch over, how far over it has fallen, how it leans.
    struct SurveyMarker
    {
        glm::vec3 foot{0.0f};
        glm::quat turn{1.0f, 0.0f, 0.0f, 0.0f};
        Entity pin;
        Entity flag;
        Entity stake;
        Entity collar;
        Entity head;
        Entity lamp;
        Entity switchBase;
        Entity lever;
        Entity vial;
        Entity ring;
        Entity bubble;
        std::array<Entity, MissionSpec::kMarkerLegs> sleeves;
        std::array<Entity, MissionSpec::kMarkerLegs> clamps;
        std::array<Entity, MissionSpec::kMarkerLegs> slides;
        float plant = -1.0f; // (below nought: not shown yet)
        float fall = 0.0f;
        float lever01 = 0.0f;
        std::array<float, MissionSpec::kMarkerLegs> out{};
        std::array<float, MissionSpec::kMarkerLegs> reach{};
        glm::vec2 lean{0.0f};
        bool shown = false;
    };
    std::vector<SurveyMarker> m_survey;
    // A server rack's drive bay (Phase 6): its housing (pressed to come up to it), its cover on its hinge, the drive's sled and the
    // board on it, its lamp, its port; the copy unit standing in front of it on the floor and its cable; and the other rack's service
    // hatch and the board in it.
    struct RackBay
    {
        bool built = false;
        bool damaged = false;
        bool archive = false;
        Entity housing;
        Moving cover;
        Entity sled;
        Entity board;
        Entity led;
        Entity port;
        Entity unit;
        Entity unitLed;
        Entity cable;
        float sledOut = 0.0f;
        bool unitShown = false;
        glm::vec3 at{0.0f};
        glm::quat turn{1.0f, 0.0f, 0.0f, 0.0f};
    } m_rack;
    struct DonorBay
    {
        bool built = false;
        Entity housing;
        Moving hatch;
        Entity board;
        glm::vec3 at{0.0f};
        glm::quat turn{1.0f, 0.0f, 0.0f, 0.0f};
    } m_donor;
    // A plant's chain (Phase 6): its generator's filler (its tank, its cap on its hinge, its gauge), its hand pump (its lever, its
    // gauge's needle; a stroke shown as it is worked) and its valves (their handwheels, each turned a quarter open, and the plate).
    struct Plant
    {
        bool built = false;
        Entity tank;
        Moving cap;
        Entity mouth;
        Entity fuelLamp;
        Entity pumpBody;
        Moving lever;
        Moving needle;
        uint8_t strokesShown = 0;
        float stroke = 1.0f; // (0 to 1 through a stroke: the lever down and back up)
        Entity manifold;
        std::array<Moving, MissionSpec::kPlantValves> wheels;
        glm::vec3 fillerAt{0.0f};
        glm::quat fillerTurn{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 pumpAt{0.0f};
        glm::quat pumpTurn{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 valvesAt{0.0f};
        glm::quat valvesTurn{1.0f, 0.0f, 0.0f, 0.0f};
    } m_plant;
    TextureHandle m_valvePlate;
    glm::vec3 m_benchSpot{0.0f};
    glm::quat m_benchTurn{1.0f, 0.0f, 0.0f, 0.0f};
    int m_openMarker = -1;
    // Its crate, on the shuttle's right-hand bench at the back: where the markers lie in it.
    std::vector<glm::vec3> m_crateSlots;
    glm::quat m_crateTurn{1.0f, 0.0f, 0.0f, 0.0f};

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
    bool m_colourSafe = false;
    glm::vec3 Good() const;
    glm::vec3 Bad() const;
};

} // namespace pred
