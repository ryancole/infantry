#include "World.h"

#include "Net.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;
using namespace DirectX::SimpleMath;

namespace
{
    constexpr float kPlayerHalf = 0.4f;
    constexpr float kMuzzleHeight = 0.6f;
    constexpr float kMuzzleOffset = 0.7f; // shots start this far ahead of the shooter
    constexpr float kGravity = 9.81f;     // must match Jolt's default gravity magnitude

    // Grace period on the trigger after a spawn, so the click that got the
    // player into the arena doesn't also fire their first shot.
    constexpr float kFireGrace = 0.3f;

    // How far an NPC can make anyone out at all — a fact about eyesight, not a
    // decision, which is why it's here and the range a brain chooses to fight
    // at is in Brain.cpp. Generous on purpose: it has to sit above whatever the
    // hungriest brain wants, or that brain quietly gets capped by a number it
    // can't see and nothing anywhere says so.
    constexpr float kNpcSightRange = 30.0f;
    // Random spread per shot, which keeps NPCs beatable up close and long-range
    // sniper duels survivable. A property of the whole AI rather than of any
    // one mind, so the body applies it.
    constexpr float kNpcAimJitter = 0.06f;

    // Line of sight is computed at eye level: colliders whose box doesn't
    // reach this height (low crates, curbs) can be seen and shot over.
    constexpr float kEyeHeight = 0.6f;

    // Soldier model walk cycle: phase advances with distance covered (radians
    // per unit moved) so stride length stays constant across class speeds; the
    // blend rate eases the walking pose in/out over ~1/8th of a second.
    constexpr float kStrideRate = 2.2f;
    constexpr float kMoveBlendRate = 8.0f;

    // A soldier carries their own weight: the walk chases the keys rather than
    // being them, so getting under way takes a stride and letting go costs a
    // coast. The two are not the same cost. Getting a body moving is work and
    // the slower of the two, and a direction change pays it again, which is
    // what stops a reversal from being free. Stopping is the body's own weight
    // going the way it was already headed, and only takes the moment it takes
    // — short enough that a dodge still answers on the frame it's asked for.
    // Both rates are MoveDef's, since how much body there is to get moving is
    // a class trait; what's left here is the floor the drift is snapped away
    // at, which is arithmetic rather than feel. Exponential decay never quite
    // reaches zero, and without a floor a released key leaves a soldier
    // creeping for the rest of the round at a speed too small to see.
    constexpr float kMoveStopSpeed = 0.1f; // units per second

    // Turning is not free: the soldier's facing chases the aim direction at a
    // fixed angular rate rather than snapping to it, so whipping the cursor
    // across the screen costs a moment spent pointed the wrong way. A full
    // about-face runs about one and two tenths of a second, which is long
    // enough that where a soldier is pointed is a commitment rather than a
    // preference, and being flanked is something to survive rather than answer.
    constexpr float kTurnRate = 2.67f; // radians per second

    // Holding steady trades the two things that get a soldier out of trouble —
    // moving and coming around — for a gun that sits where it's put. At a
    // quarter of the turn rate the facing stops chasing the hand and starts
    // filtering it, so small wobble never reaches the muzzle and a target the
    // cursor crosses isn't one the barrel swept over. What it costs is being
    // committed: a full about-face runs the better part of five seconds, which
    // means anyone who reaches the flank while it's held has already won.
    constexpr float kSteadyMoveScale = 0.4f;  // fraction of class move speed
    constexpr float kSteadyTurnScale = 0.25f; // fraction of kTurnRate

    // Launch velocity from the killing blow: a shove along it plus some lift,
    // so a body falls away from what killed it instead of dropping in place. A
    // blast throws harder, scaled by how much of it the victim caught.
    constexpr float kCorpseKnock = 4.5f;
    constexpr float kCorpseBlastKnock = 11.0f;
    constexpr float kCorpseLift = 3.0f;

    // Below this speed a bouncing grenade's contacts are a roll, not a bounce,
    // and stay silent — otherwise a grenade resting on the floor reports a
    // contact every tick and chatters.
    constexpr float kBounceSoundSpeed = 2.5f; // units per second

    // Velocity a killing blow hands to the corpse it makes: a shove along the
    // blow, flattened to the ground plane, plus enough lift that the body
    // falls away from it instead of dropping straight down.
    Vector3 CorpseKnock(const Vector3& dir, float strength)
    {
        Vector3 flat(dir.x, 0.0f, dir.z);
        if (flat.LengthSquared() > 1e-8f)
            flat.Normalize();
        return flat * strength + Vector3(0.0f, kCorpseLift, 0.0f);
    }

    // Swings `from` toward `to` about Y by at most `maxStep` radians, snapping
    // to `to` once it's within reach. Both are unit vectors on the ground
    // plane. The cross product against the dot gives the signed angle between
    // them; CreateRotationY turns the opposite way, hence the negated step.
    Vector3 TurnToward(const Vector3& from, const Vector3& to, float maxStep)
    {
        const float delta = std::atan2(from.x * to.z - from.z * to.x, from.Dot(to));
        if (std::abs(delta) <= maxStep)
            return to;
        return Vector3::Transform(from,
                                  Matrix::CreateRotationY(-std::copysign(maxStep, delta)));
    }

    // Horizontal muzzle speed for a shot aimed to come down targetDist away:
    // bullets always fire at full speed; lobbed shots slow their toss to drop
    // on the aim point, capped at the weapon's muzzle speed.
    float ShotSpeed(const WeaponDef& weapon, float targetDist)
    {
        float speed = weapon.projectileSpeed;
        if (weapon.lobVelocity > 0.0f)
        {
            const float vy = weapon.lobVelocity;
            const float flightTime =
                (vy + std::sqrt(vy * vy + 2.0f * kGravity * kMuzzleHeight)) / kGravity;
            const float d = std::max(targetDist - kMuzzleOffset, 0.2f);
            speed = std::min(d / flightTime, speed);
        }
        return speed;
    }

