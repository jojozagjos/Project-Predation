#pragma once

#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Scene.h"
#include "Game/World/FacilityMap.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pred
{

class InteractionSystem;
class MeshLibrary;

// How a lift stands: which stop its car is at or leaving, where it is going, how far open its doors are there, and what it
// has been asked for. Worked by LiftRules, the same on every machine but for one thing: only the host sends it anywhere
// (LiftRules::Tick's `authority`), and tells everybody else when it does, and when it opens its doors to somebody.
struct LiftState
{
    enum class Phase : uint8_t
    {
        Shut,    // standing at a stop, its doors shut
        Opening,
        Open,
        Closing,
        Moving   // between stops, its doors shut
    };
    Phase phase = Phase::Shut;
    int at = 0;              // the stop it is at, or leaving
    int to = 0;              // and going to, while it moves
    float doors = 0.0f;      // how far open its doors are at `at`, nought to one
    float hold = 0.0f;       // seconds left before they shut
    float travelled = 0.0f;  // seconds into its trip
    std::vector<int> queue;  // the stops it has been asked to go to, in order (the host's)
};

namespace LiftRules
{

inline constexpr float kDoorSeconds = 1.4f; // the doors, open to shut or shut to open
inline constexpr float kHoldSeconds = 4.5f; // and stood open
inline constexpr float kSpeed = 1.5f;       // metres a second, at its fastest

enum class Pressed : uint8_t
{
    Nothing, // no power to go anywhere, or asked already
    Opened,  // its doors open where it is (or are kept open): with or without power -- nobody is ever shut in it
    Queued   // it will go there, once its doors are shut
};
// A button pressed for a stop: inside the car or the call button at that stop -- either asks for the car to be there.
Pressed Press(LiftState& state, int stop, bool powered);

// How long a trip takes, and where its car's floor is now.
float TripSeconds(const FacilityMap::Lift& lift, int from, int to);
float CarHeight(const FacilityMap::Lift& lift, const LiftState& state);

struct Ticked
{
    bool departed = false; // it has just left on a trip (only where `authority`)
    bool arrived = false;  // it has just got there
};
// Time passing. `blocked`: somebody in its open doorway, so they stay open, or shutting go back open.
Ticked Tick(LiftState& state, const FacilityMap::Lift& lift, float dt, bool authority, bool powered, bool blocked);

// What the host has said: it is off from one stop to another; its doors are opening at a stop; or (to somebody joining) it is
// standing at a stop, its doors shut.
void Depart(LiftState& state, int from, int to);
void OpenAt(LiftState& state, int stop);
void RestAt(LiftState& state, int stop);

} // namespace LiftRules

// A site's lifts, built: each one's car -- its floor, walls, ceiling light, handrail and the buttons inside it --, its doors at every
// stop and the call button beside each, moved to how each stands (LiftState). The car is solid as it goes, and each stop's
// doors are while they are shut; whoever rides in it is the game's to carry (PredationGame).
class Lifts
{
public:
    // A button's payload (InteractionKind::Lift): which lift and which stop, or the inside button that opens its doors.
    static constexpr int kOpenButton = 15;
    static int Payload(size_t lift, int stop) { return static_cast<int>(lift) * 16 + stop; }
    static size_t LiftOf(int payload) { return static_cast<size_t>(payload / 16); }
    static int StopOf(int payload) { return payload % 16; }

    void Build(const std::vector<FacilityMap::Lift>& lifts, Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, InteractionSystem& interactions);
    void Clear(Scene& scene, PhysicsWorld& physics, InteractionSystem& interactions);
    // Everything moved to how each lift stands now: its car, its doors, which stops' doors are solid, its buttons.
    void Show(Scene& scene, PhysicsWorld& physics);

    size_t Count() const { return m_lifts.size(); }
    const FacilityMap::Lift& Def(size_t lift) const { return m_lifts[lift].def; }
    LiftState& State(size_t lift) { return m_lifts[lift].state; }
    const LiftState& State(size_t lift) const { return m_lifts[lift].state; }

    // Where its car's floor is, in the middle; and whether somebody's feet are in the car, or in the doorway at a stop.
    glm::vec3 CarFloor(size_t lift) const;
    bool InCar(size_t lift, const glm::vec3& feet) const;
    bool InDoorway(size_t lift, int stop, const glm::vec3& feet) const;
    // The middle of its doorway at a stop, at the floor: where its doors are heard.
    glm::vec3 Doorway(size_t lift, int stop) const;
    // What a stop is called: the ground floor, floor 2, basement 1.
    std::string StopName(size_t lift, int stop) const;
    static std::string StopNameOf(const FacilityMap::Lift& lift, int stop);

private:
    struct Part
    {
        Entity entity;
        glm::vec3 local{0.0f}; // its middle, in the car's frame (or its stop's)
    };
    struct Built
    {
        FacilityMap::Lift def;
        LiftState state;
        glm::quat turn{1.0f, 0.0f, 0.0f, 0.0f}; // +x across its doorway, -z out through it
        std::vector<Part> car;                 // drawn
        std::vector<Part> buttons;             // inside the car, pressed: a stop each, then the one that opens its doors
        std::vector<BodyHandle> carBodies;     // solid: its floor and walls
        std::vector<glm::vec3> carBodyLocal;
        std::vector<std::pair<Part, Part>> doors; // each stop's two leaves
        std::vector<BodyHandle> shut;             // each stop's doorway, solid while its doors are shut (or invalid)
        std::vector<Entity> calls;                // each stop's call button
    };

    glm::vec3 At(const Built& lift, float height, const glm::vec3& local) const;

    std::vector<Built> m_lifts;
    uint32_t m_group = 0;
};

} // namespace pred
