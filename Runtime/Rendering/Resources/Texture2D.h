#pragma once

#include <stdint.h>
#include <string>

#include "RenderDef.h"
#include "Rendering/Settings/ETextureFilteringMode.h"
#include "Rendering/Resources/Texture.h"

namespace NLS
{
	class Image;
}

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
		Texture2D() = default;
		~Texture2D() = default;

		Texture2D(Texture2D&&) noexcept;
		Texture2D& operator=(Texture2D&&) noexcept;

		void SetTextureResource(const Image*);

	public:
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t bitsPerPixel = 0;
		Settings::ETextureFilteringMode firstFilter = Settings::ETextureFilteringMode::NEAREST;
		Settings::ETextureFilteringMode secondFilter = Settings::ETextureFilteringMode::NEAREST;
		std::string path;
		bool isMimapped = false;
	};
}