    // True if the segment a->b passes within `pad` of the axis-aligned box at
    // `center` with half-extents `half` (slab test against the padded box).
    // Projectile hits are swept over the tick's travel so fast shots (the
    // sniper round moves over a body-width per tick) can't tunnel through.
    bool SegmentHitsBox(const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT3& center,
                        const XMFLOAT3& half, float pad)
    {
        const float d[3] = { b.x - a.x, b.y - a.y, b.z - a.z };
        const float o[3] = { a.x - center.x, a.y - center.y, a.z - center.z };
        const float h[3] = { half.x + pad, half.y + pad, half.z + pad };
        float tMin = 0.0f, tMax = 1.0f;
        for (int i = 0; i < 3; ++i)
        {
            if (std::abs(d[i]) < 1e-6f)
            {
                if (std::abs(o[i]) > h[i])
                    return false;
                continue;
            }
            float t0 = (-h[i] - o[i]) / d[i];
            float t1 = (h[i] - o[i]) / d[i];
            if (t0 > t1)
                std::swap(t0, t1);
            tMin = std::max(tMin, t0);
            tMax = std::min(tMax, t1);
            if (tMin > tMax)
                return false;
        }
        return true;
    }
}

void World::Init(const LevelData& level)
{
    m_arenaHalf = level.arenaHalf;
    for (const LevelData::Spawn& spawn : level.spawns)
        m_teamSpawns.push_back(spawn.pos);
    m_nextAiClass.assign(m_teamSpawns.size(), 0);
    m_humanSlots.assign(m_teamSpawns.size(), 0);

    // Arena floor. Projectiles are dynamic bodies under Jolt gravity; they
    // despawn on their first contact with anything solid.
    m_physics.AddStaticBox({ 0.0f, -0.5f, 0.0f },
                           { m_arenaHalf * 2.0f, 1.0f, m_arenaHalf * 2.0f });

    // The simulation's half of each level object: collision box, and its
    // footprint in the occluder list if it stands tall enough to block sight.
    for (const LevelData::Object& obj : level.objects)
    {
        if (!obj.collider)
            continue;
        const XMFLOAT3 size = { obj.collider->x * obj.scale, obj.collider->y * obj.scale,
                                obj.collider->z * obj.scale };
        const XMFLOAT3 center = { obj.pos.x, obj.pos.y + size.y * 0.5f, obj.pos.z };
        m_colliders.push_back({ center, size, obj.model.empty() });
        m_physics.AddStaticBox(center, size);

        if (center.y + size.y * 0.5f >= kEyeHeight && center.y - size.y * 0.5f <= kEyeHeight)
            m_occluders.push_back({ center.x - size.x * 0.5f, center.z - size.z * 0.5f,
                                    center.x + size.x * 0.5f, center.z + size.z * 0.5f });
    }
}

void World::StartMatch(const ClassDef* localClass, int localTeam)
{
    m_localClass = localClass;
    m_localTeam = std::min(localTeam, static_cast<int>(m_teamSpawns.size()) - 1);
    if (m_localClass)
    {
        m_localTeam = ClaimSlot(m_localTeam);
        SpawnLocal();
    }
    for (int team = 0; team < static_cast<int>(m_teamSpawns.size()); ++team)
        for (int i = AiCount(team); i < AiQuota(team); ++i)
            SpawnAi(team);
}

void World::SpawnLocal()
{
    // Nothing to spawn on a machine that never picked a class — a dedicated
    // server calls StartMatch with no local and never comes back here.
    if (!m_localClass)
        return;
    SpawnHuman(*m_localClass, m_localTeam, Unit::Controller::Local);
}

int World::SpawnRemote(const ClassDef& cls, int team)
{
    return SpawnHuman(cls, team, Unit::Controller::Remote);
}

int World::SpawnHuman(const ClassDef& cls, int team, Unit::Controller controller)
{
    Unit unit = {};
    unit.id = m_nextUnitId++;
    unit.cls = &cls;
    unit.controller = controller;
    unit.team = team;
    unit.pos = m_teamSpawns[team];
    unit.aimDir = Vector3::UnitX;
    unit.hp = kMaxHealth;
    unit.fireCooldown = kFireGrace; // so the click that got them here doesn't fire a shot
    unit.ammo = unit.cls->primary.magazine;
    unit.grenades = kGrenadesPerLife;
    unit.meleeCharges = kMelee.charges;
    // A fresh soldier has no last tick; they're drawn where they stand.
    unit.prevPos = unit.pos;
    unit.prevAimDir = unit.aimDir;
    unit.prevWalkPhase = unit.walkPhase;
    unit.prevMoveBlend = unit.moveBlend;
    m_units.push_back(unit);
    return unit.id;
}

int World::ClaimSlot(int team)
{
    team = std::clamp(team, 0, TeamCount() - 1);
    ++m_humanSlots[team];
    // The AI hands the slot over now rather than on its next death: a living
    // AI soldier on the side is removed outright — no corpse, no event,
    // nobody watching a squadmate evaporate would call it a death, because
    // it isn't one. Whoever is furthest down the roster rotates out.
    while (AiCount(team) > AiQuota(team))
    {
        for (size_t i = m_units.size(); i-- > 0;)
        {
            Unit& u = m_units[i];
            if (u.controller == Unit::Controller::Ai && u.team == team && u.hp > 0.0f)
            {
                m_units.erase(m_units.begin() + i);
                break;
            }
        }
    }
    return team;
}

void World::ReleaseSlot(int team)
{
    team = std::clamp(team, 0, TeamCount() - 1);
    if (m_humanSlots[team] > 0)
        --m_humanSlots[team];
    // The side is owed a soldier again, on the same clock a death starts: a
    // leaver's slot refills the way a casualty's does, not instantly.
    m_reinforcements.push_back({ team, kRespawnDelay });
}

int World::HumanSlots(int team) const
{
    return team < static_cast<int>(m_humanSlots.size()) ? m_humanSlots[team] : 0;
}

int World::Standing(int team) const
{
    if (!m_standingOverride.empty())
        return team < static_cast<int>(m_standingOverride.size()) ? m_standingOverride[team]
                                                                  : 0;
    return static_cast<int>(std::count_if(m_units.begin(), m_units.end(), [team](const Unit& u) {
        return u.team == team && u.hp > 0.0f;
    }));
}

void World::RemoveUnit(int id)
{
    std::erase_if(m_units, [id](const Unit& u) { return u.id == id; });
}

