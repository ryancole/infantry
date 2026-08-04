#pragma once

#include <cstddef>
#include <cstdint>

// What a soldier can shout, and the only place any of it is written down.
//
// A callout is not a message. It reaches no chat window, it is addressed to
// nobody, and it is not delivered to a team: it is a noise a soldier makes at
// the place they are standing, and it travels the way every other noise in this
// game travels — by earshot, to whoever is near enough, on both sides of the
// fight. That is the whole design and it is deliberate. Shouting for a medic
// tells your squad you're hurt and where; it tells the soldier on the other
// side of the trench the same thing. Being heard by the wrong people is what
// being heard by the right ones costs, and it is what makes the decision to
// shout a decision at all. A team-only callout would be a radio, and a radio is
// a different feature with a different name.
//
// It follows that nothing here is a game rule. A callout heals nobody, marks
// nobody, and obliges nobody: the medic who answers it is a person who heard it
// and decided to come. All the simulation does is agree that a sound happened
// at a spot, which is the least it can do and still have both ends of the wire
// hear the same thing.
//
// The zone this game comes from put its callouts on F5-F8 alongside a macro
// system that could paste map coordinates into them. This is the first of them
// and the plainest, and where it carries is already answered by where it was
// said — the sound arrives from a direction, which is the thing a coordinate
// was standing in for on a screen that couldn't pan.
enum class VoiceId : uint8_t
{
    Medic,
    Count
};

inline constexpr size_t kVoiceCount = static_cast<size_t>(VoiceId::Count);

// Nothing said — which is what nearly every tick of a match has to say. It
// rides in the same byte as the callout itself, the way an event with no class
// in it rides as 0xff, rather than as a flag beside a value that would then
// have two ways to mean silence.
inline constexpr uint8_t kVoiceNone = 0xff;

struct VoiceDef
{
    const char* name;  // uppercase, for the key-cap row and the binds screen
    const char* sound; // the wave bank entry it plays
};

inline constexpr VoiceDef kVoiceDefs[kVoiceCount] = {
    { "MEDIC", "medic" },
};

// How long a soldier's mouth is busy for. There is no other price on a callout
// — it takes no hand off the trigger and costs no part of the kit — so this is
// the only thing standing between the feature and a player who holds the key
// down. Three seconds is longer than it takes to say and shorter than it takes
// for a squadmate to cross to you, so calling twice while you wait is still
// allowed and calling ten times is not. It's enforced in the simulation rather
// than on the key, because the key is on a machine we don't own.
inline constexpr float kVoiceCooldown = 3.0f;
