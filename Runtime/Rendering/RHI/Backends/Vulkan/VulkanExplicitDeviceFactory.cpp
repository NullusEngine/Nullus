#include "Rendering/RHI/Backends/Vulkan/VulkanExplicitDeviceFactory.h"

#include <atomic>
#include <array>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <Debug/Logger.h>
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHISync.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/RHI/BindingPointMap.h"

#if NLS_HAS_VULKAN
#include <vulkan/vulkan.h>
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#endif

namespace NLS::Render::Backend
{

#if NLS_HAS_VULKAN
	namespace
	{
#if defined(VK_EXT_debug_utils)
        VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDebugCallback(
            VkDebugUtilsMessageSeverityFlagBitsEXT severity,
            VkDebugUtilsMessageTypeFlagsEXT,
            const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
            void*)
        {
            if (callbackData == nullptr || callbackData->pMessage == nullptr)
                return VK_FALSE;

            const char* prefix = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0
                ? "[Vulkan][Validation][Error] "
                : (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0
                    ? "[Vulkan][Validation][Warning] "
                    : "[Vulkan][Validation] ";
            if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
                NLS_LOG_ERROR(std::string(prefix) + callbackData->pMessage);
            else
                NLS_LOG_WARNING(std::string(prefix) + callbackData->pMessage);
            return VK_FALSE;
        }

        VkResult CreateVulkanDebugMessenger(
            VkInstance instance,
            const VkDebugUtilsMessengerCreateInfoEXT& createInfo,
            VkDebugUtilsMessengerEXT* messenger)
        {
            auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            return create != nullptr ? create(instance, &createInfo, nullptr, messenger) : VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        void DestroyVulkanDebugMessenger(VkInstance instance, VkDebugUtilsMessengerEXT messenger)
        {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy != nullptr && messenger != VK_NULL_HANDLE)
                destroy(instance, messenger, nullptr);
        }
#else
        // Older generated Vulkan headers (for example GLFW's bundled glad
        // header) may omit VK_EXT_debug_utils. Keep the backend buildable and
        // disable only the optional callback in that case.
        using VulkanDebugMessengerHandle = VkDebugReportCallbackEXT;
        void DestroyVulkanDebugMessenger(VkInstance, VulkanDebugMessengerHandle) {}
#endif

#if defined(VK_EXT_debug_utils)
        using VulkanDebugMessengerHandle = VkDebugUtilsMessengerEXT;
#endif

        VkCompareOp ToVkCompareOp(NLS::Render::Settings::EComparaisonAlgorithm algorithm)
        {
            switch (algorithm)
            {
            case NLS::Render::Settings::EComparaisonAlgorithm::NEVER: return VK_COMPARE_OP_NEVER;
            case NLS::Render::Settings::EComparaisonAlgorithm::LESS: return VK_COMPARE_OP_LESS;
            case NLS::Render::Settings::EComparaisonAlgorithm::EQUAL: return VK_COMPARE_OP_EQUAL;
            case NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
            case NLS::Render::Settings::EComparaisonAlgorithm::GREATER: return VK_COMPARE_OP_GREATER;
            case NLS::Render::Settings::EComparaisonAlgorithm::NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
            case NLS::Render::Settings::EComparaisonAlgorithm::GREATER_EQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
            case NLS::Render::Settings::EComparaisonAlgorithm::ALWAYS:
            default:
                return VK_COMPARE_OP_ALWAYS;
            }
        }

        VkStencilOp ToVkStencilOp(NLS::Render::Settings::EOperation operation)
        {
            switch (operation)
            {
            case NLS::Render::Settings::EOperation::KEEP: return VK_STENCIL_OP_KEEP;
            case NLS::Render::Settings::EOperation::ZERO: return VK_STENCIL_OP_ZERO;
            case NLS::Render::Settings::EOperation::REPLACE: return VK_STENCIL_OP_REPLACE;
            case NLS::Render::Settings::EOperation::INCREMENT: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
            case NLS::Render::Settings::EOperation::INCREMENT_WRAP: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
            case NLS::Render::Settings::EOperation::DECREMENT: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
            case NLS::Render::Settings::EOperation::DECREMENT_WRAP: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
            case NLS::Render::Settings::EOperation::INVERT: return VK_STENCIL_OP_INVERT;
            default: return VK_STENCIL_OP_KEEP;
            }
        }

        // The RHI uses a stable canonical set order while DXC emits SPIR-V
        // descriptor sets using the original HLSL register space. Vulkan
        // translates between those two conventions at the command boundary.
        uint32_t ToVulkanDescriptorSetIndex(const uint32_t rhiSetIndex)
        {
            switch (rhiSetIndex)
            {
            case NLS::Render::RHI::BindingPointMap::kFrameDescriptorSet: return NLS::Render::RHI::BindingPointMap::kFrameBindingSpace;
            case NLS::Render::RHI::BindingPointMap::kMaterialDescriptorSet: return NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace;
            case NLS::Render::RHI::BindingPointMap::kObjectDescriptorSet: return NLS::Render::RHI::BindingPointMap::kObjectBindingSpace;
            case NLS::Render::RHI::BindingPointMap::kPassDescriptorSet: return NLS::Render::RHI::BindingPointMap::kPassBindingSpace;
            default: return rhiSetIndex;
            }
        }

        // DXC's Vulkan register shifts use the same stable ranges for all
        // shaders. Keep the public RHI binding coordinates in HLSL terms and
        // translate only when constructing Vulkan descriptor layouts/updates.
        uint32_t ToVulkanDescriptorBinding(
            const NLS::Render::RHI::BindingType type,
            const uint32_t registerSpace,
            const uint32_t binding)
        {
            switch (type)
            {
            case NLS::Render::RHI::BindingType::UniformBuffer:
                return 8u + registerSpace * 4u + binding;
			case NLS::Render::RHI::BindingType::StructuredBuffer:
			case NLS::Render::RHI::BindingType::Texture:
				return registerSpace * 16u + binding;
			case NLS::Render::RHI::BindingType::StorageBuffer:
			case NLS::Render::RHI::BindingType::RWTexture:
				return 128u + registerSpace * 16u + binding;
			case NLS::Render::RHI::BindingType::Sampler:
				return (registerSpace == 0u ? 0u : 64u + registerSpace * 16u) + binding;
            default:
                return binding;
            }
        }

        struct VulkanDescriptorBindingSignature final
        {
            uint32_t binding = 0u;
            VkDescriptorType descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            uint32_t descriptorCount = 1u;
            VkShaderStageFlags stageFlags = 0u;

            bool operator==(const VulkanDescriptorBindingSignature& other) const
            {
                return binding == other.binding &&
                    descriptorType == other.descriptorType &&
                    descriptorCount == other.descriptorCount &&
                    stageFlags == other.stageFlags;
            }
        };

        using VulkanDescriptorLayoutSignature = std::vector<VulkanDescriptorBindingSignature>;

        VmaAllocator CreateVulkanMemoryAllocator(
            VkInstance instance,
            VkPhysicalDevice physicalDevice,
            VkDevice device)
        {
            if (instance == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE)
                return VK_NULL_HANDLE;

            VmaAllocatorCreateInfo allocatorInfo{};
            allocatorInfo.instance = instance;
            allocatorInfo.physicalDevice = physicalDevice;
            allocatorInfo.device = device;
            allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_1;

            VmaAllocator allocator = VK_NULL_HANDLE;
            const VkResult result = vmaCreateAllocator(&allocatorInfo, &allocator);
            if (result != VK_SUCCESS)
            {
                NLS_LOG_ERROR(
                    "CreateVulkanMemoryAllocator: vmaCreateAllocator failed with VkResult=" +
                    std::to_string(result));
                return VK_NULL_HANDLE;
            }
            return allocator;
        }

        VmaMemoryUsage ToVmaMemoryUsage(NLS::Render::RHI::MemoryUsage usage)
        {
            switch (usage)
            {
            case NLS::Render::RHI::MemoryUsage::CPUToGPU: return VMA_MEMORY_USAGE_CPU_TO_GPU;
            case NLS::Render::RHI::MemoryUsage::GPUToCPU: return VMA_MEMORY_USAGE_GPU_TO_CPU;
            case NLS::Render::RHI::MemoryUsage::GPUOnly:
            default:
                return VMA_MEMORY_USAGE_GPU_ONLY;
            }
        }
		class NativeVulkanFence final : public NLS::Render::RHI::RHIFence
		{
		public:
			NativeVulkanFence(VkDevice device, VkFence fence, const std::string& debugName)
				: m_device(device)
				, m_fence(fence)
				, m_debugName(debugName)
			{
			}
			~NativeVulkanFence()
			{
#if NLS_HAS_VULKAN
				if (m_device != VK_NULL_HANDLE && m_fence != VK_NULL_HANDLE)
					vkDestroyFence(m_device, m_fence, nullptr);
#endif
			}

			std::string_view GetDebugName() const override { return m_debugName; }
			bool IsSignaled() const override
			{
#if NLS_HAS_VULKAN
				if (m_device == nullptr || m_fence == nullptr)
					return false;
				VkResult result = vkGetFenceStatus(m_device, m_fence);
				return result == VK_SUCCESS;
#else
				return false;
#endif
			}
			void Reset() override
			{
#if NLS_HAS_VULKAN
				if (m_device != nullptr && m_fence != nullptr)
					vkResetFences(m_device, 1, &m_fence);
#endif
			}
			bool Wait(uint64_t timeoutNanoseconds) override
			{
#if NLS_HAS_VULKAN
				if (m_device == nullptr || m_fence == nullptr)
					return false;
				VkResult result = vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, timeoutNanoseconds);
				return result == VK_SUCCESS;
#else
				return false;
#endif
			}

#if NLS_HAS_VULKAN
			VkFence GetFence() const { return m_fence; }
#endif

		private:
			std::string m_debugName;
#if NLS_HAS_VULKAN
			VkDevice m_device = nullptr;
			VkFence m_fence = nullptr;
#endif
		};

		class NativeVulkanSemaphore final : public NLS::Render::RHI::RHISemaphore
		{
		public:
			NativeVulkanSemaphore(VkDevice device, VkSemaphore semaphore, const std::string& debugName)
				: m_device(device)
				, m_semaphore(semaphore)
				, m_debugName(debugName)
			{
			}
			~NativeVulkanSemaphore()
			{
#if NLS_HAS_VULKAN
				if (m_device != VK_NULL_HANDLE && m_semaphore != VK_NULL_HANDLE)
					vkDestroySemaphore(m_device, m_semaphore, nullptr);
#endif
			}

			std::string_view GetDebugName() const override { return m_debugName; }
			bool IsSignaled() const override
			{
#if NLS_HAS_VULKAN
				if (m_device == nullptr || m_semaphore == nullptr)
					return false;
				// Binary semaphores can't be queried - use fence instead for status
				return false;
#else
				return false;
#endif
			}
			void Reset() override
			{
#if NLS_HAS_VULKAN
				// Vulkan semaphores cannot be reset - they are binary
#endif
			}

#if NLS_HAS_VULKAN
			bool Recreate()
			{
				if (m_device == VK_NULL_HANDLE)
					return false;
				if (m_semaphore != VK_NULL_HANDLE)
					vkDestroySemaphore(m_device, m_semaphore, nullptr);
				VkSemaphoreCreateInfo semaphoreInfo{};
				semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
				m_semaphore = VK_NULL_HANDLE;
				return vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_semaphore) == VK_SUCCESS;
			}
#endif

			NLS::Render::RHI::NativeHandle GetNativeSemaphoreHandle() override
			{
				return { NLS::Render::RHI::BackendType::Vulkan, reinterpret_cast<void*>(m_semaphore) };
			}

#if NLS_HAS_VULKAN
			VkSemaphore GetSemaphore() const { return m_semaphore; }
#endif

		private:
			std::string m_debugName;
#if NLS_HAS_VULKAN
			VkDevice m_device = nullptr;
			VkSemaphore m_semaphore = nullptr;
#endif
		};

		class NativeVulkanAdapter final : public NLS::Render::RHI::RHIAdapter
		{
		public:
			NativeVulkanAdapter(const std::string& vendor, const std::string& hardware)
				: m_vendor(vendor)
				, m_hardware(hardware)
			{
			}

			std::string_view GetDebugName() const override { return "NativeVulkanAdapter"; }
			NLS::Render::RHI::NativeBackendType GetBackendType() const override { return NLS::Render::RHI::NativeBackendType::Vulkan; }
			std::string_view GetVendor() const override { return m_vendor; }
			std::string_view GetHardware() const override { return m_hardware; }

		private:
			std::string m_vendor;
			std::string m_hardware;
		};

		class NativeVulkanCommandBuffer;
		class NativeVulkanBindingSet;
		class NativeVulkanPipelineLayout;
		class NativeVulkanGraphicsPipeline;
		class NativeVulkanComputePipeline;
		class NativeVulkanSwapchain;
		class NativeVulkanTexture;

#if NLS_HAS_VULKAN
		VkFormat ResolveVulkanAttachmentFormat(const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture);
		void SetVulkanTextureLogicalState(
			const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
			NLS::Render::RHI::ResourceState state);
#endif

		class NativeVulkanQueue final : public NLS::Render::RHI::RHIQueue
		{
		public:
			NativeVulkanQueue(VkDevice device, VkQueue queue, const std::string& debugName)
				: m_device(device)
				, m_queue(queue)
				, m_debugName(debugName)
			{
			}

			std::string_view GetDebugName() const override { return m_debugName; }
			NLS::Render::RHI::QueueType GetType() const override { return NLS::Render::RHI::QueueType::Graphics; }
			void Submit(const NLS::Render::RHI::RHISubmitDesc& submitDesc) override;
			NLS::Render::RHI::RHIQueueOperationResult SubmitChecked(const NLS::Render::RHI::RHISubmitDesc& submitDesc) override;
			void Present(const NLS::Render::RHI::RHIPresentDesc& presentDesc) override;
			NLS::Render::RHI::RHIQueueOperationResult PresentChecked(const NLS::Render::RHI::RHIPresentDesc& presentDesc) override;

#if NLS_HAS_VULKAN
			VkQueue GetQueue() const { return m_queue; }
#endif

		private:
			std::mutex m_queueMutex;
			VkDevice m_device = nullptr;
			VkQueue m_queue = nullptr;
			std::string m_debugName;
		};

#if NLS_HAS_VULKAN
		static uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, VkMemoryRequirements memRequirements, NLS::Render::RHI::MemoryUsage memoryUsage)
		{
			VkPhysicalDeviceMemoryProperties memProperties;
			vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

			VkMemoryPropertyFlags desiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
			switch (memoryUsage)
			{
			case NLS::Render::RHI::MemoryUsage::CPUToGPU:
				desiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
				break;
			case NLS::Render::RHI::MemoryUsage::GPUToCPU:
				desiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
				break;
			case NLS::Render::RHI::MemoryUsage::GPUOnly:
			default:
				desiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
				break;
			}

			for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i)
			{
				if ((memRequirements.memoryTypeBits & (1u << i)) && (memProperties.memoryTypes[i].propertyFlags & desiredFlags) == desiredFlags)
				{
					return i;
				}
			}

			// Fallback: find any compatible memory type
			for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i)
			{
				if (memRequirements.memoryTypeBits & (1u << i))
				{
					return i;
				}
			}

			return 0;
		}

		// Binding sets are allowed to omit optional material resources. Vulkan still
		// requires a descriptor to be written when the shader statically references
		// that binding, so keep one valid, device-local fallback image and sampler
		// per device. The handles intentionally live until process teardown: RHI
		// resource objects can outlive the Driver's device wrapper during shutdown.
		struct VulkanFallbackResources final
		{
			VkImage image = VK_NULL_HANDLE;
			VmaAllocation allocation = VK_NULL_HANDLE;
			VkImageView view = VK_NULL_HANDLE;
			VkImageView cubeView = VK_NULL_HANDLE;
			VkSampler sampler = VK_NULL_HANDLE;
		};

        std::mutex g_vulkanFallbackResourcesMutex;
        std::unordered_map<VkDevice, VulkanFallbackResources> g_vulkanFallbackResources;

        // Command buffers are short-lived and are reset between frame slots. Keep
        // the last known layout per image at device scope so a newly-recorded
        // command buffer can use the real Vulkan oldLayout instead of assuming the
        // RHI's logical state is still the driver's current layout.
        struct VulkanImageLayoutKey final
        {
            VkImage image = VK_NULL_HANDLE;
            uint32_t mipLevel = std::numeric_limits<uint32_t>::max();
            uint32_t arrayLayer = std::numeric_limits<uint32_t>::max();

            bool operator==(const VulkanImageLayoutKey& other) const
            {
                return image == other.image &&
                    mipLevel == other.mipLevel &&
                    arrayLayer == other.arrayLayer;
            }
        };

        struct VulkanImageLayoutKeyHash final
        {
            size_t operator()(const VulkanImageLayoutKey& key) const
            {
                size_t hash = std::hash<VkImage>{}(key.image);
                hash ^= std::hash<uint32_t>{}(key.mipLevel) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
                hash ^= std::hash<uint32_t>{}(key.arrayLayer) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
                return hash;
            }
        };

        constexpr uint32_t kWholeImageSubresource = std::numeric_limits<uint32_t>::max();
        std::mutex g_vulkanImageLayoutMutex;
        std::unordered_map<VulkanImageLayoutKey, VkImageLayout, VulkanImageLayoutKeyHash> g_vulkanImageLayouts;

        VkImageLayout GetVulkanImageLayout(
            VkImage image,
            uint32_t mipLevel = kWholeImageSubresource,
            uint32_t arrayLayer = kWholeImageSubresource)
        {
            std::lock_guard lock(g_vulkanImageLayoutMutex);
            const auto exact = g_vulkanImageLayouts.find({ image, mipLevel, arrayLayer });
            if (exact != g_vulkanImageLayouts.end())
                return exact->second;
            if (mipLevel != kWholeImageSubresource || arrayLayer != kWholeImageSubresource)
            {
                const auto whole = g_vulkanImageLayouts.find({ image, kWholeImageSubresource, kWholeImageSubresource });
                if (whole != g_vulkanImageLayouts.end())
                    return whole->second;
            }
            return VK_IMAGE_LAYOUT_UNDEFINED;
        }

        bool HasVulkanImageLayout(
            VkImage image,
            uint32_t mipLevel = kWholeImageSubresource,
            uint32_t arrayLayer = kWholeImageSubresource)
        {
            std::lock_guard lock(g_vulkanImageLayoutMutex);
            if (g_vulkanImageLayouts.find({ image, mipLevel, arrayLayer }) != g_vulkanImageLayouts.end())
                return true;
            return (mipLevel != kWholeImageSubresource || arrayLayer != kWholeImageSubresource) &&
                g_vulkanImageLayouts.find({ image, kWholeImageSubresource, kWholeImageSubresource }) != g_vulkanImageLayouts.end();
        }

        void SetVulkanImageLayout(
            VkImage image,
            VkImageLayout layout,
            uint32_t mipLevel = kWholeImageSubresource,
            uint32_t arrayLayer = kWholeImageSubresource)
        {
            if (image == VK_NULL_HANDLE || layout == VK_IMAGE_LAYOUT_UNDEFINED || layout == VK_IMAGE_LAYOUT_PREINITIALIZED)
                return;
            std::lock_guard lock(g_vulkanImageLayoutMutex);
            g_vulkanImageLayouts[{ image, mipLevel, arrayLayer }] = layout;
        }

		bool EnsureVulkanFallbackResources(
			VkDevice device,
			VmaAllocator allocator,
			VkQueue queue,
			uint32_t queueFamilyIndex)
		{
			if (device == VK_NULL_HANDLE || allocator == VK_NULL_HANDLE || queue == VK_NULL_HANDLE)
				return false;

			std::lock_guard lock(g_vulkanFallbackResourcesMutex);
			auto& fallback = g_vulkanFallbackResources[device];
			if (fallback.view != VK_NULL_HANDLE && fallback.cubeView != VK_NULL_HANDLE && fallback.sampler != VK_NULL_HANDLE)
				return true;

			VkImageCreateInfo imageInfo{};
			imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
			imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
			imageInfo.imageType = VK_IMAGE_TYPE_2D;
			imageInfo.extent = { 1u, 1u, 1u };
			imageInfo.mipLevels = 1u;
			imageInfo.arrayLayers = 6u;
			imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
			imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
			imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
			imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VmaAllocationCreateInfo allocationInfo{};
			allocationInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
			if (vmaCreateImage(allocator, &imageInfo, &allocationInfo, &fallback.image, &fallback.allocation, nullptr) != VK_SUCCESS)
			{
				if (fallback.image != VK_NULL_HANDLE)
					vkDestroyImage(device, fallback.image, nullptr);
				fallback = {};
				return false;
			}

			VkImageViewCreateInfo viewInfo{};
			viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			viewInfo.image = fallback.image;
			viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
			viewInfo.subresourceRange = {
				VK_IMAGE_ASPECT_COLOR_BIT,
				0u,
				1u,
				0u,
				1u
			};
			if (vkCreateImageView(device, &viewInfo, nullptr, &fallback.view) != VK_SUCCESS)
			{
				vmaDestroyImage(allocator, fallback.image, fallback.allocation);
				fallback = {};
				return false;
			}
			viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
			viewInfo.subresourceRange.layerCount = 6u;
			if (vkCreateImageView(device, &viewInfo, nullptr, &fallback.cubeView) != VK_SUCCESS)
			{
				vkDestroyImageView(device, fallback.view, nullptr);
				vmaDestroyImage(allocator, fallback.image, fallback.allocation);
				fallback = {};
				return false;
			}

			VkSamplerCreateInfo samplerInfo{};
			samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
			samplerInfo.magFilter = VK_FILTER_LINEAR;
			samplerInfo.minFilter = VK_FILTER_LINEAR;
			samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			samplerInfo.maxLod = 1.0f;
			if (vkCreateSampler(device, &samplerInfo, nullptr, &fallback.sampler) != VK_SUCCESS)
			{
				vkDestroyImageView(device, fallback.cubeView, nullptr);
				vkDestroyImageView(device, fallback.view, nullptr);
				vmaDestroyImage(allocator, fallback.image, fallback.allocation);
				fallback = {};
				return false;
			}

			VkCommandPoolCreateInfo poolInfo{};
			poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
			poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
			poolInfo.queueFamilyIndex = queueFamilyIndex;
			VkCommandPool commandPool = VK_NULL_HANDLE;
			VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
			bool transitioned = false;
			if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) == VK_SUCCESS)
			{
				VkCommandBufferAllocateInfo commandAllocateInfo{};
				commandAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
				commandAllocateInfo.commandPool = commandPool;
				commandAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
				commandAllocateInfo.commandBufferCount = 1u;
				if (vkAllocateCommandBuffers(device, &commandAllocateInfo, &commandBuffer) == VK_SUCCESS)
				{
					VkCommandBufferBeginInfo beginInfo{};
					beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
					beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
					if (vkBeginCommandBuffer(commandBuffer, &beginInfo) == VK_SUCCESS)
					{
						VkImageMemoryBarrier barrier{};
						barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
						barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
						barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
						barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
						barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
						barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
						barrier.image = fallback.image;
						barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 6u };
						vkCmdPipelineBarrier(
							commandBuffer,
							VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
							VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
							0u,
							0u,
							nullptr,
							0u,
							nullptr,
							1u,
							&barrier);
						transitioned = vkEndCommandBuffer(commandBuffer) == VK_SUCCESS;
					}
				}
			}
			if (transitioned)
			{
				VkSubmitInfo submitInfo{};
				submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
				submitInfo.commandBufferCount = 1u;
				submitInfo.pCommandBuffers = &commandBuffer;
				transitioned = vkQueueSubmit(queue, 1u, &submitInfo, VK_NULL_HANDLE) == VK_SUCCESS;
				if (transitioned)
					transitioned = vkQueueWaitIdle(queue) == VK_SUCCESS;
			}
			if (commandPool != VK_NULL_HANDLE)
				vkDestroyCommandPool(device, commandPool, nullptr);
			if (!transitioned)
			{
				vkDestroySampler(device, fallback.sampler, nullptr);
				vkDestroyImageView(device, fallback.cubeView, nullptr);
				vkDestroyImageView(device, fallback.view, nullptr);
				vmaDestroyImage(allocator, fallback.image, fallback.allocation);
				fallback = {};
				return false;
			}
			SetVulkanImageLayout(fallback.image, VK_IMAGE_LAYOUT_GENERAL);
			return true;
		}

		VulkanFallbackResources GetVulkanFallbackResources(VkDevice device)
		{
			std::lock_guard lock(g_vulkanFallbackResourcesMutex);
			const auto it = g_vulkanFallbackResources.find(device);
			return it == g_vulkanFallbackResources.end() ? VulkanFallbackResources{} : it->second;
		}
