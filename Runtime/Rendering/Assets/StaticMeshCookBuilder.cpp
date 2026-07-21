#include "Rendering/Assets/StaticMeshCookBuilder.h"

#include "Assets/NativeArtifactContainer.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace NLS::Render::Assets
{
namespace
{
constexpr uint32_t kStaticMeshArtifactSchemaVersion = 3u;
constexpr const char* kStaticMeshCookIdentityDependency = "static-mesh-lod-build-identity";

std::string MeshContentIdentity(const MeshArtifactData& mesh)
{
    const auto bytes = SerializeMeshArtifact(mesh);
    return bytes.empty()
        ? std::string("empty")
        : NLS::Core::Assets::ComputeNativeArtifactPayloadHash(bytes);
}

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return {};

    const auto size = input.tellg();
    if (size <= 0)
        return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    return input ? bytes : std::vector<uint8_t> {};
}

bool HasMatchingBuildIdentity(
    const NLS::Core::Assets::NativeArtifactMetadata& metadata,
    const std::string& expectedIdentity)
{
    return std::any_of(
        metadata.dependencies.begin(),
        metadata.dependencies.end(),
        [&expectedIdentity](const NLS::Core::Assets::AssetDependencyRecord& dependency)
        {
            return dependency.kind == NLS::Core::Assets::AssetDependencyKind::RuntimeComponentCapability &&
                dependency.value == kStaticMeshCookIdentityDependency &&
                dependency.hashOrVersion == expectedIdentity;
        });
}

bool IsValidCookedArtifact(
    const std::vector<uint8_t>& bytes,
    const std::string& expectedIdentity)
{
    const auto container = NLS::Core::Assets::ReadNativeArtifactContainerView(
        bytes,
        NLS::Core::Assets::ArtifactType::Mesh,
        kStaticMeshArtifactSchemaVersion,
        NLS::Core::Assets::NativeArtifactPayloadValidation::VerifyHash);
    if (!container.has_value() ||
        !HasMatchingBuildIdentity(container->metadata, expectedIdentity))
    {
        return false;
    }

    return DeserializeMeshArtifactBundle(bytes).has_value() ||
        DeserializeMeshArtifact(bytes).has_value();
}

bool ReplaceFileAtomically(
    const std::filesystem::path& source,
    const std::filesystem::path& destination)
{
#if defined(_WIN32)
    return MoveFileExW(
        source.c_str(),
        destination.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    return !error;
#endif
}

bool WriteFileAtomically(
    const std::filesystem::path& path,
    const std::vector<uint8_t>& bytes)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return false;

    auto temporaryPath = path;
    temporaryPath += ".tmp-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    {
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!output)
            return false;
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output)
        {
            output.close();
            std::filesystem::remove(temporaryPath, error);
            return false;
        }
    }

    if (ReplaceFileAtomically(temporaryPath, path))
        return true;

    std::filesystem::remove(temporaryPath, error);
    return false;
}

std::vector<uint8_t> BuildStoredCookArtifact(
    const StaticMeshCookRequest& request,
    const MeshArtifactBundle& bundle,
    const std::string& buildIdentity)
{
    std::vector<uint8_t> payload;
    if (bundle.lodResources.size() == 1u)
    {
        const auto serialized = SerializeMeshArtifact(bundle.lodResources.front().mesh);
        const auto container = NLS::Core::Assets::ReadNativeArtifactContainer(
            serialized,
            NLS::Core::Assets::ArtifactType::Mesh,
            kStaticMeshArtifactSchemaVersion);
        if (!container.has_value())
            return {};
        payload = container->payload;
    }
    else
    {
        payload = SerializeMeshArtifactBundle(bundle);
    }
    if (payload.empty())
        return {};

    NLS::Core::Assets::NativeArtifactMetadata metadata;
    metadata.artifactType = NLS::Core::Assets::ArtifactType::Mesh;
    metadata.schemaName = "mesh";
    metadata.schemaVersion = kStaticMeshArtifactSchemaVersion;
    metadata.importerId = request.importerId;
    metadata.importerVersion = request.importerVersion;
    metadata.targetPlatform = request.targetPlatform;
    metadata.dependencies = {
        {NLS::Core::Assets::AssetDependencyKind::SourceAssetGuid,
         request.sourceAssetIdentity,
         request.sourceContentHash},
        {NLS::Core::Assets::AssetDependencyKind::SourceFileHash,
         request.sourceAssetIdentity,
         request.sourceContentHash},
        {NLS::Core::Assets::AssetDependencyKind::BuildTarget,
         request.targetPlatform,
         request.targetPlatform},
        {NLS::Core::Assets::AssetDependencyKind::ImporterVersion,
         request.importerId,
         std::to_string(request.importerVersion)},
        {NLS::Core::Assets::AssetDependencyKind::PostprocessorVersion,
         "static-mesh-lod-builder",
         std::to_string(request.postprocessorVersion)},
        {NLS::Core::Assets::AssetDependencyKind::RuntimeComponentCapability,
         kStaticMeshCookIdentityDependency,
         buildIdentity},
        {NLS::Core::Assets::AssetDependencyKind::RuntimeComponentCapability,
         request.reducerId,
         std::to_string(request.reducerVersion)}};
    return NLS::Core::Assets::WriteNativeArtifactContainer(std::move(metadata), payload);
}
}

