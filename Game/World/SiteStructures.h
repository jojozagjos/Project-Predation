#pragma once

#include "Engine/Render/Material.h"
#include "Game/World/SitePlan.h"

#include <glm/vec3.hpp>

namespace pred
{

class MapBuilder;

// What stands round a site's buildings by what the place is (SitePlan::FacilityType), drawn and felt: a mine's headframe over its
// shaft with its winder house, and its spoil heaps; a refinery's tank farm in its bund wall, its pipe rack and its flare stack; a
// depot's gantry crane; a relay's or an observatory's dish; solar arrays in their fence; a camp's weather station.
namespace SiteStructures
{

// What they are made of: the site's own, as SiteMap has them.
struct Materials
{
    const Material& concrete;
    const Material& steel;
    const Material& paint;   // painted steel: tanks, stacks
    const Material& hazard;  // yellow, painted: a crane, a headframe's wheels
    const Material& pipe;
    const Material& rock;    // spoil, as the world's rock is
    const Material& dark;    // panels, openings
    const Material& glow;    // a flame
};

// Whether a block is one of these.
bool Is(SitePlan::BlockKind kind);

// One of them: `foot` the middle of its foot, on the ground.
void Build(const SitePlan::Block& block, const glm::vec3& foot, MapBuilder& builder, const Materials& made);

} // namespace SiteStructures

} // namespace pred