#endif

		class NativeVulkanCommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
		{
		public:
			NativeVulkanCommandBuffer(VkDevice device, VkCommandPool commandPool, const std::string& debugName, bool dynamicRenderingEnabled)
				: m_device(device)
				, m_commandPool(commandPool)
				, m_debugName(debugName)
				, m_dynamicRenderingEnabled(dynamicRenderingEnabled)
			{
#if NLS_HAS_VULKAN
				if (device != nullptr && commandPool != nullptr)
				{
					VkCommandBufferAllocateInfo allocInfo{};
					allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
					allocInfo.commandPool = commandPool;
					allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
					allocInfo.commandBufferCount = 1;

					VkResult result = vkAllocateCommandBuffers(device, &allocInfo, &m_commandBuffer);
					if (result != VK_SUCCESS)
						m_commandBuffer = nullptr;
				}
#endif
			}

			~NativeVulkanCommandBuffer()
			{
#if NLS_HAS_VULKAN
				DestroyActiveRenderPass();
				if (m_device != nullptr && m_commandBuffer != nullptr && m_commandPool != nullptr)
					vkFreeCommandBuffers(m_device, m_commandPool, 1, &m_commandBuffer);
#endif
			}

			std::string_view GetDebugName() const override { return m_debugName; }
			void Begin() override
			{
#if NLS_HAS_VULKAN
				if (m_commandBuffer == nullptr)
					return;
				DestroyActiveRenderPass();
				// Layout tracking is command-buffer local. A frame slot can be reused
				// after another slot submitted transitions for the same image, so stale
				// entries from the previous recording must not override the FrameGraph's
				// current before-state.
				m_imageLayouts.clear();
				VkCommandBufferBeginInfo beginInfo{};
				beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
				beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
				vkBeginCommandBuffer(m_commandBuffer, &beginInfo);
				m_recording = true;
#endif
			}
			void End() override
			{
#if NLS_HAS_VULKAN
				if (m_commandBuffer == nullptr)
					return;
				vkEndCommandBuffer(m_commandBuffer);
				m_recording = false;
#endif
			}
			void Reset() override
			{
#if NLS_HAS_VULKAN
				if (m_commandBuffer == nullptr)
					return;
				DestroyActiveRenderPass();
				m_imageLayouts.clear();
				vkResetCommandBuffer(m_commandBuffer, 0);
#endif
			}
			bool IsRecording() const override { return m_recording; }
			NLS::Render::RHI::NativeHandle GetNativeCommandBuffer() const override
			{
#if NLS_HAS_VULKAN
				return { NLS::Render::RHI::BackendType::Vulkan, reinterpret_cast<void*>(m_commandBuffer) };
#else
				return {};
#endif
			}

			void BeginRenderPass(const NLS::Render::RHI::RHIRenderPassDesc& desc) override
			{
#if NLS_HAS_VULKAN
				if (m_commandBuffer == nullptr)
					return;

				// The baseline WSL device exposes Vulkan 1.1. Keep the classic
				// render-pass path as the only compiled path until dynamic rendering
				// is explicitly negotiated as a device extension.
#if 0
				if (m_dynamicRenderingEnabled)
				{
					// Use VK_KHR_dynamic_rendering (or Vulkan 1.3+ core)
					VkRenderingInfo renderingInfo{};
					renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
					renderingInfo.renderArea.offset = { static_cast<int32_t>(desc.renderArea.x), static_cast<int32_t>(desc.renderArea.y) };
					renderingInfo.renderArea.extent = { desc.renderArea.width, desc.renderArea.height };
					renderingInfo.layerCount = 1;
					renderingInfo.viewMask = 0;

					std::vector<VkRenderingAttachmentInfo> colorAttachments;
					colorAttachments.reserve(desc.colorAttachments.size());
					for (const auto& attachment : desc.colorAttachments)
					{
						VkRenderingAttachmentInfo colorAttInfo{};
						colorAttInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
						if (attachment.view != nullptr && attachment.view->GetTexture() != nullptr)
						{
							auto imgHandle = attachment.view->GetTexture()->GetNativeImageHandle();
							if (imgHandle.backend == NLS::Render::RHI::BackendType::Vulkan)
							{
								colorAttInfo.imageView = reinterpret_cast<VkImageView>(imgHandle.handle);
								colorAttInfo.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
							}
						}
						colorAttInfo.loadOp = (attachment.loadOp == NLS::Render::RHI::LoadOp::Clear) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
						colorAttInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
						if (attachment.loadOp == NLS::Render::RHI::LoadOp::Clear)
							colorAttInfo.clearValue = { attachment.clearValue.r, attachment.clearValue.g, attachment.clearValue.b, attachment.clearValue.a };
						colorAttachments.push_back(colorAttInfo);
					}

					VkRenderingAttachmentInfo depthAttInfo{};
					VkRenderingAttachmentInfo stencilAttInfo{};
					bool hasDepth = false;
					bool hasStencil = false;

					if (desc.depthStencilAttachment.has_value() && desc.depthStencilAttachment->view != nullptr)
					{
						auto dsView = desc.depthStencilAttachment->view;
						if (dsView != nullptr && dsView->GetTexture() != nullptr)
						{
							auto imgHandle = dsView->GetTexture()->GetNativeImageHandle();
							if (imgHandle.backend == NLS::Render::RHI::BackendType::Vulkan)
							{
								depthAttInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
								depthAttInfo.imageView = reinterpret_cast<VkImageView>(imgHandle.handle);
								depthAttInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
								depthAttInfo.loadOp = (desc.depthStencilAttachment->depthLoadOp == NLS::Render::RHI::LoadOp::Clear) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
								depthAttInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
								if (desc.depthStencilAttachment->depthLoadOp == NLS::Render::RHI::LoadOp::Clear)
									depthAttInfo.clearValue.depthStencil.depth = desc.depthStencilAttachment->clearValue.depth;
								hasDepth = true;

								// Stencil uses same attachment
								stencilAttInfo = depthAttInfo;
								stencilAttInfo.loadOp = (desc.depthStencilAttachment->stencilLoadOp == NLS::Render::RHI::LoadOp::Clear) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
								stencilAttInfo.storeOp = (desc.depthStencilAttachment->stencilStoreOp == NLS::Render::RHI::StoreOp::Store) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
								if (desc.depthStencilAttachment->stencilLoadOp == NLS::Render::RHI::LoadOp::Clear)
									stencilAttInfo.clearValue.depthStencil.stencil = desc.depthStencilAttachment->clearValue.stencil;
								hasStencil = true;
							}
						}
					}

					renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
					renderingInfo.pColorAttachments = colorAttachments.data();
					renderingInfo.pDepthAttachment = hasDepth ? &depthAttInfo : nullptr;
					renderingInfo.pStencilAttachment = hasStencil ? &stencilAttInfo : nullptr;

					vkCmdBeginRendering(m_commandBuffer, &renderingInfo);
				}
				#else
				{
					// FrameGraph emits attachment transitions before BeginRenderPass.
					// Repeating them here (especially from UNDEFINED) is invalid.
					if (!desc.attachmentsRequireExternalStateTransitions)
					{
					// Fallback: transition attachments to render target layout
					for (const auto& attachment : desc.colorAttachments)
					{
						if (attachment.view != nullptr && attachment.view->GetTexture() != nullptr)
						{
							auto imgHandle = attachment.view->GetTexture()->GetNativeImageHandle();
							VkImage image = (imgHandle.backend == NLS::Render::RHI::BackendType::Vulkan) ? static_cast<VkImage>(imgHandle.handle) : VK_NULL_HANDLE;
							if (image != VK_NULL_HANDLE)
							{
								VkImageMemoryBarrier barrier{};
								barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
								barrier.srcAccessMask = 0;
								barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
								barrier.oldLayout = NLS::Render::RHI::HasTextureUsage(
									attachment.view->GetTexture()->GetDesc().usage,
									NLS::Render::RHI::TextureUsageFlags::Present)
									? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
									: VK_IMAGE_LAYOUT_UNDEFINED;
								barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
								barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
								barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
								barrier.image = image;
								barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
								barrier.subresourceRange.baseMipLevel = 0;
								barrier.subresourceRange.levelCount = 1;
								barrier.subresourceRange.baseArrayLayer = 0;
								barrier.subresourceRange.layerCount = 1;
								vkCmdPipelineBarrier(m_commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
							}
						}
					}

					// Handle depth stencil if present
					if (desc.depthStencilAttachment.has_value() && desc.depthStencilAttachment->view != nullptr)
					{
						auto imgHandle = desc.depthStencilAttachment->view->GetTexture()->GetNativeImageHandle();
						VkImage image = (imgHandle.backend == NLS::Render::RHI::BackendType::Vulkan) ? static_cast<VkImage>(imgHandle.handle) : VK_NULL_HANDLE;
						if (image != VK_NULL_HANDLE)
						{
							VkImageMemoryBarrier barrier{};
							barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
							barrier.srcAccessMask = 0;
							barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
							barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
							barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
							barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
							barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
							barrier.image = image;
							barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
							barrier.subresourceRange.baseMipLevel = 0;
							barrier.subresourceRange.levelCount = 1;
							barrier.subresourceRange.baseArrayLayer = 0;
							barrier.subresourceRange.layerCount = 1;
							vkCmdPipelineBarrier(m_commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
						}
					}
					}
				}
#endif
				// Vulkan 1.1 uses an explicit render pass/framebuffer. The attachment
				// barriers above establish the layouts expected by this render pass;
				// the render pass then supplies load/store/clear semantics and makes
				// the framebuffer compatible with the graphics pipeline.
				std::vector<VkAttachmentDescription> attachments;
				std::vector<VkAttachmentReference> colorReferences;
				std::vector<VkImageView> imageViews;
				std::vector<VkClearValue> clearValues;
				attachments.reserve(desc.colorAttachments.size() + 1u);
				colorReferences.reserve(desc.colorAttachments.size());
				imageViews.reserve(desc.colorAttachments.size() + 1u);
				clearValues.reserve(desc.colorAttachments.size() + 1u);

				const auto toFormat = [](const NLS::Render::RHI::TextureFormat format)
				{
					switch (format)
					{
					case NLS::Render::RHI::TextureFormat::R8: return VK_FORMAT_R8_UNORM;
					case NLS::Render::RHI::TextureFormat::RG8: return VK_FORMAT_R8G8_UNORM;
					case NLS::Render::RHI::TextureFormat::RGB8: return VK_FORMAT_R8G8B8A8_UNORM;
					case NLS::Render::RHI::TextureFormat::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
					case NLS::Render::RHI::TextureFormat::Depth32F: return VK_FORMAT_D32_SFLOAT;
					case NLS::Render::RHI::TextureFormat::Depth24Stencil8: return VK_FORMAT_D32_SFLOAT;
					case NLS::Render::RHI::TextureFormat::RGBA8:
					default: return VK_FORMAT_R8G8B8A8_UNORM;
					}
				};
				const auto getView = [](const std::shared_ptr<NLS::Render::RHI::RHITextureView>& view) -> VkImageView
				{
					if (view == nullptr)
						return VkImageView{};
					const auto native = view->GetNativeRenderTargetView();
					return native.backend == NLS::Render::RHI::BackendType::Vulkan
						? reinterpret_cast<VkImageView>(native.handle)
						: VkImageView{};
				};
				const auto isPresent = [](const std::shared_ptr<NLS::Render::RHI::RHITextureView>& view)
				{
					return view != nullptr && view->GetTexture() != nullptr &&
						NLS::Render::RHI::HasTextureUsage(view->GetTexture()->GetDesc().usage, NLS::Render::RHI::TextureUsageFlags::Present);
				};
				const auto toLoadOp = [](const NLS::Render::RHI::LoadOp op)
				{
					return op == NLS::Render::RHI::LoadOp::Clear
						? VK_ATTACHMENT_LOAD_OP_CLEAR
						: (op == NLS::Render::RHI::LoadOp::DontCare ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD);
				};
				const auto toStoreOp = [](const NLS::Render::RHI::StoreOp op)
				{
					return op == NLS::Render::RHI::StoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
				};

				for (const auto& color : desc.colorAttachments)
				{
					if (color.view == nullptr || color.view->GetTexture() == nullptr)
						return;
					const VkImageView imageView = getView(color.view);
					if (imageView == VK_NULL_HANDLE)
						return;
					VkAttachmentDescription attachment{};
					attachment.format = toFormat(color.view->GetTexture()->GetDesc().format);
					// Swapchain textures keep RGBA8 as the logical RHI format. Use the
					// actual WSI format for framebuffer/render-pass compatibility.
					attachment.format = ResolveVulkanAttachmentFormat(color.view->GetTexture());
					attachment.samples = VK_SAMPLE_COUNT_1_BIT;
					attachment.loadOp = toLoadOp(color.loadOp);
					attachment.storeOp = toStoreOp(color.storeOp);
					attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
					attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
					// An acquired WSI image has undefined contents on its first use.
					// Subsequent frames are tracked as PRESENT by EndRenderPass. Keep the
					// first render pass compatible with the actual WSI state; later command
					// buffers use the tracked PRESENT layout or an explicit FrameGraph barrier.
					const auto nativeImage = color.view->GetTexture()->GetNativeImageHandle();
					const VkImage colorImage = nativeImage.backend == NLS::Render::RHI::BackendType::Vulkan
						? static_cast<VkImage>(nativeImage.handle)
						: VK_NULL_HANDLE;
					const bool presentAttachment = isPresent(color.view);
					const auto& colorRange = color.view->GetDesc().subresourceRange;
					attachment.initialLayout = GetTrackedImageLayout(
						colorImage,
						colorRange.baseMipLevel,
						colorRange.baseArrayLayer);
					if (attachment.initialLayout == VK_IMAGE_LAYOUT_UNDEFINED && presentAttachment)
						attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
					attachment.finalLayout = presentAttachment ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
					attachments.push_back(attachment);
					colorReferences.push_back({ static_cast<uint32_t>(attachments.size() - 1u), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
					imageViews.push_back(imageView);
					VkClearValue clear{};
					clear.color = {{ color.clearValue.r, color.clearValue.g, color.clearValue.b, color.clearValue.a }};
					clearValues.push_back(clear);
				}

				VkAttachmentReference depthReference{};
				bool hasDepth = false;
				if (desc.depthStencilAttachment.has_value())
				{
					const auto& depth = *desc.depthStencilAttachment;
					if (depth.view == nullptr || depth.view->GetTexture() == nullptr)
						return;
					const VkImageView imageView = getView(depth.view);
					if (imageView == VK_NULL_HANDLE)
						return;
					VkAttachmentDescription attachment{};
					attachment.format = toFormat(depth.view->GetTexture()->GetDesc().format);
					attachment.samples = VK_SAMPLE_COUNT_1_BIT;
					attachment.loadOp = toLoadOp(depth.depthLoadOp);
					attachment.storeOp = toStoreOp(depth.depthStoreOp);
					attachment.stencilLoadOp = toLoadOp(depth.stencilLoadOp);
					attachment.stencilStoreOp = toStoreOp(depth.stencilStoreOp);
					attachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
					attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
					attachments.push_back(attachment);
					depthReference = { static_cast<uint32_t>(attachments.size() - 1u), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
					imageViews.push_back(imageView);
					VkClearValue clear{};
					clear.depthStencil = { depth.clearValue.depth, depth.clearValue.stencil };
					clearValues.push_back(clear);
					hasDepth = true;
				}

				if (attachments.empty())
					return;
				VkSubpassDescription subpass{};
				subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
				subpass.colorAttachmentCount = static_cast<uint32_t>(colorReferences.size());
				subpass.pColorAttachments = colorReferences.data();
				subpass.pDepthStencilAttachment = hasDepth ? &depthReference : nullptr;
				VkRenderPassCreateInfo renderPassInfo{};
				renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
				renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
				renderPassInfo.pAttachments = attachments.data();
				renderPassInfo.subpassCount = 1;
				renderPassInfo.pSubpasses = &subpass;
				if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_activeRenderPass) != VK_SUCCESS)
					return;

				uint32_t width = desc.renderArea.width;
				uint32_t height = desc.renderArea.height;
				if (width == 0u || height == 0u)
				{
					const auto& textureDesc = desc.colorAttachments.empty()
						? desc.depthStencilAttachment->view->GetTexture()->GetDesc()
						: desc.colorAttachments.front().view->GetTexture()->GetDesc();
					width = textureDesc.extent.width;
					height = textureDesc.extent.height;
				}
				if (width == 0u || height == 0u)
				{
					DestroyActiveRenderPass();
					return;
				}
				VkFramebufferCreateInfo framebufferInfo{};
				framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
				framebufferInfo.renderPass = m_activeRenderPass;
				framebufferInfo.attachmentCount = static_cast<uint32_t>(imageViews.size());
				framebufferInfo.pAttachments = imageViews.data();
				framebufferInfo.width = width;
				framebufferInfo.height = height;
				framebufferInfo.layers = 1;
				if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_activeFramebuffer) != VK_SUCCESS)
				{
					DestroyActiveRenderPass();
					return;
				}
				VkRenderPassBeginInfo beginInfo{};
				beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
				beginInfo.renderPass = m_activeRenderPass;
				beginInfo.framebuffer = m_activeFramebuffer;
				beginInfo.renderArea.offset = { desc.renderArea.x, desc.renderArea.y };
				beginInfo.renderArea.extent = { width, height };
				beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
				beginInfo.pClearValues = clearValues.data();
				vkCmdBeginRenderPass(m_commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
				// Scissor is dynamic in every graphics pipeline. The common RHI does
				// not always emit an explicit scissor command, so establish the render
				// area as the safe default before the first draw.
				VkRect2D defaultScissor{};
				defaultScissor.offset = beginInfo.renderArea.offset;
				defaultScissor.extent = beginInfo.renderArea.extent;
				vkCmdSetScissor(m_commandBuffer, 0, 1, &defaultScissor);
				m_activePresentImages.clear();
				for (size_t index = 0u; index < desc.colorAttachments.size(); ++index)
				{
					const auto& color = desc.colorAttachments[index];
					if (color.view != nullptr && color.view->GetTexture() != nullptr)
					{
						const auto native = color.view->GetTexture()->GetNativeImageHandle();
					if (native.backend == NLS::Render::RHI::BackendType::Vulkan)
					{
						const auto image = static_cast<VkImage>(native.handle);
						const auto& colorRange = color.view->GetDesc().subresourceRange;
						SetTrackedImageLayout(
							image,
							VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
							colorRange.baseMipLevel,
							colorRange.baseArrayLayer);
						if (isPresent(color.view))
							m_activePresentImages.push_back(image);
					}
				}
				}
				if (desc.depthStencilAttachment.has_value() && desc.depthStencilAttachment->view != nullptr &&
					desc.depthStencilAttachment->view->GetTexture() != nullptr)
				{
					const auto native = desc.depthStencilAttachment->view->GetTexture()->GetNativeImageHandle();
					if (native.backend == NLS::Render::RHI::BackendType::Vulkan)
					{
						const auto& depthRange = desc.depthStencilAttachment->view->GetDesc().subresourceRange;
						SetTrackedImageLayout(
							static_cast<VkImage>(native.handle),
							VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
							depthRange.baseMipLevel,
							depthRange.baseArrayLayer);
					}
				}
				m_inRenderPass = true;
#endif
			}
			void EndRenderPass() override
			{
#if NLS_HAS_VULKAN
				if (m_commandBuffer == nullptr)
					return;
				if (m_inRenderPass)
				{
					vkCmdEndRenderPass(m_commandBuffer);
					for (const auto image : m_activePresentImages)
					{
						SetTrackedImageLayout(image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0u, 0u);
					}
					m_activePresentImages.clear();
				}
				m_inRenderPass = false;
#endif
			}
			void SetViewport(const NLS::Render::RHI::RHIViewport& viewport) override
			{
#if NLS_HAS_VULKAN
				if (m_commandBuffer == nullptr)
					return;
				VkViewport vp{};
				vp.x = viewport.x;
				// Match the engine's D3D-style top-left origin while retaining
				// Vulkan's conventional framebuffer coordinate system. A negative
				// height flips NDC Y, so the origin must move to the bottom edge;
				// y=0 would otherwise place the viewport entirely above the target.
				vp.y = viewport.y + viewport.height;
				vp.width = viewport.width;
				vp.height = -viewport.height;
				vp.minDepth = viewport.minDepth;
				vp.maxDepth = viewport.maxDepth;
				vkCmdSetViewport(m_commandBuffer, 0, 1, &vp);
#endif
			}
			void SetScissor(const NLS::Render::RHI::RHIRect2D& rect) override
			{
#if NLS_HAS_VULKAN
				if (m_commandBuffer == nullptr)
					return;
				VkRect2D scissor{};
				scissor.offset.x = static_cast<int32_t>(rect.x);
				scissor.offset.y = static_cast<int32_t>(rect.y);
				scissor.extent.width = rect.width;
				scissor.extent.height = rect.height;
				vkCmdSetScissor(m_commandBuffer, 0, 1, &scissor);
#endif
			}
			void BindGraphicsPipeline(const std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>& pipeline) override;
			void BindComputePipeline(const std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>& pipeline) override;
			void BindBindingSet(uint32_t setIndex, const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& bindingSet) override;
			void PushConstants(NLS::Render::RHI::ShaderStageMask stageMask, uint32_t offset, uint32_t size, const void* data) override
			{
#if NLS_HAS_VULKAN
				if (m_commandBuffer == nullptr || m_boundPipelineLayout == VK_NULL_HANDLE)
					return;

				VkShaderStageFlags flags = 0;
				if (static_cast<uint32_t>(stageMask) & static_cast<uint32_t>(NLS::Render::RHI::ShaderStageMask::Vertex))
					flags |= VK_SHADER_STAGE_VERTEX_BIT;
				if (static_cast<uint32_t>(stageMask) & static_cast<uint32_t>(NLS::Render::RHI::ShaderStageMask::Fragment))
					flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
				if (static_cast<uint32_t>(stageMask) & static_cast<uint32_t>(NLS::Render::RHI::ShaderStageMask::Compute))
					flags |= VK_SHADER_STAGE_COMPUTE_BIT;
				if (flags == 0)
					flags = VK_SHADER_STAGE_ALL_GRAPHICS;
				// The graphics pipeline layout may legally expose one shared range for
				// vertex and fragment stages even when the common renderer only labels
				// the update as a vertex write. Vulkan requires vkCmdPushConstants to
				// include every stage in an overlapping range.
				if (!m_computePipelineBound && (flags & (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)) != 0u)
					flags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

				vkCmdPushConstants(m_commandBuffer, m_boundPipelineLayout, flags, offset, size, data);
#endif
			}
			void BindVertexBuffer(uint32_t slot, const NLS::Render::RHI::RHIVertexBufferView& view) override
			{
#if NLS_HAS_VULKAN
				if (m_commandBuffer == nullptr || view.buffer == nullptr)
					return;
				auto vbHandle = view.buffer->GetNativeBufferHandle();
				if (vbHandle.backend != NLS::Render::RHI::BackendType::Vulkan)
					return;
				VkBuffer vkBuffer = static_cast<VkBuffer>(vbHandle.handle);
				if (vkBuffer == VK_NULL_HANDLE)
					return;
				VkDeviceSize offsets[] = { view.offset };
				VkBuffer buffers[] = { vkBuffer };
				vkCmdBindVertexBuffers(m_commandBuffer, slot, 1, buffers, offsets);
#endif
			}
			void BindIndexBuffer(const NLS::Render::RHI::RHIIndexBufferView& view) override
			{
#if NLS_HAS_VULKAN
				if (m_commandBuffer == nullptr || view.buffer == nullptr)
					return;
				auto ibHandle = view.buffer->GetNativeBufferHandle();
				if (ibHandle.backend != NLS::Render::RHI::BackendType::Vulkan)
					return;
				VkBuffer vkBuffer = static_cast<VkBuffer>(ibHandle.handle);
				if (vkBuffer == VK_NULL_HANDLE)
					return;
				vkCmdBindIndexBuffer(m_commandBuffer, vkBuffer, view.offset,
					view.indexType == NLS::Render::RHI::IndexType::UInt16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
#endif
			}
			void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override
			{
#if NLS_HAS_VULKAN
				if (m_commandBuffer != nullptr)
					vkCmdDraw(m_commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
#endif
			}
			void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override
			{
#if NLS_HAS_VULKAN
				if (m_commandBuffer != nullptr)
					vkCmdDrawIndexed(m_commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
#endif
			}
			void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override
			{
#if NLS_HAS_VULKAN
				if (m_commandBuffer != nullptr && m_computePipelineBound)
					vkCmdDispatch(m_commandBuffer, groupCountX, groupCountY, groupCountZ);
#endif
			}
			void CopyBuffer(const std::shared_ptr<NLS::Render::RHI::RHIBuffer>& source, const std::shared_ptr<NLS::Render::RHI::RHIBuffer>& destination, const NLS::Render::RHI::RHIBufferCopyRegion& region) override
			{
#if NLS_HAS_VULKAN
				if (m_commandBuffer == nullptr || source == nullptr || destination == nullptr)
					return;
				auto srcHandle = source->GetNativeBufferHandle();
				auto dstHandle = destination->GetNativeBufferHandle();
				if (srcHandle.backend != NLS::Render::RHI::BackendType::Vulkan || dstHandle.backend != NLS::Render::RHI::BackendType::Vulkan)
					return;
				VkBuffer srcBuffer = static_cast<VkBuffer>(srcHandle.handle);
				VkBuffer dstBuffer = static_cast<VkBuffer>(dstHandle.handle);
				if (srcBuffer == VK_NULL_HANDLE || dstBuffer == VK_NULL_HANDLE)
					return;
				VkBufferCopy copyRegion{};
				copyRegion.srcOffset = region.srcOffset;
				copyRegion.dstOffset = region.dstOffset;
				copyRegion.size = region.size;
				vkCmdCopyBuffer(m_commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);
#endif
			}
			void CopyBufferToTexture(const NLS::Render::RHI::RHIBufferToTextureCopyDesc& desc) override
			{
#if NLS_HAS_VULKAN
				if (m_commandBuffer == nullptr || desc.source == nullptr || desc.destination == nullptr)
					return;

				auto srcBufHandle = desc.source->GetNativeBufferHandle();
				VkBuffer srcBuffer = (srcBufHandle.backend == NLS::Render::RHI::BackendType::Vulkan) ? static_cast<VkBuffer>(srcBufHandle.handle) : VK_NULL_HANDLE;
				if (srcBuffer == VK_NULL_HANDLE)
					return;

				auto dstImgHandle = desc.destination->GetNativeImageHandle();
				VkImage dstImage = (dstImgHandle.backend == NLS::Render::RHI::BackendType::Vulkan) ? static_cast<VkImage>(dstImgHandle.handle) : VK_NULL_HANDLE;
				if (dstImage == VK_NULL_HANDLE)
					return;

				VkBufferImageCopy copyRegion{};
				copyRegion.bufferOffset = desc.bufferOffset;
				copyRegion.bufferRowLength = 0; // Tightly packed
				copyRegion.bufferImageHeight = 0;
				copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				copyRegion.imageSubresource.mipLevel = desc.mipLevel;
				copyRegion.imageSubresource.baseArrayLayer = desc.arrayLayer;
				copyRegion.imageSubresource.layerCount = 1;
				copyRegion.imageOffset.x = desc.textureOffset.x;
				copyRegion.imageOffset.y = desc.textureOffset.y;
				copyRegion.imageOffset.z = desc.textureOffset.z;
				copyRegion.imageExtent.width = desc.extent.width;
				copyRegion.imageExtent.height = desc.extent.height;
				copyRegion.imageExtent.depth = desc.extent.depth;

				vkCmdCopyBufferToImage(m_commandBuffer, srcBuffer, dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
#endif
			}
			void CopyTexture(const NLS::Render::RHI::RHITextureCopyDesc& desc) override
			{
#if NLS_HAS_VULKAN
				if (m_commandBuffer == nullptr || desc.source == nullptr || desc.destination == nullptr)
					return;

				auto srcImgHandle = desc.source->GetNativeImageHandle();
				VkImage srcImage = (srcImgHandle.backend == NLS::Render::RHI::BackendType::Vulkan) ? static_cast<VkImage>(srcImgHandle.handle) : VK_NULL_HANDLE;
				auto dstImgHandle = desc.destination->GetNativeImageHandle();
				VkImage dstImage = (dstImgHandle.backend == NLS::Render::RHI::BackendType::Vulkan) ? static_cast<VkImage>(dstImgHandle.handle) : VK_NULL_HANDLE;
				if (srcImage == VK_NULL_HANDLE || dstImage == VK_NULL_HANDLE)
					return;

				VkImageCopy copyRegion{};
				copyRegion.srcOffset.x = desc.sourceOffset.x;
				copyRegion.srcOffset.y = desc.sourceOffset.y;
				copyRegion.srcOffset.z = desc.sourceOffset.z;
				copyRegion.dstOffset.x = desc.destinationOffset.x;
				copyRegion.dstOffset.y = desc.destinationOffset.y;
				copyRegion.dstOffset.z = desc.destinationOffset.z;
				copyRegion.extent.width = desc.extent.width;
				copyRegion.extent.height = desc.extent.height;
				copyRegion.extent.depth = desc.extent.depth;
				copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				copyRegion.srcSubresource.mipLevel = desc.sourceRange.baseMipLevel;
				copyRegion.srcSubresource.baseArrayLayer = desc.sourceRange.baseArrayLayer;
				copyRegion.srcSubresource.layerCount = desc.sourceRange.arrayLayerCount;
				copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				copyRegion.dstSubresource.mipLevel = desc.destinationRange.baseMipLevel;
				copyRegion.dstSubresource.baseArrayLayer = desc.destinationRange.baseArrayLayer;
				copyRegion.dstSubresource.layerCount = desc.destinationRange.arrayLayerCount;

				vkCmdCopyImage(m_commandBuffer, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
#endif
			}
			void Barrier(const NLS::Render::RHI::RHIBarrierDesc& barrier) override
			{
#if NLS_HAS_VULKAN
				if (m_commandBuffer == nullptr)
					return;

				std::vector<VkMemoryBarrier> memoryBarriers;
				std::vector<VkBufferMemoryBarrier> bufferBarriers;
				std::vector<VkImageMemoryBarrier> imageBarriers;

				// Convert buffer barriers
				for (const auto& bb : barrier.bufferBarriers)
				{
					VkBufferMemoryBarrier vkBarrier{};
					vkBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
					vkBarrier.srcAccessMask = ToVkAccessFlags(bb.sourceAccessMask);
					vkBarrier.dstAccessMask = ToVkAccessFlags(bb.destinationAccessMask);
					vkBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					vkBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					auto bufHandle = bb.buffer->GetNativeBufferHandle();
					vkBarrier.buffer = (bufHandle.backend == NLS::Render::RHI::BackendType::Vulkan) ? static_cast<VkBuffer>(bufHandle.handle) : VK_NULL_HANDLE;
					vkBarrier.offset = 0;
					vkBarrier.size = bb.buffer->GetDesc().size;
					bufferBarriers.push_back(vkBarrier);
				}

				// Convert texture barriers
				for (const auto& tb : barrier.textureBarriers)
				{
					auto imgHandle = tb.texture->GetNativeImageHandle();
					VkImage image = (imgHandle.backend == NLS::Render::RHI::BackendType::Vulkan) ? static_cast<VkImage>(imgHandle.handle) : VK_NULL_HANDLE;
					if (image == VK_NULL_HANDLE)
						continue;

					VkImageMemoryBarrier vkBarrier{};
					vkBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
					vkBarrier.srcAccessMask = ToVkAccessFlags(tb.sourceAccessMask);
					vkBarrier.dstAccessMask = ToVkAccessFlags(tb.destinationAccessMask);
					const auto& textureDesc = tb.texture->GetDesc();
					const uint32_t baseMipLevel = tb.subresourceRange.baseMipLevel;
					const uint32_t baseArrayLayer = tb.subresourceRange.baseArrayLayer;
					const uint32_t mipLevelCount = tb.subresourceRange.mipLevelCount != 0u
						? tb.subresourceRange.mipLevelCount
						: (baseMipLevel < textureDesc.mipLevels ? textureDesc.mipLevels - baseMipLevel : 0u);
					const uint32_t textureLayerCount = textureDesc.dimension == NLS::Render::RHI::TextureDimension::TextureCube
						? 6u
						: std::max(1u, textureDesc.arrayLayers);
					const uint32_t arrayLayerCount = tb.subresourceRange.arrayLayerCount != 0u
						? tb.subresourceRange.arrayLayerCount
						: (baseArrayLayer < textureLayerCount ? textureLayerCount - baseArrayLayer : 0u);
					if (mipLevelCount == 0u || arrayLayerCount == 0u)
						continue;
					if (HasTrackedImageLayout(image, baseMipLevel, baseArrayLayer))
						vkBarrier.oldLayout = GetTrackedImageLayout(image, baseMipLevel, baseArrayLayer);
					else
					{
						// A newly acquired swapchain image has an undefined initial layout;
						// the first command buffer must not claim PRESENT_SRC_KHR unless this
						// command buffer has already tracked the image from an earlier pass.
						const bool isPresentImage = NLS::Render::RHI::HasTextureUsage(
							tb.texture->GetDesc().usage,
							NLS::Render::RHI::TextureUsageFlags::Present);
						vkBarrier.oldLayout = isPresentImage
							? VK_IMAGE_LAYOUT_UNDEFINED
							: ToVkImageLayout(tb.before);
					}
					vkBarrier.newLayout = ToVkImageLayout(tb.after);
					// Vulkan permits UNDEFINED as an old/discard layout, but never as the
					// destination of an image barrier.  Unknown is an RHI discard hint;
					// leave the image in its tracked layout and let the next real state
					// transition perform the discard when needed.
					if (vkBarrier.newLayout == VK_IMAGE_LAYOUT_UNDEFINED ||
						vkBarrier.newLayout == VK_IMAGE_LAYOUT_PREINITIALIZED)
						continue;
					vkBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					vkBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					vkBarrier.image = image;
					const auto textureFormat = tb.texture->GetDesc().format;
					vkBarrier.subresourceRange.aspectMask =
						(textureFormat == NLS::Render::RHI::TextureFormat::Depth24Stencil8 ||
						 textureFormat == NLS::Render::RHI::TextureFormat::Depth32F)
							? VK_IMAGE_ASPECT_DEPTH_BIT
							: VK_IMAGE_ASPECT_COLOR_BIT;
					vkBarrier.subresourceRange.baseMipLevel = baseMipLevel;
					vkBarrier.subresourceRange.levelCount = mipLevelCount;
					vkBarrier.subresourceRange.baseArrayLayer = baseArrayLayer;
					vkBarrier.subresourceRange.layerCount = arrayLayerCount;
					imageBarriers.push_back(vkBarrier);
					for (uint32_t mipLevel = baseMipLevel; mipLevel < baseMipLevel + mipLevelCount; ++mipLevel)
					{
						for (uint32_t arrayLayer = baseArrayLayer; arrayLayer < baseArrayLayer + arrayLayerCount; ++arrayLayer)
							SetTrackedImageLayout(image, vkBarrier.newLayout, mipLevel, arrayLayer);
					}
					SetVulkanTextureLogicalState(tb.texture, tb.after);
				}

				VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
				VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

				vkCmdPipelineBarrier(
					m_commandBuffer,
					srcStage,
					dstStage,
					0,
					static_cast<uint32_t>(memoryBarriers.size()), memoryBarriers.data(),
					static_cast<uint32_t>(bufferBarriers.size()), bufferBarriers.data(),
					static_cast<uint32_t>(imageBarriers.size()), imageBarriers.data());
#endif
			}

#if NLS_HAS_VULKAN
			VkCommandBuffer GetCommandBuffer() const { return m_commandBuffer; }
#endif

		private:
			std::string m_debugName;
			bool m_recording = false;
			bool m_inRenderPass = false;
			bool m_dynamicRenderingEnabled = false;
#if NLS_HAS_VULKAN
			VkDevice m_device = nullptr;
			VkCommandPool m_commandPool = nullptr;
			VkCommandBuffer m_commandBuffer = nullptr;
			VkPipelineLayout m_boundPipelineLayout = VK_NULL_HANDLE;
			NativeVulkanPipelineLayout* m_boundPipelineLayoutOwner = nullptr;
			VkRenderPass m_activeRenderPass = VK_NULL_HANDLE;
			VkFramebuffer m_activeFramebuffer = VK_NULL_HANDLE;
			std::unordered_map<VulkanImageLayoutKey, VkImageLayout, VulkanImageLayoutKeyHash> m_imageLayouts;
			std::vector<VkImage> m_activePresentImages;
#endif
			std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> m_boundPipeline;
			std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> m_boundComputePipeline;
			bool m_computePipelineBound = false;

#if NLS_HAS_VULKAN
			VkImageLayout GetTrackedImageLayout(
				VkImage image,
				uint32_t mipLevel = kWholeImageSubresource,
				uint32_t arrayLayer = kWholeImageSubresource) const
			{
				const auto exact = m_imageLayouts.find({ image, mipLevel, arrayLayer });
				if (exact != m_imageLayouts.end())
					return exact->second;
				if (mipLevel != kWholeImageSubresource || arrayLayer != kWholeImageSubresource)
				{
					const auto whole = m_imageLayouts.find({ image, kWholeImageSubresource, kWholeImageSubresource });
					if (whole != m_imageLayouts.end())
						return whole->second;
				}
				return GetVulkanImageLayout(image, mipLevel, arrayLayer);
			}

			bool HasTrackedImageLayout(
				VkImage image,
				uint32_t mipLevel = kWholeImageSubresource,
				uint32_t arrayLayer = kWholeImageSubresource) const
			{
				if (m_imageLayouts.find({ image, mipLevel, arrayLayer }) != m_imageLayouts.end())
					return true;
				if (mipLevel != kWholeImageSubresource || arrayLayer != kWholeImageSubresource)
				{
					if (m_imageLayouts.find({ image, kWholeImageSubresource, kWholeImageSubresource }) != m_imageLayouts.end())
						return true;
				}
				return HasVulkanImageLayout(image, mipLevel, arrayLayer);
			}

			void SetTrackedImageLayout(
				VkImage image,
				VkImageLayout layout,
				uint32_t mipLevel = kWholeImageSubresource,
				uint32_t arrayLayer = kWholeImageSubresource)
			{
				m_imageLayouts[{ image, mipLevel, arrayLayer }] = layout;
				SetVulkanImageLayout(image, layout, mipLevel, arrayLayer);
			}

			void DestroyActiveRenderPass()
			{
				if (m_device == VK_NULL_HANDLE)
					return;
				if (m_activeFramebuffer != VK_NULL_HANDLE)
				{
					vkDestroyFramebuffer(m_device, m_activeFramebuffer, nullptr);
					m_activeFramebuffer = VK_NULL_HANDLE;
				}
				if (m_activeRenderPass != VK_NULL_HANDLE)
				{
					vkDestroyRenderPass(m_device, m_activeRenderPass, nullptr);
					m_activeRenderPass = VK_NULL_HANDLE;
				}
				m_activePresentImages.clear();
			}
#endif

#if NLS_HAS_VULKAN
			static VkAccessFlags ToVkAccessFlags(NLS::Render::RHI::AccessMask mask)
			{
				VkAccessFlags flags = 0;
				if (static_cast<uint32_t>(mask) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::MemoryRead))
					flags |= VK_ACCESS_MEMORY_READ_BIT;
				if (static_cast<uint32_t>(mask) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::MemoryWrite))
					flags |= VK_ACCESS_MEMORY_WRITE_BIT;
				if (static_cast<uint32_t>(mask) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::ShaderRead))
					flags |= VK_ACCESS_SHADER_READ_BIT;
				if (static_cast<uint32_t>(mask) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::ShaderWrite))
					flags |= VK_ACCESS_SHADER_WRITE_BIT;
				if (static_cast<uint32_t>(mask) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::ColorAttachmentRead))
					flags |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
				if (static_cast<uint32_t>(mask) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::ColorAttachmentWrite))
					flags |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
				if (static_cast<uint32_t>(mask) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::DepthStencilRead))
					flags |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
				if (static_cast<uint32_t>(mask) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::DepthStencilWrite))
					flags |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
				if (static_cast<uint32_t>(mask) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::CopyRead))
					flags |= VK_ACCESS_TRANSFER_READ_BIT;
				if (static_cast<uint32_t>(mask) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::CopyWrite))
					flags |= VK_ACCESS_TRANSFER_WRITE_BIT;
				return flags;
			}

			static VkImageLayout ToVkImageLayout(NLS::Render::RHI::ResourceState state)
			{
				switch (state)
				{
				case NLS::Render::RHI::ResourceState::CopySrc:
					return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				case NLS::Render::RHI::ResourceState::CopyDst:
					return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				case NLS::Render::RHI::ResourceState::RenderTarget:
					return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				case NLS::Render::RHI::ResourceState::DepthRead:
				case NLS::Render::RHI::ResourceState::DepthWrite:
					return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
				case NLS::Render::RHI::ResourceState::ShaderRead:
					return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				case NLS::Render::RHI::ResourceState::Present:
					return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
				case NLS::Render::RHI::ResourceState::Unknown:
					return VK_IMAGE_LAYOUT_UNDEFINED;
				default:
					return VK_IMAGE_LAYOUT_GENERAL;
				}
			}
