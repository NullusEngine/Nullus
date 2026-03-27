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
	struct NLS_RENDER_API FrameGraphTexture
	{
		using Desc = NLS::Render::RHI::TextureDesc;

		uint32_t id = 0;
		bool ownsResource = false;
		std::shared_ptr<NLS::Render::RHI::IRHITexture> textureResource;
		std::shared_ptr<NLS::Render::RHI::RHITexture> explicitTexture;
		std::shared_ptr<NLS::Render::RHI::RHITextureView> explicitView;

		static FrameGraphTexture WrapExternal(std::shared_ptr<NLS::Render::RHI::IRHITexture> externalResource)
		{
			FrameGraphTexture texture;
			texture.id = externalResource != nullptr ? externalResource->GetResourceId() : 0;
			texture.ownsResource = false;
			texture.textureResource = std::move(externalResource);
			return texture;
		}

		static FrameGraphTexture WrapExternal(
			std::shared_ptr<NLS::Render::RHI::RHITexture> externalTexture,
			std::shared_ptr<NLS::Render::RHI::RHITextureView> externalView = {},
			uint32_t externalId = 0)
		{
			FrameGraphTexture texture;
			texture.id = externalId;
			texture.ownsResource = false;
			texture.explicitTexture = std::move(externalTexture);
			texture.explicitView = std::move(externalView);
			return texture;
		}

		static FrameGraphTexture WrapExternal(uint32_t externalId, std::shared_ptr<NLS::Render::RHI::IRHITexture> externalResource = {})
		{
			if (externalResource != nullptr)
			{
				auto texture = WrapExternal(std::move(externalResource));
				texture.id = externalId != 0 ? externalId : texture.id;
				return texture;
			}

			FrameGraphTexture texture;
			texture.id = externalId;
			texture.ownsResource = false;
			return texture;
		}

		void create(const Desc& desc, void* allocator);
		void destroy(const Desc& desc, void* allocator);
		void preRead(const Desc& desc, uint32_t flags, void* context);
		void preWrite(const Desc& desc, uint32_t flags, void* context);

		static std::string toString(const Desc& desc);
	};
}
