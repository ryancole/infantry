#pragma once

#include "Ability.h"
#include "Brain.h"

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
    // and spend a reload doing nothing, which is what keeps a high rate of
    // fire from simply being better than a slow one. 0 = not magazine-fed:
    // the weapon never reloads and its supply is limited some other way (the
    // grenade below is issued by the life).
    int magazine;
    // The refill, in two numbers, because that is how the zone writes its own
    // weapons: one price for changing a magazine with rounds still in it, a
    // longer one for being caught with none. The leftover rounds are thrown
    // away with the magazine either way (World::BeginReload), so what topping
    // up buys is not ammunition — it's the shorter wait, and the right to take
    // it in a lull instead of in the middle of the next fight. Getting that
    // wrong twice over is what an empty magazine costs. A magazine of one only
    // ever empties, so those weapons never pay anything but reloadEmpty.
    float reloadEarly;       // seconds to swap a magazine with rounds left
    float reloadEmpty;       // seconds to swap one that ran dry
    float projectileSpeed;   // horizontal muzzle speed
    float projectileRadius;
    float projectileMass;
    float lobVelocity;       // upward muzzle speed; > 0 arcs the shot under gravity
    float projectileLife;    // seconds before despawn; with bounce > 0, the fuse
    // The near end of a weapon's reach, the way projectileLife is the far one.
    // A soldier standing closer to the muzzle than this cannot be hit by the
    // round at all: it passes clean through them and carries on to whatever is
    // behind, because it isn't armed yet. Measured from the shooter to the
    // body, not along the round's flight, so it's the distance between two
    // soldiers — the thing a player can actually judge — rather than a number
    // about a bullet. 0 = no dead ground, which is every weapon here but one.
    float minRange;
    float damage;            // health removed by a direct hit
    float blastRadius;       // > 0: splash damage out to here, falling off to nothing
    // Restitution, i.e. the fraction of impact speed a bounce keeps. Above 0
    // the shot also stops dying on contact with the world: it ricochets and
    // rolls until projectileLife runs out, and that fuse is what sets it off.
    // A direct hit on a soldier still detonates it on the spot.
    float bounce;
    bool explodes;           // impact plays the explosion effect + boom, not a thud
    // Leaves a hand rather than a barrel. Only presentation reads it — a throw
    // is cloth and air where a shot is a report — but it lives here rather than
    // in the client because which weapons are thrown is a fact about the
    // loadout, and there is nothing else on a Fire event that distinguishes
    // them: the grenadier's launcher lobs an explosive shell too, and it is
    // still a gun going off.
    bool thrown;
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

// The abilities the table below hands out. What an ability *is* — and what it
// does when the key goes down — lives in Ability.h; what's here is the tuning,
// because that's the half a balance pass wants to read, and it wants to read it
// next to the speeds and the magazines it trades against.
//
// Only the medic has one so far. The other three carry Ability::kNone, and that
// isn't a placeholder for a system that hasn't landed — it's an accurate
// statement of what those three classes are today.
//
// The medic's is the first ability in the game. It patches up the medic and
// one other soldier at once: whoever on their side they happen to be pointed at
// that instant, out to seven units and a little under half a radian either way.
// Both get the full sixty over the two and a half seconds — the friendly isn't
// paid for out of the medic's share — so a medic who keeps someone in front of
// them for the whole dressing does twice the work of one who doesn't. That's
// the skill in it, and it's why the target is picked fresh every frame rather
// than locked at the start: the healing goes where the medic is looking, and
// sweeping across two wounded soldiers really does split it between them.
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
// trigger and stay hurt. Doing it for somebody else asks it twice over, since
// now there are two soldiers standing still in the open instead of one.
//
// The reach is deliberately short of the range anything shoots at. A medic has
// to close most of the way to the soldier they're treating, which is the walk
// the class's speed exists to pay for.
inline constexpr Ability::Def kFieldDressing = {
    // kind                  name              duration cooldown sound  | amount reach arc
    Ability::Kind::Heal, "FIELD DRESSING",     2.5f,    14.0f,   "heal", { 60.0f, 7.0f, 0.45f }
};

