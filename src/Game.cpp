#include "Game.h"

#include "Hud.h"
#include "Level.h"
#include "Net.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <exception>
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

    // The ground: jungle floor rather than the blue-grey the blockout arena
    // wore. The grid still says how far away things are, it just does it in
    // the colors of the place it's drawn on.
    constexpr XMFLOAT4 kGridMinor = { 0.09f, 0.15f, 0.10f, 1.0f };
    constexpr XMFLOAT4 kGridMajor = { 0.14f, 0.24f, 0.15f, 1.0f };
    constexpr XMFLOAT4 kBorder = { 0.45f, 0.30f, 0.16f, 1.0f };
    // Colliders with no model to draw them — the trench lines around the
    // bases, today — are dug earth.
    constexpr XMFLOAT4 kObstacleColor = { 0.40f, 0.33f, 0.22f, 1.0f };
    constexpr XMFLOAT4 kProjectileColor = { 1.00f, 0.80f, 0.20f, 1.0f };
    // A live grenade reads as a blinking casing rather than a tracer, so it
    // can't be mistaken for a bullet while it bounces.
    constexpr XMFLOAT4 kGrenadeLiveColor = { 1.00f, 0.30f, 0.12f, 1.0f };
    constexpr XMFLOAT4 kGrenadeCasingColor = { 0.32f, 0.30f, 0.28f, 1.0f };
    constexpr float kGrenadeBlinkPeriod = 0.24f; // seconds per on/off cycle
    constexpr float kGrenadeFlashLife = 0.4f;    // fuse left when it goes solid bright
    // The landing circle a lobbed primary would get. It's the last colored
    // thing the aim indicator has left — the line and its head are grey (see
    // kAimDotColor) — and no class carries a lobbed primary today, so nothing
    // draws it yet.
    constexpr XMFLOAT4 kAimColor = { 0.95f, 0.95f, 0.40f, 0.55f };
    // Drained of color while the magazine is out: the mark still tracks the
    // cursor, but nothing will come out of the barrel until the reload ends,
    // and that has to be visible without looking away from the fight.
    constexpr XMFLOAT4 kAimSpentColor = { 0.62f, 0.64f, 0.68f, 0.45f };
    // The dead ground in front of a weapon with a minimum range — the sniper's
    // and nobody else's. The ring is the honest shape of the rule: it's a
    // radius, not a point on the line, so a body anywhere inside it is one the
    // weapon passes through. Where it crosses the dots is the near edge of the
    // class's reach, and the dots inside it are faded to say which side of the
    // crossing is which.
    constexpr XMFLOAT4 kDeadRangeColor = { 0.62f, 0.64f, 0.68f, 0.30f };
    constexpr float kAimRingHeight = 0.05f; // above the fog quads, so it isn't dimmed
    // The indicator itself — the pips and the head that ends them. Small solid
    // marks, far enough apart to read as a run of dots rather than a dashed
    // line, and grey rather than any color of its own: the line is a thing to
    // glance at and never a thing to look past, so it's kept quiet enough that
    // the ground it crosses is still the brighter of the two.
    constexpr XMFLOAT4 kAimDotColor = { 0.72f, 0.74f, 0.78f, 0.30f };
    // The same grey, fainter, for a mark that can't be acted on: the pips
    // inside a minimum range, and the whole indicator through a reload. It's
    // the only channel a colorless line has left to say so with, and it says
    // the right thing — less mark for less weapon.
    constexpr XMFLOAT4 kAimDotFadedColor = { 0.72f, 0.74f, 0.78f, 0.14f };
    constexpr float kAimDotSpacing = 0.9f;
    constexpr float kAimDotRadius = 0.055f;
    constexpr int kAimDotSegments = 6; // a filled hexagon is a dot at arena distance
    // The head on the far end. Its point sits exactly on the stop, so the arrow
    // is read the way an arrow is read — the tip is the claim, and the body of
    // it lies back down the line the shot came along rather than out past the
    // last ground the round can reach.
    constexpr float kAimArrowLength = 0.6f;
    constexpr float kAimArrowHalfWidth = 0.26f;
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
    // Your own side's spawn, ringed on the ground it covers. The class change
    // key works inside this circle and nowhere else on the field, so where
    // "inside" is has to be a place the player can see rather than a radius
    // they have to guess at — and the ring brightens when they're standing in
    // it, which is the rule stated where they are instead of in a menu. Drawn
    // in the team's own color, like the rings under squadmates, because it's
    // the same claim about the same side.
    constexpr float kSpawnRingAlpha = 0.16f;
    constexpr float kSpawnRingAlphaLive = 0.55f;
    constexpr int kSpawnRingSegments = 64; // eight units across wants more than a shot's ring
    // Wet and bright in the air, dark and matte once it has soaked into the
    // floor. The stain is translucent, so overlapping ones deepen where a fight
    // stayed in one place.
    constexpr XMFLOAT4 kBloodColor = { 0.62f, 0.06f, 0.07f, 1.0f };
    constexpr XMFLOAT4 kSplatColor = { 0.30f, 0.03f, 0.04f, 0.85f };
    constexpr XMFLOAT4 kFogColor = { 0.01f, 0.02f, 0.04f, 0.85f };
    constexpr XMFLOAT4 kHudColor = { 0.85f, 0.90f, 0.95f, 1.0f };
    constexpr XMFLOAT4 kHudHintColor = { 0.45f, 0.52f, 0.62f, 1.0f };
    // A player's name, under the boots of the soldier they're driving. Green
    // for your side and red for theirs rather than the two teams' own colors,
    // which is the one mark on the field that breaks that rule and is worth
    // saying why: armor answers "which side is that", and it has to be the same
    // answer on every screen, so the same body is blue to both of the people
    // looking at it. A name answers "is that one of mine", which is a question
    // about whoever is reading it — so it is drawn from the reader's end, and
    // the two marks aren't in competition because they aren't answering the
    // same question. Green and red because that pair is read before it is
    // looked at, which is the only speed a label over a fight is any use at.
    //
    // Small, because there can be ten of them on screen at once and not one is
    // worth taking an eye off the fight for. About the size of the perf readout
    // in the corner: there when looked at, quiet when not.
    constexpr XMFLOAT4 kNameFriendlyColor = { 0.42f, 0.92f, 0.46f, 1.0f };
    constexpr XMFLOAT4 kNameEnemyColor = { 1.00f, 0.38f, 0.36f, 1.0f };
    constexpr float kNameSize = 0.0115f; // cap height, as a share of the window's
    constexpr float kNameDrop = 0.013f;  // boots -> the top of the name

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

    // Mixes `f` of the way to white. A team color is chosen to read as armor
    // at arena distance; the same color set as text over a lit floor wants
    // more light than that, and lifting it leaves the hue — which is the part
    // that says which side won — untouched.
    XMFLOAT4 Brighten(const XMFLOAT4& c, float f)
    {
        return { c.x + (1.0f - c.x) * f, c.y + (1.0f - c.y) * f, c.z + (1.0f - c.z) * f, c.w };
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

    // One dot of the aim indicator: a small filled disc lying flat on the floor
    // at the height every other ground mark uses, wound as a fan from its
    // center. Solid rather than an outline because at this size an outline is
    // mostly the hole in the middle of it — a pip either reads as a dot or it
    // reads as nothing. Triangles, so these go in a batch of their own.
    void AppendDot(std::vector<Vertex>& out, float x, float z, const XMFLOAT4& color)
    {
        const XMFLOAT3 center{ x, kAimRingHeight, z };
        for (int i = 0; i < kAimDotSegments; ++i)
        {
            const float a0 = XM_2PI * i / kAimDotSegments;
            const float a1 = XM_2PI * (i + 1) / kAimDotSegments;
            out.push_back({ center, color });
            out.push_back({ XMFLOAT3{ x + std::cos(a0) * kAimDotRadius, kAimRingHeight,
                                      z + std::sin(a0) * kAimDotRadius }, color });
            out.push_back({ XMFLOAT3{ x + std::cos(a1) * kAimDotRadius, kAimRingHeight,
                                      z + std::sin(a1) * kAimDotRadius }, color });
        }
    }

    // The arrow head that ends the aim indicator: one filled triangle lying in
    // the dots' plane, its point on `tip` and its base squared off across
    // `dir`. It replaces the bar that used to cross the end — a bar says where
    // the line stops but not which way it was going, and on a flat indicator
    // that direction is worth saying twice. It carries the dots' own grey, so
    // what marks it as the end is its shape and nothing else.
    void AppendArrowHead(std::vector<Vertex>& out, const Vector3& tip, const Vector3& dir,
                         const XMFLOAT4& color)
    {
        const float bx = tip.x - dir.x * kAimArrowLength;
        const float bz = tip.z - dir.z * kAimArrowLength;
        const float px = -dir.z * kAimArrowHalfWidth, pz = dir.x * kAimArrowHalfWidth;
        out.push_back({ XMFLOAT3{ tip.x, tip.y, tip.z }, color });
        out.push_back({ XMFLOAT3{ bx - px, tip.y, bz - pz }, color });
        out.push_back({ XMFLOAT3{ bx + px, tip.y, bz + pz }, color });
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
    m_settings.Load(); // same terms: a missing or broken file means the defaults

    // Pointed at somebody else's server from the command line: start the
    // handshake now, behind the menus, so by the time a class is picked the
    // socket is usually already up.
    if (!m_connectHost.empty())
        Connect(m_connectHost, Net::kPort);

    // With a class named on the command line the menus are skipped entirely —
    // launch to firefight in one step, which is what iterating on netcode
    // wants. Without a host to go with it that's a match on this machine, so
    // the same shortcut works for playing on your own.
    for (size_t i = 0; i < kClassCount; ++i)
        if (_stricmp(kClassDefs[i].name, m_autoClass.c_str()) == 0)
        {
            m_class = &kClassDefs[i];
            if (!m_net)
                HostMatch();
            m_phase = Phase::Connecting;
            break;
        }

    m_sound.Init(); // loads the wave bank and starts the ambience

    const LevelData level = LevelData::Load("assets/levels/hardcorps2t.json");
    m_team = std::min(m_team, static_cast<int>(level.spawns.size()) - 1);

    // The level splits down the same seam everything else does: the World
    // takes the parts that decide anything (colliders, occluders, spawns),
    // and what's left here is what the level looks like.
    m_world.Init(level);

    // Static floor grid. Each axis is ruled to its own half-extent, since the
    // arena is a rectangle: a square grid on a long map would either stop short
    // of the ends or hang off the sides.
    const Vector2 arenaHalf = m_world.ArenaHalf();
    const int halfX = static_cast<int>(arenaHalf.x);
    const int halfZ = static_cast<int>(arenaHalf.y);
    for (int i = -halfX; i <= halfX; ++i)
    {
        const bool border = (i == -halfX || i == halfX);
        const bool major = (i % 8) == 0;
        const XMFLOAT4 col = border ? kBorder : (major ? kGridMajor : kGridMinor);
        const float f = static_cast<float>(i);
        m_gridVerts.push_back({ XMFLOAT3{ f, 0.0f, -arenaHalf.y }, col });
        m_gridVerts.push_back({ XMFLOAT3{ f, 0.0f, arenaHalf.y }, col });
    }
    for (int i = -halfZ; i <= halfZ; ++i)
    {
        const bool border = (i == -halfZ || i == halfZ);
        const bool major = (i % 8) == 0;
        const XMFLOAT4 col = border ? kBorder : (major ? kGridMajor : kGridMinor);
        const float f = static_cast<float>(i);
        m_gridVerts.push_back({ XMFLOAT3{ -arenaHalf.x, 0.0f, f }, col });
        m_gridVerts.push_back({ XMFLOAT3{ arenaHalf.x, 0.0f, f }, col });
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
    // Same order as LeaveMatch, for the same reason: the host goes down
    // first, so the goodbye it sends is what the client's hang-up reads
    // instead of a timeout.
    m_server.reset();
    if (m_net)
        m_net->Disconnect();
    m_sound.Shutdown();
}

void Game::HostMatch()
{
    // Loopback and a port the OS picks: this match is for the person sitting
    // at this machine, so it isn't listed on the LAN and isn't reachable from
    // off the box — and a socket bound to 127.0.0.1 is one Windows never
    // raises a firewall prompt about. The well-known port is left alone so a
    // real server can go on running here at the same time.
    Server::Config config;
    config.port = 0;
    config.loopbackOnly = true;
    config.discoverable = false;
    config.log = false;

    auto server = std::make_unique<Server>();
    uint16_t port = Net::kPort;
    try
    {
        if (server->Start(config))
        {
            port = server->Port();
            m_server = std::move(server);
        }
    }
    catch (const std::exception&)
    {
        // A level that won't load, on ground this client already loaded once.
        // Nothing to do here that the failed connection below won't do better.
    }
    Connect("127.0.0.1", port);
}

void Game::Connect(const std::string& host, uint16_t port)
{
    m_connectHost = host;
    m_net = std::make_unique<NetClient>();
    m_net->Start(m_connectHost, port);
    m_joinSent = false;
}

void Game::LeaveMatch()
{
    // A match this machine was hosting had exactly one player in it, and they
    // have just left: the server goes with them, and the port goes back. It
    // goes first so its own goodbye is already sitting in the client's socket
    // when the client says its own — otherwise the polite hang-up below waits
    // out its timeout for an answer from a server that is no longer being
    // ticked, and leaving a match costs a visible hitch.
    m_server.reset();
    if (m_net)
        m_net->Disconnect();
    m_net.reset();
    m_connectHost.clear();
    m_autoClass.clear();
    m_joinSent = false;
    m_myUnitId = -1;
    m_camSnapPending = false;
    m_hasPredicted = false;
    m_pendingCmds.clear();
    m_pendingCmd = {};
    m_events.clear();
    ClearBattlefield();
    m_world.Reset();
    m_class = nullptr;
    m_classChangeOpen = false;
    m_respawnTimer = 0.0f;
    m_tickAccum = 0.0f;
    m_renderAlpha = 0.0f;
    m_snapElapsed = 0.0f;
    m_phase = Phase::MainMenu;
}

void Game::ClearBattlefield()
{
    // The corpses give their bodies back to the physics world, the weather in
    // the air goes out, and the floor gives up its history.
    while (!m_corpses.empty())
        RemoveCorpse(m_corpses.size() - 1);
    m_particles.clear();
    m_splatVerts.clear();
    m_meleeFlash = 0.0f;
}

void Game::NetPump(float dt, IsoCamera& camera)
{
    // A match hosted here takes its turn of the frame now, between the
    // commands this frame staged and the answers about to be read. The flush
    // is what makes that worth doing: the commands are put on the wire before
    // the server looks at it, so a keypress is consumed, simulated, and
    // answered inside the frame that made it rather than the one after — the
    // whole round trip in the time it takes to call two functions.
    if (m_server)
    {
        m_net->Flush();
        m_server->Update(dt);
    }

    std::vector<std::vector<uint8_t>> packets;
    m_net->Poll(packets);

    // The far end going away is the end of the session, however far into it
    // we were — but it's the end of the session, not the game: back to the
    // menu, where Deploy still starts a solo match on the same ground.
    if (m_net->GetStatus() == NetClient::Status::Failed)
    {
        LeaveMatch();
        return;
    }

    for (const std::vector<uint8_t>& packet : packets)
    {
        Net::Reader r(packet.data(), packet.size());
        switch (static_cast<Net::MsgType>(r.U8()))
        {
        // Welcome and Respawned both say the same thing — here is your
        // soldier — and neither of them puts the player in the arena. That
        // waits for the snapshot below: a name without a position is nothing
        // to draw, and a screen that went Playing on the strength of one
        // would spend a frame looking for a soldier the roster hasn't
        // mentioned yet. A frame or two either way, and never a bet on which
        // packet lands first.
        case Net::MsgType::Welcome:
        {
            m_myUnitId = r.I32();
            m_team = r.U8();
            m_camSnapPending = true;
            m_hasPredicted = false;
            m_pendingCmds.clear();
            break;
        }
        case Net::MsgType::Respawned:
        {
            m_myUnitId = r.I32();
            m_camSnapPending = true;
            m_hasPredicted = false;
            m_pendingCmds.clear();
            break;
        }
        // The class change, granted. It arrives on its own while the player is
        // waiting to respawn — the soldier they're owed will be this class when
        // it comes — and just ahead of a Respawned when they were standing on
        // their spawn and have just traded one soldier for another.
        //
        // Taken from the server rather than assumed at the press, because
        // everything on this side reads the class for a whole life: the HUD's
        // magazine, the aim indicator's weapon, and how far this player's own
        // eye sees, which is the one the fog is cut with. A client that changed
        // it on the press and was refused would be drawing a different match to
        // the one the server was sending it.
        case Net::MsgType::ClassChanged:
        {
            const uint8_t classId = r.U8();
            if (r.ok && classId < kClassCount)
                m_class = &kClassDefs[classId];
            break;
        }
        case Net::MsgType::Snapshot:
        {
            const Net::Snapshot snap = Net::ReadSnapshot(r);
            if (!r.ok)
                break;
            // A match ending and the next one starting are both facts about
            // the server's clock, and both arrive here. The only one this
            // side has to act on is the new match: it starts on ground the
            // last one left bodies and blood all over.
            const bool wasOver = m_world.MatchOver();
            m_world.ApplySnapshot(snap, m_myUnitId);
            if (wasOver && !m_world.MatchOver())
                ClearBattlefield();
            m_snapElapsed = 0.0f;
            // The cut a spawn deserves, taken on the first snapshot that
            // actually has us in it — Welcome says who we are, but only a
            // snapshot says where. It's also where the prediction is born:
            // from here the soldier under the keys is this machine's copy,
            // and the server's is what it gets squared against. And it's
            // where the player enters the arena: Phase::Playing means there
            // is a soldier of ours standing on the field, which is what every
            // path that reads one already assumed.
            if (m_camSnapPending)
                if (const Unit* me = m_world.Local())
                {
                    m_eyePos = me->pos;
                    camera.SetTarget(m_eyePos);
                    camera.SnapToTarget();
                    m_camSnapPending = false;
                    m_predicted = *me;
                    m_hasPredicted = true;
                    if (m_phase == Phase::Connecting || m_phase == Phase::Dead)
                        m_phase = Phase::Playing;
                }
            if (snap.own.has && m_hasPredicted)
                Repredict(snap.own.ackSeq);
            break;
        }
        case Net::MsgType::Events:
            Net::ReadEvents(r, m_myUnitId, m_events);
            break;
        case Net::MsgType::Reject:
            // The door, closed — wrong build or a full house. Either way the
            // session never was; the menu is where we came from.
            LeaveMatch();
            return;
        default:
            break;
        }
    }
}

void Game::Repredict(uint32_t ackSeq)
{
    Unit* me = m_world.Local();
    if (!me)
    {
        m_hasPredicted = false;
        return;
    }

    // Everything the server has consumed is history; what's left is still in
    // flight, and gets replayed onto the server's own answer.
    while (!m_pendingCmds.empty() && m_pendingCmds.front().seq <= ackSeq)
        m_pendingCmds.pop_front();

    m_predicted = *me;
    m_predicted.prevPos = m_predicted.pos;
    m_predicted.prevAimDir = m_predicted.aimDir;
    m_predicted.prevWalkPhase = m_predicted.walkPhase;
    m_predicted.prevMoveBlend = m_predicted.moveBlend;
    for (const PendingCmd& pending : m_pendingCmds)
    {
        m_predicted.prevPos = m_predicted.pos;
        m_predicted.prevAimDir = m_predicted.aimDir;
        m_predicted.prevWalkPhase = m_predicted.walkPhase;
        m_predicted.prevMoveBlend = m_predicted.moveBlend;
        m_world.MoveCommand(m_predicted, pending.cmd, World::kTickDt);
    }
}

void Game::Update(float dt, const Input& input, IsoCamera& camera)
{
    // Recomputed every frame rather than latched when either half changes,
    // because both halves move: the window is activated and deactivated by the
    // player, and the setting is flipped on a screen of ours. One line that
    // reads both is one place the two can't fall out of step, and Sound only
    // touches the engine when the answer is actually different.
    m_sound.SetMuted(!m_windowFocused && !m_settings.backgroundAudio);
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
            case MainMenu::Choice::Deploy:
                // Deploying opens a match on this machine and joins it, which
                // is the same two steps as JOIN with the walk to another
                // computer taken out. The server starts here rather than at
                // the class pick so the handshake — and the first squads
                // coming up to strength — happen behind the class card.
                //
                // Unless the command line already pointed us somewhere: a
                // connection in flight is the fight this game was launched to
                // be in, and DEPLOY means "into the fight", not "into a new
                // one".
                if (!m_net)
                    HostMatch();
                m_phase = Phase::ClassSelect;
                break;
            case MainMenu::Choice::Join:
                m_phase = Phase::Join;
                m_scan.Start();
                break;
            case MainMenu::Choice::Options: m_phase = Phase::Options; break;
            case MainMenu::Choice::Quit:    m_quit = true; break;
            }
        }
        return;
    }

    // The server browser: the scan shouts while the screen is up, and picking
    // a row — found or typed — starts the connection and moves on to the
    // class pick, the same order --connect always ran in. The handshake gets
    // a head start behind the class card.
    if (m_phase == Phase::Join)
    {
        if (input.KeyPressed(VK_ESCAPE))
        {
            m_scan.Stop();
            m_phase = Phase::MainMenu;
            return;
        }
        m_scan.Poll(dt);
        if (const auto host = m_joinMenu.Update(input, dt, m_scan.Servers(),
                                                camera.ViewportWidth(),
                                                camera.ViewportHeight()))
        {
            m_scan.Stop();
            Connect(*host, Net::kPort);
            m_phase = Phase::ClassSelect;
        }
        return;
    }

    // The settings landing. Nothing is edited here — it only says which screen
    // to open — and each of those comes back to this one rather than to the
    // main menu, so a player changing two things doesn't walk out to the title
    // and back in between them.
    if (m_phase == Phase::Options)
    {
        if (const auto picked =
                m_optionsMenu.Update(input, camera.ViewportWidth(), camera.ViewportHeight()))
        {
            switch (*picked)
            {
            case OptionsMenu::Choice::Player:   m_phase = Phase::Player; break;
            case OptionsMenu::Choice::KeyBinds: m_phase = Phase::KeyBinds; break;
            case OptionsMenu::Choice::Audio:    m_phase = Phase::Audio; break;
            case OptionsMenu::Choice::Back:     m_phase = Phase::MainMenu; break;
            }
        }
        return;
    }

    // The settings screens edit their half of the player's preferences in
    // place; the write to disk happens here, once, on the way out. A save per
    // keystroke would put a file write in the middle of a player trying three
    // keys to see which feels right, and there's nothing to lose by waiting
    // until they're done.
    //
    // Nothing takes effect anywhere else when the name changes, and nothing
    // needs to: a name is stated at the door of a server, and the player is
    // standing on a menu, so there is no match for a new one to reach until
    // they join the next one.
    if (m_phase == Phase::Player)
    {
        if (m_playerMenu.Update(input, dt, m_settings, camera.ViewportWidth(),
                                camera.ViewportHeight()))
        {
            m_settings.Save();
            m_phase = Phase::Options;
        }
        return;
    }

    if (m_phase == Phase::KeyBinds)
    {
        if (m_bindMenu.Update(input, m_binds, camera.ViewportWidth(), camera.ViewportHeight()))
        {
            m_binds.Save();
            m_phase = Phase::Options;
        }
        return;
    }

    // Audio, on the same terms. The toggles take effect the frame they're
    // flipped — Update's first line reads them — and the file is written when
    // the player is done with the screen.
    if (m_phase == Phase::Audio)
    {
        if (m_audioMenu.Update(input, m_settings, camera.ViewportWidth(),
                               camera.ViewportHeight()))
        {
            m_settings.Save();
            m_phase = Phase::Options;
        }
        return;
    }

    if (m_phase == Phase::ClassSelect)
    {
        // Escape backs out to the menu rather than closing the game, and the
        // connection warming up behind this screen — to another machine or to
        // the one under it — is hung up on the way out.
        if (input.KeyPressed(VK_ESCAPE))
        {
            LeaveMatch();
            return;
        }
        // The handshake, behind the card. Pumping it here is what actually
        // gives it the head start — and what keeps it from timing out while
        // somebody reads five class descriptions, which is longer than a
        // connection attempt is willing to wait. A match hosted here ticks
        // along with it, so both squads are on the field by the time the pick
        // lands, exactly as they would be on a server that was already
        // running.
        if (m_net)
        {
            NetPump(dt, camera);
            if (!m_net)
                return; // the far end went away while the card was up
        }
        if (const auto picked =
                m_classSelect.Update(input, camera.ViewportWidth(), camera.ViewportHeight()))
        {
            // Ask to be in the match. The join goes out from the Connecting
            // phase, which also covers a handshake that hasn't finished yet.
            m_class = &GetClassDef(*picked);
            m_phase = Phase::Connecting;
        }
        return;
    }

    // Connecting: the handshake and the join, out from under the menus. All
    // that can happen here is the server answering (NetPump flips the phase
    // on Welcome), the attempt failing, or the player giving up — and giving
    // up on a connection is backing out of it, not out of the game.
    if (m_phase == Phase::Connecting)
    {
        if (input.KeyPressed(VK_ESCAPE))
        {
            LeaveMatch();
            return;
        }
        if (m_net->GetStatus() == NetClient::Status::Connected && !m_joinSent && m_class)
        {
            Net::Writer w;
            // The name goes up with the class, because this is the one message
            // that says who is arriving. It's whatever the settings hold — the
            // account name on a first run, whatever the player typed after
            // that, and possibly nothing at all, which the server answers with
            // a name of its own.
            Net::WriteJoin(w, static_cast<uint8_t>(m_class - kClassDefs), m_settings.playerName);
            m_net->SendReliable(w.bytes);
            m_joinSent = true;
        }
        NetPump(dt, camera);
        return;
    }

    // Escape leaves the match and lands on the menu. Somebody else's fight
    // goes on without you; one hosted here ends, because you were the only
    // person in it. Either way the key means the same thing — leave — and
    // QUIT on the menu is what closes the game.
    //
    // Unless the class cards are up, in which case the key backs out of them:
    // escape means "out of the thing in front of me", and what's in front of
    // the player is a screen, not the match behind it.
    if (input.KeyPressed(VK_ESCAPE))
    {
        if (m_classChangeOpen)
            m_classChangeOpen = false;
        else
        {
            LeaveMatch();
            return;
        }
    }

    // Being somebody else. The key is armed at the player's own spawn and while
    // they're waiting to come back from it, and nowhere else — that rule is the
    // whole of the feature, and it's checked here so the key is dead in the
    // hand rather than answered with a refusal from across the wire. The server
    // asks it again on arrival anyway, off the same spawns.
    //
    // Read before the Dead branch below, like the scoreboard and the radar,
    // because the respawn wait is the other half of when this is used: a player
    // who has just been killed by something their class was wrong for gets to
    // do something about it while they wait, and the soldier they're owed
    // arrives as whatever they picked.
    if (m_classChangeOpen)
    {
        // The same key closes it again — it's a screen being held open, and a
        // player who opened it by accident should get out the way they came in.
        // So does anything that would have stopped it opening: the screen is
        // only ever up where the choice is live, which today means the whistle
        // going while somebody is reading the cards takes them away.
        if (!CanChangeClass() || m_binds.Pressed(input, Act::ClassChange))
            m_classChangeOpen = false;
        else if (const auto picked = m_classSelect.Update(input, camera.ViewportWidth(),
                                                          camera.ViewportHeight()))
        {
            Net::Writer w;
            Net::WriteChangeClass(w, static_cast<uint8_t>(*picked));
            m_net->SendReliable(w.bytes);
            m_classChangeOpen = false;
        }
    }
    else if (m_binds.Pressed(input, Act::ClassChange) && CanChangeClass())
        m_classChangeOpen = true;

    // The scoreboard is a held key, read here rather than in ReadCommand
    // because it asks nothing of the simulation: it's this machine deciding
    // what to put on its own screen, and a server has no business hearing that
    // a player looked at the board. Read before the Dead branch below so it
    // works either side of a respawn — the wait is exactly when a player has
    // nothing to do but read it.
    //
    // Not while the cards are up: the board and the cards want the same middle
    // of the same screen, and the one the player just opened wins.
    m_showScores = !m_classChangeOpen && m_binds.Down(input, Act::Scoreboard);

    // The radar's zoom, read in the same place and for the same reason: it
    // decides what this machine draws in its own corner and the simulation has
    // no opinion about it. Held rather than tapped, and geometric rather than
    // additive, so a step means the same fraction of what's on show wherever in
    // the range it's taken — the same thing the camera's wheel does. Read
    // ahead of the Dead branch below because the wait is exactly when a player
    // has time to look at the map.
    if (m_binds.Down(input, Act::RadarIn))
        m_radarZoom *= std::pow(kRadarZoomRate, dt);
    if (m_binds.Down(input, Act::RadarOut))
        m_radarZoom /= std::pow(kRadarZoomRate, dt);
    m_radarZoom = std::clamp(m_radarZoom, kRadarZoomMin, kRadarZoomMax);

    // Dead: the arena runs on without the player — NPCs keep fighting, shots
    // keep flying, the corpse keeps falling — but no command reaches the
    // simulation and the camera holds on the spot where the body dropped
    // until the wait is up. The events keep coming, and keep being shown: a
    // firefight the player is watching out still sounds like one.
    if (m_phase == Phase::Dead)
    {
        // The wait is the server's; the timer here only feeds the countdown
        // text, and Respawned is what actually brings us back (see NetPump).
        // The ragdolls still need local gravity.
        m_respawnTimer -= dt;
        NetPump(dt, camera);
        if (!m_net)
            return; // the session ended out from under the wait
        m_snapElapsed += dt;
        m_renderAlpha = std::clamp(m_snapElapsed / World::kTickDt, 0.0f, 1.0f);
        m_world.Phys().Step(dt);
        ProcessEvents();
        UpdateParticles(dt);
        UpdateCorpses(dt);
        return;
    }

    // Welcomed but not yet seen: until the first snapshot that carries our
    // soldier lands, there is nothing to command and nothing to predict —
    // pump and wait, rather than dereferencing a roster that hasn't arrived
    // or mistaking "not here yet" for "dead". A frame or two at most, but a
    // frame that used to bet on packet timing.
    if (!m_hasPredicted && !m_world.Local())
    {
        NetPump(dt, camera);
        return;
    }

    // Input becomes a Command, and the Command is all the simulation hears:
    // the bindings, the pad, and the cursor's trip through the camera all end
    // inside ReadCommand, and the World does exactly the same work on a
    // sentence that arrived over a socket. The one keepsake this side holds
    // onto is the aim distance, which the aim indicator wants at render time.
    //
    // With the class cards up the player's hands are on a menu rather than on a
    // soldier, so the command is made empty instead of not being made at all:
    // stand still, hold this facing, press nothing. Not sending one would be
    // worse than sending nothing, because a starved server reapplies the last
    // command it had (Server::Impl::Step) — a player who opened the screen
    // mid-stride would watch their soldier walk out of the spawn area they
    // opened it in.
    m_screenRight = camera.ScreenRightOnGround();
    const Command fresh =
        m_classChangeOpen
            ? Command{}
            : ReadCommand(input, camera, m_hasPredicted ? m_predicted.pos : m_world.Local()->pos);
    if (!m_classChangeOpen)
        m_aimDist = fresh.aimDist;

    // The whistle has gone: the arena is frozen and nothing pressed reaches
    // it. The controls stop being read rather than being read and ignored,
    // because a command that went on being predicted would walk this
    // machine's soldier away from the one the server has standing still.
    const bool over = m_world.MatchOver();

    m_meleeFlash = std::max(0.0f, m_meleeFlash - dt);

    // Commands are made on the same 60 Hz clock the server consumes them on —
    // one per tick, numbered, kept until acked — and each one drives the
    // predicted soldier the moment it exists. The latch below is what keeps
    // the display's rate out of it: held controls are overwritten with
    // whatever this frame's hands say, edges latch on until a tick spends
    // them, and the tick spends them into exactly one command.
    //
    // None of it happens once the match is over: no command is stamped,
    // nothing goes out on the wire, and the prediction stays where the
    // server's last word put it — Repredict keeps it glued there as the
    // frozen snapshots arrive. The pending command is wiped rather than
    // left to sit, so the next match doesn't open on a key that was
    // pressed at the end of the last one.
    if (over)
    {
        m_pendingCmd = {};
        m_tickAccum = 0.0f;
        m_predAlpha = 0.0f;
    }
    else
    {
        m_pendingCmd.move = fresh.move;
        m_pendingCmd.aim = fresh.aim;
        m_pendingCmd.aimDist = fresh.aimDist;
        m_pendingCmd.fire = fresh.fire;
        m_pendingCmd.melee = fresh.melee;
        m_pendingCmd.steady = fresh.steady;
        m_pendingCmd.reload = m_pendingCmd.reload || fresh.reload;
        m_pendingCmd.grenade = m_pendingCmd.grenade || fresh.grenade;
        m_pendingCmd.ability = m_pendingCmd.ability || fresh.ability;
        // The same latch the edges above get, spelled out because the byte
        // can't be OR'd: a callout already waiting for its tick keeps the tick,
        // and a second press inside the same frame's worth of them is a press
        // the cooldown was going to eat regardless.
        if (fresh.voice != kVoiceNone && m_pendingCmd.voice == kVoiceNone)
            m_pendingCmd.voice = fresh.voice;

        m_tickAccum += dt;
        while (m_tickAccum >= World::kTickDt)
        {
            m_tickAccum -= World::kTickDt;
            const uint32_t seq = ++m_cmdSeq;
            m_pendingCmds.push_back({ seq, m_pendingCmd });
            // A server that stops acking must not turn this buffer into
            // the whole session's history; past two seconds the oldest
            // commands are lost causes, and the next ack squares
            // everything anyway.
            if (m_pendingCmds.size() > 120)
                m_pendingCmds.pop_front();
            // The packet is this command and up to two predecessors,
            // oldest first — the pending buffer's tail, which is exactly
            // the set a lost packet would have cost the server.
            Net::CmdEntry tail[Net::kCmdRedundancy];
            size_t count = 0;
            const size_t start =
                m_pendingCmds.size() > Net::kCmdRedundancy
                    ? m_pendingCmds.size() - Net::kCmdRedundancy
                    : 0;
            for (size_t i = start; i < m_pendingCmds.size(); ++i)
                tail[count++] = { m_pendingCmds[i].seq, m_pendingCmds[i].cmd };
            Net::Writer w;
            Net::WriteCmds(w, tail, count);
            m_net->SendState(w.bytes);
            if (m_hasPredicted)
            {
                m_predicted.prevPos = m_predicted.pos;
                m_predicted.prevAimDir = m_predicted.aimDir;
                m_predicted.prevWalkPhase = m_predicted.walkPhase;
                m_predicted.prevMoveBlend = m_predicted.moveBlend;
                m_world.MoveCommand(m_predicted, m_pendingCmd, World::kTickDt);
            }
            m_pendingCmd.reload = false;
            m_pendingCmd.grenade = false;
            m_pendingCmd.ability = false;
            m_pendingCmd.voice = kVoiceNone;
        }
        m_predAlpha = m_tickAccum / World::kTickDt;
    }

    NetPump(dt, camera);
    // The pump may have ended the session — server gone, or the door
    // closed — in which case there's a menu on screen now, not a match.
    if (!m_net)
        return;
    m_snapElapsed += dt;
    m_renderAlpha = std::clamp(m_snapElapsed / World::kTickDt, 0.0f, 1.0f);
    // The replica is never Ticked, but the corpses still fall through the
    // local physics world; this is their gravity.
    m_world.Phys().Step(dt);

    // Everything the wire delivered, turned into something to see and hear.
    ProcessEvents();

    // Particles are the client's own weather — they never touch the outcome
    // of anything, so they run on render time and stay smooth whatever the
    // tick is doing. Corpse timers run here for the same reason: how long a
    // body lies around is a fact about the picture, not the fight.
    UpdateParticles(dt);
    UpdateCorpses(dt);

    // Death is noticed by absence. Whatever emptied the local soldier's
    // health, the server has already reaped them on the same terms as anyone
    // else — they're gone from the snapshot — and the Death event has already
    // put their corpse on the floor. What remains is the part that belongs to
    // the player rather than the soldier: stop drawing a swing whose arm is
    // gone, start the wait, and let the camera hold where the body fell
    // (m_eyePos keeps the spot).
    //
    // Absence with a spawn already promised isn't death, though: a class change
    // takes the old soldier off the field and puts a new one on in the same
    // tick, and if the snapshot that carries them is read a packet ahead of the
    // Respawned that names them, this is the frame in between. m_camSnapPending
    // is exactly that promise — set by Welcome and Respawned, spent by the
    // first snapshot with us in it — so waiting on it costs a real death
    // nothing and saves a class change a frame of the grey screen.
    if (!m_world.Local())
    {
        m_meleeFlash = 0.0f;
        // The prediction dies with the soldier it was predicting.
        m_hasPredicted = false;
        m_pendingCmds.clear();
        // Nothing to look through and nothing to look at yet; the eye stays
        // where it was, which is where the body was standing.
        if (m_camSnapPending)
            return;
        m_phase = Phase::Dead;
        m_respawnTimer = World::kRespawnDelay;
        return;
    }

    // The eye, the ear, and the camera all follow the soldier as drawn — the
    // blend between ticks — not the soldier as the server last said, so
    // nothing on screen leads or trails the body it's about. "As drawn" means
    // the prediction: the camera answers the keys on the frame they're
    // pressed, the same as the body does. The fallback below is the frame or
    // two before the first own-snapshot has landed.
    if (m_hasPredicted)
        m_eyePos = Vector3::Lerp(m_predicted.prevPos, m_predicted.pos, m_predAlpha);
    else
    {
        const Unit& local = *m_world.Local();
        m_eyePos = Vector3::Lerp(local.prevPos, local.pos, m_renderAlpha);
    }
    m_sound.SetListener(m_eyePos, camera.ScreenUpOnGround());
    camera.SetTarget(m_eyePos);
}

