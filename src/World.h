#pragma once

#include "Brain.h"
#include "Command.h"
#include "Level.h"
#include "Physics.h"
#include "PlayerClass.h"
#include "Visibility.h"

#include <DirectXMath.h>
#include <SimpleMath.h>
#include <algorithm>
#include <random>
#include <utility>
#include <vector>

namespace Net
{
    struct Snapshot;
}

// The simulation, whole and blind. Everything that decides the outcome of a
// match lives behind this class — the roster, the shots in the air, the
// walls, the clocks — and nothing in it knows there is a screen, a speaker,
// or a keyboard. Time only passes through Tick, in fixed steps; what a player
// wants only arrives as a Command; and everything worth showing leaves as an
// Event for whoever is presenting to spend. That's the whole contract, and
// it's the server's contract too: a dedicated server is a loop around Tick
// with a socket where the local Command used to be.

// One soldier on the field, whoever is driving it. The player used to be a
// dozen loose fields and everyone else an Npc struct, which meant every
// system that dealt damage or picked a target had to say everything twice —
// and a server can't work that way: to it a match is ten of the same thing,
// and which machine steers which is bookkeeping. So the roster is one shape,
// and the controller is one more fact about a soldier.
//
// They are not all hostile. A unit on the player's own team is a squadmate —
// it fights the same enemies, soaks the same rounds, and is the thing a
// medic's dressing has to have in front of it to be worth anything. Which
// side a unit is on is `team`, and every system that deals damage or picks a
// target reads it rather than assuming.
struct Unit
{
    using Vector3 = DirectX::SimpleMath::Vector3;

    // Who decides what this soldier does next: a Brain, this machine's
    // Command, or a Command that arrived over the wire. Nothing below the
    // controller differs by it — Remote joining this enum and not this
    // struct was the whole bet of the refactor, and it paid.
    enum class Controller
    {
        Ai,
        Local,
        Remote,
    };

    // Stable for the unit's whole life and never reused within a match. The
    // roster's indices shift every time somebody dies, so anything that
    // outlives a tick — a server's session, a snapshot, an event — names
    // soldiers by this instead.
    int id;
    const ClassDef* cls;
    Controller controller;
    int team;
    Vector3 pos;
    // Ground momentum: the keys steer it, they aren't it. Only the local
    // player moves on it today — the AI still walks on speed alone, and
    // giving it weight is a change to how it steers, not one more field
    // to read.
    Vector3 moveVel;
    Vector3 aimDir;
    float hp;
    float fireCooldown;
    int ammo;          // rounds left in the magazine
    float reloadTimer; // > 0 while reloading, and unable to shoot back
    // The rest of the kit, issued to every controller alike: today's
    // brains never throw the grenade, swing the blade, or carry a class
    // with an ability, but that's a fact about the brains, not about what
    // an AI soldier is issued.
    int grenades;
    int meleeCharges;
    float meleeRecover;  // > 0 while the charges are coming back
    float meleeCooldown; // time until the next swing
    Ability::Runtime ability;
    // Whatever this soldier's brain remembers between frames, and nothing
    // at all while the controller is Local. Meaningless out here on
    // purpose: which mind it is comes off the class, and what it keeps in
    // there is that mind's business.
    Brain::Memory mind;
    float walkPhase;   // leg-swing angle for the soldier model, advances with distance
    float moveBlend;   // 0..1 walk-pose weight, eases in/out so stops don't snap
    Vector3 knock;     // launch velocity the last hit would give its corpse

    // Where this soldier was at the top of the current tick, kept so a
    // renderer can draw the moment between two ticks rather than the stutter
    // of the last one. The simulation writes them and never reads them; they
    // are the one concession this struct makes to being watched.
    Vector3 prevPos;
    Vector3 prevAimDir;
    float prevWalkPhase;
    float prevMoveBlend;
};

// Something the simulation did that a watcher might want to show: a shot
// leaving a barrel, a body hitting the floor, a magazine coming out. The
// simulation states what happened and moves on — what any of it looks or
// sounds like is the presentation's business, and a server would forward
// these to clients rather than interpret them.
//
// One struct with a superset of fields rather than a type per event, the way
// WeaponDef carries a blast radius most weapons leave at zero: which fields
// mean anything is stated per type below, and a reader gets to see every
// event's whole vocabulary in one place.
struct Event
{
    using Vector3 = DirectX::SimpleMath::Vector3;

