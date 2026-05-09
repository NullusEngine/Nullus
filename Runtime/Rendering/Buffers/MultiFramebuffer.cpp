#include "Rendering/Buffers/MultiFramebuffer.h"
#include "Debug/Logger.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIDevice.h"

#include <utility>

namespace
{
	constexpr size_t kMaxRetiredMultiFramebufferResourceSets = 16u;

	NLS::Render::RHI::RHITextureDesc CreateColorTextureDesc(uint32_t width, uint32_t height, NLS::Render::RHI::TextureFormat format, uint32_t index)
	{
		NLS::Render::RHI::RHITextureDesc desc{};
		desc.extent.width = width;
		desc.extent.height = height;
		desc.extent.depth = 1;
		desc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
		desc.format = format;
		desc.usage = NLS::Render::RHI::TextureUsageFlags::Sampled | NLS::Render::RHI::TextureUsageFlags::ColorAttachment;
		desc.optimizedClearValue.enabled = true;
		desc.debugName = "MultiFramebufferColorTexture" + std::to_string(index);
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
		desc.usage = NLS::Render::RHI::TextureUsageFlags::DepthStencilAttachment | NLS::Render::RHI::TextureUsageFlags::Sampled;
		desc.optimizedClearValue.enabled = true;
		desc.optimizedClearValue.depth = 1.0f;
		desc.optimizedClearValue.stencil = 0u;
		desc.debugName = "MultiFramebufferDepthTexture";
		return desc;
	}
}

bool NLS::Render::Buffers::MultiFramebuffer::RetiredResourceSet::HasResources() const
{
	if (depthTexture != nullptr || depthTextureView != nullptr)
		return true;

	for (const auto& texture : colorTextures)
	{
		if (texture != nullptr)
			return true;
	}

	for (const auto& view : colorTextureViews)
	{
		if (view != nullptr)
			return true;
	}

	return false;
}

void NLS::Render::Buffers::MultiFramebuffer::RetiredResourceSet::Reset()
{
	for (auto& view : colorTextureViews)
		view.reset();
	depthTextureView.reset();

	for (auto& texture : colorTextures)
		texture.reset();
	depthTexture.reset();

	colorTextureViews.clear();
	colorTextures.clear();
}

namespace NLS::Render::Buffers
{
	MultiFramebuffer::MultiFramebuffer(uint16_t width, uint16_t height, const std::vector<AttachmentDesc>& colorAttachments, bool withDepth)
	{
		Init(width, height, colorAttachments, withDepth);
	}

	MultiFramebuffer::~MultiFramebuffer()
	{
		Release();
	}

	void MultiFramebuffer::Init(uint16_t width, uint16_t height, const std::vector<AttachmentDesc>& colorAttachments, bool withDepth)
	{
		Release();
		m_width = width;
		m_height = height;
		m_withDepth = withDepth;
		m_attachmentDescs = colorAttachments;
		if (width == 0u || height == 0u)
			return;

		if (!Allocate(width, height))
		{
			m_width = 0u;
			m_height = 0u;
		}
	}

	void MultiFramebuffer::Resize(uint16_t width, uint16_t height)
	{
		if (width == m_width && height == m_height)
			return;

		if (width == 0u || height == 0u)
		{
			RetireCurrentResources();
			m_width = width;
			m_height = height;
			PruneRetiredResources();
			return;
		}

		if (Allocate(width, height))
		{
			m_width = width;
			m_height = height;
		}
	}

	bool MultiFramebuffer::IsInitialized() const
	{
		if (m_withDepth && m_explicitDepthTexture != nullptr)
			return true;

		for (const auto& texture : m_explicitColorTextures)
		{
			if (texture != nullptr)
				return true;
		}

		return false;
	}

	void MultiFramebuffer::Release()
	{
		ResetCurrentResources();
		for (auto& resourceSet : m_retiredResourceSets)
			resourceSet.Reset();
		m_retiredResourceSets.clear();
	}

	void MultiFramebuffer::ResetCurrentResources()
	{
		m_explicitColorTextureViews.clear();
		m_explicitDepthTextureView.reset();
		m_explicitColorTextures.clear();
		m_explicitDepthTexture.reset();
	}

	void MultiFramebuffer::RetireCurrentResources()
	{
		RetiredResourceSet resourceSet;
		resourceSet.colorTextures = std::move(m_explicitColorTextures);
		resourceSet.colorTextureViews = std::move(m_explicitColorTextureViews);
		resourceSet.depthTexture = std::move(m_explicitDepthTexture);
		resourceSet.depthTextureView = std::move(m_explicitDepthTextureView);

		if (resourceSet.HasResources())
			m_retiredResourceSets.push_back(std::move(resourceSet));
	}

