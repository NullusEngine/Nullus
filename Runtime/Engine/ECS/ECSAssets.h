#pragma once
#include "EngineDef.h"
#include <ECS/Serialization/WorldDocumentSource.h>
#include <Core/Assets/ArtifactDatabase.h>
#include <Core/Assets/ArtifactWriter.h>
#include <Engine/Assets/RuntimeAssetDatabase.h>

namespace NLS::Engine::ECSIntegration
{
// Shared by the editor importer and the headless cooker. Component codecs are
// needed when instantiating, not when validating portable component documents.
NLS_ENGINE_API Core::Assets::ArtifactPayload CompileECSAsset(
    std::string_view text, ECS::DocumentKind expectedKind,
    const Core::Assets::ArtifactDatabase& database, std::string_view targetPlatform,
    const std::filesystem::path& projectRoot, ECS::ArchiveLimits limits = {});
NLS_ENGINE_API bool AreECSAssetResourcesCurrent(
    const Core::Assets::ArtifactManifest& manifest, const Core::Assets::ArtifactDatabase& database,
    std::string_view targetPlatform, const std::filesystem::path& projectRoot);

// Adapter over the existing immutable runtime asset database and content
// sources. Background document sources retain no World or engine object.
class NLS_ENGINE_API ECSAssetLoader final
{
public:
    ECSAssetLoader(std::shared_ptr<const Assets::RuntimeAssetDatabase> database, std::filesystem::path contentRoot);
    static ECSAssetLoader FromArtifactDatabase(const Core::Assets::ArtifactDatabase& database,
        const std::filesystem::path& projectRoot, std::string targetPlatform);
    ECS::ResourceDependency DescribeResource(const Assets::RuntimeAssetRef& reference) const;
    bool ResolveResource(const ECS::ResourceDependency& dependency) const;
    ECS::ArchiveLoadOptions MakeLoadOptions(uint64_t sceneOwner = 0, ECS::ArchiveLimits limits = {}) const;
    std::shared_ptr<const ECS::IWorldDocumentSource> CreateSource(const Assets::RuntimeAssetRef& reference) const;
    ECS::WorldDocument Read(const Assets::RuntimeAssetRef& reference, ECS::ArchiveLimits limits = {}) const;
    ECS::ArchiveInstance InstantiatePrefab(ECS::World& world, const Assets::RuntimeAssetRef& reference,
        std::span<const ECS::PrefabOverride> overrides = {}, uint64_t sceneOwner = 0, ECS::ArchiveLimits limits = {}) const;
private:
    const Assets::RuntimeAssetManifestEntry& Require(const Assets::RuntimeAssetRef& reference) const;
    std::shared_ptr<const Assets::RuntimeAssetDatabase> m_database;
    std::filesystem::path m_root;
};
}
