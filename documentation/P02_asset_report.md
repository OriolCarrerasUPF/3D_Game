# P02 asset validation

All runtime models are selected copies from the supplied Kenney Space Kit. The source pack is CC0; its license is copied to `data/arkfall/License.txt`. Bounds below are from source OBJ vertices before catalog scaling. Triangle counts and material-run counts are from the framework OBJ loader.

| Asset id | Source OBJ | Bounds (min .. max) | Scale | MTL runs | Loaded triangles |
|---|---|---|---:|---:|---:|
| `terrain_tile` | `terrain.obj` | `(-0.5, 0, -0.5) .. (0.5, 0, 0.5)` | 1.0 | 1 | 2 |
| `astronaut` | `astronautA.obj` | `(-0.2415919, 0, -0.2) .. (0.2415919, 0.7931839, 0.13)` | 2.269 | 18 | 296 |
| `exit_gate` | `gate_simple.obj` | `(-0.5, 0, -0.25) .. (0.5, 1.08, 0.25)` | 1.0 | 3 | 212 |
| `oxygen_machine` | `machine_barrel.obj` | `(-0.4232051, 0, -0.257735) .. (0.2232051, 0.6, 0.257735)` | 1.0 | 6 | 338 |
| `interior_floor` | `platform_large.obj` | `(-1, 0, -1) .. (1, 0.1, 1)` | 1.0 | 3 | 76 |
| `storage_barrels` | `barrels.obj` | `(-0.2750001, 0, -0.2482051) .. (0.275, 0.35, 0.2482051)` | 1.0 | 10 | 268 |
| `iron_deposit` | `rock.obj` | `(-0.3716288, 0, -0.4715883) .. (0.4298539, 0.3, 0.3793523)` | 1.0 | 2 | 172 |
| `base_boundary` | `corridor_wall.obj` | `(-0.5, 0, -0.5) .. (-0.4, 0.8, 0.5)` | 1.0 | 3 | 36 |

All asset origins touch the ground at `y=0`; `machine_barrel.obj` has a floating-point minimum of `-9.38e-14`, treated as zero. Catalog pivot offsets are zero. The astronaut's uniform scale gives a height of approximately 1.800 m. `platform_large` remains a 2×2 m module. The gate's 1 m width is on local X and its scene instance rotates it 90° to span the west-side doorway along world Z. The astronaut's `+Z` facing is inferred from the source OBJ's asymmetric front/back bounds; the first rendered frame confirms upright placement and visibility.

None of the selected MTL files declares `map_Kd`; the renderer displays their distinct `Kd` material colors with `u_maps.x = 0`. The fragment shader samples a diffuse map only when an MTL actually supplies one.

The Debug startup loaded all eight selected OBJ/MTL pairs from source files with the binary-cache path disabled. Each loader result was `[OK]`; catalog bounds matched loaded OBJ bounds within `0.0001` units. The rendered static scene contains 4,229 instances in nine asset/material groups. The 4,096 terrain tiles form one instanced group and one mesh draw call; the first scene render issued 48 mesh draw calls overall.