    enum class Type
    {
        Fire,         // a shot left a barrel: pos, dir, damage (the report's depth)
        Hit,          // a soldier took damage: pos, dir (the blow), damage, fatal, local
        Death,        // a soldier came off the roster: pos, dir (facing), walkPhase,
                      // moveBlend, knock, team, cls — everything a corpse is built from
        Detonation,   // a shot ended: pos, radius, explodes, hitUnit
        Bounce,       // a live grenade knocked off something, hard enough to hear: pos
        ReloadStart,  // pos, local
        ReloadEnd,    // pos, local
        MeleeSwing,   // pos, dir (the swing), local
        AbilityStart, // pos, sound, local
    };

    Type type;
    // The soldier the event is about, by roster id, or -1 when it's about
    // nobody in particular (a detonation, a bounce). The wire carries this
    // and each client reconstructs `local` from it, since "local" is a fact
    // about the reader, not the event.
    int unit = -1;
    // The local soldier's own doing or suffering. Presentation reads it to
    // route a sound to the listener's hands rather than a spot on the floor,
    // and to know whose pad to rumble. Set directly by the in-process path;
    // derived from `unit` on arrival over the wire.
    bool local = false;
    Vector3 pos;
    Vector3 dir;
    float damage = 0.0f;
    bool fatal = false;
    Vector3 knock;
    float walkPhase = 0.0f;
    float moveBlend = 0.0f;
    int team = 0;
    const ClassDef* cls = nullptr;
    float radius = 0.0f;
    bool explodes = false;
    bool hitUnit = false;
    const char* sound = nullptr;
};

class World
{
public:
    using Vector3 = DirectX::SimpleMath::Vector3;

    // The simulation's own clock: sixty steps a second, whatever any display
    // is doing. Fixed because everything downstream of it wants to count in
    // whole ticks — cooldowns quantize the same on every machine, a replay is
    // a list of numbered commands, and two computers stepping the same inputs
    // stay the same world. Sixty because it's the coarsest rate at which the
    // fastest thing in the game still simulates cleanly, and it matches the
    // physics tick Jolt was already stepping on.
    static constexpr float kTickDt = 1.0f / 60.0f;

    static constexpr float kMaxHealth = 100.0f;
    // Grenades are issued per life, not recharged: spend it and there isn't
    // another until you respawn, which is what makes the throw a decision.
    static constexpr int kGrenadesPerLife = 1;
    // Dying costs time as well as position: long enough that a death is felt,
    // short enough that watching it out isn't the game. A placeholder value
    // until there's something to tune it against (round length, ticket bleed).
    static constexpr float kRespawnDelay = 5.0f;
    // Soldiers a side puts on the field, the local player included. Five a side
    // is the smallest number that makes the arena read as a fight rather than a
    // duel: enough that a flank is covered by somebody, few enough that any one
    // soldier going down is felt. It's a constant rather than a level property
    // because it's a statement about the game mode, and there's only one of
    // those so far; when a server decides team sizes, this is what it sets.
    static constexpr int kTeamSize = 5;

    // Runtime halves of a level object the simulation cares about: solid
    // objects contribute a collider (physics + soldier push-out). Whether one
    // also has a model is carried along only so a presentation can draw the
    // modelless ones as debug boxes.
    struct Collider
    {
        Vector3 center;
        Vector3 size;
        bool debugDraw; // no model to represent it, so draw the box itself
    };

    struct Projectile
    {
        Physics::BodyHandle body; // 0 on a client replica: the shot is the server's to fly
        float life;
        // Where the shot is, refreshed from the physics body each tick — or
        // from a snapshot, on a replica that has no body to ask. Everything
        // outside the simulation reads this rather than the physics world.
        Vector3 pos;
        Vector3 prevPos; // last tick's position, for swept hit tests
        int team;        // whose shot this is; it only hurts the other side
        float damage;
        float radius;
        float blastRadius; // > 0: splash damage on impact
        bool fused;      // rides out `life` bouncing off the world instead of
                         // dying on its first contact; the fuse detonates it
        bool explodes;   // grenade: impact gets the big fireball, not a puff
    };

