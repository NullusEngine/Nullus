#include "ECSAssets.h"
#include <Core/Assets/ContentSource.h>
#include <Core/Assets/NativeArtifactContainer.h>
#include <ECS/Core/World.h>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace NLS::Engine::ECSIntegration
{
namespace
{
using namespace Core::Assets;
constexpr size_t MaximumMetadataBytes = 16 * 1024 * 1024;
std::filesystem::path HostPath(const std::filesystem::path& root, const std::string& path)
{
    const std::filesystem::path value(path);
    return value.is_absolute() ? value.lexically_normal() : (root / value).lexically_normal();
}
bool Present(const ArtifactDatabaseRecord& record, const std::filesystem::path& root)
{
    std::error_code error;
    return !record.artifactPath.empty() && std::filesystem::is_regular_file(HostPath(root, record.artifactPath), error);
}
const ArtifactDatabaseRecord& FindResource(const ArtifactDatabase& database, const ECS::ResourceDependency& dependency,
    std::string_view target, const std::filesystem::path& root)
{
    const auto guid = Guid::TryParse(dependency.guid);
    if (!guid || !guid->IsValid()) throw std::invalid_argument("Invalid ECS resource GUID: " + dependency.guid);
    const AssetId id(*guid);
    const ArtifactDatabaseRecord* selected = nullptr;
    int best = -1;
    auto candidates = database.FindBySource(id);
    if (candidates.empty())
        for (const auto& record : database.GetRecords())
            if (record.artifactSourceAssetId == id) candidates.push_back(&record);
    for (const auto* candidate : candidates)
    {
        const auto& record = *candidate;
        if ((record.sourceAssetId != id && record.artifactSourceAssetId != id) || record.status != ArtifactRecordStatus::UpToDate ||
            record.contentHash.empty() || (!dependency.hashOrVersion.empty() && record.contentHash != dependency.hashOrVersion) ||
            !Assets::IsRuntimeAssetPlatformCompatible(record.artifactType, record.targetPlatform, std::string(target))) continue;
        const int score = (record.targetPlatform == target ? 4 : 0) + (record.primarySubAssetKey == record.subAssetKey ? 2 : 0);
        if (score > best && Present(record, root)) { selected = &record; best = score; }
    }
    if (!selected) throw std::invalid_argument("Missing, stale or mismatched ECS resource for target " + std::string(target) + ": " + dependency.guid);
    return *selected;
}
struct ContentInput
{
    std::shared_ptr<const IContentSource> source;
    std::string path;
};
ContentInput ResolveContent(const Assets::RuntimeAssetManifestEntry& entry, const std::filesystem::path& root)
{
    const auto portable = TryMakePortableContentArtifactPath(entry.artifactPath);
    if (portable.empty()) throw std::invalid_argument("ECS assets require native content-addressed artifacts");
    if (entry.contentHash != "sha256:" + std::filesystem::path(entry.artifactPath).filename().generic_string())
        throw std::invalid_argument("ECS artifact storage key does not match its manifest hash");
    const auto hostPath = HostPath(root, entry.artifactPath);
    if (!IsRuntimeArtifactPathAuthorized(portable)) throw std::invalid_argument("ECS artifact is absent from runtime authorization: " + entry.artifactPath);
    if (auto mounted = ContentSourceTable::Get().ResolveContentPath(hostPath)) return {std::move(mounted->source), std::move(mounted->relativePath)};
    return {std::make_shared<DirectoryContentSource>(hostPath.parent_path()), hostPath.filename().generic_string()};
}
class NativeDocumentSource final : public ECS::IWorldDocumentSource
{
public:
    NativeDocumentSource(Assets::RuntimeAssetManifestEntry entry, std::string target, ContentInput input)
        : m_entry(std::move(entry)), m_target(std::move(target)), m_input(std::move(input)) {}
    ECS::WorldDocument Read(ECS::ArchiveLimits limits) const override
    {
        const auto size = m_input.source->GetSize(m_input.path);
        const auto overhead = MaximumMetadataBytes + NativeArtifactContainerHeaderSize();
        if (!size || *size > std::numeric_limits<size_t>::max() ||
            (*size > limits.maxBytes && *size - limits.maxBytes > overhead))
            throw std::length_error("Missing or oversized ECS native artifact");
        std::vector<uint8_t> bytes(static_cast<size_t>(*size)); std::string error;
        if (!m_input.source->ReadAt(m_input.path, 0, bytes.data(), bytes.size(), &error)) throw std::runtime_error("Read ECS artifact: " + error);
        const auto hash = "sha256:" + ToHexString(ComputeSha256(bytes.data(), bytes.size()));
        if (m_entry.contentHash != hash) throw std::invalid_argument("ECS artifact content hash mismatch");
        auto view = ReadNativeArtifactContainerView(bytes, m_entry.artifactType, 1, &error);
        if (!view) throw std::invalid_argument("Invalid ECS native artifact: " + error);
        const auto& metadata = view->metadata;
        if (metadata.sourceAssetId != m_entry.assetId || metadata.subAssetKey != m_entry.subAssetKey ||
            metadata.schemaName != m_entry.loaderId || metadata.targetPlatform != m_target)
            throw std::invalid_argument("ECS artifact identity, schema or platform mismatch");
        if (view->payloadSize > limits.maxBytes || static_cast<size_t>(view->payloadData - bytes.data()) > overhead)
            throw std::length_error("ECS native payload or metadata limit exceeded");
        auto document = ECS::WorldArchive::Parse(std::string_view(reinterpret_cast<const char*>(view->payloadData), view->payloadSize), limits);
        const auto expected = m_entry.artifactType == ArtifactType::ECSScene ? ECS::DocumentKind::Scene : ECS::DocumentKind::Prefab;
        if (document.kind != expected) throw std::invalid_argument("ECS artifact document kind mismatch");
        return document;
    }
private:
    const Assets::RuntimeAssetManifestEntry m_entry;
    const std::string m_target;
    const ContentInput m_input;
};
}
Core::Assets::ArtifactPayload CompileECSAsset(std::string_view text, ECS::DocumentKind expectedKind,
    const Core::Assets::ArtifactDatabase& database, std::string_view targetPlatform,
    const std::filesystem::path& projectRoot, ECS::ArchiveLimits limits)
{
    using namespace Core::Assets;
    if (targetPlatform.empty()) throw std::invalid_argument("ECS asset compilation needs a target platform");
    const auto document = ECS::WorldArchive::Parse(text, limits);
    if (document.kind != expectedKind) throw std::invalid_argument("ECS source extension/type does not match its document kind");
    ArtifactPayload result;
    result.artifactType = expectedKind == ECS::DocumentKind::Scene ? ArtifactType::ECSScene : ArtifactType::ECSPrefab;
    result.loaderId = expectedKind == ECS::DocumentKind::Scene ? "ecs-scene" : "ecs-prefab";
    result.subAssetKey = result.loaderId + ":main"; result.relativePath = result.loaderId;
    result.dependencies.push_back({AssetDependencyKind::PostprocessorVersion, "nullus-ecs-asset", "1"});
    for (const auto& resource : document.resources)
    {
        const auto& record = FindResource(database, resource, targetPlatform, projectRoot);
        result.dependencies.push_back({AssetDependencyKind::ImportedArtifact, resource.guid, record.subAssetKey});
        result.dependencies.push_back({AssetDependencyKind::SourceAssetGuid, resource.guid, record.contentHash});
    }
    const auto canonical = ECS::WorldArchive::Encode(document, limits);
    result.payload.assign(canonical.begin(), canonical.end());
    return result;
}
bool AreECSAssetResourcesCurrent(const Core::Assets::ArtifactManifest& manifest, const Core::Assets::ArtifactDatabase& database,
    std::string_view targetPlatform, const std::filesystem::path& projectRoot)
{
    try
    {
        bool compilerMatches = false;
        for (const auto& dependency : manifest.dependencies)
        {
            if (dependency.kind == Core::Assets::AssetDependencyKind::PostprocessorVersion &&
                dependency.value == "nullus-ecs-asset" && dependency.hashOrVersion == "1") compilerMatches = true;
            if (dependency.kind == Core::Assets::AssetDependencyKind::SourceAssetGuid)
                (void)FindResource(database, {dependency.value, dependency.hashOrVersion}, targetPlatform, projectRoot);
        }
        return compilerMatches;
    }
    catch (...) { return false; }
}
ECSAssetLoader::ECSAssetLoader(std::shared_ptr<const Assets::RuntimeAssetDatabase> database, std::filesystem::path contentRoot)
    : m_database(std::move(database)), m_root(std::filesystem::absolute(contentRoot))
{
    if (!m_database || m_database->GetManifest().targetPlatform.empty()) throw std::invalid_argument("ECS assets need a runtime database and target platform");
}
ECSAssetLoader ECSAssetLoader::FromArtifactDatabase(const Core::Assets::ArtifactDatabase& database,
    const std::filesystem::path& projectRoot, std::string targetPlatform)
{
    using namespace Core::Assets;
    Assets::RuntimeAssetManifest manifest; manifest.targetPlatform = std::move(targetPlatform);
    std::unordered_set<Assets::RuntimeAssetRef, Assets::RuntimeAssetRefHash> seen;
    seen.reserve(database.GetRecords().size()); manifest.entries.reserve(database.GetRecords().size());
    // Prefer exact-target records. The native runtime resolver already owns the
    // GUID/sub-asset index; this adapter does not maintain another database.
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& record : database.GetRecords())
        {
            const bool exact = record.targetPlatform == manifest.targetPlatform;
            if ((pass == 0) != exact || record.status != ArtifactRecordStatus::UpToDate ||
                !Assets::IsRuntimeAssetPlatformCompatible(record.artifactType, record.targetPlatform, manifest.targetPlatform)) continue;
            const auto id = record.artifactSourceAssetId.IsValid() ? record.artifactSourceAssetId : record.sourceAssetId;
            if (!seen.insert({id, record.subAssetKey}).second) continue;
            manifest.entries.push_back({id, record.subAssetKey, record.artifactType, record.loaderId,
                HostPath(std::filesystem::absolute(projectRoot), record.artifactPath).generic_string(), record.contentHash, {}});
        }
    return ECSAssetLoader(std::make_shared<Assets::RuntimeAssetDatabase>(std::move(manifest)), projectRoot);
}
const Assets::RuntimeAssetManifestEntry& ECSAssetLoader::Require(const Assets::RuntimeAssetRef& reference) const
{
    const auto* entry = reference.subAssetKey.empty() ? m_database->ResolvePrimary(reference.assetId) : m_database->Resolve(reference);
    if (!entry) throw std::invalid_argument("Missing ECS runtime asset: " + reference.assetId.ToString() + "/" + reference.subAssetKey);
    return *entry;
}
ECS::ResourceDependency ECSAssetLoader::DescribeResource(const Assets::RuntimeAssetRef& reference) const
{
    const auto& entry = Require(reference);
    ECS::ResourceDependency result{entry.assetId.ToString(), entry.contentHash};
    if (result.hashOrVersion.empty() || !ResolveResource(result)) throw std::invalid_argument("ECS resource has no available versioned artifact");
    return result;
}
bool ECSAssetLoader::ResolveResource(const ECS::ResourceDependency& dependency) const
{
    const auto guid = Guid::TryParse(dependency.guid);
    if (!guid || !guid->IsValid()) return false;
    const Core::Assets::AssetId id(*guid);
    for (const auto& entry : m_database->GetManifest().entries)
        if (entry.assetId == id && !entry.contentHash.empty() && (dependency.hashOrVersion.empty() || entry.contentHash == dependency.hashOrVersion))
        {
            try
            {
                const auto input = ResolveContent(entry, m_root);
                if (input.source->Exists(input.path)) return true;
            }
            catch (...) {}
        }
    return false;
}
ECS::ArchiveLoadOptions ECSAssetLoader::MakeLoadOptions(uint64_t sceneOwner, ECS::ArchiveLimits limits) const
{
    ECS::ArchiveLoadOptions options; options.sceneOwner = sceneOwner; options.limits = limits;
    options.resolveResource = [loader = *this](const auto& dependency) { return loader.ResolveResource(dependency); };
    return options;
}
std::shared_ptr<const ECS::IWorldDocumentSource> ECSAssetLoader::CreateSource(const Assets::RuntimeAssetRef& reference) const
{
    const auto& entry = Require(reference);
    using Core::Assets::ArtifactType;
    if ((entry.artifactType != ArtifactType::ECSScene && entry.artifactType != ArtifactType::ECSPrefab) ||
        entry.loaderId != (entry.artifactType == ArtifactType::ECSScene ? "ecs-scene" : "ecs-prefab"))
        throw std::invalid_argument("Runtime asset is not an ECS Scene/Prefab artifact");
    return std::make_shared<NativeDocumentSource>(entry, m_database->GetManifest().targetPlatform, ResolveContent(entry, m_root));
}
ECS::WorldDocument ECSAssetLoader::Read(const Assets::RuntimeAssetRef& reference, ECS::ArchiveLimits limits) const
{ return CreateSource(reference)->Read(limits); }
ECS::ArchiveInstance ECSAssetLoader::InstantiatePrefab(ECS::World& world, const Assets::RuntimeAssetRef& reference,
    std::span<const ECS::PrefabOverride> overrides, uint64_t sceneOwner, ECS::ArchiveLimits limits) const
{ return ECS::WorldArchive::InstantiatePrefab(world, Read(reference, limits), overrides, MakeLoadOptions(sceneOwner, limits)); }
}
