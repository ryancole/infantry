#pragma once

#include "Ability.h"
#include "Brain.h"

#include <DirectXMath.h>
#include <cstddef>
#include <cstdint>

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

// What a round looks like on its way to the target. Every shot in the game was
// the same amber ball until now, which meant the only thing the air could tell
// you was that somebody was shooting — not who, or with what. These make a
// round readable in flight, which matters most for the shot you didn't see
// fired: the streak crossing in front of you says which class is out there and
// therefore what is about to be asked of you.
//
// `length` is the round smeared along its own line of flight, and it's the
// field that decides what kind of thing is being drawn. Zero is a ball at the
// projectile's own radius — a body you watch arrive, which is what a lobbed or
// thrown thing is — and anything above it is a streak, which reads as a
// direction more than an object. `width` is deliberately its own number rather
// than the hit radius doubled: how thin a tracer looks is presentation, and
// tying it to the size the round is tested at would make a visual choice a
// balance one.
struct TracerDef
{
    DirectX::XMFLOAT4 color;
    float length; // units along the line of flight; 0 draws the round as a ball
    float width;  // units across it; unread when length is 0
};

// Which of those a round is, in one byte. The client is told this per shot
// (Net::SnapProjectile) and an index into a short fixed table is the cheapest
// honest way to say it — the alternative is shipping a color and two floats
// with every round in the air. It rides on the shot rather than being looked
// up from the shooter for the same reason the damage does: a round outlives
// the soldier who fired it, and outlives the class they were.
enum class TracerId : uint8_t
{
    None,  // no streak: the round is drawn as a ball, in the table's own color
    Rifle, // marine
    Pdw,   // medic
    Bolt,  // sniper
    Shell, // grenadier
    Count,
};

// The looks themselves. Four of the five are what a class's primary sends, so
// this table is read down the same way kClassDefs is: these are colors chosen
// against each other, and against the two sides they'll be seen over.
//
// They are not the class marks from kClassDefs::color, and shouldn't drift
// toward them. A mark is a small thing worn on a body you can already see, and
// it only has to be distinct at rest; a tracer is a fast thing crossing open
// ground and it has to be distinct at a glance, in the dark, over grass. The
// medic's mark is near-white and its round is green, and that's the shape of
// the difference rather than an inconsistency.
//
// Length is set against how far the round travels in a frame, so the streak
// reads as continuous rather than as a strobing dash: the sniper's bolt covers
// about 1.6 units between frames at sixty, the marine's about 0.85, the
// medic's about 0.5. Each is drawn a little shorter than that, so the round is
// a mark on its own line rather than a solid rod filling it. The bolt is the
// one that has to be watched — it is the fastest thing on the field by half
// again, and its length tracks projectileSpeed rather than any judgement about
// how a sniper round ought to look.
//
// None is what a thrown grenade carries, and nothing reads it: a fused round
// is drawn by its blink instead (see the client's ProjectileColor), because a
// live grenade bouncing at your feet is a different question from a bullet and
// must not be answered in the same shape. It's here as the safe default a
// zero-initialized shot lands on.
inline constexpr TracerDef kTracers[static_cast<size_t>(TracerId::Count)] = {
    //                color                          len    width
    /* None  */ { { 1.00f, 0.80f, 0.20f, 1.0f },     0.00f, 0.00f },
    /* Rifle */ { { 1.00f, 0.80f, 0.20f, 1.0f },     0.60f, 0.075f },
    /* Pdw   */ { { 0.30f, 1.00f, 0.35f, 1.0f },     0.42f, 0.060f },
    /* Bolt  */ { { 0.70f, 0.88f, 1.00f, 1.0f },     1.20f, 0.055f },
    /* Shell */ { { 1.00f, 0.52f, 0.14f, 1.0f },     0.00f, 0.00f },
};

