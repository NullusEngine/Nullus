#include "Rendering/Buffers/Framebuffer.h"
#include "Debug/Logger.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIDevice.h"

namespace
{
	NLS::Render::RHI::RHITextureDesc CreateColorTextureDesc(uint32_t width, uint32_t height)
	{
		NLS::Render::RHI::RHITextureDesc desc{};
		desc.extent.width = width;
		desc.extent.height = height;
		desc.extent.depth = 1;
		desc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
		desc.format = NLS::Render::RHI::TextureFormat::RGB8;
		desc.usage = NLS::Render::RHI::TextureUsageFlags::Sampled | NLS::Render::RHI::TextureUsageFlags::ColorAttachment;
		desc.debugName = "FramebufferColorTexture";
		return desc;
	}

	NLS::Render::RHI::RHITextureDesc CreateDepthTextureDesc(uint32_t width, uint32_t height)
	{
		NLS::Render::RHI::RHITextureDesc desc{};
		desc.extent.width = width;
		desc.extent.height = height;
		desc.extent.depth = 1;
		desc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
		desc.format = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
		desc.usage = NLS::Render::RHI::TextureUsageFlags::DepthStencilAttachment;
		desc.debugName = "FramebufferDepthTexture";
		return desc;
	}
}

NLS::Render::Buffers::Framebuffer::Framebuffer(uint16_t p_width, uint16_t p_height)
{
	auto& driver = NLS::Render::Context::RequireLocatedDriver("Framebuffer constructor");
	auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver);
	if (device == nullptr)
	{
		NLS_LOG_WARNING("Framebuffer: Explicit RHI device not available");
		return;
	}

	m_explicitRenderTexture = device->CreateTexture(CreateColorTextureDesc(p_width, p_height), nullptr);
	if (m_explicitRenderTexture == nullptr)
	{
		NLS_LOG_WARNING("Framebuffer: Failed to create color texture");
		return;
	}

	m_explicitDepthStencilTexture = device->CreateTexture(CreateDepthTextureDesc(p_width, p_height), nullptr);
	if (m_explicitDepthStencilTexture == nullptr)
	{
		NLS_LOG_WARNING("Framebuffer: Failed to create depth texture");
		return;
	}

	m_width = p_width;
	m_height = p_height;
}

NLS::Render::Buffers::Framebuffer::~Framebuffer()
{
	m_explicitRenderTexture.reset();
	m_explicitDepthStencilTexture.reset();
	m_explicitRenderTextureView.reset();
	m_explicitDepthStencilTextureView.reset();
}

void NLS::Render::Buffers::Framebuffer::Bind() const
{
	// In formal RHI, binding is handled at command buffer level through render passes
	// This is a no-op placeholder - actual binding happens during rendering
}

void NLS::Render::Buffers::Framebuffer::Unbind() const
{
	// In formal RHI, unbinding is handled at command buffer level
	// This is a no-op placeholder
}

void NLS::Render::Buffers::Framebuffer::Resize(uint16_t p_width, uint16_t p_height, bool p_forceUpdate)
{
	if (!p_forceUpdate && p_width == m_width && p_height == m_height)
		return;

	auto& driver = NLS::Render::Context::RequireLocatedDriver("Framebuffer::Resize");
	auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver);
	if (device == nullptr)
	{
		NLS_LOG_WARNING("Framebuffer::Resize: Explicit RHI device not available");
		return;
	}

	// Recreate color texture with new dimensions
	m_explicitRenderTexture = device->CreateTexture(CreateColorTextureDesc(p_width, p_height), nullptr);
	if (m_explicitRenderTexture == nullptr)
	{
		NLS_LOG_WARNING("Framebuffer::Resize: Failed to recreate color texture");
		return;
	}

	// Recreate depth texture with new dimensions
	m_explicitDepthStencilTexture = device->CreateTexture(CreateDepthTextureDesc(p_width, p_height), nullptr);
	if (m_explicitDepthStencilTexture == nullptr)
	{
		NLS_LOG_WARNING("Framebuffer::Resize: Failed to recreate depth texture");
		return;
	}

	m_width = p_width;
	m_height = p_height;
	m_explicitRenderTextureView.reset();
	m_explicitDepthStencilTextureView.reset();
}

uint32_t NLS::Render::Buffers::Framebuffer::GetID() const
{
	// In formal RHI, there's no framebuffer ID - return 0 as placeholder
	// The actual framebuffer concept is handled through render passes
	return 0;
}

const std::shared_ptr<NLS::Render::RHI::IRHITexture>& NLS::Render::Buffers::Framebuffer::GetTextureResource() const
{
	// No longer using legacy texture resource
	static std::shared_ptr<NLS::Render::RHI::IRHITexture> nullResource;
	return nullResource;
}

const std::shared_ptr<NLS::Render::RHI::RHITexture>& NLS::Render::Buffers::Framebuffer::GetExplicitTextureHandle() const
{
	return m_explicitRenderTexture;
}

const std::shared_ptr<NLS::Render::RHI::IRHITexture>& NLS::Render::Buffers::Framebuffer::GetDepthStencilTextureResource() const
{
	// No longer using legacy texture resource
	static std::shared_ptr<NLS::Render::RHI::IRHITexture> nullResource;
	return nullResource;
}

const std::shared_ptr<NLS::Render::RHI::RHITexture>& NLS::Render::Buffers::Framebuffer::GetExplicitDepthStencilTextureHandle() const
{
	return m_explicitDepthStencilTexture;
}

std::shared_ptr<NLS::Render::RHI::RHITextureView> NLS::Render::Buffers::Framebuffer::GetOrCreateExplicitColorView(const std::string& debugName) const
{
	if (m_explicitRenderTexture == nullptr)
		return nullptr;

	if (m_explicitRenderTextureView != nullptr)
		return m_explicitRenderTextureView;

	auto& driver = NLS::Render::Context::RequireLocatedDriver("Framebuffer::GetOrCreateExplicitColorView");
	auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver);
	if (device == nullptr)
		return nullptr;

	NLS::Render::RHI::RHITextureViewDesc viewDesc{};
	viewDesc.format = m_explicitRenderTexture->GetDesc().format;
	viewDesc.viewType = NLS::Render::RHI::TextureViewType::Texture2D;
	viewDesc.debugName = debugName.empty() ? "FramebufferColorView" : debugName;

	m_explicitRenderTextureView = device->CreateTextureView(m_explicitRenderTexture, viewDesc);
	return m_explicitRenderTextureView;
}

std::shared_ptr<NLS::Render::RHI::RHITextureView> NLS::Render::Buffers::Framebuffer::GetOrCreateExplicitDepthStencilView(const std::string& debugName) const
{
	if (m_explicitDepthStencilTexture == nullptr)
		return nullptr;

	if (m_explicitDepthStencilTextureView != nullptr)
		return m_explicitDepthStencilTextureView;

	auto& driver = NLS::Render::Context::RequireLocatedDriver("Framebuffer::GetOrCreateExplicitDepthStencilView");
	auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver);
	if (device == nullptr)
		return nullptr;

	NLS::Render::RHI::RHITextureViewDesc viewDesc{};
	viewDesc.format = m_explicitDepthStencilTexture->GetDesc().format;
	viewDesc.viewType = NLS::Render::RHI::TextureViewType::Texture2D;
	viewDesc.debugName = debugName.empty() ? "FramebufferDepthView" : debugName;

	m_explicitDepthStencilTextureView = device->CreateTextureView(m_explicitDepthStencilTexture, viewDesc);
	return m_explicitDepthStencilTextureView;
}
