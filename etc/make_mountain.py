#!/usr/bin/env python3
"""Generates assets/mountain.glb and assets/rock.glb: the hardcore map's terrain.

The mountain is a tepui — a jungle-topped plateau with sheer cliff faces. That
shape is chosen for a reason: a level object collides as one axis-aligned box,
so a round mountain would either bump players on invisible air or let them walk
into its skirt. A cliff that rises straight off a rectangular base is a silhouette
a box can actually describe, and it is also the most obvious way to say "you are
not climbing this" to somebody looking at it from above.

The outline undulates outward from that base rectangle and never inward, so the
collider (the plain rectangle, see FOOTPRINT) always sits inside what is drawn.

The rock is the same machinery at boulder scale: lane cover, authored around a
unit footprint so the level can scale each instance.

Run from the repo root:  python etc/make_mountain.py
"""

import json
import math
import struct
from pathlib import Path

ASSETS = Path(__file__).resolve().parent.parent / "assets"

# --- The mountain's collision footprint, in world units -----------------------
# Shared with etc/make_level.py, which imports these to author the collider that
# goes with the model. Half-extents: the massif spans [-HALF_X, HALF_X] on x and
# [-HALF_Z, HALF_Z] on z, and stands HEIGHT tall. Long on z and narrow on x so
# that a map with its bases east and west has to go around it rather than past
# it, and so the trip is long enough to be a decision.
HALF_X = 11.0
HALF_Z = 17.0
HEIGHT = 11.0

# Boulder half-extents at scale 1. The level scales instances up from here.
ROCK_HALF = 1.0
ROCK_HEIGHT = 1.3

CLIFF_COLOR = [0.34, 0.31, 0.28, 1.0]
CANOPY_COLOR = [0.13, 0.32, 0.15, 1.0]
ROCK_COLOR = [0.38, 0.36, 0.33, 1.0]

# How far the drawn outline may bulge past the collision rectangle. Outward
# only: the box has to stay inside the silhouette, or players would walk into
# rock. Small enough that the overlap at the foot of an eleven-unit cliff is
# hard to catch from the isometric camera.
BULGE = 0.9

# Perimeter samples around the base, and the cliff's rings up the face. The
# cliff is faceted on purpose — flat shading plus a coarse ring count is what
# gives it readable planes instead of a smooth lump.
OUTLINE = 44

# (height, inward scale) up the face: a slight batter, so the plateau overhangs
# nothing and the base ring stays the widest one. Kept shallow — the face is
# meant to read as sheer, and every unit of lean is a unit of collision box
# standing proud of the drawn cliff further up.
MOUNTAIN_RINGS = [(0.0, 1.000), (1.3, 0.990), (4.2, 0.965),
                  (7.6, 0.940), (9.9, 0.915), (11.0, 0.885)]
ROCK_RINGS = [(0.0, 1.000), (0.35, 0.960), (0.85, 0.820), (1.30, 0.560)]


def hash01(a, b=0.0):
    """Deterministic hash noise in [0, 1)."""
    v = math.sin(a * 12.9898 + b * 78.233) * 43758.5453
    return v - math.floor(v)


def outline(half_x, half_z, bulge, seed):
    """Points around the base rectangle, each pushed outward by a smooth wobble.

    Two sinusoids at different frequencies with hashed phases: irregular enough
    that no two faces match, smooth enough that the silhouette reads as rock
    weathered into buttresses rather than as noise.
    """
    p1 = hash01(seed, 1.0) * math.tau
    p2 = hash01(seed, 2.0) * math.tau
    pts = []
    for i in range(OUTLINE):
        u = i / OUTLINE
        # Walk the rectangle's perimeter at constant angle rather than constant
        # arc length: the corners then land between samples the same way a
        # circle's would, and the faces stay evenly divided.
        angle = u * math.tau
        cx, cz = math.cos(angle), math.sin(angle)
        # Scale the unit direction out to the rectangle's edge.
        t = min(half_x / abs(cx) if abs(cx) > 1e-6 else 1e9,
                half_z / abs(cz) if abs(cz) > 1e-6 else 1e9)
        x, z = cx * t, cz * t
        push = bulge * (0.6 * (0.5 + 0.5 * math.sin(3.0 * angle + p1)) +
                        0.4 * (0.5 + 0.5 * math.sin(7.0 * angle + p2)))
        length = math.hypot(x, z)
        pts.append((x + x / length * push, z + z / length * push))
    return pts


def face_normal(a, b, c):
    ux, uy, uz = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
    vx, vy, vz = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
    nx, ny, nz = (uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx)
    length = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
    return (nx / length, ny / length, nz / length)


def add_tri(prim, a, b, c):
    """Appends one flat-shaded triangle (verts duplicated, face normal)."""
    n = face_normal(a, b, c)
    for p in (a, b, c):
        prim["pos"].append(p)
        prim["norm"].append(n)


