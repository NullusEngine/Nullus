#include "Rendering/RHI/Backends/RenderDeviceFactory.h"

#include <Debug/Logger.h>
#include <memory>

#include "Rendering/RHI/Backends/DX11/DX11RenderDevice.h"
#include "Rendering/RHI/Backends/DX12/DX12RenderDevice.h"
#include "Rendering/RHI/Backends/Metal/MetalRenderDevice.h"
#include "Rendering/RHI/Backends/Null/NullRenderDevice.h"
#include "Rendering/RHI/Backends/OpenGL/OpenGLRenderDevice.h"
#include "Rendering/RHI/Backends/Vulkan/VulkanRenderDevice.h"

namespace NLS::Render::Backend
{
	std::unique_ptr<NLS::Render::RHI::IRenderDevice> CreateRenderDevice(NLS::Render::Settings::EGraphicsBackend backend)
	{
		switch (backend)
		{
		case NLS::Render::Settings::EGraphicsBackend::NONE:
			return std::make_unique<NullRenderDevice>();
		case NLS::Render::Settings::EGraphicsBackend::OPENGL:
			return std::make_unique<OpenGLRenderDevice>();
		case NLS::Render::Settings::EGraphicsBackend::VULKAN:
			return std::make_unique<VulkanRenderDevice>();
		case NLS::Render::Settings::EGraphicsBackend::DX12:
			return std::make_unique<DX12RenderDevice>();
		case NLS::Render::Settings::EGraphicsBackend::DX11:
			return std::make_unique<DX11RenderDevice>();
		case NLS::Render::Settings::EGraphicsBackend::METAL:
			return std::make_unique<MetalRenderDevice>();
		default:
			NLS_LOG_WARNING("Unknown graphics backend requested. Falling back to NullRenderDevice.");
			return std::make_unique<NullRenderDevice>();
		}
	}
}
