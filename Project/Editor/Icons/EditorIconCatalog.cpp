#include "Icons/EditorIconCatalog.h"
#include "Icons/Generated/IconCatalogGenerated.h"

#include <algorithm>
#include <utility>

namespace NLS::Editor::Icons
{

bool IconRegistry::IsValidId(const std::string_view p_id)
{
    return p_id.size() > IdPrefix.size() &&
        p_id.rfind(IdPrefix, 0) == 0;
}

bool IconRegistry::Register(IconDefinition p_definition)
{
    if (!IsValidId(p_definition.id))
        return false;
    if (p_definition.module.empty())
        return false;
    if (p_definition.kind == IconKind::Glyph && p_definition.glyph == 0)
        return false;
    if (p_definition.kind != IconKind::Glyph && p_definition.glyph != 0)
        return false;

    if (m_indexById.find(p_definition.id) != m_indexById.end())
        return false;

    for (const auto& alias : p_definition.aliases)
    {
        if (!IsValidId(alias) || m_indexById.find(alias) != m_indexById.end())
            return false;
    }

    const auto index = m_definitions.size();
    for (const auto& alias : p_definition.aliases)
        m_indexById.emplace(alias, index);
    m_indexById.emplace(p_definition.id, index);
    m_definitions.push_back(std::move(p_definition));
    ++m_generation;
    return true;
}

bool IconRegistry::UnregisterModule(const std::string_view p_module)
{
    if (p_module.empty())
        return false;

    std::size_t removed = 0;
    std::deque<IconDefinition> kept;
    for (auto& definition : m_definitions)
    {
        if (definition.module == p_module)
            ++removed;
        else
            kept.push_back(std::move(definition));
    }

    if (removed == 0)
        return false;

    m_definitions = std::move(kept);
    RebuildIndex();
    ++m_generation;
    return true;
}

const IconDefinition* IconRegistry::Find(const std::string_view p_id) const
{
    const auto found = m_indexById.find(std::string(p_id));
    return found != m_indexById.end() ? &m_definitions[found->second] : nullptr;
}

std::vector<char32_t> IconRegistry::UsedGlyphCodepoints() const
{
    std::vector<char32_t> codepoints;
    for (const auto& definition : m_definitions)
    {
        if (definition.kind == IconKind::Glyph && definition.glyph != 0)
            codepoints.push_back(definition.glyph);
    }
    std::sort(codepoints.begin(), codepoints.end());
    codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());
    return codepoints;
}

void IconRegistry::RebuildIndex()
{
    m_indexById.clear();
    for (std::size_t index = 0; index < m_definitions.size(); ++index)
    {
        const auto& definition = m_definitions[index];
        for (const auto& alias : definition.aliases)
            m_indexById.emplace(alias, index);
        m_indexById.emplace(definition.id, index);
    }
}

const std::vector<IconDefinition>& IconRegistry::DefaultDefinitions()
{
    return GeneratedIconDefinitions();
}

IconRegistry IconRegistry::CreateDefault()
{
    IconRegistry registry;
    for (const auto& definition : DefaultDefinitions())
        registry.Register(definition);
    return registry;
}

std::uint32_t IconTokenToLogicalPixels(const IconToken p_token)
{
    switch (p_token)
    {
    case IconToken::Tiny:    return 12;
    case IconToken::Small:   return 16;
    case IconToken::Default: return 20;
    case IconToken::Medium:  return 24;
    case IconToken::Large:   return 32;
    case IconToken::Huge:    return 48;
    }
    return 16;
}

const char* IconTokenName(const IconToken p_token)
{
    switch (p_token)
    {
    case IconToken::Tiny:    return "Tiny";
    case IconToken::Small:   return "Small";
    case IconToken::Default: return "Default";
    case IconToken::Medium:  return "Medium";
    case IconToken::Large:   return "Large";
    case IconToken::Huge:    return "Huge";
    }
    return "Small";
}

std::uint32_t SelectIconTier(
    const std::uint32_t p_requestedPixels,
    const std::vector<std::uint32_t>& p_availableTiers)
{
    if (p_availableTiers.empty())
        return p_requestedPixels;

    std::uint32_t bestAbove = 0;
    std::uint32_t largest = 0;
    for (const auto tier : p_availableTiers)
    {
        if (tier == 0)
            continue;
        largest = (std::max)(largest, tier);
        if (tier >= p_requestedPixels && (bestAbove == 0 || tier < bestAbove))
            bestAbove = tier;
    }

    if (bestAbove != 0)
        return bestAbove;
    return largest != 0 ? largest : p_requestedPixels;
}

void EncodeIconGlyphUtf8(const char32_t p_codepoint, char (&p_out)[5])
{
    p_out[0] = '\0';
    p_out[1] = '\0';
    p_out[2] = '\0';
    p_out[3] = '\0';
    p_out[4] = '\0';

    const auto codepoint = static_cast<std::uint32_t>(p_codepoint);
    if (codepoint <= 0x7f)
    {
        p_out[0] = static_cast<char>(codepoint);
    }
    else if (codepoint <= 0x7ff)
    {
        p_out[0] = static_cast<char>(0xc0 | (codepoint >> 6));
        p_out[1] = static_cast<char>(0x80 | (codepoint & 0x3f));
    }
    else if (codepoint <= 0xffff)
    {
        p_out[0] = static_cast<char>(0xe0 | (codepoint >> 12));
        p_out[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        p_out[2] = static_cast<char>(0x80 | (codepoint & 0x3f));
    }
    else
    {
        p_out[0] = static_cast<char>(0xf0 | (codepoint >> 18));
        p_out[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
        p_out[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        p_out[3] = static_cast<char>(0x80 | (codepoint & 0x3f));
    }
}

std::vector<char32_t> DefaultGlyphCodepoints()
{
    std::vector<char32_t> codepoints;
    for (const auto& definition : IconRegistry::DefaultDefinitions())
    {
        if (definition.kind == IconKind::Glyph && definition.glyph != 0)
            codepoints.push_back(definition.glyph);
    }
    std::sort(codepoints.begin(), codepoints.end());
    codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());
    return codepoints;
}
}
