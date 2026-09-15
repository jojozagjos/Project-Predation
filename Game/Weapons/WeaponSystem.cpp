#include "Game/Weapons/WeaponSystem.h"

#include "Engine/Animation/IK.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{
namespace WeaponSim
{
namespace
{

// A small integer hash. Wanted rather than a random number generator because the same shot has to
// deviate the same way on every machine that simulates it, and a generator carries state that would
// have to be replicated and kept in step.
uint32_t Hash(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

float UnitFloat(uint32_t value)
{
    return static_cast<float>(Hash(value) & 0xffffffu) / static_cast<float>(0x1000000u);
}

} // namespace

void Equip(const WeaponDefinition& definition, WeaponState& state)
{
    state.weapon = definition.id;
    state.rounds = definition.magazineSize;
    state.reserve = definition.reserveOnPickup;
    state.fireCooldown = 0.0f;
    state.reloadRemaining = 0.0f;
    state.burstRemaining = 0;
    state.bloom = 0.0f;
}

float CurrentSpread(const WeaponDefinition& definition, const WeaponState& state)
{
    const float base = definition.spreadHip + (definition.spreadAim - definition.spreadHip) * state.aim;
    return std::min(base + state.bloom, definition.spreadMax);
}

glm::vec3 DeviateShot(const glm::vec3& direction, float coneDegrees, uint32_t sequence)
{
    const float length = glm::length(direction);
    if (length < 1e-6f || coneDegrees <= 0.0f)
    {
        return length > 1e-6f ? direction / length : glm::vec3(0.0f, 0.0f, -1.0f);
    }
    const glm::vec3 forward = direction / length;

    // Uniform over the disc rather than over the radius, so shots do not cluster at the centre more
    // than they should.
    const float radius = std::sqrt(UnitFloat(sequence * 2u + 1u)) * glm::radians(coneDegrees);
    const float angle = UnitFloat(sequence * 2u + 2u) * glm::two_pi<float>();

    // Any axis not parallel to the shot will do to build the cone's frame.
    const glm::vec3 reference =
        std::abs(forward.y) > 0.95f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 right = glm::normalize(glm::cross(forward, reference));
    const glm::vec3 up = glm::cross(right, forward);

    const glm::vec3 offset = (right * std::cos(angle) + up * std::sin(angle)) * std::tan(radius);
    return glm::normalize(forward + offset);
}

void Step(const WeaponDefinition& definition, const WeaponInput& input, WeaponState& state,
          const glm::vec3& muzzle, const glm::vec3& aimDirection, float dt,
          std::vector<FireEvent>& events)
{
    if (!state.HasWeapon())
    {
        state.aim = SmoothTowards(state.aim, 0.0f, 12.0f, dt);
        return;
    }

    // Sights come up and down on their own timer, so aiming is a commitment rather than a toggle.
    const float aimStep = dt / std::max(definition.aimSeconds, 0.01f);
    const float aimTarget = input.aim && !state.IsReloading() ? 1.0f : 0.0f;
    // Stepped towards the target and stopped there. Clamping to [0, 1] instead let a fully raised
    // weapon overshoot, reverse, and jitter a step below the target every other tick.
    state.aim = aimTarget > state.aim ? std::min(state.aim + aimStep, aimTarget)
                                     : std::max(state.aim - aimStep, aimTarget);

    state.fireCooldown = std::max(state.fireCooldown - dt, 0.0f);
    state.bloom = std::max(state.bloom - definition.spreadRecover * dt, 0.0f);
    // The target falls back towards nothing, and the applied angle chases the target.
    //
    // Both, in that order. The target is what a round adds to and what recovery takes from; the
    // applied angle is what the view is actually pushed by, and it lags the target by a few
    // hundredths of a second. That lag is the whole of the difference between a weapon that shoves
    // the camera and one that cuts it to a new angle between frames.
    state.recoilTargetPitch =
        SmoothTowards(state.recoilTargetPitch, 0.0f, definition.recoilRecover, dt);
    state.recoilTargetYaw = SmoothTowards(state.recoilTargetYaw, 0.0f, definition.recoilRecover, dt);

    if (state.IsReloading())
    {
        state.reloadRemaining -= dt;
        if (state.reloadRemaining <= 0.0f)
        {
            state.reloadRemaining = 0.0f;
            const int wanted = definition.magazineSize - state.rounds;
            const int taken = std::min(wanted, state.reserve);
            state.rounds += taken;
            state.reserve -= taken;
        }
        state.triggerWasDown = input.trigger;
        return;
    }

    // Reload on request, and also the moment the magazine runs dry with the trigger held, because
    // hunting for the key while something is coming at you is not the kind of difficulty we want.
    const bool wantReload = input.reload || (input.trigger && state.rounds <= 0);
    if (wantReload && state.reserve > 0 && state.rounds < definition.magazineSize)
    {
        state.reloadRemaining = definition.reloadSeconds;
        state.burstRemaining = 0;
        state.triggerWasDown = input.trigger;
        return;
    }

    const bool pulled = input.trigger && !state.triggerWasDown;
    if (pulled && definition.mode == FireMode::Burst)
    {
        state.burstRemaining = definition.burstCount;
    }

    bool wantsToFire = false;
    switch (definition.mode)
    {
    case FireMode::Auto:
        wantsToFire = input.trigger;
        break;
    case FireMode::Burst:
        wantsToFire = state.burstRemaining > 0;
        break;
    case FireMode::Single:
    default:
        wantsToFire = pulled;
        break;
    }

    // One round per tick at most. At sixty ticks a second that caps the rate at 3600 rounds per
    // minute, which is far above anything this game will carry.
    if (wantsToFire && state.CanFire())
    {
        const float interval = 60.0f / std::max(definition.roundsPerMinute, 1.0f);

        FireEvent event;
        event.sequence = state.shotCount;
        event.weapon = state.weapon;
        event.origin = muzzle;
        event.direction = DeviateShot(aimDirection, CurrentSpread(definition, state), state.shotCount);
        event.damage = definition.damage;
        event.range = definition.range;
        events.push_back(event);

        --state.rounds;
        ++state.shotCount;
        state.fireCooldown = interval;
        state.bloom = std::min(state.bloom + definition.spreadPerShot, definition.spreadMax);

        // Alternating sideways kick, so a long burst wanders rather than climbing in a straight
        // line. Derived from the shot count, so it is the same everywhere.
        const float side = (state.shotCount % 2u) == 0u ? 1.0f : -1.0f;
        state.recoilTargetPitch += definition.recoilPitch * (1.0f - state.aim * 0.35f);
        state.recoilTargetYaw += definition.recoilYaw * side * (1.0f - state.aim * 0.35f);

        if (state.burstRemaining > 0)
        {
            --state.burstRemaining;
        }
    }

    // And the view chases the kick, after this tick's round has been added to it rather than before.
    //
    // Before, the shot landed on the target and the applied angle spent a whole tick still at the
    // old value, so every round had sixteen milliseconds of nothing happening before it moved
    // anything. A trigger pull has to answer immediately even if the movement it starts takes a
    // moment to finish.
    const float rise = std::max(definition.recoilRise, definition.recoilRecover);
    state.recoilPitch = SmoothTowards(state.recoilPitch, state.recoilTargetPitch, rise, dt);
    state.recoilYaw = SmoothTowards(state.recoilYaw, state.recoilTargetYaw, rise, dt);

    state.triggerWasDown = input.trigger;
}

} // namespace WeaponSim
} // namespace pred
