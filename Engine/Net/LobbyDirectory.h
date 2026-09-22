#pragma once

#include "Engine/Net/LobbyProtocol.h"

#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace pred
{

// Everything the lobby server knows, and every decision it makes, with no socket anywhere in it.
//
// A datagram and where it came from go in; replies and where they go come out. Time is passed in
// rather than read from a clock. That is what lets a test run a whole lobby -- open, join, a flood
// from one address, a host going quiet -- in a few microseconds and get the same answer every time,
// and it keeps the part that runs on a server nobody is watching as small and as checked as it can be.
class LobbyDirectory
{
public:
    struct Settings
    {
        // Limits on what strangers can make it hold. A server on the internet is reachable by
        // everybody, so each of these is about what a stranger costs rather than what a game needs.
        size_t maxLobbies = 4096;
        // Per outside address: a whole dormitory or office can share one, so this is generous.
        size_t maxLobbiesPerAddress = 16;
        // A host says it is still there every two seconds; after this long without a word the
        // lobby is gone and its code stops working.
        double lobbyTimeoutSeconds = 15.0;
        // Messages one address may send a second, and how many it may save up. Hosting is one every
        // two seconds and joining a handful, so only a flood ever meets these.
        double messagesPerSecond = 20.0;
        double burst = 40.0;
        // Where codes, secrets and tokens come from. Fixed in tests so the first code is always the
        // same; the server seeds it from the operating system.
        uint64_t seed = 0x9E3779B97F4A7C15ull;
    };

    struct Reply
    {
        LobbyEndpoint to;
        std::vector<uint8_t> bytes;
    };

    struct Stats
    {
        uint64_t received = 0;
        uint64_t ignored = 0;  // not ours, or malformed
        uint64_t limited = 0;  // over the rate limit
        uint64_t hosted = 0;   // lobbies opened
        uint64_t introductions = 0;
    };

    // Two constructors rather than a default argument: GCC will not take a default built from a
    // struct declared inside the same class, which the Linux server build found.
    LobbyDirectory();
    explicit LobbyDirectory(const Settings& settings);

    // One datagram, from `from`, at `now` seconds. Anything to send back is appended to `out`.
    void Handle(const uint8_t* data, size_t bytes, const LobbyEndpoint& from, double now,
                std::vector<Reply>& out);
    // Forgets lobbies whose host has gone quiet, and addresses that have. Cheap; call it every second
    // or so.
    void Expire(double now);

    size_t LobbyCount() const { return m_lobbies.size(); }
    const Stats& GetStats() const { return m_stats; }
    // A line for each thing worth knowing about, for the server's console. Optional.
    std::function<void(const std::string&)> log;

private:
    struct Introduction
    {
        LobbyEndpoint guest;
        uint32_t token = 0;
        double at = 0.0;
    };
    struct Lobby
    {
        uint32_t code = 0;
        uint32_t secret = 0;
        LobbyEndpoint host; // where the server sees the host
        std::vector<LobbyEndpoint> inside; // where the host says it is on its own network
        uint16_t version = 0;
        uint8_t players = 1;
        uint8_t maxPlayers = 4;
        bool started = false;
        bool listed = false;
        std::string name;
        double lastHeard = 0.0;
        // Recent introductions, so a guest asking again gets the same token rather than starting over
        // and the host is not told about the same guest ten times.
        std::vector<Introduction> introductions;
    };
    struct Sender
    {
        double tokens = 0.0;
        double lastSeen = 0.0;
    };

    bool Allow(uint32_t address, double now);
    uint32_t NewCode();
    Lobby* FindByHost(const LobbyEndpoint& host);
    void Say(const std::string& line) const;
    void Send(std::vector<Reply>& out, const LobbyEndpoint& to, const LobbyPacket& packet);
    void Reject(std::vector<Reply>& out, const LobbyEndpoint& to, uint32_t code, LobbyRejection reason);

    void OnHost(const LobbyPacket& packet, const LobbyEndpoint& from, double now, std::vector<Reply>& out);
    void OnUpdate(const LobbyPacket& packet, const LobbyEndpoint& from, double now, std::vector<Reply>& out);
    void OnClose(const LobbyPacket& packet, const LobbyEndpoint& from);
    void OnJoin(const LobbyPacket& packet, const LobbyEndpoint& from, double now, std::vector<Reply>& out);
    void OnList(const LobbyPacket& packet, const LobbyEndpoint& from, std::vector<Reply>& out);

    Settings m_settings;
    std::mt19937_64 m_random;
    std::unordered_map<uint32_t, Lobby> m_lobbies; // by code
    std::unordered_map<uint32_t, Sender> m_senders; // by outside address
    Stats m_stats;
};

} // namespace pred
