#pragma once

#include "Camera.h"
#include "ClassSelect.h"
#include "Input.h"
#include "Physics.h"
#include "PlayerClass.h"
#include "Renderer.h"
#include "Visibility.h"

#include <DirectXMath.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Prototype gameplay: one soldier on a grid arena, screen-relative WASD
// movement, mouse aim, and projectiles. This is the seed for Infantry-style
// systems (vehicles, weapons, teams, net play) to grow from.
class Game
{
public:
    // Loads the level (assets/levels/) and GPU assets; call once after
    // Renderer::Init. Throws std::runtime_error on a bad level or model.
    void LoadContent(Renderer& renderer);

    void Update(float dt, const Input& input, IsoCamera& camera);
    void Render(Renderer& renderer);

private:
    // Players must pick a class before they spawn; the arena only starts
    // simulating once the choice is made.
    enum class Phase
    {
        ClassSelect,
        Playing,
    };

    struct Projectile
    {
        Physics::BodyHandle body;
        float life;
    };

    // Runtime halves of a level object: solid objects contribute a Collider
    // (physics + player push-out), visible ones a Prop. Collider-only objects
    // are blockout geometry, drawn as debug cubes until they get a model.
    struct Collider
    {
        DirectX::XMFLOAT3 center;
        DirectX::XMFLOAT3 size;
        bool debugDraw; // no model to represent it, so draw the box itself
    };

    struct Prop
    {
        const Model* model; // owned by m_models
        DirectX::XMFLOAT3 pos;
        float scale;
        float yaw;
    };

    void FireWeapon();
    void AppendFog(std::vector<Vertex>& out) const;

    Physics m_physics;
    Phase m_phase = Phase::ClassSelect;
    ClassSelect m_classSelect;
    const ClassDef* m_class = nullptr; // set when the player picks; never null while Playing
    float m_arenaHalf = 32.0f;
    DirectX::XMFLOAT3 m_playerPos = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 m_aimDir = { 1.0f, 0.0f, 0.0f };
    float m_fireCooldown = 0.0f;

    std::vector<Projectile> m_projectiles;
    std::vector<Collider> m_colliders;
    std::vector<Visibility::Rect> m_occluders; // footprints of sight-blockers
    std::vector<Vertex> m_fogVerts; // reused per frame
    std::vector<Prop> m_props;
    std::unordered_map<std::string, std::unique_ptr<Model>> m_models;
    std::vector<Vertex> m_gridVerts;   // static, built once
    std::vector<Vertex> m_scratch;     // reused per draw
};
