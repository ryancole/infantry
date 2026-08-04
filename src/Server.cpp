#include "Server.h"

#include "Discovery.h"
#include "Level.h"
#include "Team.h"

#include <enet/enet.h>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <utility>
#include <vector>

// A connected player is a Session here and a Remote unit in the World.
// Commands come up the wire and are staged into the tick; snapshots and
// events go back down and are all a client ever knows about the fight. The AI
// fills every slot no human has claimed, gives one up when somebody joins,
// and takes it back — on the reinforcement clock — when they leave.
namespace
{
    // How many commands may wait in a session's queue before the server
    // starts folding them together to catch up. The queue exists to absorb
    // network jitter — a packet arriving early keeps its tick — but depth is
    // latency, so past two ticks of cushion the backlog is spent, not saved.
    constexpr size_t kCmdBacklog = 2;

    // How far away an event still reaches a client. Sight is the snapshot
    // filter's business; this one is about the ears — gunfire behind a wall
    // is meant to be heard, so events go by distance, not by line of sight.
    // The number matches the client's audio falloff (Sound::kRange, which
    // this file has no business including): past it the sound would have
    // faded to nothing anyway, so the cut is inaudible by construction.
    constexpr float kEventRange = 45.0f;

    // One connected player: the peer, the slot they claimed, and the soldier
    // they're currently driving (unitId is -1 for the length of the respawn
    // wait).
    //
    // The slot is what makes them a player rather than a succession of
    // soldiers: it holds their place through every death, it's what their kills
    // and deaths are counted against, and it's the row they occupy on every
    // client's scoreboard. `team` is the side that slot is on, kept alongside
    // because it's asked for far more often than it changes.
    struct Session
    {
        ENetPeer* peer = nullptr;
        bool joined = false;
        int slot = -1;
        int team = -1;
        uint8_t classId = 0;
        int unitId = -1;
        float respawnTimer = 0.0f;
        // Where this client's fog is drawn from: their soldier while they
        // have one, frozen on the spot they died for the length of the wait —
        // the same rule their own camera follows, so what the server sends
        // and what the dead player's screen shows stay one picture.
        DirectX::SimpleMath::Vector2 viewPos;
        // Commands waiting for their tick, in the order the client stamped
        // them. Each is applied exactly once — that's the contract prediction
        // replays against — and `acked` names the newest one consumed, which
        // rides back in every own-block so the client can retire history.
        std::deque<std::pair<uint32_t, Command>> queue;
        uint32_t acked = 0;
        // What to do on a tick the queue can't feed: the last command's
        // holds, with its edges already spent. A gap on the wire costs a beat
        // of responsiveness, not a stumble — the soldier keeps walking the
        // way they were told to, and stops pressing things.
        Command held;
    };

    void SendReliable(ENetPeer* peer, const Net::Writer& w)
    {
        enet_peer_send(peer, Net::kChannelReliable,
                       enet_packet_create(w.bytes.data(), w.bytes.size(),
                                          ENET_PACKET_FLAG_RELIABLE));
    }

    void SendState(ENetPeer* peer, const Net::Writer& w)
    {
        // Unreliable sequenced: a late snapshot is a wrong snapshot, so the
        // channel drops stale arrivals instead of delivering history.
        enet_peer_send(peer, Net::kChannelState,
                       enet_packet_create(w.bytes.data(), w.bytes.size(), 0));
    }
}

struct Server::Impl
{
    Config config;
    bool enetUp = false;
    ENetHost* host = nullptr;
    Discovery::Responder discovery;
    std::vector<std::unique_ptr<Session>> sessions;

    // Reused every tick rather than rebuilt: a server's inner loop should not
    // be allocating.
    std::vector<Event> events;
    std::vector<Event> heard;
    std::vector<Net::CmdEntry> cmdScratch;
    std::vector<Visibility::Eye> eyes; // one client's side's, rebuilt per snapshot

    float accum = 0.0f;
    uint32_t tick = 0;
    long long eventsSeen = 0;
    // The World knows the match is over; this is how the server notices it
    // just became so, which is the moment worth reporting and the moment the
    // intermission starts being watched.
    bool matchOver = false;

    // The wire: connections in, joins answered, commands staged.
    void Service(World& world);
    // Respawn waits, which run on wall time like a client's own.
    void Respawns(World& world, float dt);
    // One fixed step, and what every client hears about it.
    void Step(World& world);
    // The whistle, and the match after it. The World freezes the arena and
    // counts the result out; what it can't do is tell five clients that a new
    // match has started, so the restart is here.
    void Rematch(World& world);
    // Down the wire: what happened, then where everything is — both as seen
    // from each client's own soldier.
    void Broadcast(World& world);
};

