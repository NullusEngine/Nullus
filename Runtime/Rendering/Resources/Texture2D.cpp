#include <Image.h>
#include <memory>
#include <vector>

#include "Debug/Assertion.h"
#include "Core/ServiceLocator.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Resources/Texture2D.h"

using namespace NLS::Render::Resources;

namespace
{
	using Driver = NLS::Render::Context::Driver;

	Driver& RequireDriver()
	{
		NLS_ASSERT(NLS::Core::ServiceLocator::Contains<Driver>(), "Texture2D requires an initialized Driver.");
		return NLS_SERVICE(Driver);
	}

	NLS::Render::RHI::TextureFilter ToRHITextureFilter(NLS::Render::Settings::ETextureFilteringMode mode)
	{
		return mode == NLS::Render::Settings::ETextureFilteringMode::LINEAR
			? NLS::Render::RHI::TextureFilter::Linear
			: NLS::Render::RHI::TextureFilter::Nearest;
	}

	std::vector<uint8_t> ConvertImageToRGBA8(const NLS::Image& image)
	{
		const auto* source = image.GetData();
		if (source == nullptr)
			return {};

		const auto pixelCount = static_cast<size_t>(image.GetWidth()) * static_cast<size_t>(image.GetHeight());
		std::vector<uint8_t> converted(pixelCount * 4u, 255u);
		const int channels = image.GetChannels();

		for (size_t i = 0; i < pixelCount; ++i)
		{
			const size_t srcIndex = i * static_cast<size_t>(channels);
			const size_t dstIndex = i * 4u;
			switch (channels)
			{
			case 1:
				converted[dstIndex + 0] = source[srcIndex + 0];
				converted[dstIndex + 1] = source[srcIndex + 0];
				converted[dstIndex + 2] = source[srcIndex + 0];
				break;
			case 3:
				converted[dstIndex + 0] = source[srcIndex + 0];
				converted[dstIndex + 1] = source[srcIndex + 1];
				converted[dstIndex + 2] = source[srcIndex + 2];
				break;
			case 4:
				converted[dstIndex + 0] = source[srcIndex + 0];
				converted[dstIndex + 1] = source[srcIndex + 1];
				converted[dstIndex + 2] = source[srcIndex + 2];
				converted[dstIndex + 3] = source[srcIndex + 3];
				break;
			default:
				return {};
			}
		}

		return converted;
	}
}

void Texture2D::Bind(uint32_t p_slot) const
{
	auto& driver = RequireDriver();
	driver.ActivateTexture(p_slot);
	driver.BindTexture(NLS::Render::RHI::TextureDimension::Texture2D, GetTextureId());
}

void Texture2D::Unbind() const
{
	RequireDriver().BindTexture(NLS::Render::RHI::TextureDimension::Texture2D, 0);
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

std::unique_ptr<Texture2D> Texture2D::WrapExternal(uint32_t textureId, uint32_t inWidth, uint32_t inHeight)
{
	auto texture = std::unique_ptr<Texture2D>(new Texture2D{});
	texture->AdoptTexture(textureId, false);
	texture->width = inWidth;
	texture->height = inHeight;
	return texture;
}

void Texture2D::SetTextureResource(const Image* image)
{
	if (image == nullptr)
		return;

	width = image->GetWidth();
	height = image->GetHeight();
	bitsPerPixel = 4;

	const auto uploadData = ConvertImageToRGBA8(*image);
	if (uploadData.empty())
		return;

	NLS::Render::RHI::TextureDesc desc;
	desc.width = static_cast<uint16_t>(image->GetWidth());
	desc.height = static_cast<uint16_t>(image->GetHeight());
	desc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
	desc.format = NLS::Render::RHI::TextureFormat::RGBA8;
	desc.minFilter = ToRHITextureFilter(firstFilter);
	desc.magFilter = ToRHITextureFilter(secondFilter);
	desc.wrapS = NLS::Render::RHI::TextureWrap::Repeat;
	desc.wrapT = NLS::Render::RHI::TextureWrap::Repeat;
	desc.usage = NLS::Render::RHI::TextureUsage::Sampled;

	auto& driver = RequireDriver();
	driver.BindTexture(NLS::Render::RHI::TextureDimension::Texture2D, GetTextureId());
	driver.SetupTexture(desc, uploadData.data());
	if (isMimapped)
		driver.GenerateTextureMipmap(NLS::Render::RHI::TextureDimension::Texture2D);
}
