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
		/**
		* Bind the texture to the given slot
		* @param p_slot
		*/
		virtual void Bind(uint32_t p_slot = 0) const override;

		/**
		* Unbind the texture
		*/
		virtual void Unbind() const override;

	public:
		TextureCube() = default;
		~TextureCube() = default;

		bool SetTextureResource(const std::vector<const NLS::Image*>& images);

	public:
	};
}