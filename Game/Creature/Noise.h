#pragma once

#include <glm/vec3.hpp>

#include <cstdint>

namespace pred
{

// A sound, as something listening for prey hears it.
//
// Not audio. The creature never receives a sample of anything; it receives the fact that a sound
// happened, where, and how far it carries. Every system that makes a noise the player can hear --
// a footstep, a shot, a door -- also reports one of these, and the creature's hearing decides from
// distance and walls whether it reached it.
enum class NoiseKind : uint8_t
{
    Footstep,
    Gunshot,
    Impact, // where a round landed
    Door,
    Item,   // something dropped
    Voice
};

const char* NoiseKindName(NoiseKind kind);

struct Noise
{
    glm::vec3 position{0.0f};
    // How far away an ordinary listener can just make it out in the open, in metres. Walls between
    // halve it; a creature with sharp hearing hears it further.
    float reach = 10.0f;
    NoiseKind kind = NoiseKind::Footstep;
    // Who made it, when a player did. Hearing a footstep says roughly where that person is, which is
    // worth more than hearing that something, somewhere, went bang.
    int player = -1;
};

// How far each kind of sound carries, in one place so they can be weighed against one another.
namespace NoiseReach
{
inline constexpr float kFootstepWalk = 7.0f;
inline constexpr float kFootstepSprint = 14.0f;
inline constexpr float kFootstepCrouch = 3.0f;
inline constexpr float kFootstepProne = 1.5f;
inline constexpr float kGunshot = 70.0f;
inline constexpr float kImpact = 12.0f;
inline constexpr float kDoor = 14.0f;
inline constexpr float kItem = 8.0f;
inline constexpr float kVoice = 10.0f;
} // namespace NoiseReach

} // namespace pred