void World::SetCommand(int id, const Command& cmd)
{
    for (auto& staged : m_staged)
        if (staged.first == id)
        {
            staged.second = cmd;
            return;
        }
    m_staged.push_back({ id, cmd });
}

void World::SpawnAi(int team)
{
    // An AI soldier appears at its own team's spawn point, scattered a little
    // so a squad arriving at once doesn't stack on one tile. A squadmate turns
    // up where the player turns up and a hostile across the arena, because the
    // spawn a soldier comes from is a fact about the side they're on.
    Unit unit = {};
    unit.id = m_nextUnitId++;
    unit.cls = &kClassDefs[m_nextAiClass[team]];
    m_nextAiClass[team] = (m_nextAiClass[team] + 1) % static_cast<int>(kClassCount);
    unit.controller = Unit::Controller::Ai;
    unit.team = team;
    unit.pos = m_teamSpawns[team];
    unit.pos.x += Rand(-2.0f, 2.0f);
    unit.pos.z += Rand(-2.0f, 2.0f);
    ResolveObstacles(unit.pos);
    unit.aimDir = { -1.0f, 0.0f, 0.0f };
    unit.hp = kMaxHealth;
    unit.fireCooldown = 0.5f; // brief grace so spawns don't instantly fire
    unit.ammo = unit.cls->primary.magazine;
    unit.grenades = kGrenadesPerLife; // issued even if today's brains never throw one
    unit.meleeCharges = kMelee.charges;
    Brain::Wake(unit.mind, unit.pos, m_rng);
    unit.walkPhase = Rand(0.0f, XM_2PI); // desync strides across the squad
    // A fresh soldier has no last tick; they're drawn where they stand.
    unit.prevPos = unit.pos;
    unit.prevAimDir = unit.aimDir;
    unit.prevWalkPhase = unit.walkPhase;
    unit.prevMoveBlend = unit.moveBlend;
    m_units.push_back(unit);
}

void World::Tick(const Command* cmd, std::vector<Event>& events)
{
    m_out = &events;

    // Every soldier's "where I was" becomes the tick that's about to run:
    // a renderer draws between these and wherever the tick puts them.
    for (Unit& unit : m_units)
    {
        unit.prevPos = unit.pos;
        unit.prevAimDir = unit.aimDir;
        unit.prevWalkPhase = unit.walkPhase;
        unit.prevMoveBlend = unit.moveBlend;
    }

    if (cmd)
        if (Unit* local = Local())
            ApplyCommand(*local, *cmd, kTickDt);

    // The connected players' commands, staged since the last tick. Applied
    // the same way the local one is, because they are the same thing; the
    // stage is consumed whole, so a client that goes quiet stops moving
    // rather than repeating its last wish forever — holding the last command
    // through a gap is its server session's call to make, not this class's.
    for (const auto& [id, staged] : m_staged)
        if (Unit* unit = UnitById(id))
            if (unit->controller == Unit::Controller::Remote && unit->hp > 0.0f)
                ApplyCommand(*unit, staged, kTickDt);
    m_staged.clear();

    UpdateUnits(kTickDt);
    m_physics.Step(kTickDt);
    UpdateProjectiles(kTickDt);
    ReapDead();
    UpdateReinforcements(kTickDt);

    m_out = nullptr;
}

Unit* World::Local()
{
    for (Unit& unit : m_units)
        if (unit.controller == Unit::Controller::Local)
            return &unit;
    return nullptr;
}

const Unit* World::Local() const
{
    return const_cast<World*>(this)->Local();
}

Unit* World::UnitById(int id)
{
    for (Unit& unit : m_units)
        if (unit.id == id)
            return &unit;
    return nullptr;
}

int World::AiCount(int team) const
{
    return static_cast<int>(std::count_if(m_units.begin(), m_units.end(), [team](const Unit& u) {
        return u.controller == Unit::Controller::Ai && u.team == team && u.hp > 0.0f;
    }));
}

void World::TickClocks(Unit& unit, float dt)
{
    unit.fireCooldown -= dt;
    unit.meleeCooldown -= dt;

    // The blade's recovery runs on its own and takes no key: there's nothing
    // to decide about it, so there's nothing to press. It's a timer since the
    // last swing, not a reload of an empty magazine, so it runs with swings
    // still in hand and doesn't gate them.
    if (unit.meleeRecover > 0.0f)
    {
        unit.meleeRecover -= dt;
        if (unit.meleeRecover <= 0.0f)
        {
            unit.meleeRecover = 0.0f;
            unit.meleeCharges = kMelee.charges;
        }
    }

    if (unit.reloadTimer > 0.0f)
    {
        unit.reloadTimer -= dt;
        if (unit.reloadTimer <= 0.0f)
        {
            unit.reloadTimer = 0.0f;
            unit.ammo = unit.cls->primary.magazine;
            Event ev;
            ev.type = Event::Type::ReloadEnd;
            ev.unit = unit.id;
            ev.local = unit.controller == Unit::Controller::Local;
            ev.pos = unit.pos;
            Emit(ev);
        }
    }
}

