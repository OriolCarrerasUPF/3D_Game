# P05 — Inventory and the first playable production loop

**Goal (plan P05):** the first autonomous work loop. Buildings exist as simulation records loaded from `buildings.json`, colonists reserve a work slot before walking to it, produce only while physically present, and processors turn ore into Steel in atomic transactions against one shared item capacity (MVP-06, MVP-09, part of MVP-10).

## What was built

| Piece | Files | Notes |
|---|---|---|
| Building data | `data/arkfall/buildings.json`, `src/game/building_catalog.*` | All 13 MVP types with the C8 values (footprint, cost, construction and operating role/slots, access offsets, services, power, water, oxygen and storage capacity, recipe, visuals). The strict loader requires every key (absent features are explicit `null`/`0`/`[]`), each of the 13 types exactly once, and rejects unknown ids, roles, deposits, items and fields, invalid footprints and access offsets, recipes without workers and workers without a recipe. `validateVisuals()` checks every visual asset/material against `assets.json`/`materials.json`. C++ only names the types (`BuildingType`). |
| Quarter-turn transform | `footprint::` in `building_catalog.*` | The single C5 transform (`q=1 → (h−1−dz, dx)`, …) used for footprint cells, access offsets and building views. P07's placement preview must call the same functions. |
| Types | `src/game/arkfall_types.h` | `ResourceType` (6 items), `ResourceAmounts`, `BuildingType` (13), `ConstructionState`, `CommandKind::AssignJob` / `GameCommand::assignJob`. |
| Inventory | `src/game/inventory.h` (new, header-only) | Six integer items sharing one capacity. `apply(remove, add)` commits all of a transaction or nothing: no negative items, and an addition that would exceed the capacity is refused. The space test uses the total after the transaction, so a net-zero conversion (1 ore → 1 Steel) still runs when the inventory is exactly full. A lower capacity never deletes items. |
| Capacity | `ColonySimulation::recomputeCapacity` | Sum of `storageCapacity` over completed Storages (100 each, F3). Until the first one completes, the scenario's `cargoCapacity` applies; the HUD marks it "(cargo)". |
| Building records | `src/game/colony_simulation.*` | Stable `BuildingId`s (ascending). Footprints block their cells (each change bumps the grid revision, so routes replan). A mine stores the deposit it covers. An Exit Door building completes onto its door socket and opens it; its access cells are the crossing cells (outside first). |
| Placement rules | `ColonySimulation::validatePlacement` / `validateDoorPlacement` | Pure checks with stable codes: `out_of_bounds`, `wrong_zone`, `needs_deposit` / `wrong_deposit` / `deposit_taken` / `deposit_in_the_way`, `door_cells`, `spawn_cell`, `occupied`, `blocks_access`, `colonist_in_the_way`, `access_blocked`, `wrong_placement`, `socket_taken`. Used now for `initialBuildings`; P07 adds cost and connectivity and calls the same function from build mode. |
| Scenario start state | `src/game/scenario_loader.*`, `data/arkfall/scenario.json` | New required keys `initialInventory`, `cargoCapacity`, `initialBuildings`. An initial building gives `origin` + `quarterTurns`, or `socket` for an Exit Door. The shipped scenario has F3 cargo (60 Steel, 20 Components, 20 Vegetables in a 100-unit hold) and no buildings. Placeholders `oxygenator_preview` and `storage_preview` are removed, so `scene.json` was regenerated (4,225 instances, 2 blockers: the deposits). |
| Dev scenario | `data/arkfall/dev/production.json` | Completed Iron Mine on the iron deposit, two Storages and an Iron Processor; 6 Miners, 1 Engineer, 1 Farmer; 60 items in 200 capacity. Same map as `scene.json`. Start with `--scenario data/arkfall/dev/production.json`. |
| Names | `data/arkfall/colonists.json`, `src/game/colonist_catalog.*` | A pool of 200 unique names (12 starters + 188 Ark passengers for P09), handed out in colonist-ID order. Initialisation fails if the pool is smaller than the scenario's colonists. |
| Work slots | `ColonySimulation::selectJob/reserveSlot/releaseReservation` | Reservation `(building, slot)`; one per colonist, one owner per slot. Reserved **before walking** (C6). `releaseReservation` is idempotent, frees the slot, and stops the colonist at a cell centre (finishes the step in progress if legal, otherwise walks back to the last cell). |
| Job choice | `selectJob` | Eligible = completed, enabled, compatible operating role, a free slot, next cycle can commit (inputs present, output fits) and a walkable access cell. One BFS (`findNearest`) over all eligible access cells listed in building-ID order gives the shortest route, ties by building ID. Colonists choose in colonist-ID order. Idle colonists re-evaluate when the work revision or the grid revision changes, or every 2 s; never every tick. |
| State machine | `updateWorker` | `Idle → MovingToWork → Working`, with `MovingToWork → Idle` and `Working → Idle` on invalidation. Each tick: validate the reservation, replan after a map change, choose a job when idle, move, arrive. Arrival switches to `Working` only after re-checking that the job is still available (C6.1). Colonists with the P03 `debugRoute` keep their script and never take jobs. |
| Production | `updateProduction` | C7 pseudocode. Work time accumulates from **present** workers only (`Working`, holding a slot there, standing on one of its access cells). `perPresentWorker` recipes (mines) run at one cycle's rate per present worker, so 5 Miners make 5 ore per 5 s. Each cycle commits its inputs and outputs together; when blocked it keeps at most one ready cycle. A missing input or full capacity invalidates the job, so workers leave with the reason. |
| Role order | `CommandKind::AssignJob`, `ColonySimulation::apply` | Applied through the command queue. Results: `ok`, `unknown_colonist`, `colonist_dead`, `same_job`, `unknown_role`. Releases the old slot first, then the new role looks for work on the next tick. The view recreates the colonist with the new role tint. |
| Building views | `src/game/colony_world.*` | `syncBuildings` spawns each building's `buildings.json` visuals once: centred on the rotated footprint, yaw `q × 90°` plus the visual yaw, visual offset rotated with it, indoor buildings on the 0.1 m floor. The Exit Door sits on its crossing edge and is scaled to the crossing count (same rule as the scene generator). Buildings sharing asset and material are instanced. `pickBuilding` ray-tests a box of footprint × visual height. The selected building gets an outline and its access cells drawn. |
| HUD | `src/game/colony_ui.*`, `src/game/colony_ui_render.*` | **Resource bar** under the top bar: the six items and `Items used/capacity` (red when full, "(cargo)" before the first Storage). **Colonist popup:** name, job, state, wrapped current task or idle reason, needs bars, and Engineer/Miner/Farmer role buttons (current role highlighted). **Building popup:** type and id, footprint and rotation, slots with present/reserved counts and who holds each slot, status or blocking reason, recipe, cycle progress bar and cycles made, storage capacity. Clicking picks the nearest colonist or building. |
| Start-up | `src/game/game.*`, `src/main.cpp` | `--scenario <path>` (unknown arguments are ignored with a warning, because IDEs add their own). `buildings.json` is loaded and validated before any GL resource, and its visual assets are added to the loaded set. The temporary "test building" block from the last commit is removed: buildings are now real records. |
| Build options | `CMakeLists.txt` | `building_catalog.cpp` added to the headless test sources. Optional cache variable `ARKFALL_DEBUG_ARGS` sets the Visual Studio debugger command line. |

