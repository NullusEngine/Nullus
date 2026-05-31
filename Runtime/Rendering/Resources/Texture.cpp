#include "Rendering/Resources/Texture.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/Resources/TextureResourceUpdateUtils.h"

#include <algorithm>

using namespace NLS::Render::Resources;

namespace
{
	// Helper to get RHIDevice from driver
	std::shared_ptr<NLS::Render::RHI::RHIDevice> GetExplicitDevice()
	{
		auto& driver = NLS::Render::Context::RequireLocatedDriver("Texture::CreateRHITexture");
		return NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver);
	}
}

Texture::Texture(RHI::TextureDimension dimension)
	: m_dimension(dimension)
{
	CreateRHITexture();
}

Texture::~Texture()
{
	ReleaseRHITexture();
}

void Texture::CreateRHITexture()
{
	if (m_explicitTexture != nullptr)
		return;

	auto device = GetExplicitDevice();
	if (device == nullptr)
		return;

	RHI::RHITextureDesc desc{};
	desc.extent.width = 1;
	desc.extent.height = 1;
	desc.extent.depth = 1;
	desc.dimension = m_dimension;
	desc.format = RHI::TextureFormat::RGBA8;
	desc.usage = RHI::TextureUsageFlags::Sampled;
	desc.debugName = "TextureResource";

	m_explicitTexture = device->CreateTexture(desc);
}

void Texture::ReleaseRHITexture()
{
	m_explicitTexture.reset();
	m_explicitTextureView.reset();
}

void Texture::SetRHITexture(std::shared_ptr<RHI::RHITexture> texture)
{
	ReleaseRHITexture();
	m_explicitTexture = std::move(texture);
	if (m_explicitTexture)
		m_dimension = m_explicitTexture->GetDesc().dimension;
}

std::shared_ptr<NLS::Render::RHI::RHITextureView> Texture::GetOrCreateExplicitTextureView(const std::string& debugName) const
{
	if (m_explicitTexture == nullptr)
		return nullptr;

	if (m_explicitTextureView != nullptr)
		return m_explicitTextureView;

	auto device = GetExplicitDevice();
	if (device == nullptr)
		return nullptr;

	RHI::RHITextureViewDesc textureViewDesc{};
	textureViewDesc.viewType = m_dimension == RHI::TextureDimension::TextureCube
		? RHI::TextureViewType::Cube
		: RHI::TextureViewType::Texture2D;
	textureViewDesc.format = m_explicitTexture->GetDesc().format;
	textureViewDesc.colorSpace = m_explicitTexture->GetDesc().colorSpace;
	textureViewDesc.subresourceRange.mipLevelCount = (std::max)(m_explicitTexture->GetDesc().mipLevels, 1u);
	textureViewDesc.subresourceRange.arrayLayerCount = RHI::GetTextureLayerCount(
		m_explicitTexture->GetDesc().dimension,
		m_explicitTexture->GetDesc().arrayLayers);
	textureViewDesc.debugName = debugName;

	m_explicitTextureView = device->CreateTextureView(m_explicitTexture, textureViewDesc);
	return m_explicitTextureView;
}

bool Texture::RecreateRHITextureIfNeeded(
    uint32_t width,
    uint32_t height,
    RHI::TextureFormat format,
    RHI::TextureFilter minFilter,
    RHI::TextureFilter magFilter,
    RHI::TextureWrap wrapS,
    RHI::TextureWrap wrapT,
    bool generateMimaps,
    const void* initialData,
    size_t initialDataSize)
{
	// Only handle formal RHI path
	if (m_explicitTexture == nullptr)
		return false;

	const auto& existingDesc = m_explicitTexture->GetDesc();
	if (CanUpdateRHITextureInPlace(existingDesc, width, height, format, initialData))
	{
		auto device = GetExplicitDevice();
		if (device == nullptr)
			return false;

		RHI::RHITextureUpdateDesc updateDesc{};
		updateDesc.texture = m_explicitTexture;
		updateDesc.data = initialData;
		updateDesc.dataSize = initialDataSize;
		updateDesc.extent = {
			static_cast<uint32_t>(width),
			static_cast<uint32_t>(height),
			1u
		};
		updateDesc.rowPitch = RHI::CalculateTextureRowPitch(format, static_cast<uint32_t>(width));
		updateDesc.slicePitch = RHI::CalculateTextureSlicePitch(
			format,
			static_cast<uint32_t>(width),
			static_cast<uint32_t>(height),
			1u);
		updateDesc.debugName = "TextureResourceInPlaceUpdate";

		return device->UpdateTexture(updateDesc).Succeeded();
	}

	if (!ShouldRecreateRHITexture(m_explicitTexture->GetDesc(), width, height, format, initialData))
	{
		return true;
	}

	// Need to recreate with correct dimensions
	auto device = GetExplicitDevice();
	if (device == nullptr)
		return false;

	// Create new texture with correct dimensions and data
	RHI::RHITextureDesc desc{};
	desc.extent.width = static_cast<uint16_t>(width);
	desc.extent.height = static_cast<uint16_t>(height);
	desc.extent.depth = 1;
	desc.dimension = m_dimension;
	desc.format = format;
	desc.colorSpace = RHI::TextureColorSpace::Linear;
	desc.usage = RHI::TextureUsageFlags::Sampled;
	desc.debugName = "TextureResource";

	// Note: mipmap generation would require a separate pass after creation
	// For now, create with 1 mip level

	std::shared_ptr<RHI::RHITexture> newTexture;
	if (initialData != nullptr && initialDataSize != 0u)
	{
		RHI::RHITextureUploadDesc uploadDesc{};
		uploadDesc.data = initialData;
		uploadDesc.dataSize = initialDataSize;
		uploadDesc.extent = desc.extent;
		uploadDesc.rowPitch = RHI::CalculateTextureRowPitch(format, static_cast<uint32_t>(width));
		uploadDesc.slicePitch = RHI::CalculateTextureSlicePitch(
			format,
			static_cast<uint32_t>(width),
			static_cast<uint32_t>(height),
			1u);
		uploadDesc.debugName = "TextureResourceInitialUpload";
		newTexture = device->CreateTexture(desc, uploadDesc);
	}
	else
	{
		newTexture = device->CreateTexture(desc);
	}
	if (newTexture == nullptr)
		return false;

	m_explicitTextureView.reset();
	m_explicitTexture = std::move(newTexture);
	return true;
}
