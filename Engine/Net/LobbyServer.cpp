#include "Engine/Net/LobbyServer.h"

#include "Engine/Net/SocketSystem.h"

#include <array>
#include <vector>

#if defined(_WIN32)
#    include <winsock2.h>
#    include <ws2tcpip.h>
#    include <mstcpip.h>
#    ifndef SIO_UDP_CONNRESET
#        define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#    endif
using SocketHandle = SOCKET;
#else
#    include <arpa/inet.h>
#    include <fcntl.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
using SocketHandle = int;
#endif

namespace pred
{
namespace
{

SocketHandle HandleOf(int64_t socket)
{
    return static_cast<SocketHandle>(socket);
}

void CloseSocketHandle(int64_t socket)
{
#if defined(_WIN32)
    closesocket(HandleOf(socket));
#else
    close(HandleOf(socket));
#endif
}

} // namespace

LobbyServer::~LobbyServer()
{
    Stop();
}

bool LobbyServer::Start(uint16_t port, const LobbyDirectory::Settings& settings)
{
    Stop();
    m_directory = LobbyDirectory(settings);
    if (!SocketSystem::Acquire())
    {
        m_message = "Networking could not be started on this machine.";
        return false;
    }
    m_socketSystem = true;

    const SocketHandle handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#if defined(_WIN32)
    const bool opened = handle != INVALID_SOCKET;
#else
    const bool opened = handle >= 0;
#endif
    if (!opened)
    {
        m_message = "Could not open a UDP socket.";
        Stop();
        return false;
    }
    m_socket = static_cast<int64_t>(handle);

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(port);
    if (bind(handle, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
    {
        m_message = "Could not use UDP port " + std::to_string(port) + ": something else has it.";
        Stop();
        return false;
    }

#if defined(_WIN32)
    u_long nonBlocking = 1;
    ioctlsocket(handle, FIONBIO, &nonBlocking);
    // Windows reports "that port was closed" for a datagram this socket sent earlier as an error on
    // the next receive, from whoever it was. A server answering strangers gets those all the time,
    // and each one would end that round of reading early.
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(handle, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);
#else
    const int flags = fcntl(handle, F_GETFL, 0);
    fcntl(handle, F_SETFL, flags | O_NONBLOCK);
#endif

    sockaddr_in bound{};
#if defined(_WIN32)
    int length = sizeof(bound);
#else
    socklen_t length = sizeof(bound);
#endif
    getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &length);
    m_port = ntohs(bound.sin_port);
    m_message.clear();
    return true;
}

void LobbyServer::Stop()
{
    if (m_socket >= 0)
    {
        CloseSocketHandle(m_socket);
        m_socket = -1;
    }
    if (m_socketSystem)
    {
        SocketSystem::Release();
        m_socketSystem = false;
    }
    m_port = 0;
}

bool LobbyServer::Running() const
{
    return m_socket >= 0;
}

void LobbyServer::Poll(double now)
{
    if (m_socket < 0)
    {
        return;
    }
    const SocketHandle handle = HandleOf(m_socket);
    std::array<uint8_t, kLobbyMaxDatagram + 1> buffer{};
    std::vector<LobbyDirectory::Reply> replies;

    // Bounded, so a flood cannot keep this loop from ever reaching the expiry below.
    for (int guard = 0; guard < 1024; ++guard)
    {
        sockaddr_in from{};
#if defined(_WIN32)
        int fromLength = sizeof(from);
#else
        socklen_t fromLength = sizeof(from);
#endif
        const int received = recvfrom(handle, reinterpret_cast<char*>(buffer.data()),
                                      static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&from),
                                      &fromLength);
        if (received <= 0)
        {
            break;
        }
        LobbyEndpoint sender;
        sender.address = ntohl(from.sin_addr.s_addr);
        sender.port = ntohs(from.sin_port);
        replies.clear();
        m_directory.Handle(buffer.data(), static_cast<size_t>(received), sender, now, replies);
        for (const LobbyDirectory::Reply& reply : replies)
        {
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_addr.s_addr = htonl(reply.to.address);
            to.sin_port = htons(reply.to.port);
            sendto(handle, reinterpret_cast<const char*>(reply.bytes.data()), static_cast<int>(reply.bytes.size()),
                   0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
        }
    }

    if (now - m_lastExpire >= 1.0)
    {
        m_lastExpire = now;
        m_directory.Expire(now);
    }
}

} // namespace pred
