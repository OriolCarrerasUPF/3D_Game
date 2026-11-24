# P03 — Navigation through the Exit Door

**Goal (plan P03):** one colonist walks between a mine access point, the Exit Door and Storage, routes around obstacles and never crosses the base boundary except through the door. Risk-first milestone from MVP p. 2.

## Decisions

| Topic | Decision |
|---|---|
| Base boundary | Modelled on cell **edges**: every cell is `Inside` or `Outside`; a step between zones is only legal across a crossing of an open Exit Door. Wall meshes are visual only and never remove cells. |
| Exit Door | A door is a **list of crossings** (`DoorPortal{outside, inside}`). The shipped west door has 2 crossings, `(23,31)→(24,31)` and `(23,32)→(24,32)`; the gate is drawn at scale 2. |
| Pathfinding | 4-neighbour BFS on the 64×64, 1 m grid (all steps cost 1, so BFS is shortest). Fixed neighbour order (+X, −X, +Z, −Z) for deterministic routes; reusable scratch buffers with a generation stamp. |
| Route invalidation | `NavigationGrid::getRevision()` increases on every change (blocked cell, door state). Each colonist validates the rest of its route only when the revision changed, and replans from its last reached cell. A failed route is retried after 2 simulation seconds or on the next revision (plan C6.1). |
| Movement | Plan distance-budget pseudocode: snaps exactly to waypoints and never overshoots; yaw from the travel direction (`world_transform::yawFromDirection`). Gameplay stays at y = 0; the view adds the 0.1 m indoor floor height. |
| Timing | Simulation runs only in `Game::fixedUpdate` (1/30 s steps, 0.25 s clamp, max 8 steps/frame). Views are interpolated between the last two steps for smooth movement at any frame rate. |
| Scripted route | Temporary `debugRoute` in `scenario.json`: iron mine access `(9,12)` → door outside `(23,31)` → door inside `(24,31)` → Storage access `(32,33)`, looping with a 1 s dwell at each stop. Replaced by job selection in P05. |
| Role visuals | New `colonists.json` holds one role per job (asset, tint, `moveSpeed` 1.8 m/s). Tints follow GDD 3.1: Engineer orange, Miner yellow, Farmer green. P05 adds the name pool and capabilities. |

## Theory reuse (plan C9.2)

| Lecture | Reused in | Adaptation |
|---|---|---|
| T13 AI, pp. 18–21, 31 (grid pathfinding on a flat world) | `navigation_grid.cpp` | BFS instead of the framework's `extra/pathfinder` A* (plan A4: heap pop on a sorted container and state not cleared between searches). No slide signature (`AStarFindPathNoTieDiag`) exists in the framework, so none is called. |
| T13 AI (finite state machines) | `colony_simulation.cpp` | Explicit `ColonistState` with the full C6.1 list; P03 uses `Idle` ↔ `MovingToWork`. |
| T08 Scene representation (render entities, ownership) | `colony_world.cpp` | Colonists are dynamic instances of an `EntityMesh` group with stable integer handles, never pointers into the instance array. |
| T09 Render optimization (instancing) | `colony_world.cpp` | Colonists sharing asset + role tint are drawn as one instanced group. |

## Files

New: `src/game/navigation_grid.*`, `scenario_loader.*`, `colony_simulation.*`, `colonist_catalog.*`, `data/arkfall/colonists.json`, `tests/navigation_tests.cpp`, `tests/scenario_tests.cpp`, `tests/simulation_tests.cpp`.
Changed: `arkfall_types.h` (`Job`, deposit `id`), `colony_world.*` (debug overlay layers), `asset_catalog.*` (debug shader `flat.fs`), `game.*` (startup order, simulation, views, debug keys), `assets.json`, `materials.json`, `scenario.json` (`dwellSeconds`), `CMakeLists.txt`.

Startup order follows C9.1: catalog → scene → scenario + scene/scenario blocker cross-check → colonist roles → GPU assets → static views → simulation → colonist views. Any failure throws before the main loop and exits through the common shutdown path.

## Debug controls

| Key | Effect |
|---|---|
| F3 | Toggle the overlay: red = blocked cells, green/orange = open/closed door crossings, blue = current route |
| F4 | Block a free cell a few steps ahead on the colonist's route (the colonist detours) |
| F6 | Remove all debug obstacles |
| F7 | Close / open the Exit Door (closed: the boundary is sealed and the colonist waits, retrying every 2 s) |

## Acceptance

Automated (`arkfall_tests`, 52 cases, all passing on Debug, 24 Nov 2026):

- **Navigation:** start = goal, shortest route, unreachable goal returns an empty route, path crosses the boundary only through the door (exactly one zone change), closed door seals the base, 2-cell door, obstacle detour, revision counter, repeated searches identical, `findNearest` distance and tie order, cell↔world round trip.
- **Data:** shipped `scenario.json` + `scene.json` + `colonists.json` load and agree; rejected cases include wrong schema, unknown fields/enums, out-of-grid cells, duplicate ids, non-adjacent / swapped / non-contiguous / mixed-side crossings, blocked door cells, colonist on a blocked cell, unreachable route stops, stray or invisible scene blockers.
- **Movement:** per-tick displacement ≤ speed·dt at 0.5–19 m/s, always on cell-centre segments (no diagonal corner cutting), every reached waypoint is a legal step, the route crosses the door inward and reaches every stop, final position exactly on the stop centre, yaw faces travel, detour around a new obstacle, door closed mid-route never lets the colonist through and the route resumes when reopened, deterministic replay.

Manual: the game starts with the colonist at `(18,31)`; it walks to the iron deposit and then back toward the door with its route drawn (screenshots taken during the run). The F4/F7 keys were not exercised by hand in this run; their logic is covered by the simulation tests above.

## Left for later steps

Strategy camera, selection and UI (P04); jobs, reservations, inventory (P05); needs and suit oxygen at the door (P06); buildings blocking cells and placement validation (P07).
