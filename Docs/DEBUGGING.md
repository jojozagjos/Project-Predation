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

The creature (developer builds only):

| Command | Does |
| --- | --- |
| `spawn_creature [seed] [ahead [metres [degrees]]]` | Make one far from you, or with `ahead` in front of you and facing you (seven metres unless given), turned by `degrees` for a side view; no seed picks one at random |
| `ai.freeze 1` | Every creature stands where it is and does nothing, for looking at bodies |
| `lab` / `testmap` | Take everybody to the creature lab, with creatures coming out of its nest, or back to the test map |
| `debug.navigation 1` | The walkable surface, with the jumps across ledges drawn as orange arcs |
| `creature_clear` | Remove every creature |
| `creature_hurt [amount]` | Hurt the first creature as if you had shot it, 30 by default |
| `ai_brain` | Open or close the brain inspector |
| `ai.creatures`, `ai.seed` | How many arrive on their own in a new game -- 0 by default for now, so they are made with `spawn_creature` -- and the seed (0 for a new one each game) |
| `ai.arrival_seconds` | Roughly how long into a game it arrives, out of everybody's sight; 0 for straight away |
| `creature_pose` | Where each creature is, whether it is up, lying still or dead, how far it has fallen, and where its torso is drawn |
| `creature_mind` | The inspected creature's whole timeline and what it is weighing now, to the console and the log -- the brain window without a window |
| `hide [locker]` | Get into a locker, or out of the one you are in, through the real interaction (door, sound, the noise it hears) |
| `tracer_report` | How far the last tracer started from the eye and from the drawn barrel, and where the round was traced from |

Multiplayer, from the console or `--exec`:

| Command | Does |
| --- | --- |
| `lobby_host [name]` | Host and open the lobby, as the Start button does |
| `lobby_join <code>` | Join by code, as the join box does |
| `lobby_start` | Leave the lobby for the game, as the host's Start button does |
| `lobby_state` | Role, state, code, how the lobby server sees this PC, and whether the game has started |
| `lobby_use <web address> [stun host:port]` | Use a lobby server (and STUN server) for this run only, without saving it |
| `host_lan [name]` | Host and go straight in, skipping the lobby |
| `games` | What this PC can see: games on its network, and public ones |

Two copies on one PC, joining by code through the local lobby server (`node Tools/LobbyWorker/local-server.js`;
the first copy's log says the code after "open as"):

    ProjectPredation.exe --exec "lobby_use http://127.0.0.1:8787 127.0.0.1:3478" --exec "lobby_host kitchen" --exec "wait 4000" --exec lobby_start
    ProjectPredation.exe --exec "lobby_use http://127.0.0.1:8787 127.0.0.1:3478" --exec "lobby_join 3Z6PHF" --exec "wait 1500" --exec lobby_state

Weapon animation, with the editor open on a model (developer builds):

| Command | Does |
| --- | --- |
| `editor_reload <reload\|empty\|double> [seconds]` | Make a reload from a template, and put the playhead somewhere |
| `editor_save` | Save the model, as the Save button does |
| `reload`, `fire [ticks]` | In the game: start a reload, or hold the trigger to empty some of the magazine first |

Every sound the game plays is logged at debug level with its name (`--log-level debug`), which is how
the burst of door and drop sounds on joining a game was found.

`debug.ai`, `debug.perception` and `debug.navigation` draw its route, its senses and the walkable surface.

Reproducing input problems without hands:

| Command | Does |
| --- | --- |
| `lean [amount\|off]` | Hold a lean (-1 to 1) or let go; with no argument, report the lean, the camera roll, how far out the eye is, whether keys are reaching the game, and whether the mouse is captured |
| `sim_input key <name> <down\|up>` | Put a real key event on the queue (developer builds) |
| `sim_input mouse <x> <y>`, `sim_input button <left\|right> <down\|up>` | The same for the pointer and its buttons |
| `sim_input focus` | Tell the game its window has focus, so it captures the mouse as in play |

These go through everything a real key does -- the UI, the input system, the game -- which `lean 1`
does not. The lean-while-firing bug only showed up that way. Its replay, which should report the lean
at 1.00 throughout:

    ProjectPredation.exe --frames 1500 --exec play --exec "give_weapon carbine" --exec ai_brain
      --exec "sim_input focus" --exec "wait 20" --exec "sim_input key E down" --exec "wait 300"
      --exec lean --exec "sim_input mouse 1300 300" --exec "sim_input button left down"
      --exec "wait 200" --exec lean --exec "sim_input button left up"

Still planned from the brief: `spawn_player`, `freeze_ai`, `show_animation`, `god`, `noclip`,
`reload_assets`. They arrive with the systems they control.

## CVars

CVars are typed named values. Flags:

- **Archive**: saved to `%APPDATA%\CIRRA\ProjectPredation\settings.json` on exit and loaded on start
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
`%APPDATA%\CIRRA\ProjectPredation\Logs\predation.log`, and to the in-game console.

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
- **Animation**: clips named `reload`, `equip` and `unequip` are the whole of what those movements
  are; there is no built-in version any more. `fire` plays on top of the recoil rather than instead
  of it, so a bolt can cycle without re-authoring the kick. Anything else is yours, and playable
  from the panel that holds the weapon.
- **Hold it**: the game's own body, holding whatever is open. It stands, crouches, lies down, walks
  on the spot, aims, draws, reloads and fires. It says whether each hand is on its socket and how
  hard the arm is working to hold it, which is the only way to place a grip: the question is where
  the hand ends up, not what the model looks like.

The bench holds the weapon still while you place sockets, which is the opposite of what the game
does and the right way round for authoring. In the game the hold is fixed and the weapon hangs off
its grip socket, so the trigger hand sits at the carry point by construction and dragging the grip
moves the gun. Pinned, the gun stays put and the hand walks along it, which is the question you are
actually asking. The toggle is in the Hold it panel.

Where the hands themselves go is under "Where the hands are" in the same panel: how far across, down
and out from the eye the carried grip sits, and the same for the sights. Those belong to the player
rather than to any one weapon, so they live in `Assets/Data/player.json` under `hold`, and the panel
writes them there.

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
the process goes. The log is at `%APPDATA%/CIRRA/ProjectPredation/Logs/predation.log`, and the run
before it is kept beside it as `predation.prev.log`, which is the one to look at after a restart.
Search for `critical`.

### When the editor and the game disagree

`hold_report [frames]` prints where the weapon sits relative to the eye, in the view's own frame:
across, up and out. It reports the editor's body when the editor is open and the player's when it is
not, so the two can be compared as numbers rather than argued about from screenshots. The frame
count defers it, because commands from `--exec` all run before the first frame, when nothing has been
equipped and no body has been posed.

```
ProjectPredation.exe --frames 300 --exec "solo" --exec "give carbine" --exec "slot 1" --exec "hold_report 200"
ProjectPredation.exe --frames 300 --exec "editor m5_carbine" --exec "hold_report 200"
```