#endif
		};

		NLS::Render::RHI::RHIQueueOperationResult NativeVulkanQueue::SubmitChecked(const NLS::Render::RHI::RHISubmitDesc& submitDesc)
		{
			NLS::Render::RHI::RHIQueueOperationResult result;
#if NLS_HAS_VULKAN
			std::lock_guard<std::mutex> queueLock(m_queueMutex);
			if (m_device == VK_NULL_HANDLE || m_queue == VK_NULL_HANDLE)
			{
				result.code = NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument;
				result.message = "Vulkan queue is not initialized";
				return result;
			}

			std::vector<VkCommandBuffer> vkCommandBuffers;
			vkCommandBuffers.reserve(submitDesc.commandBuffers.size());
			for (const auto& cmdBuffer : submitDesc.commandBuffers)
			{
				if (cmdBuffer == nullptr)
					continue;
				auto* nativeCmdBuffer = dynamic_cast<NativeVulkanCommandBuffer*>(cmdBuffer.get());
				if (nativeCmdBuffer == nullptr || nativeCmdBuffer->GetCommandBuffer() == VK_NULL_HANDLE)
				{
					result.code = NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument;
					result.message = "Submit contains a non-Vulkan command buffer";
					return result;
				}
				vkCommandBuffers.push_back(nativeCmdBuffer->GetCommandBuffer());
			}
			if (vkCommandBuffers.empty())
			{
				result.code = NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument;
				result.message = "Submit contains no command buffers";
				return result;
			}

			VkSubmitInfo submitInfo{};
			submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			std::vector<VkSemaphore> waitSemaphores;
			std::vector<VkPipelineStageFlags> waitStages;
			for (const auto& semaphore : submitDesc.waitSemaphores)
			{
				auto* nativeSemaphore = dynamic_cast<NativeVulkanSemaphore*>(semaphore.get());
				if (nativeSemaphore == nullptr || nativeSemaphore->GetSemaphore() == VK_NULL_HANDLE)
				{
					result.code = NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument;
					result.message = "Submit contains a non-Vulkan wait semaphore";
					return result;
				}
				waitSemaphores.push_back(nativeSemaphore->GetSemaphore());
				waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
			}
			std::vector<VkSemaphore> signalSemaphores;
			// Binary signal semaphores cannot be reset. The common frame lifecycle
			// intentionally reuses semaphore wrappers, so retire their previous
			// signal after all earlier queue work has completed and give this submit a
			// fresh VkSemaphore handle.
			(void)vkQueueWaitIdle(m_queue);
			for (const auto& semaphore : submitDesc.signalSemaphores)
			{
				auto* nativeSemaphore = dynamic_cast<NativeVulkanSemaphore*>(semaphore.get());
				if (nativeSemaphore == nullptr || nativeSemaphore->GetSemaphore() == VK_NULL_HANDLE)
				{
					result.code = NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument;
					result.message = "Submit contains a non-Vulkan signal semaphore";
					return result;
				}
				if (!nativeSemaphore->Recreate())
				{
					result.code = NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure;
					result.message = "Failed to recreate Vulkan signal semaphore";
					return result;
				}
				signalSemaphores.push_back(nativeSemaphore->GetSemaphore());
			}
			submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
			submitInfo.pWaitSemaphores = waitSemaphores.empty() ? nullptr : waitSemaphores.data();
			submitInfo.pWaitDstStageMask = waitStages.empty() ? nullptr : waitStages.data();
			submitInfo.signalSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size());
			submitInfo.pSignalSemaphores = signalSemaphores.empty() ? nullptr : signalSemaphores.data();
			submitInfo.commandBufferCount = static_cast<uint32_t>(vkCommandBuffers.size());
			submitInfo.pCommandBuffers = vkCommandBuffers.data();

			VkFence signalFence = VK_NULL_HANDLE;
			if (submitDesc.signalFence != nullptr)
			{
				auto* nativeFence = dynamic_cast<NativeVulkanFence*>(submitDesc.signalFence.get());
				if (nativeFence == nullptr || nativeFence->GetFence() == VK_NULL_HANDLE)
				{
					result.code = NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument;
					result.message = "Submit contains a non-Vulkan signal fence";
					return result;
				}
				signalFence = nativeFence->GetFence();
				if (nativeFence->IsSignaled())
					nativeFence->Reset();
			}

			const VkResult vkResult = vkQueueSubmit(m_queue, 1u, &submitInfo, signalFence);
			result.mayHaveQueuedGpuWork = vkResult != VK_ERROR_DEVICE_LOST && vkResult != VK_ERROR_OUT_OF_HOST_MEMORY && vkResult != VK_ERROR_OUT_OF_DEVICE_MEMORY;
			result.frameFenceSignalQueued = vkResult == VK_SUCCESS && signalFence != VK_NULL_HANDLE;
			if (vkResult == VK_SUCCESS)
			{
				result.code = NLS::Render::RHI::RHIQueueOperationStatusCode::Success;
				return result;
			}
			result.code = vkResult == VK_ERROR_DEVICE_LOST
				? NLS::Render::RHI::RHIQueueOperationStatusCode::DeviceLost
				: NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure;
			result.message = "vkQueueSubmit failed with VkResult=" + std::to_string(vkResult);
#else
			(void)submitDesc;
			result.code = NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure;
			result.message = "Vulkan backend is not compiled";
#endif
			return result;
		}

		void NativeVulkanQueue::Submit(const NLS::Render::RHI::RHISubmitDesc& submitDesc)
		{
			const auto result = SubmitChecked(submitDesc);
			if (!result.Succeeded())
				NLS_LOG_ERROR("NativeVulkanQueue::Submit: " + result.message);
		}

		NLS::Render::RHI::RHIQueueOperationResult NativeVulkanQueue::PresentChecked(const NLS::Render::RHI::RHIPresentDesc& presentDesc)
		{
			NLS::Render::RHI::RHIQueueOperationResult result;
#if NLS_HAS_VULKAN
			std::lock_guard<std::mutex> queueLock(m_queueMutex);
			if (m_queue == VK_NULL_HANDLE || presentDesc.swapchain == nullptr)
			{
				result.code = NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument;
				result.message = "Present queue or swapchain is null";
				return result;
			}
			const VkSwapchainKHR swapchain = static_cast<VkSwapchainKHR>(presentDesc.swapchain->GetNativeSwapchainHandle().handle);
			if (swapchain == VK_NULL_HANDLE)
			{
				result.code = NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument;
				result.message = "Present swapchain handle is null";
				return result;
			}
			std::vector<VkSemaphore> waitSemaphores;
			for (const auto& semaphore : presentDesc.waitSemaphores)
			{
				auto* nativeSemaphore = dynamic_cast<NativeVulkanSemaphore*>(semaphore.get());
				if (nativeSemaphore == nullptr || nativeSemaphore->GetSemaphore() == VK_NULL_HANDLE)
				{
					result.code = NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument;
					result.message = "Present contains a non-Vulkan wait semaphore";
					return result;
				}
				waitSemaphores.push_back(nativeSemaphore->GetSemaphore());
			}
			if (presentDesc.uiSignalSemaphore != nullptr)
			{
				const VkSemaphore uiSemaphore = static_cast<VkSemaphore>(presentDesc.uiSignalSemaphore.handle);
				if (uiSemaphore != VK_NULL_HANDLE)
					waitSemaphores.push_back(uiSemaphore);
			}
			VkPresentInfoKHR presentInfo{};
			presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
			presentInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
			presentInfo.pWaitSemaphores = waitSemaphores.empty() ? nullptr : waitSemaphores.data();
			presentInfo.swapchainCount = 1u;
			presentInfo.pSwapchains = &swapchain;
			presentInfo.pImageIndices = &presentDesc.imageIndex;
			const VkResult vkResult = vkQueuePresentKHR(m_queue, &presentInfo);
			result.mayHaveQueuedGpuWork = vkResult == VK_SUCCESS || vkResult == VK_SUBOPTIMAL_KHR;
			if (vkResult == VK_SUCCESS || vkResult == VK_SUBOPTIMAL_KHR)
			{
				if (presentDesc.signalFence != nullptr)
				{
					auto* nativeFence = dynamic_cast<NativeVulkanFence*>(presentDesc.signalFence.get());
					if (nativeFence != nullptr && nativeFence->GetFence() != VK_NULL_HANDLE)
					{
						if (nativeFence->IsSignaled())
							nativeFence->Reset();
						VkSubmitInfo emptySubmit{};
						emptySubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
						const VkResult fenceResult = vkQueueSubmit(m_queue, 1u, &emptySubmit, nativeFence->GetFence());
						result.frameFenceSignalQueued = fenceResult == VK_SUCCESS;
						if (fenceResult != VK_SUCCESS)
						{
							result.code = NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure;
							result.message = "vkQueueSubmit for present fence failed with VkResult=" + std::to_string(fenceResult);
							return result;
						}
					}
				}
				result.code = NLS::Render::RHI::RHIQueueOperationStatusCode::Success;
				if (vkResult == VK_SUBOPTIMAL_KHR)
					result.message = "vkQueuePresentKHR returned VK_SUBOPTIMAL_KHR";
				return result;
			}
			result.code = vkResult == VK_ERROR_DEVICE_LOST
				? NLS::Render::RHI::RHIQueueOperationStatusCode::DeviceLost
				: NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure;
			result.message = vkResult == VK_ERROR_OUT_OF_DATE_KHR
				? "vkQueuePresentKHR returned VK_ERROR_OUT_OF_DATE_KHR"
				: "vkQueuePresentKHR failed with VkResult=" + std::to_string(vkResult);
#else
			(void)presentDesc;
			result.code = NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure;
			result.message = "Vulkan backend is not compiled";
#endif
			return result;
		}

		void NativeVulkanQueue::Present(const NLS::Render::RHI::RHIPresentDesc& presentDesc)
		{
			const auto result = PresentChecked(presentDesc);
			if (!result.Succeeded())
				NLS_LOG_ERROR("NativeVulkanQueue::Present: " + result.message);
		}

		class NativeVulkanCommandPool final : public NLS::Render::RHI::RHICommandPool
		{
		public:
			NativeVulkanCommandPool(VkDevice device, VkCommandPool commandPool, NLS::Render::RHI::QueueType queueType, const std::string& debugName, bool dynamicRenderingEnabled)
				: m_device(device)
				, m_commandPool(commandPool)
				, m_queueType(queueType)
				, m_debugName(debugName)
				, m_dynamicRenderingEnabled(dynamicRenderingEnabled)
			{
			}
			~NativeVulkanCommandPool()
			{
#if NLS_HAS_VULKAN
				if (m_device != VK_NULL_HANDLE && m_commandPool != VK_NULL_HANDLE)
					vkDestroyCommandPool(m_device, m_commandPool, nullptr);
#endif
			}

			std::string_view GetDebugName() const override { return m_debugName; }
			NLS::Render::RHI::QueueType GetQueueType() const override { return m_queueType; }
			std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> CreateCommandBuffer(std::string debugName) override
			{
#if NLS_HAS_VULKAN
				return std::make_shared<NativeVulkanCommandBuffer>(m_device, m_commandPool, debugName.empty() ? m_debugName : debugName, m_dynamicRenderingEnabled);
#else
				return nullptr;
#endif
			}
			void Reset() override
			{
#if NLS_HAS_VULKAN
				if (m_device != nullptr && m_commandPool != nullptr)
					vkResetCommandPool(m_device, m_commandPool, 0);
#endif
			}

#if NLS_HAS_VULKAN
			VkCommandPool GetCommandPool() const { return m_commandPool; }
#endif

		private:
			VkDevice m_device = nullptr;
			VkCommandPool m_commandPool = nullptr;
			NLS::Render::RHI::QueueType m_queueType = NLS::Render::RHI::QueueType::Graphics;
			std::string m_debugName;
			bool m_dynamicRenderingEnabled = false;
		};

		// Minimal texture wrapper for swapchain images that just provides GetNativeImageHandle()
		class NativeVulkanSwapchainTexture final : public NLS::Render::RHI::RHITexture
		{
		public:
			NativeVulkanSwapchainTexture(VkDevice device, VkImage image, VkFormat format, uint32_t width, uint32_t height)
				: m_device(device)
				, m_image(image)
				, m_format(format)
			{
				m_desc.format = NLS::Render::RHI::TextureFormat::RGBA8;
				m_desc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
				m_desc.extent.width = width;
				m_desc.extent.height = height;
				m_desc.extent.depth = 1;
				m_desc.arrayLayers = 1;
				m_desc.mipLevels = 1;
				m_desc.usage = NLS::Render::RHI::TextureUsageFlags::ColorAttachment | NLS::Render::RHI::TextureUsageFlags::Present;
			}

			std::string_view GetDebugName() const override { return "SwapchainTexture"; }
			const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
			NLS::Render::RHI::ResourceState GetState() const override { return NLS::Render::RHI::ResourceState::Present; }
			NLS::Render::RHI::NativeHandle GetNativeImageHandle() override { return { NLS::Render::RHI::BackendType::Vulkan, reinterpret_cast<void*>(m_image) }; }
			VkFormat GetFormat() const { return m_format; }

		private:
			VkDevice m_device = nullptr;
			VkImage m_image = VK_NULL_HANDLE;
			VkFormat m_format = VK_FORMAT_UNDEFINED;
			NLS::Render::RHI::RHITextureDesc m_desc{};
		};

#if NLS_HAS_VULKAN
		VkFormat ResolveVulkanAttachmentFormat(const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture)
		{
			if (const auto* swapchainTexture = dynamic_cast<const NativeVulkanSwapchainTexture*>(texture.get()))
				return swapchainTexture->GetFormat();
			if (texture == nullptr)
				return VK_FORMAT_R8G8B8A8_UNORM;
			const auto& desc = texture->GetDesc();
			const bool srgb = desc.colorSpace == NLS::Render::RHI::TextureColorSpace::SRGB;
			switch (desc.format)
			{
			case NLS::Render::RHI::TextureFormat::R8: return VK_FORMAT_R8_UNORM;
			case NLS::Render::RHI::TextureFormat::RG8: return VK_FORMAT_R8G8_UNORM;
			case NLS::Render::RHI::TextureFormat::RGB8:
			case NLS::Render::RHI::TextureFormat::RGBA8:
				return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
			case NLS::Render::RHI::TextureFormat::R16F: return VK_FORMAT_R16_SFLOAT;
			case NLS::Render::RHI::TextureFormat::RG16F: return VK_FORMAT_R16G16_SFLOAT;
			case NLS::Render::RHI::TextureFormat::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
			case NLS::Render::RHI::TextureFormat::R32F: return VK_FORMAT_R32_SFLOAT;
			case NLS::Render::RHI::TextureFormat::RG32F: return VK_FORMAT_R32G32_SFLOAT;
			case NLS::Render::RHI::TextureFormat::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
			case NLS::Render::RHI::TextureFormat::Depth32F: return VK_FORMAT_D32_SFLOAT;
			// D24_UNORM_S8_UINT is not supported by several Vulkan 1.1
			// implementations (including the AMD adapter used for the Windows
			// smoke run). The RHI only requires a depth attachment here, so use the
			// widely supported 32-bit depth format for this logical format as well.
			case NLS::Render::RHI::TextureFormat::Depth24Stencil8: return VK_FORMAT_D32_SFLOAT;
			case NLS::Render::RHI::TextureFormat::BC1: return srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
			case NLS::Render::RHI::TextureFormat::BC3: return srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
			case NLS::Render::RHI::TextureFormat::BC5: return VK_FORMAT_BC5_UNORM_BLOCK;
			case NLS::Render::RHI::TextureFormat::BC7: return srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
			case NLS::Render::RHI::TextureFormat::BC6H: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
			default: return VK_FORMAT_R8G8B8A8_UNORM;
			}
		}
