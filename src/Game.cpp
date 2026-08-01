#include "Game.h"

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

    // Default bindings. Keys are still hardcoded per action (there's no
    // rebinding UI or config file yet); naming them keeps the defaults in one
    // place for when there is one.
    constexpr int kGrenadeKey = 'F';
    constexpr int kSpawnNpcKey = 'N';

    // NPC combat tuning. Engage range is intentionally shorter than what the
    // player can see, so NPCs can be picked at from a distance.
    constexpr float kNpcEngageRange = 22.0f;    // sight radius for entering combat
    constexpr float kNpcPreferredRange = 11.0f; // closer than this they strafe, not advance
    constexpr float kNpcAimJitter = 0.06f;      // radians of random spread per shot
    constexpr float kNpcCombatSpeed = 0.85f;    // fraction of class move speed in combat
    constexpr float kNpcWanderSpeed = 0.5f;     // fraction of class move speed idling

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
    constexpr float kHealthBarY = 1.42f; // clears the helmet and antenna tip

    // Bounding sphere used to skip soldiers that fall outside the viewport.
    // Generous on purpose: it has to cover the model at full stride and the
    // health bar floating above the helmet, and popping in at the screen edge
    // is far worse than submitting a few extra draws.
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
    // The grenade's ring is orange, so it never reads as part of the primary's
    // yellow aim line.
    constexpr XMFLOAT4 kGrenadeAimColor = { 0.95f, 0.50f, 0.18f, 0.35f };
    constexpr float kAimRingHeight = 0.05f;    // above the fog quads, so it isn't dimmed
    constexpr float kGrenadeMarkRadius = 0.5f; // first-bounce marker, not the blast size
    constexpr XMFLOAT4 kFogColor = { 0.01f, 0.02f, 0.04f, 0.85f };
    constexpr XMFLOAT4 kHudColor = { 0.85f, 0.90f, 0.95f, 1.0f };
    constexpr XMFLOAT4 kHudHintColor = { 0.45f, 0.52f, 0.62f, 1.0f };

    // Appends a solid cube with fixed per-face shading. Units and obstacles
    // moved to the renderer's lit shapes; this remains for overlay-style
    // readouts (NPC health bars) that shouldn't respond to scene lighting.
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

    // A horizontal ring of line segments, for the aim indicator's landing marks.
    void AppendCircle(std::vector<Vertex>& out, const Vector3& center, float radius,
                      const XMFLOAT4& color)
    {
        constexpr int kSegments = 24;
        for (int i = 0; i < kSegments; ++i)
        {
            const float a0 = XM_2PI * i / kSegments;
            const float a1 = XM_2PI * (i + 1) / kSegments;
            out.push_back({ XMFLOAT3{ center.x + std::cos(a0) * radius, center.y,
                                      center.z + std::sin(a0) * radius }, color });
            out.push_back({ XMFLOAT3{ center.x + std::cos(a1) * radius, center.y,
                                      center.z + std::sin(a1) * radius }, color });
        }
    }

    // Draws a living soldier: the model's segments posed by the walk cycle and
    // placed at `pos` facing `aimDir`. A corpse draws the same parts, its
    // segments posed by the ragdoll instead (see Game::Render).
    void DrawSoldier(Renderer& renderer, const Vector3& pos, const Vector3& aimDir,
                     float walkPhase, float moveBlend, const XMFLOAT4& color)
    {
        XMMATRIX local[Soldier::SegmentCount];
        Soldier::Pose(local, walkPhase, moveBlend);

        const XMMATRIX base = Soldier::Base(pos, aimDir);
        XMMATRIX world[Soldier::SegmentCount];
        for (int i = 0; i < Soldier::SegmentCount; ++i)
            world[i] = local[i] * base;

        Soldier::Draw(renderer, world, color);
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
    m_sound.Init(); // loads the wave bank and starts the ambience

    const LevelData level = LevelData::Load("assets/levels/arena01.json");
    m_arenaHalf = level.arenaHalf;
    for (const LevelData::Spawn& spawn : level.spawns)
        m_teamSpawns.push_back(spawn.pos);
    m_team = std::min(m_team, static_cast<int>(m_teamSpawns.size()) - 1);
    m_playerPos = m_teamSpawns[m_team];

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

    m_deathFlashTime = std::max(0.0f, m_deathFlashTime - dt);

    if (m_phase == Phase::ClassSelect)
    {
        if (const auto picked =
                m_classSelect.Update(input, camera.ViewportWidth(), camera.ViewportHeight()))
        {
            m_class = &GetClassDef(*picked);
            m_phase = Phase::Playing;
            m_fireCooldown = 0.3f; // so the selection click doesn't fire a shot
            camera.SetTarget(m_playerPos);
            camera.SnapToTarget();
        }
        return;
    }

    // --- Movement: WASD relative to the screen ---
    const Vector3 upG = camera.ScreenUpOnGround();
    const Vector3 rightG = camera.ScreenRightOnGround();

    float moveUp = 0.0f, moveRight = 0.0f;
    if (input.Key('W')) moveUp += 1.0f;
    if (input.Key('S')) moveUp -= 1.0f;
    if (input.Key('D')) moveRight += 1.0f;
    if (input.Key('A')) moveRight -= 1.0f;
    moveUp += input.pad.thumbSticks.leftY;
    moveRight += input.pad.thumbSticks.leftX;

    Vector3 move = upG * moveUp + rightG * moveRight;
    const bool moving = move.LengthSquared() > 1e-10f;
    if (moving)
    {
        move.Normalize();
        m_playerPos += move * (m_class->moveSpeed * dt);
    }
    ResolveObstacles(m_playerPos);

    if (moving)
        m_walkPhase += m_class->moveSpeed * kStrideRate * dt;
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
        m_aimDir = aim;
    }

    // --- Firing ---
    using PadTracker = DirectX::GamePad::ButtonStateTracker;
    m_fireCooldown -= dt;
    if ((input.MouseDown(0) || input.MousePressed(0) || input.Key(VK_SPACE) ||
         input.pad.triggers.right > 0.5f) &&
        m_fireCooldown <= 0.0f)
    {
        SpawnShot(m_class->primary, m_playerPos, m_aimDir, m_team, m_aimDist);
        m_fireCooldown = m_class->primary.fireInterval;
    }

    // --- Grenade: same for every class, lobbed onto the aim point, then left
    // to bounce until its fuse runs out (UpdateProjectiles). One per life, so
    // there's no cooldown to run down — spending it is the whole cost. Thrown
    // on the key's press edge, which with a single grenade also stops a held
    // key from throwing it before the player means to ---
    if ((input.KeyPressed(kGrenadeKey) || input.padEvents.leftShoulder == PadTracker::PRESSED) &&
        m_grenades > 0)
    {
        SpawnShot(kGrenade, m_playerPos, m_aimDir, m_team, m_aimDist);
        --m_grenades;
    }

    // --- NPCs: debug spawning and AI ---
    if (input.KeyPressed(kSpawnNpcKey) || input.padEvents.y == PadTracker::PRESSED)
        SpawnNpc();
    UpdateNpcs(dt);

    // --- Projectiles (simulated by Jolt) ---
    m_physics.Step(dt);
    UpdateProjectiles(dt);
    UpdateParticles(dt);
    UpdateCorpses(dt);

    if (m_playerDied)
    {
        m_playerDied = false;
        // The body stays where it fell — the player respawns out of it.
        SpawnCorpse(m_playerPos, m_aimDir, m_walkPhase, m_moveBlend, m_class->color, m_deathKnock);
        m_playerHp = kMaxHealth;
        m_playerPos = m_teamSpawns[m_team];
        m_grenades = kGrenadesPerLife; // respawn is a fresh loadout
        m_deathFlashTime = 0.6f; // brief grayscale flash while respawning
        camera.SetTarget(m_playerPos);
        camera.SnapToTarget();
        return;
    }

    camera.SetTarget(m_playerPos);
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

