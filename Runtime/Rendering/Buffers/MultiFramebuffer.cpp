#include "Rendering/Buffers/MultiFramebuffer.h"
#include "Debug/Logger.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIDevice.h"

namespace
{
	NLS::Render::RHI::RHITextureDesc CreateColorTextureDesc(uint32_t width, uint32_t height, NLS::Render::RHI::TextureFormat format, uint32_t index)
	{
		NLS::Render::RHI::RHITextureDesc desc{};
		desc.extent.width = width;
		desc.extent.height = height;
		desc.extent.depth = 1;
		desc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
		desc.format = format;
		desc.usage = NLS::Render::RHI::TextureUsageFlags::Sampled | NLS::Render::RHI::TextureUsageFlags::ColorAttachment;
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
		desc.debugName = "MultiFramebufferDepthTexture";
		return desc;
	}
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
		Allocate();
	}

	void MultiFramebuffer::Resize(uint16_t width, uint16_t height)
	{
		if (width == m_width && height == m_height)
			return;

		m_width = width;
		m_height = height;
		Allocate();
	}

	void MultiFramebuffer::Bind() const
	{
		// In formal RHI, binding is handled at command buffer level through render passes
		// This is a no-op placeholder
	}

	void MultiFramebuffer::Unbind() const
	{
		// In formal RHI, unbinding is handled at command buffer level
		// This is a no-op placeholder
	}

	void MultiFramebuffer::Release()
	{
		m_explicitColorTextures.clear();
		m_explicitColorTextureViews.clear();
		m_explicitDepthTexture.reset();
		m_explicitDepthTextureView.reset();

		// Clear legacy resources (no longer used but kept for ABI compatibility)
		m_colorTextures.clear();
		m_colorTextureResources.clear();
		m_depthTextureResource.reset();
	}

	void MultiFramebuffer::Allocate()
	{
		auto& driver = NLS::Render::Context::RequireLocatedDriver("MultiFramebuffer::Allocate");
		auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver);
		if (device == nullptr)
		{
			NLS_LOG_WARNING("MultiFramebuffer::Allocate: Explicit RHI device not available");
			return;
		}

		// Release existing textures first
		m_explicitColorTextures.clear();
		m_explicitColorTextureViews.clear();
		m_explicitDepthTexture.reset();
		m_explicitDepthTextureView.reset();

		// Create color textures
		if (!m_attachmentDescs.empty())
		{
			m_explicitColorTextures.resize(m_attachmentDescs.size());
			m_explicitColorTextureViews.assign(m_attachmentDescs.size(), nullptr);

			for (size_t i = 0; i < m_attachmentDescs.size(); ++i)
			{
				auto desc = CreateColorTextureDesc(m_width, m_height, m_attachmentDescs[i].format, static_cast<uint32_t>(i));
				m_explicitColorTextures[i] = device->CreateTexture(desc, nullptr);
				if (m_explicitColorTextures[i] == nullptr)
				{
					NLS_LOG_WARNING("MultiFramebuffer::Allocate: Failed to create color texture " + std::to_string(i));
				}
			}
		}

		// Create depth texture
		if (m_withDepth)
		{
			auto desc = CreateDepthTextureDesc(m_width, m_height);
			m_explicitDepthTexture = device->CreateTexture(desc, nullptr);
			if (m_explicitDepthTexture == nullptr)
			{
				NLS_LOG_WARNING("MultiFramebuffer::Allocate: Failed to create depth texture");
			}
		}
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