#endif

		// Helper class to wrap a swapchain backbuffer as a texture view
		class NativeVulkanSwapchainBackbufferView final : public NLS::Render::RHI::RHITextureView
		{
		public:
			NativeVulkanSwapchainBackbufferView(VkDevice device, VkImage image, VkImageView imageView, VkFormat format, uint32_t width, uint32_t height)
				: m_device(device)
				, m_image(image)
				, m_imageView(imageView)
				, m_format(format)
			{
				m_desc.format = NLS::Render::RHI::TextureFormat::RGBA8;
				m_desc.viewType = NLS::Render::RHI::TextureViewType::Texture2D;
				m_desc.subresourceRange.baseMipLevel = 0;
				m_desc.subresourceRange.mipLevelCount = 1;
				m_desc.subresourceRange.baseArrayLayer = 0;
				m_desc.subresourceRange.arrayLayerCount = 1;

				// Create a minimal texture wrapper that provides GetNativeImageHandle()
				m_texture = std::make_shared<NativeVulkanSwapchainTexture>(device, image, format, width, height);
			}

			~NativeVulkanSwapchainBackbufferView()
			{
#if NLS_HAS_VULKAN
				if (m_device != nullptr && m_imageView != VK_NULL_HANDLE)
					vkDestroyImageView(m_device, m_imageView, nullptr);
#endif
			}

			std::string_view GetDebugName() const override { return "SwapchainBackbufferView"; }
			const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }

			const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override
			{
				return m_texture;
			}

			NLS::Render::RHI::NativeHandle GetNativeRenderTargetView() override
			{
				return { NLS::Render::RHI::BackendType::Vulkan, reinterpret_cast<void*>(m_imageView) };
			}

			NLS::Render::RHI::NativeHandle GetNativeDepthStencilView() override
			{
				return { NLS::Render::RHI::BackendType::Vulkan, nullptr };
			}

			NLS::Render::RHI::NativeHandle GetNativeShaderResourceView() override
			{
				return { NLS::Render::RHI::BackendType::Vulkan, reinterpret_cast<void*>(m_imageView) };
			}

			VkImage GetImage() const { return m_image; }
			VkFormat GetFormat() const { return m_format; }

		private:
			VkDevice m_device = nullptr;
			VkImage m_image = VK_NULL_HANDLE;
			VkImageView m_imageView = VK_NULL_HANDLE;
			VkFormat m_format = VK_FORMAT_UNDEFINED;
			NLS::Render::RHI::RHITextureViewDesc m_desc{};
			std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
		};

		class NativeVulkanSwapchain final : public NLS::Render::RHI::RHISwapchain
		{
		public:
			NativeVulkanSwapchain(VkDevice device, VkQueue queue, VkSwapchainKHR swapchain, VkSurfaceKHR surface, const NLS::Render::RHI::SwapchainDesc& desc, VkFormat imageFormat)
				: m_device(device)
				, m_queue(queue)
				, m_swapchain(swapchain)
				, m_surface(surface)
				, m_desc(desc)
				, m_imageCount(desc.imageCount > 0 ? desc.imageCount : 2)
				, m_imageFormat(imageFormat)
			{
				// Don't fetch images here - physical device may not be set yet.
				// Images will be fetched lazily when GetBackbufferView is first called.
			}

			~NativeVulkanSwapchain()
			{
#if NLS_HAS_VULKAN
				if (m_device != nullptr && m_swapchain != nullptr)
					vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
#endif
			}

			std::string_view GetDebugName() const override { return "NativeVulkanSwapchain"; }
			const NLS::Render::RHI::SwapchainDesc& GetDesc() const override { return m_desc; }
			uint32_t GetImageCount() const override { return m_imageCount; }
			std::optional<NLS::Render::RHI::RHIAcquiredImage> AcquireNextImage(
				const std::shared_ptr<NLS::Render::RHI::RHISemaphore>& signalSemaphore,
				const std::shared_ptr<NLS::Render::RHI::RHIFence>& signalFence) override
			{
#if NLS_HAS_VULKAN
				if (m_device == nullptr || m_swapchain == nullptr)
					return std::nullopt;

				// Acquire semaphores and swapchain images are binary/exclusive resources.
				// Serialize the WSL baseline path so a previous present has completed before
				// the frame slot's semaphore and image are reused.
				if (m_queue != VK_NULL_HANDLE)
					(void)vkQueueWaitIdle(m_queue);

				// Wait on fence if provided (same pattern as DX12)
				if (signalFence != nullptr)
					signalFence->Wait(5000000000ULL); // 5 second timeout

				// Get Vulkan semaphore handle if provided
				VkSemaphore vkSemaphore = VK_NULL_HANDLE;
				if (signalSemaphore != nullptr)
				{
					auto* nativeSemaphore = static_cast<NativeVulkanSemaphore*>(signalSemaphore.get());
					if (nativeSemaphore != nullptr && nativeSemaphore->Recreate())
						vkSemaphore = nativeSemaphore->GetSemaphore();
				}

				// Actually acquire the next image from the swapchain
				uint32_t imageIndex = 0;
					// The WSLg/Mesa surface can report minImageCount == imageCount - 1.
					// In that case an infinite acquire timeout is invalid while one image
					// remains acquired. A non-blocking acquire lets the frame scheduler
					// retry without tripping VUID-vkAcquireNextImageKHR-swapchain-01802.
					VkResult result = vkAcquireNextImageKHR(
					m_device,
					m_swapchain,
					0u,
					vkSemaphore,
					VK_NULL_HANDLE, // no fence in AcquireNextImage (we use the one in Driver)
					&imageIndex);

				if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
					return std::nullopt;

				NLS::Render::RHI::RHIAcquiredImage image;
				image.imageIndex = imageIndex;
				image.suboptimal = result == VK_SUBOPTIMAL_KHR;
				image.imageView = GetBackbufferView(imageIndex);
				return image;
#else
				return std::nullopt;
#endif
			}
			std::shared_ptr<NLS::Render::RHI::RHITextureView> GetBackbufferView(uint32_t index) override
			{
#if NLS_HAS_VULKAN
				if (m_swapchain == nullptr || m_device == nullptr)
					return nullptr;

				// Refresh images if needed
				if (m_swapchainImages.empty())
					FetchSwapchainImages();

				if (index >= m_swapchainImages.size())
					return nullptr;

				// Create view if not already created for this index
				if (index >= m_backbufferViews.size())
				{
					CreateBackbufferViews();
				}

				if (index < m_backbufferViews.size())
					return m_backbufferViews[index];
#endif
				return nullptr;
			}
			bool Resize(uint32_t width, uint32_t height) override
			{
#if NLS_HAS_VULKAN
				if (m_device == nullptr || m_surface == nullptr)
					return false;

				// Get surface capabilities
				VkSurfaceCapabilitiesKHR surfaceCapabilities{};
				if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &surfaceCapabilities) != VK_SUCCESS)
					return false;

				// Get surface formats
				uint32_t formatCount = 0;
				if (vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, nullptr) != VK_SUCCESS || formatCount == 0)
					return false;
				std::vector<VkSurfaceFormatKHR> formats(formatCount);
				if (vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, formats.data()) != VK_SUCCESS)
					return false;

				// Get present modes
				uint32_t presentModeCount = 0;
				if (vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &presentModeCount, nullptr) != VK_SUCCESS)
					return false;
				std::vector<VkPresentModeKHR> presentModes(presentModeCount);
				if (vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &presentModeCount, presentModes.data()) != VK_SUCCESS)
					return false;

				// Determine extent
				VkExtent2D extent{};
				extent.width = width > 0 ? width : surfaceCapabilities.currentExtent.width;
				extent.height = height > 0 ? height : surfaceCapabilities.currentExtent.height;

				// Determine image count
				uint32_t imageCount = std::max(surfaceCapabilities.minImageCount, m_imageCount);
				if (surfaceCapabilities.maxImageCount > 0)
					imageCount = std::min(imageCount, surfaceCapabilities.maxImageCount);

				// Determine present mode
				VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
				for (auto mode : presentModes)
				{
					if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
					{
						presentMode = mode;
						break;
					}
					if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
						presentMode = mode;
				}

				// Destroy old swapchain
				VkSwapchainKHR oldSwapchain = m_swapchain;

				// Create new swapchain
				VkSwapchainCreateInfoKHR createInfo{};
				createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
				createInfo.surface = m_surface;
				createInfo.minImageCount = imageCount;
				VkSurfaceFormatKHR selectedFormat = formats[0];
				for (const auto& candidate : formats)
				{
					if (candidate.format == m_imageFormat)
					{
						selectedFormat = candidate;
						break;
					}
				}
				createInfo.imageFormat = selectedFormat.format;
				createInfo.imageColorSpace = selectedFormat.colorSpace;
				createInfo.imageExtent = extent;
				createInfo.imageArrayLayers = 1;
				createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
				createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
				createInfo.preTransform = surfaceCapabilities.currentTransform;
				createInfo.compositeAlpha = (surfaceCapabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0
					? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
					: (surfaceCapabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) != 0
						? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
						: VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
				createInfo.presentMode = presentMode;
				createInfo.clipped = VK_TRUE;
				createInfo.oldSwapchain = oldSwapchain;

				VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
				if (vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &newSwapchain) != VK_SUCCESS)
					return false;

				m_swapchain = newSwapchain;
				m_imageFormat = selectedFormat.format;
				m_desc.width = extent.width;
				m_desc.height = extent.height;
				m_imageCount = imageCount;

				// Clear old views and fetch new images
				m_backbufferViews.clear();
				m_swapchainImages.clear();
				FetchSwapchainImages();

				// Destroy old swapchain after creating new one
				if (oldSwapchain != VK_NULL_HANDLE)
					vkDestroySwapchainKHR(m_device, oldSwapchain, nullptr);
				return true;
#endif
				return false;
			}

			NLS::Render::RHI::NativeHandle GetNativeSwapchainHandle() override
			{
				return { NLS::Render::RHI::BackendType::Vulkan, reinterpret_cast<void*>(m_swapchain) };
			}

#if NLS_HAS_VULKAN
			VkSwapchainKHR GetSwapchain() const { return m_swapchain; }
			VkSurfaceKHR GetSurface() const { return m_surface; }
			VkDevice GetDevice() const { return m_device; }
			VkPhysicalDevice GetPhysicalDevice() const { return m_physicalDevice; }
			void SetPhysicalDevice(VkPhysicalDevice physicalDevice)
			{
				m_physicalDevice = physicalDevice;
				FetchSwapchainImages();
				CreateBackbufferViews();
			}
#endif

		private:
			VkDevice m_device = nullptr;
			VkQueue m_queue = VK_NULL_HANDLE;
			VkPhysicalDevice m_physicalDevice = nullptr;
			VkSwapchainKHR m_swapchain = nullptr;
			VkSurfaceKHR m_surface = nullptr;
			NLS::Render::RHI::SwapchainDesc m_desc{};
			uint32_t m_imageCount = 2;
			uint32_t m_nextImageIndex = 0;
			VkFormat m_imageFormat = VK_FORMAT_UNDEFINED;
			std::vector<VkImage> m_swapchainImages;
			std::vector<std::shared_ptr<NativeVulkanSwapchainBackbufferView>> m_backbufferViews;

			void FetchSwapchainImages()
			{
#if NLS_HAS_VULKAN
				if (m_device == nullptr || m_swapchain == nullptr)
					return;

				uint32_t imageCount = 0;
				if (vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, nullptr) != VK_SUCCESS)
					return;

				m_swapchainImages.resize(imageCount);
				if (vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, m_swapchainImages.data()) != VK_SUCCESS)
				{
					m_swapchainImages.clear();
					return;
				}

				m_imageCount = imageCount;

				// Get the surface format to use for views
				// Only query if physical device is available (it may not be set yet during construction)
				if (m_surface != nullptr && m_physicalDevice != VK_NULL_HANDLE)
				{
					uint32_t formatCount = 0;
					if (vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, nullptr) == VK_SUCCESS && formatCount > 0)
					{
						std::vector<VkSurfaceFormatKHR> formats(formatCount);
						if (vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, formats.data()) == VK_SUCCESS)
						{
							if (m_imageFormat == VK_FORMAT_UNDEFINED)
								m_imageFormat = formats[0].format;
						}
					}
				}
				if (m_imageFormat == VK_FORMAT_UNDEFINED)
					m_imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
