# P06 — Survival and utility dependencies

**Goal (plan P06):** Hunger, Thirst and Oxygen with refill at 20 and death at 0; power, water and indoor oxygen as global utilities that gate production and services; Farmers growing vegetables; idle colonists waiting inside the base.

## What was built

| Piece | Files | Notes |
|---|---|---|
| Need tuning | `data/arkfall/colonists.json`, `src/game/colonist_catalog.*` | New required `needs` block: `refillThreshold` 20, drains (hunger 0.10, thirst 0.12, oxygen 0.10), `drinkPerSecond` 20, `waterPerThirstPoint` 0.01, `eatSeconds` 3, `vegetablesPerMeal` 1, `suitRefillPerSecond` 20, `serviceRetrySeconds` 2. Strict loader (`NeedRules`), no C++ constants. |
| Types | `src/game/arkfall_types.h` | `NeedType` (Oxygen, Thirst, Hunger: also the tie order). |
| Utilities | `src/game/colony_simulation.*` | `UtilitySnapshot`, computed once at the start of every tick: all-or-nothing power (generation vs. the demand of every complete enabled building); water from powered extractors, allocated to drinkers first (lowest Thirst, then ID), then Oxygenators, then other consumers (Greenhouses) in building-ID order, full flow only; oxygen capacity of operational Oxygenators against every living colonist on an inside cell. Per building: `powered`, `water_supplied`, `operational`. A change in operation bumps the work revision, so idle colonists re-evaluate. |
| Needs and death | same | Decay after the snapshot. Oxygen drains outside, and inside only without coverage; a Miner working at a powered mine does not drain. A need at 0 kills: slot released at once, `death_cause`, excluded from the population (`countLiving`). |
| Services | same | Need ≤ 20 → the service with the shortest time to zero after travel (tie Oxygen, Thirst, Hunger). Work slot released first. Eat/drink at Storage entrances (`eat`/`drink` in `buildings.json`); a vegetable is taken when eating starts, Hunger 100 after 3 s; drinking refills 20/s only while water is allocated, otherwise "waiting for water". Suit refill in the chamber of a powered, open Exit Door while the base has oxygen coverage. A colonist passing through such a chamber is also topped up. The service is kept until 100 or until it becomes unavailable. With no service the colonist keeps its task, shows `need_alert` ("needs water: no water (needs a powered Water Extractor)") and retries every 2 s or on a revision. |
| Production gates | same | Recipes also need power ("no power") and their water flow ("no water"); workers release their slot and say why. New statuses `NoPower`, `NoWater`. Status texts for Solar Panel, Water Extractor, Oxygenator, Exit Door (refills suits / no suit refill), Storage. |
| Outdoor jobs | same | An outdoor job is rejected when the suit cannot last the outside cells of the trip out plus the trip back to the nearest refill point above the threshold ("suit oxygen too low for Iron Mine #1"). |
| Idle rule | same | No job and no urgent need: wait at the nearest reachable entrance of a completed dome (hidden, listed as an occupant); with no dome, on the nearest inside cell that is not a door cell; with no base, where it stands. Idle colonists still take a job while walking there. |
| Hidden presence | same, `src/game/colony_ui_render.cpp` | `current_dome` now also covers `Refilling` and idle waiting at a dome entrance, and **workers at exterior buildings** (team decision after the first playtest: Miners no longer stand still outside the mine). Their popup lists the workers inside (clickable rows) and one "on the way" line instead of one line per slot. |
| HUD | `src/game/colony_ui_render.cpp` | Top bar: `Power demand/generation (SHORT)`, `Water used/produced`, `O2 indoors/capacity (LOW)`. Colonist popup shows the need alert in the task line. Storage text updated. |
| Dev scenarios | `data/arkfall/dev/{production,base_layout}.json`, new `dev/survival.json` | A Solar Panel at (40, 50) powers the P05/P05B scenarios. `survival.json`: airlock, corridor, Storage, Oxygenator and Greenhouse domes, Iron Mine, Water Extractor, Solar Panel; a hungry Miner, a Farmer with a low suit and a thirsty Engineer. |

