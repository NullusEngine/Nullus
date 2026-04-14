#pragma once

#include "Rendering/RHI/Core/RHIMesh.h"
#include <memory>

namespace NLS::Render::Resources
{
class Mesh;
}

namespace NLS::Render::RHI
{
class NLS_RENDER_API RHIMeshAdapter : public RHIMesh
{
public:
    explicit RHIMeshAdapter(const Resources::Mesh& mesh);
    ~RHIMeshAdapter() override;

    std::shared_ptr<RHIBuffer> GetVertexBuffer() const override;
    std::shared_ptr<RHIBuffer> GetIndexBuffer() const override;
    uint32_t GetVertexCount() const override { return m_vertexCount; }
    uint32_t GetIndexCount() const override { return m_indexCount; }
    Settings::EPrimitiveMode GetPrimitiveMode() const override { return Settings::EPrimitiveMode::TRIANGLES; }
    uint32_t GetVertexStride() const override { return m_vertexStride; }
    IndexType GetIndexType() const override { return IndexType::UInt32; }

private:
    std::shared_ptr<RHIBuffer> m_vertexBuffer;
    std::shared_ptr<RHIBuffer> m_indexBuffer;
    uint32_t m_vertexCount = 0;
    uint32_t m_indexCount = 0;
    uint32_t m_vertexStride = 0;
};
}