#include <Image.h>
#include <cstdint>
#include <vector>

#include "Debug/Assertion.h"
#include "Core/ServiceLocator.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Resources/TextureCube.h"

using namespace NLS::Render::Resources;

namespace
{
	using Driver = NLS::Render::Context::Driver;

	Driver& RequireDriver()
	{
		NLS_ASSERT(NLS::Core::ServiceLocator::Contains<Driver>(), "TextureCube requires an initialized Driver.");
		return NLS_SERVICE(Driver);
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

TextureCube::TextureCube()
	: Texture(NLS::Render::RHI::TextureDimension::TextureCube)
{
}

void TextureCube::Bind(uint32_t p_slot) const
{
	auto& driver = RequireDriver();
	driver.ActivateTexture(p_slot);
	driver.BindTexture(NLS::Render::RHI::TextureDimension::TextureCube, GetTextureId());
}

void TextureCube::Unbind() const
{
	RequireDriver().BindTexture(NLS::Render::RHI::TextureDimension::TextureCube, 0);
}

bool TextureCube::SetTextureResource(const std::vector<const NLS::Image*>& images)
{
	if (images.size() != NLS::Render::RHI::GetTextureLayerCount(NLS::Render::RHI::TextureDimension::TextureCube))
		return false;

	if (images[0] == nullptr || images[0]->GetData() == nullptr)
		return false;

	const int width = images[0]->GetWidth();
	const int height = images[0]->GetHeight();
	std::vector<uint8_t> packedFaces;
	packedFaces.reserve(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u * images.size());

	for (const auto* image : images)
	{
		if (image == nullptr || image->GetData() == nullptr || image->GetWidth() != width || image->GetHeight() != height)
			return false;

		const auto faceData = ConvertImageToRGBA8(*image);
		if (faceData.empty())
			return false;
		packedFaces.insert(packedFaces.end(), faceData.begin(), faceData.end());
	}

	NLS::Render::RHI::TextureDesc desc;
	desc.width = static_cast<uint16_t>(width);
	desc.height = static_cast<uint16_t>(height);
	desc.dimension = NLS::Render::RHI::TextureDimension::TextureCube;
	desc.format = NLS::Render::RHI::TextureFormat::RGBA8;
	desc.minFilter = NLS::Render::RHI::TextureFilter::Linear;
	desc.magFilter = NLS::Render::RHI::TextureFilter::Linear;
	desc.wrapS = NLS::Render::RHI::TextureWrap::ClampToEdge;
	desc.wrapT = NLS::Render::RHI::TextureWrap::ClampToEdge;
	desc.usage = NLS::Render::RHI::TextureUsage::Sampled;

	auto& driver = RequireDriver();
	driver.BindTexture(NLS::Render::RHI::TextureDimension::TextureCube, GetTextureId());
	driver.SetupTexture(desc, packedFaces.data());

	return true;
}
