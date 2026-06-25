#include "Rendering/Resources/MaterialSerialization.h"

#include <cctype>
#include <sstream>
#include <vector>

namespace NLS::Render::Resources
{
    namespace
    {
        bool IsMaterialFieldSafeCharacter(const unsigned char character)
        {
            return std::isalnum(character) != 0 ||
                character == '_' ||
                character == '-' ||
                character == '.' ||
                character == '/' ||
                character == ':' ||
                character == '?';
        }

        char HexDigit(const unsigned int value)
        {
            return static_cast<char>(value < 10u ? ('0' + value) : ('A' + value - 10u));
        }

        int HexValue(const char character)
        {
            if (character >= '0' && character <= '9')
                return character - '0';
            if (character >= 'A' && character <= 'F')
                return character - 'A' + 10;
            if (character >= 'a' && character <= 'f')
                return character - 'a' + 10;
            return -1;
        }

        std::vector<std::string> SplitWhitespace(const std::string_view value)
        {
            std::vector<std::string> tokens;
            std::istringstream stream{std::string(value)};
            std::string token;
            while (stream >> token)
                tokens.push_back(std::move(token));
            return tokens;
        }
    }

    std::string EscapeMaterialField(const std::string_view value)
    {
        std::string escaped;
        escaped.reserve(value.size());
        for (const unsigned char character : value)
        {
            if (IsMaterialFieldSafeCharacter(character))
            {
                escaped.push_back(static_cast<char>(character));
                continue;
            }

            escaped.push_back('%');
            escaped.push_back(HexDigit((character >> 4u) & 0xFu));
            escaped.push_back(HexDigit(character & 0xFu));
        }
        return escaped;
    }

    std::string UnescapeMaterialField(const std::string_view value)
    {
        std::string unescaped;
        unescaped.reserve(value.size());
        for (size_t index = 0u; index < value.size(); ++index)
        {
            if (value[index] != '%' || index + 2u >= value.size())
            {
                unescaped.push_back(value[index]);
                continue;
            }

            const int high = HexValue(value[index + 1u]);
            const int low = HexValue(value[index + 2u]);
            if (high < 0 || low < 0)
            {
                unescaped.push_back(value[index]);
                continue;
            }

            unescaped.push_back(static_cast<char>((high << 4) | low));
            index += 2u;
        }
        return unescaped;
    }

    std::map<std::string, std::string> ParseMaterialKeyValueTail(const std::string_view tail)
    {
        std::map<std::string, std::string> values;
        for (const auto& token : SplitWhitespace(tail))
        {
            const auto equals = token.find('=');
            if (equals == std::string::npos || equals == 0u)
                continue;
            values[token.substr(0u, equals)] = UnescapeMaterialField(token.substr(equals + 1u));
        }
        return values;
    }
}
