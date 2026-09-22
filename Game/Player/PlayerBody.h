#pragma once

#include "Engine/Animation/Skeleton.h"
#include "Engine/Scene/Scene.h"
#include "Game/Player/PlayerTypes.h"
#include "Game/Player/Ragdoll.h"
#include "Game/Weapons/WeaponAppearance.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <string>
#include <vector>

namespace pred
{

class MeshLibrary;
class PhysicsWorld;
class DebugDraw;
struct PlayerView;

// Named joints of the humanoid rig, so gameplay code never looks bones up by string at runtime.
struct HumanoidRig
{
    BoneIndex pelvis = kInvalidBone;
    BoneIndex spine = kInvalidBone;
    BoneIndex chest = kInvalidBone;
    BoneIndex neck = kInvalidBone;
    BoneIndex head = kInvalidBone;

    std::array<BoneIndex, 2> shoulder{kInvalidBone, kInvalidBone};
    std::array<BoneIndex, 2> upperArm{kInvalidBone, kInvalidBone};
    std::array<BoneIndex, 2> lowerArm{kInvalidBone, kInvalidBone};
    std::array<BoneIndex, 2> hand{kInvalidBone, kInvalidBone};

    std::array<BoneIndex, 2> upperLeg{kInvalidBone, kInvalidBone};
    std::array<BoneIndex, 2> lowerLeg{kInvalidBone, kInvalidBone};
    std::array<BoneIndex, 2> foot{kInvalidBone, kInvalidBone};

