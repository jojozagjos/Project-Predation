#pragma once

#include "Engine/Render/TextureLibrary.h"

#include <glm/vec4.hpp>

#include <cstdint>

namespace pred
{

// Leaves and grass for things that grow, drawn here rather than downloaded: one picture of four, each a quarter of it --
// a spray of broad leaves on twigs, a denser one, a conifer's branch of needles, a tuft of grass -- cut out by its alpha
// (Material::cutout) and laid on flat cards (SiteScatter). Pale and nearly grey, for what it is on to tint to its world.
namespace Foliage
{

enum Cell : int
{
    kBroad = 0,
    kBroadDense = 1,
    kNeedles = 2,
    kGrass = 3
};

// The picture: 1024 by 1024, four cells of 512.
ImageData Atlas();
// Where a cell is in it, as (u0, v0, u1, v1): v0 its top row.
glm::vec4 Cell(int cell);

} // namespace Foliage

} // namespace pred