    // Builds the arena out of the level: the floor and colliders go into the
    // physics world, sight-blockers into the occluder list, spawn points into
    // the roster's geography. The level's models are somebody else's half.
    void Init(const LevelData& level);

    // Puts both sides on the field at full strength. On a machine with a
    // player, `localClass` is the class they picked and their unit spawns
    // among the rest; on a machine without one — a dedicated server —
    // `localClass` is null, every slot is the AI's, and humans will claim
    // slots the way the local player does here: by being one more controller.
    // Called once, when the match starts — nothing simulates until then and
    // there's nobody for a squad to fight.
    void StartMatch(const ClassDef* localClass, int localTeam);

    // Puts the local player's unit back on the field: the class and team
    // StartMatch recorded, at their team's spawn, with the whole loadout
    // issued fresh off the class table. Every respawn comes through here,
    // which is what makes a respawn a new soldier rather than a checklist of
    // things to remember to put back.
    void SpawnLocal();

    // One fixed step of everything: the local command first (null while
    // there's no local soldier to drive — the arena runs on without them),
    // then every other system in the order they've always run. Whatever
    // happened worth showing is appended to `events`. This is the function a
    // server's loop calls; it is the only place simulation time passes.
    void Tick(const Command* cmd, std::vector<Event>& events);

    // The unit driven by this machine, or null while there isn't one — before
    // a match starts, and for the length of the respawn wait. Dying really
    // does take the player off the roster: the systems that sweep it (shots,
    // blasts, sights) stop finding them without any of them asking who's
    // alive.
    Unit* Local();
    const Unit* Local() const;
    Unit* UnitById(int id);

    // --- The server's half of the surface. A connected player is three
    // calls: ClaimSlot when they pick a side, SpawnRemote when they take the
    // field, SetCommand every tick they're on it. None of it means anything
    // to a client or to solo play, and none of it is one line of special case
    // inside the simulation — a Remote unit is a unit.

    // Reserves one of `team`'s slots for a human. The AI gives the slot up
    // immediately — a living AI soldier on that side is removed, quietly, no
    // corpse: they rotate out, they don't die of somebody connecting. The
    // claim holds through deaths and respawns, exactly like the local
    // player's; returns the (clamped) team actually claimed.
    int ClaimSlot(int team);
    // Hands a claimed slot back — a disconnect. The side is owed a soldier
    // again, on the same reinforcement clock a death starts: a leaver's slot
    // refills the way a casualty's does, not instantly.
    void ReleaseSlot(int team);
    // Human claims currently held on `team`, the local player's included.
    int HumanSlots(int team) const;
    int TeamCount() const { return static_cast<int>(m_teamSpawns.size()); }

    // Living soldiers on `team`. Counted off the roster when the roster is
    // whole; on a replica it's the number the server sent, because a fog-
    // filtered roster only holds what this client is allowed to see and the
    // corner panel is a scoreboard, not a wallhack.
    int Standing(int team) const;

    // Puts a connected player's soldier on the field: `cls` at `team`'s
    // spawn, full loadout, driven by whatever SetCommand says from now on.
    // Returns the unit's id — the name the server and the wire know them by.
    int SpawnRemote(const ClassDef& cls, int team);
    // Takes a soldier off the field without a death: no corpse, no event, no
    // reinforcement. For the body a disconnect leaves behind; a slot released
    // is a separate matter (ReleaseSlot).
    void RemoveUnit(int id);
    // Stages `cmd` to be applied to unit `id` on the next Tick. One command
    // per unit per tick — staging again before the tick replaces, it doesn't
    // stack. The tick consumes the whole stage.
    void SetCommand(int id, const Command& cmd);

    // --- The client replica's half: a World that is never Ticked, whose
    // state arrives readymade. Everything a snapshot doesn't mention is
    // removed; everything it does is upserted, with the previous state kept
    // for the same between-ticks blend the solo renderer does. `myUnitId`
    // marks which unit gets Controller::Local, so every drawing path that
    // asks "is this me" keeps working against a wire-fed roster.
    void ApplySnapshot(const Net::Snapshot& snap, int myUnitId);

