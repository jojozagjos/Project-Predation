#include "Engine/Net/SocketSystem.h"

#if defined(_WIN32)
#    include <winsock2.h>
#endif

namespace pred
{
namespace
{
// Not atomic. Every caller is on the game thread or the relay's own loop, and a socket being opened
// from two threads at once is not a thing this engine does; making it atomic would imply otherwise.
int g_users = 0;
} // namespace

bool SocketSystem::Acquire()
{
#if defined(_WIN32)
    if (g_users++ == 0)
    {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        {
            --g_users;
            return false;
        }
    }
#else
    ++g_users;
#endif
    return true;
}

void SocketSystem::Release()
{
    if (g_users > 0 && --g_users == 0)
    {
#if defined(_WIN32)
        WSACleanup();
#endif
    }
}

} // namespace pred
