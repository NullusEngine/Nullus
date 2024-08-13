#include "Rendering/Resources/Loaders/TextureLoader.h"

#include "Rendering/Resources/Texture2D.h"
#include "Rendering/Resources/TextureCube.h"

#include <glad/glad.h>
#include <Image.h>
#include <array>

NLS::Render::Resources::Texture2D* NLS::Render::Resources::Loaders::TextureLoader::Create(const std::string& p_filepath, NLS::Render::Settings::ETextureFilteringMode p_firstFilter, NLS::Render::Settings::ETextureFilteringMode p_secondFilter, bool p_generateMipmap)
{
	GLuint textureID;
	glGenTextures(1, &textureID);

	Image image(p_filepath, true);
    unsigned char* dataBuffer = image.GetData();
    int textureWidth = image.GetWidth();
    int textureHeight = image.GetHeight();
    int bitsPerPixel = image.GetChannels();
	if (dataBuffer)
	{
		// TODO: Cleanup this code to use "Load from Memory"
		glBindTexture(GL_TEXTURE_2D, textureID);

		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, textureWidth, textureHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, dataBuffer);

		if (p_generateMipmap)
		{
			glGenerateMipmap(GL_TEXTURE_2D);
		}
		
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(p_firstFilter));
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(p_secondFilter));
		glBindTexture(GL_TEXTURE_2D, 0);

		return new Texture2D(p_filepath, textureID, textureWidth, textureHeight, bitsPerPixel, p_firstFilter, p_secondFilter, p_generateMipmap);
	}
	else
	{
		glBindTexture(GL_TEXTURE_2D, 0);
		return nullptr;
	}
}

NLS::Render::Resources::TextureCube* NLS::Render::Resources::Loaders::TextureLoader::CreateCubeMap(const std::vector<std::string>& filePaths)
{
	if (filePaths.size() != 6)
	{
		return nullptr;
	}

	std::vector<Image> images;
	images.reserve(6);
	std::vector<Image*> imagesPtr;
	imagesPtr.reserve(6);
	for (size_t i = 0; i < 6; ++i)
	{
		images.push_back(Image(filePaths[i], false));
		imagesPtr.push_back(&images[i]);
	}

	auto cubeMap = new TextureCube();
	cubeMap->Bind();
	cubeMap->SetTextureResource(imagesPtr);
	cubeMap->Unbind();

	return cubeMap;
}

NLS::Render::Resources::Texture2D* NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	std::array<uint8_t, 4> colorData = { r, g, b, a };

	return NLS::Render::Resources::Loaders::TextureLoader::CreateFromMemory(
		colorData.data(), 1, 1,
		NLS::Render::Settings::ETextureFilteringMode::NEAREST,
		NLS::Render::Settings::ETextureFilteringMode::NEAREST,
		false
	);
}

NLS::Render::Resources::Texture2D* NLS::Render::Resources::Loaders::TextureLoader::CreateFromMemory(uint8_t* p_data, uint32_t p_width, uint32_t p_height, NLS::Render::Settings::ETextureFilteringMode p_firstFilter, NLS::Render::Settings::ETextureFilteringMode p_secondFilter, bool p_generateMipmap)
{
	GLuint textureID;
	glGenTextures(1, &textureID);
	glBindTexture(GL_TEXTURE_2D, textureID);

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, p_width, p_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, p_data);

	if (p_generateMipmap)
	{
		glGenerateMipmap(GL_TEXTURE_2D);
	}

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(p_firstFilter));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(p_secondFilter));

	glBindTexture(GL_TEXTURE_2D, 0);

	return new Texture2D("", textureID, 1, 1, 32, p_firstFilter, p_secondFilter, p_generateMipmap);
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
