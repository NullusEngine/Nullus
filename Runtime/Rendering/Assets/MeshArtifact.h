#pragma once

#include "RenderDef.h"
#include "Rendering/Geometry/BoundingSphere.h"
#include "Rendering/Geometry/Vertex.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace NLS::Render::Assets
{
struct MeshArtifactData
{
    std::vector<Geometry::Vertex> vertices;
    std::vector<uint32_t> indices;
    uint32_t materialIndex = 0u;
    Geometry::BoundingSphere boundingSphere{};
    bool hasBoundingSphere = false;
};

struct MeshArtifactHeaderPreview
{
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t materialIndex = 0u;
    Geometry::BoundingSphere boundingSphere {};
    bool hasBoundingSphere = false;
    bool isLODBundle = false;
};

struct MeshArtifactLODResource
{
    MeshArtifactData mesh;
    float screenSize = 1.0f;
};

struct MeshArtifactBundle
{
    uint32_t schemaVersion = 1u;
    uint32_t minLOD = 0u;
    std::vector<MeshArtifactLODResource> lodResources;
};

NLS_RENDER_API std::vector<uint8_t> SerializeMeshArtifact(const MeshArtifactData& mesh);
NLS_RENDER_API std::optional<MeshArtifactData> DeserializeMeshArtifact(const std::vector<uint8_t>& bytes);
NLS_RENDER_API std::vector<uint8_t> SerializeMeshArtifactBundle(const MeshArtifactBundle& bundle);
NLS_RENDER_API std::optional<MeshArtifactBundle> DeserializeMeshArtifactBundle(
    const std::vector<uint8_t>& bytes);
NLS_RENDER_API uint32_t SelectMeshArtifactLOD(
    const MeshArtifactBundle& bundle,
    float screenSize,
    uint32_t minLOD = 0u,
    uint32_t maxLOD = UINT32_MAX);
NLS_RENDER_API std::optional<MeshArtifactBundle> LoadMeshArtifactBundle(
    const std::filesystem::path& path);
NLS_RENDER_API std::optional<MeshArtifactData> LoadMeshArtifactLOD(
    const std::filesystem::path& path,
    float screenSize,
    uint32_t minLOD = 0u,
    uint32_t maxLOD = UINT32_MAX);
#if defined(NLS_ENABLE_TEST_HOOKS)
// Reproduces the trusted buffered artifact path for same-process performance comparisons.
NLS_RENDER_API std::optional<MeshArtifactData> DeserializeMeshArtifactTrustedForTesting(
    const std::vector<uint8_t>& bytes);
#endif
NLS_RENDER_API std::optional<MeshArtifactHeaderPreview> ReadMeshArtifactHeaderPreview(
    const std::filesystem::path& path,
    uint64_t maxMetadataBytes = UINT64_MAX);
// Validates the fixed native container header without parsing artifact metadata.
NLS_RENDER_API bool IsMeshArtifactFile(const std::filesystem::path& path);
NLS_RENDER_API std::optional<MeshArtifactData> LoadMeshArtifact(const std::filesystem::path& path);
NLS_RENDER_API std::optional<MeshArtifactData> LoadMeshArtifact(
    const std::filesystem::path& path,
    const std::atomic_bool* cancellationFlag);
}