    float upperLegLength = 0.44f;
    float lowerLegLength = 0.44f;
    float upperArmLength = 0.33f;
    float lowerArmLength = 0.26f;
    float ankleHeight = 0.07f;
    float height = 1.8f;
};

// The player's own body, visible when looking down.
//
// There is no authored animation and no rigged mesh yet, so the body is a skeleton driven entirely
// procedurally and drawn as one box per segment. That is enough to answer the question this
// milestone exists to answer, which is whether the camera feels attached to a physical body, and it
// builds the skeleton, pose and IK machinery the creatures will need later.
//
// The camera stays authoritative: the body is positioned to agree with the view rather than the
// view being driven by the head bone. Driving a first-person camera from an animated head is what
// produces motion sickness.
class PlayerBody
{
public:
    struct Config
    {
        // Gait
        float stepHeight = 0.14f; // how far a swinging foot lifts
        // And how much of that is left when the body is right down. A crouched foot skims the floor:
        // there is no room under a folded leg to pick it up any further.
        float crouchStepScale = 0.50f;
        float footPlantSmoothing = 22.0f;
        float weaponHandSmoothing = 26.0f;
        // Where the trigger grip sits when a weapon is carried rather than aimed, in the view frame.
        //
        // Further out and less far down than a real hand would be, and deliberately. At a 90 degree
        // horizontal field of view the frame stops about 29 degrees below the view axis, and a hand
        // held where a hand actually goes is below that: the whole weapon drops off the bottom of
        // the screen and the player is holding something they cannot see. The ratio of the drop to
        // the reach is what decides whether it is in frame, and half of it puts the hold just
        // inside the bottom edge, which is where a carried weapon belongs.
        float weaponReadyRight = 0.145f;
        float weaponReadyDown = -0.190f;
        float weaponReadyForward = 0.38f;
        float weaponAimForward = 0.42f;
        // And how that carry changes for something short. A pistol is not held where a carbine is:
        // there is no stock to tuck in, both hands go on the one grip, and the whole thing is
        // pushed out and up towards the eye line on the middle of the body. Held at a carbine's
        // grip it points at the floor beside your hip with a forearm across half the screen.
        // Blended in by how short the weapon is, so nothing has to be told which it is.
        float weaponShortForward = 0.04f;  // further out
        float weaponShortRise = 0.045f;    // and higher
        float weaponShortRight = -0.06f;   // and in towards the middle, where two hands can meet
        // How far the muzzle is turned in across the body while the weapon is carried, in degrees.
        // A rifle held ready is angled inwards, not pointed straight down the lane, and the angle
        // is what brings the support hand back towards the centre line where the other arm can
        // actually reach it: held square, the handguard sits out beyond the left arm entirely.
        float weaponReadyInward = 7.0f;
        // How far the torso blades towards the weapon while one is held, in degrees. Nobody shoots
        // square on: the support side comes forward and the firing side goes back, and that is not
        // only how it looks but where the reach comes from. Held square, the support shoulder is a
        // shoulder's width across the body from the handguard and the arm cannot get there, so the
        // hand slid back down the barrel and stopped following the socket it was given.
        float weaponCarryTurnDegrees = 7.0f;
        // And how far the support shoulder rolls forward and in while a weapon is held. This is
        // where the reach for a handguard comes from. Turning the whole torso far enough to supply
        // it reads, in first person, as the player standing skewed towards their own gun.
        float weaponShoulderForward = 0.075f;
        float weaponShoulderIn = 0.030f;
        // However crowded it gets, the sights never come closer to the eye than this. The pull-back
        // against a wall is measured from the eye, so at full aim it pulls the weapon straight down
        // the view axis and into the player's face: the near plane cuts the receiver open and you
        // end up looking at the inside of your own gun. A barrel a little way into a wall is the
        // cheaper fault, and the muzzle trace already keeps that honest for the part you can see.
        float weaponAimMinForward = 0.30f;
        // How long the reload movement takes to finish after the reload itself has. Cutting it off
        // at the moment the weapon becomes usable again is what made a reload end with a jump.
        float reloadFollowThrough = 0.28f;
        // How much of the view pitch a carried weapon follows. Following it fully swung the gun
        // round behind the player whenever they looked straight down. Aiming raises this to one,
        // because the sights have to line up with the view exactly.
        // All of it. Nine tenths meant the barrel was always a little behind the view, which
        // compounds with the limit above into the weapon visibly giving up part way. Where the
        // weapon is held is limited; where it points follows.
        float weaponCarryPitchFollow = 1.0f;
        // And how much of it a weapon follows downwards while the body is flat. A prone player has a
        // floor a hand.s width under the weapon and can still look ninety degrees down; following
        // that puts the barrel through the ground, and nothing afterwards can take it back out
        // because the thing it is pointing at is the thing the player is lying on.
        float weaponPronePitchFollow = 0.22f;
        // And how far up a carried weapon may point, in degrees, however far up the player looks,
        // and the angle it starts easing towards that at. Nobody raises a rifle at the sky to look
        // at it; equally, a weapon that tracks the view and then stops dead at one angle reads as
        // having come loose from it. Below the knee it follows exactly, above it the movement
        // shrinks with every further degree. See where they are used.
        // How much of the view pitch the *position* of the weapon follows, as opposed to the way it
        // points.
        //
        // One, and it has to be. Less than one is anatomically truer -- a person looking up does not
        // raise their rifle to their forehead -- and it is wrong for a first-person game, because a
        // weapon whose position does not pitch with the view slides down the screen and out of frame
        // as the player looks up. It was tried at 0.35 to stop the gun ending up over the head near a
        // wall and the player reported the gun no longer following the camera, which is exactly that.
        // The wall case is solved by weaponWallTipDrop instead, which only acts when there is a wall.
        //
        // At one it follows the view entirely, and that is its own fault: measured with the shipped
        // carbine in an empty room, a level view puts the grip 0.21 m below the eye and an eighty
        // degree look puts it 0.19 m above, with the muzzle 0.58 m higher again. No wall involved --
        // looking up swings the whole weapon over the head like a boom, because the offset it sits on
        // is measured in a frame that pitches with the view.
        //
        // Lowering it was tried again at 0.5 and the test caught it: the weapon moved 0.35 m in the
        // view frame, which is out of shot. That is not a tuning failure, it is geometry. Keeping a
        // weapon in the same place on screen means keeping it in the same place relative to the view,
        // and a view that pitches up carries it up with it; a pose that looks right on your own
        // screen and a pose that looks right from outside are not the same pose, and this game has
        // one body for both by design.
        //
        // So it stays at one, and the case that actually gets reported -- the receiver ending up over
        // the head -- is handled where it comes from, which is the muzzle correction at a wall. That
        // one really is length-dependent, and it is now measured from each weapon rather than
        // assumed: see weaponWallTipDrop.
        float weaponCarryRise = 1.0f;
        float weaponCarryPitchKnee = 24.0f;
        // How far up the barrel is allowed to come, in the open and with a wall in the way.
        //
        // It used to be one number, fifty-five, and it was low for a reason that only applies next
        // to a wall: the muzzle correction stops being able to help once the barrel points over the
        // top of what it is avoiding, so at a steep enough look it collapses and the barrel swings.
        // Keeping the barrel well under that kept the correction in the range it behaves in.
        //
        // But it was being paid for everywhere, including in an open field with nothing in front of
        // the player, where it reads as the weapon stopping dead while the view carries on -- "I
        // still can't look straight up with the gun". A person tipping their head right back does
        // bring the muzzle most of the way up with it.
        //
        // So the limit is low only when there is a wall in play, and the game blends between them by
        // how much the muzzle correction is actually doing. Nothing in front of you, and the barrel
        // follows almost all the way; a corridor, and it stays where the correction can still work.
        // Effectively no limit any more, and it is the hold cap below that made that safe.
        //
        // This number existed to stop the rifle standing on end beside the head, and it was doing
        // that by refusing to let the barrel follow the view -- which is what "the gun should follow
        // my camera all the way up, it stops at a section" is: eased towards 82 degrees, an
        // eighty-five degree look put the barrel at sixty-two, and the twenty-three degree gap is
        // visible as the weapon giving up.
        //
        // But standing on end was never about where the barrel points. It was about where the
        // weapon is *held*, and that has its own limit now. With the hands kept at chest height, a
        // barrel tracking the view all the way up is a person tilting a rifle up while their hands
        // stay where hands go, which is exactly right and exactly what was asked for.
        float weaponCarryPitchMaxUp = 89.0f;
        float weaponCarryPitchMaxUpNearWall = 55.0f;
        // And the same for where the weapon is *held*, as opposed to where it points. Below the knee
        // the hold follows the view exactly and nothing about normal play changes; above it, it eases
        // towards the cap, which is the angle at which the hands sit level with the eye rather than
        // over the head. See where these are used for the measurements.
        float weaponHoldPitchKnee = 20.0f;
        float weaponHoldPitchMaxUp = 30.0f;
        // The weapon lags a turn and then catches up, which is what gives it weight.
        float weaponSwayAmount = 0.34f;   // how far a turn drags the weapon behind the view
        float weaponSwayRecover = 11.0f;  // how fast it catches up again
        float weaponBreatheAmount = 0.018f; // the small movement of a weapon in someone's hands
        float weaponWalkAmount = 0.018f;  // how much walking swings it, on the stride's own phase
        // Against a wall a long weapon comes in and comes up. Traced from the eye, so it reacts to
        // what is actually in front of the muzzle rather than to what the capsule is touching.
        float wallCheckDistance = 1.10f;
        float wallCheckSpeed = 12.0f;
        float weaponWallForward = 0.34f;  // how far forward it still reaches when crowded
        // However crowded it is, the hold never comes closer to the eye than this along the view.
        // The eye sits above and behind the hold, so pulling in without a floor eventually pulls it
        // into the camera, and the near plane then cuts the receiver open.
        float weaponMinForward = 0.20f;
        // And the back of the weapon never comes closer to the eye plane than this. The floor above
        // holds the grip out; a stock is a further quarter of a metre behind the grip, and it is the
        // stock that ends up on the wrong side of the near plane when a corridor closes in.
        float weaponRearMinForward = 0.11f;
        // How far the muzzle drops when there is a wall in front of it, in degrees.
        //
        // A dip, not a swing. This was sixty degrees on the reasoning that lowering makes room where
        // pulling back only moves the problem from the wall to the camera, and it does, but sixty
        // degrees points a carried rifle at your own feet the moment you brush a doorframe: what it
        // reads as is the weapon falling out of the hold. Pulling back is what a person does with a
        // rifle in a corridor, and the floors above are what keep the camera out of it.
        float weaponWallLower = 14.0f;
        // And how far it is allowed to drop once the barrel is actually crossing something, in
        // degrees, with how fast it gets there.
        //
        // The dip above is a guess made from how boxed in the player is. This is the answer to the
        // question itself: the surface the barrel is crossing is traced for, and the weapon turns
        // about the point it is held by until the muzzle is in front of that surface and no
        // further. Turning about the hold is free in a way that pulling back is not, because the
        // hold does not move and so the receiver does not come any nearer the camera.
        //
        // It applies while aiming too, and that is the trade. Nobody can aim a rifle through a
        // wall: a barrel is half a metre long, a player can stand a third of a metre from a wall,
        // and the near plane decides how far the weapon may come back. Forty centimetres of barrel
        // in the bricks was what the sights used to cost. The barrel comes down instead, the
        // sights stop meaning anything for as long as you are that close, and shooting traces from
        // the eye regardless so nothing about where a round goes changes.
        //
        // Capped short of what would clear every wall, because past about half a right angle this
        // stops reading as a hold and starts reading as presenting arms. At eighty degrees a player
        // who walked up to a wall while looking upwards had the rifle stood vertically beside their
        // head with both arms folded around it, which is not a thing anybody does with a rifle.
        //
        // Sixty-seven, and not lower, because lowering it further breaks something worse.
        //
        // There are three things wanted here and only two can be had: the barrel out of the wall,
        // the receiver out of the camera, and the sights on the view axis. Tipping less leaves the
        // weapon further forward, so the pull-back has to take it further in, and below about
        // sixty-five the back of the stock crosses the near plane and the player is looking at the
        // inside of their own gun. That is the worse fault of the two, so this is the floor rather
        // than a preference. Eighty left about two centimetres of a sixty-two centimetre rifle in a
        // wall; sixty-seven leaves about ten, none of which is visible from the player's own eye
        // because it is on the far side of the wall face.
        //
        // Getting properly below this needs the hold to move rather than only rotate: somebody in a
        // tight space brings a rifle in against the chest, they do not pivot it about the grip
        // until the muzzle points at the floor. That is an animation, not a number, and it is the
        // real answer whenever it gets written.
        float weaponWallTipMax = 67.0f;
        // And how far the hold comes down as it tips. Rotating a weapon nose-down about the grip
        // swings its stock up, and at the angles a corridor asks for that puts the receiver over the
        // player.s head. A person tipping a rifle down drops their hands at the same time; without
        // this the hands stay put and only the gun pivots, which is where "it teleports above my
        // head" came from.
        //
        // A share of how far the stock actually rose, not a distance. It was 0.17 m, and a fixed
        // distance is only ever right for one weapon: a rifle whose stock sits a metre behind the
        // grip lifts it three times as far as a carbine's does at the same angle, so the number that
        // hid it on the carbine left the long weapon's receiver above the eye. That is why this was
        // reported as happening "only with the longer guns like the ACRD" -- the correction was the
        // same size for every weapon and the fault it was correcting was not.
        //
        // Measured from the weapon's own rear point, so it is the same rule for all of them.
        float weaponWallTipDrop = 0.80f;
        // How far below the eye the hands are held at the very most. Only ever reached by looking
        // almost straight up, where keeping the weapon still on screen would otherwise put it over
        // the head: see where this is used.
        float weaponHoldEyeMargin = 0.06f;
        // The most the whole weapon may be lifted to get its muzzle out of the floor, after un-tipping
        // has done what it can. Small on purpose: this moves the hands, and it used to be 0.45 m,
        // which is where the gun ending up over the player.s head came from.
        float weaponGroundLiftMax = 0.10f;
        float weaponWallTipSpeed = 13.0f;
        // How much barrel may be inside something before the drop starts, and over how much more it
        // comes fully in. Both in metres. See the note where they are used: the drop is a
        // second-order lever, so an exact answer has a step in it at the moment of contact, and
        // these are what turn that step into a slope.
        // How far either side of the muzzle the traces are spread, in metres. This is the width of
        // the barrel movement over which the correction lets go of an edge: too small and it lets go
        // in a jump, too large and the weapon starts reacting to walls the barrel is nowhere near.
        float weaponWallTipProbe = 0.09f;
        float weaponWallTipSlack = 0.02f;
        float weaponWallTipFade = 0.20f;
        // How much of the barrel may be inside something before the sights refuse to come up, and
        // how much further out of it the player has to get before they may come up again. The
        // second number is what stops standing exactly on the line from flickering the sights.
        float weaponAimAllowance = 0.10f;
        float weaponAimHysteresis = 0.12f;
        // How fast the sights come down when the room for them runs out. Quick enough to read as
        // the weapon being brought in, slow enough not to be a cut.
        float weaponAimBreakSpeed = 9.0f;

