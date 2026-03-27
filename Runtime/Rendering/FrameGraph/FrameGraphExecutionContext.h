#pragma once

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHIDevice.h"

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
	};
}
