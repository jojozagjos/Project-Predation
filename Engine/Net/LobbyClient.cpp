#include "Engine/Net/LobbyClient.h"

#include "Engine/Core/Log.h"
#include "Engine/Net/Transport.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <random>
#include <thread>

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

// How often to ask the lobby server while waiting on it, how often a host checks in (and hears who
// is coming), how often to try the next address of a player, and how often to ask what is open.
//
// A host in its lobby checks in every two and a half seconds, because a guest waits that long at
// most to be let in. Once the game has started, every five, which is plenty for somebody joining
// late and halves what an evening costs the server.
constexpr float kAskSeconds = 1.0f;
constexpr float kLobbyUpdateSeconds = 2.5f;
constexpr float kGameUpdateSeconds = 5.0f;
constexpr float kProbeSeconds = 0.2f;
constexpr float kListSeconds = 3.0f;
constexpr float kRetrySeconds = 5.0f;
// STUN: how often to ask each server, and how long to wait for them before going on without.
constexpr float kStunResendSeconds = 0.4f;
constexpr float kStunSeconds = 2.0f;

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

// A future from std::async waits for its work to finish when it is destroyed. Dropping one for a
// request still out on the network would freeze the game until the request timed out, so an
// unwanted one is handed to a thread that waits for it instead.
template <typename T>
void Abandon(std::future<T>& future)
{
    if (future.valid())
    {
        std::thread([pending = std::move(future)]() mutable { pending.wait(); }).detach();
    }
}

std::string BaseUrl(std::string server)
{
    while (!server.empty() && (server.back() == '/' || server.back() == ' '))
    {
        server.pop_back();
    }
    server.erase(0, server.find_first_not_of(' '));
    if (server.find("://") == std::string::npos)
    {
        server = "https://" + server;
    }
    return server;
}

uint32_t TokenFrom(const nlohmann::json& value)
{
    if (!value.is_string())
    {
        return 0;
    }
    try
    {
        return static_cast<uint32_t>(std::stoul(value.get<std::string>(), nullptr, 16));
    }
    catch (...)
    {
        return 0;
    }
}

std::vector<LobbyEndpoint> EndpointsFrom(const nlohmann::json& value)
{
    std::vector<LobbyEndpoint> out;
    if (!value.is_array())
    {
        return out;
    }
    for (const nlohmann::json& item : value)
    {
        LobbyEndpoint endpoint;
        if (item.is_string() && LobbyEndpoint::ParseWithPort(item.get<std::string>(), endpoint) &&
            out.size() < kLobbyMaxCandidates * 2)
        {
            out.push_back(endpoint);
        }
    }
    return out;
}

} // namespace

LobbyClient::~LobbyClient()
{
    Close();
}

bool LobbyClient::Begin(Role role, const Settings& settings)
{
    Close();
    if (settings.server.empty())
    {
        m_message = "No lobby server is set.";
        m_state = State::Failed;
        return false;
    }
    m_role = role;
    m_settings = settings;
    m_settings.server = BaseUrl(settings.server);
    if (!m_settings.request)
    {
        m_settings.request = [](const std::string& method, const std::string& url, const std::string& body)
        { return HttpRequest(method, url, body); };
    }
    m_elapsed = 0.0f;
    m_sendTimer = 0.0f;
    m_failures = 0;
    m_message.clear();
    return true;
}

void LobbyClient::StartDiscovery()
{
    // Asked first, before the lobby server: the answer is the address other players most need.
    m_stun.clear();
    m_stunElapsed = 0.0f;
    m_discovering = true;
    m_state = State::Resolving;
    std::random_device entropy;
    for (const std::string& entry : m_settings.stunServers)
    {
        StunQuery query;
        const size_t colon = entry.rfind(':');
        query.host = colon == std::string::npos ? entry : entry.substr(0, colon);
        query.port = static_cast<uint16_t>(colon == std::string::npos ? 3478 : std::atoi(entry.c_str() + colon + 1));
        for (uint8_t& byte : query.transaction)
        {
            byte = static_cast<uint8_t>(entropy() & 0xFFu);
        }
        if (!LobbyEndpoint::Parse(query.host, query.port, query.server))
        {
            query.resolving = std::async(std::launch::async, Resolve, query.host);
        }
        m_stun.push_back(std::move(query));
    }
}