        // The pull-back above is a soft rule measured along the view, which is why a gun still went
        // through a wall the player was looking sideways at: the trace and the barrel were pointing
        // in different directions. These are the hard limits, traced along the barrel itself and
        // straight down, so the muzzle stops at whatever is really in front of it and a hold does
        // not sink into the floor when the body lies down on it.
        float muzzleClearance = 0.05f;
        float heldItemClearance = 0.11f;
        // A swinging foot follows an already-smooth arc, so it tracks its target almost exactly. It
        // has to: any lag here lands the foot short of where the step was aimed, and it spends the
        // stance catching up, which is a visible skid at every touchdown.
        float footSwingSmoothing = 70.0f;
        // How much of the reachable leg length a step is allowed to use. Going right to the limit
        // leaves the knee locked straight at the end of every stance, which reads as stiff.
        float stepReachMargin = 0.99f;
        // How far the knee pole tips downwards as the leg folds. Straight out in front is where a
        // knee goes when the leg is nearly straight; a folded one goes forward and down.
        float kneeDropWhenFolded = 1.35f;
        // How far above the player a foot may be planted. A stair step or a kerb, not the top of a
        // wall the player is standing next to.
        float maxFootRise = 0.45f;
        // And how far below. A foot whose target hangs over an edge would otherwise reach the floor
        // underneath, which puts the leg through whatever the player is standing on.
        float maxFootDrop = 0.40f;
        float hipSwayAmount = 0.035f;
        float hipBobAmount = 0.030f;

