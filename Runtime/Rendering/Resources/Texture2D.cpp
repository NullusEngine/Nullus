
#include <glad/glad.h>

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

Texture2D::Texture2D(const std::string p_path, uint32_t p_id, uint32_t p_width, uint32_t p_height, uint32_t p_bpp, Settings::ETextureFilteringMode p_firstFilter, Settings::ETextureFilteringMode p_secondFilter, bool p_generateMipmap) : path(p_path),
	width(p_width), height(p_height), bitsPerPixel(p_bpp), firstFilter(p_firstFilter), secondFilter(p_secondFilter), isMimapped(p_generateMipmap)
{
	SetTextureId(p_id);
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
