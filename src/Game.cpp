#include "Game.h"

#include "Hud.h"
#include "Level.h"
#include "Net.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>

using namespace DirectX;
using namespace DirectX::SimpleMath;

namespace
{
    // Shorthand for the action names, which are read a dozen times in
    // ReadCommand and once each. The bindings behind them are the player's,
    // and live in Bindings.h alongside the defaults.
    using Act = Bindings::Action;

    constexpr float kFogHeight = 0.02f; // just above the floor grid lines
    constexpr float kFogFar = 6.0f;     // shadow reach, in arena-half units

    // Bounding sphere used to skip soldiers that fall outside the viewport.
    // Generous on purpose: it has to cover the model at full stride, and
    // popping in at the screen edge is far worse than submitting a few extra
    // draws.
    constexpr float kSoldierBoundsY = 0.7f;
    constexpr float kSoldierBoundsRadius = 1.2f;

    // Corpses: a killed soldier is handed to the physics world as a ragdoll and
    // left to fall. They're decoration, so what matters is that they get out of
    // the way again — a body sinks through the floor over its last seconds
    // rather than blinking out while someone is looking at it, and the field
    // holds a fixed number so a long firefight can't unbound the body count.
    constexpr float kCorpseLife = 14.0f;
    constexpr float kCorpseSink = 1.5f;      // seconds spent sinking, at the end of that life
    constexpr float kCorpseSinkDepth = 1.6f; // deep enough to swallow a body whole
    constexpr size_t kMaxCorpses = 10;
    constexpr float kCorpseSpin = 7.0f; // random angular velocity, radians/sec

    // Blood. A hit throws drops along the blow, and each one stains the floor
    // where it lands, so the mess a fight leaves points back at where the
    // shooting came from. The stains don't fade: an arena that has been fought
    // over should look like it, and a corridor of old blood is a real reading
    // of where the fighting has been. What is bounded is how many the field
    // holds at once — past the cap the oldest patch is dropped.
    //
    // The cap is set by the renderer, not by taste: the whole floor goes down
    // as one dynamic batch, and Renderer::DrawTrianglesAlpha silently drops a
    // batch bigger than 16384 vertices, so the stains have to fit inside that
    // with room to spare. At 24 vertices each that leaves well over ten deaths
    // of history — dozens for the automatics, whose individual rounds bleed
    // far less than the shot that ends someone.
    constexpr float kBloodDropsPerDamage = 0.35f;
    constexpr int kBloodMinDrops = 4;
    constexpr int kBloodMaxDrops = 22;
    constexpr float kBloodSpread = 1.1f;   // radians off the blow, either way
    constexpr float kBloodDropLife = 1.5f; // long enough that every drop lands
    constexpr int kSplatSegments = 8;      // fan segments per stain
    constexpr int kSplatVerts = kSplatSegments * 3;
    constexpr size_t kMaxSplats = 512;
    constexpr float kSplatHeight = 0.012f;    // over the grid lines, under the fog quads
    constexpr float kSplatMinRadius = 0.10f;
    constexpr float kSplatMaxRadius = 0.30f;

    constexpr float kPerfSmoothRate = 4.0f; // HUD timing smoothing, ~1/4s window

    constexpr XMFLOAT4 kGridMinor = { 0.10f, 0.13f, 0.17f, 1.0f };
    constexpr XMFLOAT4 kGridMajor = { 0.17f, 0.22f, 0.29f, 1.0f };
    constexpr XMFLOAT4 kBorder = { 0.55f, 0.25f, 0.20f, 1.0f };
    constexpr XMFLOAT4 kObstacleColor = { 0.35f, 0.40f, 0.50f, 1.0f };
    constexpr XMFLOAT4 kProjectileColor = { 1.00f, 0.80f, 0.20f, 1.0f };
    // A live grenade reads as a blinking casing rather than a tracer, so it
    // can't be mistaken for a bullet while it bounces.
    constexpr XMFLOAT4 kGrenadeLiveColor = { 1.00f, 0.30f, 0.12f, 1.0f };
    constexpr XMFLOAT4 kGrenadeCasingColor = { 0.32f, 0.30f, 0.28f, 1.0f };
    constexpr float kGrenadeBlinkPeriod = 0.24f; // seconds per on/off cycle
    constexpr float kGrenadeFlashLife = 0.4f;    // fuse left when it goes solid bright
    // The aim indicator is alpha-blended; the shaft dims via alpha rather
    // than darker RGB so whatever it crosses still shows through.
    constexpr XMFLOAT4 kAimColor = { 0.95f, 0.95f, 0.40f, 0.55f };
    constexpr XMFLOAT4 kAimDimColor = { 0.95f, 0.95f, 0.40f, 0.22f };
    // Drained of color while the magazine is out: the line still tracks the
    // cursor, but nothing will come out of the barrel until the reload ends,
    // and that has to be visible without looking away from the fight.
    constexpr XMFLOAT4 kAimSpentColor = { 0.62f, 0.64f, 0.68f, 0.45f };
    constexpr XMFLOAT4 kAimSpentDimColor = { 0.62f, 0.64f, 0.68f, 0.18f };
    // The grenade's ring is orange, so it never reads as part of the primary's
    // yellow aim line.
    constexpr XMFLOAT4 kGrenadeAimColor = { 0.95f, 0.50f, 0.18f, 0.35f };
    constexpr float kAimRingHeight = 0.05f;    // above the fog quads, so it isn't dimmed
    constexpr float kGrenadeMarkRadius = 0.5f; // first-bounce marker, not the blast size
    // The swing: a pale arc swept at the blade's real reach, fading out over an
    // eighth of a second. It's drawn after the fact rather than as an aim
    // indicator on purpose — a melee is a commitment, so what the player gets to
    // see is where the blade went, not a standing promise of where it would go.
    constexpr XMFLOAT4 kMeleeArcColor = { 0.82f, 0.90f, 1.00f, 0.85f };
    constexpr float kMeleeFlashTime = 0.13f;
    // Which soldiers are on the player's side, said a second time: a thin ring
    // on the ground under everyone friendly. The armor is the real answer now
    // that a body wears its team's color, and this is the backup — a soldier
    // half-behind a crate, or one crossing at the edge of the fog, is a few
    // pixels of plate and a ring is easier to catch than a hue. It's drawn in
    // the player's own team color rather than a fixed blue so the two marks
    // never disagree about what "your side" looks like. Only the player's side
    // gets one: a soldier with nothing under them is someone to shoot.
    constexpr float kFriendlyRingAlpha = 0.45f;
    constexpr float kFriendlyRingRadius = 0.55f;
    // Wet and bright in the air, dark and matte once it has soaked into the
    // floor. The stain is translucent, so overlapping ones deepen where a fight
    // stayed in one place.
    constexpr XMFLOAT4 kBloodColor = { 0.62f, 0.06f, 0.07f, 1.0f };
    constexpr XMFLOAT4 kSplatColor = { 0.30f, 0.03f, 0.04f, 0.85f };
    constexpr XMFLOAT4 kFogColor = { 0.01f, 0.02f, 0.04f, 0.85f };
    constexpr XMFLOAT4 kHudColor = { 0.85f, 0.90f, 0.95f, 1.0f };
    constexpr XMFLOAT4 kHudHintColor = { 0.45f, 0.52f, 0.62f, 1.0f };

    // Appends a solid cube with fixed per-face shading. Units and obstacles
    // moved to the renderer's lit shapes; this remains for effects (impact
    // debris) that shouldn't respond to scene lighting.
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

