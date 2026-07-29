#include "Game.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace
{
    constexpr float kArenaHalf = 32.0f;      // arena spans [-32, 32] on x and z
    constexpr float kPlayerSpeed = 9.0f;
    constexpr float kPlayerHalf = 0.4f;
    constexpr float kProjectileSpeed = 34.0f;
    constexpr float kProjectileLife = 3.0f;
    constexpr float kProjectileRadius = 0.11f;
    constexpr float kProjectileMass = 0.4f;
    constexpr float kMuzzleHeight = 0.6f;
    constexpr float kFireInterval = 0.12f;

    constexpr XMFLOAT4 kGridMinor = { 0.10f, 0.13f, 0.17f, 1.0f };
    constexpr XMFLOAT4 kGridMajor = { 0.17f, 0.22f, 0.29f, 1.0f };
    constexpr XMFLOAT4 kBorder = { 0.55f, 0.25f, 0.20f, 1.0f };
    constexpr XMFLOAT4 kPlayerColor = { 0.25f, 0.85f, 0.35f, 1.0f };
    constexpr XMFLOAT4 kObstacleColor = { 0.35f, 0.40f, 0.50f, 1.0f };
    constexpr XMFLOAT4 kProjectileColor = { 1.00f, 0.80f, 0.20f, 1.0f };
    constexpr XMFLOAT4 kAimColor = { 0.95f, 0.95f, 0.40f, 1.0f };

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
}

Game::Game()
{
    // Static floor grid.
    const int half = static_cast<int>(kArenaHalf);
    for (int i = -half; i <= half; ++i)
    {
        const bool border = (i == -half || i == half);
        const bool major = (i % 8) == 0;
        const XMFLOAT4 col = border ? kBorder : (major ? kGridMajor : kGridMinor);
        const float f = static_cast<float>(i);
        m_gridVerts.push_back({ XMFLOAT3{ f, 0.0f, -kArenaHalf }, col });
        m_gridVerts.push_back({ XMFLOAT3{ f, 0.0f, kArenaHalf }, col });
        m_gridVerts.push_back({ XMFLOAT3{ -kArenaHalf, 0.0f, f }, col });
        m_gridVerts.push_back({ XMFLOAT3{ kArenaHalf, 0.0f, f }, col });
    }

    // A few bunkers to give the arena some structure.
    m_obstacles = {
        { { -10.0f, 1.0f, -6.0f }, { 4.0f, 2.0f, 2.0f } },
        { { 8.0f, 1.0f, 10.0f }, { 2.0f, 2.0f, 6.0f } },
        { { 14.0f, 0.75f, -12.0f }, { 3.0f, 1.5f, 3.0f } },
        { { -18.0f, 1.25f, 14.0f }, { 5.0f, 2.5f, 2.5f } },
        { { 0.0f, 1.0f, -20.0f }, { 8.0f, 2.0f, 2.0f } },
    };

    // Scattered trees (kept clear of bunkers and the spawn point), with a
    // little deterministic variety in size and facing.
    m_trees = {
        { { -24.0f, 0.0f, -20.0f }, 1.20f, 0.4f },
        { { 20.0f, 0.0f, -24.0f }, 0.95f, 2.1f },
        { { 25.0f, 0.0f, 17.0f }, 1.35f, 4.8f },
        { { -8.0f, 0.0f, 24.0f }, 1.05f, 1.3f },
        { { -26.0f, 0.0f, 8.0f }, 0.85f, 3.6f },
        { { 15.0f, 0.0f, 5.0f }, 1.10f, 5.5f },
        { { 6.0f, 0.0f, -13.0f }, 0.90f, 0.9f },
        { { -14.0f, 0.0f, -26.0f }, 1.25f, 2.8f },
    };

    // Static collision: the arena floor plus the bunkers. Projectiles are
    // dynamic bodies, so gravity, bounces, and ricochets come from Jolt.
    m_physics.AddStaticBox({ 0.0f, -0.5f, 0.0f },
                           { kArenaHalf * 2.0f, 1.0f, kArenaHalf * 2.0f });
    for (const Obstacle& ob : m_obstacles)
        m_physics.AddStaticBox(ob.pos, ob.size);
}

void Game::LoadContent(Renderer& renderer)
{
    m_treeModel = renderer.LoadModel("assets\\tree.glb");
}

