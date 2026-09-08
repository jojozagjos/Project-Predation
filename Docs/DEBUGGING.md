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

Type `help` for the command list, `help <command>` for usage. Tab completes command and cvar names. Up and
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

AI, NAVIGATION, PERCEPTION, ANIMATION, PHYSICS, NETWORK, RENDERING, AUDIO, PLAYER. Each is a `debug.<name>`
cvar. Systems check `DebugCategories::IsEnabled()` before drawing their overlays.

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
