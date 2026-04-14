#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/RHITypes.h"

namespace NLS::Render::FrameGraph
{
	struct NLS_RENDER_API FrameGraphBuffer
	{
		// FrameGraph internal buffer descriptor - kept separate from Formal RHI types
		// because FrameGraph needs 'type' and 'usage' fields that map to RHI concepts
		struct Desc
		{
			size_t size = 0;
			NLS::Render::RHI::BufferType type = NLS::Render::RHI::BufferType::ShaderStorage;
			NLS::Render::RHI::BufferUsage usage = NLS::Render::RHI::BufferUsage::DynamicDraw;
		};

		bool ownsResource = false;
		std::shared_ptr<NLS::Render::RHI::RHIBuffer> explicitBuffer;

		static FrameGraphBuffer WrapExternal(std::shared_ptr<NLS::Render::RHI::RHIBuffer> externalBuffer)
		{
			FrameGraphBuffer buffer;
			buffer.ownsResource = false;
			buffer.explicitBuffer = std::move(externalBuffer);
			return buffer;
		}

		void create(const Desc& desc, void* allocator);
		void destroy(const Desc& desc, void* allocator);
		void preRead(const Desc& desc, uint32_t flags, void* context);
		void preWrite(const Desc& desc, uint32_t flags, void* context);

		static std::string toString(const Desc& desc);
	};
}
