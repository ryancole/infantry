#pragma once

#include "Camera.h"
#include "ClassSelect.h"
#include "Input.h"
#include "Physics.h"
#include "PlayerClass.h"
#include "Renderer.h"
#include "Soldier.h"
#include "Sound.h"
#include "Visibility.h"

#include <DirectXMath.h>
#include <SimpleMath.h>
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

    // Stops the game's hold on anything that outlives a frame; call once the
    // loop is done, before the window and devices go away.
    void Shutdown();

private:
    // Players must pick a class before they spawn; the arena only starts
    // simulating once the choice is made. Dying drops back out of Playing for
    // the respawn wait: the arena keeps running, the player just isn't in it.
    enum class Phase
    {
        ClassSelect,
        Playing,
        Dead,
    };

    using Vector2 = DirectX::SimpleMath::Vector2;
    using Vector3 = DirectX::SimpleMath::Vector3;

    struct Projectile
    {
        Physics::BodyHandle body;
        float life;
        Vector3 prevPos; // last frame's position, for swept hit tests
        int team;        // whose shot this is; it only hurts the other side
        float damage;
        float radius;
        float blastRadius; // > 0: splash damage on impact
        bool fused;      // rides out `life` bouncing off the world instead of
                         // dying on its first contact; the fuse detonates it
        bool explodes;   // grenade: impact gets the big fireball, not a puff
    };

    // A short-lived debris cube from a projectile impact: flies out under
    // gravity and shrinks to nothing over its lifetime.
    struct Particle
    {
        Vector3 pos;
        Vector3 vel;
        float life;    // seconds remaining
        float maxLife; // spawn value of life, for the shrink fraction
        float size;    // edge length at full life
        DirectX::XMFLOAT4 color;
    };

    // A computer-controlled enemy soldier. NPCs share the player's class
    // table, so every weapon can be tested from both ends: fired at them,
    // and dodged when they fire it back.
    struct Npc
    {
        const ClassDef* cls;
        Vector3 pos;
        Vector3 aimDir;
        float hp;
        float fireCooldown;
        Vector3 wanderTarget;
        float repickTimer; // forces a fresh wander target even when stuck
        float strafeSign;  // +1/-1: which way to circle while engaged
        float strafeTimer; // time until the strafe direction flips
        float walkPhase;   // leg-swing angle for the soldier model, advances with distance
        float moveBlend;   // 0..1 walk-pose weight, eases in/out so stops don't snap
        Vector3 knock;     // launch velocity the last hit would give its corpse
    };

    // What's left of a soldier: the model's segments handed to the physics
    // world as a jointed ragdoll, keeping the pose it died in and then falling
    // out of it. Corpses are decoration — they take no damage, block nothing,
    // and collide with the level alone — so all the game keeps is what it needs
    // to draw them and, once they've lain around long enough, to clear them.
    struct Corpse
    {
        Physics::BodyHandle parts[Soldier::SegmentCount];
        DirectX::XMFLOAT4 color;
        float life; // seconds until it's cleaned up
    };

    // Runtime halves of a level object: solid objects contribute a Collider
    // (physics + player push-out), visible ones a Prop. Collider-only objects
    // are blockout geometry, drawn as debug cubes until they get a model.
    struct Collider
    {
        Vector3 center;
        Vector3 size;
        bool debugDraw; // no model to represent it, so draw the box itself
    };

    struct Prop
    {
        const Model* model; // owned by m_models
        Vector3 pos;
        float scale;
        float yaw;
    };

    // Fires `weapon`'s projectile from `from` along `dir`. Bullets always fly
    // at full speed; lobbed shots (grenades) shorten their toss to come down
    // `targetDist` away, up to the weapon's max range.
    void SpawnShot(const WeaponDef& weapon, const Vector3& from, const Vector3& dir, int team,
                   float targetDist);
    void SpawnNpc();
    void UpdateNpcs(float dt);
    void UpdateProjectiles(float dt);
    // Debris burst where a projectile dies; `scale` is the projectile radius.
    void SpawnImpactBurst(const Vector3& pos, float scale);
    // Grenade detonation: core flash plus a wide fire/smoke burst.
    void SpawnExplosion(const Vector3& pos);
    // Ends `shot` at `pos`: splash damage for explosives, then the impact
    // effect and sound (which differ for a hit on `hitUnit` vs. world geometry).
    void Detonate(const Projectile& shot, const Vector3& pos, bool hitUnit);
    // Splash damage around `center`, full strength at the middle and falling
    // off to nothing at `radius`. Only the side opposing `team` is hurt, and
    // only where the blast has line of sight, so cover still protects.
    void ApplyBlast(const Vector3& center, float radius, float damage, int team);
    void UpdateParticles(float dt);
    // Leaves a ragdoll where a soldier stood, built from the pose it died in
    // and launched with `knock` so it falls away from the killing blow. Past
    // the corpse cap the oldest body on the field is recycled.
    void SpawnCorpse(const Vector3& pos, const Vector3& aimDir, float walkPhase, float moveBlend,
                     const DirectX::XMFLOAT4& color, const Vector3& knock);
    // Puts the player back on their team's spawn with a fresh loadout and
    // returns to Phase::Playing; the camera cuts rather than sweeps across.
    void Respawn(IsoCamera& camera);
    // Whether there's a live player body to shoot at. Dying takes the player
    // out of the world for the respawn wait, so bullets, blasts and NPC AI
    // all have nothing to aim at until they're back.
    bool PlayerOnField() const;
    void UpdateCorpses(float dt);
    void RemoveCorpse(size_t index);
    // Marches the ballistic arc SpawnShot would fire (aimed targetDist away)
    // against the colliders and the ground; returns the horizontal distance
    // from `from` at which the shot stops. When outArc is given, fills it
    // with the sampled trajectory (muzzle to stop). Powers the aim indicator.
    float PredictShotStop(const WeaponDef& weapon, const Vector3& from, const Vector3& dir,
                          float targetDist, std::vector<Vector3>* outArc = nullptr) const;
    // Clamps pos to the arena and pushes it out of solid colliders.
    void ResolveObstacles(Vector3& pos) const;
    // Positional one-shot SFX: panned and attenuated relative to the player.
    void PlaySoundAt(const std::string& name, const Vector3& pos, float pitch = 0.0f);
    float Rand(float lo, float hi);
    void AppendFog(std::vector<Vertex>& out) const;
    void RenderHud(Renderer& renderer);

    static constexpr float kMaxHealth = 100.0f;
    // Grenades are issued per life, not recharged: spend it and there isn't
    // another until you respawn, which is what makes the throw a decision.
    static constexpr int kGrenadesPerLife = 1;
    // Dying costs time as well as position: long enough that a death is felt,
    // short enough that watching it out isn't the game. A placeholder value
    // until there's something to tune it against (round length, ticket bleed).
    static constexpr float kRespawnDelay = 5.0f;

    Physics m_physics;
    Sound m_sound;
    Phase m_phase = Phase::ClassSelect;
    ClassSelect m_classSelect;
    const ClassDef* m_class = nullptr; // set when the player picks; never null while Playing
    float m_arenaHalf = 32.0f;
    // Team spawn points from the level, indexed by team id. The local player
    // is on m_team; a team-select flow can set it before spawning, and future
    // respawn/teammate logic reads from the same table.
    std::vector<Vector3> m_teamSpawns;
    int m_team = 0;
    Vector3 m_playerPos;
    Vector3 m_aimDir = Vector3::UnitX;
    float m_aimDist = 1e9f; // distance to the cursor's ground point; huge = unaimed (stick)
    float m_fireCooldown = 0.0f;
    int m_grenades = kGrenadesPerLife; // same issue for every class; refilled on respawn
    float m_walkPhase = 0.0f; // same walk-cycle bookkeeping as Npc::walkPhase
    float m_moveBlend = 0.0f;
    float m_playerHp = kMaxHealth;
    float m_frameMs = 0.0f;        // smoothed wall-clock frame time, for the HUD
    float m_rumbleTime = 0.0f;     // gamepad vibration left on a damage pulse
    float m_deathFlashTime = 0.0f; // grayscale post flash after dying
    float m_respawnTimer = 0.0f;   // seconds left of the wait, while Phase::Dead
    bool m_playerDied = false;     // set by projectile hits, handled in Update
    Vector3 m_deathKnock;          // launch velocity for the player's corpse, from the last hit

    std::vector<Npc> m_npcs;
    int m_nextNpcClass = 0; // spawns cycle through the class table
    std::mt19937 m_rng{ std::random_device{}() };

    std::vector<Projectile> m_projectiles;
    std::vector<Particle> m_particles;
    std::vector<Corpse> m_corpses; // oldest first, so the cap sheds the stalest body
    std::vector<Collider> m_colliders;
    std::vector<Visibility::Rect> m_occluders; // footprints of sight-blockers
    std::vector<Vertex> m_fogVerts; // reused per frame
    std::vector<Prop> m_props;
    std::unordered_map<std::string, std::unique_ptr<Model>> m_models;
    std::vector<Vertex> m_gridVerts;   // static, built once
    std::vector<Vertex> m_scratch;     // reused per draw
    std::vector<Vector3> m_aimArc;     // aim indicator trajectory, reused per frame
};