    // The movement half of a command: momentum, the walk cycle, and the
    // facing coming around — everything about a command that moves a body
    // and nothing that spends its kit. Public, and callable on a unit that
    // isn't on the roster, because this is the half a client predicts: the
    // same arithmetic runs on both ends of the wire, against this world's
    // walls, which is the whole reason a replayed prediction lands where the
    // server's answer will.
    void MoveCommand(Unit& unit, const Command& cmd, float dt) const;

    const std::vector<Unit>& Units() const { return m_units; }
    const std::vector<Projectile>& Projectiles() const { return m_projectiles; }
    const std::vector<Collider>& Colliders() const { return m_colliders; }
    const std::vector<Visibility::Rect>& Occluders() const { return m_occluders; }
    float ArenaHalf() const { return m_arenaHalf; }

    // The physics world, handed out mutable for exactly one customer: corpses.
    // A ragdoll is decoration — it decides nothing — so it belongs to the
    // presentation, but its parts still have to fall through the same world
    // the level's geometry lives in. A server never asks, and the day corpses
    // matter to an outcome is the day they move in here and this accessor
    // narrows to const.
    Physics& Phys() { return m_physics; }
    const Physics& Phys() const { return m_physics; }

    // What `user`'s ability is allowed to act on this tick: the user
    // themselves, and every living squadmate they can currently see. The
    // sight rule is applied here rather than inside the ability because the
    // fog is this class's business and an ability has never heard of a wall —
    // which is also what keeps "can a medic treat someone through cover"
    // answered in exactly one place. Rebuilt per call into m_abilityAllies,
    // which is kept around so the per-tick list costs no allocation; the
    // returned scene points into the roster and is good only until it grows
    // or shrinks.
    Ability::Scene AbilityScene(Unit& user);

    // Marches the ballistic arc SpawnShot would fire (aimed targetDist away)
    // against the colliders and the ground; returns the horizontal distance
    // from `from` at which the shot stops. When outArc is given, fills it
    // with the sampled trajectory (muzzle to stop). Powers the aim indicator.
    float PredictShotStop(const WeaponDef& weapon, const Vector3& from, const Vector3& dir,
                          float targetDist, std::vector<Vector3>* outArc = nullptr) const;

private:
    // A slot on a team waiting to be filled again. Queued when an AI soldier
    // dies and cashed in kRespawnDelay later, which is the same wait the
    // player serves — a squad is five soldiers, and the only time it isn't is
    // the moment after one of them is killed.
    //
    // It's a queue rather than one timer per side because each death should
    // start its own clock: a team that loses three at once has three soldiers
    // coming back at the times they died, not one every five seconds while the
    // other side runs it down.
    struct Reinforcement
    {
        int team;
        float timer;
    };

