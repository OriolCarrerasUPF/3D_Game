"""Build the ARKFALL airlock and corridor modules in Blender (background mode).

Usage (from TJE_Framework/):
    blender --background --factory-startup --python tools/blender/build_base_modules.py -- \
        <models_out_dir> <blend_out_path> <renders_out_dir> <astronaut_obj>

All geometry is authored in GAME coordinates (Y up, X right, +Z forward, metres,
1 m navigation cells) and converted to Blender's Z-up frame. The OBJ export uses
forward = -Z, up = Y, so the exported files are back in game coordinates.

Exported models (one object each, MTL with Kd colours only, like the Kenney kit):
  * airlock.obj      Exit Door. Footprint 2 x 3 cells, pivot at the footprint centre.
                     Row 0 (z < -0.5) is the outside apron; the outer door sits on
                     the edge z = -0.5, which is the navigation crossing; rows 1-2
                     are the inside chamber and open onto a corridor at z = +1.5.
  * corridor_floor.obj        1 x 1 m floor tile, top at y = 0.1, cell-centred.
  * corridor_wall_frame.obj   1 m wall segment on the local -X face of a cell
                              (same convention as the old `base_boundary` slab), with
                              one post at the middle of the segment.
  * corridor_arch.obj         full roof arch over a 2-cell corridor: spans local X
                              -1..+1, 0.08 m thick, rises from the rail to 2.92 m.
  * corridor_junction.obj     cap over a 2 x 2 corner / T / cross block: corner posts,
                              ring beam and two crossed diagonal arches (top 3.02 m).
Corridors are 2 cells wide: walls are drawn on every edge between a corridor cell
and a non-inside cell, so the clear width is 2.0 - 2 x 0.12 = 1.76 m and the clear
height under the rail is 2.20 m. Arches sit on the wall posts, one per metre, only
on straight 2-cell stretches (rules in tools/base_layout_rules.py).
The airlock door opening is 1.40 x 2.10 m; its chamber matches the corridor profile.
The saved .blend holds the export originals (hidden collection) and a preview layout
with an L-shaped corridor and three astronauts at the in-game scale.
"""

import math
import os
import sys

import bmesh
import bpy
from mathutils import Matrix, Vector

ARGS = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(ARGS) != 4:
    raise SystemExit("expected: <models_out_dir> <blend_out_path> <renders_out_dir> <astronaut_obj>")
MODELS_DIR, BLEND_PATH, RENDERS_DIR, ASTRONAUT_OBJ = [os.path.abspath(a) for a in ARGS]

# ---------------------------------------------------------------------------
# Dimensions (metres). The astronaut is 1.1 m wide, 1.8 m tall, 0.75 m deep.
# ---------------------------------------------------------------------------
ASTRONAUT_SCALE = 2.269          # same as assets.json
FLOOR_TOP = 0.10                 # same as the old interior floor
WALL_T = 0.12                    # wall thickness, inside the cell
WALL_TOP = 2.42                  # corridor rail top = airlock wall top
RAIL_BOTTOM = 2.30               # corridor clear height above floor = 2.20 m
STRIPE = (0.95, 1.10)            # orange band height, shared by both modules
DOOR_HALF = 0.70                 # door opening 1.40 m wide
DOOR_TOP = 2.20                  # door opening 2.10 m above the floor
ROOF_RISE = 0.55                 # rounded airlock roof: top at 2.97 m
ARCH_RISE = 0.62                 # corridor arch top at 2.92 m over a 2-cell corridor
POST_HALF = 0.04                 # wall post and arch are 0.08 m thick, at the segment middle
JUNCTION_TOP = 3.02              # top of the crossed arches over a corner / T / cross block

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from base_layout_rules import arch_spans, junction_blocks, wall_edges  # noqa: E402

