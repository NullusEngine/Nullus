#pragma once

#include <string>
#include <utility>

#include "Core/Assets/AssetId.h"
#include "EngineDef.h"
#include "Reflection/ExternalReflectionRegistration.h"

namespace NLS::Engine::UI
{
    // Authoring reference to a project texture/font asset. The AssetId is the
    // durable identity; subAssetKey selects an imported sub-resource; the path
    // is retained for display and recovery only and is never used for identity.
    //
    // The runtime-resolved texture id / font id stay transient and are filled by
    // IUIDocumentResourceResolver; they are not authored in new documents.
    class NLS_ENGINE_API UITextureReference
    {
    public:
        UITextureReference() = default;
        UITextureReference(
            Core::Assets::AssetId assetId,
            std::string subAssetKey,
            std::string resourcePath)
            : m_assetId(std::move(assetId))
            , m_subAssetKey(std::move(subAssetKey))
            , m_resourcePath(NormalizePath(std::move(resourcePath)))
        {
        }

        [[nodiscard]] const Core::Assets::AssetId& GetAssetId() const { return m_assetId; }
        [[nodiscard]] const std::string& GetSubAssetKey() const { return m_subAssetKey; }
        [[nodiscard]] const std::string& GetResourcePath() const { return m_resourcePath; }
        [[nodiscard]] bool IsNull() const
        {
            return !m_assetId.IsValid() && m_subAssetKey.empty() && m_resourcePath.empty();
        }

        [[nodiscard]] std::string GetAssetIdString() const
        {
            return m_assetId.IsValid() ? m_assetId.ToString() : std::string {};
        }
        void SetAssetIdString(std::string assetId)
        {
            const auto parsed = Guid::TryParse(assetId);
            m_assetId = parsed.has_value() ? Core::Assets::AssetId(*parsed) : Core::Assets::AssetId {};
        }
        void SetAssetId(Core::Assets::AssetId assetId) { m_assetId = std::move(assetId); }
        void SetSubAssetKey(std::string subAssetKey) { m_subAssetKey = std::move(subAssetKey); }
        void SetResourcePath(std::string resourcePath)
        {
            m_resourcePath = NormalizePath(std::move(resourcePath));
        }
        void Reset()
        {
            m_assetId = {};
            m_subAssetKey.clear();
            m_resourcePath.clear();
        }

        friend bool operator==(const UITextureReference&, const UITextureReference&) = default;

    private:
        static std::string NormalizePath(std::string value)
        {
            for (auto& character : value)
            {
                if (character == '\\')
                    character = '/';
            }
            return value;
        }

        Core::Assets::AssetId m_assetId;
        std::string m_subAssetKey;
        std::string m_resourcePath;
    };

    class NLS_ENGINE_API UIFontReference
    {
    public:
        UIFontReference() = default;
        UIFontReference(
            Core::Assets::AssetId assetId,
            std::string subAssetKey,
            std::string resourcePath)
            : m_assetId(std::move(assetId))
            , m_subAssetKey(std::move(subAssetKey))
            , m_resourcePath(NormalizePath(std::move(resourcePath)))
        {
        }

        [[nodiscard]] const Core::Assets::AssetId& GetAssetId() const { return m_assetId; }
        [[nodiscard]] const std::string& GetSubAssetKey() const { return m_subAssetKey; }
        [[nodiscard]] const std::string& GetResourcePath() const { return m_resourcePath; }
        [[nodiscard]] bool IsNull() const
        {
            return !m_assetId.IsValid() && m_subAssetKey.empty() && m_resourcePath.empty();
        }

        [[nodiscard]] std::string GetAssetIdString() const
        {
            return m_assetId.IsValid() ? m_assetId.ToString() : std::string {};
        }
        void SetAssetIdString(std::string assetId)
        {
            const auto parsed = Guid::TryParse(assetId);
            m_assetId = parsed.has_value() ? Core::Assets::AssetId(*parsed) : Core::Assets::AssetId {};
        }
        void SetAssetId(Core::Assets::AssetId assetId) { m_assetId = std::move(assetId); }
        void SetSubAssetKey(std::string subAssetKey) { m_subAssetKey = std::move(subAssetKey); }
        void SetResourcePath(std::string resourcePath)
        {
            m_resourcePath = NormalizePath(std::move(resourcePath));
        }
        void Reset()
        {
            m_assetId = {};
            m_subAssetKey.clear();
            m_resourcePath.clear();
        }

        friend bool operator==(const UIFontReference&, const UIFontReference&) = default;

    private:
        static std::string NormalizePath(std::string value)
        {
            for (auto& character : value)
            {
                if (character == '\\')
                    character = '/';
            }
            return value;
        }

        Core::Assets::AssetId m_assetId;
        std::string m_subAssetKey;
        std::string m_resourcePath;
    };
}

NLS_META_EXTERNAL_TYPE_NAME(NLS::Engine::UI::UITextureReference)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Engine::UI::UIFontReference)
