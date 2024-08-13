
#include <glad/glad.h>

#include <Image.h>
#include "Rendering/Resources/TextureCube.h"

using namespace NLS::Render::Resources;

void TextureCube::Bind(uint32_t p_slot) const
{
	glBindTexture(GL_TEXTURE_CUBE_MAP, GetTextureId());
}

void TextureCube::Unbind() const
{
	glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
}

TextureCube::TextureCube()
{
	CreateRHITexture();
}

TextureCube::~TextureCube()
{
	ReleaseRHITexture();
}

bool TextureCube::SetTextureResource(const std::vector<NLS::Image*>& images)
{
	// X+ X- Y+ Y- Z+ Z-
	for (size_t i = 0; i < 6; ++i)
	{
		const NLS::Image* image = images[i];
		if (!image)
		{
			return false;
		}

		const unsigned char* dataBuffer = image->GetData();
		int textureWidth = image->GetWidth();
		int textureHeight = image->GetHeight();
		int bitsPerPixel = image->GetChannels();

		glTexImage2D(
			GL_TEXTURE_CUBE_MAP_POSITIVE_X + i,
			0, GL_RGB, textureWidth, textureHeight, 0, GL_RGB, GL_UNSIGNED_BYTE, dataBuffer
		);
	}

	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

	return true;
}
