#include "Engine/Net/PortMapper.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace pred;

TEST_CASE("A router's description is read for the service that forwards a port", "[net][upnp]")
{
    // Hosting on a home connection reaches the local network and nothing else, because the router
    // has one public address and no idea which machine behind it a stranger's packet is for. Asking
    // it to forward the port is what fixes that, and reading its description is the part most likely
    // to be wrong against somebody's firmware. The rest needs a router in the room.
    SECTION("an address comes apart into a host, a port and a path")
    {
        std::string host;
        uint16_t port = 0;
        std::string path;

        REQUIRE(ParseHttpUrlForTesting("http://192.168.1.1:5000/rootDesc.xml", host, port, path));
        CHECK(host == "192.168.1.1");
        CHECK(port == 5000);
        CHECK(path == "/rootDesc.xml");

        // No port means the web's own, and no path means the root.
        REQUIRE(ParseHttpUrlForTesting("http://192.168.0.1", host, port, path));
        CHECK(host == "192.168.0.1");
        CHECK(port == 80);
        CHECK(path == "/");

        // Anything that is not an address is refused rather than half read.
        CHECK_FALSE(ParseHttpUrlForTesting("https://192.168.1.1/x", host, port, path));
        CHECK_FALSE(ParseHttpUrlForTesting("192.168.1.1:80", host, port, path));
        CHECK_FALSE(ParseHttpUrlForTesting("http://:80/x", host, port, path));
    }

    SECTION("the connection service is found among the others")
    {
        // Cut down from what a real gateway serves. The layer service comes first on purpose: the
        // one that forwards ports is never the only one on the device, and taking the first control
        // URL in the document sends every request to the wrong place.
        const std::string description =
            "<root><device><deviceType>InternetGatewayDevice:1</deviceType><serviceList>"
            "<service><serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>"
            "<controlURL>/ctl/L3F</controlURL></service>"
            "</serviceList><deviceList><device><serviceList>"
            "<service><serviceType>urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1"
            "</serviceType><controlURL>/ctl/CommonIfCfg</controlURL></service>"
            "<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
            "<controlURL>/ctl/IPConn</controlURL></service>"
            "</serviceList></device></deviceList></device></root>";

        std::string url;
        std::string type;
        REQUIRE(FindConnectionServiceForTesting(description, "192.168.1.1", 5000, url, type));
        CHECK(type == "urn:schemas-upnp-org:service:WANIPConnection:1");
        // A relative control URL is made absolute against the address the description came from.
        CHECK(url == "http://192.168.1.1:5000/ctl/IPConn");
    }

    SECTION("a dial-up style gateway is understood too")
    {
        const std::string description =
            "<root><device><serviceList>"
            "<service><serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType>"
            "<controlURL>http://10.0.0.1:80/upnp/control/WANPPPConn1</controlURL></service>"
            "</serviceList></device></root>";

        std::string url;
        std::string type;
        REQUIRE(FindConnectionServiceForTesting(description, "10.0.0.1", 80, url, type));
        CHECK(type == "urn:schemas-upnp-org:service:WANPPPConnection:1");
        // An absolute one is left alone.
        CHECK(url == "http://10.0.0.1:80/upnp/control/WANPPPConn1");
    }

    SECTION("a device with nothing that forwards ports is refused")
    {
        const std::string description =
            "<root><device><serviceList>"
            "<service><serviceType>urn:schemas-upnp-org:service:Printer:1</serviceType>"
            "<controlURL>/ctl/print</controlURL></service>"
            "</serviceList></device></root>";

        std::string url;
        std::string type;
        CHECK_FALSE(FindConnectionServiceForTesting(description, "10.0.0.1", 80, url, type));
    }
}

TEST_CASE("A device cannot send the port mapper somewhere else", "[net][upnp][security]")
{
    // A device description is a document fetched off the network, so every address in it was
    // written by whatever answered the search. An absolute control URL naming a different host
    // would have the game post SOAP to wherever that device fancied, which is a machine on this
    // network being used to reach a machine somewhere else.
    const std::string description =
        "<root><device><serviceList>"
        "<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
        "<controlURL>http://198.51.100.7:8080/ctl/IPConn</controlURL></service>"
        "</serviceList></device></root>";

    std::string url;
    std::string type;
    CHECK_FALSE(FindConnectionServiceForTesting(description, "192.168.1.1", 5000, url, type));

    // The same document served by the host it names is fine, because then it is that host talking
    // about itself.
    CHECK(FindConnectionServiceForTesting(description, "198.51.100.7", 8080, url, type));
    CHECK(url == "http://198.51.100.7:8080/ctl/IPConn");
}


#include <chrono>
#include <thread>

TEST_CASE("Ask this network's router to open a port, for real", "[.][upnp-live]")
{
    // Hidden, because it talks to whatever router is in the room and takes a few seconds. Run it by
    // name to find out whether hosting over the internet will open itself on this network:
    //   PredationTests.exe "[upnp-live]"
    PortMapper mapper;
    mapper.Open(27015);
    for (int i = 0; i < 150 && mapper.Status() == PortMapper::State::Working; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const char* state = mapper.Status() == PortMapper::State::Open     ? "OPEN"
                        : mapper.Status() == PortMapper::State::Failed ? "FAILED"
                                                                        : "STILL WORKING";
    WARN("router: " << state << "\noutside address: " << mapper.ExternalAddress()
                    << "\nsaid: " << mapper.Message());
    mapper.Close();
}
