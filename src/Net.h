#pragma once

#include "Command.h"
#include "PlayerClass.h"
#include "World.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// The wire. Everything the client and server say to each other is written
// down here, in both senses: the message shapes, and the bytes they become.
// Neither end owns this file more than the other, and nothing else in the
// game knows a byte order exists.
//
// The protocol is the seam the whole refactor was cut along, made literal.
// Upstream goes the Command — the same sentence ApplyCommand consumes in
// solo play. Downstream comes state and consequence: Snapshots, which say
// where everything is and are safe to lose because the next one supersedes
// them; and Events, which say what happened and are worth a little more care
// because a missed one is a corpse that never fell. Anything that must not
// be lost at all — joining, being welcomed, respawning — rides ENet's
// reliable channel instead of getting its own bookkeeping here.
//
// Encoding is by hand: little-endian scalars appended to a byte vector, no
// padding, no versioning, no cross-architecture ambitions. Two Windows
// machines on a LAN agree about float layout, and a schema library would be
// a dependency spent insuring against a machine that can't run the game
// anyway. What this costs is that every struct below has its writer and its
// reader three lines apart, and they have to match — which is also the whole
// file's virtue: when the protocol changes, this is the only place it can.
namespace Net
{
    using DirectX::SimpleMath::Vector3;

    constexpr uint16_t kPort = 27650;
    // Bumped whenever anything below changes shape. The version rides in the
    // Join, and a mismatch is refused outright — two builds disagreeing about
    // what a byte means should fail at the door, not decode each other into
    // nonsense mid-match.
    // 6: a command's walk is two body axes where it was a world direction.
    // Same two floats on the wire, which is exactly why this had to move —
    // shape here means what the bytes mean, and a build that missed the change
    // would decode a heading as a sidestep and walk off at right angles rather
    // than fail at anything.
    // 7: a snapshot's units are what the receiver's *side* can see, not what
    // they can see. Same bytes again, and again the meaning is the thing that
    // moved: an older client would take the squadmate its server just sent
    // from behind a wall and cull it back out with its own one-eyed test, so
    // the two builds would draw different fields off identical packets.
    constexpr uint8_t kProtocolVersion = 7;
    // Channel 0 carries the messages that must arrive (join, welcome,
    // respawn); channel 1 carries the streams that would rather be fresh
    // than complete (commands, snapshots, events).
    constexpr uint8_t kChannelReliable = 0;
    constexpr uint8_t kChannelState = 1;
    constexpr size_t kChannels = 2;

    // How many commands each Cmd packet carries: the newest, and this many
    // ticks of its predecessors. A lost packet's commands ride again in the
    // next two, so an edge — the one press of a grenade key — survives any
    // loss short of three consecutive packets, and the server's sequence
    // guard makes the duplicates free.
    constexpr size_t kCmdRedundancy = 3;

    enum class MsgType : uint8_t
    {
        Join,      // c->s, reliable: protocol version + uint8 classId
        Cmd,       // c->s, state: the player's recent Commands, newest last
        Welcome,   // s->c, reliable: your unit id and team
        Respawned, // s->c, reliable: your fresh unit's id
        Snapshot,  // s->c, state: the visible world, this tick
        Events,    // s->c, state: what the tick did
        Reject,    // s->c, reliable: the door, closed, with a reason
    };

    enum class RejectReason : uint8_t
    {
        Version, // different build of the game; nothing to negotiate
        Full,    // every slot is a human already
    };

    // --- Quantization ------------------------------------------------------
    // The snapshot's numbers, sized to what they can say. A position lands on
    // a 1/128-unit grid — under a hundredth of a soldier's width, well under
    // a pixel at gameplay zoom — and an angle on 1/32768th of a half-turn.
    // What is deliberately NOT quantized: commands (the client must predict
    // with exactly the bytes the server will apply, and floats make that
    // equality free) and the own-block's momentum (reconciliation replays
    // from it, and it deserves the full word).

    inline int16_t QPos(float v) { return static_cast<int16_t>(std::lround(v * 128.0f)); }
    inline float DqPos(int16_t v) { return static_cast<float>(v) / 128.0f; }

