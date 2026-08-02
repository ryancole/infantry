#pragma once

#include "Vertex.h"

#include <DirectXMath.h>
#include <SimpleMath.h>
#include <vector>

// The one thing a class can do that no other class can. Everything else a
// soldier carries is standard issue with the numbers moved around — the same
// shape of rifle, the same grenade, the same blade — so this is where a class
// stops being a stat line and starts being a job.
//
// One ability, on one key, on one clock. A soldier with three buttons is a
// soldier you have to study before you can play them, and the whole promise of
// picking one off a card is that you already know what you got.
//
// This file owns all of it: what an ability is, what its numbers mean, and what
// it actually does when the key goes down. That last part is why it exists as
// its own unit rather than as a struct in the class table. An ability's rules
// are not shaped like anything else in the game — the medic's dressing heals
// two people at different rates of nothing, picks a target every frame, and
// draws its own marks on the world — and every kind added will be differently
// shaped again. Left in Game::Update those rules become a switch in the update,
// a switch in the can-it-start test, a switch in the render, and a targeting
// function shaped entirely around whichever kind was written first. Here they
// are one function per question with the kinds inside them.
//
// It is deliberately split per ability and not per class. Two classes could
// share a kind — a second support class with a dressing shouldn't mean two
// copies of the healing — and the class table stays whole, because the value of
// that table is that four soldiers can be compared down a column. Balance is a
// property of the set.
//
// The boundary with the Game is drawn at knowledge, not at data: the Game
// answers who is on the field and who can be seen, since it owns the roster and
// the fog; the ability answers what happens to them. That's why Scene below
// hands over a flat list of soldiers rather than the roster, and why nothing in
// here has ever heard of a Unit, a team, or a wall.
namespace Ability
{
    using DirectX::SimpleMath::Vector3;

    enum class Kind
    {
        None,
        // Puts health back into the user and one other soldier at once, over
        // the ability's duration, delivered by the second rather than in a lump
        // at the end. See HealDef.
        Heal,
    };

    // What a heal is, as numbers. Split out of Def rather than sitting in it as
    // three more loose floats for the reason the class table splits WeaponDef
    // and MoveDef out of ClassDef: numbers that are one idea get a name and a
    // brace. It also puts a wall around the fields that only mean anything to
    // one kind — at a second kind this becomes a union of these, and the row it
    // belongs to is the only thing that says which arm is live.
    struct HealDef
    {
        float amount; // health restored over the full duration, to each soldier reached
        // How far the heal reaches somebody who isn't the user, and how far off
        // the aim direction still counts as being pointed at them. Same shape
        // as the blade's reach and arc, and read the same way — a cone in front
        // of the soldier — but at a range that crosses a room rather than one
        // that barely clears two bodies.
        float reach;
        float arc; // half-angle, radians
    };

    struct Def
    {
        Kind kind;
        const char* name; // uppercase, for the class card and the key hint
        float duration;   // seconds it runs for; must be > 0 for any kind but None
        // Measured from when the ability *ends*, not from when it started, so
        // being cut short costs the whole cooldown rather than a shortened one.
        // Nothing else in the loadout works this way, and it's why letting go of
        // an ability is a decision instead of a free look at it.
        float cooldown;
        // Clip played at the listener when it starts, or null for silence. It's
        // named here rather than chosen by kind so that the ability is one
        // entry in the table and not an entry plus a special case somewhere
        // else — and so two abilities of the same kind can still sound
        // different, which they should.
        const char* startSound;
        HealDef heal; // read when kind == Heal, meaningless otherwise
    };

    inline constexpr Def kNone = { Kind::None, "", 0.0f, 0.0f, nullptr, { 0.0f, 0.0f, 0.0f } };

    // The two clocks and what the last frame did, kept by whoever owns the
    // soldier. Only one of the clocks ever runs: the ability is doing its work,
    // or it's coming back.
    struct Runtime
    {
        float time = 0.0f;     // > 0 while it's running
        float cooldown = 0.0f; // > 0 while it's recharging
        // Where the ability reached somebody else on the frame that just ran,
        // for the mark drawn between them. A position and a flag rather than a
        // handle: whoever owns the roster is free to erase from it mid-frame,
        // and where they were standing is all the drawing needs.
        Vector3 reachedPos;
        bool reached = false;
    };

    // One soldier an ability can act on. Deliberately not the caller's own
    // soldier type: an ability doesn't care what a soldier is made of, only
    // about where they are and the health it's about to change. Keeping it to
    // those two facts once meant the player — then a different type from an
    // NPC — could be handed over on the same terms as anyone else; the roster
    // is one type now, and the boundary stays because it's about knowledge,
    // not convenience.
    struct Target
    {
        Vector3 pos;
        float* hp;
    };

    // What the ability is being used on and around. `allies` is everyone on the
    // user's side that the user can currently *see* — the caller applies its own
    // sight rules before handing the list over, which is why nothing in this
    // file knows what a wall is. The user is not in the list; they're `user`.
    struct Scene
    {
        Target user;
        Vector3 aimDir;
        float maxHp;
        const std::vector<Target>* allies;
    };

    // One frame of the ability: its clocks, its effect, and the two things the
    // player can do about it — ask for it (`requested`, a press edge), or take
    // it away by reaching for a weapon (`weaponUsed`). Every ordering decision
    // the ability makes is made in here, which is the point of it being one
    // call: the interesting part of an ability is when it starts and when it
    // stops, and that shouldn't be spread across its caller.
    //
    // Returns true on the frame it started, which is the one moment a caller
    // has anything to add — a noise, a nudge of the pad. Everything else about
    // a running ability can be read off the Runtime whenever it's wanted.
    bool Update(const Def& def, Runtime& rt, const Scene& scene, float dt, bool requested,
                bool weaponUsed);

    // Drops a running ability without charging for it. For the one way out
    // that isn't the player's decision: dying. A respawn hands back a fresh
    // loadout, so there's no cooldown to pay and nothing to measure it from.
    void Drop(Runtime& rt);

    // Whether the ability is ready to be asked for — off cooldown and not
    // already running. Says nothing about whether it would accomplish anything;
    // that's Update's business and depends on who's standing where.
    bool Ready(const Def& def, const Runtime& rt);

    // The ability's own marks on the world, appended as line pairs: what it's
    // doing and who it's reaching. Drawn from the runtime rather than the scene
    // so it shows what the last frame actually did, not what this one might.
    void AppendIndicator(const Def& def, const Runtime& rt, const Vector3& userPos,
                         std::vector<Vertex>& lines);

    // What the ability lets its owner see that nobody else does, appended as
    // triangles. This is a passive half — it costs nothing and runs whether or
    // not the ability is going — and it belongs with the kind rather than with
    // the class, because it's only worth seeing to someone who can act on it.
    // `screenRight` is the camera's right axis in world space, which is what
    // stands the marks square to the view.
    void AppendVision(const Def& def, const Scene& scene, const Vector3& screenRight,
                      std::vector<Vertex>& tris);
}
