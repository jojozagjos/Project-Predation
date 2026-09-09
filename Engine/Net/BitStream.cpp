#include "Engine/Net/BitStream.h"

#include <algorithm>
#include <cmath>

namespace pred
{
namespace
{

// Zigzag, so a small negative number costs as few bits as a small positive one.
uint32_t ZigZag(int32_t value)
{
    return (static_cast<uint32_t>(value) << 1) ^ static_cast<uint32_t>(value >> 31);
}

int32_t UnZigZag(uint32_t value)
{
    return static_cast<int32_t>((value >> 1) ^ (~(value & 1u) + 1u));
}

} // namespace

BitWriter::BitWriter(size_t reserveBytes)
{
    m_bytes.reserve(reserveBytes);
}

void BitWriter::WriteBits(uint32_t value, int bits)
{
    bits = std::clamp(bits, 1, 32);
    if (bits < 32)
    {
        value &= (1u << bits) - 1u;
    }

    for (int i = 0; i < bits; ++i)
    {
        const size_t byteIndex = m_bitCount >> 3;
        const int bitIndex = static_cast<int>(m_bitCount & 7u);
        if (byteIndex >= m_bytes.size())
        {
            m_bytes.push_back(0);
        }
        if (((value >> i) & 1u) != 0u)
        {
            m_bytes[byteIndex] |= static_cast<uint8_t>(1u << bitIndex);
        }
        ++m_bitCount;
    }
}

void BitWriter::WriteInt(int32_t value)
{
    WriteBits(ZigZag(value), 32);
}

void BitWriter::WriteSignedBits(int32_t value, int bits)
{
    WriteBits(ZigZag(value), std::clamp(bits, 1, 32));
}

void BitWriter::WriteFloat(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    WriteBits(bits, 32);
}

// One code short of the full range on purpose. With an even number of intervals the exact middle
// of the range has a code of its own, so a centred stick, a level pitch and a zero lean all decode
// back to exactly zero. An odd number would round them to the nearest step instead, and the player
// would drift forwards while standing still.
namespace
{
uint32_t QuantisationSteps(int bits)
{
    return bits < 2 ? 1u : (1u << bits) - 2u;
}
} // namespace

void BitWriter::WriteQuantised(float value, float min, float max, int bits)
{
    bits = std::clamp(bits, 1, 31);
    const float span = std::max(max - min, 1e-6f);
    const float normalised = std::clamp((value - min) / span, 0.0f, 1.0f);
    const uint32_t steps = QuantisationSteps(bits);
    WriteBits(static_cast<uint32_t>(std::lround(normalised * static_cast<float>(steps))), bits);
}

void BitWriter::WriteVec3(const glm::vec3& value)
{
    WriteFloat(value.x);
    WriteFloat(value.y);
    WriteFloat(value.z);
}

namespace
{
// A unit quaternion's largest component is at least 1/sqrt(2), so the other three all fit in
// [-1/sqrt(2), 1/sqrt(2)] and the largest can be rebuilt from them. Dropping the biggest one keeps
// the reconstruction best conditioned.
constexpr float kQuatRange = 0.7071068f;
constexpr int kQuatBits = 9;
} // namespace

void BitWriter::WriteQuaternion(const glm::quat& value)
{
    const glm::quat unit = glm::normalize(value);
    const float components[4] = {unit.x, unit.y, unit.z, unit.w};

    int largest = 0;
    for (int i = 1; i < 4; ++i)
    {
        if (std::abs(components[i]) > std::abs(components[largest]))
        {
            largest = i;
        }
    }

    // q and -q are the same rotation, so the dropped component is made positive and its sign never
    // has to be sent.
    const float sign = components[largest] < 0.0f ? -1.0f : 1.0f;
    WriteBits(static_cast<uint32_t>(largest), 2);
    for (int i = 0; i < 4; ++i)
    {
        if (i != largest)
        {
            WriteQuantised(components[i] * sign, -kQuatRange, kQuatRange, kQuatBits);
        }
    }
}

glm::quat BitReader::ReadQuaternion()
{
    const uint32_t largest = ReadBits(2);
    float components[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float sumOfSquares = 0.0f;
    for (int i = 0; i < 4; ++i)
    {
        if (static_cast<uint32_t>(i) != largest)
        {
            components[i] = ReadQuantised(-kQuatRange, kQuatRange, kQuatBits);
            sumOfSquares += components[i] * components[i];
        }
    }
    components[largest] = std::sqrt(std::max(1.0f - sumOfSquares, 0.0f));
    return glm::normalize(glm::quat(components[3], components[0], components[1], components[2]));
}

const std::vector<uint8_t>& BitWriter::Finish()
{
    m_finished = true;
    return m_bytes;
}

BitReader::BitReader(const uint8_t* data, size_t bytes) : m_data(data), m_totalBits(bytes * 8)
{
    if (data == nullptr)
    {
        m_totalBits = 0;
    }
}

uint32_t BitReader::ReadBits(int bits)
{
    bits = std::clamp(bits, 1, 32);
    if (m_bitCount + static_cast<size_t>(bits) > m_totalBits)
    {
        // Everything past the end is zero. A truncated or hostile packet then decodes to something
        // harmless instead of reading whatever happens to follow the buffer.
        m_overran = true;
        m_bitCount = m_totalBits;
        return 0u;
    }

    uint32_t value = 0;
    for (int i = 0; i < bits; ++i)
    {
        const size_t byteIndex = m_bitCount >> 3;
        const int bitIndex = static_cast<int>(m_bitCount & 7u);
        if (((m_data[byteIndex] >> bitIndex) & 1u) != 0u)
        {
            value |= (1u << i);
        }
        ++m_bitCount;
    }
    return value;
}

int32_t BitReader::ReadInt()
{
    return UnZigZag(ReadBits(32));
}

int32_t BitReader::ReadSignedBits(int bits)
{
    return UnZigZag(ReadBits(std::clamp(bits, 1, 32)));
}

float BitReader::ReadFloat()
{
    const uint32_t bits = ReadBits(32);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

float BitReader::ReadQuantised(float min, float max, int bits)
{
    bits = std::clamp(bits, 1, 31);
    const uint32_t steps = QuantisationSteps(bits);
    const uint32_t raw = std::min(ReadBits(bits), steps);
    const float normalised = static_cast<float>(raw) / static_cast<float>(steps);
    return min + normalised * (max - min);
}

glm::vec3 BitReader::ReadVec3()
{
    const float x = ReadFloat();
    const float y = ReadFloat();
    const float z = ReadFloat();
    return {x, y, z};
}

} // namespace pred
