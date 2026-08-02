#pragma once

#include <DirectXMath.h>
#include <SimpleMath.h>
#include <random>
#include <vector>

// How a computer-controlled soldier decides what to do, and nothing about how
// it gets done.
//
// The split is body and brain. The body — Game::UpdateUnits — is everything
// every soldier does the same way: running a reload down, working out who can
// be seen, turning, walking without walking into a wall, keeping a squad from
// collapsing into one column, spending a round and starting the cadence timer.
// The brain is the part that differs: given what a soldier can see, what does
// this one want. It answers in Intent and touches nothing.
//
// That seam exists so bespoke classes don't each arrive with their own copy of
// walking. A marksman that holds a lane, a support that follows the wounded and
// a bombardier that lobs from cover are three small answers to the same
// question, and every one of them wants the same legs underneath it.
//
// Split per behavior rather than per class, for the reason abilities were: two
// classes can share a brain, and when the question is "how do support soldiers
// behave" the answer should be in one file rather than spread across every
// class that happens to be one. The class table names which brain a class
// thinks with, the same way it names which ability it carries.
//
// Nothing here has heard of a ClassDef, a team, a weapon or a wall. What a
// soldier can see arrives already gathered; how fast they can act on it is
// decided by whoever owns their body. A brain that needed the class table would
// be a brain that had started doing the body's job.
namespace Brain
{
    using DirectX::SimpleMath::Vector3;

    enum class Kind
    {
        // Closes to a comfortable range, circles at it, and shoots whatever it
        // can see; wanders when it can't see anything. What every NPC in the
        // game does today, and — like Ability::Kind::None on three of the four
        // classes — that's a statement about where the prototype is rather than
        // a placeholder. It's also the right default for a class that hasn't
        // earned a mind of its own yet.
        Rifleman,
    };

    // One enemy a soldier can see. The distance comes along because whoever
    // gathered the list had to measure it anyway, and every brain wants it.
    struct Contact
    {
        Vector3 pos;
        float dist;
    };

    // What a soldier knows this frame. `enemies` is everyone hostile they can
    // actually see, gathered by the body — which is where the sight rules live,
    // since a brain has no business knowing what a wall is. It's unsorted and
    // may be longer than any brain cares about: how far out to engage is a
    // personality, not a fact about the world.
    struct Senses
    {
        Vector3 pos;
        Vector3 aimDir; // where they're pointed right now
        const std::vector<Contact>* enemies;
        float arenaHalf; // how far out there is anywhere to walk to
        bool canFire;    // loaded and off the cadence: a brain may want to act on being empty
    };

    // What the soldier wants. Directions are unit vectors or zero; nothing here
    // is in units per second, because how fast this particular soldier travels
    // is the body's business and the class table's.
    struct Intent
    {
        Vector3 move;              // where to walk, or zero to stand still
        float speedScale = 1.0f;   // fraction of the class's top speed
        Vector3 facing;            // where to point, or zero to leave it alone
        bool fire = false;         // pull the trigger, if the body allows it
        float fireDist = 0.0f;     // how far off what it's shooting at is, so a lob lands on them
    };

    // What a brain remembers between frames. Held by whoever owns the soldier,
    // because that's what it belongs to — a brain is a function, and this is
    // the only thing about it that persists.
    struct Memory
    {
        Vector3 wanderTarget;
        float repickTimer = 0.0f; // 0 picks a real target on the first tick
        float strafeSign = 1.0f;  // +1/-1: which way to circle
        float strafeTimer = 0.0f;
    };

    // Sets up a fresh mind. Exists for the small amount of randomness that
    // stops a squad spawning as one animal, all circling the same way at the
    // same moment.
    void Wake(Memory& mem, const Vector3& pos, std::mt19937& rng);

    // One soldier's decision for this frame.
    Intent Think(Kind kind, Memory& mem, const Senses& senses, float dt, std::mt19937& rng);
}
