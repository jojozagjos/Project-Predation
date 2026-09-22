#pragma once

#include "Engine/Net/LobbyDirectory.h"

#include <cstdint>
#include <string>

namespace pred
{

// The lobby server: one UDP socket around a LobbyDirectory.
//
// All the judgement is in the directory, which is tested without a network; this is the part that
// cannot be, so there is as little of it as possible. The standalone server program is a loop around
// Poll, and the tests run one of these in-process on a spare port to join real games through it.
//
// Portable like the directory: it is built on Linux, on the machine that runs it, from four files.
class LobbyServer
{
public:
    LobbyServer() = default;
    ~LobbyServer();
    LobbyServer(const LobbyServer&) = delete;
    LobbyServer& operator=(const LobbyServer&) = delete;

    // Opens the port. Zero picks any free one, which is what a test wants; Port() says which.
    bool Start(uint16_t port, const LobbyDirectory::Settings& settings = LobbyDirectory::Settings{});
    void Stop();
    bool Running() const;
    uint16_t Port() const { return m_port; }

    // Reads everything waiting, answers it, and forgets lobbies that have gone quiet. `now` is in
    // seconds from any fixed point.
    void Poll(double now);

    LobbyDirectory& Directory() { return m_directory; }
    const std::string& Message() const { return m_message; }

private:
    int64_t m_socket = -1; // opaque so this header does not drag in the socket headers
    bool m_socketSystem = false;
    uint16_t m_port = 0;
    double m_lastExpire = 0.0;
    LobbyDirectory m_directory;
    std::string m_message;
};

} // namespace pred
