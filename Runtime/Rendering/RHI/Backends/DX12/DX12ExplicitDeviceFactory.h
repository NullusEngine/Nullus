#pragma once

#include <memory>

#include "RenderDef.h"

namespace NLS::Render::RHI
{
	class IRenderDevice;
	class RHIDevice;
}

namespace NLS::Render::Backend
{
	NLS_RENDER_API std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateDX12ExplicitDevice(NLS::Render::RHI::IRenderDevice& renderDevice);
}
