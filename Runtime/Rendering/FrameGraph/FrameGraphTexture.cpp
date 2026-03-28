#include "Rendering/FrameGraph/FrameGraphTexture.h"

#include <Debug/Assertion.h>

#include "Rendering/Context/Driver.h"
#include "Rendering/FrameGraph/FrameGraphExecutionContext.h"

namespace NLS::Render::FrameGraph
{
	namespace
	{
		struct ExplicitTextureState
		{
			NLS::Render::RHI::ResourceState state = NLS::Render::RHI::ResourceState::Unknown;
			NLS::Render::RHI::PipelineStageMask stageMask = NLS::Render::RHI::PipelineStageMask::AllCommands;
			NLS::Render::RHI::AccessMask accessMask = NLS::Render::RHI::AccessMask::MemoryRead;
		};

		NLS::Render::RHI::TextureUsageFlags ToExplicitTextureUsage(const NLS::Render::RHI::TextureUsage usage)
		{
			NLS::Render::RHI::TextureUsageFlags flags = NLS::Render::RHI::TextureUsageFlags::None;
			if (NLS::Render::RHI::HasUsage(usage, NLS::Render::RHI::TextureUsage::Sampled))
				flags = flags | NLS::Render::RHI::TextureUsageFlags::Sampled;
			if (NLS::Render::RHI::HasUsage(usage, NLS::Render::RHI::TextureUsage::ColorAttachment))
				flags = flags | NLS::Render::RHI::TextureUsageFlags::ColorAttachment;
			if (NLS::Render::RHI::HasUsage(usage, NLS::Render::RHI::TextureUsage::DepthStencilAttachment))
				flags = flags | NLS::Render::RHI::TextureUsageFlags::DepthStencilAttachment;
			if (NLS::Render::RHI::HasUsage(usage, NLS::Render::RHI::TextureUsage::Storage))
				flags = flags | NLS::Render::RHI::TextureUsageFlags::Storage;
			return flags;
		}

		NLS::Render::RHI::RHITextureDesc ToExplicitTextureDesc(const FrameGraphTexture::Desc& desc)
		{
			NLS::Render::RHI::RHITextureDesc explicitDesc;
			explicitDesc.extent.width = desc.width;
			explicitDesc.extent.height = desc.height;
			explicitDesc.extent.depth = 1u;
			explicitDesc.dimension = desc.dimension;
			explicitDesc.format = desc.format;
			explicitDesc.arrayLayers = desc.dimension == NLS::Render::RHI::TextureDimension::TextureCube ? 6u : 1u;
			explicitDesc.usage = ToExplicitTextureUsage(desc.usage);
			explicitDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUOnly;
			explicitDesc.debugName = "FrameGraphTexture";
			return explicitDesc;
		}

		NLS::Render::RHI::RHISubresourceRange GetFullSubresourceRange(const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture)
		{
			NLS::Render::RHI::RHISubresourceRange range;
			if (texture == nullptr)
				return range;

			range.baseMipLevel = 0u;
			range.mipLevelCount = texture->GetDesc().mipLevels;
			range.baseArrayLayer = 0u;
			range.arrayLayerCount = texture->GetDesc().arrayLayers;
			return range;
		}

		ExplicitTextureState GetExplicitTextureReadState(const FrameGraphTexture::Desc& desc)
		{
			if (NLS::Render::RHI::HasUsage(desc.usage, NLS::Render::RHI::TextureUsage::DepthStencilAttachment) &&
				!NLS::Render::RHI::HasUsage(desc.usage, NLS::Render::RHI::TextureUsage::Sampled) &&
				!NLS::Render::RHI::HasUsage(desc.usage, NLS::Render::RHI::TextureUsage::Storage))
			{
				return {
					NLS::Render::RHI::ResourceState::DepthRead,
					NLS::Render::RHI::PipelineStageMask::DepthStencil,
					NLS::Render::RHI::AccessMask::DepthStencilRead
				};
			}

			return {
				NLS::Render::RHI::ResourceState::ShaderRead,
				NLS::Render::RHI::PipelineStageMask::AllGraphics | NLS::Render::RHI::PipelineStageMask::ComputeShader,
				NLS::Render::RHI::AccessMask::ShaderRead
			};
		}

		ExplicitTextureState GetExplicitTextureWriteState(const FrameGraphTexture::Desc& desc)
		{
			if (NLS::Render::RHI::HasUsage(desc.usage, NLS::Render::RHI::TextureUsage::DepthStencilAttachment))
			{
				return {
					NLS::Render::RHI::ResourceState::DepthWrite,
					NLS::Render::RHI::PipelineStageMask::DepthStencil,
					NLS::Render::RHI::AccessMask::DepthStencilWrite
				};
			}

			if (NLS::Render::RHI::HasUsage(desc.usage, NLS::Render::RHI::TextureUsage::ColorAttachment))
			{
				return {
					NLS::Render::RHI::ResourceState::RenderTarget,
					NLS::Render::RHI::PipelineStageMask::RenderTarget,
					NLS::Render::RHI::AccessMask::ColorAttachmentWrite
				};
			}

			if (NLS::Render::RHI::HasUsage(desc.usage, NLS::Render::RHI::TextureUsage::Storage))
			{
				return {
					NLS::Render::RHI::ResourceState::ShaderWrite,
					NLS::Render::RHI::PipelineStageMask::AllGraphics | NLS::Render::RHI::PipelineStageMask::ComputeShader,
					NLS::Render::RHI::AccessMask::ShaderWrite
				};
			}

			return GetExplicitTextureReadState(desc);
		}
	}

