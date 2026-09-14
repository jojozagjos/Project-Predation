#include "Engine/Net/RelayCarrier.h"

#include "Engine/Core/Log.h"

#include <algorithm>
#include <array>
#include <cstring>

#if defined(_WIN32)
#    include <winsock2.h>
#    include <ws2tcpip.h>
using SocketHandle = SOCKET;
static constexpr SocketHandle kNoSocket = INVALID_SOCKET;
#else
#    include <arpa/inet.h>
#    include <fcntl.h>
#    include <netdb.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
using SocketHandle = int;
static constexpr SocketHandle kNoSocket = -1;
#endif

namespace pred
{
namespace
{

constexpr size_t kMaxDatagram = 1200;
// How many datagrams may wait to be read before the oldest are dropped. A frame's worth of traffic
// for a full lobby is a few dozen; past this the game is not keeping up and old datagrams are worth
// less than new ones.
constexpr size_t kMaxQueued = 512;

SocketHandle AsSocket(int64_t handle)
{
    return static_cast<SocketHandle>(handle);
}

bool MakeNonBlocking(SocketHandle handle)
{
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket(handle, static_cast<long>(FIONBIO), &mode) == 0;
#else
    const int flags = fcntl(handle, F_GETFL, 0);
    return flags >= 0 && fcntl(handle, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void CloseHandle(SocketHandle handle)
{
    if (handle == kNoSocket)
    {
        return;
    }
#if defined(_WIN32)
    closesocket(handle);
#else
    close(handle);
#endif
}

// Resolves a name or an address. The relay is given by name in the ordinary case, because whoever
// runs it will move it and a name is the thing that survives that.
bool Resolve(const std::string& host, uint16_t port, sockaddr_in& out)
{
    out = sockaddr_in{};
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &out.sin_addr) == 1)
    {
        return true;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* results = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &results) != 0 || results == nullptr)
    {
        return false;
    }
    bool found = false;
    for (const addrinfo* it = results; it != nullptr; it = it->ai_next)
    {
        if (it->ai_family == AF_INET && it->ai_addr != nullptr)
        {
            const auto* address = reinterpret_cast<const sockaddr_in*>(it->ai_addr);
            out.sin_addr = address->sin_addr;
            found = true;
            break;
        }
    }
    freeaddrinfo(results);
    return found;
}

} // namespace

RelayCarrier::~RelayCarrier()
{
    Close();
}

void RelayCarrier::Close()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_socket >= 0)
    {
        // Tell the relay, so the lobby closes now rather than when it times out. Best effort: if it
        // does not arrive, the timeout is the backstop and that is what it is for.
        if (m_state == State::Ready && !m_relayAddress.empty())
        {
            RelayPacket leave;
            leave.kind = RelayMessage::Leave;
            const std::vector<uint8_t> bytes = EncodeRelay(leave);
            sendto(AsSocket(m_socket), reinterpret_cast<const char*>(bytes.data()),
                   static_cast<int>(bytes.size()), 0,
                   reinterpret_cast<const sockaddr*>(m_relayAddress.data()),
                   static_cast<int>(sizeof(sockaddr_in)));
        }
        CloseHandle(AsSocket(m_socket));
        m_socket = -1;
    }
    m_state = State::Idle;
    m_code = 0;
    m_slot = kRelayNoSlot;
    m_slots.clear();
    m_incoming.clear();
    m_relayAddress.clear();
}

bool RelayCarrier::Host(const Settings& settings)
{
    if (!Join(settings, 0))
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_hosting = true;
    RelayPacket request;
    request.kind = RelayMessage::Host;
    SendToRelay(request);
    return true;
}

bool RelayCarrier::Join(const Settings& settings, uint32_t code)
{
    Close();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_settings = settings;
    m_message.clear();
    m_rejection = RelayRejection::None;
    m_hosting = false;
    m_connectTimer = 0.0f;
    m_keepAliveTimer = 0.0f;

    sockaddr_in relay{};
    if (!Resolve(settings.relayHost, settings.relayPort, relay))
    {
        m_message = "Cannot find the relay at " + settings.relayHost;
        m_state = State::Failed;
        return false;
    }
    m_relayAddress.resize(sizeof(relay));
    std::memcpy(m_relayAddress.data(), &relay, sizeof(relay));

    const SocketHandle handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == kNoSocket)
    {
        m_message = "No socket";
        m_state = State::Failed;
        return false;
    }
    if (!MakeNonBlocking(handle))
    {
        CloseHandle(handle);
        m_message = "Could not set the socket non-blocking";
        m_state = State::Failed;
        return false;
    }
    m_socket = static_cast<int64_t>(handle);
    m_state = State::Connecting;

    if (code != 0)
    {
        RelayPacket request;
        request.kind = RelayMessage::Join;
        request.code = code;
        SendToRelay(request);
    }
    return true;
}

void RelayCarrier::SendToRelay(const RelayPacket& packet)
{
    if (m_socket < 0 || m_relayAddress.empty())
    {
        return;
    }
    const std::vector<uint8_t> bytes = EncodeRelay(packet);
    sendto(AsSocket(m_socket), reinterpret_cast<const char*>(bytes.data()),
           static_cast<int>(bytes.size()), 0,
           reinterpret_cast<const sockaddr*>(m_relayAddress.data()),
           static_cast<int>(sizeof(sockaddr_in)));
}

