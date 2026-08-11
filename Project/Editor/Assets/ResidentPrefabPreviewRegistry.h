#pragma once

#include "Assets/AssetThumbnailCache.h"
#include "Assets/PreviewRenderableSnapshot.h"
#include "Core/ResourceManagement/ResourceHandle.h"
#include "Core/ResourceManagement/ResourceLifetimeRegistry.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Resources/Texture2D.h"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace NLS::Core::ResourceManagement
{
class MaterialManager;
class MeshManager;
class TextureManager;
}

namespace NLS::Engine
{
class GameObject;
}

namespace NLS::Editor::Assets
{
/// Immutable handles to the runtime resources already owned by a scene.
///
/// The snapshot describes what to draw; this package describes which manager
/// instances can draw it without issuing another artifact request. Handles are
/// deliberately kept in one shared object so a GPU readback can retain the
/// exact scene resources until submission retirement.
struct ResidentPrefabPreviewResources final
{
    using MeshHandle = NLS::Core::ResourceManagement::ResourceHandle<
        NLS::Render::Resources::Mesh>;
    using MaterialHandle = NLS::Core::ResourceManagement::ResourceHandle<
        NLS::Render::Resources::Material>;
    using TextureHandle = NLS::Core::ResourceManagement::ResourceHandle<
        NLS::Render::Resources::Texture2D>;

    struct DrawItem final
    {
        size_t meshIndex = SIZE_MAX;
        std::vector<size_t> materialIndices;
    };

    uint64_t meshManagerInstanceId = 0u;
    uint64_t materialManagerInstanceId = 0u;
    uint64_t textureManagerInstanceId = 0u;
    std::vector<MeshHandle> meshes;
    // Scene restore can keep a ready mesh in MeshFilter without registering it
    // in MeshManager. Keep that shared owner alongside the indexed handle so
    // a resident thumbnail can reuse the exact loaded mesh without I/O.
    std::unordered_map<size_t, std::shared_ptr<NLS::Render::Resources::Mesh>> transientMeshesByIndex;
    std::vector<MaterialHandle> materials;
    std::vector<TextureHandle> textures;
    // A scene instance can expose a ready mesh/material before its texture
    // uploads have completed. Such a package is valid for an in-memory preview
    // but must remain provisional until the missing bindings are attached.
    bool hasUnresolvedMaterialBindings = false;
    bool hasUnresolvedTextureBindings = false;
    std::unordered_map<std::string, size_t> meshIndicesByPath;
    std::unordered_map<std::string, size_t> materialIndicesByPath;
    std::unordered_map<std::string, size_t> textureIndicesByPath;
    std::unordered_map<std::string, size_t> textureIndicesByRequestedPath;
    std::vector<DrawItem> drawItems;
    // A live scene instance may expose only the resources that have finished
    // resolving so far. Keep the source topology count with the package so a
    // partial preview can be shown immediately without being persisted as the
    // final thumbnail.
    size_t sourceDrawItemCount = 0u;
    size_t sourceExpectedDrawItemCount = 0u;

    [[nodiscard]] bool IsCompleteForSource() const
    {
        if (drawItems.empty())
            return false;

        if (hasUnresolvedMaterialBindings || hasUnresolvedTextureBindings)
            return false;

        // A live scene package may contain only the resources that have
        // finished resolving so far. Compare the ready package against the
        // canonical source count, not against the source snapshot count that
        // was used to build this package. When older callers do not provide a
        // source count, a non-empty package is the only available completion
        // signal and remains compatible with the legacy resource fixtures.
        if (sourceExpectedDrawItemCount != 0u)
            return drawItems.size() >= sourceExpectedDrawItemCount;
        if (sourceDrawItemCount != 0u)
            return drawItems.size() >= sourceDrawItemCount;
        return true;
    }

