#include "Engine/Net/Transport.h"

#include "Engine/Core/Log.h"

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <unordered_map>
#include <utility>

namespace pred
{
namespace
{

class LoopbackTransport;

// Every listening loopback transport puts itself here under its port. That is how a client finds a
// host with no socket involved, so the address a client passes is ignored.
std::mutex& RegistryMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::map<uint16_t, LoopbackTransport*>& Registry()
{
    static std::map<uint16_t, LoopbackTransport*> registry;
    return registry;
}

// A packet in flight, with the time left before it lands.
struct Pending
{
    Channel channel = Channel::Unreliable;
    std::vector<uint8_t> bytes;
    float delay = 0.0f;
};

// Incoming packets from one peer. Unreliable ones each carry their own timer and may therefore
// arrive out of order, which is exactly the case prediction has to survive. Reliable ones leave in
// order and land in order, so only the one at the front can ever be delivered.
struct Mailbox
{
    std::vector<Pending> unreliable;
    std::deque<Pending> reliable;
};

struct Link
{
    LoopbackTransport* other = nullptr;
    PeerId idOnTheirSide = kInvalidPeer; // what the far end calls me
};

class LoopbackTransport final : public Transport
{
public:
    explicit LoopbackTransport(uint32_t seed) : m_rng(seed) {}
    ~LoopbackTransport() override;

    bool Listen(uint16_t port) override;
    bool Connect(const std::string& address, uint16_t port) override;
    void Disconnect(PeerId peer) override;
    void Send(PeerId peer, Channel channel, const uint8_t* data, size_t bytes) override;
    void Poll(float dt, std::vector<NetPacket>& out) override;

    void SetConditions(const NetConditions& conditions) override { m_conditions = conditions; }
    const NetConditions& Conditions() const override { return m_conditions; }
    const NetStats& Stats() const override { return m_stats; }

    std::vector<PeerId> TakeConnected() override { return std::exchange(m_connected, {}); }
    std::vector<PeerId> TakeDisconnected() override { return std::exchange(m_disconnected, {}); }

    std::vector<PeerId> Peers() const override;
    std::string AddressOf(PeerId) const override { return {}; }
    bool IsListening() const override { return m_listening; }

    // Called by the transport at the other end of a link.
    void Deliver(PeerId from, Channel channel, const std::vector<uint8_t>& bytes, float delay);
    void AddPeer(PeerId id, LoopbackTransport* other, PeerId idOnTheirSide);
    void DropPeer(PeerId id);
    PeerId NextPeerId() { return m_nextPeerId++; }

private:
    float DrawDelay();
    bool DrawChance(float percent);
    void TearDown();

