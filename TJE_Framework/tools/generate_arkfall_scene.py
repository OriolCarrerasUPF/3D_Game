"""Generate data/arkfall/scene.json from scenario.json + assets.json.

scene.json is the static *visual* layout; scenario.json is the authoritative grid /
navigation data. Generating one from the other keeps them consistent:

* every grid cell gets a terrain tile (instanced as one draw group);
* the only blocking instances are the deposits and the static blockers declared in
  scenario.json, and each one is centred on its footprint;
* since P05B there is no predefined base: Exit Doors, corridors (floors, walls,
  arches, junction caps) and domes are runtime building views spawned from the
  simulation (initialBuildings use buildings.json visuals), never static scene entries;
* no dynamic actors (colonists) are written to the scene.

The script also re-checks the scenario rules that ScenarioLoader enforces for the
scene (cells in the grid, no overlapping blockers, colonists and debug-route stops on
free cells). Route reachability through doors depends on initialBuildings and is
checked by ColonySimulation::initialize.

Usage (from TJE_Framework/):  python tools/generate_arkfall_scene.py [--check]
--check  validates and compares with the existing scene.json without writing it.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "data" / "arkfall"

# Visual authoring choices (scene-only; gameplay data stays in scenario.json).
TERRAIN_ASSET = "terrain_tile"
DEPOSIT_VISUALS = {
    "iron": {"assetId": "iron_deposit", "materialId": "resource"},
    "copper": {"assetId": "iron_deposit", "materialId": "resource_copper"},
}
# Static blocker id -> visual (every scenario blocker needs one: no invisible blockers).
# P05 removed the P02/P03 placeholders (oxygenator_preview, storage_preview): buildings
# are now simulation records spawned from buildings.json, never static scene entries.
BLOCKER_VISUALS = {}


class ScenarioError(Exception):
    pass


def load(path: Path):
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def cell_tuple(value, where):
    if not (isinstance(value, list) and len(value) == 2 and all(isinstance(v, int) and not isinstance(v, bool) for v in value)):
        raise ScenarioError(f"{where}: expected [x, z] integer pair")
    return value[0], value[1]


class Grid:
    def __init__(self, grid):
        self.width = grid["width"]
        self.height = grid["height"]
        self.cell = float(grid["cellSize"])
        self.origin_x, self.origin_z = grid["originXZ"]
        if self.cell != 1.0:
            raise ScenarioError("grid.cellSize: the MVP contract is 1 m cells")

    def inside(self, c):
        return 0 <= c[0] < self.width and 0 <= c[1] < self.height

    def center(self, c):
        return (self.origin_x + (c[0] + 0.5) * self.cell, self.origin_z + (c[1] + 0.5) * self.cell)

    def edge_x(self, x):
        return self.origin_x + x * self.cell

    def edge_z(self, z):
        return self.origin_z + z * self.cell


def asset_horizontal_center(asset):
    s = asset["scale"][0]
    lo, hi = asset["pickingBounds"]["min"], asset["pickingBounds"]["max"]
    return ((lo[0] + hi[0]) * 0.5 * s + asset["pivotOffset"][0], (lo[2] + hi[2]) * 0.5 * s + asset["pivotOffset"][2])


def asset_extent(asset):
    s = asset["scale"][0]
    lo, hi = asset["pickingBounds"]["min"], asset["pickingBounds"]["max"]
    return ((hi[0] - lo[0]) * s, (hi[1] - lo[1]) * s, (hi[2] - lo[2]) * s)


def validate_scenario(scenario, grid: Grid):
    """Mirror of the ScenarioLoader rules that matter for the scene."""
    if scenario.get("schemaVersion") != 1:
        raise ScenarioError("schemaVersion: expected 1")
    if "base" in scenario:
        raise ScenarioError("base: removed in P05B (the base is built from Exit Doors and corridors)")

    blocked = {}
    for index, deposit in enumerate(scenario["deposits"]):
        c = cell_tuple(deposit["cell"], f"deposits[{index}].cell")
        if not grid.inside(c):
            raise ScenarioError(f"deposits[{index}]: deposit must be a grid cell")
        if deposit["resource"] not in DEPOSIT_VISUALS:
            raise ScenarioError(f"deposits[{index}].resource: unknown resource")
        if c in blocked:
            raise ScenarioError(f"deposits[{index}]: overlaps {blocked[c]}")
        blocked[c] = deposit["id"]
    for index, blocker in enumerate(scenario["staticBlockers"]):
        bmin = cell_tuple(blocker["min"], f"staticBlockers[{index}].min")
        bmax = cell_tuple(blocker["max"], f"staticBlockers[{index}].max")
        if not (grid.inside(bmin) and grid.inside(bmax) and bmin[0] <= bmax[0] and bmin[1] <= bmax[1]):
            raise ScenarioError(f"staticBlockers[{index}]: invalid rectangle")
        for x in range(bmin[0], bmax[0] + 1):
            for z in range(bmin[1], bmax[1] + 1):
                if (x, z) in blocked:
                    raise ScenarioError(f"staticBlockers[{index}]: overlaps {blocked[(x, z)]}")
                blocked[(x, z)] = blocker["id"]

    colonist_ids = set()
    for index, colonist in enumerate(scenario["initialColonists"]):
        c = cell_tuple(colonist["cell"], f"initialColonists[{index}].cell")
        if colonist["id"] in colonist_ids or colonist["id"] <= 0:
            raise ScenarioError(f"initialColonists[{index}].id: ids must be unique and > 0")
        colonist_ids.add(colonist["id"])
        if not grid.inside(c) or c in blocked:
            raise ScenarioError(f"initialColonists[{index}].cell: must be a free grid cell")

    route = scenario.get("debugRoute")
    if route:
        if route["colonistId"] not in colonist_ids:
            raise ScenarioError("debugRoute.colonistId: unknown colonist")
        for i, stop in enumerate(route["stops"]):
            c = cell_tuple(stop["cell"], f"debugRoute.stops[{i}].cell")
            if not grid.inside(c) or c in blocked:
                raise ScenarioError(f"debugRoute.stops[{i}]: stop {c} is not a free cell")
    return blocked


def build_scene(scenario, assets, grid: Grid):
    validate_scenario(scenario, grid)
    instances = []

    def add(instance_id, asset_id, position, rotation=0.0, scale=1.0, blocks=False, material=None):
        item = {
            "id": instance_id,
            "assetId": asset_id,
            "position": [round(position[0], 4), round(position[1], 4), round(position[2], 4)],
            "rotationYDegrees": rotation,
            "scale": [scale, scale, scale],
            "blocksNavigation": blocks,
        }
        if material is not None:
            item["materialId"] = material
        instances.append(item)

    # Terrain: one 1x1 m tile per grid cell (instanced as one draw group).
    for z in range(grid.height):
        for x in range(grid.width):
            cx, cz = grid.center((x, z))
            add(f"terrain_{x:02d}_{z:02d}", TERRAIN_ASSET, (cx, 0.0, cz))

    def centred_prop(instance_id, visual, rect_min, rect_max):
        asset = assets[visual["assetId"]]
        offset_x, offset_z = asset_horizontal_center(asset)
        cx = (grid.edge_x(rect_min[0]) + grid.edge_x(rect_max[0] + 1)) * 0.5 - offset_x
        cz = (grid.edge_z(rect_min[1]) + grid.edge_z(rect_max[1] + 1)) * 0.5 - offset_z
        add(instance_id, visual["assetId"], (cx, 0.0, cz), 0.0, 1.0, True, visual["materialId"])

    for deposit in scenario["deposits"]:
        c = tuple(deposit["cell"])
        centred_prop(f"{deposit['id']}_{c[0]}_{c[1]}", DEPOSIT_VISUALS[deposit["resource"]], c, c)
    for blocker in scenario["staticBlockers"]:
        if blocker["id"] not in BLOCKER_VISUALS:
            raise ScenarioError(f"staticBlockers: no visual configured for '{blocker['id']}' (invisible blockers are not allowed)")
        centred_prop(blocker["id"], BLOCKER_VISUALS[blocker["id"]], tuple(blocker["min"]), tuple(blocker["max"]))

    return {"schemaVersion": 1, "instances": instances}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="validate and compare without writing")
    args = parser.parse_args(argv)

    scenario = load(DATA / "scenario.json")
    assets = {a["id"]: a for a in load(DATA / "assets.json")["assets"]}
    try:
        scene = build_scene(scenario, assets, Grid(scenario["grid"]))
    except (ScenarioError, KeyError) as error:
        print(f"scenario.json: {error}", file=sys.stderr)
        return 1

    text = json.dumps(scene, indent=2) + "\n"
    target = DATA / "scene.json"
    if args.check:
        # Compare parsed JSON so CRLF/LF checkouts do not matter.
        current = load(target) if target.exists() else None
        if current != scene:
            print("scene.json is out of date: run tools/generate_arkfall_scene.py", file=sys.stderr)
            return 1
        print("scene.json is consistent with scenario.json")
        return 0
    target.write_text(text, encoding="utf-8", newline="\n")
    blockers = sum(1 for i in scene["instances"] if i["blocksNavigation"])
    print(f"wrote {target.relative_to(ROOT)}: {len(scene['instances'])} instances, {blockers} navigation blockers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
