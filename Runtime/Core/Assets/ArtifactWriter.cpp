#include "Assets/ArtifactWriter.h"
#include "Assets/NativeArtifactContainer.h"
#include "Profiling/PerformanceStageStats.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <optional>
#include <set>
namespace NLS::Core::Assets
{
namespace
{
struct CommitPlan
{
    std::filesystem::path sourcePath;
    std::filesystem::path destinationPath;
    std::filesystem::path backupPath;
    bool destinationExists = false;
    bool destinationBackedUp = false;
    bool destinationMayContainReplacement = false;
    bool replacementMoved = false;
};

struct StagedArtifact
{
    std::filesystem::path relativePath;
    bool destinationExists = false;
    bool destinationIsDirectory = false;
};

bool IsSafeRelativePath(const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory())
        return false;

    for (const auto& part : path)
    {
        if (part == ".." || part == ".")
            return false;
    }
    return true;
}

std::filesystem::path NormalizedAbsolute(const std::filesystem::path& path)
{
    return std::filesystem::absolute(path).lexically_normal();
}

std::filesystem::path BackupRootFor(const std::filesystem::path& stagingRoot)
{
    auto backupRoot = stagingRoot;
    backupRoot += ".rollback";
    return backupRoot;
}

bool IsSameOrNestedPath(
    const std::filesystem::path& candidate,
    const std::filesystem::path& root)
{
    const auto normalizedCandidate = candidate.lexically_normal();
    const auto normalizedRoot = root.lexically_normal();
    auto candidateIt = normalizedCandidate.begin();
    auto rootIt = normalizedRoot.begin();

    for (; rootIt != normalizedRoot.end(); ++rootIt, ++candidateIt)
    {
        if (candidateIt == normalizedCandidate.end() || *candidateIt != *rootIt)
            return false;
    }

    return true;
}

bool HasUnsafeRootRelationship(
    const std::filesystem::path& stagingRoot,
    const std::filesystem::path& committedRoot)
{
    if (stagingRoot.empty() || committedRoot.empty())
        return true;
    return IsSameOrNestedPath(stagingRoot, committedRoot) ||
        IsSameOrNestedPath(committedRoot, stagingRoot);
}

bool ExistingContentAddressedBlobMatchesExpectedPayload(
    const std::filesystem::path& path,
    const std::vector<uint8_t>& expectedPayload,
    std::error_code& error)
{
    error.clear();
    const auto fileSize = std::filesystem::file_size(path, error);
    if (error || fileSize != expectedPayload.size())
        return false;

    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;

    std::array<char, 64u * 1024u> buffer {};
    size_t offset = 0u;
    while (offset < expectedPayload.size())
    {
        const auto remaining = expectedPayload.size() - offset;
        const auto chunkSize = std::min(buffer.size(), remaining);
        input.read(buffer.data(), static_cast<std::streamsize>(chunkSize));
        if (input.gcount() != static_cast<std::streamsize>(chunkSize))
            return false;

        const auto* expectedBegin = reinterpret_cast<const char*>(expectedPayload.data() + offset);
        if (!std::equal(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(chunkSize), expectedBegin))
            return false;

        offset += chunkSize;
    }
    return true;
}

void AddError(
    ArtifactWriteResult& result,
    const AssetId assetId,
    const std::filesystem::path& path,
    std::string code,
    std::string message)
{
    result.diagnostics.push_back({
        AssetDiagnosticSeverity::Error,
        std::move(code),
        assetId,
        path,
        std::move(message)
    });
}

std::string ComputeArtifactContentHash(const std::filesystem::path& finalRelativePath)
{
    return "sha256:" + finalRelativePath.filename().generic_string();
}

uint64_t ToMicroseconds(const std::chrono::steady_clock::duration duration)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
}

