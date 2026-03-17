
#include "Rendering/Resources/Texture.h"

#include <glad/glad.h>

using namespace NLS::Render::Resources;

Texture::Texture()
{
	CreateRHITexture();
}

Texture::~Texture()
{
	ReleaseRHITexture();
}

Texture::Texture(Texture&& rhs) noexcept
{
	ReleaseRHITexture();
	mTextureID = rhs.mTextureID;
	m_ownsTexture = rhs.m_ownsTexture;
	rhs.mTextureID = -1;
	rhs.m_ownsTexture = true;
}

Texture& Texture::operator=(Texture&& rhs) noexcept
{
	if (this != &rhs)
	{
		ReleaseRHITexture();
		mTextureID = rhs.mTextureID;
		m_ownsTexture = rhs.m_ownsTexture;
		rhs.mTextureID = -1;
		rhs.m_ownsTexture = true;
	}
	return *this;
}

void Texture::CreateRHITexture()
{
	if (mTextureID == -1)
	{
		m_ownsTexture = true;
		glGenTextures(1, &mTextureID);
	}
}

void Texture::ReleaseRHITexture()
{
	if (mTextureID != -1 && m_ownsTexture)
	{
		glDeleteTextures(1, &mTextureID);
	}

	mTextureID = -1;
	m_ownsTexture = true;
}

void Texture::AdoptTexture(uint32_t p_id, bool p_takeOwnership)
{
	ReleaseRHITexture();
	mTextureID = p_id;
	m_ownsTexture = p_takeOwnership;
}
