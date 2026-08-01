#pragma once

#include "Ability.h"
#include "BindMenu.h"
#include "Bindings.h"
#include "Brain.h"
#include "Camera.h"
#include "ClassSelect.h"
#include "Input.h"
#include "MainMenu.h"
#include "Physics.h"
#include "PlayerClass.h"
#include "Renderer.h"
#include "Soldier.h"
#include "Sound.h"
#include "Team.h"
#include "Visibility.h"

#include <DirectXMath.h>
#include <SimpleMath.h>
#include <array>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

// Prototype gameplay: two squads on a grid arena, screen-relative WASD
// movement, mouse aim, and projectiles. This is the seed for Infantry-style
// systems (vehicles, weapons, teams, net play) to grow from.
//
// The roster is one list of Units, and it is the shape a server would keep:
// each side has a fixed number of slots, a slot that empties is refilled after
// a wait, and who drives a slot is a fact *about* the soldier in it rather
// than a difference in what a soldier is. The local player is the one unit
// carrying Controller::Local; everyone else thinks with a Brain. When soldiers
// start arriving over a socket, they'll be one more controller.
class Game
{
public:
    // Loads the level (assets/levels/) and GPU assets; call once after
    // Renderer::Init. Throws std::runtime_error on a bad level or model.
    void LoadContent(Renderer& renderer);

    void Update(float dt, const Input& input, IsoCamera& camera);
    void Render(Renderer& renderer);

    // Whether the player has asked, from the menu, to be done. The game has no
    // way to close a window and no business learning one, so it says so and the
    // platform layer does it on the next turn of the loop.
    bool QuitRequested() const { return m_quit; }

    // The player's controls. Handed out because the camera orbit is driven from
    // the platform layer — it's a mouse *mode* as much as a control, and the
    // capture belongs where the window is — and that binding has to be the same
    // one the settings screen edits.
    const Bindings& Binds() const { return m_binds; }

    // Stops the game's hold on anything that outlives a frame; call once the
    // loop is done, before the window and devices go away.
    void Shutdown();

private:
    // The game opens on the menu, not in the arena: something has to ask
    // whether the player is playing before anything asks what they're playing
    // as. From there they must pick a class before they spawn, and the arena
    // only starts simulating once that choice is made. Dying drops back out of
    // Playing for the respawn wait: the arena keeps running, the player just
    // isn't in it.
    enum class Phase
    {
        MainMenu,
        KeyBinds,
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

    // One soldier on the field, whoever is driving it. The player used to be a
    // dozen loose m_player* fields and everyone else an Npc struct, which meant
    // every system that dealt damage or picked a target had to say everything
    // twice — and a server can't work that way: to it a match is ten of the
    // same thing, and which machine steers which is bookkeeping. So the roster
    // is one shape, and the controller is one more fact about a soldier.
    //
    // They are not all hostile. A unit on the player's own team is a squadmate
    // — it fights the same enemies, soaks the same rounds, and is the thing a
    // medic's dressing has to have in front of it to be worth anything. Which
    // side a unit is on is `team`, and every system that deals damage or picks
    // a target reads it rather than assuming.
    struct Unit
    {
        // Who decides what this soldier does next: a Brain, or this machine's
        // input. Nothing below the controller differs by it — a networked
        // player joins this enum, not this struct.
        enum class Controller
        {
            Ai,
            Local,
        };

        const ClassDef* cls;
        Controller controller;
        int team;
        Vector3 pos;
        // Ground momentum: the keys steer it, they aren't it. Only the local
        // player moves on it today — the AI still walks on speed alone, and
        // giving it weight is a change to how it steers, not one more field
        // to read.
        Vector3 moveVel;
        Vector3 aimDir;
        float hp;
        float fireCooldown;
        int ammo;          // rounds left in the magazine
        float reloadTimer; // > 0 while reloading, and unable to shoot back
        // The rest of the kit, issued to every controller alike: today's
        // brains never throw the grenade, swing the blade, or carry a class
        // with an ability, but that's a fact about the brains, not about what
        // an AI soldier is issued.
        int grenades;
        int meleeCharges;
        float meleeRecover;  // > 0 while the charges are coming back
        float meleeCooldown; // time until the next swing
        Ability::Runtime ability;
        // Whatever this soldier's brain remembers between frames, and nothing
        // at all while the controller is Local. Meaningless out here on
        // purpose: which mind it is comes off the class, and what it keeps in
        // there is that mind's business.
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
        // The two colors the body was wearing, kept rather than looked up: a
        // corpse has outlived the soldier it came from, so there's no team and
        // no class left to ask. It also means a dead soldier still says whose
        // side it was on, which is most of what a body on the ground is for.
        DirectX::XMFLOAT4 teamColor;
        DirectX::XMFLOAT4 classColor;
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

