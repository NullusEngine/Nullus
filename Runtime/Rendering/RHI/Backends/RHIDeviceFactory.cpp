#include "Rendering/RHI/Backends/RHIDeviceFactory.h"

#if defined(_WIN32)
#include "Rendering/RHI/Backends/DX11/DX11ExplicitDeviceFactory.h"
#include "Rendering/RHI/Backends/DX11/DX11CommandListExecutor.h"
#include "Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.h"
#include "Rendering/RHI/Backends/DX12/DX12CommandListExecutor.h"
#endif
#include "Rendering/RHI/Backends/OpenGL/OpenGLExplicitDeviceFactory.h"
#include "Rendering/RHI/Backends/OpenGL/OpenGLCommandListExecutor.h"
#if NLS_HAS_VULKAN
#include "Rendering/RHI/Backends/Vulkan/VulkanExplicitDeviceFactory.h"
#include "Rendering/RHI/Backends/Vulkan/VulkanCommandListExecutor.h"
#endif
#if NLS_HAS_METAL
#include "Rendering/RHI/Backends/Metal/MetalExplicitDeviceFactory.h"
#include "Rendering/RHI/Backends/Metal/MetalCommandListExecutor.h"
#endif
#include "Rendering/RHI/Core/RHICommandListExecutor.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Debug/Logger.h"

#ifdef _WIN32
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>
#endif

#if defined(_WIN32)
#include <dxgi1_6.h>
#undef CreateSemaphore
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#endif

#if NLS_HAS_VULKAN
#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR
#elif defined(__linux__)
#define VK_USE_PLATFORM_XLIB_KHR
#endif
#include <vulkan/vulkan.h>
#if defined(_WIN32)
#include <vulkan/vulkan_win32.h>
#elif defined(__linux__)
#include <X11/Xlib.h>
#include <vulkan/vulkan_xlib.h>
#endif
#endif

namespace NLS::Render::Backend
{
#if defined(_WIN32)
	using Microsoft::WRL::ComPtr;
#endif

	// Forward declarations
	std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateDX12Device(const NLS::Render::Settings::DriverSettings& settings);
	std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateVulkanDevice(const NLS::Render::Settings::DriverSettings& settings);

	std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateRhiDevice(const NLS::Render::Settings::DriverSettings& settings)
	{
		NLS_LOG_INFO(std::string("CreateRhiDevice: requested backend = ") + Render::Settings::ToString(settings.graphicsBackend));
		if (settings.graphicsBackend == NLS::Render::Settings::EGraphicsBackend::NONE)
		{
			NLS_LOG_WARNING("CreateRhiDevice: None backend requested; no runtime RHI device will be created.");
			return nullptr;
		}

		if (!Render::Settings::IsBackendEnabledForCurrentBuild(settings.graphicsBackend))
		{
			NLS_LOG_WARNING(
				std::string("CreateRhiDevice: ") +
				Render::Settings::ToString(settings.graphicsBackend) +
				" is gated unsupported in the current runtime validation matrix.");
			return nullptr;
		}

#if defined(_WIN32)
		// For DX11, create Tier A device directly without IRenderDevice
		if (settings.graphicsBackend == NLS::Render::Settings::EGraphicsBackend::DX11)
		{
			// DX11 can create device directly - pass nullptr for window, will be set later via CreateSwapchain
			return CreateDX11RhiDevice(nullptr, 1920, 1080);
		}
#endif

		// For OpenGL, create Tier A device directly without IRenderDevice
		if (settings.graphicsBackend == NLS::Render::Settings::EGraphicsBackend::OPENGL)
		{
			// OpenGL can create device directly - pass nullptr for window, GLAD init deferred to CreateSwapchain
			return CreateOpenGLRhiDevice(nullptr);
		}

		switch (settings.graphicsBackend)
		{
#if defined(_WIN32)
		case NLS::Render::Settings::EGraphicsBackend::DX12:
			return CreateDX12Device(settings);
#endif
		case NLS::Render::Settings::EGraphicsBackend::VULKAN:
			return CreateVulkanDevice(settings);
		case NLS::Render::Settings::EGraphicsBackend::METAL:
			NLS_LOG_WARNING("CreateRhiDevice: Metal backend is unsupported on this platform/build.");
			return nullptr;
		default:
			NLS_LOG_WARNING(
				std::string("CreateRhiDevice: Unsupported backend requested: ") +
				Render::Settings::ToString(settings.graphicsBackend));
			return nullptr;
		}
	}

#if defined(_WIN32)
	std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateDX12Device(const NLS::Render::Settings::DriverSettings& settings)
	{
		return CreateDX12RhiDevice(settings.debugMode);
	}
#endif

