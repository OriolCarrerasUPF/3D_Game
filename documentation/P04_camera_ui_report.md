# P04 — Camera, selection and minimal command UI

**Goal (plan P04):** a strategy camera (MVP-02), selection by clicking (MVP-03) and the first HUD pieces (MVP-15 foundation), with UI-first input and every gameplay change going through a command queue.

## What was built

| Piece | Files | Notes |
|---|---|---|
| Strategy camera | `src/game/strategy_camera.*` | Orbit camera around a ground focus: yaw, pitch (default 50°, limits 25–80°), distance (8–110 m). WASD/arrows pan relative to the view and faster when zoomed out; Q/E and middle-drag rotate; the wheel zooms; optional edge scrolling (F8, off by default). The focus is clamped to the map. `frame()` fits the base, both deposits and the colonists at startup, computed from `scenario.json` (GDD 3.2). Headless; `Game` copies eye/focus into the framework `Camera` (T11 controller/camera separation). |
| Picking | `src/game/picking.*`, `ColonyWorld::pickColonist/pickGround` | Ray from `Camera::getRayDirection` (checked: world space, normalized, Y flipped). Each colonist is tested with its asset picking bounds placed at its current pose (position + yaw); the nearest hit wins and equal distances go to the lowest `ColonistId`. Ground picking intersects y = 0 and refines against the 0.1 m interior floor. Picking runs in `update()` after the camera update, so it never uses last frame's matrices. |
| Command queue | `arkfall_types.h` (`GameCommand`, `CommandResult`), `ColonySimulation::apply` | Applied once per frame, FIFO, before the ticks, also while paused. The P03 debug keys (F4/F6/F7) now go through it. Results carry `accepted`, a stable `code` and a user-facing `detail`, which the HUD shows. |
| Catalog grouping | `src/game/game_definitions.h` | `GameDefinitions` (P03 roles; P05 adds buildings) is passed to `ColonySimulation::initialize` and `ColonyWorld`. |
| View sync | `ColonyWorld::syncVisuals(simulation, alpha)` | Moved out of `Game`: creates and removes colonist views (also on role change), interpolates poses, rebuilds the navigation overlay on revision changes and draws the route, the selection ring and the build ghost. |
| HUD | `src/game/colony_ui.*` (headless), `src/game/colony_ui_render.*` (OpenGL) | Top bar with the debug status line (simulation time, speed/PAUSED, colonists, door state, FPS) and buttons Pause/Resume, −, +, Build (B), Debug (F3); colonist popup (ID, job, state, cell and zone, status, Hunger/Thirst/Oxygen bars, close button); build-mode panel; help line; short messages for command results. Colours come from `materials.json` (`ui_*`); startup fails if any is missing. |
| Build-mode shell | `ColonyUI`, `Game::updatePlacementGhost` | B toggles it; the ghost snaps to the cell under the pointer, R rotates it by 90° with a facing arrow, and the panel says whether the cell is free, blocked or a door crossing. Clicking explains that building types arrive with `buildings.json` (P05); the real placement and validation are P07. |
| Controls cleanup | `game.cpp`, `main.cpp` unchanged | Removed the framework example controls (left-drag rotation, middle-click mouse lock). Escape cancels build mode, then closes the popup, and never quits (quit with the window close button until the P10 menu). Right click cancels the same way (GDD 3.3). Space toggles pause; −/+ change the speed through pause, ×1, ×2, ×3 (GDD: −/+). |
| Clock speed | `fixed_step_clock.h` | `setTimeScale()`: real frame time is clamped to 0.25 s, then multiplied by the speed; the 8-step budget still applies. |
| Viewport | `Game::updateViewport` | `glViewport` uses the drawable size (high-DPI); UI layout, mouse and picking stay in logical window pixels. |
| Framework | `src/framework/utils.*` | Added `measureTextWidth()` (stb_easy_font metrics) to centre and clip HUD text; `stb_easy_font.h` defines a non-static global, so it cannot be included in a second source file. |

## UI-first input contract

- A left press on a button captures it; the action fires only if the release is on the same button. A press on any panel is consumed. A world press released over the UI is swallowed, so dragging across the HUD never clicks the world.
- A world click is a press and release less than 6 px apart, both outside the UI. It is resolved in `update()` after the camera moves.
- Right and middle presses over the UI are consumed; middle-drag outside the UI rotates the camera.
- The wheel zooms only when the pointer is not over the UI.

## Deviations from the plan text

- The plan's `ColonyUI::render(const ColonySimulation&)` is split into `ColonyUI` (headless state, layout and hit testing, unit-tested) and `ColonyUIRenderer` (OpenGL). The behaviour is the same.
- Mesh-level ray tests are not needed for colonists; bounds are enough. Mesh tests are kept for composite building visuals (P05/P07) if their bounds prove too coarse.
- The "debug resource display" shows the facts that exist today (time, speed, colonists, door, FPS); the inventory joins it in P05.

## Theory reuse (plan C9.2)

| Lecture | Reused in | Adaptation |
|---|---|---|
| T11 Player Controller | `strategy_camera.*`, `Game::applyCameraToFramework` | Controller computes the pose; the framework `Camera` only renders it. Indirect control: the player orders (commands), colonists decide. |
| T12 3D Collisions (ray picking) | `picking.*`, `ColonyWorld::pickColonist` | `Camera::getRayDirection` plus a slab test in the box's local frame. Coldet mesh tests are not used for colonists. |
| T16 GUI and HUDs | `colony_ui*.{h,cpp}` | Orthographic HUD after the 3D pass, UI consumes input first, `drawText` after all quads because it changes the depth/culling state. |

## Acceptance

Automated (`arkfall_tests`, **69 cases, all passing in Debug**). New P04 cases:

- **Camera:** default 50° view from +Z; panning relative to the view, scaled by distance, diagonal input normalized; Q/E rotation speed; zoom step and limits; pitch limits; NaN input; focus clamped to the map; `frame()` keeps the base and both deposits in view at 4:3 and 16:9 and is tight (10% closer loses a corner); edge-scroll axes.
- **Picking:** box hit distance, miss, box behind the ray, ray starting inside, parallel ray; ground plane at 0 and 0.1 m; a rotated box hit only along its rotated axis.
- **UI:** layout inside the window at 640×480, 800×600 and 1920×1009; a button fires only on press and release on it; press on a button and release elsewhere does nothing; a world press released on a button is swallowed; top and help bars consume clicks; the popup consumes clicks only while visible; the close button works and disappears; Escape order (build mode, then selection); message expiry.
- **Commands:** structured results for every debug command, rejections with stable codes.
- **Clock:** speed multiplier, clamp-then-scale with the step budget, invalid speeds, reset.
- **Data:** `materials.json` defines every HUD/marker colour.

Manual (Debug, 24 Nov 2026): the game starts framed on the base, both deposits and the colonist with the HUD drawn; pause, ×3 speed, camera rotation/zoom and build mode with the ghost and cell status were seen working in a live session. Colonist selection by click was not exercised in the captured run; its ray/box maths and the UI routing are covered by the tests above.

## Left for later steps

Building catalog and job assignment from the popup (P05); needs, utilities and their HUD values (P06); real placement, validation and blueprints (P07); Landing Pad L key (P09); title/pause menu, restart and the full resource bar (P10).
