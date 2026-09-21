#include "Engine/Net/PortMapper.h"

#include "Engine/Net/Transport.h"

#include "Engine/Core/Log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#    include <winsock2.h>
#    include <ws2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#    include <arpa/inet.h>
#    include <netdb.h>
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

// How long a mapping is asked for, in seconds. Renewed at well under this while the game is up, so
// the only thing the number decides is how long a hole stays open after a crash.
constexpr int kLeaseSeconds = 3600;

void CloseSocketHandle(SocketHandle handle)
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
}

// Winsock has to be started before any of this and stopped after. It is reference counted by the
// system, so starting it again while the transport already has it open costs nothing.
struct Sockets
{
    Sockets()
    {
#if defined(_WIN32)
        WSADATA data{};
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
    }
    ~Sockets()
    {
#if defined(_WIN32)
        if (ok)
        {
            WSACleanup();
        }
#endif
    }
    bool ok = true;
};

std::string Lowercase(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// The text between <tag> and </tag>, or empty. Enough for a router's device description: these are
// small documents written by the same handful of firmwares, and a real parser would be a dependency
// bought for one file.
std::string Tag(const std::string& document, const std::string& tag, size_t from = 0)
{
    const std::string open = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    const size_t start = document.find(open, from);
    if (start == std::string::npos)
    {
        return {};
    }
    const size_t end = document.find(close, start + open.size());
    if (end == std::string::npos)
    {
        return {};
    }
    return document.substr(start + open.size(), end - start - open.size());
}

struct Url
{
    std::string host;
    uint16_t port = 80;
    std::string path;
};

// Whether a piece of text is a dotted IPv4 address and nothing else.
bool IsIPv4(const std::string& text)
{
    in_addr address{};
    return !text.empty() && inet_pton(AF_INET, text.c_str(), &address) == 1;
}

// And whether that address is one of the ranges that only exist inside a building.
//
// A router is on the local network by definition. Everything this file does afterwards is driven by
// an address that arrived in a datagram from whatever felt like answering, so this is the line
// between "ask the router on this network" and "connect to wherever a stranger on this network
// said to". It is not a strong boundary and it is not meant to be one; it is the boundary that
// matches what the feature is for.
bool IsPrivateAddress(const std::string& text)
{
    in_addr address{};
    if (inet_pton(AF_INET, text.c_str(), &address) != 1)
    {
        return false;
    }
    const uint32_t host = ntohl(address.s_addr);
    const uint8_t top = static_cast<uint8_t>(host >> 24);
    const uint8_t second = static_cast<uint8_t>((host >> 16) & 0xFF);
    if (top == 10 || top == 127)
    {
        return true;
    }
    if (top == 172 && second >= 16 && second <= 31)
    {
        return true;
    }
    if (top == 192 && second == 168)
    {
        return true;
    }
    return top == 169 && second == 254; // link-local, for a network with no server handing addresses out
}

// And whether it is one of the addresses a provider hands out to its own customers behind its own
// translation. A connection with one of these has no address of its own from outside, and no
// amount of asking the router in the house will make one.
bool IsCarrierAddress(const std::string& text)
{
    in_addr address{};
    if (inet_pton(AF_INET, text.c_str(), &address) != 1)
    {
        return false;
    }
    const uint32_t host = ntohl(address.s_addr);
    // 100.64.0.0 to 100.127.255.255, the range set aside for exactly this.
    return (host >> 22) == (0x64400000u >> 22);
}

// This machine, as the router's own settings page would want it written. Named in the message that
// tells somebody to forward a port by hand, because the next question after "forward a port" is
// always "to what", and the answer is on this machine and nowhere they would think to look.
std::string HereOnThisNetwork()
{
    for (const std::string& address : LocalNetworkAddresses())
    {
        if (IsPrivateAddress(address))
        {
            return address;
        }
    }
    return "this machine";
}

bool ParseUrl(const std::string& text, Url& out)
{
    constexpr const char* kPrefix = "http://";
    if (text.rfind(kPrefix, 0) != 0)
    {
        return false;
    }
    const size_t hostStart = std::strlen(kPrefix);
    const size_t pathStart = text.find('/', hostStart);
    const std::string authority =
        text.substr(hostStart, pathStart == std::string::npos ? std::string::npos : pathStart - hostStart);
    out.path = pathStart == std::string::npos ? "/" : text.substr(pathStart);

    const size_t colon = authority.find(':');
    if (colon == std::string::npos)
    {
        out.host = authority;
        out.port = 80;
    }
    else
    {
        out.host = authority.substr(0, colon);
        const int port = std::atoi(authority.c_str() + colon + 1);
        if (port <= 0 || port > 65535)
        {
            return false;
        }
        out.port = static_cast<uint16_t>(port);
    }
    return !out.host.empty();
}

// One request, one response, connection closed. Routers are not web servers and this is not a web
// client: three requests are made in a session and none of them is worth keeping a socket for.
bool HttpRequest(const Url& url, const std::string& request, std::string& outBody)
{
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    if (getaddrinfo(url.host.c_str(), std::to_string(url.port).c_str(), &hints, &results) != 0 ||
        results == nullptr)
    {
        return false;
    }

    const SocketHandle handle = socket(results->ai_family, results->ai_socktype, results->ai_protocol);
    if (handle == kInvalidSocket)
    {
        freeaddrinfo(results);
        return false;
    }

    // A router that is not going to answer must not hold the menu open for the system's default
    // timeout, which on Windows is most of a minute.
#if defined(_WIN32)
    DWORD timeout = 3000;
    setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout{3, 0};
    setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif

    if (connect(handle, results->ai_addr, static_cast<int>(results->ai_addrlen)) != 0)
    {
        freeaddrinfo(results);
        CloseSocketHandle(handle);
        return false;
    }
    freeaddrinfo(results);

    size_t sent = 0;
    while (sent < request.size())
    {
        const int wrote = send(handle, request.data() + sent, static_cast<int>(request.size() - sent), 0);
        if (wrote <= 0)
        {
            CloseSocketHandle(handle);
            return false;
        }
        sent += static_cast<size_t>(wrote);
    }

    std::string response;
    char buffer[2048];
    for (;;)
    {
        const int read = recv(handle, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (read <= 0)
        {
            break;
        }
        response.append(buffer, static_cast<size_t>(read));
        // Router descriptions are a few kilobytes. Anything much larger is not one.
        if (response.size() > 128 * 1024)
        {
            break;
        }
    }
    CloseSocketHandle(handle);

    const size_t bodyStart = response.find("\r\n\r\n");
    outBody = bodyStart == std::string::npos ? response : response.substr(bodyStart + 4);
    return response.find(" 200 ") != std::string::npos;
}

// Shouts on the local network and collects whatever routers answer. UPnP devices listen on a
// multicast address for this, so it finds them without being told where any of them are.
std::vector<std::string> DiscoverGatewayDescriptions()
{
    std::vector<std::string> locations;

    const SocketHandle handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == kInvalidSocket)
    {
        return locations;
    }

#if defined(_WIN32)
    DWORD timeout = 300;
    setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout{0, 300000};
    setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif

    // Bound before anything is sent, and to every address rather than to whichever one the system
    // would have picked. A socket that has only ever sent is bound implicitly to the adapter it
    // sent from, and the reply to a multicast search comes back as a plain datagram from the
    // router's own address: on a machine with more than one adapter that reply can arrive at a
    // different one and be dropped before this ever sees it.
    {
        sockaddr_in any{};
        any.sin_family = AF_INET;
        any.sin_addr.s_addr = INADDR_ANY;
        any.sin_port = 0;
        bind(handle, reinterpret_cast<const sockaddr*>(&any), sizeof(any));
    }

    // Two hops rather than one. The default for a multicast datagram is to die on the wire it was
    // sent on, which is right for the router in the house and wrong the moment there is a switch or
    // an access point that routes between two segments of the same home network.
    {
        const int ttl = 2;
        setsockopt(handle, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl),
                   sizeof(ttl));
    }

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &target.sin_addr);

    // Out of every adapter, not only whichever one the system would have chosen.
    //
    // A multicast datagram goes out of one interface, and on a machine with a virtual adapter or
    // two the default is often not the one the router is on: the search then reaches nothing and
    // the answer is "no router here" on a network that has one. Sending from each address in turn
    // costs three datagrams and removes the guess.
    std::vector<std::string> interfaces = LocalNetworkAddresses();
    interfaces.emplace_back(); // and once more however the system would have done it

    // What to ask for.
    //
    // Firmwares disagree about what they advertise themselves as, and a search only gets an answer
    // from a device that recognises the exact string. Two was not enough: some routers announce
    // only the gateway device, some only one of the two connection services, and a stubborn few
    // answer nothing but the generic ones. Asking for all five costs five datagrams.
    static const char* const kWanted[] = {
        "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
        "urn:schemas-upnp-org:service:WANIPConnection:1",
        "urn:schemas-upnp-org:service:WANPPPConnection:1",
        "upnp:rootdevice",
        "ssdp:all",
    };

    const auto search = [&]()
    {
        for (const std::string& from : interfaces)
        {
            if (!from.empty())
            {
                in_addr adapter{};
                if (inet_pton(AF_INET, from.c_str(), &adapter) != 1)
                {
                    continue;
                }
                setsockopt(handle, IPPROTO_IP, IP_MULTICAST_IF,
                           reinterpret_cast<const char*>(&adapter), sizeof(adapter));
            }

            for (const char* wanted : kWanted)
            {
                // Two seconds of spread rather than one. A device waits a random time up to this
                // before answering, so that a hundred of them do not answer at once; asking for one
                // second and then listening for a second and a half is a race with the standard.
                const std::string message = std::string("M-SEARCH * HTTP/1.1\r\n"
                                                        "HOST: 239.255.255.250:1900\r\n"
                                                        "MAN: \"ssdp:discover\"\r\n"
                                                        "MX: 2\r\n"
                                                        "ST: ") +
                                            wanted + "\r\n\r\n";
                sendto(handle, message.data(), static_cast<int>(message.size()), 0,
                       reinterpret_cast<const sockaddr*>(&target), sizeof(target));
            }
        }
    };
    search();

    // Asked twice, a second apart. A search is a UDP datagram to a multicast address and there is
    // nothing at all making it arrive; one lost datagram used to be the whole answer.
    auto askAgain = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(3500);
    char buffer[2048];
    while (std::chrono::steady_clock::now() < until)
    {
        if (std::chrono::steady_clock::now() >= askAgain)
        {
            askAgain = until; // once more, and only once
            search();
        }
        sockaddr_in from{};
#if defined(_WIN32)
        int fromLength = static_cast<int>(sizeof(from));
#else
        socklen_t fromLength = sizeof(from);
#endif
        const int read = recvfrom(handle, buffer, static_cast<int>(sizeof(buffer)) - 1, 0,
                                  reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (read <= 0)
        {
            continue;
        }
        buffer[read] = '\0';
        // Who answered. Everything after this is a request made to an address that came out of this
        // datagram, so the address has to be the one that sent it: anything on the network can
        // answer a search, and without this check anything on the network could name any host and
        // port in the world and have the game go and talk to it.
        char sender[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &from.sin_addr, sender, sizeof(sender)) == nullptr)
        {
            continue;
        }
        const std::string reply(buffer, static_cast<size_t>(read));
        const std::string lower = Lowercase(reply);
        const size_t at = lower.find("location:");
        if (at == std::string::npos)
        {
            continue;
        }
        size_t start = at + std::strlen("location:");
        while (start < reply.size() && (reply[start] == ' ' || reply[start] == '\t'))
        {
            ++start;
        }
        const size_t end = reply.find_first_of("\r\n", start);
        std::string location = reply.substr(start, end == std::string::npos ? std::string::npos : end - start);
        Url parsed;
        if (!ParseUrl(location, parsed))
        {
            continue;
        }
        // The description has to live on the device that answered, and that device has to be on
        // this network. Both together mean the only thing a search can make this machine do is talk
        // to a machine that was already talking to it.
        if (parsed.host != sender || !IsPrivateAddress(parsed.host))
        {
            PRED_LOG_WARN(Network, "Ignoring a search reply from {} pointing at {}", sender,
                          parsed.host);
            continue;
        }
        if (std::find(locations.begin(), locations.end(), location) == locations.end())
        {
            locations.push_back(std::move(location));
        }
    }

    CloseSocketHandle(handle);
    return locations;
}

