#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The client's end of the wire, and only the wire: connect, pump, send,
// leave. Messages go out and come back as bytes — what they say is Net.h's
// business, and the game never learns a socket exists. ENet stays behind the
// Impl so only NetClient.cpp compiles against it, the same wall Physics puts
// around Jolt.
class NetClient
{
public:
    // Where the connection is in its life. Failed covers everything from "no
    // such host" to "server stopped answering" — the game reacts the same
    // way to all of them, so the distinctions stay in this file.
    enum class Status
    {
        Connecting,
        Connected,
        Failed,
        Closed,
    };

    NetClient();
    ~NetClient();

    // Begins connecting; the answer arrives through Poll. False only if the
    // address couldn't even be resolved to a target worth trying.
    bool Start(const std::string& host, uint16_t port);

    // Pumps the socket: advances the connection state and appends every
    // arrived packet, whole, to `packets` for the caller to decode.
    void Poll(std::vector<std::vector<uint8_t>>& packets);

    Status GetStatus() const { return m_status; }

    // The two channels the protocol defines: things that must arrive, and
    // streams that would rather be fresh than complete.
    void SendReliable(const std::vector<uint8_t>& bytes);
    void SendState(const std::vector<uint8_t>& bytes);

    // Says goodbye politely if the wire is up; either way, after this the
    // status is Closed and Poll has nothing further to say.
    void Disconnect();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    Status m_status = Status::Closed;
};
