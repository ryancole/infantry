#pragma once

#include "BindMenu.h"
#include "Bindings.h"
#include "Camera.h"
#include "ClassSelect.h"
#include "Command.h"
#include "Input.h"
#include "MainMenu.h"
#include "PlayerClass.h"
#include "Renderer.h"
#include "Soldier.h"
#include "Sound.h"
#include "Team.h"
#include "Visibility.h"
#include "World.h"

#include <DirectXMath.h>
#include <SimpleMath.h>
#include <array>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

// The machine the player is sitting at. Everything that decides a match lives
// in World; what's left here is everything that only exists because somebody
// is watching — the menus, the camera, the HUD, the fog, the blood, the
// corpses, the sound — and the two seams between them. Going down, input
// becomes a Command and the Command is all the simulation hears. Coming back
// up, the simulation says what happened as Events, and this class decides
// what each one looks and sounds like. A dedicated server keeps a World and
// none of this; a spectator client would keep all of this and drive nobody.
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

    // What's left of a soldier: the model's segments handed to the physics
    // world as a jointed ragdoll, keeping the pose it died in and then falling
    // out of it. Corpses are decoration — they take no damage, block nothing,
    // and collide with the level alone — which is why they live on this side
    // of the World seam, built from Death events: the simulation says a
    // soldier fell, and what falling looks like is presentation's business.
    // Their parts do borrow the World's physics (see World::Phys), because a
    // ragdoll still has to land on the same floor everything else stands on.
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

    // A visible level object: a model at a spot. The solid half of an object
    // lives in the World's collider list; this is only what gets drawn.
    struct Prop
    {
        const Model* model; // owned by m_models
        Vector3 pos;
        float scale;
        float yaw;
    };

    // The client half of the input seam: reads the bindings, the pad, and the
    // cursor through this machine's camera, and folds them into the one
    // sentence the simulation accepts. Everything screen-shaped dies here —
    // past this function nothing knows which way the player's monitor faces.
    Command ReadCommand(const Input& input, const IsoCamera& camera, const Vector3& pos) const;
    // The other seam, coming back up: everything the ticks just did, turned
    // into what it looks and sounds like — blood for a Hit, a ragdoll for a
    // Death, a thud for a Bounce, a rumble for the local soldier's pain. Runs
    // once a frame over however many ticks' worth of events accumulated, then
    // forgets them.
    void ProcessEvents();
    // Puts the player back on the field via the World and returns to
    // Phase::Playing; the camera cuts rather than sweeps across.
    void Respawn(IsoCamera& camera);
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
    void UpdateParticles(float dt);
    // Leaves a ragdoll where a soldier stood, built from the pose it died in
    // and launched with `knock` so it falls away from the killing blow. Past
    // the corpse cap the oldest body on the field is recycled.
    void SpawnCorpse(const Vector3& pos, const Vector3& aimDir, float walkPhase, float moveBlend,
                     const DirectX::XMFLOAT4& teamColor, const DirectX::XMFLOAT4& classColor,
                     const Vector3& knock);
    void UpdateCorpses(float dt);
    void RemoveCorpse(size_t index);
    // Positional one-shot SFX: panned and attenuated relative to the player.
    void PlaySoundAt(const std::string& name, const Vector3& pos, float pitch = 0.0f);
    float Rand(float lo, float hi);
    void AppendFog(std::vector<Vertex>& out) const;
    void RenderHud(Renderer& renderer);

    // The whole match, behind its seam. Everything this class knows about the
    // fight it learns by looking in (const accessors, for drawing and the
    // HUD) or by the events Tick hands back.
    World m_world;
    // Events the ticks of the current frame produced, spent by ProcessEvents.
    std::vector<Event> m_events;

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
    // The side the local player fights on. The World keeps its own copy for
    // the roster's arithmetic; this one is for everything drawn in "your
    // side's" color — the rings, the HUD's rows — before and after there's a
    // live unit to ask.
    int m_team = 0;
    // Where the player is looking from: their unit's drawn position while they
    // have one, frozen on the spot they died for the length of the wait. The
    // fog, the culling, and the ear all read it, which is what keeps a dead
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
    // the only one drawn — set off the MeleeSwing event.
    float m_meleeFlash = 0.0f; // seconds of swing arc left to draw
    Vector3 m_meleeSwingDir = Vector3::UnitX;
    float m_frameMs = 0.0f;      // smoothed wall-clock frame time, for the HUD
    float m_rumbleTime = 0.0f;   // gamepad vibration left on a damage pulse
    float m_respawnTimer = 0.0f; // seconds left of the wait, while Phase::Dead

    // The seam between render time and simulation time. Frames deposit their
    // dt in the accumulator and the simulation spends it in whole ticks;
    // whatever's left over is how far the current moment sits between the last
    // tick and the next, which is the fraction the renderer blends by.
    float m_tickAccum = 0.0f;
    float m_renderAlpha = 0.0f;
    // The command the next tick will consume. Held controls are overwritten
    // with whatever this frame's hands say; edges latch on and are only wiped
    // by the tick that spends them — a click that lands between two ticks
    // of a fast display would otherwise fall through the gap, and a click
    // that arrives while two ticks fire in one slow frame would otherwise
    // count twice.
    Command m_pendingCmd;

    // Presentation's own randomness — pitch detune, debris scatter, splat
    // shapes. Separate from the World's on purpose: how a shot sounds must
    // never move the dice that decide where the next one lands.
    std::mt19937 m_rng{ std::random_device{}() };

    std::vector<Particle> m_particles;
    std::vector<Corpse> m_corpses; // oldest first, so the cap sheds the stalest body
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