void Game::Update(float dt, const Input& input, IsoCamera& camera)
{
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
        m_playerPos.x += dx * kPlayerSpeed * dt;
        m_playerPos.z += dz * kPlayerSpeed * dt;
    }
    const float limit = kArenaHalf - kPlayerHalf;
    m_playerPos.x = std::clamp(m_playerPos.x, -limit, limit);
    m_playerPos.z = std::clamp(m_playerPos.z, -limit, limit);

    // Keep the player out of the bunkers: push out along the axis of least
    // penetration. (Kinematic on purpose — movement should stay crisp, so the
    // player doesn't live in the physics world yet.)
    for (const Obstacle& ob : m_obstacles)
    {
        const float ex = ob.size.x * 0.5f + kPlayerHalf;
        const float ez = ob.size.z * 0.5f + kPlayerHalf;
        const float px = m_playerPos.x - ob.pos.x;
        const float pz = m_playerPos.z - ob.pos.z;
        if (std::abs(px) >= ex || std::abs(pz) >= ez)
            continue;
        const float pushX = (px > 0.0f) ? ex - px : -ex - px;
        const float pushZ = (pz > 0.0f) ? ez - pz : -ez - pz;
        if (std::abs(pushX) < std::abs(pushZ))
            m_playerPos.x += pushX;
        else
            m_playerPos.z += pushZ;
    }

    // --- Aim: mouse cursor projected onto the ground plane ---
    const XMFLOAT3 aimPoint = camera.ScreenToGround(input.mouseX, input.mouseY);
    float ax = aimPoint.x - m_playerPos.x;
    float az = aimPoint.z - m_playerPos.z;
    const float alen = std::sqrt(ax * ax + az * az);
    if (alen > 1e-4f)
        m_aimDir = { ax / alen, 0.0f, az / alen };

    // --- Firing ---
    m_fireCooldown -= dt;
    if ((input.mouseDown[0] || input.Key(VK_SPACE)) && m_fireCooldown <= 0.0f)
    {
        FireWeapon();
        m_fireCooldown = kFireInterval;
    }

    // --- Projectiles (simulated by Jolt) ---
    m_physics.Step(dt);

    for (auto& shot : m_projectiles)
    {
        shot.life -= dt;
        const XMFLOAT3 pos = m_physics.GetPosition(shot.body);
        if (pos.x < -kArenaHalf || pos.x > kArenaHalf ||
            pos.z < -kArenaHalf || pos.z > kArenaHalf)
            shot.life = 0.0f;
    }
    for (const Projectile& shot : m_projectiles)
        if (shot.life <= 0.0f)
            m_physics.RemoveBody(shot.body);
    std::erase_if(m_projectiles, [](const Projectile& s) { return s.life <= 0.0f; });

    camera.SetTarget(m_playerPos);
}

void Game::FireWeapon()
{
    const XMFLOAT3 pos = { m_playerPos.x + m_aimDir.x * 0.7f, kMuzzleHeight,
                           m_playerPos.z + m_aimDir.z * 0.7f };
    const XMFLOAT3 vel = { m_aimDir.x * kProjectileSpeed, 0.0f, m_aimDir.z * kProjectileSpeed };
    m_projectiles.push_back({ m_physics.SpawnProjectile(pos, vel, kProjectileRadius,
                                                        kProjectileMass),
                              kProjectileLife });
}

void Game::Render(Renderer& renderer)
{
    const XMMATRIX identity = XMMatrixIdentity();

    renderer.DrawLines(m_gridVerts.data(), static_cast<uint32_t>(m_gridVerts.size()), identity);

    m_scratch.clear();
    for (const Obstacle& ob : m_obstacles)
        AppendCube(m_scratch, ob.pos, ob.size, kObstacleColor);

    // Player body plus a small "turret" cap.
    const XMFLOAT3 body = { m_playerPos.x, kPlayerHalf, m_playerPos.z };
    AppendCube(m_scratch, body, { kPlayerHalf * 2.0f, kPlayerHalf * 2.0f, kPlayerHalf * 2.0f },
               kPlayerColor);
    AppendCube(m_scratch, { body.x, kPlayerHalf * 2.2f, body.z },
               { 0.35f, 0.35f, 0.35f }, kPlayerColor);

    for (const Projectile& shot : m_projectiles)
        AppendCube(m_scratch, m_physics.GetPosition(shot.body),
                   { kProjectileRadius * 2.0f, kProjectileRadius * 2.0f, kProjectileRadius * 2.0f },
                   kProjectileColor);

    renderer.DrawTriangles(m_scratch.data(), static_cast<uint32_t>(m_scratch.size()), identity);

    if (m_treeModel)
    {
        for (const TreeInstance& tree : m_trees)
        {
            const XMMATRIX world = XMMatrixScaling(tree.scale, tree.scale, tree.scale) *
                                   XMMatrixRotationY(tree.yaw) *
                                   XMMatrixTranslation(tree.pos.x, tree.pos.y, tree.pos.z);
            renderer.DrawModel(*m_treeModel, world);
        }
    }

    // Aim indicator line.
    const Vertex aimLine[2] = {
        { XMFLOAT3{ body.x + m_aimDir.x * 0.6f, 0.45f, body.z + m_aimDir.z * 0.6f }, kAimColor },
        { XMFLOAT3{ body.x + m_aimDir.x * 1.6f, 0.45f, body.z + m_aimDir.z * 1.6f }, kAimColor },
    };
    renderer.DrawLines(aimLine, 2, identity);
}
