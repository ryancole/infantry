#pragma once

#include "Voice.h"

#include <DirectXMath.h>
#include <SimpleMath.h>

// Everything a soldier's driver may say to the simulation about one slice of
// time, and the only way to say it.
//
// The controls used to be consumed where they were read: the movement block
// asked the bindings about W, the firing block asked about the trigger, and
// the answer went straight into the world. That's fine right up until the
// simulation has to run somewhere the keyboard isn't — and that is the whole
// project now. A command is the seam: the client owns everything above it
// (bindings, cursor, camera, the screen-relative arithmetic), the simulation
// owns everything below it (speeds, turn rates, cooldowns, what a press is
// allowed to mean), and what crosses is plain data that could as easily have
// arrived over a socket. The AI's Brain::Intent is this same idea for minds;
// a command is it for hands.
//
// The aim arrives in world space, already resolved: which way the camera faces
// is a fact about this machine's screen, and the simulation has no business
// knowing there is a screen. The walk doesn't arrive resolved, for the
// opposite reason: it isn't screen-relative at all. Forward is wherever the
// soldier is currently pointed, and where that is on any given tick is the
// simulation's own business — the body swings toward the cursor at a turn rate
// only it knows. So the walk crosses as the two body axes and gets resolved
// down there against the facing of the moment. Nothing here is in units per
// second either — a command says what's wanted, and what that costs is the
// class table's side of the conversation.
//
// A tick number joins these fields when the fixed tick lands: "what you did"
// is only half a sentence until the simulation can say when.
struct Command
{
    using Vector2 = DirectX::SimpleMath::Vector2;
    using Vector3 = DirectX::SimpleMath::Vector3;

    // Where to walk, in the soldier's axes and not the world's: y runs along
    // the facing — forward, or negative to back up — and x runs across it, a
    // strafe to the right. Zero stands still. Length beyond one is the
    // simulation's to trim, so nobody walks faster by asking louder.
    Vector2 move;
    Vector3 aim; // where to come around to point, unit length, or zero to leave it
    // Distance to the aimed-at ground point, so a lobbed shot can fall on it
    // rather than at max range. Huge means there is no point, only a direction
    // — the stick aims that way, and it reads as "past everything".
    float aimDist = 1e9f;

    // Held controls: true for as long as the driver holds them.
    bool fire = false;   // the trigger
    bool melee = false;  // the blade; held like the trigger, see Game's melee notes
    bool steady = false; // the posture: slow walk, much slower turn

    // Edge controls: true on the slice the key went down, then spent. The
    // distinction is the driver's to honor because only the driver has edges
    // to detect — the simulation just believes what it's told, which is all it
    // could do about a remote player's keyboard anyway.
    bool reload = false;
    bool grenade = false;
    bool ability = false;
    // Something shouted on this slice (Voice.h), or kVoiceNone — which is what
    // nearly every command says. An edge like the three above, and one of them
    // in spirit: the reason it isn't a fourth bool is that there will be more
    // than one thing to shout, and a byte that names which is a byte the wire
    // was going to have to carry anyway.
    uint8_t voice = kVoiceNone;
};
