#include "NetClient.h"

#include "Net.h"

#include <enet/enet.h>

#include <windows.h>

namespace
{
    // How long a connection attempt gets before it's called what it is. LAN
    // handshakes finish in milliseconds; five seconds is "nobody's there".
    constexpr uint32_t kConnectTimeoutMs = 5000;
}

struct NetClient::Impl
{
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    uint64_t connectStart = 0;
};

NetClient::NetClient() : m_impl(std::make_unique<Impl>())
{
    enet_initialize();
}

NetClient::~NetClient()
{
    Disconnect();
    if (m_impl->host)
        enet_host_destroy(m_impl->host);
    enet_deinitialize();
}

bool NetClient::Start(const std::string& host, uint16_t port)
{
    m_impl->host = enet_host_create(nullptr, 1, Net::kChannels, 0, 0);
    if (!m_impl->host)
    {
        m_status = Status::Failed;
        return false;
    }

    ENetAddress address = {};
    if (enet_address_set_host(&address, host.c_str()) != 0)
    {
        m_status = Status::Failed;
        return false;
    }
    address.port = port;

    m_impl->peer = enet_host_connect(m_impl->host, &address, Net::kChannels, 0);
    if (!m_impl->peer)
    {
        m_status = Status::Failed;
        return false;
    }
    m_impl->connectStart = GetTickCount64();
    m_status = Status::Connecting;
    return true;
}

void NetClient::Poll(std::vector<std::vector<uint8_t>>& packets)
{
    if (!m_impl->host || m_status == Status::Failed || m_status == Status::Closed)
        return;

    ENetEvent event;
    while (enet_host_service(m_impl->host, &event, 0) > 0)
    {
        switch (event.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
            m_status = Status::Connected;
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            packets.emplace_back(event.packet->data,
                                 event.packet->data + event.packet->dataLength);
            enet_packet_destroy(event.packet);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            // The server hanging up mid-game and a refused connection land in
            // the same place: this end has nobody to talk to.
            m_status = Status::Failed;
            m_impl->peer = nullptr;
            break;
        default:
            break;
        }
    }

    if (m_status == Status::Connecting &&
        GetTickCount64() - m_impl->connectStart > kConnectTimeoutMs)
    {
        enet_peer_reset(m_impl->peer);
        m_impl->peer = nullptr;
        m_status = Status::Failed;
    }
}

void NetClient::SendReliable(const std::vector<uint8_t>& bytes)
{
    if (m_status != Status::Connected || !m_impl->peer)
        return;
    enet_peer_send(m_impl->peer, Net::kChannelReliable,
                   enet_packet_create(bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE));
}

void NetClient::SendState(const std::vector<uint8_t>& bytes)
{
    if (m_status != Status::Connected || !m_impl->peer)
        return;
    enet_peer_send(m_impl->peer, Net::kChannelState,
                   enet_packet_create(bytes.data(), bytes.size(), 0));
}

void NetClient::Disconnect()
{
    if (m_impl->peer && m_status == Status::Connected)
    {
        enet_peer_disconnect(m_impl->peer, 0);
        // One brief pump so the goodbye actually leaves the machine; if it
        // doesn't make it, the server's timeout says the same thing slower.
        ENetEvent event;
        enet_host_service(m_impl->host, &event, 100);
    }
    m_impl->peer = nullptr;
    m_status = Status::Closed;
}