#endif
			}

			void CreateBackbufferViews()
			{
#if NLS_HAS_VULKAN
				if (m_device == nullptr || m_swapchainImages.empty())
					return;

				m_backbufferViews.clear();

				for (size_t i = 0; i < m_swapchainImages.size(); ++i)
				{
					VkImageViewCreateInfo viewInfo{};
					viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
					viewInfo.image = m_swapchainImages[i];
					viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
					viewInfo.format = m_imageFormat;
					viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
					viewInfo.subresourceRange.baseMipLevel = 0;
					viewInfo.subresourceRange.levelCount = 1;
					viewInfo.subresourceRange.baseArrayLayer = 0;
					viewInfo.subresourceRange.layerCount = 1;

					VkImageView imageView = VK_NULL_HANDLE;
					if (vkCreateImageView(m_device, &viewInfo, nullptr, &imageView) == VK_SUCCESS)
					{
						auto view = std::make_shared<NativeVulkanSwapchainBackbufferView>(
							m_device, m_swapchainImages[i], imageView, m_imageFormat, m_desc.width, m_desc.height);
						m_backbufferViews.push_back(view);
					}
				}
#endif
			}
		};

		class NativeVulkanBuffer final : public NLS::Render::RHI::RHIBuffer
		{
		public:
			NativeVulkanBuffer(VkDevice device, VmaAllocator allocator, const NLS::Render::RHI::RHIBufferDesc& desc, const NLS::Render::RHI::RHIBufferUploadDesc& initialData)
				: m_device(device)
				, m_allocator(allocator)
				, m_desc(desc)
			{
#if NLS_HAS_VULKAN
				if (device == nullptr || allocator == VK_NULL_HANDLE)
					return;
				if (initialData.HasData() && initialData.destinationOffset + initialData.dataSize > desc.size)
					return;

				VkBufferCreateInfo bufferInfo{};
				bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
				bufferInfo.size = desc.size;
				bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
				if (NLS::Render::RHI::HasBufferUsage(desc.usage, NLS::Render::RHI::BufferUsageFlags::Vertex))
					bufferInfo.usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
				if (NLS::Render::RHI::HasBufferUsage(desc.usage, NLS::Render::RHI::BufferUsageFlags::Index))
					bufferInfo.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
				if (NLS::Render::RHI::HasBufferUsage(desc.usage, NLS::Render::RHI::BufferUsageFlags::Uniform))
					bufferInfo.usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
				if (NLS::Render::RHI::HasBufferUsage(desc.usage, NLS::Render::RHI::BufferUsageFlags::Storage))
					bufferInfo.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
				if (NLS::Render::RHI::HasBufferUsage(desc.usage, NLS::Render::RHI::BufferUsageFlags::Indirect))
					bufferInfo.usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
				bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

				const auto allocationUsage = initialData.HasData()
					? NLS::Render::RHI::MemoryUsage::CPUToGPU
					: desc.memoryUsage;
				VmaAllocationCreateInfo allocationInfo{};
				allocationInfo.usage = ToVmaMemoryUsage(allocationUsage);
				if (initialData.HasData())
					allocationInfo.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
				const VkResult result = vmaCreateBuffer(
					allocator,
					&bufferInfo,
					&allocationInfo,
					&m_buffer,
					&m_allocation,
					nullptr);
			if (result != VK_SUCCESS)
				return;

				// Copy initial data if provided
				if (initialData.HasData())
				{
					void* mappedData = nullptr;
					if (vmaMapMemory(allocator, m_allocation, &mappedData) == VK_SUCCESS && mappedData != nullptr)
					{
						std::memcpy(static_cast<uint8_t*>(mappedData) + initialData.destinationOffset, initialData.data, initialData.dataSize);
						vmaFlushAllocation(allocator, m_allocation, initialData.destinationOffset, initialData.dataSize);
						vmaUnmapMemory(allocator, m_allocation);
					}
				}
#endif
			}

			~NativeVulkanBuffer()
			{
#if NLS_HAS_VULKAN
				if (m_allocator != VK_NULL_HANDLE && m_buffer != VK_NULL_HANDLE)
				{
					vmaDestroyBuffer(m_allocator, m_buffer, m_allocation);
				}
#endif
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHIBufferDesc& GetDesc() const override { return m_desc; }
			NLS::Render::RHI::ResourceState GetState() const override { return m_state; }
			uint64_t GetGPUAddress() const override { return 0; } // Vulkan doesn't use GPU addresses like DX12
			NLS::Render::RHI::NativeHandle GetNativeBufferHandle() override { return { NLS::Render::RHI::BackendType::Vulkan, reinterpret_cast<void*>(m_buffer) }; }
			NLS::Render::RHI::RHIUpdateResult UpdateData(const NLS::Render::RHI::RHIBufferUploadDesc& uploadDesc) override
			{
#if NLS_HAS_VULKAN
				if (m_allocator == VK_NULL_HANDLE || m_allocation == VK_NULL_HANDLE || !uploadDesc.HasData() ||
					uploadDesc.destinationOffset + uploadDesc.dataSize > m_desc.size)
					return { NLS::Render::RHI::RHIUpdateStatusCode::InvalidArgument, "Vulkan buffer update is out of bounds" };
				void* mappedData = nullptr;
				if (vmaMapMemory(m_allocator, m_allocation, &mappedData) != VK_SUCCESS || mappedData == nullptr)
					return { NLS::Render::RHI::RHIUpdateStatusCode::BackendFailure, "Vulkan buffer memory is not host visible" };
				std::memcpy(static_cast<uint8_t*>(mappedData) + uploadDesc.destinationOffset, uploadDesc.data, uploadDesc.dataSize);
				vmaFlushAllocation(m_allocator, m_allocation, uploadDesc.destinationOffset, uploadDesc.dataSize);
				vmaUnmapMemory(m_allocator, m_allocation);
				return { NLS::Render::RHI::RHIUpdateStatusCode::Success, {} };
#else
				(void)uploadDesc;
				return { NLS::Render::RHI::RHIUpdateStatusCode::Unsupported, "Vulkan is not compiled" };
#endif
			}

		private:
			VkDevice m_device = nullptr;
			VmaAllocator m_allocator = VK_NULL_HANDLE;
			NLS::Render::RHI::RHIBufferDesc m_desc{};
			NLS::Render::RHI::ResourceState m_state = NLS::Render::RHI::ResourceState::Unknown;
#if NLS_HAS_VULKAN
			VkBuffer m_buffer = VK_NULL_HANDLE;
			VmaAllocation m_allocation = VK_NULL_HANDLE;
#endif
		};

		class NativeVulkanTexture final : public NLS::Render::RHI::RHITexture
		{
		public:
			NativeVulkanTexture(VkDevice device, VmaAllocator allocator, const NLS::Render::RHI::RHITextureDesc& desc, const NLS::Render::RHI::RHITextureUploadDesc&)
				: m_device(device)
				, m_allocator(allocator)
				, m_desc(desc)
			{
#if NLS_HAS_VULKAN
				if (device == nullptr || allocator == VK_NULL_HANDLE)
					return;

				VkImageType imageType = VK_IMAGE_TYPE_2D;
				if (desc.dimension == NLS::Render::RHI::TextureDimension::TextureCube)
					imageType = VK_IMAGE_TYPE_2D;

				VkImageCreateInfo imageInfo{};
				imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
				imageInfo.imageType = imageType;
				imageInfo.extent.width = desc.extent.width;
				imageInfo.extent.height = desc.extent.height;
				imageInfo.extent.depth = 1;
				imageInfo.arrayLayers = (desc.dimension == NLS::Render::RHI::TextureDimension::TextureCube) ? 6 : std::max(1u, desc.arrayLayers);
				imageInfo.mipLevels = std::max(1u, desc.mipLevels);
				imageInfo.format = ToVkFormat(desc.format, desc.colorSpace);
				imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
				imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				imageInfo.usage = ToVkImageUsage(desc.usage) | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
				imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
				imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

				if (desc.dimension == NLS::Render::RHI::TextureDimension::TextureCube)
					imageInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

				VmaAllocationCreateInfo allocationInfo{};
				allocationInfo.usage = ToVmaMemoryUsage(desc.memoryUsage);
				const VkResult result = vmaCreateImage(
					allocator,
					&imageInfo,
					&allocationInfo,
					&m_image,
					&m_allocation,
					nullptr);
				if (result != VK_SUCCESS)
					return;
#endif
			}

			~NativeVulkanTexture()
			{
#if NLS_HAS_VULKAN
				if (m_allocator != VK_NULL_HANDLE && m_image != VK_NULL_HANDLE)
				{
					vmaDestroyImage(m_allocator, m_image, m_allocation);
				}
#endif
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
			NLS::Render::RHI::ResourceState GetState() const override
			{
				return m_state.load(std::memory_order_acquire);
			}
			NLS::Render::RHI::NativeHandle GetNativeImageHandle() override { return { NLS::Render::RHI::BackendType::Vulkan, reinterpret_cast<void*>(m_image) }; }

#if NLS_HAS_VULKAN
			VkImage GetImage() const { return m_image; }
			VkFormat GetFormat() const { return ToVkFormat(m_desc.format, m_desc.colorSpace); }
			void SetState(const NLS::Render::RHI::ResourceState state)
			{
				m_state.store(state, std::memory_order_release);
			}
#endif

#if NLS_HAS_VULKAN
			static VkFormat ToVkFormat(
				NLS::Render::RHI::TextureFormat format,
				NLS::Render::RHI::TextureColorSpace colorSpace = NLS::Render::RHI::TextureColorSpace::Linear)
			{
				const bool srgb = colorSpace == NLS::Render::RHI::TextureColorSpace::SRGB;
				switch (format)
				{
				case NLS::Render::RHI::TextureFormat::RGBA8:
				case NLS::Render::RHI::TextureFormat::RGB8: return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
				case NLS::Render::RHI::TextureFormat::R8: return VK_FORMAT_R8_UNORM;
				case NLS::Render::RHI::TextureFormat::RG8: return VK_FORMAT_R8G8_UNORM;
				case NLS::Render::RHI::TextureFormat::R16F: return VK_FORMAT_R16_SFLOAT;
				case NLS::Render::RHI::TextureFormat::RG16F: return VK_FORMAT_R16G16_SFLOAT;
				case NLS::Render::RHI::TextureFormat::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
				case NLS::Render::RHI::TextureFormat::R32F: return VK_FORMAT_R32_SFLOAT;
				case NLS::Render::RHI::TextureFormat::RG32F: return VK_FORMAT_R32G32_SFLOAT;
				case NLS::Render::RHI::TextureFormat::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
				case NLS::Render::RHI::TextureFormat::BC1: return srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
				case NLS::Render::RHI::TextureFormat::BC3: return srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
				case NLS::Render::RHI::TextureFormat::BC5: return VK_FORMAT_BC5_UNORM_BLOCK;
				case NLS::Render::RHI::TextureFormat::BC7: return srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
				case NLS::Render::RHI::TextureFormat::BC6H: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
				case NLS::Render::RHI::TextureFormat::Depth32F: return VK_FORMAT_D32_SFLOAT;
				case NLS::Render::RHI::TextureFormat::Depth24Stencil8: return VK_FORMAT_D32_SFLOAT;
				default: return VK_FORMAT_R8G8B8A8_UNORM;
				}
			}

			static VkImageUsageFlags ToVkImageUsage(NLS::Render::RHI::TextureUsageFlags usage)
			{
				VkImageUsageFlags flags = 0;
				if (static_cast<uint32_t>(usage) & static_cast<uint32_t>(NLS::Render::RHI::TextureUsageFlags::Sampled))
					flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
				if (static_cast<uint32_t>(usage) & static_cast<uint32_t>(NLS::Render::RHI::TextureUsageFlags::ColorAttachment))
					flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
				if (static_cast<uint32_t>(usage) & static_cast<uint32_t>(NLS::Render::RHI::TextureUsageFlags::DepthStencilAttachment))
					flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
				if (static_cast<uint32_t>(usage) & static_cast<uint32_t>(NLS::Render::RHI::TextureUsageFlags::Storage))
					flags |= VK_IMAGE_USAGE_STORAGE_BIT;
				if (static_cast<uint32_t>(usage) & static_cast<uint32_t>(NLS::Render::RHI::TextureUsageFlags::CopySrc))
					flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
				if (static_cast<uint32_t>(usage) & static_cast<uint32_t>(NLS::Render::RHI::TextureUsageFlags::CopyDst))
					flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
				return flags;
			}
#endif

		private:
			VkDevice m_device = nullptr;
			VmaAllocator m_allocator = VK_NULL_HANDLE;
			NLS::Render::RHI::RHITextureDesc m_desc{};
			std::atomic<NLS::Render::RHI::ResourceState> m_state {
				NLS::Render::RHI::ResourceState::Unknown
			};
#if NLS_HAS_VULKAN
			VkImage m_image = VK_NULL_HANDLE;
			VmaAllocation m_allocation = VK_NULL_HANDLE;
#endif
		};

#if NLS_HAS_VULKAN
		void SetVulkanTextureLogicalState(
			const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
			const NLS::Render::RHI::ResourceState state)
		{
			if (auto* nativeTexture = dynamic_cast<NativeVulkanTexture*>(texture.get()))
				nativeTexture->SetState(state);
		}
#endif

		class NativeVulkanTextureView final : public NLS::Render::RHI::RHITextureView
		{
		public:
			NativeVulkanTextureView(VkDevice device, const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture, const NLS::Render::RHI::RHITextureViewDesc& desc)
				: m_device(device)
				, m_texture(texture)
				, m_desc(desc)
			{
#if NLS_HAS_VULKAN
				if (device == nullptr || texture == nullptr)
					return;

				// Get the native image handle from the texture
				auto imgHandle = texture->GetNativeImageHandle();
				VkImage image = (imgHandle.backend == NLS::Render::RHI::BackendType::Vulkan) ? static_cast<VkImage>(imgHandle.handle) : VK_NULL_HANDLE;
				if (image == VK_NULL_HANDLE)
					return;

				// Create image view
				VkImageViewCreateInfo viewInfo{};
				viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
				viewInfo.image = image;
				const auto textureDimension = texture->GetDesc().dimension;
				const bool isCube = desc.viewType == NLS::Render::RHI::TextureViewType::Cube ||
					(textureDimension == NLS::Render::RHI::TextureDimension::TextureCube &&
						desc.viewType == NLS::Render::RHI::TextureViewType::Auto);
				viewInfo.viewType = isCube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
				const auto& textureDesc = texture->GetDesc();
				const auto textureFormat = textureDesc.format;
				const bool usesDefaultViewFormat =
					desc.format == NLS::Render::RHI::TextureFormat::RGBA8 &&
					desc.colorSpace == NLS::Render::RHI::TextureColorSpace::Linear;
				const auto effectiveFormat = usesDefaultViewFormat ? textureFormat : desc.format;
				const auto effectiveColorSpace = usesDefaultViewFormat ? textureDesc.colorSpace : desc.colorSpace;
				viewInfo.format = ToVkFormat(effectiveFormat, effectiveColorSpace);
				viewInfo.subresourceRange.aspectMask =
					(textureFormat == NLS::Render::RHI::TextureFormat::Depth24Stencil8 ||
					 textureFormat == NLS::Render::RHI::TextureFormat::Depth32F)
						? VK_IMAGE_ASPECT_DEPTH_BIT
						: VK_IMAGE_ASPECT_COLOR_BIT;
				viewInfo.subresourceRange.baseMipLevel = desc.subresourceRange.baseMipLevel;
				viewInfo.subresourceRange.levelCount = desc.subresourceRange.mipLevelCount != 0u
					? desc.subresourceRange.mipLevelCount
					: std::max(1u, texture->GetDesc().mipLevels);
				viewInfo.subresourceRange.baseArrayLayer = desc.subresourceRange.baseArrayLayer;
				viewInfo.subresourceRange.layerCount = isCube
					? 6u
					: (desc.subresourceRange.arrayLayerCount != 0u
						? desc.subresourceRange.arrayLayerCount
						: std::max(1u, texture->GetDesc().arrayLayers));

				if (vkCreateImageView(device, &viewInfo, nullptr, &m_imageView) != VK_SUCCESS)
					return;

				// A sampled depth/stencil image must select one aspect. Keep the
				// combined view for attachment use and expose a depth-only view to
				// descriptor updates.
				if (textureFormat == NLS::Render::RHI::TextureFormat::Depth24Stencil8 ||
					textureFormat == NLS::Render::RHI::TextureFormat::Depth32F)
				{
					VkImageViewCreateInfo shaderViewInfo = viewInfo;
					shaderViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
					if (vkCreateImageView(device, &shaderViewInfo, nullptr, &m_shaderResourceView) != VK_SUCCESS)
						m_shaderResourceView = m_imageView;
				}
				else
				{
					m_shaderResourceView = m_imageView;
				}
#endif
			}

			~NativeVulkanTextureView()
			{
#if NLS_HAS_VULKAN
				if (m_device != nullptr && m_shaderResourceView != VK_NULL_HANDLE && m_shaderResourceView != m_imageView)
					vkDestroyImageView(m_device, m_shaderResourceView, nullptr);
				if (m_device != nullptr && m_imageView != VK_NULL_HANDLE)
					vkDestroyImageView(m_device, m_imageView, nullptr);
#endif
			}

#if NLS_HAS_VULKAN
			static VkFormat ToVkFormat(
				NLS::Render::RHI::TextureFormat format,
				NLS::Render::RHI::TextureColorSpace colorSpace = NLS::Render::RHI::TextureColorSpace::Linear)
			{
				const bool srgb = colorSpace == NLS::Render::RHI::TextureColorSpace::SRGB;
				switch (format)
				{
				case NLS::Render::RHI::TextureFormat::RGBA8:
				case NLS::Render::RHI::TextureFormat::RGB8: return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
				case NLS::Render::RHI::TextureFormat::R8: return VK_FORMAT_R8_UNORM;
				case NLS::Render::RHI::TextureFormat::RG8: return VK_FORMAT_R8G8_UNORM;
				case NLS::Render::RHI::TextureFormat::R16F: return VK_FORMAT_R16_SFLOAT;
				case NLS::Render::RHI::TextureFormat::RG16F: return VK_FORMAT_R16G16_SFLOAT;
				case NLS::Render::RHI::TextureFormat::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
				case NLS::Render::RHI::TextureFormat::R32F: return VK_FORMAT_R32_SFLOAT;
				case NLS::Render::RHI::TextureFormat::RG32F: return VK_FORMAT_R32G32_SFLOAT;
				case NLS::Render::RHI::TextureFormat::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
				case NLS::Render::RHI::TextureFormat::BC1: return srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
				case NLS::Render::RHI::TextureFormat::BC3: return srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
				case NLS::Render::RHI::TextureFormat::BC5: return VK_FORMAT_BC5_UNORM_BLOCK;
				case NLS::Render::RHI::TextureFormat::BC7: return srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
				case NLS::Render::RHI::TextureFormat::BC6H: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
				case NLS::Render::RHI::TextureFormat::Depth32F: return VK_FORMAT_D32_SFLOAT;
				case NLS::Render::RHI::TextureFormat::Depth24Stencil8: return VK_FORMAT_D32_SFLOAT;
				default: return VK_FORMAT_R8G8B8A8_UNORM;
				}
			}
#endif

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
			const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override { return m_texture; }
			NLS::Render::RHI::NativeHandle GetNativeRenderTargetView() override { return { NLS::Render::RHI::BackendType::Vulkan, reinterpret_cast<void*>(m_imageView) }; }
			NLS::Render::RHI::NativeHandle GetNativeShaderResourceView() override { return { NLS::Render::RHI::BackendType::Vulkan, reinterpret_cast<void*>(m_shaderResourceView) }; }

#if NLS_HAS_VULKAN
			VkImageView GetImageView() const { return m_imageView; }
#endif

		private:
			VkDevice m_device = nullptr;
			std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
			NLS::Render::RHI::RHITextureViewDesc m_desc{};
#if NLS_HAS_VULKAN
			VkImageView m_imageView = VK_NULL_HANDLE;
			VkImageView m_shaderResourceView = VK_NULL_HANDLE;
#endif
		};

		class NativeVulkanSampler final : public NLS::Render::RHI::RHISampler
		{
		public:
			NativeVulkanSampler(VkDevice device, const NLS::Render::RHI::SamplerDesc& desc, const std::string& debugName)
				: m_device(device)
				, m_desc(desc)
				, m_debugName(debugName)
			{
#if NLS_HAS_VULKAN
				if (device == nullptr)
					return;

				VkSamplerCreateInfo samplerInfo{};
				samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
				samplerInfo.magFilter = ToVkFilter(desc.magFilter);
				samplerInfo.minFilter = ToVkFilter(desc.minFilter);
				samplerInfo.mipmapMode = desc.mipFilter == NLS::Render::RHI::TextureMipFilter::Nearest
					? VK_SAMPLER_MIPMAP_MODE_NEAREST
					: VK_SAMPLER_MIPMAP_MODE_LINEAR;
				samplerInfo.addressModeU = ToVkSamplerAddressMode(desc.wrapU);
				samplerInfo.addressModeV = ToVkSamplerAddressMode(desc.wrapV);
				samplerInfo.addressModeW = ToVkSamplerAddressMode(desc.wrapW);
				samplerInfo.mipLodBias = desc.mipLodBias;
				samplerInfo.anisotropyEnable = VK_FALSE;
				samplerInfo.maxAnisotropy = 1.0f;
				samplerInfo.compareEnable = desc.compareEnabled ? VK_TRUE : VK_FALSE;
				samplerInfo.compareOp = desc.compareEnabled ? ToVkCompareOp(desc.compareFunc) : VK_COMPARE_OP_ALWAYS;
				samplerInfo.minLod = desc.minLod;
				samplerInfo.maxLod = desc.maxLod;
				samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;

				vkCreateSampler(device, &samplerInfo, nullptr, &m_sampler);
#endif
			}

			~NativeVulkanSampler()
			{
#if NLS_HAS_VULKAN
				if (m_device != nullptr && m_sampler != VK_NULL_HANDLE)
					vkDestroySampler(m_device, m_sampler, nullptr);
#endif
			}

			std::string_view GetDebugName() const override { return m_debugName; }
			const NLS::Render::RHI::SamplerDesc& GetDesc() const override { return m_desc; }
			NLS::Render::RHI::NativeHandle GetNativeSamplerHandle() override { return { NLS::Render::RHI::BackendType::Vulkan, reinterpret_cast<void*>(m_sampler) }; }

#if NLS_HAS_VULKAN
			VkSampler GetSampler() const { return m_sampler; }
#endif

		private:
			NLS::Render::RHI::SamplerDesc m_desc{};
			std::string m_debugName;
			VkDevice m_device = nullptr;
#if NLS_HAS_VULKAN
			VkSampler m_sampler = VK_NULL_HANDLE;
#endif

#if NLS_HAS_VULKAN
			static VkFilter ToVkFilter(NLS::Render::RHI::TextureFilter filter)
			{
				return filter == NLS::Render::RHI::TextureFilter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
			}

			static VkSamplerAddressMode ToVkSamplerAddressMode(NLS::Render::RHI::TextureWrap wrap)
			{
				switch (wrap)
				{
				case NLS::Render::RHI::TextureWrap::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
				case NLS::Render::RHI::TextureWrap::MirrorRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
				case NLS::Render::RHI::TextureWrap::ClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
				case NLS::Render::RHI::TextureWrap::Repeat:
				default:
					return VK_SAMPLER_ADDRESS_MODE_REPEAT;
				}
			}
#endif
		};

		class NativeVulkanBindingLayout final : public NLS::Render::RHI::RHIBindingLayout
		{
		public:
			explicit NativeVulkanBindingLayout(VkDevice device, NLS::Render::RHI::RHIBindingLayoutDesc desc)
				: m_device(device)
				, m_desc(std::move(desc))
			{
#if NLS_HAS_VULKAN
				if (m_device == nullptr)
					return;

				std::vector<VkDescriptorSetLayoutBinding> vkBindings;
				vkBindings.reserve(m_desc.entries.size());

				for (const auto& entry : m_desc.entries)
				{
					NLS_LOG_INFO(
						"NativeVulkanBindingLayout: " + m_desc.debugName +
						" binding=" + std::to_string(entry.binding) +
						" type=" + std::to_string(static_cast<uint32_t>(entry.type)) +
						" count=" + std::to_string(entry.count));
					VkDescriptorSetLayoutBinding binding{};
					binding.binding = ToVulkanDescriptorBinding(entry.type, entry.registerSpace, entry.binding);
					binding.descriptorCount = std::max(1u, entry.count);
					// Descriptor sets are shared between pipelines and the common RHI
					// may bind a set created from a broader stage declaration. Vulkan
					// requires exact layout compatibility, so use the superset here.
					binding.stageFlags = VK_SHADER_STAGE_ALL;

					switch (entry.type)
					{
					case NLS::Render::RHI::BindingType::UniformBuffer:
						binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
						break;
					case NLS::Render::RHI::BindingType::StructuredBuffer:
					case NLS::Render::RHI::BindingType::StorageBuffer:
						binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
						break;
					case NLS::Render::RHI::BindingType::Texture:
						binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
						break;
					case NLS::Render::RHI::BindingType::RWTexture:
						binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
						break;
					case NLS::Render::RHI::BindingType::Sampler:
						binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
						break;
					default:
						binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
						break;
					}

					auto existing = std::find_if(
						vkBindings.begin(),
						vkBindings.end(),
						[&binding](const VkDescriptorSetLayoutBinding& candidate)
						{
							return candidate.binding == binding.binding;
						});
					if (existing != vkBindings.end())
					{
						// A descriptor binding number is unique within a set even
						// when reflection reports it from multiple source resources.
						existing->descriptorCount = std::max(existing->descriptorCount, binding.descriptorCount);
						existing->stageFlags |= binding.stageFlags;
						continue;
					}
					vkBindings.push_back(binding);
				}

				// Keep a backend-owned structural signature. Vulkan permits descriptor
				// sets allocated from separately-created layouts when those layouts are
				// identically defined; comparing handles alone would reject that valid
				// case and either skip a required set or bind an incompatible one.
				std::sort(
					vkBindings.begin(),
					vkBindings.end(),
					[](const VkDescriptorSetLayoutBinding& lhs, const VkDescriptorSetLayoutBinding& rhs)
					{
						return lhs.binding < rhs.binding;
					});
				m_signature.clear();
				m_signature.reserve(vkBindings.size());
				for (const auto& binding : vkBindings)
				{
					m_signature.push_back({
						binding.binding,
						binding.descriptorType,
						binding.descriptorCount,
						binding.stageFlags
					});
				}

				VkDescriptorSetLayoutCreateInfo layoutInfo{};
				layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
				layoutInfo.bindingCount = static_cast<uint32_t>(vkBindings.size());
				layoutInfo.pBindings = vkBindings.data();

				const VkResult result = vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout);
				if (result != VK_SUCCESS)
				{
					NLS_LOG_ERROR(
						"NativeVulkanBindingLayout: vkCreateDescriptorSetLayout failed, result=" +
						std::to_string(result));
					m_descriptorSetLayout = VK_NULL_HANDLE;
				}
#endif
			}

			~NativeVulkanBindingLayout()
			{
#if NLS_HAS_VULKAN
				if (m_device != nullptr && m_descriptorSetLayout != VK_NULL_HANDLE)
					vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
#endif
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHIBindingLayoutDesc& GetDesc() const override { return m_desc; }

#if NLS_HAS_VULKAN
			VkDescriptorSetLayout GetDescriptorSetLayout() const { return m_descriptorSetLayout; }
			const VulkanDescriptorLayoutSignature& GetSignature() const { return m_signature; }
			bool IsIdenticallyDefined(const NativeVulkanBindingLayout& other) const
			{
				return m_signature == other.m_signature;
			}
#endif

		private:
			VkDevice m_device = nullptr;
			NLS::Render::RHI::RHIBindingLayoutDesc m_desc;
#if NLS_HAS_VULKAN
			VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
			VulkanDescriptorLayoutSignature m_signature;
#endif

#if NLS_HAS_VULKAN
			static VkShaderStageFlags ToVkShaderStageFlags(NLS::Render::RHI::ShaderStageMask stageMask)
			{
				VkShaderStageFlags flags = 0;
				if (static_cast<uint32_t>(stageMask) & static_cast<uint32_t>(NLS::Render::RHI::ShaderStageMask::Vertex))
					flags |= VK_SHADER_STAGE_VERTEX_BIT;
				if (static_cast<uint32_t>(stageMask) & static_cast<uint32_t>(NLS::Render::RHI::ShaderStageMask::Fragment))
					flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
				if (static_cast<uint32_t>(stageMask) & static_cast<uint32_t>(NLS::Render::RHI::ShaderStageMask::Compute))
					flags |= VK_SHADER_STAGE_COMPUTE_BIT;
				if (flags == 0)
					flags = VK_SHADER_STAGE_ALL_GRAPHICS;
				return flags;
			}
#endif
		};

		class NativeVulkanBindingSet final : public NLS::Render::RHI::RHIBindingSet
		{
		public:
			explicit NativeVulkanBindingSet(VkDevice device, NLS::Render::RHI::RHIBindingSetDesc desc)
				: m_device(device)
				, m_desc(std::move(desc))
			{
#if NLS_HAS_VULKAN
				if (m_device == nullptr || m_desc.layout == nullptr)
					return;

				auto* nativeLayout = dynamic_cast<NativeVulkanBindingLayout*>(m_desc.layout.get());
				if (nativeLayout == nullptr)
					return;

				// Get the descriptor set layout
				m_descriptorSetLayout = nativeLayout->GetDescriptorSetLayout();
				if (m_descriptorSetLayout == VK_NULL_HANDLE)
					return;

				// The pool must reserve every descriptor declared by the layout, even
				// when a caller intentionally leaves an optional resource unwritten.
				// Sizing from only the populated binding-set entries can make allocation
				// fail or leave a layout-backed set unusable.
				std::vector<VkDescriptorPoolSize> poolSizes;
				for (const auto& entry : m_desc.layout->GetDesc().entries)
				{
					const uint32_t descriptorCount = (std::max)(1u, entry.count);
					VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; // Default
					switch (entry.type)
					{
					case NLS::Render::RHI::BindingType::UniformBuffer:
						type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
						break;
					case NLS::Render::RHI::BindingType::StructuredBuffer:
					case NLS::Render::RHI::BindingType::StorageBuffer:
						type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
						break;
					case NLS::Render::RHI::BindingType::Texture:
						type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
						break;
					case NLS::Render::RHI::BindingType::Sampler:
						type = VK_DESCRIPTOR_TYPE_SAMPLER;
						break;
					case NLS::Render::RHI::BindingType::RWTexture:
						type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
						break;
					}

					// Check if we already have this type
					bool found = false;
					for (auto& ps : poolSizes)
					{
						if (ps.type == type)
						{
							ps.descriptorCount += descriptorCount;
							found = true;
							break;
						}
					}
					if (!found)
					{
						poolSizes.push_back({ type, descriptorCount });
					}
				}

				// If no entries, add a default
				if (poolSizes.empty())
				{
					poolSizes.push_back({ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16 });
				}

				// Create descriptor pool with calculated sizes
				VkDescriptorPoolCreateInfo poolInfo{};
				poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
				poolInfo.maxSets = 1;
				poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
				poolInfo.pPoolSizes = poolSizes.data();

				VkResult result = vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool);
				if (result != VK_SUCCESS)
				{
					NLS_LOG_ERROR("NativeVulkanBindingSet: failed to create descriptor pool, result=" + std::to_string(result));
					return;
				}

				// Allocate descriptor set
				VkDescriptorSetAllocateInfo allocInfo{};
				allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
				allocInfo.descriptorPool = m_descriptorPool;
				allocInfo.descriptorSetCount = 1;
				allocInfo.pSetLayouts = &m_descriptorSetLayout;

				result = vkAllocateDescriptorSets(m_device, &allocInfo, &m_descriptorSet);
				if (result != VK_SUCCESS)
				{
					NLS_LOG_ERROR("NativeVulkanBindingSet: failed to allocate descriptor set, result=" + std::to_string(result));
					return;
				}

				// Update descriptor set with bindings
				Update();
#endif
			}

			~NativeVulkanBindingSet()
			{
#if NLS_HAS_VULKAN
				if (m_device != nullptr)
				{
					if (m_descriptorPool != VK_NULL_HANDLE)
						vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
				}
#endif
			}

			void Update()
			{
#if NLS_HAS_VULKAN
				if (m_device == nullptr || m_descriptorSet == VK_NULL_HANDLE)
					return;

				const auto& layoutEntries = m_desc.layout != nullptr
					? m_desc.layout->GetDesc().entries
					: std::vector<NLS::Render::RHI::RHIBindingLayoutEntry>{};
				std::vector<VkWriteDescriptorSet> writes;
				writes.reserve(m_desc.entries.size() + layoutEntries.size());
				std::vector<VkDescriptorBufferInfo> bufferInfos;
				bufferInfos.reserve(m_desc.entries.size() + layoutEntries.size());
				std::vector<VkDescriptorImageInfo> imageInfos;
				imageInfos.reserve(m_desc.entries.size() + layoutEntries.size());
				std::vector<uint32_t> writtenBindings;
				writtenBindings.reserve(m_desc.entries.size() + layoutEntries.size());
				const auto fallbackResources = GetVulkanFallbackResources(m_device);
				const auto fallbackViewForBinding = [&](const uint32_t vulkanBinding)
				{
					for (const auto& layoutEntry : layoutEntries)
					{
						if (ToVulkanDescriptorBinding(layoutEntry.type, layoutEntry.registerSpace, layoutEntry.binding) != vulkanBinding)
							continue;
						const bool isCube = layoutEntry.name.find("Cube") != std::string::npos ||
							layoutEntry.name.find("cube") != std::string::npos ||
							layoutEntry.name.find("Sky") != std::string::npos ||
							layoutEntry.name.find("sky") != std::string::npos;
						if (isCube && fallbackResources.cubeView != VK_NULL_HANDLE)
							return fallbackResources.cubeView;
						break;
					}
					return fallbackResources.view;
				};

				for (const auto& entry : m_desc.entries)
				{
					uint64_t nativeBufferValue = 0u;
					if (entry.buffer != nullptr)
					{
						const auto nativeBuffer = entry.buffer->GetNativeBufferHandle();
						nativeBufferValue = reinterpret_cast<uint64_t>(nativeBuffer.handle);
					}
					NLS_LOG_INFO(
						"NativeVulkanBindingSet: " + m_desc.debugName +
						" binding=" + std::to_string(entry.binding) +
						" type=" + std::to_string(static_cast<uint32_t>(entry.type)) +
						" buffer=" + std::to_string(nativeBufferValue) +
						" offset=" + std::to_string(entry.bufferOffset) +
						" range=" + std::to_string(entry.bufferRange));
					const uint32_t vulkanBinding = ResolveVulkanBinding(entry);
					if (std::find(writtenBindings.begin(), writtenBindings.end(), vulkanBinding) != writtenBindings.end())
						continue;
					VkWriteDescriptorSet write{};
					write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
					write.dstSet = m_descriptorSet;
					write.dstBinding = vulkanBinding;
					write.descriptorCount = 1;

						switch (entry.type)
						{
						case NLS::Render::RHI::BindingType::UniformBuffer:
						case NLS::Render::RHI::BindingType::StructuredBuffer:
						case NLS::Render::RHI::BindingType::StorageBuffer:
						if (entry.buffer != nullptr)
						{
							write.descriptorType = entry.type == NLS::Render::RHI::BindingType::UniformBuffer
								? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
								: VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
							VkDescriptorBufferInfo bufferInfo{};
							auto bufHandle = entry.buffer->GetNativeBufferHandle();
							bufferInfo.buffer = (bufHandle.backend == NLS::Render::RHI::BackendType::Vulkan) ? static_cast<VkBuffer>(bufHandle.handle) : VK_NULL_HANDLE;
							bufferInfo.offset = entry.bufferOffset;
							bufferInfo.range = entry.bufferRange > 0 ? entry.bufferRange : VK_WHOLE_SIZE;
							bufferInfos.push_back(bufferInfo);
							write.pBufferInfo = &bufferInfos.back();
							writes.push_back(write);
							writtenBindings.push_back(vulkanBinding);
						}
						break;
					case NLS::Render::RHI::BindingType::Texture:
						{
							write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
							VkDescriptorImageInfo imageInfo{};
							if (entry.textureView != nullptr)
							{
								auto srvHandle = entry.textureView->GetNativeShaderResourceView();
								if (srvHandle.backend == NLS::Render::RHI::BackendType::Vulkan)
									imageInfo.imageView = static_cast<VkImageView>(srvHandle.handle);
								imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
							}
							if (imageInfo.imageView == VK_NULL_HANDLE)
							{
								imageInfo.imageView = fallbackViewForBinding(vulkanBinding);
								imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
							}
							if (imageInfo.imageView != VK_NULL_HANDLE)
							{
								imageInfos.push_back(imageInfo);
								write.pImageInfo = &imageInfos.back();
								writes.push_back(write);
								writtenBindings.push_back(vulkanBinding);
							}
						}
						break;
					case NLS::Render::RHI::BindingType::Sampler:
						{
							write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
							VkDescriptorImageInfo imageInfo{};
							if (entry.sampler != nullptr)
							{
								auto samplerHandle = entry.sampler->GetNativeSamplerHandle();
								if (samplerHandle.backend == NLS::Render::RHI::BackendType::Vulkan)
									imageInfo.sampler = static_cast<VkSampler>(samplerHandle.handle);
							}
							if (imageInfo.sampler == VK_NULL_HANDLE)
								imageInfo.sampler = fallbackResources.sampler;
							if (imageInfo.sampler != VK_NULL_HANDLE)
							{
								imageInfos.push_back(imageInfo);
								write.pImageInfo = &imageInfos.back();
								writes.push_back(write);
								writtenBindings.push_back(vulkanBinding);
							}
						}
						break;
					case NLS::Render::RHI::BindingType::RWTexture:
						{
							write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
							VkDescriptorImageInfo imageInfo{};
							if (entry.textureView != nullptr)
							{
								auto srvHandle = entry.textureView->GetNativeShaderResourceView();
								if (srvHandle.backend == NLS::Render::RHI::BackendType::Vulkan)
									imageInfo.imageView = static_cast<VkImageView>(srvHandle.handle);
							}
							if (imageInfo.imageView == VK_NULL_HANDLE)
								imageInfo.imageView = fallbackViewForBinding(vulkanBinding);
							imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
							if (imageInfo.imageView != VK_NULL_HANDLE)
							{
								imageInfos.push_back(imageInfo);
								write.pImageInfo = &imageInfos.back();
								writes.push_back(write);
								writtenBindings.push_back(vulkanBinding);
							}
						}
						break;
					default:
						break;
					}
				}

				// Reflection/layout construction can contain optional resources which
				// are intentionally absent from the material instance. Vulkan still
				// requires every statically-used descriptor to be written before a draw.
				// Populate missing image descriptors with the device-owned 1x1 fallback;
				// this also covers bindings whose logical RHI entry never reached this
				// binding-set update (for example material texture slot 4 -> Vulkan #36).
				for (const auto& layoutEntry : layoutEntries)
				{
					const uint32_t vulkanBinding = ToVulkanDescriptorBinding(
						layoutEntry.type, layoutEntry.registerSpace, layoutEntry.binding);
					if (std::find(writtenBindings.begin(), writtenBindings.end(), vulkanBinding) != writtenBindings.end())
						continue;

					VkDescriptorType descriptorType = VK_DESCRIPTOR_TYPE_MAX_ENUM;
					VkDescriptorImageInfo imageInfo{};
					switch (layoutEntry.type)
					{
					case NLS::Render::RHI::BindingType::Texture:
						descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
						imageInfo.imageView = fallbackViewForBinding(vulkanBinding);
						imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
						break;
					case NLS::Render::RHI::BindingType::Sampler:
						descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
						imageInfo.sampler = fallbackResources.sampler;
						break;
					case NLS::Render::RHI::BindingType::RWTexture:
						descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
						imageInfo.imageView = fallbackViewForBinding(vulkanBinding);
						imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
						break;
					default:
						break;
					}

					if (descriptorType == VK_DESCRIPTOR_TYPE_MAX_ENUM ||
						(descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER && imageInfo.imageView == VK_NULL_HANDLE) ||
						(descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER && imageInfo.sampler == VK_NULL_HANDLE))
						continue;

					VkWriteDescriptorSet write{};
					write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
					write.dstSet = m_descriptorSet;
					write.dstBinding = vulkanBinding;
					write.descriptorType = descriptorType;
					write.descriptorCount = 1;
					imageInfos.push_back(imageInfo);
					write.pImageInfo = &imageInfos.back();
					writes.push_back(write);
					writtenBindings.push_back(vulkanBinding);
				}

				if (!writes.empty())
					vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
#endif
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHIBindingSetDesc& GetDesc() const override { return m_desc; }

#if NLS_HAS_VULKAN
			VkDescriptorSet GetDescriptorSet() const { return m_descriptorSet; }
			VkDescriptorSetLayout GetDescriptorSetLayout() const { return m_descriptorSetLayout; }
			const NativeVulkanBindingLayout* GetBindingLayout() const
			{
				return dynamic_cast<const NativeVulkanBindingLayout*>(m_desc.layout.get());
			}
#endif

		private:
		#if NLS_HAS_VULKAN
			uint32_t ResolveVulkanBinding(const NLS::Render::RHI::RHIBindingSetEntry& entry) const
			{
				if (m_desc.layout != nullptr)
				{
					for (const auto& layoutEntry : m_desc.layout->GetDesc().entries)
					{
						if (layoutEntry.binding == entry.binding && layoutEntry.type == entry.type)
							return ToVulkanDescriptorBinding(layoutEntry.type, layoutEntry.registerSpace, layoutEntry.binding);
					}
				}
				return entry.binding;
			}
		#endif
			VkDevice m_device = nullptr;
			NLS::Render::RHI::RHIBindingSetDesc m_desc;
#if NLS_HAS_VULKAN
			VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
			VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
			VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
#endif
		};

		class NativeVulkanPipelineLayout final : public NLS::Render::RHI::RHIPipelineLayout
		{
		public:
				explicit NativeVulkanPipelineLayout(VkDevice device, NLS::Render::RHI::RHIPipelineLayoutDesc desc)
				: m_device(device)
				, m_desc(std::move(desc))
			{
#if NLS_HAS_VULKAN
				if (m_device == nullptr)
					return;

				// Keep all four standard slots so a shader that references a higher
				// register space remains valid even when intermediate spaces are unused.
				constexpr uint32_t kStandardDescriptorSetCount = 4u;
				std::vector<VkDescriptorSetLayout> setLayouts(kStandardDescriptorSetCount, VK_NULL_HANDLE);
				std::vector<const NativeVulkanBindingLayout*> layoutObjects(kStandardDescriptorSetCount, nullptr);
				uint32_t requiredSetCount = kStandardDescriptorSetCount;
				for (size_t rhiSetIndex = 0u; rhiSetIndex < m_desc.bindingLayouts.size(); ++rhiSetIndex)
				{
					const uint32_t vulkanSetIndex = ToVulkanDescriptorSetIndex(static_cast<uint32_t>(rhiSetIndex));
					requiredSetCount = std::max(requiredSetCount, vulkanSetIndex + 1u);
					if (vulkanSetIndex >= setLayouts.size())
					{
						setLayouts.resize(vulkanSetIndex + 1u, VK_NULL_HANDLE);
						layoutObjects.resize(vulkanSetIndex + 1u, nullptr);
					}

					const auto& layout = m_desc.bindingLayouts[rhiSetIndex];
					auto* nativeLayout = layout != nullptr
						? dynamic_cast<NativeVulkanBindingLayout*>(layout.get())
						: nullptr;
					if (nativeLayout == nullptr || nativeLayout->GetDescriptorSetLayout() == VK_NULL_HANDLE)
					{
						// Null layouts are valid for unused canonical sets. Material and
						// compute paths can omit a group without invalidating the pipeline.
						NLS_LOG_WARNING("NativeVulkanPipelineLayout: using empty layout for canonical set " + std::to_string(rhiSetIndex));
						continue;
					}
					setLayouts[vulkanSetIndex] = nativeLayout->GetDescriptorSetLayout();
					layoutObjects[vulkanSetIndex] = nativeLayout;
				}

				if (std::find(setLayouts.begin(), setLayouts.end(), static_cast<VkDescriptorSetLayout>(VK_NULL_HANDLE)) != setLayouts.end())
				{
					VkDescriptorSetLayoutCreateInfo emptyLayoutInfo{};
					emptyLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
					const VkResult emptyResult = vkCreateDescriptorSetLayout(m_device, &emptyLayoutInfo, nullptr, &m_emptyDescriptorSetLayout);
					if (emptyResult != VK_SUCCESS)
					{
						NLS_LOG_ERROR("NativeVulkanPipelineLayout: failed to create empty descriptor set layout, result=" + std::to_string(emptyResult));
						return;
					}
					for (auto& setLayout : setLayouts)
						if (setLayout == VK_NULL_HANDLE)
							setLayout = m_emptyDescriptorSetLayout;
				}

				std::vector<VkPushConstantRange> pushConstantRanges;
				pushConstantRanges.reserve(m_desc.pushConstants.size());
				for (const auto& pc : m_desc.pushConstants)
				{
					VkPushConstantRange range{};
					range.stageFlags = ToVkShaderStageFlags(pc.stageMask);
					// The common indexed-object command is emitted for graphics draws
					// with both vertex and fragment stage flags. Widen a graphics range
					// accordingly so a shader reflection range that mentions only VS does
					// not make the otherwise valid shared push update illegal.
					if ((range.stageFlags & VK_SHADER_STAGE_VERTEX_BIT) != 0u)
						range.stageFlags |= VK_SHADER_STAGE_FRAGMENT_BIT;
					range.offset = pc.offset;
					range.size = pc.size;
					pushConstantRanges.push_back(range);
				}
				if (pushConstantRanges.empty())
				{
					// Indexed-object data is submitted by the common renderer even
					// for pipelines whose shader metadata does not declare a range.
					// Reserve the small Vulkan minimum-sized range so that command
					// recording remains valid across all graphics/compute pipelines.
					pushConstantRanges.push_back({ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0u, 128u });
				}

				m_setLayouts = setLayouts;
				m_layoutObjects = std::move(layoutObjects);

				VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
				pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
				pipelineLayoutInfo.setLayoutCount = requiredSetCount;
				pipelineLayoutInfo.pSetLayouts = setLayouts.data();
				pipelineLayoutInfo.pushConstantRangeCount = static_cast<uint32_t>(pushConstantRanges.size());
				pipelineLayoutInfo.pPushConstantRanges = pushConstantRanges.data();

				const VkResult result = vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout);
				if (result != VK_SUCCESS)
				{
					NLS_LOG_ERROR(
						"NativeVulkanPipelineLayout: vkCreatePipelineLayout failed, result=" +
						std::to_string(result));
					m_pipelineLayout = VK_NULL_HANDLE;
				}
				else
				{
					std::string handles;
					for (const auto setLayout : setLayouts)
						handles += std::to_string(reinterpret_cast<uintptr_t>(setLayout)) + " ";
					NLS_LOG_INFO(
						"NativeVulkanPipelineLayout: created handle=" +
						std::to_string(reinterpret_cast<uintptr_t>(m_pipelineLayout)) +
						" object=" +
						std::to_string(reinterpret_cast<uintptr_t>(this)) +
						" sets=" +
					std::to_string(setLayouts.size()) +
						" setHandles=" + handles);
				}
#endif
			}

			~NativeVulkanPipelineLayout()
			{
#if NLS_HAS_VULKAN
				if (m_pipelineLayout != VK_NULL_HANDLE)
				{
					NLS_LOG_INFO(
						"NativeVulkanPipelineLayout: destroying handle=" +
						std::to_string(reinterpret_cast<uintptr_t>(m_pipelineLayout)) +
						" object=" +
						std::to_string(reinterpret_cast<uintptr_t>(this)));
				}
				if (m_device != nullptr && m_pipelineLayout != VK_NULL_HANDLE)
					vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
				if (m_device != nullptr && m_emptyDescriptorSetLayout != VK_NULL_HANDLE)
					vkDestroyDescriptorSetLayout(m_device, m_emptyDescriptorSetLayout, nullptr);
#endif
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHIPipelineLayoutDesc& GetDesc() const override { return m_desc; }

#if NLS_HAS_VULKAN
			VkPipelineLayout GetPipelineLayout() const { return m_pipelineLayout; }
			bool IsEmptyDescriptorSet(uint32_t vulkanSetIndex) const
			{
				return vulkanSetIndex >= m_setLayouts.size() ||
					(m_emptyDescriptorSetLayout != VK_NULL_HANDLE && m_setLayouts[vulkanSetIndex] == m_emptyDescriptorSetLayout);
			}
			bool IsDescriptorSetCompatible(uint32_t vulkanSetIndex, const NativeVulkanBindingSet& bindingSet) const
			{
				if (vulkanSetIndex >= m_setLayouts.size())
					return false;

				const VkDescriptorSetLayout layout = bindingSet.GetDescriptorSetLayout();
				if (layout == VK_NULL_HANDLE)
					return false;
				if (m_setLayouts[vulkanSetIndex] == layout)
					return true;

				const auto* expected = vulkanSetIndex < m_layoutObjects.size()
					? m_layoutObjects[vulkanSetIndex]
					: nullptr;
				const auto* actual = bindingSet.GetBindingLayout();
				// A canonical empty slot is compatible only with another empty layout.
				if (expected == nullptr)
					return actual != nullptr && actual->GetSignature().empty();
				return actual != nullptr && expected->IsIdenticallyDefined(*actual);
			}
			std::string DescribeDescriptorSetCompatibility(uint32_t vulkanSetIndex, const NativeVulkanBindingSet& bindingSet) const
			{
				std::string result = "set=" + std::to_string(vulkanSetIndex);
				const auto* expected = vulkanSetIndex < m_layoutObjects.size() ? m_layoutObjects[vulkanSetIndex] : nullptr;
				const auto* actual = bindingSet.GetBindingLayout();
				result += " expected=" + (expected != nullptr ? std::string(expected->GetDebugName()) : std::string("<empty>"));
				result += " actual=" + (actual != nullptr ? std::string(actual->GetDebugName()) : std::string("<null>"));
				const auto appendSignature = [&result](const char* label, const NativeVulkanBindingLayout* layout)
				{
					result += " " + std::string(label) + "[";
					if (layout != nullptr)
					{
						bool first = true;
						for (const auto& entry : layout->GetSignature())
						{
							if (!first)
								result += ",";
							first = false;
							result += std::to_string(entry.binding) + ":" + std::to_string(static_cast<uint32_t>(entry.descriptorType)) + ":" +
								std::to_string(entry.descriptorCount) + ":" + std::to_string(entry.stageFlags);
						}
					}
					result += "]";
				};
				appendSignature("expectedSig=", expected);
				appendSignature("actualSig=", actual);
				return result;
			}
#endif

		private:
			VkDevice m_device = nullptr;
			NLS::Render::RHI::RHIPipelineLayoutDesc m_desc;
#if NLS_HAS_VULKAN
			VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
			VkDescriptorSetLayout m_emptyDescriptorSetLayout = VK_NULL_HANDLE;
			std::vector<VkDescriptorSetLayout> m_setLayouts;
			std::vector<const NativeVulkanBindingLayout*> m_layoutObjects;
#endif

#if NLS_HAS_VULKAN
			static VkShaderStageFlags ToVkShaderStageFlags(NLS::Render::RHI::ShaderStageMask stageMask)
			{
				VkShaderStageFlags flags = 0;
				if (static_cast<uint32_t>(stageMask) & static_cast<uint32_t>(NLS::Render::RHI::ShaderStageMask::Vertex))
					flags |= VK_SHADER_STAGE_VERTEX_BIT;
				if (static_cast<uint32_t>(stageMask) & static_cast<uint32_t>(NLS::Render::RHI::ShaderStageMask::Fragment))
					flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
				if (static_cast<uint32_t>(stageMask) & static_cast<uint32_t>(NLS::Render::RHI::ShaderStageMask::Compute))
					flags |= VK_SHADER_STAGE_COMPUTE_BIT;
				if (flags == 0)
					flags = VK_SHADER_STAGE_ALL_GRAPHICS;
				return flags;
			}
#endif
		};

		void NativeVulkanCommandBuffer::BindGraphicsPipeline(const std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>& pipeline)
		{
#if NLS_HAS_VULKAN
			if (m_commandBuffer == nullptr || pipeline == nullptr || !m_recording)
				return;
			m_boundPipeline = pipeline;
			m_computePipelineBound = false;

			// Extract and store the pipeline layout
			if (pipeline->GetDesc().pipelineLayout != nullptr)
			{
				auto* nativePipelineLayout = dynamic_cast<NativeVulkanPipelineLayout*>(pipeline->GetDesc().pipelineLayout.get());
				if (nativePipelineLayout != nullptr)
				{
					m_boundPipelineLayout = nativePipelineLayout->GetPipelineLayout();
					m_boundPipelineLayoutOwner = nativePipelineLayout;
				}
			}

			// Get VkPipeline from the pipeline via GetPipelineHandle()
			uint64_t pipelineHandle = pipeline->GetPipelineHandle();
			if (pipelineHandle == 0)
			{
				return;
			}

			VkPipeline vkPipeline = reinterpret_cast<VkPipeline>(pipelineHandle);
			vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipeline);
#endif
		}

		void NativeVulkanCommandBuffer::BindBindingSet(uint32_t setIndex, const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& bindingSet)
		{
#if NLS_HAS_VULKAN
			if (m_commandBuffer == nullptr || bindingSet == nullptr || !m_recording)
				return;

			std::shared_ptr<NLS::Render::RHI::RHIBindingSet> resolvedBindingSet = bindingSet;
			auto* nativeBindingSet = dynamic_cast<NativeVulkanBindingSet*>(resolvedBindingSet.get());
			while (nativeBindingSet == nullptr && resolvedBindingSet != nullptr)
			{
				auto wrapped = resolvedBindingSet->GetWrappedBindingSetShared();
				if (wrapped == nullptr || wrapped.get() == resolvedBindingSet.get())
					break;
				resolvedBindingSet = std::move(wrapped);
				nativeBindingSet = dynamic_cast<NativeVulkanBindingSet*>(resolvedBindingSet.get());
			}
			if (nativeBindingSet == nullptr || m_boundPipelineLayout == VK_NULL_HANDLE)
				return;
			const uint32_t vulkanSetIndex = ToVulkanDescriptorSetIndex(setIndex);
			if (m_boundPipelineLayoutOwner != nullptr &&
				!m_boundPipelineLayoutOwner->IsDescriptorSetCompatible(
					vulkanSetIndex,
					*nativeBindingSet))
			{
					NLS_LOG_WARNING(
						"NativeVulkanCommandBuffer: skipped incompatible binding set rhi=" +
						std::to_string(setIndex) + " vk=" + std::to_string(vulkanSetIndex) +
						" pipeline=" + (m_boundPipeline != nullptr ? std::string(m_boundPipeline->GetDebugName()) : std::string("<none>")) +
						" bindingSet=" + std::string(nativeBindingSet->GetDebugName()) + " " +
						m_boundPipelineLayoutOwner->DescribeDescriptorSetCompatibility(vulkanSetIndex, *nativeBindingSet));
				return;
			}

			VkDescriptorSet vkDescriptorSet = nativeBindingSet->GetDescriptorSet();
			if (vkDescriptorSet == VK_NULL_HANDLE)
			{
				return;
			}
				vkCmdBindDescriptorSets(
					m_commandBuffer,
					m_computePipelineBound ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
					m_boundPipelineLayout,
					vulkanSetIndex,
				1,
				&vkDescriptorSet,
				0,
				nullptr);
#endif
		}

		class NativeVulkanShaderModule final : public NLS::Render::RHI::RHIShaderModule
		{
		public:
			explicit NativeVulkanShaderModule(VkDevice device, NLS::Render::RHI::RHIShaderModuleDesc desc)
				: m_device(device)
				, m_desc(std::move(desc))
			{
#if NLS_HAS_VULKAN
				if (m_device == nullptr || m_desc.bytecode.empty())
					return;

				VkShaderModuleCreateInfo createInfo{};
				createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
				createInfo.codeSize = m_desc.bytecode.size();
				createInfo.pCode = reinterpret_cast<const uint32_t*>(m_desc.bytecode.data());

				VkResult result = vkCreateShaderModule(m_device, &createInfo, nullptr, &m_shaderModule);
				if (result != VK_SUCCESS)
				{
					NLS_LOG_ERROR("NativeVulkanShaderModule: vkCreateShaderModule failed with result=" + std::to_string(result));
					m_shaderModule = VK_NULL_HANDLE;
				}
				else
				{
					NLS_LOG_INFO("NativeVulkanShaderModule: Created VkShaderModule for '" + m_desc.debugName + "'");
				}
#endif
			}

			~NativeVulkanShaderModule()
			{
#if NLS_HAS_VULKAN
				if (m_device != nullptr && m_shaderModule != VK_NULL_HANDLE)
					vkDestroyShaderModule(m_device, m_shaderModule, nullptr);
#endif
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHIShaderModuleDesc& GetDesc() const override { return m_desc; }

#if NLS_HAS_VULKAN
			VkShaderModule GetShaderModule() const { return m_shaderModule; }
#endif

		private:
			VkDevice m_device = nullptr;
			NLS::Render::RHI::RHIShaderModuleDesc m_desc;
#if NLS_HAS_VULKAN
			VkShaderModule m_shaderModule = VK_NULL_HANDLE;
#endif
		};

		class NativeVulkanGraphicsPipeline final : public NLS::Render::RHI::RHIGraphicsPipeline
		{
		public:
			explicit NativeVulkanGraphicsPipeline(
				VkDevice device,
				VkFormat swapchainFormat,
				NLS::Render::RHI::RHIGraphicsPipelineDesc desc)
				: m_device(device)
				, m_swapchainFormat(swapchainFormat)
				, m_desc(std::move(desc))
			{
#if NLS_HAS_VULKAN
				if (m_device == nullptr)
					return;

				// Get shader modules from the desc
				VkShaderModule vkVertModule = VK_NULL_HANDLE;
				VkShaderModule vkFragModule = VK_NULL_HANDLE;

				if (m_desc.vertexShader != nullptr)
				{
					auto* nativeShader = dynamic_cast<NativeVulkanShaderModule*>(m_desc.vertexShader.get());
					if (nativeShader != nullptr)
						vkVertModule = nativeShader->GetShaderModule();
				}

				if (m_desc.fragmentShader != nullptr)
				{
					auto* nativeShader = dynamic_cast<NativeVulkanShaderModule*>(m_desc.fragmentShader.get());
					if (nativeShader != nullptr)
						vkFragModule = nativeShader->GetShaderModule();
				}

				if (vkVertModule == VK_NULL_HANDLE || vkFragModule == VK_NULL_HANDLE)
				{
					NLS_LOG_ERROR("NativeVulkanGraphicsPipeline: missing valid vertex or fragment shader module for '" + m_desc.debugName + "'");
					return;
				}

				// Create render pass
				VkRenderPass renderPass = CreateRenderPass();
				if (renderPass == VK_NULL_HANDLE)
				{
					NLS_LOG_ERROR("NativeVulkanGraphicsPipeline: Failed to create render pass");
					return;
				}

				// Get pipeline layout
				VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
				if (m_desc.pipelineLayout != nullptr)
				{
					auto* nativeLayout = dynamic_cast<NativeVulkanPipelineLayout*>(m_desc.pipelineLayout.get());
					if (nativeLayout != nullptr)
						pipelineLayout = nativeLayout->GetPipelineLayout();
				}

				if (pipelineLayout == VK_NULL_HANDLE)
				{
					NLS_LOG_ERROR("NativeVulkanGraphicsPipeline: Failed to get pipeline layout");
					return;
				}
				NLS_LOG_INFO(
					"NativeVulkanGraphicsPipeline: using pipeline layout handle=" +
					std::to_string(reinterpret_cast<uintptr_t>(pipelineLayout)) +
					" object=" +
					std::to_string(reinterpret_cast<uintptr_t>(m_desc.pipelineLayout.get())));

				// Build shader stages
				std::vector<VkPipelineShaderStageCreateInfo> shaderStages;
				if (vkVertModule != VK_NULL_HANDLE)
				{
					VkPipelineShaderStageCreateInfo vertStage{};
					vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
					vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
					vertStage.module = vkVertModule;
					vertStage.pName = m_desc.vertexShader->GetDesc().entryPoint.c_str();
					shaderStages.push_back(vertStage);
				}
				if (vkFragModule != VK_NULL_HANDLE)
				{
					VkPipelineShaderStageCreateInfo fragStage{};
					fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
					fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
					fragStage.module = vkFragModule;
					fragStage.pName = m_desc.fragmentShader->GetDesc().entryPoint.c_str();
					shaderStages.push_back(fragStage);
				}

				// Vertex input state
				std::vector<VkVertexInputBindingDescription> vertexBindings;
				std::vector<VkVertexInputAttributeDescription> vertexAttributes;
				for (const auto& vb : m_desc.vertexBuffers)
				{
					VkVertexInputBindingDescription binding{};
					binding.binding = vb.binding;
					binding.stride = vb.stride;
					binding.inputRate = vb.perInstance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
					vertexBindings.push_back(binding);
				}
				for (const auto& va : m_desc.vertexAttributes)
				{
					VkVertexInputAttributeDescription attr{};
					attr.location = va.location;
					attr.binding = va.binding;
					switch (va.elementSize)
					{
					case 4u: attr.format = VK_FORMAT_R32_SFLOAT; break;
					case 8u: attr.format = VK_FORMAT_R32G32_SFLOAT; break;
					case 12u: attr.format = VK_FORMAT_R32G32B32_SFLOAT; break;
					case 16u: attr.format = VK_FORMAT_R32G32B32A32_SFLOAT; break;
					default:
						NLS_LOG_WARNING("NativeVulkanGraphicsPipeline: unsupported vertex attribute size " + std::to_string(va.elementSize));
						attr.format = VK_FORMAT_R32G32B32A32_SFLOAT;
						break;
					}
					attr.offset = va.offset;
					vertexAttributes.push_back(attr);
				}

				VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
				vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
				vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(vertexBindings.size());
				vertexInputInfo.pVertexBindingDescriptions = vertexBindings.data();
				vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexAttributes.size());
				vertexInputInfo.pVertexAttributeDescriptions = vertexAttributes.data();

				// Input assembly
				VkPrimitiveTopology vkTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
				switch (m_desc.primitiveTopology)
				{
				case NLS::Render::RHI::PrimitiveTopology::PointList: vkTopology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
				case NLS::Render::RHI::PrimitiveTopology::LineList: vkTopology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
				case NLS::Render::RHI::PrimitiveTopology::TriangleList: vkTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
				default: vkTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
				}

				VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
				inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
				inputAssembly.topology = vkTopology;
				inputAssembly.primitiveRestartEnable = VK_FALSE;

				// Viewport state
				VkPipelineViewportStateCreateInfo viewportState{};
				viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
				viewportState.viewportCount = 1;
				viewportState.scissorCount = 1;

				// Rasterization state
				VkCullModeFlags vkCullMode = VK_CULL_MODE_NONE;
				// Keep the engine's counter-clockwise front-face contract. The
				// negative-height viewport is applied at command recording time to
				// preserve the top-left origin without changing mesh winding.
				VkFrontFace vkFrontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
				if (m_desc.rasterState.cullEnabled)
				{
					switch (m_desc.rasterState.cullFace)
					{
					case NLS::Render::Settings::ECullFace::FRONT: vkCullMode = VK_CULL_MODE_FRONT_BIT; break;
					case NLS::Render::Settings::ECullFace::BACK: vkCullMode = VK_CULL_MODE_BACK_BIT; break;
					case NLS::Render::Settings::ECullFace::FRONT_AND_BACK: vkCullMode = VK_CULL_MODE_FRONT_AND_BACK; break;
					}
				}

				VkPipelineRasterizationStateCreateInfo rasterizer{};
				rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
				rasterizer.depthClampEnable = VK_FALSE;
				rasterizer.rasterizerDiscardEnable = VK_FALSE;
				// llvmpipe and the Vulkan 1.1 baseline do not guarantee
				// fillModeNonSolid; preserve a valid pipeline by falling back to fill.
				rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
				rasterizer.cullMode = vkCullMode;
				rasterizer.frontFace = vkFrontFace;
				rasterizer.depthBiasEnable = VK_FALSE;
				rasterizer.lineWidth = 1.0f;

				// Multisample state
				VkPipelineMultisampleStateCreateInfo multisampling{};
				multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
				multisampling.sampleShadingEnable = VK_FALSE;
				multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

				// Depth stencil state
				VkPipelineDepthStencilStateCreateInfo depthStencil{};
				depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
				depthStencil.depthTestEnable = m_desc.depthStencilState.depthTest ? VK_TRUE : VK_FALSE;
				depthStencil.depthWriteEnable = m_desc.depthStencilState.depthWrite ? VK_TRUE : VK_FALSE;
				depthStencil.depthCompareOp = ToVkCompareOp(m_desc.depthStencilState.depthCompare);
				depthStencil.depthBoundsTestEnable = VK_FALSE;
				depthStencil.stencilTestEnable = m_desc.depthStencilState.stencilTest ? VK_TRUE : VK_FALSE;
				depthStencil.front.failOp = ToVkStencilOp(m_desc.depthStencilState.stencilFailOp);
				depthStencil.front.passOp = ToVkStencilOp(m_desc.depthStencilState.stencilPassOp);
				depthStencil.front.depthFailOp = ToVkStencilOp(m_desc.depthStencilState.stencilDepthFailOp);
				depthStencil.front.compareOp = ToVkCompareOp(m_desc.depthStencilState.stencilCompare);
				depthStencil.front.compareMask = m_desc.depthStencilState.stencilReadMask;
				depthStencil.front.writeMask = m_desc.depthStencilState.stencilWriteMask;
				depthStencil.front.reference = m_desc.depthStencilState.stencilReference;
				depthStencil.back = depthStencil.front;

				// Color blend state must have exactly one attachment entry per color
				// attachment in the render pass. Mesa is less forgiving than the
				// validation layer here and can crash when the counts differ.
				const auto toBlendFactor = [](NLS::Render::RHI::RHIBlendFactor factor)
				{
					switch (factor)
					{
					case NLS::Render::RHI::RHIBlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
					case NLS::Render::RHI::RHIBlendFactor::One: return VK_BLEND_FACTOR_ONE;
					case NLS::Render::RHI::RHIBlendFactor::SrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
					case NLS::Render::RHI::RHIBlendFactor::InvSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
					case NLS::Render::RHI::RHIBlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
					case NLS::Render::RHI::RHIBlendFactor::InvSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
					case NLS::Render::RHI::RHIBlendFactor::DstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
					case NLS::Render::RHI::RHIBlendFactor::InvDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
					case NLS::Render::RHI::RHIBlendFactor::DstColor: return VK_BLEND_FACTOR_DST_COLOR;
					case NLS::Render::RHI::RHIBlendFactor::InvDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
					default: return VK_BLEND_FACTOR_ONE;
					}
				};
				const auto toBlendOp = [](NLS::Render::RHI::RHIBlendOp op)
				{
					switch (op)
					{
					case NLS::Render::RHI::RHIBlendOp::Subtract: return VK_BLEND_OP_SUBTRACT;
					case NLS::Render::RHI::RHIBlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
					case NLS::Render::RHI::RHIBlendOp::Min: return VK_BLEND_OP_MIN;
					case NLS::Render::RHI::RHIBlendOp::Max: return VK_BLEND_OP_MAX;
					case NLS::Render::RHI::RHIBlendOp::Add:
					default: return VK_BLEND_OP_ADD;
					}
				};
				// Vulkan graphics pipelines must describe the same color attachment
				// count as the render pass used by the common frame graph. Some depth
				// and overlay descriptors omit the color list even though they are
				// executed in a color pass, so keep one conventional RGBA attachment
				// as the backend fallback.
				const size_t colorAttachmentCount = std::max<size_t>(1u, m_desc.renderTargetLayout.colorFormats.size());
				std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments(colorAttachmentCount);
				for (size_t index = 0; index < colorBlendAttachments.size(); ++index)
				{
					const auto& target = index < m_desc.blendState.renderTargets.size()
						? m_desc.blendState.renderTargets[index]
						: NLS::Render::RHI::RHIRenderTargetBlendStateDesc{};
					auto& attachment = colorBlendAttachments[index];
					attachment.colorWriteMask = static_cast<VkColorComponentFlags>(
						static_cast<uint8_t>(target.colorWriteMask) & 0x0Fu);
					if (!m_desc.blendState.colorWrite)
						attachment.colorWriteMask = 0;
					attachment.blendEnable = target.blendEnable ? VK_TRUE : VK_FALSE;
					attachment.srcColorBlendFactor = toBlendFactor(target.srcColor);
					attachment.dstColorBlendFactor = toBlendFactor(target.dstColor);
					attachment.colorBlendOp = toBlendOp(target.colorOp);
					attachment.srcAlphaBlendFactor = toBlendFactor(target.srcAlpha);
					attachment.dstAlphaBlendFactor = toBlendFactor(target.dstAlpha);
					attachment.alphaBlendOp = toBlendOp(target.alphaOp);
				}

				VkPipelineColorBlendStateCreateInfo colorBlending{};
				colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
				colorBlending.logicOpEnable = VK_FALSE;
				colorBlending.attachmentCount = static_cast<uint32_t>(colorBlendAttachments.size());
				colorBlending.pAttachments = colorBlendAttachments.empty() ? nullptr : colorBlendAttachments.data();

				// Dynamic state
				std::vector<VkDynamicState> dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
				VkPipelineDynamicStateCreateInfo dynamicState{};
				dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
				dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
				dynamicState.pDynamicStates = dynamicStates.data();

				// Create pipeline
				VkGraphicsPipelineCreateInfo pipelineInfo{};
				pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
				pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
				pipelineInfo.pStages = shaderStages.data();
				pipelineInfo.pVertexInputState = &vertexInputInfo;
				pipelineInfo.pInputAssemblyState = &inputAssembly;
				pipelineInfo.pViewportState = &viewportState;
				pipelineInfo.pRasterizationState = &rasterizer;
				pipelineInfo.pMultisampleState = &multisampling;
				pipelineInfo.pDepthStencilState = &depthStencil;
				pipelineInfo.pColorBlendState = &colorBlending;
				pipelineInfo.pDynamicState = &dynamicState;
				pipelineInfo.layout = pipelineLayout;
				pipelineInfo.renderPass = renderPass;
				pipelineInfo.subpass = 0;

				NLS_LOG_INFO(
					"NativeVulkanGraphicsPipeline: creating '" + m_desc.debugName +
					"' colors=" + std::to_string(colorAttachmentCount) +
					" depth=" + std::to_string(m_desc.renderTargetLayout.hasDepth ? 1 : 0) +
					" stages=" + std::to_string(shaderStages.size()));
				VkResult result = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline);
				if (result != VK_SUCCESS)
				{
					NLS_LOG_ERROR("NativeVulkanGraphicsPipeline: vkCreateGraphicsPipelines failed with result=" + std::to_string(result));
					m_pipeline = VK_NULL_HANDLE;
				}
				else
				{
					NLS_LOG_INFO("NativeVulkanGraphicsPipeline: Created VkPipeline for '" + m_desc.debugName + "'");
				}

				// Clean up render pass (pipeline keeps its own reference)
				if (renderPass != VK_NULL_HANDLE)
					vkDestroyRenderPass(m_device, renderPass, nullptr);
#endif
			}

			~NativeVulkanGraphicsPipeline()
			{
#if NLS_HAS_VULKAN
				if (m_device != nullptr && m_pipeline != VK_NULL_HANDLE)
					vkDestroyPipeline(m_device, m_pipeline, nullptr);
#endif
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHIGraphicsPipelineDesc& GetDesc() const override { return m_desc; }
			uint64_t GetPipelineHandle() const override { return reinterpret_cast<uint64_t>(m_pipeline); }

#if NLS_HAS_VULKAN
			VkPipeline GetPipeline() const { return m_pipeline; }
#endif

		private:
			VkFormat ToVkFormat(NLS::Render::RHI::TextureFormat format)
			{
				switch (format)
				{
				case NLS::Render::RHI::TextureFormat::R8: return VK_FORMAT_R8_UNORM;
				case NLS::Render::RHI::TextureFormat::RG8: return VK_FORMAT_R8G8_UNORM;
				// The RHI exposes RGB8, but Vulkan has no universally supported
				// three-byte color attachment format. Textures use the same padded
				// representation as NativeVulkanTexture.
				case NLS::Render::RHI::TextureFormat::RGB8:
				case NLS::Render::RHI::TextureFormat::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
				case NLS::Render::RHI::TextureFormat::R16F: return VK_FORMAT_R16_SFLOAT;
				case NLS::Render::RHI::TextureFormat::RG16F: return VK_FORMAT_R16G16_SFLOAT;
				case NLS::Render::RHI::TextureFormat::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
				case NLS::Render::RHI::TextureFormat::R32F: return VK_FORMAT_R32_SFLOAT;
				case NLS::Render::RHI::TextureFormat::RG32F: return VK_FORMAT_R32G32_SFLOAT;
				case NLS::Render::RHI::TextureFormat::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
				case NLS::Render::RHI::TextureFormat::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
				case NLS::Render::RHI::TextureFormat::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
				case NLS::Render::RHI::TextureFormat::BC5: return VK_FORMAT_BC5_UNORM_BLOCK;
				case NLS::Render::RHI::TextureFormat::BC7: return VK_FORMAT_BC7_UNORM_BLOCK;
				case NLS::Render::RHI::TextureFormat::BC6H: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
				case NLS::Render::RHI::TextureFormat::Depth32F: return VK_FORMAT_D32_SFLOAT;
				case NLS::Render::RHI::TextureFormat::Depth24Stencil8: return VK_FORMAT_D32_SFLOAT;
				default: return VK_FORMAT_R8G8B8A8_UNORM;
				}
			}

			VkRenderPass CreateRenderPass()
			{
				if (m_device == nullptr)
					return VK_NULL_HANDLE;

				std::vector<VkAttachmentDescription> attachments;
				std::vector<VkAttachmentReference> colorRefs;
				VkAttachmentReference depthRef = {};

				// Color attachments
				const size_t pipelineColorCount = std::max<size_t>(1u, m_desc.renderTargetLayout.colorFormats.size());
				const std::string_view vertexShaderName = m_desc.vertexShader != nullptr
					? m_desc.vertexShader->GetDebugName()
					: std::string_view{};
				const bool isImGuiOverlay = vertexShaderName.find("RHIImGuiOverlay") != std::string_view::npos ||
					m_desc.debugName.find("ImGui") != std::string::npos ||
					m_desc.debugName.find("UIOverlay") != std::string::npos;
				for (size_t i = 0; i < pipelineColorCount; ++i)
				{
					VkAttachmentDescription colorAtt{};
					colorAtt.format = ToVkFormat(i < m_desc.renderTargetLayout.colorFormats.size()
						? m_desc.renderTargetLayout.colorFormats[i]
						: NLS::Render::RHI::TextureFormat::RGBA8);
					if (isImGuiOverlay && m_swapchainFormat != VK_FORMAT_UNDEFINED)
						colorAtt.format = m_swapchainFormat;
					colorAtt.samples = VK_SAMPLE_COUNT_1_BIT;
					colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
					colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
					colorAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
					colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
					colorAtt.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
					colorAtt.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
					attachments.push_back(colorAtt);

					VkAttachmentReference ref{};
					ref.attachment = static_cast<uint32_t>(i);
					ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
					colorRefs.push_back(ref);
				}

				// Depth attachment
				// Render-pass compatibility is structural: an omitted depth attachment
				// cannot be paired with a pipeline that declares one (and vice versa).
				// Use the explicit target layout carried by the pipeline descriptor. The
				// threaded FrameGraph validates this against the active pass before draw
				// recording, so color-only passes remain compatible with their PSOs.
				const bool hasDepth = m_desc.renderTargetLayout.hasDepth;
				if (hasDepth)
				{
					VkAttachmentDescription depthAtt{};
					depthAtt.format = ToVkFormat(m_desc.renderTargetLayout.depthFormat);
					NLS_LOG_INFO(
						"NativeVulkanGraphicsPipeline: render pass formats colors=" +
						std::to_string(colorRefs.size()) + " depth=" +
						std::to_string(static_cast<int>(depthAtt.format)));
					depthAtt.samples = VK_SAMPLE_COUNT_1_BIT;
					depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
					depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
					depthAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
					depthAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
					depthAtt.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
					depthAtt.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
					attachments.push_back(depthAtt);

					depthRef.attachment = static_cast<uint32_t>(attachments.size() - 1);
					depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
				}

				VkSubpassDescription subpass{};
				subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
				subpass.colorAttachmentCount = static_cast<uint32_t>(colorRefs.size());
				subpass.pColorAttachments = colorRefs.data();
				subpass.pDepthStencilAttachment = hasDepth ? &depthRef : nullptr;

				VkRenderPassCreateInfo renderPassInfo{};
				renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
				renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
				renderPassInfo.pAttachments = attachments.data();
				renderPassInfo.subpassCount = 1;
				renderPassInfo.pSubpasses = &subpass;

				VkRenderPass renderPass = VK_NULL_HANDLE;
				VkResult result = vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &renderPass);
				if (result != VK_SUCCESS)
				{
					NLS_LOG_ERROR("NativeVulkanGraphicsPipeline: vkCreateRenderPass failed with result=" + std::to_string(result));
					return VK_NULL_HANDLE;
				}

				return renderPass;
			}

			VkDevice m_device = nullptr;
			VkFormat m_swapchainFormat = VK_FORMAT_UNDEFINED;
			NLS::Render::RHI::RHIGraphicsPipelineDesc m_desc;
#if NLS_HAS_VULKAN
			VkPipeline m_pipeline = VK_NULL_HANDLE;
#endif
		};

		class NativeVulkanComputePipeline final : public NLS::Render::RHI::RHIComputePipeline
		{
		public:
			explicit NativeVulkanComputePipeline(VkDevice device, NLS::Render::RHI::RHIComputePipelineDesc desc)
				: m_device(device)
				, m_desc(std::move(desc))
			{
#if NLS_HAS_VULKAN
				if (m_device == VK_NULL_HANDLE || m_desc.computeShader == nullptr || m_desc.pipelineLayout == nullptr)
					return;
				auto* shader = dynamic_cast<NativeVulkanShaderModule*>(m_desc.computeShader.get());
				auto* layout = dynamic_cast<NativeVulkanPipelineLayout*>(m_desc.pipelineLayout.get());
				if (shader == nullptr || layout == nullptr || shader->GetShaderModule() == VK_NULL_HANDLE || layout->GetPipelineLayout() == VK_NULL_HANDLE)
					return;
				VkPipelineShaderStageCreateInfo stage{};
				stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
				stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
				stage.module = shader->GetShaderModule();
				stage.pName = m_desc.computeShader->GetDesc().entryPoint.c_str();
				VkComputePipelineCreateInfo pipelineInfo{};
				pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
				pipelineInfo.stage = stage;
				pipelineInfo.layout = layout->GetPipelineLayout();
				if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1u, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS)
					m_pipeline = VK_NULL_HANDLE;
#endif
			}
			~NativeVulkanComputePipeline()
			{
#if NLS_HAS_VULKAN
				if (m_device != VK_NULL_HANDLE && m_pipeline != VK_NULL_HANDLE)
					vkDestroyPipeline(m_device, m_pipeline, nullptr);
#endif
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHIComputePipelineDesc& GetDesc() const override { return m_desc; }
			VkPipeline GetPipeline() const { return m_pipeline; }
			VkPipelineLayout GetPipelineLayout() const
			{
				auto* layout = dynamic_cast<NativeVulkanPipelineLayout*>(m_desc.pipelineLayout.get());
				return layout != nullptr ? layout->GetPipelineLayout() : VK_NULL_HANDLE;
			}
			NativeVulkanPipelineLayout* GetPipelineLayoutObject() const
			{
				return dynamic_cast<NativeVulkanPipelineLayout*>(m_desc.pipelineLayout.get());
			}

		private:
			VkDevice m_device = nullptr;
			NLS::Render::RHI::RHIComputePipelineDesc m_desc;
			VkPipeline m_pipeline = VK_NULL_HANDLE;
		};

		void NativeVulkanCommandBuffer::BindComputePipeline(const std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>& pipeline)
		{
#if NLS_HAS_VULKAN
			if (m_commandBuffer == VK_NULL_HANDLE || pipeline == nullptr || !m_recording)
				return;
			m_boundComputePipeline = pipeline;
			m_computePipelineBound = false;
			if (auto* nativePipeline = dynamic_cast<NativeVulkanComputePipeline*>(pipeline.get()); nativePipeline != nullptr && nativePipeline->GetPipeline() != VK_NULL_HANDLE)
			{
				vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, nativePipeline->GetPipeline());
				m_boundPipelineLayout = nativePipeline->GetPipelineLayout();
				m_boundPipelineLayoutOwner = nativePipeline->GetPipelineLayoutObject();
				m_computePipelineBound = m_boundPipelineLayout != VK_NULL_HANDLE;
			}