    [[nodiscard]] bool IsValidFor(
        const NLS::Core::ResourceManagement::MeshManager& meshManager,
        const NLS::Core::ResourceManagement::MaterialManager& materialManager,
        const NLS::Core::ResourceManagement::TextureManager* textureManager) const;
};

class ResidentPrefabPreviewRegistry final
    : public std::enable_shared_from_this<ResidentPrefabPreviewRegistry>
{
public:
    struct Stats
    {
        struct ThumbnailRequestIdentityCount
        {
            std::string identity;
            size_t count = 0u;
        };

        size_t entryCount = 0u;
        size_t activeLeaseCount = 0u;
        size_t residentBytes = 0u;
        size_t hitCount = 0u;
        size_t missCount = 0u;
        size_t staleCount = 0u;
        size_t zeroArtifactReadHitCount = 0u;
        size_t evictionCount = 0u;
        size_t thumbnailHitCount = 0u;
        size_t thumbnailMissCount = 0u;
        size_t thumbnailStaleCount = 0u;
        size_t thumbnailZeroArtifactReadHitCount = 0u;
        size_t thumbnailIdentityMissCount = 0u;
        size_t thumbnailFreshnessMismatchCount = 0u;
        size_t thumbnailRequestCount = 0u;
        size_t thumbnailRequestOtherIdentityCount = 0u;
        std::vector<ThumbnailRequestIdentityCount> thumbnailRequestIdentityCounts;
        std::string lastRegisteredIdentity;
        std::string lastRegisteredLookupIdentity;
        std::string lastRegisteredFreshness;
        std::string lastThumbnailRequestedIdentity;
        std::string lastThumbnailRequestedFreshness;
        std::string lastThumbnailKnownIdentity;
        std::string lastThumbnailKnownFreshness;
        std::string lastThumbnailMismatchIdentity;
        std::string lastThumbnailMismatchRequestedFreshness;
        std::string lastThumbnailMismatchKnownFreshness;
    };

    struct SnapshotState
    {
        uint64_t revision = 0u;
        size_t readyDrawItemCount = 0u;
        size_t expectedDrawItemCount = 0u;
        bool complete = false;
        // Import-published snapshots already contain the complete Prefab graph,
        // but do not own scene GPU handles. They may resolve mesh/material
        // artifacts without reopening the Prefab payload.
        bool allowArtifactResourceLoading = false;
    };

    class Lease final
    {
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        ~Lease();

        [[nodiscard]] const std::shared_ptr<const PreviewRenderableSnapshot>& Snapshot() const
        {
            return m_snapshot;
        }

        [[nodiscard]] const std::shared_ptr<const ResidentPrefabPreviewResources>& Resources() const
        {
            return m_resources;
        }

        [[nodiscard]] std::shared_ptr<const std::vector<uint8_t>>
            TakePreparedMeshPayload(const std::string& artifactPath) const
        {
            return m_takePreparedMeshPayload
                ? m_takePreparedMeshPayload(artifactPath)
                : nullptr;
        }

        [[nodiscard]] explicit operator bool() const
        {
            return m_snapshot != nullptr;
        }

    private:
        friend class ResidentPrefabPreviewRegistry;
        Lease(
            std::shared_ptr<const PreviewRenderableSnapshot> snapshot,
            std::shared_ptr<const ResidentPrefabPreviewResources> resources,
            std::function<std::shared_ptr<const std::vector<uint8_t>>(const std::string&)>
                takePreparedMeshPayload,
            std::function<void()> release);
        void Reset();

        std::shared_ptr<const PreviewRenderableSnapshot> m_snapshot;
        std::shared_ptr<const ResidentPrefabPreviewResources> m_resources;
        std::function<std::shared_ptr<const std::vector<uint8_t>>(const std::string&)>
            m_takePreparedMeshPayload;
        std::function<void()> m_release;
    };

    explicit ResidentPrefabPreviewRegistry(size_t inactiveBudgetBytes = 64ull * 1024ull * 1024ull);

    static std::shared_ptr<ResidentPrefabPreviewRegistry> Create(
        size_t inactiveBudgetBytes = 64ull * 1024ull * 1024ull);

    void RegisterSnapshot(
        std::string runtimeCacheIdentity,
        std::string freshnessFingerprint,
        std::shared_ptr<const PreviewRenderableSnapshot> snapshot,
        size_t byteSize,
        bool acquireSceneLease = false,
        std::string lookupIdentity = {},
        std::string lookupFreshnessFingerprint = {},
        std::shared_ptr<const ResidentPrefabPreviewResources> resources = {},
        bool allowArtifactResourceLoading = false,
        std::vector<PreparedPrefabPreviewMeshPayload> preparedMeshPayloads = {},
        bool importedThumbnailPending = false);

    // Retains the complete graph produced by a successful model import. The
    // entry is published only after artifacts and ArtifactDB metadata commit,
    // so thumbnail generation can skip a second Prefab graph read and load only
    // the render dependencies that are not already resident.
    bool RegisterImportedPrefabSnapshot(
        const std::filesystem::path& projectRoot,
        const NLS::Core::Assets::AssetId& assetId,
        const std::string& sourceAssetPath,
        const std::string& prefabSubAssetKey,
        const std::string& artifactPath,
        std::shared_ptr<const PreviewRenderableSnapshot> snapshot,
        std::vector<PreparedPrefabPreviewMeshPayload> preparedMeshPayloads = {});

    // Records process-local scheduling identity for the complete import
    // snapshot above so hidden assets can finish their canonical PNG.
    bool RegisterImportedPrefabThumbnailContinuation(
        const std::filesystem::path& projectRoot,
        const NLS::Core::Assets::AssetId& assetId,
        const std::string& sourceAssetPath,
        const std::string& prefabSubAssetKey,
        const std::string& artifactPath);

    // Import snapshots and their scheduling identities never cross a process
    // boundary. A restarted editor waits for a scene load or a new import.
    [[nodiscard]] std::vector<ImportedPrefabThumbnailContinuation>
        GetImportedPrefabThumbnailContinuations(
            const std::filesystem::path& projectRoot) const;
    [[nodiscard]] bool HasImportedPrefabThumbnailContinuation(
        const std::filesystem::path& projectRoot,
        const NLS::Core::Assets::AssetId& assetId,
        const std::string& sourceAssetPath) const;
    [[nodiscard]] uint64_t GetImportedPrefabThumbnailContinuationRevision(
        const std::filesystem::path& projectRoot,
        const NLS::Core::Assets::AssetId& assetId,
        const std::string& sourceAssetPath) const;
    void CompleteImportedPrefabThumbnailContinuation(
        const std::filesystem::path& projectRoot,
        const NLS::Core::Assets::AssetId& assetId);

    // Builds and registers a canonical snapshot from a PrefabArtifact, then
    // returns the scene lease that keeps it resident until scene unload.
    [[nodiscard]] std::optional<Lease> RegisterPrefabSnapshotForScene(
        const std::filesystem::path& projectRoot,
        const NLS::Core::Assets::AssetId& assetId,
        const std::string& sourceAssetPath,
        const std::string& subAssetKey,
        const std::string& artifactPath,
        const std::string& runtimeCacheIdentity,
        const NLS::Engine::Assets::PrefabArtifact& prefab);

    // Ensures a scene-owned snapshot exists before attaching resources from a
    // live instance. The loaded PrefabArtifact supplies canonical topology;
    // the live objects supply the already-resolved runtime handles. Empty
    // artifact/runtime identities are supported so a scene instance remains a
    // usable zero-I/O thumbnail source even when it was not created through
    // the prepared prefab cache.
    [[nodiscard]] std::optional<Lease> EnsureLivePrefabSnapshotForScene(
        const std::filesystem::path& projectRoot,
        const NLS::Core::Assets::AssetId& assetId,
        const std::string& sourceAssetPath,
        const std::string& subAssetKey,
        const std::string& artifactPath,
        const std::string& runtimeCacheIdentity,
        const NLS::Engine::Assets::PrefabArtifact& prefab,
        const std::unordered_map<
            const NLS::Engine::GameObject*,
            NLS::Engine::Serialize::ObjectId>& sourceByInstanceObject,
        NLS::Core::ResourceManagement::MeshManager& meshManager,
        NLS::Core::ResourceManagement::MaterialManager& materialManager,
        NLS::Core::ResourceManagement::TextureManager* textureManager,
        NLS::Core::ResourceManagement::ResourceLifetimeRegistry& resourceLifetimeRegistry);

    [[nodiscard]] std::optional<Lease> Acquire(
        const std::string& runtimeCacheIdentity,
        const std::string& freshnessFingerprint,
        bool thumbnailRequest = false,
        bool sceneLease = false);

    // Publishes resources that are already registered in the supplied managers.
    // This function never calls a loading API. When scene restoration is still
    // resolving dependencies it publishes a progressively complete snapshot;
    // later probes rebuild it from the retained canonical source snapshot.
    bool AttachRegisteredResourcesForPrefab(
        const NLS::Core::Assets::AssetId& assetId,
        const NLS::Engine::Assets::PrefabArtifact& prefab,
        NLS::Core::ResourceManagement::MeshManager& meshManager,
        NLS::Core::ResourceManagement::MaterialManager& materialManager,
        NLS::Core::ResourceManagement::TextureManager* textureManager,
        NLS::Core::ResourceManagement::ResourceLifetimeRegistry& resourceLifetimeRegistry);

    // Publishes handles from the live instance immediately after prefab
    // instantiation. This is a no-I/O probe: it never starts an artifact load
    // and does not wait for unrelated scene renderer tasks.
    bool AttachRegisteredResourcesForLivePrefab(
        const NLS::Core::Assets::AssetId& assetId,
        const std::unordered_map<
            const NLS::Engine::GameObject*,
            NLS::Engine::Serialize::ObjectId>& sourceByInstanceObject,
        NLS::Core::ResourceManagement::MeshManager& meshManager,
        NLS::Core::ResourceManagement::MaterialManager& materialManager,
        NLS::Core::ResourceManagement::TextureManager* textureManager,
        NLS::Core::ResourceManagement::ResourceLifetimeRegistry& resourceLifetimeRegistry);
    [[nodiscard]] bool HasSnapshotForRuntimeCacheIdentity(
        const std::string& runtimeCacheIdentity) const;

    // Records that a thumbnail request carried a resident identity. This is
    // intentionally separate from Acquire so telemetry can distinguish a
    // missing request source from a stale or unavailable snapshot.
    void RecordThumbnailRequest(
        const std::string& runtimeCacheIdentity,
        const std::string& freshnessFingerprint);

    [[nodiscard]] std::weak_ptr<const PreviewRenderableSnapshot> FindWeakSnapshot(
        const std::string& runtimeCacheIdentity,
        const std::string& freshnessFingerprint) const;

    [[nodiscard]] std::optional<SnapshotState> GetSnapshotState(
        const std::string& runtimeCacheIdentity,
        const std::string& freshnessFingerprint) const;

    // Asset Browser samples this once per frame. It changes only when a
    // thumbnail-relevant resident state becomes newly usable or disappears.
    [[nodiscard]] uint64_t GetThumbnailWakeRevision() const
    {
        return m_thumbnailWakeRevision.load(std::memory_order_acquire);
    }

    // Asset Browser can draw progress frames while a scene is still restoring
    // prefab instances. A resident miss during that interval is provisional:
    // do not fall back to another Prefab artifact read until registration has
    // had a chance to publish the scene snapshot.
    void SetSceneRestoreInProgress(bool inProgress);
    [[nodiscard]] bool IsSceneRestoreInProgress() const;

    void Remove(const std::string& runtimeCacheIdentity, const std::string& freshnessFingerprint);
    void SetInactiveBudgetBytes(size_t budgetBytes);
    [[nodiscard]] Stats GetStats() const;

private:
    struct Entry
    {
        std::string runtimeCacheIdentity;
        std::string freshnessFingerprint;
        // The effective snapshot may be a partial, immediately renderable
        // view while the scene is still resolving. Keep the canonical source
        // separately so later probes can add newly ready draw items.
        std::shared_ptr<const PreviewRenderableSnapshot> sourceSnapshot;
        std::shared_ptr<const PreviewRenderableSnapshot> snapshot;
        std::shared_ptr<const ResidentPrefabPreviewResources> resources;
        std::unordered_map<std::string, std::shared_ptr<const std::vector<uint8_t>>>
            preparedMeshPayloads;
        size_t snapshotByteSize = 0u;
        size_t preparedMeshPayloadBytes = 0u;
        uint64_t revision = 1u;
        uint64_t lastUsedTick = 0u;
        size_t activeLeaseCount = 0u;
        size_t sceneLeaseCount = 0u;
        bool allowArtifactResourceLoading = false;
        // Keep the complete imported topology until its canonical thumbnail is
        // finished. Prepared mesh bytes may still be trimmed under pressure.
        bool importedThumbnailPending = false;

        [[nodiscard]] size_t TotalByteSize() const
        {
            return snapshotByteSize + preparedMeshPayloadBytes;
        }
    };

    static std::string MakeKey(
        const std::string& runtimeCacheIdentity,
        const std::string& freshnessFingerprint);
    static std::string MakeImportedThumbnailContinuationKey(
        const std::filesystem::path& projectRoot,
        const NLS::Core::Assets::AssetId& assetId);
    std::string ResolveKeyLocked(const std::string& identity, const std::string& freshnessFingerprint) const;
    void RemoveAliasesForKeyLocked(const std::string& key);
    void ReleaseLease(const std::string& key, bool sceneLease);
    std::shared_ptr<const std::vector<uint8_t>> TakePreparedMeshPayload(
        const std::string& key,
        const std::string& artifactPath);
    void EvictInactiveLocked(const std::string* protectedKey = nullptr);

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, Entry> m_entries;
    std::unordered_map<std::string, ImportedPrefabThumbnailContinuation>
        m_importedThumbnailContinuations;
    // Asset Browser requests know the stable asset/sub-asset identity, while
    // scene restore knows the full prepared-cache runtime identity. Keep an
    // alias map so requests acquire the exact entry without duplicating the
    // snapshot or its byte accounting.
    std::unordered_map<std::string, std::string> m_aliases;
    size_t m_inactiveBudgetBytes = 64ull * 1024ull * 1024ull;
    size_t m_residentBytes = 0u;
    uint64_t m_tick = 0u;
    size_t m_hitCount = 0u;
    size_t m_missCount = 0u;
    mutable size_t m_staleCount = 0u;
    size_t m_zeroArtifactReadHitCount = 0u;
    size_t m_evictionCount = 0u;
    size_t m_thumbnailHitCount = 0u;
    size_t m_thumbnailMissCount = 0u;
    mutable size_t m_thumbnailStaleCount = 0u;
    size_t m_thumbnailZeroArtifactReadHitCount = 0u;
    size_t m_thumbnailIdentityMissCount = 0u;
    size_t m_thumbnailFreshnessMismatchCount = 0u;
    size_t m_thumbnailRequestCount = 0u;
    size_t m_thumbnailRequestOtherIdentityCount = 0u;
    std::unordered_map<std::string, size_t> m_thumbnailRequestIdentityCounts;
    std::string m_lastRegisteredIdentity;
    std::string m_lastRegisteredLookupIdentity;
    std::string m_lastRegisteredFreshness;
    std::string m_lastThumbnailRequestedIdentity;
    std::string m_lastThumbnailRequestedFreshness;
    std::string m_lastThumbnailKnownIdentity;
    std::string m_lastThumbnailKnownFreshness;
    std::string m_lastThumbnailMismatchIdentity;
    std::string m_lastThumbnailMismatchRequestedFreshness;
    std::string m_lastThumbnailMismatchKnownFreshness;
    std::atomic_uint64_t m_thumbnailWakeRevision {0u};
    std::atomic_bool m_sceneRestoreInProgress {false};
};

std::string BuildResidentPrefabRuntimeCacheIdentity(
    const std::string& assetId,
    const std::string& subAssetKey);

// Asset Browser can derive the primary prefab sub-resource key when an
// importer/scene record does not carry one explicitly. Keep that derivation
// shared with scene registration so a resident snapshot is discoverable by
// the later thumbnail request.
std::string BuildCanonicalPrefabPreviewSubAssetKey(
    const std::string& sourceAssetPath,
    const std::string& subAssetKey);
}
