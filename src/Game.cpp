#include "Game.h"

#include "Hud.h"
#include "Level.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace DirectX;
using namespace DirectX::SimpleMath;

namespace
{
    constexpr float kPlayerHalf = 0.4f;
    constexpr float kMuzzleHeight = 0.6f;
    constexpr float kMuzzleOffset = 0.7f; // shots start this far ahead of the shooter
    constexpr float kGravity = 9.81f;     // must match Jolt's default gravity magnitude

    // Shorthand for the action names, which are read a dozen times in Update
    // and once each. The bindings behind them are the player's, and live in
    // Bindings.h alongside the defaults.
    using Act = Bindings::Action;

    // Grace period on the trigger after a spawn or a class pick, so the click
    // that got the player into the arena doesn't also fire their first shot.
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
    constexpr float kFogHeight = 0.02f; // just above the floor grid lines
    constexpr float kFogFar = 6.0f;     // shadow reach, in arena-half units

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

    // Bounding sphere used to skip soldiers that fall outside the viewport.
    // Generous on purpose: it has to cover the model at full stride, and
    // popping in at the screen edge is far worse than submitting a few extra
    // draws.
    constexpr float kSoldierBoundsY = 0.7f;
    constexpr float kSoldierBoundsRadius = 1.2f;

    // Corpses: a killed soldier is handed to the physics world as a ragdoll and
    // left to fall. They're decoration, so what matters is that they get out of
    // the way again — a body sinks through the floor over its last seconds
    // rather than blinking out while someone is looking at it, and the field
    // holds a fixed number so a long firefight can't unbound the body count.
    constexpr float kCorpseLife = 14.0f;
    constexpr float kCorpseSink = 1.5f;      // seconds spent sinking, at the end of that life
    constexpr float kCorpseSinkDepth = 1.6f; // deep enough to swallow a body whole
    constexpr size_t kMaxCorpses = 10;
    // Launch velocity from the killing blow: a shove along it plus some lift,
    // so a body falls away from what killed it instead of dropping in place. A
    // blast throws harder, scaled by how much of it the victim caught.
    constexpr float kCorpseKnock = 4.5f;
    constexpr float kCorpseBlastKnock = 11.0f;
    constexpr float kCorpseLift = 3.0f;
    constexpr float kCorpseSpin = 7.0f; // random angular velocity, radians/sec

    // Blood. A hit throws drops along the blow, and each one stains the floor
    // where it lands, so the mess a fight leaves points back at where the
    // shooting came from. The stains don't fade: an arena that has been fought
    // over should look like it, and a corridor of old blood is a real reading
    // of where the fighting has been. What is bounded is how many the field
    // holds at once — past the cap the oldest patch is dropped.
    //
    // The cap is set by the renderer, not by taste: the whole floor goes down
    // as one dynamic batch, and Renderer::DrawTrianglesAlpha silently drops a
    // batch bigger than 16384 vertices, so the stains have to fit inside that
    // with room to spare. At 24 vertices each that leaves well over ten deaths
    // of history — dozens for the automatics, whose individual rounds bleed
    // far less than the shot that ends someone.
    constexpr float kBloodDropsPerDamage = 0.35f;
    constexpr int kBloodMinDrops = 4;
    constexpr int kBloodMaxDrops = 22;
    constexpr float kBloodSpread = 1.1f;   // radians off the blow, either way
    constexpr float kBloodDropLife = 1.5f; // long enough that every drop lands
    constexpr int kSplatSegments = 8;      // fan segments per stain
    constexpr int kSplatVerts = kSplatSegments * 3;
    constexpr size_t kMaxSplats = 512;
    constexpr float kSplatHeight = 0.012f;    // over the grid lines, under the fog quads
    constexpr float kSplatMinRadius = 0.10f;
    constexpr float kSplatMaxRadius = 0.30f;

    constexpr float kPerfSmoothRate = 4.0f; // HUD timing smoothing, ~1/4s window

    // Below this speed a bouncing grenade's contacts are a roll, not a bounce,
    // and stay silent — otherwise a grenade resting on the floor reports a
    // contact every frame and chatters.
    constexpr float kBounceSoundSpeed = 2.5f; // units per second

    constexpr XMFLOAT4 kGridMinor = { 0.10f, 0.13f, 0.17f, 1.0f };
    constexpr XMFLOAT4 kGridMajor = { 0.17f, 0.22f, 0.29f, 1.0f };
    constexpr XMFLOAT4 kBorder = { 0.55f, 0.25f, 0.20f, 1.0f };
    constexpr XMFLOAT4 kObstacleColor = { 0.35f, 0.40f, 0.50f, 1.0f };
    constexpr XMFLOAT4 kProjectileColor = { 1.00f, 0.80f, 0.20f, 1.0f };
    // A live grenade reads as a blinking casing rather than a tracer, so it
    // can't be mistaken for a bullet while it bounces.
    constexpr XMFLOAT4 kGrenadeLiveColor = { 1.00f, 0.30f, 0.12f, 1.0f };
    constexpr XMFLOAT4 kGrenadeCasingColor = { 0.32f, 0.30f, 0.28f, 1.0f };
    constexpr float kGrenadeBlinkPeriod = 0.24f; // seconds per on/off cycle
    constexpr float kGrenadeFlashLife = 0.4f;    // fuse left when it goes solid bright
    // The aim indicator is alpha-blended; the shaft dims via alpha rather
    // than darker RGB so whatever it crosses still shows through.
    constexpr XMFLOAT4 kAimColor = { 0.95f, 0.95f, 0.40f, 0.55f };
    constexpr XMFLOAT4 kAimDimColor = { 0.95f, 0.95f, 0.40f, 0.22f };
    // Drained of color while the magazine is out: the line still tracks the
    // cursor, but nothing will come out of the barrel until the reload ends,
    // and that has to be visible without looking away from the fight.
    constexpr XMFLOAT4 kAimSpentColor = { 0.62f, 0.64f, 0.68f, 0.45f };
    constexpr XMFLOAT4 kAimSpentDimColor = { 0.62f, 0.64f, 0.68f, 0.18f };
    // The grenade's ring is orange, so it never reads as part of the primary's
    // yellow aim line.
    constexpr XMFLOAT4 kGrenadeAimColor = { 0.95f, 0.50f, 0.18f, 0.35f };
    constexpr float kAimRingHeight = 0.05f;    // above the fog quads, so it isn't dimmed
    constexpr float kGrenadeMarkRadius = 0.5f; // first-bounce marker, not the blast size
    // The swing: a pale arc swept at the blade's real reach, fading out over an
    // eighth of a second. It's drawn after the fact rather than as an aim
    // indicator on purpose — a melee is a commitment, so what the player gets to
    // see is where the blade went, not a standing promise of where it would go.
    constexpr XMFLOAT4 kMeleeArcColor = { 0.82f, 0.90f, 1.00f, 0.85f };
    constexpr float kMeleeFlashTime = 0.13f;
    // Which soldiers are on the player's side, said a second time: a thin ring
    // on the ground under everyone friendly. The armor is the real answer now
    // that a body wears its team's color, and this is the backup — a soldier
    // half-behind a crate, or one crossing at the edge of the fog, is a few
    // pixels of plate and a ring is easier to catch than a hue. It's drawn in
    // the player's own team color rather than a fixed blue so the two marks
    // never disagree about what "your side" looks like. Only the player's side
    // gets one: a soldier with nothing under them is someone to shoot.
    constexpr float kFriendlyRingAlpha = 0.45f;
    constexpr float kFriendlyRingRadius = 0.55f;
    // Wet and bright in the air, dark and matte once it has soaked into the
    // floor. The stain is translucent, so overlapping ones deepen where a fight
    // stayed in one place.
    constexpr XMFLOAT4 kBloodColor = { 0.62f, 0.06f, 0.07f, 1.0f };
    constexpr XMFLOAT4 kSplatColor = { 0.30f, 0.03f, 0.04f, 0.85f };
    constexpr XMFLOAT4 kFogColor = { 0.01f, 0.02f, 0.04f, 0.85f };
    constexpr XMFLOAT4 kHudColor = { 0.85f, 0.90f, 0.95f, 1.0f };
    constexpr XMFLOAT4 kHudHintColor = { 0.45f, 0.52f, 0.62f, 1.0f };

    // Appends a solid cube with fixed per-face shading. Units and obstacles
    // moved to the renderer's lit shapes; this remains for effects (impact
    // debris) that shouldn't respond to scene lighting.
    void AppendCube(std::vector<Vertex>& out, const XMFLOAT3& center, const XMFLOAT3& size,
                    const XMFLOAT4& color)
    {
        const float hx = size.x * 0.5f, hy = size.y * 0.5f, hz = size.z * 0.5f;
        const XMFLOAT3 c = center;
        const XMFLOAT3 p[8] = {
            { c.x - hx, c.y - hy, c.z - hz }, { c.x + hx, c.y - hy, c.z - hz },
            { c.x + hx, c.y + hy, c.z - hz }, { c.x - hx, c.y + hy, c.z - hz },
            { c.x - hx, c.y - hy, c.z + hz }, { c.x + hx, c.y - hy, c.z + hz },
            { c.x + hx, c.y + hy, c.z + hz }, { c.x - hx, c.y + hy, c.z + hz },
        };
        static constexpr int faces[6][4] = {
            { 3, 2, 6, 7 }, // top
            { 0, 4, 5, 1 }, // bottom
            { 0, 3, 7, 4 }, // -x
            { 1, 5, 6, 2 }, // +x
            { 4, 7, 6, 5 }, // +z
            { 0, 1, 2, 3 }, // -z
        };
        static constexpr float shade[6] = { 1.0f, 0.30f, 0.62f, 0.80f, 0.52f, 0.72f };

        for (int f = 0; f < 6; ++f)
        {
            const XMFLOAT4 col = { color.x * shade[f], color.y * shade[f], color.z * shade[f],
                                   color.w };
            const int* idx = faces[f];
            const int tris[6] = { idx[0], idx[1], idx[2], idx[0], idx[2], idx[3] };
            for (int i : tris)
                out.push_back({ p[i], col });
        }
    }

