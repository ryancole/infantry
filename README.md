# Infantry

A 3D isometric multiplayer shooter inspired by the classic *Infantry* (Infantry Zone / [Free Infantry](https://freeinfantry.com/)), built from scratch in C++ on Direct3D 12.

## Current state

Prototype scaffold with a working game loop:

- D3D12 renderer built on [DirectXTK12](https://github.com/microsoft/DirectXTK12) (device/swap chain/depth plumbing is ours; effects, pipeline state, and dynamic geometry go through the toolkit's `BasicEffect`/`PrimitiveBatch`/`GraphicsMemory`)
- glTF 2.0 model loading via [cgltf](https://github.com/jkuhlmann/cgltf) — flat-shaded static meshes with per-material colors (see the trees in the arena)
- Fixed-angle isometric camera (orthographic, smoothed follow, mouse-wheel zoom)
- Grid arena with obstacle bunkers and scattered trees
- Player movement, mouse aim, and projectile firing
- Five a side: picking a class starts a match rather than dropping you into an empty arena. Both squads come up to strength at their own spawn, and a slot that empties is refilled after the same wait the player serves, so the fight stays five against five. Everyone but you is driven by the AI today — the roster is one list of units, and who drives each (a brain, this machine's input) is a fact about the soldier rather than a difference in kind
- The simulation is severed from the machine it's watched on: a `World` that steps on a fixed 60 Hz tick, hears input only as a `Command`, and reports what happened as events for the presentation to spend on blood, sound, and ragdolls. The renderer draws the blend between ticks, so a fast display sees motion rather than sixty stills
- True multiplayer over that seam: `infantry_server.exe` hosts the match headless (no D3D on its link line) with AI in every unclaimed slot, and clients join over ENet — numbered commands up, snapshots and events down, sixty a second. A joining player displaces an AI soldier from the emptier side; a leaver's slot goes back to the AI on the same reinforcement clock a death starts
- Client-side prediction with server reconciliation: your own soldier answers movement and aim on the frame you press, simulated locally by the same `MoveCommand` the server runs, then squared against every snapshot — acked commands retired, in-flight ones replayed on top of the server's answer, so when nothing contradicted you the correction is exactly zero. Firing stays server-authoritative; everyone else stays snapshot-interpolated
- A soldier turns rather than pivots: the facing chases the cursor at a fixed rate instead of snapping to it, so getting behind someone is worth the trip. Holding steady drops both the walk and the turn to a fraction of normal — the gun sits where it's put, at the price of being able to leave
- Per-class primary weapons, each magazine-fed: the automatics get a few seconds of fire before they have to stop and reload, while the sniper and grenadier reload after every shot, so the wait *is* their rate of fire
- One grenade per life for every class: bounces off the world under Jolt, detonates on its fuse (or on a direct hit), with blast damage that cover blocks
- A melee swing for every class, on three charges and a recovery of its own rather than the magazine's: barely more reach than the two bodies involved and five swings to kill, so it's what's left when a primary can't answer — caught mid-reload, or with someone already inside its range
- Ragdoll corpses: the soldier model is built from seven jointed segments, so a killed soldier is handed to Jolt as a ragdoll that keeps the pose it died in, is thrown by the blow that killed it, and falls where it stood
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
| `Esc` | Quit |

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
```

Joining a server (the menus work as usual; `--class` skips them for a
one-command join, which is the fast lane while iterating on netcode):

```powershell
.\build\Debug\infantry.exe --connect 192.168.1.10
.\build\Debug\infantry.exe --connect 127.0.0.1 --class marine
```

CMake generates a Visual Studio solution in `build/` — open `build\infantry.sln` to work in the IDE.

## Layout

```
src/            C++ sources
assets/         Runtime assets (.glb models, copied next to the exe post-build)
etc/            build/run scripts, asset generators (make_tree.py)
build/          CMake output (generated, not committed)
```

`assets/tree.glb` is generated by `python etc/make_tree.py`; real assets can be
authored in Blender and exported as glTF (.glb).

## Roadmap ideas

- Entity/component structure for players, vehicles, and weapons
- Collision (projectile vs. obstacle/player, player vs. obstacle)
- Sprite/billboard rendering pass for Infantry-style readability, HUD & radar (DirectXTK12 `SpriteBatch`/`SpriteFont`)
- Replace placeholder cubes with glTF models (player, bunkers, bases)
- Tile-based map format + loader (Infantry-style zones)
- Netcode, the rest of it: fog-based relevance filtering (the anti-wallhack the visibility system already knows how to answer), delta-compressed snapshots, predicted muzzle effects so your own shot sounds on the press, a connect-to-IP box in the menu, server discovery
- Audio (DirectXTK12 audio module — re-enable `BUILD_XAUDIO_WIN10`), effects, teams/CTF game modes
