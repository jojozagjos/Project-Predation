#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <vector>

namespace pred
{

// The craft a team arrives in and leaves on: standing on the landing pad on four short legs, its ramp down at the back
// towards the site, a cabin to stand in and a console at the front of the cabin to launch from.
//
// Only its shape. SiteMap builds it, and the mission asks where its cabin and its console are. Everything is given
// from `base`, the middle of it on the pad's surface, and `yaw`, which way its ramp faces -- turned the way a look is,
// so 0 faces -z and the site's landing yaw can be used as it is.
namespace Shuttle
{

struct Piece
{
    glm::vec3 centre{0.0f};
    glm::vec3 size{1.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    bool trim = false; // the engines, the nose and the fin: a darker finish than the hull
};

// Every box of it, the ramp included.
std::vector<Piece> Pieces(const glm::vec3& base, float yaw);

// Whether a point -- somebody's feet, a drive lying on the floor -- is in its cabin.
bool Aboard(const glm::vec3& base, float yaw, const glm::vec3& at);

// Where everybody arrives: standing in the middle of the cabin, facing down the ramp.
glm::vec3 Arrival(const glm::vec3& base, float yaw);

// The launch console at the front of the cabin, against the front wall: its middle, its size, and how it is turned
// (its screen faces down the cabin, towards the ramp).
glm::vec3 Console(const glm::vec3& base, float yaw);
glm::vec3 ConsoleSize();
glm::quat Turn(float yaw);

// The lamp in the cabin's ceiling.
glm::vec3 CabinLamp(const glm::vec3& base, float yaw);

} // namespace Shuttle

} // namespace pred
