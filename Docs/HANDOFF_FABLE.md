# Handoff prompt for Claude Fable 5.2 — PREDATION

Copy everything below the line into a new Claude Code session opened at `C:\Dev\Project-Predation`.

---

You are picking up an in-progress session on **PREDATION**, a C++ game (bgfx renderer, CMake, Windows). The repo is at `C:\Dev\Project-Predation`, on GitHub as `jojozagjos/Project-Predation`, branch `main`. Another Claude session was in the middle of a list of fixes. Your job is to finish the list, to the same standard and following the same rules, and then to write a report so that session can continue afterwards.

## 1. Rules you must follow (from the user, standing)

- **The user is new to C++.** Explain things in plain language when you report.
- **Commit and push after each milestone**, and only when:
  - the build has **0 warnings**, and
  - the full test suite passes.
- Commit messages end with: `Co-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>`
- **Stage specific paths only:** use `git add path1 path2 ...`. **Never `git add -A` or `git add .`**
- No Render hosting.
- **Never write lore or story.** Ask the user about lore and story decisions. Functional text (UI labels, instructions) is fine.
  - Credit "jojozagjos".
  - Voice lines must be real recordings, never TTS.
- **Don't kill the user's running game. Don't change the user's persistent settings** (`%APPDATA%\CIRRA\ProjectPredation\settings.json`).
- **Remove dead code** you make obsolete.
- **Log every fix in `Docs/FIX_LIST.md`.**
  - Same table format as the existing rows: `| N | user's request (their words) | what was done, plain language | Done |`.
  - The last entry is **376**, so the next is **377**.
- **Test campaigns:**
  - Delete test campaigns you create (in `%APPDATA%\CIRRA\ProjectPredation\Campaigns`).
  - **Never delete `campaign` or `dwadaw`.**
- **Test screenshots** (`dev_*.png` in `%APPDATA%\CIRRA\ProjectPredation\Screenshots`) go to the Recycle Bin, not hard-deleted. Use PowerShell:
  `Add-Type -AssemblyName Microsoft.VisualBasic; [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteFile($path,'OnlyErrorDialogs','SendToRecycleBin')`
- **Downloading any file needs explicit permission** (state filename, source and size).
- **Use `rm` only with literal absolute paths.**
- **No perl or python.** node is fine.
- **Multiplayer:** sync weather, lightning and shared world events by default.
- **Outposts:** hold back on outpost work until upgrades and the tutorial.
- **Wildlife:** none until the creature AI phase.
- **Performance:** keep the game above 60 fps.
- Read `C:\Users\jgsla\.claude\projects\C--Dev\memory\MEMORY.md` and the memory files it points to before starting. They hold the user's decisions.
- Read `Docs/MASTER_PLAN.md` for the phases.

## 2. Build, test and run

- **Build** (Git Bash):
  ```
  cd /c/Dev/Project-Predation && cmd //c "call Scripts\\Windows\\vsenv.cmd && cmake --build --preset windows-relwithdebinfo" > $TEMP/build.log 2>&1; grep -i "warning\|error C\|FAILED" $TEMP/build.log | grep -v '^"C:'
  ```
  - Add `--target ProjectPredation` for a faster game-only build.
  - Run `cmake --preset windows-relwithdebinfo` first if you add new files.
- **Tests:**
  ```
  ./build/windows-relwithdebinfo/bin/PredationTests.exe > $TEMP/tests_all.log 2>&1; tail -2 $TEMP/tests_all.log
  ```
  645 test cases currently pass.
- **Run headless with console commands:**
  ```
  build/windows-relwithdebinfo/bin/ProjectPredation.exe --frames N --exec "cmd1" --exec "cmd2" ... > $TEMP/run.log 2>&1
  ```
  - `wait <frames>` delays the commands after it.
  - The frame rate varies from 100 to 260 fps, so frame waits are imprecise. Make `--frames` larger than the sum of the waits.
  - Use `cine_seek <s>` plus `cine_pause` to freeze a cinematic at an exact moment.
  - Other useful commands:
    - `screenshot dev_name`, `hud 0`
    - `site_weather rain 0.9`, `day_hour 0`
    - `goto x y z yaw pitch`, `look yaw pitch`, `seat 0`
    - `fly`, `cam_pos x y z yaw pitch`
    - `map_open ...`, `cine_play/cine_skip/cine_stop`
    - `course <body> [area]`, `travel_now`
- **Test flows:**
  - New campaign: `campaign_new NAME 7` → `solo` → `wait 120` → `cine_skip` → `wait 30`. The campaign starts landed at Kestrel, so plot a course first.
  - **Stormy sea site, shuttle landing:** `course 2 1` → `wait 7000` → `travel_now` → `wait 300` → `cine_skip` → `wait 1200` → `goto -1500 0.7 8.5 0 -10` → `seat 0` → `hud 0`.
    - The launch starts about 200 frames later.
    - `go_to_site` comes about 27 s later.
    - `surface_insertion` (42 s) then plays. Its first ~3.4 s are entry fire.
  - **Clear site, landing:** the same flow with `course 3 3`.
  - **Orbit view from the cockpit:** after arrival, `goto -1500 0 -15.2 0 -12` → `look 0 10`.
  - **Outpost landing from orbit:** `course 2` → `wait 7000` → `travel_now` → `wait 600` → `cine_skip` → `wait 600` → `goto -1500 0 -15.2 0 -12` → `course 3 4` → `travel_now`.