    // How a projectile in flight is drawn: tracers are a steady bright dot,
    // while a fused grenade blinks its casing and then holds bright for the
    // last moment before it goes off, so anyone watching can time it. Phase
    // comes from the fuse itself, so there's no clock to keep per grenade.
    XMFLOAT4 ProjectileColor(float life, bool fused)
    {
        if (!fused)
            return kProjectileColor;
        if (life <= kGrenadeFlashLife)
            return kGrenadeLiveColor;
        return std::fmod(life, kGrenadeBlinkPeriod) > kGrenadeBlinkPeriod * 0.5f
                   ? kGrenadeLiveColor
                   : kGrenadeCasingColor;
    }

    // A horizontal run of line segments on a circle, `sweep` radians wide from
    // `start`. Angles are world headings: measured from +x toward +z, the same
    // convention atan2(z, x) gives back.
    void AppendArc(std::vector<Vertex>& out, const Vector3& center, float radius, float start,
                   float sweep, int segments, const XMFLOAT4& color)
    {
        for (int i = 0; i < segments; ++i)
        {
            const float a0 = start + sweep * i / segments;
            const float a1 = start + sweep * (i + 1) / segments;
            out.push_back({ XMFLOAT3{ center.x + std::cos(a0) * radius, center.y,
                                      center.z + std::sin(a0) * radius }, color });
            out.push_back({ XMFLOAT3{ center.x + std::cos(a1) * radius, center.y,
                                      center.z + std::sin(a1) * radius }, color });
        }
    }

    // A full ring, for the aim indicator's landing marks.
    void AppendCircle(std::vector<Vertex>& out, const Vector3& center, float radius,
                      const XMFLOAT4& color)
    {
        AppendArc(out, center, radius, 0.0f, XM_2PI, 24, color);
    }

    // Draws a living soldier: the model's segments posed by the walk cycle and
    // placed at `pos` facing `aimDir`. A corpse draws the same parts, its
    // segments posed by the ragdoll instead (see Game::Render).
    void DrawSoldier(Renderer& renderer, const Vector3& pos, const Vector3& aimDir,
                     float walkPhase, float moveBlend, int team, const XMFLOAT4& classColor)
    {
        XMMATRIX local[Soldier::SegmentCount];
        Soldier::Pose(local, walkPhase, moveBlend);

        const XMMATRIX base = Soldier::Base(pos, aimDir);
        XMMATRIX world[Soldier::SegmentCount];
        for (int i = 0; i < Soldier::SegmentCount; ++i)
            world[i] = local[i] * base;

        Soldier::Draw(renderer, world, TeamColor(team), classColor);
    }
}

void Game::SetMultiplayer(std::string host, std::string autoClass)
{
    m_connectHost = std::move(host);
    m_autoClass = std::move(autoClass);
}

void Game::LoadContent(Renderer& renderer)
{
    // The player's own layout, if they've made one. A missing or unreadable
    // file isn't an error worth stopping for — it just means the defaults, which
    // is what a first run is — so nothing here checks the answer.
    m_binds.Load();

    // Connected play: start the handshake now, behind the menus, so by the
    // time a class is picked the socket is usually already up. With a class
    // named on the command line the menus are skipped entirely — launch to
    // firefight in one step, which is what iterating on netcode wants.
    if (!m_connectHost.empty())
    {
        m_net = std::make_unique<NetClient>();
        m_net->Start(m_connectHost, Net::kPort);
        for (size_t i = 0; i < kClassCount; ++i)
            if (_stricmp(kClassDefs[i].name, m_autoClass.c_str()) == 0)
            {
                m_class = &kClassDefs[i];
                m_phase = Phase::Connecting;
                break;
            }
    }

    m_sound.Init(); // loads the wave bank and starts the ambience

    const LevelData level = LevelData::Load("assets/levels/arena01.json");
    m_team = std::min(m_team, static_cast<int>(level.spawns.size()) - 1);

    // The level splits down the same seam everything else does: the World
    // takes the parts that decide anything (colliders, occluders, spawns),
    // and what's left here is what the level looks like.
    m_world.Init(level);

    // Static floor grid.
    const float arenaHalf = m_world.ArenaHalf();
    const int half = static_cast<int>(arenaHalf);
    for (int i = -half; i <= half; ++i)
    {
        const bool border = (i == -half || i == half);
        const bool major = (i % 8) == 0;
        const XMFLOAT4 col = border ? kBorder : (major ? kGridMajor : kGridMinor);
        const float f = static_cast<float>(i);
        m_gridVerts.push_back({ XMFLOAT3{ f, 0.0f, -arenaHalf }, col });
        m_gridVerts.push_back({ XMFLOAT3{ f, 0.0f, arenaHalf }, col });
        m_gridVerts.push_back({ XMFLOAT3{ -arenaHalf, 0.0f, f }, col });
        m_gridVerts.push_back({ XMFLOAT3{ arenaHalf, 0.0f, f }, col });
    }

    // The drawn half of each level object. Each distinct model is loaded
    // once; instances share it.
    for (const LevelData::Object& obj : level.objects)
    {
        if (obj.model.empty())
            continue;
        auto it = m_models.find(obj.model);
        if (it == m_models.end())
            it = m_models.emplace(obj.model, renderer.LoadModel(obj.model)).first;
        m_props.push_back({ it->second.get(), obj.pos, obj.scale, obj.yaw });
    }
}

void Game::Shutdown()
{
    if (m_net)
        m_net->Disconnect();
    m_sound.Shutdown();
}

void Game::NetPump(float dt, IsoCamera& camera)
{
    (void)dt;
    std::vector<std::vector<uint8_t>> packets;
    m_net->Poll(packets);

    // The far end going away is the end of the session, however far into it
    // we were: there's no local simulation to fall back into a match with,
    // and pretending otherwise would leave the player aiming at statues.
    if (m_net->GetStatus() == NetClient::Status::Failed)
    {
        m_quit = true;
        return;
    }

    for (const std::vector<uint8_t>& packet : packets)
    {
        Net::Reader r(packet.data(), packet.size());
        switch (static_cast<Net::MsgType>(r.U8()))
        {
        case Net::MsgType::Welcome:
        {
            m_myUnitId = r.I32();
            m_team = r.U8();
            m_camSnapPending = true;
            if (m_phase == Phase::Connecting)
                m_phase = Phase::Playing;
            break;
        }
        case Net::MsgType::Respawned:
        {
            m_myUnitId = r.I32();
            m_camSnapPending = true;
            if (m_phase == Phase::Dead)
                m_phase = Phase::Playing;
            break;
        }
        case Net::MsgType::Snapshot:
        {
            const Net::Snapshot snap = Net::ReadSnapshot(r);
            if (!r.ok)
                break;
            m_world.ApplySnapshot(snap, m_myUnitId);
            m_snapElapsed = 0.0f;
            // The cut a spawn deserves, taken on the first snapshot that
            // actually has us in it — Welcome says who we are, but only a
            // snapshot says where.
            if (m_camSnapPending)
                if (const Unit* me = m_world.Local())
                {
                    m_eyePos = me->pos;
                    camera.SetTarget(m_eyePos);
                    camera.SnapToTarget();
                    m_camSnapPending = false;
                }
            break;
        }
        case Net::MsgType::Events:
            Net::ReadEvents(r, m_myUnitId, m_events);
            break;
        default:
            break;
        }
    }
}

