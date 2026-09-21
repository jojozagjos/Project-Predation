#pragma once

#include "Engine/Net/RelayProtocol.h"
#include "Engine/Net/Transport.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace pred
{

// The game's datagrams, through a relay.
//
// Drops into the same place the punched connection did: a DatagramCarrier, with one link per other
// person in the lobby. Everything above it — the handshake, the reliability, the prediction, the
// roster — is unchanged and cannot tell the difference, which is the whole reason the carrier
// exists as an abstraction.
//
// The difference from a punched connection is that there is nothing to negotiate. One socket, one
// address, connected outwards, and outwards always works. No candidates, no roles, no waiting to
// find out whether this particular pair of routers will cooperate.
class RelayCarrier final : public DatagramCarrier
{
public:
    enum class State : uint8_t
    {
        Idle,
        Connecting, // the relay has been asked to open or join and has not answered
        Ready,      // in a lobby
        Failed,
        // Asking the relay what is open, without being in anything. The carrier half is unused in
        // this state: nothing is being relayed, so there are no links and nothing to send.
        Browsing
    };

    struct Settings
    {
        std::string relayHost = "127.0.0.1";
        uint16_t relayPort = 27020;
        // How often to tell the relay we are still here. Also what holds the router's hole open for
        // the return path, so it has to be well under any plausible NAT timeout.
        float keepAliveSeconds = 2.0f;
        // How long to wait for the relay to answer before giving up.
        float connectTimeoutSeconds = 8.0f;
    };

    RelayCarrier() = default;
    ~RelayCarrier() override;
    RelayCarrier(const RelayCarrier&) = delete;
    RelayCarrier& operator=(const RelayCarrier&) = delete;

    // Opens a lobby. The code arrives a moment later; Code() is zero until it does. The name is
    // what the lobby is called in everybody else's browser: cosmetic, clamped and stripped by the
    // relay, and not how anybody joins -- that is still the code.
    bool Host(const Settings& settings, const std::string& name = {});
    // Joins one.
    bool Join(const Settings& settings, uint32_t code);
    // Asks what is open, without joining anything. Its own entry point rather than a flag on Join,
    // because browsing is what somebody does before they have a code and the two share nothing past
    // the socket.
    bool Browse(const Settings& settings);
    void Close();

    // Must be called regularly: this is where the socket is read and the keep-alive is sent.
    void Poll(float dt);

    State Status() const;
    uint32_t Code() const;
    std::string CodeText() const;
    uint8_t Slot() const;
    RelayRejection Rejection() const;
    // What the relay last said is open. Returned by value: the socket is drained under a lock from
    // whoever is polling, and handing out a reference into it is how a list being drawn gets
    // reallocated underneath the drawing.
    std::vector<RelayLobbyInfo> Lobbies() const;
    // Whether the relay has answered a listing at least once since browsing began. The game uses a
    // relay only when one is really there: hosting "over the internet" with none answering falls
    // back to asking the router to let people in directly, rather than opening a lobby on a
    // machine that does not exist.
    bool Heard() const;
    const std::string& Message() const { return m_message; }

    // --- DatagramCarrier -------------------------------------------------------------------------
    //
    // A link is a slot in the lobby, and the mapping is kept here rather than exposed: the transport
    // wants links numbered from zero with no holes, and slots have holes in them as people leave.
    size_t Links() const override;
    bool Send(size_t link, const uint8_t* data, size_t bytes) override;
    bool Receive(size_t& link, std::vector<uint8_t>& out) override;
    bool Live(size_t link) const override;

private:
    struct Incoming
    {
        size_t link = 0;
        std::vector<uint8_t> bytes;
    };

    void SendToRelay(const RelayPacket& packet);
    // The link index for a slot, adding it if this is the first we have heard of it.
    size_t LinkForSlot(uint8_t slot);

    mutable std::mutex m_mutex;
    // Opaque so this header does not drag in winsock. Owned; closed by Close.
    int64_t m_socket = -1;
    Settings m_settings;
    State m_state = State::Idle;
    uint32_t m_code = 0;
    uint8_t m_slot = kRelayNoSlot;
    RelayRejection m_rejection = RelayRejection::None;
    std::string m_message;
    float m_keepAliveTimer = 0.0f;
    float m_connectTimer = 0.0f;
    bool m_hosting = false;
    // Browsing rather than playing. Kept apart from the state so a browse that has heard nothing
    // yet stays in the browsing state and keeps asking, instead of becoming a dead socket the way
    // a failed join does.
    bool m_browsing = false;
    std::string m_lobbyName;
    std::vector<RelayLobbyInfo> m_lobbies;
    // When to ask again, and how long since the relay last answered. A browser that has heard
    // nothing says so, because an empty list looks exactly like "nobody is playing" and those are
    // very different things to be told.
    float m_listTimer = 0.0f;
    float m_listSilence = 0.0f;
    bool m_heard = false;
    // Whether this carrier holds a reference to the socket system, so Close releases exactly the
    // ones Join took.
    bool m_socketSystem = false;
    // Slot per link, in the order the slots were first heard of. Never reordered: the transport
    // holds a peer per link and renumbering would move somebody else's peer under it.
    std::vector<uint8_t> m_slots;
    std::deque<Incoming> m_incoming;
    // Where the relay is, as a sockaddr_in. Opaque here for the same reason the socket is.
    std::vector<uint8_t> m_relayAddress;
};

} // namespace pred
