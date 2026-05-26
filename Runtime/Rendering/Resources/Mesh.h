#pragma once

#include <cstddef>
#include <vector>
#include <memory>

#include "Object/Object.h"
#include "Reflection/Macros.h"
#include "Rendering/Buffers/VertexArray.h"
#include "Rendering/Buffers/IndexBuffer.h"
#include "Rendering/Resources/IMesh.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/Geometry/BoundingSphere.h"
#include "Resources/Mesh.generated.h"
#include "RenderDef.h"
namespace NLS::Render::Resources
{
	struct NLS_RENDER_API MeshVertexUploadView
	{
		const Geometry::Vertex* data = nullptr;
		size_t byteSize = 0u;
		size_t stride = sizeof(Geometry::Vertex);
		size_t positionOffset = 0u;
		size_t texCoordOffset = sizeof(float) * 3u;
		size_t normalOffset = sizeof(float) * 5u;
		size_t tangentOffset = sizeof(float) * 8u;
		size_t bitangentOffset = sizeof(float) * 11u;
	};

	NLS_RENDER_API MeshVertexUploadView BuildMeshVertexUploadView(const std::vector<Geometry::Vertex>& vertices);

	enum class MeshBufferUploadMode
	{
		GpuOnly,
		CpuToGpu
	};

	/**
	* Standard mesh of Rendering
	*/
	CLASS(NLS_RENDER_API Mesh) : public NLS::NamedObject, public IMesh
	{
	public:
		GENERATED_BODY()

		/**
		* Create a mesh with the given vertices, indices and material index
		* @param p_vertices
		* @param p_indices
		* @param p_materialIndex
		*/
		Mesh(
			const std::vector<Geometry::Vertex>& p_vertices,
			const std::vector<uint32_t>& p_indices,
			uint32_t p_materialIndex);
		Mesh(
			const std::vector<Geometry::Vertex>& p_vertices,
			const std::vector<uint32_t>& p_indices,
			uint32_t p_materialIndex,
			MeshBufferUploadMode uploadMode);
		Mesh(
			const std::vector<Geometry::Vertex>& p_vertices,
			const std::vector<uint32_t>& p_indices,
			uint32_t p_materialIndex,
			MeshBufferUploadMode uploadMode,
			const Geometry::BoundingSphere& boundingSphere);
		Mesh(const Mesh&) = delete;
		Mesh& operator=(const Mesh&) = delete;
		Mesh(Mesh&&) = delete;
		Mesh& operator=(Mesh&&) = delete;

		/**
		* Returns the number of vertices
		*/
		virtual uint32_t GetVertexCount() const override;

		/**
		* Returns the number of indices
		*/
		virtual uint32_t GetIndexCount() const override;

		/**
		* Returns the native vertex buffer binding information.
		*/
		virtual MeshBufferView GetVertexBufferView() const override;

		/**
		* Returns the native index buffer binding information if indexed.
		*/
		virtual std::optional<MeshBufferView> GetIndexBufferView() const override;
		virtual std::shared_ptr<NLS::Render::RHI::RHIMesh> GetRHIMesh() const override;

		/**
		* Returns the material index of the mesh
		*/
		uint32_t GetMaterialIndex() const;

		/**
		* Returns the bounding sphere of the mesh
		*/
		const Render::Geometry::BoundingSphere& GetBoundingSphere() const;
		void Reload(
			const std::vector<Geometry::Vertex>& p_vertices,
			const std::vector<uint32_t>& p_indices,
			uint32_t p_materialIndex,
			MeshBufferUploadMode uploadMode,
			const Geometry::BoundingSphere& boundingSphere);
		bool UpdateVertices(
			const std::vector<Geometry::Vertex>& p_vertices,
			const Geometry::BoundingSphere& boundingSphere,
			uint32_t destinationVertexOffset = 0u);

	private:
		void CreateBuffers(
			const std::vector<Geometry::Vertex>& p_vertices,
			const std::vector<uint32_t>& p_indices,
			MeshBufferUploadMode uploadMode);
		void ComputeBoundingSphere(const std::vector<Geometry::Vertex>& p_vertices);

	private:
		uint32_t m_vertexCount;
		uint32_t m_indicesCount;
		uint32_t m_materialIndex;
		const size_t m_vertexStride = sizeof(Geometry::Vertex);
		std::shared_ptr<NLS::Render::RHI::RHIMesh> m_rhiMesh;

		Buffers::VertexArray							m_vertexArray;
		std::unique_ptr<Buffers::VertexBuffer<Geometry::Vertex>>	m_vertexBuffer;
		std::unique_ptr<Buffers::IndexBuffer>			m_indexBuffer;

		Geometry::BoundingSphere m_boundingSphere;
	};
}