void World::MoveCommand(Unit& unit, const Command& cmd, float dt) const
{
    // --- Movement. The command asks for a velocity; the body eases onto it.
    // Framed as a decay toward the wanted velocity rather than a step of
    // acceleration so the feel holds at any frame rate. Which rate it decays
    // at is decided by whether anything is being asked for at all, not by
    // whether the answer happens to be faster or slower: a soldier hauling
    // themselves round onto a new heading is doing the work of starting, even
    // though their speed never changed, and it should cost what starting costs ---
    const MoveDef& gait = unit.cls->move;
    const float speed = gait.speed * (cmd.steady ? kSteadyMoveScale : 1.0f);
    const bool pushing = cmd.move.LengthSquared() > 1e-10f;
    const float response = pushing ? gait.accel : gait.stop;
    unit.moveVel += (cmd.move * speed - unit.moveVel) * (1.0f - std::exp(-response * dt));
    if (unit.moveVel.LengthSquared() < kMoveStopSpeed * kMoveStopSpeed)
        unit.moveVel = Vector3::Zero;
    unit.pos += unit.moveVel * dt;
    ResolveObstacles(unit.pos);

    // Stride off the speed actually travelled, not the class's, so the feet
    // keep up with the ground at either pace instead of skating over it — and
    // so the legs keep walking through the coast rather than stopping with the
    // key while the body is still sliding.
    const float travelSpeed = unit.moveVel.Length();
    const bool moving = travelSpeed > 0.0f;
    unit.walkPhase += travelSpeed * kStrideRate * dt;
    unit.moveBlend = std::clamp(unit.moveBlend + (moving ? kMoveBlendRate : -kMoveBlendRate) * dt,
                                0.0f, 1.0f);

    // --- Aim. The command is where the driver wants to be pointed, not where
    // they are pointed — the body has to come around to it. Everything that
    // reads the aim, including the shots and the aim ring, follows the facing
    // rather than the wish, so what's drawn is what will fire ---
    if (cmd.aim.LengthSquared() > 1e-8f)
    {
        const float turnRate = kTurnRate * (cmd.steady ? kSteadyTurnScale : 1.0f);
        unit.aimDir = TurnToward(unit.aimDir, cmd.aim, turnRate * dt);
    }
}

void World::ApplyCommand(Unit& unit, const Command& cmd, float dt)
{
    TickClocks(unit, dt);

    MoveCommand(unit, cmd, dt);

    // --- Reload: the magazine runs out mid-firefight, and getting a fresh one
    // in costs the soldier their guns for a moment. It can be started early,
    // which is the whole decision the system asks for — top up in the lull, or
    // get caught doing it. The cadence timer keeps running underneath, so a
    // reload never doubles as a way to skip one ---
    if (cmd.reload)
        BeginReload(unit);

    // Set by anything that puts a hand back on a weapon this tick. The class
    // ability below reads it for both halves of one rule: it drops a dressing
    // that was running, and it stops one from starting. Without the second
    // half, a player leaning on the trigger could press the ability key, start
    // something, and have it cancelled by their own gunfire on the same tick
    // — paying the whole cooldown for a dressing that never got a moment to
    // itself, with nothing on screen long enough to explain why.
    bool weaponUsed = false;

    // --- Firing ---
    if (cmd.fire && unit.fireCooldown <= 0.0f && unit.reloadTimer <= 0.0f && unit.ammo > 0)
    {
        SpawnShot(unit.cls->primary, unit.pos, unit.aimDir, unit.team, cmd.aimDist);
        // Carry the overshoot, so a cadence that isn't a whole number of ticks
        // still averages the class table's number instead of rounding up to
        // the next tick every shot — but never more than one tick of credit,
        // or a lull would come back as a burst.
        unit.fireCooldown =
            std::max(unit.fireCooldown, -kTickDt) + unit.cls->primary.fireInterval;
        weaponUsed = true;
        // Empty: reload without being asked. Holding an empty weapon is never
        // the play, so making the player press for it would only cost them the
        // time it took to notice.
        if (--unit.ammo == 0)
            BeginReload(unit);
    }

    // --- Grenade: same for every class, lobbed onto the aim point, then left
    // to bounce until its fuse runs out (UpdateProjectiles). One per life, so
    // there's no cooldown to run down — spending it is the whole cost. It comes
    // off the belt rather than out of the magazine, so it's still throwable
    // mid-reload: caught empty, a player has one thing left to do. An edge in
    // the command, which with a single grenade also stops a held key from
    // throwing it before the player means to ---
    if (cmd.grenade && unit.grenades > 0)
    {
        SpawnShot(kGrenade, unit.pos, unit.aimDir, unit.team, cmd.aimDist);
        --unit.grenades;
        weaponUsed = true;
    }

    // --- Melee: the blade, for the range the primary can't cover. Its charges
    // and its recovery are entirely off the magazine, so — like the grenade —
    // it's still in the soldier's hands mid-reload, which is most of why it's
    // carried. Held rather than tapped, like the trigger and unlike the
    // grenade: the three swings are a burst, paced by swingInterval, and a
    // player who wants all three wants them as fast as the arm will go.
    // Rationing them by hand-speed would only be a test of the hand. Leaning
    // on the key through the recovery does nothing — the charges come back and
    // the swinging picks straight back up, the same deal the trigger offers ---
    if (cmd.melee && unit.meleeCooldown <= 0.0f && unit.meleeCharges > 0)
    {
        SwingMelee(unit);
        weaponUsed = true;
    }

    // --- Ability: the one thing this class can do that the others can't, on
    // an edge rather than a hold — it isn't a trigger. Everything it decides
    // it decides in Ability::Update: when it starts, what it does with the
    // time, and what taking a weapon back costs. All this end of it owns is
    // the command's ask and the answer to who's on the field ---
    const Ability::Def& ability = unit.cls->ability;
    if (Ability::Update(ability, unit.ability, AbilityScene(unit), dt, cmd.ability, weaponUsed) &&
        ability.startSound)
    {
        Event ev;
        ev.type = Event::Type::AbilityStart;
        ev.unit = unit.id;
        ev.local = unit.controller == Unit::Controller::Local;
        ev.pos = unit.pos;
        ev.cls = unit.cls; // rides the wire as a class index; the sound comes back off it
        ev.sound = ability.startSound;
        Emit(ev);
    }
}

void World::BeginReload(Unit& unit)
{
    const WeaponDef& weapon = unit.cls->primary;
    if (unit.reloadTimer > 0.0f || unit.ammo >= weapon.magazine)
        return;

    // Whatever was left in the magazine goes with it: partial reloads would
    // make tapping R between every shot strictly correct, and there's nothing
    // interesting about a player who does that.
    unit.ammo = 0;
    unit.reloadTimer = weapon.reloadTime;
    Event ev;
    ev.type = Event::Type::ReloadStart;
    ev.unit = unit.id;
    ev.local = unit.controller == Unit::Controller::Local;
    ev.pos = unit.pos;
    Emit(ev);
}

