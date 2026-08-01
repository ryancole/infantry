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
    // Shots a full magazine holds, and what it costs to refill it. Together
    // they cap how long a class can hold a trigger down before it has to stop
    // and spend reloadTime doing nothing, which is what keeps a high rate of
    // fire from simply being better than a slow one. 0 = not magazine-fed:
    // the weapon never reloads and its supply is limited some other way (the
    // grenade below is issued by the life).
    int magazine;
    float reloadTime;        // seconds to swap a magazine
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
    // Two shapes of weapon, and the magazine is what tells them apart.
    //
    // The automatics (marine, medic) get roughly the same window of fire out of
    // a magazine — 3.5 to 6 seconds — whatever their cadence, and pay for it
    // afterwards in proportion to what that magazine could do: the marine's
    // thirty rounds cost more to replace than the medic's twenty weaker ones.
    // Their decision is when to spend the pause, which is why the reload key
    // exists.
    //
    // The single-shot pair (sniper, grenadier) reload after every shot, so
    // there's no burst to ration and no decision to make: fireInterval never
    // gets to matter and the reload alone is the cadence. Both are weapons that
    // end a soldier in one or two hits, and a magazine let them do it to a whole
    // squad on one breath — five bolts at 85 damage inside four and a half
    // seconds, four shells at 40 and a 2.2 blast inside three. What they get
    // instead is one shot that has to count, and a wait long enough that missing
    // it is felt. The sniper waits longest, as it did when it carried five: the
    // deadliest shot in the game pays the highest price for the next one.
    // name         blurb              color                            move  | fire   mag reload speed  radius mass   lob   life  dmg    blast bnce  boom
    { "MARINE",    "ALL ROUNDER",      { 0.25f, 0.85f, 0.35f, 1.0f },    9.0f, { 0.12f, 30, 2.10f, 34.0f, 0.11f, 0.40f, 0.0f, 3.0f, 12.0f, 0.0f, 0.0f, false } },
    { "MEDIC",     "FAST SUPPORT",     { 0.90f, 0.90f, 0.95f, 1.0f },   11.0f, { 0.30f, 20, 1.60f, 26.0f, 0.09f, 0.30f, 0.0f, 3.0f, 10.0f, 0.0f, 0.0f, false } },
    { "SNIPER",    "LONG RANGE",       { 0.30f, 0.60f, 0.95f, 1.0f },    7.0f, { 1.10f,  1, 2.40f, 80.0f, 0.07f, 0.25f, 0.0f, 3.0f, 85.0f, 0.0f, 0.0f, false } },
    { "GRENADIER", "LOBBED GRENADES",  { 0.95f, 0.55f, 0.20f, 1.0f },    7.5f, { 0.90f,  1, 1.80f, 16.0f, 0.22f, 1.60f, 7.5f, 2.5f, 40.0f, 2.2f, 0.0f, true  } },
};

// Shots per second a weapon actually keeps up: the cadence inside a magazine
// and the reload that follows it, averaged over one full magazine. Once a class
// can only fire in bursts of `magazine`, this is the only rate that describes
// it — fireInterval alone flatters a weapon that stops after every shot, and at
// a magazine of one it describes nothing at all. Weapons that never reload are
// just their cadence.
inline constexpr float SustainedFireRate(const WeaponDef& w)
{
    if (w.magazine <= 0)
        return w.fireInterval > 0.0f ? 1.0f / w.fireInterval : 0.0f;
    const float mag = static_cast<float>(w.magazine);
    return mag / ((mag - 1.0f) * w.fireInterval + w.reloadTime);
}

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
// fireInterval is 0 because nothing about the grenade is paced by a cadence,
// and the magazine is 0 because nothing about it is paced by a reload either:
// a soldier gets Game::kGrenadesPerLife of them and no more until they die, so
// the limit is the count, not the clock.
inline constexpr WeaponDef kGrenade = {
    // fire  mag reload speed  radius mass   lob   fuse  dmg    blast bnce  boom
       0.0f,  0,  0.0f,  7.0f, 0.20f, 0.80f, 8.0f, 3.0f, 30.0f, 4.0f, 0.45f, true
};

inline const ClassDef& GetClassDef(ClassId id)
{
    return kClassDefs[static_cast<size_t>(id)];
}