size_t RelayCarrier::LinkForSlot(uint8_t slot)
{
    const auto found = std::find(m_slots.begin(), m_slots.end(), slot);
    if (found != m_slots.end())
    {
        return static_cast<size_t>(found - m_slots.begin());
    }
    m_slots.push_back(slot);
    return m_slots.size() - 1;
}

void RelayCarrier::Poll(float dt)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_socket < 0)
    {
        return;
    }

    if (m_state == State::Connecting)
    {
        m_connectTimer += dt;
        if (m_connectTimer > m_settings.connectTimeoutSeconds)
        {
            m_state = State::Failed;
            m_message = "The relay did not answer. Check the address, or it may be down.";
            return;
        }
        // Asked again while waiting, in case the first request was lost. The relay repeats what it
        // told us rather than opening a second lobby, which is why this is safe.
        m_keepAliveTimer += dt;
        if (m_keepAliveTimer >= 1.0f)
        {
            m_keepAliveTimer = 0.0f;
            RelayPacket again;
            again.kind = m_hosting ? RelayMessage::Host : RelayMessage::Join;
            again.code = m_code;
            if (m_hosting || m_code != 0)
            {
                SendToRelay(again);
            }
        }
    }
    else if (m_state == State::Ready)
    {
        m_keepAliveTimer += dt;
        if (m_keepAliveTimer >= m_settings.keepAliveSeconds)
        {
            m_keepAliveTimer = 0.0f;
            RelayPacket alive;
            alive.kind = RelayMessage::KeepAlive;
            SendToRelay(alive);
        }
    }

    std::array<uint8_t, kMaxDatagram> buffer{};
    for (int guard = 0; guard < 256; ++guard)
    {
        sockaddr_in from{};
#if defined(_WIN32)
        int fromLength = static_cast<int>(sizeof(from));
#else
        socklen_t fromLength = sizeof(from);
#endif
        const int received =
            recvfrom(AsSocket(m_socket), reinterpret_cast<char*>(buffer.data()),
                     static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&from),
                     &fromLength);
        if (received <= 0)
        {
            break;
        }
        // Only from the relay. Anything else on this socket is somebody who found the port.
        const auto* expected = reinterpret_cast<const sockaddr_in*>(m_relayAddress.data());
        if (from.sin_addr.s_addr != expected->sin_addr.s_addr || from.sin_port != expected->sin_port)
        {
            continue;
        }

        RelayPacket packet;
        if (!DecodeRelay(buffer.data(), static_cast<size_t>(received), packet))
        {
            continue;
        }

        switch (packet.kind)
        {
        case RelayMessage::Hosted:
            m_code = packet.code;
            m_slot = packet.slot;
            m_state = State::Ready;
            PRED_LOG_INFO(Network, "Lobby open, code {}", DecodeRelayCode(m_code));
            break;

        case RelayMessage::Joined:
            m_slot = packet.slot;
            m_state = State::Ready;
            PRED_LOG_INFO(Network, "Joined a lobby as slot {}", m_slot);
            break;

        case RelayMessage::Rejected:
            m_rejection = packet.reason;
            m_message = Describe(packet.reason);
            m_state = State::Failed;
            break;

        case RelayMessage::PeerJoined:
            // A link is made for them now rather than when they first speak, so the transport has
            // somewhere to dial before they say anything.
            LinkForSlot(packet.slot);
            break;

        case RelayMessage::PeerLeft:
            // The link is kept rather than removed. The transport holds a peer per link and
            // renumbering would move somebody else's peer under it; a link whose slot has gone
            // simply stops carrying anything, and the transport times that peer out as it would any
            // other silence.
            break;

        case RelayMessage::Relayed:
        {
            if (packet.payload.empty())
            {
                break;
            }
            Incoming arrival;
            arrival.link = LinkForSlot(packet.slot);
            arrival.bytes = std::move(packet.payload);
            m_incoming.push_back(std::move(arrival));
            while (m_incoming.size() > kMaxQueued)
            {
                m_incoming.pop_front();
            }
            break;
        }

        default:
            break;
        }
    }
}

RelayCarrier::State RelayCarrier::Status() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

uint32_t RelayCarrier::Code() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_code;
}

std::string RelayCarrier::CodeText() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_code == 0 ? std::string() : DecodeRelayCode(m_code);
}

uint8_t RelayCarrier::Slot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_slot;
}

RelayRejection RelayCarrier::Rejection() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_rejection;
}

size_t RelayCarrier::Links() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_slots.size();
}

bool RelayCarrier::Send(size_t link, const uint8_t* data, size_t bytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != State::Ready || link >= m_slots.size() || data == nullptr || bytes == 0 ||
        bytes > kRelayMaxPayload)
    {
        return false;
    }
    RelayPacket packet;
    packet.kind = RelayMessage::Data;
    packet.slot = m_slots[link];
    packet.payload.assign(data, data + bytes);
    SendToRelay(packet);
    return true;
}

bool RelayCarrier::Receive(size_t& link, std::vector<uint8_t>& out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_incoming.empty())
    {
        return false;
    }
    link = m_incoming.front().link;
    out = std::move(m_incoming.front().bytes);
    m_incoming.pop_front();
    return true;
}

bool RelayCarrier::Live(size_t link) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state == State::Ready && link < m_slots.size();
}

} // namespace pred
