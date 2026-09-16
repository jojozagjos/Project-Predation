#pragma once

#include <glm/vec3.hpp>

#include <cstdint>
#include <string>

namespace pred
{

using WeaponId = int;
inline constexpr WeaponId kInvalidWeapon = 0;

enum class FireMode : uint8_t
{
    Single, // one round per pull
    Auto,   // keeps firing while held
    Burst   // a fixed count per pull
};

const char* FireModeName(FireMode mode);
FireMode FireModeFromString(const std::string& name);

// A weapon's tuning, loaded from weapons.json. Nothing here changes at runtime.
struct WeaponDefinition
{
    WeaponId id = kInvalidWeapon;
    std::string key;  // stable identity for data files and network messages
    std::string name; // shown to the player
    std::string item;  // the inventory item that carries this weapon
    // Names a file in Assets/Models. When set, that model is what the player holds, which is how
    // anything built in the editor gets into the game. Empty means use the built-in shape.
    std::string model;

    FireMode mode = FireMode::Single;
    int burstCount = 3;
    float roundsPerMinute = 320.0f;
    int magazineSize = 12;
    int reserveOnPickup = 36;
    float reloadSeconds = 1.9f;

    float damage = 24.0f;
    float range = 60.0f;

    // Cone half-angles in degrees. Aiming down the sights tightens it and slows you down.
    float spreadHip = 3.0f;
    float spreadAim = 0.55f;
    float spreadPerShot = 0.6f; // added while firing, decays back
    float spreadMax = 6.0f;
    float spreadRecover = 5.0f;

    // Recoil kicks the view; it decays back towards where the player was aiming, so a burst walks
    // upwards and then settles rather than permanently stealing their aim.
    // Degrees per shot, and the view keeps them. These are therefore much smaller than they were
    // when the kick decayed away: a carbine emptying thirty rounds at 0.6 degrees apiece climbs
    // eighteen degrees, which is a magazine's worth of pulling down.
    float recoilPitch = 1.4f;
    float recoilYaw = 0.35f; // alternating sides, so a burst wanders as well as climbs
    // Left in place because the spread still reads it, and no longer used to take the kick back.
    float recoilRecover = 9.0f;
    // How fast the view chases the kick, as a rate. Higher is snappier; this is the difference
    // between a weapon that shoves the camera and one that teleports it. Around 25 puts most of a
    // round's kick in under a tenth of a second, which reads as a shove.
    float recoilRise = 26.0f;
    // The shove, in degrees, and how quickly it settles. A spring rather than a decay, because what
    // a shot does to a camera is knock it and let it come back -- a little past centre and then
    // still, which is what reads as weight rather than as a slow drift home.
    float shakeAmount = 1.1f;
    float shakeStiffness = 220.0f;
    float shakeDamping = 22.0f;

    float aimSeconds = 0.22f;   // time to raise the sights
    float aimSpeedScale = 0.5f; // movement speed multiplier while aimed

    // Placeholder appearance, in the same spirit as the item shapes.
    glm::vec3 size{0.07f, 0.16f, 0.46f};
    glm::vec3 color{0.16f, 0.17f, 0.19f};
    float muzzleForward = 0.30f; // from the grip to the muzzle, along the barrel
};

// Everything the weapon simulation needs from the player this tick.
struct WeaponInput
{
    bool trigger = false;
    bool aim = false;
    bool reload = false;
};

// The weapon's simulation state. Together with a sequence of WeaponInputs this reproduces exactly,
// which is what lets a client predict its own firing and a host confirm it.
struct WeaponState
{
    WeaponId weapon = kInvalidWeapon;
    int rounds = 0;  // in the magazine
    int reserve = 0; // spare rounds carried

    float fireCooldown = 0.0f;    // seconds until the next round may leave the barrel
    float reloadRemaining = 0.0f; // > 0 while reloading
    float aim = 0.0f;             // 0 hip, 1 fully aimed
    float bloom = 0.0f;           // extra spread from sustained fire, degrees

    // Recoil still owed to the view, in degrees, and how much of it is being handed over this tick.
    //
    // The view *keeps* what it is given. That is the whole difference between recoil you fight and
    // recoil you watch: an offset that decays back to nothing returns the player's aim to exactly
    // where it was, so the weapon climbs on screen and then politely un-climbs, and there is never
    // anything to pull down against. Reported as "it's not moving my camera up -- when I'm done
    // shooting my camera goes back to where it was", which is precisely what it was doing.
    //
    // So a round does not set an offset, it owes the view an angle. `pending` is what is owed and
    // `kick` is the instalment paid this tick; the game adds the instalment to the player's own look
    // angles, where it stays until they move the mouse. Paying it over a few tens of milliseconds
    // rather than all at once is what keeps a burst from being a stack of cuts.
    float recoilPitch = 0.0f; // owed, not yet given to the view
    float recoilYaw = 0.0f;
    float kickPitch = 0.0f; // degrees to add to the view this tick
    float kickYaw = 0.0f;
    // And a shove that is given back.
    //
    // Two different things happen to a camera when a weapon fires and they are easy to conflate. One
    // is that the aim genuinely moves and stays moved -- that is the climb above, and it is what the
    // player fights. The other is the shove: the whole view jolts and settles again within a
    // fraction of a second, moving the aim nowhere at all. Without the first there is nothing to
    // fight; without the second a shot has no weight, which is why taking the weapon's own tilt away
    // left it feeling flat.
    //
    // This is the second. It is added to the view for drawing only and never reaches the look
    // angles, so it cannot steal anybody's aim.
    float shakePitch = 0.0f;
    float shakeYaw = 0.0f;
    float shakeVelocityPitch = 0.0f;
    float shakeVelocityYaw = 0.0f;

    bool triggerWasDown = false;
    int burstRemaining = 0;
    // Counts every round this weapon has fired. It seeds the spread, so the host and the client
    // that predicted the shot deviate the round the same way without sending the direction.
    uint32_t shotCount = 0;

    bool HasWeapon() const { return weapon != kInvalidWeapon; }
    bool IsReloading() const { return reloadRemaining > 0.0f; }
    bool CanFire() const { return HasWeapon() && rounds > 0 && fireCooldown <= 0.0f && !IsReloading(); }
};

// A round leaving the barrel. The simulation produces these; whoever has authority resolves them
// against the world. Clients raise them for their own feel and send them up; the host validates and
// is the only one that turns them into damage.
struct FireEvent
{
    uint32_t sequence = 0; // the shooter's shot count at the moment of firing
    WeaponId weapon = kInvalidWeapon;
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f}; // already deviated by spread
    float damage = 0.0f;
    float range = 0.0f;
};

} // namespace pred