PALETTE = [  # (name, Kd) - Kenney-compatible flat colours
    ("hull", (0.88, 0.90, 0.93)),
    ("trim", (0.6750623, 0.7100219, 0.7735849)),
    ("dark", (0.2745098, 0.2980392, 0.3411765)),
    ("accent", (1.0, 0.6285242, 0.2028302)),
    ("light", (0.45, 0.95, 0.55)),
]
MAT_INDEX = {name: i for i, (name, _) in enumerate(PALETTE)}


def g2b(x, y, z):
    """Game (Y up, +Z forward) -> Blender (Z up)."""
    return Vector((x, -z, y))


G2B = Matrix(((1, 0, 0, 0), (0, 0, -1, 0), (0, 1, 0, 0), (0, 0, 0, 1)))


def game_matrix(x, y, z, yaw_deg=0.0, scale=1.0):
    """World transform in game space; +90 deg yaw maps +X to +Z (world_transform convention)."""
    a = math.radians(yaw_deg)
    c, s = math.cos(a), math.sin(a)
    rot = Matrix(((c, 0, -s, 0), (0, 1, 0, 0), (s, 0, c, 0), (0, 0, 0, 1)))
    return Matrix.Translation((x, y, z)) @ rot @ Matrix.Scale(scale, 4)


def to_blender_matrix(game_m):
    return G2B @ game_m @ G2B.inverted()


class Builder:
    def __init__(self):
        self.bm = bmesh.new()
        self.yaw_deg = 0.0     # applied to the following parts (game yaw: +90 maps +X to +Z)

    def prism(self, poly, axis, a0, a1, mat):
        """Closed prism: 2D polygon `poly` extruded along a game axis from a0 to a1.
        axis 'z': poly is (x, y); 'x': poly is (z, y); 'y': poly is (x, z)."""
        c, s = math.cos(math.radians(self.yaw_deg)), math.sin(math.radians(self.yaw_deg))

        def rotated(x, y, z):
            return g2b(x * c - z * s, y, x * s + z * c)

        def point(p, a):
            if axis == "z":
                return rotated(p[0], p[1], a)
            if axis == "x":
                return rotated(a, p[1], p[0])
            return rotated(p[0], a, p[1])

        v0 = [self.bm.verts.new(point(p, a0)) for p in poly]
        v1 = [self.bm.verts.new(point(p, a1)) for p in poly]
        faces = [self.bm.faces.new(list(reversed(v0))), self.bm.faces.new(v1)]
        n = len(poly)
        for i in range(n):
            j = (i + 1) % n
            faces.append(self.bm.faces.new([v0[i], v0[j], v1[j], v1[i]]))
        for f in faces:
            f.material_index = MAT_INDEX[mat]

    def box(self, x0, x1, y0, y1, z0, z1, mat):
        self.prism([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], "z", z0, z1, mat)

    def cylinder(self, axis, c0, c1, a0, a1, r, mat, seg=12):
        poly = [(c0 + r * math.cos(2 * math.pi * k / seg), c1 + r * math.sin(2 * math.pi * k / seg)) for k in range(seg)]
        self.prism(poly, axis, a0, a1, mat)

    def arch_band(self, cx, cy, outer, inner, t0, t1, z0, z1, mat, steps=8):
        """Elliptic arc band (outer/inner radii (rx, ry)) from angle t0 to t1, extruded along z."""
        ts = [t0 + (t1 - t0) * k / steps for k in range(steps + 1)]
        out_pts = [(cx + outer[0] * math.cos(t), cy + outer[1] * math.sin(t)) for t in ts]
        in_pts = [(cx + inner[0] * math.cos(t), cy + inner[1] * math.sin(t)) for t in reversed(ts)]
        self.prism(out_pts + in_pts, "z", z0, z1, mat)

    def mirror_x_box(self, x0, x1, y0, y1, z0, z1, mat):
        self.box(x0, x1, y0, y1, z0, z1, mat)
        self.box(-x1, -x0, y0, y1, z0, z1, mat)

    def to_object(self, name, collection):
        bmesh.ops.recalc_face_normals(self.bm, faces=self.bm.faces)
        mesh = bpy.data.meshes.new(name)
        self.bm.to_mesh(mesh)
        self.bm.free()
        mesh.uv_layers.new(name="UVMap")
        for mat_name, _ in PALETTE:
            mesh.materials.append(MATERIALS[mat_name])
        obj = bpy.data.objects.new(name, mesh)
        collection.objects.link(obj)
        return obj


