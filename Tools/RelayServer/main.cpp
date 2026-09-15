// The relay: a socket, a clock and a loop around RelayServer.
//
// Run this anywhere both players can reach — a cheap virtual machine, a free tier, a spare box with
// a forwarded port. Everybody connects outwards to it and it forwards between them, which is why it
// works through routers that refuse everything else: outbound always works.
//
// It is deliberately small. All the judgement lives in RelayServer, which has no socket in it and
// is tested without one; this file is the part that cannot be tested that way, so there is as
// little of it as possible.

#include "Engine/Core/Log.h"
#include "Engine/Net/RelayProtocol.h"
#include "Engine/Net/RelayServer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#    include <winsock2.h>
#    include <ws2tcpip.h>
#    include <mstcpip.h>
// Not declared by every Windows SDK layout, and it is a fixed constant rather than something that
// varies: defining it when it is missing is what every project that needs it ends up doing.
#    ifndef SIO_UDP_CONNRESET
#        define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#    endif
using SocketHandle = SOCKET;
static constexpr SocketHandle kNoSocket = INVALID_SOCKET;
#else
#    include <arpa/inet.h>
#    include <fcntl.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
using SocketHandle = int;
static constexpr SocketHandle kNoSocket = -1;
#endif

using namespace pred;

namespace
{

std::atomic<bool> g_running{true};

void OnSignal(int)
{
    g_running = false;
}

// An address as text, which is the key the relay knows a client by. It never parses it back; it
// only compares them, so the exact spelling matters only in that it must be stable and unique.
std::string KeyOf(const sockaddr_in& address)
{
    char text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text));
    return std::string(text) + ":" + std::to_string(ntohs(address.sin_port));
}

// Every address this machine answers on, so the operator can read off what to hand out.
//
// Whoever starts the relay knows it is running, because it says so. What they do not know is the
// address to give anybody else, and getting that wrong is indistinguishable from the relay being
// broken: both look like a lobby that opens, sits there, and nobody ever arrives.
std::vector<std::string> LocalAddresses()
{
    std::vector<std::string> found;
    char host[256] = {};
    if (gethostname(host, sizeof(host) - 1) != 0)
    {
        return found;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* list = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &list) != 0 || list == nullptr)
    {
        return found;
    }
    for (const addrinfo* entry = list; entry != nullptr; entry = entry->ai_next)
    {
        char text[INET_ADDRSTRLEN] = {};
        const auto* in = reinterpret_cast<const sockaddr_in*>(entry->ai_addr);
        inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text));
        const std::string address = text;
        if (!address.empty() &&
            std::find(found.begin(), found.end(), address) == found.end())
        {
            found.push_back(address);
        }
    }
    freeaddrinfo(list);
    return found;
}

} // namespace

int main(int argc, char** argv)
{
    uint16_t port = 27020;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if ((argument == "--port" || argument == "-p") && i + 1 < argc)
        {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        }
        else if (argument == "--help" || argument == "-h")
        {
            std::printf("Project Predation relay\n"
                        "  --port <n>   which UDP port to listen on (default 27020)\n");
            return 0;
        }
    }

    Log::Init({});
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

#if defined(_WIN32)
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0)
    {
        PRED_LOG_ERROR(Network, "Could not start Winsock");
        return 1;
    }
#endif

    const SocketHandle handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == kNoSocket)
    {
        PRED_LOG_ERROR(Network, "Could not open a socket");
        return 1;
    }

    sockaddr_in bound{};
    bound.sin_family = AF_INET;
    bound.sin_addr.s_addr = htonl(INADDR_ANY);
    bound.sin_port = htons(port);
    if (bind(handle, reinterpret_cast<const sockaddr*>(&bound), sizeof(bound)) != 0)
    {
        PRED_LOG_ERROR(Network, "Could not bind UDP port {}", port);
        return 1;
    }

#if defined(_WIN32)
    u_long nonBlocking = 1;
    ioctlsocket(handle, static_cast<long>(FIONBIO), &nonBlocking);
    // Without this, one ICMP "port unreachable" from a client that has gone makes every later
    // recvfrom on this socket fail with WSAECONNRESET. One departing player would take the relay
    // down for everybody, which is a thing that has to be switched off explicitly on Windows.
    DWORD behaviour = 0;
    DWORD returned = 0;
    WSAIoctl(handle, SIO_UDP_CONNRESET, &behaviour, sizeof(behaviour), nullptr, 0, &returned,
             nullptr, nullptr);
#else
    const int flags = fcntl(handle, F_GETFL, 0);
    fcntl(handle, F_SETFL, flags | O_NONBLOCK);
