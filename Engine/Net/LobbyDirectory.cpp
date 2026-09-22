#include "Engine/Net/LobbyDirectory.h"

#include <algorithm>

namespace pred
{
namespace
{

// How long an introduction is remembered: long enough for a guest's retries to all get the same
// token, short enough that the list stays a handful.
constexpr double kIntroductionSeconds = 30.0;
constexpr size_t kMaxIntroductions = 16;
// Senders are forgotten after this long without a word, so the rate table does not grow for ever.
constexpr double kSenderForgetSeconds = 120.0;
constexpr size_t kMaxSenders = 65536;

} // namespace

LobbyDirectory::LobbyDirectory() : LobbyDirectory(Settings{}) {}

LobbyDirectory::LobbyDirectory(const Settings& settings) : m_settings(settings), m_random(settings.seed) {}

void LobbyDirectory::Say(const std::string& line) const
{
    if (log)
    {
        log(line);
    }
}

bool LobbyDirectory::Allow(uint32_t address, double now)
{
    auto found = m_senders.find(address);
    if (found == m_senders.end())
    {
        if (m_senders.size() >= kMaxSenders)
        {
            return false;
        }
        found = m_senders.emplace(address, Sender{m_settings.burst, now}).first;
    }
    Sender& sender = found->second;
    // A bucket that fills at the allowed rate up to the burst, and each message takes one out.
    sender.tokens = std::min(sender.tokens + (now - sender.lastSeen) * m_settings.messagesPerSecond,
                             m_settings.burst);
    sender.lastSeen = now;
    if (sender.tokens < 1.0)
    {
        return false;
    }
    sender.tokens -= 1.0;
    return true;
}

uint32_t LobbyDirectory::NewCode()
{
    // Thirty bits, which is six characters of the code alphabet. Drawn until one is free, which with
    // a billion codes and a few thousand lobbies at most is the first draw nearly every time.
    for (int attempt = 0; attempt < 64; ++attempt)
    {
        const auto code = static_cast<uint32_t>(m_random() & 0x3FFFFFFFu);
        if (code != 0 && m_lobbies.find(code) == m_lobbies.end())
        {
            return code;
        }
    }
    return 0;
}

LobbyDirectory::Lobby* LobbyDirectory::FindByHost(const LobbyEndpoint& host)
{
    for (auto& entry : m_lobbies)
    {
        if (entry.second.host == host)
        {
            return &entry.second;
        }
    }
    return nullptr;
}

void LobbyDirectory::Send(std::vector<Reply>& out, const LobbyEndpoint& to, const LobbyPacket& packet)
{
    out.push_back({to, EncodeLobby(packet)});
}

void LobbyDirectory::Reject(std::vector<Reply>& out, const LobbyEndpoint& to, uint32_t code,
                            LobbyRejection reason)
{
    LobbyPacket packet;
    packet.kind = LobbyMessage::Rejected;
    packet.code = code;
    packet.reason = reason;
    Send(out, to, packet);
}

void LobbyDirectory::Handle(const uint8_t* data, size_t bytes, const LobbyEndpoint& from, double now,
                            std::vector<Reply>& out)
{
    ++m_stats.received;
    if (!from.Valid())
    {
        ++m_stats.ignored;
        return;
    }
    LobbyPacket packet;
    if (!DecodeLobby(data, bytes, packet))
    {
        // Scanners, strays and garbage. Nothing is said back to any of it: answering is how a
        // server gets used to bounce traffic at somebody else.
        ++m_stats.ignored;
        return;
    }
    if (!Allow(from.address, now))
    {
        ++m_stats.limited;
        return;
    }

    switch (packet.kind)
    {
    case LobbyMessage::Host:
        OnHost(packet, from, now, out);
        break;
    case LobbyMessage::Update:
        OnUpdate(packet, from, now, out);
        break;
    case LobbyMessage::Close:
        OnClose(packet, from);
        break;
    case LobbyMessage::Join:
        OnJoin(packet, from, now, out);
        break;
    case LobbyMessage::List:
        OnList(packet, from, out);
        break;
    default:
        // Messages the server sends, or that games send each other, arriving here. Ignored.
        ++m_stats.ignored;
        break;
    }
}

void LobbyDirectory::OnHost(const LobbyPacket& packet, const LobbyEndpoint& from, double now,
                            std::vector<Reply>& out)
{
    // Asked again -- the answer was lost, or the host has not heard it yet. The same lobby, the same
    // code: a second lobby for the same game would be a second code that half works.
    Lobby* lobby = FindByHost(from);
    if (lobby == nullptr)
    {
        size_t fromHere = 0;
        for (const auto& entry : m_lobbies)
        {
            fromHere += entry.second.host.address == from.address ? 1 : 0;
        }
        if (m_lobbies.size() >= m_settings.maxLobbies || fromHere >= m_settings.maxLobbiesPerAddress)
        {
            Reject(out, from, 0, LobbyRejection::ServerFull);
            return;
        }
        // The code it had before, if it asks for it and it is still free: a server that restarted, or
        // a host that dropped off for a moment, then comes back as the same code, and anybody holding
        // it can still use it.
        const bool askedFree = packet.code != 0 && packet.code <= 0x3FFFFFFFu &&
                               m_lobbies.find(packet.code) == m_lobbies.end();
        const uint32_t code = askedFree ? packet.code : NewCode();
        if (code == 0)
        {
            Reject(out, from, 0, LobbyRejection::ServerFull);
            return;
        }
        Lobby fresh;
        fresh.code = code;
        fresh.secret = static_cast<uint32_t>(m_random());
        fresh.host = from;
        lobby = &m_lobbies.emplace(code, std::move(fresh)).first->second;
        ++m_stats.hosted;
        Say("opened " + DecodeLobbyCode(code) + " for " + from.ToString() + " \"" + packet.name + "\"" +
            (packet.listed ? " (public)" : ""));
    }
    lobby->version = packet.version;
    lobby->players = packet.players;
    lobby->maxPlayers = packet.maxPlayers;
    lobby->started = packet.started;
    lobby->listed = packet.listed;
    lobby->name = packet.name;
    lobby->inside = packet.candidates;
    lobby->lastHeard = now;

    LobbyPacket reply;
    reply.kind = LobbyMessage::Hosted;
    reply.code = lobby->code;
    reply.secret = lobby->secret;
    reply.seenAs = from;
    Send(out, from, reply);
}

void LobbyDirectory::OnUpdate(const LobbyPacket& packet, const LobbyEndpoint& from, double now,
                              std::vector<Reply>& out)
{
    const auto found = m_lobbies.find(packet.code);
    if (found == m_lobbies.end())
    {
        // Forgotten: this server restarted, or the host went quiet for too long. Said, so the host
        // opens it again rather than going on updating a lobby nobody can find.
        Reject(out, from, packet.code, LobbyRejection::NoSuchLobby);
        return;
    }
    if (found->second.secret != packet.secret)
    {
        return;
    }
    Lobby& lobby = found->second;
    // The host's router may have given it a new outside port -- they do, after a while, on some
    // networks. The secret proves it is the same game, so the lobby follows it.
    if (lobby.host != from)
    {
        Say(DecodeLobbyCode(lobby.code) + " moved from " + lobby.host.ToString() + " to " + from.ToString());
        lobby.host = from;
    }
    lobby.players = packet.players;
    lobby.maxPlayers = packet.maxPlayers;
    lobby.started = packet.started;
    lobby.listed = packet.listed;
    lobby.name = packet.name;
    lobby.inside = packet.candidates;
    lobby.lastHeard = now;

    // Answered, so the host can tell a server that is there from one that is not.
    LobbyPacket reply;
    reply.kind = LobbyMessage::Hosted;
    reply.code = lobby.code;
    reply.secret = lobby.secret;
    reply.seenAs = from;
    Send(out, from, reply);
}

void LobbyDirectory::OnClose(const LobbyPacket& packet, const LobbyEndpoint& from)
{
    const auto found = m_lobbies.find(packet.code);
    if (found == m_lobbies.end() || found->second.secret != packet.secret)
    {
        return;
    }
    Say("closed " + DecodeLobbyCode(packet.code) + " (" + from.ToString() + ")");
    m_lobbies.erase(found);
}

void LobbyDirectory::OnJoin(const LobbyPacket& packet, const LobbyEndpoint& from, double now,
                            std::vector<Reply>& out)
{
    const auto found = m_lobbies.find(packet.code);
    if (found == m_lobbies.end())
    {
        Reject(out, from, packet.code, LobbyRejection::NoSuchLobby);
        return;
    }
    Lobby& lobby = found->second;
    if (lobby.version != packet.version)
    {
        Reject(out, from, packet.code, LobbyRejection::WrongVersion);
        return;
    }

    // The same guest asking again gets the same introduction. Anybody else needs a free place.
    lobby.introductions.erase(std::remove_if(lobby.introductions.begin(), lobby.introductions.end(),
                                             [&](const Introduction& known)
                                             { return now - known.at > kIntroductionSeconds; }),
                              lobby.introductions.end());
    Introduction* introduction = nullptr;
    for (Introduction& known : lobby.introductions)
    {
        if (known.guest == from)
        {
            introduction = &known;
        }
    }
    if (introduction == nullptr)
    {
        if (lobby.players >= lobby.maxPlayers)
        {
            Reject(out, from, packet.code, LobbyRejection::LobbyFull);
            return;
        }
        if (lobby.introductions.size() >= kMaxIntroductions)
        {
            lobby.introductions.erase(lobby.introductions.begin());
        }
        uint32_t token = 0;
        while (token == 0)
        {
            token = static_cast<uint32_t>(m_random());
        }
        lobby.introductions.push_back({from, token, now});
        introduction = &lobby.introductions.back();
        ++m_stats.introductions;
        Say("introduced " + from.ToString() + " to " + DecodeLobbyCode(lobby.code) + " at " +
            lobby.host.ToString());
    }

    // To the guest: where the host is. The outside address first, because it is the one that works
    // from anywhere; the inside ones after it, for a guest in the same house.
    LobbyPacket toGuest;
    toGuest.kind = LobbyMessage::Introduce;
    toGuest.code = lobby.code;
    toGuest.token = introduction->token;
    toGuest.toHost = false;
    toGuest.started = lobby.started;
    toGuest.name = lobby.name;
    toGuest.seenAs = from;
    toGuest.candidates.push_back(lobby.host);
    for (const LobbyEndpoint& inside : lobby.inside)
    {
        if (inside.Valid() && inside != lobby.host && toGuest.candidates.size() < kLobbyMaxCandidates)
        {
            toGuest.candidates.push_back(inside);
        }
    }
    Send(out, from, toGuest);

    // And to the host: somebody is coming, from here. It starts sending to them at once, which is
    // what opens its own router to them.
    LobbyPacket toHost;
    toHost.kind = LobbyMessage::Introduce;
    toHost.code = lobby.code;
    toHost.token = introduction->token;
    toHost.toHost = true;
    toHost.name = lobby.name;
    toHost.seenAs = lobby.host;
    toHost.candidates.push_back(from);
    for (const LobbyEndpoint& inside : packet.candidates)
    {
        if (inside.Valid() && inside != from && toHost.candidates.size() < kLobbyMaxCandidates)
        {
            toHost.candidates.push_back(inside);
        }
    }
    Send(out, lobby.host, toHost);
}

void LobbyDirectory::OnList(const LobbyPacket& packet, const LobbyEndpoint& from, std::vector<Reply>& out)
{
    LobbyPacket reply;
    reply.kind = LobbyMessage::Listing;
    // Only public lobbies on the same version: a game that cannot be joined from this build is not
    // worth a row. Ones with room first, then by code so the order is steady from one ask to the next.
    std::vector<const Lobby*> open;
    for (const auto& entry : m_lobbies)
    {
        if (entry.second.listed && entry.second.version == packet.version)
        {
            open.push_back(&entry.second);
        }
    }
    std::sort(open.begin(), open.end(),
              [](const Lobby* a, const Lobby* b)
              {
                  const bool aRoom = a->players < a->maxPlayers;
                  const bool bRoom = b->players < b->maxPlayers;
                  return aRoom != bRoom ? aRoom : a->code < b->code;
              });
    for (const Lobby* lobby : open)
    {
        if (reply.lobbies.size() >= kLobbyMaxListed)
        {
            break;
        }
        reply.lobbies.push_back({lobby->code, lobby->players, lobby->maxPlayers, lobby->started, lobby->name});
    }
    Send(out, from, reply);
}

void LobbyDirectory::Expire(double now)
{
    for (auto it = m_lobbies.begin(); it != m_lobbies.end();)
    {
        if (now - it->second.lastHeard > m_settings.lobbyTimeoutSeconds)
        {
            Say("forgot " + DecodeLobbyCode(it->first) + ": its host went quiet");
            it = m_lobbies.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (auto it = m_senders.begin(); it != m_senders.end();)
    {
        it = now - it->second.lastSeen > kSenderForgetSeconds ? m_senders.erase(it) : std::next(it);
    }
}

} // namespace pred
