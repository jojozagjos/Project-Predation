#include "Engine/Net/LobbyClient.h"

#include "Engine/Core/Log.h"
#include "Engine/Net/SocketSystem.h"
#include "Engine/Net/Transport.h"

#include <algorithm>
#include <chrono>

#if defined(_WIN32)
#    include <winsock2.h>
#    include <ws2tcpip.h>
#else
#    include <arpa/inet.h>
#    include <netdb.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#endif

namespace pred
{
namespace
{

// How often to ask the server while it has not answered, to tell it a lobby is still open, to try
// the next address of a player, and to ask what is open.
constexpr float kAskSeconds = 1.0f;
constexpr float kUpdateSeconds = 2.0f;
constexpr float kProbeSeconds = 0.2f;
constexpr float kListSeconds = 2.0f;
// A host that hears nothing back from its updates for this long says so, and keeps trying.
constexpr float kServerSilenceSeconds = 10.0f;
// And a host whose server has gone asks again this often, rather than giving up for good: a wifi
// blip should cost a lobby a few seconds, not the evening.
constexpr float kRetrySeconds = 5.0f;

// A name looked up away from the frame. Offline, a lookup can take seconds to fail, and the game
// would sit frozen for all of them.
uint32_t Resolve(const std::string& name)
{
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* results = nullptr;
    if (getaddrinfo(name.c_str(), nullptr, &hints, &results) != 0 || results == nullptr)
    {
        return 0;
    }
    const uint32_t address = ntohl(reinterpret_cast<const sockaddr_in*>(results->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(results);
    return address;
}

} // namespace

LobbyClient::~LobbyClient()
{
    Close(nullptr);
}

std::vector<LobbyEndpoint> LobbyClient::InsideCandidates(uint16_t port) const
{
    // Every address this machine has on a network, for somebody in the same house: two PCs behind
    // one router usually cannot reach each other through its outside address, but can directly.
    std::vector<LobbyEndpoint> found;
    for (const std::string& address : LocalNetworkAddresses())
    {
        LobbyEndpoint endpoint;
        if (LobbyEndpoint::Parse(address, port, endpoint) && found.size() < kLobbyMaxCandidates - 1)
        {
            found.push_back(endpoint);
        }
    }
    return found;
}

bool LobbyClient::Begin(Role role, const Settings& settings)
{
    Close(nullptr);
    if (settings.server.empty())
    {
        m_message = "No lobby server is set.";
        m_state = State::Failed;
        return false;
    }
    m_role = role;
    m_settings = settings;
    m_elapsed = 0.0f;
    m_sendTimer = 0.0f;
    m_message.clear();
    m_server = LobbyEndpoint{};

    LobbyEndpoint dotted;
    if (LobbyEndpoint::Parse(settings.server, settings.serverPort, dotted))
    {
        m_server = dotted;
        m_state = State::Contacting;
        return true;
    }
    // A name, such as a free dynamic-DNS one pointing at the server. Looked up on another thread.
    if (!SocketSystem::Acquire())
    {
        Fail("Networking could not be started on this machine.");
        return false;
    }
    m_socketSystem = true;
    m_resolving = std::async(std::launch::async, Resolve, settings.server);
    m_state = State::Resolving;
    return true;
}

bool LobbyClient::Host(const Settings& settings, const std::string& name, bool listed, uint8_t maxPlayers,
                       uint16_t gamePort)
{
    if (!Begin(Role::Host, settings))
    {
        return false;
    }
    m_name = name;
    m_listed = listed;
    m_maxPlayers = maxPlayers;
    m_gamePort = gamePort;
    PRED_LOG_INFO(Network, "Lobby: opening \"{}\" at {}:{}{}", name, settings.server, settings.serverPort,
                  listed ? ", public" : "");
    return true;
}

bool LobbyClient::Join(const Settings& settings, uint32_t code, uint16_t localPort)
{
    if (!Begin(Role::Guest, settings))
    {
        return false;
    }
    m_joinCode = code;
    m_localPort = localPort;
    PRED_LOG_INFO(Network, "Lobby: asking {}:{} for {}", settings.server, settings.serverPort,
                  DecodeLobbyCode(code));
    return true;
}

bool LobbyClient::Browse(const Settings& settings)
{
    return Begin(Role::Browser, settings);
}

void LobbyClient::Close(Transport* transport)
{
    if (m_role == Role::Host && m_code != 0 && transport != nullptr && m_server.Valid())
    {
        LobbyPacket close;
        close.kind = LobbyMessage::Close;
        close.code = m_code;
        close.secret = m_secret;
        SendToServer(transport, close);
        PRED_LOG_INFO(Network, "Lobby: closed {}", CodeText());
    }
    if (m_resolving.valid())
    {
        // A lookup cannot be cancelled. Waited for rather than abandoned, because a future from
        // std::async waits in its destructor anyway, and doing it here says so.
        m_resolving.wait();
        m_resolving = {};
    }
    if (m_socketSystem)
    {
        SocketSystem::Release();
        m_socketSystem = false;
    }
    m_role = Role::None;
    m_state = State::Idle;
    m_code = 0;
    m_secret = 0;
    m_seenAs = LobbyEndpoint{};
    m_extraCandidates.clear();
    m_punches.clear();
    m_joinCode = 0;
    m_token = 0;
    m_hostCandidates.clear();
    m_reached = LobbyEndpoint{};
    m_lobbyName.clear();
    m_lobbies.clear();
    m_heard = false;
}

void LobbyClient::Fail(const std::string& message)
{
    if (m_state != State::Failed || m_message != message)
    {
        PRED_LOG_WARN(Network, "Lobby: {}", message);
    }
    m_state = State::Failed;
    m_message = message;
}

void LobbyClient::SetStatus(uint8_t players, bool started, bool listed, const std::string& name)
{
    m_players = players;
    m_started = started;
    m_listed = listed;
    m_name = name;
}

void LobbyClient::AddCandidate(const LobbyEndpoint& candidate)
{
    if (candidate.Valid() && std::find(m_extraCandidates.begin(), m_extraCandidates.end(), candidate) ==
                                 m_extraCandidates.end())
    {
        m_extraCandidates.push_back(candidate);
    }
}

int LobbyClient::Arriving() const
{
    int arriving = 0;
    for (const Punch& punch : m_punches)
    {
        arriving += punch.reached ? 0 : 1;
    }
    return arriving;
}

void LobbyClient::SendToServer(Transport* transport, const LobbyPacket& packet)
{
    SendTo(transport, m_server, packet);
}

void LobbyClient::SendTo(Transport* transport, const LobbyEndpoint& to, const LobbyPacket& packet)
{
    if (transport == nullptr || !to.Valid())
    {
        return;
    }
    const std::vector<uint8_t> bytes = EncodeLobby(packet);
    transport->SendUnframed(to.AddressText(), to.port, bytes.data(), bytes.size());
}

LobbyPacket LobbyClient::HostPacket(LobbyMessage kind) const
{
    LobbyPacket packet;
    packet.kind = kind;
    packet.version = m_settings.version;
    packet.code = m_code;
    packet.secret = m_secret;
    packet.players = m_players;
    packet.maxPlayers = m_maxPlayers;
    packet.started = m_started;
    packet.listed = m_listed;
    packet.name = m_name;
    packet.candidates = InsideCandidates(m_gamePort);
    for (const LobbyEndpoint& extra : m_extraCandidates)
    {
        if (packet.candidates.size() < kLobbyMaxCandidates)
        {
            packet.candidates.push_back(extra);
        }
    }
    return packet;
}

void LobbyClient::Poll(float dt, Transport* transport)
{
    if (m_role == Role::None)
    {
        return;
    }

    if (m_state == State::Resolving)
    {
        if (m_resolving.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            return;
        }
        const uint32_t address = m_resolving.get();
        m_resolving = {};
        if (address == 0)
        {
            Fail("Could not find the lobby server (" + m_settings.server +
                 "). Check your internet connection.");
            return;
        }
        m_server.address = address;
        m_server.port = m_settings.serverPort;
        m_state = State::Contacting;
        m_sendTimer = 0.0f;
        m_elapsed = 0.0f;
    }
    if (transport == nullptr || !m_server.Valid())
    {
        return;
    }

    // What arrived.
    for (Transport::UnframedDatagram& datagram : transport->TakeUnframed())
    {
        LobbyPacket packet;
        LobbyEndpoint from;
        if (!LobbyEndpoint::Parse(datagram.address, datagram.port, from) ||
            !DecodeLobby(datagram.bytes.data(), datagram.bytes.size(), packet))
        {
            continue;
        }
        if (from == m_server)
        {
            HandleServer(transport, packet);
        }
        else
        {
            HandlePeer(transport, packet, from);
        }
    }

    m_elapsed += dt;
    m_sendTimer -= dt;

    switch (m_role)
    {
    case Role::Host:
    {
        const bool open = m_state == State::Open;
        if (m_sendTimer <= 0.0f)
        {
            // Asking for a code, or saying the lobby is still here. After a server has gone quiet,
            // asked less often, and for the same code back if it can have it.
            m_sendTimer = open ? kUpdateSeconds : (m_state == State::Failed ? kRetrySeconds : kAskSeconds);
            SendToServer(transport, HostPacket(open ? LobbyMessage::Update : LobbyMessage::Host));
        }
        if (open && m_elapsed > kServerSilenceSeconds)
        {
            Fail("Lost touch with the lobby server. Still trying. People already here can stay.");
        }
        else if (m_state == State::Contacting && m_elapsed > m_settings.answerSeconds)
        {
            Fail("The lobby server is not answering, so there is no code to share. People on your "
                 "network can still join from their list.");
        }

        // Sending to each guest the server introduced, at every address it might have, which is what
        // opens this machine's router to them.
        for (Punch& punch : m_punches)
        {
            punch.remaining -= dt;
            punch.sendTimer -= dt;
            if (punch.reached || punch.sendTimer > 0.0f)
            {
                continue;
            }
            punch.sendTimer = kProbeSeconds;
            LobbyPacket probe;
            probe.kind = LobbyMessage::Probe;
            probe.token = punch.token;
            for (const LobbyEndpoint& candidate : punch.candidates)
            {
                SendTo(transport, candidate, probe);
            }
        }
        m_punches.erase(std::remove_if(m_punches.begin(), m_punches.end(),
                                       [](const Punch& punch) { return punch.remaining <= 0.0f; }),
                        m_punches.end());
        break;
    }

    case Role::Guest:
    {
        if (m_state == State::Reached || m_state == State::Failed)
        {
            break;
        }
        if (m_sendTimer <= 0.0f)
        {
            // Asked again even once introduced, less often: if the host never heard about this guest,
            // it is not sending, and without its half nothing gets through its router. Asking again
            // tells it again.
            m_sendTimer = m_state == State::Punching ? 1.5f : kAskSeconds;
            LobbyPacket join;
            join.kind = LobbyMessage::Join;
            join.version = m_settings.version;
            join.code = m_joinCode;
            join.candidates = InsideCandidates(m_localPort);
            SendToServer(transport, join);
        }
        if (m_state == State::Contacting && m_elapsed > m_settings.answerSeconds)
        {
            Fail("The lobby server is not answering. Check your internet connection, or join by "
                 "address instead.");
            break;
        }
        if (m_state == State::Punching)
        {
            m_probeTimer -= dt;
            if (m_probeTimer <= 0.0f)
            {
                m_probeTimer = kProbeSeconds;
                LobbyPacket probe;
                probe.kind = LobbyMessage::Probe;
                probe.token = m_token;
                for (const LobbyEndpoint& candidate : m_hostCandidates)
                {
                    SendTo(transport, candidate, probe);
                }
            }
            if (m_elapsed > m_settings.punchSeconds)
            {
                Fail("Found the game, but could not connect to the host directly: one of your routers "
                     "blocks it. Try having somebody else host, or both use Tailscale and join by "
                     "address.");
            }
        }
        break;
    }

    case Role::Browser:
        if (m_sendTimer <= 0.0f)
        {
            m_sendTimer = kListSeconds;
            LobbyPacket list;
            list.kind = LobbyMessage::List;
            list.version = m_settings.version;
            SendToServer(transport, list);
        }
        if (m_elapsed > m_settings.answerSeconds)
        {
            // Said, but still asking: the list fills in by itself if the server comes back.
            m_message = "The lobby server is not answering.";
            m_heard = false;
        }
        break;

    case Role::None:
        break;
    }
}

void LobbyClient::HandleServer(Transport* transport, const LobbyPacket& packet)
{
    switch (packet.kind)
    {
    case LobbyMessage::Hosted:
        if (m_role != Role::Host)
        {
            break;
        }
        if (m_state != State::Open || m_code != packet.code)
        {
            PRED_LOG_INFO(Network, "Lobby: open as {}, seen from outside as {}", DecodeLobbyCode(packet.code),
                          packet.seenAs.ToString());
        }
        m_code = packet.code;
        m_secret = packet.secret;
        m_seenAs = packet.seenAs;
        m_state = State::Open;
        m_message.clear();
        m_elapsed = 0.0f;
        break;

    case LobbyMessage::Introduce:
        if (m_role == Role::Host && packet.toHost)
        {
            // A guest on the way. Already sending to them? Then this is the server repeating itself.
            for (Punch& punch : m_punches)
            {
                if (punch.token == packet.token)
                {
                    return;
                }
            }
            Punch punch;
            punch.token = packet.token;
            punch.candidates = packet.candidates;
            punch.remaining = m_settings.punchSeconds;
            m_punches.push_back(std::move(punch));
            PRED_LOG_INFO(Network, "Lobby: somebody is joining from {}, sending to {} address(es)",
                          packet.candidates.empty() ? std::string("?") : packet.candidates.front().ToString(),
                          packet.candidates.size());
        }
        else if (m_role == Role::Guest && !packet.toHost && m_state == State::Contacting)
        {
            m_token = packet.token;
            m_hostCandidates = packet.candidates;
            m_lobbyName = packet.name;
            m_seenAs = packet.seenAs;
            m_state = State::Punching;
            m_elapsed = 0.0f;
            std::string where;
            for (const LobbyEndpoint& candidate : m_hostCandidates)
            {
                where += (where.empty() ? "" : ", ") + candidate.ToString();
            }
            PRED_LOG_INFO(Network, "Lobby: found \"{}\"; trying {}", packet.name, where);
        }
        break;

    case LobbyMessage::Rejected:
        if (m_role == Role::Host && m_state == State::Open && packet.reason == LobbyRejection::NoSuchLobby)
        {
            // The server has forgotten this lobby: it restarted, or it did not hear from us for too
            // long. Ask again, for the same code -- it hands it back if nobody else has it -- so
            // anybody already holding the code can still use it.
            PRED_LOG_WARN(Network, "Lobby: the server forgot {}; opening it again", CodeText());
            m_state = State::Contacting;
            m_elapsed = 0.0f;
            m_sendTimer = 0.0f;
            (void)transport;
        }
        else if (m_role == Role::Guest && m_state == State::Contacting)
        {
            Fail(Describe(packet.reason));
        }
        else if (m_role == Role::Host && m_state != State::Open)
        {
            Fail(Describe(packet.reason));
        }
        break;

    case LobbyMessage::Listing:
        if (m_role == Role::Browser)
        {
            m_lobbies = packet.lobbies;
            m_heard = true;
            m_message.clear();
            m_state = State::Open;
            m_elapsed = 0.0f;
        }
        break;

    default:
        break;
    }
}

void LobbyClient::HandlePeer(Transport* transport, const LobbyPacket& packet, const LobbyEndpoint& from)
{
    if (packet.kind != LobbyMessage::Probe && packet.kind != LobbyMessage::ProbeReply)
    {
        return;
    }
    if (m_role == Role::Host)
    {
        for (Punch& punch : m_punches)
        {
            if (punch.token != packet.token)
            {
                continue;
            }
            // Through. Answer it, so the guest knows which of this machine's addresses works, and
            // stop sending: the game's own handshake takes it from here.
            if (packet.kind == LobbyMessage::Probe)
            {
                LobbyPacket reply;
                reply.kind = LobbyMessage::ProbeReply;
                reply.token = packet.token;
                SendTo(transport, from, reply);
            }
            if (!punch.reached)
            {
                PRED_LOG_INFO(Network, "Lobby: reached the guest at {}", from.ToString());
            }
            punch.reached = true;
            return;
        }
        return;
    }
    if (m_role == Role::Guest && m_state == State::Punching && packet.token == m_token)
    {
        // Whichever of the host's addresses answered first -- or the host's own probe arriving,
        // which proves the same thing. Either way, that is the one to connect to.
        m_reached = from;
        m_state = State::Reached;
        PRED_LOG_INFO(Network, "Lobby: reached the host at {} after {:.1f} s", from.ToString(), m_elapsed);
        if (packet.kind == LobbyMessage::Probe)
        {
            LobbyPacket reply;
            reply.kind = LobbyMessage::ProbeReply;
            reply.token = packet.token;
            SendTo(transport, from, reply);
        }
    }
}

} // namespace pred