void World::SwingMelee(Unit& attacker)
{
    // The charge goes with the swing, not with the hit: the arm travels the
    // same arc either way, and a blade that only charged for connecting would
    // be free to flail with from across the room.
    --attacker.meleeCharges;
    attacker.meleeCooldown = kMelee.swingInterval;
    // Every swing restarts the recovery, spent or not, so what comes back is
    // always the full three and the wait is always measured from the last thing
    // the arm did. Swinging twice and stopping costs the same wait as swinging
    // three times; the third swing is free to take or leave.
    attacker.meleeRecover = kMelee.recoverTime;

    Event swing;
    swing.type = Event::Type::MeleeSwing;
    swing.unit = attacker.id;
    swing.local = attacker.controller == Unit::Controller::Local;
    swing.pos = attacker.pos;
    swing.dir = attacker.aimDir;
    Emit(swing);

    // One edge, not a blast: of everyone standing in the arc only the nearest
    // is struck. It gets no sight test, unlike a bullet or a blast — anything
    // solid enough to be cover is wider than the reach and keeps the two of
    // them apart by itself, and what's left is a tree trunk they're both
    // already touching, which isn't worth stopping a swing over.
    const float cosArc = std::cos(kMelee.arc);
    Unit* target = nullptr;
    float nearest = 0.0f;
    for (Unit& other : m_units)
    {
        if (other.hp <= 0.0f || other.team == attacker.team) // the blade doesn't cut its own side
            continue;
        Vector3 toward = other.pos - attacker.pos;
        toward.y = 0.0f;
        const float dist = toward.Length();
        if (dist > kMelee.reach || dist < 1e-4f)
            continue;
        if (toward.Dot(attacker.aimDir) / dist < cosArc)
            continue;
        if (!target || dist < nearest)
        {
            target = &other;
            nearest = dist;
        }
    }
    if (!target)
        return;

    // From here it's a hit like any other: the same blood, thrown the way the
    // blade was travelling, and the same knock on the corpse it may leave. The
    // body itself is collected by ReapDead at the end of the tick.
    target->hp -= kMelee.damage;
    target->knock = CorpseKnock(attacker.aimDir, kCorpseKnock);
    Event hit;
    hit.type = Event::Type::Hit;
    hit.unit = target->id;
    hit.local = target->controller == Unit::Controller::Local;
    hit.pos = { target->pos.x, kPlayerHalf, target->pos.z };
    hit.dir = attacker.aimDir;
    hit.damage = kMelee.damage;
    hit.fatal = target->hp <= 0.0f;
    Emit(hit);
}

Ability::Scene World::AbilityScene(Unit& user)
{
    m_abilityAllies.clear();
    const Vector2 userXZ = { user.pos.x, user.pos.z };
    // Three of the four classes have no ability, and a sight test per squadmate
    // per tick isn't free enough to spend on nobody.
    if (user.cls->ability.kind != Ability::Kind::None)
    {
        for (Unit& other : m_units)
        {
            if (&other == &user || other.team != user.team || other.hp <= 0.0f)
                continue;
            if (!Visibility::IsPointVisible(userXZ, { other.pos.x, other.pos.z }, m_occluders))
                continue;
            m_abilityAllies.push_back({ other.pos, &other.hp });
        }
    }

    return { { user.pos, &user.hp }, user.aimDir, kMaxHealth, &m_abilityAllies };
}

void World::SpawnShot(const WeaponDef& weapon, const Vector3& from, const Vector3& dir, int team,
                      float targetDist)
{
    Vector3 pos = from + dir * kMuzzleOffset;
    pos.y = kMuzzleHeight;

    // Grenades keep their fixed upward lob (constant arc height and flight
    // time) and vary horizontal speed to land on the aim point; bullets fire
    // level at full speed, so max range comes from gravity.
    const Vector3 vel =
        dir * ShotSpeed(weapon, targetDist) + Vector3(0.0f, weapon.lobVelocity, 0.0f);
    m_projectiles.push_back({ m_physics.SpawnProjectile(pos, vel, weapon.projectileRadius,
                                                        weapon.projectileMass, weapon.bounce),
                              weapon.projectileLife, pos, pos, team, weapon.damage,
                              weapon.projectileRadius, weapon.blastRadius,
                              weapon.bounce > 0.0f, weapon.explodes });

    Event ev;
    ev.type = Event::Type::Fire;
    ev.pos = from;
    ev.dir = dir;
    ev.damage = weapon.damage; // heavier weapons report deeper; see the Fire handler
    Emit(ev);
}

float World::PredictShotStop(const WeaponDef& weapon, const Vector3& from, const Vector3& dir,
                             float targetDist, std::vector<Vector3>* outArc) const
{
    const float speed = ShotSpeed(weapon, targetDist);
    const float radius = weapon.projectileRadius;
    if (outArc)
    {
        outArc->clear();
        outArc->push_back({ from.x + dir.x * kMuzzleOffset, kMuzzleHeight,
                            from.z + dir.z * kMuzzleOffset });
    }
    // Coarser than the physics tick, but the arc is smooth and the boxes are
    // fat relative to per-step travel, so the indicator lands within a step.
    constexpr float kStep = 1.0f / 120.0f;
    for (float t = kStep; t < weapon.projectileLife; t += kStep)
    {
        const float ht = kMuzzleOffset + speed * t;
        const float y = kMuzzleHeight + weapon.lobVelocity * t - 0.5f * kGravity * t * t;
        if (outArc)
            outArc->push_back({ from.x + dir.x * ht, std::max(y, radius),
                                from.z + dir.z * ht });
        if (y <= radius) // came back down to the ground
            return ht;

        const Vector3 p(from.x + dir.x * ht, y, from.z + dir.z * ht);
        for (const Collider& c : m_colliders)
            if (std::abs(p.x - c.center.x) <= c.size.x * 0.5f + radius &&
                std::abs(p.y - c.center.y) <= c.size.y * 0.5f + radius &&
                std::abs(p.z - c.center.z) <= c.size.z * 0.5f + radius)
                return ht;
    }
    return kMuzzleOffset + speed * weapon.projectileLife;
}

float World::Rand(float lo, float hi)
{
    return std::uniform_real_distribution<float>(lo, hi)(m_rng);
}

void World::Emit(const Event& ev)
{
    if (m_out)
        m_out->push_back(ev);
}

