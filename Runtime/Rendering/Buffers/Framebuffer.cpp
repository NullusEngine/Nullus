#include "Rendering/Buffers/Framebuffer.h"
#include "Debug/Logger.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIDevice.h"

#include <utility>

namespace
{
	constexpr size_t kMaxRetiredFramebufferResourceSets = 16u;

	bool SameOptimizedColorClearValue(
		const NLS::Render::RHI::RHITextureDesc::OptimizedClearValue& left,
		const NLS::Render::RHI::RHITextureDesc::OptimizedClearValue& right)
	{
		return left.enabled == right.enabled &&
			left.color[0] == right.color[0] &&
			left.color[1] == right.color[1] &&
			left.color[2] == right.color[2] &&
			left.color[3] == right.color[3];
	}

		NLS::Render::RHI::RHITextureDesc CreateColorTextureDesc(
			uint32_t width,
			uint32_t height,
			NLS::Render::RHI::TextureColorSpace colorSpace,
			const NLS::Render::RHI::RHITextureDesc::OptimizedClearValue& optimizedClearValue)
	{
		NLS::Render::RHI::RHITextureDesc desc{};
		desc.extent.width = width;
		desc.extent.height = height;
		desc.extent.depth = 1;
			desc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
			desc.format = NLS::Render::RHI::TextureFormat::RGBA8;
			desc.colorSpace = colorSpace;
		desc.usage = NLS::Render::RHI::TextureUsageFlags::Sampled | NLS::Render::RHI::TextureUsageFlags::ColorAttachment;
		desc.optimizedClearValue = optimizedClearValue;
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
		desc.optimizedClearValue = NLS::Render::RHI::RHITextureDesc::OptimizedClearValue::DepthStencil();
		desc.debugName = "FramebufferDepthTexture";
		return desc;
	}
}

bool NLS::Render::Buffers::Framebuffer::RetiredResourceSet::HasResources() const
{
	return colorTexture != nullptr ||
		depthStencilTexture != nullptr ||
		colorView != nullptr ||
		depthStencilView != nullptr;
}

void NLS::Render::Buffers::Framebuffer::RetiredResourceSet::Reset()
{
	colorView.reset();
	depthStencilView.reset();
	colorTexture.reset();
	depthStencilTexture.reset();
}

NLS::Render::Buffers::Framebuffer::Framebuffer(
	uint16_t p_width,
	uint16_t p_height,
	NLS::Render::RHI::TextureColorSpace colorSpace)
	: m_colorSpace(colorSpace)
{
	if (p_width == 0u || p_height == 0u)
		return;

	auto& driver = NLS::Render::Context::RequireLocatedDriver("Framebuffer constructor");
	auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver);
	if (device == nullptr)
	{
		NLS_LOG_WARNING("Framebuffer: Explicit RHI device not available");
		return;
	}

		m_explicitRenderTexture = device->CreateTexture(CreateColorTextureDesc(
			p_width,
			p_height,
			m_colorSpace,
			m_colorOptimizedClearValue));
	if (m_explicitRenderTexture == nullptr)
	{
		NLS_LOG_WARNING("Framebuffer: Failed to create color texture");
		return;
	}

	m_explicitDepthStencilTexture = device->CreateTexture(CreateDepthTextureDesc(p_width, p_height));
	if (m_explicitDepthStencilTexture == nullptr)
	{
		NLS_LOG_WARNING("Framebuffer: Failed to create depth texture");
		Release();
		return;
	}

	m_width = p_width;
	m_height = p_height;
}

NLS::Render::Buffers::Framebuffer::~Framebuffer()
{
	Release();
}

void NLS::Render::Buffers::Framebuffer::Release()
{
	ResetCurrentResources();
	for (auto& resourceSet : m_retiredResourceSets)
		resourceSet.Reset();
	m_retiredResourceSets.clear();
}

void NLS::Render::Buffers::Framebuffer::ResetCurrentResources()
{
	m_explicitRenderTextureView.reset();
	m_explicitDepthStencilTextureView.reset();
	m_explicitRenderTexture.reset();
	m_explicitDepthStencilTexture.reset();
}

void NLS::Render::Buffers::Framebuffer::RetireCurrentResources()
{
	RetiredResourceSet resourceSet;
	resourceSet.colorTexture = std::move(m_explicitRenderTexture);
	resourceSet.depthStencilTexture = std::move(m_explicitDepthStencilTexture);
	resourceSet.colorView = std::move(m_explicitRenderTextureView);
	resourceSet.depthStencilView = std::move(m_explicitDepthStencilTextureView);

	if (resourceSet.HasResources())
		m_retiredResourceSets.push_back(std::move(resourceSet));
}

void NLS::Render::Buffers::Framebuffer::PruneRetiredResources()
{
	while (m_retiredResourceSets.size() > kMaxRetiredFramebufferResourceSets)
	{
		m_retiredResourceSets.front().Reset();
		m_retiredResourceSets.pop_front();
	}
}

void NLS::Render::Buffers::Framebuffer::Resize(uint16_t p_width, uint16_t p_height, bool p_forceUpdate)
{
	if (!p_forceUpdate && p_width == m_width && p_height == m_height)
		return;

	if (p_width == 0u || p_height == 0u)
	{
		RetireCurrentResources();
		m_width = p_width;
		m_height = p_height;
		return;
	}

	auto& driver = NLS::Render::Context::RequireLocatedDriver("Framebuffer::Resize");
	auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver);
	if (device == nullptr)
	{
		NLS_LOG_WARNING("Framebuffer::Resize: Explicit RHI device not available");
		return;
	}

		auto nextColorTexture = device->CreateTexture(CreateColorTextureDesc(
			p_width,
			p_height,
			m_colorSpace,
			m_colorOptimizedClearValue));
	if (nextColorTexture == nullptr)
	{
		NLS_LOG_WARNING("Framebuffer::Resize: Failed to recreate color texture");
		PruneRetiredResources();
		return;
	}

	auto nextDepthStencilTexture = device->CreateTexture(CreateDepthTextureDesc(p_width, p_height));
	if (nextDepthStencilTexture == nullptr)
	{
		NLS_LOG_WARNING("Framebuffer::Resize: Failed to recreate depth texture");
		PruneRetiredResources();
		return;
	}

	RetireCurrentResources();
	m_explicitRenderTexture = std::move(nextColorTexture);
	m_explicitDepthStencilTexture = std::move(nextDepthStencilTexture);
	m_width = p_width;
	m_height = p_height;
	PruneRetiredResources();
}

void NLS::Render::Buffers::Framebuffer::SetOptimizedColorClearValue(float r, float g, float b, float a)
{
	const auto clearValue = NLS::Render::RHI::RHITextureDesc::OptimizedClearValue::Color(r, g, b, a);

	if (SameOptimizedColorClearValue(m_colorOptimizedClearValue, clearValue))
		return;

	m_colorOptimizedClearValue = clearValue;
	if (m_explicitRenderTexture != nullptr)
		Resize(m_width, m_height, true);
}

const std::shared_ptr<NLS::Render::RHI::RHITexture>& NLS::Render::Buffers::Framebuffer::GetExplicitTextureHandle() const
{
	return m_explicitRenderTexture;
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
		viewDesc.colorSpace = m_explicitRenderTexture->GetDesc().colorSpace;
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
