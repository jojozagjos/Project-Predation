#pragma once

#include "Game/World/FacilityLayout.h"
#include "Game/World/FacilityMap.h"

namespace pred
{

// What runs through a building to keep it going, drawn (FacilityMap::Dressing in Blueprint::furnishing, nothing felt): along every
// corridor under its ceiling, a cable tray with its cables on one walled side and a pair of pipes on brackets on the other; in
// each plant room its big pipes on hangers; in each room an air diffuser in the ceiling; and the crawlspace's mouths framed as
// the vents they are, each one's grille taken off and left on the floor beside it. Only a plan, like the building's.
namespace FacilityServices
{

void Fit(const FacilityLayout& layout, FacilityMap::Blueprint& out);

} // namespace FacilityServices

} // namespace pred