    inline int16_t QAngle(float rad) // [-pi, pi], atan2's own range
    {
        return static_cast<int16_t>(std::lround(rad * (32767.0f / 3.14159265f)));
    }
    inline float DqAngle(int16_t v) { return static_cast<float>(v) * (3.14159265f / 32767.0f); }

    inline uint16_t QPhase(float rad) // any angle, wrapped onto one cycle
    {
        constexpr float kTau = 6.2831853f;
        float wrapped = std::fmod(rad, kTau);
        if (wrapped < 0.0f)
            wrapped += kTau;
        return static_cast<uint16_t>(wrapped * (65535.0f / kTau));
    }
    inline float DqPhase(uint16_t v)
    {
        return static_cast<float>(v) * (6.2831853f / 65535.0f);
    }

    inline uint8_t QUnorm8(float v)
    {
        return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    }
    inline float DqUnorm8(uint8_t v) { return static_cast<float>(v) / 255.0f; }

    // --- Byte plumbing -----------------------------------------------------

    struct Writer
    {
        std::vector<uint8_t> bytes;

        void U8(uint8_t v) { bytes.push_back(v); }
        void I16(int16_t v) { Raw(&v, sizeof v); }
        void U16(uint16_t v) { Raw(&v, sizeof v); }
        void I32(int32_t v) { Raw(&v, sizeof v); }
        void U32(uint32_t v) { Raw(&v, sizeof v); }
        void F32(float v) { Raw(&v, sizeof v); }
        void Vec2XZ(const Vector3& v) { F32(v.x); F32(v.z); }
        void Vec3(const Vector3& v) { F32(v.x); F32(v.y); F32(v.z); }
        void Raw(const void* p, size_t n)
        {
            const uint8_t* b = static_cast<const uint8_t*>(p);
            bytes.insert(bytes.end(), b, b + n);
        }
    };

    // Reads run ahead of a length check on purpose: a short or garbled packet
    // turns every further read into zeros rather than a crash, and `ok` says
    // the message was nonsense so the caller can drop it whole. Hostile input
    // gets exactly this much ceremony and no more.
    struct Reader
    {
        const uint8_t* p;
        size_t left;
        bool ok = true;

        Reader(const void* data, size_t n) : p(static_cast<const uint8_t*>(data)), left(n) {}

        uint8_t U8() { uint8_t v = 0; Raw(&v, sizeof v); return v; }
        int16_t I16() { int16_t v = 0; Raw(&v, sizeof v); return v; }
        uint16_t U16() { uint16_t v = 0; Raw(&v, sizeof v); return v; }
        int32_t I32() { int32_t v = 0; Raw(&v, sizeof v); return v; }
        uint32_t U32() { uint32_t v = 0; Raw(&v, sizeof v); return v; }
        float F32() { float v = 0; Raw(&v, sizeof v); return v; }
        Vector3 Vec2XZ() { const float x = F32(); const float z = F32(); return { x, 0.0f, z }; }
        Vector3 Vec3() { const float x = F32(); const float y = F32(); const float z = F32(); return { x, y, z }; }
        void Raw(void* out, size_t n)
        {
            if (n > left)
            {
                ok = false;
                left = 0;
                return;
            }
            std::memcpy(out, p, n);
            p += n;
            left -= n;
        }
    };

    // --- Command (client -> server) ----------------------------------------

    enum CmdBits : uint8_t
    {
        kCmdFire = 1 << 0,
        kCmdMelee = 1 << 1,
        kCmdSteady = 1 << 2,
        kCmdReload = 1 << 3,
        kCmdGrenade = 1 << 4,
        kCmdAbility = 1 << 5,
    };

    // One numbered command: the tick-stamp on the client's own clock, and
    // what was asked for on it. The server applies each exactly once and
    // acks the highest it has consumed (SnapOwn::ackSeq), which is what lets
    // the client throw away confirmed history and replay only the commands
    // still in flight.
    struct CmdEntry
    {
        uint32_t seq;
        Command cmd;
    };