def make_material(name, rgb, emission=False):
    mat = bpy.data.materials.new(name)
    mat.diffuse_color = (*rgb, 1.0)
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    if emission:
        nodes.clear()
        out = nodes.new("ShaderNodeOutputMaterial")
        em = nodes.new("ShaderNodeEmission")
        em.inputs["Color"].default_value = (*rgb, 1.0)
        em.inputs["Strength"].default_value = 1.0
        mat.node_tree.links.new(em.outputs["Emission"], out.inputs["Surface"])
    else:
        bsdf = nodes.get("Principled BSDF")
        bsdf.inputs["Base Color"].default_value = (*rgb, 1.0)
        bsdf.inputs["Roughness"].default_value = 0.6
    return mat


# ---------------------------------------------------------------------------
# Models
# ---------------------------------------------------------------------------

def build_airlock(collection):
    b = Builder()
    z_front, z_back = -0.60, 1.50
    w_in = 1.0 - WALL_T       # inner wall face at 0.88 -> clear width 1.76 m, same as a corridor
    w_wall = 0.97             # outer wall face; the ribs reach the 1.0 m footprint edge

    # Floor: base plate + darker grating, top at FLOOR_TOP.
    b.box(-w_in, w_in, 0.0, 0.08, z_front, z_back, "trim")
    b.box(-0.85, 0.85, 0.08, FLOOR_TOP, -0.48, z_back, "dark")
    # Side walls, a dark foundation skirt and the rounded (half-ellipse) roof.
    b.mirror_x_box(w_in, w_wall, 0.0, WALL_TOP, z_front, z_back, "hull")
    b.mirror_x_box(w_wall, 1.0, 0.0, 0.16, -0.45, 1.35, "dark")
    roof = [(w_wall * math.cos(math.pi * k / 10), WALL_TOP + ROOF_RISE * math.sin(math.pi * k / 10)) for k in range(11)]
    b.prism(roof, "z", z_front, z_back, "hull")
    # Structural ribs (front, middle at the inner door, back) and the orange band.
    for za, zb in ((z_front, -0.45), (0.42, 0.58), (1.35, z_back)):
        b.mirror_x_box(w_wall, 1.0, 0.0, WALL_TOP, za, zb, "trim")
        b.arch_band(0.0, WALL_TOP, (1.0, ROOF_RISE + 0.04), (0.9, ROOF_RISE - 0.06), 0.0, math.pi, za, zb, "trim", steps=10)
    for za, zb in ((-0.45, 0.42), (0.58, 1.35)):
        b.mirror_x_box(w_wall, 1.0, STRIPE[0], STRIPE[1], za, zb, "accent")

    # Front wall with the outer door opening (1.40 x 2.10 m above the floor).
    b.mirror_x_box(DOOR_HALF, w_in, 0.0, WALL_TOP, z_front, z_front + WALL_T, "hull")
    b.box(-DOOR_HALF, DOOR_HALF, DOOR_TOP, WALL_TOP, z_front, z_front + WALL_T, "hull")
    # Outer door frame (hazard orange) and threshold.
    b.mirror_x_box(DOOR_HALF, DOOR_HALF + 0.16, FLOOR_TOP, DOOR_TOP, -0.75, z_front, "accent")
    b.box(-(DOOR_HALF + 0.16), DOOR_HALF + 0.16, DOOR_TOP, DOOR_TOP + 0.18, -0.75, z_front, "accent")
    b.box(-(DOOR_HALF + 0.16), DOOR_HALF + 0.16, 0.0, FLOOR_TOP, -0.75, z_front, "trim")
    # Status light above the outer door.
    b.box(-0.30, 0.30, DOOR_TOP + 0.20, DOOR_TOP + 0.30, -0.68, z_front, "light")

    # Inner door frame (visual second door: chamber between the two doors).
    b.mirror_x_box(DOOR_HALF, w_in, FLOOR_TOP, WALL_TOP, 0.44, 0.56, "trim")
    b.box(-DOOR_HALF, DOOR_HALF, DOOR_TOP, WALL_TOP, 0.44, 0.56, "trim")
    b.mirror_x_box(DOOR_HALF, DOOR_HALF + 0.04, FLOOR_TOP, DOOR_TOP, 0.42, 0.58, "accent")

    # Roof equipment: pressure tank with saddles, vent, beacon (seated on the curved roof).
    def roof_y(x):
        return WALL_TOP + ROOF_RISE * math.sqrt(max(0.0, 1.0 - (x / w_wall) ** 2))

    tank_y = roof_y(-0.35) + 0.12
    b.cylinder("z", -0.35, tank_y, -0.10, 1.20, 0.14, "trim", seg=14)
    for za in (0.0, 0.95):
        b.box(-0.52, -0.18, roof_y(-0.52) - 0.04, tank_y - 0.04, za, za + 0.12, "dark")
    b.box(0.12, 0.52, roof_y(0.52) - 0.04, roof_y(0.12) + 0.08, 0.50, 1.10, "dark")
    b.cylinder("y", 0.35, -0.30, roof_y(0.44) - 0.03, roof_y(0.44) + 0.05, 0.09, "accent", seg=10)
    b.cylinder("y", 0.35, -0.30, roof_y(0.44) + 0.05, roof_y(0.44) + 0.17, 0.06, "light", seg=10)

    # Outside apron: ramp up to the floor, hazard edges and two bollards.
    b.prism([(-1.45, 0.0), (-0.75, 0.0), (-0.75, FLOOR_TOP)], "x", -0.68, 0.68, "trim")
    for xa, xb in ((-0.76, -0.68), (0.68, 0.76)):
        b.prism([(-1.45, 0.0), (-0.75, 0.0), (-0.75, FLOOR_TOP + 0.015), (-1.45, 0.015)], "x", xa, xb, "accent")
    for x in (-0.88, 0.88):
        b.cylinder("y", x, -1.32, 0.0, 0.55, 0.07, "accent", seg=10)
        b.cylinder("y", x, -1.32, 0.55, 0.60, 0.075, "dark", seg=10)
    return b.to_object("airlock", collection)