void World::ResolveObstacles(Vector3& pos) const
{
    const float limit = m_arenaHalf - kPlayerHalf;
    pos.x = std::clamp(pos.x, -limit, limit);
    pos.z = std::clamp(pos.z, -limit, limit);

    // Keep soldiers out of solid objects: push out along the axis of least
    // penetration. (Kinematic on purpose — movement should stay crisp, so
    // soldiers don't live in the physics world yet.)
    for (const Collider& c : m_colliders)
    {
        const float ex = c.size.x * 0.5f + kPlayerHalf;
        const float ez = c.size.z * 0.5f + kPlayerHalf;
        const float px = pos.x - c.center.x;
        const float pz = pos.z - c.center.z;
        if (std::abs(px) >= ex || std::abs(pz) >= ez)
            continue;
        const float pushX = (px > 0.0f) ? ex - px : -ex - px;
        const float pushZ = (pz > 0.0f) ? ez - pz : -ez - pz;
        if (std::abs(pushX) < std::abs(pushZ))
            pos.x += pushX;
        else
            pos.z += pushZ;
    }
}

void World::UpdateReinforcements(float dt)
{
    // Two passes and a sweep, the way the dead are collected: the spawn has to
    // happen between deciding a slot is due and forgetting about it, and doing
    // all three in one loop would mean adding to one list while erasing from
    // another.
    for (Reinforcement& slot : m_reinforcements)
        slot.timer -= dt;

    for (const Reinforcement& slot : m_reinforcements)
    {
        // Counted fresh each time, so two slots coming due on the same tick
        // both get filled. The quota is checked rather than assumed because the
        // roster is what says how many a side fields — the queue only says when
        // — and a wait that outlived its slot should be dropped, not honored.
        if (slot.timer <= 0.0f && AiCount(slot.team) < AiQuota(slot.team))
            SpawnAi(slot.team);
    }

    std::erase_if(m_reinforcements, [](const Reinforcement& slot) { return slot.timer <= 0.0f; });
}

void World::UpdateUnits(float dt)
{
    for (Unit& unit : m_units)
    {
        // The command-driven bodies are ApplyCommand's business — input rather
        // than Intent — so only the AI-driven ones run here.
        if (unit.controller != Unit::Controller::Ai)
            continue;

        // The same clocks the player's body runs, ticked by the same code —
        // NPCs reload on the same terms the player does, so the lull after a
        // squad empties its magazines is a real opening rather than something
        // only one side has to live with. They reload wherever they are —
        // breaking off to do it under cover is a decision the AI isn't smart
        // enough to make yet.
        TickClocks(unit, dt);

        // Who this soldier can see. Everyone hostile, at whatever range, with
        // the same sight test the player's fog of war uses so nobody shoots
        // through a wall the player can't see through either — and no further
        // opinion than that. How close is close enough to fight is a matter of
        // temperament, so it belongs to the brain; how far a soldier can pick
        // anyone out at all is a fact about the world, so it's here. The local
        // player is just one more hostile on the roster — and while they're
        // dead they aren't on it, so nothing aims at where they were.
        //
        // It runs a sight test per candidate, which makes this quadratic in the
        // number of soldiers on the field; at two squads of five that's a
        // hundred segment tests a tick, which is cheaper than the bookkeeping
        // to avoid it. The number to watch it against is kTeamSize, and it will
        // want revisiting long before this arena holds a proper Infantry zone.
        m_contacts.clear();
        const Vector2 unitXZ = { unit.pos.x, unit.pos.z };
        const auto sight = [&](const Vector3& pos) {
            const float d = Vector2(pos.x - unit.pos.x, pos.z - unit.pos.z).Length();
            if (d < 1e-3f || d > kNpcSightRange)
                return;
            if (Visibility::IsPointVisible(unitXZ, { pos.x, pos.z }, m_occluders))
                m_contacts.push_back({ pos, d });
        };
        for (const Unit& other : m_units)
            if (&other != &unit && other.hp > 0.0f && other.team != unit.team)
                sight(other.pos);

        const WeaponDef& weapon = unit.cls->primary;
        const bool canFire =
            unit.fireCooldown <= 0.0f && unit.reloadTimer <= 0.0f && unit.ammo > 0;
        const Brain::Senses senses = { unit.pos, unit.aimDir, &m_contacts, m_arenaHalf, canFire };
        const Brain::Intent intent =
            Brain::Think(unit.cls->brain, unit.mind, senses, dt, m_rng);

        // From here it's the body: everything a soldier does the same way
        // whichever mind is driving it.
        if (intent.facing.LengthSquared() > 1e-6f)
            unit.aimDir = intent.facing;

        if (intent.fire && canFire)
        {
            // A touch of angular spread keeps NPCs beatable up close and makes
            // long-range sniper duels survivable. It stays out here rather than
            // being a brain's to set: it's a fairness knob on the whole AI, not
            // a personality — a brain that could tighten its own aim would be a
            // brain that could decide how hard it is to play against.
            const Vector3 dir = Vector3::Transform(
                unit.aimDir, Matrix::CreateRotationY(Rand(-kNpcAimJitter, kNpcAimJitter)));
            // NPCs "aim" at what they're shooting at rather than at max range,
            // so a grenadier's lob comes down on them.
            SpawnShot(weapon, unit.pos, dir, unit.team, intent.fireDist);
            // Same overshoot carry as the command path, for the same cadence.
            unit.fireCooldown = std::max(unit.fireCooldown, -kTickDt) + weapon.fireInterval;
            if (--unit.ammo == 0)
                BeginReload(unit);
        }

        // NPCs still move on the speed alone — the momentum rates are the
        // player's, and giving the AI weight is a change to how it steers
        // rather than one more field to read.
        const float speed = unit.cls->move.speed * intent.speedScale;
        const bool moving = intent.move.LengthSquared() > 1e-6f;
        if (moving)
            unit.walkPhase += speed * kStrideRate * dt;
        unit.moveBlend =
            std::clamp(unit.moveBlend + (moving ? kMoveBlendRate : -kMoveBlendRate) * dt,
                       0.0f, 1.0f);

        unit.pos += intent.move * (speed * dt);
        ResolveObstacles(unit.pos);
    }

    // Pairwise separation so a group never collapses into a single column.
    // Still an AI-only rule: the local player has never been shoved by their
    // squad, and starting to would be a gameplay change, not a refactor.
    for (size_t i = 0; i < m_units.size(); ++i)
        for (size_t j = i + 1; j < m_units.size(); ++j)
        {
            if (m_units[i].controller != Unit::Controller::Ai ||
                m_units[j].controller != Unit::Controller::Ai)
                continue;
            const Vector3 between = m_units[j].pos - m_units[i].pos;
            const float len = between.Length();
            const float minDist = kPlayerHalf * 2.0f;
            if (len >= minDist || len < 1e-5f)
                continue;
            const Vector3 push = between * ((minDist - len) * 0.5f / len);
            m_units[i].pos -= push;
            m_units[j].pos += push;
        }
}

