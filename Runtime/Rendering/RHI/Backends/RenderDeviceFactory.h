#pragma once

#include <memory>

#include "RenderDef.h"
#include "Rendering/Settings/EGraphicsBackend.h"

namespace NLS::Render::RHI
{
	class IRenderDevice;
}

namespace NLS::Render::Backend
{
	NLS_RENDER_API std::unique_ptr<NLS::Render::RHI::IRenderDevice> CreateRenderDevice(NLS::Render::Settings::EGraphicsBackend backend);
}