#else
			(void)pipeline;
#endif
		}

		class NativeVulkanExplicitDevice final : public NLS::Render::RHI::RHIDevice
		{
		public:
			~NativeVulkanExplicitDevice()
			{
#if NLS_HAS_VULKAN
				if (m_device != VK_NULL_HANDLE)
					vkDeviceWaitIdle(m_device);
#endif
				DestroyUIResources();
#if NLS_HAS_VULKAN
				// RHI resource shared_ptrs are owned by renderer/material caches and may
				// outlive Driver::ShutdownRhiResources. Destroying the device here would
				// leave those objects with invalid VkDevice handles and causes both
				// validation child-object errors and shutdown crashes. Keep the Vulkan
				// instance/device alive for process lifetime; the OS reclaims them after
				// the last cache is released. This is deliberately scoped to the backend
				// wrapper and does not affect DX12/Metal lifetime behavior.
#endif
			}

			NativeVulkanExplicitDevice(
				VkInstance instance,
				VulkanDebugMessengerHandle debugMessenger,
				VkPhysicalDevice physicalDevice,
				VkDevice device,
				VkQueue graphicsQueue,
				VkSurfaceKHR surface,
				VkSwapchainKHR swapchain,
				uint32_t graphicsQueueFamilyIndex,
				const NLS::Render::RHI::RHIDeviceCapabilities& capabilities,
				const std::string& vendor,
				const std::string& hardware,
				bool dynamicRenderingEnabled = false)
				: m_instance(instance)
				, m_debugMessenger(debugMessenger)
				, m_physicalDevice(physicalDevice)
				, m_device(device)
				, m_allocator(CreateVulkanMemoryAllocator(instance, physicalDevice, device))
				, m_graphicsQueue(graphicsQueue)
				, m_surface(surface)
				, m_swapchain(swapchain)
				, m_graphicsQueueFamilyIndex(graphicsQueueFamilyIndex)
				, m_capabilities(capabilities)
				, m_rhiAdapter(std::make_shared<NativeVulkanAdapter>(vendor, hardware))
				, m_dynamicRenderingEnabled(dynamicRenderingEnabled)
			{
				if (m_allocator == VK_NULL_HANDLE)
					NLS_LOG_ERROR("NativeVulkanExplicitDevice: VulkanMemoryAllocator initialization failed");
				CreateUIResources();
			EnsureVulkanFallbackResources(m_device, m_allocator, m_graphicsQueue, m_graphicsQueueFamilyIndex);
			}

			std::string_view GetDebugName() const override { return "NativeVulkanExplicitDevice"; }
			const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_rhiAdapter; }
			const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
			NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override
			{
				NLS::Render::RHI::NativeRenderDeviceInfo info{};
				info.backend = NLS::Render::RHI::NativeBackendType::Vulkan;
#if NLS_HAS_VULKAN
				info.instance = reinterpret_cast<void*>(m_instance);
				info.physicalDevice = reinterpret_cast<void*>(m_physicalDevice);
				info.device = reinterpret_cast<void*>(m_device);
				info.graphicsQueue = reinterpret_cast<void*>(m_graphicsQueue);
				info.surface = reinterpret_cast<void*>(m_surface);
				info.swapchain = reinterpret_cast<void*>(m_swapchain);
				info.graphicsQueueFamilyIndex = m_graphicsQueueFamilyIndex;
				info.uiRenderPass = reinterpret_cast<void*>(m_uiRenderPass);
				info.uiDescriptorPool = reinterpret_cast<void*>(m_uiDescriptorPool);
				info.currentCommandBuffer = m_currentCommandBuffer;
#endif
				return info;
			}
			bool IsBackendReady() const override { return m_device != nullptr && m_allocator != VK_NULL_HANDLE; }

			std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType queueType) override
			{
				// The baseline WSL llvmpipe device exposes one queue family/queue.  Return
				// the same RHI wrapper for every queue type so async-compute capability
				// detection cannot manufacture cross-queue semaphores on the same VkQueue.
				(void)queueType;
				if (m_queues[0] == nullptr)
					m_queues[0] = std::make_shared<NativeVulkanQueue>(m_device, m_graphicsQueue, "GraphicsQueue");
				return m_queues[0];
			}

			std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(const NLS::Render::RHI::SwapchainDesc& desc) override
			{
#if NLS_HAS_VULKAN
				if (m_device == nullptr || m_physicalDevice == nullptr)
					return nullptr;

				// Create surface if it doesn't exist yet (deferred surface creation)
				if (m_surface == nullptr && desc.platformWindow != nullptr)
				{
					GLFWwindow* window = static_cast<GLFWwindow*>(desc.platformWindow);
					if (glfwCreateWindowSurface(m_instance, window, nullptr, &m_surface) != VK_SUCCESS)
					{
						NLS_LOG_ERROR("NativeVulkanExplicitDevice::CreateSwapchain: failed to create surface from window");
						return nullptr;
					}
					NLS_LOG_INFO("NativeVulkanExplicitDevice::CreateSwapchain: created surface from window");
				}

				if (m_surface == nullptr)
					return nullptr;

				// Get surface capabilities
				VkSurfaceCapabilitiesKHR surfaceCapabilities{};
				if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &surfaceCapabilities) != VK_SUCCESS)
					return nullptr;

				// Get surface formats
				uint32_t formatCount = 0;
				if (vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, nullptr) != VK_SUCCESS || formatCount == 0)
					return nullptr;
				std::vector<VkSurfaceFormatKHR> formats(formatCount);
				if (vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, formats.data()) != VK_SUCCESS)
					return nullptr;
				// DX12 uses an RGBA8 UNORM swapchain. Prefer the same logical format
				// when the WSI exposes it; the swapchain must not silently add an sRGB
				// encode step that the common RHI did not request.
				VkSurfaceFormatKHR selectedFormat = formats[0];
				for (const auto& candidate : formats)
				{
					if (candidate.format == VK_FORMAT_R8G8B8A8_UNORM &&
						candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
					{
						selectedFormat = candidate;
						break;
					}
				}

				// Get present modes
				uint32_t presentModeCount = 0;
				if (vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &presentModeCount, nullptr) != VK_SUCCESS)
					return nullptr;
				std::vector<VkPresentModeKHR> presentModes(presentModeCount);
				if (vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &presentModeCount, presentModes.data()) != VK_SUCCESS)
					return nullptr;

				// Determine extent
				VkExtent2D extent{};
				extent.width = desc.width > 0 ? desc.width : surfaceCapabilities.currentExtent.width;
				extent.height = desc.height > 0 ? desc.height : surfaceCapabilities.currentExtent.height;

				// Determine image count
				uint32_t imageCount = std::max(surfaceCapabilities.minImageCount, desc.imageCount > 0 ? desc.imageCount : 2);
				if (surfaceCapabilities.maxImageCount > 0)
					imageCount = std::min(imageCount, surfaceCapabilities.maxImageCount);

				// Determine present mode
				VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
				if (!desc.vsync)
				{
					for (auto mode : presentModes)
					{
						if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
						{
							presentMode = mode;
							break;
						}
						if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
							presentMode = mode;
					}
				}

				// Create swapchain
				VkSwapchainCreateInfoKHR createInfo{};
				createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
				createInfo.surface = m_surface;
				createInfo.minImageCount = imageCount;
				createInfo.imageFormat = selectedFormat.format;
				createInfo.imageColorSpace = selectedFormat.colorSpace;
				createInfo.imageExtent = extent;
				createInfo.imageArrayLayers = 1;
				createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
				createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
				createInfo.preTransform = surfaceCapabilities.currentTransform;
				createInfo.compositeAlpha = (surfaceCapabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0
					? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
					: (surfaceCapabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) != 0
						? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
						: VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
				createInfo.presentMode = presentMode;
				createInfo.clipped = VK_TRUE;
				createInfo.oldSwapchain = VK_NULL_HANDLE;

				VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
				if (vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &newSwapchain) != VK_SUCCESS)
					return nullptr;

				// Update stored swapchain
				m_swapchain = newSwapchain;
				m_swapchainFormat = selectedFormat.format;
				// CreateUIResources runs in the device constructor, before the first
				// swapchain exists. Rebuild the small UI render-pass cache now that the
				// WSI format is known (WSLg commonly selects BGRA8 sRGB).
				DestroyUIResources();
				CreateUIResources();

				// Create SwapchainDesc for the NativeVulkanSwapchain
				NLS::Render::RHI::SwapchainDesc swapchainDesc = desc;
				swapchainDesc.width = extent.width;
				swapchainDesc.height = extent.height;
				swapchainDesc.imageCount = imageCount;

				auto swapchain = std::make_shared<NativeVulkanSwapchain>(m_device, m_graphicsQueue, newSwapchain, m_surface, swapchainDesc, selectedFormat.format);
				swapchain->SetPhysicalDevice(m_physicalDevice);

				return swapchain;
