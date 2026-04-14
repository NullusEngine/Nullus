#pragma once

#include <memory>
#include <string>

#include "RenderDef.h"
#include "Rendering/RHI/RHITypes.h"

struct VkInstance_T;
struct VkPhysicalDevice_T;
struct VkDevice_T;
struct VkQueue_T;
struct VkSurfaceKHR_T;
struct VkSwapchainKHR_T;
struct VkBuffer_T;
struct VkImage_T;
struct VkImageView_T;
struct VkSampler_T;

namespace NLS::Render::RHI
{
	class RHIDevice;
}

namespace NLS::Render::Backend
{
	// Typed NativeHandle variants for Vulkan backend
	// These provide type safety by preventing implicit conversions between different handle types
	struct VulkanBufferHandle
	{
		VkBuffer_T* buffer = nullptr;
	};

	struct VulkanImageHandle
	{
		VkImage_T* image = nullptr;
	};

	struct VulkanImageViewHandle
	{
		VkImageView_T* imageView = nullptr;
	};

	struct VulkanSamplerHandle
	{
		VkSampler_T* sampler = nullptr;
	};

	NLS_RENDER_API std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateNativeVulkanExplicitDevice(
		VkInstance_T* instance,
		VkPhysicalDevice_T* physicalDevice,
		VkDevice_T* device,
		VkQueue_T* graphicsQueue,
		VkSurfaceKHR_T* surface,
		VkSwapchainKHR_T* swapchain,
		uint32_t graphicsQueueFamilyIndex,
		const NLS::Render::RHI::RHIDeviceCapabilities& capabilities,
		const std::string& vendor,
		const std::string& hardware,
		bool dynamicRenderingEnabled = false);

	// Direct creation - creates Vulkan Tier A device without IRenderDevice
	NLS_RENDER_API std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateVulkanRhiDevice(void* platformWindow);
}