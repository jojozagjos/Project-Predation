#include "Engine/Net/Transport.h"

#include "Engine/Core/Log.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <deque>
#include <random>
#include <utility>
#include <vector>

#if defined(_WIN32)
#    include <winsock2.h>
#    include <ws2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#    include <arpa/inet.h>
#    include <fcntl.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace pred
{
namespace
{

// Reliability is built here rather than taken from TCP because a game wants exactly one of the two
// things TCP bundles together. Ordered, guaranteed delivery is right for a fire event. It is wrong
// for a snapshot: TCP would hold every later snapshot behind a lost one, and by the time the
// resend arrived the world would have moved on twice. So both channels share one UDP socket, and
// only the reliable one waits.

constexpr uint32_t kMagic = 0x50524544u; // 'PRED', so a stray datagram on the port is ignored
constexpr size_t kHeaderBytes = 12;
constexpr size_t kMaxDatagram = 1200; // comfortably inside the smallest path MTU anyone still has
constexpr float kResendSeconds = 0.10f;
constexpr float kKeepAliveSeconds = 1.0f;
constexpr float kTimeoutSeconds = 10.0f;
constexpr float kConnectRetrySeconds = 0.25f;
constexpr float kConnectTimeoutSeconds = 6.0f;

enum class PacketKind : uint8_t
{
    ConnectRequest = 1,
    ConnectAccept,
    Disconnect,
    KeepAlive,
    Payload
};

// Sequence numbers are sixteen bits and wrap. Comparing them with < would decide that 1 is older
// than 65535 forever after the first wrap, so the comparison has to be made on the short way round.
bool SequenceGreater(uint16_t a, uint16_t b)
{
    constexpr uint16_t kHalf = 32768;
    return ((a > b) && (a - b <= kHalf)) || ((a < b) && (b - a > kHalf));
}

void WriteU16(uint8_t* at, uint16_t value)
{
    at[0] = static_cast<uint8_t>(value & 0xFFu);
    at[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

uint16_t ReadU16(const uint8_t* at)
{
    return static_cast<uint16_t>(at[0] | (static_cast<uint16_t>(at[1]) << 8));
}

void WriteU32(uint8_t* at, uint32_t value)
{
    at[0] = static_cast<uint8_t>(value & 0xFFu);
    at[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    at[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    at[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

uint32_t ReadU32(const uint8_t* at)
{
    return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) |
           (static_cast<uint32_t>(at[2]) << 16) | (static_cast<uint32_t>(at[3]) << 24);
}

bool SameAddress(const sockaddr_in& a, const sockaddr_in& b)
{
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

std::string DescribeAddress(const sockaddr_in& address)
{
    char text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text));
    return std::string(text) + ":" + std::to_string(ntohs(address.sin_port));
}

// Winsock has to be started once per process and stopped when the last user is done with it.
class SocketSystem
{
public:
    static bool Acquire()
    {
#if defined(_WIN32)
        if (s_users++ == 0)
        {
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            {
                --s_users;
                return false;
            }
        }
#else
        ++s_users;
#endif
        return true;
    }

    static void Release()
    {
        if (s_users > 0 && --s_users == 0)
        {
#if defined(_WIN32)
            WSACleanup();
#endif
        }
    }

private:
    static int s_users;
};

int SocketSystem::s_users = 0;

void CloseSocket(SocketHandle handle)
{
    if (handle == kInvalidSocket)
    {
        return;
    }
#if defined(_WIN32)
    closesocket(handle);
#else
    close(handle);
#endif
}

bool SetNonBlocking(SocketHandle handle)
{
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket(handle, static_cast<long>(FIONBIO), &mode) == 0;
#else
    const int flags = fcntl(handle, F_GETFL, 0);
    return flags >= 0 && fcntl(handle, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool WouldBlock()
{
#if defined(_WIN32)
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAECONNRESET;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

class UdpTransport final : public Transport
{
public:
    explicit UdpTransport(uint32_t seed) : m_rng(seed) {}
    ~UdpTransport() override;

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
    std::string AddressOf(PeerId peer) const override;
    bool IsListening() const override { return m_listening; }

private:
    struct Outgoing // waiting for an acknowledgement
    {
        uint16_t sequence = 0;
        std::vector<uint8_t> datagram;
        float sinceSent = 0.0f;
    };

    struct Buffered // arrived out of order, waiting for the gap in front of it to be filled
    {
        uint16_t sequence = 0;
        std::vector<uint8_t> payload;
    };

    struct Delayed // held back by the simulated conditions
    {
        std::vector<uint8_t> datagram;
        sockaddr_in address{};
        float delay = 0.0f;
    };

    struct Peer
    {
        PeerId id = kInvalidPeer;
        sockaddr_in address{};
        bool established = false;
        float sinceHeard = 0.0f;
        float sinceSent = 0.0f;
        uint16_t nextReliable = 1;
        uint16_t expectedReliable = 1;
        std::vector<Outgoing> unacked;
        std::vector<Buffered> received;
    };

    bool OpenSocket(uint16_t port);
    Peer* FindPeer(PeerId id);
    Peer* FindPeer(const sockaddr_in& address);
    Peer& AddPeer(const sockaddr_in& address, PeerId id);
    void DropPeer(PeerId id, bool tellThem);

    void Transmit(std::vector<uint8_t> datagram, const sockaddr_in& address);
    void SendRaw(const std::vector<uint8_t>& datagram, const sockaddr_in& address);
    void SendControl(PacketKind kind, const sockaddr_in& address, Peer* peer);
    void Receive(std::vector<NetPacket>& out);
    void HandleDatagram(const uint8_t* data, size_t bytes, const sockaddr_in& from,
                        std::vector<NetPacket>& out);
    void DeliverReliable(Peer& peer, uint16_t sequence, const uint8_t* payload, size_t bytes,
                         std::vector<NetPacket>& out);
    float DrawDelay();
    bool DrawChance(float percent);

    SocketHandle m_socket = kInvalidSocket;
    bool m_socketStarted = false;
    std::vector<Peer> m_peers;
    std::vector<Delayed> m_delayed;
    std::vector<PeerId> m_connected;
    std::vector<PeerId> m_disconnected;
    NetConditions m_conditions;
    NetStats m_stats;
    std::mt19937 m_rng;
    sockaddr_in m_hostAddress{};
    PeerId m_nextPeerId = kHostPeer + 1;
    float m_connectTimer = 0.0f;
    float m_connectElapsed = 0.0f;
    bool m_listening = false;
    bool m_connecting = false;
};

UdpTransport::~UdpTransport()
{
    for (Peer& peer : m_peers)
    {
        SendControl(PacketKind::Disconnect, peer.address, &peer);
    }
    CloseSocket(m_socket);
    if (m_socketStarted)
    {
        SocketSystem::Release();
    }
}

bool UdpTransport::OpenSocket(uint16_t port)
{
    if (!SocketSystem::Acquire())
    {
        PRED_LOG_ERROR(Network, "Could not start the socket system");
        return false;
    }
    m_socketStarted = true;

    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == kInvalidSocket)
    {
        PRED_LOG_ERROR(Network, "Could not create a UDP socket");
        return false;
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(port);
    if (bind(m_socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
    {
        PRED_LOG_ERROR(Network, "Could not bind UDP port {}", port);
        CloseSocket(m_socket);
        m_socket = kInvalidSocket;
        return false;
    }

    // Everything here polls; nothing is allowed to block the frame.
    if (!SetNonBlocking(m_socket))
    {
        PRED_LOG_ERROR(Network, "Could not put the socket into non-blocking mode");
        CloseSocket(m_socket);
        m_socket = kInvalidSocket;
        return false;
    }
    return true;
}

bool UdpTransport::Listen(uint16_t port)
{
    if (!OpenSocket(port))
    {
        return false;
    }
    m_listening = true;
    PRED_LOG_INFO(Network, "Listening on UDP port {}", port);
    return true;
}

bool UdpTransport::Connect(const std::string& address, uint16_t port)
{
    if (!OpenSocket(0)) // any free local port
    {
        return false;
    }

    m_hostAddress = sockaddr_in{};
    m_hostAddress.sin_family = AF_INET;
    m_hostAddress.sin_port = htons(port);
    const std::string target = address.empty() ? "127.0.0.1" : address;
    if (inet_pton(AF_INET, target.c_str(), &m_hostAddress.sin_addr) != 1)
    {
        PRED_LOG_ERROR(Network, "{} is not an address this build can reach", target);
        return false;
    }

    // The host end of the connection is always peer 1, whatever its address turns out to be.
    Peer& peer = AddPeer(m_hostAddress, kHostPeer);
    peer.established = false;
    m_connecting = true;
    m_connectTimer = 0.0f;
    m_connectElapsed = 0.0f;
    SendControl(PacketKind::ConnectRequest, m_hostAddress, nullptr);
    PRED_LOG_INFO(Network, "Reaching for {}", DescribeAddress(m_hostAddress));
    return true;
}

UdpTransport::Peer* UdpTransport::FindPeer(PeerId id)
{
    const auto found = std::find_if(m_peers.begin(), m_peers.end(),
                                    [&](const Peer& peer) { return peer.id == id; });
    return found == m_peers.end() ? nullptr : &*found;
}

UdpTransport::Peer* UdpTransport::FindPeer(const sockaddr_in& address)
{
    const auto found = std::find_if(m_peers.begin(), m_peers.end(),
                                    [&](const Peer& peer) { return SameAddress(peer.address, address); });
    return found == m_peers.end() ? nullptr : &*found;
}

UdpTransport::Peer& UdpTransport::AddPeer(const sockaddr_in& address, PeerId id)
{
    Peer peer;
    peer.id = id;
    peer.address = address;
    m_peers.push_back(std::move(peer));
    return m_peers.back();
}

void UdpTransport::DropPeer(PeerId id, bool tellThem)
{
    const auto found = std::find_if(m_peers.begin(), m_peers.end(),
                                    [&](const Peer& peer) { return peer.id == id; });
    if (found == m_peers.end())
    {
        return;
    }
    if (tellThem)
    {
        SendControl(PacketKind::Disconnect, found->address, &*found);
    }
    const bool wasEstablished = found->established;
    m_peers.erase(found);
    if (wasEstablished)
    {
        m_disconnected.push_back(id);
    }
}

void UdpTransport::Disconnect(PeerId peer)
{
    DropPeer(peer, true);
}

std::vector<PeerId> UdpTransport::Peers() const
{
    std::vector<PeerId> ids;
    for (const Peer& peer : m_peers)
    {
        if (peer.established)
        {
            ids.push_back(peer.id);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::string UdpTransport::AddressOf(PeerId peer) const
{
    for (const Peer& entry : m_peers)
    {
        if (entry.id == peer)
        {
            // The address as seen from here. On one network that is the machine's own address and
            // works for everybody; through a router it is the hole that router punched, which is
            // open only as long as it stays open.
            return DescribeAddress(entry.address);
        }
    }
    return {};
}

float UdpTransport::DrawDelay()
{
    const float jitter =
        m_conditions.jitterMs <= 0.0f
            ? 0.0f
            : std::uniform_real_distribution<float>(0.0f, m_conditions.jitterMs)(m_rng);
    return std::max(m_conditions.latencyMs + jitter, 0.0f) * 0.001f;
}

bool UdpTransport::DrawChance(float percent)
{
    if (percent <= 0.0f)
    {
        return false;
    }
    return std::uniform_real_distribution<float>(0.0f, 100.0f)(m_rng) < percent;
}

void UdpTransport::SendRaw(const std::vector<uint8_t>& datagram, const sockaddr_in& address)
{
    if (m_socket == kInvalidSocket)
    {
        return;
    }
    sendto(m_socket, reinterpret_cast<const char*>(datagram.data()), static_cast<int>(datagram.size()), 0,
           reinterpret_cast<const sockaddr*>(&address), sizeof(address));
}

void UdpTransport::Transmit(std::vector<uint8_t> datagram, const sockaddr_in& address)
{
    // The simulated conditions are applied to real sockets too, so a bad connection can be
    // reproduced on a good one. On a real link with nothing configured this is a straight send.
    if (m_conditions.latencyMs <= 0.0f && m_conditions.jitterMs <= 0.0f &&
        m_conditions.lossPercent <= 0.0f && m_conditions.duplicatePercent <= 0.0f)
    {
        SendRaw(datagram, address);
        return;
    }

    if (DrawChance(m_conditions.lossPercent))
    {
        ++m_stats.packetsDropped;
        return;
    }

    Delayed held;
    held.datagram = datagram;
    held.address = address;
    held.delay = DrawDelay();
    m_delayed.push_back(held);

    if (DrawChance(m_conditions.duplicatePercent))
    {
        ++m_stats.packetsDuplicated;
        held.datagram = std::move(datagram);
        held.delay = DrawDelay();
        m_delayed.push_back(std::move(held));
    }
}

void UdpTransport::SendControl(PacketKind kind, const sockaddr_in& address, Peer* peer)
{
    std::vector<uint8_t> datagram(kHeaderBytes, 0);
    WriteU32(datagram.data(), kMagic);
    datagram[4] = static_cast<uint8_t>(kind);
    datagram[5] = 0;
    WriteU16(&datagram[6], 0);
    WriteU16(&datagram[8], peer == nullptr ? 0 : static_cast<uint16_t>(peer->expectedReliable - 1));
    WriteU16(&datagram[10], 0);
    Transmit(std::move(datagram), address);
    if (peer != nullptr)
    {
        peer->sinceSent = 0.0f;
    }
}

void UdpTransport::Send(PeerId peerId, Channel channel, const uint8_t* data, size_t bytes)
{
    Peer* peer = FindPeer(peerId);
    if (peer == nullptr || !peer->established || data == nullptr || bytes == 0)
    {
        return;
    }
    if (bytes + kHeaderBytes > kMaxDatagram)
    {
        // Fragmenting is a whole subsystem and nothing in this game needs it: the largest message
        // is a snapshot, and a snapshot is under a hundred bytes.
        PRED_LOG_WARN(Network, "Dropping a {} byte message, over the datagram limit", bytes);
        return;
    }

    ++m_stats.packetsSent;
    m_stats.bytesSent += bytes;

    std::vector<uint8_t> datagram(kHeaderBytes + bytes);
    WriteU32(datagram.data(), kMagic);
    datagram[4] = static_cast<uint8_t>(PacketKind::Payload);
    datagram[5] = static_cast<uint8_t>(channel);
    uint16_t sequence = 0;
    if (channel == Channel::Reliable)
    {
        sequence = peer->nextReliable++;
        if (peer->nextReliable == 0)
        {
            // Zero means "nothing acknowledged yet" in the ack field, so it is never a real
            // sequence number. After sixty-five thousand reliable messages the counter skips it.
            peer->nextReliable = 1;
        }
    }
    WriteU16(&datagram[6], sequence);
    WriteU16(&datagram[8], static_cast<uint16_t>(peer->expectedReliable - 1));
    WriteU16(&datagram[10], 0);
    std::memcpy(datagram.data() + kHeaderBytes, data, bytes);

    if (channel == Channel::Reliable)
    {
        // Kept until the far end says it has it. This is the entire difference between the two
        // channels: an unreliable packet is forgotten the moment it leaves.
        Outgoing pending;
        pending.sequence = sequence;
        pending.datagram = datagram;
        pending.sinceSent = 0.0f;
        peer->unacked.push_back(std::move(pending));
    }

    peer->sinceSent = 0.0f;
    Transmit(std::move(datagram), peer->address);
}

void UdpTransport::DeliverReliable(Peer& peer, uint16_t sequence, const uint8_t* payload, size_t bytes,
                                   std::vector<NetPacket>& out)
{
    if (SequenceGreater(peer.expectedReliable, sequence))
    {
        return; // already delivered: this is a resend of something the ack for was lost
    }

    if (sequence != peer.expectedReliable)
    {
        // Arrived early. Hold it until the gap in front is filled, because reliable means in order
        // as well as eventually.
        const bool known = std::any_of(peer.received.begin(), peer.received.end(),
                                       [&](const Buffered& held) { return held.sequence == sequence; });
        if (!known)
        {
            Buffered held;
            held.sequence = sequence;
            held.payload.assign(payload, payload + bytes);
            peer.received.push_back(std::move(held));
        }
        return;
    }

    NetPacket packet;
    packet.peer = peer.id;
    packet.channel = Channel::Reliable;
    packet.bytes.assign(payload, payload + bytes);
    ++m_stats.packetsReceived;
    m_stats.bytesReceived += packet.bytes.size();
    out.push_back(std::move(packet));
    ++peer.expectedReliable;

    // Whatever was waiting behind it can go now.
    bool progressed = true;
    while (progressed)
    {
        progressed = false;
        for (size_t i = 0; i < peer.received.size(); ++i)
        {
            if (peer.received[i].sequence != peer.expectedReliable)
            {
                continue;
            }
            NetPacket held;
            held.peer = peer.id;
            held.channel = Channel::Reliable;
            held.bytes = std::move(peer.received[i].payload);
            ++m_stats.packetsReceived;
            m_stats.bytesReceived += held.bytes.size();
            out.push_back(std::move(held));
            peer.received.erase(peer.received.begin() + static_cast<ptrdiff_t>(i));
            ++peer.expectedReliable;
            progressed = true;
            break;
        }
    }
}

void UdpTransport::HandleDatagram(const uint8_t* data, size_t bytes, const sockaddr_in& from,
                                  std::vector<NetPacket>& out)
{
    if (bytes < kHeaderBytes || ReadU32(data) != kMagic)
    {
        return; // not ours. Any open UDP port collects scanner traffic.
    }

    const auto kind = static_cast<PacketKind>(data[4]);
    const auto channel = static_cast<Channel>(data[5]);
    const uint16_t sequence = ReadU16(&data[6]);
    const uint16_t ack = ReadU16(&data[8]);

    Peer* peer = FindPeer(from);

    if (kind == PacketKind::ConnectRequest)
    {
        if (!m_listening)
        {
            return;
        }
        if (peer == nullptr)
        {
            peer = &AddPeer(from, m_nextPeerId++);
            peer->established = true;
            m_connected.push_back(peer->id);
            PRED_LOG_INFO(Network, "Peer {} connected from {}", peer->id, DescribeAddress(from));
        }
        // Repeated because the accept can be lost, and a client that never hears one keeps asking.
        SendControl(PacketKind::ConnectAccept, from, peer);
        peer->sinceHeard = 0.0f;
        return;
    }

    if (peer == nullptr)
    {
        return; // anything else from a stranger is noise
    }
    peer->sinceHeard = 0.0f;

    if (kind == PacketKind::ConnectAccept)
    {
        if (!peer->established)
        {
            peer->established = true;
            m_connecting = false;
            m_connected.push_back(peer->id);
            PRED_LOG_INFO(Network, "Connected to {}", DescribeAddress(from));
        }
        return;
    }

    if (kind == PacketKind::Disconnect)
    {
        DropPeer(peer->id, false);
        return;
    }

    // Every packet carries the highest reliable sequence its sender has seen, so acknowledgements
    // cost nothing: they ride along on traffic that was going anyway.
    if (ack != 0)
    {
        peer->unacked.erase(std::remove_if(peer->unacked.begin(), peer->unacked.end(),
                                           [&](const Outgoing& pending) {
                                               return pending.sequence == ack ||
                                                      SequenceGreater(ack, pending.sequence);
                                           }),
                            peer->unacked.end());
    }

    if (kind != PacketKind::Payload || bytes <= kHeaderBytes)
    {
        return;
    }

    const uint8_t* payload = data + kHeaderBytes;
    const size_t payloadBytes = bytes - kHeaderBytes;

    if (channel == Channel::Reliable)
    {
        DeliverReliable(*peer, sequence, payload, payloadBytes, out);
        return;
    }

    NetPacket packet;
    packet.peer = peer->id;
    packet.channel = Channel::Unreliable;
    packet.bytes.assign(payload, payload + payloadBytes);
    ++m_stats.packetsReceived;
    m_stats.bytesReceived += packet.bytes.size();
    out.push_back(std::move(packet));
}

void UdpTransport::Receive(std::vector<NetPacket>& out)
{
    if (m_socket == kInvalidSocket)
    {
        return;
    }

    std::array<uint8_t, kMaxDatagram> buffer{};
    for (int guard = 0; guard < 256; ++guard) // a bound, so a flood cannot hold the frame open
    {
        sockaddr_in from{};
#if defined(_WIN32)
        int fromLength = static_cast<int>(sizeof(from));
#else
        socklen_t fromLength = sizeof(from);
#endif
        const int received =
            recvfrom(m_socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0,
                     reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (received <= 0)
        {
            if (received < 0 && !WouldBlock())
            {
                // On Windows an unreachable destination surfaces here as an error about a datagram
                // this socket sent earlier. It says nothing about the socket, so keep going.
            }
            return;
        }
        HandleDatagram(buffer.data(), static_cast<size_t>(received), from, out);
    }
}

void UdpTransport::Poll(float dt, std::vector<NetPacket>& out)
{
    out.clear();
    if (m_socket == kInvalidSocket)
    {
        return;
    }

    // Anything the simulated conditions were holding back.
    for (size_t i = 0; i < m_delayed.size();)
    {
        m_delayed[i].delay -= dt;
        if (m_delayed[i].delay <= 0.0f)
        {
            SendRaw(m_delayed[i].datagram, m_delayed[i].address);
            m_delayed.erase(m_delayed.begin() + static_cast<ptrdiff_t>(i));
        }
        else
        {
            ++i;
        }
    }

    Receive(out);

    if (m_connecting)
    {
        m_connectTimer += dt;
        m_connectElapsed += dt;
        if (m_connectTimer >= kConnectRetrySeconds)
        {
            m_connectTimer = 0.0f;
            SendControl(PacketKind::ConnectRequest, m_hostAddress, nullptr);
        }
        if (m_connectElapsed >= kConnectTimeoutSeconds)
        {
            PRED_LOG_WARN(Network, "No answer from {}", DescribeAddress(m_hostAddress));
            m_connecting = false;
            DropPeer(kHostPeer, false);
            // A connection that never came up still has to be reported, or the game sits waiting
            // for a welcome that is never coming and never says so.
            m_disconnected.push_back(kHostPeer);
        }
    }

    std::vector<PeerId> lost;
    for (Peer& peer : m_peers)
    {
        peer.sinceHeard += dt;
        peer.sinceSent += dt;

        if (peer.established && peer.sinceHeard >= kTimeoutSeconds)
        {
            lost.push_back(peer.id);
            continue;
        }

        for (Outgoing& pending : peer.unacked)
        {
            pending.sinceSent += dt;
            if (pending.sinceSent >= kResendSeconds)
            {
                pending.sinceSent = 0.0f;
                // Resent with the ack field as it stands now, not as it stood when the packet was
                // first built, so a resend also carries the newest acknowledgement.
                WriteU16(&pending.datagram[8], static_cast<uint16_t>(peer.expectedReliable - 1));
                Transmit(pending.datagram, peer.address);
                peer.sinceSent = 0.0f;
            }
        }

        if (peer.established && peer.sinceSent >= kKeepAliveSeconds)
        {
            // Silence is indistinguishable from a dead connection, and it also lets a router forget
            // the hole a client punched through it.
            SendControl(PacketKind::KeepAlive, peer.address, &peer);
        }
    }

    for (const PeerId id : lost)
    {
        PRED_LOG_WARN(Network, "Peer {} timed out", id);
        DropPeer(id, false);
    }
}

} // namespace

std::unique_ptr<Transport> CreateUdpTransport(uint32_t seed)
{
    return std::make_unique<UdpTransport>(seed);
}

} // namespace pred
