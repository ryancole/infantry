#include "Level.h"
#include "Net.h"
#include "World.h"

#include <enet/enet.h>

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <memory>
#include <vector>

// The dedicated server: a World, a clock, and now a socket. No window, no
// renderer, no sound device — this binary linking without them is still the
// proof that the simulation is severed from the machines it's watched on.
//
// A connected player is a Session here and a Remote unit in the World.
// Commands come up the wire and are staged into the tick exactly where the
// client's own hands go in solo play; snapshots and events go back down and
// are all a client ever knows about the fight. The AI fills every slot no
// human has claimed, gives one up when somebody joins, and takes it back —
// on the reinforcement clock — when they leave.
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
    // wait, same as the local player's absence from a solo roster).
    struct Session
    {
        ENetPeer* peer = nullptr;
        bool joined = false;
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

int main(int argc, char** argv)
{
    // A server's stdout is usually a pipe or a log file, and a line that sat
    // in a buffer while the process was killed is a line that never happened.
    // Unbuffered costs nothing at one line a second.
    setvbuf(stdout, nullptr, _IONBF, 0);

    // Optional run length in seconds, so a smoke test can end on its own.
    // Unattended is the default: a server's natural lifespan is "until told".
    const float runSeconds = argc > 1 ? static_cast<float>(std::atof(argv[1])) : 0.0f;

    World world;
    try
    {
        world.Init(LevelData::Load("assets/levels/arena01.json"));
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "startup failed: %s\n", e.what());
        return 1;
    }

    if (enet_initialize() != 0)
    {
        std::fprintf(stderr, "enet failed to initialize\n");
        return 1;
    }
    std::atexit(enet_deinitialize);

    ENetAddress address = {};
    address.host = ENET_HOST_ANY;
    address.port = Net::kPort;
    ENetHost* host = enet_host_create(&address, 16, Net::kChannels, 0, 0);
    if (!host)
    {
        std::fprintf(stderr, "could not open port %u\n", Net::kPort);
        return 1;
    }

    // No local class: every slot on both sides is the AI's until players
    // arrive over the wire.
    world.StartMatch(nullptr, 0);

    std::printf("infantry_server: port %u, arena %.0f half-units, %d a side, tick %d Hz\n",
                Net::kPort, world.ArenaHalf(), World::kTeamSize,
                static_cast<int>(1.0f / World::kTickDt + 0.5f));

    std::vector<std::unique_ptr<Session>> sessions;

    LARGE_INTEGER freq, prev;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);

    std::vector<Event> events;
    std::vector<Net::CmdEntry> cmdScratch;
    float accum = 0.0f;
    float sinceReport = 0.0f;
    uint32_t tick = 0;
    long long eventsSeen = 0;

    for (;;)
    {
        // --- The wire: connections in, commands up ---
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

                    // The two ways the door stays shut: a different build of
                    // the game, or a match already full of humans. Refused
                    // loudly and then hung up on — a rejected client should
                    // know why, not time out wondering.
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
                        session->team = world.ClaimSlot(team);
                        session->unitId =
                            world.SpawnRemote(kClassDefs[session->classId], session->team);
                        if (const Unit* me = world.UnitById(session->unitId))
                            session->viewPos = { me->pos.x, me->pos.z };
                        session->joined = true;
                        Net::Writer w;
                        Net::WriteWelcome(w, session->unitId,
                                          static_cast<uint8_t>(session->team));
                        SendReliable(session->peer, w);
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
                            (session->queue.empty() ||
                             entry.seq > session->queue.back().first))
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
                        // The soldier leaves without dying; the slot goes back
                        // to the AI on the reinforcement clock.
                        if (session->unitId >= 0)
                            world.RemoveUnit(session->unitId);
                        world.ReleaseSlot(session->team);
                        std::printf("player left team %d\n", session->team);
                    }
                    netEvent.peer->data = nullptr;
                    std::erase_if(sessions, [session](const auto& s) {
                        return s.get() == session;
                    });
                    std::printf("peer disconnected (%zu online)\n", sessions.size());
                }
                break;
            }
            default:
                break;
            }
        }

        // --- Time ---
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = static_cast<float>(now.QuadPart - prev.QuadPart) /
                   static_cast<float>(freq.QuadPart);
        prev = now;
        dt = std::min(dt, 0.1f); // avoid huge steps after stalls, same as the client

        // Respawn waits run on wall time, like the client's own.
        for (auto& session : sessions)
        {
            if (!session->joined || session->unitId >= 0)
                continue;
            session->respawnTimer -= dt;
            if (session->respawnTimer <= 0.0f)
            {
                session->unitId =
                    world.SpawnRemote(kClassDefs[session->classId], session->team);
                Net::Writer w;
                Net::WriteRespawned(w, session->unitId);
                SendReliable(session->peer, w);
            }
        }

        // --- Ticks ---
        accum += dt;
        while (accum >= World::kTickDt)
        {
            accum -= World::kTickDt;

            for (auto& session : sessions)
            {
                if (!session->joined || session->unitId < 0)
                    continue;

                // Spend backlog beyond the jitter cushion: a skipped command
                // folds its edges into the one that will actually run, so a
                // reload pressed during a hiccup still happens — once.
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
                    // Starved: reapply the held command, edges long spent.
                    // The ack stays put — nothing new was consumed, and the
                    // client's pending list is still pending.
                    world.SetCommand(session->unitId, session->held);
                }
            }

            events.clear();
            world.Tick(nullptr, events);
            ++tick;
            eventsSeen += static_cast<long long>(events.size());

            // A death among the connected starts their respawn clock; the
            // AI's dead are the World's own business.
            for (const Event& ev : events)
                if (ev.type == Event::Type::Death)
                    for (auto& session : sessions)
                        if (session->joined && session->unitId == ev.unit)
                        {
                            session->unitId = -1;
                            session->respawnTimer = World::kRespawnDelay;
                            // The dead watch from where they fell.
                            session->viewPos = { ev.pos.x, ev.pos.z };
                            // Orders addressed to a dead soldier die with
                            // them; the fresh one starts on fresh commands.
                            session->queue.clear();
                            session->held = {};
                        }

            // --- Down the wire: what happened, then where everything is —
            // both as seen from each client's own soldier. The snapshot is
            // fog-filtered per viewer (a soldier you can't see is absent from
            // the bytes, not merely undrawn), and events go by earshot, so a
            // firefight behind a wall is still a thing you hear.
            std::vector<Event> heard;
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

                Net::Writer snap;
                Net::WriteSnapshotVisible(snap, world, tick, session->viewPos,
                                          session->unitId);
                Net::WriteSnapshotOwn(snap,
                                      session->unitId >= 0
                                          ? world.UnitById(session->unitId)
                                          : nullptr,
                                      session->acked);
                SendState(session->peer, snap);
            }
            if (!sessions.empty())
                enet_host_flush(host);
        }

        sinceReport += dt;
        if (sinceReport >= 1.0f)
        {
            sinceReport = 0.0f;
            int alive[2] = { 0, 0 };
            for (const Unit& unit : world.Units())
                if (unit.hp > 0.0f)
                    ++alive[unit.team % 2];
            std::printf("tick %u  blue %d  red %d  shots %zu  players %zu  events %lld\n",
                        tick, alive[0], alive[1], world.Projectiles().size(),
                        sessions.size(), eventsSeen);
        }

        if (runSeconds > 0.0f && tick >= static_cast<uint32_t>(runSeconds / World::kTickDt))
        {
            std::printf("done: %u ticks, %lld events\n", tick, eventsSeen);
            enet_host_destroy(host);
            return 0;
        }

        // The tick is 16.7ms of budget and the work is a fraction of it;
        // sleeping keeps a headless box from spinning a core to wait.
        Sleep(1);
    }
}
