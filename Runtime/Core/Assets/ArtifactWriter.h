#pragma once

#include "Assets/ArtifactManifest.h"
#include "Assets/AssetDiagnostics.h"
#include "CoreDef.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace NLS::Core::Assets
{
class IArtifactWriteCancellation
{
public:
    virtual ~IArtifactWriteCancellation() = default;
    virtual bool IsCancellationRequested() const = 0;
};

struct ArtifactPayload
{
    ArtifactPayload() = default;

    ArtifactPayload(
        std::string subAssetKeyValue,
        ArtifactType artifactTypeValue,
        std::string loaderIdValue,
        std::filesystem::path relativePathValue,
        std::vector<uint8_t> payloadValue)
        : subAssetKey(std::move(subAssetKeyValue))
        , artifactType(artifactTypeValue)
        , loaderId(std::move(loaderIdValue))
        , relativePath(std::move(relativePathValue))
        , payload(std::move(payloadValue))
    {
    }

    ArtifactPayload(
        std::string subAssetKeyValue,
        ArtifactType artifactTypeValue,
        std::string loaderIdValue,
        std::string displayNameValue,
        std::filesystem::path relativePathValue,
        std::vector<uint8_t> payloadValue)
        : subAssetKey(std::move(subAssetKeyValue))
        , artifactType(artifactTypeValue)
        , loaderId(std::move(loaderIdValue))
        , displayName(std::move(displayNameValue))
        , relativePath(std::move(relativePathValue))
        , payload(std::move(payloadValue))
    {
    }

    std::string subAssetKey;
    ArtifactType artifactType = ArtifactType::Unknown;
    std::string loaderId;
    std::string displayName;
    std::filesystem::path relativePath;
    std::vector<uint8_t> payload;
};

struct ArtifactWriteRequest
{
    AssetId sourceAssetId;
    std::string importerId;
    uint32_t importerVersion = 1u;
    std::string targetPlatform;
    std::string primarySubAssetKey;
    std::vector<ArtifactPayload> artifacts;
    std::vector<AssetDependencyRecord> dependencies;
};

struct ArtifactWriteResult
{
    bool committed = false;
    ArtifactManifest manifest;
    AssetDiagnostics diagnostics;
};

class NLS_CORE_API ArtifactWriter
{
public:
    ArtifactWriter(std::filesystem::path stagingRoot, std::filesystem::path committedRoot);

    ArtifactWriteResult WriteAndCommit(
        const ArtifactWriteRequest& request,
        const ArtifactManifest* previousSuccessfulManifest,
        const IArtifactWriteCancellation* cancellation = nullptr) const;

private:
    std::filesystem::path m_stagingRoot;
    std::filesystem::path m_committedRoot;
};
}
