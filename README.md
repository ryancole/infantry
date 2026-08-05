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
- Players have names, and a name belongs to a *player* — the third identity this game keeps, after a unit (one life) and a slot (one match). It is stated once, at the door: it rides up in the join alongside the class, and the server issues every connection an id of its own to hang it on, which is what lets two people called RYAN be two people — separate rows, separate records, and a console that can say which of them left. Twelve characters of A-Z, digits, space, dash and underscore, and cleaned to exactly that at all three points it could arrive wrong: typed into a settings screen, read out of a file somebody edited by hand, or sent up by a client somebody rewrote. What a name may be is one rule in one file, because a rule enforced in three places by three copies is a rule that holds in two of them. Say nothing at all and the server calls you SOLDIER and the number it gave you, which is still somebody rather than a blank row. The scoreboard is where it lands: a person's row is their name with the class they're holding beside it, an AI's row is the class alone — the honest name for a place whose class is re-dealt every life, and printing it twice would be the board talking to itself — and a player who leaves keeps their name on the board along with their kills, because a record nobody can be identified from is a row that doesn't account for the total above it. The default is the account name Windows already knows, so a first run puts a real name on the board without anybody visiting a menu, and OPTIONS -> PLAYER is where it's changed
- Five a side: picking a class starts a match rather than dropping you into an empty arena. Both squads come up to strength at their own spawn, and a slot that empties is refilled after the same wait the player serves, so the fight stays five against five. Everyone but you is driven by the AI today — the roster is one list of units, and who drives each (a brain, a command off the wire) is a fact about the soldier rather than a difference in kind. A soldier with nothing in sight moves up rather than wandering: it still picks somewhere at random, but only somewhere standing closer to the enemy's ground than it does now, so a squad crosses the map instead of milling about its own half. That is not a route and it has never heard of a wall — what it means in practice is that a mountain in the way gets gone around, because going around it is what the points on the far side of it require
- Which class you are is a decision you get to revisit, but only where lives begin: standing inside your own spawn area, or waiting out a respawn, `F11` puts the class cards back up over the top of the match. Picking one there trades the soldier you have for a fresh one of the class you picked — full health, full magazine, the grenade back — on the ground you were already standing on, and your kills and deaths come across with you, because the record was never the soldier's. It hangs on the slot, and a slot outlives everyone who stands in it. Dead, there is nothing to trade and nothing to grant: the soldier you are already owed simply arrives as whatever you picked while you waited. What it costs is the walk. The spawn is eight units of ground at your end of a map nearly two hundred long, so changing your mind mid-match means leaving the fight and crossing back into it — which is the same reason walking home to come back whole is a real option rather than a free heal. The area is drawn as a ring in your side's color that brightens when you're inside it, and its key cap appears on the HUD only where the key would actually work, so the rule gets taught by the one thing that can teach it: walking home for something else and finding the key there. The server checks the same rule against the same spawns when the ask arrives, so a client edited to offer it in the middle of the map is a client being ignored
- The simulation is severed from the machine it's watched on: a `World` that steps on a fixed 60 Hz tick, hears input only as a `Command`, and reports what happened as events for the presentation to spend on blood, sound, and ragdolls. The renderer draws the blend between ticks, so a fast display sees motion rather than sixty stills
- True multiplayer over that seam: a `Server` hosts the match with AI in every unclaimed slot, and clients join over ENet — numbered commands up, snapshots and events down, sixty a second. A joining player displaces an AI soldier from the emptier side; a leaver's slot goes back to the AI on the same reinforcement clock a death starts. `infantry_server.exe` is that class with a console loop around it and no D3D on its link line
- There is no offline mode, because there is no second way to run a match. DEPLOY starts a `Server` inside the game's own process — bound to loopback on a port the OS picks, so it's listed nowhere and reachable from nothing — and joins it as a client over that socket. Playing on your own is a one-player match on a real server: the fog is server-enforced, your soldier is predicted and reconciled, the snapshots are quantized and the bytes are the same bytes. What it costs is that the trigger is answered on the server's next tick, which is a tick away when the server is an inch away. What it buys is that the code that decides a match has one reader and one set of bugs, and a fix to either lands in both places at once
- Client-side prediction with server reconciliation: your own soldier answers movement and aim on the frame you press, simulated locally by the same `MoveCommand` the server runs, then squared against every snapshot — acked commands retired, in-flight ones replayed on top of the server's answer, so when nothing contradicted you the correction is exactly zero. Firing stays server-authoritative; everyone else stays snapshot-interpolated
- The visibility sweep is binned by angle: a rectangle seen from a point covers one arc and can only be hit from inside it, so the walls are filed into bins around the viewer and a ray is only tested against its own bin. Nothing is approximated — the polygon is the same one the exhaustive version produced, checked against it — but the sweep stopped being quadratic in occluders, which is what a real map needs. On hardcorps2t it is 23× fewer segment tests, and the map now costs less to fog than the blockout arena did before any of this
- The fog of war is enforced by the server, not the renderer: each client's snapshot is filtered through the same `Visibility` test the fog is drawn with, from the same eyes, so an enemy nobody can see is absent from the bytes and no packet sniffer can find them. Events travel by earshot instead — gunfire behind cover is still a thing you hear — and the corner scoreboard arrives unfiltered, because a scoreboard is meant to know
- Sight reaches a fixed distance and stops, wall or no wall, and that number is the same one whether a person or a brain is behind the eyes. Bounding it at all matters because without it the fog was decoration on this map — only the outcrops block sight, so an unbounded eye saw the far base from the near one. Wherever the number lands it stays under earshot (forty-five), so the map you hear is always wider than the map you see; clear of what a brain will engage at (twenty-two), so an NPC is never quietly capped by an eyesight number nothing reports; and wider than the screen at the zoom the camera opens on, so you meet the edge of your sight by looking down a long corridor or by zooming out, not by standing still
- How far is a class stat rather than one constant (`ClassDef::sight`), and it comes from the zone: hardcorps2t's own class list gives the sniper *"enhanced viewing distance and LOS"* and says nothing of the kind about anyone else. So the sniper sees thirty-eight — past where its own bolt drops into the ground, around thirty-six, so for the first time the class called LONG RANGE can see the whole of what it shoots at instead of firing into fog it has to be told about. The marine keeps the thirty everything was tuned against. The medic and the grenadier see twenty-six, for opposite reasons that cost the same: the medic is fast and has to close on the soldier it treats anyway, the grenadier throws over cover at ground it was never going to see. Both want somebody out in front of them, which is what a shorter eye is for — it makes being spotted for something a class needs rather than something it enjoys, and it gives the sniper a job on a quiet flank. The column is on the class cards as LOS, next to RNG, because the pair is only interesting when they disagree
- A side sees with all of its eyes at once. What the fog is cut with is every living soldier on your team — your own (or the spot you died on, while you wait), and each squadmate's — so an enemy inside any one of their circles, with a clear line to them, is an enemy on your screen, standing in the hole their sight cut in the dark. The circles are no longer the same size as each other, which is what makes a squad's makeup a thing you can feel: a sniper on the flank widens what everyone knows. It is the difference between five soldiers and a squad: spotting is a thing a teammate can do for you, and walking point means something. Your own side never needs spotting at all — squadmates are always on the field, at any distance, wall or no wall, because a soldier who blinks out of existence on turning a corner is one nobody can fight alongside. Both ends build the list the same way (`World::TeamEyes`), which is what stops a spotted enemy from arriving in the snapshot only to be culled back out by the client drawing it
- Which is also why the fog stopped being drawn as darkness and started being drawn as light. Darkness doesn't union: laid down per soldier it stacks where two of them overlap, and it covers ground one of them can see perfectly well. So the lit polygons go into the stencil buffer first — one fan per eye, marking rather than painting, and marking twice is marking — and a single dark sheet follows over everything left unmarked. The sweep casts a ring of evenly spaced rays as well as the ones the occluder corners ask for, because in the open the far edge of sight is an arc and there is no corner out there to find it
- A radar in the bottom-left corner, because the camera shows twenty-six units of a map that is nearly two hundred across and the rest of the match happens off screen. It draws the outcrops as well as the bodies — a field of dots with no shape behind it says where everyone is without saying what they're behind — and it is turned to face the camera rather than laid out on the arena's own axes. That is the one thing about it that isn't taste: the view is isometric and its yaw starts at 45°, so world +x travels left and down the screen, and a radar drawn on the map's axes doesn't merely sit at an angle to what the player sees — it puts their own base on the wrong side of the panel. Left and right disagreeing is the one error a radar can't survive, so the arena arrives as a diamond and the panel is the box that holds it, which is why a long letterbox map gets a near enough square readout. How much of the map is in that box is the player's to set, on two keys held rather than tapped, from the whole arena fitted to a window about thirty-six units across — under a marine's sight, so there is always a zoom at which the radar shows less than the soldier does. It opens well in from the wide end, far enough that the diamond covers the box: a panel with empty corners is a panel spending screen on nothing, and it makes the fight in front of you the default reading with "which way across" one key away, which is the right way round. Past that point the box is a window that follows the eye — the soldier, or the spot they died on while they wait — and stops at the arena's edge rather than scrolling the ground out from under somebody standing still. A blip wears the two colors its body wears: the side's armor underneath, because that is the only thing that decides whether to shoot, and the class mark at its middle, so a squad's makeup is readable from the corner as well as from the field. Your own soldier is the same two colors and a shape nobody else has, with a heading out of the front of it — where you are is what the camera already centers on; which way you're pointed is the part it doesn't say. The blips are filtered through the same eyes and the same test the arena's bodies are drawn through, in the same place, so the radar cannot be the one readout that outranks the fog
- The wire is built for roads worse than a LAN: every command packet carries its two predecessors, so a lost packet can't eat a grenade press; snapshots are quantized (a position is two bytes an axis, an angle two bytes, health one) to about half their float weight with no statefulness to resync; the protocol carries a version and a server refuses a mismatched build or a full house at the door; and losing the server — or pressing Escape in somebody else's match — lands on the main menu with the world swept clean, not on the desktop. Delta compression is deliberately absent: full snapshots self-heal from any loss, and ten soldiers don't weigh enough to trade that away
- Servers are found, not typed: JOIN on the main menu shouts a UDP broadcast at the LAN once a second and lists every server that answers — machine name, address, how full — refreshing live and forgetting the ones that go quiet. One typed row covers servers a broadcast can't reach. An internet server browser is the piece that still needs a landlord (somewhere to host the list), and it's the one piece of the netcode left
- A soldier turns rather than pivots: the facing chases the cursor at a fixed rate instead of snapping to it, so getting behind someone is worth the trip. Holding steady drops both the walk and the turn to a fraction of normal — the gun sits where it's put, at the price of being able to leave
- Per-class primary weapons, each magazine-fed: the automatics get a few seconds of fire before they have to stop and reload, while the sniper and grenadier reload after every shot, so the wait *is* their rate of fire. Every weapon has two reload times, taken from the zone's own numbers — changing a magazine with rounds still in it is seconds quicker than being caught with an empty one, so when to stop and top up is a decision worth making
- The sniper rifle has a minimum range, and it's the first stat in the class table that takes something away rather than handing it out. Six units of ground in front of the muzzle where the bolt does nothing at all: a soldier standing inside it is passed straight through and the round carries on to whatever is behind them. It isn't weakened and it isn't stopped — it simply isn't armed yet — so a sniper with somebody in their face is a sniper holding a stick, and the answer has to be the blade, the grenade, or the walk backwards. It's there because a class called LONG RANGE should be answerable up close and until now it wasn't: the same 85 that ends a soldier at thirty units ended them at two, so the counterplay to the longest reach in the game was to walk into it. Every other cost the class pays is a wait, and a wait is something a good player spends well; dead ground is not, which is what a class this deadly at range needs somewhere on it. Six is measured against what a soldier meets — well past the blade (1.6) so the fallback is a real weapon, wide enough that a trench (4.9 across) is dead end to end, and short of the medic's dressing (7), the shortest reach in the game that isn't a swing. The rule is a radius, so that's how it's drawn: a ring on the ground around the soldier, with the stretch of the aim line inside it greyed out, and the class card says it in words because the RNG bar is a reading of muzzle speed and a notch cut in its near end would be drawn against the wrong axis. NPCs are told the number the same way they're told they're out of ammo — as a bare fact about what's in their hands, not a class table a brain has to read — and a rifleman inside it holds fire and backs straight off, gun still up
- Soldiers can shout, and the first thing they can shout is `MEDIC`. It is a sound and nothing else — it heals nobody, marks nobody on any readout, and obliges nobody; the medic who answers it is a person who heard it and decided to come. Which means it travels the way every other sound in this game travels: by earshot, out to forty-five units, to whoever is standing near enough on *either* side. Being heard by the wrong people is what being heard by the right ones costs, and it is the whole of what makes calling a decision — a wounded player behind a trench who screams for help has just told the soldier on the other side of it that somebody over here is hurt, and roughly where. The callout rides in the command like the grenade press does, so it gets the same three chances to arrive, and the mouth is on a three-second cooldown enforced in the simulation rather than on the key, because the key is on a machine the server doesn't own. `F5`, where the zone this game comes from put its callouts, and where the next three are going. The line itself is spoken by the Windows speech engine and then worked over (`tools/Generate-Voice.ps1`): sped up, which drags the formants up with the pitch so it reads as strained rather than merely hurried, driven into a soft clip the way a voice at the top of its range distorts, and rolled off underneath so it cuts through gunfire instead of sitting in the mud with the explosions. Unlike the synthesized SFX it is committed rather than reproduced on demand, because which voices a machine has is a fact about the machine — and it is a placeholder either way, since every speech engine reads and a callout is a yell
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
| `R` | Reload (automatic when the magazine empties, and quicker before it does) |
| `F` | Throw grenade (every class, one per life) |
| `V` | Melee swing (every class, three charges then a recovery) |
| `Shift` | Hold steady (slow walk, much slower turn) |
| `F5` | Shout for a medic (heard by both sides, out to earshot) |
| Mouse wheel | Zoom the camera |
| `[` `]` | Zoom the radar out and in (held) |
| `F11` | Change class — inside your own spawn area, or while waiting to respawn |
| `Esc` | Leave the match (back to the main menu; QUIT there closes the game) |

OPTIONS on the main menu holds the settings screens: PLAYER is the name you
fight under, KEY BINDS puts any of the above on another key or mouse button, and
AUDIO decides whether the game keeps making noise while its window is behind
something else (off by default). All three save to `%LOCALAPPDATA%\Infantry\` —
`bindings.toml` and `settings.toml` — and both files are safe to edit by hand. A
name is stated when you join a server, so changing it applies to the next match
you join rather than the one you are in.

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
- Sprite/billboard rendering pass for Infantry-style readability (DirectXTK12 `SpriteBatch`/`SpriteFont`)
- Replace placeholder cubes with glTF models (player, bunkers, bases)
- Tile-based map format + loader (Infantry-style zones)
- Netcode, the rest of it: predicted muzzle effects so your own shot sounds on the press, and an internet server browser once there's somewhere to host the list
- Audio (DirectXTK12 audio module — re-enable `BUILD_XAUDIO_WIN10`), effects, game modes past the kill count (CTF, ticket bleed, objectives)