        // Posture
        float leanPerSpeed = 1.4f;    // degrees of forward lean per m/s
        float maxLean = 12.0f;
        float armSwingDegrees = 22.0f;
        // Splays the arms clear of the thighs. Anything under about 10 degrees and the upper arms
        // overlap the legs at this build's proportions.
        float armRestDegrees = 13.0f;

        // One posture per stance, blended between. Limb lengths never change: lowering the hips and
        // letting the knees bend is what actually happens when someone crouches.
        struct StancePose
        {
            float pelvisPitchDeg = 0.0f;  // tips the whole body; this is what lays it down for prone
            float spineLeanDeg = 0.0f;    // torso folds forward over the hips; this is the crouch
            float footBackRatio = 0.0f;   // feet behind the hips, as a fraction of total leg length
            float footSpread = 1.0f;      // stance width multiplier
            float armForwardDeg = 0.0f;
        };

        // Crouching folds at the hip and knee with the pelvis kept upright. Pitching the pelvis
        // instead threw the legs out behind and read as a ski jump rather than a squat.
        //
        // The lean is large because the hips have to come up to meet it. Dropping the eye to
        // crouch height with the torso near-upright leaves the hips so low that the leg folds double
        // and the thigh ends up flat: knees out in front, feet under the body, nothing under the
        // hips at all, which reads as sitting on an invisible chair. Folding the torso forward is
        // what a real crouch does with the height it has to lose, and it lets the hips sit high
        // enough for the thigh and shin to share the bend evenly.
        //
        // The feet then sit under the hips rather than in front of them, so the body has something
        // beneath it. See crouchBodyForward, which keeps the pelvis over the capsule while it does.
        StancePose stand{0.0f, 0.0f, 0.00f, 1.00f, 0.0f};
        StancePose crouch{0.0f, 46.0f, 0.15f, 1.20f, 10.0f};
        // Prone lays the pelvis flat so the spine continues horizontally. The feet go almost a full
        // leg length back so the legs lie out straight; leaving slack let the knees fold up into the
        // air, because once the pelvis is flat the knee's bend direction points at the sky.
        // Arms need no extra rotation here: with the pelvis flat, "hanging down" in body space
        // already points forward along the ground.
        StancePose prone{87.0f, 0.0f, 0.97f, 0.80f, 0.0f};

        float stanceBlendSpeed = 7.0f;

        // Hips follow the direction of travel; the torso twists back to stay aimed where the player
        // is looking. Locking the whole body to the camera makes it look welded to the view, because
        // nothing ever rotates relative to it.
        float hipTurnSpeedMoving = 9.0f;
        float hipTurnSpeedIdle = 6.0f;
        float maxTorsoTwistDegrees = 55.0f; // past this, the hips turn to catch up
        float turnInPlaceDegrees = 42.0f;   // how far a turn-in-place swings the hips

        // Crawling. Prone movement is driven by the arms: the hands reach forward, plant, and pull
        // the body along, with the legs pushing on the opposite beat.
        float crawlCycleLength = 1.1f; // metres of travel per full reach-and-pull cycle
        float crawlReach = 0.30f;      // how far the hands swing fore and aft
        float crawlLift = 0.10f;       // how far a hand lifts while swinging forward
        float crawlHandForward = 0.42f; // where the hands plant relative to the shoulders
        // How much air a crawling hand leaves between itself and whatever it is reaching into. A
        // crawl reaches forward and out, which in a vent is into the wall either side.
        float crawlHandClearance = 0.09f;
        float crawlLegDraw = 0.40f;     // how far a knee swings out to the side as it is drawn up
        float crawlShoulderRollDegrees = 9.0f; // shoulders roll as each arm reaches and pulls
        // And the hips turn the other way. A body pulling itself along does not move as one piece:
        // the shoulder that reaches drives that hip back and the spine carries the twist.
        float crawlHipTwistDegrees = 6.0f;
        // A prone body holding still breathes, because everything else in the pose is scaled by how
        // fast it is moving and stopping switched all of it off at once. Degrees of rise and fall
        // through the chest, and how many radians a second the clock runs at.
        float proneBreatheDegrees = 1.9f;
        float proneBreatheRate = 1.15f;
        // How fast the body settles onto the slope it is lying on, per second. Slow enough that
        // crawling over a step does not snap it, fast enough that it is lined up by the time
        // anybody looks.
        float proneGroundFollow = 6.0f;
        // Prone turning is slow and deliberate: the body pivots towards where you are crawling.
        float proneTurnSpeed = 3.2f;
        // Turned further than this from the way the body is lying, a prone body shuffles round on
        // its front to catch up, at this rate. Rolling onto your back is gone; see ADR-018.
        float pronePivotSpeed = 4.2f;
        float pronePivotDegrees = 55.0f;

        // Climbing. The hands take the lip of the ledge while the body rises, then let go as it
        // comes over the top.

        float mantleGripSpread = 0.13f;  // how far apart the hands go, as a fraction of height
        float mantleReleaseAt = 0.62f;   // how far through the climb the hands let go
        // How long the hands take to come off the ledge after the climb ends. Cutting straight back
        // to the normal arms moved them somewhere else in a single frame.
        float mantleArmFadeSeconds = 0.28f;
        // And what the legs do while the arms are on the ledge. A knee comes up onto the top
        // partway through and the other follows it; before that they hang, which is what a body
        // pulling itself up actually looks like from the second before it gets a foot down.
        float mantleKneeAt = 0.42f;      // how far through the climb the leading knee comes up
        float mantleTrailingFoot = 0.18f; // how far behind it the other one is
        float mantleFootAhead = 0.10f;   // how far past the lip the foot lands, as a fraction of height
        // And how far through the climb the feet let go of the lip again. A foot planted on a fixed
        // point in the world has to be given up before the body walks past it, or the legs are left
        // out behind the player at the top.
        float mantleLegRelease = 0.70f;
        // How far the torso folds forward over the ledge at the middle of the pull.
        float mantleFoldDegrees = 34.0f;

