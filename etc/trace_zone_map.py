"""Traces an Infantry Zone map's minimap into a layout table we can build from.

Those maps ship a .png alongside the .lvl at exactly one pixel per tile, which
makes the minimap a readable description of the terrain: classify it by colour
and the parts our engine can represent fall out as axis-aligned rectangles —
trench walls, rock outcrops, base slabs — plus points for the jungle.

Run by hand, rarely. Its output is committed (etc/hardcorps2t_layout.py); the
source image is not, since it isn't ours to redistribute. To re-derive:

    curl -O https://raw.githubusercontent.com/InfantryOnline/Zone-Assets/master/\\
Complete%20Zones/hardcorps2t/hardcorps2t.png
    python etc/trace_zone_map.py hardcorps2t.png etc/hardcorps2t_layout.py 2

Needs Pillow. The trailing argument is how many source tiles go into one traced
cell; see the note on CELL below for why it isn't 1.
"""
import sys
from collections import Counter, deque
from pathlib import Path

from PIL import Image

SRC = Path(sys.argv[1] if len(sys.argv) > 1 else "hardcorps2t.png")
OUT = Path(sys.argv[2] if len(sys.argv) > 2 else "hardcorps2t_layout.py")

im = Image.open(SRC).convert("RGB")
W, H = im.size
px = im.load()

GRASS, FLOOR, STONE, PAD, BRUSH, OUTSIDE = range(6)

# Per-pixel ink. Nothing on this map is a flat fill: an outcrop is rocky texture
# scattered over the same grass as everywhere else, and only about a third of
# its pixels are actually stone. So these are votes, counted below, rather than
# an answer on their own.
def ink(r, g, b):
    if r == g == b == 0:                        # letterbox above and below
        return OUTSIDE
    if g > r + 10 and g > b + 10:               # green: jungle floor or foliage
        return BRUSH if g > 70 else GRASS
    if b >= r - 4 and max(r, g, b) < 70:        # dark bluish slab: base structure
        return PAD
    if r - b > 20 and max(r, g, b) > 55:        # yellow-tan: dug trench floor
        return FLOOR
    if max(r, g, b) > 28:                       # warm neutral, down to its shading
        return STONE
    return GRASS


pixels = [[ink(*px[x, y]) for x in range(W)] for y in range(H)]

# Trace on a coarser grid than the source. Every collider is scanned linearly by
# soldier push-out and by the aim indicator's arc march, so rectangle count is a
# frame-time cost, and a staircase drawn at tile resolution spends hundreds of
# them on detail nobody can walk through anyway. At CELL=2 a corridor is still
# seven cells across.
CELL = int(sys.argv[3]) if len(sys.argv) > 3 else 2
PAD_PX = 2  # extra pixels each way when measuring how dense a texture is

CW, CH = W // CELL, H // CELL
grid = [[GRASS] * CW for _ in range(CH)]
for cy in range(CH):
    for cx in range(CW):
        # Two different questions, two different windows. A trench edge is a
        # hard line and is read off the cell itself, or corridors come out
        # blurred; a texture is a density and needs a wider window than four
        # pixels to be measurable at all.
        own = Counter(pixels[y][x]
                      for y in range(cy * CELL, (cy + 1) * CELL)
                      for x in range(cx * CELL, (cx + 1) * CELL))
        wide = Counter(pixels[y][x]
                       for y in range(max(cy * CELL - PAD_PX, 0),
                                      min((cy + 1) * CELL + PAD_PX, H))
                       for x in range(max(cx * CELL - PAD_PX, 0),
                                      min((cx + 1) * CELL + PAD_PX, W)))
        n = sum(wide.values())
        if wide[OUTSIDE] / n > 0.5:
            grid[cy][cx] = OUTSIDE
        elif wide[PAD] / n > 0.5:
            grid[cy][cx] = PAD
        elif own[FLOOR] / max(sum(own.values()), 1) >= 0.5:
            grid[cy][cx] = FLOOR
        elif wide[STONE] / n > 0.30:
            grid[cy][cx] = STONE
        elif wide[BRUSH] / n > 0.25:
            grid[cy][cx] = BRUSH
W, H = CW, CH
print(f"  traced at {CELL} tiles per cell -> {W} x {H} cells")


def counts():
    c = Counter(v for row in grid for v in row)
    names = {GRASS: "grass", FLOOR: "trench floor", STONE: "stone", PAD: "pad",
             BRUSH: "brush", OUTSIDE: "outside"}
    return ", ".join(f"{names[k]} {v}" for k, v in sorted(c.items()))


print(f"{SRC.name}: {W} x {H} tiles")
print(f"  {counts()}")

# --- Playable box -------------------------------------------------------------
# The map's stone frame is not a separate thing from its rock: the outcrops in
# the middle chain into the edges, so a flood fill from the border swallows the
# whole map. The frame is therefore defined by where the walkable ground stops,
# and every piece of stone inside that box is an outcrop.
border = [[False] * W for _ in range(H)]


# A column counts as inside the map when enough of it is ground somebody could
# stand on. A bare "any tile that isn't border" test doesn't work: the stone
# frame has grass tufts drawn into it, so every column has a few.
def live(x, y):
    return grid[y][x] in (GRASS, FLOOR, BRUSH, PAD) and not border[y][x]


col = [sum(live(x, y) for y in range(H)) for x in range(W)]
row = [sum(live(x, y) for x in range(W)) for y in range(H)]


def span(a, limit):
    lo = next(i for i, v in enumerate(a) if v > limit)
    hi = len(a) - 1 - next(i for i, v in enumerate(reversed(a)) if v > limit)
    return lo, hi


