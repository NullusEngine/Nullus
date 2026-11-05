#include "PluginAssetPath.h"
#include <algorithm>
#include <stdexcept>
#include <tuple>

namespace NLS::Core::Assets
{
namespace
{
struct UnicodeMapping { uint32_t point, size, points[3]; };
struct UnicodeCombining { uint32_t point, value; };
struct UnicodeComposition { uint32_t first, second, composed; };
#include "PluginUnicodeData.inl"
template<class T, size_t N> const T* Lookup(const T (&table)[N], uint32_t cp)
{
    auto it = std::lower_bound(std::begin(table), std::end(table), cp, [](const T& v, uint32_t p) { return v.point < p; });
    return it != std::end(table) && it->point == cp ? it : nullptr;
}
uint32_t Combining(uint32_t cp) { auto* p = Lookup(kCombiningClasses, cp); return p ? p->value : 0; }
std::vector<uint32_t> Decode(std::string_view input)
{
    std::vector<uint32_t> result;
    for (size_t i = 0; i < input.size();)
    {
        const auto first = static_cast<uint8_t>(input[i++]);
        uint32_t cp = first, remaining = 0, minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { cp &= 31; remaining = 1; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { cp &= 15; remaining = 2; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { cp &= 7; remaining = 3; minimum = 0x10000; }
        else if (first >= 0x80) throw std::invalid_argument("invalid UTF-8 plugin path");
        for (uint32_t n = 0; n < remaining; ++n)
        {
            if (i >= input.size() || (static_cast<uint8_t>(input[i]) & 0xc0) != 0x80) throw std::invalid_argument("invalid UTF-8 plugin path");
            cp = (cp << 6) | (static_cast<uint8_t>(input[i++]) & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || cp < 32 || cp == 127)
            throw std::invalid_argument("invalid character in plugin path");
        result.push_back(cp);
    }
    return result;
}
void Decompose(uint32_t cp, std::vector<uint32_t>& output)
{
    if (cp >= 0xac00 && cp <= 0xd7a3)
    {
        const auto index = cp - 0xac00; output.push_back(0x1100 + index / 588); output.push_back(0x1161 + (index % 588) / 28);
        if (index % 28) output.push_back(0x11a7 + index % 28); return;
    }
    if (auto* map = Lookup(kDecompositions, cp))
        for (uint32_t i = 0; i < map->size; ++i) Decompose(map->points[i], output);
    else output.push_back(cp);
}
uint32_t Compose(uint32_t a, uint32_t b)
{
    if (a >= 0x1100 && a <= 0x1112 && b >= 0x1161 && b <= 0x1175) return 0xac00 + ((a - 0x1100) * 21 + b - 0x1161) * 28;
    if (a >= 0xac00 && a <= 0xd7a3 && (a - 0xac00) % 28 == 0 && b >= 0x11a8 && b <= 0x11c2) return a + b - 0x11a7;
    const auto pair = std::pair{a, b};
    auto it = std::lower_bound(std::begin(kCompositions), std::end(kCompositions), pair,
        [](const auto& p, const auto& key) { return std::pair{p.first, p.second} < key; });
    return it != std::end(kCompositions) && it->first == a && it->second == b ? it->composed : 0;
}
std::vector<uint32_t> Normalize(const std::vector<uint32_t>& input)
{
    std::vector<uint32_t> decomposed;
    for (auto cp : input) Decompose(cp, decomposed);
    for (size_t i = 1; i < decomposed.size(); ++i)
        for (size_t j = i; j > 0 && Combining(decomposed[j]) && Combining(decomposed[j - 1]) > Combining(decomposed[j]); --j)
            std::swap(decomposed[j], decomposed[j - 1]);
    std::vector<uint32_t> output; size_t starter = 0; uint32_t previousClass = 0;
    for (auto cp : decomposed)
    {
        const auto cls = Combining(cp);
        const auto combined = output.empty() ? 0 : Compose(output[starter], cp);
        if (combined && (previousClass == 0 || previousClass < cls)) output[starter] = combined;
        else
        {
            if (cls == 0) starter = output.size();
            output.push_back(cp); previousClass = cls;
        }
    }
    return output;
}
std::string Encode(const std::vector<uint32_t>& points)
{
    std::string result;
    for (auto cp : points)
    {
        if (cp < 0x80) result += static_cast<char>(cp);
        else if (cp < 0x800) { result += static_cast<char>(0xc0 | (cp >> 6)); result += static_cast<char>(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { result += static_cast<char>(0xe0 | (cp >> 12)); result += static_cast<char>(0x80 | ((cp >> 6) & 63)); result += static_cast<char>(0x80 | (cp & 63)); }
        else { result += static_cast<char>(0xf0 | (cp >> 18)); result += static_cast<char>(0x80 | ((cp >> 12) & 63)); result += static_cast<char>(0x80 | ((cp >> 6) & 63)); result += static_cast<char>(0x80 | (cp & 63)); }
    }
    return result;
}
}
std::string NormalizePluginAssetPath(std::string_view path)
{
    auto normalized = Encode(Normalize(Decode(path)));
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.empty() || normalized.front() == '/' || normalized.find(':') != std::string::npos) throw std::invalid_argument("plugin path must be relative");
    std::string result; size_t begin = 0;
    while (begin <= normalized.size())
    {
        auto end = normalized.find('/', begin); auto part = normalized.substr(begin, end == std::string::npos ? end : end - begin);
        if (part == "..") throw std::invalid_argument("plugin path escapes mount");
        if (!part.empty() && part != ".")
        {
            if (part.back() == '.' || part.back() == ' ') throw std::invalid_argument("non-portable plugin path suffix");
            if (part.find_first_of("<>\"|?*") != std::string::npos) throw std::invalid_argument("non-portable plugin path character");
            auto stem = part.substr(0,part.find('.'));
            std::transform(stem.begin(),stem.end(),stem.begin(),[](unsigned char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : static_cast<char>(c); });
            if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
                (stem.size() == 4 && (stem.rfind("COM",0) == 0 || stem.rfind("LPT",0) == 0) && stem[3] >= '1' && stem[3] <= '9'))
                throw std::invalid_argument("reserved platform filename in plugin path");
            if (!result.empty()) result += '/'; result += part;
        }
        if (end == std::string::npos) break; begin = end + 1;
    }
    if (result.empty()) throw std::invalid_argument("empty plugin asset path"); return result;
}
std::string PluginAssetCollisionKey(std::string_view path)
{
    std::vector<uint32_t> folded;
    for (auto cp : Decode(NormalizePluginAssetPath(path)))
        if (const auto* map = Lookup(kCaseFolds, cp)) folded.insert(folded.end(), map->points, map->points + map->size);
        else folded.push_back(cp);
    return Encode(Normalize(folded));
}
Guid PluginAssetGuid(std::string_view id, std::string_view mount, std::string_view path)
{
    return Guid::NewDeterministic("nullus.plugin.asset.v1:" + std::string(id) + ":" + std::string(mount) + ":" + NormalizePluginAssetPath(path));
}
}
