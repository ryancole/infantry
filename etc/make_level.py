#!/usr/bin/env python3
"""Generates assets/levels/hardcore.json.

A jungle map after Infantry Zone's HardCorps: an unclimbable massif planted in
the middle of the arena, a fortified base at either end of it, and a canopy of
trees over everything else.

The three ideas it is built out of:

  The mountain.  Wide enough that the two bases cannot see each other and long
  enough that going around it is a commitment — pick the north lane or the south
  lane at the gate and you have spent the trip. It is a tepui, sheer on every
  face, because a level object collides as one axis-aligned box and a cliff off a
  rectangular base is the silhouette a box can honestly describe (see
  etc/make_mountain.py, which this imports its footprint from so the two can't
  drift).

  The trenches.  Each base is ringed by a chest-high embankment: taller than a
  muzzle and far under a grenade's arc, so rifle fire cannot touch anyone inside
  and the only way to reach them from outside is to lob something over the top or
  come through a gap. Three gaps each — the gate facing the field, and one at
  either shoulder onto the flanking lanes — so a base is a strongpoint rather
  than a trap.

  The jungle.  Trunks stop rounds and split up a rush, but they do not block
  sight (see the blocksSight note in src/Level.h): a jungle's worth of
  sight-blockers would be fog made of flickering slivers, and would cost the
  visibility sweep, which is quadratic in occluders, more than trees are worth.
  What shapes the fog on this map is the mountain, the trench lines, and the
  boulders in the lanes.

Run from the repo root:  python etc/make_level.py
"""

import json
import math
import random
from pathlib import Path

import make_mountain as mtn

OUT = Path(__file__).resolve().parent.parent / "assets" / "levels" / "hardcore.json"

SEED = 20260803

ARENA_HALF = 40.0

# --- Bases -------------------------------------------------------------------
# One at either end of the x axis, backed onto the arena wall so there's no
# useless alley behind them. Each is a rectangle of embankment around a spawn.
BASE_CX = 32.0        # distance from the middle to a base's centre
BASE_HALF_X = 8.0     # so a base spans 16 x 19 including its walls
BASE_HALF_Z = 9.5
WALL = 1.0            # embankment thickness
WALL_H = 1.4          # and height: over a muzzle (0.6), far under a lobbed grenade
GATE_HALF = 2.5       # the front opening, facing the field
SHOULDER_GAP = 3.0    # the side openings onto the north and south lanes

# --- Trees -------------------------------------------------------------------
TREE_CLUMPS = 22
TREES_PER_CLUMP = (2, 4)
CLUMP_RADIUS = 3.4
TREE_SPACING = 2.0    # nothing closer than this to another trunk
PALM_SHARE = 0.6      # the rest are conifers, for a canopy that isn't one note

# Clearances kept around everything a tree must not grow into.
EDGE_MARGIN = 1.6
MOUNTAIN_MARGIN = 2.0
BASE_MARGIN = 2.0
PROP_CLEARANCE = 3.4  # clears the widest boulder the lanes are seeded with


def rect(cx, cz, half_x, half_z):
    return (cx - half_x, cz - half_z, cx + half_x, cz + half_z)


def in_rect(x, z, r, margin=0.0):
    return (r[0] - margin <= x <= r[2] + margin and
            r[1] - margin <= z <= r[3] + margin)


def wall(cx, cz, size_x, size_z):
    """One run of embankment: no model, so the game draws the collider itself."""
    return {"pos": [cx, 0.0, cz], "collider": {"size": [size_x, WALL_H, size_z]}}


def base_walls(cx, facing):
    """The ring around one base. `facing` is +1 for a base whose gate opens
    toward +x, -1 for the one across from it."""
    front = cx + facing * (BASE_HALF_X - WALL * 0.5)
    rear = cx - facing * (BASE_HALF_X - WALL * 0.5)
    span_z = BASE_HALF_Z * 2.0
    side_z = BASE_HALF_Z - WALL * 0.5

    out = [wall(rear, 0.0, WALL, span_z)]

    # Front: split around the gate.
    seg = BASE_HALF_Z - GATE_HALF
    for sign in (1.0, -1.0):
        out.append(wall(front, sign * (GATE_HALF + seg * 0.5), WALL, seg))

    # Sides: split around a shoulder gap set toward the front, so a defender can
    # step straight out into a lane without crossing the gate everyone is
    # shooting at.
    gap_near = 2.0                        # gap's rear edge, measured from centre
    gap_far = gap_near + SHOULDER_GAP
    rear_len = BASE_HALF_X + gap_near
    front_len = BASE_HALF_X - gap_far
    for sign in (1.0, -1.0):
        out.append(wall(cx - facing * (BASE_HALF_X - rear_len * 0.5),
                        sign * side_z, rear_len, WALL))
        out.append(wall(cx + facing * (BASE_HALF_X - front_len * 0.5),
                        sign * side_z, front_len, WALL))
    return out


def rock(x, z, scale, yaw):
    return {"model": "assets/rock.glb", "pos": [x, 0.0, z], "scale": scale, "yaw": yaw,
            "collider": {"size": [mtn.ROCK_HALF * 2.0, mtn.ROCK_HEIGHT, mtn.ROCK_HALF * 2.0]}}


def crate(x, z, scale, yaw):
    return {"model": "assets/crate.glb", "pos": [x, 0.0, z], "scale": scale, "yaw": yaw,
            "collider": {"size": [1.0, 1.0, 1.0]}}


