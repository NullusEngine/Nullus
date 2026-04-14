#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHIResource.h"

namespace NLS::Render::FrameGraph
{
	struct NLS_RENDER_API FrameGraphTexture
	{
		using Desc = NLS::Render::RHI::RHITextureDesc;

		bool ownsResource = false;
		std::shared_ptr<NLS::Render::RHI::RHITexture> explicitTexture;
		std::shared_ptr<NLS::Render::RHI::RHITextureView> explicitView;

		static FrameGraphTexture WrapExternal(
			std::shared_ptr<NLS::Render::RHI::RHITexture> externalTexture,
			std::shared_ptr<NLS::Render::RHI::RHITextureView> externalView = {})
		{
			FrameGraphTexture texture;
			texture.ownsResource = false;
			texture.explicitTexture = std::move(externalTexture);
			texture.explicitView = std::move(externalView);
			return texture;
		}

		void create(const Desc& desc, void* allocator);
		void destroy(const Desc& desc, void* allocator);
		void preRead(const Desc& desc, uint32_t flags, void* context);
		void preWrite(const Desc& desc, uint32_t flags, void* context);

		static std::string toString(const Desc& desc);
	};
}
