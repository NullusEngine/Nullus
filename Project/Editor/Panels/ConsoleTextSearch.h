#pragma once

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <locale>
#include <string_view>
#include <vector>

namespace NLS::Editor::Panels
{
struct ConsoleTextMatch
{
    size_t begin = std::string_view::npos;
    size_t end = std::string_view::npos;
    explicit operator bool() const { return begin != std::string_view::npos; }
};

// Keep byte offsets into the original UTF-8 while comparing folded Unicode
// code points, so highlighting never splits a multi-byte character.
class ConsoleTextSearch final
{
public:
    explicit ConsoleTextSearch(std::string_view query)
    {
        for (const auto& unit : Decode(query)) m_query.push_back(unit.value);
    }

    bool Empty() const { return m_query.empty(); }

    ConsoleTextMatch Find(std::string_view text) const
    {
        if (m_query.empty()) return {0, 0};
        const auto units = Decode(text);
        const auto found = std::search(units.begin(), units.end(), m_query.begin(), m_query.end(),
            [](const Unit& left, uint32_t right) { return left.value == right; });
        if (found == units.end()) return {};
        const auto last = found + static_cast<std::ptrdiff_t>(m_query.size() - 1);
        return {found->begin, last->end};
    }

private:
    struct Unit { uint32_t value; size_t begin; size_t end; };

    static uint32_t Fold(uint32_t value)
    {
        if (value >= 'A' && value <= 'Z') return value + 0x20;
        if ((value >= 0x00c0 && value <= 0x00d6) || (value >= 0x00d8 && value <= 0x00de)) return value + 0x20;
        if ((value >= 0x0391 && value <= 0x03a1) || (value >= 0x03a3 && value <= 0x03ab)) return value + 0x20;
        if (value == 0x03c2) return 0x03c3;
        if (value >= 0x0410 && value <= 0x042f) return value + 0x20;
        if (value >= 0x0400 && value <= 0x040f) return value + 0x50;
        if (value <= static_cast<uint32_t>(WCHAR_MAX))
        {
            static const std::locale locale = []
            {
                try { return std::locale(""); }
                catch (...) { return std::locale::classic(); }
            }();
            return static_cast<uint32_t>(std::use_facet<std::ctype<wchar_t>>(locale).tolower(static_cast<wchar_t>(value)));
        }
        return value;
    }

    static std::vector<Unit> Decode(std::string_view text)
    {
        std::vector<Unit> units;
        units.reserve(text.size());
        for (size_t i = 0; i < text.size();)
        {
            const auto first = static_cast<unsigned char>(text[i]);
            size_t length = first < 0x80 ? 1 : (first & 0xe0) == 0xc0 ? 2 :
                (first & 0xf0) == 0xe0 ? 3 : (first & 0xf8) == 0xf0 ? 4 : 1;
            uint32_t value = length == 1 ? first : first & (0x7f >> length);
            if (i + length > text.size()) { length = 1; value = first; }
            bool valid = length > 1;
            for (size_t j = 1; j < length; ++j)
            {
                const auto next = static_cast<unsigned char>(text[i + j]);
                if ((next & 0xc0) != 0x80) { valid = false; break; }
                value = (value << 6) | (next & 0x3f);
            }
            if (length > 1 && !valid) { length = 1; value = first; }
            units.push_back({Fold(value), i, i + length});
            i += length;
        }
        return units;
    }

    std::vector<uint32_t> m_query;
};
}