def build_massif(half_x, half_z, height, rings, bulge, seed, crown):
    """A plateau: faceted cliff faces to the top ring, then a capped crown.

    Returns (sides, top) primitives — separate so the cliff and whatever grows
    on the plateau can take different materials.
    """
    swollen = outline(half_x, half_z, bulge, seed)
    plain = outline(half_x, half_z, 0.0, seed)

    # Each ring is the plain rectangle scaled by the batter, with the bulge
    # blended back on top of it — at full strength along the foot and easing
    # off with height, so the buttresses belong to the base of the cliff and
    # the plateau's rim stays clean. Blending outward from the plain outline
    # rather than shrinking the swollen one is what keeps every ring at least
    # as wide as the collider it has to hide.
    levels = []
    for y, scale in rings:
        fade = 1.0 - 0.55 * (y / height)
        ring = []
        for (px, pz), (sx, sz) in zip(plain, swollen):
            ring.append((scale * (px + (sx - px) * fade), y,
                         scale * (pz + (sz - pz) * fade)))
        levels.append(ring)

    sides = {"pos": [], "norm": []}
    for k in range(len(levels) - 1):
        lo, hi = levels[k], levels[k + 1]
        for i in range(OUTLINE):
            j = (i + 1) % OUTLINE
            add_tri(sides, lo[i], hi[i], hi[j])
            add_tri(sides, lo[i], hi[j], lo[j])

    top = {"pos": [], "norm": []}
    rim = levels[-1]
    apex = (0.0, height + crown, 0.0)
    for i in range(OUTLINE):
        j = (i + 1) % OUTLINE
        add_tri(top, rim[i], apex, rim[j])
    return sides, top


def pack_glb(primitives, name):
    binary = bytearray()
    buffer_views, accessors, gltf_prims, materials = [], [], [], []

    def add_view(blob, target):
        buffer_views.append({"buffer": 0, "byteOffset": len(binary),
                             "byteLength": len(blob), "target": target})
        binary.extend(blob)
        while len(binary) % 4:
            binary.append(0)
        return len(buffer_views) - 1

    for mat_index, (prim, mat_name, color) in enumerate(primitives):
        count = len(prim["pos"])
        pos_blob = b"".join(struct.pack("<3f", *p) for p in prim["pos"])
        norm_blob = b"".join(struct.pack("<3f", *n) for n in prim["norm"])
        idx_blob = b"".join(struct.pack("<H", i) for i in range(count))

        pos_acc = len(accessors)
        accessors.append({
            "bufferView": add_view(pos_blob, 34962), "componentType": 5126,
            "count": count, "type": "VEC3",
            "min": [min(p[i] for p in prim["pos"]) for i in range(3)],
            "max": [max(p[i] for p in prim["pos"]) for i in range(3)],
        })
        norm_acc = len(accessors)
        accessors.append({
            "bufferView": add_view(norm_blob, 34962), "componentType": 5126,
            "count": count, "type": "VEC3",
        })
        idx_acc = len(accessors)
        accessors.append({
            "bufferView": add_view(idx_blob, 34963), "componentType": 5123,
            "count": count, "type": "SCALAR",
        })

        materials.append({
            "name": mat_name, "doubleSided": True,
            "pbrMetallicRoughness": {"baseColorFactor": color,
                                     "metallicFactor": 0.0, "roughnessFactor": 1.0},
        })
        gltf_prims.append({
            "attributes": {"POSITION": pos_acc, "NORMAL": norm_acc},
            "indices": idx_acc, "material": mat_index, "mode": 4,
        })

    gltf = {
        "asset": {"version": "2.0", "generator": "infantry etc/make_mountain.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": name}],
        "meshes": [{"primitives": gltf_prims, "name": name}],
        "materials": materials,
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"byteLength": len(binary)}],
    }

    json_blob = json.dumps(gltf, separators=(",", ":")).encode()
    while len(json_blob) % 4:
        json_blob += b" "

    total = 12 + 8 + len(json_blob) + 8 + len(binary)
    out = bytearray()
    out += struct.pack("<3I", 0x46546C67, 2, total)                   # glTF header
    out += struct.pack("<2I", len(json_blob), 0x4E4F534A) + json_blob  # JSON chunk
    out += struct.pack("<2I", len(binary), 0x004E4942) + binary        # BIN chunk
    return bytes(out)


def main():
    ASSETS.mkdir(parents=True, exist_ok=True)

    sides, top = build_massif(HALF_X, HALF_Z, HEIGHT, MOUNTAIN_RINGS, BULGE,
                              seed=7.0, crown=0.9)
    path = ASSETS / "mountain.glb"
    path.write_bytes(pack_glb([(sides, "Cliff", CLIFF_COLOR),
                               (top, "Canopy", CANOPY_COLOR)], "Mountain"))
    print(f"wrote {path} ({path.stat().st_size} bytes)")

    sides, top = build_massif(ROCK_HALF, ROCK_HALF, ROCK_HEIGHT, ROCK_RINGS,
                              bulge=0.30, seed=3.0, crown=0.10)
    path = ASSETS / "rock.glb"
    path.write_bytes(pack_glb([(sides, "Rock", ROCK_COLOR),
                               (top, "RockTop", ROCK_COLOR)], "Rock"))
    print(f"wrote {path} ({path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
