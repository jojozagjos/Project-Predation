#include "Game/Weapons/ShotResolver.h"

#include <glm/geometric.hpp>

namespace pred
{

ShotResult ResolveShot(const PhysicsWorld& physics, const FireEvent& event, BodyHandle ignore)
{
    ShotResult result;
    if (event.range <= 0.0f || glm::length(event.direction) < 1e-6f)
    {
        return result;
    }

    const RayHit hit = physics.RayCast(event.origin, event.direction, event.range);
    if (!hit || hit.body == ignore)
    {
        // A ray that starts inside the shooter's own capsule is the common case here; treating it
        // as a miss is right, because the round has not reached anything else.
        return result;
    }

    result.hit = true;
    result.position = hit.position;
    result.normal = hit.normal;
    result.distance = hit.distance;
    result.body = hit.body;
    result.damage = event.damage;
    // Level geometry, as far as this knows. A dynamic prop struck here would take a mark that then
    // moves with it, which is wrong in the same way and not yet worth a motion-type query for: the
    // only dynamic bodies in the game are the ones a console command drops.
    result.surface = true;
    return result;
}

} // namespace pred