- **Viewing screenshots:** Read the PNG with the Read tool. To crop at full resolution, write a small PowerShell `System.Drawing` script in your scratchpad; downscaled grids create false colour banding.
- **Editing files:** Edit or Write tools are safest. Long bash heredocs with apostrophes broke once. Write big code blocks to a file first, then splice them in.

## 3. Key technical facts

- **Scene and passes:** the scene renders to RGBA16F, then a post pass (`Engine/Shaders/post/fs_post_final.sc`) adds haze, the 3D cloud deck and fog.
- **The post pass works from the depth buffer.** Anything that writes no depth (glass, `Material::glass > 0`, drawn in `SceneRenderer::DrawGlass`) gets the haze and cloud of the sky *behind* it painted over it.
  - This round's fix: a new `Material::glassDepth` flag makes such glass write depth. The entry-fire "plasma cap" uses it.
  - The windscreen rain film is a cut-out (`cutout = true`, black albedo, `emissiveTextured`, glow set from the sky's colour), not glass, for the same reason.
- **Cinematics:** JSON files in `Assets/Cinematics/`, run by `m_cine`. They have cameras, shots, actor tracks, markers, texts, sounds, fade, heat, shake and fog/ambient scale. The list:
  - `ship_arrive` (14 s), `ship_depart` (11 s), `ship_docking` (23.5 s), `ship_launch` (27 s)
  - `ship_return_empty` (17.5 s), `surface_insertion` (42 s), `surface_extraction` (30 s)
  - All have `pauses_gameplay: true`.
- **Camera takeover:**
  - `PredationGame::CinematicCamera` (`Game/PredationGameCinematics.cpp` ~line 464) takes the camera whenever a cinematic is active.
  - `SeatedCamera` overrides it only when the player is seated in **first person**.
  - `cameraAway` is computed in `PredationGame.cpp` ~line 3198.
- **Holds:** `m_cineHolds` (set from `pausesGameplay`, ~line 440) freezes players (`CinematicHoldsPlayers`) and the world (`CinematicHoldsWorld`). That blocks creatures, and campaign travel in `PredationGameMap.cpp` ~551/572.
- **Fade:** drawn as a black overlay in `DrawCinematicOverlay` (~line 907), with `m_cineFadeIn` on stop.
- **Ship descent onto outposts:** `StartDescent`/`UpdateDescent`, `DescentDrop()`, `DescentDive()` and `DescentHeat()` (heat scaled by the body's air). The land stays hidden above 2500 m and emerges from haze.
- **Windscreen rain:** `PredationGame::UpdateWindscreenRain` in `Game/PredationGameSeats.cpp`. The drop texture is 1024×512; in it, **rows increasing = up the glass**.
- **Landing lights:** `GatherShuttleLights` (same file).
- **Body visibility:** `PlayerBody::SetVisible` (`Game/Player/PlayerBody.cpp`).

## 4. Done this round, built but NOT yet committed

Uncommitted files:
- `Engine/Render/Material.h`, `Engine/Render/SceneRenderer.cpp`
- `Engine/Shaders/post/fs_post_final.sc`
- `Game/Player/PlayerBody.cpp`, `Game/Player/PlayerBody.h`
- `Game/PredationGame.h`, `Game/PredationGameCinematics.cpp`, `Game/PredationGameHud.cpp`, `Game/PredationGameSeats.cpp`

What they contain:

1. **Body disappears and never comes back** (fixed).
   - Cause: `PlayerBody::SetVisible` also overwrote the tuning flag `m_config.visible`. Once a cinematic hid the body, every restore (`Tuning().visible && ...`) stayed false.
   - Fix: a new `m_shown` member. `SetVisible` sets only that. The body is drawn when `m_config.visible && m_shown`. `RefreshVisible()` is called from the debug "Show body" checkbox. Callers were updated.
   - The user was in third-person or free camera while seated when it happened.
   - Verify it in third person too.
2. **Windscreen drops reworked:**
   - Beads stay still and merge when one lands on another.
   - Beads bigger than about 8 mm run *down* in fits and starts, pick up beads in their path, and leave a wet trail that dries and breaks up.
   - Airflow (from the shuttle's measured speed) pushes big drops up the glass in streaks, and small ones cling on.
   - Drops are drawn on whole texels so still ones don't shimmer.
   - Drops slowly dry when the rain stops.
   - The old version moved drops the wrong way (the texture rows were flipped) and wiggled them with a sine wave.
   - Checked roughly in screenshots; a closer look at a parked run would be good.
3. **Horizon line under the cloud deck** removed.
   - In `fs_post_final.sc`, sky seen level with or below the horizon from under the deck now also gets the far-cloud colour.
   - Before, a one-pixel bright line showed at the horizon.
4. **Entry fire had clouds and the horizon drawn over it** (fixed with `glassDepth`). Verified in paused screenshots.

**Before committing:** build, run all tests, and add FIX entries 377+ for these four items.

## 5. Remaining list (user's words, paraphrased; do all of these)

1. **Cutscenes still exist and still play** (the user asked why).
   - The user wants players always in their own view: in the ship or seated in the shuttle, never an external cutscene camera. That includes third-person or free-camera players.
   - In normal play a cinematic should only move actors (shuttle and ship), play its sounds, show its heat/shake/texts and fire its markers. It must **not**:
     - take the camera,
     - fade the screen to black,
     - hold or freeze players.
   - Keep the camera takeover only for dev playback (console `cine_play` / cinematic editor). For example, add a flag set only on those paths.
   - Check that nothing relies on `pausesGameplay` to stop campaign travel or creatures mid-cinematic. `CinematicHoldsWorld` gates `Travel::Step`: the hold freezing ship travel during `ship_arrive` may matter.
   - `ship_arrive` in a campaign plays when the ship reaches a body (`PredationGameMap.cpp` ~583). Its `arrive` marker at 6.9 s calls `ArriveAtBody`. Consider calling `ArriveAtBody` at once, with the intercom "orbit" line and the planet title card, since the player watches from inside now.
   - Also check `ship_depart`, `ship_return_empty` (the user is put in the briefing room) and the remote-player flows (`PredationGameNet.cpp` ~752).
2. **"For the transition you are in the ship and shuttle now, so the sound going away doesn't matter anymore."**
   - Find the audio fade/duck tied to cinematic transitions or scene changes (search the audio code for a fade or mute during cinematics, `GoToMap` or loading) and remove it, so sound carries on through.
3. **The ship lining up over the planet: the planet moves really quickly all of a sudden and takes a weird path. Make it realistic.**
   - Look at how the ship's position and orientation relative to the body is animated while it comes round in orbit to sit over the chosen landing area ("Going round the orbit to be over ...").
   - Also look at `ArriveAtBody` and anything that snaps the view.
   - Make it a smooth, physically sensible orbital motion: constant angular rate, no snaps, the planet turning steadily under the ship.
4. **Airless planet descent has no transition.** Going down to a planet with no atmosphere, the ship flies into it and everything disappears, then appears.
   - With air, the entry fire and haze hide the switch from orbit globe to local land. Without air there is nothing.
   - Make it seamless: for example, a shader trick that renders the globe's surface (planet_surface) close up and crossfades it into the local terrain at matching colour and lighting, or blends by altitude.
   - Look at `UpdateDescent`, the hidden-land region (high > 2500) and `DescentDive`.
5. **Tunnels have gaps** (the user's screenshot: a concrete tunnel interior with dark triangular holes at the joins and corners).
   - Find the facility tunnel / corridor mesh builder (search `Game/World` for "tunnel") and close the gaps.
6. **The wrapping on the outside of the tunnels looks weird.** The user's screenshot shows a white tube outside with wobbly black straps over it, standing on posts on the sea floor or ground. Make it look like a proper engineered tunnel exterior.
7. **Multiplayer menu rework:**
   - Make a separate **Solo** button in the main menu.
   - **Remove the button in the pause menu** that opens the game to a multiplayer lobby (console `mp_open` exists).
   - Check `PredationGameMenus.cpp`.
8. **Remove the orange dashed orbit line** drawn round a planet for the ship on the navigation map (the ship doesn't orbit any more). Search `PredationGameMap.cpp` / the map view drawing.
9. **Finish Phase 4 if it isn't done.** Read `Docs/MASTER_PLAN.md` Phase 4 (contracts at a ship terminal, etc.; see memory `predation-contracts.md`). Complete anything left that doesn't need lore. Ask the user about anything lore-related instead of writing it.
10. **Night sky:** stars showed through a heavy-rain overcast at night in a screenshot. Under full cloud cover the stars should be hidden. Check the sky/star rendering against `deckCover`.

## 6. How to work

- Keep a running checklist in your scratchpad.
- **Verify each visual fix in the game with screenshots** before calling it done.
- **Commit and push in milestones**, each with FIX entries. Only with 0 warnings and all tests passing.
- Clean up test campaigns and screenshots at the end.

## 7. When you finish (or run out), write the report

Create `Docs/HANDOFF_REPORT.md` with:
- every commit hash and what it contains;
- every FIX number you added;
- what is done and verified (and how);
- what is partly done, with file:line pointers and what's left;
- anything you tried that didn't work and why;
- questions you need the user to answer;
- any uncommitted changes.

The original session will read this file to continue, so be precise and complete.