const char* SchemaNameForArtifactType(const ArtifactType artifactType)
{
    switch (artifactType)
    {
    case ArtifactType::Mesh: return "mesh";
    case ArtifactType::Material: return "material";
    case ArtifactType::Texture: return "texture";
    case ArtifactType::Prefab: return "prefab";
    case ArtifactType::Scene: return "scene";
    case ArtifactType::Shader: return "shader";
    case ArtifactType::Audio: return "audio";
    case ArtifactType::Skeleton: return "skeleton";
    case ArtifactType::Skin: return "skin";
    case ArtifactType::AnimationClip: return "animationClip";
    case ArtifactType::MorphTarget: return "morphTarget";
    case ArtifactType::Model: return "model";
    case ArtifactType::Unknown:
    case ArtifactType::Count:
        break;
    }
    return "";
}

uint32_t SchemaVersionForArtifactType(const ArtifactType artifactType)
{
    switch (artifactType)
    {
    case ArtifactType::Mesh:
        return 3u;
    case ArtifactType::Texture:
        return 4u;
    case ArtifactType::Material:
    case ArtifactType::Prefab:
    case ArtifactType::Shader:
        return 1u;
    case ArtifactType::Scene:
    case ArtifactType::Audio:
    case ArtifactType::Skeleton:
    case ArtifactType::Skin:
    case ArtifactType::AnimationClip:
    case ArtifactType::MorphTarget:
    case ArtifactType::Model:
    case ArtifactType::Unknown:
    case ArtifactType::Count:
        return 1u;
    }
    return 1u;
}

std::vector<AssetDependencyRecord> BuildStoredArtifactDependencies(
    const ArtifactWriteRequest& request,
    const ArtifactPayload& artifact)
{
    std::vector<AssetDependencyRecord> dependencies = request.dependencies;
    dependencies.insert(
        dependencies.end(),
        artifact.dependencies.begin(),
        artifact.dependencies.end());
    return dependencies;
}

NativeArtifactMetadata BuildStoredArtifactMetadata(
    const ArtifactWriteRequest& request,
    const ArtifactPayload& artifact,
    std::vector<AssetDependencyRecord> dependencies)
{
    NativeArtifactMetadata metadata;
    metadata.artifactType = artifact.artifactType;
    metadata.schemaName = artifact.loaderId.empty()
        ? SchemaNameForArtifactType(artifact.artifactType)
        : artifact.loaderId;
    metadata.schemaVersion = SchemaVersionForArtifactType(artifact.artifactType);
    metadata.sourceAssetId = request.sourceAssetId;
    metadata.subAssetKey = artifact.subAssetKey;
    metadata.displayName = artifact.displayName;
    metadata.importerId = request.importerId;
    metadata.importerVersion = request.importerVersion;
    metadata.targetPlatform = request.targetPlatform;
    metadata.dependencies = std::move(dependencies);
    return metadata;
}

bool AssetDependencyRecordEquals(
    const AssetDependencyRecord& lhs,
    const AssetDependencyRecord& rhs)
{
    return lhs.kind == rhs.kind &&
        lhs.value == rhs.value &&
        lhs.hashOrVersion == rhs.hashOrVersion;
}

bool AssetDependencyRecordsEqual(
    const std::vector<AssetDependencyRecord>& lhs,
    const std::vector<AssetDependencyRecord>& rhs)
{
    return lhs.size() == rhs.size() &&
        std::equal(
            lhs.begin(),
            lhs.end(),
            rhs.begin(),
            AssetDependencyRecordEquals);
}

bool NativeArtifactMetadataEquals(
    const NativeArtifactMetadata& lhs,
    const NativeArtifactMetadata& rhs)
{
    return lhs.artifactType == rhs.artifactType &&
        lhs.schemaName == rhs.schemaName &&
        lhs.schemaVersion == rhs.schemaVersion &&
        lhs.sourceAssetId == rhs.sourceAssetId &&
        lhs.subAssetKey == rhs.subAssetKey &&
        lhs.displayName == rhs.displayName &&
        lhs.importerId == rhs.importerId &&
        lhs.importerVersion == rhs.importerVersion &&
        lhs.targetPlatform == rhs.targetPlatform &&
        lhs.payloadHash == rhs.payloadHash &&
        lhs.dependencyHash == rhs.dependencyHash &&
        AssetDependencyRecordsEqual(lhs.dependencies, rhs.dependencies);
}