    // A Cmd packet is the newest command and up to kCmdRedundancy-1 of its
    // predecessors, oldest first. The tail is the loss insurance: every
    // command gets three chances to arrive, and the ones the server has
    // already seen cost it two comparisons each to ignore.
    inline void WriteCmds(Writer& w, const CmdEntry* entries, size_t count)
    {
        w.U8(static_cast<uint8_t>(MsgType::Cmd));
        w.U8(static_cast<uint8_t>(count));
        for (size_t i = 0; i < count; ++i)
        {
            const Command& cmd = entries[i].cmd;
            w.U32(entries[i].seq);
            // Two body axes, not a world direction — see Command.
            w.F32(cmd.move.x);
            w.F32(cmd.move.y);
            w.Vec2XZ(cmd.aim);
            w.F32(cmd.aimDist);
            uint8_t bits = 0;
            if (cmd.fire) bits |= kCmdFire;
            if (cmd.melee) bits |= kCmdMelee;
            if (cmd.steady) bits |= kCmdSteady;
            if (cmd.reload) bits |= kCmdReload;
            if (cmd.grenade) bits |= kCmdGrenade;
            if (cmd.ability) bits |= kCmdAbility;
            w.U8(bits);
        }
    }

    inline void ReadCmds(Reader& r, std::vector<CmdEntry>& out)
    {
        const size_t count = std::min<size_t>(r.U8(), kCmdRedundancy);
        for (size_t i = 0; i < count && r.ok; ++i)
        {
            CmdEntry entry;
            entry.seq = r.U32();
            entry.cmd.move.x = r.F32();
            entry.cmd.move.y = r.F32();
            entry.cmd.aim = r.Vec2XZ();
            entry.cmd.aimDist = r.F32();
            const uint8_t bits = r.U8();
            entry.cmd.fire = bits & kCmdFire;
            entry.cmd.melee = bits & kCmdMelee;
            entry.cmd.steady = bits & kCmdSteady;
            entry.cmd.reload = bits & kCmdReload;
            entry.cmd.grenade = bits & kCmdGrenade;
            entry.cmd.ability = bits & kCmdAbility;
            if (r.ok)
                out.push_back(entry);
        }
    }

    // --- Snapshot (server -> client) ----------------------------------------

    // One soldier as the wire sees them: enough to draw them and to read the
    // fight, and nothing the client has no business simulating. The aim
    // arrives as a heading because a unit vector on the ground plane is one
    // number pretending to be three.
    struct SnapUnit
    {
        int32_t id;
        uint8_t classId;
        uint8_t team;
        float posX, posZ;
        float aimYaw;
        float walkPhase;
        float moveBlend;
        float hp;
    };

    struct SnapProjectile
    {
        Vector3 pos;
        float radius;
        float life;
        bool fused;
    };

    // The receiving player's own loadout — the HUD's half of the snapshot —
    // plus the two things prediction reconciles against: the momentum the
    // server has for this soldier, and the newest command it has consumed.
    // Everyone else's magazine is their own business, so this block is the
    // one part of a snapshot built per client rather than shared.
    struct SnapOwn
    {
        bool has = false; // false while you're dead: nothing is in your hands
        int32_t id = -1;
        uint32_t ackSeq = 0; // commands up to here are history, not pending
        float moveVelX = 0.0f, moveVelZ = 0.0f;
        int32_t ammo = 0;
        float reloadTimer = 0.0f;
        uint8_t grenades = 0;
        uint8_t meleeCharges = 0;
        float meleeRecover = 0.0f;
        float meleeCooldown = 0.0f;
        float abilityTime = 0.0f;
        float abilityCooldown = 0.0f;
    };

    // One place on the roster as the wire sees it: who holds it and what
    // they've done with it. The client can't count any of this for itself — the
    // kills happen to soldiers it will never see through the fog — so the whole
    // board comes down, unfiltered, every snapshot. Departed players come down
    // with it: their rows are what make the columns add up to the totals.
    struct SnapSlot
    {
        uint8_t team;
        uint8_t classId;
        uint8_t held; // World::Slot::Held, as its own value
        bool you;     // the receiving player's own row
        uint16_t kills;
        uint16_t deaths;
    };

    // Who a slot is held by, in one byte, low two bits. `you` rides in the same
    // byte because it's the one thing about a slot that differs per recipient.
    constexpr uint8_t kSlotHeldMask = 0x03;
    constexpr uint8_t kSlotYou = 1 << 2;

