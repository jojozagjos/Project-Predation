#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

namespace pred
{

// Bit-level packet writing and reading.
//
// Bandwidth is the budget that decides how much of the world can be replicated, so values are
// written in as many bits as they actually need rather than rounded up to whole bytes. A player's
// stance takes two bits; a button takes one.
//
// Every read is bounds-checked and a stream that has overrun reports it rather than returning
// rubbish. Packets arrive from the network, and a reader that trusts its input is an exploit.
class BitWriter
{
public:
    explicit BitWriter(size_t reserveBytes = 256);

    void WriteBits(uint32_t value, int bits);
    void WriteBool(bool value) { WriteBits(value ? 1u : 0u, 1); }
    void WriteByte(uint8_t value) { WriteBits(value, 8); }
    void WriteUInt(uint32_t value) { WriteBits(value, 32); }
    void WriteInt(int32_t value);
    // Signed, in as few bits as you say. Zigzagged first, so -3 costs what 3 costs instead of
    // costing a full word because of its sign bits.
    void WriteSignedBits(int32_t value, int bits);
    void WriteFloat(float value);
    // Quantised to `bits` steps across [min, max]. Positions and angles do not need the full range
    // or precision of a float, and saying so is most of what keeps a snapshot small.
    void WriteQuantised(float value, float min, float max, int bits);
    void WriteVec3(const glm::vec3& value);
    // A unit quaternion in 29 bits, by dropping its largest component and rebuilding it from the
    // other three. A rotation only has three degrees of freedom, so sending four numbers sends one
    // of them twice, and the dropped one is always the best conditioned to reconstruct.
    void WriteQuaternion(const glm::quat& value);

    // Pads to the next byte boundary and hands back the finished packet.
    const std::vector<uint8_t>& Finish();

    size_t BitsWritten() const { return m_bitCount; }
    size_t BytesWritten() const { return (m_bitCount + 7) / 8; }

private:
    std::vector<uint8_t> m_bytes;
    size_t m_bitCount = 0;
    bool m_finished = false;
};

class BitReader
{
public:
    BitReader(const uint8_t* data, size_t bytes);

    uint32_t ReadBits(int bits);
    bool ReadBool() { return ReadBits(1) != 0u; }
    uint8_t ReadByte() { return static_cast<uint8_t>(ReadBits(8)); }
    uint32_t ReadUInt() { return ReadBits(32); }
    int32_t ReadInt();
    int32_t ReadSignedBits(int bits);
    float ReadFloat();
    float ReadQuantised(float min, float max, int bits);
    glm::vec3 ReadVec3();
    glm::quat ReadQuaternion();

    // True once a read has run off the end. Everything after that returns zero, so a malformed
    // packet produces a harmless message rather than reading whatever happened to be in memory.
    bool Overran() const { return m_overran; }
    size_t BitsRead() const { return m_bitCount; }
    size_t BitsRemaining() const { return m_totalBits > m_bitCount ? m_totalBits - m_bitCount : 0; }

private:
    const uint8_t* m_data = nullptr;
    size_t m_totalBits = 0;
    size_t m_bitCount = 0;
    bool m_overran = false;
};

} // namespace pred
