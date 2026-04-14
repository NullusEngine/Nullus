#include "Rendering/Resources/Texture.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/Resources/TextureResourceUpdateUtils.h"

using namespace NLS::Render::Resources;

namespace
{
	// Helper to get RHIDevice from driver
	std::shared_ptr<NLS::Render::RHI::RHIDevice> GetExplicitDevice()
	{
		try
		{
			auto& driver = NLS::Render::Context::RequireLocatedDriver("Texture::CreateRHITexture");
			return NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver);
		}
		catch (...)
		{
			return nullptr;
		}
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

Texture::Texture(Texture&& rhs) noexcept
{
	ReleaseRHITexture();
	m_dimension = rhs.m_dimension;
	m_explicitTexture = std::move(rhs.m_explicitTexture);
	m_explicitTextureView = std::move(rhs.m_explicitTextureView);
	rhs.m_dimension = RHI::TextureDimension::Texture2D;
}

Texture& Texture::operator=(Texture&& rhs) noexcept
{
	if (this != &rhs)
	{
		ReleaseRHITexture();
		m_dimension = rhs.m_dimension;
		m_explicitTexture = std::move(rhs.m_explicitTexture);
		m_explicitTextureView = std::move(rhs.m_explicitTextureView);
		rhs.m_dimension = RHI::TextureDimension::Texture2D;
	}
	return *this;
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

	m_explicitTexture = device->CreateTexture(desc, nullptr);
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
	textureViewDesc.debugName = debugName;

	m_explicitTextureView = device->CreateTextureView(m_explicitTexture, textureViewDesc);
	return m_explicitTextureView;
}

void Texture::RecreateRHITextureIfNeeded(
    uint32_t width,
    uint32_t height,
    RHI::TextureFormat format,
    RHI::TextureFilter minFilter,
    RHI::TextureFilter magFilter,
    RHI::TextureWrap wrapS,
    RHI::TextureWrap wrapT,
    bool generateMimaps,
    const void* initialData)
{
	// Only handle formal RHI path
	if (m_explicitTexture == nullptr)
		return;

	if (!ShouldRecreateRHITexture(m_explicitTexture->GetDesc(), width, height, format, initialData))
	{
		return;
	}

	// Need to recreate with correct dimensions
	auto device = GetExplicitDevice();
	if (device == nullptr)
		return;

	// Release old texture
	m_explicitTexture.reset();
	m_explicitTextureView.reset();

	// Create new texture with correct dimensions and data
	RHI::RHITextureDesc desc{};
	desc.extent.width = static_cast<uint16_t>(width);
	desc.extent.height = static_cast<uint16_t>(height);
	desc.extent.depth = 1;
	desc.dimension = m_dimension;
	desc.format = format;
	desc.usage = RHI::TextureUsageFlags::Sampled;
	desc.debugName = "TextureResource";

	// Note: mipmap generation would require a separate pass after creation
	// For now, create with 1 mip level

	m_explicitTexture = device->CreateTexture(desc, initialData);
}
