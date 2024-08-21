#pragma once

#include "RenderDef.h"
#include "Rendering/Settings/ETextureFilteringMode.h"

#include <string>
#include <vector>

namespace NLS
{
	class Image;
}

namespace NLS::Render::Resources
{
	class Texture2D;
	class TextureCube;
}

namespace NLS::Render::Resources::Loaders
{
	/**
	* Handle the Texture creation and destruction
	*/
	class NLS_RENDER_API TextureLoader
	{
	public:
		/**
		* Disabled constructor
		*/
		TextureLoader() = delete;

		/**
		* Create a texture from file
		* @param p_filePath
		* @param p_firstFilter
		* @param p_secondFilter
		* @param p_generateMipmap
		*/
		static Texture2D* Create(const std::string& p_filepath, NLS::Render::Settings::ETextureFilteringMode p_firstFilter, NLS::Render::Settings::ETextureFilteringMode p_secondFilter, bool p_generateMipmap);

		static TextureCube* CreateCubeMap(const std::vector<std::string>& filePaths);

		/**
		* Create a texture from a single pixel color
		* @param p_r
		* @param p_g
		* @param p_b
		* @param p_a
		*/
		static Texture2D* CreatePixel(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

		/**
		* Create a texture from memory
		* @param p_data
		* @param p_width
		* @param p_height
		* @param p_firstFilder
		* @param p_secondFilter
		* @param p_generateMipmap
		*/
		static Texture2D* CreateFromMemory(uint8_t* p_data, uint32_t p_width, uint32_t p_height, NLS::Render::Settings::ETextureFilteringMode p_firstFilter, NLS::Render::Settings::ETextureFilteringMode p_secondFilter, bool p_generateMipmap);

		static Texture2D* CreateFromImage(const Image* iamge, NLS::Render::Settings::ETextureFilteringMode p_firstFilter, NLS::Render::Settings::ETextureFilteringMode p_secondFilter, bool p_generateMipmap);

		/**
		* Reload a texture from file
		* @param p_texture
		* @param p_filePath
		* @param p_firstFilter
		* @param p_secondFilter
		* @param p_generateMipmap
		*/
		static void Reload(Texture2D* p_texture, const std::string& p_filePath, NLS::Render::Settings::ETextureFilteringMode p_firstFilter, NLS::Render::Settings::ETextureFilteringMode p_secondFilter, bool p_generateMipmap);

		/**
		* Destroy a texture
		* @param p_textureInstance
		*/
		static bool Destroy(Texture2D*& p_textureInstance);
	};
}