## Walkable domes (team decision after the first playtest)

A base like door → corridor → Storage → corridor → Iron Processor did not work: the second corridor was rejected (`needs_network`) and dome footprints blocked navigation. Now:

- `NavigationGrid` has **rooms** (`setRoom`, `getRoom`, `addRoomOpening`): a step between different rooms needs a room opening. A completed dome turns its footprint into inside cells of its room (no longer blocked), with one opening per entrance candidate.
- Placement: a corridor also joins on a dome entrance candidate (`isPassageCell` = corridor/chamber cells; `isNetworkCell` now includes dome footprints). A dome entrance on another dome is `access_blocked`.
- A colonist standing in a dome footprint is hidden and listed as its occupant (`current_dome`).
- Corridor view: dome cells count as not inside, so the corridor keeps its walls and arches along a dome, open only at its entrances.
- Tests (`base_layout_tests.cpp`): the chain is walkable only through the Storage's entrances (not through its side walls or from outside), the Miner crosses the Storage hidden and works at the processor; placement rules for walkable domes. **121 cases pass.**

## Rules decided in this step

- **Suit refill point = the airlock chamber** (its two inside crossing cells) of a powered, open door while the base has oxygen coverage. A colonist outside has to cross inward to reach it, so the plan rule holds. A colonist already inside with a low suit (after losing coverage) may also go there. Without this, a low suit inside the base could never be refilled.
- **No service available → the colonist keeps working.** C6.1 interrupts work "to select a service". When none exists, leaving work would only make things worse (it would also lose mine oxygen). The need keeps draining and the reason is shown.
- **Without any refill point**, the outdoor-job check only counts the trip out (the return cannot be planned). This keeps the P07 bootstrap possible before the first door exists.
- **Services have no slots** (colonists may share cells, C5). Only work slots are reserved.
- Connected power demand counts every complete enabled consumer, even an idle one.
- A worker at an exterior building is hidden only while `Working` there; for oxygen it is still outside (mine supply).
- The temporary P03 scripted walker (`debugRoute`) has no needs.

## Acceptance

**Automated:** `arkfall_tests` has **121 cases, all passing** (Windows, Debug); `generate_arkfall_scene.py --check` passes. New `tests/survival_tests.cpp` (17 cases): C8 need rules; decay rates; death releases the slot and leaves the population (mine oxygen kept while working); no power → no mine production; brownout at exactly 11/11 vs 11/10.9 (the door stays passable); water order Oxygenator before Greenhouse; drinkers first, lowest Thirst first, waiting drinker, Oxygenator stopped while someone drinks; oxygen 25/26 (everyone indoors drains at 26); powered Miners keep mine oxygen; a thirsty Miner leaves work, drinks hidden in the Storage (listed) and returns; eating takes one vegetable and 3 s; unavailable services never refill; suit refill in the chamber, and none without an Oxygenator (outdoor job refused); several urgent needs; no Farmer → no vegetables, a Farmer works hidden in the Greenhouse; idle colonists wait hidden in a dome; deterministic replay (all 4 dev colonists alive after 2 min). Updated: dev scenarios get a Solar Panel; the idle tests now expect waiting inside the base; test `colonists.json` files carry `needs`; shipped-data test starts `dev/survival.json`.

**Manual:** `TJE_Framework.exe --scenario data/arkfall/dev/survival.json` starts and runs with no errors (35 buildings, 4 colonists). **The HUD and popups have not been checked visually yet**: watch the hungry Miner eat in the Storage, the Farmer refill its suit in the airlock, the Engineer drink, and the Power/Water/O2 values in the top bar.

## Left for later steps

- **P07:** construction (blueprints are not powered and need no utilities to make progress).
- **P09:** outcomes (defeat when everyone is dead uses `countLiving`).
- **P10:** a utilities panel listing stopped buildings (`UtilitySnapshot::stopped` already exists).
