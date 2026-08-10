#pragma once

#include "Assets/AssetId.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Editor::Assets
{
class AssetDatabaseFacade;
class ResidentPrefabPreviewRegistry;
struct PreviewRenderableSnapshot;

enum class AssetThumbnailKind
{
    Icon,
    Texture,
    MaterialSphere,
    ModelPreview,
    PrefabPreview,
    GenericPreview,
    Count
};

inline constexpr size_t kAssetThumbnailKindCount = static_cast<size_t>(AssetThumbnailKind::Count);

enum class AssetThumbnailCacheStatus
{
    Fresh,
    Stale,
    Missing,
    Failed,
    Count
};

inline constexpr size_t kAssetThumbnailCacheStatusCount = static_cast<size_t>(AssetThumbnailCacheStatus::Count);

enum class AssetThumbnailCacheIntegrityMode
{
    Fast,
    Full
};

enum class ThumbnailRequestPriority
{
    Background,
    Inspector,
    Prefetch,
    Visible
};

struct AssetThumbnailFreshnessInput
{
    std::string name;
    std::string stamp;
};

struct ResidentPrefabPreviewSource
{
    std::string runtimeCacheIdentity;
    std::string freshnessFingerprint;
    std::weak_ptr<const PreviewRenderableSnapshot> snapshot;
    std::weak_ptr<ResidentPrefabPreviewRegistry> registry;
    bool allowArtifactResourceLoading = false;

    [[nodiscard]] bool HasIdentity() const
    {
        return !runtimeCacheIdentity.empty() && !freshnessFingerprint.empty();
    }
};

struct AssetThumbnailRequest
{
    std::filesystem::path projectRoot;
    // Optional diagnostic cache root. Empty preserves the project-local
    // Library/AssetThumbnails location. Custom roots must still remain
    // physically contained by projectRoot.
    std::filesystem::path cacheRoot;
    NLS::Core::Assets::AssetId assetId;
    std::string sourceAssetPath;
    std::string subAssetKey;
    std::string artifactPath;
    AssetThumbnailKind kind = AssetThumbnailKind::GenericPreview;
    uint32_t requestedSize = 96u;
    std::string previewRendererVersion;
    std::string settingsFingerprint;
    std::string dependencyStamp;
    std::string colorSpaceMode;
    std::string hdrMode;
    // Diagnostic-only scheduling switch. It is deliberately excluded from
    // both cache identities so A/B runs share the same visual contract.
    bool enableReadbackRing = false;
    // Diagnostic-only lifetime switch. The renderer uses transient preview
    // objects when disabled and the serialised proxy pool when enabled.
    bool enablePreviewProxyPool = false;
    // Resource continuation is allowed to use a larger bounded slice when
    // adaptive thumbnail scheduling is enabled. This is scheduling state only
    // and is excluded from both visual cache identities.
    uint32_t previewResourcePumpBudgetMicroseconds = 1000u;
    // Stable display identity. It deliberately excludes freshness and dependency
    // stamps so an old canonical image can remain visible while a new generation
    // is being prepared.
    std::string presentationKey;
    // Monotonically increasing revision used by asynchronous result/persistence
    // commit checks within one editor process. Zero means the service will
    // assign one on first enqueue.
    uint64_t requestRevision = 0u;
    // Process-local ordering must never be compared across editor restarts.
    // The service assigns a non-zero session identity together with the first
    // request revision. Zero preserves legacy/test callers that assign their
    // own revisions.
    uint64_t requestSessionId = 0u;
    // The request builder has already verified that the source image is a
    // bounded, directly readable texture. This is scheduling metadata only;
    // it is excluded from both visual cache identities.
    bool directSourceTexture = false;
    ThumbnailRequestPriority priority = ThumbnailRequestPriority::Background;
    bool generatedSubAsset = false;
    std::vector<AssetThumbnailFreshnessInput> freshnessInputs;
    // The Asset Browser owns this immutable snapshot; it prevents background preview
    // preparation from reopening ArtifactDB for an asset that was just enumerated.
    std::shared_ptr<const AssetDatabaseFacade> assetDatabaseSnapshot;
    std::optional<ResidentPrefabPreviewSource> residentPrefabPreviewSource;
    // A successful import published this Prefab for thumbnail generation in
    // the current process. The matching resident snapshot remains the only
    // authorization to load render dependencies for a large Prefab.
    bool importedPrefabThumbnailContinuation = false;
    // Process-local registration revision for the continuation above. It lets
    // one explicit reimport supersede an older negative thumbnail cache entry
    // without turning a new terminal failure into an automatic retry loop.
    uint64_t importedPrefabThumbnailContinuationRevision = 0u;
    // Resident scene resources are attached incrementally. These fields are
    // scheduling/presentation state only; they are deliberately excluded from
    // the durable cache key so a loading scene does not create one cache file
    // per draw-item progress step.
    uint64_t residentPreviewRevision = 0u;
    bool residentPreviewPartial = false;
};

struct ImportedPrefabThumbnailContinuation
{
    std::filesystem::path projectRoot;
    NLS::Core::Assets::AssetId assetId;
    std::string sourceAssetPath;
    std::string prefabSubAssetKey;
    std::string artifactPath;
    // Scheduling state assigned by the process-local registry.
    uint64_t registrationRevision = 0u;
};

struct AssetThumbnailCacheEntry
{
    std::string cacheKey;
    std::filesystem::path imagePath;
    std::filesystem::path metadataPath;
};

struct AssetThumbnailCacheEvaluation
{
    AssetThumbnailCacheStatus status = AssetThumbnailCacheStatus::Missing;
    std::optional<AssetThumbnailCacheEntry> entry;
    std::optional<bool> freshnessCurrent;
    std::string diagnostic;
};

struct AssetThumbnailPresentationIndexEntry
{
    std::string cacheKey;
    std::filesystem::path imagePath;
    std::filesystem::path metadataPath;
    uint64_t requestRevision = 0u;
    uint64_t requestSessionId = 0u;
    // True only when every freshness input can be independently validated.
    bool freshnessCurrent = false;
    // Opaque identity inputs are excluded, but every source/artifact stamp that
    // can be re-read from disk or the artifact database is still current.
    bool verifiableFreshnessCurrent = false;
    // Cross-session recovery requires positive filesystem/database evidence;
    // an entry containing only opaque identity inputs cannot be revived merely
    // because there was nothing independently verifiable.
    bool hasVerifiableFreshnessInputs = false;
    std::string freshnessDiagnostic;
};

struct AssetThumbnailPresentationIndex
{
    std::string presentationKey;
    std::optional<AssetThumbnailPresentationIndexEntry> current;
    std::optional<AssetThumbnailPresentationIndexEntry> previous;
    uint64_t committedRevision = 0u;
    uint64_t committedSessionId = 0u;
};

struct AssetThumbnailDiskCachePruneOptions
{
    size_t maxEntries = 1024u;
    uint64_t maxBytes = 256ull * 1024ull * 1024ull;
};

struct AssetThumbnailDiskCachePruneResult
{
    size_t scannedEntries = 0u;
    size_t removedEntries = 0u;
    uint64_t removedBytes = 0u;
    size_t remainingEntries = 0u;
    uint64_t remainingBytes = 0u;
};


std::string BuildAssetThumbnailCacheKey(const AssetThumbnailRequest& request);

std::string BuildAssetThumbnailPresentationKey(const AssetThumbnailRequest& request);

std::optional<AssetThumbnailPresentationIndex> ReadAssetThumbnailPresentationIndex(
    const AssetThumbnailRequest& request);

bool CommitAssetThumbnailPresentation(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEntry& canonicalEntry,
    uint64_t requestRevision);

// Removes an image/metadata candidate only when neither generation of its
// presentation index references it. This closes the cancellation/freshness
// race after an image has been atomically written but before its canonical
// metadata/index commit.
bool DiscardUnreferencedAssetThumbnailCacheCandidate(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEntry& candidate);

// Removes both generations referenced by the stable presentation index and
// the current freshness entry. This is used when an asset is deleted; a
// rename deliberately does not call it because presentation keys are path
// independent.
bool RemoveAssetThumbnailPresentation(const AssetThumbnailRequest& request);

// Removes all thumbnail cache records whose source asset path is the supplied
// path, or a descendant when includeDescendants is true. The cache root must
// be physically contained by projectRoot.
size_t RemoveAssetThumbnailCachesForSourcePath(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& cacheRoot,
    const std::string& sourceAssetPath,
    bool includeDescendants);

std::optional<AssetThumbnailCacheEntry> ResolveAssetThumbnailCacheEntryPathForRead(
    const AssetThumbnailRequest& request);

std::optional<AssetThumbnailCacheEntry> ResolveAssetThumbnailCacheEntry(
    const AssetThumbnailRequest& request);

#if defined(NLS_ENABLE_TEST_HOOKS)
void ResetAssetThumbnailCacheCanonicalPathAttemptCountForTesting();
size_t GetAssetThumbnailCacheCanonicalPathAttemptCountForTesting();
void ResetAssetThumbnailCacheContainmentStampAttemptCountForTesting();
size_t GetAssetThumbnailCacheContainmentStampAttemptCountForTesting();
void ResetAssetThumbnailCacheMetadataFileLoadCountForTesting();
size_t GetAssetThumbnailCacheMetadataFileLoadCountForTesting();
size_t GetAssetThumbnailCacheMetadataCacheEntryCountForTesting();
void ResetAssetThumbnailCacheEvaluationCountForTesting();
size_t GetAssetThumbnailCacheEvaluationCountForTesting();
#endif

bool IsAssetThumbnailCachePathContained(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& candidate);

const std::array<AssetThumbnailCacheStatus, kAssetThumbnailCacheStatusCount>& AssetThumbnailCacheStatusValues();
const char* AssetThumbnailCacheStatusStorageToken(AssetThumbnailCacheStatus status);
std::optional<AssetThumbnailCacheStatus> AssetThumbnailCacheStatusFromStorageToken(
    const std::string& value);

AssetThumbnailCacheEvaluation EvaluateAssetThumbnailCache(
    const AssetThumbnailRequest& request,
    AssetThumbnailCacheIntegrityMode integrityMode = AssetThumbnailCacheIntegrityMode::Full);

bool WriteAssetThumbnailCacheMetadata(
    const AssetThumbnailRequest& request,
    AssetThumbnailCacheStatus status,
    const std::string& diagnostic);
bool WriteAssetThumbnailCacheMetadata(
    const AssetThumbnailRequest& metadataRequest,
    const AssetThumbnailCacheEntry& entry,
    AssetThumbnailCacheStatus status,
    const std::string& diagnostic);

// Removes only the independent negative-cache marker. The current/previous
// presentation generations and their canonical PNGs are left untouched.
bool ClearAssetThumbnailCacheFailureMetadata(
    const AssetThumbnailRequest& metadataRequest,
    const AssetThumbnailCacheEntry& entry);

bool WriteAssetThumbnailCacheFile(
    const AssetThumbnailRequest& request,
    const std::filesystem::path& path,
    const std::vector<uint8_t>& bytes);

AssetThumbnailDiskCachePruneResult PruneAssetThumbnailDiskCache(
    const std::filesystem::path& projectRoot,
    const AssetThumbnailDiskCachePruneOptions& options);

}