// What a class is, in one row. Increasingly this table is a manifest as much as
// a balance sheet: the stats are still here to be compared down a column, but
// the last two fields only name things — which ability this class carries, which
// mind its NPCs think with — and the things themselves live in their own files.
// That's the shape to keep as classes get more bespoke. Whatever a class grows
// next, this row should end up naming it rather than holding it, so that the one
// place four soldiers can be compared side by side stays readable.
struct ClassDef
{
    const char* name;  // uppercase: the debug line font has no lowercase
    const char* blurb;
    // The class mark: the soldier's helmet, and the accent on the screens that
    // talk about the class (selection, HUD panel). It is no longer what a body
    // is painted — the armor wears the team's color now (see Team.h), and this
    // is the small thing riding on top of it. That's what these four hues have
    // to be chosen against: they need to be distinct from each other, and none
    // of them may sit close to blue or red, or the mark disappears into the
    // side it's supposed to be marking.
    DirectX::XMFLOAT4 color;
    MoveDef move;
    // How far this soldier can make anything out at all: the radius of their
    // share of the fog of war, and the same number whether a person or a brain
    // is behind the eyes. Sight is this and a clear line both — a wall inside
    // the radius still stops it dead — and a side sees with all of its eyes at
    // once, so this is one soldier's contribution to what the squad knows
    // rather than a limit on what reaches the player.
    //
    // It is a class stat rather than the one constant it used to be because
    // the zone this map comes from says it should be: hardcorps2t's own class
    // list gives the sniper "enhanced viewing distance and LOS" and says
    // nothing of the kind about anybody else. That sentence is the whole of
    // what's sourced here; the spread below is ours.
    //
    // Three things bound this column wherever it goes, and they're worth
    // checking against any new row. Every value stays under earshot
    // (Sound::kRange, forty-five), so the map you hear is always wider than
    // the map you see and gunfire out of the dark stays a thing that happens
    // to you. Every value stays clear of Brain::kEngageRange (twenty-two), or
    // an NPC would be quietly capped by an eyesight number nothing reports.
    // And every value stays wider than the screen at the zoom the camera opens
    // on (twenty-six units across the viewport), so the fog remains a fact
    // about walls and distance rather than a circle painted around the player.
    float sight;
    WeaponDef primary;
    Ability::Def ability;
    // How this class fights when the computer is playing it. All four name the
    // same one today, which is what "the AI doesn't know one class from another
    // yet" looks like written down — and it's where a class stops sharing that
    // behavior the moment it has its own.
    Brain::Kind brain;
};