void LobbyClient::UpdateDiscovery(float dt, Transport* transport)
{
    m_stunElapsed += dt;
    int answered = 0;
    for (StunQuery& query : m_stun)
    {
        if (query.resolving.valid() &&
            query.resolving.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            query.server = LobbyEndpoint{query.resolving.get(), query.port};
        }
        answered += query.answered ? 1 : 0;
        if (query.answered || !query.server.Valid() || transport == nullptr)
        {
            continue;
        }
        query.sendTimer -= dt;
        if (query.sendTimer <= 0.0f)
        {
            query.sendTimer = kStunResendSeconds;
            const std::vector<uint8_t> request = EncodeStunRequest(query.transaction);
            transport->SendUnframed(query.server.AddressText(), query.server.port, request.data(), request.size());
        }
    }

    if (answered < static_cast<int>(m_stun.size()) && m_stunElapsed < kStunSeconds)
    {
        return;
    }

    // Done: every server answered, or it is time to go on with what there is.
    m_discovering = false;
    for (StunQuery& query : m_stun)
    {
        Abandon(query.resolving);
    }
    std::vector<LobbyEndpoint> seen;
    for (const StunQuery& query : m_stun)
    {
        if (query.answered)
        {
            seen.push_back(query.mapped);
        }
    }
    if (!seen.empty())
    {
        m_seenAs = seen.front();
    }
    // Two servers, two different outside ports for the same socket: this router makes a new hole for
    // every destination, and a hole made for the STUN server is no use to anybody else.
    m_strictRouter = seen.size() >= 2 && seen[0].port != seen[1].port;
    PRED_LOG_INFO(Network, "Lobby: seen from outside as {}{}", m_seenAs.Valid() ? m_seenAs.ToString() : "(no answer)",
                  m_strictRouter ? " -- a strict router, which may not let other players through" : "");
    m_state = State::Contacting;
    m_sendTimer = 0.0f;
    m_elapsed = 0.0f;
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
    StartDiscovery();
    PRED_LOG_INFO(Network, "Lobby: opening \"{}\" at {}{}", name, m_settings.server, listed ? ", public" : "");
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
    StartDiscovery();
    PRED_LOG_INFO(Network, "Lobby: asking {} for {}", m_settings.server, DecodeLobbyCode(code));
    return true;
}

bool LobbyClient::Browse(const Settings& settings)
{
    if (!Begin(Role::Browser, settings))
    {
        return false;
    }
    m_state = State::Contacting;
    return true;
}