        // In the air. Legs tuck on the way up and reach on the way down; the arms come out either
        // way. Without any of this the legs simply stretch straight down towards a floor that is
        // metres away, which is what a jump looked like.
        float airBlendSpeed = 9.0f;
        float airRiseReference = 4.5f;  // vertical speed counted as fully rising or fully falling
        float airTuck = 0.30f;          // how far the feet come up under the hips when rising
        float airReach = 0.16f;         // how far they stretch down when falling
        float airArmDegrees = 46.0f;    // how far the arms come out
        float airLeanDegrees = 9.0f;    // torso tips back rising, forward falling

        // Leaning to peek round cover.
        float leanAngleDegrees = 16.0f;
        float leanOffset = 0.32f; // metres the eye shifts sideways at full lean
        float leanSpeed = 9.0f;

        // Presentation
        float responsiveness = 14.0f; // smoothing rate for posture changes
        // Where the camera sits relative to the head bone: eyes are in front of and above the skull
        // pivot. The body is anchored so the head lands exactly here, rather than being placed by a
        // guessed offset and hoping it lines up. Guessing meant the head drifted off the camera
        // whenever the hips turned away from the view, such as when strafing.
        // Only the vertical part. A horizontal offset here is measured along the view, so turning
        // round swung the whole body through twice that distance: the body slid out from under the
        // player exactly when they turned to look at it. The skull is drawn behind this point
        // instead, in the head bone's own frame, where turning the head moves the skull and not the
        // body, which is what actually happens.
        // How far in front of the body the eye sits. Your chest is behind your face by about this
        // much, and it is what makes looking down show you the front of your torso instead of the
        // tops of your shoulders.
        float eyeForwardOfHead = 0.070f;
        // Added on top of that as the view pitches down, so looking at your boots shows you the
        // length of your body. It can afford to be larger than the standing offset because nobody
        // turns on the spot while staring at the floor.
        float eyeForwardLookingDown = 0.115f;
        float eyeAboveHead = 0.085f;
        // Almost nothing. The head bone is anchored under the eye and the neck is directly below
        // it, so whatever this is, the drawn skull sits that far behind the neck. At forty
        // millimetres the head read as floating off the back of the shoulders.
        float skullBehindEye = 0.006f;

        // Crouching folds the torso forward, and the head is pinned to the camera, so the hips have
        // to go somewhere: they swing out behind. That is what a real squat does, but the camera in
        // a game is locked over the capsule rather than over the feet, so the result was a body
        // sitting well behind the space it occupies. Sliding the whole crouched body forward by this
        // much puts the pelvis back over the feet. The head then sits slightly in front of the eye,
        // which costs nothing: it is hidden in first person, and nobody else can see the camera.
        float crouchBodyForward = 0.12f;
        bool hideHead = true; // the camera lives inside it
        bool visible = true;
    };

    void Build(Scene& scene, MeshLibrary& meshes, const PlayerConfig& playerConfig);
    // Skeleton only, with nothing to draw. Lets the posing be exercised in tests, which need no
    // renderer and where uploading meshes would mean standing up a GPU device.
    void BuildForSimulation(const PlayerConfig& playerConfig);
    void Destroy(Scene& scene);

    // Runs once per frame after the view is updated. Presentation only: it reads the simulation but
    // never writes to it.
    void Update(Scene& scene, const PlayerState& state, const PlayerView& view,
                const PlayerConfig& playerConfig, PhysicsWorld& physics, float dt);

    // Death. The body stops being animated and starts falling: the pose is handed to a set of
    // points with the bone lengths between them, and everything drawn follows those instead.
    void Collapse(const glm::vec3& impulse);
    void Revive();
    bool IsCollapsed() const { return m_ragdoll.Active(); }
    const Ragdoll& GetRagdoll() const { return m_ragdoll; }

    void DebugDraw(class DebugDraw& draw) const;

    Config& Tuning() { return m_config; }
    const Config& Tuning() const { return m_config; }
    const Skeleton& GetSkeleton() const { return m_skeleton; }
    const Pose& GetPose() const { return m_pose; }
    const HumanoidRig& Rig() const { return m_rig; }

    void SetVisible(Scene& scene, bool visible);

    // How the weapon is being held this frame. The body owns where it sits and which way it points;
    // the game owns what it is and what it is doing.
    struct WeaponPose
    {
        float aim = 0.0f;    // 0 held ready, 1 sighted
        float reload = 0.0f; // 0 at the start of a reload, 1 at the end; negative when not reloading
        float kick = 0.0f;   // 0 to 1, decaying after each shot
        float draw = 1.0f;   // 0 as a weapon is brought up, 1 once it is ready
        // And the other way: 1 while a weapon is in the hands, falling to 0 as it is put away. A
        // model's "unequip" clip runs on this; without one its "equip" clip is played backwards,
        // because a weapon leaving the hands the way it arrived beats one that vanishes.
        float holster = 1.0f;
        bool reloading = false;
        // The magazine was empty when this reload began, which plays the model's "reload_empty" clip
        // when it has one.
        bool reloadEmpty = false;

        // A clip to play by name, and how far through it is. Only the editor sets these: in the
        // game, what plays is decided by what the weapon is doing. An animation nobody can watch
        // on demand is one nobody can make.
        std::string clip;
        float clipProgress = 0.0f;
    };

