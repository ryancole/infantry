#pragma once

#include <DirectXMath.h>
#include <cstddef>

// The soldier archetypes a player must pick from before spawning. Pure data,
// so the selection screen and gameplay code share one source of truth. Stats
// only cover what the prototype simulates today (movement, projectiles, and
// one ability per class); equipment slots layer on once those systems exist.
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

// How a class carries itself on the ground. Split out of ClassDef for the same
// reason WeaponDef was: the three numbers are one idea and they're read
// together, so they're worth a name and a brace rather than three more loose
// fields in a row that's already long.
//
// The rates are how fast the walk converges on what the keys are asking for
// (1/s, so the time constant is their reciprocal): `accel` is what getting
// under way or hauling round onto a new heading costs, `stop` is how quickly a
// released key settles. Keeping them per-class is what lets weight be a stat
// rather than a global feel — top speed says how fast a soldier travels, and
// these say how much of a body has to be moved to get there.
struct MoveDef
{
    float speed; // units per second
    float accel; // 1/s; higher answers the keys sooner
    float stop;  // 1/s; higher plants harder
};

// The one thing a class can do that no other class can. Everything else in this
// file is standard issue with the numbers moved around — the same shape of
// rifle, the same grenade, the same blade — so a class is currently a stat line
// and not a job. This is where that changes.
//
// One ability, on one key, on one clock. A soldier with three buttons is a
// soldier you have to study before you can play them, and the whole promise of
// picking one off a card is that you already know what you got.
//
// `kind` says what the ability does; the numbers under it are read by that kind
// and mean nothing without it. Only the medic has one so far, so this is a
// one-entry enum next to the None the other three carry — and that None isn't a
// placeholder for a system that hasn't landed, it's an accurate statement of
// what those three classes are today.
enum class AbilityKind
{
    None,
    // Puts `amount` health back into the user over `duration` seconds,
    // delivered by the second rather than in a lump at the end.
    Heal,
};

struct AbilityDef
{
    AbilityKind kind;
    const char* name; // uppercase, for the class card and the key hint
    float duration;   // seconds it runs for; must be > 0 for any kind but None
    // Measured from when the ability *ends*, not from when it started, so being
    // cut short costs the whole cooldown rather than a shortened one. Nothing
    // else in the loadout works this way, and it's why letting go of the
    // ability is a decision instead of a free look at it.
    float cooldown;
    float amount; // what it delivers over the duration; read by the kind
};

inline constexpr AbilityDef kNoAbility = { AbilityKind::None, "", 0.0f, 0.0f, 0.0f };

// The medic's, and the first ability in the game. It heals the medic and nobody
// else, because there is nobody else yet — every other soldier on the field is
// an enemy. When teammates exist this is the ability that should reach them;
// until then a self-dressing is the honest version of it, and it's the one that
// can actually be played against the arena as it stands.
//
// Sixty health over two and a half seconds is most of a body brought back, and
// far faster than the fourteen-second wait says it should be — which is the
// point. The ability isn't rationed by how much it gives, it's rationed by when
// a medic can afford to give it: the dressing drops the moment they fire,
// throw, or swing, so using it means choosing to be unarmed in the middle of a
// fight they're already losing. Everything else stays available — the medic can
// still run, still turn, still be shot at — because a class built to cross open
// ground shouldn't be rooted by its own ability. What it hands the player is the
// same question every time: break contact and come back whole, or hold the
// trigger and stay hurt.
inline constexpr AbilityDef kFieldDressing = {
    // kind               name              duration cooldown amount
    AbilityKind::Heal, "FIELD DRESSING",    2.5f,    14.0f,   60.0f
};

