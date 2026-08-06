#pragma once

#include "Command.h"
#include "PlayerClass.h"
#include "PlayerName.h"
#include "World.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
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
    // 8: sight has a range. A client from 7 draws its fog to the horizon, so
    // the ground an enemy has just vanished from would still be lit on its
    // screen — the same disagreement, in the other direction.
    // 9: that range is per class (ClassDef::sight) rather than one number for
    // everyone. Same bytes for the third time; what moved is how far each eye
    // in the list reaches. A client from 8 would light a sniper's ground to
    // thirty and cull at thirty the enemies its server sent it from
    // thirty-eight — drawing an emptier field than the one it is being told
    // about, and quietly making the class's whole point invisible.
    // 10: a reload has two prices, and the own-block carries which one is
    // being paid (SnapOwn::reloadSpan) because the magazine is emptied at the
    // start and nothing downstream could work it out otherwise. This one is
    // real bytes rather than a change of meaning — a client from 9 would read
    // the float as the grenade count and everything after it as rubbish.
    // 11: a class is no longer settled at the door. ChangeClass goes up and
    // ClassChanged comes back down, so a player standing on their own spawn —
    // or waiting out a respawn — can put down one soldier and pick up another.
    // Two new message types rather than a change to an old one, which is the
    // kind of break an older client survives by ignoring bytes it can't read;
    // the version still moves, because a client from 10 would sit there with a
    // key that does nothing and no way to be told why.
    // 12: soldiers can shout. A command carries what was said (Command::voice)
    // and an event carries it back down (Event::voice) — one byte on the end of
    // each record, which is the plainest kind of break there is. Both messages
    // pack their records nose to tail behind a count, so a build from 11 reads
    // the first one correctly and then takes a callout for the head of the next
    // command it wasn't expecting.
    // 13: a player is somebody rather than a class. The Join carries the name
    // they asked to be called and every roster row in the snapshot carries the
    // name of whoever is standing in it — the first strings this protocol has
    // ever moved, each a length byte and that many characters. Real bytes in
    // both directions: a build from 12 would take a slot's name length for the
    // next slot's team and read the rest of the board as rubbish.
    // 14: and a soldier says which row they're standing in (SnapUnit::slot),
    // which is the thread that lets a name be drawn under the body rather than
    // only on the board. One byte in the middle of the unit record, so a build
    // from 13 reads it as half a position and puts everybody somewhere else.
    // 15: two more things a tick can report. A soldier going onto the field is
    // an event now (Event::Type::Spawn), which lands in the middle of the type
    // enum rather than on the end of it, so every type after Death has moved a
    // number — a build from 14 would take a spawn for a detonation and read the
    // rest of the list one meaning out of step. And a Fire event says whether
    // what left was thrown (kEvThrown), which is a spare bit in a byte that was
    // already there and would go unread rather than misread.
    constexpr uint8_t kProtocolVersion = 15;
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
        Join,        // c->s, reliable: protocol version + uint8 classId + name
        Cmd,         // c->s, state: the player's recent Commands, newest last
        Welcome,     // s->c, reliable: your unit id and team
        Respawned,   // s->c, reliable: your fresh unit's id
        Snapshot,    // s->c, state: the visible world, this tick
        Events,      // s->c, state: what the tick did
        Reject,      // s->c, reliable: the door, closed, with a reason
        ChangeClass, // c->s, reliable: uint8 classId — asking to be somebody else
        // s->c, reliable: uint8 classId — and you are, from now on. A change
        // that also puts a fresh soldier on the field is followed by a
        // Respawned; one made by a player waiting to respawn is this alone, and
        // the soldier they're owed arrives as the class they just picked. A
        // refusal is silence: the client asks the same question of the same
        // ground before it sends, so the only way to be told no is to have
        // walked out of the spawn area in the time the packet took.
        ClassChanged,
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
        // A length byte and that many characters. Counted rather than
        // terminated because a reader that trusts a zero byte to arrive is a
        // reader that walks off the end of a truncated packet looking for one.
        void Str(std::string_view s)
        {
            const size_t n = std::min<size_t>(s.size(), 255);
            U8(static_cast<uint8_t>(n));
            if (n) // an empty view's data() is allowed to be null
                Raw(s.data(), n);
        }
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
        // The whole field is consumed whatever `max` is: a sender claiming a
        // longer string than the reader will keep is still a sender whose next
        // field starts after it, and stopping short would decode the remainder
        // of the packet against the wrong bytes. What's kept is the front of
        // it, cut to what the caller has room for.
        std::string Str(size_t max)
        {
            std::string out(U8(), '\0');
            Raw(out.data(), out.size());
            if (!ok)
                return {};
            if (out.size() > max)
                out.resize(max);
            return out;
        }
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
            // Not a bit, because it isn't one thing: the byte names which
            // callout, and kVoiceNone — the answer on all but a handful of
            // ticks in a match — is one of the values it can name.
            w.U8(cmd.voice);
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
            // Whatever the byte says, unchecked: what a callout number has to
            // be is the simulation's rule, and it's applied where the rest of
            // them are (World::ApplyCommand).
            entry.cmd.voice = r.U8();
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
        // Which roster row this soldier is standing in, or -1 for a soldier the
        // board the client was sent doesn't cover. It's here for one reason: a
        // name hangs on a slot, and without this a client holding both lists
        // has no way to say which of them belongs to the body it's about to
        // draw. Everything else about a slot is already down here in the
        // roster; this is the thread back to it.
        int slot;
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
        float reloadSpan = 0.0f; // what the running reload was charged, for the bar
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
        // Who is standing here, if it's anybody who has a name — a player's,
        // kept after they've gone, and empty for a slot the AI is fielding.
        // It rides in every snapshot rather than arriving once on a reliable
        // channel and being remembered, which is the same bet the rest of this
        // message makes: a full snapshot heals from any loss, a table of names
        // built up over a session is a table that can be one dropped update
        // wrong, and twelve characters a slot is not enough weight to trade
        // that away for. It costs about a tenth of what the soldiers do.
        std::string name;
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
    // the viewer's own, and one per living soldier on their side, each reaching
    // as far as the class behind it sees. This is the fog of war made real — a soldier
    // nobody on the receiving side can see isn't dimmed or skipped by their
    // renderer, they're absent from the bytes, so no amount of client
    // cleverness can find an enemy behind a wall or out in the dark. The test
    // is the same Visibility the fog is drawn with, from the same eyes and to
    // the same distance, which keeps the server's opinion of "hidden" and the
    // player's picture of it one opinion.
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
                                     const std::vector<Visibility::Eye>& eyes, int ownId,
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
            // Last, so the numbers stay nose to tail: the only variable-length
            // thing in the record is the one thing after which nothing has to
            // be found.
            w.Str(slot.name);
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
            // One byte, and 0xff for "not on the board you were sent" — which
            // is free as a sentinel because the roster above is cut at 255
            // rows, so index 255 is a row no client has ever been told about.
            w.U8(u.slot >= 0 && u.slot < 255 ? static_cast<uint8_t>(u.slot) : 0xff);
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
        w.F32(own->reloadSpan);
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
            // Cut to what a name may be, whatever the sender claimed: the
            // limit is the readout's, and the readout is on this side.
            slot.name = r.Str(PlayerName::kMaxLength);
            snap.roster.push_back(std::move(slot));
        }
        const int unitCount = r.U8();
        snap.units.reserve(unitCount);
        for (int i = 0; i < unitCount && r.ok; ++i)
        {
            SnapUnit u;
            u.id = r.I32();
            u.classId = r.U8();
            u.team = r.U8();
            const uint8_t slotId = r.U8();
            u.slot = slotId == 0xff ? -1 : slotId;
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
            snap.own.reloadSpan = r.F32();
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
        kEvThrown = 1 << 3,
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
            // And the callout as its own, with kVoiceNone for the events that
            // aren't one, which is all of them but Voice.
            w.U8(ev.voice);
            uint8_t bits = 0;
            if (ev.fatal) bits |= kEvFatal;
            if (ev.explodes) bits |= kEvExplodes;
            if (ev.thrown) bits |= kEvThrown;
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
            ev.voice = r.U8();
            const uint8_t bits = r.U8();
            ev.fatal = bits & kEvFatal;
            ev.explodes = bits & kEvExplodes;
            ev.hitUnit = bits & kEvHitUnit;
            ev.thrown = bits & kEvThrown;
            ev.local = ev.unit >= 0 && ev.unit == myUnitId;
            if (ev.type == Event::Type::AbilityStart && ev.cls)
                ev.sound = ev.cls->ability.startSound;
            // Same trade, off the other table: the callout arrived as an index
            // and the clip it plays is a pointer this side of the wire. A
            // number the table doesn't have leaves the sound null, which is a
            // presentation that plays nothing rather than one that reads off
            // the end of an array.
            if (ev.type == Event::Type::Voice && ev.voice < kVoiceCount)
                ev.sound = kVoiceDefs[ev.voice].sound;
            if (r.ok)
                out.push_back(ev);
        }
    }

    // --- The small reliable messages -----------------------------------------

    // The name goes up at the door and nowhere else: it's what a player is
    // called for the length of the connection, so the one moment it can be
    // stated is the moment the connection begins. Sent as the player typed it
    // (already cleaned on the way out of their settings) and cleaned again on
    // arrival, because between here and there is a client we don't own.
    inline void WriteJoin(Writer& w, uint8_t classId, std::string_view name)
    {
        w.U8(static_cast<uint8_t>(MsgType::Join));
        w.U8(kProtocolVersion);
        w.U8(classId);
        w.Str(name);
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

    inline void WriteChangeClass(Writer& w, uint8_t classId)
    {
        w.U8(static_cast<uint8_t>(MsgType::ChangeClass));
        w.U8(classId);
    }

    inline void WriteClassChanged(Writer& w, uint8_t classId)
    {
        w.U8(static_cast<uint8_t>(MsgType::ClassChanged));
        w.U8(classId);
    }
}