Server::Server() : m_impl(std::make_unique<Impl>()) {}

Server::~Server()
{
    Stop();
}

bool Server::Start(const Config& config)
{
    m_impl->config = config;
    m_world.Init(LevelData::Load(config.level));

    if (enet_initialize() != 0)
        return false;
    m_impl->enetUp = true;

    ENetAddress address = {};
    address.host = ENET_HOST_ANY;
    if (config.loopbackOnly)
        enet_address_set_host_ip(&address, "127.0.0.1");
    address.port = config.port;
    m_impl->host = enet_host_create(&address, 16, Net::kChannels, 0, 0);
    if (!m_impl->host)
        return false;

    // No local class: every slot on both sides is the AI's until players
    // arrive over the wire. Which is every player there is — the machine
    // hosting the match has no soldier in it except through a client.
    m_world.StartMatch();

    // Answer the LAN's "who's out there": one small reply per probe is what
    // puts this server on a join screen across the room. Failing to bind the
    // discovery port isn't fatal — a second server on the same box just
    // won't be listed, and can still be joined by address.
    if (config.discoverable && !m_impl->discovery.Start(Port()) && config.log)
        std::printf("discovery port %u unavailable; joinable by address only\n",
                    Discovery::kPort);

    return true;
}

void Server::Stop()
{
    if (!m_impl)
        return;
    // Hang up rather than vanish: a client that gets a goodbye lands on its
    // menu now instead of on a five-second timeout.
    for (const auto& session : m_impl->sessions)
        if (session->peer)
            enet_peer_disconnect_now(session->peer, 0);
    m_impl->sessions.clear();
    if (m_impl->host)
    {
        enet_host_flush(m_impl->host);
        enet_host_destroy(m_impl->host);
        m_impl->host = nullptr;
    }
    if (m_impl->enetUp)
    {
        enet_deinitialize();
        m_impl->enetUp = false;
    }
}

bool Server::Running() const
{
    return m_impl->host != nullptr;
}

uint16_t Server::Port() const
{
    // What the socket actually got, which is the only useful answer when the
    // config asked for whatever was free.
    return m_impl->host ? static_cast<uint16_t>(m_impl->host->address.port)
                        : m_impl->config.port;
}

uint32_t Server::Ticks() const
{
    return m_impl->tick;
}

long long Server::EventsSeen() const
{
    return m_impl->eventsSeen;
}

size_t Server::Players() const
{
    size_t joined = 0;
    for (const auto& session : m_impl->sessions)
        if (session->joined)
            ++joined;
    return joined;
}

void Server::Update(float dt)
{
    if (!m_impl->host)
        return;

    m_impl->Service(m_world);

    if (m_impl->config.discoverable)
    {
        m_impl->discovery.SetStanding(static_cast<int>(Players()),
                                      World::kTeamSize * m_world.TeamCount());
        m_impl->discovery.Poll();
    }

    m_impl->Respawns(m_world, dt);

    // Simulation time passes in whole ticks; this call deposits its dt and
    // the loop spends what's there.
    m_impl->accum += dt;
    while (m_impl->accum >= World::kTickDt)
    {
        m_impl->accum -= World::kTickDt;
        m_impl->Step(m_world);
    }
    if (!m_impl->sessions.empty())
        enet_host_flush(m_impl->host);
}

