#include <Image.h>
#include <cstdint>
#include <vector>

#include "Rendering/Resources/TextureCube.h"

using namespace NLS::Render::Resources;

namespace
{
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
	// In formal RHI, binding is handled at command buffer level through descriptor sets
	// This is a no-op placeholder
	(void)p_slot;
}

void TextureCube::Unbind() const
{
	// In formal RHI, unbinding is handled at command buffer level
	// This is a no-op placeholder
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

	// For formal RHI texture, we need to recreate it with correct dimensions
	// The initial CreateRHITexture() only creates a 1x1 placeholder
	if (m_explicitTexture != nullptr)
	{
		RecreateRHITextureIfNeeded(
		    width,
		    height,
		    NLS::Render::RHI::TextureFormat::RGBA8,
		    NLS::Render::RHI::TextureFilter::Linear,
		    NLS::Render::RHI::TextureFilter::Linear,
		    NLS::Render::RHI::TextureWrap::ClampToEdge,
		    NLS::Render::RHI::TextureWrap::ClampToEdge,
		    false, // no mipmap support for cubemap in this path
		    packedFaces.data());
	}

	return true;
}