void World::ApplyBlast(const Vector3& center, float radius, float damage, int team)
{
    // Linear falloff from full damage at the center to nothing at the rim,
    // measured to the body's middle so a blast overhead still counts. A wall
    // between the two eats it: same sight test the fog of war and NPC AI use,
    // so what stops a bullet stops the shrapnel.
    const auto splash = [&](const Vector3& target) {
        if (!Visibility::IsPointVisible({ center.x, center.z }, { target.x, target.z },
                                        m_occluders))
            return 0.0f;
        const float d = (target - center).Length();
        return d >= radius ? 0.0f : damage * (1.0f - d / radius);
    };

    // Like direct hits, a blast only hurts the other side — every soldier on
    // it, the local player on exactly the same terms as their squadmates: one
    // loop over the roster, with no second copy for whoever this machine is
    // driving.
    for (Unit& unit : m_units)
    {
        if (unit.hp <= 0.0f || unit.team == team) // already killed this tick, or on the thrower's side
            continue;
        const float dmg = splash({ unit.pos.x, kPlayerHalf, unit.pos.z });
        if (dmg <= 0.0f)
            continue;
        unit.hp -= dmg;
        // Blown outward from the blast, as hard as the share of it they
        // caught: a body at the rim topples, one on top of it is thrown.
        unit.knock = CorpseKnock(unit.pos - center, kCorpseBlastKnock * (dmg / damage));
        // Blood goes the same way the blast threw them, out of the middle.
        Event ev;
        ev.type = Event::Type::Hit;
        ev.unit = unit.id;
        ev.local = unit.controller == Unit::Controller::Local;
        ev.pos = { unit.pos.x, kPlayerHalf, unit.pos.z };
        ev.dir = unit.pos - center;
        ev.damage = dmg;
        ev.fatal = unit.hp <= 0.0f;
        Emit(ev);
    }
}

void World::Detonate(const Projectile& shot, const Vector3& pos, bool hitUnit)
{
    if (shot.blastRadius > 0.0f)
        ApplyBlast(pos, shot.blastRadius, shot.damage, shot.team);

    Event ev;
    ev.type = Event::Type::Detonation;
    ev.pos = pos;
    ev.radius = shot.radius;
    ev.explodes = shot.explodes;
    ev.hitUnit = hitUnit;
    Emit(ev);
}

void World::UpdateProjectiles(float dt)
{
    for (auto& shot : m_projectiles)
    {
        shot.life -= dt;
        const XMFLOAT3 pos = m_physics.GetPosition(shot.body);
        shot.pos = pos; // the one place flight state leaves the physics world
        // Out of the arena: gone quietly, no impact and no detonation, even for
        // a live fuse. Skips the rest so a fused shot doesn't blow up out there.
        if (pos.x < -m_arenaHalf || pos.x > m_arenaHalf ||
            pos.z < -m_arenaHalf || pos.z > m_arenaHalf)
        {
            shot.life = 0.0f;
            shot.prevPos = pos;
            continue;
        }

        bool detonated = false;

        // An explosive's damage comes entirely from its blast, so a body shot
        // only ends its flight: Detonate hands out the damage from the impact
        // point, which spares the target no damage but stops a direct hit from
        // being counted twice.
        const bool blast = shot.blastRadius > 0.0f;
        const XMFLOAT3 bodyHalf = { kPlayerHalf, kPlayerHalf, kPlayerHalf };
        // Where the round was heading this tick — the direction a corpse it
        // makes gets thrown. (An explosive's shove comes from ApplyBlast
        // instead, radially out of the detonation.)
        const Vector3 travel(pos.x - shot.prevPos.x, pos.y - shot.prevPos.y,
                             pos.z - shot.prevPos.z);
        // Everyone the round could stop in, in one sweep: whoever is on the
        // other side from whoever fired it. This used to be a choice between
        // the NPC roster and the player, which was the same thing only while
        // every NPC was hostile — and a friendly standing in front of the
        // player would have been shot straight through. On one roster the
        // sweep can't help but get it right: a squadmate can take a round
        // meant for the player, and a dead player simply isn't there to hit.
        //
        // The first body along the roster wins, not the first one along the
        // segment. Two soldiers overlapping in the path of one round is rare
        // enough, and close enough, that sorting them would be arithmetic
        // nobody could see the result of.
        for (Unit& unit : m_units)
        {
            if (shot.life <= 0.0f || unit.hp <= 0.0f || unit.team == shot.team)
                continue;
            const XMFLOAT3 center = { unit.pos.x, kPlayerHalf, unit.pos.z };
            if (!SegmentHitsBox(shot.prevPos, pos, center, bodyHalf, shot.radius))
                continue;
            if (!blast)
            {
                unit.hp -= shot.damage;
                unit.knock = CorpseKnock(travel, kCorpseKnock);
                // Sprayed from where the round went in, carrying on the way it
                // was going.
                Event ev;
                ev.type = Event::Type::Hit;
                ev.unit = unit.id;
                ev.local = unit.controller == Unit::Controller::Local;
                ev.pos = pos;
                ev.dir = travel;
                ev.damage = shot.damage;
                ev.fatal = unit.hp <= 0.0f;
                Emit(ev);
            }
            shot.life = 0.0f;
            Detonate(shot, pos, true);
            detonated = true;
        }

        // Most projectiles stop where they land: first touch of world geometry
        // (walls, floor) removes them. Checked after the unit sweep so a shot
        // that clips a target on its impact tick still deals its damage.
        // A fused grenade instead rides the bounce out — Jolt has already
        // deflected it — and only a knock hard enough to hear gets an event: it
        // reports contact every tick once it settles into a roll.
        if (shot.life > 0.0f && m_physics.HadContact(shot.body))
        {
            if (shot.fused)
            {
                const Vector3 bounceTravel(pos.x - shot.prevPos.x, pos.y - shot.prevPos.y,
                                           pos.z - shot.prevPos.z);
                if (bounceTravel.Length() > kBounceSoundSpeed * dt)
                {
                    Event ev;
                    ev.type = Event::Type::Bounce;
                    ev.pos = pos;
                    Emit(ev);
                }
            }
            else
            {
                shot.life = 0.0f;
                Detonate(shot, pos, false);
                detonated = true;
            }
        }

        // Fuse ran out: for a grenade that's the whole point, so it goes off
        // wherever it has bounced to rather than being quietly collected.
        if (shot.life <= 0.0f && shot.fused && !detonated)
            Detonate(shot, pos, false);

        shot.prevPos = pos;
    }

    for (const Projectile& shot : m_projectiles)
        if (shot.life <= 0.0f)
            m_physics.RemoveBody(shot.body);
    std::erase_if(m_projectiles, [](const Projectile& s) { return s.life <= 0.0f; });
}