	std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateVulkanDevice(const NLS::Render::Settings::DriverSettings& settings)
	{
#if NLS_HAS_VULKAN
		VkApplicationInfo appInfo = {};
		appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		appInfo.pApplicationName = "Nullus";
		appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.pEngineName = "Nullus Engine";
		appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.apiVersion = VK_API_VERSION_1_0;

		// Enable required extensions
		std::vector<const char*> extensions;
		extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#if defined(_WIN32)
		extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif defined(__linux__)
		extensions.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
#endif

		VkInstanceCreateInfo instanceInfo = {};
		instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		instanceInfo.pApplicationInfo = &appInfo;
		instanceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
		instanceInfo.ppEnabledExtensionNames = extensions.data();

		VkInstance instance = VK_NULL_HANDLE;
		VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance);
		if (result != VK_SUCCESS)
		{
			NLS_LOG_WARNING("CreateVulkanDevice: Failed to create VkInstance");
			return nullptr;
		}

		// Enumerate physical devices
		uint32_t deviceCount = 0;
		result = vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
		if (result != VK_SUCCESS || deviceCount == 0)
		{
			NLS_LOG_WARNING("CreateVulkanDevice: No Vulkan devices found");
			vkDestroyInstance(instance, nullptr);
			return nullptr;
		}

		std::vector<VkPhysicalDevice> devices(deviceCount);
		result = vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
		if (result != VK_SUCCESS)
		{
			NLS_LOG_WARNING("CreateVulkanDevice: Failed to enumerate devices");
			vkDestroyInstance(instance, nullptr);
			return nullptr;
		}

		// Use first discrete GPU if available, otherwise first device
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		VkPhysicalDeviceProperties deviceProperties;
		for (VkPhysicalDevice dev : devices)
		{
			vkGetPhysicalDeviceProperties(dev, &deviceProperties);
			if (deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
			{
				physicalDevice = dev;
				break;
			}
			if (physicalDevice == VK_NULL_HANDLE)
				physicalDevice = dev;
		}

		if (physicalDevice == VK_NULL_HANDLE)
		{
			NLS_LOG_WARNING("CreateVulkanDevice: No suitable device found");
			vkDestroyInstance(instance, nullptr);
			return nullptr;
		}

		// Get device properties for vendor info
		vkGetPhysicalDeviceProperties(physicalDevice, &deviceProperties);
		std::string vendor, hardware;
		switch (deviceProperties.vendorID)
		{
		case 0x10DE: vendor = "NVIDIA"; break;
		case 0x1002: vendor = "AMD"; break;
		case 0x8086: vendor = "Intel"; break;
		default: vendor = "Unknown"; break;
		}
		hardware = deviceProperties.deviceName;

		// Find graphics queue family
		uint32_t queueFamilyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
		std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

		int32_t graphicsQueueFamily = -1;
		for (uint32_t i = 0; i < queueFamilyCount; i++)
		{
			if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
			{
				graphicsQueueFamily = static_cast<int32_t>(i);
				break;
			}
		}

		if (graphicsQueueFamily < 0)
		{
			NLS_LOG_WARNING("CreateVulkanDevice: No graphics queue family found");
			vkDestroyInstance(instance, nullptr);
			return nullptr;
		}

		// Create device
		float queuePriority = 1.0f;
		VkDeviceQueueCreateInfo queueInfo = {};
		queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueInfo.queueFamilyIndex = static_cast<uint32_t>(graphicsQueueFamily);
		queueInfo.queueCount = 1;
		queueInfo.pQueuePriorities = &queuePriority;

		VkPhysicalDeviceFeatures deviceFeatures = {};
		const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
		VkDeviceCreateInfo deviceInfo = {};
		deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		deviceInfo.queueCreateInfoCount = 1;
		deviceInfo.pQueueCreateInfos = &queueInfo;
		deviceInfo.pEnabledFeatures = &deviceFeatures;
		deviceInfo.enabledExtensionCount = 1;
		deviceInfo.ppEnabledExtensionNames = deviceExtensions;

		VkDevice device = VK_NULL_HANDLE;
		result = vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device);
		if (result != VK_SUCCESS)
		{
			NLS_LOG_WARNING("CreateVulkanDevice: Failed to create VkDevice");
			vkDestroyInstance(instance, nullptr);
			return nullptr;
		}

