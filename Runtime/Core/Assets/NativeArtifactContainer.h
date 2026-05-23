#pragma once

#include "Assets/ArtifactManifest.h"
#include "CoreDef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Core::Assets
{
struct NativeArtifactMetadata
{
    ArtifactType artifactType = ArtifactType::Unknown;
    std::string schemaName;
    uint32_t schemaVersion = 1u;
    AssetId sourceAssetId;
    std::string subAssetKey;
    std::string importerId;
    uint32_t importerVersion = 0u;
    std::string targetPlatform;
    std::vector<AssetDependencyRecord> dependencies;
    std::string payloadHash;
    std::string dependencyHash;
};

struct NativeArtifactContainer
{
    NativeArtifactMetadata metadata;
    std::vector<uint8_t> payload;
};

NLS_CORE_API size_t NativeArtifactContainerHeaderSize();
NLS_CORE_API std::string ComputeNativeArtifactPayloadHash(const std::vector<uint8_t>& payload);
NLS_CORE_API std::string ComputeNativeArtifactDependencyHash(const std::vector<AssetDependencyRecord>& dependencies);
NLS_CORE_API std::vector<uint8_t> WriteNativeArtifactContainer(
    NativeArtifactMetadata metadata,
    const std::vector<uint8_t>& payload);
NLS_CORE_API std::optional<NativeArtifactContainer> ReadNativeArtifactContainer(
    const std::vector<uint8_t>& bytes,
    ArtifactType expectedType,
    uint32_t expectedSchemaVersion);
NLS_CORE_API bool IsNativeArtifactContainer(const std::vector<uint8_t>& bytes);
}