	void MultiFramebuffer::PruneRetiredResources()
	{
		while (m_retiredResourceSets.size() > kMaxRetiredMultiFramebufferResourceSets)
		{
			m_retiredResourceSets.front().Reset();
			m_retiredResourceSets.pop_front();
		}
	}

	bool MultiFramebuffer::Allocate(uint16_t width, uint16_t height)
	{
		auto& driver = NLS::Render::Context::RequireLocatedDriver("MultiFramebuffer::Allocate");
		auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver);
		if (device == nullptr)
		{
			NLS_LOG_WARNING("MultiFramebuffer::Allocate: Explicit RHI device not available");
			return false;
		}

		std::vector<std::shared_ptr<NLS::Render::RHI::RHITexture>> nextColorTextures;
		std::shared_ptr<NLS::Render::RHI::RHITexture> nextDepthTexture;

		// Create color textures
		if (!m_attachmentDescs.empty())
		{
			nextColorTextures.resize(m_attachmentDescs.size());

			for (size_t i = 0; i < m_attachmentDescs.size(); ++i)
			{
				auto desc = CreateColorTextureDesc(width, height, m_attachmentDescs[i].format, static_cast<uint32_t>(i));
				nextColorTextures[i] = device->CreateTexture(desc);
				if (nextColorTextures[i] == nullptr)
				{
					NLS_LOG_WARNING("MultiFramebuffer::Allocate: Failed to create color texture " + std::to_string(i));
					PruneRetiredResources();
					return false;
				}
			}
		}

		// Create depth texture
		if (m_withDepth)
		{
			auto desc = CreateDepthTextureDesc(width, height);
			nextDepthTexture = device->CreateTexture(desc);
			if (nextDepthTexture == nullptr)
			{
				NLS_LOG_WARNING("MultiFramebuffer::Allocate: Failed to create depth texture");
				PruneRetiredResources();
				return false;
			}
		}

		RetireCurrentResources();
		m_explicitColorTextures = std::move(nextColorTextures);
		m_explicitColorTextureViews.assign(m_explicitColorTextures.size(), nullptr);
		m_explicitDepthTexture = std::move(nextDepthTexture);
		PruneRetiredResources();
		return true;
	}

	std::shared_ptr<NLS::Render::RHI::RHITextureView> MultiFramebuffer::GetOrCreateExplicitColorView(size_t index, const std::string& debugName) const
	{
		if (index >= m_explicitColorTextures.size() || m_explicitColorTextures[index] == nullptr)
			return nullptr;

		if (index < m_explicitColorTextureViews.size() && m_explicitColorTextureViews[index] != nullptr)
			return m_explicitColorTextureViews[index];

		auto& driver = NLS::Render::Context::RequireLocatedDriver("MultiFramebuffer::GetOrCreateExplicitColorView");
		auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver);
		if (device == nullptr)
			return nullptr;

		if (index >= m_explicitColorTextureViews.size())
			m_explicitColorTextureViews.resize(m_explicitColorTextures.size());

		NLS::Render::RHI::RHITextureViewDesc viewDesc{};
		viewDesc.format = m_explicitColorTextures[index]->GetDesc().format;
		viewDesc.viewType = NLS::Render::RHI::TextureViewType::Texture2D;
		viewDesc.debugName = debugName.empty() ? ("MultiFramebufferColorView" + std::to_string(index)) : debugName;

		m_explicitColorTextureViews[index] = device->CreateTextureView(m_explicitColorTextures[index], viewDesc);
		return m_explicitColorTextureViews[index];
	}

	std::shared_ptr<NLS::Render::RHI::RHITextureView> MultiFramebuffer::GetOrCreateExplicitDepthView(const std::string& debugName) const
	{
		if (m_explicitDepthTexture == nullptr)
			return nullptr;

		if (m_explicitDepthTextureView != nullptr)
			return m_explicitDepthTextureView;

		auto& driver = NLS::Render::Context::RequireLocatedDriver("MultiFramebuffer::GetOrCreateExplicitDepthView");
		auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver);
		if (device == nullptr)
			return nullptr;

		NLS::Render::RHI::RHITextureViewDesc viewDesc{};
		viewDesc.format = m_explicitDepthTexture->GetDesc().format;
		viewDesc.viewType = NLS::Render::RHI::TextureViewType::Texture2D;
		viewDesc.debugName = debugName.empty() ? "MultiFramebufferDepthView" : debugName;

		m_explicitDepthTextureView = device->CreateTextureView(m_explicitDepthTexture, viewDesc);
		return m_explicitDepthTextureView;
	}
}
