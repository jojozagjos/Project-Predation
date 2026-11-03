#pragma once

#include "Game/World/FacilityLayout.h"
#include "Game/World/FacilityMap.h"

namespace pred
{

// A building half fallen in (FacilityLayout::Ruin), drawn where its corner was -- as broken concrete is, not in blocks: the stumps of its
// outside walls on the line they stood on, their tops torn ragged and the cladding ripped off lower than the concrete behind it, highest
// where they meet what still stands; the ends of its floors jutting from the wall left standing, their edges broken and sagging; heaps of
// rubble along the foot of every wall, inside and spilt out past the stumps, to be walked over; chunks of concrete and torn cladding on
// them and over the floor; a slab or two come down, lying tipped; steel bars out of what broke. By the building's seed, the same
// everywhere.
namespace FacilityRuin
{

void Draw(const FacilityLayout& layout, FacilityMap::Blueprint& out);

} // namespace FacilityRuin

} // namespace pred
