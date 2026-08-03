#!/usr/bin/env python3
"""Generates assets/levels/hardcorps2t.json from the traced layout.

A rebuild of the Infantry Zone map of that name in our geometry: the two bases
notched into the cliffs at either end, the trench mazes they fight out of, the
jungle and rock belt down the middle. The shapes come from
etc/hardcorps2t_layout.py, traced off the zone's own minimap; what this file
decides is how tall everything stands, what stops what, and how big a tile is.

Scale. One source tile becomes 0.35 units. That number is chosen off the
corridors rather than off the map: a trench runs about 14 tiles wide, which at
this scale is 4.9 units, or six soldiers abreast — close to what it looked like
to play, and the same width as the gate on our own hand-built map. It also puts
the whole map at 197 x 106, which is inside what the wire format can address
(positions quantize to an int16 at 1/128, so nothing may exceed 256 units from
the middle, and shots overrun the edge by up to 28 before gravity lands them).

What blocks what:

  Trench walls are chest high — over a muzzle, under a grenade's arc — and do
  not block sight. That is the map's own answer to a question our first pass
  got wrong: in Infantry you could see over a trench line but not shoot through
  it, and modelling them as sight-blockers would both wall the fog in and put
  140 more rectangles into a visibility sweep that is quadratic in them.

  Rock outcrops are tall and do block sight. They are the map's real cover, and
  what the middle is navigated around.

  The base slabs are floor, not structure: each sits in a notch in the cliff,
  open toward the field, so the rock around it is the wall.

Run from the repo root:  python etc/make_hardcorps2t.py
"""

import json
import math
import random
from pathlib import Path

import hardcorps2t_layout as src

OUT = Path(__file__).resolve().parent.parent / "assets" / "levels" / "hardcorps2t.json"

SEED = 815

# World units per traced cell. The layout is traced two source tiles to a cell,
# so this is twice the per-tile scale the docstring argues for.
UNITS = 0.70

WALL_H = 1.4    # trench line: over a 0.6 muzzle, well under a grenade's arc
ROCK_H = 3.6    # outcrop: over the eye line, so it hides what's behind it
TREE_SCALE = (0.85, 1.35)
PALM_SHARE = 0.6

# Boulder models dropped on the biggest outcrops. The rock itself is collision
# geometry drawn as a box; these break the silhouette so a ridge reads as rock
# rather than as masonry. Decoration only — no collider, so they cost a draw
# call and nothing else.
BOULDER_ON_TOP = 34


def build():
    rng = random.Random(SEED)
    px0, py0, pw, ph = src.PLAYABLE
    # The playable box, centred on the origin. Traced y runs south; our z runs
    # north, so it flips — otherwise the map would come out mirrored against
    # every screenshot of it.
    cx = px0 + pw / 2.0
    cy = py0 + ph / 2.0

    def to_world(x, y):
        return round((x - cx) * UNITS, 2), round(-(y - cy) * UNITS, 2)

    def rect_to_object(r, height, blocks_sight, model=None):
        x, y, w, h = r
        wx, wz = to_world(x + w / 2.0, y + h / 2.0)
        obj = {}
        if model:
            obj["model"] = model
        obj["pos"] = [wx, 0.0, wz]
        if model:
            obj["scale"] = 1.0
            obj["yaw"] = 0.0
        collider = {"size": [round(w * UNITS, 2), height, round(h * UNITS, 2)]}
        if not blocks_sight:
            collider["blocksSight"] = False
        obj["collider"] = collider
        return obj

    objects = []
    for r in src.WALLS:
        objects.append(rect_to_object(r, WALL_H, blocks_sight=False))
    for r in src.ROCKS:
        objects.append(rect_to_object(r, ROCK_H, blocks_sight=True))

    # Boulders on the largest outcrops, biggest first.
    for x, y, w, h in sorted(src.ROCKS, key=lambda r: r[2] * r[3], reverse=True)[:BOULDER_ON_TOP]:
        for _ in range(1 if w * h < 40 else 2):
            fx = x + rng.uniform(0.25, 0.75) * w
            fy = y + rng.uniform(0.25, 0.75) * h
            wx, wz = to_world(fx, fy)
            objects.append({
                "model": "assets/rock.glb", "pos": [wx, 0.0, wz],
                "scale": round(min(w, h) * UNITS * rng.uniform(0.55, 0.9), 2),
                "yaw": round(rng.uniform(0.0, math.tau), 2),
            })

    for x, y in src.TREES:
        wx, wz = to_world(x, y)
        palm = rng.random() < PALM_SHARE
        objects.append({
            "model": "assets/palm.glb" if palm else "assets/tree.glb",
            "pos": [wx, 0.0, wz],
            "scale": round(rng.uniform(*TREE_SCALE), 2),
            "yaw": round(rng.uniform(0.0, math.tau), 2),
            # A trunk stops a round and splits up a rush; it hides nobody.
            "collider": {"size": [0.6, 3.0, 0.6] if palm else [0.5, 2.5, 0.5],
                         "blocksSight": False},
        })

    spawns = []
    for team, (sx, sy) in enumerate(src.SPAWNS):
        wx, wz = to_world(sx, sy)
        spawns.append({"team": team, "pos": [wx, 0.0, wz]})

    return {
        "name": "hardcorps2t",
        "bounds": {"halfExtent": [round(pw / 2.0 * UNITS, 2), round(ph / 2.0 * UNITS, 2)]},
        "objects": objects,
        "spawns": spawns,
    }


def dump(level):
    """One object per line — a level file is meant to be opened and poked at."""
    def num(v):
        if not isinstance(v, float):
            return str(v)
        s = f"{v:.2f}"
        return s.rstrip("0").rstrip(".")

    def inline(value):
        if isinstance(value, dict):
            return "{ " + ", ".join(f'"{k}": {inline(v)}' for k, v in value.items()) + " }"
        if isinstance(value, list):
            return "[" + ", ".join(inline(v) for v in value) + "]"
        if isinstance(value, bool):
            return "true" if value else "false"
        if isinstance(value, str):
            return json.dumps(value)
        return num(value)

    lines = ["{",
             f'  "name": {json.dumps(level["name"])},',
             f'  "bounds": {inline(level["bounds"])},',
             '  "objects": [']
    lines.append(",\n".join(f"    {inline(o)}" for o in level["objects"]))
    lines.append("  ],")
    lines.append('  "spawns": [')
    lines.append(",\n".join(f"    {inline(s)}" for s in level["spawns"]))
    lines.append("  ]")
    lines.append("}")
    return "\n".join(lines) + "\n"


def main():
    level = build()
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(dump(level), encoding="utf-8")
    half = level["bounds"]["halfExtent"]
    colliders = sum(1 for o in level["objects"] if "collider" in o)
    sight = sum(1 for o in level["objects"]
                if "collider" in o and o["collider"].get("blocksSight", True))
    print(f"wrote {OUT}")
    print(f"  arena {half[0] * 2:.0f} x {half[1] * 2:.0f} units "
          f"(half-extents {half[0]} x {half[1]})")
    print(f"  {len(level['objects'])} objects, {colliders} colliders, "
          f"{sight} sight-blockers")
    print(f"  spawns {[s['pos'] for s in level['spawns']]}")


if __name__ == "__main__":
    main()