struct ArtifactPayloadIdentity
{
    std::string payloadHash;
    uint64_t payloadSize = 0u;
    std::optional<NativeArtifactMetadata> nativeContainerMetadata;
};

struct StoredPayloadReuseProbeStats
{
    uint64_t nativeContainerDirectHashCount = 0u;
    uint64_t nativeContainerDirectReuseCount = 0u;
};

std::optional<ArtifactPayloadIdentity> BuildArtifactPayloadIdentity(const ArtifactPayload& artifact)
{
    if (IsNativeArtifactContainer(artifact.payload))
    {
        const auto view = ReadNativeArtifactContainerView(
            artifact.payload,
            artifact.artifactType,
            SchemaVersionForArtifactType(artifact.artifactType));
        if (!view.has_value())
            return std::nullopt;

        return ArtifactPayloadIdentity {
            view->metadata.payloadHash,
            static_cast<uint64_t>(view->payloadSize),
            std::move(view->metadata)
        };
    }

    return ArtifactPayloadIdentity {
        ComputeNativeArtifactPayloadHash(artifact.payload),
        static_cast<uint64_t>(artifact.payload.size()),
        std::nullopt
    };
}

std::vector<uint8_t> BuildStoredArtifactPayload(
    const ArtifactWriteRequest& request,
    const ArtifactPayload& artifact)
{
    std::vector<uint8_t> rawPayload = artifact.payload;
    if (IsNativeArtifactContainer(artifact.payload))
    {
        auto parsed = ReadNativeArtifactContainer(
            artifact.payload,
            artifact.artifactType,
            SchemaVersionForArtifactType(artifact.artifactType));
        if (!parsed.has_value())
            return {};
        rawPayload = std::move(parsed->payload);
    }

    auto metadata = BuildStoredArtifactMetadata(
        request,
        artifact,
        BuildStoredArtifactDependencies(request, artifact));
    return WriteNativeArtifactContainer(std::move(metadata), rawPayload);
}

std::filesystem::path BuildContentAddressedArtifactRelativePathFromStoredPayload(
    const std::vector<uint8_t>& storedPayload)
{
    return BuildArtifactStorageRelativePath(BuildArtifactStorageFileName(storedPayload.data(), storedPayload.size()));
}

std::filesystem::path BuildPortableArtifactPath(
    const std::filesystem::path& committedRoot,
    const std::filesystem::path& finalRelativePath)
{
    const auto normalizedRoot = committedRoot.lexically_normal();
    const auto rootIt = std::find(normalizedRoot.begin(), normalizedRoot.end(), std::filesystem::path("Library"));
    if (rootIt != normalizedRoot.end())
    {
        std::filesystem::path portable;
        for (auto it = rootIt; it != normalizedRoot.end(); ++it)
            portable /= *it;
        portable /= finalRelativePath;
        return portable.lexically_normal();
    }
    return finalRelativePath.lexically_normal();
}

