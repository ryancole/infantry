#pragma once

#include "Net.h"
#include "World.h"

#include <cstdint>
#include <memory>
#include <string>

// The match, hosted. A World, a clock, and a socket: commands come up the
// wire and are staged into the tick, snapshots and events go back down, and
// the AI fills every slot no human has claimed. This is the only thing in the
// game that runs a match — a dedicated server is this class with a console
// loop around it, and a player deploying on their own machine is this class
// bound to loopback with one client on it. There is no third way to play, and
// that's the point: the arena a lone player fights in is the arena five
// people would, down to the bytes it's described in.
//
// Nothing here knows about a window, a device, or a frame: Update takes the
// wall time that has passed and spends it in whole ticks, so a caller can be
// a headless loop sleeping a millisecond at a time or a game that calls it
// once between reading its keys and drawing its picture. ENet stays behind
// the Impl, the same wall NetClient and Physics put around their libraries.
class Server
{
public:
    // Everything that differs between the two ways of hosting, which turns
    // out to be four small facts about the socket and the console.
    struct Config
    {
        // Zero asks the OS for whatever port is free. That's what an embedded
        // server wants: it's connected to by the process that started it, and
        // squatting the well-known port would stop a real server sharing the
        // machine.
        uint16_t port = Net::kPort;
        // Bind loopback rather than every interface. A match hosted for the
        // player sitting at this machine is nobody else's business, and a
        // socket that can't be reached from off the box is also one Windows
        // never asks about the firewall.
        bool loopbackOnly = false;
        // Answer the LAN's "who's out there". Off for an embedded server, for
        // the same reason as above: an unlisted match can't be joined by
        // somebody wandering past the join screen.
        bool discoverable = false;
        // Narrate joins, departures, and the result to stdout. A console
        // server's only window on the match it's running; a client has no
        // console to narrate to.
        bool log = false;
        // Has to match what the client draws: a level is loaded twice, once
        // for the collision that decides the match and once for the scenery,
        // and nothing negotiates which one at the door yet.
        std::string level = "assets/levels/hardcore.json";
    };

    Server();
    ~Server();

    // Builds the arena, opens the socket, and starts the first match. Throws
    // std::runtime_error if the level won't load — that's a broken install,
    // not a runtime condition — and returns false if the port couldn't be
    // had, which is somebody else already listening on it.
    bool Start(const Config& config);

    // One turn of the loop: the wire in, whole ticks of simulation, the wire
    // out. `dt` is banked in an accumulator and spent in fixed ticks, exactly
    // the way a client banks a frame, so calling this at 30 Hz or 300 Hz runs
    // the same match at the same speed.
    void Update(float dt);

    // Hangs up on everybody and closes the socket. The destructor does it too;
    // this is for callers that want the port back before then.
    void Stop();

    bool Running() const;
    // The port actually bound, which is the one a client has to be told about
    // when Config::port was zero.
    uint16_t Port() const;

    // What a console loop puts in its status line. The World is handed out
    // const because a caller may read the match — the score, the clock, who's
    // standing — but the only thing allowed to advance it is Update.
    const World& Match() const { return m_world; }
    uint32_t Ticks() const;
    long long EventsSeen() const;
    size_t Players() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    World m_world;
};