void Game::SpawnNpc()
{
    // NPCs belong to the other team and appear at its spawn point, scattered
    // a little so repeated presses don't stack them on one tile.
    const int enemyTeam = (m_team + 1) % static_cast<int>(m_teamSpawns.size());

    Npc npc;
    npc.cls = &kClassDefs[m_nextNpcClass];
    m_nextNpcClass = (m_nextNpcClass + 1) % static_cast<int>(kClassCount);
    npc.pos = m_teamSpawns[enemyTeam];
    npc.pos.x += Rand(-2.0f, 2.0f);
    npc.pos.z += Rand(-2.0f, 2.0f);
    ResolveObstacles(npc.pos);
    npc.aimDir = { -1.0f, 0.0f, 0.0f };
    npc.hp = kMaxHealth;
    npc.fireCooldown = 0.5f; // brief grace so spawns don't instantly fire
    npc.wanderTarget = npc.pos;
    npc.repickTimer = 0.0f; // picks a real wander target on the first tick
    npc.strafeSign = (Rand(0.0f, 1.0f) < 0.5f) ? -1.0f : 1.0f;
    npc.strafeTimer = Rand(1.0f, 2.5f);
    npc.walkPhase = Rand(0.0f, XM_2PI); // desync strides across the squad
    npc.moveBlend = 0.0f;
    m_npcs.push_back(npc);
}

