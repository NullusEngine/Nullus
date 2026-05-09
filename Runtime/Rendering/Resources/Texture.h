#pragma once

#include <memory>
#include <string>
#include <stdint.h>

#include "Rendering/RHI/Core/RHIResource.h"
#include "RenderDef.h"

namespace NLS::Render::Resources
{
	/**
	* Backend-neutral texture wrapper over the formal RHI surface.
	*/
	class NLS_RENDER_API Texture
	{
	public:
		explicit Texture(RHI::TextureDimension dimension = RHI::TextureDimension::Texture2D);
		~Texture();

		Texture(Texture&&) noexcept;
		Texture& operator=(Texture&&) noexcept;

		void CreateRHITexture();
		void ReleaseRHITexture();
		void SetRHITexture(std::shared_ptr<RHI::RHITexture> texture);

		const std::shared_ptr<RHI::RHITexture>& GetTextureHandle() const { return m_explicitTexture; }
		const std::shared_ptr<RHI::RHITexture>& GetExplicitRHITextureHandle() const { return GetTextureHandle(); }
		std::shared_ptr<RHI::RHITextureView> GetOrCreateExplicitTextureView(const std::string& debugName = {}) const;

	protected:
		bool RecreateRHITextureIfNeeded(
		    uint32_t width,
		    uint32_t height,
		    RHI::TextureFormat format,
		    RHI::TextureFilter minFilter,
		    RHI::TextureFilter magFilter,
		    RHI::TextureWrap wrapS,
		    RHI::TextureWrap wrapT,
		    bool generateMimaps,
		    const void* initialData,
		    size_t initialDataSize);

		RHI::TextureDimension m_dimension = RHI::TextureDimension::Texture2D;
		std::shared_ptr<RHI::RHITexture> m_explicitTexture;
		mutable std::shared_ptr<RHI::RHITextureView> m_explicitTextureView;
	};
}
