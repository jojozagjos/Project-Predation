#pragma once

#include "Engine/Assets/ModelAsset.h"

#include <glm/vec3.hpp>

#include <cstdint>

namespace pred
{

// Cargo and the things people leave about, made of shaped parts rather than one textured box each, so they read as what
// they are: a ribbed plastic crate with its lid and handles, a hard case with its corner guards and latches, a steel drum
// with its rolling hoops, a rack of gas bottles, a pallet with its load wrapped and strapped, a duffel bag. One library for
// the ship, the outposts, the facilities and wrecks.
//
// Each is built standing on the floor at its origin (y up), its front towards +z, its middle over the origin. Parts only
// turn about x and y, so a prop turned about y (Place) stays right.
namespace Cargo
{

// A heavy plastic crate, `size` across (x), high (y) and deep (z), in `colour`: ribbed sides, corner posts, a lid with a
// rim, recessed handles at the ends, a blank label.
ModelAsset PlasticCrate(const glm::vec3& size, const glm::vec3& colour);
// A hard equipment case, `size`, its shell in `colour` (worn paint): aluminium edges and corner guards, the seam where
// the lid closes, two latches, a handle on top, rubber feet.
ModelAsset HardCase(const glm::vec3& size, const glm::vec3& colour);
// A steel drum, its paint `colour`, worn: rolling hoops, the chime rims at top and bottom, the bungs in the lid.
ModelAsset Drum(const glm::vec3& colour);
// A rack of `count` gas bottles side by side along x, their shoulders `colour`: a base plate, two rails behind, a chain
// across their fronts, a valve and cap on each.
ModelAsset GasRack(int count, const glm::vec3& colour);
// A wooden pallet with a load on it `high`, wrapped in film and strapped down.
ModelAsset WrappedPallet(float high);
// A duffel bag lying along x, in `colour`.
ModelAsset Duffel(const glm::vec3& colour);

// `prop`'s parts into `into`, stood at `at` and turned `yawDegrees` about up; each part named `prefix` and its own name.
void Place(ModelAsset& into, const ModelAsset& prop, const glm::vec3& at, float yawDegrees, const std::string& prefix = "cargo_");
// The box round a prop as built: its corners.
void Bounds(const ModelAsset& prop, glm::vec3& lo, glm::vec3& hi);

} // namespace Cargo

} // namespace pred