    struct Snapshot
    {
        uint32_t tick = 0;
        // The match clock: seconds left of it, or — once `matchOver` — of the
        // wait before the next one. One field for two clocks because only one
        // of them is ever running, and the flag next to it says which.
        float clock = 0.0f;
        bool matchOver = false;
        // Living soldiers per team, counted before the fog filter below did
        // its work: the corner panel is a scoreboard, and a scoreboard that
        // only counted what you can see would say "outnumbered" every time
        // you were alone.
        std::vector<uint8_t> standing;
        // Every place on both sides. The thing the match is decided on lives in
        // here now — a team's score is the sum of its slots' kills — so it
        // travels on the same terms the standing counts do and is filtered by
        // nothing: both sides' scores are known to both sides.
        std::vector<SnapSlot> roster;
        std::vector<SnapUnit> units;
        std::vector<SnapProjectile> projectiles;
        SnapOwn own;
    };

    // One client's snapshot: the fight as seen by `ownTeam`, through `eyes` —
    // the viewer's own, and one per living soldier on their side. This is the
    // fog of war made real — a soldier nobody on the receiving side can see
    // isn't dimmed or skipped by their renderer, they're absent from the bytes,
    // so no amount of client cleverness can find an enemy behind a wall. The
    // test is the same Visibility the fog is drawn with, from the same eyes,
    // which keeps the server's opinion of "hidden" and the player's picture of
    // it one opinion.
    //
    // Two things skip the filter outright. The viewer's own soldier, because
    // they are the eye. And every soldier on their side, because a squad that
    // blinks out of existence when it turns a corner is a squad nobody can
    // fight alongside — where your side is standing is the one thing the fog
    // was never keeping from you. Per-team standing counts go out unfiltered
    // too, because the scoreboard is meant to be known.
    //
    // Built per client rather than shared, which is what buys the filter; at
    // ten soldiers a match the redundant serialization is nothing. `ownSlot` is
    // the place this client holds — the one thing about the roster that differs
    // between recipients, and the reason it's a parameter rather than something
    // the World could be asked.
    inline void WriteSnapshotVisible(Writer& w, const World& world, uint32_t tick,
                                     const std::vector<DirectX::XMFLOAT2>& eyes, int ownId,
                                     int ownTeam, int ownSlot)
    {
        w.U8(static_cast<uint8_t>(MsgType::Snapshot));
        w.U32(tick);

        // The match: the clock in tenths of a second (a countdown wants to
        // look continuous, not to be accurate to the frame — and fifteen
        // minutes of tenths still fits a uint16 with room to spare), and
        // whether it has run out.
        const float clock = world.MatchOver() ? world.Intermission() : world.MatchTime();
        w.U16(static_cast<uint16_t>(std::clamp(std::lround(clock * 10.0f), 0l, 65535l)));
        w.U8(world.MatchOver() ? 1 : 0);

        w.U8(static_cast<uint8_t>(world.TeamCount()));
        for (int team = 0; team < world.TeamCount(); ++team)
            w.U8(static_cast<uint8_t>(world.Standing(team)));

        // The scoreboard, whole. A slot that has never been filled has no class
        // to name yet, which rides as the same 0xff a classless event uses.
        const std::vector<World::Slot>& roster = world.Roster();
        w.U8(static_cast<uint8_t>(std::min<size_t>(roster.size(), 255)));
        for (size_t i = 0; i < roster.size() && i < 255; ++i)
        {
            const World::Slot& slot = roster[i];
            w.U8(static_cast<uint8_t>(slot.team));
            w.U8(slot.cls ? static_cast<uint8_t>(slot.cls - kClassDefs) : 0xff);
            uint8_t bits = static_cast<uint8_t>(slot.held) & kSlotHeldMask;
            if (static_cast<int>(i) == ownSlot) bits |= kSlotYou;
            w.U8(bits);
            w.U16(static_cast<uint16_t>(std::clamp(slot.kills, 0, 65535)));
            w.U16(static_cast<uint16_t>(std::clamp(slot.deaths, 0, 65535)));
        }

        const auto visible = [&](float x, float z) {
            return Visibility::IsPointVisibleAny(eyes, { x, z }, world.Occluders());
        };
        const auto sendUnit = [&](const Unit& u) {
            return u.id == ownId || u.team == ownTeam || visible(u.pos.x, u.pos.z);
        };

        uint8_t unitCount = 0;
        for (const Unit& u : world.Units())
            if (sendUnit(u))
                ++unitCount;
        w.U8(unitCount);
        for (const Unit& u : world.Units())
        {
            if (!sendUnit(u))
                continue;
            w.I32(u.id);
            w.U8(static_cast<uint8_t>(u.cls - kClassDefs)); // index into the one table
            w.U8(static_cast<uint8_t>(u.team));
            w.I16(QPos(u.pos.x));
            w.I16(QPos(u.pos.z));
            w.I16(QAngle(std::atan2(u.aimDir.z, u.aimDir.x)));
            w.U16(QPhase(u.walkPhase));
            w.U8(QUnorm8(u.moveBlend));
            // Ceiling, not rounding: a soldier at half a point of health is
            // wounded, not a corpse, and their bar shouldn't say otherwise.
            w.U8(static_cast<uint8_t>(std::clamp(std::ceil(u.hp), 0.0f, 255.0f)));
        }

        uint8_t shotCount = 0;
        for (const World::Projectile& shot : world.Projectiles())
            if (visible(shot.pos.x, shot.pos.z))
                ++shotCount;
        w.U8(shotCount);
        for (const World::Projectile& shot : world.Projectiles())
        {
            if (!visible(shot.pos.x, shot.pos.z))
                continue;
            w.I16(QPos(shot.pos.x));
            w.I16(QPos(shot.pos.y));
            w.I16(QPos(shot.pos.z));
            w.U8(static_cast<uint8_t>(std::lround(shot.radius * 64.0f)));
            w.U16(static_cast<uint16_t>(std::clamp(std::lround(shot.life * 256.0f), 0l, 65535l)));
            w.U8(shot.fused ? 1 : 0);
        }
    }

