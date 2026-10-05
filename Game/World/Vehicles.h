#pragma once

#include "Engine/Assets/ModelAsset.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Scene.h"
#include "Game/Cinematic/Cinematic.h"

#include <memory>
#include <string>
#include <vector>

namespace pred
{

class MeshLibrary;

// The vehicles of a deployment, as models the editor keeps (Assets/Models/Vehicles): the shuttle that sets the team
// down on a site and takes them off it again. The first time one is wanted and
// there is no file, it is made from the numbers here and written there; from then on the file is the vehicle, and it is
// edited in the model editor like any other model.
//
// A vehicle model faces -z, stands on y = 0, and names what the game needs in it with sockets and clips:
//   sockets   "arrival"      where somebody stands in it when a cinematic hands them control, facing the way out
//             "console"      where its launch console stands (the shuttle, at the front of its cabin)
//             "cabin_min"/"cabin_max"   two corners of the space that counts as aboard
//             "lamp"         where its cabin lamp hangs
//             "light_..."    a lamp that goes with it, shining down its socket's -z: headlights, a landing light
//   clips     "ramp_closed", "ramp_opening", "ramp_closing"
// The ship's outside is made by ShipMap from its look (ShipMap::HullModel): parts named "fx_engine_glow..." are what a
// burn lights up.
// The ship's hangar doors are one of these too: two leaves in the floor, hinged at their outer edges, swinging down --
// clips "doors_closed", "doors_opening", "doors_open", "doors_closing".
// Parts whose names begin "fx_" -- glass, lamps, glow -- are drawn and never solid.
namespace Vehicles
{
ModelAsset ShuttleModel();
ModelAsset BayDoorsModel();
// The model by name, from its file, or made and written when there is no file yet. Null for a name it does not know.
std::shared_ptr<ModelAsset> Load(const std::string& name);
} // namespace Vehicles

// One of them standing in the world: its parts drawn one by one, so a cinematic can move it and play its clips, and solid
// where it rests -- its home -- so people can walk in and out of it. A cinematic moves only the picture of it; the solid
// parts stay home, which is always where it comes to rest.
class VehicleProp
{
public:
    // `restClip` at `restTime` is how it is solid and how it rests (empty: its model as built -- ramps down).
    bool Build(Scene& scene, MeshLibrary& meshes, PhysicsWorld* physics, std::shared_ptr<ModelAsset> model, const CinePose& home,
               uint32_t overlapGroup, const std::string& prefix);
    void Clear(Scene& scene, PhysicsWorld* physics);
    // The picture of it: where, and one of its clips at a moment of it (empty: as built).
    void Show(Scene& scene, const CinePose& pose, const std::string& clip = std::string(), float clipTime = 0.0f);
    void ShowClip(Scene& scene, const std::string& clip, float clipTime) { Show(scene, m_shown, clip, clipTime); }
    void GoHome(Scene& scene) { Show(scene, m_home); }
    // Out of sight altogether, or back: the ship's two outsides, only one of which belongs in any picture.
    void SetHidden(Scene& scene, bool hidden);
    bool Hidden() const { return m_hidden; }

    bool Built() const { return m_model != nullptr; }
    const CinePose& Home() const { return m_home; }
    const CinePose& Shown() const { return m_shown; }
    const ModelAsset* Model() const { return m_model.get(); }
    // A socket where the vehicle rests, in the world; false when the model has none of that name.
    bool Socket(const std::string& name, CinePose& out) const;
    // The same, where it is shown now: moving with it.
    bool SocketShown(const std::string& name, CinePose& out) const;
    // Whether a point is aboard: between its cabin corners, where it rests.
    bool Aboard(const glm::vec3& point) const;
    const std::vector<BodyHandle>& Bodies() const { return m_bodies; }
    // The drawn piece of one of its parts, to press (a console) or to light (a screen); invalid when there is none.
    Entity Part(const std::string& name) const;

private:
    std::shared_ptr<ModelAsset> m_model;
    CinePose m_home;
    CinePose m_shown;
    bool m_hidden = false;
    std::vector<Entity> m_parts;
    std::vector<BodyHandle> m_bodies;
};

} // namespace pred
