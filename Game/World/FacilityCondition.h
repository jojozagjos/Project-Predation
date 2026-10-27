#pragma once

#include "Game/World/FacilityLayout.h"
#include "Game/World/FacilityMap.h"

namespace pred
{

// What has become of a building (FacilityLayout::Condition, and frozen or overgrown over that), drawn in it -- nothing of it
// felt but a chair knocked over, which is in the way as a chair is: left long ago, ceiling tiles down, rubble and paper about;
// left in a hurry, papers everywhere, bags dropped, chairs knocked over; burnt, soot up the walls and across the ceilings, the
// floors strewn with what burnt; frozen, frost along the walls and over the floors' edges, icicles from the ceilings; overgrown,
// vines down the walls, moss and leaves over the floors. On the floors only where nothing stands and no doorway is; on the walls
// only inside ones with no opening in them. Only a plan, the same on every machine.
namespace FacilityCondition
{

void Dress(const FacilityLayout& layout, FacilityMap::Blueprint& out);

} // namespace FacilityCondition

} // namespace pred