    // How a projectile in flight is drawn: tracers are a steady bright dot,
    // while a fused grenade blinks its casing and then holds bright for the
    // last moment before it goes off, so anyone watching can time it. Phase
    // comes from the fuse itself, so there's no clock to keep per grenade.
    XMFLOAT4 ProjectileColor(float life, bool fused)
    {
        if (!fused)
            return kProjectileColor;
        if (life <= kGrenadeFlashLife)
            return kGrenadeLiveColor;
        return std::fmod(life, kGrenadeBlinkPeriod) > kGrenadeBlinkPeriod * 0.5f
                   ? kGrenadeLiveColor
                   : kGrenadeCasingColor;
    }

    // A horizontal run of line segments on a circle, `sweep` radians wide from
    // `start`. Angles are world headings: measured from +x toward +z, the same
    // convention atan2(z, x) gives back.
    void AppendArc(std::vector<Vertex>& out, const Vector3& center, float radius, float start,
                   float sweep, int segments, const XMFLOAT4& color)
    {
        for (int i = 0; i < segments; ++i)
        {
            const float a0 = start + sweep * i / segments;
            const float a1 = start + sweep * (i + 1) / segments;
            out.push_back({ XMFLOAT3{ center.x + std::cos(a0) * radius, center.y,
                                      center.z + std::sin(a0) * radius }, color });
            out.push_back({ XMFLOAT3{ center.x + std::cos(a1) * radius, center.y,
                                      center.z + std::sin(a1) * radius }, color });
        }
    }

    // A full ring, for the aim indicator's landing marks.
    void AppendCircle(std::vector<Vertex>& out, const Vector3& center, float radius,
                      const XMFLOAT4& color)
    {
        AppendArc(out, center, radius, 0.0f, XM_2PI, 24, color);
    }

