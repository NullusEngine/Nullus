#include "Rendering/Resources/Texture.h"

#include "Debug/Assertion.h"
#include "Core/ServiceLocator.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/RHI/Backends/OpenGL/Compat/ExplicitRHICompat.h"

using namespace NLS::Render::Resources;

namespace
{
	using Driver = NLS::Render::Context::Driver;

	std::shared_ptr<NLS::Render::RHI::RHITextureView> CreateTextureViewForExplicitPath(
		Driver& driver,
		const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
		const NLS::Render::RHI::RHITextureViewDesc& desc)
	{
		if (texture == nullptr)
			return nullptr;

		if (const auto explicitDevice = driver.GetExplicitDevice(); explicitDevice != nullptr)
			return explicitDevice->CreateTextureView(texture, desc);

		return NLS::Render::RHI::CreateCompatibilityTextureView(texture, desc);
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
	mTextureID = rhs.mTextureID;
	m_ownsTexture = rhs.m_ownsTexture;
	m_dimension = rhs.m_dimension;
	m_textureResource = std::move(rhs.m_textureResource);
	m_explicitTexture = std::move(rhs.m_explicitTexture);
	m_explicitTextureView = std::move(rhs.m_explicitTextureView);
	rhs.mTextureID = -1;
	rhs.m_ownsTexture = true;
	rhs.m_dimension = RHI::TextureDimension::Texture2D;
}

Texture& Texture::operator=(Texture&& rhs) noexcept
{
	if (this != &rhs)
	{
		ReleaseRHITexture();
		mTextureID = rhs.mTextureID;
		m_ownsTexture = rhs.m_ownsTexture;
		m_dimension = rhs.m_dimension;
		m_textureResource = std::move(rhs.m_textureResource);
		m_explicitTexture = std::move(rhs.m_explicitTexture);
		m_explicitTextureView = std::move(rhs.m_explicitTextureView);
		rhs.mTextureID = -1;
		rhs.m_ownsTexture = true;
		rhs.m_dimension = RHI::TextureDimension::Texture2D;
	}
	return *this;
}

void Texture::CreateRHITexture()
{
	if (mTextureID == static_cast<uint32_t>(-1))
	{
		m_ownsTexture = true;
		NLS_ASSERT(NLS::Core::ServiceLocator::Contains<Driver>(), "Texture resources require an initialized Driver.");
		m_textureResource = NLS_SERVICE(Driver).CreateTextureResource(m_dimension);
		m_explicitTexture = m_textureResource
			? RHI::WrapCompatibilityTexture(m_textureResource, "TextureResource")
			: nullptr;
		m_explicitTextureView.reset();
		mTextureID = m_textureResource ? m_textureResource->GetResourceId() : static_cast<uint32_t>(-1);
	}
}

void Texture::ReleaseRHITexture()
{
	if (m_textureResource)
	{
		m_textureResource.reset();
	}
	else if (mTextureID != static_cast<uint32_t>(-1) && m_ownsTexture)
	{
		NLS_ASSERT(NLS::Core::ServiceLocator::Contains<Driver>(), "Texture destruction requires an initialized Driver.");
		NLS_SERVICE(Driver).DestroyTexture(mTextureID);
	}

	mTextureID = static_cast<uint32_t>(-1);
	m_ownsTexture = true;
	m_explicitTexture.reset();
	m_explicitTextureView.reset();
}

void Texture::AdoptTexture(uint32_t p_id, bool p_takeOwnership)
{
	ReleaseRHITexture();
	mTextureID = p_id;
	m_ownsTexture = p_takeOwnership;
	m_explicitTexture.reset();
	m_explicitTextureView.reset();
}

void Texture::SetRHITexture(std::shared_ptr<RHI::IRHITexture> texture)
{
	ReleaseRHITexture();
	m_textureResource = std::move(texture);
	if (m_textureResource)
		m_dimension = m_textureResource->GetDimension();
	m_explicitTexture = m_textureResource
		? RHI::WrapCompatibilityTexture(m_textureResource, "TextureResource")
		: nullptr;
	m_explicitTextureView.reset();
	mTextureID = m_textureResource ? m_textureResource->GetResourceId() : static_cast<uint32_t>(-1);
	m_ownsTexture = false;
}

std::shared_ptr<NLS::Render::RHI::RHITextureView> Texture::GetOrCreateExplicitTextureView(const std::string& debugName) const
{
	if (m_explicitTexture == nullptr)
		return nullptr;

	if (m_explicitTextureView != nullptr)
		return m_explicitTextureView;

	RHI::RHITextureViewDesc textureViewDesc;
	textureViewDesc.viewType = m_dimension == RHI::TextureDimension::TextureCube
		? RHI::TextureViewType::Cube
		: RHI::TextureViewType::Texture2D;
	textureViewDesc.format = m_explicitTexture->GetDesc().format;
	textureViewDesc.debugName = debugName;
	auto& driver = NLS_SERVICE(Driver);
	m_explicitTextureView = CreateTextureViewForExplicitPath(driver, m_explicitTexture, textureViewDesc);
	return m_explicitTextureView;
}
