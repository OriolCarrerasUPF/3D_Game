"""Generate data/arkfall/scene.json from scenario.json + assets.json.

scene.json is the static *visual* layout; scenario.json is the authoritative grid /
navigation data. Generating one from the other keeps them consistent:

* the base boundary is drawn on cell EDGES around the interior rectangle; wall
  pieces never block navigation (the navigation grid models the boundary as
  inside/outside zones that only connect through Exit Door crossings);
* every Exit Door crossing edge is left open and gets a gate visual whose width
  matches the number of crossings (gate_simple is 1 m wide at scale 1);
* the only blocking instances are the deposits and the static blockers declared in
  scenario.json, and each one is centred on its footprint;
* no dynamic actors (colonists, runtime or initial buildings) are written to the
  scene: ColonyWorld spawns them from the simulation (initialBuildings use
  buildings.json visuals).

The script also re-checks the scenario rules that P03's ScenarioLoader enforces and
verifies with a 4-neighbour BFS that the debug route is walkable through the door.

Usage (from TJE_Framework/):  python tools/generate_arkfall_scene.py [--check]
--check  validates and compares with the existing scene.json without writing it.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "data" / "arkfall"

# Visual authoring choices (scene-only; gameplay data stays in scenario.json).
TERRAIN_ASSET = "terrain_tile"
FLOOR_ASSET = "interior_floor"          # 2x2 m module, 0.1 m high
FLOOR_MODULE_CELLS = 2
WALL_ASSET = "base_boundary"            # thin slab on the local -X face of a 1 m cell
GATE_ASSET = "exit_gate"                # 1 m wide on local X at scale 1
DEPOSIT_VISUALS = {
    "iron": {"assetId": "iron_deposit", "materialId": "resource"},
    "copper": {"assetId": "iron_deposit", "materialId": "resource_copper"},
}
# Static blocker id -> visual (every scenario blocker needs one: no invisible blockers).
# P05 removed the P02/P03 placeholders (oxygenator_preview, storage_preview): buildings
# are now simulation records spawned from buildings.json, never static scene entries.
BLOCKER_VISUALS = {}
# Rotation (degrees, world_transform convention) that moves the wall slab from the
# local -X face to the requested side of its cell.
WALL_ROTATION = {"west": 0.0, "east": 180.0, "south": 90.0, "north": -90.0}


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
    """Mirror of the P03 ScenarioLoader rules that matter for the scene."""
    if scenario.get("schemaVersion") != 1:
        raise ScenarioError("schemaVersion: expected 1")
    interior = scenario["base"]["interior"]
    imin, imax = cell_tuple(interior["min"], "base.interior.min"), cell_tuple(interior["max"], "base.interior.max")
    if not (grid.inside(imin) and grid.inside(imax) and imin[0] <= imax[0] and imin[1] <= imax[1]):
        raise ScenarioError("base.interior: rectangle must be inside the grid with min <= max")
    if imin[0] == 0 or imin[1] == 0 or imax[0] == grid.width - 1 or imax[1] == grid.height - 1:
        raise ScenarioError("base.interior: leave at least one exterior cell around the base")

    def is_interior(c):
        return imin[0] <= c[0] <= imax[0] and imin[1] <= c[1] <= imax[1]

    crossings = {}
    door_cells = set()
    door_ids = set()
    for d_index, door in enumerate(scenario["base"]["exitDoors"]):
        where = f"base.exitDoors[{d_index}]"
        if door["id"] in door_ids:
            raise ScenarioError(f"{where}.id: duplicate door id")
        door_ids.add(door["id"])
        if not door["crossings"]:
            raise ScenarioError(f"{where}.crossings: at least one crossing is required")
        sides = set()
        for c_index, crossing in enumerate(door["crossings"]):
            cw = f"{where}.crossings[{c_index}]"
            out_c = cell_tuple(crossing["outside"], cw + ".outside")
            in_c = cell_tuple(crossing["inside"], cw + ".inside")
            if not (grid.inside(out_c) and grid.inside(in_c)):
                raise ScenarioError(f"{cw}: cells must be inside the grid")
            if is_interior(out_c) or not is_interior(in_c):
                raise ScenarioError(f"{cw}: 'outside' must be exterior and 'inside' interior")
            delta = (in_c[0] - out_c[0], in_c[1] - out_c[1])
            side = {(1, 0): "west", (-1, 0): "east", (0, 1): "south", (0, -1): "north"}.get(delta)
            if side is None:
                raise ScenarioError(f"{cw}: cells must be 4-neighbours")
            key = frozenset((out_c, in_c))
            if key in crossings:
                raise ScenarioError(f"{cw}: crossing already used by another door")
            crossings[key] = door["id"]
            sides.add(side)
            door_cells.update((out_c, in_c))
        if len(sides) != 1:
            raise ScenarioError(f"{where}: all crossings of one door must be on the same side")
        along = sorted(cell_tuple(c["inside"], "")[1 if next(iter(sides)) in ("west", "east") else 0] for c in door["crossings"])
        if along != list(range(along[0], along[0] + len(along))):
            raise ScenarioError(f"{where}: crossings must be contiguous")
        door["_side"] = next(iter(sides))

    blocked = {}
    for index, deposit in enumerate(scenario["deposits"]):
        c = cell_tuple(deposit["cell"], f"deposits[{index}].cell")
        if not grid.inside(c) or is_interior(c):
            raise ScenarioError(f"deposits[{index}]: deposit must be an exterior grid cell")
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
    for c in door_cells:
        if c in blocked:
            raise ScenarioError(f"door crossing cell {c} is blocked by {blocked[c]}")

    colonist_ids = set()
    for index, colonist in enumerate(scenario["initialColonists"]):
        c = cell_tuple(colonist["cell"], f"initialColonists[{index}].cell")
        if colonist["id"] in colonist_ids or colonist["id"] <= 0:
            raise ScenarioError(f"initialColonists[{index}].id: ids must be unique and > 0")
        colonist_ids.add(colonist["id"])
        if not grid.inside(c) or c in blocked:
            raise ScenarioError(f"initialColonists[{index}].cell: must be a free grid cell")

    def neighbours(c):
        for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            n = (c[0] + dx, c[1] + dz)
            if not grid.inside(n) or n in blocked:
                continue
            if is_interior(c) != is_interior(n):
                door_id = crossings.get(frozenset((c, n)))
                door = next((d for d in scenario["base"]["exitDoors"] if d["id"] == door_id), None)
                if door is None or not door["openAtStart"]:
                    continue
            yield n

    def reachable(a, b):
        seen, queue = {a}, deque([a])
        while queue:
            c = queue.popleft()
            if c == b:
                return True
            for n in neighbours(c):
                if n not in seen:
                    seen.add(n)
                    queue.append(n)
        return False

    route = scenario.get("debugRoute")
    if route:
        colonist = next((c for c in scenario["initialColonists"] if c["id"] == route["colonistId"]), None)
        if colonist is None:
            raise ScenarioError("debugRoute.colonistId: unknown colonist")
        cells = [cell_tuple(colonist["cell"], "")] + [cell_tuple(s["cell"], f"debugRoute.stops[{i}].cell") for i, s in enumerate(route["stops"])]
        if route["loop"]:
            cells.append(cells[1])
        for a, b in zip(cells, cells[1:]):
            if not grid.inside(b) or b in blocked:
                raise ScenarioError(f"debugRoute: stop {b} is not a free cell")
            if not reachable(a, b):
                raise ScenarioError(f"debugRoute: no 4-neighbour path from {a} to {b}")
    return imin, imax, blocked


def build_scene(scenario, assets, grid: Grid):
    imin, imax, _ = validate_scenario(scenario, grid)
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

    # Interior floor: 2x2 m modules covering the interior rectangle.
    size_x, size_z = imax[0] - imin[0] + 1, imax[1] - imin[1] + 1
    if size_x % FLOOR_MODULE_CELLS or size_z % FLOOR_MODULE_CELLS:
        raise ScenarioError("base.interior: size must be a multiple of the 2x2 floor module")
    floor_height = asset_extent(assets[FLOOR_ASSET])[1]
    for j in range(size_z // FLOOR_MODULE_CELLS):
        for i in range(size_x // FLOOR_MODULE_CELLS):
            x = grid.edge_x(imin[0] + i * FLOOR_MODULE_CELLS) + FLOOR_MODULE_CELLS * grid.cell * 0.5
            z = grid.edge_z(imin[1] + j * FLOOR_MODULE_CELLS) + FLOOR_MODULE_CELLS * grid.cell * 0.5
            add(f"floor_{i}_{j}", FLOOR_ASSET, (x, 0.0, z))

    # Boundary walls on the interior perimeter edges, skipping door crossing edges.
    open_edges = set()
    for door in scenario["base"]["exitDoors"]:
        for crossing in door["crossings"]:
            open_edges.add(tuple(crossing["inside"]) + (door["_side"],))
    for z in range(imin[1], imax[1] + 1):
        for side, x in (("west", imin[0]), ("east", imax[0])):
            if (x, z, side) not in open_edges:
                cx, cz = grid.center((x, z))
                add(f"wall_{side}_{z}", WALL_ASSET, (cx, 0.0, cz), WALL_ROTATION[side])
    for x in range(imin[0], imax[0] + 1):
        for side, z in (("south", imin[1]), ("north", imax[1])):
            if (x, z, side) not in open_edges:
                cx, cz = grid.center((x, z))
                add(f"wall_{side}_{x}", WALL_ASSET, (cx, 0.0, cz), WALL_ROTATION[side])

    # One gate per Exit Door, centred on its crossing edges and as wide as the door.
    gate_width = asset_extent(assets[GATE_ASSET])[0]
    for door in scenario["base"]["exitDoors"]:
        side = door["_side"]
        insides = [tuple(c["inside"]) for c in door["crossings"]]
        if side in ("west", "east"):
            edge = grid.edge_x(insides[0][0] + (0 if side == "west" else 1))
            zs = [grid.center(c)[1] for c in insides]
            position, rotation = (edge, 0.0, sum(zs) / len(zs)), 90.0
        else:
            edge = grid.edge_z(insides[0][1] + (0 if side == "south" else 1))
            xs = [grid.center(c)[0] for c in insides]
            position, rotation = (sum(xs) / len(xs), 0.0, edge), 0.0
        scale = round(len(insides) * grid.cell / gate_width, 4)
        add(f"exit_door_{door['id']}", GATE_ASSET, position, rotation, scale)

    def centred_prop(instance_id, visual, rect_min, rect_max):
        asset = assets[visual["assetId"]]
        offset_x, offset_z = asset_horizontal_center(asset)
        cx = (grid.edge_x(rect_min[0]) + grid.edge_x(rect_max[0] + 1)) * 0.5 - offset_x
        cz = (grid.edge_z(rect_min[1]) + grid.edge_z(rect_max[1] + 1)) * 0.5 - offset_z
        indoor = imin[0] <= rect_min[0] and rect_max[0] <= imax[0] and imin[1] <= rect_min[1] and rect_max[1] <= imax[1]
        add(instance_id, visual["assetId"], (cx, floor_height if indoor else 0.0, cz), 0.0, 1.0, True, visual["materialId"])

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
