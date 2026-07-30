#pragma once

#include "Camera.h"
#include "ClassSelect.h"
#include "Input.h"
#include "Physics.h"
#include "PlayerClass.h"
#include "Renderer.h"
#include "Sound.h"
#include "Visibility.h"

#include <DirectXMath.h>
#include <memory>
#include <random>
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
        DirectX::XMFLOAT3 prevPos; // last frame's position, for swept hit tests
        int team;                  // whose shot this is; it only hurts the other side
        float damage;
        float radius;
    };

    // A computer-controlled enemy soldier. NPCs share the player's class
    // table, so every weapon can be tested from both ends: fired at them,
    // and dodged when they fire it back.
    struct Npc
    {
        const ClassDef* cls;
        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT3 aimDir;
        float hp;
        float fireCooldown;
        DirectX::XMFLOAT3 wanderTarget;
        float repickTimer; // forces a fresh wander target even when stuck
        float strafeSign;  // +1/-1: which way to circle while engaged
        float strafeTimer; // time until the strafe direction flips
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

    void SpawnShot(const ClassDef& cls, const DirectX::XMFLOAT3& from,
                   const DirectX::XMFLOAT3& dir, int team);
    void SpawnNpc();
    void UpdateNpcs(float dt);
    void UpdateProjectiles(float dt);
    // Clamps pos to the arena and pushes it out of solid colliders.
    void ResolveObstacles(DirectX::XMFLOAT3& pos) const;
    // One-shot SFX attenuated by distance from the player's ear.
    void PlaySoundAt(const std::string& name, const DirectX::XMFLOAT3& pos, float pitch = 0.0f);
    float Rand(float lo, float hi);
    void AppendFog(std::vector<Vertex>& out) const;
    void RenderHud(Renderer& renderer);

    static constexpr float kMaxHealth = 100.0f;

    Physics m_physics;
    Sound m_sound;
    Phase m_phase = Phase::ClassSelect;
    ClassSelect m_classSelect;
    const ClassDef* m_class = nullptr; // set when the player picks; never null while Playing
    float m_arenaHalf = 32.0f;
    // Team spawn points from the level, indexed by team id. The local player
    // is on m_team; a team-select flow can set it before spawning, and future
    // respawn/teammate logic reads from the same table.
    std::vector<DirectX::XMFLOAT3> m_teamSpawns;
    int m_team = 0;
    DirectX::XMFLOAT3 m_playerPos = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 m_aimDir = { 1.0f, 0.0f, 0.0f };
    float m_fireCooldown = 0.0f;
    float m_playerHp = kMaxHealth;
    bool m_playerDied = false; // set by projectile hits, handled in Update

    std::vector<Npc> m_npcs;
    int m_nextNpcClass = 0; // spawns cycle through the class table
    std::mt19937 m_rng{ std::random_device{}() };

    std::vector<Projectile> m_projectiles;
    std::vector<Collider> m_colliders;
    std::vector<Visibility::Rect> m_occluders; // footprints of sight-blockers
    std::vector<Vertex> m_fogVerts; // reused per frame
    std::vector<Prop> m_props;
    std::unordered_map<std::string, std::unique_ptr<Model>> m_models;
    std::vector<Vertex> m_gridVerts;   // static, built once
    std::vector<Vertex> m_scratch;     // reused per draw
    std::vector<Vertex> m_hudVerts;    // screen-space text, reused per frame
};