// The control URL of whichever service on this device can forward a port, and what that service
// calls itself. Both are needed: a SOAP request names the service type in its action.
bool FindConnectionService(const std::string& description, const Url& base, std::string& outUrl,
                           std::string& outType)
{
    for (const char* wanted : {"WANIPConnection", "WANPPPConnection"})
    {
        size_t at = 0;
        while ((at = description.find(wanted, at)) != std::string::npos)
        {
            // Back to the start of the service this type belongs to, then forward for its control
            // URL. Walking the document this way beats a real tree for a file with four services in
            // it, and the layout is fixed by the specification.
            const size_t serviceStart = description.rfind("<service>", at);
            const size_t serviceEnd = description.find("</service>", at);
            if (serviceStart == std::string::npos || serviceEnd == std::string::npos)
            {
                at += std::strlen(wanted);
                continue;
            }
            const std::string service = description.substr(serviceStart, serviceEnd - serviceStart);
            const std::string control = Tag(service, "controlURL");
            const std::string type = Tag(service, "serviceType");
            if (control.empty() || type.empty())
            {
                at += std::strlen(wanted);
                continue;
            }

            // An absolute control URL has to stay on the device whose description this is. A
            // description is a document fetched over the network, so a device that wanted to could
            // otherwise name any host in the world here and have the game post to it.
            if (control.rfind("http://", 0) == 0)
            {
                Url absolute;
                if (!ParseUrl(control, absolute) || absolute.host != base.host)
                {
                    at += std::strlen(wanted);
                    continue;
                }
                outUrl = control;
            }
            else
            {
                outUrl = "http://" + base.host + ":" + std::to_string(base.port) +
                         (control.front() == '/' ? control : "/" + control);
            }
            outType = type;
            return true;
        }
    }
    return false;
}

