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
};

NLS_RENDER_API std::vector<uint8_t> SerializeMeshArtifact(const MeshArtifactData& mesh);
NLS_RENDER_API std::optional<MeshArtifactData> DeserializeMeshArtifact(const std::vector<uint8_t>& bytes);
NLS_RENDER_API std::optional<MeshArtifactHeaderPreview> ReadMeshArtifactHeaderPreview(
    const std::filesystem::path& path,
    uint64_t maxMetadataBytes = UINT64_MAX);
// Validates the fixed native container header without parsing artifact metadata.
NLS_RENDER_API bool IsMeshArtifactFile(const std::filesystem::path& path);
NLS_RENDER_API std::optional<MeshArtifactData> LoadMeshArtifactPreviewSample(
    const std::filesystem::path& path,
    uint32_t maxVertices,
    uint32_t maxIndices,
    uint64_t maxMetadataBytes = UINT64_MAX);
NLS_RENDER_API std::optional<MeshArtifactData> LoadMeshArtifact(const std::filesystem::path& path);
NLS_RENDER_API std::optional<MeshArtifactData> LoadMeshArtifact(
    const std::filesystem::path& path,
    const std::atomic_bool* cancellationFlag);
}