void Game::Update(float dt, const Input& input, IsoCamera& camera)
{
    m_sound.Update();

    // Smoothed so the HUD is readable; raw frame times jitter far too much to
    // compare by eye. Same exponential form as the camera's follow.
    m_frameMs += (dt * 1000.0f - m_frameMs) * (1.0f - std::exp(-kPerfSmoothRate * dt));

    // Short rumble pulse while the damage timer runs (no-op without a pad).
    m_rumbleTime = std::max(0.0f, m_rumbleTime - dt);
    const float rumble = m_rumbleTime > 0.0f ? 0.6f : 0.0f;
    DirectX::GamePad::Get().SetVibration(0, rumble, rumble);

    if (m_phase == Phase::MainMenu)
    {
        if (const auto picked =
                m_mainMenu.Update(input, camera.ViewportWidth(), camera.ViewportHeight()))
        {
            switch (*picked)
            {
            case MainMenu::Choice::Deploy:   m_phase = Phase::ClassSelect; break;
            case MainMenu::Choice::KeyBinds: m_phase = Phase::KeyBinds; break;
            case MainMenu::Choice::Quit:     m_quit = true; break;
            }
        }
        return;
    }

    // The settings screen edits the bindings in place; the write to disk
    // happens here, once, on the way out. A save per keystroke would put a file
    // write in the middle of a player trying three keys to see which feels
    // right, and there's nothing to lose by waiting until they're done.
    if (m_phase == Phase::KeyBinds)
    {
        if (m_bindMenu.Update(input, m_binds, camera.ViewportWidth(), camera.ViewportHeight()))
        {
            m_binds.Save();
            m_phase = Phase::MainMenu;
        }
        return;
    }

    if (m_phase == Phase::ClassSelect)
    {
        // Escape backs out to the menu rather than closing the game, now that
        // there's a menu to back out to. It's the same key that ends the game
        // from inside the arena, one level further in — out of the screen you're
        // on, and then out of the game.
        if (input.KeyPressed(VK_ESCAPE))
        {
            m_phase = Phase::MainMenu;
            return;
        }
        if (const auto picked =
                m_classSelect.Update(input, camera.ViewportWidth(), camera.ViewportHeight()))
        {
            m_class = &GetClassDef(*picked);
            if (m_net)
            {
                // The match is somebody else's: ask to be in it. The join
                // goes out from the Connecting phase, which also covers a
                // handshake that hasn't finished yet.
                m_phase = Phase::Connecting;
            }
            else
            {
                m_phase = Phase::Playing;
                // The whole match turns up with the player — their own unit
                // included, loadout and all. The class pick is the last thing
                // standing between the menu and the arena, so it's also the
                // first moment there's anything for two squads to do.
                m_world.StartMatch(m_class, m_team);
                m_eyePos = m_world.Local()->pos;
                camera.SetTarget(m_eyePos);
                camera.SnapToTarget();
            }
        }
        return;
    }

    // Connecting: the handshake and the join, out from under the menus. All
    // that can happen here is the server answering (NetPump flips the phase
    // on Welcome), the attempt failing, or the player giving up.
    if (m_phase == Phase::Connecting)
    {
        if (input.KeyPressed(VK_ESCAPE))
        {
            m_quit = true;
            return;
        }
        if (m_net->GetStatus() == NetClient::Status::Connected && !m_joinSent && m_class)
        {
            Net::Writer w;
            Net::WriteJoin(w, static_cast<uint8_t>(m_class - kClassDefs));
            m_net->SendReliable(w.bytes);
            m_joinSent = true;
        }
        NetPump(dt, camera);
        return;
    }

    // In the arena, escape still ends the game outright, which is what it did
    // from the window procedure before there were any screens to back out
    // through. It's the honest state of things: there's no pause menu to fall
    // into yet, and quietly making the key do nothing would be worse than the
    // bluntness of what it does.
    if (input.KeyPressed(VK_ESCAPE))
    {
        m_quit = true;
        return;
    }

    // Dead: the arena runs on without the player — NPCs keep fighting, shots
    // keep flying, the corpse keeps falling — but no command reaches the
    // simulation and the camera holds on the spot where the body dropped
    // until the wait is up. The events keep coming, and keep being shown: a
    // firefight the player is watching out still sounds like one.
    if (m_phase == Phase::Dead)
    {
        m_respawnTimer -= dt;
        if (m_net)
        {
            // The wait is really the server's; the timer here only feeds the
            // countdown text, and Respawned is what actually brings us back
            // (see NetPump). The ragdolls still need local gravity.
            NetPump(dt, camera);
            m_snapElapsed += dt;
            m_renderAlpha = std::clamp(m_snapElapsed / World::kTickDt, 0.0f, 1.0f);
            m_world.Phys().Step(dt);
        }
        else
        {
            m_tickAccum += dt;
            while (m_tickAccum >= World::kTickDt)
            {
                m_tickAccum -= World::kTickDt;
                m_world.Tick(nullptr, m_events);
            }
            m_renderAlpha = m_tickAccum / World::kTickDt;
        }
        ProcessEvents();
        UpdateParticles(dt);
        UpdateCorpses(dt);
        if (!m_net && m_respawnTimer <= 0.0f)
            Respawn(camera);
        return;
    }

    // Input becomes a Command, and the Command is all the simulation hears:
    // the bindings, the pad, and the cursor's trip through the camera all end
    // inside ReadCommand, and the World does exactly the same work on a
    // sentence that arrived over a socket. The one keepsake this side holds
    // onto is the aim distance, which the aim indicator wants at render time.
    m_screenRight = camera.ScreenRightOnGround();
    const Command fresh = ReadCommand(input, camera, m_world.Local()->pos);
    m_aimDist = fresh.aimDist;

    m_meleeFlash = std::max(0.0f, m_meleeFlash - dt);

    if (m_net)
    {
        // Connected: the command goes to the far end instead of a local tick.
        // Sent fresh every frame — the server latches edges between its own
        // ticks the way solo play latches them here, so a click can't slip
        // between two of anybody's ticks.
        Net::Writer w;
        Net::WriteCmd(w, fresh);
        m_net->SendState(w.bytes);

        NetPump(dt, camera);
        m_snapElapsed += dt;
        m_renderAlpha = std::clamp(m_snapElapsed / World::kTickDt, 0.0f, 1.0f);
        // The replica is never Ticked, but the corpses still fall through the
        // local physics world; this is their gravity.
        m_world.Phys().Step(dt);
    }
    else
    {
        // Fold this frame's hands into the pending command: held controls are
        // whatever they are right now, edges latch until a tick spends them.
        m_pendingCmd.move = fresh.move;
        m_pendingCmd.aim = fresh.aim;
        m_pendingCmd.aimDist = fresh.aimDist;
        m_pendingCmd.fire = fresh.fire;
        m_pendingCmd.melee = fresh.melee;
        m_pendingCmd.steady = fresh.steady;
        m_pendingCmd.reload = m_pendingCmd.reload || fresh.reload;
        m_pendingCmd.grenade = m_pendingCmd.grenade || fresh.grenade;
        m_pendingCmd.ability = m_pendingCmd.ability || fresh.ability;

        // Simulation time passes in whole ticks; the frame deposits its dt
        // and the loop spends what's there. The leftover is where this
        // frame's picture sits between two ticks, which is what the renderer
        // blends by.
        m_tickAccum += dt;
        while (m_tickAccum >= World::kTickDt)
        {
            m_tickAccum -= World::kTickDt;
            m_world.Tick(&m_pendingCmd, m_events);
            // The tick spent the edges; a second tick in the same frame must
            // not spend them again.
            m_pendingCmd.reload = false;
            m_pendingCmd.grenade = false;
            m_pendingCmd.ability = false;
        }
        m_renderAlpha = m_tickAccum / World::kTickDt;
    }

    // Everything those ticks did — or the wire delivered — turned into
    // something to see and hear.
    ProcessEvents();

    // Particles are the client's own weather — they never touch the outcome
    // of anything, so they run on render time and stay smooth whatever the
    // tick is doing. Corpse timers run here for the same reason: how long a
    // body lies around is a fact about the picture, not the fight.
    UpdateParticles(dt);
    UpdateCorpses(dt);

    // Death is noticed by absence. Whatever emptied the local soldier's
    // health, the World has already reaped them on the same terms as anyone
    // else, and the Death event has already put their corpse on the floor.
    // What remains is the part that belongs to the player rather than the
    // soldier: stop drawing a swing whose arm is gone, start the wait, and
    // let the camera hold where the body fell (m_eyePos keeps the spot).
    if (!m_world.Local())
    {
        m_meleeFlash = 0.0f;
        m_phase = Phase::Dead;
        m_respawnTimer = World::kRespawnDelay;
        return;
    }

    // The eye, the ear, and the camera all follow the soldier as drawn — the
    // blend between ticks — not the soldier as simulated, so nothing on
    // screen leads or trails the body it's about.
    const Unit& local = *m_world.Local();
    m_eyePos = Vector3::Lerp(local.prevPos, local.pos, m_renderAlpha);
    m_sound.SetListener(m_eyePos, camera.ScreenUpOnGround());
    camera.SetTarget(m_eyePos);
}

