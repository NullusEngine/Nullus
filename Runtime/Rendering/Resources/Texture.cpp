
#include "Rendering/Resources/Texture.h"

#include <glad/glad.h>

using namespace NLS::Render::Resources;

Texture::Texture(Texture&& rhs) noexcept
{
	ReleaseRHITexture();
	mTextureID = rhs.mTextureID;
	rhs.mTextureID = -1;
}

Texture& Texture::operator=(Texture&& rhs) noexcept
{
	if (this != &rhs)
	{
		ReleaseRHITexture();
		mTextureID = rhs.mTextureID;
		rhs.mTextureID = -1;
	}
	return *this;
}

void Texture::CreateRHITexture()
{
	if (mTextureID == -1)
	{
		glGenTextures(1, &mTextureID);
	}
}

void Texture::ReleaseRHITexture()
{
	if (mTextureID != -1)
	{
		glDeleteTextures(1, &mTextureID);
		mTextureID = -1;
	}
}