    // Puts a weapon in the character's hands, built from its data entry. Passing nothing takes it
    // away again.
    void SetWeapon(Scene& scene, MeshLibrary& meshes, const WeaponDefinition* definition);
    // Puts a model in the hands directly, without going through the weapon database or the disk.
    // The editor needs this: what it is holding is whatever is open in front of it, which has
    // usually not been saved yet and belongs to no weapon at all.
    void SetWeaponFromModel(Scene& scene, MeshLibrary& meshes, const WeaponDefinition& definition,
                            const ModelAsset& model);
    // Rereads the sockets of a model already in the hands, without rebuilding a thing that is
    // drawn. Moving a grip changes where a hand goes and nothing else, and the editor moves grips
    // constantly.
    void RefreshWeaponSockets(const WeaponDefinition& definition, const ModelAsset& model);
    // Where the images a model names are loaded from. Set once, because it is one object for the
    // life of the renderer; a body without one draws its weapon in flat colours, which is what a
    // headless pose test wants and all it can have.
    void SetTextureLibrary(TextureLibrary& textures) { m_textures = &textures; }
    // The same, with nothing to draw. Lets the hold be exercised in tests, which have no renderer
    // and where uploading a mesh would mean standing up a GPU device.
    void SetWeaponForSimulation(const WeaponDefinition* definition);
    // The same, but taking the sockets from a real model instead of the generated box.
    //
    // Without this every pose test was holding a different gun from the one the player holds: a
    // generated weapon has its origin at its grip and its sockets at their defaults, and an
    // imported one has its origin in the middle of the receiver and its sockets wherever the person
    // who made it put them. Corrections that look right against the first can be wrong against the
    // second, and were.
    void SetWeaponModelForSimulation(const WeaponDefinition& definition, const ModelAsset& model);
    void SetWeaponPose(const WeaponPose& pose) { m_weaponPose = pose; }
    // Something that is not a weapon, carried in one hand. A rifle takes both hands and the whole
    // upper body; a medical kit is just held, which is a different pose and a much smaller one.
    void SetHeldItem(Scene& scene, MeshLibrary& meshes, const std::string& name, const MeshData& mesh,
                     const Material& material);
    void ClearHeldItem(Scene& scene);
    // Where in the hand a carried item sits. Live, so the editor can place it by eye and the game
    // reads the same numbers out of items.json.
    void SetHeldItemPlacement(const glm::vec3& offset, const glm::vec3& rotationDegrees)
    {
        m_heldItemOffset = offset;
        m_heldItemRotation = rotationDegrees;
    }
    // The same, with nothing to draw, so the one-handed carry can be posed in a test.
    void SetHeldItemForSimulation(bool held) { m_hasHeldItem = held; }
    // Where the carried item is drawn. Exposed for the same reason WeaponOrigin is.
    glm::vec3 HeldItemOrigin() const { return m_heldItemTransform.position; }
    bool HasWeapon() const { return m_hasWeapon; }
    // Where a round would appear to leave the model, for the muzzle flash. Not where rounds are
    // actually traced from: that comes from the eye, so what is under the crosshair is what is hit.
    glm::vec3 MuzzlePoint() const;
    // Where the weapon is held. Exposed so a test can check the hand is actually on it.
    glm::vec3 WeaponOrigin() const { return m_weaponTransform.position; }
    // The point on the weapon the carry tuning actually places: the trigger grip while it is
    // carried, the sight once it is up, and a blend of the two on the way between. The model's
    // origin is wherever it was authored around and means nothing on its own.
    glm::vec3 HoldPoint() const { return m_weaponHold; }
    // How far the muzzle is currently dropped to stay out of what is in front of it, in degrees.
    // Zero in the open. Worth reading when a weapon looks wrong in a corridor.
    float MuzzleTipDegrees() const { return glm::degrees(m_muzzleTip); }
    // Whether there is room in front of the player to put the sights up. False against a wall, where
    // a sighted weapon would have most of its barrel inside it. The game reads this to decide
    // whether the player is aiming at all, so that what is drawn and what is simulated agree.
    bool AimHasRoom() const { return !m_aimBlocked; }
    // Whether a climb still has hold of the arms. True through the fade after one finishes, which
    // is when they are still coming off the ledge and nothing should be put back in them.
    bool ArmsAreClimbing() const { return m_mantleFade > 0.001f; }
    // Which way the held weapon is turned, so a socket on it can be put into world space.
    glm::quat WeaponRotation() const { return m_weaponTransform.rotation; }
    // What is being held, for the weapon bench: its sockets are what the hands are placed by.
    const WeaponVisual& Weapon() const { return m_weaponVisual; }
    // Where the sight line leaves the weapon. Aiming has to put this on the view axis, and that is
    // the whole of what aiming means, so it is worth being able to measure.
    glm::vec3 SightPoint() const
    {
        return m_weaponTransform.position + m_weaponTransform.rotation * m_weaponVisual.sightPoint;
    }
    // How far a joint keeps off the floor once the body is a ragdoll, sized to what is drawn at it.
    float BoneRadius(BoneIndex bone) const
    {
        return bone != kInvalidBone && static_cast<size_t>(bone) < m_boneRadius.size()
                   ? m_boneRadius[static_cast<size_t>(bone)]
                   : 0.0f;
    }
    // Where the drawn skull actually sits, as opposed to where the head bone is. They differ, and
    // which one is wrong is the first question when somebody says the head looks off.
    glm::vec3 DebugSkullCentre() const;

    // Which way the body is lying or facing. Exposed for tests; nothing in the game reads it.
    float DebugBodyYaw() const { return m_bodyYaw; }

private:
    // How a piece of gear is oriented. Limb bones are re-solved by IK and end up with an arbitrary
    // roll about their own axis, so anything that has to face forwards (knee pads, pouches, the
    // holster) is aligned to the body instead of to the bone it hangs from.
    enum class PartFrame : uint8_t
    {
        BodyYaw,  // upright, facing the way the body faces
        BoneFrame // inherits the bone's full orientation, used for the head
    };

