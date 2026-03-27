#pragma once

// Legacy compatibility header. New code should use Rendering/RHI/Backends/Null/NullRenderDevice.h.

#include "Rendering/RHI/Backends/Null/NullRenderDevice.h"

namespace NLS::Render::RHI
{
	using NullRenderDevice = NLS::Render::Backend::NullRenderDevice;
}
