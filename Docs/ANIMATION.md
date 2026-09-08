# Animation

**Status**: design. Player animation starts in Milestone 4; creature procedural animation in Milestone 11.

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