    // The simulation half of a command: one tick of acting on what a driver
    // asked for. Movement, aim, and every weapon in the kit — all against
    // `unit`'s own class table and clocks, which is what keeps a command
    // honest whoever sent it.
    void ApplyCommand(Unit& unit, const Command& cmd, float dt);
    // The clocks every soldier runs the same way whoever is driving: cadence,
    // the blade's recovery, and a reload finishing. Called once per tick by
    // whichever path owns the body — ApplyCommand for a command-driven
    // soldier, UpdateUnits for a mind-driven one — so nobody is ticked twice.
    void TickClocks(Unit& unit, float dt);
    // Starts `unit`'s reload: empties the rest of the magazine into the
    // timer. A no-op if one is already running or the magazine is full, so
    // the reload key can be leaned on harmlessly.
    void BeginReload(Unit& unit);
    // Spends one of `attacker`'s melee charges on a swing through the arc in
    // front of them and strikes the nearest enemy standing in it, if any. The
    // charge goes whether or not it lands.
    void SwingMelee(Unit& attacker);
    // Fires `weapon`'s projectile from `from` along `dir`. Bullets always fly
    // at full speed; lobbed shots (grenades) shorten their toss to come down
    // `targetDist` away, up to the weapon's max range.
    void SpawnShot(const WeaponDef& weapon, const Vector3& from, const Vector3& dir, int team,
                   float targetDist);
    // Deals the next class off `team`'s rotation and puts an AI soldier on
    // the field with it.
    void SpawnAi(int team);
    // The one spawn under SpawnLocal and SpawnRemote: a human's soldier —
    // full loadout off the class table, no brain woken — differing only in
    // which controller drives it.
    int SpawnHuman(const ClassDef& cls, int team, Unit::Controller controller);
    // How many AI soldiers `team` is meant to be fielding: its share of the
    // roster, less every slot a human has claimed on it — the local player
    // and connected ones alike. A claim holds whether its holder is alive or
    // waiting to respawn, so nothing fills in for them and nothing has to be
    // sent away when they're back.
    int AiQuota(int team) const
    {
        const int humans =
            team < static_cast<int>(m_humanSlots.size()) ? m_humanSlots[team] : 0;
        // A side already full of humans owes the AI nothing — and never a
        // negative number of soldiers, however oversubscribed it gets.
        return std::max(0, kTeamSize - humans);
    }
    // Living AI soldiers currently on `team`.
    int AiCount(int team) const;
    void UpdateUnits(float dt);
    void UpdateProjectiles(float dt);
    // Turns whatever died this tick into a Death event and takes it off the
    // roster. Runs once, after every system that can deal damage has had its
    // turn, so it doesn't matter which of them landed the killing blow.
    void ReapDead();
    // Runs the queue of waiting slots down and puts a soldier back on the
    // field when one comes due, unless the side is somehow already up to
    // strength.
    void UpdateReinforcements(float dt);
    // Ends `shot` at `pos`: splash damage for explosives, then the Detonation
    // event (which differs for a hit on `hitUnit` vs. world geometry).
    void Detonate(const Projectile& shot, const Vector3& pos, bool hitUnit);
    // Splash damage around `center`, full strength at the middle and falling
    // off to nothing at `radius`. Only the side opposing `team` is hurt, and
    // only where the blast has line of sight, so cover still protects.
    void ApplyBlast(const Vector3& center, float radius, float damage, int team);
    // Clamps pos to the arena and pushes it out of solid colliders.
    void ResolveObstacles(Vector3& pos) const;
    float Rand(float lo, float hi);
    // Appends to the event list of the Tick in flight. Outside a Tick there
    // is no audience, and anything worth saying then isn't said this way.
    void Emit(const Event& ev);

    Physics m_physics;
    const ClassDef* m_localClass = nullptr; // set by StartMatch; null on a server
    int m_localTeam = 0;
    int m_nextUnitId = 1; // 0 never issued, so an uninitialized id matches nobody
    // Human claims per team (see ClaimSlot); what AiQuota subtracts.
    std::vector<int> m_humanSlots;
    // Commands staged for remote units, consumed whole by the next Tick.
    std::vector<std::pair<int, Command>> m_staged;
    // The server's per-team standing counts, held by a replica whose own
    // roster is fog-filtered. Empty on any world whose roster is the truth.
    std::vector<int> m_standingOverride;
    float m_arenaHalf = 32.0f;
    // Team spawn points from the level, indexed by team id.
    std::vector<Vector3> m_teamSpawns;
    // Where each side has got to in the class table. Per team rather than
    // global so both squads are dealt from the same rotation and come out with
    // the same makeup, however many soldiers each of them is owed — which is
    // not the same number, since the player fills one of the slots on their
    // own side. Sized to the level's team count in Init.
    std::vector<int> m_nextAiClass;
    // The roster: every soldier on the field, whichever machine or mind is
    // driving them. Dead units are swept off it by ReapDead, so anything
    // holding a pointer or index into it is good for one tick at most.
    std::vector<Unit> m_units;
    std::vector<Projectile> m_projectiles;
    std::vector<Reinforcement> m_reinforcements; // slots waiting to be refilled
    std::vector<Collider> m_colliders;
    std::vector<Visibility::Rect> m_occluders; // footprints of sight-blockers
    std::vector<Ability::Target> m_abilityAllies; // reused per tick; see AbilityScene
    std::vector<Brain::Contact> m_contacts;       // reused per unit; see UpdateUnits
    std::mt19937 m_rng{ std::random_device{}() };
    std::vector<Event>* m_out = nullptr; // the Tick in flight's event list
};
