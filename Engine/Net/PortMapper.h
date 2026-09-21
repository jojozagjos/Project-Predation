#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace pred
{

// Asks the router to forward a port, so people outside the house can reach this machine.
//
// Hosting on a home connection works on the local network and nowhere else, because the router has
// one public address and no idea which machine behind it a stranger's packet is for. The answer
// everybody uses is UPnP: routers advertise themselves on the local network and accept a request to
// forward a port to whoever asked. It is the same thing as setting up port forwarding by hand, done
// by the game instead of by a person reading their router's manual.
//
// It is best-effort and says so. Routers with UPnP switched off, networks with more than one router
// between here and the internet, and connections where the provider itself does the translating are
// all real and none of them can be fixed from in here; the menu says what happened and falls back
// to telling the host their local address, which is what it did before.
//
// Everything runs on a worker thread. Finding a router means waiting a second or two for it to
// answer, and a menu that freezes while a button is pressed reads as a game that has crashed.
class PortMapper
{
public:
    enum class State : uint8_t
    {
        Idle,     // nothing asked for
        Working,  // looking for a router, or asking it
        Open,     // the port is forwarded, and ExternalAddress() says where to
        Failed    // it is not, and Message() says why
    };

    PortMapper() = default;
    ~PortMapper();
    PortMapper(const PortMapper&) = delete;
    PortMapper& operator=(const PortMapper&) = delete;

    // Starts asking. Returns at once; watch Status().
    void Open(uint16_t port);
    // Takes the mapping down again. Routers keep them for a long time otherwise, and a forwarded
    // port pointing at a machine that is no longer listening is worse than none.
    void Close();

    State Status() const { return m_state.load(); }
    // The address to give other people, once it is known. Empty until then.
    std::string ExternalAddress() const;
    // One line about what happened, for the menu to show.
    std::string Message() const;

private:
    void Run(uint16_t port);

    std::thread m_worker;
    std::atomic<State> m_state{State::Idle};
    std::atomic<bool> m_cancel{false};

    mutable std::mutex m_textMutex;
    std::string m_external;
    std::string m_message;

    // Kept so the mapping can be taken down: the router is addressed by the same control URL that
    // set it up, and the port is what identifies the mapping.
    std::string m_controlUrl;
    std::string m_serviceType;
    uint16_t m_port = 0;
    std::atomic<bool> m_mapped{false};
};

// The text handling, exposed so it can be checked without a router in the room.
//
// Discovery and the SOAP calls need a real gateway and cannot be tested here; what can be, and what
// is most likely to be wrong against somebody's firmware, is reading an address apart and finding
// the right service in a device description. Those are pure functions of a string.
bool ParseHttpUrlForTesting(const std::string& text, std::string& outHost, uint16_t& outPort,
                            std::string& outPath);
bool FindConnectionServiceForTesting(const std::string& description, const std::string& host,
                                     uint16_t port, std::string& outUrl, std::string& outType);

} // namespace pred
