#pragma once


#include "RenderDef.h"
#include "Rendering/Resources/Texture.h"

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
	class NLS_RENDER_API TextureCube : public Texture
	{

	public:
		TextureCube();
		~TextureCube() = default;

		bool SetTextureResource(const std::vector<const NLS::Image*>& images);

	public:
	};
}
