#include "Rendering/Resources/Mesh.h"
#include "Rendering/Geometry/BoundingSphereUtils.h"
#include "Rendering/RHI/Core/RHIMeshAdapter.h"

#include <atomic>

namespace NLS::Render::Resources
{
MeshVertexUploadView BuildMeshVertexUploadView(const std::vector<Geometry::Vertex>& vertices)
{
	MeshVertexUploadView view;
	view.data = vertices.empty() ? nullptr : vertices.data();
	view.byteSize = vertices.size() * sizeof(Geometry::Vertex);
	return view;
}

namespace
{
uint64_t NextMeshInstanceId()
{
	static std::atomic<uint64_t> nextInstanceId { 1u };
	auto instanceId = nextInstanceId.fetch_add(1u, std::memory_order_relaxed);
	if (instanceId == 0u)
		instanceId = nextInstanceId.fetch_add(1u, std::memory_order_relaxed);
	return instanceId;
}

NLS::Render::RHI::MemoryUsage ToBufferMemoryUsage(const MeshBufferUploadMode uploadMode)
{
	return uploadMode == MeshBufferUploadMode::CpuToGpu
		? NLS::Render::RHI::MemoryUsage::CPUToGPU
		: NLS::Render::RHI::MemoryUsage::GPUOnly;
}
}

Mesh::Mesh(
	const std::vector<Geometry::Vertex>& vertices,
	const std::vector<uint32_t>& indices,
	uint32_t materialIndex)
	: Mesh(vertices, indices, materialIndex, MeshBufferUploadMode::GpuOnly)
{
}

Mesh::Mesh(
	const std::vector<Geometry::Vertex>& vertices,
	const std::vector<uint32_t>& indices,
	uint32_t materialIndex,
	const MeshBufferUploadMode uploadMode)
	: m_vertexCount(static_cast<uint32_t>(vertices.size()))
	, m_indicesCount(static_cast<uint32_t>(indices.size()))
	, m_materialIndex(materialIndex)
	, m_instanceId(NextMeshInstanceId())
{
	CreateBuffers(vertices, indices, uploadMode);
	ComputeBoundingSphere(vertices);
	m_rhiMesh = std::make_shared<RHI::RHIMeshAdapter>(*this);
}

Mesh::Mesh(
	const std::vector<Geometry::Vertex>& vertices,
	const std::vector<uint32_t>& indices,
	uint32_t materialIndex,
	const MeshBufferUploadMode uploadMode,
	const Geometry::BoundingSphere& boundingSphere)
	: m_vertexCount(static_cast<uint32_t>(vertices.size()))
	, m_indicesCount(static_cast<uint32_t>(indices.size()))
	, m_materialIndex(materialIndex)
	, m_boundingSphere(boundingSphere)
	, m_instanceId(NextMeshInstanceId())
{
	CreateBuffers(vertices, indices, uploadMode);
	m_rhiMesh = std::make_shared<RHI::RHIMeshAdapter>(*this);
}

uint32_t Mesh::GetVertexCount() const
{
	return m_vertexCount;
}

uint32_t Mesh::GetIndexCount() const
{
	return m_indicesCount;
}

MeshBufferView Mesh::GetVertexBufferView() const
{
	return MeshBufferView{
		m_vertexBuffer ? m_vertexBuffer->GetBufferHandle() : nullptr,
		m_vertexStride,
		0u
	};
}

std::optional<MeshBufferView> Mesh::GetIndexBufferView() const
{
	if (!m_indexBuffer || m_indicesCount == 0)
		return std::nullopt;

	return MeshBufferView{
		m_indexBuffer ? m_indexBuffer->GetBufferHandle() : nullptr,
		sizeof(uint32_t),
		0u
	};
}

uint32_t Mesh::GetMaterialIndex() const
{
	return m_materialIndex;
}

const Render::Geometry::BoundingSphere& Mesh::GetBoundingSphere() const
{
	return m_boundingSphere;
}

uint64_t Mesh::GetInstanceId() const
{
	return m_instanceId;
}

uint64_t Mesh::GetContentRevision() const
{
	return m_contentRevision;
}

void Mesh::Reload(
	const std::vector<Geometry::Vertex>& vertices,
	const std::vector<uint32_t>& indices,
	const uint32_t materialIndex,
	const MeshBufferUploadMode uploadMode,
	const Geometry::BoundingSphere& boundingSphere)
{
	m_vertexCount = static_cast<uint32_t>(vertices.size());
	m_indicesCount = static_cast<uint32_t>(indices.size());
	m_materialIndex = materialIndex;
	m_boundingSphere = boundingSphere;
	CreateBuffers(vertices, indices, uploadMode);
	m_rhiMesh = std::make_shared<RHI::RHIMeshAdapter>(*this);
	TouchContentRevision();
}

bool Mesh::UpdateVertices(
	const std::vector<Geometry::Vertex>& vertices,
	const Geometry::BoundingSphere& boundingSphere,
	const uint32_t destinationVertexOffset)
{
	const auto destinationEnd = static_cast<size_t>(destinationVertexOffset) + vertices.size();
	if (destinationEnd > m_vertexCount || m_vertexBuffer == nullptr || m_indexBuffer != nullptr)
		return false;

	if (!m_vertexBuffer->Update(vertices.data(), vertices.size(), destinationVertexOffset))
		return false;

	m_boundingSphere = boundingSphere;
	TouchContentRevision();
	return true;
}

void Mesh::TouchContentRevision()
{
	++m_contentRevision;
	if (m_contentRevision == 0u)
		m_contentRevision = 1u;
}

void Mesh::CreateBuffers(
	const std::vector<Geometry::Vertex>& vertices,
	const std::vector<uint32_t>& indices,
	const MeshBufferUploadMode uploadMode)
{
	const auto memoryUsage = ToBufferMemoryUsage(uploadMode);
	m_vertexBuffer = std::make_unique<Buffers::VertexBuffer<Geometry::Vertex>>(
		BuildMeshVertexUploadView(vertices).data,
		vertices.size(),
		memoryUsage);
	if (!indices.empty())
		m_indexBuffer = std::make_unique<Buffers::IndexBuffer>(
			const_cast<uint32_t*>(indices.data()),
			indices.size(),
			memoryUsage);

	uint64_t vertexSize = sizeof(Geometry::Vertex);

	m_vertexArray.BindAttribute(0, *m_vertexBuffer, Settings::EDataType::FLOAT, 3, vertexSize, 0);
	m_vertexArray.BindAttribute(1, *m_vertexBuffer, Settings::EDataType::FLOAT, 2, vertexSize, sizeof(float) * 3);
	m_vertexArray.BindAttribute(2, *m_vertexBuffer, Settings::EDataType::FLOAT, 3, vertexSize, sizeof(float) * 5);
	m_vertexArray.BindAttribute(3, *m_vertexBuffer, Settings::EDataType::FLOAT, 3, vertexSize, sizeof(float) * 8);
	m_vertexArray.BindAttribute(4, *m_vertexBuffer, Settings::EDataType::FLOAT, 3, vertexSize, sizeof(float) * 11);
}

void Mesh::ComputeBoundingSphere(const std::vector<Geometry::Vertex>& vertices)
{
	m_boundingSphere = Geometry::ComputeBoundingSphere(vertices);
}

std::shared_ptr<NLS::Render::RHI::RHIMesh> Mesh::GetRHIMesh() const
{
	return m_rhiMesh;
}
}