inline constexpr ClassDef kClassDefs[kClassCount] = {
    // Two shapes of weapon, and the magazine is what tells them apart.
    //
    // The automatics (marine, medic) get roughly the same window of fire out of
    // a magazine — 3.5 to 6 seconds — whatever their cadence, and then have to
    // stop for about as long again. Their decision is when to spend the pause,
    // which is why the reload key exists, and the pair of reload numbers is
    // what makes it a real decision rather than a formality: topping up costs
    // the marine 3.5 seconds and the medic 3, while being caught dry costs 5
    // and 6. The medic has the wider gap and so the most to gain by changing a
    // magazine in a lull — a PDW that runs out in the middle of something
    // takes twice as long to replace as one changed before it did, which is a
    // hard price for a class that has to be somewhere. The marine's gap is the
    // narrowest, which is the same as saying it's the class least punished for
    // firing until the click.
    //
    // The single-shot pair (sniper, grenadier) reload after every shot, so
    // there's no burst to ration and no decision to make: fireInterval never
    // gets to matter, the early number can never be paid at all, and the empty
    // reload alone is the cadence. Both are weapons that end a soldier in one
    // or two hits, and a magazine let them do it to a whole squad on one breath
    // — five bolts at 85 damage inside four and a half seconds, four shells at
    // 40 and a 2.2 blast inside three. What they get instead is one shot that
    // has to count, and a wait long enough that missing it is felt. The sniper
    // waits longest, as it did when it carried five: the deadliest shot in the
    // game pays the highest price for the next one.
    //
    // The reload column is the second one with a source, after sight. The zone
    // lists both halves for every weapon it issues, and the four rows below
    // take its numbers unaltered — assault rifle 3.5/5, PDW 3/6, grenade
    // launcher 3.2/6.5, sniper rifle 5/7. That is between two and a half and
    // nearly four times what this game used to charge (the marine waited 2.1
    // seconds, the sniper 2.4), and taking it at face value is a deliberate
    // bet: that the wait is meant to be long enough to be a thing that happens
    // to you rather than a hitch in the shooting, and that a sniper firing once
    // every seven seconds is a sniper whose misses matter. Nothing else in the
    // table has been moved to soften it, so this is the change that will be
    // felt first if any of it is wrong. Two of the zone's rows have nowhere to
    // land here: the ripper gun is 4/6 and no class carries one, and the hand
    // grenade's 2/2 is a reload for a weapon that gets issued by the life
    // instead (kGrenade below, which never reloads).
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
    // The rates are times, not distances, so they say the same thing at any
    // top speed: a released key still settles in about a twentieth of a second
    // (sniper) to a thirteenth (grenadier), and that ordering is the weight.
    // What the ground covered in that time is depends on the speed: at the
    // speeds below, coast on release runs from about a sixth of a unit
    // (sniper) to a little under three tenths (grenadier), against a soldier
    // 0.8 wide — so the spread between the lightest class and the heaviest is
    // roughly a sixth of a body. It is meant to be felt in the hands rather
    // than seen from the camera. Should the speeds move again, these distances
    // move with them and the rates do not; bringing the rates along is what
    // holds the coast where it is.
    //
    // Sight is the newest column and the one that most changes what a class
    // is, because it decides how much of the map a soldier brings back to the
    // squad. The sniper's thirty-eight is the only number in it with a source:
    // the zone's class list gives that class "enhanced viewing distance and
    // LOS", and thirty-eight is what that has to mean on this ground — the
    // bolt drops into the dirt around thirty-six units out, so for the first
    // time LONG RANGE can see the whole of what it shoots at rather than
    // firing into fog it has to be told about. The marine keeps the thirty
    // everything else was tuned against and stays the class the others are
    // read against here too. The medic and the grenadier give four units of it
    // up, for opposite reasons that come to the same price: the medic is fast
    // and has to close on the soldier it treats anyway, and the grenadier
    // throws over cover at ground it was never going to see for itself. Both
    // want somebody out in front of them, which is what a shorter eye is for —
    // it makes being spotted for something a class needs rather than something
    // it enjoys, and it gives the sniper a job on a quiet flank.
    //
    // Minimum range is the sniper's alone, and it is the first stat in the
    // table that takes something away rather than handing it out. Six units of
    // ground in front of the muzzle where the rifle does nothing at all: a
    // soldier standing inside it is passed straight through, and the bolt goes
    // on to whatever is behind them. Nothing about the shot changes — it isn't
    // weakened, it isn't stopped, it simply isn't armed yet — so a sniper with
    // someone in their face is a sniper holding a stick, and the answer has to
    // be the blade, the grenade, or the walk backwards.
    //
    // It's here because a class called LONG RANGE should be answerable up
    // close, and until now it wasn't: the same 85 that ends a soldier at thirty
    // units ended them at two, so the counterplay to the longest reach in the
    // game was to walk into it, and the sniper was rewarded for being found.
    // Every other cost the class pays is a wait — the slowest legs, the longest
    // reload — and a wait is something a good player spends well. Dead ground
    // is not: no amount of skill fires a round that hasn't armed, so this is
    // the one place the class is simply beaten, which is what a class this
    // deadly at range needs somewhere on it.
    //
    // Six is measured against the things a soldier meets rather than picked off
    // a scale. It is well past the blade (1.6) so the fallback is a real
    // weapon rather than a technicality, wide enough that a trench (4.9 across)
    // is dead ground end to end so getting into the sniper's own line is worth
    // doing, and short of the medic's dressing (7) — the shortest reach in the
    // game that isn't a swing — so the sniper's blind spot never becomes the
    // widest thing on the field. Against Brain::kPreferredRange, the eleven
    // units an NPC circles at, it leaves the AI fighting where it always did
    // and treats being closer than that as the emergency it is.
    //
    // Two of the colors moved when the armor became the team's. The sniper's
    // blue and the grenadier's orange were the marks that sat nearest the sides
    // they'd now have to be read against — a blue helmet on a blue soldier says
    // nothing at all. Violet and amber are as far from each other as those were,
    // as far from the marine's green and the medic's white, and neither can be
    // mistaken for a team.
    // Reach is the third column with a source, and the first to come out of the
    // zone's item table rather than its prose. Every projectile in
    // hardcorps2.itm carries a muzzle velocity and an alive time — the server
    // repo's Docs/Itm names the columns, and its own aiming code fixes the unit
    // at thousandths of a pixel per ten-millisecond tick — and what survives the
    // trip is a set of ratios rather than distances. The sniper rifle reaches
    // about 1.6 times the assault rifle; the PDW about 0.6. The absolute
    // numbers don't transfer, since that game measured across a map five
    // hundred tiles wide, but the spread does, and it's tighter than this table
    // had: our sniper was reaching 2.4 times the marine.
    //
    // Reach here isn't a stat, it's a consequence. A round leaves the muzzle
    // level at 0.6 up and dies where it meets the floor, so how far it gets is
    // its speed times the same fall — about a third of a second for all of
    // them, a hair longer for the thinner rounds. That makes projectileSpeed
    // the only lever there is, and the speeds below are set so the reaches land
    // on the zone's ratios: marine 16.8 units, medic 10.1, sniper 27.
    //
    // The sniper is what the other two were fitted around, rather than the
    // marine everything else in this table is read against. Its 27 units is
    // already load-bearing: the sight of 38 exists so LONG RANGE can see the
    // whole of what it shoots at, and the dead ground of 6 was measured against
    // a reach that long. Anchoring on the marine instead would have cut the
    // bolt to 18 and left both of those paragraphs describing a class that no
    // longer existed. The price is paid by the marine, whose round is now half
    // again as fast (34 -> 51) and reaches half again as far — the largest
    // single change in this table, and the first thing to look at if the
    // shooting starts feeling wrong. Against the AI it reads as a fix: a
    // rifleman opens fire at Brain::kEngageRange (22) and now covers most of
    // that ground instead of half of it. The medic still can't reach
    // kPreferredRange (11), the distance its own brain circles at, which is a
    // thing that was already true and isn't made worse here.
    //
    // The grenadier sits out of it. Its shell is lobbed, so the arc ends the
    // flight long before the fall would, and the zone's own launcher agrees —
    // the alive time on that row is a twenty-second safety net, not a range.
    // The zone's flamethrower ratio (0.4) has no class here to land on.
    // name         blurb              color                          | speed accel  stop | sight | fire   mag early empty speed  radius mass   lob   life  min   dmg    blast bnce  boom   thrw  | ability         brain
    { "MARINE",    "ALL ROUNDER",      { 0.25f, 0.85f, 0.35f, 1.0f }, {  4.50f,  8.0f, 20.0f }, 30.0f, { 0.12f, 30, 3.50f, 5.00f, 51.0f, 0.11f, 0.40f, 0.0f, 3.0f, 0.0f, 12.0f, 0.0f, 0.0f, false, false }, Ability::kNone, Brain::Kind::Rifleman },
    { "MEDIC",     "FAST SUPPORT",     { 0.90f, 0.90f, 0.95f, 1.0f }, {  5.50f, 11.0f, 24.0f }, 26.0f, { 0.30f, 20, 3.00f, 6.00f, 29.0f, 0.09f, 0.30f, 0.0f, 3.0f, 0.0f, 10.0f, 0.0f, 0.0f, false, false }, kFieldDressing, Brain::Kind::Rifleman },
    { "SNIPER",    "LONG RANGE",       { 0.62f, 0.40f, 0.96f, 1.0f }, {  3.50f,  6.5f, 22.0f }, 38.0f, { 1.10f,  1, 5.00f, 7.00f, 80.0f, 0.07f, 0.25f, 0.0f, 3.0f, 6.0f, 85.0f, 0.0f, 0.0f, false, false }, Ability::kNone, Brain::Kind::Rifleman },
    { "GRENADIER", "LOBBED GRENADES",  { 0.98f, 0.70f, 0.12f, 1.0f }, {  3.75f,  6.0f, 13.0f }, 26.0f, { 0.90f,  1, 3.20f, 6.50f, 16.0f, 0.22f, 1.60f, 7.5f, 2.5f, 0.0f, 40.0f, 2.2f, 0.0f, true,  false }, Ability::kNone, Brain::Kind::Rifleman },
};

