#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/IRHIResource.h"
#include "Rendering/RHI/RHITypes.h"

namespace NLS::Render::FrameGraph
{
	struct NLS_RENDER_API FrameGraphBuffer
	{
		using Desc = NLS::Render::RHI::BufferDesc;

		uint32_t id = 0;
		bool ownsResource = false;
		std::shared_ptr<NLS::Render::RHI::IRHIBuffer> bufferResource;
		std::shared_ptr<NLS::Render::RHI::RHIBuffer> explicitBuffer;

		static FrameGraphBuffer WrapExternal(std::shared_ptr<NLS::Render::RHI::IRHIBuffer> externalResource)
		{
			FrameGraphBuffer buffer;
			buffer.id = externalResource != nullptr ? externalResource->GetResourceId() : 0;
			buffer.ownsResource = false;
			buffer.bufferResource = std::move(externalResource);
			return buffer;
		}

		static FrameGraphBuffer WrapExternal(
			std::shared_ptr<NLS::Render::RHI::RHIBuffer> externalBuffer,
			uint32_t externalId = 0)
		{
			FrameGraphBuffer buffer;
			buffer.id = externalId;
			buffer.ownsResource = false;
			buffer.explicitBuffer = std::move(externalBuffer);
			return buffer;
		}

		static FrameGraphBuffer WrapExternal(uint32_t externalId, std::shared_ptr<NLS::Render::RHI::IRHIBuffer> externalResource = {})
		{
			if (externalResource != nullptr)
			{
				auto buffer = WrapExternal(std::move(externalResource));
				buffer.id = externalId != 0 ? externalId : buffer.id;
				return buffer;
			}

			FrameGraphBuffer buffer;
			buffer.id = externalId;
			buffer.ownsResource = false;
			return buffer;
		}

		void create(const Desc& desc, void* allocator);
		void destroy(const Desc& desc, void* allocator);
		void preRead(const Desc& desc, uint32_t flags, void* context);
		void preWrite(const Desc& desc, uint32_t flags, void* context);

		static std::string toString(const Desc& desc);
	};
}