    inline void WriteSnapshotOwn(Writer& w, const Unit* own, uint32_t ackSeq)
    {
        w.U8(own ? 1 : 0);
        if (!own)
            return;
        w.I32(own->id);
        w.U32(ackSeq);
        w.F32(own->moveVel.x);
        w.F32(own->moveVel.z);
        w.I32(own->ammo);
        w.F32(own->reloadTimer);
        w.U8(static_cast<uint8_t>(own->grenades));
        w.U8(static_cast<uint8_t>(own->meleeCharges));
        w.F32(own->meleeRecover);
        w.F32(own->meleeCooldown);
        w.F32(own->ability.time);
        w.F32(own->ability.cooldown);
    }

    inline Snapshot ReadSnapshot(Reader& r)
    {
        Snapshot snap;
        snap.tick = r.U32();
        snap.clock = static_cast<float>(r.U16()) / 10.0f;
        snap.matchOver = r.U8() != 0;
        const int teamCount = r.U8();
        for (int i = 0; i < teamCount && r.ok; ++i)
            snap.standing.push_back(r.U8());
        const int slotCount = r.U8();
        snap.roster.reserve(slotCount);
        for (int i = 0; i < slotCount && r.ok; ++i)
        {
            SnapSlot slot;
            slot.team = r.U8();
            slot.classId = r.U8();
            const uint8_t bits = r.U8();
            slot.held = bits & kSlotHeldMask;
            slot.you = (bits & kSlotYou) != 0;
            slot.kills = r.U16();
            slot.deaths = r.U16();
            snap.roster.push_back(slot);
        }
        const int unitCount = r.U8();
        snap.units.reserve(unitCount);
        for (int i = 0; i < unitCount && r.ok; ++i)
        {
            SnapUnit u;
            u.id = r.I32();
            u.classId = r.U8();
            u.team = r.U8();
            u.posX = DqPos(r.I16());
            u.posZ = DqPos(r.I16());
            u.aimYaw = DqAngle(r.I16());
            u.walkPhase = DqPhase(r.U16());
            u.moveBlend = DqUnorm8(r.U8());
            u.hp = static_cast<float>(r.U8());
            snap.units.push_back(u);
        }
        const int shotCount = r.U8();
        snap.projectiles.reserve(shotCount);
        for (int i = 0; i < shotCount && r.ok; ++i)
        {
            SnapProjectile s;
            s.pos.x = DqPos(r.I16());
            s.pos.y = DqPos(r.I16());
            s.pos.z = DqPos(r.I16());
            s.radius = static_cast<float>(r.U8()) / 64.0f;
            s.life = static_cast<float>(r.U16()) / 256.0f;
            s.fused = r.U8() != 0;
            snap.projectiles.push_back(s);
        }
        snap.own.has = r.U8() != 0;
        if (snap.own.has)
        {
            snap.own.id = r.I32();
            snap.own.ackSeq = r.U32();
            snap.own.moveVelX = r.F32();
            snap.own.moveVelZ = r.F32();
            snap.own.ammo = r.I32();
            snap.own.reloadTimer = r.F32();
            snap.own.grenades = r.U8();
            snap.own.meleeCharges = r.U8();
            snap.own.meleeRecover = r.F32();
            snap.own.meleeCooldown = r.F32();
            snap.own.abilityTime = r.F32();
            snap.own.abilityCooldown = r.F32();
        }
        return snap;
    }

