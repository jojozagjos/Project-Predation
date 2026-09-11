#include "Engine/Net/PortMapper.h"

#include "Engine/Net/Transport.h"

#include "Engine/Core/Log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
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
    DWORD timeout = 1500;
    setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout{1, 500000};
    setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif

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

    for (const std::string& from : interfaces)
    {
        if (!from.empty())
        {
            in_addr adapter{};
            if (inet_pton(AF_INET, from.c_str(), &adapter) != 1)
            {
                continue;
            }
            setsockopt(handle, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&adapter),
                       sizeof(adapter));
        }

        // Both the gateway device and the connection service are asked for, because firmwares
        // differ about which of the two they advertise and answering either is enough to go on.
        for (const char* wanted : {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                                   "urn:schemas-upnp-org:service:WANIPConnection:1"})
        {
            const std::string search = std::string("M-SEARCH * HTTP/1.1\r\n"
                                                   "HOST: 239.255.255.250:1900\r\n"
                                                   "MAN: \"ssdp:discover\"\r\n"
                                                   "MX: 1\r\n"
                                                   "ST: ") +
                                       wanted + "\r\n\r\n";
            sendto(handle, search.data(), static_cast<int>(search.size()), 0,
                   reinterpret_cast<const sockaddr*>(&target), sizeof(target));
        }
    }

    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1600);
    char buffer[2048];
    while (std::chrono::steady_clock::now() < until)
    {
        const int read = recv(handle, buffer, static_cast<int>(sizeof(buffer)) - 1, 0);
        if (read <= 0)
        {
            continue;
        }
        buffer[read] = '\0';
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
        if (!location.empty() &&
            std::find(locations.begin(), locations.end(), location) == locations.end())
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

            outType = type;
            outUrl = control.rfind("http://", 0) == 0
                         ? control
                         : "http://" + base.host + ":" + std::to_string(base.port) +
                               (control.front() == '/' ? control : "/" + control);
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
        fail("No router on this network offered to forward a port. Forward UDP " +
             std::to_string(port) + " by hand, or play on one network.");
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

        // Asked for with no lease, because a lease that expires mid-game closes the door on
        // everybody who is already through it. It is taken down on the way out instead.
        const std::string arguments =
            "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) +
            "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" + std::to_string(port) +
            "</NewInternalPort><NewInternalClient>" + internal +
            "</NewInternalClient><NewEnabled>1</NewEnabled>"
            "<NewPortMappingDescription>Project Predation</NewPortMappingDescription>"
            "<NewLeaseDuration>0</NewLeaseDuration>";
        std::string reply;
        if (!SoapAction(controlUrl, serviceType, "AddPortMapping", arguments, reply))
        {
            continue;
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

        {
            std::lock_guard lock(m_textMutex);
            m_external = external;
            m_message = external.empty()
                            ? "The router is forwarding the port, but would not say what this "
                              "connection's address is."
                            : std::string();
        }
        m_state.store(State::Open);
        PRED_LOG_INFO(Network, "Router is forwarding UDP {} to {} (outside address {})", port, internal,
                      external.empty() ? "unknown" : external);
        return;
    }

    fail("A router answered but would not forward the port. It may have UPnP switched off. Forward "
         "UDP " +
         std::to_string(port) + " by hand, or play on one network.");
}

} // namespace pred