	void FrameGraphTexture::create(const Desc& desc, void* allocator)
	{
		auto* executionContext = static_cast<FrameGraphExecutionContext*>(allocator);
		NLS_ASSERT(executionContext != nullptr, "FrameGraphTexture requires a valid frame graph execution context");
		auto& driver = executionContext->driver;
		auto* device = executionContext->device;

		if (id != 0)
		{
			if (explicitTexture != nullptr && explicitView == nullptr && device != nullptr)
			{
				NLS::Render::RHI::RHITextureViewDesc viewDesc;
				viewDesc.format = explicitTexture->GetDesc().format;
				viewDesc.debugName = "FrameGraphTextureView";
				explicitView = device->CreateTextureView(explicitTexture, viewDesc);
			}
			return;
		}

		if (device != nullptr)
		{
			explicitTexture = device->CreateTexture(ToExplicitTextureDesc(desc));
			ownsResource = explicitTexture != nullptr;
		}

		if (explicitTexture != nullptr)
		{
			id = 0;
			NLS::Render::RHI::RHITextureViewDesc viewDesc;
			viewDesc.format = explicitTexture->GetDesc().format;
			viewDesc.debugName = "FrameGraphTextureView";
			explicitView = device != nullptr ? device->CreateTextureView(explicitTexture, viewDesc) : nullptr;
			return;
		}

		textureResource = driver.CreateTextureResource(desc.dimension);
		id = textureResource != nullptr ? textureResource->GetResourceId() : driver.CreateTexture();
		ownsResource = true;
		driver.BindTexture(desc.dimension, id);
		driver.SetupTexture(desc, nullptr);
		driver.BindTexture(desc.dimension, 0);
	}

	void FrameGraphTexture::destroy(const Desc& desc, void* allocator)
	{
		(void)desc;
		auto* executionContext = static_cast<FrameGraphExecutionContext*>(allocator);
		NLS_ASSERT(executionContext != nullptr, "FrameGraphTexture requires a valid frame graph execution context");
		auto& driver = executionContext->driver;
		const bool hadExplicitResource = explicitTexture != nullptr || explicitView != nullptr;

		if (id == 0 && explicitTexture == nullptr && explicitView == nullptr && textureResource == nullptr)
			return;

		explicitView.reset();
		explicitTexture.reset();
		if (hadExplicitResource)
		{
			id = 0;
			ownsResource = false;
			return;
		}
		if (ownsResource)
		{
			if (textureResource != nullptr)
				textureResource.reset();
			else
				driver.DestroyTexture(id);
		}
		id = 0;
		ownsResource = false;
	}

	void FrameGraphTexture::preRead(const Desc& desc, uint32_t flags, void* context)
	{
		auto* executionContext = static_cast<FrameGraphExecutionContext*>(context);
		NLS_ASSERT(executionContext != nullptr, "FrameGraphTexture requires a valid frame graph execution context");
		auto& driver = executionContext->driver;

		if (explicitTexture != nullptr && executionContext->CanTrackExplicitResourceState())
		{
			const auto targetState = GetExplicitTextureReadState(desc);
			NLS::Render::RHI::RHIBarrierDesc barrierDesc;
			barrierDesc.textureBarriers.push_back({
				explicitTexture,
				NLS::Render::RHI::ResourceState::Unknown,
				targetState.state,
				GetFullSubresourceRange(explicitTexture),
				NLS::Render::RHI::PipelineStageMask::AllCommands,
				targetState.stageMask,
				NLS::Render::RHI::AccessMask::MemoryRead | NLS::Render::RHI::AccessMask::MemoryWrite,
				targetState.accessMask
			});
			executionContext->RecordResourceBarriers(barrierDesc);
			return;
		}

		if (id == 0)
			return;

		const auto slot = flags == 0xFFFFFFFFu ? 0u : flags;
		driver.ActivateTexture(slot);
		driver.BindTexture(desc.dimension, id);
	}

	void FrameGraphTexture::preWrite(const Desc& desc, uint32_t, void* context)
	{
		auto* executionContext = static_cast<FrameGraphExecutionContext*>(context);
		NLS_ASSERT(executionContext != nullptr, "FrameGraphTexture requires a valid frame graph execution context");

		if (explicitTexture != nullptr && executionContext->CanTrackExplicitResourceState())
		{
			const auto targetState = GetExplicitTextureWriteState(desc);
			NLS::Render::RHI::RHIBarrierDesc barrierDesc;
			barrierDesc.textureBarriers.push_back({
				explicitTexture,
				NLS::Render::RHI::ResourceState::Unknown,
				targetState.state,
				GetFullSubresourceRange(explicitTexture),
				NLS::Render::RHI::PipelineStageMask::AllCommands,
				targetState.stageMask,
				NLS::Render::RHI::AccessMask::MemoryRead | NLS::Render::RHI::AccessMask::MemoryWrite,
				targetState.accessMask
			});
			executionContext->RecordResourceBarriers(barrierDesc);
		}
	}

	std::string FrameGraphTexture::toString(const Desc& desc)
	{
		return std::to_string(desc.width) + "x" + std::to_string(desc.height);
	}
}
