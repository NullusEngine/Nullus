#include "Guid.h"

#include <algorithm>
#include <random>
#include <stdexcept>

namespace
{
    constexpr uint64_t kFnvOffset = 14695981039346656037ull;
    constexpr uint64_t kFnvPrime = 1099511628211ull;

    int HexValue(const char character)
    {
        if (character >= '0' && character <= '9')
            return character - '0';
        if (character >= 'a' && character <= 'f')
            return 10 + character - 'a';
        if (character >= 'A' && character <= 'F')
            return 10 + character - 'A';
        return -1;
    }

    char HexDigit(const uint8_t value)
    {
        constexpr char kDigits[] = "0123456789abcdef";
        return kDigits[value & 0x0f];
    }

    uint64_t HashLabel(std::string_view label, const uint64_t seed)
    {
        uint64_t hash = kFnvOffset ^ seed;
        for (const unsigned char character : label)
        {
            hash ^= character;
            hash *= kFnvPrime;
        }
        return hash;
    }

    void ApplyUuidVersionAndVariant(NLS::Guid::Bytes& bytes)
    {
        bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0f) | 0x40);
        bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3f) | 0x80);
    }
}

namespace NLS
{
    Guid::Guid(const Bytes& bytes)
        : m_bytes(bytes)
    {
    }

    Guid Guid::New()
    {
        Bytes bytes {};
        std::random_device randomDevice;

        for (size_t index = 0; index < bytes.size(); ++index)
            bytes[index] = static_cast<uint8_t>(randomDevice());

        ApplyUuidVersionAndVariant(bytes);
        return Guid(bytes);
    }

    Guid Guid::NewDeterministic(std::string_view label)
    {
        Bytes bytes {};
        const auto first = HashLabel(label, 0x4e4c535f47554944ull);
        const auto second = HashLabel(label, 0x646975675f534c4eull);

        for (size_t index = 0; index < 8; ++index)
        {
            bytes[index] = static_cast<uint8_t>((first >> ((7 - index) * 8)) & 0xff);
            bytes[index + 8] = static_cast<uint8_t>((second >> ((7 - index) * 8)) & 0xff);
        }

        ApplyUuidVersionAndVariant(bytes);
        return Guid(bytes);
    }

    Guid Guid::Empty()
    {
        return Guid();
    }

    std::optional<Guid> Guid::TryParse(std::string_view text)
    {
        if (text.size() != 36)
            return std::nullopt;

        constexpr size_t kDashPositions[] = { 8, 13, 18, 23 };
        for (const auto dashPosition : kDashPositions)
        {
            if (text[dashPosition] != '-')
                return std::nullopt;
        }

        Bytes bytes {};
        size_t nibbleIndex = 0;
        for (size_t textIndex = 0; textIndex < text.size(); ++textIndex)
        {
            if (text[textIndex] == '-')
                continue;

            const auto value = HexValue(text[textIndex]);
            if (value < 0)
                return std::nullopt;

            const auto byteIndex = nibbleIndex / 2;
            if ((nibbleIndex % 2) == 0)
                bytes[byteIndex] = static_cast<uint8_t>(value << 4);
            else
                bytes[byteIndex] = static_cast<uint8_t>(bytes[byteIndex] | value);

            ++nibbleIndex;
        }

        if (nibbleIndex != 32)
            return std::nullopt;

        return Guid(bytes);
    }

    Guid Guid::Parse(std::string_view text)
    {
        auto parsed = TryParse(text);
        if (!parsed.has_value())
            throw std::invalid_argument("Invalid GUID text");
        return *parsed;
    }

    bool Guid::IsValid() const
    {
        return std::any_of(
            m_bytes.begin(),
            m_bytes.end(),
            [](const uint8_t value)
            {
                return value != 0;
            });
    }

    std::string Guid::ToString() const
    {
        std::string output;
        output.reserve(36);

        for (size_t index = 0; index < m_bytes.size(); ++index)
        {
            if (index == 4 || index == 6 || index == 8 || index == 10)
                output.push_back('-');

            output.push_back(HexDigit(static_cast<uint8_t>(m_bytes[index] >> 4)));
            output.push_back(HexDigit(m_bytes[index]));
        }

        return output;
    }

    const Guid::Bytes& Guid::GetBytes() const
    {
        return m_bytes;
    }

    bool operator<(const Guid& lhs, const Guid& rhs)
    {
        return lhs.m_bytes < rhs.m_bytes;
    }

    void to_json(nlohmann::json& json, const Guid& guid)
    {
        json = guid.ToString();
    }

    void from_json(const nlohmann::json& json, Guid& guid)
    {
        if (!json.is_string())
            throw std::invalid_argument("GUID JSON value must be a string");
        guid = Guid::Parse(json.get<std::string>());
    }
}
