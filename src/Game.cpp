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
    constexpr float kProjectileLife = 1.6f;
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
        m_gridVerts.push_back({ { f, 0.0f, -kArenaHalf }, col });
        m_gridVerts.push_back({ { f, 0.0f, kArenaHalf }, col });
        m_gridVerts.push_back({ { -kArenaHalf, 0.0f, f }, col });
        m_gridVerts.push_back({ { kArenaHalf, 0.0f, f }, col });
    }

    // A few bunkers to give the arena some structure.
    m_obstacles = {
        { { -10.0f, 1.0f, -6.0f }, { 4.0f, 2.0f, 2.0f } },
        { { 8.0f, 1.0f, 10.0f }, { 2.0f, 2.0f, 6.0f } },
        { { 14.0f, 0.75f, -12.0f }, { 3.0f, 1.5f, 3.0f } },
        { { -18.0f, 1.25f, 14.0f }, { 5.0f, 2.5f, 2.5f } },
        { { 0.0f, 1.0f, -20.0f }, { 8.0f, 2.0f, 2.0f } },
    };
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

    // --- Projectiles ---
    for (auto& shot : m_projectiles)
    {
        shot.pos.x += shot.vel.x * dt;
        shot.pos.z += shot.vel.z * dt;
        shot.life -= dt;
        if (shot.pos.x < -kArenaHalf || shot.pos.x > kArenaHalf ||
            shot.pos.z < -kArenaHalf || shot.pos.z > kArenaHalf)
            shot.life = 0.0f;
    }
    std::erase_if(m_projectiles, [](const Projectile& s) { return s.life <= 0.0f; });

    camera.SetTarget(m_playerPos);
}

void Game::FireWeapon()
{
    Projectile shot;
    shot.pos = { m_playerPos.x + m_aimDir.x * 0.7f, 0.5f, m_playerPos.z + m_aimDir.z * 0.7f };
    shot.vel = { m_aimDir.x * kProjectileSpeed, 0.0f, m_aimDir.z * kProjectileSpeed };
    shot.life = kProjectileLife;
    m_projectiles.push_back(shot);
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
        AppendCube(m_scratch, shot.pos, { 0.22f, 0.22f, 0.22f }, kProjectileColor);

    renderer.DrawTriangles(m_scratch.data(), static_cast<uint32_t>(m_scratch.size()), identity);

    // Aim indicator line.
    const Vertex aimLine[2] = {
        { { body.x + m_aimDir.x * 0.6f, 0.45f, body.z + m_aimDir.z * 0.6f }, kAimColor },
        { { body.x + m_aimDir.x * 1.6f, 0.45f, body.z + m_aimDir.z * 1.6f }, kAimColor },
    };
    renderer.DrawLines(aimLine, 2, identity);
}