    // --- Events (server -> client) ------------------------------------------

    enum EventBits : uint8_t
    {
        kEvFatal = 1 << 0,
        kEvExplodes = 1 << 1,
        kEvHitUnit = 1 << 2,
    };

    inline void WriteEvents(Writer& w, const std::vector<Event>& events)
    {
        w.U8(static_cast<uint8_t>(MsgType::Events));
        w.U8(static_cast<uint8_t>(events.size()));
        for (const Event& ev : events)
        {
            w.U8(static_cast<uint8_t>(ev.type));
            w.I32(ev.unit);
            w.Vec3(ev.pos);
            w.Vec3(ev.dir);
            w.Vec3(ev.knock);
            w.F32(ev.damage);
            w.F32(ev.walkPhase);
            w.F32(ev.moveBlend);
            w.F32(ev.radius);
            w.U8(static_cast<uint8_t>(ev.team));
            // The class rides as its table index; -1 is "no class involved".
            w.U8(ev.cls ? static_cast<uint8_t>(ev.cls - kClassDefs) : 0xff);
            uint8_t bits = 0;
            if (ev.fatal) bits |= kEvFatal;
            if (ev.explodes) bits |= kEvExplodes;
            if (ev.hitUnit) bits |= kEvHitUnit;
            w.U8(bits);
        }
    }

    // `myUnitId` is how the receiving end reclaims the two facts the wire
    // dropped: whether an event is about *this* player (local), and what an
    // ability start should sound like (off the class it arrived with).
    inline void ReadEvents(Reader& r, int32_t myUnitId, std::vector<Event>& out)
    {
        const int count = r.U8();
        for (int i = 0; i < count && r.ok; ++i)
        {
            Event ev;
            ev.type = static_cast<Event::Type>(r.U8());
            ev.unit = r.I32();
            ev.pos = r.Vec3();
            ev.dir = r.Vec3();
            ev.knock = r.Vec3();
            ev.damage = r.F32();
            ev.walkPhase = r.F32();
            ev.moveBlend = r.F32();
            ev.radius = r.F32();
            ev.team = r.U8();
            const uint8_t clsId = r.U8();
            ev.cls = clsId < kClassCount ? &kClassDefs[clsId] : nullptr;
            const uint8_t bits = r.U8();
            ev.fatal = bits & kEvFatal;
            ev.explodes = bits & kEvExplodes;
            ev.hitUnit = bits & kEvHitUnit;
            ev.local = ev.unit >= 0 && ev.unit == myUnitId;
            if (ev.type == Event::Type::AbilityStart && ev.cls)
                ev.sound = ev.cls->ability.startSound;
            if (r.ok)
                out.push_back(ev);
        }
    }

    // --- The small reliable messages -----------------------------------------

    inline void WriteJoin(Writer& w, uint8_t classId)
    {
        w.U8(static_cast<uint8_t>(MsgType::Join));
        w.U8(kProtocolVersion);
        w.U8(classId);
    }

    inline void WriteReject(Writer& w, RejectReason why)
    {
        w.U8(static_cast<uint8_t>(MsgType::Reject));
        w.U8(static_cast<uint8_t>(why));
    }

    inline void WriteWelcome(Writer& w, int32_t unitId, uint8_t team)
    {
        w.U8(static_cast<uint8_t>(MsgType::Welcome));
        w.I32(unitId);
        w.U8(team);
    }

    inline void WriteRespawned(Writer& w, int32_t unitId)
    {
        w.U8(static_cast<uint8_t>(MsgType::Respawned));
        w.I32(unitId);
    }
}