#else
				return nullptr;
#endif
			}

			std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(const NLS::Render::RHI::RHIBufferDesc& desc, const NLS::Render::RHI::RHIBufferUploadDesc& uploadDesc) override;
			std::shared_ptr<NLS::Render::RHI::RHITexture> CreateTexture(const NLS::Render::RHI::RHITextureDesc& desc, const NLS::Render::RHI::RHITextureUploadDesc& uploadDesc) override;
			std::shared_ptr<NLS::Render::RHI::RHITextureView> CreateTextureView(const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture, const NLS::Render::RHI::RHITextureViewDesc& desc) override;
			std::shared_ptr<NLS::Render::RHI::RHISampler> CreateSampler(const NLS::Render::RHI::SamplerDesc& desc, std::string debugName) override;
			std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateBindingLayout(const NLS::Render::RHI::RHIBindingLayoutDesc& desc) override;
			std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateBindingSet(const NLS::Render::RHI::RHIBindingSetDesc& desc) override;
			std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> CreatePipelineLayout(const NLS::Render::RHI::RHIPipelineLayoutDesc& desc) override;
			std::shared_ptr<NLS::Render::RHI::RHIShaderModule> CreateShaderModule(const NLS::Render::RHI::RHIShaderModuleDesc& desc) override;
			std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> CreateGraphicsPipeline(const NLS::Render::RHI::RHIGraphicsPipelineDesc& desc) override;
			std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> CreateComputePipeline(const NLS::Render::RHI::RHIComputePipelineDesc& desc) override;
			std::shared_ptr<NLS::Render::RHI::RHICommandPool> CreateCommandPool(NLS::Render::RHI::QueueType queueType, std::string debugName) override
			{
#if NLS_HAS_VULKAN
				if (m_device == nullptr)
					return nullptr;

				VkCommandPoolCreateInfo poolInfo{};
				poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
				poolInfo.queueFamilyIndex = m_graphicsQueueFamilyIndex;
				poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

				VkCommandPool vkCommandPool;
				VkResult result = vkCreateCommandPool(m_device, &poolInfo, nullptr, &vkCommandPool);
				if (result != VK_SUCCESS)
					return nullptr;

				return std::make_shared<NativeVulkanCommandPool>(m_device, vkCommandPool, queueType, debugName.empty() ? "CommandPool" : debugName, m_dynamicRenderingEnabled);
#else
				return nullptr;
#endif
			}
			std::shared_ptr<NLS::Render::RHI::RHIFence> CreateFence(std::string debugName) override
			{
#if NLS_HAS_VULKAN
				if (m_device == nullptr)
					return nullptr;

				VkFenceCreateInfo fenceInfo{};
				fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
				fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

				VkFence vkFence;
				VkResult result = vkCreateFence(m_device, &fenceInfo, nullptr, &vkFence);
				if (result != VK_SUCCESS)
					return nullptr;

				return std::make_shared<NativeVulkanFence>(m_device, vkFence, debugName.empty() ? "Fence" : debugName);
#else
				return nullptr;
#endif
			}
			std::shared_ptr<NLS::Render::RHI::RHISemaphore> CreateSemaphore(std::string debugName) override
			{
#if NLS_HAS_VULKAN
				if (m_device == nullptr)
					return nullptr;

				VkSemaphoreCreateInfo semaphoreInfo{};
				semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

				VkSemaphore vkSemaphore;
				VkResult result = vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &vkSemaphore);
				if (result != VK_SUCCESS)
					return nullptr;

				return std::make_shared<NativeVulkanSemaphore>(m_device, vkSemaphore, debugName.empty() ? "Semaphore" : debugName);
#else
				return nullptr;
#endif
			}

			// Readback support
			void ReadPixels(
			    const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
			    uint32_t x,
			    uint32_t y,
			    uint32_t width,
			    uint32_t height,
			    NLS::Render::Settings::EPixelDataFormat format,
			    NLS::Render::Settings::EPixelDataType type,
				void* data) override;
			NLS::Render::RHI::RHIReadbackResult ReadPixelsChecked(
			    const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
			    uint32_t x,
			    uint32_t y,
			    uint32_t width,
			    uint32_t height,
			    NLS::Render::Settings::EPixelDataFormat format,
			    NLS::Render::Settings::EPixelDataType type,
			    void* data) override;

		private:
				VkInstance m_instance = nullptr;
				VulkanDebugMessengerHandle m_debugMessenger = VK_NULL_HANDLE;
			VkPhysicalDevice m_physicalDevice = nullptr;
			VkDevice m_device = nullptr;
			VmaAllocator m_allocator = VK_NULL_HANDLE;
			VkQueue m_graphicsQueue = nullptr;
			VkSurfaceKHR m_surface = nullptr;
			VkSwapchainKHR m_swapchain = nullptr;
			VkFormat m_swapchainFormat = VK_FORMAT_R8G8B8A8_UNORM;
			uint32_t m_graphicsQueueFamilyIndex = 0;
			NLS::Render::RHI::RHIDeviceCapabilities m_capabilities{};
			std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_rhiAdapter;
			std::array<std::shared_ptr<NLS::Render::RHI::RHIQueue>, 3> m_queues{};
			bool m_dynamicRenderingEnabled = false;

			// UI rendering resources for ImGui
			VkRenderPass m_uiRenderPass = VK_NULL_HANDLE;
			VkDescriptorPool m_uiDescriptorPool = VK_NULL_HANDLE;
			VkSampler m_uiTextureSampler = VK_NULL_HANDLE;

			// Current command buffer for UI rendering - set by Driver before PrepareUIRender
			void* m_currentCommandBuffer = nullptr;

			void CreateUIResources();
			void DestroyUIResources();
			bool UploadTexture(const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture, const NLS::Render::RHI::RHITextureUploadDesc& uploadDesc);

			// UI submission uses the generic FrameGraph overlay path. The native
			// command buffer is exposed through NativeRenderDeviceInfo when needed.
		};

		void NativeVulkanExplicitDevice::CreateUIResources()
		{
#if NLS_HAS_VULKAN && NLS_HAS_IMGUI_VULKAN_BACKEND
			if (m_device == nullptr)
				return;

			// Create UI render pass
			const VkAttachmentDescription colorAttachment{
				0,
				m_swapchainFormat,
				VK_SAMPLE_COUNT_1_BIT,
				VK_ATTACHMENT_LOAD_OP_LOAD,
				VK_ATTACHMENT_STORE_OP_STORE,
				VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				VK_ATTACHMENT_STORE_OP_DONT_CARE,
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
			};
			const VkAttachmentReference colorAttachmentRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
			const VkSubpassDescription subpass{
				0,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				0,
				nullptr,
				1,
				&colorAttachmentRef,
				nullptr,
				nullptr,
				0,
				nullptr
			};
			const VkSubpassDependency dependency{
				VK_SUBPASS_EXTERNAL,
				0,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				0,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				0
			};
			const VkRenderPassCreateInfo renderPassInfo{
				VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
				nullptr,
				0,
				1,
				&colorAttachment,
				1,
				&subpass,
				1,
				&dependency
			};
			if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_uiRenderPass) != VK_SUCCESS)
			{
				NLS_LOG_WARNING("Failed to create Vulkan UI render pass");
				m_uiRenderPass = VK_NULL_HANDLE;
			}

			// Create UI descriptor pool
			const VkDescriptorPoolSize poolSize{
				VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				256
			};
			const VkDescriptorPoolCreateInfo poolInfo{
				VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
				nullptr,
				VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
				256,
				1,
				&poolSize
			};
			if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_uiDescriptorPool) != VK_SUCCESS)
			{
				NLS_LOG_WARNING("Failed to create Vulkan UI descriptor pool");
				m_uiDescriptorPool = VK_NULL_HANDLE;
			}

			// Create UI texture sampler (linear, clamp to edge)
			const VkSamplerCreateInfo samplerInfo{
				VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
				nullptr,
				0,
				VK_FILTER_LINEAR,
				VK_FILTER_LINEAR,
				VK_SAMPLER_MIPMAP_MODE_LINEAR,
				VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
				VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
				VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
				0.0f,
				VK_FALSE,
				1.0f,
				VK_FALSE,
				VK_COMPARE_OP_ALWAYS,
				0.0f,
				0.0f,
				VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
				VK_FALSE
			};
			if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_uiTextureSampler) != VK_SUCCESS)
			{
				NLS_LOG_WARNING("Failed to create Vulkan UI texture sampler");
				m_uiTextureSampler = VK_NULL_HANDLE;
			}
