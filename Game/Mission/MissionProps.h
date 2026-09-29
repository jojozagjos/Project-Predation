#pragma once

#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Scene.h"
#include "Game/Mission/Mission.h"
#include "Game/World/Vehicles.h"

#include <vector>

namespace pred
{

class InteractionSystem;
class MeshLibrary;

// The mission's things in the world: the terminal on its bench, a breaker panel on a wall of every building, and the launch
// console in the shuttle on the pad -- which the team comes in on and leaves on. Each is something solid, something that
// shows how things stand -- the terminal's screen, the panel's lamp, the console's -- and something to press.
//
// Built with the site and taken away with it; Show is told how the mission stands whenever that changes, on every
// machine, so everybody sees the same screens lit and is offered the same things to do.
class MissionProps
{
public:
    // `shuttle` is the site's, on the pad: its console is the one the launch is worked from.
    void Build(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, InteractionSystem& interactions, const SitePlan& site,
               const MissionPlan& plan, const VehicleProp& shuttle);
    void Clear(Scene& scene, PhysicsWorld& physics, InteractionSystem& interactions);
    void Show(Scene& scene, InteractionSystem& interactions, const MissionState& state, float time);

private:
    struct Prop
    {
        Entity body;  // what is pressed, and what the prompt is about
        Entity face;  // the screen or the lamp on it
        BodyHandle collider;
        int building = -1;
    };

    Prop Make(Scene& scene, PhysicsWorld& physics, const char* name, MeshHandle bodyMesh, const Material& bodyMaterial,
              const glm::vec3& size, const glm::vec3& at, const glm::quat& turn, MeshHandle faceMesh, const glm::vec3& faceAt);

    Prop m_terminal;
    std::vector<Prop> m_breakers;
    // The shuttle's console, and its screen.
    Entity m_console;
    Entity m_consoleScreen;
    int m_building = -1; // the terminal's
};

} // namespace pred
