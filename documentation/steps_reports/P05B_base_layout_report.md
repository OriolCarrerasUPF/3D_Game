# P05B — Base built from domes and corridors

**Goal (plan P05B, base layout change of 27 Nov 2026):** replace the fixed base rectangle with a base the crew builds: an Exit Door (airlock) and 2-cell corridors make the inside, interior buildings are closed domes entered from the corridor, and a colonist working inside a dome is hidden and listed in the dome popup.

## What was built

| Piece | Files | Notes |
|---|---|---|
| Types | `src/game/arkfall_types.h` | `BuildingType::Corridor` (14 types). |
| Per-cell zones | `src/game/navigation_grid.*` | `setZone(Cell, Zone)`, bumps the revision only on change. `addDoor` is now used at run time. |
| Building data | `data/arkfall/buildings.json`, `src/game/building_catalog.*` | Zones `interior` (dome) / `exterior` / `network`; placements `footprint` / `airlock` / `corridor`; new required keys `insideOffsets` and `crossings`. Exit Door: 2×3 walkable airlock, chamber rows inside, two crossings, `airlock` visual. Corridor: 1×1 walkable, `[[0, 0]]` inside, 1 Steel, 3 s, `corridor_floor` visual. Strict validation of all new rules. |
| Scenario | `src/game/scenario_loader.*`, `data/arkfall/scenario.json` | `base` block removed (unknown field now). `initialBuildings` accepts corridor strips `{type: corridor, from, to}`: straight, exactly 2 cells wide, expanded by `corridorStripCells` row by row from `from`. `socket` removed. Debug-route reachability moved to the simulation, because it depends on the doors built from `initialBuildings`. The shipped scenario has no base, no door and no debug route. |
| Simulation | `src/game/colony_simulation.*` | Completion effects: footprints block only for domes/exterior buildings; `insideOffsets` cells turn inside; an airlock registers its crossings as one open door. Placement rules: overlap with any footprint (`occupied`), corridors must join the network (`needs_network`), domes need an entrance candidate on it (`needs_network`), exterior buildings and airlock access cells stay off it (`on_network`); a corridor may cover a dome's entrance candidates and another corridor's neighbours; colonists may start in a corridor. Domes are worked from entrances only (`usableAccessCells`). `current_dome` per colonist and `occupants(BuildingId)`. Status texts: "corridor (inside)", "no entrance (connect it to a corridor)". |
| View rules | `src/game/base_layout.*` (new) | C++ port of `tools/base_layout_rules.py`: walls per edge to a non-inside cell (dome entrances stay open), one arch per straight 2-cell row, one junction cap per uncovered 2×2 block, and the passage-axis offsets (arches, junctions, airlock). |
| Views | `src/game/colony_world.*` | Every building is placed on its footprint centre (the airlock too). Corridor walls/arches/junctions rebuilt when the grid revision changes. Corridor and airlock footprints register a 0.1 m walkable floor. Colonists inside a dome have no view (not drawn, not pickable); colonists in corridors and the airlock are drawn on the passage axis, blended between the cell they leave and the next one. Building and piece visuals stand on y = 0 (`setDynamicViewPose(..., on_visual_surface = false)`). |
| UI | `src/game/colony_ui.*`, `src/game/colony_ui_render.cpp`, `src/game/game.*` | Dome popup lists "Inside (N)" with up to 4 clickable rows (`UiAction::SelectOccupant`); the colonist popup says "Inside Iron Processor #28". Initial camera frames buildings, deposits and colonists. Corridor piece assets are loaded at start. |
| Scenes and tools | `tools/generate_arkfall_scene.py`, `data/arkfall/scene.json` | The scene is terrain + deposits only (4,098 instances). The static preview of 27 Nov (`generate_base_modules_preview.py`, `dev/base_modules_preview*.json`) is removed: the real base replaces it. |
| Dev scenarios | `data/arkfall/dev/{production,base_layout,p03_navigation}.json` | `production.json`: mine outside; airlock, 10-metre corridor, two Storages and an Iron Processor along it. `base_layout.json`: airlock, L-shaped corridor, Storage and Iron Processor domes, 6 Miners and an Engineer. `p03_navigation.json`: the P03 scripted route, now through the airlock and along the corridor. |

## Rules decided in this step

- **Corridor strips** in `initialBuildings` are placed cell by cell in strip order, so each cell joins the door or an earlier cell of the strip.
- **Completed corridors need no access cell**: other buildings may stand next to them; only a blueprint corridor (P07) needs a free neighbour.
- **Colonists may start in a corridor** (walkable footprint); blocked footprints still refuse spawn cells.
- **Dome entrances** are recomputed on demand from the zone map, so a dome gains an entrance as soon as a corridor completes next to it.
- **Hidden presence** in P05B covers `Working`; P06 adds `Refilling` and idle waiting (plan C6).

## Deviations from the plan text

- `colony_ui.*` popup height is 320 px (was 290) to fit the occupant list.
- Dome shells are **not** made yet: domes still use their P05 machine models. The dome shell models (plan A5/C5.2, about 2.5–3 m high) remain for P07 with the other building visuals.
- `--scene` (27 Nov) is kept: useful for future visual tests, not used by any shipped scenario.

## Acceptance

**Automated:** `arkfall_tests` has **102 cases, all passing** (Windows, Debug). New: `base_layout_tests.cpp` (6 cases): L/T/cross pieces equal to the Python reference (22 walls, 8 arches, 1 cap for the L; 32 walls, 12 arches, 1 cap for the T), open edges and passage offsets (corridor, junction, airlock at 0° and 270°), inside cells/door/entrances from `dev/base_layout.json` (exactly two outside–inside steps exist), a Miner crossing the door only through door cells and working hidden inside the processor dome, release on role change, deterministic replay. Updated: building catalog rules for network buildings (10 new rejection cases), scenario strips (6 rejection cases), simulation tests on an airlock + corridor base (including a route rejected because blockers cut it off), production tests on the new `dev/production.json` (airlock crossings, 20 placement codes), shipped data (no fixed base in the scene, base modules fit the astronaut, all three dev scenarios start).

**Manual (Windows, done):** `TJE_Framework.exe --scenario data/arkfall/dev/base_layout.json` — airlock, L corridor with arches and the junction cap, Storage and Iron Processor along it; after ~25 s the spare Miner walks through the airlock, disappears into the processor, the processor runs, its popup shows "Inside (1): Farid Reyes (Miner) - Working", and clicking the row opens the colonist popup ("Inside Iron Processor #28").

## Left for later steps

- **P06:** needs and services; hidden presence while refilling and idle-waiting in a dome; indoor oxygen counts colonists on inside cells.
- **P07:** paid construction with blueprints, corridor strip dragging, the `enclosed` check, dome shell models, copper deposit model.
