#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "EngineDef.h"

namespace NLS::Engine::Serialize
{
// Small, deterministic little-endian primitives shared by editor snapshots and
// runtime binary caches.  The writer never exposes native object layout and the
// reader rejects truncated or overflowing input before touching an output.
class NLS_ENGINE_API BinaryBufferWriter
{
public:
    explicit BinaryBufferWriter(const size_t alignmentOrigin = 0u)
        : m_alignmentOrigin(alignmentOrigin)
    {
    }

    std::vector<uint8_t> bytes;

    void WriteBytes(const void* data, const size_t size)
    {
        if (size == 0u)
            return;
        const auto* begin = static_cast<const uint8_t*>(data);
        bytes.insert(bytes.end(), begin, begin + size);
    }

    void WriteU8(const uint8_t value) { bytes.push_back(value); }

    void WriteBool(const bool value) { WriteU8(value ? 1u : 0u); }

    void WriteU16(const uint16_t value)
    {
        for (uint32_t shift = 0u; shift < 16u; shift += 8u)
            WriteU8(static_cast<uint8_t>((value >> shift) & 0xffu));
    }

    void WriteU32(const uint32_t value)
    {
        for (uint32_t shift = 0u; shift < 32u; shift += 8u)
            WriteU8(static_cast<uint8_t>((value >> shift) & 0xffu));
    }

    void WriteU64(const uint64_t value)
    {
        for (uint32_t shift = 0u; shift < 64u; shift += 8u)
            WriteU8(static_cast<uint8_t>((value >> shift) & 0xffu));
    }

    void WriteI8(const int8_t value) { WriteU8(static_cast<uint8_t>(value)); }
    void WriteI16(const int16_t value) { WriteU16(static_cast<uint16_t>(value)); }
    void WriteI32(const int32_t value) { WriteU32(static_cast<uint32_t>(value)); }
    void WriteI64(const int64_t value) { WriteU64(static_cast<uint64_t>(value)); }

    void WriteFloat(const float value)
    {
        uint32_t bits = 0u;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        WriteU32(bits);
    }

    void WriteDouble(const double value)
    {
        uint64_t bits = 0u;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        WriteU64(bits);
    }

    void WriteString(const std::string_view value)
    {
        WriteU32(static_cast<uint32_t>(value.size()));
        WriteBytes(value.data(), value.size());
    }

    void WriteAlignedString(const std::string_view value)
    {
        WriteString(value);
        Align4();
    }

    void Align4()
    {
        if (bytes.size() > std::numeric_limits<size_t>::max() - m_alignmentOrigin)
            return;
        while (((m_alignmentOrigin + bytes.size()) & 3u) != 0u)
            bytes.push_back(0u);
    }

    [[nodiscard]] size_t GetAlignmentOrigin() const { return m_alignmentOrigin; }

private:
    size_t m_alignmentOrigin = 0u;
};

class NLS_ENGINE_API BinaryBufferReader
{
public:
    BinaryBufferReader() = default;

    explicit BinaryBufferReader(
        const std::span<const uint8_t> input,
        const size_t alignmentOrigin = 0u)
        : m_data(input.data())
        , m_size(input.size())
        , m_alignmentOrigin(alignmentOrigin)
    {
    }

    BinaryBufferReader(
        const uint8_t* data,
        const size_t size,
        const size_t alignmentOrigin = 0u)
        : m_data(data)
        , m_size(size)
        , m_alignmentOrigin(alignmentOrigin)
    {
    }

    [[nodiscard]] size_t Position() const { return m_position; }
    [[nodiscard]] size_t Remaining() const { return m_position <= m_size ? m_size - m_position : 0u; }
    [[nodiscard]] bool IsComplete() const { return m_position == m_size; }

    bool ReadBytes(void* output, const size_t size)
    {
        if (output == nullptr && size != 0u)
            return false;
        if (!CanRead(size))
            return false;
        if (size != 0u)
            std::memcpy(output, m_data + m_position, size);
        m_position += size;
        return true;
    }

    bool ReadU8(uint8_t& value)
    {
        if (!CanRead(1u))
            return false;
        value = m_data[m_position++];
        return true;
    }

