#include "Engine/Net/LobbyProtocol.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace pred
{
namespace
{

// 'PLBY'. Different from the game's own and from the network beacon's, so a datagram that reaches
// the wrong one of the three is dropped rather than half understood.
constexpr uint32_t kLobbyMagic = 0x50424C59u;
constexpr int kAlphabetBits = 5;

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

// Plain bytes, little-endian, rather than the game's bit stream: this file is built on its own for the
// server, and every byte of a lobby message is spent once a second at most.
class Writer
{
public:
    void U8(uint8_t value) { m_bytes.push_back(value); }
    void U16(uint16_t value)
    {
        U8(static_cast<uint8_t>(value & 0xFFu));
        U8(static_cast<uint8_t>(value >> 8));
    }
    void U32(uint32_t value)
    {
        U16(static_cast<uint16_t>(value & 0xFFFFu));
        U16(static_cast<uint16_t>(value >> 16));
    }
    void Text(const std::string& text)
    {
        const size_t length = std::min(text.size(), kLobbyMaxNameLength);
        U8(static_cast<uint8_t>(length));
        m_bytes.insert(m_bytes.end(), text.begin(), text.begin() + static_cast<std::ptrdiff_t>(length));
    }
    void Endpoint(const LobbyEndpoint& endpoint)
    {
        U32(endpoint.address);
        U16(endpoint.port);
    }
    std::vector<uint8_t> Take() { return std::move(m_bytes); }

private:
    std::vector<uint8_t> m_bytes;
};

class Reader
{
public:
    Reader(const uint8_t* data, size_t bytes) : m_data(data), m_bytes(bytes) {}
    bool Overran() const { return m_overran; }
    bool AtEnd() const { return m_at == m_bytes; }

    uint8_t U8()
    {
        if (m_at >= m_bytes)
        {
            m_overran = true;
            return 0;
        }
        return m_data[m_at++];
    }
    uint16_t U16()
    {
        const uint16_t low = U8();
        return static_cast<uint16_t>(low | (static_cast<uint16_t>(U8()) << 8));
    }
    uint32_t U32()
    {
        const uint32_t low = U16();
        return low | (static_cast<uint32_t>(U16()) << 16);
    }
    std::string Text()
    {
        const size_t length = U8();
        if (length > kLobbyMaxNameLength)
        {
            m_overran = true;
            return {};
        }
        std::string text;
        for (size_t i = 0; i < length; ++i)
        {
            const auto c = static_cast<char>(U8());
            // Printable only. A lobby name is drawn on the screen of everybody who browses, and
            // anybody at all can open a lobby, so it does not get to carry control characters.
            text.push_back(c >= 0x20 && c < 0x7F ? c : '?');
        }
        return text;
    }
    LobbyEndpoint Endpoint()
    {
        LobbyEndpoint endpoint;
        endpoint.address = U32();
        endpoint.port = U16();
        return endpoint;
    }

private:
    const uint8_t* m_data = nullptr;
    size_t m_bytes = 0;
    size_t m_at = 0;
    bool m_overran = false;
};

void WriteCandidates(Writer& writer, const std::vector<LobbyEndpoint>& candidates)
{
    const size_t count = std::min(candidates.size(), kLobbyMaxCandidates);
    writer.U8(static_cast<uint8_t>(count));
    for (size_t i = 0; i < count; ++i)
    {
        writer.Endpoint(candidates[i]);
    }
}

bool ReadCandidates(Reader& reader, std::vector<LobbyEndpoint>& out)
{
    const size_t count = reader.U8();
    if (count > kLobbyMaxCandidates)
    {
        return false;
    }
    out.clear();
    for (size_t i = 0; i < count; ++i)
    {
        out.push_back(reader.Endpoint());
    }
    return !reader.Overran();
}

uint8_t Flags(const LobbyPacket& packet)
{
    return static_cast<uint8_t>((packet.started ? 1u : 0u) | (packet.listed ? 2u : 0u) |
                                (packet.toHost ? 4u : 0u));
}

void ApplyFlags(uint8_t flags, LobbyPacket& packet)
{
    packet.started = (flags & 1u) != 0;
    packet.listed = (flags & 2u) != 0;
    packet.toHost = (flags & 4u) != 0;
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

std::vector<uint8_t> EncodeLobby(const LobbyPacket& packet)
{
    Writer writer;
    writer.U32(kLobbyMagic);
    writer.U8(static_cast<uint8_t>(packet.kind));
    switch (packet.kind)
    {
    case LobbyMessage::Host:
    case LobbyMessage::Update:
        writer.U16(packet.version);
        writer.U32(packet.code);
        writer.U32(packet.secret);
        writer.U8(packet.players);
        writer.U8(packet.maxPlayers);
        writer.U8(Flags(packet));
        writer.Text(packet.name);
        WriteCandidates(writer, packet.candidates);
        break;

    case LobbyMessage::Close:
        writer.U32(packet.code);
        writer.U32(packet.secret);
        break;

    case LobbyMessage::Join:
        writer.U16(packet.version);
        writer.U32(packet.code);
        WriteCandidates(writer, packet.candidates);
        break;

    case LobbyMessage::List:
        writer.U16(packet.version);
        break;

    case LobbyMessage::Hosted:
        writer.U32(packet.code);
        writer.U32(packet.secret);
        writer.Endpoint(packet.seenAs);
        break;

    case LobbyMessage::Introduce:
        writer.U32(packet.code);
        writer.U32(packet.token);
        writer.U8(Flags(packet));
        writer.Text(packet.name);
        writer.Endpoint(packet.seenAs);
        WriteCandidates(writer, packet.candidates);
        break;

    case LobbyMessage::Rejected:
        writer.U32(packet.code);
        writer.U8(static_cast<uint8_t>(packet.reason));
        break;

    case LobbyMessage::Listing:
    {
        const size_t count = std::min(packet.lobbies.size(), kLobbyMaxListed);
        writer.U8(static_cast<uint8_t>(count));
        for (size_t i = 0; i < count; ++i)
        {
            const LobbyListing& row = packet.lobbies[i];
            writer.U32(row.code);
            writer.U8(row.players);
            writer.U8(row.maxPlayers);
            writer.U8(row.started ? 1 : 0);
            writer.Text(row.name);
        }
        break;
    }

    case LobbyMessage::Probe:
    case LobbyMessage::ProbeReply:
        writer.U32(packet.token);
        break;
    }
    return writer.Take();
}

bool IsLobbyDatagram(const uint8_t* data, size_t bytes)
{
    if (data == nullptr || bytes < 5 || bytes > kLobbyMaxDatagram)
    {
        return false;
    }
    Reader reader(data, bytes);
    return reader.U32() == kLobbyMagic;
}

bool DecodeLobby(const uint8_t* data, size_t bytes, LobbyPacket& out)
{
    if (!IsLobbyDatagram(data, bytes))
    {
        return false;
    }
    Reader reader(data, bytes);
    reader.U32();
    out = LobbyPacket{};
    out.kind = static_cast<LobbyMessage>(reader.U8());
    switch (out.kind)
    {
    case LobbyMessage::Host:
    case LobbyMessage::Update:
        out.version = reader.U16();
        out.code = reader.U32();
        out.secret = reader.U32();
        out.players = reader.U8();
        out.maxPlayers = reader.U8();
        ApplyFlags(reader.U8(), out);
        out.name = reader.Text();
        if (!ReadCandidates(reader, out.candidates))
        {
            return false;
        }
        break;

    case LobbyMessage::Close:
        out.code = reader.U32();
        out.secret = reader.U32();
        break;

    case LobbyMessage::Join:
        out.version = reader.U16();
        out.code = reader.U32();
        if (!ReadCandidates(reader, out.candidates))
        {
            return false;
        }
        break;

    case LobbyMessage::List:
        out.version = reader.U16();
        break;

    case LobbyMessage::Hosted:
        out.code = reader.U32();
        out.secret = reader.U32();
        out.seenAs = reader.Endpoint();
        break;

    case LobbyMessage::Introduce:
        out.code = reader.U32();
        out.token = reader.U32();
        ApplyFlags(reader.U8(), out);
        out.name = reader.Text();
        out.seenAs = reader.Endpoint();
        if (!ReadCandidates(reader, out.candidates))
        {
            return false;
        }
        break;

    case LobbyMessage::Rejected:
    {
        out.code = reader.U32();
        const uint8_t reason = reader.U8();
        if (reason > static_cast<uint8_t>(LobbyRejection::Malformed))
        {
            return false;
        }
        out.reason = static_cast<LobbyRejection>(reason);
        break;
    }

    case LobbyMessage::Listing:
    {
        const size_t count = reader.U8();
        if (count > kLobbyMaxListed)
        {
            return false;
        }
        for (size_t i = 0; i < count && !reader.Overran(); ++i)
        {
            LobbyListing row;
            row.code = reader.U32();
            row.players = reader.U8();
            row.maxPlayers = reader.U8();
            row.started = (reader.U8() & 1u) != 0;
            row.name = reader.Text();
            out.lobbies.push_back(std::move(row));
        }
        break;
    }

    case LobbyMessage::Probe:
    case LobbyMessage::ProbeReply:
        out.token = reader.U32();
        break;

    default:
        return false;
    }
    // Exactly the length it should be. Trailing bytes are somebody else's message, or a mistake.
    return !reader.Overran() && reader.AtEnd();
}

} // namespace pred
