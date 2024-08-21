
#include <glad/glad.h>
#include <Image.h>

#include "Rendering/Resources/Texture2D.h"

using namespace NLS::Render::Resources;

void Texture2D::Bind(uint32_t p_slot) const
{
	glActiveTexture(GL_TEXTURE0 + p_slot);
	glBindTexture(GL_TEXTURE_2D, GetTextureId());
}

void Texture2D::Unbind() const
{
	glBindTexture(GL_TEXTURE_2D, 0);
}

Texture2D::Texture2D(Texture2D&& rhs) noexcept : Texture(std::move(rhs))
{
	width = rhs.width;
	height = rhs.height;
	bitsPerPixel = rhs.bitsPerPixel;
	firstFilter = rhs.firstFilter;
	secondFilter = rhs.secondFilter;
	isMimapped = rhs.isMimapped;
}

Texture2D& Texture2D::operator=(Texture2D&& rhs) noexcept
{
	Texture::operator=(std::move(rhs));

	width = rhs.width;
	height = rhs.height;
	bitsPerPixel = rhs.bitsPerPixel;
	firstFilter = rhs.firstFilter;
	secondFilter = rhs.secondFilter;
	isMimapped = rhs.isMimapped;
	return *this;
}

void Texture2D::SetTextureResource(const Image* image)
{
	if (image == nullptr)
	{
		return;
	}

	width = image->GetWidth();
	height = image->GetHeight();
	bitsPerPixel = image->GetChannels();

	auto nrComponents = image->GetChannels();
	GLenum format;
	if (nrComponents == 1)
		format = GL_RED;
	else if (nrComponents == 3)
		format = GL_RGB;
	else if (nrComponents == 4)
		format = GL_RGBA;
	else
		return;

	glTexImage2D(GL_TEXTURE_2D, 0, format, image->GetWidth(), image->GetHeight(), 0, format, GL_UNSIGNED_BYTE, image->GetData());

	if (isMimapped)
	{
		glGenerateMipmap(GL_TEXTURE_2D);
	}

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(firstFilter));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(secondFilter));
}