def build_corridor_floor(collection):
    b = Builder()
    b.box(-0.5, 0.5, 0.0, 0.08, -0.5, 0.5, "trim")
    b.box(-0.44, 0.44, 0.08, FLOOR_TOP, -0.44, 0.44, "dark")
    return b.to_object("corridor_floor", collection)


def build_corridor_wall(collection):
    b = Builder()
    x0, x1 = -0.5, -0.5 + WALL_T
    b.box(x0, x1, 0.0, FLOOR_TOP, -0.5, 0.5, "trim")                 # sill
    b.box(x0, x1, FLOOR_TOP, STRIPE[0], -0.5, 0.5, "hull")           # lower panel
    b.box(x0, x1, STRIPE[0], STRIPE[1], -0.5, 0.5, "accent")         # orange band
    # One post in the middle of the segment, under the arch of that corridor row.
    b.box(x0, x1, STRIPE[1], RAIL_BOTTOM, -POST_HALF, POST_HALF, "trim")
    b.box(x0, x1, RAIL_BOTTOM, WALL_TOP, -0.5, 0.5, "hull")          # top rail
    return b.to_object("corridor_wall_frame", collection)


def build_corridor_arch(collection):
    """Full arch across a 2-cell corridor. Local X spans the corridor (-1..+1, the outer
    wall faces), centred between the two cells; its feet sit on the wall posts."""
    b = Builder()
    b.arch_band(0.0, RAIL_BOTTOM, (1.0, ARCH_RISE), (1.0 - WALL_T, ARCH_RISE - 0.1), 0.0, math.pi,
                -POST_HALF, POST_HALF, "trim", steps=16)
    return b.to_object("corridor_arch", collection)