void Server::Impl::Service(World& world)
{
    ENetEvent netEvent;
    while (enet_host_service(host, &netEvent, 0) > 0)
    {
        switch (netEvent.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
        {
            auto session = std::make_unique<Session>();
            session->peer = netEvent.peer;
            netEvent.peer->data = session.get();
            sessions.push_back(std::move(session));
            if (config.log)
                std::printf("peer connected (%zu online)\n", sessions.size());
            break;
        }
        case ENET_EVENT_TYPE_RECEIVE:
        {
            Session* session = static_cast<Session*>(netEvent.peer->data);
            Net::Reader r(netEvent.packet->data, netEvent.packet->dataLength);
            const auto type = static_cast<Net::MsgType>(r.U8());
            if (session && type == Net::MsgType::Join && !session->joined)
            {
                const uint8_t version = r.U8();
                const uint8_t classId = r.U8() % kClassCount;

                // The two ways the door stays shut: a different build of the
                // game, or a match already full of humans. Refused loudly and
                // then hung up on — a rejected client should know why, not
                // time out wondering.
                Net::RejectReason why = {};
                bool reject = false;
                if (!r.ok || version != Net::kProtocolVersion)
                {
                    why = Net::RejectReason::Version;
                    reject = true;
                }
                else
                {
                    int humans = 0;
                    for (int t = 0; t < world.TeamCount(); ++t)
                        humans += world.HumanSlots(t);
                    if (humans >= World::kTeamSize * world.TeamCount())
                    {
                        why = Net::RejectReason::Full;
                        reject = true;
                    }
                }
                if (reject)
                {
                    Net::Writer w;
                    Net::WriteReject(w, why);
                    SendReliable(session->peer, w);
                    enet_peer_disconnect_later(session->peer, 0);
                    if (config.log)
                        std::printf("join refused (%s)\n",
                                    why == Net::RejectReason::Version ? "version" : "full");
                }
                else
                {
                    session->classId = classId;
                    // The emptier side of the fence, humans-wise: the AI
                    // balances itself around whatever this decides.
                    int team = 0;
                    for (int t = 1; t < world.TeamCount(); ++t)
                        if (world.HumanSlots(t) < world.HumanSlots(team))
                            team = t;
                    session->slot = world.ClaimSlot(team);
                    session->team = world.SlotTeam(session->slot);
                    session->unitId =
                        world.SpawnRemote(kClassDefs[session->classId], session->slot);
                    if (const Unit* me = world.UnitById(session->unitId))
                        session->viewPos = { me->pos.x, me->pos.z };
                    session->joined = true;
                    Net::Writer w;
                    Net::WriteWelcome(w, session->unitId,
                                      static_cast<uint8_t>(session->team));
                    SendReliable(session->peer, w);
                    if (config.log)
                        std::printf("player joined: %s on team %d, unit %d\n",
                                    kClassDefs[session->classId].name, session->team,
                                    session->unitId);
                }
            }
            else if (session && type == Net::MsgType::Cmd && session->joined)
            {
                // The packet is the newest command plus its recent
                // predecessors; anything already seen fails the sequence
                // guard and costs nothing. The state channel drops
                // out-of-order arrivals, so within one packet the entries
                // are the only ordering to keep.
                cmdScratch.clear();
                Net::ReadCmds(r, cmdScratch);
                for (const Net::CmdEntry& entry : cmdScratch)
                    if (entry.seq > session->acked &&
                        (session->queue.empty() || entry.seq > session->queue.back().first))
                        session->queue.push_back({ entry.seq, entry.cmd });
            }
            enet_packet_destroy(netEvent.packet);
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT:
        {
            Session* session = static_cast<Session*>(netEvent.peer->data);
            if (session)
            {
                if (session->joined)
                {
                    // The soldier leaves without dying; the slot goes back to
                    // the AI on the reinforcement clock.
                    if (session->unitId >= 0)
                        world.RemoveUnit(session->unitId);
                    world.ReleaseSlot(session->slot);
                    if (config.log)
                        std::printf("player left team %d\n", session->team);
                }
                netEvent.peer->data = nullptr;
                std::erase_if(sessions, [session](const auto& s) { return s.get() == session; });
                if (config.log)
                    std::printf("peer disconnected (%zu online)\n", sessions.size());
            }
            break;
        }
        default:
            break;
        }
    }
}

void Server::Impl::Respawns(World& world, float dt)
{
    // A match that's over hands nobody back their soldier: the next one puts
    // everybody on the field at once, and a wait that expired during the
    // result is served by that instead.
    for (auto& session : sessions)
    {
        if (!session->joined || session->unitId >= 0 || world.MatchOver())
            continue;
        session->respawnTimer -= dt;
        if (session->respawnTimer <= 0.0f)
        {
            session->unitId = world.SpawnRemote(kClassDefs[session->classId], session->slot);
            Net::Writer w;
            Net::WriteRespawned(w, session->unitId);
            SendReliable(session->peer, w);
        }
    }
}

void Server::Impl::Step(World& world)
{
    for (auto& session : sessions)
    {
        if (!session->joined || session->unitId < 0)
            continue;

        // Spend backlog beyond the jitter cushion: a skipped command folds
        // its edges into the one that will actually run, so a reload pressed
        // during a hiccup still happens — once.
        while (session->queue.size() > kCmdBacklog + 1)
        {
            const auto [seq, skipped] = session->queue.front();
            session->queue.pop_front();
            session->queue.front().second.reload |= skipped.reload;
            session->queue.front().second.grenade |= skipped.grenade;
            session->queue.front().second.ability |= skipped.ability;
            session->acked = seq;
        }

        if (!session->queue.empty())
        {
            const auto [seq, cmd] = session->queue.front();
            session->queue.pop_front();
            world.SetCommand(session->unitId, cmd);
            session->acked = seq;
            session->held = cmd;
            session->held.reload = false;
            session->held.grenade = false;
            session->held.ability = false;
        }
        else
        {
            // Starved: reapply the held command, edges long spent. The ack
            // stays put — nothing new was consumed, and the client's pending
            // list is still pending.
            world.SetCommand(session->unitId, session->held);
        }
    }

    events.clear();
    world.Tick(events);
    ++tick;
    eventsSeen += static_cast<long long>(events.size());

    // A death among the connected starts their respawn clock; the AI's dead
    // are the World's own business.
    for (const Event& ev : events)
        if (ev.type == Event::Type::Death)
            for (auto& session : sessions)
                if (session->joined && session->unitId == ev.unit)
                {
                    session->unitId = -1;
                    session->respawnTimer = World::kRespawnDelay;
                    // The dead watch from where they fell.
                    session->viewPos = { ev.pos.x, ev.pos.z };
                    // Orders addressed to a dead soldier die with them; the
                    // fresh one starts on fresh commands.
                    session->queue.clear();
                    session->held = {};
                }

    Rematch(world);
    Broadcast(world);
}

void Server::Impl::Rematch(World& world)
{
    if (world.MatchOver() != matchOver)
    {
        matchOver = world.MatchOver();
        if (matchOver && config.log)
        {
            const int winner = world.Winner();
            std::printf("match over: %s %d, %s %d (%s)\n", GetTeamDef(0).name, world.Score(0),
                        GetTeamDef(1).name, world.Score(1),
                        winner < 0 ? "draw" : GetTeamDef(winner).name);
        }
    }
    if (!matchOver || world.Intermission() > 0.0f)
        return;

    // A new match on the same ground: the arena is emptied and both squads
    // come up to strength again, then everyone still connected takes back the
    // slot they were holding. They get a Respawned, which is the message their
    // client already knows means "here is your new soldier".
    world.Reset();
    world.StartMatch();
    for (auto& session : sessions)
    {
        if (!session->joined)
            continue;
        session->slot = world.ClaimSlot(session->team);
        session->team = world.SlotTeam(session->slot);
        session->unitId = world.SpawnRemote(kClassDefs[session->classId], session->slot);
        session->respawnTimer = 0.0f;
        session->queue.clear();
        session->held = {};
        if (const Unit* me = world.UnitById(session->unitId))
            session->viewPos = { me->pos.x, me->pos.z };
        Net::Writer w;
        Net::WriteRespawned(w, session->unitId);
        SendReliable(session->peer, w);
    }
    matchOver = false;
    if (config.log)
        std::printf("new match: %.0f minutes\n", World::kMatchLength / 60.0f);
}

void Server::Impl::Broadcast(World& world)
{
    // The snapshot is fog-filtered per viewer (a soldier nobody on your side
    // can see is absent from the bytes, not merely undrawn), and events go by
    // earshot, so a firefight behind a wall is still a thing you hear.
    for (auto& session : sessions)
    {
        if (!session->joined)
            continue;

        if (session->unitId >= 0)
            if (const Unit* me = world.UnitById(session->unitId))
                session->viewPos = { me->pos.x, me->pos.z };

        if (!events.empty())
        {
            heard.clear();
            for (const Event& ev : events)
            {
                const float dx = ev.pos.x - session->viewPos.x;
                const float dz = ev.pos.z - session->viewPos.y;
                if ((ev.unit >= 0 && ev.unit == session->unitId) ||
                    dx * dx + dz * dz <= kEventRange * kEventRange)
                    heard.push_back(ev);
            }
            if (!heard.empty())
            {
                Net::Writer eventMsg;
                Net::WriteEvents(eventMsg, heard);
                SendState(session->peer, eventMsg);
            }
        }

        // What this client's side can see, from every eye it has: their own
        // first — which is their soldier while they have one and the spot they
        // fell on while they don't, reaching as far as the class they picked
        // sees — then the rest of the squad, each with its own class's reach,
        // that one left out so a living player isn't looked through twice.
        // The session's class is the authority for their own eye because it
        // outlives their unit, which is the whole reason viewPos is kept here
        // as well.
        eyes.clear();
        eyes.push_back({ { session->viewPos.x, session->viewPos.y },
                         kClassDefs[session->classId].sight });
        world.TeamEyes(session->team, eyes, session->unitId);

        Net::Writer snap;
        Net::WriteSnapshotVisible(snap, world, tick, eyes, session->unitId, session->team,
                                  session->slot);
        Net::WriteSnapshotOwn(snap, session->unitId >= 0 ? world.UnitById(session->unitId)
                                                         : nullptr,
                              session->acked);
        SendState(session->peer, snap);
    }
}