    // A slot on a team waiting to be filled again. Queued when a soldier dies
    // and cashed in kRespawnDelay later, which is the same wait the player
    // serves — a squad is five soldiers, and the only time it isn't is the
    // moment after one of them is killed.
    //
    // It's a queue rather than one timer per side because each death should
    // start its own clock: a team that loses three at once has three soldiers
    // coming back at the times they died, not one every five seconds while the
    // other side runs it down.
    struct Reinforcement
    {
        int team;
        float timer;
    };

    // Fires `weapon`'s projectile from `from` along `dir`. Bullets always fly
    // at full speed; lobbed shots (grenades) shorten their toss to come down
    // `targetDist` away, up to the weapon's max range.
    void SpawnShot(const WeaponDef& weapon, const Vector3& from, const Vector3& dir, int team,
                   float targetDist);
    // Starts `unit`'s reload: empties the rest of the magazine into the timer
    // and plays the mag-out click. A no-op if one is already running or the
    // magazine is full, so the reload key can be leaned on harmlessly.
    void BeginReload(Unit& unit);
    // Spends one of `attacker`'s melee charges on a swing through the arc in
    // front of them and strikes the nearest enemy standing in it, if any. The
    // charge goes whether or not it lands.
    void SwingMelee(Unit& attacker);
    // What `user`'s ability is allowed to act on this frame: the user
    // themselves, and every living squadmate they can currently see. The sight
    // rule is applied here rather than inside the ability because the fog is
    // this class's business and an ability has never heard of a wall — which is
    // also what keeps "can a medic treat someone through cover" answered in
    // exactly one place. Rebuilt per call into m_abilityAllies, which is kept
    // around so the per-frame list costs no allocation.
    Ability::Scene AbilityScene(Unit& user);
    // The unit this machine is driving, or null while there isn't one — before
    // a class is picked, and for the length of the respawn wait. Dying really
    // does take the player off the roster: the systems that sweep it (shots,
    // blasts, sights) stop finding them without any of them asking who's alive.
    Unit* LocalUnit();
    const Unit* LocalUnit() const;
    // Deals the next class off `team`'s rotation and puts an AI soldier on the
    // field with it.
    void SpawnAi(int team);
    // Puts the player's own unit on the field: the class they picked, at their
    // team's spawn, with the whole loadout issued fresh off the class table.
    // Both the match start and every respawn come through here, which is what
    // makes a respawn a new soldier rather than a checklist of things to
    // remember to put back.
    void SpawnLocal();
    // Puts both sides on the field at full strength, the local player among
    // them. Called once, when the player commits to a class, because that's
    // the moment the match starts — nothing simulates until then and there's
    // nobody for a squad to fight.
    void FillRosters();
    // Runs the queue of waiting slots down and puts a soldier back on the field
    // when one comes due, unless the side is somehow already up to strength.
    void UpdateReinforcements(float dt);
    // How many AI soldiers `team` is meant to be fielding: its share of the
    // roster, less the slot the local player holds on their own side. The
    // player's slot is theirs whether they're alive or waiting to respawn, so
    // nothing fills in for them and nothing has to be sent away when they're
    // back.
    int AiQuota(int team) const
    {
        return kTeamSize - (team == m_team ? 1 : 0);
    }
    // Living AI soldiers currently on `team`.
    int AiCount(int team) const;
    void UpdateUnits(float dt);
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
                     const DirectX::XMFLOAT4& teamColor, const DirectX::XMFLOAT4& classColor,
                     const Vector3& knock);
    // Puts the player back on their team's spawn with a fresh loadout and
    // returns to Phase::Playing; the camera cuts rather than sweeps across.
    void Respawn(IsoCamera& camera);
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
    // Soldiers a side puts on the field, the local player included. Five a side
    // is the smallest number that makes the arena read as a fight rather than a
    // duel: enough that a flank is covered by somebody, few enough that any one
    // soldier going down is felt. It's a constant rather than a level property
    // because it's a statement about the game mode, and there's only one of
    // those so far; when a server decides team sizes, this is what it sets.
    static constexpr int kTeamSize = 5;