bool SoapAction(const std::string& controlUrl, const std::string& serviceType, const std::string& action,
                const std::string& arguments, std::string& outBody)
{
    Url url;
    if (!ParseUrl(controlUrl, url))
    {
        return false;
    }

    const std::string body = "<?xml version=\"1.0\"?>"
                             "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
                             "<s:Body><u:" +
                             action + " xmlns:u=\"" + serviceType + "\">" + arguments + "</u:" + action +
                             "></s:Body></s:Envelope>";

    const std::string request = "POST " + url.path + " HTTP/1.1\r\n" + "HOST: " + url.host + ":" +
                                std::to_string(url.port) + "\r\n" +
                                "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n" + "SOAPACTION: \"" +
                                serviceType + "#" + action + "\"\r\n" +
                                "CONTENT-LENGTH: " + std::to_string(body.size()) + "\r\n" +
                                "CONNECTION: close\r\n\r\n" + body;
    return HttpRequest(url, request, outBody);
}

// This machine's address on the network the router is on. The mapping has to name it, and the
// address that reaches the router is the one to use: a machine with several adapters has several.
std::string AddressFacing(const std::string& host)
{
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* results = nullptr;
    if (getaddrinfo(host.c_str(), "80", &hints, &results) != 0 || results == nullptr)
    {
        return {};
    }

    std::string found;
    const SocketHandle handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle != kInvalidSocket)
    {
        // Connecting a datagram socket sends nothing; it just asks the system which adapter it
        // would use, which is exactly the question.
        if (connect(handle, results->ai_addr, static_cast<int>(results->ai_addrlen)) == 0)
        {
            sockaddr_in local{};
            socklen_t length = sizeof(local);
            if (getsockname(handle, reinterpret_cast<sockaddr*>(&local), &length) == 0)
            {
                char text[INET_ADDRSTRLEN] = {};
                if (inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text)) != nullptr)
                {
                    found = text;
                }
            }
        }
        CloseSocketHandle(handle);
    }
    freeaddrinfo(results);
    return found;
}

} // namespace

