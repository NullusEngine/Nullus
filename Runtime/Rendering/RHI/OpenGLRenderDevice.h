#pragma once

// Legacy compatibility header. New code should use Rendering/Backend/OpenGL/OpenGLRenderDevice.h.

#include "Rendering/Backend/OpenGL/OpenGLRenderDevice.h"

namespace NLS::Render::RHI
{
	using OpenGLRenderDevice = NLS::Render::Backend::OpenGLRenderDevice;
}
