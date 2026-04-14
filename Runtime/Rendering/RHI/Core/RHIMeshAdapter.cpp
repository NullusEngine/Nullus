#include "Rendering/RHI/Core/RHIMeshAdapter.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/RHI/Core/RHIDevice.h"

namespace NLS::Render::RHI
{
RHIMeshAdapter::RHIMeshAdapter(const Resources::Mesh& mesh)
{
    m_vertexCount = mesh.GetVertexCount();
    m_indexCount = mesh.GetIndexCount();
    m_vertexStride = sizeof(Geometry::Vertex);

    // Get formal RHI buffers from mesh's VertexBuffer/IndexBuffer
    auto vertexView = mesh.GetVertexBufferView();
    if (vertexView.explicitBuffer)
        m_vertexBuffer = vertexView.explicitBuffer;

    auto indexView = mesh.GetIndexBufferView();
    if (indexView.has_value() && indexView->explicitBuffer)
        m_indexBuffer = indexView->explicitBuffer;
}

RHIMeshAdapter::~RHIMeshAdapter() = default;

std::shared_ptr<RHIBuffer> RHIMeshAdapter::GetVertexBuffer() const
{
    return m_vertexBuffer;
}

std::shared_ptr<RHIBuffer> RHIMeshAdapter::GetIndexBuffer() const
{
    return m_indexBuffer;
}
}