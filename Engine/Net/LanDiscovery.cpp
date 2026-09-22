#include "Engine/Net/LanDiscovery.h"

#include "Engine/Net/BitStream.h"
#include "Engine/Net/SocketSystem.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

#if defined(_WIN32)
#    include <winsock2.h>
#    include <ws2tcpip.h>
#    include <iphlpapi.h>
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#    include <arpa/inet.h>
#    include <fcntl.h>
#    include <ifaddrs.h>
#    include <net/if.h>
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

// Its own magic, different from the game's and from the relay's. All three can end up on one
// machine and a datagram arriving at the wrong one should be dropped, not half understood.
constexpr uint32_t kLanMagic = 0x504C414Eu; // 'PLAN'
// Four a second. Small enough to be nothing -- a beacon is about forty bytes -- and frequent enough
// that a browser opened after the host started fills in immediately rather than after a pause long
// enough to make somebody think it is broken.
constexpr float kBeaconInterval = 0.25f;
constexpr size_t kMaxBeaconBytes = 128;

void CloseHandle(SocketHandle& handle)
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
    handle = kInvalidSocket;
}

bool SetNonBlocking(SocketHandle handle)
{
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket(handle, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(handle, F_GETFL, 0);
    return flags >= 0 && fcntl(handle, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// 'PLAQ': "is anybody hosting?", from somebody browsing. Its own magic, so a host can tell it from the
// beacons it also hears on the same port.
constexpr uint32_t kLanQueryMagic = 0x504C4151u;
// How often a browser asks, and how often the list of places to shout is looked up again. Adapters
// come and go -- a VPN connects, wifi changes -- but not every quarter of a second.
constexpr float kQueryInterval = 1.0f;
constexpr float kTargetRefreshSeconds = 5.0f;

// Where to shout, in network byte order: everywhere at once, and each network adapter's own
// broadcast address.
//
// Windows sends the first of those -- 255.255.255.255 -- out of one adapter only, whichever it thinks
// is the default. A PC with a VPN, a virtual switch for WSL or Docker, or wired and wifi both
// connected was shouting into the wrong one, and the PC across the room never heard it. An adapter's
// own broadcast address (192.168.1.255 for 192.168.1.20/24) goes out of that adapter, so sending to
// each of them reaches every network this PC is on.
std::vector<uint32_t> BroadcastTargets()
{
    std::vector<uint32_t> targets{htonl(INADDR_BROADCAST)};
    const auto add = [&](uint32_t address, uint32_t mask)
    {
        // Host byte order in, network order out. Link-local and loopback are nowhere to shout.
        if ((address >> 24) == 127 || (address >> 16) == 0xA9FEu || mask == 0 || mask == 0xFFFFFFFFu)
        {
            return;
        }
        const uint32_t broadcast = htonl((address & mask) | ~mask);
        if (std::find(targets.begin(), targets.end(), broadcast) == targets.end() && targets.size() < 16)
        {
            targets.push_back(broadcast);
        }
    };
#if defined(_WIN32)
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer(size);
    constexpr ULONG kFlags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG result = GetAdaptersAddresses(AF_INET, kFlags, nullptr,
                                        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (result == ERROR_BUFFER_OVERFLOW)
    {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_INET, kFlags, nullptr,
                                      reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (result == NO_ERROR)
    {
        for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter != nullptr;
             adapter = adapter->Next)
        {
            if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            {
                continue;
            }
            for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next)
            {
                if (unicast->Address.lpSockaddr == nullptr || unicast->Address.lpSockaddr->sa_family != AF_INET)
                {
                    continue;
                }
                const auto* in = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
                const ULONG prefix = unicast->OnLinkPrefixLength;
                const uint32_t mask = prefix == 0 || prefix > 32 ? 0u : (0xFFFFFFFFu << (32u - prefix));
                add(ntohl(in->sin_addr.s_addr), mask);
            }
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0)
    {
        for (const ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next)
        {
            if (entry->ifa_addr == nullptr || entry->ifa_netmask == nullptr ||
                entry->ifa_addr->sa_family != AF_INET || (entry->ifa_flags & IFF_BROADCAST) == 0)
            {
                continue;
            }
            add(ntohl(reinterpret_cast<const sockaddr_in*>(entry->ifa_addr)->sin_addr.s_addr),
                ntohl(reinterpret_cast<const sockaddr_in*>(entry->ifa_netmask)->sin_addr.s_addr));
        }
        freeifaddrs(list);
    }
#endif
    return targets;
}

void SendToAll(SocketHandle handle, const std::vector<uint32_t>& targets, const std::vector<uint8_t>& datagram)
{
    for (const uint32_t target : targets)
    {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(kLanDiscoveryPort);
        to.sin_addr.s_addr = target;
        // Results ignored on purpose: an adapter that refuses broadcast -- a VPN, a virtual switch --
        // fails on its own address and the others still go.
        sendto(handle, reinterpret_cast<const char*>(datagram.data()), static_cast<int>(datagram.size()), 0,
               reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }
}

} // namespace

std::vector<uint8_t> EncodeLanQuery()
{
    BitWriter writer(8);
    writer.WriteUInt(kLanQueryMagic);
    return writer.Finish();
}

bool IsLanQuery(const uint8_t* data, size_t bytes)
{
    if (data == nullptr || bytes != 4)
    {
        return false;
    }
    BitReader reader(data, bytes);
    return reader.ReadUInt() == kLanQueryMagic;
}

// --- The wire ----------------------------------------------------------------------------------

std::vector<uint8_t> EncodeLanBeacon(const LanLobby& lobby)
{
    BitWriter writer(kMaxBeaconBytes);
    writer.WriteUInt(kLanMagic);
    writer.WriteBits(lobby.protocol, 16);
    writer.WriteBits(lobby.port, 16);
    writer.WriteBits(lobby.players, 4);
    writer.WriteBits(lobby.maxPlayers, 4);
    writer.WriteBool(lobby.started);
    const auto length = static_cast<uint32_t>(std::min(lobby.name.size(), kLanMaxNameLength));
    writer.WriteBits(length, 5);
    for (uint32_t i = 0; i < length; ++i)
    {
        writer.WriteByte(static_cast<uint8_t>(lobby.name[i]));
    }
    return writer.Finish();
}

bool DecodeLanBeacon(const uint8_t* data, size_t bytes, LanLobby& out)
{
    if (data == nullptr || bytes < 8 || bytes > kMaxBeaconBytes)
    {
        return false;
    }
    BitReader reader(data, bytes);
    if (reader.ReadUInt() != kLanMagic)
    {
        return false;
    }
    out = LanLobby{};
    out.protocol = static_cast<uint16_t>(reader.ReadBits(16));
    out.port = static_cast<uint16_t>(reader.ReadBits(16));
    out.players = static_cast<uint8_t>(reader.ReadBits(4));
    out.maxPlayers = static_cast<uint8_t>(reader.ReadBits(4));
    out.started = reader.ReadBool();
    const uint32_t length =
        std::min<uint32_t>(reader.ReadBits(5), static_cast<uint32_t>(kLanMaxNameLength));
    out.name.reserve(length);
    for (uint32_t i = 0; i < length && !reader.Overran(); ++i)
    {
        const auto c = static_cast<char>(reader.ReadByte());
        // Printable ASCII only. This is drawn in a list on the screen of everybody on the network,
        // and anybody on the network can send one, so it does not get to carry control characters.
        out.name.push_back(c >= 0x20 && c < 0x7F ? c : '?');
    }
    // A port of zero is a beacon pointing nowhere. Refused rather than listed, because the only
    // thing clicking it could do is fail.
    return !reader.Overran() && out.port != 0;
}

void MergeLanLobby(std::vector<LanLobby>& lobbies, const LanLobby& heard)
{
    for (LanLobby& known : lobbies)
    {
        if (known.address == heard.address && known.port == heard.port)
        {
            // Replaced rather than merged: the beacon is the whole truth about that lobby, and the
            // player count is the field most likely to have changed.
            const std::string address = known.address;
            known = heard;
            known.address = address;
            known.silentFor = 0.0f;
            return;
        }
    }
    // A cap, because the port is reachable by everybody on the network and the list is drawn on a
    // screen. Far more than any house has.
    if (lobbies.size() < 32)
    {
        lobbies.push_back(heard);
        lobbies.back().silentFor = 0.0f;
    }
}

void AgeLanLobbies(std::vector<LanLobby>& lobbies, float dt)
{
    for (LanLobby& lobby : lobbies)
    {
        lobby.silentFor += dt;
    }
    lobbies.erase(std::remove_if(lobbies.begin(), lobbies.end(),
                                 [](const LanLobby& lobby)
                                 { return lobby.silentFor > kLanForgetSeconds; }),
                  lobbies.end());
}

// --- The beacon --------------------------------------------------------------------------------

struct LanBeacon::Impl
{
    SocketHandle handle = kInvalidSocket;
    LanLobby lobby;
    float timer = 0.0f;
    bool acquired = false;
    // Bound to the discovery port, so it hears browsers asking and can answer them.
    bool answering = false;
    std::vector<uint32_t> targets;
    float targetAge = 1.0e9f;
};

LanBeacon::LanBeacon() = default;

LanBeacon::~LanBeacon()
{
    Stop();
}

bool LanBeacon::Start()
{
    Stop();
    if (!SocketSystem::Acquire())
    {
        m_message = "Networking could not be started on this machine.";
        return false;
    }

    auto impl = std::make_unique<Impl>();
    impl->acquired = true;
    impl->handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl->handle == kInvalidSocket)
    {
        SocketSystem::Release();
        m_message = "Could not open a socket to announce the game.";
        return false;
    }

    int broadcast = 1;
    if (setsockopt(impl->handle, SOL_SOCKET, SO_BROADCAST,
                   reinterpret_cast<const char*>(&broadcast), sizeof(broadcast)) != 0)
    {
        CloseHandle(impl->handle);
        SocketSystem::Release();
        m_message = "This machine will not let the game announce itself on the network.";
        return false;
    }
    SetNonBlocking(impl->handle);

    // Bound to the discovery port, shared, so it also hears browsers asking who is there and can
    // answer each one directly. That answer is what gets through when the browsing PC's firewall
    // throws away announcements nobody asked for: Windows lets a reply to its own broadcast back in
    // for a few seconds, even on a network it treats as public. Shared with the listener in the same
    // process, and not fatal if it cannot be had -- the beacon still announces.
    int reuse = 1;
    setsockopt(impl->handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(kLanDiscoveryPort);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    impl->answering = bind(impl->handle, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == 0;
    m_impl = impl.release();
    m_message.clear();
    return true;
}

void LanBeacon::Stop()
{
    if (m_impl == nullptr)
    {
        return;
    }
    CloseHandle(m_impl->handle);
    if (m_impl->acquired)
    {
        SocketSystem::Release();
    }
    delete m_impl;
    m_impl = nullptr;
}

bool LanBeacon::Running() const
{
    return m_impl != nullptr;
}

void LanBeacon::Describe(const LanLobby& lobby)
{
    if (m_impl == nullptr)
    {
        return;
    }
    m_impl->lobby = lobby;
    m_impl->lobby.address.clear();
    m_impl->lobby.silentFor = 0.0f;
}

void LanBeacon::Tick(float dt)
{
    if (m_impl == nullptr || m_impl->lobby.port == 0)
    {
        return;
    }
    const std::vector<uint8_t> datagram = EncodeLanBeacon(m_impl->lobby);

    // Anybody asking who is there is answered straight back, to exactly where they asked from. It is
    // a reply to their own question, so their firewall lets it in. Bounded, like everything else a
    // stranger on the network can make this do.
    if (m_impl->answering)
    {
        std::array<uint8_t, kMaxBeaconBytes> buffer{};
        for (int i = 0; i < 32; ++i)
        {
            sockaddr_in from{};
#if defined(_WIN32)
            int fromLength = sizeof(from);
#else
            socklen_t fromLength = sizeof(from);
#endif
            const int received = recvfrom(m_impl->handle, reinterpret_cast<char*>(buffer.data()),
                                          static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&from),
                                          &fromLength);
            if (received <= 0)
            {
                break;
            }
            if (IsLanQuery(buffer.data(), static_cast<size_t>(received)))
            {
                sendto(m_impl->handle, reinterpret_cast<const char*>(datagram.data()),
                       static_cast<int>(datagram.size()), 0, reinterpret_cast<const sockaddr*>(&from),
                       sizeof(from));
            }
        }
    }

    m_impl->targetAge += dt;
    if (m_impl->targetAge >= kTargetRefreshSeconds)
    {
        m_impl->targetAge = 0.0f;
        m_impl->targets = BroadcastTargets();
    }
    m_impl->timer += dt;
    if (m_impl->timer < kBeaconInterval)
    {
        return;
    }
    m_impl->timer = 0.0f;
    // Out of every network adapter, not only the one Windows picks for the all-networks address.
    SendToAll(m_impl->handle, m_impl->targets, datagram);
}

// --- The listener ------------------------------------------------------------------------------

struct LanListener::Impl
{
    SocketHandle handle = kInvalidSocket;
    bool acquired = false;
    // Asking, as well as listening: see Tick.
    float queryTimer = 0.0f;
    float targetAge = 1.0e9f;
    std::vector<uint32_t> targets;
};

LanListener::LanListener() = default;

LanListener::~LanListener()
{
    Stop();
}

bool LanListener::Start()
{
    Stop();
    if (!SocketSystem::Acquire())
    {
        m_message = "Networking could not be started on this machine.";
        return false;
    }

    auto impl = std::make_unique<Impl>();
    impl->acquired = true;
    impl->handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl->handle == kInvalidSocket)
    {
        SocketSystem::Release();
        m_message = "Could not open a socket to look for games.";
        return false;
    }

    // Two copies of the game on one machine is the ordinary way this gets tested, and it is also
    // two people on one PC with two accounts. Without this the second one cannot bind and simply
    // never finds anything.
    int reuse = 1;
    setsockopt(impl->handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));
    // And allowed to shout, because it asks as well as listens.
    int broadcast = 1;
    setsockopt(impl->handle, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast),
               sizeof(broadcast));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kLanDiscoveryPort);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(impl->handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
    {
        CloseHandle(impl->handle);
        SocketSystem::Release();
        m_message = "Something else on this machine already has the port games are announced on.";
        return false;
    }
    SetNonBlocking(impl->handle);

    m_impl = impl.release();
    m_message.clear();
    return true;
}

void LanListener::Stop()
{
    m_lobbies.clear();
    if (m_impl == nullptr)
    {
        return;
    }
    CloseHandle(m_impl->handle);
    if (m_impl->acquired)
    {
        SocketSystem::Release();
    }
    delete m_impl;
    m_impl = nullptr;
}

bool LanListener::Running() const
{
    return m_impl != nullptr;
}

void LanListener::Tick(float dt)
{
    AgeLanLobbies(m_lobbies, dt);
    if (m_impl == nullptr)
    {
        return;
    }

    // Asking who is there, once a second, out of every adapter. Hosts answer straight back, and an
    // answer to a question this machine asked gets through its firewall even where announcements
    // nobody asked for do not -- which is the usual reason a game on the same wifi never showed up.
    m_impl->targetAge += dt;
    if (m_impl->targetAge >= kTargetRefreshSeconds)
    {
        m_impl->targetAge = 0.0f;
        m_impl->targets = BroadcastTargets();
    }
    m_impl->queryTimer -= dt;
    if (m_impl->queryTimer <= 0.0f)
    {
        m_impl->queryTimer = kQueryInterval;
        SendToAll(m_impl->handle, m_impl->targets, EncodeLanQuery());
    }

    std::array<uint8_t, kMaxBeaconBytes> buffer{};
    // Bounded, so a flood of beacons cannot hold the frame. Anything past this waits for the next
    // one, which at four beacons a second per host is a long way beyond any real network.
    for (int i = 0; i < 64; ++i)
    {
        sockaddr_in from{};
#if defined(_WIN32)
        int fromLength = sizeof(from);
#else
        socklen_t fromLength = sizeof(from);
#endif
        const int received =
            recvfrom(m_impl->handle, reinterpret_cast<char*>(buffer.data()),
                     static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&from),
                     &fromLength);
        if (received <= 0)
        {
            break;
        }

        LanLobby heard;
        if (!DecodeLanBeacon(buffer.data(), static_cast<size_t>(received), heard))
        {
            continue;
        }
        char text[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text)) == nullptr)
        {
            continue;
        }
        heard.address = text;
        MergeLanLobby(m_lobbies, heard);
    }
}

} // namespace pred