#endif

    RelayServer::Settings settings;
    RelayServer relay(settings);
    PRED_LOG_INFO(Network, "Relay listening on UDP {}", port);
    for (const std::string& address : LocalAddresses())
    {
        PRED_LOG_INFO(Network, "  reachable on this network at {}:{}", address, port);
    }
    PRED_LOG_INFO(Network,
                  "  players on this machine use 127.0.0.1:{}; players elsewhere on the internet "
                  "need this machine's public address and UDP {} forwarded to it",
                  port, port);

    std::array<uint8_t, 1400> buffer{};
    std::vector<RelayServer::Outgoing> outgoing;
    // Where each key was last seen, so a reply can be addressed. The relay deals in keys and never
    // in addresses; this is the one place the two meet.
    std::unordered_map<std::string, sockaddr_in> addresses;

    auto previous = std::chrono::steady_clock::now();
    size_t lastLobbies = 0;
    size_t lastClients = 0;
    // Stray traffic is noted per datagram and logged per address, because a port scanner can send
    // thousands and the useful information is "this address is talking to us and it is not the
    // game", which is one line however many datagrams it took.
    std::unordered_map<std::string, int> strays;
    auto lastStrayReport = std::chrono::steady_clock::now();

    while (g_running)
    {
        // Everything waiting, bounded so a flood cannot hold the loop open past its tick.
        for (int guard = 0; guard < 1024; ++guard)
        {
            sockaddr_in from{};
#if defined(_WIN32)
            int fromLength = static_cast<int>(sizeof(from));
#else
            socklen_t fromLength = sizeof(from);
#endif
            const int received =
                recvfrom(handle, reinterpret_cast<char*>(buffer.data()),
                         static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&from),
                         &fromLength);
            if (received <= 0)
            {
                break;
            }
            std::string key = KeyOf(from);
            addresses[key] = from;
            relay.Receive(key, buffer.data(), static_cast<size_t>(received), outgoing);
        }

        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - previous).count();
        previous = now;
        relay.Tick(dt, outgoing);

        for (const RelayServer::Outgoing& entry : outgoing)
        {
            const auto found = addresses.find(entry.to);
            if (found == addresses.end())
            {
                continue;
            }
            sendto(handle, reinterpret_cast<const char*>(entry.datagram.data()),
                   static_cast<int>(entry.datagram.size()), 0,
                   reinterpret_cast<const sockaddr*>(&found->second), sizeof(found->second));
        }
        outgoing.clear();

        // Addresses of clients the relay has forgotten are forgotten here too, or this map is a
        // slow leak for as long as the relay runs.
        if (addresses.size() > relay.ClientCount() * 4 + 64)
        {
            for (auto it = addresses.begin(); it != addresses.end();)
            {
                it = relay.CodeOf(it->first) == 0 ? addresses.erase(it) : std::next(it);
            }
        }

        // What actually happened, one line each.
        //
        // This is the relay's only window into itself, so it errs towards saying too much: a lobby
        // that opens and is never joined and one that is joined by somebody the game then cannot
        // reach are the same picture from outside, and the difference is the whole diagnosis.
        for (const RelayServer::Note& note : relay.Notes())
        {
            const std::string code = note.code == 0 ? std::string("-") : DecodeRelayCode(note.code);
            switch (note.kind)
            {
            case RelayServer::Note::Kind::Opened:
                PRED_LOG_INFO(Network, "Lobby {} opened by {}", code, note.client);
                break;
            case RelayServer::Note::Kind::Joined:
                PRED_LOG_INFO(Network, "Lobby {}: {} joined as slot {}", code, note.client,
                              static_cast<int>(note.slot));
                break;
            case RelayServer::Note::Kind::Left:
                PRED_LOG_INFO(Network, "Lobby {}: slot {} ({}) {}", code,
                              static_cast<int>(note.slot), note.client,
                              note.timedOut ? "went quiet and was dropped" : "left");
                break;
            case RelayServer::Note::Kind::Closed:
                PRED_LOG_INFO(Network, "Lobby {} closed{}", code,
                              note.timedOut ? " because the host went quiet" : "");
                break;
            case RelayServer::Note::Kind::Refused:
                PRED_LOG_WARN(Network, "Refused {} asking for lobby {}: {}", note.client, code,
                              pred::Describe(note.reason));
                break;
            case RelayServer::Note::Kind::Ignored:
                ++strays[note.client];
                break;
            }
        }
        relay.Notes().clear();

        if (!strays.empty() && now - lastStrayReport > std::chrono::seconds(10))
        {
            lastStrayReport = now;
            for (const auto& [who, count] : strays)
            {
                PRED_LOG_WARN(Network, "{} datagram(s) from {} that are not this game's", count, who);
            }
            strays.clear();
        }

        // And the standing count, on any change. It used to print only when the number of lobbies
        // changed, which meant a second player joining -- the one event anybody starts this program
        // to watch for -- printed nothing at all.
        if (relay.LobbyCount() != lastLobbies || relay.ClientCount() != lastClients)
        {
            lastLobbies = relay.LobbyCount();
            lastClients = relay.ClientCount();
            PRED_LOG_INFO(Network, "{} lobbies, {} clients", lastLobbies, lastClients);
        }

        // A relay is not a game: it has nothing to do between datagrams, and spinning would burn a
        // core on somebody's virtual machine for no reason.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    PRED_LOG_INFO(Network, "Relay stopping");
#if defined(_WIN32)
    closesocket(handle);
    WSACleanup();
#else
    close(handle);
#endif
    Log::Shutdown();
    return 0;
}
