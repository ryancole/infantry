#pragma once

#include <cstdint>
#include <string>
#include <vector>

// How a client finds a server without being told an address: it shouts on
// the local network and listens for who answers. One UDP broadcast a second
// while the join screen is open, one small reply per server — no directory,
// no registration, no infrastructure, which is the whole reason it works on
// a LAN you set up in a living room. An internet server browser is a
// different feature with a landlord (somewhere to host the list), and this
// deliberately isn't it.
//
// The exchange rides its own port and its own four-byte magics, apart from
// the game protocol: a probe is answerable by any build whose reply format
// matches, and the protocol version rides along so the join screen can show
// a server the game could actually join.
namespace Discovery
{
    constexpr uint16_t kPort = 27651;

    // One server that answered, as the join screen shows it: where, who, and
    // how full. `sinceSeen` ages toward the cutoff — a server that stops
    // answering fades off the list rather than lingering as a dead row.
    struct ServerInfo
    {
        std::string host; // dotted address the reply came from
        uint16_t gamePort;
        std::string name; // the machine's own name for itself
        int humans;
        int capacity;
        float sinceSeen;
    };

    // The server's half: answers any probe with who we are and how full.
    // Owns one non-blocking socket; Poll drains it and costs nothing when
    // nobody is asking.
    class Responder
    {
    public:
        ~Responder();

        // Binds the discovery port and learns the machine's name. False if
        // the port can't be had (a second server on the same box — the first
        // one answers for the address, which is at least honest).
        bool Start(uint16_t gamePort);

        // What the next reply will claim. The server updates this as players
        // come and go; stale fullness on a browser row is a small lie that
        // reads as a bug.
        void SetStanding(int humans, int capacity);

        // Answers everything that has arrived since last time.
        void Poll();

    private:
        uintptr_t m_socket = static_cast<uintptr_t>(-1);
        bool m_wsa = false;
        uint16_t m_gamePort = 0;
        std::string m_name;
        int m_humans = 0;
        int m_capacity = 0;
    };

    // The client's half: shouts, collects, forgets. Alive only while the
    // join screen is open — Start on the way in, Stop on the way out, and
    // the socket goes with it.
    class Scan
    {
    public:
        ~Scan();

        bool Start();
        void Stop();

        // Sends the next probe when one is due, harvests replies, and ages
        // the list. Call every frame the screen is up.
        void Poll(float dt);

        const std::vector<ServerInfo>& Servers() const { return m_servers; }

    private:
        uintptr_t m_socket = static_cast<uintptr_t>(-1);
        bool m_wsa = false;
        float m_sinceProbe = 1e9f; // huge, so the first Poll probes at once
        std::vector<ServerInfo> m_servers;
    };
}