## Rules decided in this step

- **Job tie-break:** shortest BFS route, then lowest building ID; colonists choose in ID order, so two Miners competing for the last slot always give it to the lower ID.
- **"Present":** standing on one of the building's access cells in state `Working`. A reserved worker who is still walking never counts.
- **Invalidation:** a working or walking worker releases its slot when the building cannot run its next cycle (`Storage full`, `missing <item>`), when the slot is no longer its own, or when the route fails. The released colonist re-evaluates on the next relevant change.
- **Net-zero processing at full capacity** is allowed (1 ore → 1 Steel), and mines stop. This is how "full Storage never deletes resources" is enforced.
- **Exit Door access cells** are its crossing cells, outside first, so P07 can build the first door from the exterior.

## Deviations from the plan text

- **`inventory.h` is a new file** that the P05 file list does not name. Keeping the transaction rules in one small header made them easy to test and reuse in P06–P08.
- **Generic recipes:** production is data-driven, so a Greenhouse with a Farmer already produces vegetables. P06 only has to add the power and water gate ("no Farmer means no vegetables" already holds).
- **No power gating yet:** utilities are P06, so mines and processors run without power in P05. The data (demand/output) is already in `buildings.json`.
- **Placement subset:** P05 checks bounds, zones, deposits, overlaps, door and spawn cells, access cells and colonists in the way. P07 adds cost and the "service stays reachable / nobody is trapped" connectivity checks.
- **Idle colonists stand still.** The C6 idle rule (wait inside the base) arrives with suit oxygen in P06.
- **`main.cpp` ignores unknown arguments** with a warning instead of failing, because Xcode passes its own launch arguments.

## Theory reuse (plan C9.2)