std::optional<std::filesystem::path> TryReusePreviousContentAddressedPathWithoutStoredPayload(
    const ArtifactWriteRequest& request,
    const ArtifactPayload& artifact,
    const ArtifactManifest* previousSuccessfulManifest,
    const std::filesystem::path& committedRoot,
    StoredPayloadReuseProbeStats* stats)
{
    if (previousSuccessfulManifest == nullptr)
        return std::nullopt;

    const auto* previous = previousSuccessfulManifest->FindSubAsset(artifact.subAssetKey);
    if (previous == nullptr ||
        previous->sourceAssetId != request.sourceAssetId ||
        previous->artifactType != artifact.artifactType ||
        previous->loaderId != artifact.loaderId ||
        previous->targetPlatform != request.targetPlatform ||
        previous->displayName != artifact.displayName ||
        previous->contentHash.rfind("sha256:", 0u) != 0u)
    {
        return std::nullopt;
    }

    const auto previousFileName = previous->contentHash.substr(7u);
    if (!IsArtifactStorageFileName(previousFileName))
        return std::nullopt;

    const auto previousRelativePath = BuildArtifactStorageRelativePath(previousFileName);
    if (previousRelativePath.empty())
        return std::nullopt;

    const auto normalizedPreviousPath = std::filesystem::path(previous->artifactPath).lexically_normal();
    if (normalizedPreviousPath.filename() != previousFileName)
        return std::nullopt;

    const auto payloadIdentity = BuildArtifactPayloadIdentity(artifact);
    if (!payloadIdentity.has_value())
        return std::nullopt;

    auto currentMetadata = BuildStoredArtifactMetadata(
        request,
        artifact,
        BuildStoredArtifactDependencies(request, artifact));
    currentMetadata.payloadHash = payloadIdentity->payloadHash;
    currentMetadata.dependencyHash = ComputeNativeArtifactDependencyHash(currentMetadata.dependencies);

    if (payloadIdentity->nativeContainerMetadata.has_value() &&
        NativeArtifactMetadataEquals(*payloadIdentity->nativeContainerMetadata, currentMetadata))
    {
        if (stats != nullptr)
            ++stats->nativeContainerDirectHashCount;

        const auto currentFileName = BuildArtifactStorageFileName(
            artifact.payload.data(),
            artifact.payload.size());
        if (currentFileName == previousFileName)
        {
            std::error_code currentError;
            if (!ExistingContentAddressedBlobMatchesExpectedPayload(
                    committedRoot / previousRelativePath,
                    artifact.payload,
                    currentError))
            {
                return std::nullopt;
            }

            if (stats != nullptr)
                ++stats->nativeContainerDirectReuseCount;
            return previousRelativePath;
        }
    }

    const auto destinationPath = committedRoot / previousRelativePath;
    constexpr uint64_t kMaxReuseProbeMetadataBytes = 1024ull * 1024ull;
    const auto previousPrefix = ReadNativeArtifactPayloadPrefixFromFile(
        destinationPath,
        artifact.artifactType,
        SchemaVersionForArtifactType(artifact.artifactType),
        0u,
        kMaxReuseProbeMetadataBytes);
    if (!previousPrefix.has_value() || previousPrefix->payloadSize != payloadIdentity->payloadSize)
        return std::nullopt;

    if (!NativeArtifactMetadataEquals(previousPrefix->metadata, currentMetadata))
        return std::nullopt;

    return previousRelativePath;
}

void MoveOrCopyFile(
    const std::filesystem::path& from,
    const std::filesystem::path& to,
    std::error_code& error)
{
    error.clear();
    std::filesystem::rename(from, to, error);
    if (!error)
        return;

    error.clear();
    std::filesystem::copy_file(
        from,
        to,
        std::filesystem::copy_options::overwrite_existing,
        error);
    if (!error)
    {
        std::error_code removeError;
        std::filesystem::remove(from, removeError);
    }
}

void RollbackCommit(const std::vector<CommitPlan>& plans)
{
    std::error_code error;
    for (auto it = plans.rbegin(); it != plans.rend(); ++it)
    {
        const auto& plan = *it;
        if (plan.destinationMayContainReplacement || plan.replacementMoved)
            std::filesystem::remove(plan.destinationPath, error);

        if (plan.destinationBackedUp)
        {
            std::filesystem::create_directories(plan.destinationPath.parent_path(), error);
            error.clear();
            std::filesystem::rename(plan.backupPath, plan.destinationPath, error);
            if (error)
            {
                error.clear();
                std::filesystem::copy_file(
                    plan.backupPath,
                    plan.destinationPath,
                    std::filesystem::copy_options::overwrite_existing,
                    error);
            }
        }
    }
}
}

ArtifactWriter::ArtifactWriter(std::filesystem::path stagingRoot, std::filesystem::path committedRoot)
    : m_stagingRoot(std::move(stagingRoot))
    , m_committedRoot(std::move(committedRoot))
{
}

