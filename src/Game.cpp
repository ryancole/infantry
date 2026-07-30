#include "Game.h"

#include "DebugText.h"
#include "Level.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace DirectX;

namespace
{
    constexpr float kPlayerHalf = 0.4f;
    constexpr float kMuzzleHeight = 0.6f;

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
    constexpr XMFLOAT4 kAimColor = { 0.95f, 0.95f, 0.40f, 1.0f };
    constexpr XMFLOAT4 kFogColor = { 0.01f, 0.02f, 0.04f, 0.85f };
    constexpr XMFLOAT4 kHudColor = { 0.85f, 0.90f, 0.95f, 1.0f };
    constexpr XMFLOAT4 kHudHintColor = { 0.45f, 0.52f, 0.62f, 1.0f };

    // Appends a solid cube with per-face shading (fakes lighting until a real
    // lit pipeline exists).
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

    // Arena floor. Projectiles are dynamic bodies, so gravity, bounces, and
    // ricochets come from Jolt.
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
    const XMFLOAT3 upG = camera.ScreenUpOnGround();
    const XMFLOAT3 rightG = camera.ScreenRightOnGround();

    float moveUp = 0.0f, moveRight = 0.0f;
    if (input.Key('W')) moveUp += 1.0f;
    if (input.Key('S')) moveUp -= 1.0f;
    if (input.Key('D')) moveRight += 1.0f;
    if (input.Key('A')) moveRight -= 1.0f;

    float dx = upG.x * moveUp + rightG.x * moveRight;
    float dz = upG.z * moveUp + rightG.z * moveRight;
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len > 1e-5f)
    {
        dx /= len;
        dz /= len;
        m_playerPos.x += dx * m_class->moveSpeed * dt;
        m_playerPos.z += dz * m_class->moveSpeed * dt;
    }
    ResolveObstacles(m_playerPos);

    // --- Aim: mouse cursor projected onto the ground plane ---
    const XMFLOAT3 aimPoint = camera.ScreenToGround(input.mouseX, input.mouseY);
    float ax = aimPoint.x - m_playerPos.x;
    float az = aimPoint.z - m_playerPos.z;
    const float alen = std::sqrt(ax * ax + az * az);
    if (alen > 1e-4f)
        m_aimDir = { ax / alen, 0.0f, az / alen };

    // --- Firing ---
    m_fireCooldown -= dt;
    if ((input.mouseDown[0] || input.MousePressed(0) || input.Key(VK_SPACE)) &&
        m_fireCooldown <= 0.0f)
    {
        SpawnShot(*m_class, m_playerPos, m_aimDir, m_team);
        m_fireCooldown = m_class->fireInterval;
    }

    // --- NPCs: debug spawning and AI ---
    if (input.KeyPressed('N'))
        SpawnNpc();
    UpdateNpcs(dt);

    // --- Projectiles (simulated by Jolt) ---
    m_physics.Step(dt);
    UpdateProjectiles(dt);

    if (m_playerDied)
    {
        m_playerDied = false;
        m_playerHp = kMaxHealth;
        m_playerPos = m_teamSpawns[m_team];
        camera.SetTarget(m_playerPos);
        camera.SnapToTarget();
        return;
    }

    camera.SetTarget(m_playerPos);
}

void Game::SpawnShot(const ClassDef& cls, const XMFLOAT3& from, const XMFLOAT3& dir, int team)
{
    const XMFLOAT3 pos = { from.x + dir.x * 0.7f, kMuzzleHeight, from.z + dir.z * 0.7f };
    // lobVelocity arcs the shot (grenades); flat-shooting classes fire level.
    const XMFLOAT3 vel = { dir.x * cls.projectileSpeed, cls.lobVelocity,
                           dir.z * cls.projectileSpeed };
    m_projectiles.push_back({ m_physics.SpawnProjectile(pos, vel, cls.projectileRadius,
                                                        cls.projectileMass),
                              cls.projectileLife, pos, team, cls.damage, cls.projectileRadius });
}

float Game::Rand(float lo, float hi)
{
    return std::uniform_real_distribution<float>(lo, hi)(m_rng);
}

