#!/usr/bin/env python3
"""Generates assets/crate.glb: a unit wooden crate with baked textures.

One cube (24 verts, per-face UVs mapping the full texture) plus two
procedurally generated PNGs embedded in the GLB: a plank base-color texture
and a normal map derived from the same height field (raised frame, grooves
between planks). Exercises the renderer's textured NormalMapEffect path.
Run from the repo root:  python etc/make_crate.py
"""

import json
import math
import struct
import zlib
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "assets" / "crate.glb"

TEX = 128          # texture size in pixels
FRAME = 18         # crate frame border width
BEVEL = 5          # bevel ramp width on frame and groove edges
PLANK = 24         # interior plank height
GROOVE = 3         # gap between planks
WOOD = (0.55, 0.38, 0.20)
FRAME_WOOD = (0.42, 0.28, 0.15)


def fract_noise(x, y, seed=0.0):
    """Deterministic hash noise in [0, 1)."""
    v = math.sin(x * 12.9898 + y * 78.233 + seed * 37.719) * 43758.5453
    return v - math.floor(v)


def in_frame(x, y):
    return min(x, y, TEX - 1 - x, TEX - 1 - y) < FRAME


def height_at(x, y):
    """Height field in [0, 1]: raised frame, planks split by grooves."""
    edge = min(x, y, TEX - 1 - x, TEX - 1 - y)
    if edge < FRAME:
        # Frame plank, beveled down toward the outer edge and the interior.
        h = 1.0
        if edge < BEVEL:
            h = 0.55 + 0.45 * edge / BEVEL
        elif edge > FRAME - BEVEL:
            h = 1.0 - 0.25 * (edge - (FRAME - BEVEL)) / BEVEL
        return h
    # Interior planks: dip into grooves between them.
    local = (y - FRAME) % (PLANK + GROOVE)
    if local >= PLANK:
        return 0.30
    h = 0.75
    if local < BEVEL:
        h = 0.35 + 0.40 * local / BEVEL
    elif local > PLANK - BEVEL:
        h = 0.75 - 0.40 * (1.0 - (PLANK - local) / BEVEL)
    return h + 0.03 * fract_noise(x, y, 5.0)  # wood grain roughness


def base_color():
    rows = []
    for y in range(TEX):
        row = bytearray()
        for x in range(TEX):
            if in_frame(x, y):
                r, g, b = FRAME_WOOD
                plank_id = 100 + min(x, y, TEX - 1 - x, TEX - 1 - y) // FRAME
                grain = fract_noise((x + y) * 0.5, 0.0, 2.0)
            else:
                r, g, b = WOOD
                plank_id = (y - FRAME) // (PLANK + GROOVE)
                if (y - FRAME) % (PLANK + GROOVE) >= PLANK:
                    r, g, b = (c * 0.35 for c in WOOD)  # groove shadow
                grain = fract_noise(x * 0.9, plank_id * 7.0, 1.0)
            tint = 0.85 + 0.25 * fract_noise(plank_id * 13.0, 0.0, 3.0)
            streak = 0.92 + 0.16 * grain
            px = (r * tint * streak, g * tint * streak, b * tint * streak)
            row += bytes(max(0, min(255, round(c * 255))) for c in px) + b"\xff"
        rows.append(bytes(row))
    return rows


def normal_map():
    strength = 3.0
    rows = []
    for y in range(TEX):
        row = bytearray()
        for x in range(TEX):
            dx = (height_at(min(x + 1, TEX - 1), y) - height_at(max(x - 1, 0), y)) * strength
            dy = (height_at(x, min(y + 1, TEX - 1)) - height_at(x, max(y - 1, 0))) * strength
            inv = 1.0 / math.sqrt(dx * dx + dy * dy + 1.0)
            n = (-dx * inv, -dy * inv, inv)
            row += bytes(max(0, min(255, round((c * 0.5 + 0.5) * 255))) for c in n) + b"\xff"
        rows.append(bytes(row))
    return rows


def pack_png(rows):
    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", zlib.crc32(kind + payload)))

    ihdr = struct.pack(">2I5B", TEX, TEX, 8, 6, 0, 0, 0)  # 8-bit RGBA
    idat = zlib.compress(b"".join(b"\x00" + r for r in rows), 9)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", idat) + chunk(b"IEND", b""))