std::string BuildStaticMeshCookIdentity(
    const StaticMeshCookRequest& request,
    const StaticMeshLODSettingsRegistry& settings)
{
    std::ostringstream stream;
    stream
        << "sourceAsset=" << request.sourceAssetIdentity
        << "|sourceHash=" << request.sourceContentHash
        << "|lod0=" << MeshContentIdentity(request.importedLOD0)
        << "|lodGroup=" << request.sourceAsset.lodGroup
        << "|minLOD=" << request.sourceAsset.minLOD
        << "|autoScreen=" << (request.sourceAsset.autoComputeLODScreenSize ? 1u : 0u)
        << "|sourceModels=" << request.sourceAsset.sourceModels.size();
    for (size_t index = 0u; index < request.sourceAsset.sourceModels.size(); ++index)
    {
        const auto& sourceModel = request.sourceAsset.sourceModels[index];
        stream
            << "|lod" << index << ".kind=" << static_cast<uint32_t>(sourceModel.sourceKind)
            << "|lod" << index << ".screen=" << sourceModel.screenSize
            << "|lod" << index << ".mesh=" << MeshContentIdentity(sourceModel.mesh);
    }
    if (const auto* preset = settings.Find(request.sourceAsset.lodGroup))
    {
        stream
            << "|preset.count=" << preset->numLODs
            << "|preset.percent=" << preset->lodPercentTriangles
            << "|preset.error=" << preset->pixelError;
    }
    stream
        << "|importer=" << request.importerId << ':' << request.importerVersion
        << "|post=" << request.postprocessorVersion
        << "|reducer=" << request.reducerId << ':' << request.reducerVersion
        << "|schema=" << kStaticMeshArtifactSchemaVersion
        << "|target=" << request.targetPlatform;
    return stream.str();
}

StaticMeshCookResult BuildStaticMeshCookArtifact(
    const StaticMeshCookRequest& request,
    const StaticMeshLODSettingsRegistry& settings)
{
    StaticMeshCookResult result;
    result.buildIdentity = BuildStaticMeshCookIdentity(request, settings);
    if (request.outputPath.empty())
    {
        result.diagnostics.push_back("static-mesh-cook-output-path-missing");
        return result;
    }

    if (std::filesystem::exists(request.outputPath))
    {
        auto cachedBytes = ReadFileBytes(request.outputPath);
        if (IsValidCookedArtifact(cachedBytes, result.buildIdentity))
        {
            result.success = true;
            result.cacheHit = true;
            result.artifactBytes = std::move(cachedBytes);
            return result;
        }
    }

    const auto build = BuildStaticMeshLODArtifact(
        request.sourceAsset,
        request.importedLOD0,
        settings);
    if (!build.success)
    {
        result.diagnostics = build.diagnostics;
        return result;
    }

    result.artifactBytes = BuildStoredCookArtifact(
        request,
        build.bundle,
        result.buildIdentity);
    if (result.artifactBytes.empty())
    {
        result.diagnostics.push_back("static-mesh-cook-serialization-failed");
        return result;
    }
    if (!WriteFileAtomically(request.outputPath, result.artifactBytes))
    {
        result.artifactBytes.clear();
        result.diagnostics.push_back("static-mesh-cook-publish-failed");
        return result;
    }

    result.success = true;
    return result;
}
}
