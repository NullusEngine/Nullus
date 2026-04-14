#pragma once

#include <cstddef>
#include <optional>
#include <memory>
#include <vector>

#include "Rendering/Buffers/VertexArray.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/RHIMesh.h"
#include "Rendering/RHI/RHITypes.h"

namespace NLS::Render::Resources
{
	struct MeshBufferView
	{
		std::shared_ptr<RHI::IRHIBuffer> buffer;
		std::shared_ptr<RHI::RHIBuffer> explicitBuffer;
		uint32_t bufferId = 0;
		size_t stride = 0;
		size_t offset = 0;
	};

	/**
	* Interface for any mesh
	*/
	class IMesh
	{
	public:
		virtual ~IMesh() = default;
		virtual void Bind() const = 0;
		virtual void Unbind() const = 0;
		virtual uint32_t GetVertexCount() const = 0;
		virtual uint32_t GetIndexCount() const = 0;
		virtual MeshBufferView GetVertexBufferView() const = 0;
		virtual std::optional<MeshBufferView> GetIndexBufferView() const = 0;
		virtual std::shared_ptr<NLS::Render::RHI::RHIMesh> GetRHIMesh() const = 0;
	};
}