def tree(x, z, palm, scale, yaw):
    model = "assets/palm.glb" if palm else "assets/tree.glb"
    size = [0.6, 3.0, 0.6] if palm else [0.5, 2.5, 0.5]
    return {"model": model, "pos": [x, 0.0, z], "scale": scale, "yaw": yaw,
            # A trunk stops a round and splits up a rush; it hides nobody.
            "collider": {"size": size, "blocksSight": False}}


def build():
    rng = random.Random(SEED)
    objects = []

    # The massif, one object: the model and the box that stands in for it.
    objects.append({
        "model": "assets/mountain.glb", "pos": [0.0, 0.0, 0.0], "scale": 1.0, "yaw": 0.0,
        "collider": {"size": [mtn.HALF_X * 2.0, mtn.HEIGHT, mtn.HALF_Z * 2.0]},
    })

    for facing in (1.0, -1.0):
        objects.extend(base_walls(-BASE_CX * facing, facing))

    # Boulders: cover in the two lanes, and a pair off the mountain's tips where
    # the lanes turn. They are the only sight-blockers out in the open, so they
    # are what a crossing is timed around.
    lane_rocks = [(-14.0, 24.0), (-11.5, 27.5), (0.0, 31.0), (2.6, 29.0),
                  (14.0, 24.0), (11.0, 27.0), (-20.0, 30.5), (20.5, 29.0)]
    for x, z in lane_rocks:
        for sign in (1.0, -1.0):
            objects.append(rock(x, z * sign, rng.uniform(1.7, 2.6),
                                rng.uniform(0.0, math.tau)))
    for sign in (1.0, -1.0):
        objects.append(rock(0.0, sign * (mtn.HALF_Z + 3.0), 2.4, rng.uniform(0.0, math.tau)))

    # Crates: something to fight from behind just inside each gate, and a stack
    # out in each lane.
    for facing in (1.0, -1.0):
        cx = -BASE_CX * facing
        for dz, dx, scale in ((3.4, 3.2, 1.0), (4.4, 4.4, 0.85), (-3.6, 3.6, 1.1)):
            objects.append(crate(cx + facing * dx, dz, scale, rng.uniform(0.0, math.tau)))
    for x, z in ((-24.0, 22.0), (24.0, 23.0), (-6.0, 24.5), (7.0, 26.0)):
        for sign in (1.0, -1.0):
            objects.append(crate(x, z * sign, rng.uniform(0.85, 1.15),
                                 rng.uniform(0.0, math.tau)))

    # --- The canopy ---
    mountain_rect = rect(0.0, 0.0, mtn.HALF_X, mtn.HALF_Z)
    base_rects = [rect(-BASE_CX, 0.0, BASE_HALF_X, BASE_HALF_Z),
                  rect(BASE_CX, 0.0, BASE_HALF_X, BASE_HALF_Z)]
    props = [(o["pos"][0], o["pos"][2]) for o in objects if o.get("model")]
    trunks = []

    def clear(x, z):
        if abs(x) > ARENA_HALF - EDGE_MARGIN or abs(z) > ARENA_HALF - EDGE_MARGIN:
            return False
        if in_rect(x, z, mountain_rect, MOUNTAIN_MARGIN):
            return False
        if any(in_rect(x, z, r, BASE_MARGIN) for r in base_rects):
            return False
        if any(math.hypot(x - px, z - pz) < PROP_CLEARANCE for px, pz in props):
            return False
        return all(math.hypot(x - tx, z - tz) >= TREE_SPACING for tx, tz in trunks)

    for _ in range(TREE_CLUMPS):
        # A clump is a spot plus a handful of trunks around it. Trees grow in
        # company, and a clump is also the only thing on an open map that makes
        # one stretch of ground look different from another.
        for _attempt in range(40):
            cx = rng.uniform(-ARENA_HALF, ARENA_HALF)
            cz = rng.uniform(-ARENA_HALF, ARENA_HALF)
            if clear(cx, cz):
                break
        else:
            continue
        for _ in range(rng.randint(*TREES_PER_CLUMP)):
            for _attempt in range(24):
                a = rng.uniform(0.0, math.tau)
                r = CLUMP_RADIUS * math.sqrt(rng.random())
                x, z = cx + math.cos(a) * r, cz + math.sin(a) * r
                if clear(x, z):
                    trunks.append((x, z))
                    objects.append(tree(x, z, rng.random() < PALM_SHARE,
                                        rng.uniform(0.85, 1.35),
                                        rng.uniform(0.0, math.tau)))
                    break

    return {
        "name": "hardcore",
        "bounds": {"halfExtent": ARENA_HALF},
        "objects": objects,
        "spawns": [{"team": 0, "pos": [-BASE_CX, 0.0, 0.0]},
                   {"team": 1, "pos": [BASE_CX, 0.0, 0.0]}],
    }


def dump(level):
    """One object per line, the way the hand-authored levels are written — a
    level file is meant to be read and poked at, and a pretty-printer that puts
    every number on its own line makes that impossible."""
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
    body = [f"    {inline(o)}" for o in level["objects"]]
    lines.append(",\n".join(body))
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
    trees = sum(1 for o in level["objects"] if "tree" in o.get("model", "") or
                "palm" in o.get("model", ""))
    sight = sum(1 for o in level["objects"]
                if "collider" in o and o["collider"].get("blocksSight", True))
    print(f"wrote {OUT}: {len(level['objects'])} objects "
          f"({trees} trees, {sight} sight-blockers)")


if __name__ == "__main__":
    main()
