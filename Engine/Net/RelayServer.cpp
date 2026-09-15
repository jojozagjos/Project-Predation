#include "Engine/Net/RelayServer.h"

#include "Engine/Core/Log.h"

#include <algorithm>

namespace pred
{

RelayServer::RelayServer(const Settings& settings) : m_settings(settings), m_random(settings.seed)
{
    if (m_random == 0)
    {
        m_random = 1;
    }
}

size_t RelayServer::ClientCount() const
{
    size_t total = 0;
    for (const Lobby& lobby : m_lobbies)
    {
        total += lobby.members.size();
    }
    return total;
}

RelayServer::Lobby* RelayServer::FindLobby(uint32_t code)
{
    const auto found = std::find_if(m_lobbies.begin(), m_lobbies.end(),
                                    [&](const Lobby& lobby) { return lobby.code == code; });
    return found == m_lobbies.end() ? nullptr : &*found;
}

RelayServer::Lobby* RelayServer::LobbyOf(const std::string& client, Member** outMember)
{
    for (Lobby& lobby : m_lobbies)
    {
        for (Member& member : lobby.members)
        {
            if (member.key == client)
            {
                if (outMember != nullptr)
                {
                    *outMember = &member;
                }
                return &lobby;
            }
        }
    }
    return nullptr;
}

uint32_t RelayServer::CodeOf(const std::string& client) const
{
    for (const Lobby& lobby : m_lobbies)
    {
        for (const Member& member : lobby.members)
        {
            if (member.key == client)
            {
                return lobby.code;
            }
        }
    }
    return 0;
}

uint32_t RelayServer::NextCode()
{
    // xorshift, and then checked against what is already open. Not std::mt19937 because a relay
    // wants a code nobody can predict from the last one rather than a good distribution, and
    // because the same seed giving the same first code makes a test readable.
    for (int attempt = 0; attempt < 64; ++attempt)
    {
        m_random ^= m_random << 13;
        m_random ^= m_random >> 17;
        m_random ^= m_random << 5;
        const uint32_t code = m_random & 0x3FFFFFFFu; // thirty bits, which is six symbols of five
        if (FindLobby(code) == nullptr)
        {
            return code;
        }
    }
    return 0;
}

void RelayServer::Send(std::vector<Outgoing>& out, const std::string& to,
                       const RelayPacket& packet) const
{
    out.push_back({to, EncodeRelay(packet)});
}

void RelayServer::Reject(std::vector<Outgoing>& out, const std::string& to,
                         RelayRejection reason) const
{
    RelayPacket packet;
    packet.kind = RelayMessage::Rejected;
    packet.reason = reason;
    Send(out, to, packet);
}

void RelayServer::AddNote(Note::Kind kind, const std::string& client, uint32_t code, uint8_t slot,
                          RelayRejection reason, bool timedOut)
{
    // Dropped rather than queued without limit when nobody is draining them. A relay left running
    // for a week with no operator watching must not grow a note per datagram.
    if (m_notes.size() >= kMaxNotes)
    {
        m_notes.erase(m_notes.begin());
    }
    Note note;
    note.kind = kind;
    note.client = client;
    note.code = code;
    note.slot = slot;
    note.reason = reason;
    note.timedOut = timedOut;
    m_notes.push_back(std::move(note));
}

void RelayServer::RemoveMember(Lobby& lobby, uint8_t slot, std::vector<Outgoing>& out, bool timedOut)
{
    const auto found = std::find_if(lobby.members.begin(), lobby.members.end(),
                                    [&](const Member& member) { return member.slot == slot; });
    if (found == lobby.members.end())
    {
        return;
    }
    AddNote(Note::Kind::Left, found->key, lobby.code, slot, RelayRejection::None, timedOut);
    lobby.members.erase(found);

    RelayPacket left;
    left.kind = RelayMessage::PeerLeft;
    left.slot = slot;
    for (const Member& member : lobby.members)
    {
        Send(out, member.key, left);
    }
}

void RelayServer::Receive(const std::string& from, const uint8_t* data, size_t bytes,
                          std::vector<Outgoing>& out)
{
    RelayPacket packet;
    if (!DecodeRelay(data, bytes, packet))
    {
        // Not ours. An open UDP port on the internet receives scans, stray game traffic and noise,
        // and none of it gets a reply: answering tells a scanner something is here.
        //
        // Noted, though, because "something is arriving from your friend's address and none of it is
        // ours" and "nothing is arriving from your friend at all" are different faults with
        // different fixes, and from the outside they look identical.
        AddNote(Note::Kind::Ignored, from, 0, kRelayNoSlot);
        return;
    }

    Member* member = nullptr;
    Lobby* lobby = LobbyOf(from, &member);

    // Everything below this point costs allowance, including messages from somebody not in a lobby.
    if (member != nullptr)
    {
        if (member->allowance < 1.0f)
        {
            return;
        }
        member->allowance -= 1.0f;
        member->silentFor = 0.0f;
    }

    switch (packet.kind)
    {
    case RelayMessage::Host:
    {
        if (lobby != nullptr)
        {
            // Already in one. Hosting again would strand whoever had joined the first, so the
            // existing code is repeated instead: this is also what a lost Hosted reply looks like.
            RelayPacket hosted;
            hosted.kind = RelayMessage::Hosted;
            hosted.code = lobby->code;
            hosted.slot = member->slot;
            Send(out, from, hosted);
            return;
        }
        if (m_lobbies.size() >= m_settings.maxLobbies || ClientCount() >= m_settings.maxClients)
        {
            Reject(out, from, RelayRejection::RelayFull);
            return;
        }
        const uint32_t code = NextCode();
        if (code == 0)
        {
            Reject(out, from, RelayRejection::RelayFull);
            return;
        }
        Lobby fresh;
        fresh.code = code;
        Member host;
        host.key = from;
        host.slot = kRelayHostSlot;
        host.allowance = m_settings.messagesPerSecond;
        fresh.members.push_back(std::move(host));
        m_lobbies.push_back(std::move(fresh));

        RelayPacket hosted;
        hosted.kind = RelayMessage::Hosted;
        hosted.code = code;
        hosted.slot = kRelayHostSlot;
        Send(out, from, hosted);
        AddNote(Note::Kind::Opened, from, code, kRelayHostSlot);
        return;
    }

    case RelayMessage::Join:
    {
        if (lobby != nullptr)
        {
            // Already somewhere. Repeat what they were told rather than putting them in two places.
            RelayPacket joined;
            joined.kind = RelayMessage::Joined;
            joined.slot = member->slot;
            Send(out, from, joined);
            return;
        }
        if (ClientCount() >= m_settings.maxClients)
        {
            Reject(out, from, RelayRejection::RelayFull);
            return;
        }
        Lobby* target = FindLobby(packet.code);
        if (target == nullptr)
        {
            Reject(out, from, RelayRejection::NoSuchLobby);
            AddNote(Note::Kind::Refused, from, packet.code, kRelayNoSlot,
                    RelayRejection::NoSuchLobby);
            return;
        }
        // The lowest free slot, so a lobby people come and go from does not run out of numbers.
        uint8_t slot = kRelayNoSlot;
        for (uint8_t candidate = 1; candidate < kRelayMaxSlots; ++candidate)
        {
            const bool taken = std::any_of(target->members.begin(), target->members.end(),
                                           [&](const Member& other) { return other.slot == candidate; });
            if (!taken)
            {
                slot = candidate;
                break;
            }
        }
        if (slot == kRelayNoSlot)
        {
            Reject(out, from, RelayRejection::LobbyFull);
            AddNote(Note::Kind::Refused, from, packet.code, kRelayNoSlot,
                    RelayRejection::LobbyFull);
            return;
        }

        Member arrival;
        arrival.key = from;
        arrival.slot = slot;
        arrival.allowance = m_settings.messagesPerSecond;

        // Told to the people already there before the arrival is told anything, so that by the time
        // the newcomer starts sending, everybody already knows which slot that is.
        RelayPacket announce;
        announce.kind = RelayMessage::PeerJoined;
        announce.slot = slot;
        for (const Member& other : target->members)
        {
            Send(out, other.key, announce);
        }
        // And the newcomer is told about each of them, so a host that joined first is not invisible
        // to somebody who arrived later.
        RelayPacket joined;
        joined.kind = RelayMessage::Joined;
        joined.slot = slot;
        Send(out, from, joined);
        RelayPacket existing;
        existing.kind = RelayMessage::PeerJoined;
        for (const Member& other : target->members)
        {
            existing.slot = other.slot;
            Send(out, from, existing);
        }
        target->members.push_back(std::move(arrival));
        AddNote(Note::Kind::Joined, from, target->code, slot);
        return;
    }

    case RelayMessage::Data:
    {
        if (lobby == nullptr)
        {
            return; // nothing to forward into
        }
        if (packet.payload.empty() || packet.payload.size() > kRelayMaxPayload)
        {
            return;
        }
        // Only within the sender's own lobby, and never back to the sender. A client can name a
        // slot; it cannot name a lobby, so there is no way to speak into one it is not in.
        const auto target = std::find_if(lobby->members.begin(), lobby->members.end(),
                                         [&](const Member& other) { return other.slot == packet.slot; });
        if (target == lobby->members.end() || target->key == from)
        {
            return;
        }
        RelayPacket relayed;
        relayed.kind = RelayMessage::Relayed;
        relayed.slot = member->slot; // who it is from, which the relay knows and the sender cannot forge
        relayed.payload = std::move(packet.payload);
        Send(out, target->key, relayed);
        return;
    }

    case RelayMessage::Leave:
    {
        if (lobby == nullptr)
        {
            return;
        }
        const uint8_t slot = member->slot;
        RemoveMember(*lobby, slot, out, false);
        // An empty lobby is closed, and so is one whose host has gone: the others cannot play
        // without it and leaving the code alive would let somebody join a game that is not there.
        if (lobby->members.empty() || slot == kRelayHostSlot)
        {
            for (const Member& other : lobby->members)
            {
                Reject(out, other.key, RelayRejection::NoSuchLobby);
            }
            AddNote(Note::Kind::Closed, from, lobby->code, slot);
            m_lobbies.erase(m_lobbies.begin() +
                            static_cast<ptrdiff_t>(lobby - m_lobbies.data()));
        }
        return;
    }

    case RelayMessage::KeepAlive:
        // The silence timer was already reset above, which is the whole of what this is for.
        //
        // Unless we have never heard of them, in which case they are told so. A client that has been
        // dropped -- timed out, or in a lobby the host left -- goes on sending keep-alives forever,
        // because nothing in the protocol told it otherwise: it believes it is in a lobby and the
        // relay believes nobody is there, and neither ever finds out. The player sees a lobby with a
        // code in it that nobody can join, which is indistinguishable from a working lobby nobody
        // has tried to join.
        //
        // This is not a new thing to tell a stranger. Host and Join already answer an address the
        // relay has never heard of, so a scanner learns nothing here it could not learn there.
        if (lobby == nullptr)
        {
            Reject(out, from, RelayRejection::NoSuchLobby);
        }
        return;

    default:
        // A relay-to-client message arriving at the relay. Somebody is confused or probing.
        return;
    }
}

void RelayServer::Tick(float dt, std::vector<Outgoing>& out)
{
    for (size_t index = 0; index < m_lobbies.size();)
    {
        Lobby& lobby = m_lobbies[index];
        bool hostGone = false;

        for (size_t member = 0; member < lobby.members.size();)
        {
            Member& entry = lobby.members[member];
            entry.silentFor += dt;
            entry.allowance =
                std::min(entry.allowance + m_settings.messagesPerSecond * dt, m_settings.messagesPerSecond);
            if (entry.silentFor > m_settings.timeoutSeconds)
            {
                const uint8_t slot = entry.slot;
                hostGone = hostGone || slot == kRelayHostSlot;
                RemoveMember(lobby, slot, out, true);
                continue; // the vector shifted under us
            }
            ++member;
        }

        if (lobby.members.empty() || hostGone)
        {
            for (const Member& other : lobby.members)
            {
                Reject(out, other.key, RelayRejection::NoSuchLobby);
            }
            AddNote(Note::Kind::Closed, std::string(), lobby.code, kRelayHostSlot,
                    RelayRejection::None, true);
            m_lobbies.erase(m_lobbies.begin() + static_cast<ptrdiff_t>(index));
            continue;
        }
        ++index;
    }
}

} // namespace pred
