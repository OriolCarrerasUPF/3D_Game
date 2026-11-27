"""View rules for corridor visuals, shared by the Blender preview and the dev scene.

Cells are (x, z) grid indices. `corridor` is the set of corridor cells and `inside`
every inside cell (corridors plus Exit Door chamber cells). P05B implements the same
two rules in the C++ view.

* Walls: one `corridor_wall_frame` per edge between a corridor cell and a cell that is
  not inside. The asset is authored on the local -X face of its cell.
* Arches: one full 2 m `corridor_arch` per row of a straight 2-cell corridor, that is
  where the cross-section through two neighbouring corridor cells is exactly
  "not inside | corridor | corridor | not inside". Junctions, corners, wider areas and
  single cells get no arch, so no rib is ever left hanging or crossing another one.
* Junctions: one `corridor_junction` cap per 2 x 2 block of corridor cells that no
  arch covers (an L corner, a T or a cross of 2-cell corridors). The cap stands on
  its own four corner posts, so it fits whichever sides of the block are open.
  Blocks are chosen in cell order without overlapping.
"""

# Neighbour offset -> yaw (degrees, world_transform convention: +90 maps +X to +Z)
# that moves the wall from the local -X face to that side of the cell.
WALL_YAW = {(-1, 0): 0.0, (1, 0): 180.0, (0, -1): 90.0, (0, 1): -90.0}


def wall_edges(corridor, inside):
    """[(cell, yaw)] for every wall segment, in a stable order."""
    walls = []
    for cell in sorted(corridor):
        for (dx, dz), yaw in WALL_YAW.items():
            if (cell[0] + dx, cell[1] + dz) not in inside:
                walls.append((cell, yaw))
    return walls


def arch_spans(corridor, inside):
    """[(centre_x, centre_z, yaw)] in cell units (cell (x, z) spans x..x+1, z..z+1).
    yaw 0 spans along X, yaw 90 spans along Z."""
    arches = []
    for x, z in sorted(corridor):
        if (x - 1, z) not in inside and (x + 1, z) in corridor and (x + 2, z) not in inside:
            arches.append((x + 1.0, z + 0.5, 0.0))
        if (x, z - 1) not in inside and (x, z + 1) in corridor and (x, z + 2) not in inside:
            arches.append((x + 0.5, z + 1.0, 90.0))
    return arches


def junction_blocks(corridor, inside):
    """[(centre_x, centre_z)] in cell units for every junction cap (block x..x+2, z..z+2)."""
    covered = set()
    for cx, cz, yaw in arch_spans(corridor, inside):
        if yaw == 0.0:
            covered |= {(int(cx) - 1, int(cz - 0.5)), (int(cx), int(cz - 0.5))}
        else:
            covered |= {(int(cx - 0.5), int(cz) - 1), (int(cx - 0.5), int(cz))}
    used, caps = set(), []
    for x, z in sorted(corridor):
        block = {(x, z), (x + 1, z), (x, z + 1), (x + 1, z + 1)}
        if block <= corridor and not block & covered and not block & used:
            used |= block
            caps.append((x + 1.0, z + 1.0))
    return caps
