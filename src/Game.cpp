#include "Game.h"

#include "Level.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace DirectX;
using namespace DirectX::SimpleMath;

namespace
{
    constexpr float kPlayerHalf = 0.4f;
    constexpr float kMuzzleHeight = 0.6f;
    constexpr float kMuzzleOffset = 0.7f; // shots start this far ahead of the shooter
    constexpr float kGravity = 9.81f;     // must match Jolt's default gravity magnitude

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

    constexpr XMFLOAT4 kGridMinor = { 0.10f, 0.13f, 0.17f, 1.0f };
    constexpr XMFLOAT4 kGridMajor = { 0.17f, 0.22f, 0.29f, 1.0f };
    constexpr XMFLOAT4 kBorder = { 0.55f, 0.25f, 0.20f, 1.0f };
    constexpr XMFLOAT4 kObstacleColor = { 0.35f, 0.40f, 0.50f, 1.0f };
    constexpr XMFLOAT4 kProjectileColor = { 1.00f, 0.80f, 0.20f, 1.0f };
    // The aim indicator is alpha-blended; the shaft dims via alpha rather
    // than darker RGB so whatever it crosses still shows through.
    constexpr XMFLOAT4 kAimColor = { 0.95f, 0.95f, 0.40f, 0.55f };
    constexpr XMFLOAT4 kAimDimColor = { 0.95f, 0.95f, 0.40f, 0.22f };
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

