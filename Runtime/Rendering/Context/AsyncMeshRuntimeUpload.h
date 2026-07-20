#pragma once

#include "Rendering/RenderDef.h"
#include "Rendering/Geometry/BoundingSphere.h"
#include "Rendering/Geometry/Vertex.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace NLS::Render::Resources
{
class Mesh;
}

namespace NLS::Render::Context
{
namespace Detail
{
struct MeshRuntimeUploadResultStorage;
}

struct NLS_RENDER_API MeshRuntimeUploadRequest
{
    struct LODResource
    {
        std::vector<Geometry::Vertex> vertices;
        std::vector<uint32_t> indices;
        uint32_t materialIndex = 0u;
        Geometry::BoundingSphere boundingSphere;
        float screenSize = 0.0f;
    };

    std::vector<Geometry::Vertex> vertices;
    std::vector<uint32_t> indices;
    uint32_t materialIndex = 0u;
    Geometry::BoundingSphere boundingSphere;
    std::string debugName;
    std::vector<LODResource> lodResources;
    uint32_t minLOD = 0u;

    size_t ByteSize() const
    {
        size_t size = vertices.size() * sizeof(Geometry::Vertex) + indices.size() * sizeof(uint32_t);
        for (const auto& lod : lodResources)
            size += lod.vertices.size() * sizeof(Geometry::Vertex) + lod.indices.size() * sizeof(uint32_t);
        return size;
    }
};

struct NLS_RENDER_API MeshRuntimeUploadResult
{
    MeshRuntimeUploadResult();
    ~MeshRuntimeUploadResult();
    MeshRuntimeUploadResult(MeshRuntimeUploadResult&&) noexcept;
    MeshRuntimeUploadResult& operator=(MeshRuntimeUploadResult&&) noexcept;
    MeshRuntimeUploadResult(const MeshRuntimeUploadResult&) = delete;
    MeshRuntimeUploadResult& operator=(const MeshRuntimeUploadResult&) = delete;

    bool ready = false;
    bool success = false;
    std::unique_ptr<NLS::Render::Resources::Mesh> mesh;
    std::string diagnostic;
};
}
