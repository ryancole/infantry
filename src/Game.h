#pragma once

#include "Ability.h"
#include "Brain.h"
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
        // Blood: instead of settling and shrinking away, the drop is spent
        // the moment it touches the floor and leaves a stain behind (see
        // Game::SpawnSplat).
        bool blood = false;
    };

    // A computer-controlled soldier. NPCs share the player's class table, so
    // every weapon can be tested from both ends: fired at them, and dodged when
    // they fire it back.
    //
    // They are not all hostile. An NPC on the player's own team is a squadmate
    // — it fights the same enemies, soaks the same rounds, and is the thing a
    // medic's dressing has to have in front of it to be worth anything. Which
    // side an NPC is on is `team`, and every system that deals damage or picks
    // a target reads it rather than assuming, the way they all used to.
    struct Npc
    {
        const ClassDef* cls;
        int team;
        Vector3 pos;
        Vector3 aimDir;
        float hp;
        float fireCooldown;
        int ammo;          // rounds left in the magazine
        float reloadTimer; // > 0 while reloading, and unable to shoot back
        // Whatever this soldier's brain remembers between frames. Meaningless
        // out here on purpose: which mind it is comes off the class, and what
        // it keeps in there is that mind's business.
        Brain::Memory mind;
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
    // Starts the player's reload: empties the rest of the magazine into the
    // timer and plays the mag-out click. A no-op if one is already running or
    // the magazine is full, so the reload key can be leaned on harmlessly.
    void BeginReload();
    // Spends a melee charge on a swing through the arc in front of the player
    // and strikes the nearest enemy standing in it, if any. The charge goes
    // whether or not it lands.
    void SwingMelee();
    // What the player's ability is allowed to act on this frame: the player
    // themselves, and every living squadmate they can currently see. The sight
    // rule is applied here rather than inside the ability because the fog is
    // this class's business and an ability has never heard of a wall — which is
    // also what keeps "can a medic treat someone through cover" answered in
    // exactly one place. Rebuilt per call into m_abilityAllies, which is kept
    // around so the per-frame list costs no allocation.
    Ability::Scene AbilityScene();
    void SpawnNpc(int team);
    void UpdateNpcs(float dt);
    void UpdateProjectiles(float dt);
    // Turns whatever died this frame into a corpse and takes it off the
    // roster. Runs once, after every system that can deal damage has had its
    // turn, so it doesn't matter which of them landed the killing blow.
    void ReapDead();
    // Debris burst where a projectile dies; `scale` is the projectile radius.
    void SpawnImpactBurst(const Vector3& pos, float scale);
    // Grenade detonation: core flash plus a wide fire/smoke burst.
    void SpawnExplosion(const Vector3& pos);
    // Blood off a soldier hit at `pos`: a spray of drops thrown along `dir`
    // (the blow), sized by the damage done and thrown hardest by a fatal one.
    // Each drop stains the ground where it lands.
    void SpawnBlood(const Vector3& pos, const Vector3& dir, float damage, bool fatal);
    // Stains the floor under `pos` with a patch of blood. Splats never fade —
    // the ground keeps a record of the fight — so the field holds a fixed
    // number of them and the oldest is dropped once it's full.
    void SpawnSplat(const Vector3& pos);
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
    // The side the player isn't on. A two-team arena, so "the other team" is a
    // single answer rather than a list; when it isn't, this is the one place
    // that has to learn otherwise.
    int EnemyTeam() const
    {
        return (m_team + 1) % static_cast<int>(m_teamSpawns.size());
    }
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
    Vector3 m_moveVel;                 // current ground velocity; the keys steer it, they aren't it
    Vector3 m_aimDir = Vector3::UnitX;
    // Which way is right on screen, in world space, taken off the camera during
    // Update because Render isn't handed one. It's what stands a health bar
    // square to the view. Read a frame later than it's written, which costs
    // nothing: yaw is applied before Update runs, and nothing between there and
    // the draw can turn the camera.
    Vector3 m_screenRight = Vector3::UnitX;
    float m_aimDist = 1e9f; // distance to the cursor's ground point; huge = unaimed (stick)
    float m_fireCooldown = 0.0f;
    // The magazine, and the wait to refill it. A reload starts on the last
    // round automatically or on the reload key, and until it finishes the
    // trigger does nothing — no ammo pool behind it, so the only cost of a
    // reload is the time.
    int m_ammo = 0;
    float m_reloadTimer = 0.0f; // > 0 while reloading
    int m_grenades = kGrenadesPerLife; // same issue for every class; refilled on respawn
    // The blade, on its own charges and its own recovery — both entirely off
    // the magazine, which is what leaves it in the player's hands during a
    // reload. The flash is the swing the arc indicator draws, and it keeps the
    // direction it was swung in so aiming away mid-swing doesn't drag it round.
    int m_meleeCharges = kMelee.charges;
    float m_meleeRecover = 0.0f;  // > 0 while the charges are coming back
    float m_meleeCooldown = 0.0f; // time until the next swing
    float m_meleeFlash = 0.0f;    // seconds of swing arc left to draw
    Vector3 m_meleeSwingDir = Vector3::UnitX;
    // The class ability's clocks — off the magazine like the grenade and the
    // blade, so a reload never takes it away. What they mean and what runs them
    // is Ability's business; all that's kept here is that they belong to this
    // soldier, and that a respawn hands back a fresh set.
    Ability::Runtime m_ability;
    std::vector<Ability::Target> m_abilityAllies; // reused per frame; see AbilityScene
    std::vector<Brain::Contact> m_contacts;       // reused per NPC; see UpdateNpcs
    float m_walkPhase = 0.0f; // same walk-cycle bookkeeping as Npc::walkPhase
    float m_moveBlend = 0.0f;
    float m_playerHp = kMaxHealth;
    float m_frameMs = 0.0f;        // smoothed wall-clock frame time, for the HUD
    float m_rumbleTime = 0.0f;     // gamepad vibration left on a damage pulse
    float m_respawnTimer = 0.0f; // seconds left of the wait, while Phase::Dead
    bool m_playerDied = false;   // set by projectile hits, handled in Update
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
    // Blood on the floor, kept as finished geometry rather than as splats to
    // rebuild: a stain never moves or fades once it lands, so the vertices it
    // was born with are the vertices it dies with. Oldest first, so the cap
    // sheds the stalest patch.
    std::vector<Vertex> m_splatVerts;
    std::vector<Vertex> m_scratch;     // reused per draw
    std::vector<Vector3> m_aimArc;     // aim indicator trajectory, reused per frame
};
