#pragma once

#include "Rendering/RHI/Backends/Null/NullRenderDevice.h"

namespace NLS::Render::Backend
{
	class NLS_RENDER_API DX11RenderDevice final : public NullRenderDevice
	{
	public:
		DX11RenderDevice();
	};
}
