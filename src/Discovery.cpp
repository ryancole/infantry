#include "Discovery.h"

#include "Net.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace
{
    // Four bytes of "this is us" at the front of both packets, so a stray
    // datagram on the port reads as noise instead of a server. Question and
    // reply differ in the last letter.
    constexpr char kProbeMagic[4] = { 'I', 'N', 'F', 'Q' };
    constexpr char kReplyMagic[4] = { 'I', 'N', 'F', 'R' };

    constexpr float kProbeInterval = 1.0f; // one shout a second is plenty
    constexpr float kForgetAfter = 4.0f;   // missed four probes: gone

    SOCKET OpenUdp(bool broadcast, uint16_t bindPort, bool& wsaOut)
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return INVALID_SOCKET;
        wsaOut = true;

        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET)
            return INVALID_SOCKET;

        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        if (broadcast)
        {
            BOOL yes = TRUE;
            setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes),
                       sizeof yes);
        }

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(bindPort);
        if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0)
        {
            closesocket(s);
            return INVALID_SOCKET;
        }
        return s;
    }
}

namespace Discovery
{
    // --- Responder -----------------------------------------------------------

    Responder::~Responder()
    {
        if (m_socket != static_cast<uintptr_t>(-1))
            closesocket(static_cast<SOCKET>(m_socket));
        if (m_wsa)
            WSACleanup();
    }

    bool Responder::Start(uint16_t gamePort)
    {
        m_gamePort = gamePort;

        char name[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD len = sizeof name;
        if (GetComputerNameA(name, &len))
            m_name.assign(name, len);
        else
            m_name = "INFANTRY SERVER";
        if (m_name.size() > 24)
            m_name.resize(24); // a browser row, not a hostname field

        const SOCKET s = OpenUdp(false, kPort, m_wsa);
        if (s == INVALID_SOCKET)
            return false;
        m_socket = static_cast<uintptr_t>(s);
        return true;
    }

    void Responder::SetStanding(int humans, int capacity)
    {
        m_humans = humans;
        m_capacity = capacity;
    }

    void Responder::Poll()
    {
        if (m_socket == static_cast<uintptr_t>(-1))
            return;
        const SOCKET s = static_cast<SOCKET>(m_socket);

        char buf[64];
        sockaddr_in from = {};
        int fromLen = sizeof from;
        int got;
        while ((got = recvfrom(s, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from),
                               &fromLen)) > 0)
        {
            fromLen = sizeof from;
            if (got < 5 || std::memcmp(buf, kProbeMagic, 4) != 0 ||
                static_cast<uint8_t>(buf[4]) != Net::kProtocolVersion)
                continue;

            char reply[64];
            size_t at = 0;
            std::memcpy(reply + at, kReplyMagic, 4);
            at += 4;
            reply[at++] = static_cast<char>(Net::kProtocolVersion);
            std::memcpy(reply + at, &m_gamePort, sizeof m_gamePort);
            at += sizeof m_gamePort;
            reply[at++] = static_cast<char>(std::clamp(m_humans, 0, 255));
            reply[at++] = static_cast<char>(std::clamp(m_capacity, 0, 255));
            reply[at++] = static_cast<char>(m_name.size());
            std::memcpy(reply + at, m_name.data(), m_name.size());
            at += m_name.size();

            sendto(s, reply, static_cast<int>(at), 0, reinterpret_cast<const sockaddr*>(&from),
                   sizeof from);
        }
    }

    // --- Scan ------------------------------------------------------------------

    Scan::~Scan()
    {
        Stop();
    }

    bool Scan::Start()
    {
        Stop();
        const SOCKET s = OpenUdp(true, 0, m_wsa); // any port; replies come back to it
        if (s == INVALID_SOCKET)
            return false;
        m_socket = static_cast<uintptr_t>(s);
        m_sinceProbe = 1e9f;
        m_servers.clear();
        return true;
    }

    void Scan::Stop()
    {
        if (m_socket != static_cast<uintptr_t>(-1))
        {
            closesocket(static_cast<SOCKET>(m_socket));
            m_socket = static_cast<uintptr_t>(-1);
        }
        if (m_wsa)
        {
            WSACleanup();
            m_wsa = false;
        }
        m_servers.clear();
    }

    void Scan::Poll(float dt)
    {
        if (m_socket == static_cast<uintptr_t>(-1))
            return;
        const SOCKET s = static_cast<SOCKET>(m_socket);

        // The shout. Broadcast reaches the LAN; a second copy goes to the
        // machine itself, because a server on this very box is the most
        // common case while developing and the least worth losing to a
        // broadcast a firewall or adapter decided not to loop back.
        m_sinceProbe += dt;
        if (m_sinceProbe >= kProbeInterval)
        {
            m_sinceProbe = 0.0f;
            char probe[5];
            std::memcpy(probe, kProbeMagic, 4);
            probe[4] = static_cast<char>(Net::kProtocolVersion);

            sockaddr_in to = {};
            to.sin_family = AF_INET;
            to.sin_port = htons(kPort);
            to.sin_addr.s_addr = INADDR_BROADCAST;
            sendto(s, probe, sizeof probe, 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
            inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
            sendto(s, probe, sizeof probe, 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
        }

        // The harvest.
        char buf[64];
        sockaddr_in from = {};
        int fromLen = sizeof from;
        int got;
        while ((got = recvfrom(s, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from),
                               &fromLen)) > 0)
        {
            fromLen = sizeof from;
            if (got < 10 || std::memcmp(buf, kReplyMagic, 4) != 0 ||
                static_cast<uint8_t>(buf[4]) != Net::kProtocolVersion)
                continue;

            ServerInfo info;
            char host[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &from.sin_addr, host, sizeof host);
            info.host = host;
            std::memcpy(&info.gamePort, buf + 5, sizeof info.gamePort);
            info.humans = static_cast<uint8_t>(buf[7]);
            info.capacity = static_cast<uint8_t>(buf[8]);
            const size_t nameLen =
                std::min<size_t>(static_cast<uint8_t>(buf[9]), static_cast<size_t>(got) - 10);
            info.name.assign(buf + 10, nameLen);
            info.sinceSeen = 0.0f;

            // The same server answering again refreshes its row in place, so
            // the list holds still under the cursor instead of reshuffling
            // once a second.
            bool known = false;
            for (ServerInfo& existing : m_servers)
                if (existing.host == info.host && existing.gamePort == info.gamePort)
                {
                    const float seen = 0.0f;
                    existing = info;
                    existing.sinceSeen = seen;
                    known = true;
                    break;
                }
            if (!known)
                m_servers.push_back(info);
        }

        // The forgetting.
        for (ServerInfo& info : m_servers)
            info.sinceSeen += dt;
        std::erase_if(m_servers,
                      [](const ServerInfo& info) { return info.sinceSeen > kForgetAfter; });
    }
}
