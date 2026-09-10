# Debugging

Debug tooling is a first-class part of the engine. This page lists what exists and how to use it.

## Keys

| Key | Action | Binding name in `input.json` |
|---|---|---|
| F3 | Toggle the debug overlay | `debug_overlay` |
| ` (grave) | Toggle the developer console | `console` |
| F12 | Save a screenshot to the user Screenshots folder | `screenshot` |
| Right mouse (hold) | Look around with the fly camera | `look` |
| Escape | Release mouse capture | `quit_capture` |

Hotkeys work even while the console or an ImGui window has focus.

## F3 overlay

Top-left panel with:

- FPS, average and last frame time, a 240-frame history graph
- CPU main thread time, GPU frame time and bgfx CPU time from bgfx's timers
- Per-phase CPU timings: Events, FixedUpdate, Update, Render, ImGui, Present
- Fixed step rate, steps taken this frame, and whether time was dropped
- Renderer backend, resolution, vsync, draw calls, views, GPU memory
- Process working set and private memory
- Entity count and debug line count
- Debug category checkboxes

Systems added later add their own rows (physics, AI, animation, network, audio).

## Console

Type `help` for the command list, `help <command>` for usage. What could be typed appears under the
input as you type it, with each command's own usage beside it; click one to fill it in. Tab completes
command and cvar names to their common prefix. Up and
Down recall history. Every log line is mirrored into the console with level colors.

Built-in commands:

| Command | Purpose |
|---|---|
| `help [command]` | List commands or describe one |
| `cvars [prefix]` | List cvars, optionally filtered |
| `<cvar>` | Print a cvar's value, type, default, and description |
| `<cvar> <value>` | Set a cvar |
| `set`, `get`, `reset` | Explicit forms of the above |
| `log_level <category\|all> <level>` | Change log verbosity per category |
| `debug [category] [on\|off]` | List or toggle debug categories |
| `reload_config` | Reload `defaults.json` and user settings |
| `screenshot [name]` | Save a PNG |
| `paths`, `sysinfo` | Print directories and machine info |
| `cam_reset`, `cam_pos [x y z]` | Fly camera helpers (game) |
| `quit` | Exit |

Planned game commands from the brief: `spawn_creature <seed>`, `spawn_creature_random`, `spawn_player`,
`kill_creature`, `freeze_ai`, `show_ai`, `show_nav`, `show_animation`, `god`, `noclip`, `teleport`,
`reload_assets`. They arrive with the systems they control.

## CVars

CVars are typed named values. Flags:

- **Archive**: saved to `%APPDATA%\ACRD\ProjectPredation\settings.json` on exit and loaded on start
- **ReadOnly**: cannot be changed from console or config
- **Cheat**: only settable in developer mode (enforcement arrives with multiplayer)
- **Hidden**: not listed by default

Values come from, lowest to highest priority: code default, `Assets/Config/defaults.json`, user
`settings.json`, command line (`--set r.vsync=false` or `+r.vsync false`), console.

JSON files are nested objects joined with dots: `{"r": {"vsync": true}}` sets `r.vsync`.

Notable cvars in Milestone 1:

| CVar | Meaning |
|---|---|
| `app.fixed_hz` | Simulation tick rate |
| `app.max_frames` | Exit after N frames (automation) |
| `r.width`, `r.height`, `r.fullscreen`, `r.msaa`, `r.backend` | Applied at startup |
| `r.vsync` | Live |
| `r.fov` | Horizontal field of view |
| `r.bgfx_stats` | bgfx's built-in stats text |
| `r.debug` | Graphics API validation (startup) |
| `input.mouse_sensitivity` | Degrees per pixel |
| `cam.fly_speed` | Fly camera speed |
| `debug.overlay`, `debug.ai`, `debug.navigation`, ... | Overlay and category toggles |
| `scene.show_test` | Draw the reference scene |

## Debug categories

AI, NAVIGATION, PERCEPTION, ANIMATION, PHYSICS, NETWORK, RENDERING, AUDIO, PLAYER, COMBAT. Each is a
`debug.<name>` cvar. Systems check `DebugCategories::IsEnabled()` before drawing their overlays.

COMBAT is the one to reach for when a shot does not go where it looked like it should. It draws two
lines per round: the line it was traced along, from the eye, and the line it was drawn along, from
the muzzle. Those are different points by most of an arm's length, on purpose, and seeing both at
once is how you tell a mismatch from a miss.

## Logging

Categories: ENGINE, PLATFORM, RENDER, AUDIO, NETWORK, AI, ANIMATION, PHYSICS, GAMEPLAY, ASSET, DEBUG.
Use the macros `PRED_LOG_INFO(Category, "fmt {}", value)` and friends. Output goes to the terminal, to
`%APPDATA%\ACRD\ProjectPredation\Logs\predation.log`, and to the in-game console.

Set verbosity with `--log-level` on the command line or `log_level` in the console.

## Screenshots and automated checks

`--frames N --screenshot path.png` renders N frames and writes a PNG before exiting. Use at least 4 frames so
the capture happens after the scene has drawn. This is how automated smoke tests confirm rendering without a
person looking at the window.

## Profiling

`PRED_PROFILE_SCOPE("Name")` records a CPU timing that appears in the overlay. Tracy integration with GPU zones
is planned for Milestone 2.

## Planned debuggers

From the brief, to be built with their systems:

- **AI brain inspector**: current behavior, goal, scored options with per-consideration breakdown, traits,
  internal state, memories, target, last-known player positions, perception events, threat level
- **AI event timeline**: timestamped transitions with reasons
- **AI visualization**: vision cone, hearing events, detection strength, paths, target, last-known positions,
  hiding and ambush candidates, vent routes, climbable surfaces, reachable areas
- **Animation**: skeletons, joints, IK targets, foot placement, hand targets, constraints, collision bodies,
  blend weights, modifier stack editor
- **Navigation**: navmesh, links, vent navigation, climb paths, per-creature traversal, blocked paths,
  destination, path cost
- **Network**: stats, replication log, latency and packet loss simulation

## Working on a weapon model

The loop, end to end:

1. `model_import <file.glb> <name> [size] [turn x y z]` reads a download into `Assets/Models`. Size
   is the longest side in metres. The turn is what puts the barrel down +Z; both models shipped so
   far ran along +X and needed `0 -90 0`. If the muzzle comes out at the back, use `0 90 0`.
2. `editor <name>` opens it. Parts, sockets and clips are all here.
3. `bench` opens the weapon bench beside it. Point a weapon at the model, put it in your hands, and
   play the reload and the draw.
4. The bench says whether each hand is on its socket, how far behind it the wrist sits, and how much
   of the arm's reach is being used. A wrist sits about seven centimetres behind whatever the palm
   closes on, so seven is what a hand properly on a grip reads. The number to watch is the reach: an
   arm at its full reach is one with the elbow locked straight, which stops looking like a hold.
   Move the socket in the editor, save, press "Reload from disk" on the bench, and watch it come
   down.
5. `camera third` to watch it from outside.

Nothing the bench does is written to `weapons.json`. Once a model is right, set its name in that
file by hand so it survives a restart.

A model imports as one part per primitive, which is what lets a magazine be animated separately. A
download that came in as a single part has to be split before anything on it can move on its own.

## The model editor

Its own screen, reached from the menu. Nothing of the game is behind it: its own scene, no world, no
player simulating. Escape goes back to the menu.

What is in it:

- **Model**: turn, mirror, rescale and recentre everything at once, sockets included. A download
  arrives however its author left it and turning forty parts by hand is not editing.
- **Parts**: one per glTF primitive. Click one in the viewport to select it, or pick it from the
  list. Duplicate and delete are there.
- **Sockets**: the contract with the game. `grip` and `support` are where the hands close, `muzzle`
  is where rounds appear, `magazine` is where the magazine seats, `sight` is the line aiming puts on
  the view axis. Click one in the viewport and drag a handle.
- **Animation**: clips named `reload`, `equip` and `fire` replace the built-in movements of the same
  name. Anything else is yours, and playable from the panel that holds the weapon.
- **Hold it**: the game's own body, holding whatever is open. It stands, crouches, lies down, walks
  on the spot, aims, draws, reloads and fires. It says whether each hand is on its socket and how
  hard the arm is working to hold it, which is the only way to place a grip: the question is where
  the hand ends up, not what the model looks like.

Where the sockets go, in practice. The weapon is carried by its `grip`, so that socket decides where
the whole model sits relative to the player: put it on the stock and the gun ends up a hand-span too
far forward with its butt in the camera. `support` has to be somewhere the other arm can reach with
the weapon out where the camera can see it, which is closer to `grip` than a photograph of someone
shooting suggests; the support hand slides back down the barrel on its own when it cannot reach, so
a socket that is too far forward shows up as a hand that is not on it.

Keys: right mouse to look with WASD while held, left click to select, drag a handle to move, Ctrl+Z
and Ctrl+Y, F to put the view back on the model, Escape to leave.

### The first-person panel

Everything else in the editor looks at a model from outside, and outside is not where the player is.
A grip that reads perfectly in the viewport can put the receiver across half the screen from behind
the eye. The **First person** panel is the editor body's own eye, at the game's field of view, drawn
every frame. Turn it off while working on something else: it is a second render of the scene.

### Turning a model in the hand

The `grip` socket carries a turn as well as a place, and that turn is how the weapon sits in the
hand. Select the socket and use the Turn fields, or the X+90, Y+90 and Z+90 buttons, under it.

Do not turn the geometry instead. The Model panel's turn moves the vertices, the sockets, the clips
and everything else together, so a model righted that way comes out facing the other way with its
muzzle where its stock was. The grip's turn moves nothing but the hold.

### When the game aborts

A window saying `abort() has been called` is a renderer fatal, and it is written to the log before
the process goes. The log is at `%APPDATA%/ACRD/ProjectPredation/Logs/predation.log`, and the run
before it is kept beside it as `predation.prev.log`, which is the one to look at after a restart.
Search for `critical`.
