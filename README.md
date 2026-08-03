# Infantry

A 3D isometric multiplayer shooter inspired by the classic *Infantry* (Infantry Zone / [Free Infantry](https://freeinfantry.com/)), built from scratch in C++ on Direct3D 12.

## Current state

Prototype scaffold with a working game loop:

- D3D12 renderer built on [DirectXTK12](https://github.com/microsoft/DirectXTK12) (device/swap chain/depth plumbing is ours; effects, pipeline state, and dynamic geometry go through the toolkit's `BasicEffect`/`PrimitiveBatch`/`GraphicsMemory`)
- glTF 2.0 model loading via [cgltf](https://github.com/jkuhlmann/cgltf) — flat-shaded static meshes with per-material colors (see the trees in the arena)
- Fixed-angle isometric camera (orthographic, smoothed follow, mouse-wheel zoom)
- **hardcorps2t**, the ground every match is fought on: the Infantry Zone map of that name, rebuilt in our geometry rather than imagined from a description of it. Those zones publish a minimap at exactly one pixel per tile, which makes the image a readable account of the terrain, so the map is traced (`etc/trace_zone_map.py`) instead of guessed — the trench mazes, the outcrop belt, and the two bases are the shapes it actually had. It is 197 × 106 units and reads as a letterbox rather than a square, because that is what the map is: a base notched into the cliff at either end and everything between them a decision about which way across. One source tile is 0.35 units, and that number comes off the corridors rather than off the map — a trench runs about fourteen tiles wide, which lands at 4.9 units, six soldiers abreast, close to what it looked like to play
- What stops what, on that map: a trench line is chest high, over a muzzle and under a grenade's arc, so rifle fire cannot cross one and a lobbed grenade can — but you can see over it, which is how Infantry played and is also the difference between 125 sight-blockers and 265. Rock outcrops are tall and do block sight; they are the map's real cover and what the middle is navigated around. Trunks stop a round that happens to hit one and hide nobody. The base slabs are floor rather than structure — each sits in a notch in the cliff, open toward the field, so the rock around it is the wall
- Arenas are rectangles. A level's bounds are half-extents on x and z, and `"halfExtent": 32` still means a 64-unit square, because a map with two bases facing each other wants to be long the way they face and no deeper than going around is worth
- Player movement, mouse aim, and projectile firing
- A match is fifteen minutes long and won by the side that has killed more when the clock runs out. The kill goes to whoever last put damage on the body, so the score is a reading of who is winning rather than of who is standing — a side can be wiped out repeatedly and still be ahead. The clock and both scores live in the corner panel next to the strength counts, because "who's winning, by how much, and how long have I got" is one glance. On the whistle the arena freezes where it stands: nothing further is decided, the rounds still in the air are swept, and the result stands for fifteen seconds before a fresh match starts on the same ground — a draw is a real outcome, not something broken by a tiebreak nobody saw
- Every soldier's kills and deaths are counted, and holding TAB puts the whole board up: both sides, five rows each, sorted by kills and then by deaths, with your own row lifted out of the list and the people told from the bots by color. The tally hangs on a *slot* rather than on a unit — a unit is one life and is swept off the roster when it ends, so a slot is a place on a side for the length of the match that a succession of soldiers stand in, and it's what a record can outlive its holder on. A side's score is only ever the sum of its slots' kills, so the corner panel and the board can't disagree. It arrives unfiltered over the wire like the standing counts do: the fog hides bodies, not the board. A player who leaves keeps their row, greyed out, and the side keeps their kills — the soldier the side is owed goes to a fresh slot instead, so the AI filling in starts from nothing rather than inheriting a stranger's tally, and quitting can never take points off the board
- Five a side: picking a class starts a match rather than dropping you into an empty arena. Both squads come up to strength at their own spawn, and a slot that empties is refilled after the same wait the player serves, so the fight stays five against five. Everyone but you is driven by the AI today — the roster is one list of units, and who drives each (a brain, a command off the wire) is a fact about the soldier rather than a difference in kind. A soldier with nothing in sight moves up rather than wandering: it still picks somewhere at random, but only somewhere standing closer to the enemy's ground than it does now, so a squad crosses the map instead of milling about its own half. That is not a route and it has never heard of a wall — what it means in practice is that a mountain in the way gets gone around, because going around it is what the points on the far side of it require
- The simulation is severed from the machine it's watched on: a `World` that steps on a fixed 60 Hz tick, hears input only as a `Command`, and reports what happened as events for the presentation to spend on blood, sound, and ragdolls. The renderer draws the blend between ticks, so a fast display sees motion rather than sixty stills
- True multiplayer over that seam: a `Server` hosts the match with AI in every unclaimed slot, and clients join over ENet — numbered commands up, snapshots and events down, sixty a second. A joining player displaces an AI soldier from the emptier side; a leaver's slot goes back to the AI on the same reinforcement clock a death starts. `infantry_server.exe` is that class with a console loop around it and no D3D on its link line
- There is no offline mode, because there is no second way to run a match. DEPLOY starts a `Server` inside the game's own process — bound to loopback on a port the OS picks, so it's listed nowhere and reachable from nothing — and joins it as a client over that socket. Playing on your own is a one-player match on a real server: the fog is server-enforced, your soldier is predicted and reconciled, the snapshots are quantized and the bytes are the same bytes. What it costs is that the trigger is answered on the server's next tick, which is a tick away when the server is an inch away. What it buys is that the code that decides a match has one reader and one set of bugs, and a fix to either lands in both places at once
- Client-side prediction with server reconciliation: your own soldier answers movement and aim on the frame you press, simulated locally by the same `MoveCommand` the server runs, then squared against every snapshot — acked commands retired, in-flight ones replayed on top of the server's answer, so when nothing contradicted you the correction is exactly zero. Firing stays server-authoritative; everyone else stays snapshot-interpolated
- The visibility sweep is binned by angle: a rectangle seen from a point covers one arc and can only be hit from inside it, so the walls are filed into bins around the viewer and a ray is only tested against its own bin. Nothing is approximated — the polygon is the same one the exhaustive version produced, checked against it — but the sweep stopped being quadratic in occluders, which is what a real map needs. On hardcorps2t it is 23× fewer segment tests, and the map now costs less to fog than the blockout arena did before any of this
- The fog of war is enforced by the server, not the renderer: each client's snapshot is filtered through the same `Visibility` test the fog is drawn with, from their own soldier's eye (or the spot they died), so an enemy behind a wall is absent from the bytes and no packet sniffer can find them. Events travel by earshot instead — gunfire behind cover is still a thing you hear — and the corner scoreboard arrives unfiltered, because a scoreboard is meant to know
- The wire is built for roads worse than a LAN: every command packet carries its two predecessors, so a lost packet can't eat a grenade press; snapshots are quantized (a position is two bytes an axis, an angle two bytes, health one) to about half their float weight with no statefulness to resync; the protocol carries a version and a server refuses a mismatched build or a full house at the door; and losing the server — or pressing Escape in somebody else's match — lands on the main menu with the world swept clean, not on the desktop. Delta compression is deliberately absent: full snapshots self-heal from any loss, and ten soldiers don't weigh enough to trade that away
- Servers are found, not typed: JOIN on the main menu shouts a UDP broadcast at the LAN once a second and lists every server that answers — machine name, address, how full — refreshing live and forgetting the ones that go quiet. One typed row covers servers a broadcast can't reach. An internet server browser is the piece that still needs a landlord (somewhere to host the list), and it's the one piece of the netcode left
- A soldier turns rather than pivots: the facing chases the cursor at a fixed rate instead of snapping to it, so getting behind someone is worth the trip. Holding steady drops both the walk and the turn to a fraction of normal — the gun sits where it's put, at the price of being able to leave
- Per-class primary weapons, each magazine-fed: the automatics get a few seconds of fire before they have to stop and reload, while the sniper and grenadier reload after every shot, so the wait *is* their rate of fire
- One grenade per life for every class: bounces off the world under Jolt, detonates on its fuse (or on a direct hit), with blast damage that cover blocks
- A melee swing for every class, on three charges and a recovery of its own rather than the magazine's: barely more reach than the two bodies involved and five swings to kill, so it's what's left when a primary can't answer — caught mid-reload, or with someone already inside its range
- Ragdoll corpses: the soldier model is built from eleven jointed segments, so a killed soldier is handed to Jolt as a ragdoll that keeps the pose it died in, is thrown by the blow that killed it, and falls where it stood
- Blood: a hit sprays drops along the blow, and each one stains the floor where it lands. The stains don't fade, so the ground ends up reading back where the fighting has been

Dependencies are fetched and built automatically by CMake (`FetchContent`) on first configure.

## Controls

| Input | Action |
| --- | --- |
| `W` `A` `S` `D` | Move (screen-relative) |
| Mouse | Aim |
| Left mouse / `Space` | Fire |
| `R` | Reload (automatic when the magazine empties) |
| `F` | Throw grenade (every class, one per life) |
| `V` | Melee swing (every class, three charges then a recovery) |
| `Shift` | Hold steady (slow walk, much slower turn) |
| Mouse wheel | Zoom |
| `Esc` | Leave the match (back to the main menu; QUIT there closes the game) |

OPTIONS on the main menu holds the settings screens: KEY BINDS puts any of the
above on another key or mouse button, and AUDIO decides whether the game keeps
making noise while its window is behind something else (off by default). Both
save to `%LOCALAPPDATA%\Infantry\` — `bindings.toml` and `settings.toml` — and
both files are safe to edit by hand.

## Requirements

- Windows 10/11 with a D3D12-capable GPU
- Visual Studio 2022+ with the C++ workload (MSVC)
- CMake 3.24+

## Build & run

```powershell
.\etc\run.ps1              # build (Debug) and launch
.\etc\build.ps1 -Config Release
```

The dedicated server builds alongside the game:

```powershell
.\build\Debug\infantry_server.exe     # host on port 27650 until stopped
.\build\Debug\infantry_server.exe 60  # run a 60-second AI match and report
.\build\Debug\infantry_server.exe 60 assets/levels/arena01.json   # ...on another level
```

A client draws whatever level its own copy loads, so the level argument is for
smoke tests and for a host who knows what their players are running.

Joining a server: JOIN on the main menu lists every server on the LAN and
takes a typed address for the rest. DEPLOY hosts a match on this machine and
joins that instead — same road, shorter. The command line remains as the fast
lane while iterating on netcode:

```powershell
.\build\Debug\infantry.exe --connect 192.168.1.10
.\build\Debug\infantry.exe --connect 127.0.0.1 --class marine
.\build\Debug\infantry.exe --class marine   # host here and take the field, no menus
```

CMake generates a Visual Studio solution in `build/` — open `build\infantry.sln` to work in the IDE.

## Layout

```
src/            C++ sources
assets/         Runtime assets (.glb models, levels, sounds; copied next to the exe post-build)
etc/            build/run scripts, asset and level generators (make_tree.py, make_level.py)
build/          CMake output (generated, not committed)
```

The models and the maps are generated rather than committed by hand:

| Script | Writes |
| --- | --- |
| `make_tree.py`, `make_palm.py`, `make_crate.py` | `assets/tree.glb`, `palm.glb`, `crate.glb` |
| `make_mountain.py` | `assets/mountain.glb`, `rock.glb` |
| `make_level.py` | `assets/levels/hardcore.json` — a hand-designed jungle map, laid out in code |
| `trace_zone_map.py` | `etc/hardcorps2t_layout.py` — geometry traced off an Infantry Zone minimap |
| `make_hardcorps2t.py` | `assets/levels/hardcorps2t.json` — the shipped map, built from that trace |

Every one of them reproduces byte-identical output. `trace_zone_map.py` is the
odd one out: it needs the zone's own minimap, which isn't committed because it
isn't ours to redistribute — its header says where to fetch it, and its output
is checked in so nobody else has to. Real assets can be authored in Blender and
exported as glTF (.glb); a level file is plain JSON and is meant to be opened
and poked at.

## Roadmap ideas

- Entity/component structure for players, vehicles, and weapons
- Collision (projectile vs. obstacle/player, player vs. obstacle)
- Sprite/billboard rendering pass for Infantry-style readability, HUD & radar (DirectXTK12 `SpriteBatch`/`SpriteFont`)
- Replace placeholder cubes with glTF models (player, bunkers, bases)
- Tile-based map format + loader (Infantry-style zones)
- Netcode, the rest of it: predicted muzzle effects so your own shot sounds on the press, and an internet server browser once there's somewhere to host the list
- Audio (DirectXTK12 audio module — re-enable `BUILD_XAUDIO_WIN10`), effects, game modes past the kill count (CTF, ticket bleed, objectives)