bool ParseHttpUrlForTesting(const std::string& text, std::string& outHost, uint16_t& outPort,
                            std::string& outPath)
{
    Url url;
    if (!ParseUrl(text, url))
    {
        return false;
    }
    outHost = url.host;
    outPort = url.port;
    outPath = url.path;
    return true;
}

bool FindConnectionServiceForTesting(const std::string& description, const std::string& host,
                                     uint16_t port, std::string& outUrl, std::string& outType)
{
    Url base;
    base.host = host;
    base.port = port;
    return FindConnectionService(description, base, outUrl, outType);
}

PortMapper::~PortMapper()
{
    Close();
}

void PortMapper::Open(uint16_t port)
{
    Close();
    m_cancel.store(false);
    m_state.store(State::Working);
    {
        std::lock_guard lock(m_textMutex);
        m_external.clear();
        m_message = "Asking the router to let people in...";
    }
    m_port = port;
    m_worker = std::thread([this, port] { Run(port); });
}

void PortMapper::Close()
{
    m_cancel.store(true);
    if (m_worker.joinable())
    {
        m_worker.join();
    }

    if (m_mapped.load() && !m_controlUrl.empty())
    {
        const std::string arguments = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" +
                                      std::to_string(m_port) +
                                      "</NewExternalPort><NewProtocol>UDP</NewProtocol>";
        std::string reply;
        SoapAction(m_controlUrl, m_serviceType, "DeletePortMapping", arguments, reply);
        PRED_LOG_INFO(Network, "Asked the router to close port {}", m_port);
    }
    m_mapped.store(false);
    m_controlUrl.clear();
    m_serviceType.clear();
    m_state.store(State::Idle);
    std::lock_guard lock(m_textMutex);
    m_external.clear();
    m_message.clear();
}

