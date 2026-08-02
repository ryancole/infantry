#pragma once

#include "Command.h"
#include "PlayerClass.h"
#include "World.h"

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
    // Channel 0 carries the messages that must arrive (join, welcome,
    // respawn); channel 1 carries the streams that would rather be fresh
    // than complete (commands, snapshots, events).
    constexpr uint8_t kChannelReliable = 0;
    constexpr uint8_t kChannelState = 1;
    constexpr size_t kChannels = 2;

    enum class MsgType : uint8_t
    {
        Join,      // c->s, reliable: uint8 classId
        Cmd,       // c->s, state: the player's Command for the next tick(s)
        Welcome,   // s->c, reliable: your unit id and team
        Respawned, // s->c, reliable: your fresh unit's id
        Snapshot,  // s->c, state: the whole visible world, this tick
        Events,    // s->c, state: what the tick did
    };

    // --- Byte plumbing -----------------------------------------------------

    struct Writer
    {
        std::vector<uint8_t> bytes;

        void U8(uint8_t v) { bytes.push_back(v); }
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

    inline void WriteCmd(Writer& w, const Command& cmd)
    {
        w.U8(static_cast<uint8_t>(MsgType::Cmd));
        w.Vec2XZ(cmd.move);
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

    inline Command ReadCmd(Reader& r)
    {
        Command cmd;
        cmd.move = r.Vec2XZ();
        cmd.aim = r.Vec2XZ();
        cmd.aimDist = r.F32();
        const uint8_t bits = r.U8();
        cmd.fire = bits & kCmdFire;
        cmd.melee = bits & kCmdMelee;
        cmd.steady = bits & kCmdSteady;
        cmd.reload = bits & kCmdReload;
        cmd.grenade = bits & kCmdGrenade;
        cmd.ability = bits & kCmdAbility;
        return cmd;
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

    // The receiving player's own loadout — the HUD's half of the snapshot.
    // Everyone else's magazine is their own business, so this block is the
    // one part of a snapshot built per client rather than shared.
    struct SnapOwn
    {
        bool has = false; // false while you're dead: nothing is in your hands
        int32_t id = -1;
        int32_t ammo = 0;
        float reloadTimer = 0.0f;
        uint8_t grenades = 0;
        uint8_t meleeCharges = 0;
        float meleeRecover = 0.0f;
        float meleeCooldown = 0.0f;
        float abilityTime = 0.0f;
        float abilityCooldown = 0.0f;
    };

    struct Snapshot
    {
        uint32_t tick = 0;
        std::vector<SnapUnit> units;
        std::vector<SnapProjectile> projectiles;
        SnapOwn own;
    };

    // The shared body of a snapshot: everything every client gets alike.
    // Split from the own-block so the server serializes the roster once per
    // tick, not once per client.
    inline void WriteSnapshotShared(Writer& w, const World& world, uint32_t tick)
    {
        w.U8(static_cast<uint8_t>(MsgType::Snapshot));
        w.U32(tick);
        w.U8(static_cast<uint8_t>(world.Units().size()));
        for (const Unit& u : world.Units())
        {
            w.I32(u.id);
            w.U8(static_cast<uint8_t>(u.cls - kClassDefs)); // index into the one table
            w.U8(static_cast<uint8_t>(u.team));
            w.F32(u.pos.x);
            w.F32(u.pos.z);
            w.F32(std::atan2(u.aimDir.z, u.aimDir.x));
            w.F32(u.walkPhase);
            w.F32(u.moveBlend);
            w.F32(u.hp);
        }
        w.U8(static_cast<uint8_t>(world.Projectiles().size()));
        for (const World::Projectile& shot : world.Projectiles())
        {
            w.Vec3(shot.pos);
            w.F32(shot.radius);
            w.F32(shot.life);
            w.U8(shot.fused ? 1 : 0);
        }
    }

    inline void WriteSnapshotOwn(Writer& w, const Unit* own)
    {
        w.U8(own ? 1 : 0);
        if (!own)
            return;
        w.I32(own->id);
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
        const int unitCount = r.U8();
        snap.units.reserve(unitCount);
        for (int i = 0; i < unitCount && r.ok; ++i)
        {
            SnapUnit u;
            u.id = r.I32();
            u.classId = r.U8();
            u.team = r.U8();
            u.posX = r.F32();
            u.posZ = r.F32();
            u.aimYaw = r.F32();
            u.walkPhase = r.F32();
            u.moveBlend = r.F32();
            u.hp = r.F32();
            snap.units.push_back(u);
        }
        const int shotCount = r.U8();
        snap.projectiles.reserve(shotCount);
        for (int i = 0; i < shotCount && r.ok; ++i)
        {
            SnapProjectile s;
            s.pos = r.Vec3();
            s.radius = r.F32();
            s.life = r.F32();
            s.fused = r.U8() != 0;
            snap.projectiles.push_back(s);
        }
        snap.own.has = r.U8() != 0;
        if (snap.own.has)
        {
            snap.own.id = r.I32();
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
        w.U8(classId);
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