bool Game::CanChangeClass() const
{
    // Nothing to change on a menu, and nothing to change into once the whistle
    // has gone: the arena is frozen for the length of the result, the next
    // match deals everybody a fresh soldier anyway, and this is one more key
    // that stops reaching the fight along with all the others.
    if (!m_net || m_world.MatchOver())
        return false;
    // Waiting to come back is the other place it's allowed, and the plainer of
    // the two: there is no soldier to trade, only the one being owed, and
    // saying now what that one should be costs the fight nothing.
    if (m_phase == Phase::Dead)
        return true;
    if (m_phase != Phase::Playing)
        return false;
    // Otherwise it's a question about where they're standing. The eye is the
    // soldier as drawn, which is the position the player is actually looking
    // out of, and the spawns are the level's — the same ones the server will
    // measure against when the ask arrives.
    return m_world.InSpawnArea(m_team, m_eyePos);
}

Command Game::ReadCommand(const Input& input, const IsoCamera& camera, const Vector3& pos) const
{
    using PadTracker = DirectX::GamePad::ButtonStateTracker;

    Command cmd;

    // --- Movement: forward and back along the soldier rather than along the
    // screen. W walks the way the body is pointed, which is the way the cursor
    // is dragging it round, and S backs out along the same line; A and D step
    // across it. Which way that is on the tick it lands is the simulation's to
    // say — the facing is turning under the keys — so the keys leave here as
    // the two body axes and nothing about the screen goes with them. The
    // stick's Y and X are the same two axes, which is what the stick was
    // always shaped like ---
    float forward = 0.0f, strafe = 0.0f;
    if (m_binds.Down(input, Act::MoveForward)) forward += 1.0f;
    if (m_binds.Down(input, Act::MoveBack)) forward -= 1.0f;
    if (m_binds.Down(input, Act::MoveRight)) strafe += 1.0f;
    if (m_binds.Down(input, Act::MoveLeft)) strafe -= 1.0f;
    forward += input.pad.thumbSticks.leftY;
    strafe += input.pad.thumbSticks.leftX;
    cmd.move = Vector2(strafe, forward);

    // --- Aim: mouse cursor projected onto the ground plane, or the right
    // stick as a screen-relative direction when deflected ---
    const Vector3 upG = camera.ScreenUpOnGround();
    const Vector3 rightG = camera.ScreenRightOnGround();
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
    // The one thing a soldier says out loud. No pad button: the face buttons
    // are spent on the kit, and the day there are four callouts they'll want
    // the d-pad as a set rather than one of them taking a button now.
    if (m_binds.Pressed(input, Act::VoiceMedic))
        cmd.voice = static_cast<uint8_t>(VoiceId::Medic);

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

        case Event::Type::Voice:
            // A shout is played on exactly the terms it arrived on, with no
            // second opinion taken here at all: it's in this list because the
            // server decided this player is near enough to hear it, and near
            // enough is the only question a callout asks. Whose side the mouth
            // was on is not a second question — an enemy calling for a medic is
            // information, and being given it is the point.
            //
            // Your own goes to the ear rather than to your feet, the same as
            // your own reload and your own ability. The two are inches apart
            // under this camera and the reason isn't distance: the sound is the
            // only confirmation the player gets that anything went out, so it
            // has to arrive the same way every time rather than being panned by
            // where they happen to be standing.
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
    if (std::abs(pos.x) > m_world.ArenaHalf().x || std::abs(pos.z) > m_world.ArenaHalf().y)
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

// Who the player's side is seeing through, for the frame about to be drawn:
// their own eye — the drawn one, so the fog answers the keys on the frame the
// body does, and the spot they fell on once they're dead — and every living
// squadmate. That the same list is built on the server (World::TeamEyes, from
// the same World) is what keeps a spotted enemy from arriving in the snapshot
// only to be culled back out here.
// The player's own eye takes its reach from the class they picked rather than
// from the roster, for the same reason it takes its position from m_eyePos:
// while they're dead there is no unit of theirs to read either off, and a
// sniper watching their own body cool should still be watching it from a
// sniper's distance.
void Game::UpdateTeamEyes()
{
    m_teamEyes.clear();
    m_teamEyes.push_back({ { m_eyePos.x, m_eyePos.z }, m_class->sight });
    m_world.TeamEyes(m_team, m_teamEyes, m_myUnitId);
}

// One eye's share of the fog mask. The visibility polygon around it is
// star-shaped by construction — consecutive points always span a straight run
// of one edge, or one chord of the arc where the range ran out before a wall
// did — so a fan of triangles back to the eye is exactly the ground that eye
// can see, and the mask is the union of those fans over the squad.
//
// Which is why the fog is a mask now rather than dark quads laid over the
// gaps: darkness doesn't union. Drawn per soldier it would double up wherever
// two of them see the same ground, and it would cover ground one of them sees
// perfectly well.
void Game::AppendSight(const Visibility::Eye& eye, std::vector<Vertex>& out) const
{
    const std::vector<XMFLOAT2> poly =
        Visibility::ComputePolygon(eye.pos, m_world.Occluders(), m_world.ArenaHalf(), eye.range);
    if (poly.size() < 3)
        return;

    const Vertex center = { XMFLOAT3{ eye.pos.x, kFogHeight, eye.pos.y }, kFogColor };
    for (size_t i = 0; i < poly.size(); ++i)
    {
        const XMFLOAT2& a = poly[i];
        const XMFLOAT2& b = poly[(i + 1) % poly.size()];
        out.push_back(center);
        out.push_back({ XMFLOAT3{ a.x, kFogHeight, a.y }, kFogColor });
        out.push_back({ XMFLOAT3{ b.x, kFogHeight, b.y }, kFogColor });
    }
}

// And the darkness itself: one sheet over the player, wide enough to leave the
// arena in any direction, so the ground nobody on their side can see — the
// far side of every wall, and the void past the map's edge — runs off all four
// edges of the screen.
void Game::AppendFogSheet(std::vector<Vertex>& out) const
{
    const float reach = std::max(m_world.ArenaHalf().x, m_world.ArenaHalf().y) * kFogFar;
    const float x0 = m_eyePos.x - reach, x1 = m_eyePos.x + reach;
    const float z0 = m_eyePos.z - reach, z1 = m_eyePos.z + reach;
    const Vertex quad[4] = {
        { XMFLOAT3{ x0, kFogHeight, z0 }, kFogColor },
        { XMFLOAT3{ x1, kFogHeight, z0 }, kFogColor },
        { XMFLOAT3{ x1, kFogHeight, z1 }, kFogColor },
        { XMFLOAT3{ x0, kFogHeight, z1 }, kFogColor },
    };
    out.push_back(quad[0]);
    out.push_back(quad[1]);
    out.push_back(quad[2]);
    out.push_back(quad[0]);
    out.push_back(quad[2]);
    out.push_back(quad[3]);
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

    if (m_phase == Phase::Join)
    {
        m_joinMenu.Render(renderer, m_scan.Servers());
        return;
    }

    if (m_phase == Phase::Options)
    {
        m_optionsMenu.Render(renderer);
        return;
    }

    if (m_phase == Phase::Player)
    {
        m_playerMenu.Render(renderer, m_settings);
        return;
    }

    if (m_phase == Phase::KeyBinds)
    {
        m_bindMenu.Render(renderer, m_binds);
        return;
    }

    if (m_phase == Phase::Audio)
    {
        m_audioMenu.Render(renderer, m_settings);
        return;
    }

    if (m_phase == Phase::ClassSelect)
    {
        m_classSelect.Render(renderer, m_binds);
        return;
    }

    // Connecting: nothing to draw but the fact of it. The arena arrives with
    // the first snapshot, moments after this screen stops existing. A match
    // hosted here says what it's doing rather than where it's dialing —
    // "connecting to 127.0.0.1" is true and tells the player nothing.
    if (m_phase == Phase::Connecting)
    {
        std::string text = m_server ? "STARTING MATCH" : "CONNECTING TO " + m_connectHost;
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

    // The soldiers, off the World's roster. An enemy nobody on the player's
    // side can see stays hidden — behind a wall, or simply out past what any
    // of them can make out, they disappear until somebody on the squad gets
    // eyes on them again. The player's own side skips the test: where
    // your squad is standing is not something the fog was ever keeping from
    // you, and a soldier who vanished on turning a corner would be one nobody
    // could fight alongside. The local unit skips the frustum test too — the
    // camera follows them, so they're always on screen. While the player is
    // dead there's no local unit to draw: what's standing at m_eyePos is their
    // corpse, and the view stays on it.
    UpdateTeamEyes();
    m_scratch.clear();
    // The moment between two ticks that this frame is a picture of. Every
    // soldier is drawn at the blend of where the last tick left them and
    // where this one put them; a display running faster than the simulation
    // sees motion, not the simulation's sixty stills a second.
    const float alpha = m_renderAlpha;
    const auto blendDir = [](const Vector3& prev, const Vector3& cur, float t) {
        Vector3 dir = Vector3::Lerp(prev, cur, t);
        if (dir.LengthSquared() > 1e-8f)
            dir.Normalize();
        else
            dir = cur; // a half-turn's midpoint has no direction; take the newer one
        return dir;
    };
    for (const Unit& unit : m_world.Units())
    {
        // In connected play the local soldier is drawn from the prediction,
        // on the prediction's own clock — the one body on the field that
        // answers this machine's keys instead of the wire's history.
        const bool predicted = m_hasPredicted && unit.controller == Unit::Controller::Local;
        const Unit& src = predicted ? m_predicted : unit;
        const float a = predicted ? m_predAlpha : alpha;
        const Vector3 pos = Vector3::Lerp(src.prevPos, src.pos, a);
        if (unit.controller != Unit::Controller::Local)
        {
            // Cheapest rejection first: the arena is far wider than the view,
            // so most of a large squad is usually off screen entirely.
            if (!renderer.IsSphereVisible({ pos.x, kSoldierBoundsY, pos.z },
                                          kSoldierBoundsRadius))
                continue;
            if (unit.team != m_team &&
                !Visibility::IsPointVisibleAny(m_teamEyes, { pos.x, pos.z }, m_world.Occluders()))
                continue;
        }
        DrawSoldier(renderer, pos, blendDir(src.prevAimDir, src.aimDir, a),
                    src.prevWalkPhase + (src.walkPhase - src.prevWalkPhase) * a,
                    src.prevMoveBlend + (src.moveBlend - src.prevMoveBlend) * a,
                    unit.team, unit.cls->color);
        DrawName(renderer, unit, pos);
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
        if (!Visibility::IsPointVisibleAny(m_teamEyes, { pelvis.pos.x, pelvis.pos.z },
                                           m_world.Occluders()))
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
        if (Visibility::IsPointVisibleAny(m_teamEyes, { pos.x, pos.z }, m_world.Occluders()))
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
        if (!Visibility::IsPointVisibleAny(m_teamEyes, { p.pos.x, p.pos.z }, m_world.Occluders()))
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
    // floor while walls (which wrote depth) still punch through it. Every eye
    // on the player's side marks what it can see first — a pass each, since a
    // squad's worth of fans over a real map's occluders can outgrow the batch,
    // and the mark cares about neither order nor overlap — and then one dark
    // sheet lands on everything left unmarked.
    for (const Visibility::Eye& eye : m_teamEyes)
    {
        m_fogVerts.clear();
        AppendSight(eye, m_fogVerts);
        renderer.MarkSeen(m_fogVerts.data(), static_cast<uint32_t>(m_fogVerts.size()), identity);
    }
    m_fogVerts.clear();
    AppendFogSheet(m_fogVerts);
    renderer.DrawTrianglesUnseen(m_fogVerts.data(), static_cast<uint32_t>(m_fogVerts.size()),
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

    // Aim indicator: the ground the shot travels over, laid down as dots on
    // the floor and ending where the shot really stops (max range, the first
    // wall the arc can't clear, or the ground). Flat rather than the trajectory
    // itself, because a line hanging in the air reads at the height it's drawn
    // and not the height it means — a shaft passing over a crate looks like it
    // ends on the crate. A dot can't be misread that way: it's a piece of
    // ground, which is what the player is picking. What that gives up is the
    // arc's shape, so whether a lob clears the wall in front of it isn't
    // something the indicator claims any more — where it stops still is. An
    // arrow head takes the end point, and the primary is the only weapon it
    // speaks for: the grenade is thrown on a feel for the arc, not off a mark
    // that has to be read. Drawn after the fog pass so it isn't dimmed twice.
    // There's nothing to aim while dead, so it goes
    // with the soldier. The whole line fades through a reload — where the shot
    // would land is still worth showing, but not as something the player can
    // act on yet. It goes entirely once the match is over: the arena is frozen,
    // and a line of dots still tracking the cursor across it would be the one
    // thing on screen claiming there was still something to shoot.
    if (m_phase == Phase::Playing && !m_world.MatchOver())
    {
        Unit& u = *m_world.Local();
        // The indicators hang off the soldier as drawn, so the aim line grows
        // out of the muzzle on screen rather than out of where the simulation
        // has quietly moved it. As drawn means the prediction, when there is
        // one — the loadout underneath (the reload, the grenade count) stays
        // the server's word.
        const Unit& drawn = m_hasPredicted ? m_predicted : u;
        const float da = m_hasPredicted ? m_predAlpha : alpha;
        const Vector3 lpos = Vector3::Lerp(drawn.prevPos, drawn.pos, da);
        const Vector3 laim = blendDir(drawn.prevAimDir, drawn.aimDir, da);
        const bool reloading = u.reloadTimer > 0.0f;
        const XMFLOAT4 aimColor = reloading ? kAimSpentColor : kAimColor;

        // How far down the line the shot gets. The flight arches and droops,
        // but its shadow doesn't: horizontal travel is a straight run along the
        // aim, so one distance is the whole of what the dots need.
        const float stop = m_world.PredictShotStop(u.cls->primary, lpos, laim, m_aimDist);

        // The near edge of what this weapon can do, for the one class that has
        // one. Everything inside it is ground the primary is useless on, so
        // those dots are drawn as dead rather than as aim — the player should
        // be able to see, without reading anything, that the first stride in
        // front of them isn't theirs. At 0 no dot is ever inside it and nothing
        // about the indicator changes, which is every class but the sniper.
        const float minRange = u.cls->primary.minRange;

        // Where the run of dots ends: on a wall it's the floor the shot dies
        // over, which is the ground the player was pointing at anyway.
        const Vector3 end(lpos.x + laim.x * stop, kAimRingHeight, lpos.z + laim.z * stop);

        // Stepped in world distance rather than per sample of the flight, so
        // the spacing is a length of ground and stays the same length whether
        // it's a thrown grenade crawling out or a rifle round. Filled pips and
        // the arrow head are triangles, so they share a batch of their own,
        // laid down before the marks that are made of lines.
        // The run stops where the head starts, so no pip is buried under it.
        // Head and pips are the one grey; the head is told apart by being the
        // only mark on the line with a shape, which is all a terminus needs.
        m_scratch.clear();
        const XMFLOAT4 dotColor = reloading ? kAimDotFadedColor : kAimDotColor;
        for (float d = kAimDotSpacing; d < stop - kAimArrowLength; d += kAimDotSpacing)
            AppendDot(m_scratch, lpos.x + laim.x * d, lpos.z + laim.z * d,
                      d < minRange ? kAimDotFadedColor : dotColor);
        AppendArrowHead(m_scratch, end, laim, dotColor);
        renderer.DrawTrianglesAlpha(m_scratch.data(), static_cast<uint32_t>(m_scratch.size()),
                                    identity);
        m_scratch.clear();

        // The near end as a ring on the ground around the soldier,
        // because that is the shape the rule actually has: a body anywhere
        // inside it is one this weapon passes through, whichever way the gun is
        // pointed. It sits under the soldier at the height every other ground
        // ring uses, and it never greys out with the reload — the dead ground
        // is a fact about the weapon rather than about its state.
        if (minRange > 0.0f)
            AppendCircle(m_scratch, { lpos.x, kAimRingHeight, lpos.z }, minRange,
                         kDeadRangeColor);

        if (u.cls->primary.lobVelocity > 0.0f)
            AppendCircle(m_scratch, end, 0.4f, aimColor);

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
        // on their own side. Same visibility rule the bodies themselves get,
        // which is now no rule at all — a squad knows where it is, so the ring
        // goes under every squadmate the frustum can reach, wall or no wall.
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
            AppendCircle(m_scratch, { pos.x, kAimRingHeight, pos.z }, kFriendlyRingRadius,
                         ringColor);
        }

        renderer.DrawLinesAlpha(m_scratch.data(), static_cast<uint32_t>(m_scratch.size()),
                                identity);
    }

    // The ground a player can change class on, drawn in a pass of its own
    // rather than with the indicators above, because it outlasts having a
    // soldier: the respawn wait is when the choice is most often made, and the
    // spawn is where it will be served.
    if (m_phase == Phase::Playing || m_phase == Phase::Dead)
    {
        const XMFLOAT4& teamTint = TeamColor(m_team);
        const XMFLOAT4 ring = { teamTint.x, teamTint.y, teamTint.z,
                                CanChangeClass() ? kSpawnRingAlphaLive : kSpawnRingAlpha };
        const Vector3 spawn = m_world.TeamSpawn(m_team);
        m_scratch.clear();
        AppendArc(m_scratch, { spawn.x, kAimRingHeight, spawn.z }, World::kSpawnArea, 0.0f,
                  XM_2PI, kSpawnRingSegments, ring);
        renderer.DrawLinesAlpha(m_scratch.data(), static_cast<uint32_t>(m_scratch.size()),
                                identity);
    }

    RenderHud(renderer);

    // The class cards, last of everything: a screen laid over a match that is
    // still being fought behind it. It goes after the HUD rather than instead
    // of it — the corner panel, the radar and the scoreboard's own key all keep
    // working, because a player choosing a class is choosing it about the fight
    // they can see.
    if (m_classChangeOpen)
        m_classSelect.Render(renderer, m_binds, ClassSelect::Mode::Change);
}

// The name under a soldier: whoever is standing in the roster row this unit
// holds, colored by what they are to the person reading it.
//
// Only players have one. An AI slot's name is empty, so nothing is drawn, and
// that is the whole of the "which of these are people" filter — it needs no
// flag of its own, because having a name is what being a person is here.
//
// Called from inside the soldier loop rather than from a pass of its own, so a
// name and the body it belongs to are subject to exactly one rule: whatever
// that loop decided to draw is what gets labelled, fog and frustum and all. A
// second pass would be a second opinion about who can be seen, and the one
// thing a name must never do is out a soldier the arena is hiding.
void Game::DrawName(Renderer& renderer, const Unit& unit, const Vector3& pos) const
{
    // Not while something is deliberately holding the middle of the screen.
    // Screen text draws over overlay geometry whatever order it was queued in,
    // so a name behind the class cards or the scoreboard would be a name
    // written across them — and neither is a moment when where somebody is
    // standing is the question being asked.
    if (m_classChangeOpen || m_showScores)
        return;

    const std::vector<World::Slot>& roster = m_world.Roster();
    if (unit.slot < 0 || unit.slot >= static_cast<int>(roster.size()))
        return;
    const std::string& name = roster[unit.slot].name;
    if (name.empty())
        return;

    // Anchored to the ground the soldier is standing on rather than to the
    // model, which is what puts it under the boots: `pos` is the feet and the
    // body is drawn upward from there, so a fixed drop in pixels clears it at
    // any zoom without ever having to ask how tall the model came out.
    XMFLOAT2 screen;
    if (!renderer.WorldToScreen({ pos.x, 0.0f, pos.z }, screen))
        return;

    const float h = static_cast<float>(renderer.Height());
    const float size = h * kNameSize;
    renderer.DrawScreenText(name, screen.x - renderer.MeasureScreenText(name, size) * 0.5f,
                            screen.y + h * kNameDrop, size,
                            unit.team == m_team ? kNameFriendlyColor : kNameEnemyColor);
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
        // Against the wait that was actually charged, not the class's longest:
        // a reload taken early finishes sooner, and a bar measured off the
        // empty price would crawl to four fifths and then jump.
        hud.reloadFraction = u->reloadTimer > 0.0f && u->reloadSpan > 0.0f
                                 ? 1.0f - u->reloadTimer / u->reloadSpan
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
    // The roster, both sides counted the same way: everyone standing. Asked
    // of the World rather than counted here because a connected client's
    // roster is fog-filtered — it only holds what this player can see — and
    // the panel is a scoreboard, which is meant to know more than the fog
    // shows. Solo, it's the same numbers it always was.
    hud.allies = m_world.Standing(m_team);
    hud.enemies = 0;
    for (int team = 0; team < m_world.TeamCount(); ++team)
        if (team != m_team)
            hud.enemies += m_world.Standing(team);
    hud.teamSize = World::kTeamSize;
    // What the match is decided on, counted the same way the standing is: the
    // World's word, because on a connected client the kills happened to
    // soldiers this player never saw. The clock reads zero once the match is
    // over — what's counting then is the wait for the next one, and that goes
    // under the result rather than in the corner.
    hud.clock = m_world.MatchTime();
    hud.matchOver = m_world.MatchOver();
    hud.allyScore = m_world.Score(m_team);
    hud.enemyScore = 0;
    for (int team = 0; team < m_world.TeamCount(); ++team)
        if (team != m_team)
            hud.enemyScore += m_world.Score(team);
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
    // The cap is the word that gets shouted, off the callout table rather than
    // spelled again here — the same deal the ability cap has with its class.
    // MEDIC over VOICE for the same reason FIELD DRESSING beat ABILITY: a
    // player wants to know what the key does to the match, not what kind of
    // key it is.
    addHint(m_binds.Label(Act::VoiceMedic), kVoiceDefs[static_cast<size_t>(VoiceId::Medic)].name);
    addHint(m_binds.Label(Act::Scoreboard), "SCORES");
    // The class change, but only where it would work: standing on your own
    // spawn, or waiting to come back from it. A cap that was always up would be
    // a cap that lies five minutes out of every six, and one that appears
    // exactly when the key goes live is how the rule gets taught — the player
    // walks home for something else, the row grows a key they didn't know they
    // had, and that is the whole of the explanation.
    if (CanChangeClass())
        addHint(m_binds.Label(Act::ClassChange), "CHANGE CLASS");

    hud.hints = hints;
    hud.hintCount = hintCount;
    Hud::Render(renderer, hud);

    // The radar, bottom left: where everyone the player is allowed to know
    // about is standing, on a picture of the ground they're standing on.
    //
    // The blips are filtered here, against the same eyes and the same test the
    // arena's bodies are drawn through — your own side always, an enemy only
    // while somebody on your side has a clear line to them. It has to be this
    // test and not a weaker one: a snapshot arrives fog-filtered already, so
    // trusting it would be nearly right, and nearly right means a soldier who
    // walked behind an outcrop this frame keeps a blip for as long as the
    // client hasn't been told otherwise. The radar is the one readout on
    // screen with the reach to make that mistake visible.
    //
    // Positions are the drawn ones rather than the simulated ones — the same
    // blend, off the same prediction — so a blip crossing the panel moves as
    // smoothly as the body it belongs to.
    m_radarBlips.clear();
    for (const Unit& unit : m_world.Units())
    {
        const bool predicted = m_hasPredicted && unit.controller == Unit::Controller::Local;
        const Unit& src = predicted ? m_predicted : unit;
        const float a = predicted ? m_predAlpha : m_renderAlpha;
        const Vector3 pos = Vector3::Lerp(src.prevPos, src.pos, a);
        if (unit.team != m_team && unit.controller != Unit::Controller::Local &&
            !Visibility::IsPointVisibleAny(m_teamEyes, { pos.x, pos.z }, m_world.Occluders()))
            continue;

        Vector3 aim = Vector3::Lerp(src.prevAimDir, src.aimDir, a);
        m_radarBlips.push_back({ pos.x, pos.z, TeamColor(unit.team), unit.cls->color,
                                 unit.controller == Unit::Controller::Local, aim.x, aim.z });
    }

    Hud::Radar radar = {};
    radar.arenaHalf = { m_world.ArenaHalf().x, m_world.ArenaHalf().y };
    // The panel is turned to face the camera with the same vector that stands
    // a health bar square to the view. The view is isometric and its yaw
    // starts at 45°, so the arena's own axes are not the screen's — a radar
    // laid out on them puts the player's base on the wrong side of the panel,
    // which is worse than being at an angle to it. One vector, read once in
    // Update, so the panel and the arena can't disagree about which way the
    // camera is pointing.
    radar.screenRight = { m_screenRight.x, m_screenRight.z };
    radar.walls = m_world.Occluders().data();
    radar.wallCount = m_world.Occluders().size();
    radar.blips = m_radarBlips.data();
    radar.blipCount = m_radarBlips.size();
    radar.zoom = m_radarZoom;
    // What the window follows: the eye, which is the soldier while they have
    // one and the spot they died on for the length of the wait. The same point
    // the fog is cut from and the ear listens at, so the map keeps looking at
    // the place the player is actually experiencing the match from rather than
    // blinking to the middle of the arena the moment they go down.
    radar.focusX = m_eyePos.x;
    radar.focusZ = m_eyePos.z;
    // The zoom's own key caps, over the panel. They aren't on the loadout's
    // hint row because that row is the kit — what this soldier is carrying and
    // what spends it — and the radar's zoom is neither. Saying it next to the
    // radar costs the same two caps and puts the answer where the question is
    // asked. The labels are the directions rather than the word ZOOM: the
    // panel they sit on is a map, so what else would they be zooming.
    const Hud::Hint radarHints[] = {
        { m_binds.Label(Act::RadarOut).c_str(), "OUT" },
        { m_binds.Label(Act::RadarIn).c_str(), "IN" },
    };
    radar.hints = radarHints;
    radar.hintCount = std::size(radarHints);
    Hud::RenderRadar(renderer, radar);

    // The scoreboard, while the key is down. Every place on the field, both
    // sides, whether or not this player has ever laid eyes on the soldier
    // standing in it — the fog hides bodies, not the board, which is why the
    // rows come off the World (the server's word, in connected play) rather
    // than off the roster this client can see.
    if (m_showScores)
    {
        // The simulation's three holders become the HUD's three, which is the
        // whole of what that file needs to know about a slot: how loudly to
        // draw it. A departed player's row comes over with the rest — their
        // kills are still in their side's total, so leaving them out would
        // leave a column that didn't add up.
        const auto holder = [](World::Slot::Held held) {
            switch (held)
            {
            case World::Slot::Held::Human: return Hud::Holder::Human;
            case World::Slot::Held::Left:  return Hud::Holder::Left;
            default:                       return Hud::Holder::Ai;
            }
        };
        // What a row is called, and what it's holding. A place with a person in
        // it is named after the person and says the class beside it; a place the
        // AI is fielding is named after the class and says nothing beside it,
        // because the class is the whole of what that place is and printing it
        // twice would be the board talking to itself.
        //
        // The names are pointers into the roster's own strings rather than
        // copies. That's safe for exactly as long as it needs to be — the rows
        // are built and handed to the HUD inside this function, and the World
        // they point into can't change until the wire is pumped again next
        // frame — and it's why they're rebuilt every frame rather than kept.
        m_scoreRows.clear();
        for (const World::Slot& slot : m_world.Roster())
        {
            const char* cls = slot.cls ? slot.cls->name : nullptr;
            const bool named = !slot.name.empty();
            m_scoreRows.push_back({ slot.team, named ? slot.name.c_str() : cls,
                                    named ? cls : nullptr, holder(slot.held), slot.local,
                                    slot.kills, slot.deaths });
        }

        // Your side's column first, the same way the corner panel puts your row
        // on top. Two columns for two sides; a third team would want a third
        // column, and it would want the corner panel rebuilt too.
        const int enemyTeam = (m_team + 1) % std::max(m_world.TeamCount(), 1);
        Hud::Scoreboard board = {};
        board.rows = m_scoreRows.data();
        board.rowCount = m_scoreRows.size();
        board.teams[0] = m_team;
        board.teams[1] = enemyTeam;
        for (int side = 0; side < 2; ++side)
        {
            board.teamNames[side] = GetTeamDef(board.teams[side]).name;
            board.teamColors[side] = TeamColor(board.teams[side]);
            board.teamScores[side] = m_world.Score(board.teams[side]);
        }
        board.clock = m_world.MatchTime();
        board.matchOver = m_world.MatchOver();
        Hud::RenderScoreboard(renderer, board);
    }

    // The result. Fifteen minutes are up and the side that killed more has
    // won — said in that side's own color, in the middle of the screen, over
    // an arena that has stopped moving. It replaces the respawn countdown
    // rather than sharing the screen with it: a player waiting to respawn
    // into a match that has ended isn't waiting for anything, and what they
    // are actually waiting for is the line underneath.
    //
    // Both of these are the middle of the screen, which is where the
    // scoreboard is, so they stand down while it's up. Nothing is lost: the
    // board is titled FINAL once the match is over and both totals are on it,
    // which is the result said in more detail than the headline says it.
    if (m_showScores)
    {
        // Nothing centered while the board has the middle.
    }
    else if (m_world.MatchOver())
    {
        const int winner = m_world.Winner();
        const std::string headline =
            winner < 0 ? "DRAW" : std::string(GetTeamDef(winner).name) + " WINS";
        const float headSize = size * 2.2f;
        renderer.DrawScreenText(headline,
                                (w - renderer.MeasureScreenText(headline, headSize)) * 0.5f,
                                h * 0.36f, headSize,
                                winner < 0 ? kHudColor : Brighten(TeamColor(winner), 0.35f));

        const std::string next =
            "NEXT MATCH IN " + std::to_string(static_cast<int>(std::ceil(m_world.Intermission())));
        renderer.DrawScreenText(next, (w - renderer.MeasureScreenText(next, size)) * 0.5f,
                                h * 0.36f + headSize * 1.5f, size, kHudHintColor);
    }
    // Respawn countdown, centered and big: while it's up there's nothing else
    // to do, so it's the one thing on screen worth reading. Counts whole
    // seconds remaining, so it reaches 1 for the last second and never shows a
    // 0 the player can't act on.
    // Nor while the class cards are up, for the same reason: they have the
    // middle of the screen, and text is drawn over overlay geometry whatever
    // order it was queued in. Nothing is lost — the wait is running, and it
    // ends by handing the player a soldier of whatever they've just picked.
    else if (m_phase == Phase::Dead && !m_classChangeOpen)
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