    std::unordered_map<PeerId, Link> m_peers;
    std::unordered_map<PeerId, Mailbox> m_inbox;
    std::vector<PeerId> m_connected;
    std::vector<PeerId> m_disconnected;
    NetConditions m_conditions;
    NetStats m_stats;
    std::mt19937 m_rng;
    uint16_t m_port = 0;
    PeerId m_nextPeerId = kHostPeer + 1;
    bool m_listening = false;
};

LoopbackTransport::~LoopbackTransport()
{
    TearDown();
}

void LoopbackTransport::TearDown()
{
    // Tell the far end first: it holds a raw pointer to this object and has to forget it before the
    // object goes away.
    for (auto& entry : m_peers)
    {
        if (entry.second.other != nullptr)
        {
            entry.second.other->DropPeer(entry.second.idOnTheirSide);
        }
    }
    m_peers.clear();
    m_inbox.clear();

    if (m_listening)
    {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        const auto found = Registry().find(m_port);
        if (found != Registry().end() && found->second == this)
        {
            Registry().erase(found);
        }
        m_listening = false;
    }
}

bool LoopbackTransport::Listen(uint16_t port)
{
    std::lock_guard<std::mutex> lock(RegistryMutex());
    if (Registry().count(port) != 0)
    {
        PRED_LOG_WARN(Network, "Loopback port {} is already in use", port);
        return false;
    }
    Registry()[port] = this;
    m_port = port;
    m_listening = true;
    return true;
}

bool LoopbackTransport::Connect(const std::string& address, uint16_t port)
{
    LoopbackTransport* host = nullptr;
    {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        const auto found = Registry().find(port);
        if (found == Registry().end())
        {
            PRED_LOG_WARN(Network, "No loopback host is listening on port {}", port);
            return false;
        }
        host = found->second;
    }
    if (host == this)
    {
        return false;
    }

    // The host names the connection. A client always calls the host peer 1.
    const PeerId assigned = host->NextPeerId();
    host->AddPeer(assigned, this, kHostPeer);
    AddPeer(kHostPeer, host, assigned);
    PRED_LOG_INFO(Network, "Loopback client {} joined the host on port {} at {}", assigned, port, address);
    return true;
}

void LoopbackTransport::AddPeer(PeerId id, LoopbackTransport* other, PeerId idOnTheirSide)
{
    Link link;
    link.other = other;
    link.idOnTheirSide = idOnTheirSide;
    m_peers[id] = link;
    m_inbox[id] = Mailbox{};
    m_connected.push_back(id);
}

void LoopbackTransport::DropPeer(PeerId id)
{
    if (m_peers.erase(id) != 0)
    {
        m_inbox.erase(id);
        m_disconnected.push_back(id);
    }
}

void LoopbackTransport::Disconnect(PeerId peer)
{
    const auto found = m_peers.find(peer);
    if (found == m_peers.end())
    {
        return;
    }
    if (found->second.other != nullptr)
    {
        found->second.other->DropPeer(found->second.idOnTheirSide);
    }
    m_peers.erase(found);
    m_inbox.erase(peer);
    m_disconnected.push_back(peer);
}

std::vector<PeerId> LoopbackTransport::Peers() const
{
    std::vector<PeerId> ids;
    ids.reserve(m_peers.size());
    for (const auto& entry : m_peers)
    {
        ids.push_back(entry.first);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

float LoopbackTransport::DrawDelay()
{
    const float jitter =
        m_conditions.jitterMs <= 0.0f
            ? 0.0f
            : std::uniform_real_distribution<float>(0.0f, m_conditions.jitterMs)(m_rng);
    return std::max(m_conditions.latencyMs + jitter, 0.0f) * 0.001f;
}

bool LoopbackTransport::DrawChance(float percent)
{
    if (percent <= 0.0f)
    {
        return false;
    }
    return std::uniform_real_distribution<float>(0.0f, 100.0f)(m_rng) < percent;
}

void LoopbackTransport::Send(PeerId peer, Channel channel, const uint8_t* data, size_t bytes)
{
    const auto found = m_peers.find(peer);
    if (found == m_peers.end() || found->second.other == nullptr || data == nullptr || bytes == 0)
    {
        return;
    }

    ++m_stats.packetsSent;
    m_stats.bytesSent += bytes;

    const std::vector<uint8_t> payload(data, data + bytes);
    LoopbackTransport& target = *found->second.other;
    const PeerId from = found->second.idOnTheirSide;

    if (channel == Channel::Reliable)
    {
        // A reliable packet is never lost, but losing one costs a round trip to notice and resend,
        // and everything queued behind it waits. That stall is the reason it matters which channel
        // a message goes on, so the simulation has to show it.
        float delay = DrawDelay();
        if (DrawChance(m_conditions.lossPercent))
        {
            delay += m_conditions.latencyMs * 2.0f * 0.001f + 0.02f;
        }
        target.Deliver(from, channel, payload, delay);
        return;
    }

    if (DrawChance(m_conditions.lossPercent))
    {
        ++m_stats.packetsDropped;
        return;
    }

    target.Deliver(from, channel, payload, DrawDelay());
    if (DrawChance(m_conditions.duplicatePercent))
    {
        ++m_stats.packetsDuplicated;
        target.Deliver(from, channel, payload, DrawDelay());
    }
}

void LoopbackTransport::Deliver(PeerId from, Channel channel, const std::vector<uint8_t>& bytes,
                                float delay)
{
    Mailbox& box = m_inbox[from];
    Pending pending;
    pending.channel = channel;
    pending.bytes = bytes;
    pending.delay = delay;
    if (channel == Channel::Reliable)
    {
        box.reliable.push_back(std::move(pending));
    }
    else
    {
        box.unreliable.push_back(std::move(pending));
    }
}

void LoopbackTransport::Poll(float dt, std::vector<NetPacket>& out)
{
    out.clear();
    for (auto& entry : m_inbox)
    {
        const PeerId peer = entry.first;
        Mailbox& box = entry.second;

        for (size_t i = 0; i < box.unreliable.size();)
        {
            box.unreliable[i].delay -= dt;
            if (box.unreliable[i].delay <= 0.0f)
            {
                NetPacket packet;
                packet.peer = peer;
                packet.channel = Channel::Unreliable;
                packet.bytes = std::move(box.unreliable[i].bytes);
                ++m_stats.packetsReceived;
                m_stats.bytesReceived += packet.bytes.size();
                out.push_back(std::move(packet));
                box.unreliable.erase(box.unreliable.begin() + static_cast<ptrdiff_t>(i));
            }
            else
            {
                ++i;
            }
        }

        // Only the front of the reliable queue can land, whatever the jitter drew for the ones
        // behind it. Everything still counts down, so a stalled queue catches up in one go.
        for (Pending& pending : box.reliable)
        {
            pending.delay -= dt;
        }
        while (!box.reliable.empty() && box.reliable.front().delay <= 0.0f)
        {
            NetPacket packet;
            packet.peer = peer;
            packet.channel = Channel::Reliable;
            packet.bytes = std::move(box.reliable.front().bytes);
            ++m_stats.packetsReceived;
            m_stats.bytesReceived += packet.bytes.size();
            out.push_back(std::move(packet));
            box.reliable.pop_front();
        }
    }
}

} // namespace

std::unique_ptr<Transport> CreateLoopbackTransport(uint32_t seed)
{
    return std::make_unique<LoopbackTransport>(seed);
}

} // namespace pred
