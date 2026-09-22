// The lobby server: introduces players to each other so they can connect directly.
//
// Run it on any machine with a public address -- the guide in Docs/SERVER.md sets it up on Oracle
// Cloud's free tier. It hands out lobby codes and tells each player where the other is. The games
// themselves never pass through it, so it uses almost no bandwidth: an open lobby is a few dozen bytes
// every two seconds.
//
// Built on its own, with nothing from the engine but the four lobby files:
//
//   g++ -std=c++20 -O2 -I. Tools/LobbyServer/main.cpp Engine/Net/LobbyServer.cpp \
//       Engine/Net/LobbyDirectory.cpp Engine/Net/LobbyProtocol.cpp Engine/Net/SocketSystem.cpp \
//       -o predation-lobby-server
//
// (run from the root of the repository). Tools/LobbyServer/setup-linux.sh does that and the rest.

#include "Engine/Net/LobbyServer.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <string>
#include <thread>

using namespace pred;

namespace
{

std::atomic<bool> g_running{true};

void OnSignal(int)
{
    g_running = false;
}

// A line with the time in front, flushed at once so a service log shows it as it happens.
void Print(const std::string& line)
{
    const std::time_t now = std::time(nullptr);
    char stamp[32] = {};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::gmtime(&now));
    std::printf("[%s] %s\n", stamp, line.c_str());
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv)
{
    uint16_t port = kLobbyServerPort;
    bool quiet = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if ((argument == "--port" || argument == "-p") && i + 1 < argc)
        {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        }
        else if (argument == "--quiet" || argument == "-q")
        {
            quiet = true;
        }
        else if (argument == "--help" || argument == "-h")
        {
            std::printf("Project Predation lobby server\n"
                        "  --port <n>   UDP port to listen on (default %u)\n"
                        "  --quiet      only print the hourly summary\n"
                        "\n"
                        "Hands out lobby codes and introduces players so they connect to each\n"
                        "other directly. No game traffic passes through it.\n",
                        static_cast<unsigned>(kLobbyServerPort));
            return 0;
        }
    }

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    LobbyDirectory::Settings settings;
    std::random_device entropy;
    settings.seed = (static_cast<uint64_t>(entropy()) << 32) ^ entropy() ^
                    static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());

    LobbyServer server;
    if (!server.Start(port, settings))
    {
        Print("Could not start: " + server.Message());
        return 1;
    }
    if (!quiet)
    {
        server.Directory().log = Print;
    }
    Print("Lobby server listening on UDP port " + std::to_string(server.Port()));

    const auto start = std::chrono::steady_clock::now();
    double lastSummary = 0.0;
    while (g_running)
    {
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        server.Poll(now);
        if (now - lastSummary >= 3600.0)
        {
            lastSummary = now;
            const LobbyDirectory::Stats& stats = server.Directory().GetStats();
            Print("open lobbies " + std::to_string(server.Directory().LobbyCount()) + ", opened " +
                  std::to_string(stats.hosted) + ", introductions " + std::to_string(stats.introductions) +
                  ", messages " + std::to_string(stats.received) + " (" + std::to_string(stats.ignored) +
                  " ignored, " + std::to_string(stats.limited) + " rate limited)");
        }
        // A lobby server has nothing to do between datagrams, and a few milliseconds of waiting is
        // nothing to somebody joining a game. It keeps the machine's processor near idle.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Print("Stopping");
    return 0;
}
