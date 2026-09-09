#include "Game/Weapons/WeaponAppearance.h"

#include "Engine/Render/Primitives.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

namespace pred
{
namespace
{

// Appends a box at a position in the weapon's frame, optionally tilted about +X so the grip and the
// magazine rake back the way they do on a real firearm.
void AddPart(MeshData& out, const glm::vec3& size, const glm::vec3& centre, float pitchDegrees = 0.0f)
{
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), centre);
    if (std::abs(pitchDegrees) > 0.01f)
    {
        transform = transform * glm::rotate(glm::mat4(1.0f), glm::radians(pitchDegrees),
                                            glm::vec3(1.0f, 0.0f, 0.0f));
    }
    out.Append(Primitives::Box(size), transform);
}

} // namespace

WeaponVisual BuildWeaponVisual(const WeaponDefinition& definition)
{
    WeaponVisual visual;

    // The definition carries an overall footprint; the parts are proportions of it, so a short
    // sidearm and a long carbine both come out looking like themselves.
    const float length = std::max(definition.size.z, 0.12f);
    const float height = std::max(definition.size.y, 0.06f);
    const float width = std::max(definition.size.x, 0.03f);
    const bool longArm = length > 0.36f; // anything this size gets a stock and a handguard

    // Receiver: the block the hand sits under, running from behind the grip to over the handguard.
    AddPart(visual.body, {width, height * 0.44f, length * 0.46f}, {0.0f, height * 0.34f, length * 0.10f});

    // Pistol grip, raked back.
    AddPart(visual.body, {width * 0.78f, height * 0.82f, length * 0.13f},
            {0.0f, -height * 0.30f, -length * 0.02f}, -14.0f);

    if (longArm)
    {
        // Handguard, then a thinner barrel out of the front of it.
        AddPart(visual.body, {width * 0.86f, height * 0.34f, length * 0.30f},
                {0.0f, height * 0.30f, length * 0.42f});
        AddPart(visual.body, {width * 0.34f, height * 0.22f, length * 0.24f},
                {0.0f, height * 0.32f, length * 0.68f});
        // Stock, out the back, level with the receiver so the sights line up along it.
        AddPart(visual.body, {width * 0.80f, height * 0.50f, length * 0.30f},
                {0.0f, height * 0.30f, -length * 0.28f});
    }
    else
    {
        // A sidearm is a slide over a barrel, with no stock and nothing to hold out front.
        AddPart(visual.body, {width * 0.92f, height * 0.30f, length * 0.44f},
                {0.0f, height * 0.42f, length * 0.28f});
    }

    // Sight block on top. Its top face is the sight line, which is what aiming puts on the view
    // axis, so where it sits decides how the weapon is held when the sights come up.
    const float sightBase = height * (longArm ? 0.56f : 0.60f);
    const float sightBlock = height * 0.20f;
    AddPart(visual.body, {width * 0.32f, sightBlock, length * 0.09f},
            {0.0f, sightBase + sightBlock * 0.5f, length * (longArm ? 0.16f : 0.30f)});
    // A front post, so there is something to line the rear block up with.
    AddPart(visual.body, {width * 0.16f, sightBlock * 0.9f, length * 0.04f},
            {0.0f, sightBase + sightBlock * 0.45f, length * (longArm ? 0.62f : 0.44f)});

    visual.sightHeight = sightBase + sightBlock;

    // Magazine: its own mesh, built about its own top so a reload can slide it straight down.
    const float magazineDepth = height * (longArm ? 1.05f : 0.86f);
    AddPart(visual.magazine, {width * 0.70f, magazineDepth, length * 0.13f},
            {0.0f, -magazineDepth * 0.5f, 0.0f}, -6.0f);

    visual.magazineSeated = {0.0f, -height * 0.10f, length * (longArm ? 0.14f : 0.0f)};
    visual.supportGrip = {0.0f, height * 0.16f, longArm ? length * 0.42f : length * 0.16f};
    visual.muzzle = {0.0f, height * 0.32f, length * (longArm ? 0.80f : 0.50f)};
    return visual;
}

} // namespace pred
