#include "Assets/ArtifactWriter.h"
#include "Assets/NativeArtifactContainer.h"

#include <fstream>
namespace NLS::Core::Assets
{
namespace
{
struct CommitPlan
{
    std::filesystem::path sourcePath;
    std::filesystem::path destinationPath;
    std::filesystem::path backupPath;
    bool destinationBackedUp = false;
    bool destinationMayContainReplacement = false;
    bool replacementMoved = false;
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

std::string PayloadHash(const std::vector<uint8_t>& payload)
{
    uint64_t hash = 1469598103934665603ull;
    for (const auto byte : payload)
    {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return "fnv1a64:" + std::to_string(hash);
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
    default: return "";
    }
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
    default:
        return 1u;
    }
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

    NativeArtifactMetadata metadata;
    metadata.artifactType = artifact.artifactType;
    metadata.schemaName = artifact.loaderId.empty()
        ? SchemaNameForArtifactType(artifact.artifactType)
        : artifact.loaderId;
    metadata.schemaVersion = SchemaVersionForArtifactType(artifact.artifactType);
    metadata.sourceAssetId = request.sourceAssetId;
    metadata.subAssetKey = artifact.subAssetKey;
    metadata.importerId = request.importerId;
    metadata.importerVersion = request.importerVersion;
    metadata.targetPlatform = request.targetPlatform;
    metadata.dependencies = request.dependencies;
    return WriteNativeArtifactContainer(std::move(metadata), rawPayload);
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

    ArtifactManifest nextManifest;
    nextManifest.sourceAssetId = request.sourceAssetId;
    nextManifest.importerId = request.importerId;
    nextManifest.importerVersion = request.importerVersion;
    nextManifest.targetPlatform = request.targetPlatform;
    nextManifest.primarySubAssetKey = request.primarySubAssetKey;
    nextManifest.dependencies = request.dependencies;

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

        const auto stagedPath = stagingRoot / artifact.relativePath;
        std::filesystem::create_directories(stagedPath.parent_path(), error);
        if (error)
        {
            AddError(result, request.sourceAssetId, stagedPath, "artifact-directory-create-failed", error.message());
            std::filesystem::remove_all(stagingRoot, error);
            return result;
        }

        const auto storedPayload = BuildStoredArtifactPayload(request, artifact);
        if (storedPayload.empty())
        {
            AddError(
                result,
                request.sourceAssetId,
                stagedPath,
                "artifact-container-write-failed",
                "Artifact payload could not be serialized into a native artifact container.");
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
        imported.artifactPath = (committedRoot / artifact.relativePath).string();
        imported.contentHash = PayloadHash(storedPayload);
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
    commitPlans.reserve(request.artifacts.size());
    for (const auto& artifact : request.artifacts)
    {
        const auto sourcePath = stagingRoot / artifact.relativePath;
        const auto destinationPath = committedRoot / artifact.relativePath;
        std::filesystem::create_directories(destinationPath.parent_path(), error);
        if (error)
        {
            AddError(result, request.sourceAssetId, destinationPath, "artifact-commit-directory-create-failed", error.message());
            std::filesystem::remove_all(stagingRoot, error);
            return result;
        }

        if (std::filesystem::is_directory(destinationPath, error))
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

        commitPlans.push_back({
            sourcePath,
            destinationPath,
            backupRoot / artifact.relativePath
        });
    }

    std::filesystem::create_directories(backupRoot, error);
    if (error)
    {
        AddError(result, request.sourceAssetId, backupRoot, "artifact-backup-create-failed", error.message());
        std::filesystem::remove_all(stagingRoot, error);
        return result;
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

        if (std::filesystem::exists(plan.destinationPath, error))
        {
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
        }

        plan.destinationMayContainReplacement = true;
        MoveOrCopyFile(plan.sourcePath, plan.destinationPath, error);
        if (error)
        {
            AddError(result, request.sourceAssetId, plan.destinationPath, "artifact-commit-failed", error.message());
            RollbackCommit(commitPlans);
            std::filesystem::remove_all(stagingRoot, error);
            std::filesystem::remove_all(backupRoot, error);
            return result;
        }
        plan.replacementMoved = true;
    }

    std::filesystem::remove_all(stagingRoot, error);
    std::filesystem::remove_all(backupRoot, error);
    result.committed = true;
    result.manifest = std::move(nextManifest);
    return result;
}
}
