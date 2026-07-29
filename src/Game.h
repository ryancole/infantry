#pragma once

#include "Camera.h"
#include "Input.h"
#include "Renderer.h"

#include <DirectXMath.h>
#include <vector>

// Prototype gameplay: one soldier on a grid arena, screen-relative WASD
// movement, mouse aim, and projectiles. This is the seed for Infantry-style
// systems (vehicles, weapons, teams, net play) to grow from.
class Game
{
public:
    Game();

    void Update(float dt, const Input& input, IsoCamera& camera);
    void Render(Renderer& renderer);

private:
    struct Projectile
    {
        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT3 vel;
        float life;
    };

    struct Obstacle
    {
        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT3 size;
    };

    void FireWeapon();

    DirectX::XMFLOAT3 m_playerPos = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 m_aimDir = { 1.0f, 0.0f, 0.0f };
    float m_fireCooldown = 0.0f;

    std::vector<Projectile> m_projectiles;
    std::vector<Obstacle> m_obstacles;
    std::vector<Vertex> m_gridVerts;   // static, built once
    std::vector<Vertex> m_scratch;     // reused per draw
};
