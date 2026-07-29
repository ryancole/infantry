# Infantry

A 3D isometric multiplayer shooter inspired by the classic *Infantry* (Infantry Zone / [Free Infantry](https://freeinfantry.com/)), built from scratch in C++ on Direct3D 12.

## Current state

Prototype scaffold with a working game loop:

- D3D12 renderer (device, flip-model swap chain, depth buffer, color pipeline, dynamic geometry streaming)
- Fixed-angle isometric camera (orthographic, smoothed follow, mouse-wheel zoom)
- Grid arena with obstacle bunkers
- Player movement, mouse aim, and projectile firing

## Controls

| Input | Action |
| --- | --- |
| `W` `A` `S` `D` | Move (screen-relative) |
| Mouse | Aim |
| Left mouse / `Space` | Fire |
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

CMake generates a Visual Studio solution in `build/` — open `build\infantry.sln` to work in the IDE.

## Layout

```
src/            C++ sources
src/shaders/    HLSL (compiled at runtime, copied next to the exe post-build)
etc/            build/run scripts
build/          CMake output (generated, not committed)
```

## Roadmap ideas

- Entity/component structure for players, vehicles, and weapons
- Collision (projectile vs. obstacle/player, player vs. obstacle)
- Sprite/billboard rendering pass for Infantry-style readability, HUD & radar
- Tile-based map format + loader (Infantry-style zones)
- Networking (client/server, snapshot interpolation)
- Audio, effects, teams/CTF game modes
