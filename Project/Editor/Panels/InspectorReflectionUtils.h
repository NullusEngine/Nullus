#pragma once

#include <Reflection/Enum.h>
#include <Reflection/Type.h>
#include <Reflection/Variant.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <string_view>

namespace NLS::Editor::Panels
{
inline std::string FormatEnumChoiceLabel(std::string_view key)
{
    if (key.empty())
        return {};

    auto formatWord = [](std::string_view word) -> std::string
    {
        if (word.empty())
            return {};

        std::string formatted;
        formatted.reserve(word.size());

        bool upperNext = true;
        for (const char raw : word)
        {
            const auto ch = static_cast<unsigned char>(raw);
            if (std::isdigit(ch) != 0)
            {
                formatted.push_back(static_cast<char>(ch));
                upperNext = false;
                continue;
            }

            if (std::isalpha(ch) == 0)
            {
                formatted.push_back(static_cast<char>(ch));
                continue;
            }

            formatted.push_back(static_cast<char>(upperNext ? std::toupper(ch) : std::tolower(ch)));
            upperNext = false;
        }

        return formatted;
    };

    std::string result;
    std::string current;

    const auto flushCurrent = [&]()
    {
        if (current.empty())
            return;

        if (!result.empty())
            result.push_back(' ');

        result += formatWord(current);
        current.clear();
    };

    const bool hasUnderscore = key.find('_') != std::string_view::npos;
    for (size_t i = 0; i < key.size(); ++i)
    {
        const char raw = key[i];
        const auto ch = static_cast<unsigned char>(raw);

        if (raw == '_')
        {
            flushCurrent();
            continue;
        }

        if (!hasUnderscore
            && !current.empty()
            && std::isupper(ch) != 0
            && std::islower(static_cast<unsigned char>(current.back())) != 0)
        {
            flushCurrent();
        }

        current.push_back(static_cast<char>(ch));
    }

    flushCurrent();
    return result;
}

inline std::map<int, std::string> BuildEnumChoices(const meta::Type& enumType)
{
    std::map<int, std::string> choices;
    if (!enumType.IsValid() || !enumType.IsEnum())
        return choices;

    const auto keys = enumType.GetEnum().GetKeys();
    for (const auto& key : keys)
    {
        const auto value = enumType.GetEnum().GetValue(key);
        if (!value.IsValid())
            continue;

        choices.emplace(value.ToInt(), FormatEnumChoiceLabel(key));
    }

    return choices;
}
} // namespace NLS::Editor::Panels
