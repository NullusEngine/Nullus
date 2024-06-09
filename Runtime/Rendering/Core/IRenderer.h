#pragma once

#include "Rendering/Data/FrameDescriptor.h"

namespace NLS::Rendering::Core
{
	class IRenderer
	{
	public:
		virtual void BeginFrame(const Data::FrameDescriptor& p_frameDescriptor) = 0;
		virtual void DrawFrame() = 0;
		virtual void EndFrame() = 0;
	};
}