#endif
		}

		void NativeVulkanExplicitDevice::DestroyUIResources()
		{
#if NLS_HAS_VULKAN
			if (m_device == nullptr)
				return;

			if (m_uiTextureSampler != VK_NULL_HANDLE)
			{
				vkDestroySampler(m_device, m_uiTextureSampler, nullptr);
				m_uiTextureSampler = VK_NULL_HANDLE;
			}
			if (m_uiDescriptorPool != VK_NULL_HANDLE)
			{
				vkDestroyDescriptorPool(m_device, m_uiDescriptorPool, nullptr);
				m_uiDescriptorPool = VK_NULL_HANDLE;
			}
			if (m_uiRenderPass != VK_NULL_HANDLE)
			{
				vkDestroyRenderPass(m_device, m_uiRenderPass, nullptr);
				m_uiRenderPass = VK_NULL_HANDLE;
			}
#endif
		}

		// NativeVulkanExplicitDevice method implementations
		bool NativeVulkanExplicitDevice::UploadTexture(
			const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
			const NLS::Render::RHI::RHITextureUploadDesc& uploadDesc)
		{
#if NLS_HAS_VULKAN
			if (!uploadDesc.HasData())
				return true;
			auto* nativeTexture = dynamic_cast<NativeVulkanTexture*>(texture.get());
			if (nativeTexture == nullptr || m_device == VK_NULL_HANDLE || m_graphicsQueue == VK_NULL_HANDLE)
				return false;

			struct UploadBlock
			{
				const void* data = nullptr;
				size_t size = 0u;
				uint32_t mipLevel = 0u;
				uint32_t arrayLayer = 0u;
			};
			std::vector<UploadBlock> blocks;
			if (uploadDesc.data != nullptr && uploadDesc.dataSize != 0u)
				blocks.push_back({ uploadDesc.data, uploadDesc.dataSize, uploadDesc.mipLevel, uploadDesc.arrayLayer });
			else
			{
				for (uint32_t index = 0u; index < uploadDesc.subresources.size(); ++index)
				{
					const auto& subresource = uploadDesc.subresources[index];
					if (subresource.data != nullptr && subresource.dataSize != 0u)
						blocks.push_back({ subresource.data, subresource.dataSize, uploadDesc.mipLevel + index, uploadDesc.arrayLayer });
				}
			}
			if (blocks.empty())
				return true;

			VkDeviceSize stagingSize = 0u;
			for (const auto& block : blocks)
				stagingSize += static_cast<VkDeviceSize>(block.size);
			VkBuffer stagingBuffer = VK_NULL_HANDLE;
			VmaAllocation stagingAllocation = VK_NULL_HANDLE;
			const VkBufferCreateInfo bufferInfo{
				VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				nullptr,
				0,
				stagingSize,
				VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
				VK_SHARING_MODE_EXCLUSIVE,
				0,
				nullptr
			};
			VmaAllocationCreateInfo allocationInfo{};
			allocationInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
			allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
			if (vmaCreateBuffer(m_allocator, &bufferInfo, &allocationInfo, &stagingBuffer, &stagingAllocation, nullptr) != VK_SUCCESS)
			{
				return false;
			}
			void* mappedData = nullptr;
			if (vmaMapMemory(m_allocator, stagingAllocation, &mappedData) != VK_SUCCESS || mappedData == nullptr)
			{
				vmaDestroyBuffer(m_allocator, stagingBuffer, stagingAllocation);
				return false;
			}
			std::vector<VkBufferImageCopy> copyRegions;
			copyRegions.reserve(blocks.size());
			VkDeviceSize blockOffset = 0u;
			const auto& textureDesc = nativeTexture->GetDesc();
			for (const auto& block : blocks)
			{
				std::memcpy(static_cast<uint8_t*>(mappedData) + blockOffset, block.data, block.size);
				VkBufferImageCopy copy{};
				copy.bufferOffset = blockOffset;
				copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				copy.imageSubresource.mipLevel = block.mipLevel;
				copy.imageSubresource.baseArrayLayer = block.arrayLayer;
				copy.imageSubresource.layerCount = 1u;
				copy.imageExtent.width = uploadDesc.extent.width != 0u
					? std::max(1u, uploadDesc.extent.width >> block.mipLevel)
					: std::max(1u, textureDesc.extent.width >> block.mipLevel);
				copy.imageExtent.height = uploadDesc.extent.height != 0u
					? std::max(1u, uploadDesc.extent.height >> block.mipLevel)
					: std::max(1u, textureDesc.extent.height >> block.mipLevel);
				copy.imageExtent.depth = 1u;
				copyRegions.push_back(copy);
				blockOffset += static_cast<VkDeviceSize>(block.size);
			}
			vmaFlushAllocation(m_allocator, stagingAllocation, 0u, stagingSize);
			vmaUnmapMemory(m_allocator, stagingAllocation);

			VkCommandPool commandPool = VK_NULL_HANDLE;
			const VkCommandPoolCreateInfo poolInfo{
				VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
				nullptr,
				VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
				m_graphicsQueueFamilyIndex
			};
			if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS)
			{
				vmaDestroyBuffer(m_allocator, stagingBuffer, stagingAllocation);
				return false;
			}
			VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
			const VkCommandBufferAllocateInfo commandBufferInfo{
				VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				nullptr,
				commandPool,
				VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				1u
			};
			if (vkAllocateCommandBuffers(m_device, &commandBufferInfo, &commandBuffer) != VK_SUCCESS)
			{
				vkDestroyCommandPool(m_device, commandPool, nullptr);
				vmaDestroyBuffer(m_allocator, stagingBuffer, stagingAllocation);
				return false;
			}
			const VkCommandBufferBeginInfo beginInfo{
				VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				nullptr,
				VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
				nullptr
			};
			if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
			{
				vkFreeCommandBuffers(m_device, commandPool, 1u, &commandBuffer);
				vkDestroyCommandPool(m_device, commandPool, nullptr);
				vmaDestroyBuffer(m_allocator, stagingBuffer, stagingAllocation);
				return false;
			}
			VkImageMemoryBarrier uploadBarrier{};
			uploadBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			uploadBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			uploadBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			uploadBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			uploadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			uploadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			uploadBarrier.image = nativeTexture->GetImage();
			uploadBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			uploadBarrier.subresourceRange.levelCount = std::max(1u, textureDesc.mipLevels);
			uploadBarrier.subresourceRange.layerCount = textureDesc.dimension == NLS::Render::RHI::TextureDimension::TextureCube ? 6u : std::max(1u, textureDesc.arrayLayers);
			vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &uploadBarrier);
			vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, nativeTexture->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(copyRegions.size()), copyRegions.data());
			VkImageMemoryBarrier shaderBarrier = uploadBarrier;
			shaderBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			shaderBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			shaderBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			shaderBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &shaderBarrier);
			if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
			{
				vkFreeCommandBuffers(m_device, commandPool, 1u, &commandBuffer);
				vkDestroyCommandPool(m_device, commandPool, nullptr);
				vmaDestroyBuffer(m_allocator, stagingBuffer, stagingAllocation);
				return false;
			}
			const VkSubmitInfo submitInfo{
				VK_STRUCTURE_TYPE_SUBMIT_INFO,
				nullptr,
				0u,
				nullptr,
				nullptr,
				1u,
				&commandBuffer,
				0u,
				nullptr
			};
			const bool submitted = vkQueueSubmit(m_graphicsQueue, 1u, &submitInfo, VK_NULL_HANDLE) == VK_SUCCESS;
			if (submitted)
				vkQueueWaitIdle(m_graphicsQueue);
			vkFreeCommandBuffers(m_device, commandPool, 1u, &commandBuffer);
			vkDestroyCommandPool(m_device, commandPool, nullptr);
			vmaDestroyBuffer(m_allocator, stagingBuffer, stagingAllocation);
			if (submitted)
			{
				nativeTexture->SetState(NLS::Render::RHI::ResourceState::ShaderRead);
				const uint32_t mipCount = std::max(1u, textureDesc.mipLevels);
				const uint32_t layerCount = textureDesc.dimension == NLS::Render::RHI::TextureDimension::TextureCube
					? 6u
					: std::max(1u, textureDesc.arrayLayers);
				for (uint32_t mipLevel = 0u; mipLevel < mipCount; ++mipLevel)
				{
					for (uint32_t arrayLayer = 0u; arrayLayer < layerCount; ++arrayLayer)
						SetVulkanImageLayout(nativeTexture->GetImage(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, mipLevel, arrayLayer);
				}
			}
			return submitted;
#else
			(void)texture;
			(void)uploadDesc;
			return false;
#endif
		}

		std::shared_ptr<NLS::Render::RHI::RHIBuffer> NativeVulkanExplicitDevice::CreateBuffer(const NLS::Render::RHI::RHIBufferDesc& desc, const NLS::Render::RHI::RHIBufferUploadDesc& uploadDesc)
		{
			if (m_device == nullptr)
				return nullptr;
			auto buffer = std::make_shared<NativeVulkanBuffer>(m_device, m_allocator, desc, uploadDesc);
			return buffer != nullptr && buffer->GetNativeBufferHandle().handle != nullptr ? buffer : nullptr;
		}

		std::shared_ptr<NLS::Render::RHI::RHITexture> NativeVulkanExplicitDevice::CreateTexture(const NLS::Render::RHI::RHITextureDesc& desc, const NLS::Render::RHI::RHITextureUploadDesc& uploadDesc)
		{
#if NLS_HAS_VULKAN
			if (m_device == nullptr)
				return nullptr;
			auto texture = std::make_shared<NativeVulkanTexture>(m_device, m_allocator, desc, uploadDesc);
			if (texture == nullptr || texture->GetNativeImageHandle().handle == nullptr)
				return nullptr;
			if (!UploadTexture(texture, uploadDesc))
				return nullptr;
			return texture;
#else
			return nullptr;
#endif
		}

		std::shared_ptr<NLS::Render::RHI::RHITextureView> NativeVulkanExplicitDevice::CreateTextureView(const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture, const NLS::Render::RHI::RHITextureViewDesc& desc)
		{
#if NLS_HAS_VULKAN
			if (texture == nullptr)
				return nullptr;
			auto view = std::make_shared<NativeVulkanTextureView>(m_device, texture, desc);
			return view != nullptr && view->GetNativeShaderResourceView().handle != nullptr ? view : nullptr;
#else
			return nullptr;
#endif
		}

		std::shared_ptr<NLS::Render::RHI::RHISampler> NativeVulkanExplicitDevice::CreateSampler(const NLS::Render::RHI::SamplerDesc& desc, std::string debugName)
		{
#if NLS_HAS_VULKAN
			if (m_device == nullptr)
				return nullptr;
			auto sampler = std::make_shared<NativeVulkanSampler>(m_device, desc, debugName.empty() ? "Sampler" : debugName);
			return sampler->GetSampler() != VK_NULL_HANDLE ? sampler : nullptr;
#else
			return nullptr;
#endif
		}

		std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> NativeVulkanExplicitDevice::CreateBindingLayout(const NLS::Render::RHI::RHIBindingLayoutDesc& desc)
		{
			return std::make_shared<NativeVulkanBindingLayout>(m_device, desc);
		}

		std::shared_ptr<NLS::Render::RHI::RHIBindingSet> NativeVulkanExplicitDevice::CreateBindingSet(const NLS::Render::RHI::RHIBindingSetDesc& desc)
		{
			return std::make_shared<NativeVulkanBindingSet>(m_device, desc);
		}

		std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> NativeVulkanExplicitDevice::CreatePipelineLayout(const NLS::Render::RHI::RHIPipelineLayoutDesc& desc)
		{
			return std::make_shared<NativeVulkanPipelineLayout>(m_device, desc);
		}

		std::shared_ptr<NLS::Render::RHI::RHIShaderModule> NativeVulkanExplicitDevice::CreateShaderModule(const NLS::Render::RHI::RHIShaderModuleDesc& desc)
		{
			return std::make_shared<NativeVulkanShaderModule>(m_device, desc);
		}

		std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> NativeVulkanExplicitDevice::CreateGraphicsPipeline(const NLS::Render::RHI::RHIGraphicsPipelineDesc& desc)
		{
			return std::make_shared<NativeVulkanGraphicsPipeline>(m_device, m_swapchainFormat, desc);
		}

		std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> NativeVulkanExplicitDevice::CreateComputePipeline(const NLS::Render::RHI::RHIComputePipelineDesc& desc)
		{
			return std::make_shared<NativeVulkanComputePipeline>(m_device, desc);
		}

		void NativeVulkanExplicitDevice::ReadPixels(
		    const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
		    uint32_t x,
		    uint32_t y,
		    uint32_t width,
		    uint32_t height,
		    NLS::Render::Settings::EPixelDataFormat format,
		    NLS::Render::Settings::EPixelDataType type,
		    void* data)
		{
#if NLS_HAS_VULKAN
			if (texture == nullptr || data == nullptr || width == 0 || height == 0 || m_device == nullptr)
				return;

			// Get VkImage from the texture
			auto imgHandle = texture->GetNativeImageHandle();
			VkImage srcImage = (imgHandle.backend == NLS::Render::RHI::BackendType::Vulkan) ? static_cast<VkImage>(imgHandle.handle) : VK_NULL_HANDLE;
			if (srcImage == VK_NULL_HANDLE)
				return;

			const auto& desc = texture->GetDesc();
			const auto bytesPerPixel = [srcFormat = desc.format]()
			{
				switch (srcFormat)
				{
				case NLS::Render::RHI::TextureFormat::RGB8: return 3u;
				case NLS::Render::RHI::TextureFormat::RGBA16F: return 8u;
				case NLS::Render::RHI::TextureFormat::RGBA8:
				default:
					return 4u;
				}
			}();
			const VkDeviceSize readbackSize = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * bytesPerPixel;

			// Create staging buffer
			VkBuffer readbackBuffer = VK_NULL_HANDLE;
			VmaAllocation readbackAllocation = VK_NULL_HANDLE;
			const VkBufferCreateInfo bufferInfo{
				VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				nullptr,
				0,
				readbackSize,
				VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				VK_SHARING_MODE_EXCLUSIVE,
				0,
				nullptr
			};
			VmaAllocationCreateInfo allocationInfo{};
			allocationInfo.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
			allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
			if (vmaCreateBuffer(m_allocator, &bufferInfo, &allocationInfo, &readbackBuffer, &readbackAllocation, nullptr) != VK_SUCCESS)
			{
				return;
			}

			// Create command buffer for the copy operation
			VkCommandPool commandPool = VK_NULL_HANDLE;
			const VkCommandPoolCreateInfo poolInfo{
				VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
				nullptr,
				0,
				m_graphicsQueueFamilyIndex
			};
			if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS)
			{
				vmaDestroyBuffer(m_allocator, readbackBuffer, readbackAllocation);
				return;
			}

			VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
			const VkCommandBufferAllocateInfo allocInfo{
				VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				nullptr,
				commandPool,
				VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				1
			};
			if (vkAllocateCommandBuffers(m_device, &allocInfo, &commandBuffer) != VK_SUCCESS)
			{
				vkDestroyCommandPool(m_device, commandPool, nullptr);
				vmaDestroyBuffer(m_allocator, readbackBuffer, readbackAllocation);
				return;
			}

			// Begin command buffer
			const VkCommandBufferBeginInfo beginInfo{
				VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				nullptr,
				VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
				nullptr
			};
			if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
			{
				vkFreeCommandBuffers(m_device, commandPool, 1, &commandBuffer);
				vkDestroyCommandPool(m_device, commandPool, nullptr);
				vmaDestroyBuffer(m_allocator, readbackBuffer, readbackAllocation);
				return;
			}

			// Transition the source from the RHI-tracked state and restore that
			// state after the copy. This is important for swapchain readback:
			// Present must remain Present after the synchronous copy completes.
			const auto sourceLayout = [&texture, srcImage]()
			{
				if (HasVulkanImageLayout(srcImage, 0u, 0u))
					return GetVulkanImageLayout(srcImage, 0u, 0u);
				switch (texture->GetState())
				{
				case NLS::Render::RHI::ResourceState::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
				case NLS::Render::RHI::ResourceState::ShaderRead: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				case NLS::Render::RHI::ResourceState::RenderTarget: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				case NLS::Render::RHI::ResourceState::CopySrc: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				// Readback must restore a legal destination layout. Unknown is a
				// discard hint for normal FrameGraph barriers, but it cannot be used as
				// the destination of this copy's restore barrier.
				default: return VK_IMAGE_LAYOUT_GENERAL;
				}
			}();
			VkImageMemoryBarrier transitionBarrier{
				VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				nullptr,
				0,  // srcAccessMask
				VK_ACCESS_TRANSFER_READ_BIT,  // dstAccessMask
				sourceLayout,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,  // newLayout
				VK_QUEUE_FAMILY_IGNORED,
				VK_QUEUE_FAMILY_IGNORED,
				srcImage,
				{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
			};
			vkCmdPipelineBarrier(
				commandBuffer,
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				0,
				0, nullptr,
				0, nullptr,
				1, &transitionBarrier);

			// Copy image to buffer
			const VkBufferImageCopy copyRegion{
				0,
				0,
				0,
				{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
				{ static_cast<int32_t>(x), static_cast<int32_t>(y), 0 },
				{ width, height, 1 }
			};
			vkCmdCopyImageToBuffer(
				commandBuffer,
				srcImage,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				readbackBuffer,
				1,
				&copyRegion);

			VkImageMemoryBarrier restoreBarrier = transitionBarrier;
			restoreBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			restoreBarrier.dstAccessMask = sourceLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
				? VK_ACCESS_MEMORY_READ_BIT
				: (sourceLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ? VK_ACCESS_SHADER_READ_BIT : 0u);
			restoreBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			restoreBarrier.newLayout = sourceLayout;
			vkCmdPipelineBarrier(
				commandBuffer,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				sourceLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
					? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
					: VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				0,
				0, nullptr,
				0, nullptr,
				1, &restoreBarrier);

			if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
			{
				vkFreeCommandBuffers(m_device, commandPool, 1, &commandBuffer);
				vkDestroyCommandPool(m_device, commandPool, nullptr);
				vmaDestroyBuffer(m_allocator, readbackBuffer, readbackAllocation);
				return;
			}

			// Submit command buffer
			const VkSubmitInfo submitInfo{
				VK_STRUCTURE_TYPE_SUBMIT_INFO,
				nullptr,
				0,
				nullptr,
				nullptr,
				1,
				&commandBuffer,
				0,
				nullptr
			};
			if (vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS)
			{
				vkFreeCommandBuffers(m_device, commandPool, 1, &commandBuffer);
				vkDestroyCommandPool(m_device, commandPool, nullptr);
				vmaDestroyBuffer(m_allocator, readbackBuffer, readbackAllocation);
				return;
			}

			// Wait for completion
			vkQueueWaitIdle(m_graphicsQueue);
			SetVulkanImageLayout(srcImage, sourceLayout, 0u, 0u);

			// Map memory and copy data
			void* mappedData = nullptr;
			if (vmaInvalidateAllocation(m_allocator, readbackAllocation, 0u, readbackSize) == VK_SUCCESS &&
				vmaMapMemory(m_allocator, readbackAllocation, &mappedData) == VK_SUCCESS)
			{
				const VkFormat sourceFormat = ResolveVulkanAttachmentFormat(texture);
				const bool sourceIsBgra = sourceFormat == VK_FORMAT_B8G8R8A8_UNORM ||
					sourceFormat == VK_FORMAT_B8G8R8A8_SRGB;
				if (format == NLS::Render::Settings::EPixelDataFormat::RGB && bytesPerPixel >= 3)
				{
					// RGB format: expand from source format to RGB
					const auto* srcBytes = static_cast<const uint8_t*>(mappedData);
					auto* dstBytes = static_cast<uint8_t*>(data);
					for (uint32_t i = 0; i < width * height; ++i)
					{
						dstBytes[i * 3 + 0] = srcBytes[i * bytesPerPixel + (sourceIsBgra ? 2u : 0u)];
						dstBytes[i * 3 + 1] = srcBytes[i * bytesPerPixel + 1u];
						dstBytes[i * 3 + 2] = srcBytes[i * bytesPerPixel + (sourceIsBgra ? 0u : 2u)];
					}
				}
				else if (sourceIsBgra && bytesPerPixel >= 4u && format == NLS::Render::Settings::EPixelDataFormat::RGBA)
				{
					const auto* srcBytes = static_cast<const uint8_t*>(mappedData);
					auto* dstBytes = static_cast<uint8_t*>(data);
					for (uint32_t i = 0; i < width * height; ++i)
					{
						dstBytes[i * 4u + 0u] = srcBytes[i * bytesPerPixel + 2u];
						dstBytes[i * 4u + 1u] = srcBytes[i * bytesPerPixel + 1u];
						dstBytes[i * 4u + 2u] = srcBytes[i * bytesPerPixel + 0u];
						dstBytes[i * 4u + 3u] = srcBytes[i * bytesPerPixel + 3u];
					}
				}
				else
				{
					std::memcpy(data, mappedData, static_cast<size_t>(readbackSize));
				}
				vmaUnmapMemory(m_allocator, readbackAllocation);
			}

			// Cleanup
			vkFreeCommandBuffers(m_device, commandPool, 1, &commandBuffer);
			vkDestroyCommandPool(m_device, commandPool, nullptr);
			vmaDestroyBuffer(m_allocator, readbackBuffer, readbackAllocation);
#else
			(void)texture;
			(void)x;
			(void)y;
			(void)width;
			(void)height;
			(void)format;
			(void)type;
			(void)data;
#endif
		}

		NLS::Render::RHI::RHIReadbackResult NativeVulkanExplicitDevice::ReadPixelsChecked(
		    const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
		    uint32_t x,
		    uint32_t y,
		    uint32_t width,
		    uint32_t height,
		    NLS::Render::Settings::EPixelDataFormat format,
		    NLS::Render::Settings::EPixelDataType type,
		    void* data)
		{
			if (texture == nullptr || data == nullptr || width == 0u || height == 0u || m_device == VK_NULL_HANDLE)
				return { NLS::Render::RHI::RHIReadbackStatusCode::InvalidArgument, "Vulkan ReadPixels arguments are invalid" };
			const auto nativeHandle = texture->GetNativeImageHandle();
			if (nativeHandle.backend != NLS::Render::RHI::BackendType::Vulkan || nativeHandle.handle == nullptr)
				return { NLS::Render::RHI::RHIReadbackStatusCode::InvalidArgument, "Texture has no Vulkan image handle" };
			ReadPixels(texture, x, y, width, height, format, type, data);
			return { NLS::Render::RHI::RHIReadbackStatusCode::Success, {} };
		}
	}


	std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateNativeVulkanExplicitDevice(
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
		bool dynamicRenderingEnabled)
	{
		return std::make_shared<NativeVulkanExplicitDevice>(
			instance,
			nullptr,
			physicalDevice,
			device,
			graphicsQueue,
			surface,
			swapchain,
			graphicsQueueFamilyIndex,
			capabilities,
			vendor,
			hardware,
			dynamicRenderingEnabled);
	}
#endif

#if NLS_HAS_VULKAN
	std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateVulkanRhiDevice(void* platformWindow)
	{
		if (platformWindow == nullptr)
		{
			NLS_LOG_ERROR("CreateVulkanRhiDevice: platformWindow is nullptr");
			return nullptr;
		}

		// Create Vulkan instance
		const VkApplicationInfo appInfo{
			VK_STRUCTURE_TYPE_APPLICATION_INFO,
			nullptr,
			"Nullus",
			VK_MAKE_VERSION(1, 1, 0),
			"Nullus",
			VK_MAKE_VERSION(1, 0, 0),
			VK_API_VERSION_1_1
		};

		uint32_t requiredExtensionCount = 0;
		const char** requiredExtensions = glfwGetRequiredInstanceExtensions(&requiredExtensionCount);
		if (requiredExtensions == nullptr || requiredExtensionCount == 0u)
		{
			NLS_LOG_ERROR("CreateVulkanRhiDevice: GLFW did not provide Vulkan WSI instance extensions");
			return nullptr;
		}
		std::vector<const char*> instanceExtensions;
		if (requiredExtensions != nullptr)
			instanceExtensions.assign(requiredExtensions, requiredExtensions + requiredExtensionCount);

		uint32_t availableExtensionCount = 0u;
		std::vector<VkExtensionProperties> availableExtensions;
		if (vkEnumerateInstanceExtensionProperties(nullptr, &availableExtensionCount, nullptr) == VK_SUCCESS)
		{
			availableExtensions.resize(availableExtensionCount);
			vkEnumerateInstanceExtensionProperties(nullptr, &availableExtensionCount, availableExtensions.data());
		}
		const auto hasInstanceExtension = [&availableExtensions](const char* name)
		{
			return std::any_of(availableExtensions.begin(), availableExtensions.end(), [name](const VkExtensionProperties& extension)
			{
				return std::strcmp(extension.extensionName, name) == 0;
			});
		};
#if defined(VK_EXT_debug_utils)
		const bool debugUtilsAvailable = hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		if (debugUtilsAvailable && std::find(instanceExtensions.begin(), instanceExtensions.end(), VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == instanceExtensions.end())
			instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#else
		const bool debugUtilsAvailable = false;
#endif

		std::vector<const char*> validationLayers;
		const bool validationRequested = std::getenv("NLS_VULKAN_VALIDATION") != nullptr || std::getenv("VK_INSTANCE_LAYERS") != nullptr;
		if (validationRequested)
		{
			uint32_t layerCount = 0u;
			std::vector<VkLayerProperties> availableLayers;
			if (vkEnumerateInstanceLayerProperties(&layerCount, nullptr) == VK_SUCCESS)
			{
				availableLayers.resize(layerCount);
				vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());
			}
			const auto hasLayer = [&availableLayers](const char* name)
			{
				return std::any_of(availableLayers.begin(), availableLayers.end(), [name](const VkLayerProperties& layer)
				{
					return std::strcmp(layer.layerName, name) == 0;
				});
			};
			if (hasLayer("VK_LAYER_KHRONOS_validation"))
				validationLayers.push_back("VK_LAYER_KHRONOS_validation");
			else if (hasLayer("VK_LAYER_LUNARG_standard_validation"))
				validationLayers.push_back("VK_LAYER_LUNARG_standard_validation");
			else if (std::getenv("NLS_VULKAN_VALIDATION") != nullptr)
				NLS_LOG_WARNING("CreateVulkanRhiDevice: NLS_VULKAN_VALIDATION requested but no validation layer is installed");
		}

#if defined(VK_EXT_debug_utils)
		VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
		if (debugUtilsAvailable)
		{
			debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
			debugCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
			debugCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
			debugCreateInfo.pfnUserCallback = VulkanDebugCallback;
		}
#endif

		VkInstanceCreateInfo instanceCreateInfo{};
		instanceCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
#if defined(VK_EXT_debug_utils)
		instanceCreateInfo.pNext = debugUtilsAvailable ? &debugCreateInfo : nullptr;
#else
		instanceCreateInfo.pNext = nullptr;
#endif
		instanceCreateInfo.pApplicationInfo = &appInfo;
		instanceCreateInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
		instanceCreateInfo.ppEnabledLayerNames = validationLayers.empty() ? nullptr : validationLayers.data();
		instanceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size());
		instanceCreateInfo.ppEnabledExtensionNames = instanceExtensions.empty() ? nullptr : instanceExtensions.data();

		VkInstance instance = VK_NULL_HANDLE;
		if (vkCreateInstance(&instanceCreateInfo, nullptr, &instance) != VK_SUCCESS)
		{
			NLS_LOG_ERROR("CreateVulkanRhiDevice: failed to create Vulkan instance");
			return nullptr;
		}

		VulkanDebugMessengerHandle debugMessenger = VK_NULL_HANDLE;
#if defined(VK_EXT_debug_utils)
		if (debugUtilsAvailable)
		{
			if (CreateVulkanDebugMessenger(instance, debugCreateInfo, &debugMessenger) != VK_SUCCESS)
				NLS_LOG_WARNING("CreateVulkanRhiDevice: failed to create Vulkan debug messenger");
		}
#endif

		// Create surface from window
		VkSurfaceKHR surface = VK_NULL_HANDLE;
		GLFWwindow* window = static_cast<GLFWwindow*>(platformWindow);
		if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS)
		{
			NLS_LOG_ERROR("CreateVulkanRhiDevice: failed to create Vulkan surface");
			DestroyVulkanDebugMessenger(instance, debugMessenger);
			vkDestroyInstance(instance, nullptr);
			return nullptr;
		}

		// Enumerate and select physical device
		uint32_t physicalDeviceCount = 0;
		if (vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, nullptr) != VK_SUCCESS || physicalDeviceCount == 0)
		{
			NLS_LOG_ERROR("CreateVulkanRhiDevice: failed to enumerate physical devices");
			vkDestroySurfaceKHR(instance, surface, nullptr);
			DestroyVulkanDebugMessenger(instance, debugMessenger);
			vkDestroyInstance(instance, nullptr);
			return nullptr;
		}

		std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
		vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, physicalDevices.data());

		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		uint32_t graphicsQueueFamilyIndex = 0;
		for (const auto device : physicalDevices)
		{
			uint32_t queueFamilyCount = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
			std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
			vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

			uint32_t candidateQueueFamily = UINT32_MAX;
			for (uint32_t queueFamilyIndex = 0; queueFamilyIndex < queueFamilyCount; ++queueFamilyIndex)
			{
				VkBool32 supportsPresent = VK_FALSE;
				if (vkGetPhysicalDeviceSurfaceSupportKHR(device, queueFamilyIndex, surface, &supportsPresent) != VK_SUCCESS)
					continue;
				if ((queueFamilies[queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && supportsPresent == VK_TRUE)
				{
					candidateQueueFamily = queueFamilyIndex;
					break;
				}
			}
			if (candidateQueueFamily != UINT32_MAX)
			{
				uint32_t deviceExtensionCount = 0u;
				if (vkEnumerateDeviceExtensionProperties(device, nullptr, &deviceExtensionCount, nullptr) != VK_SUCCESS)
					continue;
				std::vector<VkExtensionProperties> deviceExtensions(deviceExtensionCount);
				if (vkEnumerateDeviceExtensionProperties(device, nullptr, &deviceExtensionCount, deviceExtensions.data()) != VK_SUCCESS)
					continue;
				const bool hasSwapchainExtension = std::any_of(deviceExtensions.begin(), deviceExtensions.end(), [](const VkExtensionProperties& extension)
				{
					return std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
				});
				if (hasSwapchainExtension)
				{
					physicalDevice = device;
					graphicsQueueFamilyIndex = candidateQueueFamily;
				}
			}

			if (physicalDevice != VK_NULL_HANDLE)
				break;
		}

		if (physicalDevice == VK_NULL_HANDLE)
		{
			NLS_LOG_ERROR("CreateVulkanRhiDevice: failed to find a Vulkan graphics queue family");
			vkDestroySurfaceKHR(instance, surface, nullptr);
			DestroyVulkanDebugMessenger(instance, debugMessenger);
			vkDestroyInstance(instance, nullptr);
			return nullptr;
		}

		// Create logical device
		const float queuePriority = 1.0f;
		const VkDeviceQueueCreateInfo queueCreateInfo{
			VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			nullptr,
			0,
			graphicsQueueFamilyIndex,
			1,
			&queuePriority
		};

		// Vulkan 1.1 baseline: use the classic render-pass path.
		constexpr const char* deviceExtensions[] = {
			VK_KHR_SWAPCHAIN_EXTENSION_NAME
		};

		const VkDeviceCreateInfo deviceCreateInfo{
			VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			nullptr,
			0,
			1,
			&queueCreateInfo,
			0,
			nullptr,
			1,
			deviceExtensions,
			nullptr
		};

		VkDevice device = VK_NULL_HANDLE;
		if (vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &device) != VK_SUCCESS)
		{
			NLS_LOG_ERROR("CreateVulkanRhiDevice: failed to create Vulkan logical device");
			vkDestroySurfaceKHR(instance, surface, nullptr);
			DestroyVulkanDebugMessenger(instance, debugMessenger);
			vkDestroyInstance(instance, nullptr);
			return nullptr;
		}

		// Get graphics queue
		VkQueue graphicsQueue = VK_NULL_HANDLE;
		vkGetDeviceQueue(device, graphicsQueueFamilyIndex, 0, &graphicsQueue);

		// Build capabilities
		NLS::Render::RHI::RHIDeviceCapabilities capabilities{};
		capabilities.backendReady = true;
		capabilities.supportsGraphics = true;
		capabilities.supportsCompute = true;
		capabilities.supportsSwapchain = true;
		capabilities.supportsFramebufferBlit = true;
		// Vulkan uses the same transfer/blit path for depth resources as the
		// existing renderer contract.  Report it so the editor readiness gate
		// can select the offscreen Scene View path.
		capabilities.supportsDepthBlit = true;
		capabilities.supportsCurrentSceneRenderer = true;
		capabilities.supportsOffscreenFramebuffers = true;
		capabilities.supportsFramebufferReadback = true;
		capabilities.supportsEditorPickingReadback = true;
		capabilities.supportsUITextureHandles = true;
		capabilities.supportsCubemaps = true;
		capabilities.supportsMultiRenderTargets = true;
		capabilities.supportsExplicitBarriers = true;
		capabilities.supportsCentralizedDescriptorManagement = true;
		capabilities.supportsPipelineStateCache = true;
		// The threaded foundation path uses the backend's explicit resource
		// lifetime management even though command recording remains serial.
		capabilities.supportsTransientResourceAllocator = true;
		capabilities.supportsAsyncReadback = true;
		capabilities.supportsUIOverlayFrameGraph = true;
		capabilities.maxTextureDimension2D = 4096;  // Conservative default
		capabilities.maxColorAttachments = 8;
		capabilities.SetFeature(
			NLS::Render::RHI::RHIDeviceFeature::UITextureAtlasRegionUpload,
			true);

		// Get vendor/hardware info
		VkPhysicalDeviceProperties properties;
		vkGetPhysicalDeviceProperties(physicalDevice, &properties);
		std::string vendor = "Unknown";
		std::string hardware = std::string(properties.deviceName);

		NLS_LOG_INFO("CreateVulkanRhiDevice: created Vulkan device directly, vendor=" + vendor + ", device=" + hardware);

		return std::make_shared<NativeVulkanExplicitDevice>(
			instance,
			debugMessenger,
			physicalDevice,
			device,
			graphicsQueue,
			surface,
			nullptr,  // No initial swapchain, created later via CreateSwapchain
			graphicsQueueFamilyIndex,
			capabilities,
			vendor,
			hardware,
			false);
	}
#else
	std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateVulkanRhiDevice(void* /*platformWindow*/)
	{
		NLS_LOG_WARNING("CreateVulkanRhiDevice: Vulkan not available at build time");
		return nullptr;
	}
#endif
}
