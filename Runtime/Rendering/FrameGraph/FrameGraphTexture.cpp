#include "Rendering/FrameGraph/FrameGraphTexture.h"

#include <Debug/Assertion.h>

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

		NLS::Render::RHI::TextureUsageFlags ToExplicitTextureUsage(const NLS::Render::RHI::TextureUsageFlags usage)
		{
			return usage;
		}

		NLS::Render::RHI::RHITextureDesc ToExplicitTextureDesc(const FrameGraphTexture::Desc& desc)
		{
			NLS::Render::RHI::RHITextureDesc explicitDesc = desc;
			explicitDesc.arrayLayers = desc.dimension == NLS::Render::RHI::TextureDimension::TextureCube ? 6u : 1u;
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
			if (NLS::Render::RHI::HasTextureUsage(desc.usage, NLS::Render::RHI::TextureUsageFlags::DepthStencilAttachment) &&
				!NLS::Render::RHI::HasTextureUsage(desc.usage, NLS::Render::RHI::TextureUsageFlags::Sampled) &&
				!NLS::Render::RHI::HasTextureUsage(desc.usage, NLS::Render::RHI::TextureUsageFlags::Storage))
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
			if (NLS::Render::RHI::HasTextureUsage(desc.usage, NLS::Render::RHI::TextureUsageFlags::DepthStencilAttachment))
			{
				return {
					NLS::Render::RHI::ResourceState::DepthWrite,
					NLS::Render::RHI::PipelineStageMask::DepthStencil,
					NLS::Render::RHI::AccessMask::DepthStencilWrite
				};
			}

			if (NLS::Render::RHI::HasTextureUsage(desc.usage, NLS::Render::RHI::TextureUsageFlags::ColorAttachment))
			{
				return {
					NLS::Render::RHI::ResourceState::RenderTarget,
					NLS::Render::RHI::PipelineStageMask::RenderTarget,
					NLS::Render::RHI::AccessMask::ColorAttachmentWrite
				};
			}

			if (NLS::Render::RHI::HasTextureUsage(desc.usage, NLS::Render::RHI::TextureUsageFlags::Storage))
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
		auto* device = executionContext->device;
		NLS_ASSERT(device != nullptr, "FrameGraphTexture requires an explicit RHIDevice in formal-only mode.");

		if (explicitTexture != nullptr)
		{
			if (explicitView == nullptr)
			{
				NLS::Render::RHI::RHITextureViewDesc viewDesc;
				viewDesc.format = explicitTexture->GetDesc().format;
				viewDesc.debugName = "FrameGraphTextureView";
				explicitView = device->CreateTextureView(explicitTexture, viewDesc);
			}
			return;
		}

		explicitTexture = device->CreateTexture(ToExplicitTextureDesc(desc));
		NLS_ASSERT(explicitTexture != nullptr, "FrameGraphTexture failed to create explicit texture.");
		ownsResource = true;

		NLS::Render::RHI::RHITextureViewDesc viewDesc;
		viewDesc.format = explicitTexture->GetDesc().format;
		viewDesc.debugName = "FrameGraphTextureView";
		explicitView = device->CreateTextureView(explicitTexture, viewDesc);
	}

	void FrameGraphTexture::destroy(const Desc& desc, void* allocator)
	{
		(void)desc;
		auto* executionContext = static_cast<FrameGraphExecutionContext*>(allocator);
		NLS_ASSERT(executionContext != nullptr, "FrameGraphTexture requires a valid frame graph execution context");
		if (explicitTexture == nullptr && explicitView == nullptr)
			return;

		explicitView.reset();
		explicitTexture.reset();
		ownsResource = false;
	}

	void FrameGraphTexture::preRead(const Desc& desc, uint32_t flags, void* context)
	{
		auto* executionContext = static_cast<FrameGraphExecutionContext*>(context);
		NLS_ASSERT(executionContext != nullptr, "FrameGraphTexture requires a valid frame graph execution context");
		(void)flags;

		if (explicitTexture == nullptr || !executionContext->CanTrackExplicitResourceState())
			return;

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
		return std::to_string(desc.extent.width) + "x" + std::to_string(desc.extent.height);
	}
}
