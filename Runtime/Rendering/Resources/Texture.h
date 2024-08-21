#pragma once

#include <stdint.h>

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
		Texture();
		~Texture();

		Texture(Texture&&) noexcept;
		Texture& operator=(Texture&&) noexcept;

		void CreateRHITexture();
		void ReleaseRHITexture();

		void SetTextureId(uint32_t p_id) { mTextureID = p_id; }
		uint32_t GetTextureId() const { return mTextureID; }

	private:
		/**
		 * @brief opengl texture id
		 */
		uint32_t mTextureID = -1;
	};
}