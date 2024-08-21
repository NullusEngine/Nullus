#include "Rendering/Resources/Loaders/TextureLoader.h"

#include "Rendering/Resources/Texture2D.h"
#include "Rendering/Resources/TextureCube.h"

#include <glad/glad.h>
#include <Image.h>
#include <array>

NLS::Render::Resources::Texture2D* NLS::Render::Resources::Loaders::TextureLoader::Create(const std::string& p_filepath, NLS::Render::Settings::ETextureFilteringMode p_firstFilter, NLS::Render::Settings::ETextureFilteringMode p_secondFilter, bool p_generateMipmap)
{
	Image image(p_filepath, true);
	if (image.GetData() == nullptr)
		return nullptr;

	Texture2D* texture = CreateFromImage(&image, p_firstFilter, p_secondFilter, p_generateMipmap);
	if (texture)
	{
		texture->path = p_filepath;
	}

	return texture;
}

NLS::Render::Resources::TextureCube* NLS::Render::Resources::Loaders::TextureLoader::CreateCubeMap(const std::vector<std::string>& filePaths)
{
	if (filePaths.size() != 6)
	{
		return nullptr;
	}

	std::vector<Image> images;
	images.reserve(6);
	for (size_t i = 0; i < 6; ++i)
	{
		images.push_back(Image(filePaths[i], false));
	}

	auto cubeMap = new TextureCube();
	cubeMap->Bind();
	cubeMap->SetTextureResource({&images[0], &images[1], &images[2], &images[3], &images[4], &images[5]});
	cubeMap->Unbind();

	return cubeMap;
}

NLS::Render::Resources::Texture2D* NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	std::array<uint8_t, 4> colorData = { r, g, b, a };

	Image image(1, 1, 4);
	image.SetData(colorData.data());

	return CreateFromImage(
		&image,
		NLS::Render::Settings::ETextureFilteringMode::NEAREST,
		NLS::Render::Settings::ETextureFilteringMode::NEAREST,
		false
	);
}

NLS::Render::Resources::Texture2D* NLS::Render::Resources::Loaders::TextureLoader::CreateFromMemory(uint8_t* p_data, uint32_t p_width, uint32_t p_height, NLS::Render::Settings::ETextureFilteringMode p_firstFilter, NLS::Render::Settings::ETextureFilteringMode p_secondFilter, bool p_generateMipmap)
{
	Image image(p_width, p_height, 4);
	image.SetData(p_data);

	return CreateFromImage(&image, p_firstFilter, p_secondFilter, p_generateMipmap);
}

NLS::Render::Resources::Texture2D* NLS::Render::Resources::Loaders::TextureLoader::CreateFromImage(const Image* image, NLS::Render::Settings::ETextureFilteringMode p_firstFilter, NLS::Render::Settings::ETextureFilteringMode p_secondFilter, bool p_generateMipmap)
{
	Texture2D* texture = new Texture2D;

	texture->Bind();

	texture->isMimapped = p_generateMipmap;
	texture->firstFilter = p_firstFilter;
	texture->secondFilter = p_secondFilter;
	texture->SetTextureResource(image);

	texture->Unbind();

	return texture;
}

void NLS::Render::Resources::Loaders::TextureLoader::Reload(Texture2D* p_texture, const std::string& p_filePath, NLS::Render::Settings::ETextureFilteringMode p_firstFilter, NLS::Render::Settings::ETextureFilteringMode p_secondFilter, bool p_generateMipmap)
{
	if (!p_texture)
	{
		return;
	}

	Texture2D* newTexture = Create(p_filePath, p_firstFilter, p_secondFilter, p_generateMipmap);

	if (newTexture)
	{
		*p_texture = std::move(*newTexture);
		delete newTexture;
	}
}

bool NLS::Render::Resources::Loaders::TextureLoader::Destroy(Texture2D*& p_textureInstance)
{
	if (p_textureInstance)
	{
		delete p_textureInstance;
		p_textureInstance = nullptr;
		return true;
	}

	return false;
}