    // Draws a living soldier: the model's segments posed by the walk cycle and
    // placed at `pos` facing `aimDir`. A corpse draws the same parts, its
    // segments posed by the ragdoll instead (see Game::Render).
    void DrawSoldier(Renderer& renderer, const Vector3& pos, const Vector3& aimDir,
                     float walkPhase, float moveBlend, int team, const XMFLOAT4& classColor)
    {
        XMMATRIX local[Soldier::SegmentCount];
        Soldier::Pose(local, walkPhase, moveBlend);

        const XMMATRIX base = Soldier::Base(pos, aimDir);
        XMMATRIX world[Soldier::SegmentCount];
        for (int i = 0; i < Soldier::SegmentCount; ++i)
            world[i] = local[i] * base;

        Soldier::Draw(renderer, world, TeamColor(team), classColor);
    }

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
    // Projectile hits are swept over the frame's travel so fast shots (the
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

void Game::LoadContent(Renderer& renderer)
{
    // The player's own layout, if they've made one. A missing or unreadable
    // file isn't an error worth stopping for — it just means the defaults, which
    // is what a first run is — so nothing here checks the answer.
    m_binds.Load();

    m_sound.Init(); // loads the wave bank and starts the ambience

    const LevelData level = LevelData::Load("assets/levels/arena01.json");
    m_arenaHalf = level.arenaHalf;
    for (const LevelData::Spawn& spawn : level.spawns)
        m_teamSpawns.push_back(spawn.pos);
    m_team = std::min(m_team, static_cast<int>(m_teamSpawns.size()) - 1);
    m_playerPos = m_teamSpawns[m_team];
    m_nextNpcClass.assign(m_teamSpawns.size(), 0);

    // Static floor grid.
    const int half = static_cast<int>(m_arenaHalf);
    for (int i = -half; i <= half; ++i)
    {
        const bool border = (i == -half || i == half);
        const bool major = (i % 8) == 0;
        const XMFLOAT4 col = border ? kBorder : (major ? kGridMajor : kGridMinor);
        const float f = static_cast<float>(i);
        m_gridVerts.push_back({ XMFLOAT3{ f, 0.0f, -m_arenaHalf }, col });
        m_gridVerts.push_back({ XMFLOAT3{ f, 0.0f, m_arenaHalf }, col });
        m_gridVerts.push_back({ XMFLOAT3{ -m_arenaHalf, 0.0f, f }, col });
        m_gridVerts.push_back({ XMFLOAT3{ m_arenaHalf, 0.0f, f }, col });
    }

    // Arena floor. Projectiles are dynamic bodies under Jolt gravity; they
    // despawn on their first contact with anything solid.
    m_physics.AddStaticBox({ 0.0f, -0.5f, 0.0f },
                           { m_arenaHalf * 2.0f, 1.0f, m_arenaHalf * 2.0f });

    // Split each level object into its runtime halves: collision box and/or
    // rendered model. Each distinct model is loaded once; instances share it.
    for (const LevelData::Object& obj : level.objects)
    {
        if (obj.collider)
        {
            const XMFLOAT3 size = { obj.collider->x * obj.scale, obj.collider->y * obj.scale,
                                    obj.collider->z * obj.scale };
            const XMFLOAT3 center = { obj.pos.x, obj.pos.y + size.y * 0.5f, obj.pos.z };
            m_colliders.push_back({ center, size, obj.model.empty() });
            m_physics.AddStaticBox(center, size);

            if (center.y + size.y * 0.5f >= kEyeHeight && center.y - size.y * 0.5f <= kEyeHeight)
                m_occluders.push_back({ center.x - size.x * 0.5f, center.z - size.z * 0.5f,
                                        center.x + size.x * 0.5f, center.z + size.z * 0.5f });
        }
        if (!obj.model.empty())
        {
            auto it = m_models.find(obj.model);
            if (it == m_models.end())
                it = m_models.emplace(obj.model, renderer.LoadModel(obj.model)).first;
            m_props.push_back({ it->second.get(), obj.pos, obj.scale, obj.yaw });
        }
    }
}

void Game::Shutdown()
{
    m_sound.Shutdown();
}

void Game::Update(float dt, const Input& input, IsoCamera& camera)
{
    m_sound.Update();

    // Smoothed so the HUD is readable; raw frame times jitter far too much to
    // compare by eye. Same exponential form as the camera's follow.
    m_frameMs += (dt * 1000.0f - m_frameMs) * (1.0f - std::exp(-kPerfSmoothRate * dt));

    // Short rumble pulse while the damage timer runs (no-op without a pad).
    m_rumbleTime = std::max(0.0f, m_rumbleTime - dt);
    const float rumble = m_rumbleTime > 0.0f ? 0.6f : 0.0f;
    DirectX::GamePad::Get().SetVibration(0, rumble, rumble);

    if (m_phase == Phase::MainMenu)
    {
        if (const auto picked =
                m_mainMenu.Update(input, camera.ViewportWidth(), camera.ViewportHeight()))
        {
            switch (*picked)
            {
            case MainMenu::Choice::Deploy:   m_phase = Phase::ClassSelect; break;
            case MainMenu::Choice::KeyBinds: m_phase = Phase::KeyBinds; break;
            case MainMenu::Choice::Quit:     m_quit = true; break;
            }
        }
        return;
    }

    // The settings screen edits the bindings in place; the write to disk
    // happens here, once, on the way out. A save per keystroke would put a file
    // write in the middle of a player trying three keys to see which feels
    // right, and there's nothing to lose by waiting until they're done.
    if (m_phase == Phase::KeyBinds)
    {
        if (m_bindMenu.Update(input, m_binds, camera.ViewportWidth(), camera.ViewportHeight()))
        {
            m_binds.Save();
            m_phase = Phase::MainMenu;
        }
        return;
    }

    if (m_phase == Phase::ClassSelect)
    {
        // Escape backs out to the menu rather than closing the game, now that
        // there's a menu to back out to. It's the same key that ends the game
        // from inside the arena, one level further in — out of the screen you're
        // on, and then out of the game.
        if (input.KeyPressed(VK_ESCAPE))
        {
            m_phase = Phase::MainMenu;
            return;
        }
        if (const auto picked =
                m_classSelect.Update(input, camera.ViewportWidth(), camera.ViewportHeight()))
        {
            m_class = &GetClassDef(*picked);
            m_phase = Phase::Playing;
            m_ammo = m_class->primary.magazine;
            m_reloadTimer = 0.0f;
            m_meleeCharges = kMelee.charges;
            m_meleeRecover = 0.0f;
            m_meleeCooldown = 0.0f;
            m_ability = {};
            m_fireCooldown = kFireGrace; // so the selection click doesn't fire a shot
            // The rest of the match turns up with the player. The class pick is
            // the last thing standing between the menu and the arena, so it's
            // also the first moment there's anything for two squads to do.
            FillRosters();
            camera.SetTarget(m_playerPos);
            camera.SnapToTarget();
        }
        return;
    }

    // In the arena, escape still ends the game outright, which is what it did
    // from the window procedure before there were any screens to back out
    // through. It's the honest state of things: there's no pause menu to fall
    // into yet, and quietly making the key do nothing would be worse than the
    // bluntness of what it does.
    if (input.KeyPressed(VK_ESCAPE))
    {
        m_quit = true;
        return;
    }

    // Dead: the arena runs on without the player — NPCs keep fighting, shots
    // keep flying, the corpse keeps falling — but nothing reads input and the
    // camera holds on the spot where the body dropped until the wait is up.
    if (m_phase == Phase::Dead)
    {
        m_respawnTimer -= dt;
        UpdateNpcs(dt);
        m_physics.Step(dt);
        UpdateProjectiles(dt);
        ReapDead();
        UpdateReinforcements(dt);
        UpdateParticles(dt);
        UpdateCorpses(dt);
        if (m_respawnTimer <= 0.0f)
            Respawn(camera);
        return;
    }

    // --- Steady: held, not toggled, because it's a posture and not a mode. A
    // toggle would leave the player standing in it having forgotten, and the
    // whole cost of the thing is that it's a decision you're currently making ---
    const bool steady = m_binds.Down(input, Act::Steady) || input.pad.triggers.left > 0.5f;

    // --- Movement: WASD relative to the screen ---
    const Vector3 upG = camera.ScreenUpOnGround();
    const Vector3 rightG = camera.ScreenRightOnGround();
    m_screenRight = rightG;

    float moveUp = 0.0f, moveRight = 0.0f;
    if (m_binds.Down(input, Act::MoveForward)) moveUp += 1.0f;
    if (m_binds.Down(input, Act::MoveBack)) moveUp -= 1.0f;
    if (m_binds.Down(input, Act::MoveRight)) moveRight += 1.0f;
    if (m_binds.Down(input, Act::MoveLeft)) moveRight -= 1.0f;
    moveUp += input.pad.thumbSticks.leftY;
    moveRight += input.pad.thumbSticks.leftX;

    Vector3 move = upG * moveUp + rightG * moveRight;
    const MoveDef& gait = m_class->move;
    const float speed = gait.speed * (steady ? kSteadyMoveScale : 1.0f);
    const bool pushing = move.LengthSquared() > 1e-10f;
    if (pushing)
        move.Normalize();
    else
        move = Vector3::Zero;

    // The keys ask for a velocity; the body eases onto it. Framed as a decay
    // toward the wanted velocity rather than a step of acceleration so the
    // feel holds at any frame rate. Which rate it decays at is decided by
    // whether anything is being asked for at all, not by whether the answer
    // happens to be faster or slower: a soldier hauling themselves round onto
    // a new heading is doing the work of starting, even though their speed
    // never changed, and it should cost what starting costs.
    const float response = pushing ? gait.accel : gait.stop;
    m_moveVel += (move * speed - m_moveVel) * (1.0f - std::exp(-response * dt));
    if (m_moveVel.LengthSquared() < kMoveStopSpeed * kMoveStopSpeed)
        m_moveVel = Vector3::Zero;
    m_playerPos += m_moveVel * dt;
    ResolveObstacles(m_playerPos);

    // Stride off the speed actually travelled, not the class's, so the feet
    // keep up with the ground at either pace instead of skating over it — and
    // so the legs keep walking through the coast rather than stopping with the
    // key while the body is still sliding.
    const float travelSpeed = m_moveVel.Length();
    const bool moving = travelSpeed > 0.0f;
    m_walkPhase += travelSpeed * kStrideRate * dt;
    m_moveBlend = std::clamp(m_moveBlend + (moving ? kMoveBlendRate : -kMoveBlendRate) * dt,
                             0.0f, 1.0f);

    // The ear follows the player; orienting it to the screen's up direction
    // makes stereo panning line up with what's on screen.
    m_sound.SetListener(m_playerPos, upG);

    // --- Aim: mouse cursor projected onto the ground plane, or the right
    // stick as a screen-relative direction when deflected ---
    Vector3 aim = camera.ScreenToGround(input.mouseX, input.mouseY) - m_playerPos;
    const Vector2 stick(input.pad.thumbSticks.rightX, input.pad.thumbSticks.rightY);
    if (stick.LengthSquared() > 0.1f)
        aim = upG * stick.y + rightG * stick.x;
    aim.y = 0.0f;
    if (aim.LengthSquared() > 1e-8f)
    {
        // The stick gives a direction but no point to land on; lobbed shots
        // fall back to full range, like aiming past max range with the mouse.
        m_aimDist = stick.LengthSquared() > 0.1f ? 1e9f : aim.Length();
        aim.Normalize();
        // The cursor is where the player wants to be pointed, not where they
        // are pointed — the body has to come around to it. Everything that
        // reads the aim, including the shots and the aim ring, follows the
        // facing rather than the cursor, so what's drawn is what will fire.
        const float turnRate = kTurnRate * (steady ? kSteadyTurnScale : 1.0f);
        m_aimDir = TurnToward(m_aimDir, aim, turnRate * dt);
    }

    using PadTracker = DirectX::GamePad::ButtonStateTracker;

    // --- Reload: the magazine runs out mid-firefight, and getting a fresh one
    // in costs the player their guns for a moment. It can be started early,
    // which is the whole decision the system asks for — top up in the lull, or
    // get caught doing it. The cadence timer keeps running underneath, so a
    // reload never doubles as a way to skip one ---
    if (m_reloadTimer > 0.0f)
    {
        m_reloadTimer -= dt;
        if (m_reloadTimer <= 0.0f)
        {
            m_reloadTimer = 0.0f;
            m_ammo = m_class->primary.magazine;
            m_sound.Play("reload", 1.0f, 0.25f); // mag in, a note above mag out
        }
    }
    else if (m_binds.Pressed(input, Act::Reload) || input.padEvents.x == PadTracker::PRESSED)
    {
        BeginReload();
    }

    // Set by anything that puts a hand back on a weapon this frame. The class
    // ability below reads it for both halves of one rule: it drops a dressing
    // that was running, and it stops one from starting. Without the second
    // half, a player leaning on the trigger could press the ability key, start
    // something, and have it cancelled by their own gunfire on the same frame
    // — paying the whole cooldown for a dressing that never got a frame to
    // itself, with nothing on screen long enough to explain why.
    bool weaponUsed = false;

    // --- Firing ---
    m_fireCooldown -= dt;
    if ((m_binds.Down(input, Act::Fire) || m_binds.Pressed(input, Act::Fire) ||
         input.pad.triggers.right > 0.5f) &&
        m_fireCooldown <= 0.0f && m_reloadTimer <= 0.0f && m_ammo > 0)
    {
        SpawnShot(m_class->primary, m_playerPos, m_aimDir, m_team, m_aimDist);
        m_fireCooldown = m_class->primary.fireInterval;
        weaponUsed = true;
        // Empty: reload without being asked. Holding an empty weapon is never
        // the play, so making the player press for it would only cost them the
        // time it took to notice.
        if (--m_ammo == 0)
            BeginReload();
    }

    // --- Grenade: same for every class, lobbed onto the aim point, then left
    // to bounce until its fuse runs out (UpdateProjectiles). One per life, so
    // there's no cooldown to run down — spending it is the whole cost. It comes
    // off the belt rather than out of the magazine, so it's still throwable
    // mid-reload: caught empty, a player has one thing left to do. Thrown
    // on the key's press edge, which with a single grenade also stops a held
    // key from throwing it before the player means to ---
    if ((m_binds.Pressed(input, Act::Grenade) ||
         input.padEvents.leftShoulder == PadTracker::PRESSED) &&
        m_grenades > 0)
    {
        SpawnShot(kGrenade, m_playerPos, m_aimDir, m_team, m_aimDist);
        --m_grenades;
        weaponUsed = true;
    }

    // --- Melee: the blade, for the range the primary can't cover. Its charges
    // and its recovery are entirely off the magazine, so — like the grenade —
    // it's still in the player's hands mid-reload, which is most of why it's
    // carried. The recovery runs on its own and takes no key: there's nothing
    // to decide about it, so there's nothing to press.
    //
    // Held rather than tapped, like the trigger and unlike the grenade: the
    // three swings are a burst, paced by swingInterval, and a player who wants
    // all three wants them as fast as the arm will go. Rationing them by
    // hand-speed would only be a test of the hand. Leaning on the key through
    // the recovery does nothing — the charges come back and the swinging picks
    // straight back up, which is the same deal the trigger offers.
    //
    // The recovery is a timer since the last swing, not a reload of an empty
    // magazine, so it runs with swings still in hand and doesn't gate them:
    // what stops a swing is having none left, and nothing else ---
    m_meleeCooldown -= dt;
    m_meleeFlash = std::max(0.0f, m_meleeFlash - dt);
    if (m_meleeRecover > 0.0f)
    {
        m_meleeRecover -= dt;
        if (m_meleeRecover <= 0.0f)
        {
            m_meleeRecover = 0.0f;
            m_meleeCharges = kMelee.charges;
        }
    }
    if ((m_binds.Down(input, Act::Melee) || input.pad.IsRightShoulderPressed()) &&
        m_meleeCooldown <= 0.0f && m_meleeCharges > 0)
    {
        SwingMelee();
        weaponUsed = true;
    }

    // --- Ability: the one thing this class can do that the others can't, on a
    // press edge rather than a hold — it isn't a trigger. Everything it decides
    // it decides in Ability::Update: when it starts, what it does with the time,
    // and what taking a weapon back costs. All this end of it owns is the two
    // inputs and the answer to who's on the field ---
    const bool abilityPressed =
        m_binds.Pressed(input, Act::Ability) || input.padEvents.b == PadTracker::PRESSED;
    const Ability::Def& ability = m_class->ability;
    if (Ability::Update(ability, m_ability, AbilityScene(), dt, abilityPressed, weaponUsed) &&
        ability.startSound)
    {
        // At the listener, not in the world: it's the player's own kit, heard
        // the way the reload is.
        m_sound.Play(ability.startSound);
    }

    // --- NPCs ---
    UpdateNpcs(dt);

    // --- Projectiles (simulated by Jolt) ---
    m_physics.Step(dt);
    UpdateProjectiles(dt);
    ReapDead();
    UpdateReinforcements(dt);
    UpdateParticles(dt);
    UpdateCorpses(dt);

    if (m_playerDied)
    {
        m_playerDied = false;
        // A reload dies with the soldier: nothing ticks it down during the
        // respawn wait, so leaving it running would hold the HUD on RELOADING
        // for the whole countdown over a magazine no one is holding. The
        // blade's recovery stops for the same reason, and the swing in progress
        // goes with the arm that was making it.
        m_reloadTimer = 0.0f;
        m_meleeRecover = 0.0f;
        m_meleeFlash = 0.0f;
        // A dressing dies with the soldier too, and the ring it was drawing
        // goes with it. It doesn't pay the cooldown on the way out — the
        // respawn hands back a fresh loadout regardless — so the ability is
        // dropped rather than ended. Whoever was being treated stops being
        // treated, which is what the line going out says.
        Ability::Drop(m_ability);
        // The body stays where it fell — the player respawns out of it.
        SpawnCorpse(m_playerPos, m_aimDir, m_walkPhase, m_moveBlend, TeamColor(m_team),
                    m_class->color, m_deathKnock);
        m_phase = Phase::Dead;
        m_respawnTimer = kRespawnDelay;
        return;
    }

    camera.SetTarget(m_playerPos);
}

void Game::BeginReload()
{
    const WeaponDef& weapon = m_class->primary;
    if (m_reloadTimer > 0.0f || m_ammo >= weapon.magazine)
        return;

    // Whatever was left in the magazine goes with it: partial reloads would
    // make tapping R between every shot strictly correct, and there's nothing
    // interesting about a player who does that.
    m_ammo = 0;
    m_reloadTimer = weapon.reloadTime;
    m_sound.Play("reload");
}

void Game::SwingMelee()
{
    // The charge goes with the swing, not with the hit: the arm travels the
    // same arc either way, and a blade that only charged for connecting would
    // be free to flail with from across the room.
    --m_meleeCharges;
    m_meleeCooldown = kMelee.swingInterval;
    m_meleeFlash = kMeleeFlashTime;
    m_meleeSwingDir = m_aimDir;
    // Every swing restarts the recovery, spent or not, so what comes back is
    // always the full three and the wait is always measured from the last thing
    // the arm did. Swinging twice and stopping costs the same wait as swinging
    // three times; the third swing is free to take or leave.
    m_meleeRecover = kMelee.recoverTime;
    PlaySoundAt("swing", m_playerPos, Rand(-0.1f, 0.1f));

    // One edge, not a blast: of everyone standing in the arc only the nearest
    // is struck. It gets no sight test, unlike a bullet or a blast — anything
    // solid enough to be cover is wider than the reach and keeps the two of
    // them apart by itself, and what's left is a tree trunk they're both
    // already touching, which isn't worth stopping a swing over.
    const float cosArc = std::cos(kMelee.arc);
    Npc* target = nullptr;
    float nearest = 0.0f;
    for (Npc& npc : m_npcs)
    {
        if (npc.hp <= 0.0f || npc.team == m_team) // the blade doesn't cut its own side
            continue;
        Vector3 toward = npc.pos - m_playerPos;
        toward.y = 0.0f;
        const float dist = toward.Length();
        if (dist > kMelee.reach || dist < 1e-4f)
            continue;
        if (toward.Dot(m_aimDir) / dist < cosArc)
            continue;
        if (!target || dist < nearest)
        {
            target = &npc;
            nearest = dist;
        }
    }
    if (!target)
        return;

    // From here it's a hit like any other: the same blood, thrown the way the
    // blade was travelling, and the same knock on the corpse it may leave. The
    // body itself is collected by ReapDead at the end of the frame.
    target->hp -= kMelee.damage;
    target->knock = CorpseKnock(m_aimDir, kCorpseKnock);
    SpawnBlood({ target->pos.x, kPlayerHalf, target->pos.z }, m_aimDir, kMelee.damage,
               target->hp <= 0.0f);
    PlaySoundAt(target->hp <= 0.0f ? "death" : "hit", target->pos);
}

Ability::Scene Game::AbilityScene()
{
    // Everyone on the player's side they can actually see. The sight test is
    // the one the blast and the NPC AI use, applied here because the fog is
    // this class's business: an ability decides what happens to a soldier, not
    // whether there's a wall in the way, and doing it here is what stops
    // "treating someone through cover" from being answerable in two places.
    //
    // The list points straight into the roster, so it's good for exactly as
    // long as nothing is added to or taken out of m_npcs. Every caller uses it
    // and drops it inside the same statement, which is the only reason that's
    // safe — and the reason this returns a value rather than keeping one.
    m_abilityAllies.clear();
    const Vector2 playerXZ = { m_playerPos.x, m_playerPos.z };
    // Three of the four classes have no ability, and a sight test per squadmate
    // per frame isn't free enough to spend on nobody.
    if (m_class->ability.kind != Ability::Kind::None)
    {
        for (Npc& npc : m_npcs)
        {
            if (npc.team != m_team || npc.hp <= 0.0f)
                continue;
            if (!Visibility::IsPointVisible(playerXZ, { npc.pos.x, npc.pos.z }, m_occluders))
                continue;
            m_abilityAllies.push_back({ npc.pos, &npc.hp });
        }
    }

    return { { m_playerPos, &m_playerHp }, m_aimDir, kMaxHealth, &m_abilityAllies };
}

void Game::Respawn(IsoCamera& camera)
{
    m_playerHp = kMaxHealth;
    m_playerPos = m_teamSpawns[m_team];
    m_grenades = kGrenadesPerLife;      // respawn is a fresh loadout: the grenade back,
    m_ammo = m_class->primary.magazine; // and a full magazine, however they died holding it
    m_reloadTimer = 0.0f;
    m_meleeCharges = kMelee.charges;    // all three swings, likewise
    m_meleeRecover = 0.0f;
    m_meleeCooldown = 0.0f;
    m_ability = {};                     // and the ability off its cooldown, however it was left
    m_fireCooldown = kFireGrace;        // brief grace, as after the class pick
    m_moveBlend = 0.0f;                 // stands still on arrival instead of resuming mid-stride
    m_moveVel = Vector3::Zero;          // and without the momentum they died carrying
    m_phase = Phase::Playing;
    // A cut, not a sweep: the spawn is somewhere else entirely, and panning
    // the whole arena to get there would take longer than the wait did.
    camera.SetTarget(m_playerPos);
    camera.SnapToTarget();
}

bool Game::PlayerOnField() const
{
    // The health test also covers the frame the player dies on: the phase only
    // flips at the end of Update, so until then hp is what says they're down.
    return m_phase == Phase::Playing && m_playerHp > 0.0f;
}

void Game::SpawnShot(const WeaponDef& weapon, const Vector3& from, const Vector3& dir, int team,
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
                              weapon.projectileLife, pos, team, weapon.damage,
                              weapon.projectileRadius, weapon.blastRadius,
                              weapon.bounce > 0.0f, weapon.explodes });

    // One shared fire sample; heavier weapons play deeper. A little random
    // detune keeps rapid fire from sounding like a loop.
    const float pitch = 0.25f - weapon.damage / 120.0f + Rand(-0.05f, 0.05f);
    PlaySoundAt("fire", from, pitch);
}