    bool ReadBool(bool& value)
    {
        uint8_t raw = 0u;
        if (!ReadU8(raw) || raw > 1u)
            return false;
        value = raw != 0u;
        return true;
    }

    bool ReadU16(uint16_t& value)
    {
        uint8_t bytes[2] {};
        if (!ReadBytes(bytes, sizeof(bytes)))
            return false;
        value = static_cast<uint16_t>(bytes[0]) |
            (static_cast<uint16_t>(bytes[1]) << 8u);
        return true;
    }

    bool ReadU32(uint32_t& value)
    {
        uint8_t bytes[4] {};
        if (!ReadBytes(bytes, sizeof(bytes)))
            return false;
        value = static_cast<uint32_t>(bytes[0]) |
            (static_cast<uint32_t>(bytes[1]) << 8u) |
            (static_cast<uint32_t>(bytes[2]) << 16u) |
            (static_cast<uint32_t>(bytes[3]) << 24u);
        return true;
    }

    bool ReadU64(uint64_t& value)
    {
        uint8_t bytes[8] {};
        if (!ReadBytes(bytes, sizeof(bytes)))
            return false;
        value = static_cast<uint64_t>(bytes[0]) |
            (static_cast<uint64_t>(bytes[1]) << 8u) |
            (static_cast<uint64_t>(bytes[2]) << 16u) |
            (static_cast<uint64_t>(bytes[3]) << 24u) |
            (static_cast<uint64_t>(bytes[4]) << 32u) |
            (static_cast<uint64_t>(bytes[5]) << 40u) |
            (static_cast<uint64_t>(bytes[6]) << 48u) |
            (static_cast<uint64_t>(bytes[7]) << 56u);
        return true;
    }

    bool ReadI8(int8_t& value)
    {
        uint8_t raw = 0u;
        if (!ReadU8(raw))
            return false;
        value = static_cast<int8_t>(raw);
        return true;
    }

    bool ReadI16(int16_t& value)
    {
        uint16_t raw = 0u;
        if (!ReadU16(raw))
            return false;
        value = static_cast<int16_t>(raw);
        return true;
    }

    bool ReadI32(int32_t& value)
    {
        uint32_t raw = 0u;
        if (!ReadU32(raw))
            return false;
        value = static_cast<int32_t>(raw);
        return true;
    }

    bool ReadI64(int64_t& value)
    {
        uint64_t raw = 0u;
        if (!ReadU64(raw))
            return false;
        value = static_cast<int64_t>(raw);
        return true;
    }

    bool ReadFloat(float& value)
    {
        uint32_t bits = 0u;
        if (!ReadU32(bits))
            return false;
        std::memcpy(&value, &bits, sizeof(value));
        return true;
    }

    bool ReadDouble(double& value)
    {
        uint64_t bits = 0u;
        if (!ReadU64(bits))
            return false;
        std::memcpy(&value, &bits, sizeof(value));
        return true;
    }

    bool ReadString(std::string& value)
    {
        uint32_t length = 0u;
        if (!ReadU32(length) || !CanRead(length))
            return false;
        value.assign(reinterpret_cast<const char*>(m_data + m_position), length);
        m_position += length;
        return true;
    }

    bool ReadAlignedString(std::string& value)
    {
        return ReadString(value) && Align4();
    }

    bool Skip(const size_t size)
    {
        if (!CanRead(size))
            return false;
        m_position += size;
        return true;
    }

    bool Align4()
    {
        if (m_position > std::numeric_limits<size_t>::max() - m_alignmentOrigin)
            return false;
        const auto absolute = m_alignmentOrigin + m_position;
        const auto aligned = (absolute + 3u) & ~size_t {3u};
        if (aligned < absolute)
            return false;
        return Skip(aligned - absolute);
    }

private:
    [[nodiscard]] bool CanRead(const size_t size) const
    {
        return m_position <= m_size && size <= m_size - m_position;
    }

    const uint8_t* m_data = nullptr;
    size_t m_size = 0u;
    size_t m_position = 0u;
    size_t m_alignmentOrigin = 0u;
};
} // namespace NLS::Engine::Serialize
