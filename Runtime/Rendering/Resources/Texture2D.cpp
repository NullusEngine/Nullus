#include <Image.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include <vector>

#include "Assets/ArtifactLoadTelemetry.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/Resources/Texture2D.h"

using namespace NLS::Render::Resources;

namespace
{
	NLS::Render::RHI::TextureColorSpace ToRhiTextureColorSpace(
		const NLS::Render::Assets::TextureArtifactColorSpace colorSpace)
	{
		return colorSpace == NLS::Render::Assets::TextureArtifactColorSpace::Srgb
			? NLS::Render::RHI::TextureColorSpace::SRGB
			: NLS::Render::RHI::TextureColorSpace::Linear;
	}

		NLS::Render::RHI::TextureFilter ToRHITextureFilter(NLS::Render::Settings::ETextureFilteringMode mode)
		{
			return mode == NLS::Render::Settings::ETextureFilteringMode::LINEAR
				? NLS::Render::RHI::TextureFilter::Linear
				: NLS::Render::RHI::TextureFilter::Nearest;
		}

		std::chrono::microseconds NonZeroElapsedMicros(
			const std::chrono::steady_clock::time_point begin,
			const std::chrono::steady_clock::time_point end)
		{
			auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - begin);
			if (elapsed.count() == 0)
				elapsed = std::chrono::microseconds(1);
			return elapsed;
		}

		void RecordTextureGpuUploadTelemetry(
			const std::chrono::steady_clock::time_point begin,
			const std::chrono::steady_clock::time_point end,
			const size_t byteCount)
		{
			NLS::Core::Assets::RecordArtifactLoadTelemetry({
				NLS::Core::Assets::ArtifactLoadTelemetryStage::GpuUpload,
				NonZeroElapsedMicros(begin, end),
				byteCount,
				{}
			});
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

		std::shared_ptr<NLS::Render::RHI::RHITexture> CreateUploadedRgba8Texture(
			const uint32_t width,
			const uint32_t height,
			const void* uploadData,
			const size_t uploadDataSize)
		{
			auto* driver = NLS::Render::Context::TryGetLocatedDriver();
			auto device = driver != nullptr
				? NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(*driver)
				: nullptr;
			if (device == nullptr || width == 0u || height == 0u || uploadData == nullptr || uploadDataSize == 0u)
				return nullptr;

			NLS::Render::RHI::RHITextureDesc desc{};
			desc.extent.width = static_cast<uint16_t>(width);
			desc.extent.height = static_cast<uint16_t>(height);
			desc.extent.depth = 1u;
			desc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
			desc.format = NLS::Render::RHI::TextureFormat::RGBA8;
			desc.colorSpace = NLS::Render::RHI::TextureColorSpace::Linear;
			desc.usage = NLS::Render::RHI::TextureUsageFlags::Sampled;
			desc.debugName = "TextureResourceInitialUpload";

			NLS::Render::RHI::RHITextureUploadDesc uploadDesc{};
			uploadDesc.data = uploadData;
			uploadDesc.dataSize = uploadDataSize;
			uploadDesc.extent = desc.extent;
			uploadDesc.rowPitch = NLS::Render::RHI::CalculateTextureRowPitch(desc.format, width);
			uploadDesc.slicePitch = NLS::Render::RHI::CalculateTextureSlicePitch(desc.format, width, height, 1u);
			uploadDesc.debugName = "TextureResourceInitialUpload";
			return device->CreateTexture(desc, uploadDesc);
		}
	}