float Game::PredictShotStop(const WeaponDef& weapon, const Vector3& from, const Vector3& dir,
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

void Game::PlaySoundAt(const std::string& name, const Vector3& pos, float pitch)
{
    // Past kRange the voice would be silent anyway; skip spawning it.
    if (Vector2(pos.x - m_playerPos.x, pos.z - m_playerPos.z).LengthSquared() <
        Sound::kRange * Sound::kRange)
        m_sound.Play3D(name, pos, pitch);
}

float Game::Rand(float lo, float hi)
{
    return std::uniform_real_distribution<float>(lo, hi)(m_rng);
}

void Game::ResolveObstacles(Vector3& pos) const
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

void Game::SpawnNpc(int team)
{
    // An NPC appears at its own team's spawn point, scattered a little so a
    // squad arriving at once doesn't stack on one tile. A squadmate turns up
    // where the player turns up and a hostile across the arena, because the
    // spawn a soldier comes from is a fact about the side they're on.
    Npc npc;
    npc.cls = &kClassDefs[m_nextNpcClass[team]];
    m_nextNpcClass[team] = (m_nextNpcClass[team] + 1) % static_cast<int>(kClassCount);
    npc.team = team;
    npc.pos = m_teamSpawns[team];
    npc.pos.x += Rand(-2.0f, 2.0f);
    npc.pos.z += Rand(-2.0f, 2.0f);
    ResolveObstacles(npc.pos);
    npc.aimDir = { -1.0f, 0.0f, 0.0f };
    npc.hp = kMaxHealth;
    npc.fireCooldown = 0.5f; // brief grace so spawns don't instantly fire
    npc.ammo = npc.cls->primary.magazine;
    npc.reloadTimer = 0.0f;
    Brain::Wake(npc.mind, npc.pos, m_rng);
    npc.walkPhase = Rand(0.0f, XM_2PI); // desync strides across the squad
    npc.moveBlend = 0.0f;
    m_npcs.push_back(npc);
}

int Game::NpcCount(int team) const
{
    return static_cast<int>(std::count_if(m_npcs.begin(), m_npcs.end(), [team](const Npc& n) {
        return n.team == team && n.hp > 0.0f;
    }));
}

void Game::FillRosters()
{
    for (int team = 0; team < static_cast<int>(m_teamSpawns.size()); ++team)
        for (int i = NpcCount(team); i < NpcQuota(team); ++i)
            SpawnNpc(team);
}

void Game::UpdateReinforcements(float dt)
{
    // Two passes and a sweep, the way the dead are collected: the spawn has to
    // happen between deciding a slot is due and forgetting about it, and doing
    // all three in one loop would mean adding to one list while erasing from
    // another.
    for (Reinforcement& slot : m_reinforcements)
        slot.timer -= dt;

    for (const Reinforcement& slot : m_reinforcements)
    {
        // Counted fresh each time, so two slots coming due on the same frame
        // both get filled. The quota is checked rather than assumed because the
        // roster is what says how many a side fields — the queue only says when
        // — and a wait that outlived its slot should be dropped, not honored.
        if (slot.timer <= 0.0f && NpcCount(slot.team) < NpcQuota(slot.team))
            SpawnNpc(slot.team);
    }

    std::erase_if(m_reinforcements, [](const Reinforcement& slot) { return slot.timer <= 0.0f; });
}

void Game::UpdateNpcs(float dt)
{
    for (Npc& npc : m_npcs)
    {
        npc.fireCooldown -= dt;

        // NPCs reload on the same terms the player does, so the lull after a
        // squad empties its magazines is a real opening rather than something
        // only one side has to live with. They reload wherever they are —
        // breaking off to do it under cover is a decision the AI isn't smart
        // enough to make yet.
        if (npc.reloadTimer > 0.0f)
        {
            npc.reloadTimer -= dt;
            if (npc.reloadTimer <= 0.0f)
            {
                npc.reloadTimer = 0.0f;
                npc.ammo = npc.cls->primary.magazine;
                PlaySoundAt("reload", npc.pos, 0.25f);
            }
        }

        // Who this soldier can see. Everyone hostile, at whatever range, with
        // the same sight test the player's fog of war uses so nobody shoots
        // through a wall the player can't see through either — and no further
        // opinion than that. How close is close enough to fight is a matter of
        // temperament, so it belongs to the brain; how far a soldier can pick
        // anyone out at all is a fact about the world, so it's here.
        //
        // It runs a sight test per candidate, which makes this quadratic in the
        // number of soldiers on the field; at two squads of five that's a
        // hundred segment tests a frame, which is cheaper than the bookkeeping
        // to avoid it. The number to watch it against is kTeamSize, and it will
        // want revisiting long before this arena holds a proper Infantry zone.
        m_contacts.clear();
        const Vector2 npcXZ = { npc.pos.x, npc.pos.z };
        const auto sight = [&](const Vector3& pos) {
            const float d = Vector2(pos.x - npc.pos.x, pos.z - npc.pos.z).Length();
            if (d < 1e-3f || d > kNpcSightRange)
                return;
            if (Visibility::IsPointVisible(npcXZ, { pos.x, pos.z }, m_occluders))
                m_contacts.push_back({ pos, d });
        };
        if (PlayerOnField() && npc.team != m_team)
            sight(m_playerPos);
        for (const Npc& other : m_npcs)
            if (&other != &npc && other.hp > 0.0f && other.team != npc.team)
                sight(other.pos);

        const WeaponDef& weapon = npc.cls->primary;
        const bool canFire = npc.fireCooldown <= 0.0f && npc.reloadTimer <= 0.0f && npc.ammo > 0;
        const Brain::Senses senses = { npc.pos, npc.aimDir, &m_contacts, m_arenaHalf, canFire };
        const Brain::Intent intent =
            Brain::Think(npc.cls->brain, npc.mind, senses, dt, m_rng);

        // From here it's the body: everything a soldier does the same way
        // whichever mind is driving it.
        if (intent.facing.LengthSquared() > 1e-6f)
            npc.aimDir = intent.facing;

        if (intent.fire && canFire)
        {
            // A touch of angular spread keeps NPCs beatable up close and makes
            // long-range sniper duels survivable. It stays out here rather than
            // being a brain's to set: it's a fairness knob on the whole AI, not
            // a personality — a brain that could tighten its own aim would be a
            // brain that could decide how hard it is to play against.
            const Vector3 dir = Vector3::Transform(
                npc.aimDir, Matrix::CreateRotationY(Rand(-kNpcAimJitter, kNpcAimJitter)));
            // NPCs "aim" at what they're shooting at rather than at max range,
            // so a grenadier's lob comes down on them.
            SpawnShot(weapon, npc.pos, dir, npc.team, intent.fireDist);
            npc.fireCooldown = weapon.fireInterval;
            if (--npc.ammo == 0)
            {
                npc.reloadTimer = weapon.reloadTime;
                PlaySoundAt("reload", npc.pos);
            }
        }

        // NPCs still move on the speed alone — the momentum rates are the
        // player's, and giving the AI weight is a change to how it steers
        // rather than one more field to read.
        const float speed = npc.cls->move.speed * intent.speedScale;
        const bool moving = intent.move.LengthSquared() > 1e-6f;
        if (moving)
            npc.walkPhase += speed * kStrideRate * dt;
        npc.moveBlend = std::clamp(npc.moveBlend + (moving ? kMoveBlendRate : -kMoveBlendRate) * dt,
                                   0.0f, 1.0f);

        npc.pos += intent.move * (speed * dt);
        ResolveObstacles(npc.pos);
    }

    // Pairwise separation so a group never collapses into a single column.
    for (size_t i = 0; i < m_npcs.size(); ++i)
        for (size_t j = i + 1; j < m_npcs.size(); ++j)
        {
            const Vector3 between = m_npcs[j].pos - m_npcs[i].pos;
            const float len = between.Length();
            const float minDist = kPlayerHalf * 2.0f;
            if (len >= minDist || len < 1e-5f)
                continue;
            const Vector3 push = between * ((minDist - len) * 0.5f / len);
            m_npcs[i].pos -= push;
            m_npcs[j].pos += push;
        }
}

void Game::SpawnImpactBurst(const Vector3& pos, float scale)
{
    constexpr int kBurstCount = 12;
    constexpr XMFLOAT4 kDustColor = { 0.45f, 0.48f, 0.55f, 1.0f };
    for (int i = 0; i < kBurstCount; ++i)
    {
        const float yaw = Rand(0.0f, XM_2PI);
        const float speed = Rand(1.5f, 4.5f);
        Particle p;
        p.pos = pos;
        p.vel = { std::cos(yaw) * speed, Rand(1.0f, 4.5f), std::sin(yaw) * speed };
        p.maxLife = p.life = Rand(0.2f, 0.45f);
        p.size = scale * Rand(0.35f, 0.7f);
        // Roughly half glowing sparks in the tracer color, half neutral dust.
        p.color = (i % 2 == 0) ? kProjectileColor : kDustColor;
        m_particles.push_back(p);
    }
}

void Game::SpawnExplosion(const Vector3& pos)
{
    // Core flash: one big, very short-lived bright cube the bloom pass turns
    // into a glow. Zero velocity — it just pops and shrinks away.
    m_particles.push_back({ pos, Vector3::Zero, 0.09f, 0.09f, 1.4f, { 1.0f, 0.95f, 0.6f, 1.0f } });

    constexpr XMFLOAT4 kFireColor = { 1.0f, 0.55f, 0.15f, 1.0f };
    constexpr XMFLOAT4 kSmokeColor = { 0.30f, 0.30f, 0.33f, 1.0f };
    constexpr int kExplosionCount = 26;
    for (int i = 0; i < kExplosionCount; ++i)
    {
        const float yaw = Rand(0.0f, XM_2PI);
        const float speed = Rand(2.0f, 8.0f);
        Particle p;
        p.pos = pos;
        p.vel = { std::cos(yaw) * speed, Rand(2.0f, 7.0f), std::sin(yaw) * speed };
        p.maxLife = p.life = Rand(0.35f, 0.8f);
        p.size = Rand(0.12f, 0.3f);
        // A mix of embers (tracer yellow), fire, and lingering smoke.
        p.color = i % 3 == 0 ? kSmokeColor : (i % 3 == 1 ? kFireColor : kProjectileColor);
        m_particles.push_back(p);
    }
}

void Game::SpawnBlood(const Vector3& pos, const Vector3& dir, float damage, bool fatal)
{
    // The spray goes the way the blow was travelling — a round that goes
    // through a soldier takes the blood out the far side — spread wide enough
    // that it lands as a scatter rather than a line. How much comes off scales
    // with the damage, and a killing blow lets go of everything at once.
    Vector3 flat(dir.x, 0.0f, dir.z);
    if (flat.LengthSquared() > 1e-8f)
        flat.Normalize();
    else
        flat = Vector3::UnitX; // straight-down blow (a blast overhead): any way will do
    const float heading = std::atan2(flat.z, flat.x);

    int count = std::clamp(static_cast<int>(damage * kBloodDropsPerDamage), kBloodMinDrops,
                           kBloodMaxDrops);
    if (fatal)
        count *= 2;

    for (int i = 0; i < count; ++i)
    {
        const float yaw = heading + Rand(-kBloodSpread, kBloodSpread);
        const float speed = Rand(1.5f, 5.5f);
        Particle p;
        p.pos = pos;
        p.vel = { std::cos(yaw) * speed, Rand(1.5f, 4.5f), std::sin(yaw) * speed };
        p.maxLife = p.life = kBloodDropLife;
        p.size = Rand(0.05f, 0.11f);
        p.color = kBloodColor;
        p.blood = true;
        m_particles.push_back(p);
    }
}

void Game::SpawnSplat(const Vector3& pos)
{
    // Drops that carry over the arena wall have nothing to land on.
    if (std::abs(pos.x) > m_arenaHalf || std::abs(pos.z) > m_arenaHalf)
        return;

    if (m_splatVerts.size() >= kMaxSplats * kSplatVerts)
        m_splatVerts.erase(m_splatVerts.begin(), m_splatVerts.begin() + kSplatVerts);

    // A ragged fan around the landing point: the rim wanders in and out per
    // segment and the whole patch starts at a random angle, so no two stains
    // read as the same circle stamped twice.
    const XMFLOAT3 center = { pos.x, kSplatHeight, pos.z };
    const float radius = Rand(kSplatMinRadius, kSplatMaxRadius);
    float rim[kSplatSegments];
    for (float& r : rim)
        r = radius * Rand(0.5f, 1.0f);

    const float spin = Rand(0.0f, XM_2PI);
    for (int i = 0; i < kSplatSegments; ++i)
    {
        const float a0 = spin + XM_2PI * i / kSplatSegments;
        const float a1 = spin + XM_2PI * (i + 1) / kSplatSegments;
        const float r0 = rim[i];
        const float r1 = rim[(i + 1) % kSplatSegments];
        m_splatVerts.push_back({ center, kSplatColor });
        m_splatVerts.push_back({ XMFLOAT3{ center.x + std::cos(a0) * r0, kSplatHeight,
                                           center.z + std::sin(a0) * r0 }, kSplatColor });
        m_splatVerts.push_back({ XMFLOAT3{ center.x + std::cos(a1) * r1, kSplatHeight,
                                           center.z + std::sin(a1) * r1 }, kSplatColor });
    }
}

void Game::ApplyBlast(const Vector3& center, float radius, float damage, int team)
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
    // it, which since the roster stopped being uniformly hostile can mean the
    // player and their squadmates from the same grenade.
    for (Npc& npc : m_npcs)
    {
        if (npc.hp <= 0.0f || npc.team == team) // already killed this frame, or on the thrower's side
            continue;
        const float dmg = splash({ npc.pos.x, kPlayerHalf, npc.pos.z });
        if (dmg <= 0.0f)
            continue;
        npc.hp -= dmg;
        // Blown outward from the blast, as hard as the share of it they
        // caught: a body at the rim topples, one on top of it is thrown.
        npc.knock = CorpseKnock(npc.pos - center, kCorpseBlastKnock * (dmg / damage));
        // Blood goes the same way the blast threw them, out of the middle.
        SpawnBlood({ npc.pos.x, kPlayerHalf, npc.pos.z }, npc.pos - center, dmg, npc.hp <= 0.0f);
        PlaySoundAt(npc.hp <= 0.0f ? "death" : "hit", npc.pos);
    }

    if (team == m_team || !PlayerOnField())
        return; // the thrower's own side, or already down: nothing left to catch it

    const float dmg = splash({ m_playerPos.x, kPlayerHalf, m_playerPos.z });
    if (dmg > 0.0f)
    {
        m_playerHp -= dmg;
        m_deathKnock = CorpseKnock(m_playerPos - center, kCorpseBlastKnock * (dmg / damage));
        SpawnBlood({ m_playerPos.x, kPlayerHalf, m_playerPos.z }, m_playerPos - center, dmg,
                   m_playerHp <= 0.0f);
        m_rumbleTime = 0.25f;
        if (m_playerHp <= 0.0f)
            m_playerDied = true;
        m_sound.Play(m_playerDied ? "death" : "hit");
    }
}