// The dead leave the field as a Death event — everything a corpse is built
// from — before they come off the roster. This is deliberately nobody's
// private business: rounds, blasts and the blade all just take health off,
// and whoever emptied it is swept up here at the end of the tick rather than
// by the system that did it.
void World::ReapDead()
{
    for (const Unit& unit : m_units)
        if (unit.hp <= 0.0f)
        {
            Event ev;
            ev.type = Event::Type::Death;
            ev.unit = unit.id;
            ev.local = unit.controller == Unit::Controller::Local;
            ev.pos = unit.pos;
            ev.dir = unit.aimDir;
            ev.walkPhase = unit.walkPhase;
            ev.moveBlend = unit.moveBlend;
            ev.knock = unit.knock;
            ev.team = unit.team;
            ev.cls = unit.cls;
            Emit(ev);
            // The soldier is gone, but the slot they held isn't. An AI slot is
            // owed back to its side on the respawn clock; the local player's
            // return is their phase machine's business, which is what keeps
            // one death from being paid back twice.
            if (unit.controller == Unit::Controller::Ai)
                m_reinforcements.push_back({ unit.team, kRespawnDelay });
        }
    std::erase_if(m_units, [](const Unit& u) { return u.hp <= 0.0f; });
}

void World::ApplySnapshot(const Net::Snapshot& snap, int myUnitId)
{
    // The scoreboard arrives whole even though the roster below doesn't.
    m_standingOverride.assign(snap.standing.begin(), snap.standing.end());

    // Rebuild the roster in the snapshot's image, carrying each surviving
    // unit's current state over as its previous state — the same two-frame
    // pair a Tick writes, so the renderer's between-ticks blend works
    // unchanged on a roster it has never seen ticked.
    std::vector<Unit> next;
    next.reserve(snap.units.size());
    for (const Net::SnapUnit& su : snap.units)
    {
        const Unit* old = UnitById(su.id);
        Unit unit = {};
        unit.id = su.id;
        unit.cls = &kClassDefs[su.classId % kClassCount];
        unit.controller =
            su.id == myUnitId ? Unit::Controller::Local : Unit::Controller::Remote;
        unit.team = su.team;
        unit.pos = { su.posX, 0.0f, su.posZ };
        unit.aimDir = { std::cos(su.aimYaw), 0.0f, std::sin(su.aimYaw) };
        unit.walkPhase = su.walkPhase;
        unit.moveBlend = su.moveBlend;
        unit.hp = su.hp;
        if (old)
        {
            unit.prevPos = old->pos;
            unit.prevAimDir = old->aimDir;
            unit.prevWalkPhase = old->walkPhase;
            unit.prevMoveBlend = old->moveBlend;
            // The loadout fields below ride along from last time; for anyone
            // but the own-unit they're meaningless and stay zero.
            unit.ammo = old->ammo;
            unit.reloadTimer = old->reloadTimer;
            unit.grenades = old->grenades;
            unit.meleeCharges = old->meleeCharges;
            unit.meleeRecover = old->meleeRecover;
            unit.meleeCooldown = old->meleeCooldown;
            unit.ability = old->ability;
        }
        else
        {
            unit.prevPos = unit.pos;
            unit.prevAimDir = unit.aimDir;
            unit.prevWalkPhase = unit.walkPhase;
            unit.prevMoveBlend = unit.moveBlend;
        }
        next.push_back(unit);
    }
    m_units = std::move(next);

    // The own-block: the receiving player's loadout, which is the one part
    // of a soldier the wire only tells its owner.
    if (snap.own.has)
        if (Unit* me = UnitById(snap.own.id))
        {
            // Momentum comes down with the loadout: it's the piece of
            // movement state a position alone doesn't carry, and replaying
            // pending commands from a soldier with the wrong momentum would
            // predict a different body.
            me->moveVel = { snap.own.moveVelX, 0.0f, snap.own.moveVelZ };
            me->ammo = snap.own.ammo;
            me->reloadTimer = snap.own.reloadTimer;
            me->grenades = snap.own.grenades;
            me->meleeCharges = snap.own.meleeCharges;
            me->meleeRecover = snap.own.meleeRecover;
            me->meleeCooldown = snap.own.meleeCooldown;
            me->ability.time = snap.own.abilityTime;
            me->ability.cooldown = snap.own.abilityCooldown;
        }

    // Shots are replaced wholesale: they have no ids because they need no
    // history — a replica draws them where the server says and never asks the
    // physics for a body that isn't there.
    m_projectiles.clear();
    m_projectiles.reserve(snap.projectiles.size());
    for (const Net::SnapProjectile& sp : snap.projectiles)
    {
        Projectile shot = {};
        shot.body = 0;
        shot.life = sp.life;
        shot.pos = sp.pos;
        shot.prevPos = sp.pos;
        shot.radius = sp.radius;
        shot.fused = sp.fused;
        m_projectiles.push_back(shot);
    }
}