void LobbyClient::Close()
{
    if (m_role == Role::Host && m_code != 0 && m_settings.request)
    {
        // Said, and not waited for: the code stops working at once rather than when the server
        // notices the host has gone quiet. On a thread of its own, because leaving a game must not
        // wait on a web request.
        nlohmann::json body{{"code", DecodeLobbyCode(m_code)}, {"secret", m_secret}};
        std::future<HttpResult> closing = m_settings.request("POST", m_settings.server + "/close", body.dump());
        Abandon(closing);
        PRED_LOG_INFO(Network, "Lobby: closed {}", CodeText());
    }
    Abandon(m_pending);
    for (StunQuery& query : m_stun)
    {
        Abandon(query.resolving);
    }
    m_stun.clear();
    m_discovering = false;
    m_role = Role::None;
    m_state = State::Idle;
    m_code = 0;
    m_secret.clear();
    m_seenAs = LobbyEndpoint{};
    m_strictRouter = false;
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

std::vector<std::string> LobbyClient::Candidates(uint16_t port) const
{
    // How the outside world sees this socket first, because it is the address that works from
    // anywhere; then every address this machine has on a network, for somebody in the same house --
    // two PCs behind one router usually cannot reach each other through its outside address.
    std::vector<std::string> out;
    const auto add = [&](const LobbyEndpoint& endpoint)
    {
        const std::string text = endpoint.ToString();
        if (endpoint.Valid() && std::find(out.begin(), out.end(), text) == out.end() &&
            out.size() < kLobbyMaxCandidates - 1)
        {
            out.push_back(text);
        }
    };
    add(m_seenAs);
    for (const std::string& address : LocalNetworkAddresses())
    {
        LobbyEndpoint inside;
        if (LobbyEndpoint::Parse(address, port, inside))
        {
            add(inside);
        }
    }
    for (const LobbyEndpoint& extra : m_extraCandidates)
    {
        add(extra);
    }
    return out;
}

std::string LobbyClient::HostBody() const
{
    nlohmann::json body{{"version", m_settings.version}, {"name", m_name},       {"listed", m_listed},
                        {"started", m_started},          {"players", m_players}, {"maxPlayers", m_maxPlayers},
                        {"localPort", m_gamePort},       {"candidates", Candidates(m_gamePort)}};
    if (m_code != 0)
    {
        body["code"] = DecodeLobbyCode(m_code);
        body["secret"] = m_secret;
    }
    return body.dump();
}

void LobbyClient::Ask(const std::string& method, const std::string& path, const std::string& body)
{
    m_pendingWhat = path;
    m_pending = m_settings.request(method, m_settings.server + path, body);
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

void LobbyClient::Poll(float dt, Transport* transport)
{
    if (m_role == Role::None)
    {
        return;
    }

    // What arrived on the game's socket that was not the game's: STUN answers and other players'
    // probes.
    if (transport != nullptr)
    {
        for (Transport::UnframedDatagram& datagram : transport->TakeUnframed())
        {
            LobbyEndpoint from;
            if (!LobbyEndpoint::Parse(datagram.address, datagram.port, from))
            {
                continue;
            }
            const uint8_t* data = datagram.bytes.data();
            const size_t bytes = datagram.bytes.size();
            if (IsStunDatagram(data, bytes))
            {
                for (StunQuery& query : m_stun)
                {
                    LobbyEndpoint mapped;
                    if (!query.answered && DecodeStunResponse(data, bytes, query.transaction, mapped))
                    {
                        query.answered = true;
                        query.mapped = mapped;
                    }
                }
                continue;
            }
            LobbyPacket packet;
            if (DecodeLobby(data, bytes, packet))
            {
                HandlePeer(transport, packet, from);
            }
        }
    }

    if (m_discovering)
    {
        UpdateDiscovery(dt, transport);
        if (m_discovering)
        {
            return;
        }
    }

    // The lobby server's answer, when it comes.
    if (m_pending.valid() && m_pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        HandleAnswer(m_pending.get());
    }

    m_elapsed += dt;
    m_sendTimer -= dt;
    const bool idle = !m_pending.valid();

    switch (m_role)
    {
    case Role::Host:
    {
        if (idle && m_sendTimer <= 0.0f)
        {
            const bool open = m_state == State::Open;
            m_sendTimer = open ? (m_started ? kGameUpdateSeconds : kLobbyUpdateSeconds)
                               : (m_state == State::Failed ? kRetrySeconds : kAskSeconds);
            Ask("POST", open ? "/update" : "/host", HostBody());
        }
        if (m_state == State::Contacting && m_elapsed > m_settings.answerSeconds)
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
        if (idle && m_sendTimer <= 0.0f)
        {
            // Asked again even once introduced, less often: if the host has not heard about this guest
            // yet it is not sending, and without its half nothing gets through its router. Asking
            // again tells it again.
            m_sendTimer = m_state == State::Punching ? 2.0f : kAskSeconds;
            nlohmann::json body{{"version", m_settings.version},
                                {"code", DecodeLobbyCode(m_joinCode)},
                                {"localPort", m_localPort},
                                {"candidates", Candidates(m_localPort)}};
            Ask("POST", "/join", body.dump());
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
            m_punchElapsed += dt;
            if (m_punchElapsed > m_settings.punchSeconds)
            {
                Fail(m_strictRouter
                         ? "Found the game, but your router will not let a direct connection through. "
                           "Try having somebody else host, or both use Tailscale and join by address."
                         : "Found the game, but could not connect to the host directly: one of your "
                           "routers blocks it. Try having somebody else host, or both use Tailscale and "
                           "join by address.");
            }
        }
        break;
    }

    case Role::Browser:
        if (idle && m_sendTimer <= 0.0f)
        {
            m_sendTimer = kListSeconds;
            Ask("GET", "/list?version=" + std::to_string(m_settings.version), {});
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

void LobbyClient::HandleAnswer(const HttpResult& answer)
{
    const std::string what = m_pendingWhat;
    if (answer.status == 0)
    {
        // No answer at all. Counted, and the timers above decide when that becomes worth saying.
        ++m_failures;
        if (m_role == Role::Host && m_state == State::Open && m_failures >= 3)
        {
            Fail("Lost touch with the lobby server (" + answer.error +
                 "). Still trying. People already here can stay.");
        }
        return;
    }
    m_failures = 0;
    m_elapsed = 0.0f;

    nlohmann::json body = nlohmann::json::parse(answer.body, nullptr, false);
    if (body.is_discarded() || !body.is_object())
    {
        Fail("The lobby server's answer made no sense. Is the address in Settings right?");
        return;
    }
    const std::string error = body.value("error", std::string());

    if (what == "/host" || what == "/update")
    {
        if (m_role != Role::Host)
        {
            return;
        }
        if (what == "/update" && error == "no-such-lobby")
        {
            // The server has forgotten this lobby: it restarted, or it did not hear from us for too
            // long. Asked again, for the same code -- it hands it back if nobody else has it -- so
            // anybody already holding the code can still use it.
            PRED_LOG_WARN(Network, "Lobby: the server forgot {}; opening it again", CodeText());
            m_state = State::Contacting;
            m_sendTimer = 0.0f;
            return;
        }
        if (!error.empty())
        {
            Fail(Describe(RejectionFromText(error)));
            return;
        }
        if (what == "/host")
        {
            uint32_t code = 0;
            if (!EncodeLobbyCode(body.value("code", std::string()), code))
            {
                Fail("The lobby server's answer made no sense. Is the address in Settings right?");
                return;
            }
            if (m_code != code)
            {
                PRED_LOG_INFO(Network, "Lobby: open as {}", DecodeLobbyCode(code));
            }
            m_code = code;
            m_secret = body.value("secret", std::string());
            m_message.clear();
            m_state = State::Open;
            m_sendTimer = kLobbyUpdateSeconds;
            return;
        }
        m_state = State::Open;
        m_message.clear();
        // Guests on their way. Already sending to one? Then this is the server repeating itself.
        if (body.contains("guests") && body["guests"].is_array())
        {
            for (const nlohmann::json& guest : body["guests"])
            {
                const uint32_t token = TokenFrom(guest.value("token", nlohmann::json()));
                if (token == 0)
                {
                    continue;
                }
                const auto known = std::find_if(m_punches.begin(), m_punches.end(),
                                                [&](const Punch& punch) { return punch.token == token; });
                // Decided before anything is added: adding moves the end of the list, and asking
                // afterwards whether this was the end compares against the wrong one.
                const bool fresh = known == m_punches.end();
                Punch& punch = fresh ? m_punches.emplace_back() : *known;
                if (fresh)
                {
                    punch.token = token;
                    PRED_LOG_INFO(Network, "Lobby: somebody is joining; sending to {} address(es)",
                                  guest.contains("candidates") ? guest["candidates"].size() : 0);
                }
                punch.candidates = EndpointsFrom(guest.value("candidates", nlohmann::json::array()));
                punch.remaining = m_settings.punchSeconds;
                punch.reached = false;
            }
        }
        return;
    }

    if (what == "/join")
    {
        if (m_role != Role::Guest || m_state == State::Reached)
        {
            return;
        }
        if (!error.empty())
        {
            Fail(Describe(RejectionFromText(error)));
            return;
        }
        m_token = TokenFrom(body.value("token", nlohmann::json()));
        m_hostCandidates = EndpointsFrom(body.value("candidates", nlohmann::json::array()));
        m_lobbyName = body.value("name", std::string());
        if (m_state != State::Punching)
        {
            m_state = State::Punching;
            m_punchElapsed = 0.0f;
            std::string where;
            for (const LobbyEndpoint& candidate : m_hostCandidates)
            {
                where += (where.empty() ? "" : ", ") + candidate.ToString();
            }
            PRED_LOG_INFO(Network, "Lobby: found \"{}\"; trying {}", m_lobbyName, where);
        }
        return;
    }

    if (what.rfind("/list", 0) == 0 && m_role == Role::Browser)
    {
        m_lobbies.clear();
        if (body.contains("lobbies") && body["lobbies"].is_array())
        {
            for (const nlohmann::json& row : body["lobbies"])
            {
                LobbyListing listing;
                if (!EncodeLobbyCode(row.value("code", std::string()), listing.code))
                {
                    continue;
                }
                listing.name = row.value("name", std::string()).substr(0, kLobbyMaxNameLength);
                listing.players = static_cast<uint8_t>(std::clamp(row.value("players", 0), 0, 15));
                listing.maxPlayers = static_cast<uint8_t>(std::clamp(row.value("maxPlayers", 4), 1, 15));
                listing.started = row.value("started", false);
                m_lobbies.push_back(std::move(listing));
            }
        }
        m_heard = true;
        m_message.clear();
        m_state = State::Open;
    }
}

void LobbyClient::HandlePeer(Transport* transport, const LobbyPacket& packet, const LobbyEndpoint& from)
{
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
    if (m_role == Role::Guest && m_state == State::Punching && packet.token == m_token && m_token != 0)
    {
        // Whichever of the host's addresses answered first -- or the host's own probe arriving,
        // which proves the same thing. Either way, that is the one to connect to.
        m_reached = from;
        m_state = State::Reached;
        PRED_LOG_INFO(Network, "Lobby: reached the host at {} after {:.1f} s", from.ToString(), m_punchElapsed);
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
