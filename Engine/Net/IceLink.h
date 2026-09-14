#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

typedef struct juice_agent juice_agent_t;

namespace pred
{

// A way through two routers that neither of them has been asked to open.
//
// Hosting across the internet needs somebody's router to forward a port, and on a network somebody
// else runs there is nobody to ask. What works instead is what every peer-to-peer game does: both
// machines ask a public server what their address looks like from outside, they tell each other,
// and then both start sending at the same moment. Each router sees a packet going out first and
// opens a hole for the reply, so a connection nobody was allowed to accept is made by both ends
// dialling at once. That is ICE, and this is libjuice doing it.
//
// What it needs from the outside is the one thing it cannot do itself: the two machines have to
// exchange a short block of text before there is any connection to exchange it over. A game with a
// server of its own would pass that through the server. This one has no server, so the players pass
// it themselves, over whatever they are already talking on. It is two pastes and it costs nothing
// to run.
//
// It is not certain to work. Some networks use a translation that gives a different hole to every
// destination, and against one of those nothing short of a relay helps, which is a machine somebody
// has to pay for. Status() says which happened.
class IceLink
{
public:
    enum class State : uint8_t
    {
        Idle,
        // Asking a public server what this connection looks like from outside.
        Gathering,
        // The code is ready to send. Waiting for theirs.
        Ready,
        // Both codes are in and the two ends are dialling each other.
        Connecting,
        Connected,
        Failed
    };

    struct Settings
    {
        // A public server that answers "what does this connection look like from outside". It is
        // asked one question and told nothing: no game traffic goes near it, and it never learns
        // that the two machines went on to talk to each other.
        std::string stunHost = "stun.l.google.com";
        uint16_t stunPort = 19302;
    };

    IceLink() = default;
    ~IceLink();
    IceLink(const IceLink&) = delete;
    IceLink& operator=(const IceLink&) = delete;

    bool Start(const Settings& settings);
    void Stop();

    State Status() const;
    // One line of text to send to the other player. Empty until the gathering has finished.
    std::string LocalCode() const;
    // Their line. False when it is not one, which is almost always a paste that lost some of itself.
    bool SetRemoteCode(const std::string& code);
    bool HasRemote() const;
    std::string Message() const;

    // --- Datagrams ------------------------------------------------------------------------------
    bool Send(const uint8_t* data, size_t bytes);
    // The oldest datagram waiting, or false when there is none.
    bool Receive(std::vector<uint8_t>& out);

private:
    static void OnState(juice_agent_t* agent, int state, void* user);
    static void OnGatheringDone(juice_agent_t* agent, void* user);
    static void OnReceive(juice_agent_t* agent, const char* data, size_t size, void* user);

    juice_agent_t* m_agent = nullptr;
    mutable std::mutex m_mutex;
    State m_state = State::Idle;
    std::string m_localCode;
    std::string m_message;
    bool m_hasRemote = false;
    // Datagrams arrive on libjuice's own thread and are read on the game's, so they queue here.
    // Bounded: a peer that floods must not be able to grow this without limit.
    std::deque<std::vector<uint8_t>> m_incoming;
};

// The text a code is made of. Exposed so it can be tested without a network: a code that does not
// survive a trip through a chat window is the most likely thing to be wrong here.
std::string EncodeIceCode(const std::string& description);
bool DecodeIceCode(const std::string& code, std::string& outDescription);

} // namespace pred
