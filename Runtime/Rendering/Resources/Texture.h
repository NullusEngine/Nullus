#pragma once

#include <memory>
#include <string>
#include <stdint.h>

#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/IRHIResource.h"
#include "RenderDef.h"

namespace NLS::Render::Resources
{
	/**
	* OpenGL texture wrapper
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
		void AdoptTexture(uint32_t p_id, bool p_takeOwnership = false);
		void SetRHITexture(std::shared_ptr<RHI::IRHITexture> texture);

		void SetTextureId(uint32_t p_id) { mTextureID = p_id; }
		uint32_t GetTextureId() const { return mTextureID; }
		const RHI::IRHITexture* GetRHITexture() const { return m_textureResource.get(); }
		const std::shared_ptr<RHI::IRHITexture>& GetRHITextureHandle() const { return m_textureResource; }
		const std::shared_ptr<RHI::RHITexture>& GetExplicitRHITextureHandle() const { return m_explicitTexture; }
		std::shared_ptr<RHI::RHITextureView> GetOrCreateExplicitTextureView(const std::string& debugName = {}) const;

	private:
		/**
		 * @brief opengl texture id
		 */
		uint32_t mTextureID = -1;
		bool m_ownsTexture = true;
		RHI::TextureDimension m_dimension = RHI::TextureDimension::Texture2D;
		std::shared_ptr<RHI::IRHITexture> m_textureResource;
		std::shared_ptr<RHI::RHITexture> m_explicitTexture;
		mutable std::shared_ptr<RHI::RHITextureView> m_explicitTextureView;
	};
}
