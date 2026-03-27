#include "Rendering/RHI/Backends/ExplicitDeviceFactory.h"

#include "Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.h"
#include "Rendering/RHI/Backends/OpenGL/Compat/ExplicitRHICompat.h"
#include "Rendering/RHI/Backends/Vulkan/VulkanExplicitDeviceFactory.h"
#include "Rendering/RHI/IRenderDevice.h"

namespace NLS::Render::Backend
{
	std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateExplicitDevice(NLS::Render::RHI::IRenderDevice& renderDevice)
	{
		switch (renderDevice.GetNativeDeviceInfo().backend)
		{
		case NLS::Render::RHI::NativeBackendType::DX12:
			return CreateDX12ExplicitDevice(renderDevice);
		case NLS::Render::RHI::NativeBackendType::Vulkan:
			return CreateVulkanExplicitDevice(renderDevice);
		case NLS::Render::RHI::NativeBackendType::OpenGL:
		case NLS::Render::RHI::NativeBackendType::Metal:
		case NLS::Render::RHI::NativeBackendType::None:
		default:
			return NLS::Render::RHI::CreateCompatibilityExplicitDevice(renderDevice);
		}
	}
}