std::filesystem::path BuildContentAddressedArtifactRelativePath(
    const ArtifactWriteRequest& request,
    const ArtifactPayload& artifact)
{
    const auto storedPayload = BuildStoredArtifactPayload(request, artifact);
    if (storedPayload.empty())
        return {};
    return BuildContentAddressedArtifactRelativePathFromStoredPayload(storedPayload);
}

ArtifactWriteResult ArtifactWriter::WriteAndCommit(
    const ArtifactWriteRequest& request,
    const ArtifactManifest* previousSuccessfulManifest,
    const IArtifactWriteCancellation* cancellation) const
{
    ArtifactWriteResult result;
    if (previousSuccessfulManifest)
        result.manifest = *previousSuccessfulManifest;

    const auto stagingRoot = NormalizedAbsolute(m_stagingRoot);
    const auto committedRoot = NormalizedAbsolute(m_committedRoot);
    const auto backupRoot = BackupRootFor(stagingRoot);

    std::error_code error;
    if (m_stagingRoot.empty() ||
        m_committedRoot.empty() ||
        HasUnsafeRootRelationship(stagingRoot, committedRoot) ||
        HasUnsafeRootRelationship(backupRoot, committedRoot) ||
        HasUnsafeRootRelationship(backupRoot, stagingRoot))
    {
        AddError(
            result,
            request.sourceAssetId,
            stagingRoot,
            "artifact-root-unsafe",
            "Artifact staging and committed roots must be non-empty, distinct, and outside each other's directory trees.");
        return result;
    }

    std::filesystem::remove_all(stagingRoot, error);
    error.clear();
    std::filesystem::remove_all(backupRoot, error);
    error.clear();
    std::filesystem::create_directories(stagingRoot, error);
    if (error)
    {
        AddError(result, request.sourceAssetId, stagingRoot, "artifact-staging-create-failed", error.message());
        return result;
    }

    const auto cancelIfRequested = [&]() -> bool
    {
        if (!cancellation || !cancellation->IsCancellationRequested())
            return false;
        AddError(
            result,
            request.sourceAssetId,
            stagingRoot,
            "artifact-write-cancelled",
            "Artifact write was cancelled before commit.");
        return true;
    };

    if (cancelIfRequested())
    {
        std::filesystem::remove_all(stagingRoot, error);
        std::filesystem::remove_all(backupRoot, error);
        return result;
    }

    NLS::Base::Profiling::PerformanceStageScope writeScope(
        NLS::Base::Profiling::PerformanceStageDomain::Prefab,
        "WriteAndCommitArtifactPayloads",
        NLS::Base::Profiling::PerformanceStageThread::Main);
    writeScope.AddCounter("artifactCount", request.artifacts.size());

    uint64_t buildStoredPayloadMs = 0u;
    uint64_t contentPathMs = 0u;
    uint64_t existingCheckMs = 0u;
    uint64_t stagingWriteMs = 0u;
    uint64_t commitPlanMs = 0u;
    uint64_t commitMoveMs = 0u;
    uint64_t storedPayloadReuseProbeMs = 0u;
    uint64_t destinationAlreadyCurrentCount = 0u;
    uint64_t stagedCount = 0u;
    uint64_t duplicateStagedPathCount = 0u;
    uint64_t commitPlanCount = 0u;
    uint64_t committedMoveCount = 0u;
    uint64_t contentPathReusedCount = 0u;
    uint64_t contentPathHashedCount = 0u;
    uint64_t storedPayloadBypassCount = 0u;
    uint64_t storedPayloadBuiltCount = 0u;
    uint64_t nativeContainerDirectHashCount = 0u;
    uint64_t nativeContainerDirectReuseCount = 0u;

    ArtifactManifest nextManifest;
    nextManifest.sourceAssetId = request.sourceAssetId;
    nextManifest.importerId = request.importerId;
    nextManifest.importerVersion = request.importerVersion;
    nextManifest.targetPlatform = request.targetPlatform;
    nextManifest.primarySubAssetKey = request.primarySubAssetKey;
    nextManifest.dependencies = request.dependencies;

    std::vector<StagedArtifact> stagedArtifacts;
    stagedArtifacts.reserve(request.artifacts.size());
    std::set<std::filesystem::path> stagedRelativePaths;
    for (const auto& artifact : request.artifacts)
    {
        if (cancelIfRequested())
        {
            std::filesystem::remove_all(stagingRoot, error);
            std::filesystem::remove_all(backupRoot, error);
            return result;
        }

        if (!IsSafeRelativePath(artifact.relativePath))
        {
            AddError(
                result,
                request.sourceAssetId,
                artifact.relativePath,
                "artifact-path-escape",
                "Artifact payload path must stay below the committed artifact root.");
            std::filesystem::remove_all(stagingRoot, error);
            return result;
        }

        std::filesystem::path finalRelativePath;
        std::vector<uint8_t> storedPayload;
        bool destinationExists = false;
        bool destinationIsDirectory = false;
        bool destinationAlreadyCurrent = false;
        const auto reuseProbeStart = std::chrono::steady_clock::now();
        StoredPayloadReuseProbeStats reuseProbeStats;
        auto reusedWithoutStoredPayload = TryReusePreviousContentAddressedPathWithoutStoredPayload(
            request,
            artifact,
            previousSuccessfulManifest,
            committedRoot,
            &reuseProbeStats);
        storedPayloadReuseProbeMs += ToMicroseconds(std::chrono::steady_clock::now() - reuseProbeStart);
        nativeContainerDirectHashCount += reuseProbeStats.nativeContainerDirectHashCount;
        nativeContainerDirectReuseCount += reuseProbeStats.nativeContainerDirectReuseCount;
        if (reusedWithoutStoredPayload)
        {
            finalRelativePath = std::move(*reusedWithoutStoredPayload);
            ++contentPathReusedCount;
            ++storedPayloadBypassCount;
            ++destinationAlreadyCurrentCount;
            destinationAlreadyCurrent = true;
        }
        else
        {
            const auto buildStoredPayloadStart = std::chrono::steady_clock::now();
            storedPayload = BuildStoredArtifactPayload(request, artifact);
            ++storedPayloadBuiltCount;
            buildStoredPayloadMs += ToMicroseconds(std::chrono::steady_clock::now() - buildStoredPayloadStart);
            if (storedPayload.empty())
            {
                AddError(
                    result,
                    request.sourceAssetId,
                    artifact.relativePath,
                    "artifact-container-write-failed",
                    "Artifact payload could not be serialized into a native artifact container.");
                std::filesystem::remove_all(stagingRoot, error);
                return result;
            }

            const auto contentPathStart = std::chrono::steady_clock::now();
            finalRelativePath = BuildContentAddressedArtifactRelativePathFromStoredPayload(storedPayload);
            contentPathMs += ToMicroseconds(std::chrono::steady_clock::now() - contentPathStart);
            ++contentPathHashedCount;
        }
        if (!IsSafeRelativePath(finalRelativePath))
        {
            AddError(
                result,
                request.sourceAssetId,
                finalRelativePath,
                "artifact-path-escape",
                "Artifact payload path must stay below the committed artifact root.");
            std::filesystem::remove_all(stagingRoot, error);
            return result;
        }

        const auto stagedPath = stagingRoot / finalRelativePath;
        const auto destinationPath = committedRoot / finalRelativePath;
        if (!destinationAlreadyCurrent)
        {
            const auto existingCheckStart = std::chrono::steady_clock::now();
            const auto destinationStatus = std::filesystem::status(destinationPath, error);
            destinationExists = !error && std::filesystem::exists(destinationStatus);
            destinationIsDirectory = !error && std::filesystem::is_directory(destinationStatus);
            destinationAlreadyCurrent =
                !error &&
                std::filesystem::is_regular_file(destinationStatus) &&
                ExistingContentAddressedBlobMatchesExpectedPayload(destinationPath, storedPayload, error);
            existingCheckMs += ToMicroseconds(std::chrono::steady_clock::now() - existingCheckStart);
            error.clear();

            if (destinationAlreadyCurrent)
                ++destinationAlreadyCurrentCount;
        }

        const bool shouldStage = !destinationAlreadyCurrent && stagedRelativePaths.insert(finalRelativePath).second;
        if (!destinationAlreadyCurrent && !shouldStage)
            ++duplicateStagedPathCount;

        if (shouldStage)
        {
            const auto stagingWriteStart = std::chrono::steady_clock::now();
            std::filesystem::create_directories(stagedPath.parent_path(), error);
            if (error)
            {
                AddError(result, request.sourceAssetId, stagedPath, "artifact-directory-create-failed", error.message());
                std::filesystem::remove_all(stagingRoot, error);
                return result;
            }

            std::ofstream output(stagedPath, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                AddError(result, request.sourceAssetId, stagedPath, "artifact-write-failed", "Artifact payload could not be opened for writing.");
                std::filesystem::remove_all(stagingRoot, error);
                return result;
            }
            output.write(
                reinterpret_cast<const char*>(storedPayload.data()),
                static_cast<std::streamsize>(storedPayload.size()));
            output.close();
            if (!output)
            {
                AddError(result, request.sourceAssetId, stagedPath, "artifact-write-failed", "Artifact payload write did not complete successfully.");
                std::filesystem::remove_all(stagingRoot, error);
                return result;
            }

            stagedArtifacts.push_back({ finalRelativePath, destinationExists, destinationIsDirectory });
            stagingWriteMs += ToMicroseconds(std::chrono::steady_clock::now() - stagingWriteStart);
            ++stagedCount;
        }

        if (cancelIfRequested())
        {
            std::filesystem::remove_all(stagingRoot, error);
            std::filesystem::remove_all(backupRoot, error);
            return result;
        }

        ImportedArtifact imported;
        imported.sourceAssetId = request.sourceAssetId;
        imported.subAssetKey = artifact.subAssetKey;
        imported.artifactType = artifact.artifactType;
        imported.loaderId = artifact.loaderId;
        imported.targetPlatform = request.targetPlatform;
        imported.artifactPath = BuildPortableArtifactPath(committedRoot, finalRelativePath).generic_string();
        imported.contentHash = ComputeArtifactContentHash(finalRelativePath);
        imported.displayName = artifact.displayName;
        nextManifest.subAssets.push_back(std::move(imported));
    }

    std::filesystem::create_directories(committedRoot, error);
    if (error)
    {
        AddError(result, request.sourceAssetId, committedRoot, "artifact-commit-directory-create-failed", error.message());
        std::filesystem::remove_all(stagingRoot, error);
        return result;
    }

    std::vector<CommitPlan> commitPlans;
    commitPlans.reserve(stagedArtifacts.size());
    std::set<std::filesystem::path> plannedRelativePaths;
    const auto commitPlanStart = std::chrono::steady_clock::now();
    for (const auto& artifact : stagedArtifacts)
    {
        if (!plannedRelativePaths.insert(artifact.relativePath).second)
            continue;

        const auto sourcePath = stagingRoot / artifact.relativePath;
        const auto destinationPath = committedRoot / artifact.relativePath;
        std::filesystem::create_directories(destinationPath.parent_path(), error);
        if (error)
        {
            AddError(result, request.sourceAssetId, destinationPath, "artifact-commit-directory-create-failed", error.message());
            std::filesystem::remove_all(stagingRoot, error);
            return result;
        }

        if (artifact.destinationIsDirectory)
        {
            AddError(
                result,
                request.sourceAssetId,
                destinationPath,
                "artifact-commit-failed",
                "Committed artifact destination is a directory.");
            std::filesystem::remove_all(stagingRoot, error);
            return result;
        }
        error.clear();

        commitPlans.push_back({
            sourcePath,
            destinationPath,
            backupRoot / artifact.relativePath,
            artifact.destinationExists
        });
        ++commitPlanCount;
    }
    commitPlanMs += ToMicroseconds(std::chrono::steady_clock::now() - commitPlanStart);

    if (!commitPlans.empty())
    {
        std::filesystem::create_directories(backupRoot, error);
        if (error)
        {
            AddError(result, request.sourceAssetId, backupRoot, "artifact-backup-create-failed", error.message());
            std::filesystem::remove_all(stagingRoot, error);
            return result;
        }
    }

    for (auto& plan : commitPlans)
    {
        if (cancelIfRequested())
        {
            RollbackCommit(commitPlans);
            std::filesystem::remove_all(stagingRoot, error);
            std::filesystem::remove_all(backupRoot, error);
            return result;
        }

        if (plan.destinationExists)
        {
            const auto commitMoveStart = std::chrono::steady_clock::now();
            std::filesystem::create_directories(plan.backupPath.parent_path(), error);
            if (error)
            {
                AddError(result, request.sourceAssetId, plan.backupPath, "artifact-backup-create-failed", error.message());
                RollbackCommit(commitPlans);
                std::filesystem::remove_all(stagingRoot, error);
                std::filesystem::remove_all(backupRoot, error);
                return result;
            }

            MoveOrCopyFile(plan.destinationPath, plan.backupPath, error);
            if (error)
            {
                AddError(result, request.sourceAssetId, plan.destinationPath, "artifact-backup-failed", error.message());
                RollbackCommit(commitPlans);
                std::filesystem::remove_all(stagingRoot, error);
                std::filesystem::remove_all(backupRoot, error);
                return result;
            }
            plan.destinationBackedUp = true;
            commitMoveMs += ToMicroseconds(std::chrono::steady_clock::now() - commitMoveStart);
        }

        plan.destinationMayContainReplacement = true;
        const auto commitMoveStart = std::chrono::steady_clock::now();
        MoveOrCopyFile(plan.sourcePath, plan.destinationPath, error);
        commitMoveMs += ToMicroseconds(std::chrono::steady_clock::now() - commitMoveStart);
        if (error)
        {
            AddError(result, request.sourceAssetId, plan.destinationPath, "artifact-commit-failed", error.message());
            RollbackCommit(commitPlans);
            std::filesystem::remove_all(stagingRoot, error);
            std::filesystem::remove_all(backupRoot, error);
            return result;
        }
        plan.replacementMoved = true;
        ++committedMoveCount;
    }

    std::filesystem::remove_all(stagingRoot, error);
    std::filesystem::remove_all(backupRoot, error);
    writeScope.AddCounter("buildStoredPayloadMs", buildStoredPayloadMs / 1000u);
    writeScope.AddCounter("contentPathMs", contentPathMs / 1000u);
    writeScope.AddCounter("existingCheckMs", existingCheckMs / 1000u);
    writeScope.AddCounter("stagingWriteMs", stagingWriteMs / 1000u);
    writeScope.AddCounter("commitPlanMs", commitPlanMs / 1000u);
    writeScope.AddCounter("commitMoveMs", commitMoveMs / 1000u);
    writeScope.AddCounter("storedPayloadReuseProbeMs", storedPayloadReuseProbeMs / 1000u);
    writeScope.AddCounter("destinationAlreadyCurrentCount", destinationAlreadyCurrentCount);
    writeScope.AddCounter("stagedCount", stagedCount);
    writeScope.AddCounter("duplicateStagedPathCount", duplicateStagedPathCount);
    writeScope.AddCounter("commitPlanCount", commitPlanCount);
    writeScope.AddCounter("committedMoveCount", committedMoveCount);
    writeScope.AddCounter("contentPathReusedCount", contentPathReusedCount);
    writeScope.AddCounter("contentPathHashedCount", contentPathHashedCount);
    writeScope.AddCounter("storedPayloadBypassCount", storedPayloadBypassCount);
    writeScope.AddCounter("storedPayloadBuiltCount", storedPayloadBuiltCount);
    writeScope.AddCounter("nativeContainerDirectHashCount", nativeContainerDirectHashCount);
    writeScope.AddCounter("nativeContainerDirectReuseCount", nativeContainerDirectReuseCount);
    result.committed = true;
    result.manifest = std::move(nextManifest);
    return result;
}
}
