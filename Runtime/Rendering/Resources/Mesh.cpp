#include <algorithm>

#include "Rendering/Resources/Mesh.h"

namespace NLS::Render::Resources
{
Mesh::Mesh(const std::vector<Geometry::Vertex>& vertices, const std::vector<uint32_t>& indices, uint32_t materialIndex)
	: m_vertexCount(static_cast<uint32_t>(vertices.size()))
	, m_indicesCount(static_cast<uint32_t>(indices.size()))
	, m_materialIndex(materialIndex)
{
	CreateBuffers(vertices, indices);
	ComputeBoundingSphere(vertices);
}

void Mesh::Bind() const
{
	m_vertexArray.Bind();
	if (m_vertexBuffer)
		m_vertexBuffer->Bind();
	if (m_indexBuffer)
		m_indexBuffer->Bind();
}

void Mesh::Unbind() const
{
	if (m_indexBuffer)
		m_indexBuffer->Unbind();
	if (m_vertexBuffer)
		m_vertexBuffer->Unbind();
	m_vertexArray.Unbind();
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
		m_vertexBuffer ? m_vertexBuffer->GetRHIBufferHandle() : nullptr,
		m_vertexBuffer ? m_vertexBuffer->GetExplicitRHIBufferHandle() : nullptr,
		m_vertexBuffer ? m_vertexBuffer->GetID() : 0u,
		m_vertexStride,
		0u
	};
}

std::optional<MeshBufferView> Mesh::GetIndexBufferView() const
{
	if (!m_indexBuffer || m_indicesCount == 0)
		return std::nullopt;

	return MeshBufferView{
		m_indexBuffer ? m_indexBuffer->GetRHIBufferHandle() : nullptr,
		m_indexBuffer ? m_indexBuffer->GetExplicitRHIBufferHandle() : nullptr,
		m_indexBuffer->GetID(),
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

void Mesh::CreateBuffers(const std::vector<Geometry::Vertex>& vertices, const std::vector<uint32_t>& indices)
{
	std::vector<float> vertexData;

	for (const auto& vertex : vertices)
	{
		vertexData.push_back(vertex.position[0]);
		vertexData.push_back(vertex.position[1]);
		vertexData.push_back(vertex.position[2]);

		vertexData.push_back(vertex.texCoords[0]);
		vertexData.push_back(vertex.texCoords[1]);

		vertexData.push_back(vertex.normals[0]);
		vertexData.push_back(vertex.normals[1]);
		vertexData.push_back(vertex.normals[2]);

		vertexData.push_back(vertex.tangent[0]);
		vertexData.push_back(vertex.tangent[1]);
		vertexData.push_back(vertex.tangent[2]);

		vertexData.push_back(vertex.bitangent[0]);
		vertexData.push_back(vertex.bitangent[1]);
		vertexData.push_back(vertex.bitangent[2]);
	}

	m_vertexBuffer = std::make_unique<Buffers::VertexBuffer<float>>(vertexData);
	m_indexBuffer = std::make_unique<Buffers::IndexBuffer>(const_cast<uint32_t*>(indices.data()), indices.size());

	uint64_t vertexSize = sizeof(Geometry::Vertex);

	m_vertexArray.BindAttribute(0, *m_vertexBuffer, Settings::EDataType::FLOAT, 3, vertexSize, 0);
	m_vertexArray.BindAttribute(1, *m_vertexBuffer, Settings::EDataType::FLOAT, 2, vertexSize, sizeof(float) * 3);
	m_vertexArray.BindAttribute(2, *m_vertexBuffer, Settings::EDataType::FLOAT, 3, vertexSize, sizeof(float) * 5);
	m_vertexArray.BindAttribute(3, *m_vertexBuffer, Settings::EDataType::FLOAT, 3, vertexSize, sizeof(float) * 8);
	m_vertexArray.BindAttribute(4, *m_vertexBuffer, Settings::EDataType::FLOAT, 3, vertexSize, sizeof(float) * 11);
}

void Mesh::ComputeBoundingSphere(const std::vector<Geometry::Vertex>& vertices)
{
	m_boundingSphere.position = Maths::Vector3::Zero;
	m_boundingSphere.radius = 0.0f;

	if (!vertices.empty())
	{
		float minX = std::numeric_limits<float>::max();
		float minY = std::numeric_limits<float>::max();
		float minZ = std::numeric_limits<float>::max();

		float maxX = std::numeric_limits<float>::min();
		float maxY = std::numeric_limits<float>::min();
		float maxZ = std::numeric_limits<float>::min();

		for (const auto& vertex : vertices)
		{
			minX = std::min(minX, vertex.position[0]);
			minY = std::min(minY, vertex.position[1]);
			minZ = std::min(minZ, vertex.position[2]);

			maxX = std::max(maxX, vertex.position[0]);
			maxY = std::max(maxY, vertex.position[1]);
			maxZ = std::max(maxZ, vertex.position[2]);
		}

		m_boundingSphere.position = Maths::Vector3{ minX + maxX, minY + maxY, minZ + maxZ } / 2.0f;

		for (const auto& vertex : vertices)
		{
			const auto& position = reinterpret_cast<const Maths::Vector3&>(vertex.position);
			m_boundingSphere.radius = std::max(m_boundingSphere.radius, Maths::Vector3::Distance(m_boundingSphere.position, position));
		}
	}
}
}