Command Game::ReadCommand(const Input& input, const IsoCamera& camera, const Vector3& pos) const
{
    using PadTracker = DirectX::GamePad::ButtonStateTracker;

    Command cmd;

    // --- Movement: WASD relative to the screen, resolved to world space here
    // because the screen is this side's business ---
    const Vector3 upG = camera.ScreenUpOnGround();
    const Vector3 rightG = camera.ScreenRightOnGround();

    float moveUp = 0.0f, moveRight = 0.0f;
    if (m_binds.Down(input, Act::MoveForward)) moveUp += 1.0f;
    if (m_binds.Down(input, Act::MoveBack)) moveUp -= 1.0f;
    if (m_binds.Down(input, Act::MoveRight)) moveRight += 1.0f;
    if (m_binds.Down(input, Act::MoveLeft)) moveRight -= 1.0f;
    moveUp += input.pad.thumbSticks.leftY;
    moveRight += input.pad.thumbSticks.leftX;

    Vector3 move = upG * moveUp + rightG * moveRight;
    if (move.LengthSquared() > 1e-10f)
    {
        move.Normalize();
        cmd.move = move;
    }

    // --- Aim: mouse cursor projected onto the ground plane, or the right
    // stick as a screen-relative direction when deflected ---
    Vector3 aim = camera.ScreenToGround(input.mouseX, input.mouseY) - pos;
    const Vector2 stick(input.pad.thumbSticks.rightX, input.pad.thumbSticks.rightY);
    if (stick.LengthSquared() > 0.1f)
        aim = upG * stick.y + rightG * stick.x;
    aim.y = 0.0f;
    if (aim.LengthSquared() > 1e-8f)
    {
        // The stick gives a direction but no point to land on; lobbed shots
        // fall back to full range, like aiming past max range with the mouse.
        cmd.aimDist = stick.LengthSquared() > 0.1f ? 1e9f : aim.Length();
        aim.Normalize();
        cmd.aim = aim;
    }

    // Steady is held, not toggled, because it's a posture and not a mode: a
    // toggle would leave the player standing in it having forgotten, and the
    // whole cost of the thing is that it's a decision you're currently making.
    cmd.steady = m_binds.Down(input, Act::Steady) || input.pad.triggers.left > 0.5f;
    cmd.fire = m_binds.Down(input, Act::Fire) || m_binds.Pressed(input, Act::Fire) ||
               input.pad.triggers.right > 0.5f;
    cmd.melee = m_binds.Down(input, Act::Melee) || input.pad.IsRightShoulderPressed();
    cmd.reload = m_binds.Pressed(input, Act::Reload) || input.padEvents.x == PadTracker::PRESSED;
    cmd.grenade = m_binds.Pressed(input, Act::Grenade) ||
                  input.padEvents.leftShoulder == PadTracker::PRESSED;
    cmd.ability =
        m_binds.Pressed(input, Act::Ability) || input.padEvents.b == PadTracker::PRESSED;

    return cmd;
}

void Game::ProcessEvents()
{
    for (const Event& ev : m_events)
    {
        switch (ev.type)
        {
        case Event::Type::Fire:
            // One shared fire sample; heavier weapons play deeper. A little
            // random detune keeps rapid fire from sounding like a loop.
            PlaySoundAt("fire", ev.pos, 0.25f - ev.damage / 120.0f + Rand(-0.05f, 0.05f));
            break;

        case Event::Type::Hit:
            SpawnBlood(ev.pos, ev.dir, ev.damage, ev.fatal);
            PlaySoundAt(ev.fatal ? "death" : "hit", ev.pos);
            // The pad's share of being hit is the one part that is about the
            // player rather than the soldier.
            if (ev.local)
                m_rumbleTime = 0.25f;
            break;

        case Event::Type::Death:
            // The body stays where it fell — everything a ragdoll needs came
            // over in the event, because the soldier it describes is already
            // off the roster.
            SpawnCorpse(ev.pos, ev.dir, ev.walkPhase, ev.moveBlend, TeamColor(ev.team),
                        ev.cls->color, ev.knock);
            break;

        case Event::Type::Detonation:
            if (ev.explodes)
            {
                SpawnExplosion(ev.pos);
                PlaySoundAt("explode", ev.pos, Rand(-0.06f, 0.06f));
            }
            else
            {
                SpawnImpactBurst(ev.pos, ev.radius);
                // Unit hits already play their own hit/death sound; the thud is
                // only for shots stopping in the ground or a wall.
                if (!ev.hitUnit)
                    PlaySoundAt("thud", ev.pos, Rand(-0.15f, 0.15f));
            }
            break;

        case Event::Type::Bounce:
            PlaySoundAt("thud", ev.pos, Rand(0.35f, 0.55f));
            break;

        case Event::Type::ReloadStart:
            // The local player hears their own hands rather than a spot on the
            // floor; everyone else's mag-out happens somewhere in the world.
            if (ev.local)
                m_sound.Play("reload");
            else
                PlaySoundAt("reload", ev.pos);
            break;

        case Event::Type::ReloadEnd:
            // Mag in, a note above mag out.
            if (ev.local)
                m_sound.Play("reload", 1.0f, 0.25f);
            else
                PlaySoundAt("reload", ev.pos, 0.25f);
            break;

        case Event::Type::MeleeSwing:
            // The flash is the local soldier's own arc; anyone else's swing is
            // a sound with no mark until somebody else's swing is worth seeing.
            if (ev.local)
            {
                m_meleeFlash = kMeleeFlashTime;
                m_meleeSwingDir = ev.dir;
            }
            PlaySoundAt("swing", ev.pos, Rand(-0.1f, 0.1f));
            break;

        case Event::Type::AbilityStart:
            // The player's own kit is heard at the listener, the way the
            // reload is; anyone else's is a thing happening in the world.
            if (ev.sound)
            {
                if (ev.local)
                    m_sound.Play(ev.sound);
                else
                    PlaySoundAt(ev.sound, ev.pos);
            }
            break;
        }
    }
    m_events.clear();
}

void Game::Respawn(IsoCamera& camera)
{
    // A respawn is a fresh soldier, not a repaired one: the World issues the
    // whole loadout off the class table the same way it did at the class pick,
    // so there's no list here of things to remember to put back.
    m_world.SpawnLocal();
    m_phase = Phase::Playing;
    // The eye arrives with the body: the camera cut and the first fog polygon
    // both read from here, and neither should spend a frame looking at
    // wherever the last life ended.
    m_eyePos = m_world.Local()->pos;
    // A cut, not a sweep: the spawn is somewhere else entirely, and panning
    // the whole arena to get there would take longer than the wait did.
    camera.SetTarget(m_eyePos);
    camera.SnapToTarget();
}

void Game::PlaySoundAt(const std::string& name, const Vector3& pos, float pitch)
{
    // Past kRange the voice would be silent anyway; skip spawning it.
    if (Vector2(pos.x - m_eyePos.x, pos.z - m_eyePos.z).LengthSquared() <
        Sound::kRange * Sound::kRange)
        m_sound.Play3D(name, pos, pitch);
}

