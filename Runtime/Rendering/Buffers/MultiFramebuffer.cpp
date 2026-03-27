#include "Rendering/Buffers/MultiFramebuffer.h"
#include "Core/ServiceLocator.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/RHI/Backends/OpenGL/Compat/ExplicitRHICompat.h"

using Driver = NLS::Render::Context::Driver;

namespace NLS::Render::Buffers
{
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
        NLS_SERVICE(Driver).BindFramebuffer(m_bufferId);
    }

    void MultiFramebuffer::Unbind() const
    {
        NLS_SERVICE(Driver).BindFramebuffer(0);
    }

    void MultiFramebuffer::Release()
    {
        auto& rhi = NLS_SERVICE(Driver);

        if (!m_colorTextures.empty())
        {
            for (size_t i = 0; i < m_colorTextures.size(); ++i)
            {
                if (i < m_colorTextureResources.size() && m_colorTextureResources[i])
                    m_colorTextureResources[i].reset();
                else if (m_colorTextures[i] != 0)
                    rhi.DestroyTexture(m_colorTextures[i]);
            }
            m_colorTextures.clear();
        }
        m_colorTextureResources.clear();
        m_explicitColorTextures.clear();
        m_explicitColorTextureViews.clear();

        if (m_depthTexture != 0)
        {
            if (m_depthTextureResource)
                m_depthTextureResource.reset();
            else
                rhi.DestroyTexture(m_depthTexture);
            m_depthTexture = 0;
        }
        m_depthTextureResource.reset();
        m_explicitDepthTexture.reset();
        m_explicitDepthTextureView.reset();

        if (m_bufferId != 0)
        {
            rhi.DestroyFramebuffer(m_bufferId);
            m_bufferId = 0;
        }
    }

    void MultiFramebuffer::Allocate()
    {
        auto& rhi = NLS_SERVICE(Driver);

		if (m_bufferId == 0)
			m_bufferId = rhi.CreateFramebuffer();

		Bind();

        if (!m_colorTextures.empty())
        {
            for (const auto textureId : m_colorTextures)
                rhi.DestroyTexture(textureId);
            m_colorTextures.clear();
        }

		if (!m_attachmentDescs.empty())
		{
			m_colorTextures.resize(m_attachmentDescs.size(), 0);
			m_colorTextureResources.resize(m_attachmentDescs.size());
			m_explicitColorTextures.resize(m_attachmentDescs.size());
			m_explicitColorTextureViews.assign(m_attachmentDescs.size(), nullptr);

            for (size_t i = 0; i < m_attachmentDescs.size(); ++i)
            {
                m_colorTextureResources[i] = rhi.CreateTextureResource();
                m_explicitColorTextures[i] = m_colorTextureResources[i]
                    ? NLS::Render::RHI::WrapCompatibilityTexture(m_colorTextureResources[i], "MultiFramebufferColorTexture" + std::to_string(i))
                    : nullptr;
                m_colorTextures[i] = m_colorTextureResources[i]
                    ? m_colorTextureResources[i]->GetResourceId()
                    : rhi.CreateTexture();
                NLS::Render::RHI::TextureDesc colorDesc{};
                colorDesc.width = m_width;
                colorDesc.height = m_height;
                colorDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
                colorDesc.format = m_attachmentDescs[i].format;
                colorDesc.minFilter = NLS::Render::RHI::TextureFilter::Nearest;
                colorDesc.magFilter = NLS::Render::RHI::TextureFilter::Nearest;
                colorDesc.wrapS = NLS::Render::RHI::TextureWrap::ClampToEdge;
                colorDesc.wrapT = NLS::Render::RHI::TextureWrap::ClampToEdge;
                colorDesc.usage = NLS::Render::RHI::TextureUsage::Sampled | NLS::Render::RHI::TextureUsage::ColorAttachment;

				rhi.BindTexture(NLS::Render::RHI::TextureDimension::Texture2D, m_colorTextures[i]);
				rhi.SetupTexture(colorDesc, nullptr);
			}
		}

        if (m_withDepth)
        {
            if (m_depthTexture == 0)
            {
                m_depthTextureResource = rhi.CreateTextureResource();
                m_explicitDepthTexture = m_depthTextureResource
                    ? NLS::Render::RHI::WrapCompatibilityTexture(m_depthTextureResource, "MultiFramebufferDepthTexture")
                    : nullptr;
                m_depthTexture = m_depthTextureResource
                    ? m_depthTextureResource->GetResourceId()
                    : rhi.CreateTexture();
            }

            NLS::Render::RHI::TextureDesc depthDesc{};
            depthDesc.width = m_width;
            depthDesc.height = m_height;
            depthDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
            depthDesc.format = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
            depthDesc.minFilter = NLS::Render::RHI::TextureFilter::Nearest;
            depthDesc.magFilter = NLS::Render::RHI::TextureFilter::Nearest;
            depthDesc.wrapS = NLS::Render::RHI::TextureWrap::ClampToEdge;
            depthDesc.wrapT = NLS::Render::RHI::TextureWrap::ClampToEdge;
            depthDesc.usage = NLS::Render::RHI::TextureUsage::DepthStencilAttachment | NLS::Render::RHI::TextureUsage::Sampled;

			rhi.BindTexture(NLS::Render::RHI::TextureDimension::Texture2D, m_depthTexture);
			rhi.SetupTexture(depthDesc, nullptr);
		}

		NLS::Render::RHI::FramebufferDesc framebufferDesc{};
		framebufferDesc.drawBufferCount = static_cast<uint32_t>(m_attachmentDescs.size());
		for (size_t i = 0; i < m_attachmentDescs.size(); ++i)
		{
			framebufferDesc.colorAttachments.push_back({
				m_colorTextures[i],
				m_attachmentDescs[i].format
			});
		}
		framebufferDesc.depthStencilTextureId = m_depthTexture;
		framebufferDesc.depthStencilFormat = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
		rhi.ConfigureFramebuffer(m_bufferId, framebufferDesc);

		Unbind();
	}

    std::shared_ptr<NLS::Render::RHI::RHITextureView> MultiFramebuffer::GetOrCreateExplicitColorView(size_t index, const std::string& debugName) const
    {
        if (index >= m_explicitColorTextures.size() || m_explicitColorTextures[index] == nullptr)
            return nullptr;

        if (index < m_explicitColorTextureViews.size() && m_explicitColorTextureViews[index] != nullptr)
            return m_explicitColorTextureViews[index];

        if (index >= m_explicitColorTextureViews.size())
            m_explicitColorTextureViews.resize(m_explicitColorTextures.size());

        NLS::Render::RHI::RHITextureViewDesc viewDesc;
        viewDesc.format = m_explicitColorTextures[index]->GetDesc().format;
        viewDesc.viewType = NLS::Render::RHI::TextureViewType::Texture2D;
        viewDesc.debugName = debugName;
        auto& driver = NLS_SERVICE(Driver);
        m_explicitColorTextureViews[index] = CreateTextureViewForExplicitPath(driver, m_explicitColorTextures[index], viewDesc);
        return m_explicitColorTextureViews[index];
    }

    std::shared_ptr<NLS::Render::RHI::RHITextureView> MultiFramebuffer::GetOrCreateExplicitDepthView(const std::string& debugName) const
    {
        if (m_explicitDepthTexture == nullptr)
            return nullptr;

        if (m_explicitDepthTextureView != nullptr)
            return m_explicitDepthTextureView;

        NLS::Render::RHI::RHITextureViewDesc viewDesc;
        viewDesc.format = m_explicitDepthTexture->GetDesc().format;
        viewDesc.viewType = NLS::Render::RHI::TextureViewType::Texture2D;
        viewDesc.debugName = debugName;
        auto& driver = NLS_SERVICE(Driver);
        m_explicitDepthTextureView = CreateTextureViewForExplicitPath(driver, m_explicitDepthTexture, viewDesc);
        return m_explicitDepthTextureView;
    }
}
