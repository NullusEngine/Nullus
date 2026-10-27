#include "UIDocumentResourceResolver.h"

#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Core/Assets/NativeArtifactContainer.h"
#include "Core/Assets/ContentSource.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Core/ServiceLocator.h"
#include "Engine/Assets/RuntimeAssetDatabase.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/UI/UIFontAtlas.h"
#include "Rendering/UI/UiDrawDataSnapshot.h"

namespace NLS::Engine::UI
{
    namespace
    {
        std::mutex& ResolverStorageMutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        std::shared_ptr<IUIDocumentResourceResolver>& ResolverStorage()
        {
            static std::shared_ptr<IUIDocumentResourceResolver> resolver;
            return resolver;
        }

        struct ResourceKey
        {
            std::string assetId;
            std::string subAssetKey;

            friend bool operator==(const ResourceKey&, const ResourceKey&) = default;
        };

        struct ResourceKeyHash
        {
            size_t operator()(const ResourceKey& key) const noexcept
            {
                size_t hash = std::hash<std::string>{}(key.assetId);
                hash ^= std::hash<std::string>{}(key.subAssetKey) +
                    0x9e3779b9u + (hash << 6u) + (hash >> 2u);
                return hash;
            }
        };

        ResourceKey MakeKey(const UITextureReference& reference)
        {
            return { reference.GetAssetIdString(), reference.GetSubAssetKey() };
        }

        ResourceKey MakeKey(const UIFontReference& reference)
        {
            return { reference.GetAssetIdString(), reference.GetSubAssetKey() };
        }

        bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& output)
        {
            if (path.empty())
                return false;
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return false;
            output.assign(
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
            return !output.empty();
        }

        struct TextureEntry
        {
            std::string artifactPath;
            Render::Resources::Texture2D* texture = nullptr;
            std::shared_ptr<Render::RHI::RHITextureView> view;
            Render::UI::UiTextureId textureId {};
            UIResolvedTexture resolved {};
            uint32_t misses = 0u;
            bool failed = false;
        };

        struct FontEntry
        {
            uint64_t fontId = 0u;
            std::string artifactPath;
            bool loaded = false;
            bool failed = false;
        };

        class RuntimeUIDocumentResourceResolver final : public IUIDocumentResourceResolver
        {
        public:
            std::optional<UIResolvedTexture> ResolveTexture(
                const UITextureReference& reference) override
            {
                if (reference.IsNull())
                    return std::nullopt;

                auto& entry = m_textures[MakeKey(reference)];
                if (entry.failed)
                    return std::nullopt;
                if (entry.resolved.IsValid())
                    return entry.resolved;

                if (entry.artifactPath.empty())
                {
                    entry.artifactPath = ResolveArtifactPath(
                        reference.GetAssetId(),
                        reference.GetSubAssetKey(),
                        reference.GetResourcePath());
                    if (entry.artifactPath.empty())
                    {
                        entry.failed = true;
                        return std::nullopt;
                    }
                }

                if (!Core::ServiceLocator::Contains<
                        Core::ResourceManagement::TextureManager>())
                {
                    return std::nullopt;
                }

                auto& textureManager =
                    NLS_SERVICE(Core::ResourceManagement::TextureManager);
                if (entry.texture == nullptr)
                    entry.texture = textureManager.RequestAsyncArtifact(entry.artifactPath);
                if (entry.texture == nullptr)
                {
                    if (textureManager.IsAsyncArtifactLoadFailed(entry.artifactPath))
                    {
                        entry.failed = true;
                    }
                    else if (!textureManager.IsAsyncArtifactLoadPending(entry.artifactPath))
                    {
                        // A path that is neither pending nor failed (for
                        // example a non-artifact source path) would otherwise be
                        // retried forever. Give the manager a short grace period
                        // for first-use indexing, then stop.
                        if (++entry.misses > 120u)
                            entry.failed = true;
                    }
                    else
                    {
                        entry.misses = 0u;
                    }
                    return std::nullopt;
                }
                entry.misses = 0u;

                // The resource exists but its RHI upload may still be completing.
                if (entry.texture->GetTextureHandle() == nullptr)
                    return std::nullopt;

                const auto view =
                    entry.texture->GetOrCreateExplicitTextureView(entry.artifactPath);
                if (view == nullptr)
                {
                    entry.failed = true;
                    return std::nullopt;
                }

                auto* driver = Render::Context::TryGetLocatedDriver();
                if (driver == nullptr)
                    return std::nullopt;

                if (entry.view != view)
                {
                    if (entry.textureId.IsValid() && entry.view != nullptr)
                    {
                        Render::Context::DriverUIAccess::ReleaseUiTextureView(
                            *driver,
                            entry.view);
                    }
                    entry.view = view;
                    entry.textureId = Render::Context::DriverUIAccess::RegisterUiTextureView(
                        *driver,
                        view,
                        Render::UI::UiTextureSynchronizationScope::PreviousFrameOrStatic);
                }
                if (!entry.textureId.IsValid())
                {
                    entry.failed = true;
                    return std::nullopt;
                }

                entry.resolved = {
                    entry.textureId.value,
                    entry.textureId.generation,
                    entry.texture->width,
                    entry.texture->height };
                return entry.resolved;
            }

            std::optional<uint64_t> ResolveFont(
                const UIFontReference& reference) override
            {
                if (reference.IsNull())
                    return std::nullopt;

                auto& entry = m_fonts[MakeKey(reference)];
                if (entry.fontId == 0u)
                    entry.fontId = m_nextFontId++;
                if (entry.loaded)
                    return entry.fontId;
                if (entry.failed)
                    return std::nullopt;

                if (entry.artifactPath.empty())
                {
                    entry.artifactPath = ResolveArtifactPath(
                        reference.GetAssetId(),
                        reference.GetSubAssetKey(),
                        reference.GetResourcePath());
                    if (entry.artifactPath.empty())
                    {
                        entry.failed = true;
                        return std::nullopt;
                    }
                }

                std::vector<uint8_t> bytes;
                const auto resolvedFontPath =
                    Core::ResourceManagement::TextureManager::ResolveResourcePath(
                        entry.artifactPath);
                auto& contentSources = Core::Assets::ContentSourceTable::Get();
                bool read = contentSources.ReadAll(entry.artifactPath, bytes);
                if ((!read || bytes.empty()) && resolvedFontPath != entry.artifactPath)
                    read = contentSources.ReadAll(resolvedFontPath, bytes);
                if ((!read || bytes.empty()) && !ReadFileBytes(resolvedFontPath, bytes))
                {
                    entry.failed = true;
                    return std::nullopt;
                }
                if (bytes.empty())
                {
                    entry.failed = true;
                    return std::nullopt;
                }

                // Imported fonts are native artifacts (payload = TTF/OTF bytes);
                // editor recovery paths may still point directly at a source font.
                if (Core::Assets::IsNativeArtifactContainer(bytes))
                {
                    auto container = Core::Assets::ReadNativeArtifactContainer(
                        bytes,
                        Core::Assets::ArtifactType::Font,
                        1u);
                    if (!container.has_value())
                    {
                        entry.failed = true;
                        return std::nullopt;
                    }
                    bytes = std::move(container->payload);
                }

                if (!Render::UI::GetSharedUIFontAtlas().SetFaceData(
                        entry.fontId,
                        bytes.data(),
                        bytes.size()))
                {
                    entry.failed = true;
                    return std::nullopt;
                }

                entry.loaded = true;
                return entry.fontId;
            }

            void Pump() override
            {
                if (Core::ServiceLocator::Contains<
                        Core::ResourceManagement::TextureManager>())
                {
                    NLS_SERVICE(Core::ResourceManagement::TextureManager)
                        .PumpAsyncLoads(1u);
                }
            }

            void Invalidate(const UITextureReference& reference) override
            {
                const auto key = MakeKey(reference);
                const auto iterator = m_textures.find(key);
                if (iterator == m_textures.end())
                    return;

                ReleaseTextureEntry(iterator->second);
                m_textures.erase(iterator);
            }

            void Invalidate(const UIFontReference& reference) override
            {
                const auto iterator = m_fonts.find(MakeKey(reference));
                if (iterator == m_fonts.end())
                    return;

                if (iterator->second.fontId != 0u)
                {
                    Render::UI::GetSharedUIFontAtlas().InvalidateFont(
                        iterator->second.fontId,
                        0u);
                }
                iterator->second.artifactPath.clear();
                iterator->second.loaded = false;
                iterator->second.failed = false;
            }

            void InvalidateAsset(const Core::Assets::AssetId& assetId) override
            {
                if (!assetId.IsValid())
                    return;
                const auto id = assetId.ToString();
                for (auto iterator = m_textures.begin(); iterator != m_textures.end();)
                {
                    if (iterator->first.assetId == id)
                    {
                        ReleaseTextureEntry(iterator->second);
                        iterator = m_textures.erase(iterator);
                    }
                    else
                    {
                        ++iterator;
                    }
                }
                for (auto iterator = m_fonts.begin(); iterator != m_fonts.end();)
                {
                    if (iterator->first.assetId == id)
                    {
                        if (iterator->second.fontId != 0u)
                        {
                            Render::UI::GetSharedUIFontAtlas().InvalidateFont(
                                iterator->second.fontId,
                                0u);
                        }
                        iterator = m_fonts.erase(iterator);
                    }
                    else
                    {
                        ++iterator;
                    }
                }
            }

        private:
            static void ReleaseTextureEntry(TextureEntry& entry)
            {
                if (!entry.textureId.IsValid() || entry.view == nullptr)
                    return;
                if (auto* driver = Render::Context::TryGetLocatedDriver())
                {
                    Render::Context::DriverUIAccess::ReleaseUiTextureView(
                        *driver,
                        entry.view);
                }
                entry.textureId = {};
                entry.view.reset();
            }

            static std::string ResolveArtifactPath(
                const Core::Assets::AssetId& assetId,
                const std::string& subAssetKey,
                const std::string& fallbackPath)
            {
                if (Core::ServiceLocator::Contains<Assets::RuntimeAssetDatabase>())
                {
                    const auto& database = NLS_SERVICE(Assets::RuntimeAssetDatabase);
                    if (const auto* entry = database.Resolve(assetId, subAssetKey))
                        return entry->artifactPath;
                    // Authoring references may not record a sub-asset key.
                    if (subAssetKey.empty())
                    {
                        if (const auto* primary = database.ResolvePrimary(assetId))
                            return primary->artifactPath;
                    }
                }
                return fallbackPath;
            }

            std::unordered_map<ResourceKey, TextureEntry, ResourceKeyHash> m_textures;
            std::unordered_map<ResourceKey, FontEntry, ResourceKeyHash> m_fonts;
            uint64_t m_nextFontId = 2u;
        };
    }

    void SetUIDocumentResourceResolver(
        std::shared_ptr<IUIDocumentResourceResolver> resolver)
    {
        std::lock_guard lock(ResolverStorageMutex());
        ResolverStorage() = std::move(resolver);
    }

    std::shared_ptr<IUIDocumentResourceResolver> GetUIDocumentResourceResolver()
    {
        std::lock_guard lock(ResolverStorageMutex());
        return ResolverStorage();
    }

    void ClearUIDocumentResourceResolver()
    {
        SetUIDocumentResourceResolver(nullptr);
    }

    std::shared_ptr<IUIDocumentResourceResolver>
        CreateRuntimeUIDocumentResourceResolver()
    {
        return std::make_shared<RuntimeUIDocumentResourceResolver>();
    }

    std::shared_ptr<IUIDocumentResourceResolver>
        EnsureUIDocumentResourceResolver()
    {
        std::lock_guard lock(ResolverStorageMutex());
        if (!ResolverStorage())
            ResolverStorage() = std::make_shared<RuntimeUIDocumentResourceResolver>();
        return ResolverStorage();
    }
}
