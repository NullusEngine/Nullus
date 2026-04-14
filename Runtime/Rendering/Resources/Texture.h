#pragma once

#include <memory>
#include <string>
#include <stdint.h>

#include "Rendering/RHI/Core/RHIResource.h"
#include "RenderDef.h"

namespace NLS::Render::Resources
{
	/**
	* OpenGL texture wrapper - Formal RHI only
	*/
	class NLS_RENDER_API Texture
	{
	public:
		/**
		* Bind the texture to the given slot
		* @param p_slot
		*/
		virtual void Bind(uint32_t p_slot = 0) const = 0;

		/**
		* Unbind the texture
		*/
		virtual void Unbind() const = 0;

	public:
		explicit Texture(RHI::TextureDimension dimension = RHI::TextureDimension::Texture2D);
		~Texture();

		Texture(Texture&&) noexcept;
		Texture& operator=(Texture&&) noexcept;

		void CreateRHITexture();
		void ReleaseRHITexture();
		void SetRHITexture(std::shared_ptr<RHI::RHITexture> texture);

		// Legacy API - always returns nullptr now
		void* GetRHITexture() const { return nullptr; }
		const std::shared_ptr<RHI::RHITexture>& GetTextureHandle() const { return m_explicitTexture; }
		// Legacy - always returns nullptr
		void* GetRHITextureHandle() const { return nullptr; }
		const std::shared_ptr<RHI::RHITexture>& GetExplicitRHITextureHandle() const { return GetTextureHandle(); }
		std::shared_ptr<RHI::RHITextureView> GetOrCreateExplicitTextureView(const std::string& debugName = {}) const;

	protected:
		// Legacy - always returns -1
		uint32_t GetCompatibilityTextureId() const { return static_cast<uint32_t>(-1); }

		void RecreateRHITextureIfNeeded(
		    uint32_t width,
		    uint32_t height,
		    RHI::TextureFormat format,
		    RHI::TextureFilter minFilter,
		    RHI::TextureFilter magFilter,
		    RHI::TextureWrap wrapS,
		    RHI::TextureWrap wrapT,
		    bool generateMimaps,
		    const void* initialData);

		RHI::TextureDimension m_dimension = RHI::TextureDimension::Texture2D;
		std::shared_ptr<RHI::RHITexture> m_explicitTexture;
		mutable std::shared_ptr<RHI::RHITextureView> m_explicitTextureView;
	};
}
