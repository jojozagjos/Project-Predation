#include "Engine/Net/LobbyProtocol.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pred
{
namespace
{

// 'PLBY'. Different from the game's own and from the network beacon's, so a datagram that reaches
// the wrong one is dropped rather than half understood.
constexpr uint32_t kLobbyMagic = 0x50424C59u;
constexpr int kAlphabetBits = 5;
constexpr size_t kProbeBytes = 9;

constexpr uint32_t kStunCookie = 0x2112A442u;
constexpr uint16_t kStunBindingRequest = 0x0001;
constexpr uint16_t kStunBindingSuccess = 0x0101;
constexpr uint16_t kStunMappedAddress = 0x0001;
constexpr uint16_t kStunXorMappedAddress = 0x0020;

int SymbolIndex(char c)
{
    const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (int i = 0; i < 32; ++i)
    {
        if (kLobbyCodeAlphabet[i] == upper)
        {
            return i;
        }
    }
    return -1;
}

void PutU32(uint8_t* at, uint32_t value)
{
    at[0] = static_cast<uint8_t>(value & 0xFFu);
    at[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    at[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    at[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

uint32_t GetU32(const uint8_t* at)
{
    return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) |
           (static_cast<uint32_t>(at[2]) << 16) | (static_cast<uint32_t>(at[3]) << 24);
}

// STUN is big-endian, unlike everything else of ours.
void PutBig16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
}

void PutBig32(std::vector<uint8_t>& out, uint32_t value)
{
    PutBig16(out, static_cast<uint16_t>(value >> 16));
    PutBig16(out, static_cast<uint16_t>(value & 0xFFFFu));
}

uint16_t GetBig16(const uint8_t* at)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(at[0]) << 8) | at[1]);
}

uint32_t GetBig32(const uint8_t* at)
{
    return (static_cast<uint32_t>(GetBig16(at)) << 16) | GetBig16(at + 2);
}

} // namespace

bool EncodeLobbyCode(const std::string& text, uint32_t& out)
{
    // Whatever somebody pasted, with the spacing and dashes people add to codes taken back out.
    // "k7x-2qm" and "K7X2QM" are the same code.
    std::string cleaned;
    for (const char c : text)
    {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0)
        {
            cleaned.push_back(c);
        }
        else if (c != ' ' && c != '-' && c != '\t' && c != '\r' && c != '\n')
        {
            return false; // a dot or a colon: an address, not a code
        }
    }
    if (static_cast<int>(cleaned.size()) != kLobbyCodeLength)
    {
        return false;
    }
    uint32_t value = 0;
    for (const char c : cleaned)
    {
        const int index = SymbolIndex(c);
        if (index < 0)
        {
            return false;
        }
        value = (value << kAlphabetBits) | static_cast<uint32_t>(index);
    }
    out = value;
    return true;
}

std::string DecodeLobbyCode(uint32_t value)
{
    std::string text(static_cast<size_t>(kLobbyCodeLength), '?');
    for (int i = kLobbyCodeLength - 1; i >= 0; --i)
    {
        text[static_cast<size_t>(i)] = kLobbyCodeAlphabet[value & 0x1Fu];
        value >>= kAlphabetBits;
    }
    return text;
}

std::string LobbyEndpoint::AddressText() const
{
    char text[32];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", (address >> 24) & 0xFFu, (address >> 16) & 0xFFu,
                  (address >> 8) & 0xFFu, address & 0xFFu);
    return text;
}

std::string LobbyEndpoint::ToString() const
{
    return AddressText() + ":" + std::to_string(port);
}

bool LobbyEndpoint::Parse(const std::string& text, uint16_t port, LobbyEndpoint& out)
{
    unsigned a = 0;
    unsigned b = 0;
    unsigned c = 0;
    unsigned d = 0;
    char trailing = 0;
    if (std::sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &trailing) != 4 || a > 255 || b > 255 ||
        c > 255 || d > 255)
    {
        return false;
    }
    out.address = (a << 24) | (b << 16) | (c << 8) | d;
    out.port = port;
    return true;
}

bool LobbyEndpoint::ParseWithPort(const std::string& text, LobbyEndpoint& out)
{
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos)
    {
        return false;
    }
    const int port = std::atoi(text.c_str() + colon + 1);
    if (port <= 0 || port > 65535)
    {
        return false;
    }
    return Parse(text.substr(0, colon), static_cast<uint16_t>(port), out);
}

const char* Describe(LobbyRejection reason)
{
    switch (reason)
    {
    case LobbyRejection::NoSuchLobby:
        return "There is no game with that code. Check it, or ask them for it again: a code stops "
               "working when its game closes.";
    case LobbyRejection::LobbyFull:
        return "That game is full.";
    case LobbyRejection::WrongVersion:
        return "That game is running a different version. Both of you need the same one.";
    case LobbyRejection::ServerFull:
        return "The lobby server is busy. Try again in a moment.";
    case LobbyRejection::TooFast:
        return "Too many tries from here. Wait a moment and try again.";
    case LobbyRejection::Malformed:
        return "The lobby server did not understand that.";
    case LobbyRejection::None:
    default:
        return "";
    }
}