    Physics m_physics;
    Sound m_sound;
    Phase m_phase = Phase::MainMenu;
    MainMenu m_mainMenu;
    BindMenu m_bindMenu;
    ClassSelect m_classSelect;
    // The player's layout, read off disk at startup and written back whenever
    // they leave the settings screen. Defaults stand if there's no file yet.
    Bindings m_binds;
    bool m_quit = false; // see QuitRequested
    // The HUD's key caps, rebuilt each frame from the bindings above. They're
    // strings rather than the letters the HUD used to hold because a cap can now
    // say anything the player has bound, including two of them joined by a plus.
    // They live here rather than in RenderHud because the HUD is handed pointers
    // to them and draws after that function has returned.
    static constexpr size_t kMaxHints = 8;
    std::array<std::string, kMaxHints> m_hintKeys;
    const ClassDef* m_class = nullptr; // set when the player picks; never null while Playing
    float m_arenaHalf = 32.0f;
    // Team spawn points from the level, indexed by team id. The local player
    // is on m_team; a team-select flow can set it before spawning, and future
    // respawn/teammate logic reads from the same table.
    std::vector<Vector3> m_teamSpawns;
    int m_team = 0;
    // Where the player is looking from: their unit's position while they have
    // one, frozen on the spot they died for the length of the wait. The fog,
    // the culling, and the ear all read it, which is what keeps a dead
    // player's view — and what they can hear moving through it — anchored to
    // the body instead of snapping to nowhere when the unit comes off the
    // roster.
    Vector3 m_eyePos;
    // Which way is right on screen, in world space, taken off the camera during
    // Update because Render isn't handed one. It's what stands a health bar
    // square to the view. Read a frame later than it's written, which costs
    // nothing: yaw is applied before Update runs, and nothing between there and
    // the draw can turn the camera.
    Vector3 m_screenRight = Vector3::UnitX;
    float m_aimDist = 1e9f; // distance to the cursor's ground point; huge = unaimed (stick)
    // The swing the arc indicator draws, kept in the direction it was swung so
    // aiming away mid-swing doesn't drag it round. Presentation rather than
    // soldier state — it's the mark of the local player's last swing, which is
    // the only one drawn — so it stays out of Unit until a brain learns the
    // blade and somebody else's swing is worth seeing.
    float m_meleeFlash = 0.0f; // seconds of swing arc left to draw
    Vector3 m_meleeSwingDir = Vector3::UnitX;
    std::vector<Ability::Target> m_abilityAllies; // reused per frame; see AbilityScene
    std::vector<Brain::Contact> m_contacts;       // reused per unit; see UpdateUnits
    float m_frameMs = 0.0f;      // smoothed wall-clock frame time, for the HUD
    float m_rumbleTime = 0.0f;   // gamepad vibration left on a damage pulse
    float m_respawnTimer = 0.0f; // seconds left of the wait, while Phase::Dead

    // The roster: every soldier on the field, whichever machine or mind is
    // driving them. Dead units are swept off it by ReapDead, so anything
    // holding a pointer or index into it is good for one frame at most.
    std::vector<Unit> m_units;
    // Where each side has got to in the class table. Per team rather than
    // global so both squads are dealt from the same rotation and come out with
    // the same makeup, however many soldiers each of them is owed — which is
    // not the same number, since the player fills one of the slots on their
    // own side. Sized to the level's team count in LoadContent.
    std::vector<int> m_nextAiClass;
    std::vector<Reinforcement> m_reinforcements; // slots waiting to be refilled
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
