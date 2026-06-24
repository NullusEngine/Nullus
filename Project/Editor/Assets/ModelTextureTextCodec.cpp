#include "Assets/ModelTextureTextCodec.h"

namespace NLS::Editor::Assets
{
namespace
{
int HexValue(const char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return 10 + value - 'a';
    if (value >= 'A' && value <= 'F')
        return 10 + value - 'A';
    return -1;
}

bool ShouldPercentEncode(const unsigned char value)
{
    return value == '%' ||
        value == '=' ||
        value == '#' ||
        value == ';' ||
        value == '|' ||
        value == '\r' ||
        value == '\n' ||
        value >= 0x80u;
}
}

std::string EncodeModelTextureTextField(std::string_view value)
{
    constexpr char kHex[] = "0123456789ABCDEF";

    std::string encoded;
    encoded.reserve(value.size());
    for (const unsigned char byte : value)
    {
        if (!ShouldPercentEncode(byte))
        {
            encoded.push_back(static_cast<char>(byte));
            continue;
        }

        encoded.push_back('%');
        encoded.push_back(kHex[(byte >> 4u) & 0x0fu]);
        encoded.push_back(kHex[byte & 0x0fu]);
    }
    return encoded;
}

std::optional<std::string> DecodeModelTextureTextField(std::string_view value)
{
    std::string decoded;
    decoded.reserve(value.size());
    for (size_t index = 0u; index < value.size(); ++index)
    {
        if (value[index] != '%')
        {
            decoded.push_back(value[index]);
            continue;
        }

        if (index + 2u >= value.size())
            return std::nullopt;

        const int high = HexValue(value[index + 1u]);
        const int low = HexValue(value[index + 2u]);
        if (high < 0 || low < 0)
            return std::nullopt;

        decoded.push_back(static_cast<char>((high << 4) | low));
        index += 2u;
    }
    return decoded;
}
}