struct ClassDef
{
    const char* name;  // uppercase: the debug line font has no lowercase
    const char* blurb;
    DirectX::XMFLOAT4 color; // player body + UI accent
    MoveDef move;
    WeaponDef primary;
    AbilityDef ability;
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
    // Weight is its own axis, and it does not follow top speed. The medic is
    // the light one at both ends — quickest onto its speed and quickest off it
    // — because a class whose job is crossing open ground to reach someone
    // should be able to change its mind. The grenadier is the heavy one, slow
    // to set off and carrying the longest coast: it lobs from a spot it chose,
    // and being hard to reposition is what it pays for the blast. The sniper
    // splits them. It sets off slowest of all, but plants harder than anyone,
    // which is the pair of numbers that matches what it does — a class that
    // takes one shot at a time wants to be stopped the moment it decides to
    // be, and doesn't much care how long the walk over took. The marine sits
    // in the middle of both, and is still the class the others are read
    // against.
    //
    // Coast on release runs from about a third of a unit (sniper) to a little
    // under six tenths (grenadier), against a soldier 0.8 wide — so the spread
    // between the lightest class and the heaviest is roughly a third of a body.
    // It is meant to be felt in the hands rather than seen from the camera.
    // name         blurb              color                          | speed accel  stop | fire   mag reload speed  radius mass   lob   life  dmg    blast bnce  boom  | ability
    { "MARINE",    "ALL ROUNDER",      { 0.25f, 0.85f, 0.35f, 1.0f }, {  9.0f,  8.0f, 20.0f }, { 0.12f, 30, 2.10f, 34.0f, 0.11f, 0.40f, 0.0f, 3.0f, 12.0f, 0.0f, 0.0f, false }, kNoAbility },
    { "MEDIC",     "FAST SUPPORT",     { 0.90f, 0.90f, 0.95f, 1.0f }, { 11.0f, 11.0f, 24.0f }, { 0.30f, 20, 1.60f, 26.0f, 0.09f, 0.30f, 0.0f, 3.0f, 10.0f, 0.0f, 0.0f, false }, kFieldDressing },
    { "SNIPER",    "LONG RANGE",       { 0.30f, 0.60f, 0.95f, 1.0f }, {  7.0f,  6.5f, 22.0f }, { 1.10f,  1, 2.40f, 80.0f, 0.07f, 0.25f, 0.0f, 3.0f, 85.0f, 0.0f, 0.0f, false }, kNoAbility },
    { "GRENADIER", "LOBBED GRENADES",  { 0.95f, 0.55f, 0.20f, 1.0f }, {  7.5f,  6.0f, 13.0f }, { 0.90f,  1, 1.80f, 16.0f, 0.22f, 1.60f, 7.5f, 2.5f, 40.0f, 2.2f, 0.0f, true  }, kNoAbility },
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

// The blade every soldier carries, swung with the melee key. Standard issue
// like the grenade, and for the same reason: it isn't what separates one class
// from another, it's the thing all four of them fall back on.
//
// It is deliberately the worst weapon on the field at everything except the one
// range nothing else covers. The reach barely clears the two bodies involved,
// and at 20 a swing it takes five of them to end a soldier — so a melee that
// wins a fight is a fight the primary had already lost. What it is for is the
// moment the primary can't answer: a magazine out, or someone already inside
// the distance the class was built to fight at.
//
// The three swings are there to be spent at once, and they all come back
// together a fixed time after the last one — measured from when the swinging
// stops, not from when the charges run out. Two swings and a step back costs
// exactly what three would have, which is the point: a wait that only started
// on the third swing would make hanging onto the last one strictly wrong, and
// a player flicking out a swing they don't want just to start the clock isn't
// playing anything. There's no key to bring them back early for the same
// reason there's no decision in it.
struct MeleeDef
{
    float reach;         // center-to-center distance that still connects
    float arc;           // half-angle off the aim direction that still connects
    float damage;
    int charges;         // swings there are to spend
    float recoverTime;   // seconds after the last swing before they all return
    float swingInterval; // seconds between swings
};

inline constexpr MeleeDef kMelee = {
    // reach  arc    dmg    charges recover swing
       1.6f,  0.80f, 20.0f, 3,      2.0f,   0.45f
};

inline const ClassDef& GetClassDef(ClassId id)
{
    return kClassDefs[static_cast<size_t>(id)];
}
