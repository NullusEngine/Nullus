#pragma once

#include <stdint.h>
#include <memory>
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
		~Texture2D() = default;

	private:
		Texture2D() = default;

		Texture2D(Texture2D&&) noexcept;
		Texture2D& operator=(Texture2D&&) noexcept;

		void SetTextureResource(const Image*);

	public:
		static std::unique_ptr<Texture2D> WrapExternal(
			const std::shared_ptr<NLS::Render::RHI::RHITexture>& textureResource,
			uint32_t width,
			uint32_t height);

		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t bitsPerPixel = 0;
		Settings::ETextureFilteringMode firstFilter = Settings::ETextureFilteringMode::NEAREST;
		Settings::ETextureFilteringMode secondFilter = Settings::ETextureFilteringMode::NEAREST;
		std::string path;
		bool isMimapped = false;
	};
}
