#!/usr/bin/env python3
"""Generates assets/tree.glb: a low-poly flat-shaded pine tree.

Trunk = hexagonal prism, canopy = two stacked cones. Two glTF primitives
(trunk material, leaf material) sharing one binary buffer. Run from the
repo root:  python etc/make_tree.py
"""

import json
import math
import struct
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "assets" / "tree.glb"

TRUNK_COLOR = [0.42, 0.28, 0.16, 1.0]
LEAF_COLOR = [0.16, 0.46, 0.20, 1.0]


def ring(n, r, y):
    return [(r * math.cos(2 * math.pi * i / n), y, r * math.sin(2 * math.pi * i / n))
            for i in range(n)]


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


def add_prism(prim, n, r, y0, y1):
    bottom, top = ring(n, r, y0), ring(n, r, y1)
    for i in range(n):
        j = (i + 1) % n
        add_tri(prim, bottom[i], top[i], top[j])
        add_tri(prim, bottom[i], top[j], bottom[j])


def add_cone(prim, n, r, y0, y1, cap=True):
    base = ring(n, r, y0)
    apex = (0.0, y1, 0.0)
    for i in range(n):
        j = (i + 1) % n
        add_tri(prim, base[i], apex, base[j])
        if cap:
            add_tri(prim, base[i], base[j], (0.0, y0, 0.0))


def build_primitives():
    trunk = {"pos": [], "norm": []}
    add_prism(trunk, 6, 0.16, 0.0, 1.0)

    leaves = {"pos": [], "norm": []}
    add_cone(leaves, 7, 0.85, 0.8, 2.2)
    add_cone(leaves, 7, 0.62, 1.6, 2.9, cap=False)
    return [(trunk, "Trunk", TRUNK_COLOR), (leaves, "Leaves", LEAF_COLOR)]


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
        "asset": {"version": "2.0", "generator": "infantry etc/make_tree.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "Tree"}],
        "meshes": [{"primitives": gltf_prims, "name": "Tree"}],
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
    out += struct.pack("<3I", 0x46546C67, 2, total)              # glTF header
    out += struct.pack("<2I", len(json_blob), 0x4E4F534A) + json_blob  # JSON chunk
    out += struct.pack("<2I", len(binary), 0x004E4942) + binary        # BIN chunk
    return bytes(out)


def main():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(pack_glb(build_primitives()))
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
