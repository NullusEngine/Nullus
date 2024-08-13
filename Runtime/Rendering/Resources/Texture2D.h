#pragma once

#include <stdint.h>
#include <string>

#include "RenderDef.h"
#include "Rendering/Settings/ETextureFilteringMode.h"
#include "Rendering/Resources/Texture.h"

namespace NLS::Render::Resources
{
	namespace Loaders { class TextureLoader; }

	class NLS_RENDER_API Texture2D : public Texture
	{
		friend class Loaders::TextureLoader;

	public:
		/**
		* Bind the texture to the given slot
		* @param p_slot
		*/
		virtual void Bind(uint32_t p_slot = 0) const override;

		/**
		* Unbind the texture
		*/
		virtual void Unbind() const override;

	private:
		Texture2D(const std::string p_path, uint32_t p_id, uint32_t p_width, uint32_t p_height, uint32_t p_bpp, Settings::ETextureFilteringMode p_firstFilter, Settings::ETextureFilteringMode p_secondFilter, bool p_generateMipmap);
		~Texture2D() = default;

		Texture2D(Texture2D&&) noexcept;
		Texture2D& operator=(Texture2D&&) noexcept;

	public:
		uint32_t width;
		uint32_t height;
		uint32_t bitsPerPixel;
		Settings::ETextureFilteringMode firstFilter;
		Settings::ETextureFilteringMode secondFilter;
		std::string path;
		bool isMimapped;
	};
}