def build_cube():
    """Unit cube: x/z in [-0.5, 0.5], y in [0, 1]; full texture per face."""
    faces = [
        ((1, 0, 0), [(0.5, 0, -0.5), (0.5, 1, -0.5), (0.5, 1, 0.5), (0.5, 0, 0.5)]),
        ((-1, 0, 0), [(-0.5, 0, 0.5), (-0.5, 1, 0.5), (-0.5, 1, -0.5), (-0.5, 0, -0.5)]),
        ((0, 1, 0), [(-0.5, 1, -0.5), (-0.5, 1, 0.5), (0.5, 1, 0.5), (0.5, 1, -0.5)]),
        ((0, -1, 0), [(-0.5, 0, 0.5), (-0.5, 0, -0.5), (0.5, 0, -0.5), (0.5, 0, 0.5)]),
        ((0, 0, 1), [(0.5, 0, 0.5), (0.5, 1, 0.5), (-0.5, 1, 0.5), (-0.5, 0, 0.5)]),
        ((0, 0, -1), [(-0.5, 0, -0.5), (-0.5, 1, -0.5), (0.5, 1, -0.5), (0.5, 0, -0.5)]),
    ]
    pos, norm, uv, idx = [], [], [], []
    for n, corners in faces:
        base = len(pos)
        pos += corners
        norm += [n] * 4
        uv += [(0, 1), (0, 0), (1, 0), (1, 1)]
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    return pos, norm, uv, idx


def pack_glb():
    pos, norm, uv, idx = build_cube()
    binary = bytearray()
    buffer_views = []

    def add_view(blob, target=None):
        view = {"buffer": 0, "byteOffset": len(binary), "byteLength": len(blob)}
        if target:
            view["target"] = target
        buffer_views.append(view)
        binary.extend(blob)
        while len(binary) % 4:
            binary.append(0)
        return len(buffer_views) - 1

    pos_view = add_view(b"".join(struct.pack("<3f", *p) for p in pos), 34962)
    norm_view = add_view(b"".join(struct.pack("<3f", *n) for n in norm), 34962)
    uv_view = add_view(b"".join(struct.pack("<2f", *t) for t in uv), 34962)
    idx_view = add_view(b"".join(struct.pack("<H", i) for i in idx), 34963)
    color_view = add_view(pack_png(base_color()))
    normal_view = add_view(pack_png(normal_map()))

    gltf = {
        "asset": {"version": "2.0", "generator": "infantry etc/make_crate.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "Crate"}],
        "meshes": [{"name": "Crate", "primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
            "indices": 3, "material": 0, "mode": 4,
        }]}],
        "materials": [{
            "name": "CrateWood",
            "pbrMetallicRoughness": {
                "baseColorTexture": {"index": 0},
                "metallicFactor": 0.0, "roughnessFactor": 1.0,
            },
            "normalTexture": {"index": 1},
        }],
        "textures": [{"source": 0, "sampler": 0}, {"source": 1, "sampler": 0}],
        "images": [{"bufferView": color_view, "mimeType": "image/png"},
                   {"bufferView": normal_view, "mimeType": "image/png"}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987,
                      "wrapS": 10497, "wrapT": 10497}],
        "accessors": [
            {"bufferView": pos_view, "componentType": 5126, "count": len(pos),
             "type": "VEC3",
             "min": [min(p[i] for p in pos) for i in range(3)],
             "max": [max(p[i] for p in pos) for i in range(3)]},
            {"bufferView": norm_view, "componentType": 5126, "count": len(norm),
             "type": "VEC3"},
            {"bufferView": uv_view, "componentType": 5126, "count": len(uv),
             "type": "VEC2"},
            {"bufferView": idx_view, "componentType": 5123, "count": len(idx),
             "type": "SCALAR"},
        ],
        "bufferViews": buffer_views,
        "buffers": [{"byteLength": len(binary)}],
    }

    json_blob = json.dumps(gltf, separators=(",", ":")).encode()
    while len(json_blob) % 4:
        json_blob += b" "

    total = 12 + 8 + len(json_blob) + 8 + len(binary)
    out = bytearray()
    out += struct.pack("<3I", 0x46546C67, 2, total)                    # glTF header
    out += struct.pack("<2I", len(json_blob), 0x4E4F534A) + json_blob  # JSON chunk
    out += struct.pack("<2I", len(binary), 0x004E4942) + binary        # BIN chunk
    return bytes(out)


def main():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(pack_glb())
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
