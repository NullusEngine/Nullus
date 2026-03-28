#pragma once

#include <algorithm>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Utils/ResourceStateTracker/ResourceStateTracker.h"

namespace NLS::Render::Context
{
	class Driver;
}

namespace NLS::Render::RHI
{
	class RHIDevice;
	class RHICommandBuffer;
}

namespace NLS::Render::FrameGraph
{
	struct NLS_RENDER_API FrameGraphExecutionContext
	{
		NLS::Render::Context::Driver& driver;
		NLS::Render::RHI::RHIDevice* device = nullptr;
		NLS::Render::RHI::RHICommandBuffer* commandBuffer = nullptr;
		NLS::Render::RHI::RHIFrameContext* frameContext = nullptr;

		bool HasExplicitContext() const
		{
			return device != nullptr && commandBuffer != nullptr && frameContext != nullptr;
		}

		bool CanTrackExplicitResourceState() const
		{
			return commandBuffer != nullptr &&
				frameContext != nullptr &&
				frameContext->resourceStateTracker != nullptr;
		}

		void RecordResourceBarriers(const NLS::Render::RHI::RHIBarrierDesc& requestedBarriers) const
		{
			if (!CanTrackExplicitResourceState())
				return;

			const auto hasWriteAccess = [](const NLS::Render::RHI::AccessMask accessMask)
			{
				constexpr uint32_t kWriteAccessMask =
					static_cast<uint32_t>(NLS::Render::RHI::AccessMask::CopyWrite) |
					static_cast<uint32_t>(NLS::Render::RHI::AccessMask::ShaderWrite) |
					static_cast<uint32_t>(NLS::Render::RHI::AccessMask::ColorAttachmentWrite) |
					static_cast<uint32_t>(NLS::Render::RHI::AccessMask::DepthStencilWrite) |
					static_cast<uint32_t>(NLS::Render::RHI::AccessMask::HostWrite) |
					static_cast<uint32_t>(NLS::Render::RHI::AccessMask::MemoryWrite);
				return (static_cast<uint32_t>(accessMask) & kWriteAccessMask) != 0u;
			};

			auto resolvedBarriers = frameContext->resourceStateTracker->BuildTransitionBarriers(
				requestedBarriers.bufferBarriers,
				requestedBarriers.textureBarriers);
			std::erase_if(
				resolvedBarriers.bufferBarriers,
				[&](const NLS::Render::RHI::RHIBufferBarrier& barrier)
				{
					return barrier.buffer == nullptr ||
						(barrier.before == barrier.after &&
						 !hasWriteAccess(barrier.sourceAccessMask) &&
						 !hasWriteAccess(barrier.destinationAccessMask));
				});
			std::erase_if(
				resolvedBarriers.textureBarriers,
				[&](const NLS::Render::RHI::RHITextureBarrier& barrier)
				{
					return barrier.texture == nullptr ||
						(barrier.before == barrier.after &&
						 !hasWriteAccess(barrier.sourceAccessMask) &&
						 !hasWriteAccess(barrier.destinationAccessMask));
				});
			if (resolvedBarriers.bufferBarriers.empty() && resolvedBarriers.textureBarriers.empty())
				return;

			commandBuffer->Barrier(resolvedBarriers);
			frameContext->resourceStateTracker->Commit(resolvedBarriers);
		}
	};
}