std::unique_ptr<Texture2D> Texture2D::WrapExternal(
	const std::shared_ptr<NLS::Render::RHI::RHITexture>& textureResource,
	uint32_t inWidth,
	uint32_t inHeight)
{
	auto texture = std::unique_ptr<Texture2D>(new Texture2D { SkipInitialTextureTag {} });
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

		if (m_explicitTexture == nullptr)
		{
			if (auto uploadedTexture = CreateUploadedRgba8Texture(
					nextWidth,
					nextHeight,
					uploadData.data(),
					uploadData.size()))
			{
				SetRHITexture(std::move(uploadedTexture));
			}
		}
		else
		{
			// Default-constructed Texture2D starts with a 1x1 placeholder, so replace it
			// with the requested dimensions before UI or material code creates views.
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

bool Texture2D::SetRgba8TextureResource(
	const void* pixels,
	const size_t pixelBytes,
	const uint32_t nextWidth,
	const uint32_t nextHeight)
{
	if (nextWidth > UINT16_MAX || nextHeight > UINT16_MAX)
		return false;
	if (nextHeight != 0u && nextWidth > SIZE_MAX / static_cast<size_t>(nextHeight) / 4u)
		return false;
	const size_t expectedBytes = static_cast<size_t>(nextWidth) * static_cast<size_t>(nextHeight) * 4u;
	if (pixels == nullptr || nextWidth == 0u || nextHeight == 0u || pixelBytes < expectedBytes)
		return false;

	if (auto uploadedTexture = CreateUploadedRgba8Texture(
			nextWidth,
			nextHeight,
			pixels,
			expectedBytes))
	{
		SetRHITexture(std::move(uploadedTexture));
	}

	width = nextWidth;
	height = nextHeight;
	bitsPerPixel = 4;
	return m_explicitTexture != nullptr;
}

bool Texture2D::SetTextureResource(const NLS::Render::Assets::TextureArtifactData& artifact)
{
	if (artifact.width == 0u || artifact.height == 0u || artifact.mips.empty())
		return false;

	if (!artifact.mips.front().HasPixels())
		return false;

	const auto applyArtifactMetadata = [&]()
	{
		width = artifact.width;
		height = artifact.height;
		bitsPerPixel = NLS::Render::RHI::GetTextureFormatBytesPerPixel(artifact.format);
		isMimapped = artifact.mips.size() > 1u;
	};

	auto* driver = NLS::Render::Context::TryGetLocatedDriver();
	auto device = driver != nullptr
		? NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(*driver)
		: nullptr;
	if (device == nullptr)
	{
		if (NLS::Render::RHI::IsTextureFormatCompressed(artifact.format))
			return false;
		applyArtifactMetadata();
		return true;
	}
	if (NLS::Render::RHI::IsTextureFormatCompressed(artifact.format))
	{
		const auto& capability = device->GetCapabilities().GetTextureFormatCapability(artifact.format);
		if (!capability.sampled || !capability.upload)
			return false;
		if (artifact.colorSpace == NLS::Render::Assets::TextureArtifactColorSpace::Srgb && !capability.supportsSrgbView)
			return false;
	}

	NLS::Render::RHI::RHITextureDesc desc{};
	desc.extent.width = artifact.width;
	desc.extent.height = artifact.height;
	desc.extent.depth = 1u;
	desc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
	desc.format = artifact.format;
	desc.colorSpace = ToRhiTextureColorSpace(artifact.colorSpace);
	desc.mipLevels = static_cast<uint32_t>(artifact.mips.size());
	desc.usage = NLS::Render::RHI::TextureUsageFlags::Sampled;
	desc.debugName = "TextureArtifactResource";

	NLS::Render::RHI::RHITextureUploadDesc uploadDesc{};
	size_t uploadDataSize = 0u;
	uploadDesc.subresources.reserve(artifact.mips.size());
	for (const auto& mip : artifact.mips)
	{
		if (!mip.HasPixels())
			return false;
		uploadDataSize += mip.PixelSize();
		uploadDesc.subresources.push_back({
			mip.PixelData(),
			mip.PixelSize()
		});
	}
	uploadDesc.dataSize = uploadDataSize;
	uploadDesc.extent = desc.extent;
	uploadDesc.rowPitch = artifact.mips.front().rowPitch;
	uploadDesc.slicePitch = artifact.mips.front().slicePitch;
	uploadDesc.debugName = "TextureArtifactInitialUpload";

		const auto uploadBegin = std::chrono::steady_clock::now();
		auto texture = device->CreateTexture(desc, uploadDesc);
		const auto uploadEnd = std::chrono::steady_clock::now();
		RecordTextureGpuUploadTelemetry(uploadBegin, uploadEnd, uploadDataSize);
		if (texture == nullptr)
			return false;
	SetRHITexture(std::move(texture));

	applyArtifactMetadata();
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