def build_corridor_junction(collection):
    """Cap over a 2 x 2 junction block (corner, T or cross), centred on the block.
    It stands on its own four corner posts, so it does not depend on which sides of
    the block have walls: a ring beam at rail height bridges the open sides and two
    diagonal arches cross under a hub at the top."""
    b = Builder()
    half = POST_HALF + 0.02                                         # 0.12 m corner posts
    for sx in (-1.0, 1.0):
        for sz in (-1.0, 1.0):
            b.box(sx - half, sx + half, 0.0, WALL_TOP, sz - half, sz + half, "trim")
    # Ring beam: same section as the wall top rail, on the inner side of each edge.
    b.box(-1.0, -1.0 + WALL_T, RAIL_BOTTOM, WALL_TOP, -1.0, 1.0, "hull")
    b.box(1.0 - WALL_T, 1.0, RAIL_BOTTOM, WALL_TOP, -1.0, 1.0, "hull")
    b.box(-1.0, 1.0, RAIL_BOTTOM, WALL_TOP, -1.0, -1.0 + WALL_T, "hull")
    b.box(-1.0, 1.0, RAIL_BOTTOM, WALL_TOP, 1.0 - WALL_T, 1.0, "hull")
    # Two diagonal arches from corner to corner, crossing at the top.
    diagonal = math.sqrt(2.0)
    rise = JUNCTION_TOP - RAIL_BOTTOM
    for yaw in (45.0, 135.0):
        b.yaw_deg = yaw
        b.arch_band(0.0, RAIL_BOTTOM, (diagonal, rise), (diagonal - WALL_T, rise - 0.1), 0.0, math.pi,
                    -POST_HALF, POST_HALF, "trim", steps=16)
    b.yaw_deg = 0.0
    # Hub where the arches cross, with a small beacon light.
    b.box(-0.14, 0.14, JUNCTION_TOP - 0.12, JUNCTION_TOP + 0.04, -0.14, 0.14, "accent")
    b.cylinder("y", 0.0, 0.0, JUNCTION_TOP + 0.04, JUNCTION_TOP + 0.10, 0.06, "light", seg=10)
    return b.to_object("corridor_junction", collection)


# ---------------------------------------------------------------------------
# Scene setup
# ---------------------------------------------------------------------------

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
MATERIALS = {name: make_material(name, rgb) for name, rgb in PALETTE}

export_col = bpy.data.collections.new("ARKFALL_export")
preview_col = bpy.data.collections.new("Preview_layout")
scene.collection.children.link(export_col)
scene.collection.children.link(preview_col)

airlock = build_airlock(export_col)
floor = build_corridor_floor(export_col)
wall = build_corridor_wall(export_col)
arch = build_corridor_arch(export_col)
junction = build_corridor_junction(export_col)

