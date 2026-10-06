#pragma once

#include <bgfx/bgfx.h>
#include <glm/mat4x4.hpp>

#include <cstdint>

namespace pred
{

// Which way depth runs in the world's pictures (the game's view, its reflections, the item icons, the live offscreen views):
// reversed -- 1 at the eye, 0 far off -- in a buffer of floats, wherever the backend's depth runs 0 to 1.
//
// A standard depth buffer spends nearly all its precision within a few metres of the eye: with the eye's near plane at five
// centimetres and 24 bits of depth, two surfaces a couple of centimetres apart a hundred metres off could not be told apart,
// and they flickered -- paint on a road, a pad on a slab, a sign on a wall. Reversed into floats, the precision of a float's
// exponent cancels the projection's crowding towards the eye, and it is near enough even all the way out: millimetres at a
// kilometre. Faces that truly lie in one plane still fight (see the tests that look for them); nothing else does.
//
// Where depth runs -1 to 1 (OpenGL without clip control) reversing gains nothing, and the standard convention is kept.
// Everything that draws into one of these pictures with a depth test takes the test, the clear and the projection from here.
// Pictures of their own -- shadow maps, the navigation map -- keep the standard convention.
namespace Depth
{

// Set once, when the renderer starts: whether the backend's depth is 0 to 1, and whether floats can be drawn to with MSAA.
void Configure(bool homogeneous, bool floatTargets);
bool Reversed();
// The test that keeps the nearer: greater when reversed, less otherwise.
uint64_t Test();
// What a depth buffer is cleared to: the far end.
float Clear();
// The format of a depth target.
bgfx::TextureFormat::Enum Format();
// A perspective projection in this convention, right-handed, looking down -z.
glm::mat4 Perspective(float verticalFov, float aspect, float nearPlane, float farPlane);

} // namespace Depth

} // namespace pred