float Game::Rand(float lo, float hi)
{
    return std::uniform_real_distribution<float>(lo, hi)(m_rng);
}

void Game::SpawnImpactBurst(const Vector3& pos, float scale)
{
    constexpr int kBurstCount = 12;
    constexpr XMFLOAT4 kDustColor = { 0.45f, 0.48f, 0.55f, 1.0f };
    for (int i = 0; i < kBurstCount; ++i)
    {
        const float yaw = Rand(0.0f, XM_2PI);
        const float speed = Rand(1.5f, 4.5f);
        Particle p;
        p.pos = pos;
        p.vel = { std::cos(yaw) * speed, Rand(1.0f, 4.5f), std::sin(yaw) * speed };
        p.maxLife = p.life = Rand(0.2f, 0.45f);
        p.size = scale * Rand(0.35f, 0.7f);
        // Roughly half glowing sparks in the tracer color, half neutral dust.
        p.color = (i % 2 == 0) ? kProjectileColor : kDustColor;
        m_particles.push_back(p);
    }
}

void Game::SpawnExplosion(const Vector3& pos)
{
    // Core flash: one big, very short-lived bright cube the bloom pass turns
    // into a glow. Zero velocity — it just pops and shrinks away.
    m_particles.push_back({ pos, Vector3::Zero, 0.09f, 0.09f, 1.4f, { 1.0f, 0.95f, 0.6f, 1.0f } });

    constexpr XMFLOAT4 kFireColor = { 1.0f, 0.55f, 0.15f, 1.0f };
    constexpr XMFLOAT4 kSmokeColor = { 0.30f, 0.30f, 0.33f, 1.0f };
    constexpr int kExplosionCount = 26;
    for (int i = 0; i < kExplosionCount; ++i)
    {
        const float yaw = Rand(0.0f, XM_2PI);
        const float speed = Rand(2.0f, 8.0f);
        Particle p;
        p.pos = pos;
        p.vel = { std::cos(yaw) * speed, Rand(2.0f, 7.0f), std::sin(yaw) * speed };
        p.maxLife = p.life = Rand(0.35f, 0.8f);
        p.size = Rand(0.12f, 0.3f);
        // A mix of embers (tracer yellow), fire, and lingering smoke.
        p.color = i % 3 == 0 ? kSmokeColor : (i % 3 == 1 ? kFireColor : kProjectileColor);
        m_particles.push_back(p);
    }
}

void Game::SpawnBlood(const Vector3& pos, const Vector3& dir, float damage, bool fatal)
{
    // The spray goes the way the blow was travelling — a round that goes
    // through a soldier takes the blood out the far side — spread wide enough
    // that it lands as a scatter rather than a line. How much comes off scales
    // with the damage, and a killing blow lets go of everything at once.
    Vector3 flat(dir.x, 0.0f, dir.z);
    if (flat.LengthSquared() > 1e-8f)
        flat.Normalize();
    else
        flat = Vector3::UnitX; // straight-down blow (a blast overhead): any way will do
    const float heading = std::atan2(flat.z, flat.x);

    int count = std::clamp(static_cast<int>(damage * kBloodDropsPerDamage), kBloodMinDrops,
                           kBloodMaxDrops);
    if (fatal)
        count *= 2;

    for (int i = 0; i < count; ++i)
    {
        const float yaw = heading + Rand(-kBloodSpread, kBloodSpread);
        const float speed = Rand(1.5f, 5.5f);
        Particle p;
        p.pos = pos;
        p.vel = { std::cos(yaw) * speed, Rand(1.5f, 4.5f), std::sin(yaw) * speed };
        p.maxLife = p.life = kBloodDropLife;
        p.size = Rand(0.05f, 0.11f);
        p.color = kBloodColor;
        p.blood = true;
        m_particles.push_back(p);
    }
}

void Game::SpawnSplat(const Vector3& pos)
{
    // Drops that carry over the arena wall have nothing to land on.
    if (std::abs(pos.x) > m_world.ArenaHalf() || std::abs(pos.z) > m_world.ArenaHalf())
        return;

    if (m_splatVerts.size() >= kMaxSplats * kSplatVerts)
        m_splatVerts.erase(m_splatVerts.begin(), m_splatVerts.begin() + kSplatVerts);

    // A ragged fan around the landing point: the rim wanders in and out per
    // segment and the whole patch starts at a random angle, so no two stains
    // read as the same circle stamped twice.
    const XMFLOAT3 center = { pos.x, kSplatHeight, pos.z };
    const float radius = Rand(kSplatMinRadius, kSplatMaxRadius);
    float rim[kSplatSegments];
    for (float& r : rim)
        r = radius * Rand(0.5f, 1.0f);

    const float spin = Rand(0.0f, XM_2PI);
    for (int i = 0; i < kSplatSegments; ++i)
    {
        const float a0 = spin + XM_2PI * i / kSplatSegments;
        const float a1 = spin + XM_2PI * (i + 1) / kSplatSegments;
        const float r0 = rim[i];
        const float r1 = rim[(i + 1) % kSplatSegments];
        m_splatVerts.push_back({ center, kSplatColor });
        m_splatVerts.push_back({ XMFLOAT3{ center.x + std::cos(a0) * r0, kSplatHeight,
                                           center.z + std::sin(a0) * r0 }, kSplatColor });
        m_splatVerts.push_back({ XMFLOAT3{ center.x + std::cos(a1) * r1, kSplatHeight,
                                           center.z + std::sin(a1) * r1 }, kSplatColor });
    }
}

void Game::UpdateParticles(float dt)
{
    for (Particle& p : m_particles)
    {
        p.life -= dt;
        p.vel.y -= 18.0f * dt;
        p.pos += p.vel * dt;
        // Settle on the floor instead of sinking through it.
        const float half = p.size * 0.5f;
        if (p.pos.y < half)
        {
            // A drop of blood is spent the moment it touches down: what's
            // left of it is the stain, and that stays.
            if (p.blood)
            {
                SpawnSplat(p.pos);
                p.life = 0.0f;
                continue;
            }
            p.pos.y = half;
            p.vel = { p.vel.x * 0.6f, 0.0f, p.vel.z * 0.6f };
        }
    }
    std::erase_if(m_particles, [](const Particle& p) { return p.life <= 0.0f; });
}

void Game::SpawnCorpse(const Vector3& pos, const Vector3& aimDir, float walkPhase, float moveBlend,
                       const XMFLOAT4& teamColor, const XMFLOAT4& classColor, const Vector3& knock)
{
    if (m_corpses.size() >= kMaxCorpses)
        RemoveCorpse(0);

    // The ragdoll is built in the pose the soldier was drawn in on its last
    // frame, so death is a body carrying on from the stride it was in rather
    // than a model snapping to a T-pose and dropping.
    XMMATRIX local[Soldier::SegmentCount];
    Soldier::Pose(local, walkPhase, moveBlend);
    const XMMATRIX base = Soldier::Base(pos, aimDir);

    Corpse corpse;
    corpse.teamColor = teamColor;
    corpse.classColor = classColor;
    corpse.life = kCorpseLife;

    XMMATRIX world[Soldier::SegmentCount];
    for (int i = 0; i < Soldier::SegmentCount; ++i)
    {
        world[i] = local[i] * base;
        XMFLOAT3 center;
        XMFLOAT4 rot;
        XMStoreFloat3(&center, world[i].r[3]);
        XMStoreFloat4(&rot, XMQuaternionRotationMatrix(world[i]));

        // Every segment leaves with the same shove, and its own random tumble
        // on top: a corpse that only translated would fall like a statue.
        const XMFLOAT3 spin = { Rand(-kCorpseSpin, kCorpseSpin), Rand(-kCorpseSpin, kCorpseSpin),
                                Rand(-kCorpseSpin, kCorpseSpin) };
        corpse.parts[i] = m_world.Phys().SpawnDebrisBox(center, Soldier::kBodies[i].size, rot,
                                                        knock, spin, Soldier::kBodies[i].mass);
    }

    // Joints are anchored in the parent segment's frame and centered on the
    // child's bone, both taken from the pose the bodies were just built in.
    for (const Soldier::Joint& joint : Soldier::kJoints)
    {
        XMFLOAT3 anchor, boneAxis;
        XMStoreFloat3(&anchor,
                      XMVector3Transform(XMLoadFloat3(&joint.anchor), world[joint.parent]));
        XMStoreFloat3(&boneAxis, XMVector3Normalize(world[joint.child].r[1]));
        m_world.Phys().AddConeJoint(corpse.parts[joint.parent], corpse.parts[joint.child], anchor,
                                    boneAxis, joint.coneAngle, joint.twistAngle);
    }

    m_corpses.push_back(corpse);
}

