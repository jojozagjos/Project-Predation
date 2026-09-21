#pragma once

#include "Engine/Net/RelayProtocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pred
{

// The relay, as a function.
//
// Datagrams in, datagrams out, and no socket anywhere. The same discipline as the audio mixer and
// for the same reason: a relay is a thing whose failures are timing and bookkeeping, and both of
// those can be tested exactly if nothing in here waits on a network. The executable around it is a
// socket, a clock and a loop.
//
// A client is known by whatever the socket says its address is, passed through as an opaque key.
// The relay never parses it and never tells anybody else what it is: a client learns slot numbers
// and nothing more, so joining a lobby does not hand out everybody's address.
class RelayServer
{
public:
    struct Settings
    {
        // Bounds on what strangers can make this allocate. An open UDP port is reachable by
        // everybody, so every one of these is a limit on what a stranger costs rather than on what
        // a game needs: a full game is four, and nobody legitimately opens two hundred lobbies.
        size_t maxLobbies = 256;
        size_t maxClients = 2048;
        // How long without a word before a client is dropped. Clients send a keep-alive several
        // times a second, so this is generous.
        float timeoutSeconds = 20.0f;
        // How many messages one address may send per second before the rest are dropped. A relayed
        // game is about a hundred and fifty datagrams a second per player at most.
        float messagesPerSecond = 600.0f;
        // Where the codes come from. Fixed so a test gets the same lobby code every run.
        uint32_t seed = 0x9E3779B9u;
    };

    // One datagram to send, addressed by the key the caller gave for that client.
    struct Outgoing
    {
        std::string to;
        std::vector<uint8_t> datagram;
    };

    // Something worth telling the operator about.
    //
    // The relay has no logger in it, for the same reason it has no socket: it is a function, and a
    // function that writes to a log is one a test has to read a log to check. So it says what
    // happened and the executable around it decides whether anybody is listening.
    //
    // This exists because the relay was unreadable in the one situation it matters. Its only output
    // was a line printed when the number of lobbies changed, and a second player joining an existing
    // lobby does not change that number -- so the one event anybody ever needs to see was the one
    // event that printed nothing. "Did my friend reach the relay at all" could not be answered from
    // the relay's own console.
    struct Note
    {
        enum class Kind : uint8_t
        {
            Opened,   // a lobby was opened
            Joined,   // somebody joined one
            Left,     // somebody left or was dropped
            Closed,   // a lobby ended
            Refused,  // somebody asked for something and was told no
            Ignored   // a datagram arrived that is not ours at all
        };
        Kind kind = Kind::Ignored;
        std::string client;
        uint32_t code = 0;
        uint8_t slot = kRelayNoSlot;
        RelayRejection reason = RelayRejection::None;
        bool timedOut = false; // Left: dropped for silence rather than having said goodbye
    };

    // Drained by whoever is logging. Left to grow otherwise, so it is bounded: past this many the
    // oldest are dropped rather than the relay quietly eating memory because nobody is reading.
    std::vector<Note>& Notes() { return m_notes; }
    static constexpr size_t kMaxNotes = 256;

    explicit RelayServer(const Settings& settings = {});

    // A datagram has arrived from `from`. Anything to send goes on `out`, appended rather than
    // replacing, so a caller can drain a whole socket before sending.
    void Receive(const std::string& from, const uint8_t* data, size_t bytes,
                 std::vector<Outgoing>& out);
    // Time passes: drop whoever has gone quiet and tell the rest of their lobby.
    void Tick(float dt, std::vector<Outgoing>& out);

    size_t LobbyCount() const { return m_lobbies.size(); }
    size_t ClientCount() const;
    // The code of the lobby a client is in, for tests and for the operator's log. Zero when none.
    uint32_t CodeOf(const std::string& client) const;

private:
    struct Member
    {
        std::string key;
        uint8_t slot = kRelayNoSlot;
        float silentFor = 0.0f;
        // A leaky bucket of allowance. Refilled at messagesPerSecond and spent one per message, so
        // a burst is fine and a flood is not.
        float allowance = 0.0f;
    };

    struct Lobby
    {
        uint32_t code = 0;
        // What the host called it, for the browser. Cosmetic and untrusted: it is whatever a
        // stranger typed, so it is clamped and stripped of anything unprintable on the way in.
        std::string name;
        // Whether they have gone in. A lobby somebody is already playing in can still be joined --
        // the game allows it -- but a browser that does not say so is hiding the one thing anybody
        // wants to know before clicking.
        bool started = false;
        std::vector<Member> members;
    };

    Lobby* FindLobby(uint32_t code);
    Lobby* LobbyOf(const std::string& client, Member** outMember);
    uint32_t NextCode();
    void Send(std::vector<Outgoing>& out, const std::string& to, const RelayPacket& packet) const;
    void Reject(std::vector<Outgoing>& out, const std::string& to, RelayRejection reason) const;
    void RemoveMember(Lobby& lobby, uint8_t slot, std::vector<Outgoing>& out, bool timedOut);
    void AddNote(Note::Kind kind, const std::string& client, uint32_t code, uint8_t slot,
               RelayRejection reason = RelayRejection::None, bool timedOut = false);

    Settings m_settings;
    std::vector<Lobby> m_lobbies;
    uint32_t m_random = 0;
    std::vector<Note> m_notes;
};

} // namespace pred