    // One drawn piece. With `to` set it is a limb or torso link that stretches between two joints;
    // without it, a fixed-size piece of equipment attached at a joint.
    struct Part
    {
        BoneIndex from = kInvalidBone;
        BoneIndex to = kInvalidBone;
        glm::vec3 size{0.1f}; // stretched parts use x and z as width and depth
        glm::vec3 offset{0.0f};
        PartFrame frame = PartFrame::BodyYaw;
        Entity entity;
        MeshHandle mesh;
        float bindLength = 0.0f;
        bool hiddenInFirstPerson = false;
    };

    struct FootState
    {
        glm::vec3 position{0.0f};  // smoothed, and what actually gets drawn
        glm::vec3 plant{0.0f};     // the world point this foot is standing on
        glm::vec3 swingFrom{0.0f}; // where the current step started
        bool inSwing = false;
        bool planted = true;
        // Whether this foot has been left standing where it was put. Standing still, a foot does
        // not move because the body above it leaned or turned its shoulders: the leg takes that up.
        bool holding = false;
        // The height of the surface this foot last landed on, and whether it has landed at all. A
        // planted foot keeps its surface until it lifts, or it snaps up and down the edge of a
        // ledge as its target crosses it.
        float groundY = 0.0f;
        bool standing = false;
    };

    void BuildSkeleton(const PlayerConfig& playerConfig);
    void BuildParts(Scene& scene, MeshLibrary& meshes);
    // Places the weapon and puts both hands on it. Returns false when there is nothing to hold, so
    // the caller can fall through to whatever the arms would otherwise be doing.
    // Places the weapon and puts the hands on it. Returns false when there is nothing to hold.
    bool UpdateWeaponHold(const PlayerState& state, const PlayerView& view, PhysicsWorld& physics,
                          float dt);
    // Where the trigger hand goes during a climb, and how far through the climb it is. Shared by
    // the arm solve and by whatever is being carried, so the two agree.
    // Moves the whole drawn weapon, parts and all, after the hold has already been solved.
    void ShiftWeapon(const glm::vec3& delta);
    // One entity per weapon part, however the visual was arrived at.
    void BuildWeaponEntities(Scene& scene, MeshLibrary& meshes, const WeaponDefinition& definition);
    // Which way a bone's front faces, carried between frames so a limb's roll can never flip.
    glm::vec3 RollFront(BoneIndex bone, const glm::vec3& axis, const glm::vec3& hinge);
    // Places a limb bone from `a` to `b`, rolled so its front faces the way its joint bends.
    glm::mat4 SegmentFrame(BoneIndex bone, const glm::vec3& a, const glm::vec3& b,
                           const glm::vec3& hinge);
    // Pulls a point the hands are reaching for back out of whatever it has gone into: anything
    // between the eye and it, and the floor underneath it.
    glm::vec3 ClearOfWorld(PhysicsWorld& physics, const glm::vec3& eye, glm::vec3 wanted,
                           float clearance) const;
    void DestroyWeapon(Scene& scene);
    // The reach-and-pull crawl. With a weapon in hand only the support arm crawls; the other keeps
    // hold of the gun.
    void UpdateCrawlArms(const PlayerState& state, const PlayerView& view, PhysicsWorld& physics,
                         float dt, bool supportArmOnly);
    void UpdatePosture(const PlayerState& state, const PlayerView& view, const PlayerConfig& playerConfig,
                       float dt);
    // Arms are solved before legs on purpose: writing a global transform rebuilds every bone after
    // it, and the legs come last in the hierarchy, so doing it the other way round would undo the
    // leg IK every frame.
    void UpdateArms(const PlayerState& state, const PlayerView& view, PhysicsWorld& physics, float dt);
    // Poses the right arm to carry a held item and puts the item in the hand.
    void UpdateHeldItem(const PlayerState& state, const PlayerView& view, PhysicsWorld& physics,
                        float dt);
    // Both hands on the lip of the ledge for the pull, then released as the body comes over.
    void UpdateMantleArms(const PlayerState& state, float weight);
    // And the legs, which draw a knee up onto the ledge and stand on it.
    void UpdateMantleLegs(const PlayerState& state, float weight);
    void UpdateLegs(const PlayerState& state, const PlayerView& view, const PlayerConfig& playerConfig,
                    PhysicsWorld& physics, float dt);
    void PushToScene(Scene& scene);

    Skeleton m_skeleton;
    Pose m_pose;
    HumanoidRig m_rig;
    Config m_config;
    std::vector<Part> m_parts;

