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
    std::vector<Geometry::Vertex> vertices;
    std::vector<uint32_t> indices;
    uint32_t materialIndex = 0u;
    Geometry::BoundingSphere boundingSphere;
    std::string debugName;

    size_t ByteSize() const
    {
        return vertices.size() * sizeof(Geometry::Vertex) +
            indices.size() * sizeof(uint32_t);
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
