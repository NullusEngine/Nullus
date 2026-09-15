#pragma once

#include <algorithm>
#include <string>
#include <utility>

#include "Core/Assets/AssetId.h"
#include "EngineDef.h"
#include "Reflection/ExternalReflectionRegistration.h"

namespace NLS::Engine::UI
{
// Stable authoring reference to a UI document asset. The AssetId is the
// primary identity; resourcePath is retained for display and recovery while
// the asset database is rebuilding. It never owns a loaded document instance.
class NLS_ENGINE_API UIDocumentReference
{
public:
    UIDocumentReference() = default;

    UIDocumentReference(Core::Assets::AssetId assetId, std::string resourcePath)
        : m_assetId(std::move(assetId))
        , m_resourcePath(NormalizePath(std::move(resourcePath)))
    {
    }

    [[nodiscard]] const Core::Assets::AssetId& GetAssetId() const { return m_assetId; }
    [[nodiscard]] const std::string& GetResourcePath() const { return m_resourcePath; }
    [[nodiscard]] bool IsNull() const { return !m_assetId.IsValid() && m_resourcePath.empty(); }

    void SetAssetId(Core::Assets::AssetId assetId) { m_assetId = std::move(assetId); }
    void SetResourcePath(std::string resourcePath) { m_resourcePath = NormalizePath(std::move(resourcePath)); }
    void Reset()
    {
        m_assetId = {};
        m_resourcePath.clear();
    }

    // Reflection serializes the stable identifier as text so this value type
    // stays independent from AssetId's private representation.
    [[nodiscard]] std::string GetAssetIdString() const
    {
        return m_assetId.IsValid() ? m_assetId.ToString() : std::string {};
    }

    void SetAssetIdString(std::string assetId)
    {
        const auto parsed = Guid::TryParse(assetId);
        m_assetId = parsed.has_value() ? Core::Assets::AssetId(*parsed) : Core::Assets::AssetId {};
    }

    friend bool operator==(const UIDocumentReference&, const UIDocumentReference&) = default;

private:
    static std::string NormalizePath(std::string value)
    {
        std::replace(value.begin(), value.end(), '\\', '/');
        return value;
    }

    Core::Assets::AssetId m_assetId;
    std::string m_resourcePath;
};

NLS_META_EXTERNAL_TYPE_NAME(NLS::Engine::UI::UIDocumentReference)
} // namespace NLS::Engine::UI