    // One entity per model part, so a reload can take the magazine out and an authored clip can
    // move anything it likes.
    std::vector<Entity> m_weaponParts;
    std::vector<Transform> m_weaponPartTransforms;
    Entity m_weaponEntity;  // the first part, and what HasWeapon asks about
    Entity m_muzzleFlashEntity;
    Transform m_weaponTransform;
    // Where the carry tuning put the hold this frame, in world space. Kept so the point the floors
    // act on can be measured rather than guessed at from the origin.
    glm::vec3 m_weaponHold{0.0f};
    // Where the shoulders sit in the chest when nothing is being held, so the carry can move them
    // and put them back. Taken from the skeleton at build time.
    std::array<glm::vec3, 2> m_shoulderRest{glm::vec3(0.0f), glm::vec3(0.0f)};
    Transform m_muzzleFlashTransform;
 public:
    // Set when a shot is fired, so every flash is a different shape. See where it is used.
    void SetMuzzleFlashShape(float roll, float spread)
    {
        m_muzzleFlashRoll = roll;
        m_muzzleFlashSpread = spread;
    }
    // How bright the flash is this frame, 0 to 1, for the light that goes with it.
    float MuzzleFlashStrength() const { return m_muzzleFlashTransform.scale.z; }
 private:
    // Rolled and stretched differently every shot. A flash that is identical each time is the thing
    // the eye picks out as a repeated sprite rather than as fire.
    float m_muzzleFlashRoll = 0.0f;
    float m_muzzleFlashSpread = 1.0f;
    WeaponVisual m_weaponVisual;
    WeaponId m_weaponId = kInvalidWeapon;
    bool m_hasWeapon = false;
    WeaponPose m_weaponPose;
    // The weapon lags the view a little when the player turns, then catches up. Held on the weapon
    // rather than on the hands, so the grip never separates from the gun.
    glm::vec2 m_weaponSway{0.0f};
    float m_lastViewYaw = 0.0f;
    float m_lastViewPitch = 0.0f;
    glm::vec2 m_viewRate{0.0f}; // radians per second, so sway does not depend on the frame rate
    float m_swayClock = 0.0f;   // drives the breathing movement
    // What each stage of the weapon hold did this frame, in degrees and metres. Written every frame
    // and read by nothing in the game: it exists so a diagnostic can say which correction moved the
    // weapon, rather than somebody guessing from a screenshot of where it ended up.
public:
    struct HoldTrace
    {
        float holdPitchDeg = 0.0f;   // the frame the weapon is positioned in
        float carryPitchDeg = 0.0f;  // the frame it points along
        float tipDeg = 0.0f;         // muzzle correction at a wall
        float tipDropM = 0.0f;       // how far the hands came down with it
        float groundLiftM = 0.0f;    // how far the whole weapon was lifted out of the floor
        float holdAboveEyeM = 0.0f;  // where the grip ended up
    };
    const HoldTrace& LastHoldTrace() const { return m_holdTrace; }

private:
    HoldTrace m_holdTrace;
    // Which way the ground under the player faces, eased. Only prone reads it: see BodyRotation.
    glm::vec3 m_groundNormal{0.0f, 1.0f, 0.0f};
    std::array<FootState, 2> m_feet;
    std::array<FootState, 2> m_hands; // same shape: a smoothed target and whether it is planted
    // How far each joint keeps off the floor as a ragdoll, sized to what is drawn there.
    std::vector<float> m_boneRadius;
    // Which way each bone was facing last frame, so its roll about its own length is continuous.
    std::vector<glm::vec3> m_boneFront;
    TextureLibrary* m_textures = nullptr;
    // How far through the reload movement, kept separately from the reload itself. The weapon is
    // usable again the moment the simulation says so; the hands still have to finish.
    float m_reloadPlay = 0.0f;
    // How far the support hand has got back onto the weapon since it let go of it, 0 to 1. A hand on
    // a weapon is placed rather than smoothed; this is the exception, for the frame it starts.
    float m_supportRejoin = 1.0f;
    // And the same for the hand a crawl uses. A reload borrows the support arm while the body is
    // lying down, and when it gives it back the arm has to travel from the magazine well back to
    // the floor. Handed straight to the crawl's own smoothing that is a fling.
    float m_crawlHandReturn = 1.0f;
    // Where that hand was when the reload let go of it, measured from the shoulder rather than in
    // the world: the body is crawling while the arm comes back, so a world-space start would drag
    // the hand along behind a body that has moved on.
    glm::vec3 m_crawlHandRelease{0.0f};
    // Where the reloading hand is, in the carry frame rather than in the world. Both places it goes
    // are attached to the player, so easing towards them in the world charges the smoothing for
    // every degree the camera turns and the hand never catches up with what it is reaching into.
    glm::vec3 m_reloadHandLocal{0.0f};
    bool m_reloadHandValid = false;
    bool m_reloadRunning = false;
    float m_flatness = 0.0f;            // 0 upright, 1 fully prone
    // How far through the crouch, 0 standing to 1 fully down. Smoothed with the rest of the pose.
    float m_crouchness = 0.0f;
    float m_airborne = 0.0f;          // 0 on the ground, 1 fully in the air
    float m_airRise = 0.0f;           // +1 rising, -1 falling
    // Shuffling round on the spot while prone. The direction is held for the whole pivot so the
    // body does not jitter at half a turn, where which way is shorter keeps flipping.
    bool m_pronePivoting = false;
    float m_pronePivotSign = 1.0f;

    glm::quat BodyRotation() const;
public:
    // The same rotation, for a test that has to ask how the body is lying. Not otherwise useful:
    // everything in the game reads the posed bones rather than this.
    glm::quat BodyRotationForTest() const { return BodyRotation(); }
private:

    glm::vec3 m_rootPosition{0.0f};
    float m_bodyYaw = 0.0f;
    float m_lean = 0.0f;
    // The live posture, smoothed towards the target stance so transitions animate rather than snap.
    Config::StancePose m_pose_blend;
    float m_stridePhase = 0.0f;
    // Signed distance crawled along the body. Negative when backing up, which runs the reach and
    // pull the other way round.
    float m_crawlDistance = 0.0f;
    // How much of the arms the climb owns. Fades out after it ends so the release is not a cut.
    float m_mantleFade = 0.0f;
    // How much extra sway injury is adding to a held weapon.
    float m_injurySway = 0.0f;
    // 1 when nothing is in front of the eye, 0 when a wall is right against it.
    float m_wallClearance = 1.0f;
    // How far the muzzle is currently dropped to keep the barrel out of whatever is in front of
    // it, in radians. Solved from the surface the barrel is actually crossing rather than from how
    // boxed in the player is, and smoothed, because a trace that flickers on an edge would
    // otherwise flick the weapon with it.
    float m_muzzleTip = 0.0f;
    // Whether there is room in front to put the sights up, and how far down they have got. The
    // first is a decision with a margin on it; the second is what the pose actually follows.
    bool m_aimBlocked = false;
    float m_aimRoom = 1.0f;
    // Set when the body has just been put somewhere rather than having walked there, so the next
    // frame places the feet and hands outright instead of easing them in from wherever they were.
    bool m_placeLimbs = true;
    Ragdoll m_ragdoll;
    Entity m_heldItemEntity;
    // How the thing in the hand sits there, from its own definition. Nothing about a box says which
    // way up a flare goes, so each item carries its own and it is placed by eye in the editor.
    glm::vec3 m_heldItemOffset{0.0f};
    glm::vec3 m_heldItemRotation{0.0f};
    MeshHandle m_heldItemMesh;
    bool m_hasHeldItem = false;
    Transform m_heldItemTransform;
    float m_gaitWeight = 0.0f;
    bool m_built = false;
};

} // namespace pred