void Game::UpdateCorpses(float dt)
{
    for (size_t i = m_corpses.size(); i-- > 0;)
    {
        m_corpses[i].life -= dt;
        if (m_corpses[i].life <= 0.0f)
            RemoveCorpse(i);
    }
}

void Game::RemoveCorpse(size_t index)
{
    // Removing a part takes its joints with it, so the ragdoll comes apart in
    // whatever order the segments happen to be in.
    for (Physics::BodyHandle part : m_corpses[index].parts)
        m_world.Phys().RemoveBody(part);
    m_corpses.erase(m_corpses.begin() + index);
}

// Builds the fog overlay: the visibility polygon splits the world into
// angular wedges around the player, and for each boundary run between
// consecutive polygon points the far side gets a dark quad (boundary edge
// extruded radially outward). Wedges partition the plane by angle, so the
// semi-transparent quads never overlap and nothing double-darkens.
void Game::AppendFog(std::vector<Vertex>& out) const
{
    const Vector2 viewer = { m_eyePos.x, m_eyePos.z };
    const std::vector<XMFLOAT2> poly =
        Visibility::ComputePolygon(viewer, m_world.Occluders(), m_world.ArenaHalf());
    if (poly.size() < 2)
        return;

    const float farDist = m_world.ArenaHalf() * kFogFar;
    const auto extrude = [&](const Vector2& v) -> Vector2 {
        const Vector2 d = v - viewer;
        const float len = d.Length();
        if (len < 1e-5f)
            return v;
        return viewer + d * (farDist / len);
    };

    for (size_t i = 0; i < poly.size(); ++i)
    {
        const Vector2 a = poly[i];
        const Vector2 b = poly[(i + 1) % poly.size()];
        const Vector2 af = extrude(a);
        const Vector2 bf = extrude(b);
        const Vertex quad[4] = {
            { XMFLOAT3{ a.x, kFogHeight, a.y }, kFogColor },
            { XMFLOAT3{ b.x, kFogHeight, b.y }, kFogColor },
            { XMFLOAT3{ bf.x, kFogHeight, bf.y }, kFogColor },
            { XMFLOAT3{ af.x, kFogHeight, af.y }, kFogColor },
        };
        out.push_back(quad[0]);
        out.push_back(quad[1]);
        out.push_back(quad[2]);
        out.push_back(quad[0]);
        out.push_back(quad[2]);
        out.push_back(quad[3]);
    }
}

