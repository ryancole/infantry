#pragma once

#include "BindMenu.h"
#include "Bindings.h"
#include "Camera.h"
#include "ClassSelect.h"
#include "Command.h"
#include "Input.h"
#include "Discovery.h"
#include "Hud.h"
#include "JoinMenu.h"
#include "MainMenu.h"
#include "NetClient.h"
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
#include <deque>
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
    // Points the game at a server instead of its own simulation; call before
    // LoadContent. `autoClass` may name a class ("marine") to skip the menus
    // and join on launch — a dev convenience that makes "second machine into
    // the fight" one command line. Empty strings mean solo play, exactly as
    // before this function existed.
    void SetMultiplayer(std::string host, std::string autoClass);

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
    // isn't in it. Connecting sits between the class pick and Playing when
    // the arena is somebody else's: the class is chosen, the join is in
    // flight, and there's nothing to draw until the server says who you are.
    enum class Phase
    {
        MainMenu,
        Join, // the server browser: the LAN scan runs while this is up
        KeyBinds,
        ClassSelect,
        Connecting,
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
    // forgets them. In connected play the events arrived over the wire
    // instead of from a local Tick, and nothing in here can tell.
    void ProcessEvents();
    // Connected play's inbox: pumps the socket and applies what came —
    // snapshots into the replica World, events into the queue, Welcome and
    // Respawned into who-am-I and the phase. The camera comes along for the
    // cut a (re)spawn makes.
    void NetPump(float dt, IsoCamera& camera);
    // Ends a connected session and lands back on the main menu, whoever ended
    // it — the player leaving, the server dying, or the door being closed at
    // the join. The replica empties (World::Reset), the corpses and weather
    // this client conjured from its events go with it, and the menus work
    // exactly as if the game had just launched: Deploy starts a solo match on
    // the same ground.
    void LeaveMatch();
    // Reconciliation: the server's own-block just landed, naming the newest
    // command it consumed. Everything up to it is history and retired;
    // everything after is replayed — the same MoveCommand, the same tick
    // length, from the server's authoritative soldier — onto a fresh
    // prediction. When nothing contradicted us, the replay lands exactly
    // where the old prediction stood and the correction is invisible, which
    // is the normal case and the whole design.
    void Repredict(uint32_t ackSeq);
    // Puts the player back on the field via the World and returns to
    // Phase::Playing; the camera cuts rather than sweeps across.
    void Respawn(IsoCamera& camera);
    // The result has stood long enough: a fresh match on the same ground,
    // with the clock and both scores back at the start. Solo only — in
    // connected play the server decides when the next match begins, and this
    // client hears about it as a Respawned and a snapshot whose match is no
    // longer over.
    void StartNextMatch(IsoCamera& camera);
    // Sweeps everything this client conjured out of a match's events: the
    // corpses (whose bodies go back to the physics world), the weather in the
    // air, the blood on the floor. What a match left behind belongs to that
    // match — a new one starts on clean ground, and leaving a server takes
    // its mess with it.
    void ClearBattlefield();
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

    // The whole match, behind its seam. In solo play this is the match; in
    // connected play it's a replica the server's snapshots keep dressed, and
    // it is never Ticked. Everything this class knows about the fight it
    // learns by looking in (const accessors, for drawing and the HUD) or by
    // the events that arrive — from Tick or from the wire, same shape.
    World m_world;
    // Events the ticks of the current frame produced, spent by ProcessEvents.
    std::vector<Event> m_events;

    // The wire, when there is one. Null is solo play, and every branch on it
    // reads as "who runs the simulation" — this machine, or the far end.
    std::unique_ptr<NetClient> m_net;
    std::string m_connectHost; // where SetMultiplayer pointed us
    std::string m_autoClass;   // class to auto-join as, or empty for the menus
    int m_myUnitId = -1;       // the wire's name for our soldier; -1 while dead or unjoined
    bool m_joinSent = false;
    // Cut the camera to our soldier when they next appear in a snapshot: set
    // by Welcome and Respawned, spent by the first snapshot that has us. The
    // spawn is somewhere else entirely, and the camera shouldn't sweep there.
    bool m_camSnapPending = false;
    // Render-time interpolation against snapshot cadence: seconds since the
    // last snapshot landed, over the tick length, is how far the drawn frame
    // sits past the state we're holding — the wire's version of m_tickAccum.
    float m_snapElapsed = 0.0f;

    // --- Prediction: the local soldier, answered on the frame. In connected
    // play the replica's own unit is the server's truth, a round trip old; the
    // player shouldn't steer that. So their soldier is simulated twice: every
    // command is applied to this detached copy the tick it's made (movement
    // and aim only — nothing that spends the kit), and Repredict squares the
    // copy with the server every time an own-block arrives. Everyone else on
    // the field stays snapshot-fed; this is one soldier's private fast-forward.
    struct PendingCmd
    {
        uint32_t seq;
        Command cmd;
    };
    std::deque<PendingCmd> m_pendingCmds; // sent but not yet acked
    uint32_t m_cmdSeq = 0;                // stamp of the newest command made
    Unit m_predicted = {};                // the soldier as the player feels them
    bool m_hasPredicted = false;          // false until the first own-snapshot
    // The prediction's own between-ticks blend, distinct from the snapshot
    // one: the local soldier advances on this machine's command clock while
    // everyone else advances on the wire's, and each gets drawn on its own.
    float m_predAlpha = 0.0f;

    Sound m_sound;
    Phase m_phase = Phase::MainMenu;
    MainMenu m_mainMenu;
    JoinMenu m_joinMenu;
    // The LAN scan behind the join screen: started walking in, stopped
    // walking out, so the socket and the once-a-second shout exist only
    // while somebody is actually looking at the list.
    Discovery::Scan m_scan;
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
    // Whether the scoreboard key is down. Held rather than toggled, and stored
    // rather than read at draw time because Render isn't handed the input.
    bool m_showScores = false;
    // The board's rows, rebuilt each frame it's up. A member rather than a
    // local so the frames it isn't up cost nothing but a capacity that's
    // already been paid for.
    std::vector<Hud::ScoreRow> m_scoreRows;

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
