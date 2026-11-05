#pragma once

#include "Rendering/Assets/DistanceFieldBuilder.h"

namespace NLS::Render::DistanceField
{
inline constexpr const char* DistanceFieldSourceKey = "distancefield-source";
struct DistanceFieldSourceMesh
{
    std::string key, logicalKey, geometryIdentity;
    bool participates = true;
};
struct DistanceFieldSourceGroup
{
    std::string key, geometryIdentity;
    std::vector<std::string> members;
};
struct DistanceFieldSourceDescription
{
    std::string identity;
    std::vector<DistanceFieldSourceMesh> meshes;
    std::vector<DistanceFieldSourceGroup> groups;
};
struct DistanceFieldModelBuildProgress
{
    std::string meshKey, stage;
    uint32_t mip = 2, completed = 0, total = 0;
    DistanceFieldBuildStats stats;
};
struct DistanceFieldModelBuildOptions
{
    DistanceFieldBuildOptions kernel;
    bool includeFallbacks = true;
    const NLS::Core::Assets::ArtifactManifest* previousManifest = nullptr;
    std::function<std::optional<Assets::MeshArtifactData>(const std::string&)> loadMesh;
    std::function<std::vector<uint8_t>(const NLS::Core::Assets::ImportedArtifact&)> readArtifact;
    std::function<bool(const NLS::Core::Assets::ImportedArtifact&)> artifactExists;
    std::function<void(const DistanceFieldModelBuildProgress&)> progress;
    // Called only after a complete mip is available. False rejects obsolete jobs.
    std::function<bool(const std::vector<NLS::Core::Assets::ArtifactPayload>&,
        const std::vector<std::string>&, const DistanceFieldModelBuildProgress&)> publish;
};
struct DistanceFieldModelBuildResult
{
    bool success = false;
    uint64_t skippedMips = 0, meshLoads = 0;
    DistanceFieldBuildStats stats;
    std::vector<NLS::Core::Assets::ArtifactPayload> payloads;
    std::vector<std::string> diagnostics;
};

NLS_STATIC_MESH_BUILD_API DistanceFieldSourceDescription DescribeDistanceFieldSource(
    const Assets::ImportedScene& scene, const std::map<std::string, Assets::MeshArtifactData>& meshes);
NLS_STATIC_MESH_BUILD_API std::vector<uint8_t> SerializeDistanceFieldSource(const DistanceFieldSourceDescription& source);
NLS_STATIC_MESH_BUILD_API std::optional<DistanceFieldSourceDescription> DeserializeDistanceFieldSource(const std::vector<uint8_t>& bytes);
NLS_STATIC_MESH_BUILD_API NLS::Core::Assets::ArtifactPayload DistanceFieldSourcePayload(const DistanceFieldSourceDescription& source);
NLS_STATIC_MESH_BUILD_API std::string DistanceFieldSourceManifestIdentity(const NLS::Core::Assets::ArtifactManifest& manifest);

class NLS_STATIC_MESH_BUILD_API DistanceFieldBuildService
{
public:
    static DistanceFieldModelBuildResult Build(const DistanceFieldSourceDescription& source,
        const DistanceFieldBuildSettings& settings, const DistanceFieldModelBuildOptions& options,
        const std::atomic_bool* cancellation = nullptr,
        const NLS::Core::Assets::IArtifactWriteCancellation* importCancellation = nullptr);
};
}
