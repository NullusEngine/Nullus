#include "Rendering/Buffers/Framebuffer.h"
#include "Core/ServiceLocator.h"
#include "Rendering/RHI/Backends/OpenGL/Compat/ExplicitRHICompat.h"

using Driver = NLS::Render::Context::Driver;

namespace
{
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

NLS::Render::Buffers::Framebuffer::Framebuffer(uint16_t p_width, uint16_t p_height)
{
	auto& rhi = NLS_SERVICE(Driver);

	m_renderTextureResource = rhi.CreateTextureResource(NLS::Render::RHI::TextureDimension::Texture2D);
	m_depthStencilTextureResource = rhi.CreateTextureResource(NLS::Render::RHI::TextureDimension::Texture2D);
	m_explicitRenderTexture = m_renderTextureResource
		? NLS::Render::RHI::WrapCompatibilityTexture(m_renderTextureResource, "FramebufferColorTexture")
		: nullptr;
	m_explicitDepthStencilTexture = m_depthStencilTextureResource
		? NLS::Render::RHI::WrapCompatibilityTexture(m_depthStencilTextureResource, "FramebufferDepthTexture")
		: nullptr;
	m_renderTexture = m_renderTextureResource ? m_renderTextureResource->GetResourceId() : rhi.CreateTexture();
	m_depthStencilTexture = m_depthStencilTextureResource ? m_depthStencilTextureResource->GetResourceId() : rhi.CreateTexture();

	NLS::Render::RHI::FramebufferDesc framebufferDesc{};
	framebufferDesc.colorAttachments.push_back({ m_renderTexture, NLS::Render::RHI::TextureFormat::RGB8 });
	framebufferDesc.depthStencilTextureId = m_depthStencilTexture;
	framebufferDesc.depthStencilFormat = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
	framebufferDesc.drawBufferCount = 1;
	m_bufferID = rhi.CreateFramebuffer(framebufferDesc);

	rhi.BindFramebuffer(0);

	Resize(p_width, p_height, true);
}

NLS::Render::Buffers::Framebuffer::~Framebuffer()
{
	auto& rhi = NLS_SERVICE(Driver);

	rhi.DestroyFramebuffer(m_bufferID);
	if (m_renderTextureResource)
		m_renderTextureResource.reset();
	else
		rhi.DestroyTexture(m_renderTexture);

	if (m_depthStencilTextureResource)
		m_depthStencilTextureResource.reset();
	else
		rhi.DestroyTexture(m_depthStencilTexture);

	m_explicitRenderTexture.reset();
	m_explicitDepthStencilTexture.reset();
	m_explicitRenderTextureView.reset();
	m_explicitDepthStencilTextureView.reset();
}

void NLS::Render::Buffers::Framebuffer::Bind() const
{
	NLS_SERVICE(Driver).BindFramebuffer(m_bufferID);
}

void NLS::Render::Buffers::Framebuffer::Unbind() const
{
	NLS_SERVICE(Driver).BindFramebuffer(0);
}

void NLS::Render::Buffers::Framebuffer::Resize(uint16_t p_width, uint16_t p_height, bool p_forceUpdate)
{
	if (p_forceUpdate || p_width != m_width || p_height != m_height)
	{
		auto& rhi = NLS_SERVICE(Driver);
		NLS::Render::RHI::TextureDesc colorDesc{};
		colorDesc.width = p_width;
		colorDesc.height = p_height;
		colorDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
		colorDesc.format = NLS::Render::RHI::TextureFormat::RGB8;
		colorDesc.minFilter = NLS::Render::RHI::TextureFilter::Nearest;
		colorDesc.magFilter = NLS::Render::RHI::TextureFilter::Nearest;
		colorDesc.wrapS = NLS::Render::RHI::TextureWrap::ClampToEdge;
		colorDesc.wrapT = NLS::Render::RHI::TextureWrap::ClampToEdge;
		colorDesc.usage = NLS::Render::RHI::TextureUsage::Sampled | NLS::Render::RHI::TextureUsage::ColorAttachment;

		rhi.BindTexture(NLS::Render::RHI::TextureDimension::Texture2D, m_renderTexture);
		rhi.SetupTexture(colorDesc, nullptr);

		NLS::Render::RHI::TextureDesc depthDesc = colorDesc;
		depthDesc.format = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
		depthDesc.usage = NLS::Render::RHI::TextureUsage::DepthStencilAttachment;
		rhi.BindTexture(NLS::Render::RHI::TextureDimension::Texture2D, m_depthStencilTexture);
		rhi.SetupTexture(depthDesc, nullptr);

		NLS::Render::RHI::FramebufferDesc framebufferDesc{};
		framebufferDesc.colorAttachments.push_back({ m_renderTexture, colorDesc.format });
		framebufferDesc.depthStencilTextureId = m_depthStencilTexture;
		framebufferDesc.depthStencilFormat = depthDesc.format;
		framebufferDesc.drawBufferCount = 1;
		rhi.ConfigureFramebuffer(m_bufferID, framebufferDesc);
		rhi.BindFramebuffer(0);

		m_width = p_width;
		m_height = p_height;
		m_explicitRenderTextureView.reset();
		m_explicitDepthStencilTextureView.reset();
	}
}

uint32_t NLS::Render::Buffers::Framebuffer::GetID() const
{
	return m_bufferID;
}

uint32_t NLS::Render::Buffers::Framebuffer::GetTextureID() const
{
	return m_renderTexture;
}

const std::shared_ptr<NLS::Render::RHI::IRHITexture>& NLS::Render::Buffers::Framebuffer::GetTextureResource() const
{
	return m_renderTextureResource;
}

const std::shared_ptr<NLS::Render::RHI::RHITexture>& NLS::Render::Buffers::Framebuffer::GetExplicitTextureHandle() const
{
	return m_explicitRenderTexture;
}

uint32_t NLS::Render::Buffers::Framebuffer::GetDepthStencilTextureID() const
{
	return m_depthStencilTexture;
}

const std::shared_ptr<NLS::Render::RHI::IRHITexture>& NLS::Render::Buffers::Framebuffer::GetDepthStencilTextureResource() const
{
	return m_depthStencilTextureResource;
}

const std::shared_ptr<NLS::Render::RHI::RHITexture>& NLS::Render::Buffers::Framebuffer::GetExplicitDepthStencilTextureHandle() const
{
	return m_explicitDepthStencilTexture;
}

uint32_t NLS::Render::Buffers::Framebuffer::GetRenderBufferID() const
{
	return GetDepthStencilTextureID();
}

std::shared_ptr<NLS::Render::RHI::RHITextureView> NLS::Render::Buffers::Framebuffer::GetOrCreateExplicitColorView(const std::string& debugName) const
{
	if (m_explicitRenderTexture == nullptr)
		return nullptr;

	if (m_explicitRenderTextureView != nullptr)
		return m_explicitRenderTextureView;

	NLS::Render::RHI::RHITextureViewDesc viewDesc;
	viewDesc.format = m_explicitRenderTexture->GetDesc().format;
	viewDesc.viewType = NLS::Render::RHI::TextureViewType::Texture2D;
	viewDesc.debugName = debugName;
	auto& driver = NLS_SERVICE(Driver);
	m_explicitRenderTextureView = CreateTextureViewForExplicitPath(driver, m_explicitRenderTexture, viewDesc);
	return m_explicitRenderTextureView;
}

std::shared_ptr<NLS::Render::RHI::RHITextureView> NLS::Render::Buffers::Framebuffer::GetOrCreateExplicitDepthStencilView(const std::string& debugName) const
{
	if (m_explicitDepthStencilTexture == nullptr)
		return nullptr;

	if (m_explicitDepthStencilTextureView != nullptr)
		return m_explicitDepthStencilTextureView;

	NLS::Render::RHI::RHITextureViewDesc viewDesc;
	viewDesc.format = m_explicitDepthStencilTexture->GetDesc().format;
	viewDesc.viewType = NLS::Render::RHI::TextureViewType::Texture2D;
	viewDesc.debugName = debugName;
	auto& driver = NLS_SERVICE(Driver);
	m_explicitDepthStencilTextureView = CreateTextureViewForExplicitPath(driver, m_explicitDepthStencilTexture, viewDesc);
	return m_explicitDepthStencilTextureView;
}