void Game::Detonate(const Projectile& shot, const Vector3& pos, bool hitUnit)
{
    if (shot.blastRadius > 0.0f)
        ApplyBlast(pos, shot.blastRadius, shot.damage, shot.team);

    if (shot.explodes)
    {
        SpawnExplosion(pos);
        PlaySoundAt("explode", pos, Rand(-0.06f, 0.06f));
    }
    else
    {
        SpawnImpactBurst(pos, shot.radius);
        // Unit hits already play their own hit/death sound; the thud is only
        // for shots stopping in the ground or a wall.
        if (!hitUnit)
            PlaySoundAt("thud", pos, Rand(-0.15f, 0.15f));
    }
}

void Game::UpdateParticles(float dt)
{
    for (Particle& p : m_particles)
    {
        p.life -= dt;
        p.vel.y -= 18.0f * dt;
        p.pos += p.vel * dt;
        // Settle on the floor instead of sinking through it.
        const float half = p.size * 0.5f;
        if (p.pos.y < half)
        {
            // A drop of blood is spent the moment it touches down: what's
            // left of it is the stain, and that stays.
            if (p.blood)
            {
                SpawnSplat(p.pos);
                p.life = 0.0f;
                continue;
            }
            p.pos.y = half;
            p.vel = { p.vel.x * 0.6f, 0.0f, p.vel.z * 0.6f };
        }
    }
    std::erase_if(m_particles, [](const Particle& p) { return p.life <= 0.0f; });
}

