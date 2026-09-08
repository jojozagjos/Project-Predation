# Animation

**Status**: the foundation is built. `Engine/Animation` has the skeleton, pose and IK solvers, and
`Game/Player/PlayerBody` drives a fully procedural first-person body with them. Authored clips,
stride warping, weapon poses and creature gaits are still design, marked **(planned)** below.

## What exists

- **Skeleton**: a flat bone array with parent indices, kept in parent-before-child order so global
  transforms resolve in a single forward pass with no recursion.
- **Pose**: local transforms plus evaluated globals. `SetGlobal` writes a world transform and
  rebuilds that bone's descendants, which is what IK needs since it produces global positions.
- **Two-bone IK**: analytic, not iterative, so it is exact and free of jitter. A pole vector decides
  which way the joint bends, and the reach is clamped with a span-relative margin. Degenerate input
  (zero-length bones, a pole parallel to the chain, a target on the root) is handled rather than
  producing NaNs or aborting.
- **Player body**: a 19-bone humanoid built from anthropometric ratios, drawn as 31 parts. Parts are
  either limb segments stretched between two joints or fixed equipment attached at a joint. Gear
  uses the body's facing rather than the bone's, because IK-solved limb bones carry an arbitrary
  roll about their own axis.
- **Procedural locomotion**: stride phase driven by distance travelled rather than time, so the legs
  stay in step as speed changes. Feet swing along the direction of travel, lift on the forward half
  of the cycle, and trace downward for real ground so they land on stairs and slopes. Hip sway and
  bob, speed-scaled forward lean, and opposed arm swing.

The camera stays authoritative: the body is positioned to agree with the view, never the reverse.
Driving a first-person camera from an animated head is what produces motion sickness.

**Known limitation.** With no art assets the body is box geometry, so looking straight down reads as
blocks rather than a person. The framing is deliberately conservative: the torso is flatter than it
is wide and the body sits slightly behind the eyes, because a square torso on the camera's own axis
hides the legs entirely. This wants a real mesh, not more tuning.

## Principle

Procedural where it produces better results, authored where it produces better results. Quality over
ideological purity. Human locomotion is authored clips with procedural layers on top; creature locomotion is
fully procedural because generated anatomy has no clips.

## Shared foundation

- Skeleton: hierarchy, bind pose, named sockets.
- Pose: local transforms per bone; utilities for blending, additive, masking.
- Clip sampling from glTF animations.
- Blend graph nodes: blend by parameter, additive, layered by bone mask.
- IK solvers: two-bone, FABRIK for chains, look-at with distribution across a chain.
- **Procedural modifier stack**: an ordered list of nodes applied after the blend graph. Each node exposes
  tunable properties (through the property system) to ImGui panels and JSON files. Nodes can be enabled,
  weighted, and reordered without code changes.

## Player

- Locomotion: authored walk, run, crouch, prone, and transition clips. Stride warping matches the clip to
  actual speed; foot locking removes sliding; foot IK plants feet on stairs and slopes; hip adjustment keeps
  the body grounded.
- Upper body: look-at distributed through the spine, aim IK, leaning, breathing, weapon sway and recoil
  springs.
- Weapon: the weapon is the parent of the hands. Hands IK to grip sockets. Weapon position is a data-driven
  base pose plus sway, recoil, and ADS blend. Reload uses authored hand animation layered onto the procedural
  base.
- Camera: attached to the head socket with filtered translation and unfiltered rotation from input. Body yaw
  follows camera yaw with turn-in-place thresholds. This keeps the camera attached to the body without turning
  head bob into nausea.
- Mantling, crouch, prone, and vaulting combine authored key poses with IK to actual geometry.

## Creature

- Gait generator: assigns limb phases from a gait pattern chosen by limb count (biped, quadruped, hexapod,
  custom), predicts step targets from velocity and terrain, drives step curves, oscillates the body from limb
  support state, keeps the center of mass over the support polygon.
- Spine chains follow a path for elongated bodies; head tracking sits on top.
- Climbing and ceilings run the same gait with gravity redefined in the body frame.
- Squeezing compresses spine and limbs along a path through a gap.
- Secondary motion via springs for tails and appendages.
- Posture library: stalking, crouched, perched, hanging, reaching, inspecting, all as parameter sets rather
  than clips.

## Interaction rig

Grabs, drags, carries, and kills use an interaction rig that pins creature contact targets to player body
sockets and drives both rigs with IK, parameterized by data (approach direction, contact points, hold pose).
No bespoke cinematic executions initially; variation comes from anatomy and parameters.

## Ragdoll

Jolt ragdolls for deaths, blended from the current animated pose. Partial ragdoll for hit reactions later.

## Tuning tools

In-engine panels expose hand placement, weapon position, reload positioning, IK targets, foot offsets, stride
lengths, blend weights, procedural response strength, camera offsets, sway, recoil, and movement curves. Edits
write JSON, not source.

## Debugging

Skeleton and joint drawing, IK targets, foot placement points, hand targets, constraint visualization,
collision bodies, blend weights, and a modifier stack view with per-node enable and weight.