void Game::ResolveObstacles(XMFLOAT3& pos) const
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
    const XMFLOAT2 playerXZ = { m_playerPos.x, m_playerPos.z };
    for (Npc& npc : m_npcs)
    {
        npc.fireCooldown -= dt;

        float toPx = m_playerPos.x - npc.pos.x;
        float toPz = m_playerPos.z - npc.pos.z;
        const float dist = std::sqrt(toPx * toPx + toPz * toPz);
        // NPCs obey the same sight rules as the player's fog of war, so they
        // can't shoot through walls the player can't see through either.
        const bool engaged = dist < kNpcEngageRange && dist > 1e-3f &&
                             Visibility::IsPointVisible({ npc.pos.x, npc.pos.z }, playerXZ,
                                                        m_occluders);

        float moveX = 0.0f, moveZ = 0.0f;
        float speed = npc.cls->moveSpeed;
        if (engaged)
        {
            toPx /= dist;
            toPz /= dist;
            npc.aimDir = { toPx, 0.0f, toPz };

            npc.strafeTimer -= dt;
            if (npc.strafeTimer <= 0.0f)
            {
                npc.strafeSign = -npc.strafeSign;
                npc.strafeTimer = Rand(1.0f, 2.5f);
            }
            if (dist > kNpcPreferredRange)
            {
                moveX = toPx;
                moveZ = toPz;
            }
            else
            {
                moveX = -toPz * npc.strafeSign;
                moveZ = toPx * npc.strafeSign;
            }
            speed *= kNpcCombatSpeed;

            if (npc.fireCooldown <= 0.0f)
            {
                // A touch of angular spread keeps NPCs beatable up close and
                // makes long-range sniper duels survivable.
                const float jitter = Rand(-kNpcAimJitter, kNpcAimJitter);
                const float s = std::sin(jitter), c = std::cos(jitter);
                const XMFLOAT3 dir = { npc.aimDir.x * c - npc.aimDir.z * s, 0.0f,
                                       npc.aimDir.x * s + npc.aimDir.z * c };
                const int npcTeam = (m_team + 1) % static_cast<int>(m_teamSpawns.size());
                SpawnShot(*npc.cls, npc.pos, dir, npcTeam);
                npc.fireCooldown = npc.cls->fireInterval;
            }
        }
        else
        {
            npc.repickTimer -= dt;
            const float wx = npc.wanderTarget.x - npc.pos.x;
            const float wz = npc.wanderTarget.z - npc.pos.z;
            const float wlen = std::sqrt(wx * wx + wz * wz);
            if (wlen < 1.0f || npc.repickTimer <= 0.0f)
            {
                const float margin = m_arenaHalf - 2.0f;
                npc.wanderTarget = { Rand(-margin, margin), 0.0f, Rand(-margin, margin) };
                npc.repickTimer = Rand(4.0f, 8.0f);
            }
            else
            {
                moveX = wx / wlen;
                moveZ = wz / wlen;
                npc.aimDir = { moveX, 0.0f, moveZ };
            }
            speed *= kNpcWanderSpeed;
        }

        npc.pos.x += moveX * speed * dt;
        npc.pos.z += moveZ * speed * dt;
        ResolveObstacles(npc.pos);
    }

    // Pairwise separation so a group never collapses into a single column.
    for (size_t i = 0; i < m_npcs.size(); ++i)
        for (size_t j = i + 1; j < m_npcs.size(); ++j)
        {
            const float dx = m_npcs[j].pos.x - m_npcs[i].pos.x;
            const float dz = m_npcs[j].pos.z - m_npcs[i].pos.z;
            const float len = std::sqrt(dx * dx + dz * dz);
            const float minDist = kPlayerHalf * 2.0f;
            if (len >= minDist || len < 1e-5f)
                continue;
            const float push = (minDist - len) * 0.5f;
            const float nx = dx / len, nz = dz / len;
            m_npcs[i].pos.x -= nx * push;
            m_npcs[i].pos.z -= nz * push;
            m_npcs[j].pos.x += nx * push;
            m_npcs[j].pos.z += nz * push;
        }
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
                    shot.life = 0.0f;
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
                if (m_playerHp <= 0.0f)
                    m_playerDied = true;
                shot.life = 0.0f;
            }
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
    const XMFLOAT2 viewer = { m_playerPos.x, m_playerPos.z };
    const std::vector<XMFLOAT2> poly =
        Visibility::ComputePolygon(viewer, m_occluders, m_arenaHalf);
    if (poly.size() < 2)
        return;

    const float farDist = m_arenaHalf * kFogFar;
    const auto extrude = [&](const XMFLOAT2& v) -> XMFLOAT2 {
        const float dx = v.x - viewer.x;
        const float dz = v.y - viewer.y;
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len < 1e-5f)
            return v;
        return { viewer.x + dx / len * farDist, viewer.y + dz / len * farDist };
    };

    for (size_t i = 0; i < poly.size(); ++i)
    {
        const XMFLOAT2& a = poly[i];
        const XMFLOAT2& b = poly[(i + 1) % poly.size()];
        const XMFLOAT2 af = extrude(a);
        const XMFLOAT2 bf = extrude(b);
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
    if (m_phase == Phase::ClassSelect)
    {
        m_classSelect.Render(renderer);
        return;
    }

    const XMMATRIX identity = XMMatrixIdentity();

    renderer.DrawLines(m_gridVerts.data(), static_cast<uint32_t>(m_gridVerts.size()), identity);

    m_scratch.clear();
    for (const Collider& c : m_colliders)
        if (c.debugDraw)
            AppendCube(m_scratch, c.center, c.size, kObstacleColor);

    // Player body plus a small "turret" cap, tinted by class.
    const XMFLOAT3 body = { m_playerPos.x, kPlayerHalf, m_playerPos.z };
    AppendCube(m_scratch, body, { kPlayerHalf * 2.0f, kPlayerHalf * 2.0f, kPlayerHalf * 2.0f },
               m_class->color);
    AppendCube(m_scratch, { body.x, kPlayerHalf * 2.2f, body.z },
               { 0.35f, 0.35f, 0.35f }, m_class->color);

    // NPCs and projectiles the player can't see stay hidden — an enemy behind
    // a wall disappears until it re-emerges.
    const XMFLOAT2 eye = { m_playerPos.x, m_playerPos.z };
    for (const Npc& npc : m_npcs)
    {
        if (!Visibility::IsPointVisible(eye, { npc.pos.x, npc.pos.z }, m_occluders))
            continue;
        const XMFLOAT3 nb = { npc.pos.x, kPlayerHalf, npc.pos.z };
        AppendCube(m_scratch, nb,
                   { kPlayerHalf * 2.0f, kPlayerHalf * 2.0f, kPlayerHalf * 2.0f },
                   npc.cls->color);
        AppendCube(m_scratch, { nb.x, kPlayerHalf * 2.2f, nb.z }, { 0.35f, 0.35f, 0.35f },
                   npc.cls->color);

        // Health bar: a thin slab that shrinks toward its left edge and shifts
        // green -> red, so weapon damage is readable per hit.
        const float frac = std::max(npc.hp, 0.0f) / kMaxHealth;
        const float barW = 0.9f * frac;
        const XMFLOAT4 barColor = { 0.9f * (1.0f - frac), 0.8f * frac, 0.1f, 1.0f };
        AppendCube(m_scratch, { nb.x - (0.9f - barW) * 0.5f, kPlayerHalf * 3.0f, nb.z },
                   { barW, 0.08f, 0.08f }, barColor);
    }
    for (const Projectile& shot : m_projectiles)
    {
        const XMFLOAT3 pos = m_physics.GetPosition(shot.body);
        if (Visibility::IsPointVisible(eye, { pos.x, pos.z }, m_occluders))
        {
            const float d = m_class->projectileRadius * 2.0f;
            AppendCube(m_scratch, pos, { d, d, d }, kProjectileColor);
        }
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

    // Aim indicator line.
    const Vertex aimLine[2] = {
        { XMFLOAT3{ body.x + m_aimDir.x * 0.6f, 0.45f, body.z + m_aimDir.z * 0.6f }, kAimColor },
        { XMFLOAT3{ body.x + m_aimDir.x * 1.6f, 0.45f, body.z + m_aimDir.z * 1.6f }, kAimColor },
    };
    renderer.DrawLines(aimLine, 2, identity);

    RenderHud(renderer);
}

// Screen-space overlay: player health, NPC count, and the spawn hint. Uses
// its own ortho projection; main.cpp restores the world view-proj next frame.
void Game::RenderHud(Renderer& renderer)
{
    const float w = static_cast<float>(renderer.Width());
    const float h = static_cast<float>(renderer.Height());
    renderer.SetViewProj(XMMatrixOrthographicOffCenterLH(0.0f, w, h, 0.0f, 0.0f, 1.0f));

    m_hudVerts.clear();
    const float size = h * 0.024f;
    const float y = h - size * 2.0f;
    const std::string status = "HP " + std::to_string(static_cast<int>(std::ceil(m_playerHp))) +
                               "   NPCS " + std::to_string(m_npcs.size());
    DebugText::Append(m_hudVerts, status, size, y, size, kHudColor);

    const std::string hint = "N - SPAWN NPC";
    DebugText::Append(m_hudVerts, hint, w - DebugText::Measure(hint, size) - size, y, size,
                      kHudHintColor);
    renderer.DrawLines(m_hudVerts.data(), static_cast<uint32_t>(m_hudVerts.size()),
                       XMMatrixIdentity());
}