    // Horizontal muzzle speed for cls's shot aimed to come down targetDist
    // away: bullets always fire at full speed; lobbed shots slow their toss to
    // drop on the aim point, capped at the class's muzzle speed.
    float ShotSpeed(const ClassDef& cls, float targetDist)
    {
        float speed = cls.projectileSpeed;
        if (cls.lobVelocity > 0.0f)
        {
            const float vy = cls.lobVelocity;
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

void Game::Update(float dt, const Input& input, IsoCamera& camera)
{
    m_sound.Update();

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
    if (move.LengthSquared() > 1e-10f)
    {
        move.Normalize();
        m_playerPos += move * (m_class->moveSpeed * dt);
    }
    ResolveObstacles(m_playerPos);

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
    m_fireCooldown -= dt;
    if ((input.MouseDown(0) || input.MousePressed(0) || input.Key(VK_SPACE) ||
         input.pad.triggers.right > 0.5f) &&
        m_fireCooldown <= 0.0f)
    {
        SpawnShot(*m_class, m_playerPos, m_aimDir, m_team, m_aimDist);
        m_fireCooldown = m_class->fireInterval;
    }

    // --- NPCs: debug spawning and AI ---
    using PadTracker = DirectX::GamePad::ButtonStateTracker;
    if (input.KeyPressed('N') || input.padEvents.y == PadTracker::PRESSED)
        SpawnNpc();
    UpdateNpcs(dt);

    // --- Projectiles (simulated by Jolt) ---
    m_physics.Step(dt);
    UpdateProjectiles(dt);
    UpdateParticles(dt);

    if (m_playerDied)
    {
        m_playerDied = false;
        m_playerHp = kMaxHealth;
        m_playerPos = m_teamSpawns[m_team];
        m_deathFlashTime = 0.6f; // brief grayscale flash while respawning
        camera.SetTarget(m_playerPos);
        camera.SnapToTarget();
        return;
    }

    camera.SetTarget(m_playerPos);
}

void Game::SpawnShot(const ClassDef& cls, const Vector3& from, const Vector3& dir, int team,
                     float targetDist)
{
    Vector3 pos = from + dir * kMuzzleOffset;
    pos.y = kMuzzleHeight;

    // Grenades keep their fixed upward lob (constant arc height and flight
    // time) and vary horizontal speed to land on the aim point; bullets fire
    // level at full speed, so max range comes from gravity.
    const Vector3 vel =
        dir * ShotSpeed(cls, targetDist) + Vector3(0.0f, cls.lobVelocity, 0.0f);
    m_projectiles.push_back({ m_physics.SpawnProjectile(pos, vel, cls.projectileRadius,
                                                        cls.projectileMass),
                              cls.projectileLife, pos, team, cls.damage, cls.projectileRadius,
                              cls.explodes });

    // One shared fire sample; heavier weapons play deeper. A little random
    // detune keeps rapid fire from sounding like a loop.
    const float pitch = 0.25f - cls.damage / 120.0f + Rand(-0.05f, 0.05f);
    PlaySoundAt("fire", from, pitch);
}

float Game::PredictShotStop(const ClassDef& cls, const Vector3& from, const Vector3& dir,
                            float targetDist, std::vector<Vector3>* outArc) const
{
    const float speed = ShotSpeed(cls, targetDist);
    const float radius = cls.projectileRadius;
    if (outArc)
    {
        outArc->clear();
        outArc->push_back({ from.x + dir.x * kMuzzleOffset, kMuzzleHeight,
                            from.z + dir.z * kMuzzleOffset });
    }
    // Coarser than the physics tick, but the arc is smooth and the boxes are
    // fat relative to per-step travel, so the indicator lands within a step.
    constexpr float kStep = 1.0f / 120.0f;
    for (float t = kStep; t < cls.projectileLife; t += kStep)
    {
        const float ht = kMuzzleOffset + speed * t;
        const float y = kMuzzleHeight + cls.lobVelocity * t - 0.5f * kGravity * t * t;
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
    return kMuzzleOffset + speed * cls.projectileLife;
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
                SpawnShot(*npc.cls, npc.pos, dir, npcTeam, dist);
                npc.fireCooldown = npc.cls->fireInterval;
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

void Game::ImpactEffect(const Projectile& shot, const Vector3& pos, bool hitUnit)
{
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
        if (pos.x < -m_arenaHalf || pos.x > m_arenaHalf ||
            pos.z < -m_arenaHalf || pos.z > m_arenaHalf)
            shot.life = 0.0f;

        const XMFLOAT3 bodyHalf = { kPlayerHalf, kPlayerHalf, kPlayerHalf };
        if (shot.life > 0.0f && shot.team == m_team)
        {
            for (Npc& npc : m_npcs)
            {
                const XMFLOAT3 center = { npc.pos.x, kPlayerHalf, npc.pos.z };
                if (npc.hp > 0.0f &&
                    SegmentHitsBox(shot.prevPos, pos, center, bodyHalf, shot.radius))
                {
                    npc.hp -= shot.damage;
                    PlaySoundAt(npc.hp <= 0.0f ? "death" : "hit", npc.pos);
                    shot.life = 0.0f;
                    ImpactEffect(shot, pos, true);
                    break;
                }
            }
        }
        else if (shot.life > 0.0f)
        {
            const XMFLOAT3 center = { m_playerPos.x, kPlayerHalf, m_playerPos.z };
            if (SegmentHitsBox(shot.prevPos, pos, center, bodyHalf, shot.radius))
            {
                m_playerHp -= shot.damage;
                m_rumbleTime = 0.25f;
                if (m_playerHp <= 0.0f)
                    m_playerDied = true;
                m_sound.Play(m_playerDied ? "death" : "hit");
                shot.life = 0.0f;
                ImpactEffect(shot, pos, true);
            }
        }

        // Projectiles stop where they land: first touch of world geometry
        // (walls, floor) removes them. Checked after the unit sweeps so a shot
        // that clips a target on its impact tick still deals its damage.
        if (shot.life > 0.0f && m_physics.HadContact(shot.body))
        {
            shot.life = 0.0f;
            ImpactEffect(shot, pos, false);
        }

        shot.prevPos = pos;
    }

    for (const Projectile& shot : m_projectiles)
        if (shot.life <= 0.0f)
            m_physics.RemoveBody(shot.body);
    std::erase_if(m_projectiles, [](const Projectile& s) { return s.life <= 0.0f; });
    std::erase_if(m_npcs, [](const Npc& n) { return n.hp <= 0.0f; });
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

    // Units are a cylinder body with a sphere "head" cap, tinted by class.
    auto drawUnit = [&](const XMFLOAT3& pos, const XMFLOAT4& color) {
        const float bodySize = kPlayerHalf * 2.0f;
        renderer.DrawShape(Shape::Cylinder,
                           XMMatrixScaling(bodySize, bodySize, bodySize) *
                               XMMatrixTranslation(pos.x, kPlayerHalf, pos.z),
                           color);
        renderer.DrawShape(Shape::Sphere,
                           XMMatrixScaling(0.4f, 0.4f, 0.4f) *
                               XMMatrixTranslation(pos.x, kPlayerHalf * 2.35f, pos.z),
                           color);
    };
    drawUnit(m_playerPos, m_class->color);

    // NPCs and projectiles the player can't see stay hidden — an enemy behind
    // a wall disappears until it re-emerges.
    m_scratch.clear();
    const XMFLOAT2 eye = { m_playerPos.x, m_playerPos.z };
    for (const Npc& npc : m_npcs)
    {
        if (!Visibility::IsPointVisible(eye, { npc.pos.x, npc.pos.z }, m_occluders))
            continue;
        drawUnit(npc.pos, npc.cls->color);

        // Health bar: a thin slab that shrinks toward its left edge and shifts
        // green -> red, so weapon damage is readable per hit. Stays a
        // vertex-color cube: it's a readout, not a lit object.
        const float frac = std::max(npc.hp, 0.0f) / kMaxHealth;
        const float barW = 0.9f * frac;
        const XMFLOAT4 barColor = { 0.9f * (1.0f - frac), 0.8f * frac, 0.1f, 1.0f };
        AppendCube(m_scratch, { npc.pos.x - (0.9f - barW) * 0.5f, kPlayerHalf * 3.0f, npc.pos.z },
                   { barW, 0.08f, 0.08f }, barColor);
    }
    for (const Projectile& shot : m_projectiles)
    {
        const XMFLOAT3 pos = m_physics.GetPosition(shot.body);
        if (Visibility::IsPointVisible(eye, { pos.x, pos.z }, m_occluders))
        {
            const float d = m_class->projectileRadius * 2.0f;
            renderer.DrawShape(Shape::Sphere,
                               XMMatrixScaling(d, d, d) *
                                   XMMatrixTranslation(pos.x, pos.y, pos.z),
                               kProjectileColor);
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
    // can't clear, or the ground). A bright tick crosses the end point, and
    // lobbed weapons get a landing circle. Drawn after the fog pass so it
    // stays bright.
    {
        PredictShotStop(*m_class, m_playerPos, m_aimDir, m_aimDist, &m_aimArc);

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

        if (m_class->lobVelocity > 0.0f)
        {
            constexpr int kSegments = 20;
            constexpr float kRadius = 0.4f;
            for (int i = 0; i < kSegments; ++i)
            {
                const float a0 = XM_2PI * i / kSegments;
                const float a1 = XM_2PI * (i + 1) / kSegments;
                m_scratch.push_back({ { end.x + std::cos(a0) * kRadius, end.y,
                                        end.z + std::sin(a0) * kRadius }, kAimColor });
                m_scratch.push_back({ { end.x + std::cos(a1) * kRadius, end.y,
                                        end.z + std::sin(a1) * kRadius }, kAimColor });
            }
        }
        renderer.DrawLinesAlpha(m_scratch.data(), static_cast<uint32_t>(m_scratch.size()),
                                identity);
    }

    RenderHud(renderer);
}

// Screen-space overlay: player health, NPC count, and the spawn hint.
void Game::RenderHud(Renderer& renderer)
{
    const float w = static_cast<float>(renderer.Width());
    const float h = static_cast<float>(renderer.Height());
    const float size = h * 0.024f;
    const float y = h - size * 2.0f;
    const std::string status = "HP " + std::to_string(static_cast<int>(std::ceil(m_playerHp))) +
                               "   NPCS " + std::to_string(m_npcs.size());
    renderer.DrawScreenText(status, size, y, size, kHudColor);

    const std::string hint = "N - SPAWN NPC";
    renderer.DrawScreenText(hint, w - renderer.MeasureScreenText(hint, size) - size, y, size,
                            kHudHintColor);
}