PX0, PX1 = span(col, H * 0.25)
PY0, PY1 = span(row, W * 0.25)
print(f"  playable {PX1 - PX0 + 1} x {PY1 - PY0 + 1} tiles at ({PX0},{PY0})")


def mask(pred):
    return [[pred(x, y) for x in range(W)] for y in range(H)]


def rectangles(m, min_area=1):
    """Greedy maximal-rectangle cover of a boolean mask."""
    m = [row[:] for row in m]
    out = []
    for y in range(H):
        for x in range(W):
            if not m[y][x]:
                continue
            w = 0
            while x + w < W and m[y][x + w]:
                w += 1
            h = 1
            while y + h < H and all(m[y + h][x + i] for i in range(w)):
                h += 1
            for yy in range(y, y + h):
                for xx in range(x, x + w):
                    m[yy][xx] = False
            if w * h >= min_area:
                out.append((x, y, w, h))
    return out


# --- Trench walls: the rim around the dug floor -------------------------------
# The minimap draws the corridor, not its sides; the wall is the boundary the
# corridor is cut into, so it is the floor grown by a tile with the floor itself
# taken back out.
floor = mask(lambda x, y: grid[y][x] == FLOOR)
grown = [[False] * W for _ in range(H)]
for y in range(H):
    for x in range(W):
        if not floor[y][x]:
            continue
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                nx, ny = x + dx, y + dy
                if 0 <= nx < W and 0 <= ny < H:
                    grown[ny][nx] = True
wall = mask(lambda x, y: grown[y][x] and not floor[y][x] and grid[y][x] != PAD
            and not border[y][x] and PX0 <= x <= PX1 and PY0 <= y <= PY1)

inner_stone = mask(lambda x, y: grid[y][x] == STONE and not border[y][x]
                   and PX0 <= x <= PX1 and PY0 <= y <= PY1)
pads = mask(lambda x, y: grid[y][x] == PAD and PX0 <= x <= PX1 and PY0 <= y <= PY1)

walls = rectangles(wall, min_area=6)
rocks = rectangles(inner_stone, min_area=12)
pad_rects = rectangles(pads, min_area=60)
print(f"  walls {len(walls)} rects, rocks {len(rocks)} rects, pads {len(pad_rects)} rects")

# --- Trees: thin the brush down to something a renderer can carry -------------
STEP = 4
trees = []
for cy in range(PY0, PY1, STEP):
    for cx in range(PX0, PX1, STEP):
        cells = [(x, y) for y in range(cy, min(cy + STEP, PY1))
                 for x in range(cx, min(cx + STEP, PX1)) if grid[y][x] == BRUSH]
        if len(cells) >= STEP * STEP * 0.30:
            sx = sum(c[0] for c in cells) / len(cells)
            sy = sum(c[1] for c in cells) / len(cells)
            trees.append((round(sx, 1), round(sy, 1)))
print(f"  trees {len(trees)}")

# --- Spawns: the middle of each base slab -------------------------------------
# The slabs are the two big blocks of PAD; everything else classed that way is
# shadow. Take connected components, keep the substantial ones, and pick the
# westmost and eastmost — which is what the two sides are.
seen = [[False] * W for _ in range(H)]
blobs = []
for y in range(PY0, PY1 + 1):
    for x in range(PX0, PX1 + 1):
        if seen[y][x] or grid[y][x] != PAD:
            continue
        cells, q = [], deque([(x, y)])
        seen[y][x] = True
        while q:
            cx, cy = q.popleft()
            cells.append((cx, cy))
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = cx + dx, cy + dy
                if (PX0 <= nx <= PX1 and PY0 <= ny <= PY1 and not seen[ny][nx]
                        and grid[ny][nx] == PAD):
                    seen[ny][nx] = True
                    q.append((nx, ny))
        if len(cells) >= 400:
            blobs.append(cells)
blobs.sort(key=lambda c: sum(p[0] for p in c) / len(c))
print(f"  base slabs: {[len(b) for b in blobs]} tiles")
spawns = [(round(sum(p[0] for p in b) / len(b), 1), round(sum(p[1] for p in b) / len(b), 1))
          for b in (blobs[0], blobs[-1])]
print(f"  spawns {spawns}")


def fmt(rects):
    return "\n".join(f"    ({x}, {y}, {w}, {h})," for x, y, w, h in rects)


OUT.write_text(f'''"""Traced layout of the Infantry Zone map hardcorps2t.

Written by etc/trace_zone_map.py from that zone's published minimap — see the
header there for the source and how to re-derive this file. Coordinates are in
traced cells of {CELL} source tiles each, with y running south the way the image
does; etc/make_hardcorps2t.py places the playable box in the world and scales it.

Rectangles are (x, y, width, height).
"""

# The playable region, inside the map's stone border.
PLAYABLE = ({PX0}, {PY0}, {PX1 - PX0 + 1}, {PY1 - PY0 + 1})

# Where the two sides come from: the middle of each base slab.
SPAWNS = [{", ".join(f"({x:.1f}, {y:.1f})" for x, y in spawns)}]

# The rim around every dug corridor: chest-high, and what makes a trench a
# trench. Not sight-blocking - see make_level.py.
WALLS = [
{fmt(walls)}
]

# Rock outcrops standing in the open. These are the map's real cover: tall
# enough to break line of sight, and what the middle is navigated around.
ROCKS = [
{fmt(rocks)}
]

# The two base slabs.
PADS = [
{fmt(pad_rects)}
]

# Jungle, thinned to one trunk per patch.
TREES = [
{chr(10).join(f"    ({x}, {y})," for x, y in trees)}
]
''', encoding="utf-8")
print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")
