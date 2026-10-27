#include "Icons/IconService.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace NLS::Editor::Icons
{
namespace
{
    constexpr std::string_view kDefaultAssetIconId = "editor.icon.asset.default";
    constexpr char kCacheKeySeparator = '\x1f';

    std::string MakeCacheKey(const std::string_view p_id, const std::uint32_t p_pixels)
    {
        std::string key(p_id);
        key.push_back(kCacheKeySeparator);
        key += std::to_string(p_pixels);
        return key;
    }

    // asset-scene@128.png + tier 32 -> asset-scene@32.png
    std::string MakeTierPath(std::string p_path, const std::uint32_t p_tier)
    {
        const auto at = p_path.rfind('@');
        if (at == std::string::npos)
            return p_path;
        const auto dot = p_path.find('.', at);
        if (dot == std::string::npos)
            return p_path;
        return p_path.substr(0, at + 1) + std::to_string(p_tier) + p_path.substr(dot);
    }
}

IconService::IconService(IconRegistry& p_registry, IconServiceBindings p_bindings)
    : m_registry(&p_registry)
    , m_bindings(std::move(p_bindings))
{
}

IconService::~IconService()
{
    Clear();
}

IconLoadResult IconService::Resolve(
    const IconId p_id,
    const std::uint32_t p_targetPixels,
    const bool p_countUsage)
{
    ++m_resolutionCount;
    const std::uint32_t pixels = (std::max)(p_targetPixels, 1u);
    const auto result = ResolveInternal(p_id.value, pixels);
    if (result.status == IconResolveStatus::Fallback)
        ++m_fallbackCount;

    if (p_countUsage)
    {
        ++m_usageCounts[std::string(p_id.value)];
        if (!result.resolvedId.empty() && result.resolvedId != p_id.value)
            ++m_usageCounts[result.resolvedId];
    }
    return result;
}

IconLoadResult IconService::ResolveInternal(
    const std::string_view p_id,
    const std::uint32_t p_pixels)
{
    if (m_registry == nullptr)
        return Fallback(p_pixels, IconResolveStatus::Missing);

    const IconDefinition* definition = m_registry->Find(p_id);
    if (definition == nullptr)
    {
        ReportOnce("unknown:" + std::string(p_id),
            "[IconService] Unknown icon \"" + std::string(p_id) + "\" falls back to the asset default.");
        return Fallback(p_pixels, IconResolveStatus::Fallback);
    }

    if (definition->kind == IconKind::Generated)
        return Fallback(p_pixels, IconResolveStatus::Fallback);

    if (definition->kind == IconKind::Glyph)
    {
        IconLoadResult result;
        result.resolvedId = definition->id;
        result.status = IconResolveStatus::Missing;
        return result;
    }

    const std::string cacheKey = MakeCacheKey(definition->id, p_pixels);
    if (const auto cached = m_entries.find(cacheKey); cached != m_entries.end())
    {
        ++m_cacheHitCount;
        IconLoadResult result;
        result.icon = cached->second.icon;
        result.resolvedId = definition->id;
        result.status = IconResolveStatus::Resolved;
        return result;
    }

    const auto path = m_bindings.resolvePath
        ? m_bindings.resolvePath(definition->id)
        : std::nullopt;
    if (!path.has_value() || path->empty())
    {
        ReportOnce("path:" + definition->id,
            "[IconService] Icon \"" + definition->id + "\" has no resolved path.");
        return Fallback(p_pixels, IconResolveStatus::Fallback);
    }

    const std::string basePath = path->string();
    std::uint32_t tierPixels = p_pixels;
    std::vector<std::uint32_t> tiers = definition->tiers;
    if (tiers.empty() && m_bindings.availableTiers)
        tiers = m_bindings.availableTiers(basePath);
    if (!tiers.empty())
        tierPixels = SelectIconTier(p_pixels, tiers);

    const std::string tierPath = MakeTierPath(basePath, tierPixels);
    std::string texturePath = tierPath;
    std::uint32_t texturePixels = tierPixels;
    float u0 = 0.0f;
    float v0 = 1.0f;
    float u1 = 1.0f;
    float v1 = 0.0f;
    bool useAtlas = false;

    if (m_bindings.findAtlasFrame && m_bindings.resolveAtlasPagePath)
    {
        if (const auto* frame = m_bindings.findAtlasFrame(definition->id, tierPixels))
        {
            const auto pagePath = m_bindings.resolveAtlasPagePath(frame->pageResourceId);
            if (pagePath.has_value())
            {
                const std::string candidate = pagePath->string();
                if (!m_bindings.fileExists || m_bindings.fileExists(candidate))
                {
                    texturePath = candidate;
                    texturePixels = frame->pageSize;
                    useAtlas = true;
                    // The loader flips the whole page vertically. Map each
                    // corner independently so the icon's top stays at the top.
                    u0 = frame->u0;
                    v0 = 1.0f - frame->v0;
                    u1 = frame->u1;
                    v1 = 1.0f - frame->v1;
                }
            }
        }
    }

    if (!useAtlas && m_bindings.fileExists && !m_bindings.fileExists(tierPath))
    {
        ReportOnce("file:" + definition->id,
            "[IconService] Icon \"" + definition->id + "\" is missing file " + tierPath + ".");
        return Fallback(p_pixels, IconResolveStatus::Fallback);
    }

    const auto loadTextureAt = [&](const std::string& p_texturePath, const std::uint32_t p_texturePixels) -> bool
    {
        if (m_textures.find(p_texturePath) != m_textures.end())
            return true;
        if (!m_bindings.loadTexture)
            return false;

        auto loaded = m_bindings.loadTexture(p_texturePath);
        if (!loaded.has_value() || !(*loaded))
            return false;

        m_textures.emplace(p_texturePath, std::move(*loaded));
        const auto textureBytes =
            static_cast<std::size_t>(p_texturePixels) * static_cast<std::size_t>(p_texturePixels) * 4u;
        m_textureBytesByPath[p_texturePath] = textureBytes;
        m_estimatedTextureBytes += textureBytes;
        return true;
    };

    if (!loadTextureAt(texturePath, texturePixels))
    {
        if (!useAtlas)
        {
            ReportOnce("load:" + definition->id,
                "[IconService] Failed to load icon \"" + definition->id + "\" from " + texturePath + ".");
            return Fallback(p_pixels, IconResolveStatus::Fallback);
        }

        // Atlas page unavailable: fall back to the standalone tier file.
        useAtlas = false;
        texturePath = tierPath;
        texturePixels = tierPixels;
        u0 = 0.0f;
        v0 = 1.0f;
        u1 = 1.0f;
        v1 = 0.0f;
        if ((m_bindings.fileExists && !m_bindings.fileExists(tierPath)) ||
            !loadTextureAt(texturePath, texturePixels))
        {
            ReportOnce("load:" + definition->id,
                "[IconService] Failed to load icon \"" + definition->id + "\" from " + texturePath + ".");
            return Fallback(p_pixels, IconResolveStatus::Fallback);
        }
    }

    const auto textureIterator = m_textures.find(texturePath);
    if (textureIterator == m_textures.end())
        return Fallback(p_pixels, IconResolveStatus::Fallback);

    CacheEntry entry;
    entry.path = texturePath;
    entry.module = definition->module;
    entry.icon.uvMin = Maths::Vector2(u0, v0);
    entry.icon.uvMax = Maths::Vector2(u1, v1);
    entry.icon.pixelSize = static_cast<float>(tierPixels);
    entry.icon.tintable = definition->kind == IconKind::Monochrome && definition->themeable;
    entry.icon.registryGeneration = m_registry->Generation();

    if (m_bindings.registerTextureView)
    {
        entry.icon.texture = m_bindings.registerTextureView(textureIterator->second.view);
    }

    if (!entry.icon.texture.IsValid())
    {
        ReportOnce("register:" + definition->id,
            "[IconService] Icon \"" + definition->id + "\" could not be registered with the UI texture registry.");
        return Fallback(p_pixels, IconResolveStatus::Fallback);
    }

    entry.icon.valid = true;
    m_entries.emplace(cacheKey, std::move(entry));

    IconLoadResult result;
    result.icon = m_entries.at(cacheKey).icon;
    result.resolvedId = definition->id;
    result.status = IconResolveStatus::Resolved;
    return result;
}

IconLoadResult IconService::Fallback(
    const std::uint32_t p_pixels,
    const IconResolveStatus p_status)
{
    if (m_registry == nullptr || m_registry->Find(kDefaultAssetIconId) == nullptr)
    {
        IconLoadResult missing;
        missing.status = IconResolveStatus::Missing;
        return missing;
    }

    const std::string cacheKey = MakeCacheKey(kDefaultAssetIconId, p_pixels);
    if (const auto cached = m_entries.find(cacheKey); cached != m_entries.end())
    {
        IconLoadResult result;
        result.icon = cached->second.icon;
        result.resolvedId = std::string(kDefaultAssetIconId);
        result.status = p_status;
        return result;
    }

    IconLoadResult result = ResolveInternal(kDefaultAssetIconId, p_pixels);
    if (result.status == IconResolveStatus::Resolved)
        result.status = p_status;
    return result;
}

GlyphIcon IconService::ResolveGlyph(
    const IconId p_id,
    const float p_logicalPixels,
    const bool p_countUsage)
{
    GlyphIcon glyph;
    if (m_registry == nullptr)
        return glyph;

    const IconDefinition* definition = m_registry->Find(p_id);
    if (definition == nullptr || definition->kind != IconKind::Glyph || definition->glyph == 0)
        return glyph;

    EncodeIconGlyphUtf8(definition->glyph, glyph.utf8);
    glyph.pixelSize = p_logicalPixels;
    glyph.valid = glyph.utf8[0] != '\0';
    if (glyph.valid && p_countUsage)
        ++m_usageCounts[definition->id];
    return glyph;
}

std::size_t IconService::Preload(
    const std::initializer_list<IconId> p_ids,
    const std::uint32_t p_targetPixels)
{
    std::size_t loaded = 0;
    for (const auto id : p_ids)
    {
        // Preloading is not usage; keep telemetry meaningful for deprecation.
        const auto result = ResolveInternal(id.value, p_targetPixels);
        if (result.status == IconResolveStatus::Resolved && result.icon.valid)
            ++loaded;
    }
    return loaded;
}

bool IconService::PreloadIdleStep(
    const std::uint32_t p_maxPerFrame,
    const std::uint32_t p_targetPixels)
{
    if (m_registry == nullptr)
        return false;

    const auto& definitions = m_registry->GetDefinitions();
    std::uint32_t loaded = 0;
    while (m_idlePreloadIndex < definitions.size() && loaded < p_maxPerFrame)
    {
        const auto& definition = definitions[m_idlePreloadIndex++];
        if (definition.kind == IconKind::Glyph || definition.kind == IconKind::Generated)
            continue;
        ResolveInternal(definition.id, p_targetPixels);
        ++loaded;
    }
    return m_idlePreloadIndex < definitions.size();
}

std::uint32_t IconService::GetUsageCount(const std::string_view p_id) const
{
    const auto found = m_usageCounts.find(std::string(p_id));
    return found != m_usageCounts.end() ? found->second : 0u;
}

void IconService::ResetUsageCounts()
{
    m_usageCounts.clear();
}

IconDiagnostics IconService::GetDiagnostics() const
{
    IconDiagnostics diagnostics;
    diagnostics.loadedTextureCount = m_textures.size();
    diagnostics.estimatedTextureBytes = m_estimatedTextureBytes;
    diagnostics.resolutionCount = m_resolutionCount;
    diagnostics.fallbackCount = m_fallbackCount;
    diagnostics.cacheHitCount = m_cacheHitCount;

    if (m_registry != nullptr)
    {
        for (const auto& definition : m_registry->GetDefinitions())
        {
            if (definition.kind == IconKind::Generated)
                continue;
            if (GetUsageCount(definition.id) == 0)
                ++diagnostics.unusedDefinitionCount;
        }
    }
    return diagnostics;
}

void IconService::InvalidateModule(const std::string_view p_module)
{
    for (auto iterator = m_entries.begin(); iterator != m_entries.end();)
    {
        if (iterator->second.module == p_module)
            iterator = m_entries.erase(iterator);
        else
            ++iterator;
    }
    RetireOrphanTextures();
}

void IconService::RetireOrphanTextures()
{
    for (auto iterator = m_textures.begin(); iterator != m_textures.end();)
    {
        const bool stillReferenced = std::any_of(
            m_entries.begin(),
            m_entries.end(),
            [&iterator](const auto& entry) { return entry.second.path == iterator->first; });

        if (stillReferenced)
        {
            ++iterator;
            continue;
        }

        m_retired.push_back(RetiredTexture{ std::move(iterator->second), m_frameId + 2u });
        if (const auto bytes = m_textureBytesByPath.find(iterator->first);
            bytes != m_textureBytesByPath.end())
        {
            m_estimatedTextureBytes -= bytes->second;
            m_textureBytesByPath.erase(bytes);
        }
        iterator = m_textures.erase(iterator);
    }
}

void IconService::SetFrameId(const std::uint64_t p_frameId)
{
    m_frameId = p_frameId;
}

void IconService::TickRetirement()
{
    for (auto iterator = m_retired.begin(); iterator != m_retired.end();)
    {
        if (iterator->releaseFrame > m_frameId)
        {
            ++iterator;
            continue;
        }

        if (iterator->texture.view && m_bindings.releaseTextureView)
            m_bindings.releaseTextureView(iterator->texture.view);
        if (iterator->texture.texture && m_bindings.destroyTexture)
            m_bindings.destroyTexture(iterator->texture.texture);

        iterator = m_retired.erase(iterator);
    }
}

void IconService::OnDeviceRecreated()
{
    // Device reset invalidates every texture and registry id. Drop the caches
    // without touching the registry so ids stay stable; the next Resolve
    // reloads and re-registers lazily.
    for (auto& [path, texture] : m_textures)
    {
        if (texture.texture && m_bindings.destroyTexture)
            m_bindings.destroyTexture(texture.texture);
    }
    for (auto& retired : m_retired)
    {
        if (retired.texture.texture && m_bindings.destroyTexture)
            m_bindings.destroyTexture(retired.texture.texture);
    }

    m_textures.clear();
    m_textureBytesByPath.clear();
    m_estimatedTextureBytes = 0;
    m_retired.clear();
    m_entries.clear();
    m_reported.clear();
}

void IconService::Clear()
{
    for (auto& [path, texture] : m_textures)
    {
        if (texture.view && m_bindings.releaseTextureView)
            m_bindings.releaseTextureView(texture.view);
        if (texture.texture && m_bindings.destroyTexture)
            m_bindings.destroyTexture(texture.texture);
    }
    for (auto& retired : m_retired)
    {
        if (retired.texture.view && m_bindings.releaseTextureView)
            m_bindings.releaseTextureView(retired.texture.view);
        if (retired.texture.texture && m_bindings.destroyTexture)
            m_bindings.destroyTexture(retired.texture.texture);
    }

    m_textures.clear();
    m_textureBytesByPath.clear();
    m_estimatedTextureBytes = 0;
    m_retired.clear();
    m_entries.clear();
}

void IconService::ReportOnce(const std::string& p_key, const std::string& p_message)
{
    if (!m_reported.insert(p_key).second)
        return;
    if (m_bindings.logWarning)
        m_bindings.logWarning(p_message);
}
}
