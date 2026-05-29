#pragma once

#include <stdint.h>
#include <memory>
#include <string>

#include "Rendering/Assets/TextureArtifact.h"
#include "Reflection/Macros.h"
#include "RenderDef.h"
#include "Rendering/Settings/ETextureFilteringMode.h"
#include "Rendering/Resources/Texture.h"
#include "Resources/Texture2D.generated.h"

namespace NLS
{
    class Image;
}

namespace NLS::Render::Resources
{
	namespace Loaders { class TextureLoader; }

	CLASS(NLS_RENDER_API Texture2D) : public Texture
	{
		friend class Loaders::TextureLoader;

	public:
		GENERATED_BODY()

		~Texture2D() = default;
		Texture2D(const Texture2D&) = delete;
		Texture2D& operator=(const Texture2D&) = delete;
		Texture2D(Texture2D&&) = delete;
		Texture2D& operator=(Texture2D&&) = delete;

	private:
		Texture2D() = default;

		void SetTextureResource(const Image*);
		bool SetTextureResource(const NLS::Render::Assets::TextureArtifactData& artifact);
		void ReloadFrom(const Texture2D& source);

	public:
		static std::unique_ptr<Texture2D> WrapExternal(
			const std::shared_ptr<NLS::Render::RHI::RHITexture>& textureResource,
			uint32_t width,
			uint32_t height);
		void WrapExternalInPlace(
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
