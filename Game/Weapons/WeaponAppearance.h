#pragma once

#include "Engine/Assets/ModelAsset.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Game/Weapons/WeaponTypes.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <memory>
#include <string>
#include <vector>

namespace pred
{

// The shape of a weapon, and the handful of points on it that other systems need.
//
// It comes from one of two places. If the weapon's data entry names a model, that model is loaded
// from Assets/Models and used as it stands, which is how anything built in the editor gets into the
// game. Otherwise the shape is built from the weapon's numbers, so a new line in weapons.json still
// produces something that reads as a firearm before anyone has opened the editor.
//
// The weapon's own frame has its origin at the pistol grip, +Z along the barrel, +Y up. Everything
// that positions a weapon works in that frame, so nothing has to know how the parts are laid out.
struct WeaponVisual
{
    // One drawn piece. Split into parts rather than baked into a single mesh so a reload can take
    // the magazine out and an authored clip can move anything it likes.
    struct Part
    {
        std::string name;
        MeshData mesh;
        Material material;
        glm::mat4 rest{1.0f};
    };

    std::vector<Part> parts;
    int magazinePart = -1; // index into parts, or -1 when the weapon has no detachable magazine

    glm::vec3 magazineSeated{0.0f}; // where the magazine sits when it is in
    // Where the trigger hand closes. Read from the model rather than assumed to be its origin: an
    // imported model has its origin wherever the person who made it left it, which for the two that
    // arrived first is the middle of the weapon and nowhere near the grip.
    glm::vec3 triggerGrip{0.0f};
    glm::vec3 supportGrip{0.0f};    // where the support hand goes
    glm::vec3 muzzle{0.0f};         // where a round appears to leave
    float sightHeight = 0.0f;       // sight line above the origin; aiming puts this on the view axis

    // Present only for authored models, and only so their animation clips can be played.
    std::shared_ptr<const ModelAsset> asset;

    // Everything merged into one mesh, for the inventory icon and for a weapon lying on the floor,
    // neither of which animates.
    MeshData Combined() const;
};

WeaponVisual BuildWeaponVisual(const WeaponDefinition& definition);

// The same, from a model already in memory rather than one named on disk. The editor holds what is
// open in front of it, which has usually not been saved and belongs to no weapon.
WeaponVisual BuildWeaponVisualFrom(const ModelAsset& model, const WeaponDefinition& definition);

// Writes a procedurally built weapon out as an editable model file, so the editor has something to
// start from rather than a blank page.
bool ExportWeaponModel(const WeaponDefinition& definition, const std::string& modelName);

// Forgets every model read from disk, so the next weapon built picks up what was just saved.
// Without it the editor and the hands holding the model disagree until the game is restarted, which
// is the whole difficulty with placing a grip: it is placed by looking at where the hand lands.
void ForgetWeaponModels();

} // namespace pred
