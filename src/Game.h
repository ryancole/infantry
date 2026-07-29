#pragma once

#include "Camera.h"
#include "Input.h"
#include "Physics.h"
#include "Renderer.h"

#include <DirectXMath.h>
#include <memory>
#include <vector>

// Prototype gameplay: one soldier on a grid arena, screen-relative WASD
// movement, mouse aim, and projectiles. This is the seed for Infantry-style
// systems (vehicles, weapons, teams, net play) to grow from.
class Game
{
public:
    Game();

    // Loads GPU assets; call once after Renderer::Init.
    void LoadContent(Renderer& renderer);

    void Update(float dt, const Input& input, IsoCamera& camera);
    void Render(Renderer& renderer);

private:
    struct Projectile
    {
        Physics::BodyHandle body;
        float life;
    };

    struct Obstacle
    {
        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT3 size;
    };

    struct TreeInstance
    {
        DirectX::XMFLOAT3 pos;
        float scale;
        float yaw;
    };

    void FireWeapon();

    Physics m_physics;
    DirectX::XMFLOAT3 m_playerPos = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 m_aimDir = { 1.0f, 0.0f, 0.0f };
    float m_fireCooldown = 0.0f;

    std::vector<Projectile> m_projectiles;
    std::vector<Obstacle> m_obstacles;
    std::vector<TreeInstance> m_trees;
    std::unique_ptr<Model> m_treeModel;
    std::vector<Vertex> m_gridVerts;   // static, built once
    std::vector<Vertex> m_scratch;     // reused per draw
};
