#!/usr/bin/env python3
"""Generates assets/palm.glb: a low-poly flat-shaded palm.

The second tree in the set, and the one that makes a map read as jungle rather
than as woodland: a bare leaning trunk under a crown of drooping fronds, against
tree.glb's conifer. Two glTF primitives (trunk material, frond material) sharing
one binary buffer, the same shape as etc/make_tree.py.

Run from the repo root:  python etc/make_palm.py
"""

import json
import math
import struct
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "assets" / "palm.glb"

TRUNK_COLOR = [0.40, 0.31, 0.21, 1.0]
FROND_COLOR = [0.15, 0.44, 0.19, 1.0]

TRUNK_HEIGHT = 3.5
TRUNK_SEGMENTS = 7   # rings up the trunk; each one is where the lean bends
TRUNK_SIDES = 6
TRUNK_BASE_R = 0.20
TRUNK_TOP_R = 0.12
LEAN = 0.55          # how far the crown ends up off the root, in world units

FRONDS = 9
FROND_STEPS = 6      # quads along one frond
FROND_REACH = 2.0    # horizontal span, root to tip
FROND_LIFT = 1.05    # how high it arcs before gravity takes it
FROND_FALL = 2.05    # and how hard it comes back down
FROND_WIDTH = 0.30


def hash01(a, b=0.0):
    """Deterministic hash noise in [0, 1)."""
    v = math.sin(a * 12.9898 + b * 78.233) * 43758.5453
    return v - math.floor(v)


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


def trunk_axis(t):
    """Centre of the trunk a fraction t up it. Leans on a parabola, so the bend
    is all in the upper half and the palm stands on its root rather than out of
    the ground at an angle."""
    return (LEAN * t * t, TRUNK_HEIGHT * t, 0.0)


def build_trunk(prim):
    def ring(t):
        cx, cy, cz = trunk_axis(t)
        r = TRUNK_BASE_R + (TRUNK_TOP_R - TRUNK_BASE_R) * t
        return [(cx + r * math.cos(math.tau * i / TRUNK_SIDES), cy,
                 cz + r * math.sin(math.tau * i / TRUNK_SIDES))
                for i in range(TRUNK_SIDES)]

    for k in range(TRUNK_SEGMENTS):
        lo = ring(k / TRUNK_SEGMENTS)
        hi = ring((k + 1) / TRUNK_SEGMENTS)
        for i in range(TRUNK_SIDES):
            j = (i + 1) % TRUNK_SIDES
            add_tri(prim, lo[i], hi[i], hi[j])
            add_tri(prim, lo[i], hi[j], lo[j])


def build_fronds(prim):
    ox, oy, oz = trunk_axis(1.0)
    for f in range(FRONDS):
        # Evenly spread, then nudged, so no two palms in a row read as the same
        # rosette. Reach and droop vary per frond for the same reason.
        angle = math.tau * f / FRONDS + (hash01(f, 1.0) - 0.5) * 0.35
        dx, dz = math.cos(angle), math.sin(angle)
        px, pz = -dz, dx  # across the frond, on the ground plane
        reach = FROND_REACH * (0.82 + 0.36 * hash01(f, 2.0))
        lift = FROND_LIFT * (0.75 + 0.5 * hash01(f, 3.0))

        def point(s, side):
            h = reach * s
            y = oy + lift * s - FROND_FALL * s * s
            # Tapered at the stem and drawn to a tip: widest around the middle,
            # which is where a frond's leaflets actually are.
            w = FROND_WIDTH * math.sin(math.pi * s ** 0.7) * (1.0 - 0.25 * s)
            return (ox + dx * h + px * w * side, y, oz + dz * h + pz * w * side)

        for k in range(FROND_STEPS):
            s0 = k / FROND_STEPS
            s1 = (k + 1) / FROND_STEPS
            a, b = point(s0, -1.0), point(s0, 1.0)
            c, d = point(s1, -1.0), point(s1, 1.0)
            add_tri(prim, a, c, d)
            add_tri(prim, a, d, b)


def build_primitives():
    trunk = {"pos": [], "norm": []}
    build_trunk(trunk)

    fronds = {"pos": [], "norm": []}
    build_fronds(fronds)
    return [(trunk, "Trunk", TRUNK_COLOR), (fronds, "Fronds", FROND_COLOR)]


def pack_glb(primitives):
    binary = bytearray()
    buffer_views, accessors, gltf_prims, materials = [], [], [], []

    def add_view(blob, target):
        buffer_views.append({"buffer": 0, "byteOffset": len(binary),
                             "byteLength": len(blob), "target": target})
        binary.extend(blob)
        while len(binary) % 4:
            binary.append(0)
        return len(buffer_views) - 1

    for mat_index, (prim, name, color) in enumerate(primitives):
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
            "name": name, "doubleSided": True,
            "pbrMetallicRoughness": {"baseColorFactor": color,
                                     "metallicFactor": 0.0, "roughnessFactor": 1.0},
        })
        gltf_prims.append({
            "attributes": {"POSITION": pos_acc, "NORMAL": norm_acc},
            "indices": idx_acc, "material": mat_index, "mode": 4,
        })

    gltf = {
        "asset": {"version": "2.0", "generator": "infantry etc/make_palm.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "Palm"}],
        "meshes": [{"primitives": gltf_prims, "name": "Palm"}],
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
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(pack_glb(build_primitives()))
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