std::string PortMapper::ExternalAddress() const
{
    std::lock_guard lock(m_textMutex);
    return m_external;
}

std::string PortMapper::Message() const
{
    std::lock_guard lock(m_textMutex);
    return m_message;
}

void PortMapper::Run(uint16_t port)
{
    Sockets sockets;
    const auto fail = [this](std::string why)
    {
        PRED_LOG_WARN(Network, "Port forwarding: {}", why);
        {
            std::lock_guard lock(m_textMutex);
            m_message = std::move(why);
        }
        m_state.store(State::Failed);
    };

    if (!sockets.ok)
    {
        fail("Could not start networking.");
        return;
    }

    const std::vector<std::string> locations = DiscoverGatewayDescriptions();
    if (m_cancel.load())
    {
        return;
    }
    if (locations.empty())
    {
        fail(std::string("No router here answered. Most routers can do this and ship with it "
                         "switched off: look for UPnP in the router's settings and turn it on. "
                         "Otherwise forward UDP ") +
             std::to_string(port) + " to " + HereOnThisNetwork() +
             " by hand. On a network somebody else runs, neither is likely to be possible, and a "
             "virtual network tool that puts both machines on one network is the way round it.");
        return;
    }

    for (const std::string& location : locations)
    {
        if (m_cancel.load())
        {
            return;
        }

        Url base;
        if (!ParseUrl(location, base))
        {
            continue;
        }
        const std::string request = "GET " + base.path + " HTTP/1.1\r\nHOST: " + base.host + ":" +
                                    std::to_string(base.port) + "\r\nCONNECTION: close\r\n\r\n";
        std::string description;
        if (!HttpRequest(base, request, description))
        {
            continue;
        }

        std::string controlUrl;
        std::string serviceType;
        if (!FindConnectionService(description, base, controlUrl, serviceType))
        {
            continue;
        }

        const std::string internal = AddressFacing(base.host);
        if (internal.empty())
        {
            continue;
        }

        // Asked for with a lease, and renewed while the game is up.
        //
        // A mapping with no lease lasts until something deletes it, and the thing that deletes this
        // one is the game shutting down tidily. A game that crashes, or a machine that loses power,
        // therefore leaves a hole in the router pointing at a port nothing is listening on, and
        // leaves it there for ever. An hour's lease that is renewed every twenty minutes behaves
        // the same way while the game is running and closes itself within the hour if it is not.
        //
        // Some firmwares refuse any lease but zero, which is not a reason to give up on the
        // feature, so that is what the second attempt is for.
        const auto mappingArguments = [&](int leaseSeconds)
        {
            return "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) +
                   "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" +
                   std::to_string(port) + "</NewInternalPort><NewInternalClient>" + internal +
                   "</NewInternalClient><NewEnabled>1</NewEnabled>"
                   "<NewPortMappingDescription>Project Predation</NewPortMappingDescription>"
                   "<NewLeaseDuration>" +
                   std::to_string(leaseSeconds) + "</NewLeaseDuration>";
        };
        std::string reply;
        int lease = kLeaseSeconds;
        if (!SoapAction(controlUrl, serviceType, "AddPortMapping", mappingArguments(lease), reply))
        {
            lease = 0;
            if (!SoapAction(controlUrl, serviceType, "AddPortMapping", mappingArguments(lease), reply))
            {
                continue;
            }
            PRED_LOG_INFO(Network, "Router would not take a lease; the mapping is open ended");
        }

        m_controlUrl = controlUrl;
        m_serviceType = serviceType;
        m_mapped.store(true);

        std::string external;
        std::string addressReply;
        if (SoapAction(controlUrl, serviceType, "GetExternalIPAddress", {}, addressReply))
        {
            external = Tag(addressReply, "NewExternalIPAddress");
        }
        // Checked before it is put on the screen for somebody to copy and send to a friend. It is
        // whatever text the router put between two tags, and an address that is not an address is
        // worse than no address: it reads as the feature having worked.
        std::string trouble;
        if (!IsIPv4(external))
        {
            external.clear();
            trouble = "The router is forwarding the port, but would not say what this connection's "
                      "address is.";
        }
        else if (IsPrivateAddress(external) || IsCarrierAddress(external))
        {
            // The router forwarded the port and the address it is forwarding to is one that only
            // exists inside somebody else's network, which means this router is itself behind
            // another one. Nothing here can open that second door, and handing the player an
            // address that cannot be reached is worse than telling them why: they send it to a
            // friend, the friend cannot connect, and neither of them has anything to go on.
            PRED_LOG_WARN(Network, "Router reports {} as this connection's address, which is not one "
                                   "the outside world can reach",
                          external);
            external.clear();
            trouble = "The router forwarded the port, but this connection is behind a second router "
                      "or your provider's own, so there is no address from outside to give anyone. "
                      "Play on one network, or ask your provider about a public address.";
        }

        {
            std::lock_guard lock(m_textMutex);
            m_external = external;
            m_message = trouble;
        }
        m_state.store(State::Open);
        PRED_LOG_INFO(Network, "Router is forwarding UDP {} to {} (outside address {})", port, internal,
                      external.empty() ? "unknown" : external);

        // Stay alive to renew it. The wait is in short slices so that quitting the game does not
        // wait out a renewal interval before the mapping is taken down.
        if (lease > 0)
        {
            float sinceRenewed = 0.0f;
            while (!m_cancel.load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                sinceRenewed += 0.2f;
                if (sinceRenewed >= static_cast<float>(lease) * 0.4f)
                {
                    sinceRenewed = 0.0f;
                    std::string renewal;
                    SoapAction(controlUrl, serviceType, "AddPortMapping", mappingArguments(lease),
                               renewal);
                }
            }
        }
        return;
    }

    fail(std::string("A router answered but refused to forward the port, which usually means UPnP "
                     "is switched off in its settings. Turn it on, or forward UDP ") +
         std::to_string(port) + " to " + HereOnThisNetwork() +
         " by hand, or play on one network.");
}

} // namespace pred
