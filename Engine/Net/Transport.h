#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pred
{

using PeerId = uint32_t;
inline constexpr PeerId kInvalidPeer = 0;
inline constexpr PeerId kHostPeer = 1; // what a client calls the host it is connected to

enum class Channel : uint8_t
{
    Reliable,  // discrete events: fire, interact, mission changes. Ordered, never dropped.
    Unreliable // snapshots and inputs. The next one supersedes the last, so a loss is a hiccup
               // rather than a problem, and waiting for a resend would cost more than it saves.
};

struct NetPacket
{
    PeerId peer = kInvalidPeer; // who it came from
    Channel channel = Channel::Unreliable;
    std::vector<uint8_t> bytes;
};

// How bad the network is pretended to be. Present from the first line of networking code on
// purpose: prediction and reconciliation are only exercised by latency and loss, and a system that
// has only ever been tested on a perfect link has not been tested.
struct NetConditions
{
    float latencyMs = 0.0f;      // one way, before jitter
    float jitterMs = 0.0f;       // added, uniform, in [0, jitterMs]
    float lossPercent = 0.0f;    // unreliable packets thrown away; reliable ones pay a resend delay
    float duplicatePercent = 0.0f;
};

// Counters for the network overlay, and for tests that want to know a packet really was dropped.
struct NetStats
{
    uint64_t packetsSent = 0;
    uint64_t packetsReceived = 0;
    uint64_t packetsDropped = 0;
    uint64_t packetsDuplicated = 0;
    uint64_t bytesSent = 0;
    uint64_t bytesReceived = 0;
};

// What the rest of the game talks to. Five operations on the data path, deliberately: anything the
// transport can do beyond these is something the game would come to depend on and could not then
// swap out.
//
// The first implementation is in-process, so a host and its clients run in one executable and can
// be tested without a socket. ENet comes next, and Steam networking after that, when NAT traversal
// and encryption are needed. Nothing above this interface changes when they do.
class Transport
{
public:
    virtual ~Transport() = default;

    virtual bool Listen(uint16_t port) = 0;
    virtual bool Connect(const std::string& address, uint16_t port) = 0;
    virtual void Disconnect(PeerId peer) = 0;
    virtual void Send(PeerId peer, Channel channel, const uint8_t* data, size_t bytes) = 0;
    // Delivers everything that has arrived since the last call. Advancing time is the transport's
    // business, which is what lets a simulated one hold a packet back for a while.
    virtual void Poll(float dt, std::vector<NetPacket>& out) = 0;

    virtual void SetConditions(const NetConditions& conditions) = 0;
    virtual const NetConditions& Conditions() const = 0;
    virtual const NetStats& Stats() const = 0;

    // Peers that arrived or left since the last call, so the game is told rather than having to
    // notice. Both lists are cleared by reading them.
    virtual std::vector<PeerId> TakeConnected() = 0;
    virtual std::vector<PeerId> TakeDisconnected() = 0;

    virtual std::vector<PeerId> Peers() const = 0;
    virtual bool IsListening() const = 0;
};

// A transport with no network under it: a host and up to three clients in one process, wired
// together through the simulated conditions. Replication and prediction can be built and tested
// against this before a socket exists. `seed` fixes the jitter and loss draws so a test that fails
// fails again.
std::unique_ptr<Transport> CreateLoopbackTransport(uint32_t seed = 0x9E3779B9u);

// The real one, over UDP. Reliability is built on top rather than taken from TCP on purpose: TCP
// stalls every later message while it resends a lost one, which for a game means the whole world
// freezing because one snapshot went missing. Here a lost snapshot is simply skipped, and only the
// reliable channel waits.
//
// The simulated conditions apply to this too, so a bad link can be reproduced on a good one.
std::unique_ptr<Transport> CreateUdpTransport(uint32_t seed = 0x9E3779B9u);

} // namespace pred
