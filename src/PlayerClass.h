#pragma once

#include <DirectXMath.h>
#include <cstddef>

// The soldier archetypes a player must pick from before spawning. Pure data,
// so the selection screen and gameplay code share one source of truth. Stats
// only cover what the prototype simulates today (movement + projectiles);
// health, abilities, and equipment slots layer on once those systems exist.
enum class ClassId
{
    Marine,
    Medic,
    Sniper,
    Grenadier,
};

inline constexpr size_t kClassCount = 4;

// One weapon's projectile behavior, split out of ClassDef so the grenade every
// soldier carries can be described by the same data as a class's primary and
// fired through the same code path.
struct WeaponDef
{
    float fireInterval;      // seconds between shots
    float projectileSpeed;   // horizontal muzzle speed
    float projectileRadius;
    float projectileMass;
    float lobVelocity;       // upward muzzle speed; > 0 arcs the shot under gravity
    float projectileLife;    // seconds before despawn; with bounce > 0, the fuse
    float damage;            // health removed by a direct hit
    float blastRadius;       // > 0: splash damage out to here, falling off to nothing
    // Restitution, i.e. the fraction of impact speed a bounce keeps. Above 0
    // the shot also stops dying on contact with the world: it ricochets and
    // rolls until projectileLife runs out, and that fuse is what sets it off.
    // A direct hit on a soldier still detonates it on the spot.
    float bounce;
    bool explodes;           // impact plays the explosion effect + boom, not a thud
};

struct ClassDef
{
    const char* name;  // uppercase: the debug line font has no lowercase
    const char* blurb;
    DirectX::XMFLOAT4 color; // player body + UI accent
    float moveSpeed;         // units per second
    WeaponDef primary;
};

inline constexpr ClassDef kClassDefs[kClassCount] = {
    // name         blurb              color                            move  | fire   speed  radius mass   lob   life  dmg    blast bnce  boom
    { "MARINE",    "ALL ROUNDER",      { 0.25f, 0.85f, 0.35f, 1.0f },    9.0f, { 0.12f, 34.0f, 0.11f, 0.40f, 0.0f, 3.0f, 12.0f, 0.0f, 0.0f, false } },
    { "MEDIC",     "FAST SUPPORT",     { 0.90f, 0.90f, 0.95f, 1.0f },   11.0f, { 0.30f, 26.0f, 0.09f, 0.30f, 0.0f, 3.0f, 10.0f, 0.0f, 0.0f, false } },
    { "SNIPER",    "LONG RANGE",       { 0.30f, 0.60f, 0.95f, 1.0f },    7.0f, { 1.10f, 80.0f, 0.07f, 0.25f, 0.0f, 3.0f, 85.0f, 0.0f, 0.0f, false } },
    { "GRENADIER", "LOBBED GRENADES",  { 0.95f, 0.55f, 0.20f, 1.0f },    7.5f, { 0.90f, 16.0f, 0.22f, 1.60f, 7.5f, 2.5f, 40.0f, 2.2f, 0.0f, true  } },
};

// Standard issue for every class, thrown with the grenade key rather than the
// fire trigger. Hits softer than the grenadier's shell and there's only one of
// it per life, so it's never a second primary; what makes it worth carrying is
// the wide blast, which punishes clustered enemies and flushes anyone camping a
// corner. Being unrepeatable is the point: the throw has to be worth spending.
//
// Unlike every other shot, it doesn't die where it lands: it bounces off walls
// and rolls, and the fuse below is what detonates it, so the throw has to be
// led and the target gets a moment to clear out. The fixed lob puts first
// contact at roughly 1.7s, leaving over a second of live grenade after that.
// Range comes out of the horizontal speed alone (~12 units at full throw): the
// lob stays where it is, so the arc keeps its height and its time to first
// contact, and a shorter throw just means a steeper one.
//
// fireInterval is 0 because nothing about the grenade is paced by a cadence:
// a soldier gets Game::kGrenadesPerLife of them and no more until they die, so
// the limit is the count, not the clock.
inline constexpr WeaponDef kGrenade = {
    // fire  speed  radius mass   lob   fuse  dmg    blast bnce  boom
       0.0f,  7.0f, 0.20f, 0.80f, 8.0f, 3.0f, 30.0f, 4.0f, 0.45f, true
};

inline const ClassDef& GetClassDef(ClassId id)
{
    return kClassDefs[static_cast<size_t>(id)];
}