void Game::Render(Renderer& renderer)
{
    // Grayscale for as long as the player is dead, not a flash on the way
    // down: the drained color is what separates watching the arena from
    // playing in it, so it holds until the moment they're back in control.
    renderer.SetMonochrome(m_phase == Phase::Dead);

    if (m_phase == Phase::MainMenu)
    {
        m_mainMenu.Render(renderer);
        return;
    }

    if (m_phase == Phase::KeyBinds)
    {
        m_bindMenu.Render(renderer, m_binds);
        return;
    }

    if (m_phase == Phase::ClassSelect)
    {
        m_classSelect.Render(renderer, m_binds);
        return;
    }

    // Connecting: nothing to draw but the fact of it. The arena arrives with
    // the first snapshot, moments after this screen stops existing.
    if (m_phase == Phase::Connecting)
    {
        std::string text = "CONNECTING TO " + m_connectHost;
        for (char& c : text)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const float size = renderer.Height() * 0.03f;
        renderer.DrawScreenText(text,
                                (renderer.Width() - renderer.MeasureScreenText(text, size)) *
                                    0.5f,
                                renderer.Height() * 0.46f, size, kHudColor);
        return;
    }

    const XMMATRIX identity = XMMatrixIdentity();

    renderer.DrawLines(m_gridVerts.data(), static_cast<uint32_t>(m_gridVerts.size()), identity);

    for (const World::Collider& c : m_world.Colliders())
        if (c.debugDraw)
            renderer.DrawShape(Shape::Box,
                               XMMatrixScaling(c.size.x, c.size.y, c.size.z) *
                                   XMMatrixTranslation(c.center.x, c.center.y, c.center.z),
                               kObstacleColor);

    // The soldiers, off the World's roster. Anyone the player can't see stays
    // hidden — an enemy behind a wall disappears until it re-emerges. The
    // local unit skips both tests: the camera follows them, so they're always
    // on screen, and they are the eye the fog is drawn for. While the player
    // is dead there's no local unit to draw — what's standing at m_eyePos is
    // their corpse, and the view stays on it.
    m_scratch.clear();
    const XMFLOAT2 eye = { m_eyePos.x, m_eyePos.z };
    // The moment between two ticks that this frame is a picture of. Every
    // soldier is drawn at the blend of where the last tick left them and
    // where this one put them; a display running faster than the simulation
    // sees motion, not the simulation's sixty stills a second.
    const float alpha = m_renderAlpha;
    const auto blendDir = [alpha](const Vector3& prev, const Vector3& cur) {
        Vector3 dir = Vector3::Lerp(prev, cur, alpha);
        if (dir.LengthSquared() > 1e-8f)
            dir.Normalize();
        else
            dir = cur; // a half-turn's midpoint has no direction; take the newer one
        return dir;
    };
    for (const Unit& unit : m_world.Units())
    {
        const Vector3 pos = Vector3::Lerp(unit.prevPos, unit.pos, alpha);
        if (unit.controller != Unit::Controller::Local)
        {
            // Cheapest rejection first: the arena is far wider than the view,
            // so most of a large squad is usually off screen entirely.
            if (!renderer.IsSphereVisible({ pos.x, kSoldierBoundsY, pos.z },
                                          kSoldierBoundsRadius))
                continue;
            if (!Visibility::IsPointVisible(eye, { pos.x, pos.z }, m_world.Occluders()))
                continue;
        }
        DrawSoldier(renderer, pos, blendDir(unit.prevAimDir, unit.aimDir),
                    unit.prevWalkPhase + (unit.walkPhase - unit.prevWalkPhase) * alpha,
                    unit.prevMoveBlend + (unit.moveBlend - unit.prevMoveBlend) * alpha,
                    unit.team, unit.cls->color);
    }
    // Corpses, drawn from their ragdolls: the same model as a living soldier,
    // with every segment placed by the physics body it was built from. Same
    // culling as the living, tested at the pelvis — where a body ends up is the
    // ragdoll's business, so there's no single position to key off otherwise.
    for (const Corpse& corpse : m_corpses)
    {
        const Physics::Transform pelvis =
            m_world.Phys().GetTransform(corpse.parts[Soldier::Pelvis]);
        if (!renderer.IsSphereVisible(pelvis.pos, kSoldierBoundsRadius))
            continue;
        if (!Visibility::IsPointVisible(eye, { pelvis.pos.x, pelvis.pos.z }, m_world.Occluders()))
            continue;

        // Sinking is a drawing trick, not a physical one: the bodies stay put
        // (asleep, by then) and the model is drawn further under the floor each
        // frame until it's gone.
        const float sink =
            std::max(0.0f, kCorpseSink - corpse.life) / kCorpseSink * kCorpseSinkDepth;

        XMMATRIX world[Soldier::SegmentCount];
        for (int i = 0; i < Soldier::SegmentCount; ++i)
        {
            const Physics::Transform t = m_world.Phys().GetTransform(corpse.parts[i]);
            world[i] = XMMatrixRotationQuaternion(XMLoadFloat4(&t.rot)) *
                       XMMatrixTranslation(t.pos.x, t.pos.y - sink, t.pos.z);
        }
        Soldier::Draw(renderer, world, corpse.teamColor, corpse.classColor);
    }

    for (const World::Projectile& shot : m_world.Projectiles())
    {
        const Vector3& pos = shot.pos;
        if (Visibility::IsPointVisible(eye, { pos.x, pos.z }, m_world.Occluders()))
        {
            // Per-shot radius, not the class's: a thrown grenade is a fatter
            // projectile than most primaries fire.
            const float d = shot.radius * 2.0f;
            renderer.DrawShape(Shape::Sphere,
                               XMMatrixScaling(d, d, d) *
                                   XMMatrixTranslation(pos.x, pos.y, pos.z),
                               ProjectileColor(shot.life, shot.fused));
        }
    }

    // Impact debris shrinks out over its lifetime. Same visibility rule as
    // projectiles: a burst behind a wall stays hidden.
    for (const Particle& p : m_particles)
    {
        if (!Visibility::IsPointVisible(eye, { p.pos.x, p.pos.z }, m_world.Occluders()))
            continue;
        const float s = p.size * (p.life / p.maxLife);
        AppendCube(m_scratch, p.pos, { s, s, s }, p.color);
    }

    renderer.DrawTriangles(m_scratch.data(), static_cast<uint32_t>(m_scratch.size()), identity);

    for (const Prop& prop : m_props)
    {
        const XMMATRIX world = XMMatrixScaling(prop.scale, prop.scale, prop.scale) *
                               XMMatrixRotationY(prop.yaw) *
                               XMMatrixTranslation(prop.pos.x, prop.pos.y, prop.pos.z);
        renderer.DrawModel(*prop.model, world);
    }

    // Blood, laid flat on the floor. It goes down after every opaque thing has
    // written depth, so a stain behind a wall is hidden by the wall rather than
    // painted over it, and before the fog, so blood out of sight is dimmed
    // along with the ground it's on.
    renderer.DrawTrianglesAlpha(m_splatVerts.data(), static_cast<uint32_t>(m_splatVerts.size()),
                                identity);

    // Fog of war goes on after all opaque geometry so it blends over the
    // floor while walls (which wrote depth) still punch through it.
    m_fogVerts.clear();
    AppendFog(m_fogVerts);
    renderer.DrawTrianglesAlpha(m_fogVerts.data(), static_cast<uint32_t>(m_fogVerts.size()),
                                identity);

    // What the player's ability lets them see that nobody else does — for the
    // medic, squadmate health over the heads of the wounded. Which soldiers are
    // in that list is settled by the World's AbilityScene, so a mark can never
    // be the thing that tells a medic where somebody is standing: it draws over
    // the ones they can already see and nobody else. What it draws is the
    // ability's own business, and no kind of it is named here.
    if (m_phase == Phase::Playing)
    {
        Unit& u = *m_world.Local();
        m_scratch.clear();
        Ability::AppendVision(u.cls->ability, m_world.AbilityScene(u), m_screenRight, m_scratch);
        renderer.DrawTrianglesAlpha(m_scratch.data(), static_cast<uint32_t>(m_scratch.size()),
                                    identity);
    }

    // Aim indicator: the shot's actual trajectory as a dim 3D polyline — an
    // arch for the grenade's lob, a near-level line with droop for bullets —
    // ending where the shot really stops (max range, the first wall the arc
    // can't clear, or the ground). A bright tick crosses the end point, lobbed
    // weapons get a landing circle, and the grenade gets a marker where the
    // throw touches down. Drawn after the fog pass so it stays bright. There's
    // nothing to aim while dead, so it goes with the soldier. The whole
    // indicator greys out through a reload — where the shot would land is still
    // worth showing, but not as something the player can act on yet.
    if (m_phase == Phase::Playing)
    {
        Unit& u = *m_world.Local();
        // The indicators hang off the soldier as drawn, so the aim line grows
        // out of the muzzle on screen rather than out of where the simulation
        // has quietly moved it.
        const Vector3 lpos = Vector3::Lerp(u.prevPos, u.pos, alpha);
        const Vector3 laim = blendDir(u.prevAimDir, u.aimDir);
        const bool reloading = u.reloadTimer > 0.0f;
        const XMFLOAT4 aimColor = reloading ? kAimSpentColor : kAimColor;
        const XMFLOAT4 aimDim = reloading ? kAimSpentDimColor : kAimDimColor;

        m_world.PredictShotStop(u.cls->primary, lpos, laim, m_aimDist, &m_aimArc);

        m_scratch.clear();
        for (size_t i = 1; i < m_aimArc.size(); ++i)
        {
            m_scratch.push_back({ m_aimArc[i - 1], aimDim });
            m_scratch.push_back({ m_aimArc[i], aimDim });
        }

        // End-of-flight tick, perpendicular to the aim direction, at the
        // height of the impact (on a wall it floats at the hit point).
        const Vector3 end = m_aimArc.back();
        const float px = -laim.z * 0.45f, pz = laim.x * 0.45f;
        m_scratch.push_back({ { end.x - px, end.y, end.z - pz }, aimColor });
        m_scratch.push_back({ { end.x + px, end.y, end.z + pz }, aimColor });

        if (u.cls->primary.lobVelocity > 0.0f)
            AppendCircle(m_scratch, end, 0.4f, aimColor);

        // Grenade marker: where the throw first touches down. Deliberately not
        // the blast radius — the grenade bounces and rolls on from here before
        // its fuse ends it, so a ring sized to the blast would promise a
        // detonation point nothing can predict. What it does show honestly is
        // the throw's reach, so a lob falling short of the cursor is visible
        // before it leaves the hand. Gone for good once the grenade is spent,
        // which is the in-world half of the HUD's count.
        if (u.grenades > 0)
        {
            const float dist = m_world.PredictShotStop(kGrenade, lpos, laim, m_aimDist);
            const Vector3 land(lpos.x + laim.x * dist, kAimRingHeight,
                               lpos.z + laim.z * dist);
            AppendCircle(m_scratch, land, kGrenadeMarkRadius, kGrenadeAimColor);
        }

        // The swing that just happened: the blade's arc at its real reach,
        // frozen where it was aimed and fading out over the flash. It's the
        // only account of a melee there is — a miss makes no impact, spills no
        // blood, and would otherwise leave nothing on screen to have missed.
        if (m_meleeFlash > 0.0f)
        {
            const XMFLOAT4 color = { kMeleeArcColor.x, kMeleeArcColor.y, kMeleeArcColor.z,
                                     kMeleeArcColor.w * (m_meleeFlash / kMeleeFlashTime) };
            const float heading = std::atan2(m_meleeSwingDir.z, m_meleeSwingDir.x);
            const Vector3 center(lpos.x, kAimRingHeight, lpos.z);
            AppendArc(m_scratch, center, kMelee.reach, heading - kMelee.arc, kMelee.arc * 2.0f, 12,
                      color);
        }

        // Whatever the ability has to say about itself: how much of it is left
        // to run, and who it's reaching. It goes in this batch because it's the
        // same kind of thing as the aim line and the swing arc — a mark on the
        // world about what this soldier is doing — but what those marks are is
        // the ability's own business.
        Ability::AppendIndicator(u.cls->ability, u.ability, lpos, m_scratch);

        // Friend or foe, for everyone but the player: a ring under the soldiers
        // on their own side. Same visibility rule the bodies themselves get —
        // a marker that showed through a wall would be a squadmate radar, which
        // is a different feature and one the fog of war exists to deny.
        const XMFLOAT4& teamTint = TeamColor(m_team);
        const XMFLOAT4 ringColor = { teamTint.x, teamTint.y, teamTint.z, kFriendlyRingAlpha };
        for (const Unit& other : m_world.Units())
        {
            if (other.team != m_team || other.controller == Unit::Controller::Local)
                continue;
            const Vector3 pos = Vector3::Lerp(other.prevPos, other.pos, alpha);
            if (!renderer.IsSphereVisible({ pos.x, kSoldierBoundsY, pos.z },
                                          kSoldierBoundsRadius))
                continue;
            if (!Visibility::IsPointVisible(eye, { pos.x, pos.z }, m_world.Occluders()))
                continue;
            AppendCircle(m_scratch, { pos.x, kAimRingHeight, pos.z }, kFriendlyRingRadius,
                         ringColor);
        }

        renderer.DrawLinesAlpha(m_scratch.data(), static_cast<uint32_t>(m_scratch.size()),
                                identity);
    }

    RenderHud(renderer);
}