# ---- export -----------------------------------------------------------------
os.makedirs(MODELS_DIR, exist_ok=True)
report = {}
for obj in (airlock, floor, wall, arch, junction):
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    obj_path = os.path.join(MODELS_DIR, obj.name + ".obj")
    bpy.ops.wm.obj_export(filepath=obj_path, export_selected_objects=True, export_uv=True,
                          export_normals=True, export_materials=True, export_triangulated_mesh=True,
                          export_smooth_groups=False, export_colors=False, apply_modifiers=True,
                          forward_axis="NEGATIVE_Z", up_axis="Y", path_mode="AUTO")
    # Rewrite the MTL in the Kenney style the engine parser expects (Kd only).
    used = []
    with open(obj_path, encoding="utf-8") as fh:
        lines = fh.read().splitlines()
    for line in lines:
        if line.startswith("usemtl "):
            name = line.split()[1]
            if name not in used:
                used.append(name)
    with open(obj_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# ARKFALL base module, generated by tools/blender/build_base_modules.py\n")
        fh.write("\n".join(l for l in lines if not l.startswith("#")) + "\n")
    with open(os.path.join(MODELS_DIR, obj.name + ".mtl"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# ARKFALL base module, generated by tools/blender/build_base_modules.py\n")
        for name in used:
            rgb = dict(PALETTE)[name]
            fh.write(f"\nnewmtl {name}\nKd {rgb[0]:.7g} {rgb[1]:.7g} {rgb[2]:.7g}\n")
    xs, ys, zs = [], [], []
    for v in obj.data.vertices:   # Blender (X, Y, Z) -> game (x, y, z) = (X, Z, -Y)
        xs.append(v.co.x); ys.append(v.co.z); zs.append(-v.co.y)
    report[obj.name] = {
        "bounds_min": [round(min(xs), 3), round(min(ys), 3), round(min(zs), 3)],
        "bounds_max": [round(max(xs), 3), round(max(ys), 3), round(max(zs), 3)],
        "triangles": sum(len(p.vertices) - 2 for p in obj.data.polygons),
        "materials": used,
    }

# The originals stay at the origin for export; the preview below uses linked copies.
export_col.hide_render = True

# ---- preview layout ---------------------------------------------------------
# Airlock footprint origin cell (0, 0): cells x 0..1, z 0..2, centre (1, 1.5).
AIRLOCK_CELLS_INSIDE = {(0, 1), (0, 2), (1, 1), (1, 2)}
corridor_cells = {(x, z) for x in (0, 1) for z in range(3, 8)} | {(x, z) for x in range(2, 7) for z in (6, 7)}
inside = corridor_cells | AIRLOCK_CELLS_INSIDE


def place(mesh_obj, name, game_m):
    inst = bpy.data.objects.new(name, mesh_obj.data)      # linked duplicate
    inst.matrix_world = to_blender_matrix(game_m)
    preview_col.objects.link(inst)
    return inst


place(airlock, "airlock_preview", game_matrix(1.0, 0.0, 1.5))
for (cx, cz) in sorted(corridor_cells):
    place(floor, f"floor_{cx}_{cz}", game_matrix(cx + 0.5, 0.0, cz + 0.5))
for (cx, cz), yaw in wall_edges(corridor_cells, inside):
    place(wall, f"wall_{cx}_{cz}_{int(yaw)}", game_matrix(cx + 0.5, 0.0, cz + 0.5, yaw))
for ax_, az_, yaw in arch_spans(corridor_cells, inside):
    place(arch, f"arch_{ax_}_{az_}", game_matrix(ax_, 0.0, az_, yaw))
for jx, jz in junction_blocks(corridor_cells, inside):
    place(junction, f"junction_{jx}_{jz}", game_matrix(jx, 0.0, jz))

# Ground.
ground_mat = make_material("mars_ground", (0.72, 0.38, 0.24))
bpy.ops.mesh.primitive_plane_add(size=60, location=g2b(3.0, -0.002, 4.0))
ground = bpy.context.active_object
ground.name = "ground"
ground.data.materials.append(ground_mat)
for c in ground.users_collection:
    c.objects.unlink(ground)
preview_col.objects.link(ground)

# Astronauts (assets.json scale 2.269 -> 1.10 x 1.80 x 0.75 m).
bpy.ops.object.select_all(action="DESELECT")
bpy.ops.wm.obj_import(filepath=ASTRONAUT_OBJ, forward_axis="NEGATIVE_Z", up_axis="Y")
parts = list(bpy.context.selected_objects)
bpy.context.view_layer.objects.active = parts[0]
if len(parts) > 1:
    bpy.ops.object.join()
astro_src = bpy.context.active_object
astro_src.name = "astronaut_source"
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
for c in astro_src.users_collection:
    c.objects.unlink(astro_src)
export_col.objects.link(astro_src)
astro_dims = [round(d * ASTRONAUT_SCALE, 3) for d in astro_src.dimensions]  # Blender X, Y(depth), Z(height)
ASTRONAUTS = {
    "astronaut_door": (1.0, FLOOR_TOP, 0.95, 0.0),       # standing in the outer doorway
    "astronaut_corridor": (1.0, FLOOR_TOP, 3.4, 0.0),     # in the corridor, facing the section camera
    "astronaut_corner": (4.2, FLOOR_TOP, 7.0, 90.0),      # turning into the east branch
}
for name, (x, y, z, yaw) in ASTRONAUTS.items():
    place(astro_src, name, game_matrix(x, y, z, yaw, ASTRONAUT_SCALE))

# Lighting and world.
sun_data = bpy.data.lights.new("sun", "SUN")
sun_data.energy = 3.2
sun_data.angle = math.radians(8)
sun = bpy.data.objects.new("sun", sun_data)
sun.rotation_euler = (math.radians(50), math.radians(10), math.radians(35))
preview_col.objects.link(sun)
world = bpy.data.worlds.new("world")
world.use_nodes = True
world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.55, 0.47, 0.42, 1.0)
world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.9
scene.world = world
scene.view_settings.view_transform = "Standard"

try:
    scene.render.engine = "BLENDER_EEVEE"
except TypeError:
    scene.render.engine = "BLENDER_EEVEE_NEXT"
scene.render.resolution_x, scene.render.resolution_y = 1600, 1000
scene.render.film_transparent = False

label_mat = make_material("label", (1.0, 0.93, 0.25), emission=True)


def make_camera(name, location, target, ortho_scale=None, lens=32, clip_start=0.05):
    cam_data = bpy.data.cameras.new(name)
    cam_data.clip_start = clip_start
    cam_data.clip_end = 200
    if ortho_scale:
        cam_data.type = "ORTHO"
        cam_data.ortho_scale = ortho_scale
    else:
        cam_data.lens = lens
    cam = bpy.data.objects.new(name, cam_data)
    loc, tgt = g2b(*location), g2b(*target)
    cam.location = loc
    cam.rotation_euler = (tgt - loc).to_track_quat("-Z", "Y").to_euler()
    preview_col.objects.link(cam)
    return cam


def label(cam, text, u, v, size=0.16):
    """Text overlay in an ortho camera's view plane; (u, v) in metres from the view centre."""
    curve = bpy.data.curves.new(f"lbl_{text}", "FONT")
    curve.body = text
    curve.size = size
    curve.align_x = "CENTER"
    obj = bpy.data.objects.new(f"lbl_{cam.name}_{text}", curve)
    obj.data.materials.append(label_mat)
    obj.parent = cam
    obj.location = (u, v, -0.1)       # in front of everything the ortho camera sees
    preview_col.objects.link(obj)


def dim_line(cam, u0, v0, u1, v1, width=0.025):
    length = math.hypot(u1 - u0, v1 - v0)
    bpy.ops.mesh.primitive_plane_add(size=1)
    line = bpy.context.active_object
    for c in line.users_collection:
        c.objects.unlink(line)
    preview_col.objects.link(line)
    line.data.materials.append(label_mat)
    line.parent = cam
    line.location = ((u0 + u1) / 2, (v0 + v1) / 2, -0.1)
    line.rotation_euler = (0, 0, math.atan2(v1 - v0, u1 - u0))
    line.scale = (length, width, 1)
    for u, v in ((u0, v0), (u1, v1)):   # end ticks
        bpy.ops.mesh.primitive_plane_add(size=1)
        tick = bpy.context.active_object
        for c in tick.users_collection:
            c.objects.unlink(tick)
        preview_col.objects.link(tick)
        tick.data.materials.append(label_mat)
        tick.parent = cam
        tick.location = (u, v, -0.1)
        tick.rotation_euler = (0, 0, math.atan2(v1 - v0, u1 - u0) + math.pi / 2)
        tick.scale = (0.16, width, 1)


renders = {}
os.makedirs(RENDERS_DIR, exist_ok=True)


def render(cam, filename):
    scene.camera = cam
    for obj in preview_col.objects:   # labels only for their own camera
        if obj.parent and obj.parent.type == "CAMERA":
            obj.hide_render = obj.parent != cam
    path = os.path.join(RENDERS_DIR, filename)
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    renders[filename] = path


# 1) Strategy-style overview.
cam_overview = make_camera("cam_overview", (9.5, 9.0, -3.5), (2.4, 0.4, 4.4), lens=30)
# 2) Airlock front, orthographic, astronaut in the outer doorway.
cx, cy = 1.0, 1.35
cam_front = make_camera("cam_front", (cx, cy, -9.0), (cx, cy, 0.0), ortho_scale=6.2)
# Camera looks along +Z, so camera-right is world -X: u = -(x - cx).
dim_line(cam_front, -(1.7 - cx), 0.35 - cy, -(0.3 - cx), 0.35 - cy)
label(cam_front, "door 1.40 m", 0.0, 0.42 - cy)
dim_line(cam_front, -(1.92 - cx) - 0.25, FLOOR_TOP - cy, -(1.92 - cx) - 0.25, DOOR_TOP - cy)
label(cam_front, "2.10 m", -(1.92 - cx) - 0.62, (FLOOR_TOP + DOOR_TOP) / 2 - cy)
dim_line(cam_front, -(2.0 - cx), -0.2 - cy, -(0.0 - cx), -0.2 - cy)
label(cam_front, "airlock 2.0 m = 2 cells (footprint 2 x 3)", 0.0, -0.45 - cy)
label(cam_front, "astronaut 1.10 x 1.80 m", 1.55, 1.05, size=0.14)
# 3) Corridor cross-section, orthographic, looking back at the airlock's inner opening.
cam_section = make_camera("cam_section", (1.0, 1.25, 4.6), (1.0, 1.25, 0.0), ortho_scale=5.2, clip_start=0.01)
# Looking along -Z: camera-right is world +X.
sx, sy = 1.0, 1.25
dim_line(cam_section, WALL_T - sx, 2.04 - sy, (2.0 - WALL_T) - sx, 2.04 - sy)   # between head and rail
label(cam_section, "clear 1.76 m", 0.0, 2.11 - sy, size=0.13)
dim_line(cam_section, 1.62 - sx, FLOOR_TOP - sy, 1.62 - sx, RAIL_BOTTOM - sy)
label(cam_section, "2.20 m", 1.62 - sx + 0.36, (FLOOR_TOP + RAIL_BOTTOM) / 2 - sy)
label(cam_section, "corridor 2 cells wide", 0.0, -0.20 - sy)

render(cam_overview, "arkfall_airlock_overview.png")
render(cam_front, "arkfall_airlock_front.png")
render(cam_section, "arkfall_corridor_section.png")

# Keep the export originals out of the way in the saved file; the preview uses linked copies.
export_col.hide_viewport = True
export_col.hide_render = True
scene.camera = cam_overview
os.makedirs(os.path.dirname(BLEND_PATH), exist_ok=True)
bpy.ops.wm.save_as_mainfile(filepath=BLEND_PATH)

report["astronaut_scaled_dims_xyz_blender"] = astro_dims
report["renders"] = renders
report["engine"] = scene.render.engine
print("ARKFALL_REPORT", report)
