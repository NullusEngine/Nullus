#include <glad/glad.h>
#include <Image.h>
#include <array>

#include "Rendering/Resources/Loaders/TextureLoader.h"

NLS::Rendering::Resources::Texture* NLS::Rendering::Resources::Loaders::TextureLoader::Create(const std::string& p_filepath, NLS::Rendering::Settings::ETextureFilteringMode p_firstFilter, NLS::Rendering::Settings::ETextureFilteringMode p_secondFilter, bool p_generateMipmap)
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

		return new Texture(p_filepath, textureID, textureWidth, textureHeight, bitsPerPixel, p_firstFilter, p_secondFilter, p_generateMipmap);
	}
	else
	{
		glBindTexture(GL_TEXTURE_2D, 0);
		return nullptr;
	}
}

NLS::Rendering::Resources::Texture* NLS::Rendering::Resources::Loaders::TextureLoader::CreatePixel(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	std::array<uint8_t, 4> colorData = { r, g, b, a };

	return NLS::Rendering::Resources::Loaders::TextureLoader::CreateFromMemory(
		colorData.data(), 1, 1,
		NLS::Rendering::Settings::ETextureFilteringMode::NEAREST,
		NLS::Rendering::Settings::ETextureFilteringMode::NEAREST,
		false
	);
}

NLS::Rendering::Resources::Texture* NLS::Rendering::Resources::Loaders::TextureLoader::CreateFromMemory(uint8_t* p_data, uint32_t p_width, uint32_t p_height, NLS::Rendering::Settings::ETextureFilteringMode p_firstFilter, NLS::Rendering::Settings::ETextureFilteringMode p_secondFilter, bool p_generateMipmap)
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

	return new Texture("", textureID, 1, 1, 32, p_firstFilter, p_secondFilter, p_generateMipmap);
}

void NLS::Rendering::Resources::Loaders::TextureLoader::Reload(Texture& p_texture, const std::string& p_filePath, NLS::Rendering::Settings::ETextureFilteringMode p_firstFilter, NLS::Rendering::Settings::ETextureFilteringMode p_secondFilter, bool p_generateMipmap)
{
	Texture* newTexture = Create(p_filePath, p_firstFilter, p_secondFilter, p_generateMipmap);

	if (newTexture)
	{
		glDeleteTextures(1, &p_texture.id);

		*const_cast<uint32_t*>(&p_texture.id) = newTexture->id;
		*const_cast<uint32_t*>(&p_texture.width) = newTexture->width;
		*const_cast<uint32_t*>(&p_texture.height) = newTexture->height;
		*const_cast<uint32_t*>(&p_texture.bitsPerPixel) = newTexture->bitsPerPixel;
		*const_cast<Settings::ETextureFilteringMode*>(&p_texture.firstFilter) = newTexture->firstFilter;
		*const_cast<Settings::ETextureFilteringMode*>(&p_texture.secondFilter) = newTexture->secondFilter;
		*const_cast<bool*>(&p_texture.isMimapped) = newTexture->isMimapped;
		delete newTexture;
	}
}

bool NLS::Rendering::Resources::Loaders::TextureLoader::Destroy(Texture*& p_textureInstance)
{
	if (p_textureInstance)
	{
		glDeleteTextures(1, &p_textureInstance->id);
		delete p_textureInstance;
		p_textureInstance = nullptr;
		return true;
	}

	return false;
}