// Screen-space overlay: the gameplay cluster (Hud.cpp), the respawn countdown,
// and the perf counters.
void Game::RenderHud(Renderer& renderer)
{
    const float w = static_cast<float>(renderer.Width());
    const float h = static_cast<float>(renderer.Height());
    const float size = h * 0.024f;

    // The loadout readout is a snapshot handed over whole, so the HUD reads
    // nothing out of the game itself. Reload progress runs 0 -> 1 rather than
    // counting seconds down, because that's what a bar wants; a negative value
    // means the magazine is in and there's nothing to wait for.
    //
    // While the player is dead there's no unit to read a loadout off, and the
    // zeros say so honestly: the cluster is faded (alive, below) and a dead
    // soldier is holding nothing anyway.
    const Unit* u = m_world.Local();
    Hud::State hud = {};
    hud.maxHp = World::kMaxHealth;
    hud.magazine = m_class->primary.magazine;
    hud.meleeCharges = kMelee.charges;
    hud.reloadFraction = -1.0f;
    hud.meleeRecoverFraction = -1.0f;
    hud.abilityFraction = -1.0f;
    if (u)
    {
        hud.hp = u->hp;
        hud.ammo = u->ammo;
        hud.reloadFraction =
            u->reloadTimer > 0.0f && u->cls->primary.reloadTime > 0.0f
                ? 1.0f - u->reloadTimer / u->cls->primary.reloadTime
                : -1.0f;
        hud.melee = u->meleeCharges;
        // The recovery runs after every swing, including the ones that leave
        // charges in hand, but a refill the player can swing straight through
        // isn't a wait and isn't worth a bar. What the pips say — how many
        // swings are there right now — is the whole answer until there are
        // none, and only then does the clock become the thing to read.
        hud.meleeRecoverFraction =
            u->meleeCharges == 0 && u->meleeRecover > 0.0f && kMelee.recoverTime > 0.0f
                ? 1.0f - u->meleeRecover / kMelee.recoverTime
                : -1.0f;
        hud.grenades = u->grenades;
        hud.abilityFraction = u->ability.time > 0.0f && u->cls->ability.duration > 0.0f
                                  ? 1.0f - u->ability.time / u->cls->ability.duration
                                  : -1.0f;
        hud.abilityCooldown = u->ability.cooldown;
    }
    // The ability module is the class's own, so it's handed over as the whole
    // definition: the HUD needs its name for the key hint and its cooldown to
    // turn the seconds left into a bar. A class without one hands over nothing
    // and gets no module, rather than getting an empty one — an ability isn't
    // equipment that ran out. It comes off the class rather than the unit so
    // the module doesn't blink out of the cluster for the respawn wait.
    hud.ability = m_class->ability.kind != Ability::Kind::None ? &m_class->ability : nullptr;
    // The roster, both sides counted the same way: everyone standing. On one
    // roster that isn't an adjustment to make, it's just counting — the player
    // is one of the soldiers on their own side, which is what makes the two
    // rows comparable: five against five reads as five against five, and a
    // side is short exactly when somebody on it is waiting to come back.
    const std::vector<Unit>& units = m_world.Units();
    hud.allies =
        static_cast<int>(std::count_if(units.begin(), units.end(), [this](const Unit& n) {
            return n.team == m_team && n.hp > 0.0f;
        }));
    hud.enemies =
        static_cast<int>(std::count_if(units.begin(), units.end(), [this](const Unit& n) {
            return n.team != m_team && n.hp > 0.0f;
        }));
    hud.teamSize = World::kTeamSize;
    // The same two colors the soldiers are wearing, so the corner and the field
    // agree. With two sides the enemy is simply the other one; a third team
    // would need this to be something better than "not mine", but it would need
    // a third row too, and the panel is built for a pair.
    hud.allyColor = TeamColor(m_team);
    hud.enemyColor = TeamColor(m_team + 1);
    hud.accent = m_class->color;
    hud.alive = m_phase == Phase::Playing;

    // The key-cap row. The ability leads it, named after what it does rather
    // than after the slot it sits in — FIELD DRESSING says more about the class
    // than ABILITY ever would, and it's the only cap here a player might not
    // already know. The rest is the standard kit every soldier carries.
    //
    // The caps are the player's own bindings, so a rebind is on the HUD the
    // moment they leave the settings screen and nothing here has to be told.
    Hud::Hint hints[kMaxHints];
    size_t hintCount = 0;
    const auto addHint = [&](std::string key, const char* label) {
        m_hintKeys[hintCount] = std::move(key);
        hints[hintCount] = { m_hintKeys[hintCount].c_str(), label };
        ++hintCount;
    };
    if (hud.ability)
        addHint(m_binds.Label(Act::Ability), m_class->ability.name);
    addHint(m_binds.Label(Act::Reload), "RELOAD");
    addHint(m_binds.Label(Act::Grenade), "GRENADE");
    addHint(m_binds.Label(Act::Melee), "MELEE");
    addHint(m_binds.Label(Act::Steady), "STEADY");

    hud.hints = hints;
    hud.hintCount = hintCount;
    Hud::Render(renderer, hud);

    // Respawn countdown, centered and big: while it's up there's nothing else
    // to do, so it's the one thing on screen worth reading. Counts whole
    // seconds remaining, so it reaches 1 for the last second and never shows a
    // 0 the player can't act on.
    if (m_phase == Phase::Dead)
    {
        const std::string countdown =
            "RESPAWNING IN " + std::to_string(static_cast<int>(std::ceil(m_respawnTimer)));
        const float countSize = size * 1.6f;
        renderer.DrawScreenText(countdown,
                                (w - renderer.MeasureScreenText(countdown, countSize)) * 0.5f,
                                h * 0.42f, countSize, kHudColor);
    }

    // Perf counters in the top-left corner, clear of the gameplay HUD. FRAME is
    // wall clock and pins to the refresh rate while there's headroom; CPU
    // (command recording) and the draw count respond to render-side changes.
    // Half the gameplay HUD's size: it's a debug readout, not something to read
    // mid-fight, so it stays legible without competing for attention.
    const float perfSize = size * 0.5f;
    char perf[96];
    std::snprintf(perf, sizeof(perf), "FRAME %.1fMS  CPU %.2fMS  DRAWS %u", m_frameMs,
                  renderer.CpuFrameMs(), renderer.LastDrawCalls());
    renderer.DrawScreenText(perf, size, size, perfSize, kHudHintColor);
}
