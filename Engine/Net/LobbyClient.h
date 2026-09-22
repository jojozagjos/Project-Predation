#pragma once

#include "Engine/Net/Http.h"
#include "Engine/Net/LobbyProtocol.h"

#include <cstdint>
#include <functional>
#include <future>
#include <string>
#include <vector>

namespace pred
{

class Transport;

// This machine's half of a lobby: asking a STUN server what the game's socket looks like from
// outside, talking to the lobby server, and punching through to the other players once it has
// introduced them.
//
// Everything that has to reach another player goes through the game's own transport, by its side
// door (see Transport.h), because a hole punched through a router is only open for the socket that
// punched it -- and that includes the STUN question, whose answer is only true of the socket that
// asked it. The lobby server is a web service, so talking to it is ordinary HTTPS on a thread of its
// own. Nothing is owned here but timers and what was said; the transport is handed in every Poll, so
// whoever owns it stays its only owner.
//
// Three roles, one at a time:
//
//   Host     registers the game already running on the transport, gets a code, keeps the lobby
//            alive, and when told a guest is coming sends to every address it might be at.
//   Guest    asks to be introduced to a code, then sends to every address the host might be at
//            until one answers. Reached() is where the game should connect.
//   Browser  asks what public lobbies are open. Needs no transport.
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
        Resolving,  // asking the STUN servers what this socket looks like from outside
        Contacting, // asked the lobby server; no answer yet
        Open,       // host: the lobby is open with a code. Browser: the server has answered
        Punching,   // guest: introduced, and sending to the host until it answers
        Reached,    // guest: the host answered. Connect to Reached()
        Failed      // Message() says why, in words for a player
    };

    // Asks the lobby server something. The default is a real web request; tests put a stand-in here.
    using Requester =
        std::function<std::future<HttpResult>(const std::string& method, const std::string& url, const std::string& body)>;

    struct Settings
    {
        std::string server; // the lobby server's web address, such as https://predation-lobby.you.workers.dev
        // Free public STUN servers. Two, from different companies, because comparing their answers is
        // how a router that cannot be punched through is recognised.
        std::vector<std::string> stunServers{"stun.cloudflare.com:3478", "stun.l.google.com:19302"};
        uint16_t version = 0;          // the game's protocol version
        float answerSeconds = 8.0f;    // how long to wait for the lobby server
        float punchSeconds = 12.0f;    // how long to try to reach the other player
        Requester request;             // empty: real web requests
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
    // Stops. A host tells the server its code is finished.
    void Close();

    // Reads what arrived on the transport's side door, and sends whatever is due. A browser may pass
    // no transport.
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
    // How the STUN servers see this machine's game socket: its router's outside address and port.
    const LobbyEndpoint& SeenAs() const { return m_seenAs; }
    // Whether this router gives a different outside port to every destination, which hole punching
    // cannot get through. Known once two STUN servers have answered.
    bool StrictRouter() const { return m_strictRouter; }
    // Guest: where the host answered from, once State is Reached.
    const LobbyEndpoint& Reached() const { return m_reached; }
    // Guest: what the host's lobby is called.
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

    // One STUN server being asked.
    struct StunQuery
    {
        std::string host;
        uint16_t port = 0;
        std::future<uint32_t> resolving;
        LobbyEndpoint server;
        StunTransaction transaction{};
        float sendTimer = 0.0f;
        bool answered = false;
        LobbyEndpoint mapped;
    };

    bool Begin(Role role, const Settings& settings);
    void StartDiscovery();
    void UpdateDiscovery(float dt, Transport* transport);
    void Ask(const std::string& method, const std::string& path, const std::string& body);
    void HandleAnswer(const HttpResult& answer);
    void HandlePeer(Transport* transport, const LobbyPacket& packet, const LobbyEndpoint& from);
    void SendTo(Transport* transport, const LobbyEndpoint& to, const LobbyPacket& packet);
    void Fail(const std::string& message);
    // Everything this machine might be reached at, as the lobby server wants it: "a.b.c.d:port".
    std::vector<std::string> Candidates(uint16_t port) const;
    std::string HostBody() const;

    Role m_role = Role::None;
    State m_state = State::Idle;
    Settings m_settings;

    // The STUN questions, and how long they have been going.
    std::vector<StunQuery> m_stun;
    float m_stunElapsed = 0.0f;
    bool m_discovering = false;
    LobbyEndpoint m_seenAs;
    bool m_strictRouter = false;

    // The one request to the lobby server in flight, if any, and what it was.
    std::future<HttpResult> m_pending;
    std::string m_pendingWhat;
    float m_elapsed = 0.0f;   // since the server last answered
    float m_sendTimer = 0.0f; // until the next request is due
    int m_failures = 0;       // requests in a row with no answer
    std::string m_message;

    // Host.
    uint32_t m_code = 0;
    std::string m_secret;
    std::string m_name;
    bool m_listed = false;
    bool m_started = false;
    uint8_t m_players = 1;
    uint8_t m_maxPlayers = 4;
    uint16_t m_gamePort = 0;
    std::vector<LobbyEndpoint> m_extraCandidates;
    std::vector<Punch> m_punches;

    // Guest.
    uint32_t m_joinCode = 0;
    uint16_t m_localPort = 0;
    uint32_t m_token = 0;
    float m_probeTimer = 0.0f;
    float m_punchElapsed = 0.0f; // since the host's addresses were first tried
    std::vector<LobbyEndpoint> m_hostCandidates;
    LobbyEndpoint m_reached;
    std::string m_lobbyName;

    // Browser.
    std::vector<LobbyListing> m_lobbies;
    bool m_heard = false;
};

} // namespace pred
