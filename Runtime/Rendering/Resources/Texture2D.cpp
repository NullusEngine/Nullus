#include <Image.h>
#include <algorithm>
#include <memory>
#include <vector>

#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/Resources/Texture2D.h"

using namespace NLS::Render::Resources;

namespace
{
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
			case 2:
				converted[dstIndex + 0] = source[srcIndex + 0];
				converted[dstIndex + 1] = source[srcIndex + 0];
				converted[dstIndex + 2] = source[srcIndex + 0];
				converted[dstIndex + 3] = source[srcIndex + 1];
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

std::unique_ptr<Texture2D> Texture2D::WrapExternal(
	const std::shared_ptr<NLS::Render::RHI::RHITexture>& textureResource,
	uint32_t inWidth,
	uint32_t inHeight)
{
	auto texture = std::unique_ptr<Texture2D>(new Texture2D{});
	texture->WrapExternalInPlace(textureResource, inWidth, inHeight);
	return texture;
}

void Texture2D::WrapExternalInPlace(
	const std::shared_ptr<NLS::Render::RHI::RHITexture>& textureResource,
	uint32_t inWidth,
	uint32_t inHeight)
{
	if (GetTextureHandle() != textureResource)
		SetRHITexture(textureResource);

	width = inWidth;
	height = inHeight;
}

void Texture2D::SetTextureResource(const Image* image)
{
	if (image == nullptr)
		return;

	const uint32_t nextWidth = static_cast<uint32_t>(image->GetWidth());
	const uint32_t nextHeight = static_cast<uint32_t>(image->GetHeight());

	const auto uploadData = ConvertImageToRGBA8(*image);
	if (uploadData.empty())
		return;

	// For formal RHI texture, we need to recreate it with correct dimensions
	// The initial CreateRHITexture() only creates a 1x1 placeholder
	if (m_explicitTexture != nullptr)
	{
		if (!RecreateRHITextureIfNeeded(
		    nextWidth,
		    nextHeight,
		    NLS::Render::RHI::TextureFormat::RGBA8,
		    ToRHITextureFilter(firstFilter),
		    ToRHITextureFilter(secondFilter),
		    NLS::Render::RHI::TextureWrap::Repeat,
		    NLS::Render::RHI::TextureWrap::Repeat,
		    isMimapped,
		    uploadData.data(),
		    uploadData.size()))
		{
			width = m_explicitTexture->GetDesc().extent.width;
			height = m_explicitTexture->GetDesc().extent.height;
			bitsPerPixel = 4;
			return;
		}
	}

	width = nextWidth;
	height = nextHeight;
	bitsPerPixel = 4;
}

bool Texture2D::SetTextureResource(const NLS::Render::Assets::TextureArtifactData& artifact)
{
	if (artifact.width == 0u || artifact.height == 0u || artifact.mips.empty())
		return false;

	size_t totalBytes = 0u;
	for (const auto& mip : artifact.mips)
		totalBytes += mip.pixels.size();
	if (totalBytes == 0u)
		return false;

	std::vector<uint8_t> uploadData;
	uploadData.reserve(totalBytes);
	for (const auto& mip : artifact.mips)
		uploadData.insert(uploadData.end(), mip.pixels.begin(), mip.pixels.end());

	if (m_explicitTexture == nullptr)
		CreateRHITexture();
	if (m_explicitTexture != nullptr)
	{
		auto& driver = NLS::Render::Context::RequireLocatedDriver("Texture2D::SetTextureResource(TextureArtifact)");
		auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver);
		if (device != nullptr)
		{
			NLS::Render::RHI::RHITextureDesc desc{};
			desc.extent.width = artifact.width;
			desc.extent.height = artifact.height;
			desc.extent.depth = 1u;
			desc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
			desc.format = artifact.format;
			desc.mipLevels = static_cast<uint32_t>(artifact.mips.size());
			desc.usage = NLS::Render::RHI::TextureUsageFlags::Sampled;
			desc.debugName = "TextureArtifactResource";

			NLS::Render::RHI::RHITextureUploadDesc uploadDesc{};
			uploadDesc.data = uploadData.data();
			uploadDesc.dataSize = uploadData.size();
			uploadDesc.extent = desc.extent;
			uploadDesc.rowPitch = artifact.mips.front().rowPitch;
			uploadDesc.slicePitch = artifact.mips.front().slicePitch;
			uploadDesc.debugName = "TextureArtifactInitialUpload";

			auto texture = device->CreateTexture(desc, uploadDesc);
			if (texture != nullptr)
			{
				SetRHITexture(std::move(texture));
			}
		}
	}

	width = artifact.width;
	height = artifact.height;
	bitsPerPixel = NLS::Render::RHI::GetTextureFormatBytesPerPixel(artifact.format);
	isMimapped = artifact.mips.size() > 1u;
	return true;
}

void Texture2D::ReloadFrom(const Texture2D& source)
{
	ReleaseRHITexture();
	m_dimension = source.m_dimension;
	m_explicitTexture = source.m_explicitTexture;
	m_explicitTextureView = source.m_explicitTextureView;
	SetName(source.GetName());
	width = source.width;
	height = source.height;
	bitsPerPixel = source.bitsPerPixel;
	firstFilter = source.firstFilter;
	secondFilter = source.secondFilter;
	path = source.path;
	isMimapped = source.isMimapped;
}