LobbyRejection RejectionFromText(const std::string& error)
{
    if (error == "no-such-lobby")
    {
        return LobbyRejection::NoSuchLobby;
    }
    if (error == "full")
    {
        return LobbyRejection::LobbyFull;
    }
    if (error == "wrong-version")
    {
        return LobbyRejection::WrongVersion;
    }
    if (error == "server-full")
    {
        return LobbyRejection::ServerFull;
    }
    if (error == "too-fast")
    {
        return LobbyRejection::TooFast;
    }
    return LobbyRejection::Malformed;
}

// --- Probes ------------------------------------------------------------------------------------

std::vector<uint8_t> EncodeLobby(const LobbyPacket& packet)
{
    std::vector<uint8_t> bytes(kProbeBytes);
    PutU32(bytes.data(), kLobbyMagic);
    bytes[4] = static_cast<uint8_t>(packet.kind);
    PutU32(bytes.data() + 5, packet.token);
    return bytes;
}

bool IsLobbyDatagram(const uint8_t* data, size_t bytes)
{
    return data != nullptr && bytes == kProbeBytes && GetU32(data) == kLobbyMagic;
}

bool DecodeLobby(const uint8_t* data, size_t bytes, LobbyPacket& out)
{
    if (!IsLobbyDatagram(data, bytes))
    {
        return false;
    }
    const uint8_t kind = data[4];
    if (kind != static_cast<uint8_t>(LobbyMessage::Probe) && kind != static_cast<uint8_t>(LobbyMessage::ProbeReply))
    {
        return false;
    }
    out.kind = static_cast<LobbyMessage>(kind);
    out.token = GetU32(data + 5);
    return true;
}

// --- STUN --------------------------------------------------------------------------------------

std::vector<uint8_t> EncodeStunRequest(const StunTransaction& transaction)
{
    std::vector<uint8_t> out;
    out.reserve(20);
    PutBig16(out, kStunBindingRequest);
    PutBig16(out, 0); // no attributes
    PutBig32(out, kStunCookie);
    out.insert(out.end(), transaction.begin(), transaction.end());
    return out;
}

bool IsStunDatagram(const uint8_t* data, size_t bytes)
{
    // The top two bits of every STUN message are zero and the cookie is always in the same place,
    // which is how STUN is told apart from anything else arriving on the same socket.
    return data != nullptr && bytes >= 20 && bytes <= 1024 && (data[0] & 0xC0u) == 0 && GetBig32(data + 4) == kStunCookie;
}

bool DecodeStunRequest(const uint8_t* data, size_t bytes, StunTransaction& transaction)
{
    if (!IsStunDatagram(data, bytes) || GetBig16(data) != kStunBindingRequest)
    {
        return false;
    }
    std::memcpy(transaction.data(), data + 8, transaction.size());
    return true;
}

bool DecodeStunResponse(const uint8_t* data, size_t bytes, const StunTransaction& transaction,
                        LobbyEndpoint& mapped)
{
    if (!IsStunDatagram(data, bytes) || GetBig16(data) != kStunBindingSuccess ||
        std::memcmp(transaction.data(), data + 8, transaction.size()) != 0)
    {
        return false;
    }
    const size_t length = GetBig16(data + 2);
    if (20 + length > bytes)
    {
        return false;
    }
    // Attributes, each four-byte aligned. XOR-MAPPED-ADDRESS is what every modern server sends; plain
    // MAPPED-ADDRESS is the old spelling of the same answer, kept for servers that still use it.
    LobbyEndpoint plain;
    for (size_t at = 20; at + 4 <= 20 + length;)
    {
        const uint16_t type = GetBig16(data + at);
        const uint16_t size = GetBig16(data + at + 2);
        const uint8_t* value = data + at + 4;
        if (at + 4 + size > 20 + length)
        {
            return false;
        }
        if ((type == kStunXorMappedAddress || type == kStunMappedAddress) && size >= 8 && value[1] == 0x01)
        {
            uint16_t port = GetBig16(value + 2);
            uint32_t address = GetBig32(value + 4);
            if (type == kStunXorMappedAddress)
            {
                port = static_cast<uint16_t>(port ^ (kStunCookie >> 16));
                address ^= kStunCookie;
                mapped = LobbyEndpoint{address, port};
                return mapped.Valid();
            }
            plain = LobbyEndpoint{address, port};
        }
        at += 4 + ((static_cast<size_t>(size) + 3u) & ~static_cast<size_t>(3u));
    }
    if (plain.Valid())
    {
        mapped = plain;
        return true;
    }
    return false;
}

std::vector<uint8_t> EncodeStunResponse(const StunTransaction& transaction, const LobbyEndpoint& mapped)
{
    std::vector<uint8_t> out;
    out.reserve(32);
    PutBig16(out, kStunBindingSuccess);
    PutBig16(out, 12);
    PutBig32(out, kStunCookie);
    out.insert(out.end(), transaction.begin(), transaction.end());
    PutBig16(out, kStunXorMappedAddress);
    PutBig16(out, 8);
    out.push_back(0);
    out.push_back(0x01);
    PutBig16(out, static_cast<uint16_t>(mapped.port ^ (kStunCookie >> 16)));
    PutBig32(out, mapped.address ^ kStunCookie);
    return out;
}

} // namespace pred