void Game::UpdateProjectiles(float dt)
{
    for (auto& shot : m_projectiles)
    {
        shot.life -= dt;
        const XMFLOAT3 pos = m_physics.GetPosition(shot.body);
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
        // only ends its flight: Detonate hands out the damage (and the
        // hit/death sound) from the impact point, which spares the target no
        // damage but stops a direct hit from being counted twice.
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
        // player would have been shot straight through. Now they aren't, which
        // means a squadmate can take a round meant for the player, and that is
        // the correct outcome rather than a side effect to design around.
        //
        // NPCs are swept before the player and the first body along the roster
        // wins, not the first one along the segment. Two soldiers overlapping
        // in the path of one round is rare enough, and close enough, that
        // sorting them would be arithmetic nobody could see the result of.
        for (Npc& npc : m_npcs)
        {
            if (shot.life <= 0.0f || npc.hp <= 0.0f || npc.team == shot.team)
                continue;
            const XMFLOAT3 center = { npc.pos.x, kPlayerHalf, npc.pos.z };
            if (!SegmentHitsBox(shot.prevPos, pos, center, bodyHalf, shot.radius))
                continue;
            if (!blast)
            {
                npc.hp -= shot.damage;
                npc.knock = CorpseKnock(travel, kCorpseKnock);
                // Sprayed from where the round went in, carrying on the way it
                // was going.
                SpawnBlood(pos, travel, shot.damage, npc.hp <= 0.0f);
                PlaySoundAt(npc.hp <= 0.0f ? "death" : "hit", npc.pos);
            }
            shot.life = 0.0f;
            Detonate(shot, pos, true);
            detonated = true;
        }
        if (shot.life > 0.0f && shot.team != m_team && PlayerOnField())
        {
            const XMFLOAT3 center = { m_playerPos.x, kPlayerHalf, m_playerPos.z };
            if (SegmentHitsBox(shot.prevPos, pos, center, bodyHalf, shot.radius))
            {
                if (!blast)
                {
                    m_playerHp -= shot.damage;
                    m_deathKnock = CorpseKnock(travel, kCorpseKnock);
                    SpawnBlood(pos, travel, shot.damage, m_playerHp <= 0.0f);
                    m_rumbleTime = 0.25f;
                    if (m_playerHp <= 0.0f)
                        m_playerDied = true;
                    m_sound.Play(m_playerDied ? "death" : "hit");
                }
                shot.life = 0.0f;
                Detonate(shot, pos, true);
                detonated = true;
            }
        }

        // Most projectiles stop where they land: first touch of world geometry
        // (walls, floor) removes them. Checked after the unit sweeps so a shot
        // that clips a target on its impact tick still deals its damage.
        // A fused grenade instead rides the bounce out — Jolt has already
        // deflected it — and only a knock hard enough to hear gets a sound: it
        // reports contact every frame once it settles into a roll.
        if (shot.life > 0.0f && m_physics.HadContact(shot.body))
        {
            if (shot.fused)
            {
                const Vector3 travel(pos.x - shot.prevPos.x, pos.y - shot.prevPos.y,
                                     pos.z - shot.prevPos.z);
                if (travel.Length() > kBounceSoundSpeed * dt)
                    PlaySoundAt("thud", pos, Rand(0.35f, 0.55f));
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

// The dead leave a ragdoll standing exactly where they fell before they come
// off the roster. This is deliberately nobody's private business: rounds,
// blasts and the blade all just take health off, and whoever emptied it is
// swept up here at the end of the frame rather than by the system that did it.
void Game::ReapDead()
{
    for (const Npc& npc : m_npcs)
        if (npc.hp <= 0.0f)
        {
            SpawnCorpse(npc.pos, npc.aimDir, npc.walkPhase, npc.moveBlend, TeamColor(npc.team),
                        npc.cls->color, npc.knock);
            // The soldier is gone, but the slot they held isn't: their side is
            // owed a body, and it comes back on the same clock the player's
            // does. Whether it's actually sent is settled when the wait is up
            // rather than here — see UpdateReinforcements.
            m_reinforcements.push_back({ npc.team, kRespawnDelay });
        }
    std::erase_if(m_npcs, [](const Npc& n) { return n.hp <= 0.0f; });
}

void Game::SpawnCorpse(const Vector3& pos, const Vector3& aimDir, float walkPhase, float moveBlend,
                       const XMFLOAT4& teamColor, const XMFLOAT4& classColor, const Vector3& knock)
{
    if (m_corpses.size() >= kMaxCorpses)
        RemoveCorpse(0);

    // The ragdoll is built in the pose the soldier was drawn in on its last
    // frame, so death is a body carrying on from the stride it was in rather
    // than a model snapping to a T-pose and dropping.
    XMMATRIX local[Soldier::SegmentCount];
    Soldier::Pose(local, walkPhase, moveBlend);
    const XMMATRIX base = Soldier::Base(pos, aimDir);

    Corpse corpse;
    corpse.teamColor = teamColor;
    corpse.classColor = classColor;
    corpse.life = kCorpseLife;

    XMMATRIX world[Soldier::SegmentCount];
    for (int i = 0; i < Soldier::SegmentCount; ++i)
    {
        world[i] = local[i] * base;
        XMFLOAT3 center;
        XMFLOAT4 rot;
        XMStoreFloat3(&center, world[i].r[3]);
        XMStoreFloat4(&rot, XMQuaternionRotationMatrix(world[i]));

        // Every segment leaves with the same shove, and its own random tumble
        // on top: a corpse that only translated would fall like a statue.
        const XMFLOAT3 spin = { Rand(-kCorpseSpin, kCorpseSpin), Rand(-kCorpseSpin, kCorpseSpin),
                                Rand(-kCorpseSpin, kCorpseSpin) };
        corpse.parts[i] = m_physics.SpawnDebrisBox(center, Soldier::kBodies[i].size, rot, knock,
                                                   spin, Soldier::kBodies[i].mass);
    }

    // Joints are anchored in the parent segment's frame and centered on the
    // child's bone, both taken from the pose the bodies were just built in.
    for (const Soldier::Joint& joint : Soldier::kJoints)
    {
        XMFLOAT3 anchor, boneAxis;
        XMStoreFloat3(&anchor,
                      XMVector3Transform(XMLoadFloat3(&joint.anchor), world[joint.parent]));
        XMStoreFloat3(&boneAxis, XMVector3Normalize(world[joint.child].r[1]));
        m_physics.AddConeJoint(corpse.parts[joint.parent], corpse.parts[joint.child], anchor,
                               boneAxis, joint.coneAngle, joint.twistAngle);
    }

    m_corpses.push_back(corpse);
}

void Game::UpdateCorpses(float dt)
{
    for (size_t i = m_corpses.size(); i-- > 0;)
    {
        m_corpses[i].life -= dt;
        if (m_corpses[i].life <= 0.0f)
            RemoveCorpse(i);
    }
}

void Game::RemoveCorpse(size_t index)
{
    // Removing a part takes its joints with it, so the ragdoll comes apart in
    // whatever order the segments happen to be in.
    for (Physics::BodyHandle part : m_corpses[index].parts)
        m_physics.RemoveBody(part);
    m_corpses.erase(m_corpses.begin() + index);
}

// Builds the fog overlay: the visibility polygon splits the world into
// angular wedges around the player, and for each boundary run between
// consecutive polygon points the far side gets a dark quad (boundary edge
// extruded radially outward). Wedges partition the plane by angle, so the
// semi-transparent quads never overlap and nothing double-darkens.
void Game::AppendFog(std::vector<Vertex>& out) const
{
    const Vector2 viewer = { m_playerPos.x, m_playerPos.z };
    const std::vector<XMFLOAT2> poly =
        Visibility::ComputePolygon(viewer, m_occluders, m_arenaHalf);
    if (poly.size() < 2)
        return;

    const float farDist = m_arenaHalf * kFogFar;
    const auto extrude = [&](const Vector2& v) -> Vector2 {
        const Vector2 d = v - viewer;
        const float len = d.Length();
        if (len < 1e-5f)
            return v;
        return viewer + d * (farDist / len);
    };

    for (size_t i = 0; i < poly.size(); ++i)
    {
        const Vector2 a = poly[i];
        const Vector2 b = poly[(i + 1) % poly.size()];
        const Vector2 af = extrude(a);
        const Vector2 bf = extrude(b);
        const Vertex quad[4] = {
            { XMFLOAT3{ a.x, kFogHeight, a.y }, kFogColor },
            { XMFLOAT3{ b.x, kFogHeight, b.y }, kFogColor },
            { XMFLOAT3{ bf.x, kFogHeight, bf.y }, kFogColor },
            { XMFLOAT3{ af.x, kFogHeight, af.y }, kFogColor },
        };
        out.push_back(quad[0]);
        out.push_back(quad[1]);
        out.push_back(quad[2]);
        out.push_back(quad[0]);
        out.push_back(quad[2]);
        out.push_back(quad[3]);
    }
}

void Game::Render(Renderer& renderer)
{
    // Grayscale for as long as the player is dead, not a flash on the way
    // down: the drained color is what separates watching the arena from
    // playing in it, so it holds until the moment they're back in control.
    renderer.SetMonochrome(m_phase == Phase::Dead);

    if (m_phase == Phase::MainMenu)
    {
        m_mainMenu.Render(renderer);
        return;
    }

    if (m_phase == Phase::KeyBinds)
    {
        m_bindMenu.Render(renderer, m_binds);
        return;
    }

    if (m_phase == Phase::ClassSelect)
    {
        m_classSelect.Render(renderer, m_binds);
        return;
    }

    const XMMATRIX identity = XMMatrixIdentity();

    renderer.DrawLines(m_gridVerts.data(), static_cast<uint32_t>(m_gridVerts.size()), identity);

    for (const Collider& c : m_colliders)
        if (c.debugDraw)
            renderer.DrawShape(Shape::Box,
                               XMMatrixScaling(c.size.x, c.size.y, c.size.z) *
                                   XMMatrixTranslation(c.center.x, c.center.y, c.center.z),
                               kObstacleColor);

    // Nothing of the player is drawn while they're dead — what's standing at
    // m_playerPos is their corpse, and the view stays on it.
    if (m_phase == Phase::Playing)
        DrawSoldier(renderer, m_playerPos, m_aimDir, m_walkPhase, m_moveBlend, m_team,
                    m_class->color);

    // NPCs and projectiles the player can't see stay hidden — an enemy behind
    // a wall disappears until it re-emerges. The player's own soldier needs no
    // viewport test: the camera follows them, so they are always on screen.
    m_scratch.clear();
    const XMFLOAT2 eye = { m_playerPos.x, m_playerPos.z };
    for (const Npc& npc : m_npcs)
    {
        // Cheapest rejection first: the arena is far wider than the view, so
        // most of a large squad is usually off screen entirely.
        if (!renderer.IsSphereVisible({ npc.pos.x, kSoldierBoundsY, npc.pos.z },
                                      kSoldierBoundsRadius))
            continue;
        if (!Visibility::IsPointVisible(eye, { npc.pos.x, npc.pos.z }, m_occluders))
            continue;
        DrawSoldier(renderer, npc.pos, npc.aimDir, npc.walkPhase, npc.moveBlend, npc.team,
                    npc.cls->color);
    }
    // Corpses, drawn from their ragdolls: the same model as a living soldier,
    // with every segment placed by the physics body it was built from. Same
    // culling as the living, tested at the pelvis — where a body ends up is the
    // ragdoll's business, so there's no single position to key off otherwise.
    for (const Corpse& corpse : m_corpses)
    {
        const Physics::Transform pelvis = m_physics.GetTransform(corpse.parts[Soldier::Pelvis]);
        if (!renderer.IsSphereVisible(pelvis.pos, kSoldierBoundsRadius))
            continue;
        if (!Visibility::IsPointVisible(eye, { pelvis.pos.x, pelvis.pos.z }, m_occluders))
            continue;

        // Sinking is a drawing trick, not a physical one: the bodies stay put
        // (asleep, by then) and the model is drawn further under the floor each
        // frame until it's gone.
        const float sink =
            std::max(0.0f, kCorpseSink - corpse.life) / kCorpseSink * kCorpseSinkDepth;

        XMMATRIX world[Soldier::SegmentCount];
        for (int i = 0; i < Soldier::SegmentCount; ++i)
        {
            const Physics::Transform t = m_physics.GetTransform(corpse.parts[i]);
            world[i] = XMMatrixRotationQuaternion(XMLoadFloat4(&t.rot)) *
                       XMMatrixTranslation(t.pos.x, t.pos.y - sink, t.pos.z);
        }
        Soldier::Draw(renderer, world, corpse.teamColor, corpse.classColor);
    }

    for (const Projectile& shot : m_projectiles)
    {
        const XMFLOAT3 pos = m_physics.GetPosition(shot.body);
        if (Visibility::IsPointVisible(eye, { pos.x, pos.z }, m_occluders))
        {
            // Per-shot radius, not the class's: a thrown grenade is a fatter
            // projectile than most primaries fire.
            const float d = shot.radius * 2.0f;
            renderer.DrawShape(Shape::Sphere,
                               XMMatrixScaling(d, d, d) *
                                   XMMatrixTranslation(pos.x, pos.y, pos.z),
                               ProjectileColor(shot.life, shot.fused));
        }
    }

    // Impact debris shrinks out over its lifetime. Same visibility rule as
    // projectiles: a burst behind a wall stays hidden.
    for (const Particle& p : m_particles)
    {
        if (!Visibility::IsPointVisible(eye, { p.pos.x, p.pos.z }, m_occluders))
            continue;
        const float s = p.size * (p.life / p.maxLife);
        AppendCube(m_scratch, p.pos, { s, s, s }, p.color);
    }

    renderer.DrawTriangles(m_scratch.data(), static_cast<uint32_t>(m_scratch.size()), identity);

    for (const Prop& prop : m_props)
    {
        const XMMATRIX world = XMMatrixScaling(prop.scale, prop.scale, prop.scale) *
                               XMMatrixRotationY(prop.yaw) *
                               XMMatrixTranslation(prop.pos.x, prop.pos.y, prop.pos.z);
        renderer.DrawModel(*prop.model, world);
    }

    // Blood, laid flat on the floor. It goes down after every opaque thing has
    // written depth, so a stain behind a wall is hidden by the wall rather than
    // painted over it, and before the fog, so blood out of sight is dimmed
    // along with the ground it's on.
    renderer.DrawTrianglesAlpha(m_splatVerts.data(), static_cast<uint32_t>(m_splatVerts.size()),
                                identity);

    // Fog of war goes on after all opaque geometry so it blends over the
    // floor while walls (which wrote depth) still punch through it.
    m_fogVerts.clear();
    AppendFog(m_fogVerts);
    renderer.DrawTrianglesAlpha(m_fogVerts.data(), static_cast<uint32_t>(m_fogVerts.size()),
                                identity);

    // What the player's ability lets them see that nobody else does — for the
    // medic, squadmate health over the heads of the wounded. Which soldiers are
    // in that list is settled by AbilityScene, so a mark can never be the thing
    // that tells a medic where somebody is standing: it draws over the ones
    // they can already see and nobody else. What it draws is the ability's own
    // business, and no kind of it is named here.
    if (m_phase == Phase::Playing)
    {
        m_scratch.clear();
        Ability::AppendVision(m_class->ability, AbilityScene(), m_screenRight, m_scratch);
        renderer.DrawTrianglesAlpha(m_scratch.data(), static_cast<uint32_t>(m_scratch.size()),
                                    identity);
    }

    // Aim indicator: the shot's actual trajectory as a dim 3D polyline — an
    // arch for the grenade's lob, a near-level line with droop for bullets —
    // ending where the shot really stops (max range, the first wall the arc
    // can't clear, or the ground). A bright tick crosses the end point, lobbed
    // weapons get a landing circle, and the grenade gets a marker where the
    // throw touches down. Drawn after the fog pass so it stays bright. There's
    // nothing to aim while dead, so it goes with the soldier. The whole
    // indicator greys out through a reload — where the shot would land is still
    // worth showing, but not as something the player can act on yet.
    if (m_phase == Phase::Playing)
    {
        const bool reloading = m_reloadTimer > 0.0f;
        const XMFLOAT4 aimColor = reloading ? kAimSpentColor : kAimColor;
        const XMFLOAT4 aimDim = reloading ? kAimSpentDimColor : kAimDimColor;

        PredictShotStop(m_class->primary, m_playerPos, m_aimDir, m_aimDist, &m_aimArc);

        m_scratch.clear();
        for (size_t i = 1; i < m_aimArc.size(); ++i)
        {
            m_scratch.push_back({ m_aimArc[i - 1], aimDim });
            m_scratch.push_back({ m_aimArc[i], aimDim });
        }

        // End-of-flight tick, perpendicular to the aim direction, at the
        // height of the impact (on a wall it floats at the hit point).
        const Vector3 end = m_aimArc.back();
        const float px = -m_aimDir.z * 0.45f, pz = m_aimDir.x * 0.45f;
        m_scratch.push_back({ { end.x - px, end.y, end.z - pz }, aimColor });
        m_scratch.push_back({ { end.x + px, end.y, end.z + pz }, aimColor });

        if (m_class->primary.lobVelocity > 0.0f)
            AppendCircle(m_scratch, end, 0.4f, aimColor);

        // Grenade marker: where the throw first touches down. Deliberately not
        // the blast radius — the grenade bounces and rolls on from here before
        // its fuse ends it, so a ring sized to the blast would promise a
        // detonation point nothing can predict. What it does show honestly is
        // the throw's reach, so a lob falling short of the cursor is visible
        // before it leaves the hand. Gone for good once the grenade is spent,
        // which is the in-world half of the HUD's count.
        if (m_grenades > 0)
        {
            const float dist = PredictShotStop(kGrenade, m_playerPos, m_aimDir, m_aimDist);
            const Vector3 land(m_playerPos.x + m_aimDir.x * dist, kAimRingHeight,
                               m_playerPos.z + m_aimDir.z * dist);
            AppendCircle(m_scratch, land, kGrenadeMarkRadius, kGrenadeAimColor);
        }

        // The swing that just happened: the blade's arc at its real reach,
        // frozen where it was aimed and fading out over the flash. It's the
        // only account of a melee there is — a miss makes no impact, spills no
        // blood, and would otherwise leave nothing on screen to have missed.
        if (m_meleeFlash > 0.0f)
        {
            const XMFLOAT4 color = { kMeleeArcColor.x, kMeleeArcColor.y, kMeleeArcColor.z,
                                     kMeleeArcColor.w * (m_meleeFlash / kMeleeFlashTime) };
            const float heading = std::atan2(m_meleeSwingDir.z, m_meleeSwingDir.x);
            const Vector3 center(m_playerPos.x, kAimRingHeight, m_playerPos.z);
            AppendArc(m_scratch, center, kMelee.reach, heading - kMelee.arc, kMelee.arc * 2.0f, 12,
                      color);
        }

        // Whatever the ability has to say about itself: how much of it is left
        // to run, and who it's reaching. It goes in this batch because it's the
        // same kind of thing as the aim line and the swing arc — a mark on the
        // world about what this soldier is doing — but what those marks are is
        // the ability's own business.
        Ability::AppendIndicator(m_class->ability, m_ability, m_playerPos, m_scratch);

        // Friend or foe, for everyone but the player: a ring under the soldiers
        // on their own side. Same visibility rule the bodies themselves get —
        // a marker that showed through a wall would be a squadmate radar, which
        // is a different feature and one the fog of war exists to deny.
        const XMFLOAT4& teamTint = TeamColor(m_team);
        const XMFLOAT4 ringColor = { teamTint.x, teamTint.y, teamTint.z, kFriendlyRingAlpha };
        for (const Npc& npc : m_npcs)
        {
            if (npc.team != m_team)
                continue;
            if (!renderer.IsSphereVisible({ npc.pos.x, kSoldierBoundsY, npc.pos.z },
                                          kSoldierBoundsRadius))
                continue;
            if (!Visibility::IsPointVisible(eye, { npc.pos.x, npc.pos.z }, m_occluders))
                continue;
            AppendCircle(m_scratch, { npc.pos.x, kAimRingHeight, npc.pos.z }, kFriendlyRingRadius,
                         ringColor);
        }

        renderer.DrawLinesAlpha(m_scratch.data(), static_cast<uint32_t>(m_scratch.size()),
                                identity);
    }

    RenderHud(renderer);
}

// Screen-space overlay: the gameplay cluster (Hud.cpp), the respawn countdown,
// and the perf counters.
void Game::RenderHud(Renderer& renderer)
{
    const float w = static_cast<float>(renderer.Width());
    const float h = static_cast<float>(renderer.Height());
    const float size = h * 0.024f;

    // The loadout readout is a snapshot handed over whole, so the HUD reads
    // nothing out of the game itself. Reload progress runs 0 -> 1 rather than
    // counting seconds down, because that's what a bar wants; a negative value
    // means the magazine is in and there's nothing to wait for.
    Hud::State hud = {};
    hud.hp = m_playerHp;
    hud.maxHp = kMaxHealth;
    hud.ammo = m_ammo;
    hud.magazine = m_class->primary.magazine;
    hud.reloadFraction =
        m_reloadTimer > 0.0f && m_class->primary.reloadTime > 0.0f
            ? 1.0f - m_reloadTimer / m_class->primary.reloadTime
            : -1.0f;
    hud.melee = m_meleeCharges;
    hud.meleeCharges = kMelee.charges;
    // The recovery runs after every swing, including the ones that leave
    // charges in hand, but a refill the player can swing straight through isn't
    // a wait and isn't worth a bar. What the pips say — how many swings are
    // there right now — is the whole answer until there are none, and only then
    // does the clock become the thing to read.
    hud.meleeRecoverFraction =
        m_meleeCharges == 0 && m_meleeRecover > 0.0f && kMelee.recoverTime > 0.0f
            ? 1.0f - m_meleeRecover / kMelee.recoverTime
            : -1.0f;
    hud.grenades = m_grenades;
    // The ability module is the class's own, so it's handed over as the whole
    // definition: the HUD needs its name for the key hint and its cooldown to
    // turn the seconds left into a bar. A class without one hands over nothing
    // and gets no module, rather than getting an empty one — an ability isn't
    // equipment that ran out.
    hud.ability = m_class->ability.kind != Ability::Kind::None ? &m_class->ability : nullptr;
    hud.abilityFraction = m_ability.time > 0.0f && m_class->ability.duration > 0.0f
                              ? 1.0f - m_ability.time / m_class->ability.duration
                              : -1.0f;
    hud.abilityCooldown = m_ability.cooldown;
    // The roster, both sides counted the same way: everyone standing. The
    // player is one of the soldiers on their own side rather than an extra on
    // top of it, which is what makes the two rows comparable — five against
    // five should read as five against five, and it wouldn't if one row
    // counted the local soldier and the other had nobody to leave out.
    hud.allies = NpcCount(m_team) + (PlayerOnField() ? 1 : 0);
    hud.enemies = static_cast<int>(std::count_if(m_npcs.begin(), m_npcs.end(), [this](const Npc& n) {
        return n.team != m_team && n.hp > 0.0f;
    }));
    hud.teamSize = kTeamSize;
    // The same two colors the soldiers are wearing, so the corner and the field
    // agree. With two sides the enemy is simply the other one; a third team
    // would need this to be something better than "not mine", but it would need
    // a third row too, and the panel is built for a pair.
    hud.allyColor = TeamColor(m_team);
    hud.enemyColor = TeamColor(m_team + 1);
    hud.accent = m_class->color;
    hud.alive = m_phase == Phase::Playing;

    // The key-cap row. The ability leads it, named after what it does rather
    // than after the slot it sits in — FIELD DRESSING says more about the class
    // than ABILITY ever would, and it's the only cap here a player might not
    // already know. The rest is the standard kit every soldier carries.
    //
    // The caps are the player's own bindings, so a rebind is on the HUD the
    // moment they leave the settings screen and nothing here has to be told.
    Hud::Hint hints[kMaxHints];
    size_t hintCount = 0;
    const auto addHint = [&](std::string key, const char* label) {
        m_hintKeys[hintCount] = std::move(key);
        hints[hintCount] = { m_hintKeys[hintCount].c_str(), label };
        ++hintCount;
    };
    if (hud.ability)
        addHint(m_binds.Label(Act::Ability), m_class->ability.name);
    addHint(m_binds.Label(Act::Reload), "RELOAD");
    addHint(m_binds.Label(Act::Grenade), "GRENADE");
    addHint(m_binds.Label(Act::Melee), "MELEE");
    addHint(m_binds.Label(Act::Steady), "STEADY");

    hud.hints = hints;
    hud.hintCount = hintCount;
    Hud::Render(renderer, hud);

    // Respawn countdown, centered and big: while it's up there's nothing else
    // to do, so it's the one thing on screen worth reading. Counts whole
    // seconds remaining, so it reaches 1 for the last second and never shows a
    // 0 the player can't act on.
    if (m_phase == Phase::Dead)
    {
        const std::string countdown =
            "RESPAWNING IN " + std::to_string(static_cast<int>(std::ceil(m_respawnTimer)));
        const float countSize = size * 1.6f;
        renderer.DrawScreenText(countdown,
                                (w - renderer.MeasureScreenText(countdown, countSize)) * 0.5f,
                                h * 0.42f, countSize, kHudColor);
    }

    // Perf counters in the top-left corner, clear of the gameplay HUD. FRAME is
    // wall clock and pins to the refresh rate while there's headroom; CPU
    // (command recording) and the draw count respond to render-side changes.
    // Half the gameplay HUD's size: it's a debug readout, not something to read
    // mid-fight, so it stays legible without competing for attention.
    const float perfSize = size * 0.5f;
    char perf[96];
    std::snprintf(perf, sizeof(perf), "FRAME %.1fMS  CPU %.2fMS  DRAWS %u", m_frameMs,
                  renderer.CpuFrameMs(), renderer.LastDrawCalls());
    renderer.DrawScreenText(perf, size, size, perfSize, kHudHintColor);
}
