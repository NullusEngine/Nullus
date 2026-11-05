#pragma once

#include <atomic>
#include <span>
#include <map>
#include <functional>
#include "Assets/ArtifactWriter.h"
#include "Rendering/Assets/StaticMeshBuildDef.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/ImportedScene.h"
#include "Rendering/DistanceField/DistanceFieldTypes.h"

namespace NLS::Render::DistanceField
{
    enum class DistanceFieldQueryBackend : uint8_t { Auto, BuiltIn, Embree };
    struct DistanceFieldBuildStats
    {
        uint64_t triangles = 0, builtMips = 0, cachedMips = 0, bvhBuilds = 0;
        uint64_t distanceQueries = 0, signRays = 0, triangleTests = 0, sampledVoxels = 0, sharedSamples = 0;
        uint64_t candidateBricks = 0, constantBricks = 0, scratchBytes = 0;
        double geometryMs = 0, bvhMs = 0, distanceMs = 0, signMs = 0, cacheMs = 0, totalMs = 0;
        std::string backend;
    };
    struct DistanceFieldBuildOptions
    {
        std::filesystem::path cacheRoot;
        uint32_t mipMask = 7;
        uint32_t workerCount = 0;
        bool forceRebuild = false;
        // Used by the numerical regression oracle, never by Editor/Cook jobs.
        bool referenceQueries = false;
        DistanceFieldQueryBackend backend = DistanceFieldQueryBackend::Auto;
        DistanceFieldBuildStats* stats = nullptr;
        const MeshDistanceFieldArtifact* seed = nullptr;
        std::function<void(uint32_t, const MeshDistanceFieldArtifact&)> mipReady;
    };
    struct DistanceFieldBuildResult
    {
        bool success = false;
        std::shared_ptr<MeshDistanceFieldArtifact> asset;
        std::vector<std::string> diagnostics;
    };

    NLS_STATIC_MESH_BUILD_API DistanceFieldBuildResult BuildMeshDistanceField(
        std::span<const Assets::MeshArtifactData> sections, const DistanceFieldBuildSettings& settings,
        const std::atomic_bool* cancellation = nullptr,
        const NLS::Core::Assets::IArtifactWriteCancellation* importCancellation = nullptr,
        const DistanceFieldBuildOptions* options = nullptr);

    NLS_STATIC_MESH_BUILD_API std::string ComputeDistanceFieldGeometryIdentity(
        std::span<const Assets::MeshArtifactData> sections);
    NLS_STATIC_MESH_BUILD_API std::string ComputeDistanceFieldBuildIdentity(
        const std::string& geometryIdentity, const DistanceFieldBuildSettings& settings,
        DistanceFieldQueryBackend backend = DistanceFieldQueryBackend::Auto);
    NLS_STATIC_MESH_BUILD_API std::string DistanceFieldQueryBackendName(DistanceFieldQueryBackend backend);
#if defined(NLS_ENABLE_TEST_HOOKS)
    NLS_STATIC_MESH_BUILD_API DistanceFieldBuildResult BuildReferenceMeshDistanceField(
        std::span<const Assets::MeshArtifactData> sections, const DistanceFieldBuildSettings& settings,
        const std::atomic_bool* cancellation = nullptr,
        const NLS::Core::Assets::IArtifactWriteCancellation* importCancellation = nullptr);
#endif

    NLS_STATIC_MESH_BUILD_API DistanceFieldBuildSettings ReadDistanceFieldBuildSettings(
        const std::map<std::string, std::string>& settings, const std::filesystem::path& projectRoot = {});
    NLS_STATIC_MESH_BUILD_API bool AppendDistanceFieldPayloads(const std::string& meshKey,
        std::span<const Assets::MeshArtifactData> sections, const DistanceFieldBuildSettings& settings,
        std::vector<NLS::Core::Assets::ArtifactPayload>& payloads, std::vector<std::string>& diagnostics,
        const std::atomic_bool* cancellation = nullptr,
        const NLS::Core::Assets::IArtifactWriteCancellation* importCancellation = nullptr);
    NLS_STATIC_MESH_BUILD_API bool AppendDistanceFieldModelPayloads(const Assets::ImportedScene& scene,
        const std::map<std::string,Assets::MeshArtifactData>& meshes, const DistanceFieldBuildSettings& settings,
        std::vector<NLS::Core::Assets::ArtifactPayload>& payloads, std::vector<std::string>& diagnostics,
        const std::atomic_bool* cancellation = nullptr,
        const NLS::Core::Assets::IArtifactWriteCancellation* importCancellation = nullptr,
        const struct DistanceFieldModelBuildOptions* options = nullptr);
}