		// Get graphics queue
		VkQueue graphicsQueue = VK_NULL_HANDLE;
		vkGetDeviceQueue(device, static_cast<uint32_t>(graphicsQueueFamily), 0, &graphicsQueue);

		// Build capabilities
		NLS::Render::RHI::RHIDeviceCapabilities capabilities = {};
		capabilities.supportsFramebufferReadback = true;
		capabilities.supportsEditorPickingReadback = true;
		capabilities.supportsExplicitBarriers = true;
		capabilities.backendReady = true;
		capabilities.supportsSwapchain = true;
		capabilities.supportsCurrentSceneRenderer = true;
		capabilities.supportsOffscreenFramebuffers = true;
		capabilities.supportsUITextureHandles = true;
		capabilities.supportsDepthBlit = true;
		capabilities.supportsCubemaps = true;

		return CreateNativeVulkanExplicitDevice(
			instance,
			physicalDevice,
			device,
			graphicsQueue,
			nullptr,  // surface - created later with window
			nullptr,  // swapchain - created later
			static_cast<uint32_t>(graphicsQueueFamily),
			capabilities,
			vendor,
			hardware);
#else
		NLS_LOG_WARNING("CreateVulkanDevice: Vulkan not available on this platform");
		return nullptr;
#endif
	}
}

namespace NLS::Render::RHI
{
	std::unique_ptr<IRHICommandListExecutor> CreateCommandListExecutor(ERHIBackend backend, const NativeRenderDeviceInfo& nativeInfo)
	{
		switch (backend)
		{
#ifdef _WIN32
		case ERHIBackend::DX11:
		{
			auto device = reinterpret_cast<ID3D11Device*>(nativeInfo.device);
			auto context = reinterpret_cast<ID3D11DeviceContext*>(nativeInfo.graphicsQueue);
			return std::make_unique<DX11::DX11CommandListExecutor>(device, context);
		}
		case ERHIBackend::DX12:
		{
			auto device = reinterpret_cast<ID3D12Device*>(nativeInfo.device);
			auto commandQueue = reinterpret_cast<ID3D12CommandQueue*>(nativeInfo.graphicsQueue);
			return std::make_unique<DX12::DX12CommandListExecutor>(device, commandQueue);
		}
#endif
		case ERHIBackend::Vulkan:
		{
#if NLS_HAS_VULKAN
			auto device = reinterpret_cast<VkDevice>(nativeInfo.device);
			auto graphicsQueue = reinterpret_cast<VkQueue>(nativeInfo.graphicsQueue);

			if (device == VK_NULL_HANDLE || graphicsQueue == VK_NULL_HANDLE)
			{
				NLS_LOG_ERROR("CreateCommandListExecutor: Vulkan device or queue is null");
				return nullptr;
			}

			// Create command pool for the executor
			VkCommandPoolCreateInfo poolInfo = {};
			poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
			poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
			poolInfo.queueFamilyIndex = nativeInfo.graphicsQueueFamilyIndex;

			VkCommandPool commandPool = VK_NULL_HANDLE;
			VkResult result = vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool);
			if (result != VK_SUCCESS)
			{
				NLS_LOG_ERROR("CreateCommandListExecutor: Failed to create Vulkan command pool");
				return nullptr;
			}

			return std::make_unique<Vulkan::VulkanCommandListExecutor>(device, commandPool, graphicsQueue);
#else
			NLS_LOG_WARNING("CreateCommandListExecutor: Vulkan not available on this platform");
			return nullptr;
#endif
		}
		case ERHIBackend::OpenGL:
		{
			return std::make_unique<OpenGL::OpenGLCommandListExecutor>();
		}
		case ERHIBackend::Metal:
		{
#if NLS_HAS_METAL
			return std::make_unique<Metal::MetalCommandListExecutor>();
#else
			NLS_LOG_WARNING("CreateCommandListExecutor: Metal not available on this platform");
			return nullptr;
#endif
		}
		case ERHIBackend::Null:
		default:
			return nullptr;
		}
	}
}
