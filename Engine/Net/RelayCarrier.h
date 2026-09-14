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
        Failed
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

    // Opens a lobby. The code arrives a moment later; Code() is zero until it does.
    bool Host(const Settings& settings);
    // Joins one.
    bool Join(const Settings& settings, uint32_t code);
    void Close();

    // Must be called regularly: this is where the socket is read and the keep-alive is sent.
    void Poll(float dt);

    State Status() const;
    uint32_t Code() const;
    std::string CodeText() const;
    uint8_t Slot() const;
    RelayRejection Rejection() const;
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
    // Slot per link, in the order the slots were first heard of. Never reordered: the transport
    // holds a peer per link and renumbering would move somebody else's peer under it.
    std::vector<uint8_t> m_slots;
    std::deque<Incoming> m_incoming;
    // Where the relay is, as a sockaddr_in. Opaque here for the same reason the socket is.
    std::vector<uint8_t> m_relayAddress;
};

} // namespace pred