void Game::UpdateNpcs(float dt)
{
    const Vector2 playerXZ = { m_playerPos.x, m_playerPos.z };
    for (Npc& npc : m_npcs)
    {
        npc.fireCooldown -= dt;

        Vector3 toPlayer = m_playerPos - npc.pos;
        toPlayer.y = 0.0f;
        const float dist = toPlayer.Length();
        // NPCs obey the same sight rules as the player's fog of war, so they
        // can't shoot through walls the player can't see through either.
        const bool engaged = dist < kNpcEngageRange && dist > 1e-3f &&
                             Visibility::IsPointVisible({ npc.pos.x, npc.pos.z }, playerXZ,
                                                        m_occluders);

        Vector3 move;
        float speed = npc.cls->moveSpeed;
        const WeaponDef& weapon = npc.cls->primary;
        if (engaged)
        {
            toPlayer /= dist;
            npc.aimDir = toPlayer;

            npc.strafeTimer -= dt;
            if (npc.strafeTimer <= 0.0f)
            {
                npc.strafeSign = -npc.strafeSign;
                npc.strafeTimer = Rand(1.0f, 2.5f);
            }
            if (dist > kNpcPreferredRange)
                move = toPlayer;
            else
                move = Vector3(-toPlayer.z, 0.0f, toPlayer.x) * npc.strafeSign;
            speed *= kNpcCombatSpeed;

            if (npc.fireCooldown <= 0.0f)
            {
                // A touch of angular spread keeps NPCs beatable up close and
                // makes long-range sniper duels survivable.
                const Vector3 dir = Vector3::Transform(
                    npc.aimDir, Matrix::CreateRotationY(Rand(-kNpcAimJitter, kNpcAimJitter)));
                const int npcTeam = (m_team + 1) % static_cast<int>(m_teamSpawns.size());
                // NPCs "aim" at the player's feet, so a grenadier's lob comes
                // down on the player instead of sailing to max range.
                SpawnShot(weapon, npc.pos, dir, npcTeam, dist);
                npc.fireCooldown = weapon.fireInterval;
            }
        }
        else
        {
            npc.repickTimer -= dt;
            const Vector3 toTarget = npc.wanderTarget - npc.pos;
            const float wlen = toTarget.Length();
            if (wlen < 1.0f || npc.repickTimer <= 0.0f)
            {
                const float margin = m_arenaHalf - 2.0f;
                npc.wanderTarget = { Rand(-margin, margin), 0.0f, Rand(-margin, margin) };
                npc.repickTimer = Rand(4.0f, 8.0f);
            }
            else
            {
                move = toTarget / wlen;
                npc.aimDir = move;
            }
            speed *= kNpcWanderSpeed;
        }

        const bool moving = move.LengthSquared() > 1e-6f;
        if (moving)
            npc.walkPhase += speed * kStrideRate * dt;
        npc.moveBlend = std::clamp(npc.moveBlend + (moving ? kMoveBlendRate : -kMoveBlendRate) * dt,
                                   0.0f, 1.0f);

        npc.pos += move * (speed * dt);
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

    // Like direct hits, a blast only hurts the other side.
    if (team == m_team)
    {
        for (Npc& npc : m_npcs)
        {
            if (npc.hp <= 0.0f) // already killed this frame, by the direct hit or an earlier blast
                continue;
            const float dmg = splash({ npc.pos.x, kPlayerHalf, npc.pos.z });
            if (dmg <= 0.0f)
                continue;
            npc.hp -= dmg;
            // Blown outward from the blast, as hard as the share of it they
            // caught: a body at the rim topples, one on top of it is thrown.
            npc.knock = CorpseKnock(npc.pos - center, kCorpseBlastKnock * (dmg / damage));
            PlaySoundAt(npc.hp <= 0.0f ? "death" : "hit", npc.pos);
        }
    }
    else
    {
        const float dmg = splash({ m_playerPos.x, kPlayerHalf, m_playerPos.z });
        if (dmg > 0.0f && m_playerHp > 0.0f)
        {
            m_playerHp -= dmg;
            m_deathKnock = CorpseKnock(m_playerPos - center, kCorpseBlastKnock * (dmg / damage));
            m_rumbleTime = 0.25f;
            if (m_playerHp <= 0.0f)
                m_playerDied = true;
            m_sound.Play(m_playerDied ? "death" : "hit");
        }
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
        if (shot.life > 0.0f && shot.team == m_team)
        {
            for (Npc& npc : m_npcs)
            {
                const XMFLOAT3 center = { npc.pos.x, kPlayerHalf, npc.pos.z };
                if (npc.hp > 0.0f &&
                    SegmentHitsBox(shot.prevPos, pos, center, bodyHalf, shot.radius))
                {
                    if (!blast)
                    {
                        npc.hp -= shot.damage;
                        npc.knock = CorpseKnock(travel, kCorpseKnock);
                        PlaySoundAt(npc.hp <= 0.0f ? "death" : "hit", npc.pos);
                    }
                    shot.life = 0.0f;
                    Detonate(shot, pos, true);
                    detonated = true;
                    break;
                }
            }
        }
        else if (shot.life > 0.0f)
        {
            const XMFLOAT3 center = { m_playerPos.x, kPlayerHalf, m_playerPos.z };
            if (SegmentHitsBox(shot.prevPos, pos, center, bodyHalf, shot.radius))
            {
                if (!blast)
                {
                    m_playerHp -= shot.damage;
                    m_deathKnock = CorpseKnock(travel, kCorpseKnock);
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

    // The dead leave a ragdoll standing exactly where they fell before they
    // come off the roster.
    for (const Npc& npc : m_npcs)
        if (npc.hp <= 0.0f)
            SpawnCorpse(npc.pos, npc.aimDir, npc.walkPhase, npc.moveBlend, npc.cls->color,
                        npc.knock);
    std::erase_if(m_npcs, [](const Npc& n) { return n.hp <= 0.0f; });
}

void Game::SpawnCorpse(const Vector3& pos, const Vector3& aimDir, float walkPhase, float moveBlend,
                       const XMFLOAT4& color, const Vector3& knock)
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
    corpse.color = color;
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
    renderer.SetMonochrome(m_deathFlashTime > 0.0f);

    if (m_phase == Phase::ClassSelect)
    {
        m_classSelect.Render(renderer);
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

    DrawSoldier(renderer, m_playerPos, m_aimDir, m_walkPhase, m_moveBlend, m_class->color);

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
        DrawSoldier(renderer, npc.pos, npc.aimDir, npc.walkPhase, npc.moveBlend, npc.cls->color);

        // Health bar: a thin slab that shrinks toward its left edge and shifts
        // green -> red, so weapon damage is readable per hit. Stays a
        // vertex-color cube: it's a readout, not a lit object. Undamaged
        // soldiers show no bar at all, so a full row of them reads as clean
        // silhouettes and any bar on screen means something took a hit.
        if (npc.hp >= kMaxHealth)
            continue;
        const float frac = std::max(npc.hp, 0.0f) / kMaxHealth;
        const float barW = 0.9f * frac;
        const XMFLOAT4 barColor = { 0.9f * (1.0f - frac), 0.8f * frac, 0.1f, 1.0f };
        AppendCube(m_scratch, { npc.pos.x - (0.9f - barW) * 0.5f, kHealthBarY, npc.pos.z },
                   { barW, 0.08f, 0.08f }, barColor);
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
        Soldier::Draw(renderer, world, corpse.color);
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

    // Fog of war goes on after all opaque geometry so it blends over the
    // floor while walls (which wrote depth) still punch through it.
    m_fogVerts.clear();
    AppendFog(m_fogVerts);
    renderer.DrawTrianglesAlpha(m_fogVerts.data(), static_cast<uint32_t>(m_fogVerts.size()),
                                identity);

    // Aim indicator: the shot's actual trajectory as a dim 3D polyline — an
    // arch for the grenade's lob, a near-level line with droop for bullets —
    // ending where the shot really stops (max range, the first wall the arc
    // can't clear, or the ground). A bright tick crosses the end point, lobbed
    // weapons get a landing circle, and the grenade gets a marker where the
    // throw touches down. Drawn after the fog pass so it stays bright.
    {
        PredictShotStop(m_class->primary, m_playerPos, m_aimDir, m_aimDist, &m_aimArc);

        m_scratch.clear();
        for (size_t i = 1; i < m_aimArc.size(); ++i)
        {
            m_scratch.push_back({ m_aimArc[i - 1], kAimDimColor });
            m_scratch.push_back({ m_aimArc[i], kAimDimColor });
        }

        // End-of-flight tick, perpendicular to the aim direction, at the
        // height of the impact (on a wall it floats at the hit point).
        const Vector3 end = m_aimArc.back();
        const float px = -m_aimDir.z * 0.45f, pz = m_aimDir.x * 0.45f;
        m_scratch.push_back({ { end.x - px, end.y, end.z - pz }, kAimColor });
        m_scratch.push_back({ { end.x + px, end.y, end.z + pz }, kAimColor });

        if (m_class->primary.lobVelocity > 0.0f)
            AppendCircle(m_scratch, end, 0.4f, kAimColor);

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

        renderer.DrawLinesAlpha(m_scratch.data(), static_cast<uint32_t>(m_scratch.size()),
                                identity);
    }

    RenderHud(renderer);
}

// Screen-space overlay: player health, grenade state, NPC count, perf counters,
// control hints.
void Game::RenderHud(Renderer& renderer)
{
    const float w = static_cast<float>(renderer.Width());
    const float h = static_cast<float>(renderer.Height());
    const float size = h * 0.024f;
    const float y = h - size * 2.0f;
    // Grenades read as a count, not a timer: there's nothing to wait out, so
    // what matters is whether one is left.
    const std::string status = "HP " + std::to_string(static_cast<int>(std::ceil(m_playerHp))) +
                               "   NADES " + std::to_string(m_grenades) +
                               "   NPCS " + std::to_string(m_npcs.size());
    renderer.DrawScreenText(status, size, y, size, kHudColor);

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

    const std::string hint = "F - GRENADE   N - SPAWN NPC";
    renderer.DrawScreenText(hint, w - renderer.MeasureScreenText(hint, size) - size, y, size,
                            kHudHintColor);
}