inline constexpr const TracerDef& GetTracer(TracerId id)
{
    return kTracers[static_cast<size_t>(id) < static_cast<size_t>(TracerId::Count)
                        ? static_cast<size_t>(id)
                        : 0];
}

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
    // ever empties, so such a weapon never pays anything but reloadEmpty —
    // which is the grenadier's launcher and nothing else in the table.
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
    // What the round looks like crossing the ground between two soldiers. The
    // only field here that nothing in the simulation reads — but it belongs in
    // the weapon rather than the client for the same reason `thrown` does:
    // which weapon is being fired at you is a fact about the loadout, and the
    // shot is the only thing that can carry it to the person being shot at.
    TracerId tracer;
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
    // checking against any new row. All three are distances from a soldier, so
    // all three are compared against a radius — which is the thing this comment
    // used to get wrong, and it cost the game a bug in each of the other two.
    // The camera's zoom is a WIDTH: at the twenty-six it opened on, a player
    // could see under thirteen units in any direction, and both of the numbers
    // below had been set as though they could see twenty-six. So gunfire was
    // audible from three and a half screens away, and riflemen opened up from
    // most of a screen outside the frame. Both are fixed at their own
    // definitions; the arithmetic is written out at IsoCamera::m_zoom.
    //
    // Every value stays at or above earshot (Sound::kRange, twenty-six), so the
    // map you hear is never wider than the map you can see into, and a shot you
    // hear is one you could have seen coming. Every value stays clear of
    // Brain::kEngageRange (fourteen), or an NPC would be quietly capped by an
    // eyesight number nothing reports. And every value stays past the corner of
    // the screen at the zoom the camera opens on (thirty-four across, putting
    // the corner near twenty-four), so the fog remains a fact about walls and
    // distance rather than a circle painted around the player — that last one
    // is what caps the zoom, since the shortest sight here is twenty-six.
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
    // Three shapes of weapon, and the magazine is what tells them apart.
    //
    // The automatics (marine, medic) hold a magazine for about the same short
    // while — the marine just under three seconds of fire, the medic just under
    // two — and inside it they are the same weapon. Twelve points every 0.12
    // seconds and ten every 0.10 both come to a hundred a second, so nothing
    // that happens in a firefight tells the two classes' shooting apart. The
    // medic simply delivers the same damage in smaller, faster pieces, which is
    // what the class is: it hits softer per round and pulls the trigger more
    // often, and those cancel.
    //
    // Where they part is the reload, and it's the reverse of what the source
    // had. The marine tops up quickest of anyone — 1.5 seconds — and pays 7.5
    // for being caught dry, five times over. The medic pays 1.75 either way it
    // matters, 3 seconds when the magazine runs out, and the gap between its
    // two numbers is the narrowest in the table. So the harsh magazine now sits
    // on the heavy class and the forgiving one on the light class, which is
    // where the two of them belong: a rifleman firing a twenty-five round
    // magazine to the click has made a choice and can be made to live with it,
    // while a medic is somewhere it had to run to and cannot be rooted there by
    // its own gun.
    //
    // The arithmetic of that is worth stating plainly, because it inverts the
    // order these two classes sat in for the whole life of this table.
    // SustainedFireRate now puts the medic ahead: about forty-one damage a
    // second against the marine's twenty-nine, half again as much, from the
    // weapon that hits softer per round. Nothing about the shooting causes it —
    // the burst rates are still identical — it is entirely that the marine's
    // dry reload is two and a half times the medic's and that a magazine fired
    // to the end is always replaced dry. A marine who never empties beats a
    // medic comfortably; a marine who holds the trigger down loses to one.
    //
    // That is a real trade rather than an oversight, and it is the marine's
    // reload key that carries it. ALL ROUNDER is the only class in the table
    // whose sustained output moves that much on how it's played, and the whole
    // of the difference is whether the pause gets spent in a lull at 1.5 or in
    // the open at 7.5. If it turns out the marine simply feels weak, that is
    // the number, and 7.5 is the one to move — not the cadence, and not the
    // damage, both of which are the zone's and both of which are fine.
    //
    // The sniper is its own shape: three bolts, fired one at a time, and then
    // the longest wait in the game. It carried a single round for a while on
    // the theory that a class this deadly wanted one shot that had to count,
    // and the theory was wrong in a way worth writing down, because it's the
    // kind of mistake this table invites. A magazine of one isn't a hard
    // version of a small magazine — it's a different mechanic. It deletes the
    // decision the reload key exists for, since a weapon that empties on every
    // shot can only ever pay reloadEmpty, and it makes a miss cost exactly what
    // a hit costs, so the class's whole skill expression collapses into the
    // seven-second wait. Three restores it: a bolt that misses is now a bolt
    // you can answer with, and a sniper sitting on two rounds in a lull has a
    // real question to ask — spend 5 seconds now, or risk paying 7 later.
    //
    // Three is also the zone's number, and it's the size that makes sense of
    // the 85 damage sitting beside it. Against a hundred-point soldier that's
    // two bolts a body, so a magazine is a kill and a follow-up for whoever
    // steps out to look — enough to punish a pair caught in the open, and not
    // enough for the squad the old comment here was afraid of. What holds it in
    // check isn't scarcity of rounds, it's everything else the class pays: the
    // slowest legs in the table, six units of dead ground in front of the
    // muzzle, and 2.2 seconds to empty against seven to refill. Over a long
    // fight SustainedFireRate puts it at about twenty-eight damage a second —
    // which, after everything above, is now level with the marine's twenty-nine
    // and well under the medic's forty-one, from the weapon that hits seven
    // times harder per round than either.
    //
    // This row has not moved through any of it, and the reloads around it have
    // moved three times, so the sniper has drifted from the class that spent
    // the most of a fight reloading to one of two that do. That's worth reading
    // as it stands rather than as a column to tidy: a sniper level with a
    // marine on damage over a long fight, while ending a body in two rounds at
    // twice the range, is not obviously wrong. Left alone deliberately — the
    // seven-second wait is the price the paragraph above says the class pays,
    // and every reload change so far came out of somebody playing the class and
    // finding the pause wrong. This one hasn't had that yet.
    //
    // The grenadier is the last shape and the only true single-shot left: one
    // shell, reloaded after every firing, which is the zone's own number too —
    // the launcher holds one there as well. For that row alone fireInterval
    // never gets to matter, the early reload can never be paid, and the empty
    // reload is the whole cadence.
    //
    // The magazine column comes from the zone entire — the marine's 25, the
    // medic's 20, the sniper's 3, the grenadier's 1 — and so did the reloads
    // beside it, until they were played. The zone lists both halves for every
    // weapon it issues: assault rifle 3.5/6, PDW 3/15, grenade launcher
    // 3.2/6.5, sniper rifle 5/7. Two of those four still stand — the sniper's
    // and the grenadier's, untouched. The automatics have had theirs replaced
    // outright, and they are the first numbers in this table to overrule the
    // source on nothing but how it felt to play.
    //
    // Which is the right reason, and is worth saying plainly so the next person
    // doesn't quietly restore them. Taking the zone's reloads at face value was
    // written down here as a deliberate bet — that the wait was meant to be a
    // thing that happens to you rather than a hitch in the shooting — and the
    // bet was explicitly the one to look at first if any of this felt wrong. It
    // did, in the direction the bet risked: the pauses were long enough to stop
    // being tension and start being downtime, and a fight kept emptying out
    // while both sides stood around waiting to be armed again. Everything below
    // came out of playing it, in three passes.
    //
    // First both automatics were halved, which fixed the downtime. That left
    // the medic still holding the zone's PDW shape at 1.5/7.5, and so still
    // reloading slower than the marine it runs rings around — a contradiction
    // the moment it's played, since FAST SUPPORT was quick everywhere except
    // the one moment it had to stand still. So the medic took the marine's
    // gentler pair. And then the pair the medic had been carrying turned out to
    // be the one that felt right on a rifleman, so the marine took that: the
    // long dry reload went to the class that can afford to be caught by it.
    //
    // The two ended up crossed over, and that's the whole design rather than an
    // accident of the order they were edited in. The zone's harshest magazine
    // is now on the heavy class and its most forgiving on the light one. If
    // these move again the thing to keep is which way round they sit — the
    // medic must not end up paying both a longer pause and a more frequent one,
    // which is being charged twice for one idea, and is exactly the state the
    // three passes above were walking away from.
    //
    // The PDW's fifteen is the number that got dropped, and it's worth
    // recording that dropping it was never about doubting the reading. The
    // zone's other fast-firing gun, the light machinegun, is 4/15 as well — a
    // punishing dry reload is that zone's standing price for a high rate of
    // fire, paid by every weapon that has one, and it is exactly what it looks
    // like. It just isn't a price this medic can pay. That zone hung the PDW on
    // whoever wanted it; here it is welded to the class built to be the first
    // one somewhere, and the same number that reads as a fair tax on a rate of
    // fire reads as a punishment for being the support class when there is only
    // one weapon it can be attached to. Its five-to-one wasn't thrown away
    // though — it came off the PDW and went onto the assault rifle, where the
    // class carrying it can afford the wait. What the zone was right about was
    // the shape; what it couldn't know is which of our four soldiers should
    // wear it.
    //
    // Two of the zone's rows have nowhere to land here: the ripper gun
    // is 4/6 and no class carries one, and the hand grenade's 2/2 is a reload
    // for a weapon that gets issued by the life instead (kGrenade below, which
    // never reloads).
    //
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
    //
    // Reach has a source too, and unlike sight it is read rather than derived:
    // it comes out of the same two rows of hardcorps2.itm the reloads and the
    // magazines do, which is also where the cadences come from — the assault
    // rifle's fire delay is 12 hundredths and the PDW's 10, and those are the
    // 0.12 and the 0.10 in the rows below. Every projectile in
    // hardcorps2.itm carries a muzzle velocity and an alive time — the server
    // repo's Docs/Itm names the columns, and its own aiming code fixes the unit
    // at thousandths of a pixel per ten-millisecond tick — and what survives the
    // trip is a set of ratios rather than distances. A range is the two columns
    // multiplied — speed times how long the round lives — and reading it off
    // either column alone is the mistake this paragraph used to make. It had
    // the sniper at 1.6, which is 400/250, the alive times divided. That looked
    // right because the PDW's 0.6 really is 150/250, and the PDW gets away with
    // it: the zone gives that weapon and the assault rifle the same muzzle
    // velocity, 15000, so the speeds cancel and the times alone happen to be
    // the answer. The sniper's is 18000, and the moment the speeds differ the
    // shortcut breaks. Multiplied out properly the rifle reaches 1.92 times the
    // assault rifle, not 1.6, and the PDW's 0.6 stands.
    //
    // Neither friction nor gravity muddies this: horizontal friction is 10000
    // on every projectile in the zone and gravity is 0 on all three flat
    // shooters, so speed times time really is the distance. The absolute
    // numbers still don't transfer, since that game measured across a map five
    // hundred tiles wide, but the spread does, and it's tighter than this table
    // had before any of it: our sniper was once reaching 2.4 times the marine.
    //
    // Reach here isn't a stat, it's a consequence. A round leaves the muzzle
    // level at 0.6 up and dies where it meets the floor, so how far it gets is
    // its speed times the same fall — about a third of a second for all of
    // them, a hair longer for the thinner rounds. That makes projectileSpeed
    // the only lever there is, and the speeds below are set so the reaches land
    // on the zone's ratios: marine 16.8 units, medic 10.1, sniper 32.3.
    //
    // The sniper is what the other two were fitted around, rather than the
    // marine everything else in this table is read against, and its reach is
    // load-bearing in both directions. The sight of 38 exists so LONG RANGE can
    // see the whole of what it shoots at, and correcting 1.6 to 1.92 spends
    // most of the room that was left: 32.3 against 38 leaves under six units of
    // margin where there were eleven. That is the number to watch if the ratio
    // moves again — a bolt that outranges the eye would put the class back to
    // firing into fog it has to be told about, which is the thing the 38 was
    // raised to end. The dead ground of 6 is unaffected, since it was measured
    // against the things a soldier meets rather than against the reach.
    //
    // The price of anchoring here is paid by the marine, whose round is half
    // again as fast (34 -> 51) and reaches half again as far — still the
    // largest single change in this table, and the first thing to look at if
    // the shooting starts feeling wrong. Against the AI it reads as a fix: a
    // rifleman opens fire at Brain::kEngageRange (14) and now covers that
    // ground with room to spare where it used to fall short. The medic still
    // can't reach kPreferredRange (11), the distance its own brain circles at,
    // which is a thing that was already true and isn't made worse here.
    //
    // The grenadier sits out of it. Its shell is lobbed, so the arc ends the
    // flight long before the fall would, and the zone's own launcher agrees —
    // the alive time on that row is a twenty-second safety net, not a range.
    // The zone's flamethrower has no class here to land on either way; its
    // ratio is 0.07, not the 0.4 this line used to give it, which was the same
    // divided-alive-times slip as the sniper's — that weapon crawls out at 2500
    // against the rifle's 15000, and dividing the times alone hid all of it.
    // name         blurb              color                          | speed accel  stop | sight | fire   mag early empty speed  radius mass   lob   life  min   dmg    blast bnce  boom   thrw   tracer            | ability         brain
    { "MARINE",    "ALL ROUNDER",      { 0.25f, 0.85f, 0.35f, 1.0f }, {  4.50f,  8.0f, 20.0f }, 30.0f, { 0.12f, 25, 1.50f, 7.50f, 51.0f, 0.11f, 0.40f, 0.0f, 3.0f, 0.0f, 12.0f, 0.0f, 0.0f, false, false, TracerId::Rifle }, Ability::kNone, Brain::Kind::Rifleman },
    { "MEDIC",     "FAST SUPPORT",     { 0.90f, 0.90f, 0.95f, 1.0f }, {  5.50f, 11.0f, 24.0f }, 26.0f, { 0.10f, 20, 1.75f, 3.00f, 29.0f, 0.09f, 0.30f, 0.0f, 3.0f, 0.0f, 10.0f, 0.0f, 0.0f, false, false, TracerId::Pdw   }, kFieldDressing, Brain::Kind::Rifleman },
    { "SNIPER",    "LONG RANGE",       { 0.62f, 0.40f, 0.96f, 1.0f }, {  3.50f,  6.5f, 22.0f }, 38.0f, { 1.10f,  3, 5.00f, 7.00f, 96.0f, 0.07f, 0.25f, 0.0f, 3.0f, 6.0f, 85.0f, 0.0f, 0.0f, false, false, TracerId::Bolt  }, Ability::kNone, Brain::Kind::Rifleman },
    { "GRENADIER", "LOBBED GRENADES",  { 0.98f, 0.70f, 0.12f, 1.0f }, {  3.75f,  6.0f, 13.0f }, 26.0f, { 0.90f,  1, 3.20f, 6.50f, 16.0f, 0.22f, 1.60f, 7.5f, 2.5f, 0.0f, 40.0f, 2.2f, 0.0f, true,  false, TracerId::Shell }, Ability::kNone, Brain::Kind::Rifleman },
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
    // fire  mag early empty speed  radius mass   lob   fuse  min   dmg    blast bnce  boom  thrw  tracer
       0.0f,  0,  0.0f, 0.0f,  7.0f, 0.20f, 0.80f, 8.0f, 3.0f, 0.0f, 30.0f, 4.0f, 0.45f, true, true, TracerId::None
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
