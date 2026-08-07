#pragma once

#include "Brain.h"
#include "Command.h"
#include "Level.h"
#include "Physics.h"
#include "PlayerClass.h"
#include "Visibility.h"
#include "Voice.h"

#include <DirectXMath.h>
#include <SimpleMath.h>
#include <algorithm>
#include <random>
#include <string>
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

    // Who decides what this soldier does next: a Brain, or a Command that
    // arrived over the wire. Nothing below the controller differs by it —
    // Remote joining this enum and not this struct was the whole bet of the
    // refactor, and it paid.
    //
    // Local is the odd one out: no simulation ever produces it, because every
    // player reaches a match through a socket. It exists on a client's replica
    // alone, where ApplySnapshot stamps it on whichever unit the wire says is
    // yours — the mark a drawing path reads to ask "is this me".
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
    // How long the reload underway was going to take: whichever of the
    // weapon's two numbers was charged when it started. Kept because the
    // timer alone can't say how much of the wait is left as a fraction, and
    // by the time anything reads it the magazine is already empty, so there's
    // nothing left to work it back out from.
    float reloadSpan;
    // The rest of the kit, issued to every controller alike: today's
    // brains never throw the grenade, swing the blade, or carry a class
    // with an ability, but that's a fact about the brains, not about what
    // an AI soldier is issued.
    int grenades;
    int meleeCharges;
    float meleeRecover;  // > 0 while the charges are coming back
    float meleeCooldown; // time until the next swing
    // > 0 while this soldier has just said something and can't say anything
    // else (kVoiceCooldown). Kept on the body rather than on the connection
    // because it's the mouth that's busy, and the simulation is the one end of
    // the wire that gets to decide a client is shouting too often.
    float voiceCooldown;
    Ability::Runtime ability;
    // Whatever this soldier's brain remembers between frames, and nothing
    // at all while the controller is Local. Meaningless out here on
    // purpose: which mind it is comes off the class, and what it keeps in
    // there is that mind's business.
    Brain::Memory mind;
    float walkPhase;   // leg-swing angle for the soldier model, advances with distance
    float moveBlend;   // 0..1 walk-pose weight, eases in/out so stops don't snap
    Vector3 knock;     // launch velocity the last hit would give its corpse
    // Which place on the roster this soldier is filling (World::Slot), or -1
    // for a soldier standing in no place a holder of this roster knows about. A
    // unit is one life; a slot is the whole match, and that is the difference a
    // kill/death record has to be hung on — a tally kept on the unit would be
    // swept off the field with the body.
    //
    // It rides the wire too, and on a replica it is the only thing it's for:
    // nothing over there has ever killed anybody, but the roster arrives whole
    // and a name hangs on a row of it, so this is how a body gets connected to
    // the name that goes under it.
    int slot = -1;
    // The slot that last put damage on this soldier, and so the one that gets
    // the kill if the next blow is the last one. -1 until somebody has touched
    // them. Every system that takes health off sets it, which is the same
    // reason ReapDead can collect the dead without knowing who killed them:
    // whoever it was said so on the way past. A slot rather than a side,
    // because a scoreboard is about who did it — which side they were on is
    // one lookup away, and the team score is that lookup summed.
    int lastHitSlot = -1;

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
        Fire,         // a shot left a barrel: pos, dir, damage (the report's depth), thrown
        Hit,          // a soldier took damage: pos, dir (the blow), damage, fatal, local
        Death,        // a soldier came off the roster: pos, dir (facing), walkPhase,
                      // moveBlend, knock, team, cls — everything a corpse is built from
        Spawn,        // a soldier went onto the field: pos, local
        Detonation,   // a shot ended: pos, radius, explodes, hitUnit
        Bounce,       // a live grenade knocked off something, hard enough to hear: pos
        ReloadStart,  // pos, local
        ReloadEnd,    // pos, local
        MeleeSwing,   // pos, dir (the swing), local
        AbilityStart, // pos, sound, local
        Voice,        // a soldier shouted: pos, voice, local
    };

    Type type;
    // The soldier the event is about, by roster id, or -1 when it's about
    // nobody in particular (a detonation, a bounce). The wire carries this
    // and each client reconstructs `local` from it, since "local" is a fact
    // about the reader, not the event.
    int unit = -1;
    // The local soldier's own doing or suffering. Presentation reads it to
    // route a sound to the listener's hands rather than a spot on the floor,
    // and to know whose pad to rumble. Never set by the simulation, which has
    // no local soldier: it's derived from `unit` on arrival over the wire,
    // because "local" is a fact about the reader, not the event.
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
    // The shot came off a belt rather than out of a barrel (WeaponDef::thrown),
    // for a Fire event and nothing else. Read straight off the weapon at the
    // muzzle, because by the time the round is in the air there is nothing left
    // to ask: a grenade and a grenadier's shell are the same projectile.
    bool thrown = false;
    // Which callout was shouted (Voice.h), for a Voice event and nothing else.
    // It rides the wire as itself and the sound comes back off it at the far
    // end, the same trade AbilityStart makes with `cls`: a table index is a
    // byte and a clip name is a pointer into this process.
    uint8_t voice = kVoiceNone;
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
    // short enough that watching it out isn't the game. There's a match length
    // to weigh it against now — five seconds against fifteen minutes is a
    // couple of hundred lives a side, which is enough that the score reads as
    // the whole match rather than as whoever won the last exchange.
    static constexpr float kRespawnDelay = 5.0f;
    // How far from a side's spawn point still counts as standing at it. What
    // it gates is the class change: a soldier is what a player is committed to
    // for a life, and the ground where lives begin is the one place that
    // commitment can be revised without dying first. So it has to be a place
    // rather than a point — eight units is room to stand, wide enough to still
    // contain the ground a side now spawns across, and on hardcorps2t it fills
    // the notch each base sits in without reaching past the rock around it. It
    // stays where it is at twenty-five a side: it's sized to the map's base,
    // not to the roster, and a zone that grew with the team would be a zone
    // spilling out of the notch it was cut to fit. A player who wants a
    // different soldier walks home for it, and the walk is the price.
    static constexpr float kSpawnArea = 8.0f;
    // Soldiers a side puts on the field, the local player included. Twenty-five
    // a side is a company rather than a squad, and it changes what the arena is:
    // at five the whole fight was one engagement that everyone was in, and at
    // twenty-five there are several going on at once and a player picks which
    // one they're at. That's the trade — no single soldier's death is felt the
    // way it was, and in exchange a flank is a place you can go and find
    // somebody rather than a direction nobody was covering. It's a constant
    // rather than a level property because it's a statement about the game
    // mode, and there's only one of those so far; when a server decides team
    // sizes, this is what it sets.
    //
    // Two things scale off it and are set to match: the ground a side spawns
    // across (SpawnAi, which has to hold five times the soldiers without
    // stacking them) and the per-soldier sight sweep in UpdateUnits, which is
    // quadratic in the number on the field and is now doing twenty-five times
    // the work it was.
    static constexpr int kTeamSize = 25;

    // How far a soldier can see is no longer the World's to say: it is
    // ClassDef::sight, read off whoever is standing there, and the reasoning
    // that used to live here lives next to that column. What stayed behind is
    // the shape of the thing — sight is a range and a clear line both, a side
    // sees with all of its eyes at once, and every place that asks now asks
    // per soldier. TeamEyes below is where that answer gets assembled.

    // How long a match lasts, and what the whole thing is for: at the end of
    // it the side that has killed more has won. Fifteen minutes is long
    // enough that one good push doesn't decide it and short enough that a
    // side being ground down can see the end of it — and at a five second
    // respawn it's a couple of hundred lives a side, so the score is a
    // reading of the whole match rather than of one exchange.
    static constexpr float kMatchLength = 15.0f * 60.0f;
    // How long the result stands before the next match starts on the same
    // ground. Long enough to read who won and by how much, short enough that
    // nobody goes to make tea — a server between matches is a server nobody
    // is playing on.
    static constexpr float kIntermission = 15.0f;

    // One place on a side, for the length of a match. A slot is filled by a
    // succession of soldiers — the AI's dead are replaced on the reinforcement
    // clock, a player's are replaced by their own respawn — and it is the slot,
    // not any of them, that the fight is scored against.
    //
    // This is the identity the game didn't have. A Unit is one life: it is
    // erased by ReapDead and the next one comes back under a new id, so a tally
    // kept there would reset every five seconds and mean nothing. The roster of
    // slots is created once per match and only ever counted up, which is what
    // makes "what has this soldier done today" a question with an answer.
    //
    // Who holds it is a property of the slot rather than of the unit standing
    // in it, because it survives the wait between lives: a player who is dead
    // still holds their place, and the AI is not owed a soldier for it. That is
    // the same claim ClaimSlot used to record as a per-team count; it's here
    // now, where it can also carry a name and a score.
    struct Slot
    {
        enum class Held
        {
            Ai,    // the AI is fielding this place
            Human, // a player holds it, alive or waiting to respawn
            // A player held it and has gone. Nobody stands here and nobody
            // will: the side gets a fresh slot for the soldier it's owed (see
            // ReleaseSlot), and this one stays behind as the record of what
            // that player did. It has to stay, or the side's score would drop
            // when somebody quit — the kills happened, and a scoreboard whose
            // rows stop adding up to the total at the top is a scoreboard
            // nobody can trust.
            Left,
        };

        int team;
        // What the slot is fielding. A human's is whatever class they are
        // currently holding; an AI slot's is dealt off the team's rotation and
        // is re-dealt every time the slot refills, so a squad wiped out comes
        // back with a different makeup and the same soldiers. Null on a slot
        // nothing has stood in yet — the five seconds between a side being owed
        // a soldier and getting one.
        const ClassDef* cls;
        // Who is standing here, as far as anybody watching is concerned. A
        // player's is what they asked to be called, cleaned to what a name may
        // be (PlayerName::Clean) by whoever let them in; a bot's is dealt off
        // the pool in the same file when the place is created.
        //
        // It belongs to the slot rather than to the soldier for the same reason
        // the kills do, and the bots are what make that visible: a slot's class
        // is re-dealt every life, so a name kept on the unit would rename the
        // row every five seconds and the board would be a list of strangers. A
        // name outlives the soldier wearing it, and a departed player's stays on
        // their row for the rest of the match, because a record with the name
        // taken off it is a row that can't be accounted for.
        //
        // Never empty on a slot anybody has been put in. Which of them are
        // people is not a question this string answers — Held does that — and
        // that is deliberate: the field draws these labels without asking.
        std::string name;
        Held held;
        // The player sitting at *this* machine, so a scoreboard can mark their
        // row. False for every slot on a dedicated server, and set on a client
        // from the snapshot rather than worked out locally — which slot is
        // yours is a fact the server holds.
        bool local;
        int kills;
        int deaths;
    };

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
        // kInvalidBody on a client replica: the shot is the server's to fly.
        Physics::BodyHandle body;
        float life;
        // Where the shot is, refreshed from the physics body each tick — or
        // from a snapshot, on a replica that has no body to ask. Everything
        // outside the simulation reads this rather than the physics world.
        Vector3 pos;
        Vector3 prevPos; // last tick's position, for swept hit tests
        // Where it was fired from, and the dead ground in front of that point:
        // a body closer to `origin` than `minRange` is one this round passes
        // straight through (WeaponDef::minRange). The pair rides on the shot
        // rather than being looked up off the shooter's class because the round
        // outlives the soldier — and outlives the class they were, since F11
        // can trade one for another while the bolt is still in the air.
        // minRange of 0 is every weapon but the sniper's, and costs the sweep
        // a compare.
        Vector3 origin;
        float minRange;
        // Who fired it, and so who is credited if it kills: the shooter's slot
        // rather than their unit, because a round outlives the soldier who
        // sent it and a grenade can outlive them by seconds. `team` is that
        // slot's side, carried alongside because the hit tests ask which side a
        // shot is on far more often than they ask whose it was.
        int owner;
        int team;        // whose shot this is; it only hurts the other side
        float damage;
        float radius;
        float blastRadius; // > 0: splash damage on impact
        bool fused;      // rides out `life` bouncing off the world instead of
                         // dying on its first contact; the fuse detonates it
        bool explodes;   // grenade: impact gets the big fireball, not a puff
        // What the round looks like in the air, and which way round to draw it.
        // Presentation only — nothing in the simulation reads either — but they
        // ride on the shot for the same reason `damage` does: the round outlives
        // the soldier who fired it and the class they were, and by the time it
        // is drawn there may be nobody left to ask.
        //
        // `yaw` is the heading it left the muzzle on rather than the one it is
        // travelling now, which are the same thing for everything that draws a
        // streak: those fly level and straight. The two that don't — the lobbed
        // shell and the bouncing grenade — are drawn as balls, and a ball has no
        // heading to be wrong about.
        TracerId tracer;
        float yaw;
    };

    // Builds the arena out of the level: the floor and colliders go into the
    // physics world, sight-blockers into the occluder list, spawn points into
    // the roster's geography. The level's models are somebody else's half.
    void Init(const LevelData& level);

    // Empties the match out of the arena: the roster, the shots (and their
    // physics bodies, where they're real), the reinforcement queue, every
    // human claim. The level stays — walls, floor, spawns — because leaving a
    // server doesn't unload the ground, and the next match starts on it.
    void Reset();

    // Puts both sides on the field at full strength: every slot is the AI's,
    // and humans claim them one at a time as they join (ClaimSlot). Nobody
    // sitting at the machine running this has a soldier in it — a player is
    // always a connection, even when the connection is to a server inside
    // their own process. Called once, when the match starts: nothing
    // simulates until then and there's nobody for a squad to fight.
    void StartMatch();

    // One fixed step of everything, in the order the systems have always run.
    // Whatever happened worth showing is appended to `events`. This is the
    // function a server's loop calls, and the only place simulation time
    // passes — a client's replica is never handed one.
    void Tick(std::vector<Event>& events);

    // The unit this machine's player is driving, or null while there isn't
    // one — before the first snapshot arrives, and for the length of the
    // respawn wait. Only ever set on a replica (see ApplySnapshot): the
    // simulation itself has no idea which of its soldiers somebody is
    // watching, and every system that sweeps the roster is better for it.
    Unit* Local();
    const Unit* Local() const;
    Unit* UnitById(int id);

    // --- The server's half of the surface. A connected player is three
    // calls: ClaimSlot when they pick a side, SpawnRemote when they take the
    // field, SetCommand every tick they're on it. None of it means anything
    // to a client or to solo play, and none of it is one line of special case
    // inside the simulation — a Remote unit is a unit.

    // Reserves one of `team`'s slots for a human and returns its index — the
    // handle the caller holds them by from here on, and the row they'll appear
    // on. `name` is what that player is called, cleaned before it gets here
    // (PlayerName::Clean); it's stamped on the slot rather than looked up later
    // because the slot outlives every soldier who stands in it and the player
    // who leaves halfway through. The AI gives the slot up immediately: a
    // living AI soldier standing in it is removed, quietly, no corpse — they
    // rotate out, they don't die of somebody connecting. The claim holds
    // through deaths and respawns, exactly like the local player's.
    //
    // A side with no slot left to give gets one more rather than a refusal.
    // Under the current door policy that can't happen — a server sends every
    // joiner to the emptier side and shuts the door at kTeamSize a side — and
    // the day some mode oversubscribes a team, an extra row on the scoreboard
    // is a better answer than a player with nowhere to stand.
    int ClaimSlot(int team, std::string name);
    // Hands a claimed slot back — a disconnect. The slot itself doesn't go
    // back into service: it becomes the leaver's record (Held::Left) and keeps
    // their name, their kills and their deaths for the rest of the match,
    // because a side's score is the sum of its slots and quitting must not take
    // points off the board.
    // The soldier the side is owed goes to a *fresh* slot instead, on the same
    // reinforcement clock a death starts — so a leaver's place refills the way
    // a casualty's does, not instantly, and the AI that fills it starts from
    // nothing rather than inheriting a stranger's tally.
    //
    // Slot indices are never reused or shifted for exactly this reason:
    // everything that outlives a tick holds one (a unit, a shot in the air, a
    // server session), and a roster that compacted itself would rename all of
    // them mid-match.
    void ReleaseSlot(int slot);
    // Human claims currently held on `team`, the local player's included.
    // Players who have left aren't holding anything, and aren't counted.
    int HumanSlots(int team) const;
    // The side `slot` is on, or -1 for a slot that doesn't exist. What a caller
    // holding a claim asks when it needs the team it ended up on.
    int SlotTeam(int slot) const;
    // Every place on both sides, in the order they were created: one team's
    // worth, then the next's, then whatever a departure or an oversubscribed
    // side has added since. This is the scoreboard — a presentation reads it
    // straight, and on a client it's the server's copy, delivered whole.
    const std::vector<Slot>& Roster() const { return m_roster; }
    int TeamCount() const { return static_cast<int>(m_teamSpawns.size()); }

    // Where `team` comes onto the field, and whether a point is near enough to
    // it to count as being there (kSpawnArea). Both ends ask: the server, to
    // decide whether a player may swap their soldier for another class, and the
    // client, to decide whether to offer it and where to draw the ring. The
    // spawns come off the level, which both of them loaded, so the two are
    // asking the same question of the same ground. Zero and false for a team
    // the level doesn't have.
    Vector3 TeamSpawn(int team) const;
    bool InSpawnArea(int team, const Vector3& pos) const;

    // Living soldiers on `team`. Counted off the roster when the roster is
    // whole; on a replica it's the number the server sent, because a fog-
    // filtered roster only holds what this client is allowed to see and the
    // corner panel is a scoreboard, not a wallhack.
    int Standing(int team) const;

    // Where a side is looking from, and how far: every living soldier on
    // `team` as an eye, appended to `out`. A side sees with all of its eyes at
    // once — what one soldier has line of sight to, the whole side has — so
    // this list, rather than one point, is what the fog of war and the
    // snapshot's filter are both built from, and building it here is what
    // keeps them building it the same way.
    //
    // Each eye carries its own reach, off the class standing behind it, so a
    // squad with a sniper in it genuinely knows more than the same squad
    // without one. That is also why the range can't be a parameter of the
    // asking any more: there is no one number a caller could pass that would
    // be true of the whole list.
    //
    // `except` leaves one soldier out by unit id, for the caller that has a
    // better answer for that one than the roster does: a viewer's own eye is
    // their predicted position on a client and their last standing place while
    // they're dead, and it goes in the list ahead of this call — with their own
    // class's reach on it, which is the one eye the roster can't supply.
    void TeamEyes(int team, std::vector<Visibility::Eye>& out, int except = -1) const;

    // --- The match: a clock, a score, and the one thing they decide.
    //
    // The clock runs down from kMatchLength on every Tick, and when it
    // reaches zero the match is over: the arena freezes where it stands,
    // nothing further is decided, and the side with the most kills has won.
    // What happens next is deliberately not this class's business — it can
    // count the intermission down, but it can't start the next match, because
    // starting one means telling somebody (a player, five connected clients)
    // that it started. Whoever called StartMatch calls it again.

    // Seconds left in the match; zero once it's over.
    float MatchTime() const { return m_matchTime; }
    // Seconds until the next match is due; zero while one is running. Only
    // meaningful alongside MatchOver.
    float Intermission() const { return m_intermission; }
    bool MatchOver() const { return m_matchOver; }
    // Kills credited to `team` this match — one per enemy soldier its fire
    // brought down. Zero for a team that doesn't exist, so a replica can be
    // asked about a side its snapshot hasn't mentioned.
    //
    // Summed off the roster rather than kept alongside it: a side's score is
    // exactly what the soldiers on it have done, and storing that a second time
    // is storing a way for the two to disagree. Ten slots, once a frame.
    int Score(int team) const;
    // The side that is ahead, or -1 when two or more are level at the top. A
    // draw is a real outcome of "most kills wins" rather than a tie broken by
    // something the players never saw.
    int Winner() const;

    // Puts a connected player's soldier on the field: `cls` in the slot they
    // claimed, at their side's spawn, full loadout, driven by whatever
    // SetCommand says from now on. Returns the unit's id — the name the server
    // and the wire know them by.
    int SpawnRemote(const ClassDef& cls, int slot);
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
    // Half-extents on x and z: the arena spans [-x, x] by [-z, z]. A pair
    // rather than a number because levels stopped being square.
    DirectX::SimpleMath::Vector2 ArenaHalf() const { return m_arenaHalf; }

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
    // from `from` at which the shot stops. Powers the aim indicator, which
    // draws that distance along the ground and never needs the arc's height.
    float PredictShotStop(const WeaponDef& weapon, const Vector3& from, const Vector3& dir,
                          float targetDist) const;

