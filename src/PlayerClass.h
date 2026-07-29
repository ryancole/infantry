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

struct ClassDef
{
    const char* name;  // uppercase: the debug line font has no lowercase
    const char* blurb;
    DirectX::XMFLOAT4 color; // player body + UI accent
    float moveSpeed;         // units per second
    float fireInterval;      // seconds between shots
    float projectileSpeed;   // horizontal muzzle speed
    float projectileRadius;
    float projectileMass;
    float lobVelocity;       // upward muzzle speed; > 0 arcs the shot under gravity
    float projectileLife;    // seconds before despawn
};

inline constexpr ClassDef kClassDefs[kClassCount] = {
    // name         blurb                 color                             move  fire   speed  radius mass   lob   life
    { "MARINE",    "ALL ROUNDER",         { 0.25f, 0.85f, 0.35f, 1.0f },    9.0f, 0.12f, 34.0f, 0.11f, 0.40f, 0.0f, 3.0f },
    { "MEDIC",     "FAST SUPPORT",        { 0.90f, 0.90f, 0.95f, 1.0f },   11.0f, 0.30f, 26.0f, 0.09f, 0.30f, 0.0f, 3.0f },
    { "SNIPER",    "LONG RANGE",          { 0.30f, 0.60f, 0.95f, 1.0f },    7.0f, 1.10f, 80.0f, 0.07f, 0.25f, 0.0f, 3.0f },
    { "GRENADIER", "BOUNCING GRENADES",   { 0.95f, 0.55f, 0.20f, 1.0f },    7.5f, 0.90f, 16.0f, 0.22f, 1.60f, 7.5f, 2.5f },
};

inline const ClassDef& GetClassDef(ClassId id)
{
    return kClassDefs[static_cast<size_t>(id)];
}
