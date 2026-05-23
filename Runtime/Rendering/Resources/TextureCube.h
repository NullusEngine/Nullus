#pragma once


#include "RenderDef.h"
#include "Reflection/Macros.h"
#include "Rendering/Resources/Texture.h"
#include "Resources/TextureCube.generated.h"

#include <vector>

namespace NLS
{
    class Image;
}
namespace NLS::Render::Resources
{
	/**
	* Texture CubeMap
	*/
	CLASS(NLS_RENDER_API TextureCube) : public Texture
	{

	public:
		GENERATED_BODY()

		TextureCube();
		~TextureCube() = default;

		bool SetTextureResource(const std::vector<const NLS::Image*>& images);

	public:
	};
}