private:
    // A slot waiting to be filled again. Queued when an AI soldier dies and
    // cashed in kRespawnDelay later, which is the same wait the player serves —
    // a squad is five soldiers, and the only time it isn't is the moment after
    // one of them is killed.
    //
    // It's a queue rather than one timer per side because each death should
    // start its own clock: a team that loses three at once has three soldiers
    // coming back at the times they died, not one every five seconds while the
    // other side runs it down. It names the slot rather than the side because
    // the slot is what's empty — which is also what keeps a dead AI's record
    // attached to the soldier who replaces them.
    struct Reinforcement
    {
        int slot;
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
    // timer, and charges the weapon's early or empty price depending on
    // whether there was anything in it. A no-op if one is already running or
    // the magazine is full, so the reload key can be leaned on harmlessly.
    void BeginReload(Unit& unit);
    // Spends one of `attacker`'s melee charges on a swing through the arc in
    // front of them and strikes the nearest enemy standing in it, if any. The
    // charge goes whether or not it lands.
    void SwingMelee(Unit& attacker);
    // Fires `weapon`'s projectile from `from` along `dir`, on behalf of the
    // slot that pulled the trigger. Bullets always fly at full speed; lobbed
    // shots (grenades) shorten their toss to come down `targetDist` away, up to
    // the weapon's max range.
    void SpawnShot(const WeaponDef& weapon, const Vector3& from, const Vector3& dir, int owner,
                   float targetDist);
    // Deals the next class off the slot's team rotation and puts an AI soldier
    // in it. The class is re-dealt per life on purpose: an AI slot is a place
    // in the squad, not a character, and the squad's makeup is the rotation's
    // business.
    void SpawnAi(int slot);
    // What SpawnRemote is: a human's soldier — full loadout off the class
    // table, no brain woken — spawned into the slot they hold.
    int SpawnHuman(const ClassDef& cls, int slot, Unit::Controller controller);
    // The first slot on `team` the AI still holds and nobody is standing in, or
    // -1 if the side has none: what a joining player takes over. Preferring an
    // empty slot to a filled one is what lets ClaimSlot hand a player a place
    // without anyone having to rotate out. A departed player's slot is never
    // offered — it isn't a place any more, it's a record.
    int FreeAiSlot(int team) const;
    // Appends a fresh AI place on `team` and returns it. What a side gets when
    // it's owed a soldier but has nowhere to put one, which today means exactly
    // one thing: somebody left, and their slot stayed behind as their score.
    int AddAiSlot(int team);
    // A name for a place nobody is going to choose one for, off the pool in
    // PlayerName. Never one already on this roster while there's a spare, so
    // the ten soldiers in a match are ten different people and a player who
    // shares a name with a bot doesn't end up with a twin. Once every name in
    // the pool is spoken for it starts repeating rather than handing back
    // nothing: a soldier with a name shared with somebody across the map is
    // still better than a soldier with none.
    std::string PickBotName();
    // Whether anybody is currently standing in `slot`. A slot with no unit is a
    // side one soldier short: the AI's are refilled on the reinforcement clock,
    // a human's when their own respawn comes due.
    bool SlotFilled(int slot) const;
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
    // The whistle: stops the clock, starts the intermission, and sweeps the
    // rounds still in the air — they were fired at a match that no longer
    // has anything to decide, and leaving them would hang tracers in the
    // frozen arena for the length of the result.
    void EndMatch();
    // Ends `shot` at `pos`: splash damage for explosives, then the Detonation
    // event (which differs for a hit on `hitUnit` vs. world geometry).
    void Detonate(const Projectile& shot, const Vector3& pos, bool hitUnit);
    // Splash damage around `center`, full strength at the middle and falling
    // off to nothing at `radius`, on behalf of the slot that threw it. Only the
    // opposing side is hurt, and only where the blast has line of sight, so
    // cover still protects.
    void ApplyBlast(const Vector3& center, float radius, float damage, int owner);
    // Where `team`'s soldiers are trying to get to when they have nothing in
    // sight: the nearest other team's spawn. What a brain is handed as its
    // objective, and the only thing that makes a wander a push rather than a
    // random walk.
    Vector3 EnemySpawn(int team) const;
    // Clamps pos to the arena and pushes it out of solid colliders.
    void ResolveObstacles(Vector3& pos) const;
    // Writes one attributed line per shot when INFANTRY_SHOT_LOG names a file;
    // a no-op otherwise. See the definition for what the columns settle.
    void LogShot(const Vector3& from, int owner, float targetDist) const;
    float Rand(float lo, float hi);
    // Appends to the event list of the Tick in flight, or holds the event for
    // the next one when there's no Tick running. Almost everything here is said
    // from inside a tick and takes the first path; a spawn is the exception,
    // because a soldier is handed back by the server between steps (a join, a
    // respawn timer running out) and a soldier arriving is exactly the kind of
    // thing the room should hear.
    void Emit(const Event& ev);

    Physics m_physics;
    // Events said between ticks, waiting for one to carry them. Spliced in at
    // the head of the next Tick's list and cleared; emptied outright by Reset,
    // because a match that has ended has no news left to deliver.
    std::vector<Event> m_pending;
    int m_nextUnitId = 1; // 0 never issued, so an uninitialized id matches nobody
    // The slot this client's player holds for the match, or -1 on a world
    // running one — a server holds no claim of its own — and on a client
    // before it's been welcomed. Set from the snapshot, like everything else
    // on a replica.
    int m_localSlot = -1;
    // Commands staged for remote units, consumed whole by the next Tick.
    std::vector<std::pair<int, Command>> m_staged;
    // The server's per-team standing counts, held by a replica whose own
    // roster is fog-filtered. Empty on any world whose roster is the truth.
    std::vector<int> m_standingOverride;
    // The match: what's left of it, whether it's finished, and how long the
    // result has left to stand. On a replica all three arrive in the snapshot
    // rather than being counted here — the clock is the server's word, the same
    // as the standing counts above, and so is the roster the score comes off.
    float m_matchTime = kMatchLength;
    float m_intermission = 0.0f;
    bool m_matchOver = false;
    // Every place on the field for this match: who holds it, and what they've
    // done with it. Built by StartMatch, only ever added to (see ClaimSlot),
    // and on a client replaced wholesale by each snapshot.
    std::vector<Slot> m_roster;
    DirectX::SimpleMath::Vector2 m_arenaHalf = { 32.0f, 32.0f };
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
