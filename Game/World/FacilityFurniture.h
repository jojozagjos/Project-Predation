#pragma once

#include "Game/World/FacilityLayout.h"
#include "Game/World/FacilityMap.h"

namespace pred
{

// A building's rooms furnished as what each is for (FacilityLayout::fittings): every bed, desk, console, bench, counter,
// basin, rack and machine as it is made -- frames and legs, tops and doors, screens lit, bedding, what is on its shelves --
// drawn in its detail (FacilityMap::Dressing) and felt as one solid box the size of it (a FacilityMap::Piece of Kind::Furniture),
// so what walks and what is shot at meets the whole of it and not its legs. Only a plan, like the building's.
namespace FacilityFurniture
{

void Furnish(const FacilityLayout& layout, FacilityMap::Blueprint& out);

} // namespace FacilityFurniture

} // namespace pred
