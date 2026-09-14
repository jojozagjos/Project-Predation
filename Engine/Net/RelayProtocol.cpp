#include "Engine/Net/RelayProtocol.h"

#include "Engine/Net/BitStream.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace pred
{
namespace
{

// So a stray datagram on an open port is dropped before anything looks at it. Different from the
// game transport's magic on purpose: a relay and a game server may end up on the same machine, and
// a packet arriving at the wrong one should be ignored rather than half understood.
constexpr uint32_t kRelayMagic = 0x50524C59u; // 'PRLY'
constexpr int kAlphabetBits = 5;

int SymbolIndex(char c)
{
    const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (int i = 0; i < 32; ++i)
    {
        if (kRelayCodeAlphabet[i] == upper)
        {
            return i;
        }
    }
    // The three pairs a person confuses, folded onto the one that is in the alphabet. Somebody
    // reading a code aloud says "oh" and the other types O; both should work.
    switch (upper)
    {
    case 'O':
        return SymbolIndex('0' + 0) >= 0 ? -1 : -1; // '0' is not in the alphabet either
    default:
        break;
    }
    return -1;
}

} // namespace

const char* Describe(RelayRejection reason)
{
    switch (reason)
    {
    case RelayRejection::NoSuchLobby:
        return "No game with that code. Check it, or ask them for a new one.";
    case RelayRejection::LobbyFull:
        return "That game is full.";
    case RelayRejection::RelayFull:
        return "The relay is busy. Try again in a moment.";
    case RelayRejection::TooFast:
        return "Too many attempts from here. Wait a moment.";
    case RelayRejection::Malformed:
        return "The relay did not understand that.";
    case RelayRejection::None:
    default:
        return "";
    }
}

bool EncodeRelayCode(const std::string& text, uint32_t& out)
{
    // Whatever somebody pasted, with the spacing and punctuation people add to long codes taken
    // back out. "k7x2-qm" and "K7X2QM" are the same code.
    std::string cleaned;
    for (const char c : text)
    {
        if (std::isalnum(static_cast<unsigned char>(c)))
        {
            cleaned.push_back(c);
        }
    }
    if (static_cast<int>(cleaned.size()) != kRelayCodeLength)
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

std::string DecodeRelayCode(uint32_t value)
{
    std::string text(static_cast<size_t>(kRelayCodeLength), '?');
    for (int i = kRelayCodeLength - 1; i >= 0; --i)
    {
        text[static_cast<size_t>(i)] = kRelayCodeAlphabet[value & 0x1Fu];
        value >>= kAlphabetBits;
    }
    return text;
}

std::vector<uint8_t> EncodeRelay(const RelayPacket& packet)
{
    BitWriter writer(packet.payload.size() + 16);
    writer.WriteUInt(kRelayMagic);
    writer.WriteBits(static_cast<uint32_t>(packet.kind), 4);

    switch (packet.kind)
    {
    case RelayMessage::Host:
    case RelayMessage::Leave:
    case RelayMessage::KeepAlive:
        break;
    case RelayMessage::Join:
        writer.WriteBits(packet.code, kAlphabetBits * kRelayCodeLength);
        break;
    case RelayMessage::Hosted:
        writer.WriteBits(packet.code, kAlphabetBits * kRelayCodeLength);
        writer.WriteBits(packet.slot, 3);
        break;
    case RelayMessage::Joined:
    case RelayMessage::PeerJoined:
    case RelayMessage::PeerLeft:
        writer.WriteBits(packet.slot, 3);
        break;
    case RelayMessage::Rejected:
        writer.WriteBits(static_cast<uint32_t>(packet.reason), 3);
        break;
    case RelayMessage::Data:
    case RelayMessage::Relayed:
    {
        writer.WriteBits(packet.slot, 3);
        const auto length = static_cast<uint32_t>(std::min(packet.payload.size(), kRelayMaxPayload));
        writer.WriteBits(length, 11); // 2047, comfortably over the cap
        for (uint32_t i = 0; i < length; ++i)
        {
            writer.WriteByte(packet.payload[i]);
        }
        break;
    }
    }
    return writer.Finish();
}

bool DecodeRelay(const uint8_t* data, size_t bytes, RelayPacket& out)
{
    if (data == nullptr || bytes < 5)
    {
        return false;
    }
    BitReader reader(data, bytes);
    if (reader.ReadUInt() != kRelayMagic)
    {
        return false;
    }
    const uint32_t kind = reader.ReadBits(4);
    out = RelayPacket{};
    out.kind = static_cast<RelayMessage>(kind);

    switch (out.kind)
    {
    case RelayMessage::Host:
    case RelayMessage::Leave:
    case RelayMessage::KeepAlive:
        break;
    case RelayMessage::Join:
        out.code = reader.ReadBits(kAlphabetBits * kRelayCodeLength);
        break;
    case RelayMessage::Hosted:
        out.code = reader.ReadBits(kAlphabetBits * kRelayCodeLength);
        out.slot = static_cast<uint8_t>(reader.ReadBits(3));
        break;
    case RelayMessage::Joined:
    case RelayMessage::PeerJoined:
    case RelayMessage::PeerLeft:
        out.slot = static_cast<uint8_t>(reader.ReadBits(3));
        break;
    case RelayMessage::Rejected:
        out.reason = static_cast<RelayRejection>(reader.ReadBits(3));
        break;
    case RelayMessage::Data:
    case RelayMessage::Relayed:
    {
        out.slot = static_cast<uint8_t>(reader.ReadBits(3));
        const uint32_t length = reader.ReadBits(11);
        if (length > kRelayMaxPayload)
        {
            return false;
        }
        // Checked against what is actually there before anything is reserved, or a packet claiming
        // two thousand bytes it does not have buys an allocation for free.
        if (reader.BitsRemaining() < static_cast<size_t>(length) * 8)
        {
            return false;
        }
        out.payload.resize(length);
        for (uint32_t i = 0; i < length; ++i)
        {
            out.payload[i] = reader.ReadByte();
        }
        break;
    }
    default:
        // A kind this relay does not have. Somebody else's traffic, or a newer client.
        return false;
    }

    return !reader.Overran();
}

} // namespace pred
