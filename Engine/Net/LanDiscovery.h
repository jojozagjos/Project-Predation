#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pred
{

// Finding a game on your own network, with nothing typed and nothing forwarded.
//
// A relay exists because two machines behind different routers cannot reach each other. Two
// machines in the same house can, and asking them to swap a code through a service on the far side
// of the internet to do it is absurd. So a host on a LAN simply shouts, several times a second, on
// a fixed port that every machine on that network can hear, and anybody browsing listens.
//
// Nothing here is authoritative and nothing here is trusted. A beacon is an advertisement: it says
// where to knock, and the knocking is the existing join handshake, which does its own checking. The
// one field a beacon does not get to claim is its own address -- that is taken from where the
// datagram actually came from, because a host that could name its own address could name somebody
// else's and point a roomful of players at them.
inline constexpr uint16_t kLanDiscoveryPort = 27016;
inline constexpr size_t kLanMaxNameLength = 24;
// How long a lobby stays in the list after its last beacon. Several beacons' worth, so one lost
// datagram does not make a game flicker out of the list and back in.
inline constexpr float kLanForgetSeconds = 4.0f;

// What a host says about itself.
struct LanLobby
{
    std::string name;
    // Where to connect. Filled in by the listener from the datagram's source address, never from
    // anything the sender wrote.
    std::string address;
    uint16_t port = 0;
    uint8_t players = 0;
    uint8_t maxPlayers = 0;
    bool started = false;
    // The game build. A different one is listed and greyed rather than hidden, because "their game
    // is not showing up" is a much worse thing to debug than "their game says it is a different
    // version".
    uint16_t protocol = 0;
    // Since the last beacon from this host, for expiry.
    float silentFor = 0.0f;
};

// The host half. Opens one socket and shouts into the network every so often.
class LanBeacon
{
public:
    LanBeacon();
    ~LanBeacon();
    LanBeacon(const LanBeacon&) = delete;
    LanBeacon& operator=(const LanBeacon&) = delete;

    // Starts shouting. False when the socket could not be opened, which on Windows nearly always
    // means the firewall; the game says so rather than looking like it worked.
    bool Start();
    void Stop();
    bool Running() const;

    // What to say. Cheap, so the game can call it every frame and let the beacon decide when to
    // actually send.
    //
    // The whole struct rather than a parameter each, because a parameter each is how the build
    // number got left out: the beacon had no reason to know what it was, so it sent zero, and
    // every game on the network would have appeared in everybody's list greyed out as a different
    // version. `address` and `silentFor` are ignored -- those belong to the listener.
    void Describe(const LanLobby& lobby);
    // Sends if enough time has passed.
    void Tick(float dt);

    const std::string& Message() const { return m_message; }

private:
    struct Impl;
    Impl* m_impl = nullptr;
    std::string m_message;
};

// The browsing half. Binds the discovery port and collects what it hears.
class LanListener
{
public:
    LanListener();
    ~LanListener();
    LanListener(const LanListener&) = delete;
    LanListener& operator=(const LanListener&) = delete;

    bool Start();
    void Stop();
    bool Running() const;

    // Drains the socket and ages the list.
    void Tick(float dt);
    const std::vector<LanLobby>& Lobbies() const { return m_lobbies; }
    const std::string& Message() const { return m_message; }

private:
    struct Impl;
    Impl* m_impl = nullptr;
    std::vector<LanLobby> m_lobbies;
    std::string m_message;
};

// --- The wire ----------------------------------------------------------------------------------
//
// Separate from the sockets so a test can check the encoding without one. The same discipline as
// the relay: the bookkeeping is testable and the socket is a thin shell around it.

std::vector<uint8_t> EncodeLanBeacon(const LanLobby& lobby);
// `out.address` is left alone: the caller fills it from where the datagram came from.
bool DecodeLanBeacon(const uint8_t* data, size_t bytes, LanLobby& out);
// Folds a heard beacon into a list, replacing an entry from the same address and port. Pulled out
// of the listener so the ageing and replacing rules are testable without a network.
void MergeLanLobby(std::vector<LanLobby>& lobbies, const LanLobby& heard);
void AgeLanLobbies(std::vector<LanLobby>& lobbies, float dt);

} // namespace pred