| Lecture | Reused in | Adaptation |
|---|---|---|
| T13 AI pp. 3, 9–12 (perception, behaviour, realization; state machine as an enum evaluated every update) | `ColonySimulation::updateWorker` | Same pattern with `ColonistState`: each fixed tick first checks the transition conditions (reservation valid? job still possible?), then performs the state's action (walk / work). Perception is the read-only world state: inventory, slots, the grid. |
| T13 AI pp. 18–21, 31 (grid pathfinding, keep it 2D) | `selectJob` via `NavigationGrid::findNearest` | One multi-target BFS gives the nearest eligible access cell instead of one path search per building. |
| T16 GUI and HUDs pp. 10–11, 17–18 (HUD after the 3D pass, ortho quads then text, click detection) | `colony_ui*.{h,cpp}` | Same pipeline as P04; role buttons use the existing press/release capture, and long lines are word-wrapped with the framework font metrics (`measureTextWidth`). |
| T12 3D Collisions (ray vs box) | `ColonyWorld::pickBuilding` | Axis-aligned slab test against footprint × visual height; grid occupancy stays the gameplay authority. |
| T09 Render optimization (instancing) | `ColonyWorld::syncBuildings` | Building visuals are dynamic instances grouped by asset + material (the two Storages are one draw). |

## Acceptance

**Automated:** `arkfall_tests` has **95 cases, all passing** (built headless with g++ 13 on Linux; the Windows Debug build still needs to be run by the team). There are 26 new cases.

- **Building data (`building_tests.cpp`):** shipped values match C8 and every visual exists. The transform keeps footprints one-to-one and access cells edge-adjacent for all 4 rotations; four turns return to the start; q=1 maps +X to +Z. Rejection cases: missing type, duplicate id, unknown id, unknown role, unknown deposit, deposit on an interior building, unknown item, negative cost, unknown field, missing key, zero/short/missing footprint, footprint on a door socket, access offset inside/diagonal/duplicate/none, recipe without operation, operation without recipe, empty outputs, no visuals, unknown visual asset.
- **Production (`production_tests.cpp`):**
  - footprints block cells and bump the revision; capacity is 2 × 100 Storage;
  - a Miner reserves before walking and makes nothing until present, then 1 ore / 5 s; 5 present Miners make about 10 ore in 10 s;
  - acceptance: the spare Miner waits ("missing Iron ore") and staffs the processor once ore exists;
  - **two Miners competing for the last slot** (the lower ID wins, never overstaffed);
  - **repeated job changes** (14 role changes, mid-walk and mid-work, with slot bookkeeping checked both ways every time);
  - **processing while inventory is exactly full** (3 ore → 3 Steel at 20/20, mine blocked, worker released);
  - 300 s run where capacity is never exceeded and every item is accounted for by completed cycles, ending "Storage full";
  - Exit Door building opens its socket; socket used twice is rejected;
  - 15 placement checks covering 11 rejection codes; initial-building and inventory errors carry their JSON pointer;
  - names in ID order, and a too-small pool fails;
  - `AssignJob` results; deterministic replay.
- **Scenario, shipped data and UI:** start-state parsing and rejections; shipped start (no buildings, F3 cargo, no placeholders); 200 names; `dev/production.json` agrees with `scene.json`; role buttons only in the colonist popup and inside it; building selection without role buttons; resource bar consumes clicks.

Two deliberate mutations were caught: removing the capacity check fails the capacity and conservation tests, and counting reserved instead of present workers fails the presence test.

**Headless run of `dev/production.json` (400 s):**
- All five Miners are present at the mine at 14.5 s. The mine then makes 1 ore/s.
- Miner 6 waits ("Iron Processor #4: missing Iron ore") and is working at the processor from 20.6 s.
- At 154 s the inventory reaches 200/200: the mine stops ("Storage full") and its Miners go idle with that reason. The processor keeps converting ore into Steel at full capacity.
- The Engineer and the Farmer stay idle ("no workplace for Engineers/Farmers yet").

**Manual (still to do on Windows):**
1. Run `TJE_Framework.exe --scenario data/arkfall/dev/production.json`.
2. Check that all four building models appear on their footprints (two Storages and the processor on the interior floor) and that the resource bar counts up.
3. Click the mine to see its slots fill, and click a Miner to see their task.
4. Use the role buttons and check that the slot frees immediately.
5. Run the default scenario to check that the P03 route still works.

This environment could not open a window, so the OpenGL files (`colony_world.cpp`, `colony_ui_render.cpp`, `game.cpp`, `main.cpp`) were compile-checked but not run. Watch in particular whether the building visual scales and pivots look right on their footprints; they come from the team's `assets.json` values and can be tuned in `buildings.json` (`offset`, `yawDegrees`, `scale`) without touching code.

## Left for later steps

- **P06:** power, water and oxygen snapshots gating production; needs, services (`eat`/`drink` at Storage) and the idle rule.
- **P07:** paid construction with blueprints and Engineers; build-mode placement with this step's `validatePlacement` plus cost and connectivity; the 12-colonist start; moving the P03 route to `dev/p03_navigation.json`.
- **P08:** copper, Aurite and multiple-Storage progression.
- **P09:** Landing Pad arrivals using the name pool.
