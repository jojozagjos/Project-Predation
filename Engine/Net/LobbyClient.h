#pragma once

#include "Engine/Net/LobbyProtocol.h"

#include <cstdint>
#include <future>
#include <string>
#include <vector>

namespace pred
{

class Transport;

// This machine's half of a lobby: talking to the lobby server, and punching through to the other
// players once it has introduced them.
//
// Everything goes through the game's own transport, by its side door (see Transport.h), because a
// hole punched through a router is only open for the socket that punched it. Nothing is owned here but
// timers and what the server said; the transport is handed in every Poll, so whoever owns it -- the
// host's session, or the game while a guest is still finding its way in -- stays its only owner.
//
// Three roles, one at a time:
//
//   Host     registers the game that is already running on the transport, gets a code, keeps the
//            lobby alive, and when a guest is introduced sends to every address it might be at.
//   Guest    asks to be introduced to a code, then sends to every address the host might be at
//            until one answers. Reached() is where the game should connect.
//   Browser  asks what public lobbies are open.
class LobbyClient
{
public:
    enum class Role : uint8_t
    {
        None,
        Host,
        Guest,
        Browser
    };

    enum class State : uint8_t
    {
        Idle,
        Resolving,  // looking up the server's name
        Contacting, // asked the server; no answer yet
        Open,       // host: the lobby is open with a code. Browser: the server has answered
        Punching,   // guest: introduced, and sending to the host until it answers
        Reached,    // guest: the host answered. Connect to Reached()
        Failed      // Message() says why, in words for a player
    };

    struct Settings
    {
        std::string server;                  // a name or a dotted address
        uint16_t serverPort = kLobbyServerPort;
        uint16_t version = 0;                // the game's protocol version
        float answerSeconds = 8.0f;          // how long to wait for the server
        float punchSeconds = 12.0f;          // how long to try to reach the other player
    };

    LobbyClient() = default;
    ~LobbyClient();
    LobbyClient(const LobbyClient&) = delete;
    LobbyClient& operator=(const LobbyClient&) = delete;

    // Opens a lobby for the game listening on `gamePort` of the transport that will be polled.
    bool Host(const Settings& settings, const std::string& name, bool listed, uint8_t maxPlayers,
              uint16_t gamePort);
    // Asks to join a code, from a transport whose socket is on `localPort`.
    bool Join(const Settings& settings, uint32_t code, uint16_t localPort);
    bool Browse(const Settings& settings);
    // Stops. A host tells the server its code is finished, through `transport` if it still has one.
    void Close(Transport* transport);

    // Reads what arrived on the transport's side door, and sends whatever is due.
    void Poll(float dt, Transport* transport);

    // Host: what to tell the server about the game, as it changes. Cheap; call it every frame.
    void SetStatus(uint8_t players, bool started, bool listed, const std::string& name);
    // Host: one more address a guest might reach this machine at, such as the one the router opened
    // for it. Sent with the next update.
    void AddCandidate(const LobbyEndpoint& candidate);

    Role GetRole() const { return m_role; }
    State Status() const { return m_state; }
    bool Active() const { return m_role != Role::None; }
    uint32_t Code() const { return m_code; }
    std::string CodeText() const { return m_code != 0 ? DecodeLobbyCode(m_code) : std::string(); }
    const std::string& Message() const { return m_message; }
    // How the server sees this machine: its router's outside address and port.
    const LobbyEndpoint& SeenAs() const { return m_seenAs; }
    // Guest: where the host answered from, once State is Reached.
    const LobbyEndpoint& Reached() const { return m_reached; }
    // Guest: what the host's lobby is called, and whether its game had started when introduced.
    const std::string& LobbyName() const { return m_lobbyName; }
    // Host: guests the server has introduced in the last few seconds and not yet reached.
    int Arriving() const;
    // Browser: what is open, and whether the server has answered at all. An empty list and a server
    // that is not there look identical otherwise, and only one of them is worth fixing.
    const std::vector<LobbyListing>& Lobbies() const { return m_lobbies; }
    bool Heard() const { return m_heard; }

private:
    struct Punch
    {
        uint32_t token = 0;
        std::vector<LobbyEndpoint> candidates;
        float remaining = 0.0f;
        float sendTimer = 0.0f;
        bool reached = false;
    };

    bool Begin(Role role, const Settings& settings);
    void SendToServer(Transport* transport, const LobbyPacket& packet);
    void SendTo(Transport* transport, const LobbyEndpoint& to, const LobbyPacket& packet);
    void HandleServer(Transport* transport, const LobbyPacket& packet);
    void HandlePeer(Transport* transport, const LobbyPacket& packet, const LobbyEndpoint& from);
    void Fail(const std::string& message);
    LobbyPacket HostPacket(LobbyMessage kind) const;
    std::vector<LobbyEndpoint> InsideCandidates(uint16_t port) const;

    Role m_role = Role::None;
    State m_state = State::Idle;
    Settings m_settings;
    LobbyEndpoint m_server;
    std::future<uint32_t> m_resolving;
    bool m_socketSystem = false;

    float m_elapsed = 0.0f;   // since the server was first asked, or since the last answer
    float m_sendTimer = 0.0f; // until the next message to the server
    std::string m_message;

    // Host.
    uint32_t m_code = 0;
    uint32_t m_secret = 0;
    std::string m_name;
    bool m_listed = false;
    bool m_started = false;
    uint8_t m_players = 1;
    uint8_t m_maxPlayers = 4;
    uint16_t m_gamePort = 0;
    std::vector<LobbyEndpoint> m_extraCandidates;
    std::vector<Punch> m_punches;
    LobbyEndpoint m_seenAs;

    // Guest.
    uint32_t m_joinCode = 0;
    uint16_t m_localPort = 0;
    uint32_t m_token = 0;
    float m_probeTimer = 0.0f;
    std::vector<LobbyEndpoint> m_hostCandidates;
    LobbyEndpoint m_reached;
    std::string m_lobbyName;

    // Browser.
    std::vector<LobbyListing> m_lobbies;
    bool m_heard = false;
};

} // namespace pred