// Shots per second a weapon actually keeps up: the cadence inside a magazine
// and the reload that follows it, averaged over one full magazine. Once a class
// can only fire in bursts of `magazine`, this is the only rate that describes
// it — fireInterval alone flatters a weapon that stops after every shot, and at
// a magazine of one it describes nothing at all. Weapons that never reload are
// just their cadence.
//
// It's the empty reload that belongs here, not the early one: firing a whole
// magazine is what this rate measures, and a magazine fired to the end is
// always replaced dry. The early number is what a player can do better than
// this figure by not emptying it.
inline constexpr float SustainedFireRate(const WeaponDef& w)
{
    if (w.magazine <= 0)
        return w.fireInterval > 0.0f ? 1.0f / w.fireInterval : 0.0f;
    const float mag = static_cast<float>(w.magazine);
    return mag / ((mag - 1.0f) * w.fireInterval + w.reloadEmpty);
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
// the limit is the count, not the clock. The zone does give the hand grenade a
// reload of its own — two seconds, both halves the same — but that's the
// pacing of a thing you carry several of, and this one is issued by the life.
inline constexpr WeaponDef kGrenade = {
    // fire  mag early empty speed  radius mass   lob   fuse  min   dmg    blast bnce  boom  thrw
       0.0f,  0,  0.0f, 0.0f,  7.0f, 0.20f, 0.80f, 8.0f, 3.0f, 0.0f, 30.0f, 4.0f, 0.45f, true, true
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
