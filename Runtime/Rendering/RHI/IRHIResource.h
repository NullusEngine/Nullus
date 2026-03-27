#pragma once

#include <cstddef>
#include <cstdint>

#include "Rendering/RHI/RHITypes.h"
#include "RenderDef.h"

namespace NLS::Render::RHI
{
	enum class NLS_RENDER_API RHIResourceType : uint8_t
	{
		Unknown,
		Texture,
		Buffer
	};

	class NLS_RENDER_API IRHIResource
	{
	public:
		virtual ~IRHIResource() = default;

		virtual RHIResourceType GetResourceType() const = 0;
		virtual uint32_t GetResourceId() const = 0;
	};

	class NLS_RENDER_API IRHITexture : public IRHIResource
	{
	public:
		virtual TextureDimension GetDimension() const = 0;
		virtual const TextureDesc& GetDesc() const = 0;
		virtual void SetDesc(const TextureDesc& desc) = 0;
	};

	class NLS_RENDER_API IRHIBuffer : public IRHIResource
	{
	public:
		virtual BufferType GetBufferType() const = 0;
		virtual size_t GetSize() const = 0;
		virtual void SetSize(size_t size) = 0;
	};
}
