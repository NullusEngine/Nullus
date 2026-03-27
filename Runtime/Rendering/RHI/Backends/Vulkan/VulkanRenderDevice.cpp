#include "Rendering/RHI/Backends/Vulkan/VulkanRenderDevice.h"

#include <Debug/Logger.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <set>
#include <unordered_map>
#include <vector>

#include "Rendering/RHI/BindingPointMap.h"
#include "Rendering/Resources/BindingSetInstance.h"

#if NLS_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif
#if NLS_HAS_VULKAN && NLS_HAS_IMGUI_VULKAN_BACKEND
#include "ImGui/backends/imgui_impl_vulkan.h"
#endif

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace NLS::Render::Backend
{
	namespace
	{
#if NLS_HAS_VULKAN
		VkPrimitiveTopology ToVkPrimitiveTopology(NLS::Render::Settings::EPrimitiveMode primitiveMode)
		{
			switch (primitiveMode)
			{
			case NLS::Render::Settings::EPrimitiveMode::LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
			case NLS::Render::Settings::EPrimitiveMode::POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
			case NLS::Render::Settings::EPrimitiveMode::TRIANGLES:
			default:
				return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			}
		}

		VkCullModeFlags ToVkCullMode(NLS::Render::Settings::ECullFace cullFace)
		{
			switch (cullFace)
			{
			case NLS::Render::Settings::ECullFace::FRONT: return VK_CULL_MODE_FRONT_BIT;
			case NLS::Render::Settings::ECullFace::BACK: return VK_CULL_MODE_BACK_BIT;
			case NLS::Render::Settings::ECullFace::FRONT_AND_BACK: return VK_CULL_MODE_FRONT_AND_BACK;
			default: return VK_CULL_MODE_NONE;
			}
		}

		VkCompareOp ToVkCompareOp(NLS::Render::Settings::EComparaisonAlgorithm algorithm)
		{
			switch (algorithm)
			{
			case NLS::Render::Settings::EComparaisonAlgorithm::LESS: return VK_COMPARE_OP_LESS;
			case NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
			case NLS::Render::Settings::EComparaisonAlgorithm::GREATER: return VK_COMPARE_OP_GREATER;
			case NLS::Render::Settings::EComparaisonAlgorithm::GREATER_EQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
			case NLS::Render::Settings::EComparaisonAlgorithm::EQUAL: return VK_COMPARE_OP_EQUAL;
			case NLS::Render::Settings::EComparaisonAlgorithm::NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
			case NLS::Render::Settings::EComparaisonAlgorithm::ALWAYS: return VK_COMPARE_OP_ALWAYS;
			case NLS::Render::Settings::EComparaisonAlgorithm::NEVER:
			default:
				return VK_COMPARE_OP_NEVER;
			}
		}

		VkFormat ToVkFormat(NLS::Render::RHI::TextureFormat format)
		{
			switch (format)
			{
			case NLS::Render::RHI::TextureFormat::RGB8: return VK_FORMAT_R8G8B8_UNORM;
			case NLS::Render::RHI::TextureFormat::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
			case NLS::Render::RHI::TextureFormat::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
			case NLS::Render::RHI::TextureFormat::Depth24Stencil8: return VK_FORMAT_D24_UNORM_S8_UINT;
			default: return VK_FORMAT_R8G8B8A8_UNORM;
			}
		}

		VkFilter ToVkFilter(NLS::Render::RHI::TextureFilter filter)
		{
			return filter == NLS::Render::RHI::TextureFilter::Nearest
				? VK_FILTER_NEAREST
				: VK_FILTER_LINEAR;
		}

		VkSamplerAddressMode ToVkSamplerAddressMode(NLS::Render::RHI::TextureWrap wrap)
		{
			return wrap == NLS::Render::RHI::TextureWrap::ClampToEdge
				? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
				: VK_SAMPLER_ADDRESS_MODE_REPEAT;
		}

		VkImageViewType ToVkImageViewType(NLS::Render::RHI::TextureDimension dimension)
		{
			return dimension == NLS::Render::RHI::TextureDimension::TextureCube
				? VK_IMAGE_VIEW_TYPE_CUBE
				: VK_IMAGE_VIEW_TYPE_2D;
		}

		VkImageCreateFlags GetVkImageCreateFlags(NLS::Render::RHI::TextureDimension dimension)
		{
			return dimension == NLS::Render::RHI::TextureDimension::TextureCube
				? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT
				: 0u;
		}

		VkImageUsageFlags GetVkImageUsage(const NLS::Render::RHI::TextureDesc& desc)
		{
			VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			if (NLS::Render::RHI::HasUsage(desc.usage, NLS::Render::RHI::TextureUsage::Sampled))
				usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
			if (NLS::Render::RHI::HasUsage(desc.usage, NLS::Render::RHI::TextureUsage::ColorAttachment))
				usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			if (NLS::Render::RHI::HasUsage(desc.usage, NLS::Render::RHI::TextureUsage::DepthStencilAttachment))
				usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
			return usage;
		}

		VkImageLayout GetPreferredTextureLayout(const NLS::Render::RHI::TextureDesc& desc)
		{
			if (NLS::Render::RHI::HasUsage(desc.usage, NLS::Render::RHI::TextureUsage::DepthStencilAttachment))
				return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			if (NLS::Render::RHI::HasUsage(desc.usage, NLS::Render::RHI::TextureUsage::ColorAttachment))
				return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			if (NLS::Render::RHI::HasUsage(desc.usage, NLS::Render::RHI::TextureUsage::Sampled))
				return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			return VK_IMAGE_LAYOUT_GENERAL;
		}

		VkShaderStageFlagBits ToVkShaderStage(NLS::Render::RHI::ShaderStage stage)
		{
			switch (stage)
			{
			case NLS::Render::RHI::ShaderStage::Vertex: return VK_SHADER_STAGE_VERTEX_BIT;
			case NLS::Render::RHI::ShaderStage::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
			case NLS::Render::RHI::ShaderStage::Compute: return VK_SHADER_STAGE_COMPUTE_BIT;
			default: return VK_SHADER_STAGE_VERTEX_BIT;
			}
		}
#endif

		uint64_t HashBytes(const std::vector<uint8_t>& bytes)
		{
			uint64_t hash = 1469598103934665603ull;
			for (const auto byte : bytes)
			{
				hash ^= static_cast<uint64_t>(byte);
				hash *= 1099511628211ull;
			}
			return hash;
		}

		std::string BuildPipelineCacheKey(const NLS::Render::RHI::GraphicsPipelineDesc& desc)
		{
			std::string key = std::to_string(static_cast<int>(desc.rasterState.cullFace)) + "|" +
				std::to_string(desc.rasterState.culling ? 1 : 0) + "|" +
				std::to_string(desc.depthStencilState.depthTest ? 1 : 0) + "|" +
				std::to_string(desc.depthStencilState.depthWrite ? 1 : 0) + "|" +
				std::to_string(static_cast<int>(desc.depthStencilState.depthCompare)) + "|" +
				std::to_string(desc.blendState.enabled ? 1 : 0) + "|" +
				std::to_string(desc.layout.uniformBufferBindingCount) + "|" +
				std::to_string(desc.layout.sampledTextureBindingCount) + "|" +
				std::to_string(desc.layout.samplerBindingCount) + "|" +
				std::to_string(desc.layout.storageBufferBindingCount) + "|" +
				std::to_string(static_cast<int>(desc.primitiveMode)) + "|" +
				std::to_string(desc.attachmentLayout.sampleCount) + "|" +
				std::to_string(desc.attachmentLayout.hasDepthAttachment ? 1 : 0);

			for (const auto format : desc.attachmentLayout.colorAttachmentFormats)
				key += "|rtf:" + std::to_string(static_cast<int>(format));
			key += "|dsf:" + std::to_string(static_cast<int>(desc.attachmentLayout.depthAttachmentFormat));

			for (const auto& stage : desc.shaderStages)
			{
				key += "|" + std::to_string(static_cast<int>(stage.stage)) +
					":" + std::to_string(static_cast<int>(stage.targetPlatform)) +
					":" + stage.entryPoint +
					":" + std::to_string(stage.bytecode.size()) +
					":" + std::to_string(HashBytes(stage.bytecode));
			}

			if (desc.reflection != nullptr)
			{
				for (const auto& constantBuffer : desc.reflection->constantBuffers)
				{
					key += "|cb:" + constantBuffer.name +
						":" + std::to_string(constantBuffer.bindingSpace) +
						":" + std::to_string(constantBuffer.bindingIndex);
				}

				for (const auto& property : desc.reflection->properties)
				{
					if (property.kind != NLS::Render::Resources::ShaderResourceKind::SampledTexture &&
						property.kind != NLS::Render::Resources::ShaderResourceKind::Sampler)
					{
						continue;
					}

					key += "|res:" + property.name +
						":" + std::to_string(static_cast<int>(property.kind)) +
						":" + std::to_string(property.bindingSpace) +
						":" + std::to_string(property.bindingIndex);
				}
			}

			return key;
		}

#if NLS_HAS_VULKAN
		struct PipelineCacheEntry
		{
			struct DescriptorBinding
			{
				std::string name;
				NLS::Render::Resources::ShaderResourceKind kind = NLS::Render::Resources::ShaderResourceKind::SampledTexture;
				NLS::Render::Resources::UniformType type = NLS::Render::Resources::UniformType::UNIFORM_FLOAT;
				uint32_t bindingSpace = 0;
				uint32_t bindingIndex = 0;
				uint32_t descriptorCount = 1;
				NLS::Render::RHI::TextureDimension defaultDimension = NLS::Render::RHI::TextureDimension::Texture2D;
			};

			VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
			VkPipeline pipeline = VK_NULL_HANDLE;
			std::vector<VkDescriptorSetLayout> descriptorSetLayouts;
			std::vector<uint32_t> descriptorSetSpaces;
			std::vector<VkShaderModule> shaderModules;
			std::vector<DescriptorBinding> descriptorBindings;
		};
#endif

		class VulkanTextureResource final : public NLS::Render::RHI::IRHITexture
		{
		public:
			VulkanTextureResource(uint32_t id, NLS::Render::RHI::TextureDesc desc, std::function<void(uint32_t)> destroy)
				: m_id(id), m_desc(desc), m_destroy(std::move(destroy))
			{
			}

			~VulkanTextureResource() override
			{
				if (m_destroy && m_id != 0)
					m_destroy(m_id);
			}

			NLS::Render::RHI::RHIResourceType GetResourceType() const override { return NLS::Render::RHI::RHIResourceType::Texture; }
			uint32_t GetResourceId() const override { return m_id; }
			NLS::Render::RHI::TextureDimension GetDimension() const override { return m_desc.dimension; }
			const NLS::Render::RHI::TextureDesc& GetDesc() const override { return m_desc; }
			void SetDesc(const NLS::Render::RHI::TextureDesc& desc) override { m_desc = desc; }

		private:
			uint32_t m_id = 0;
			NLS::Render::RHI::TextureDesc m_desc{};
			std::function<void(uint32_t)> m_destroy;
		};

		class VulkanBufferResource final : public NLS::Render::RHI::IRHIBuffer
		{
		public:
			VulkanBufferResource(uint32_t id, NLS::Render::RHI::BufferType type, std::function<void(uint32_t)> destroy)
				: m_id(id), m_type(type), m_destroy(std::move(destroy))
			{
			}

			~VulkanBufferResource() override
			{
				if (m_destroy && m_id != 0)
					m_destroy(m_id);
			}

			NLS::Render::RHI::RHIResourceType GetResourceType() const override { return NLS::Render::RHI::RHIResourceType::Buffer; }
			uint32_t GetResourceId() const override { return m_id; }
			NLS::Render::RHI::BufferType GetBufferType() const override { return m_type; }
			size_t GetSize() const override { return m_size; }
			void SetSize(size_t size) override { m_size = size; }

		private:
			uint32_t m_id = 0;
			NLS::Render::RHI::BufferType m_type = NLS::Render::RHI::BufferType::Uniform;
			size_t m_size = 0;
			std::function<void(uint32_t)> m_destroy;
		};
	}

	struct VulkanRenderDevice::Impl
	{
#if NLS_HAS_VULKAN
		struct BufferResource
		{
			VkBuffer buffer = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			VkDeviceSize size = 0;
		};

		struct TextureResource
		{
			VkImage image = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			VkImageView view = VK_NULL_HANDLE;
			NLS::Render::RHI::TextureDesc desc{};
			VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
			bool initialized = false;
		};

		struct FramebufferResource
		{
			std::vector<uint32_t> colorTextureIds;
			uint32_t depthTextureId = 0;
			uint32_t drawBufferCount = 0;
			VkRenderPass renderPass = VK_NULL_HANDLE;
			VkFramebuffer framebuffer = VK_NULL_HANDLE;
			bool attachmentsDirty = true;
		};

		struct FrameContext
		{
			VkImage image = VK_NULL_HANDLE;
			VkImageView view = VK_NULL_HANDLE;
			VkFramebuffer framebuffer = VK_NULL_HANDLE;
			VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
			VkFence fence = VK_NULL_HANDLE;
			VkSemaphore imageAvailableSemaphore = VK_NULL_HANDLE;
			VkSemaphore renderFinishedSemaphore = VK_NULL_HANDLE;
			VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
			bool imageInitialized = false;
			bool renderPassActive = false;
			bool imageAvailablePendingWait = false;
		};

		VkInstance instance = VK_NULL_HANDLE;
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE;
		VkQueue graphicsQueue = VK_NULL_HANDLE;
		uint32_t graphicsQueueFamilyIndex = 0;
		VkPhysicalDeviceProperties properties{};
		VkSurfaceKHR surface = VK_NULL_HANDLE;
		VkSwapchainKHR swapchain = VK_NULL_HANDLE;
		std::vector<VkImage> swapchainImages;
		std::vector<FrameContext> frameContexts;
		std::vector<VkFence> acquireFences;
		VkCommandPool commandPool = VK_NULL_HANDLE;
		VkRenderPass swapchainRenderPass = VK_NULL_HANDLE;
		VkDescriptorPool runtimeDescriptorPool = VK_NULL_HANDLE;
		VkDescriptorPool uiDescriptorPool = VK_NULL_HANDLE;
		VkSampler linearWrapSampler = VK_NULL_HANDLE;
		VkSampler linearClampSampler = VK_NULL_HANDLE;
		VkSampler pointWrapSampler = VK_NULL_HANDLE;
		VkSampler pointClampSampler = VK_NULL_HANDLE;
		VkSampler uiTextureSampler = VK_NULL_HANDLE;
		VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
		VkExtent2D swapchainExtent{ 0, 0 };
		GLFWwindow* swapchainWindow = nullptr;
		uint32_t swapchainImageCount = 2;
		bool swapchainVsync = true;
		uint32_t currentSwapchainImageIndex = 0;
		float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
		bool hasPendingCommands = false;
		bool isFrameRecording = false;
		bool swapchainImageAcquired = false;
		VkSemaphore pendingAcquireSemaphore = VK_NULL_HANDLE;
		std::unordered_map<uint32_t, BufferResource> buffers;
		std::unordered_map<uint32_t, TextureResource> textures;
		std::unordered_map<uint32_t, FramebufferResource> framebuffers;
		std::unordered_map<uint32_t, std::weak_ptr<NLS::Render::RHI::IRHITexture>> textureObjects;
		std::unordered_map<uint32_t, std::weak_ptr<NLS::Render::RHI::IRHIBuffer>> bufferObjects;
		std::unordered_map<NLS::Render::RHI::TextureDimension, uint32_t> fallbackTextureIds;
		std::unordered_map<uint32_t, VkDescriptorSet> uiTextureHandles;
		std::unordered_map<std::string, PipelineCacheEntry> pipelineCache;
		std::unordered_map<NLS::Render::RHI::BufferType, uint32_t> boundBuffers;
		std::unordered_map<uint32_t, uint32_t> uniformBufferBindings;
		std::set<uint32_t> uiTexturesUsedThisFrame;
		NLS::Render::RHI::GraphicsPipelineDesc currentPipelineDesc{};
		const NLS::Render::Resources::BindingSetInstance* currentBindingSet = nullptr;
		uint32_t activeRenderTarget = 0;
		uint32_t boundFramebuffer = 0;
		uint32_t boundTexture = 0;
		uint32_t fallbackUniformBufferId = 0;
		uint32_t nextResourceId = 1;
		bool hasLoggedDrawStub = false;
		bool swapchainHasColorContent = false;
#endif
		std::string vendor = "Vulkan";
		std::string hardware = "Stub";
		std::string version = "Stub";
		std::string shadingLanguageVersion = "SPIR-V";
		NLS::Render::RHI::RHIDeviceCapabilities capabilities{};
		bool backendReady = false;
	};

#if NLS_HAS_VULKAN
	namespace
	{
		VkImageAspectFlags GetImageAspectMask(NLS::Render::RHI::TextureFormat format)
		{
			if (format == NLS::Render::RHI::TextureFormat::Depth24Stencil8)
				return static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
			return VK_IMAGE_ASPECT_COLOR_BIT;
		}

		bool IsDepthStencilFormat(NLS::Render::RHI::TextureFormat format)
		{
			return format == NLS::Render::RHI::TextureFormat::Depth24Stencil8;
		}

		uint32_t FindMemoryTypeIndex(VkPhysicalDevice physicalDevice, uint32_t typeBits, VkMemoryPropertyFlags properties)
		{
			VkPhysicalDeviceMemoryProperties memoryProperties{};
			vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
			for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
			{
				if (((typeBits & (1u << i)) != 0u) && (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
					return i;
			}
			return 0u;
		}

		auto ExecuteImmediateCommands = [](auto& impl, const std::function<bool(VkCommandBuffer)>& record)
		{
			if (impl.device == VK_NULL_HANDLE || impl.commandPool == VK_NULL_HANDLE || impl.graphicsQueue == VK_NULL_HANDLE)
				return false;

			VkCommandBufferAllocateInfo allocateInfo{
				VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				nullptr,
				impl.commandPool,
				VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				1
			};
			VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
			if (vkAllocateCommandBuffers(impl.device, &allocateInfo, &commandBuffer) != VK_SUCCESS)
				return false;

			const VkCommandBufferBeginInfo beginInfo{
				VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				nullptr,
				VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
				nullptr
			};
			if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
			{
				vkFreeCommandBuffers(impl.device, impl.commandPool, 1, &commandBuffer);
				return false;
			}

			if (!record(commandBuffer) || vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
			{
				vkFreeCommandBuffers(impl.device, impl.commandPool, 1, &commandBuffer);
				return false;
			}

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
			const VkFenceCreateInfo fenceInfo{
				VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
				nullptr,
				0
			};
			VkFence fence = VK_NULL_HANDLE;
			if (vkCreateFence(impl.device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
			{
				vkFreeCommandBuffers(impl.device, impl.commandPool, 1, &commandBuffer);
				return false;
			}

			const bool submitSucceeded = vkQueueSubmit(impl.graphicsQueue, 1, &submitInfo, fence) == VK_SUCCESS &&
				vkWaitForFences(impl.device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;

			vkDestroyFence(impl.device, fence, nullptr);
			vkFreeCommandBuffers(impl.device, impl.commandPool, 1, &commandBuffer);
			return submitSucceeded;
		};

		auto TransitionTexture = [](auto& impl, VkCommandBuffer commandBuffer, auto& texture, VkImageLayout newLayout, VkAccessFlags dstAccess, VkPipelineStageFlags dstStage)
		{
			if (texture.image == VK_NULL_HANDLE || commandBuffer == VK_NULL_HANDLE || texture.layout == newLayout)
				return;

			VkAccessFlags srcAccess = 0;
			VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			switch (texture.layout)
			{
			case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
				srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
				srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
				break;
			case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
				srcAccess = VK_ACCESS_TRANSFER_READ_BIT;
				srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
				break;
			case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
				srcAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
				srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				break;
			case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
				srcAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
				srcStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
				break;
			case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
				srcAccess = VK_ACCESS_SHADER_READ_BIT;
				srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
				break;
			case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
				srcAccess = 0;
				srcStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
				break;
			default:
				break;
			}

			VkImageMemoryBarrier barrier{
				VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				nullptr,
				srcAccess,
				dstAccess,
				texture.initialized ? texture.layout : VK_IMAGE_LAYOUT_UNDEFINED,
				newLayout,
				VK_QUEUE_FAMILY_IGNORED,
				VK_QUEUE_FAMILY_IGNORED,
				texture.image,
				{ GetImageAspectMask(texture.desc.format), 0, 1, 0, NLS::Render::RHI::GetTextureLayerCount(texture.desc.dimension) }
			};
			vkCmdPipelineBarrier(
				commandBuffer,
				texture.initialized ? srcStage : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				dstStage,
				0,
				0, nullptr,
				0, nullptr,
				1, &barrier);

			texture.layout = newLayout;
			texture.initialized = true;
		};

		auto EndActiveRenderPass = [](auto& impl)
		{
			if (!impl.isFrameRecording || impl.frameContexts.empty())
				return;

			auto& frame = impl.frameContexts[impl.currentSwapchainImageIndex % impl.frameContexts.size()];
			if (!frame.renderPassActive || frame.commandBuffer == VK_NULL_HANDLE)
				return;

			vkCmdEndRenderPass(frame.commandBuffer);
			frame.renderPassActive = false;
			impl.activeRenderTarget = std::numeric_limits<uint32_t>::max();
		};

		auto EnsureFrameRecording = [](auto& impl) -> bool
		{
			if (impl.device == VK_NULL_HANDLE || impl.swapchain == VK_NULL_HANDLE || impl.frameContexts.empty())
				return false;

			if (impl.isFrameRecording)
				return true;

			if (!impl.swapchainImageAcquired)
			{
				auto& acquireFrame = impl.frameContexts[impl.currentSwapchainImageIndex % impl.frameContexts.size()];
				uint32_t imageIndex = 0;
				if (vkAcquireNextImageKHR(impl.device, impl.swapchain, UINT64_MAX, acquireFrame.imageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex) != VK_SUCCESS)
					return false;

				impl.currentSwapchainImageIndex = imageIndex;
				impl.swapchainImageAcquired = true;
				impl.pendingAcquireSemaphore = acquireFrame.imageAvailableSemaphore;
				impl.swapchainHasColorContent = false;
			}

			auto& currentFrame = impl.frameContexts[impl.currentSwapchainImageIndex % impl.frameContexts.size()];
			if (currentFrame.fence != VK_NULL_HANDLE)
			{
				vkWaitForFences(impl.device, 1, &currentFrame.fence, VK_TRUE, UINT64_MAX);
				vkResetFences(impl.device, 1, &currentFrame.fence);
			}

			currentFrame.renderPassActive = false;
			vkResetCommandBuffer(currentFrame.commandBuffer, 0);
			const VkCommandBufferBeginInfo beginInfo{
				VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				nullptr,
				VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
				nullptr
			};
			if (vkBeginCommandBuffer(currentFrame.commandBuffer, &beginInfo) != VK_SUCCESS)
				return false;

			if (impl.runtimeDescriptorPool != VK_NULL_HANDLE)
				vkResetDescriptorPool(impl.device, impl.runtimeDescriptorPool, 0);

			impl.isFrameRecording = true;
			return true;
		};

		auto EnsureFramebufferReady = [](auto& impl, uint32_t framebufferId) -> bool
		{
			auto framebufferIt = impl.framebuffers.find(framebufferId);
			if (framebufferIt == impl.framebuffers.end())
				return false;

			auto& framebuffer = framebufferIt->second;
			if (!framebuffer.attachmentsDirty && framebuffer.renderPass != VK_NULL_HANDLE && framebuffer.framebuffer != VK_NULL_HANDLE)
				return true;

			if (framebuffer.framebuffer != VK_NULL_HANDLE)
			{
				vkDestroyFramebuffer(impl.device, framebuffer.framebuffer, nullptr);
				framebuffer.framebuffer = VK_NULL_HANDLE;
			}
			if (framebuffer.renderPass != VK_NULL_HANDLE)
			{
				vkDestroyRenderPass(impl.device, framebuffer.renderPass, nullptr);
				framebuffer.renderPass = VK_NULL_HANDLE;
			}

			const uint32_t colorCount = framebuffer.drawBufferCount > 0
				? framebuffer.drawBufferCount
				: static_cast<uint32_t>(framebuffer.colorTextureIds.size());
			if (colorCount == 0)
				return false;

			std::vector<VkAttachmentDescription> attachments;
			std::vector<VkAttachmentReference> colorRefs;
			std::vector<VkImageView> attachmentViews;
			attachments.reserve(colorCount + (framebuffer.depthTextureId != 0 ? 1u : 0u));
			colorRefs.reserve(colorCount);
			attachmentViews.reserve(colorCount + (framebuffer.depthTextureId != 0 ? 1u : 0u));

			uint32_t width = 0;
			uint32_t height = 0;
			for (uint32_t i = 0; i < colorCount; ++i)
			{
				if (i >= framebuffer.colorTextureIds.size())
					return false;

				auto textureIt = impl.textures.find(framebuffer.colorTextureIds[i]);
				if (textureIt == impl.textures.end() || textureIt->second.view == VK_NULL_HANDLE)
					return false;

				width = textureIt->second.desc.width;
				height = textureIt->second.desc.height;
				attachments.push_back({
					0,
					ToVkFormat(textureIt->second.desc.format),
					VK_SAMPLE_COUNT_1_BIT,
					VK_ATTACHMENT_LOAD_OP_LOAD,
					VK_ATTACHMENT_STORE_OP_STORE,
					VK_ATTACHMENT_LOAD_OP_DONT_CARE,
					VK_ATTACHMENT_STORE_OP_DONT_CARE,
					VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
				});
				colorRefs.push_back({ i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
				attachmentViews.push_back(textureIt->second.view);
			}

			VkAttachmentReference depthRef{};
			VkAttachmentReference* depthRefPtr = nullptr;
			if (framebuffer.depthTextureId != 0)
			{
				auto depthIt = impl.textures.find(framebuffer.depthTextureId);
				if (depthIt == impl.textures.end() || depthIt->second.view == VK_NULL_HANDLE)
					return false;

				if (width == 0 || height == 0)
				{
					width = depthIt->second.desc.width;
					height = depthIt->second.desc.height;
				}

				attachments.push_back({
					0,
					ToVkFormat(depthIt->second.desc.format),
					VK_SAMPLE_COUNT_1_BIT,
					VK_ATTACHMENT_LOAD_OP_LOAD,
					VK_ATTACHMENT_STORE_OP_STORE,
					VK_ATTACHMENT_LOAD_OP_LOAD,
					VK_ATTACHMENT_STORE_OP_STORE,
					VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
					VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
				});
				depthRef = { static_cast<uint32_t>(attachments.size() - 1u), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
				depthRefPtr = &depthRef;
				attachmentViews.push_back(depthIt->second.view);
			}

			const VkSubpassDescription subpass{
				0,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				0,
				nullptr,
				static_cast<uint32_t>(colorRefs.size()),
				colorRefs.data(),
				nullptr,
				depthRefPtr,
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
				static_cast<uint32_t>(attachments.size()),
				attachments.data(),
				1,
				&subpass,
				1,
				&dependency
			};
			if (vkCreateRenderPass(impl.device, &renderPassInfo, nullptr, &framebuffer.renderPass) != VK_SUCCESS)
				return false;

			const VkFramebufferCreateInfo framebufferInfo{
				VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
				nullptr,
				0,
				framebuffer.renderPass,
				static_cast<uint32_t>(attachmentViews.size()),
				attachmentViews.data(),
				width,
				height,
				1
			};
			if (vkCreateFramebuffer(impl.device, &framebufferInfo, nullptr, &framebuffer.framebuffer) != VK_SUCCESS)
				return false;

			framebuffer.attachmentsDirty = false;
			return true;
		};

		auto CreateSampler = [](VkDevice device, const NLS::Render::RHI::SamplerDesc& desc) -> VkSampler
		{
			if (device == VK_NULL_HANDLE)
				return VK_NULL_HANDLE;

			const VkSamplerCreateInfo samplerInfo{
				VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
				nullptr,
				0,
				ToVkFilter(desc.minFilter),
				ToVkFilter(desc.magFilter),
				desc.minFilter == NLS::Render::RHI::TextureFilter::Nearest
					? VK_SAMPLER_MIPMAP_MODE_NEAREST
					: VK_SAMPLER_MIPMAP_MODE_LINEAR,
				ToVkSamplerAddressMode(desc.wrapU),
				ToVkSamplerAddressMode(desc.wrapV),
				ToVkSamplerAddressMode(desc.wrapW),
				0.0f,
				VK_FALSE,
				1.0f,
				VK_FALSE,
				VK_COMPARE_OP_ALWAYS,
				0.0f,
				VK_LOD_CLAMP_NONE,
				VK_BORDER_COLOR_INT_OPAQUE_BLACK,
				VK_FALSE
			};

			VkSampler sampler = VK_NULL_HANDLE;
			if (vkCreateSampler(device, &samplerInfo, nullptr, &sampler) != VK_SUCCESS)
				return VK_NULL_HANDLE;
			return sampler;
		};

		auto SelectSampler = [](auto& impl, const NLS::Render::Resources::ResourceBindingEntry* entry) -> VkSampler
		{
			if (entry != nullptr && entry->hasSampler)
			{
				const auto& desc = entry->sampler;
				const auto isPoint = desc.minFilter == NLS::Render::RHI::TextureFilter::Nearest ||
					desc.magFilter == NLS::Render::RHI::TextureFilter::Nearest;
				const auto isClamp =
					desc.wrapU == NLS::Render::RHI::TextureWrap::ClampToEdge ||
					desc.wrapV == NLS::Render::RHI::TextureWrap::ClampToEdge ||
					desc.wrapW == NLS::Render::RHI::TextureWrap::ClampToEdge;

				if (isPoint)
					return isClamp ? impl.pointClampSampler : impl.pointWrapSampler;
				return isClamp ? impl.linearClampSampler : impl.linearWrapSampler;
			}

			return impl.linearWrapSampler;
		};

		auto FindResolvedTexture = [](auto& impl, const NLS::Render::Resources::BindingSetInstance* bindingSet, const PipelineCacheEntry::DescriptorBinding& descriptorBinding)
			-> decltype(impl.textures.begin())
		{
			if (bindingSet != nullptr)
			{
				if (const auto* entry = bindingSet->Find(descriptorBinding.name); entry != nullptr && entry->textureResource != nullptr)
				{
					if (const auto boundTexture = impl.textures.find(entry->textureResource->GetResourceId()); boundTexture != impl.textures.end())
						return boundTexture;
				}
			}

			if (const auto fallbackIdIt = impl.fallbackTextureIds.find(descriptorBinding.defaultDimension); fallbackIdIt != impl.fallbackTextureIds.end())
				return impl.textures.find(fallbackIdIt->second);

			return impl.textures.end();
		};

		auto TransitionBindingSetTexturesToShaderRead = [](auto& impl, VkCommandBuffer commandBuffer, const PipelineCacheEntry& pipelineEntry)
		{
			if (commandBuffer == VK_NULL_HANDLE)
				return;

			std::set<uint32_t> transitionedTextureIds;
			for (const auto& descriptorBinding : pipelineEntry.descriptorBindings)
			{
				if (descriptorBinding.kind != NLS::Render::Resources::ShaderResourceKind::SampledTexture)
					continue;

				const auto textureIt = FindResolvedTexture(impl, impl.currentBindingSet, descriptorBinding);
				if (textureIt == impl.textures.end() || textureIt->second.image == VK_NULL_HANDLE)
					continue;

				const auto textureId = static_cast<uint32_t>(textureIt->first);
				if (!transitionedTextureIds.insert(textureId).second)
					continue;

				TransitionTexture(
					impl,
					commandBuffer,
					textureIt->second,
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					VK_ACCESS_SHADER_READ_BIT,
					VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
			}
		};

		auto BindRuntimeDescriptorSets = [](auto& impl, VkCommandBuffer commandBuffer, const PipelineCacheEntry& pipelineEntry)
		{
			if (impl.runtimeDescriptorPool == VK_NULL_HANDLE ||
				commandBuffer == VK_NULL_HANDLE ||
				pipelineEntry.pipelineLayout == VK_NULL_HANDLE ||
				pipelineEntry.descriptorSetLayouts.empty())
			{
				return;
			}

			std::vector<VkDescriptorSet> descriptorSets(pipelineEntry.descriptorSetLayouts.size(), VK_NULL_HANDLE);
			VkDescriptorSetAllocateInfo allocateInfo{};
			allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			allocateInfo.descriptorPool = impl.runtimeDescriptorPool;
			allocateInfo.descriptorSetCount = static_cast<uint32_t>(pipelineEntry.descriptorSetLayouts.size());
			allocateInfo.pSetLayouts = pipelineEntry.descriptorSetLayouts.data();
			if (vkAllocateDescriptorSets(impl.device, &allocateInfo, descriptorSets.data()) != VK_SUCCESS)
			{
				NLS_LOG_WARNING("Failed to allocate Vulkan runtime descriptor sets for graphics pipeline binding.");
				return;
			}

			std::vector<VkWriteDescriptorSet> writes;
			std::vector<VkDescriptorBufferInfo> bufferInfos;
			std::vector<VkDescriptorImageInfo> imageInfos;
			writes.reserve(pipelineEntry.descriptorBindings.size());
			bufferInfos.reserve(pipelineEntry.descriptorBindings.size());
			imageInfos.reserve(pipelineEntry.descriptorBindings.size());

			auto resolveUniformBufferId = [&impl](const NLS::Render::Resources::BindingSetInstance* bindingSet, const uint32_t bindingSpace, const uint32_t bindingIndex)
			{
				if (bindingSet != nullptr)
				{
					for (const auto& entry : bindingSet->Entries())
					{
						if (entry.kind != NLS::Render::Resources::ShaderResourceKind::UniformBuffer ||
							entry.bindingSpace != bindingSpace ||
							entry.bindingIndex != bindingIndex ||
							entry.bufferResource == nullptr)
						{
							continue;
						}

						return entry.bufferResource->GetResourceId();
					}
				}

				const auto bindingPoint = NLS::Render::RHI::BindingPointMap::GetUniformBufferBindingPoint(bindingSpace, bindingIndex);
				if (const auto bufferIdIt = impl.uniformBufferBindings.find(bindingPoint); bufferIdIt != impl.uniformBufferBindings.end())
					return bufferIdIt->second;

				return impl.fallbackUniformBufferId;
			};

			for (size_t setIndex = 0; setIndex < pipelineEntry.descriptorSetSpaces.size(); ++setIndex)
			{
				const auto bindingSpace = pipelineEntry.descriptorSetSpaces[setIndex];
				for (const auto& descriptorBinding : pipelineEntry.descriptorBindings)
				{
					if (descriptorBinding.bindingSpace != bindingSpace)
						continue;

					VkWriteDescriptorSet write{};
					write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
					write.dstSet = descriptorSets[setIndex];
					write.dstBinding = descriptorBinding.bindingIndex;
					write.dstArrayElement = 0;
					write.descriptorCount = descriptorBinding.descriptorCount;

					switch (descriptorBinding.kind)
					{
					case NLS::Render::Resources::ShaderResourceKind::UniformBuffer:
					{
						const auto bufferId = resolveUniformBufferId(
							impl.currentBindingSet,
							descriptorBinding.bindingSpace,
							descriptorBinding.bindingIndex);
						const auto bufferIt = impl.buffers.find(bufferId);
						if (bufferIt == impl.buffers.end() || bufferIt->second.buffer == VK_NULL_HANDLE)
							continue;

						bufferInfos.push_back({
							bufferIt->second.buffer,
							0,
							bufferIt->second.size
						});

						write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
						write.pBufferInfo = &bufferInfos.back();
						writes.push_back(write);
						break;
					}
					case NLS::Render::Resources::ShaderResourceKind::SampledTexture:
					{
						const auto textureIt = FindResolvedTexture(impl, impl.currentBindingSet, descriptorBinding);
						if (textureIt == impl.textures.end() || textureIt->second.view == VK_NULL_HANDLE)
							continue;

						imageInfos.push_back({
							VK_NULL_HANDLE,
							textureIt->second.view,
							VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
						});

						write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
						write.pImageInfo = &imageInfos.back();
						writes.push_back(write);
						break;
					}
					case NLS::Render::Resources::ShaderResourceKind::Sampler:
					{
						const auto* entry = impl.currentBindingSet != nullptr
							? impl.currentBindingSet->Find(descriptorBinding.name)
							: nullptr;
						const auto sampler = SelectSampler(impl, entry);
						if (sampler == VK_NULL_HANDLE)
							continue;

						imageInfos.push_back({
							sampler,
							VK_NULL_HANDLE,
							VK_IMAGE_LAYOUT_UNDEFINED
						});

						write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
						write.pImageInfo = &imageInfos.back();
						writes.push_back(write);
						break;
					}
					default:
						break;
					}
				}
			}

			if (!writes.empty())
				vkUpdateDescriptorSets(impl.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

			vkCmdBindDescriptorSets(
				commandBuffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				pipelineEntry.pipelineLayout,
				0,
				static_cast<uint32_t>(descriptorSets.size()),
				descriptorSets.data(),
				0,
				nullptr);
		};
	}
#endif

	VulkanRenderDevice::VulkanRenderDevice() : m_impl(std::make_unique<Impl>())
	{
	}

	VulkanRenderDevice::~VulkanRenderDevice()
	{
#if NLS_HAS_VULKAN
		const auto destroyBufferResource = [this](Impl::BufferResource& resource)
		{
			if (resource.buffer != VK_NULL_HANDLE)
				vkDestroyBuffer(m_impl->device, resource.buffer, nullptr);
			if (resource.memory != VK_NULL_HANDLE)
				vkFreeMemory(m_impl->device, resource.memory, nullptr);
		};

		for (auto& [_, resource] : m_impl->buffers)
			destroyBufferResource(resource);

		for (auto& [_, resource] : m_impl->textures)
		{
			if (resource.view != VK_NULL_HANDLE)
				vkDestroyImageView(m_impl->device, resource.view, nullptr);
			if (resource.image != VK_NULL_HANDLE)
				vkDestroyImage(m_impl->device, resource.image, nullptr);
			if (resource.memory != VK_NULL_HANDLE)
				vkFreeMemory(m_impl->device, resource.memory, nullptr);
		}

		for (auto& [_, framebuffer] : m_impl->framebuffers)
		{
			if (framebuffer.framebuffer != VK_NULL_HANDLE)
				vkDestroyFramebuffer(m_impl->device, framebuffer.framebuffer, nullptr);
			if (framebuffer.renderPass != VK_NULL_HANDLE)
				vkDestroyRenderPass(m_impl->device, framebuffer.renderPass, nullptr);
		}

		if (m_impl->device != VK_NULL_HANDLE)
		{
			vkDeviceWaitIdle(m_impl->device);
			for (auto& [_, pipeline] : m_impl->pipelineCache)
			{
				for (auto shaderModule : pipeline.shaderModules)
				{
					if (shaderModule != VK_NULL_HANDLE)
						vkDestroyShaderModule(m_impl->device, shaderModule, nullptr);
				}

				for (auto descriptorSetLayout : pipeline.descriptorSetLayouts)
				{
					if (descriptorSetLayout != VK_NULL_HANDLE)
						vkDestroyDescriptorSetLayout(m_impl->device, descriptorSetLayout, nullptr);
				}

				if (pipeline.pipelineLayout != VK_NULL_HANDLE)
					vkDestroyPipelineLayout(m_impl->device, pipeline.pipelineLayout, nullptr);
				if (pipeline.pipeline != VK_NULL_HANDLE)
					vkDestroyPipeline(m_impl->device, pipeline.pipeline, nullptr);
			}

			for (auto& frame : m_impl->frameContexts)
			{
				if (frame.framebuffer != VK_NULL_HANDLE)
					vkDestroyFramebuffer(m_impl->device, frame.framebuffer, nullptr);
				if (frame.view != VK_NULL_HANDLE)
					vkDestroyImageView(m_impl->device, frame.view, nullptr);
				if (frame.fence != VK_NULL_HANDLE)
					vkDestroyFence(m_impl->device, frame.fence, nullptr);
				if (frame.imageAvailableSemaphore != VK_NULL_HANDLE)
					vkDestroySemaphore(m_impl->device, frame.imageAvailableSemaphore, nullptr);
				if (frame.renderFinishedSemaphore != VK_NULL_HANDLE)
					vkDestroySemaphore(m_impl->device, frame.renderFinishedSemaphore, nullptr);
			}
			if (m_impl->swapchainRenderPass != VK_NULL_HANDLE)
				vkDestroyRenderPass(m_impl->device, m_impl->swapchainRenderPass, nullptr);
			if (m_impl->pointClampSampler != VK_NULL_HANDLE)
				vkDestroySampler(m_impl->device, m_impl->pointClampSampler, nullptr);
			if (m_impl->pointWrapSampler != VK_NULL_HANDLE)
				vkDestroySampler(m_impl->device, m_impl->pointWrapSampler, nullptr);
			if (m_impl->linearClampSampler != VK_NULL_HANDLE)
				vkDestroySampler(m_impl->device, m_impl->linearClampSampler, nullptr);
			if (m_impl->linearWrapSampler != VK_NULL_HANDLE)
				vkDestroySampler(m_impl->device, m_impl->linearWrapSampler, nullptr);
			if (m_impl->uiDescriptorPool != VK_NULL_HANDLE)
				vkDestroyDescriptorPool(m_impl->device, m_impl->uiDescriptorPool, nullptr);
			if (m_impl->runtimeDescriptorPool != VK_NULL_HANDLE)
				vkDestroyDescriptorPool(m_impl->device, m_impl->runtimeDescriptorPool, nullptr);
			if (m_impl->commandPool != VK_NULL_HANDLE)
				vkDestroyCommandPool(m_impl->device, m_impl->commandPool, nullptr);
			for (auto fence : m_impl->acquireFences)
			{
				if (fence != VK_NULL_HANDLE)
					vkDestroyFence(m_impl->device, fence, nullptr);
			}
			if (m_impl->swapchain != VK_NULL_HANDLE)
				vkDestroySwapchainKHR(m_impl->device, m_impl->swapchain, nullptr);
			if (m_impl->surface != VK_NULL_HANDLE)
				vkDestroySurfaceKHR(m_impl->instance, m_impl->surface, nullptr);
			vkDestroyDevice(m_impl->device, nullptr);
		}

		if (m_impl->instance != VK_NULL_HANDLE)
			vkDestroyInstance(m_impl->instance, nullptr);
#endif
	}

	std::optional<NLS::Render::Data::PipelineState> VulkanRenderDevice::Init(const NLS::Render::Settings::DriverSettings&)
	{
#if !NLS_HAS_VULKAN
		NLS_LOG_WARNING("Vulkan SDK not found at build time. Vulkan backend remains in stub mode.");
		return NLS::Render::Data::PipelineState{};
#else
		const VkApplicationInfo appInfo{
			VK_STRUCTURE_TYPE_APPLICATION_INFO,
			nullptr,
			"Nullus",
			VK_MAKE_VERSION(1, 0, 0),
			"Nullus",
			VK_MAKE_VERSION(1, 0, 0),
			VK_API_VERSION_1_0
		};

		uint32_t requiredExtensionCount = 0;
		const char** requiredExtensions = glfwGetRequiredInstanceExtensions(&requiredExtensionCount);
		std::vector<const char*> instanceExtensions;
		if (requiredExtensions != nullptr)
			instanceExtensions.assign(requiredExtensions, requiredExtensions + requiredExtensionCount);

		const VkInstanceCreateInfo instanceCreateInfo{
			VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
			nullptr,
			0,
			&appInfo,
			0,
			nullptr,
			static_cast<uint32_t>(instanceExtensions.size()),
			instanceExtensions.empty() ? nullptr : instanceExtensions.data()
		};

		if (vkCreateInstance(&instanceCreateInfo, nullptr, &m_impl->instance) != VK_SUCCESS)
		{
			NLS_LOG_ERROR("Failed to create Vulkan instance.");
			return std::nullopt;
		}

		uint32_t physicalDeviceCount = 0;
		if (vkEnumeratePhysicalDevices(m_impl->instance, &physicalDeviceCount, nullptr) != VK_SUCCESS || physicalDeviceCount == 0)
		{
			NLS_LOG_ERROR("Failed to enumerate Vulkan physical devices.");
			return std::nullopt;
		}

		std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
		vkEnumeratePhysicalDevices(m_impl->instance, &physicalDeviceCount, physicalDevices.data());

		for (const auto device : physicalDevices)
		{
			uint32_t queueFamilyCount = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
			std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
			vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

			for (uint32_t queueFamilyIndex = 0; queueFamilyIndex < queueFamilyCount; ++queueFamilyIndex)
			{
				if ((queueFamilies[queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
				{
					m_impl->physicalDevice = device;
					m_impl->graphicsQueueFamilyIndex = queueFamilyIndex;
					vkGetPhysicalDeviceProperties(device, &m_impl->properties);
					break;
				}
			}

			if (m_impl->physicalDevice != VK_NULL_HANDLE)
				break;
		}

		if (m_impl->physicalDevice == VK_NULL_HANDLE)
		{
			NLS_LOG_ERROR("Failed to find a Vulkan graphics queue family.");
			return std::nullopt;
		}

		const float queuePriority = 1.0f;
		const VkDeviceQueueCreateInfo queueCreateInfo{
			VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			nullptr,
			0,
			m_impl->graphicsQueueFamilyIndex,
			1,
			&queuePriority
		};

		constexpr const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

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

		if (vkCreateDevice(m_impl->physicalDevice, &deviceCreateInfo, nullptr, &m_impl->device) != VK_SUCCESS)
		{
			NLS_LOG_ERROR("Failed to create Vulkan logical device.");
			return std::nullopt;
		}

		vkGetDeviceQueue(m_impl->device, m_impl->graphicsQueueFamilyIndex, 0, &m_impl->graphicsQueue);

		const VkCommandPoolCreateInfo commandPoolInfo{
			VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			nullptr,
			VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
			m_impl->graphicsQueueFamilyIndex
		};

		if (vkCreateCommandPool(m_impl->device, &commandPoolInfo, nullptr, &m_impl->commandPool) != VK_SUCCESS)
		{
			NLS_LOG_ERROR("Failed to create Vulkan command pool.");
			return std::nullopt;
		}

		const std::array<VkDescriptorPoolSize, 3> runtimePoolSizes{ {
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1024 },
			{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024 },
			{ VK_DESCRIPTOR_TYPE_SAMPLER, 256 }
		} };
		const VkDescriptorPoolCreateInfo runtimeDescriptorPoolInfo{
			VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			nullptr,
			VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
			512,
			static_cast<uint32_t>(runtimePoolSizes.size()),
			runtimePoolSizes.data()
		};
		if (vkCreateDescriptorPool(m_impl->device, &runtimeDescriptorPoolInfo, nullptr, &m_impl->runtimeDescriptorPool) != VK_SUCCESS)
		{
			NLS_LOG_ERROR("Failed to create Vulkan runtime descriptor pool.");
			return std::nullopt;
		}

		m_impl->linearWrapSampler = CreateSampler(m_impl->device, NLS::Render::RHI::SamplerDesc{});
		m_impl->linearClampSampler = CreateSampler(m_impl->device, {
			NLS::Render::RHI::TextureFilter::Linear,
			NLS::Render::RHI::TextureFilter::Linear,
			NLS::Render::RHI::TextureWrap::ClampToEdge,
			NLS::Render::RHI::TextureWrap::ClampToEdge,
			NLS::Render::RHI::TextureWrap::ClampToEdge
		});
		m_impl->pointWrapSampler = CreateSampler(m_impl->device, {
			NLS::Render::RHI::TextureFilter::Nearest,
			NLS::Render::RHI::TextureFilter::Nearest,
			NLS::Render::RHI::TextureWrap::Repeat,
			NLS::Render::RHI::TextureWrap::Repeat,
			NLS::Render::RHI::TextureWrap::Repeat
		});
		m_impl->pointClampSampler = CreateSampler(m_impl->device, {
			NLS::Render::RHI::TextureFilter::Nearest,
			NLS::Render::RHI::TextureFilter::Nearest,
			NLS::Render::RHI::TextureWrap::ClampToEdge,
			NLS::Render::RHI::TextureWrap::ClampToEdge,
			NLS::Render::RHI::TextureWrap::ClampToEdge
		});
		m_impl->uiTextureSampler = m_impl->linearWrapSampler;

#if NLS_HAS_IMGUI_VULKAN_BACKEND
		const VkDescriptorPoolSize uiPoolSize{
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			256
		};
		const VkDescriptorPoolCreateInfo uiDescriptorPoolInfo{
			VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			nullptr,
			VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
			256,
			1,
			&uiPoolSize
		};
		if (vkCreateDescriptorPool(m_impl->device, &uiDescriptorPoolInfo, nullptr, &m_impl->uiDescriptorPool) != VK_SUCCESS)
		{
			NLS_LOG_WARNING("Failed to create Vulkan UI descriptor pool. Vulkan UI textures will remain unavailable.");
		}
#endif

		m_impl->hardware = m_impl->properties.deviceName;
		m_impl->vendor = "Vulkan";
		m_impl->version =
			std::to_string(VK_VERSION_MAJOR(m_impl->properties.apiVersion)) + "." +
			std::to_string(VK_VERSION_MINOR(m_impl->properties.apiVersion)) + "." +
			std::to_string(VK_VERSION_PATCH(m_impl->properties.apiVersion));
		m_impl->shadingLanguageVersion = "SPIR-V";
		m_impl->capabilities.backendReady = true;
		m_impl->capabilities.supportsGraphics = true;
		m_impl->capabilities.supportsCompute = true;
		m_impl->capabilities.supportsSwapchain = true;
		m_impl->capabilities.supportsFramebufferBlit = true;
		m_impl->capabilities.supportsDepthBlit = true;
		m_impl->capabilities.supportsCurrentSceneRenderer = true;
		m_impl->capabilities.supportsOffscreenFramebuffers = true;
		m_impl->capabilities.supportsFramebufferReadback = true;
		m_impl->capabilities.supportsUITextureHandles = true;
		m_impl->capabilities.supportsCubemaps = true;
		m_impl->capabilities.supportsMultiRenderTargets = m_impl->properties.limits.maxColorAttachments > 1;
		m_impl->capabilities.maxTextureDimension2D = m_impl->properties.limits.maxImageDimension2D;
		m_impl->capabilities.maxColorAttachments = m_impl->properties.limits.maxColorAttachments;
		m_impl->backendReady = true;

		{
			const std::array<uint8_t, 4> whitePixel{ 255, 255, 255, 255 };
			const std::array<uint8_t, 24> whiteCube{
				255, 255, 255, 255,
				255, 255, 255, 255,
				255, 255, 255, 255,
				255, 255, 255, 255,
				255, 255, 255, 255,
				255, 255, 255, 255
			};

			m_impl->fallbackUniformBufferId = CreateBuffer();
			BindBuffer(NLS::Render::RHI::BufferType::Uniform, m_impl->fallbackUniformBufferId);
			std::array<uint8_t, 256> zeroConstants{};
			SetBufferData(NLS::Render::RHI::BufferType::Uniform, zeroConstants.size(), zeroConstants.data(), NLS::Render::RHI::BufferUsage::DynamicDraw);

			const auto createFallbackTexture = [this](const NLS::Render::RHI::TextureDimension dimension, const void* data, const uint16_t width, const uint16_t height)
			{
				const auto textureId = CreateTexture();
				BindTexture(dimension, textureId);

				NLS::Render::RHI::TextureDesc desc{};
				desc.width = width;
				desc.height = height;
				desc.dimension = dimension;
				desc.format = NLS::Render::RHI::TextureFormat::RGBA8;
				desc.minFilter = NLS::Render::RHI::TextureFilter::Linear;
				desc.magFilter = NLS::Render::RHI::TextureFilter::Linear;
				desc.wrapS = NLS::Render::RHI::TextureWrap::Repeat;
				desc.wrapT = NLS::Render::RHI::TextureWrap::Repeat;
				desc.usage = NLS::Render::RHI::TextureUsage::Sampled;
				SetupTexture(desc, data);
				m_impl->fallbackTextureIds[dimension] = textureId;
			};

			createFallbackTexture(NLS::Render::RHI::TextureDimension::Texture2D, whitePixel.data(), 1, 1);
			createFallbackTexture(NLS::Render::RHI::TextureDimension::TextureCube, whiteCube.data(), 1, 1);
		}

		NLS_LOG_INFO("Initialized Vulkan backend on device: " + m_impl->hardware);
		return NLS::Render::Data::PipelineState{};
#endif
	}

	void VulkanRenderDevice::Clear(bool colorBuffer, bool, bool)
	{
#if NLS_HAS_VULKAN
		if (!colorBuffer || !EnsureFrameRecording(*m_impl))
			return;

		EndActiveRenderPass(*m_impl);

		auto& currentFrame = m_impl->frameContexts[m_impl->currentSwapchainImageIndex % m_impl->frameContexts.size()];
		if (currentFrame.commandBuffer == VK_NULL_HANDLE)
			return;

		const VkClearColorValue clearValue{ { m_impl->clearColor[0], m_impl->clearColor[1], m_impl->clearColor[2], m_impl->clearColor[3] } };
		if (m_impl->boundFramebuffer != 0)
		{
			if (!EnsureFramebufferReady(*m_impl, m_impl->boundFramebuffer))
				return;

			auto framebufferIt = m_impl->framebuffers.find(m_impl->boundFramebuffer);
			if (framebufferIt == m_impl->framebuffers.end())
				return;

			const auto colorCount = framebufferIt->second.drawBufferCount > 0
				? framebufferIt->second.drawBufferCount
				: static_cast<uint32_t>(framebufferIt->second.colorTextureIds.size());
			for (uint32_t i = 0; i < colorCount && i < framebufferIt->second.colorTextureIds.size(); ++i)
			{
				auto textureIt = m_impl->textures.find(framebufferIt->second.colorTextureIds[i]);
				if (textureIt == m_impl->textures.end())
					continue;

				TransitionTexture(*m_impl, currentFrame.commandBuffer, textureIt->second,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					VK_ACCESS_TRANSFER_WRITE_BIT,
					VK_PIPELINE_STAGE_TRANSFER_BIT);

				const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				vkCmdClearColorImage(currentFrame.commandBuffer, textureIt->second.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearValue, 1, &range);
			}

			if (framebufferIt->second.depthTextureId != 0)
			{
				auto depthIt = m_impl->textures.find(framebufferIt->second.depthTextureId);
				if (depthIt != m_impl->textures.end())
				{
					TransitionTexture(*m_impl, currentFrame.commandBuffer, depthIt->second,
						VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						static_cast<VkAccessFlags>(VK_ACCESS_TRANSFER_WRITE_BIT),
						VK_PIPELINE_STAGE_TRANSFER_BIT);

					const VkClearDepthStencilValue depthClear{ 1.0f, 0u };
					const VkImageSubresourceRange depthRange{
						static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT),
						0, 1, 0, 1
					};
					vkCmdClearDepthStencilImage(currentFrame.commandBuffer, depthIt->second.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &depthClear, 1, &depthRange);
				}
			}
		}
		else
		{
			auto& frame = m_impl->frameContexts[m_impl->currentSwapchainImageIndex % m_impl->frameContexts.size()];
			if (frame.image == VK_NULL_HANDLE)
				return;

			VkImageMemoryBarrier toTransfer{
				VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				nullptr,
				0,
				VK_ACCESS_TRANSFER_WRITE_BIT,
				frame.imageInitialized ? frame.layout : VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_QUEUE_FAMILY_IGNORED,
				VK_QUEUE_FAMILY_IGNORED,
				frame.image,
				{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
			};
			vkCmdPipelineBarrier(
				frame.commandBuffer,
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				0,
				0, nullptr,
				0, nullptr,
				1, &toTransfer);

			const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			vkCmdClearColorImage(frame.commandBuffer, frame.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearValue, 1, &range);
			frame.imageInitialized = true;
			frame.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			m_impl->swapchainHasColorContent = true;
		}

		m_impl->activeRenderTarget = std::numeric_limits<uint32_t>::max();
		m_impl->hasPendingCommands = true;
		m_impl->isFrameRecording = true;
#endif
	}
	void VulkanRenderDevice::ReadPixels(
		uint32_t x,
		uint32_t y,
		uint32_t width,
		uint32_t height,
		NLS::Render::Settings::EPixelDataFormat format,
		NLS::Render::Settings::EPixelDataType,
		void* data)
	{
#if NLS_HAS_VULKAN
		if (data == nullptr || width == 0 || height == 0 || m_impl->boundFramebuffer == 0)
			return;

		if (!EnsureFrameRecording(*m_impl))
			return;

		auto framebufferIt = m_impl->framebuffers.find(m_impl->boundFramebuffer);
		if (framebufferIt == m_impl->framebuffers.end() || framebufferIt->second.colorTextureIds.empty())
			return;

		auto textureIt = m_impl->textures.find(framebufferIt->second.colorTextureIds[0]);
		if (textureIt == m_impl->textures.end() || textureIt->second.image == VK_NULL_HANDLE)
			return;

		auto& frame = m_impl->frameContexts[m_impl->currentSwapchainImageIndex % m_impl->frameContexts.size()];
		if (frame.commandBuffer == VK_NULL_HANDLE)
			return;

		EndActiveRenderPass(*m_impl);

		auto& sourceTexture = textureIt->second;
		TransitionTexture(*m_impl, frame.commandBuffer, sourceTexture,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_ACCESS_TRANSFER_READ_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT);

		const auto bytesPerPixel = [sourceFormat = sourceTexture.desc.format]()
		{
			switch (sourceFormat)
			{
			case NLS::Render::RHI::TextureFormat::RGB8: return 3u;
			case NLS::Render::RHI::TextureFormat::RGBA16F: return 8u;
			case NLS::Render::RHI::TextureFormat::RGBA8:
			default:
				return 4u;
			}
		}();
		const VkDeviceSize readbackSize = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * bytesPerPixel;

		VkBuffer readbackBuffer = VK_NULL_HANDLE;
		VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
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
		if (vkCreateBuffer(m_impl->device, &bufferInfo, nullptr, &readbackBuffer) != VK_SUCCESS)
			return;

		VkMemoryRequirements memoryRequirements{};
		vkGetBufferMemoryRequirements(m_impl->device, readbackBuffer, &memoryRequirements);
		const VkMemoryAllocateInfo allocationInfo{
			VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			nullptr,
			memoryRequirements.size,
			FindMemoryTypeIndex(m_impl->physicalDevice, memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
		};
		if (vkAllocateMemory(m_impl->device, &allocationInfo, nullptr, &readbackMemory) != VK_SUCCESS)
		{
			vkDestroyBuffer(m_impl->device, readbackBuffer, nullptr);
			return;
		}

		vkBindBufferMemory(m_impl->device, readbackBuffer, readbackMemory, 0);

		const VkBufferImageCopy copyRegion{
			0,
			0,
			0,
			{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			{ static_cast<int32_t>(x), static_cast<int32_t>(y), 0 },
			{ width, height, 1 }
		};
		vkCmdCopyImageToBuffer(
			frame.commandBuffer,
			sourceTexture.image,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			readbackBuffer,
			1,
			&copyRegion);

		if (vkEndCommandBuffer(frame.commandBuffer) != VK_SUCCESS)
		{
			vkDestroyBuffer(m_impl->device, readbackBuffer, nullptr);
			vkFreeMemory(m_impl->device, readbackMemory, nullptr);
			m_impl->isFrameRecording = false;
			return;
		}
		m_impl->isFrameRecording = false;
		m_impl->hasPendingCommands = false;
		m_impl->activeRenderTarget = std::numeric_limits<uint32_t>::max();

		const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		const uint32_t waitSemaphoreCount = m_impl->pendingAcquireSemaphore != VK_NULL_HANDLE ? 1u : 0u;
		const VkSubmitInfo submitInfo{
			VK_STRUCTURE_TYPE_SUBMIT_INFO,
			nullptr,
			waitSemaphoreCount,
			waitSemaphoreCount == 0 ? nullptr : &m_impl->pendingAcquireSemaphore,
			waitSemaphoreCount == 0 ? nullptr : &waitStage,
			1,
			&frame.commandBuffer,
			0,
			nullptr
		};
		if (vkQueueSubmit(m_impl->graphicsQueue, 1, &submitInfo, frame.fence) != VK_SUCCESS)
		{
			vkDestroyBuffer(m_impl->device, readbackBuffer, nullptr);
			vkFreeMemory(m_impl->device, readbackMemory, nullptr);
			return;
		}
		m_impl->pendingAcquireSemaphore = VK_NULL_HANDLE;

		vkWaitForFences(m_impl->device, 1, &frame.fence, VK_TRUE, UINT64_MAX);

		void* mappedData = nullptr;
		if (vkMapMemory(m_impl->device, readbackMemory, 0, readbackSize, 0, &mappedData) == VK_SUCCESS)
		{
			if (format == NLS::Render::Settings::EPixelDataFormat::RGB && bytesPerPixel >= 3)
			{
				const auto* srcBytes = static_cast<const uint8_t*>(mappedData);
				auto* dstBytes = static_cast<uint8_t*>(data);
				for (uint32_t i = 0; i < width * height; ++i)
				{
					dstBytes[i * 3 + 0] = srcBytes[i * bytesPerPixel + 0];
					dstBytes[i * 3 + 1] = srcBytes[i * bytesPerPixel + 1];
					dstBytes[i * 3 + 2] = srcBytes[i * bytesPerPixel + 2];
				}
			}
			else
			{
				std::memcpy(data, mappedData, static_cast<size_t>(readbackSize));
			}
			vkUnmapMemory(m_impl->device, readbackMemory);
		}

		vkDestroyBuffer(m_impl->device, readbackBuffer, nullptr);
		vkFreeMemory(m_impl->device, readbackMemory, nullptr);
#else
		(void)x;
		(void)y;
		(void)width;
		(void)height;
		(void)format;
		(void)data;
#endif
	}
	void VulkanRenderDevice::DrawElements(NLS::Render::Settings::EPrimitiveMode, uint32_t indexCount)
	{
#if NLS_HAS_VULKAN
		const auto hasVertexSpirv = std::any_of(m_impl->currentPipelineDesc.shaderStages.begin(), m_impl->currentPipelineDesc.shaderStages.end(), [](const NLS::Render::RHI::ShaderStageDesc& stage)
		{
			return stage.stage == NLS::Render::RHI::ShaderStage::Vertex &&
				stage.targetPlatform == NLS::Render::ShaderCompiler::ShaderTargetPlatform::SPIRV &&
				!stage.bytecode.empty();
		});
		const auto hasPixelSpirv = std::any_of(m_impl->currentPipelineDesc.shaderStages.begin(), m_impl->currentPipelineDesc.shaderStages.end(), [](const NLS::Render::RHI::ShaderStageDesc& stage)
		{
			return stage.stage == NLS::Render::RHI::ShaderStage::Fragment &&
				stage.targetPlatform == NLS::Render::ShaderCompiler::ShaderTargetPlatform::SPIRV &&
				!stage.bytecode.empty();
		});
		if (!hasVertexSpirv || !hasPixelSpirv)
		{
			if (!m_impl->hasLoggedDrawStub)
			{
				NLS_LOG_WARNING("Vulkan draw skipped because the current material does not provide complete SPIR-V shader stages yet.");
				m_impl->hasLoggedDrawStub = true;
			}
			return;
		}

		if (m_impl->boundBuffers.find(NLS::Render::RHI::BufferType::Vertex) == m_impl->boundBuffers.end() ||
			m_impl->boundBuffers.find(NLS::Render::RHI::BufferType::Index) == m_impl->boundBuffers.end())
		{
			if (!m_impl->hasLoggedDrawStub)
			{
				NLS_LOG_WARNING("Vulkan draw skipped because vertex/index buffers are not both bound yet.");
				m_impl->hasLoggedDrawStub = true;
			}
			return;
		}

		if (!EnsureFrameRecording(*m_impl))
			return;

		VkRenderPass targetRenderPass = m_impl->swapchainRenderPass;
		VkFramebuffer targetFramebuffer = VK_NULL_HANDLE;
		VkExtent2D targetExtent = m_impl->swapchainExtent;
		if (m_impl->boundFramebuffer != 0)
		{
			if (!EnsureFramebufferReady(*m_impl, m_impl->boundFramebuffer))
				return;

			auto framebufferIt = m_impl->framebuffers.find(m_impl->boundFramebuffer);
			if (framebufferIt == m_impl->framebuffers.end())
				return;

			targetRenderPass = framebufferIt->second.renderPass;
			targetFramebuffer = framebufferIt->second.framebuffer;
			if (!framebufferIt->second.colorTextureIds.empty())
			{
				if (const auto textureIt = m_impl->textures.find(framebufferIt->second.colorTextureIds[0]); textureIt != m_impl->textures.end())
				{
					targetExtent.width = textureIt->second.desc.width;
					targetExtent.height = textureIt->second.desc.height;
				}
			}
		}
		else
		{
			auto& frame = m_impl->frameContexts[m_impl->currentSwapchainImageIndex % m_impl->frameContexts.size()];
			targetFramebuffer = frame.framebuffer;
		}

		const auto pipelineKey = BuildPipelineCacheKey(m_impl->currentPipelineDesc) + "|rp=" + std::to_string(static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(targetRenderPass)));
		auto pipelineIt = m_impl->pipelineCache.find(pipelineKey);
		if (pipelineIt == m_impl->pipelineCache.end() || pipelineIt->second.pipeline == VK_NULL_HANDLE)
			return;

		auto& frame = m_impl->frameContexts[m_impl->currentSwapchainImageIndex % m_impl->frameContexts.size()];
		if (frame.commandBuffer == VK_NULL_HANDLE || targetFramebuffer == VK_NULL_HANDLE)
			return;

		if (frame.renderPassActive && m_impl->activeRenderTarget != m_impl->boundFramebuffer)
			EndActiveRenderPass(*m_impl);

		if (!frame.renderPassActive)
			TransitionBindingSetTexturesToShaderRead(*m_impl, frame.commandBuffer, pipelineIt->second);

		if (!frame.renderPassActive)
		{
			if (m_impl->boundFramebuffer != 0)
			{
				auto framebufferIt = m_impl->framebuffers.find(m_impl->boundFramebuffer);
				if (framebufferIt == m_impl->framebuffers.end())
					return;

				const auto colorCount = framebufferIt->second.drawBufferCount > 0
					? framebufferIt->second.drawBufferCount
					: static_cast<uint32_t>(framebufferIt->second.colorTextureIds.size());
				for (uint32_t i = 0; i < colorCount && i < framebufferIt->second.colorTextureIds.size(); ++i)
				{
					if (auto textureIt = m_impl->textures.find(framebufferIt->second.colorTextureIds[i]); textureIt != m_impl->textures.end())
					{
						TransitionTexture(*m_impl, frame.commandBuffer, textureIt->second,
							VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
							VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
							VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
					}
				}

				if (framebufferIt->second.depthTextureId != 0)
				{
					if (auto depthIt = m_impl->textures.find(framebufferIt->second.depthTextureId); depthIt != m_impl->textures.end())
					{
						TransitionTexture(*m_impl, frame.commandBuffer, depthIt->second,
							VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
							VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
							VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
					}
				}
			}
			else
			{
				VkImageMemoryBarrier toColorAttachment{
					VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					nullptr,
					0,
					VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
					frame.imageInitialized ? frame.layout : VK_IMAGE_LAYOUT_UNDEFINED,
					VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					VK_QUEUE_FAMILY_IGNORED,
					VK_QUEUE_FAMILY_IGNORED,
					frame.image,
					{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
				};
				vkCmdPipelineBarrier(
					frame.commandBuffer,
					frame.imageInitialized ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
					VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
					0,
					0, nullptr,
					0, nullptr,
					1, &toColorAttachment);
				frame.imageInitialized = true;
				frame.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			}

			const VkRenderPassBeginInfo renderPassBeginInfo{
				VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
				nullptr,
				targetRenderPass,
				targetFramebuffer,
				{ { 0, 0 }, targetExtent },
				0,
				nullptr
			};
			vkCmdBeginRenderPass(frame.commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
			frame.renderPassActive = true;
			m_impl->activeRenderTarget = m_impl->boundFramebuffer;
			if (m_impl->boundFramebuffer == 0)
				m_impl->swapchainHasColorContent = true;
		}

		const auto vertexBufferId = m_impl->boundBuffers[NLS::Render::RHI::BufferType::Vertex];
		const auto indexBufferId = m_impl->boundBuffers[NLS::Render::RHI::BufferType::Index];
		const auto vertexBufferIt = m_impl->buffers.find(vertexBufferId);
		const auto indexBufferIt = m_impl->buffers.find(indexBufferId);
		if (vertexBufferIt == m_impl->buffers.end() || indexBufferIt == m_impl->buffers.end())
			return;

		const VkViewport viewport{
			0.0f,
			0.0f,
			static_cast<float>(targetExtent.width),
			static_cast<float>(targetExtent.height),
			0.0f,
			1.0f
		};
		const VkRect2D scissor{ { 0, 0 }, targetExtent };
		const VkBuffer vertexBuffer = vertexBufferIt->second.buffer;
		const VkDeviceSize vertexOffset = 0;
		vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineIt->second.pipeline);
		BindRuntimeDescriptorSets(*m_impl, frame.commandBuffer, pipelineIt->second);
		vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
		vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
		vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &vertexBuffer, &vertexOffset);
		vkCmdBindIndexBuffer(frame.commandBuffer, indexBufferIt->second.buffer, 0, VK_INDEX_TYPE_UINT32);
		vkCmdDrawIndexed(frame.commandBuffer, indexCount, 1, 0, 0, 0);
		m_impl->hasPendingCommands = true;
#endif
	}
	void VulkanRenderDevice::DrawElementsInstanced(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t indexCount, uint32_t)
	{
		DrawElements(primitiveMode, indexCount);
	}
	void VulkanRenderDevice::DrawArrays(NLS::Render::Settings::EPrimitiveMode, uint32_t vertexCount)
	{
#if NLS_HAS_VULKAN
		if (m_impl->boundBuffers.find(NLS::Render::RHI::BufferType::Vertex) == m_impl->boundBuffers.end())
		{
			if (!m_impl->hasLoggedDrawStub)
			{
				NLS_LOG_WARNING("Vulkan draw skipped because no vertex buffer is bound yet.");
				m_impl->hasLoggedDrawStub = true;
			}
			return;
		}

		if (!EnsureFrameRecording(*m_impl))
			return;

		VkRenderPass targetRenderPass = m_impl->swapchainRenderPass;
		VkFramebuffer targetFramebuffer = VK_NULL_HANDLE;
		VkExtent2D targetExtent = m_impl->swapchainExtent;
		if (m_impl->boundFramebuffer != 0)
		{
			if (!EnsureFramebufferReady(*m_impl, m_impl->boundFramebuffer))
				return;

			auto framebufferIt = m_impl->framebuffers.find(m_impl->boundFramebuffer);
			if (framebufferIt == m_impl->framebuffers.end())
				return;

			targetRenderPass = framebufferIt->second.renderPass;
			targetFramebuffer = framebufferIt->second.framebuffer;
			if (!framebufferIt->second.colorTextureIds.empty())
			{
				if (const auto textureIt = m_impl->textures.find(framebufferIt->second.colorTextureIds[0]); textureIt != m_impl->textures.end())
				{
					targetExtent.width = textureIt->second.desc.width;
					targetExtent.height = textureIt->second.desc.height;
				}
			}
		}
		else
		{
			auto& frame = m_impl->frameContexts[m_impl->currentSwapchainImageIndex % m_impl->frameContexts.size()];
			targetFramebuffer = frame.framebuffer;
		}

		const auto pipelineKey = BuildPipelineCacheKey(m_impl->currentPipelineDesc) + "|rp=" + std::to_string(static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(targetRenderPass)));
		auto pipelineIt = m_impl->pipelineCache.find(pipelineKey);
		if (pipelineIt == m_impl->pipelineCache.end() || pipelineIt->second.pipeline == VK_NULL_HANDLE)
			return;

		auto& frame = m_impl->frameContexts[m_impl->currentSwapchainImageIndex % m_impl->frameContexts.size()];
		if (frame.commandBuffer == VK_NULL_HANDLE || targetFramebuffer == VK_NULL_HANDLE)
			return;

		if (frame.renderPassActive && m_impl->activeRenderTarget != m_impl->boundFramebuffer)
			EndActiveRenderPass(*m_impl);

		if (!frame.renderPassActive)
			TransitionBindingSetTexturesToShaderRead(*m_impl, frame.commandBuffer, pipelineIt->second);

		if (!frame.renderPassActive)
		{
			if (m_impl->boundFramebuffer != 0)
			{
				auto framebufferIt = m_impl->framebuffers.find(m_impl->boundFramebuffer);
				if (framebufferIt == m_impl->framebuffers.end())
					return;

				const auto colorCount = framebufferIt->second.drawBufferCount > 0
					? framebufferIt->second.drawBufferCount
					: static_cast<uint32_t>(framebufferIt->second.colorTextureIds.size());
				for (uint32_t i = 0; i < colorCount && i < framebufferIt->second.colorTextureIds.size(); ++i)
				{
					if (auto textureIt = m_impl->textures.find(framebufferIt->second.colorTextureIds[i]); textureIt != m_impl->textures.end())
					{
						TransitionTexture(*m_impl, frame.commandBuffer, textureIt->second,
							VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
							VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
							VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
					}
				}

				if (framebufferIt->second.depthTextureId != 0)
				{
					if (auto depthIt = m_impl->textures.find(framebufferIt->second.depthTextureId); depthIt != m_impl->textures.end())
					{
						TransitionTexture(*m_impl, frame.commandBuffer, depthIt->second,
							VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
							VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
							VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
					}
				}
			}
			else
			{
				VkImageMemoryBarrier toColorAttachment{
					VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					nullptr,
					0,
					VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
					frame.imageInitialized ? frame.layout : VK_IMAGE_LAYOUT_UNDEFINED,
					VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					VK_QUEUE_FAMILY_IGNORED,
					VK_QUEUE_FAMILY_IGNORED,
					frame.image,
					{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
				};
				vkCmdPipelineBarrier(
					frame.commandBuffer,
					frame.imageInitialized ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
					VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
					0,
					0, nullptr,
					0, nullptr,
					1, &toColorAttachment);
				frame.imageInitialized = true;
				frame.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			}

			const VkRenderPassBeginInfo renderPassBeginInfo{
				VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
				nullptr,
				targetRenderPass,
				targetFramebuffer,
				{ { 0, 0 }, targetExtent },
				0,
				nullptr
			};
			vkCmdBeginRenderPass(frame.commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
			frame.renderPassActive = true;
			m_impl->activeRenderTarget = m_impl->boundFramebuffer;
			if (m_impl->boundFramebuffer == 0)
				m_impl->swapchainHasColorContent = true;
		}

		const auto vertexBufferId = m_impl->boundBuffers[NLS::Render::RHI::BufferType::Vertex];
		const auto vertexBufferIt = m_impl->buffers.find(vertexBufferId);
		if (vertexBufferIt == m_impl->buffers.end())
			return;

		const VkViewport viewport{
			0.0f,
			0.0f,
			static_cast<float>(targetExtent.width),
			static_cast<float>(targetExtent.height),
			0.0f,
			1.0f
		};
		const VkRect2D scissor{ { 0, 0 }, targetExtent };
		const VkBuffer vertexBuffer = vertexBufferIt->second.buffer;
		const VkDeviceSize vertexOffset = 0;
		vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineIt->second.pipeline);
		BindRuntimeDescriptorSets(*m_impl, frame.commandBuffer, pipelineIt->second);
		vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
		vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
		vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &vertexBuffer, &vertexOffset);
		vkCmdDraw(frame.commandBuffer, vertexCount, 1, 0, 0);
		m_impl->hasPendingCommands = true;
#endif
	}
	void VulkanRenderDevice::DrawArraysInstanced(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t vertexCount, uint32_t)
	{
		DrawArrays(primitiveMode, vertexCount);
	}
	void VulkanRenderDevice::SetClearColor(float red, float green, float blue, float alpha)
	{
#if NLS_HAS_VULKAN
		m_impl->clearColor[0] = red;
		m_impl->clearColor[1] = green;
		m_impl->clearColor[2] = blue;
		m_impl->clearColor[3] = alpha;
#endif
	}
	void VulkanRenderDevice::SetRasterizationLinesWidth(float) {}
	void VulkanRenderDevice::SetRasterizationMode(NLS::Render::Settings::ERasterizationMode) {}
	void VulkanRenderDevice::SetCapability(NLS::Render::Settings::ERenderingCapability, bool) {}
	bool VulkanRenderDevice::GetCapability(NLS::Render::Settings::ERenderingCapability) { return false; }
	void VulkanRenderDevice::SetStencilAlgorithm(NLS::Render::Settings::EComparaisonAlgorithm, int32_t, uint32_t) {}
	void VulkanRenderDevice::SetDepthAlgorithm(NLS::Render::Settings::EComparaisonAlgorithm) {}
	void VulkanRenderDevice::SetStencilMask(uint32_t) {}
	void VulkanRenderDevice::SetStencilOperations(NLS::Render::Settings::EOperation, NLS::Render::Settings::EOperation, NLS::Render::Settings::EOperation) {}
	void VulkanRenderDevice::SetCullFace(NLS::Render::Settings::ECullFace) {}
	void VulkanRenderDevice::SetDepthWriting(bool) {}
	void VulkanRenderDevice::SetColorWriting(bool, bool, bool, bool) {}
	void VulkanRenderDevice::SetViewport(uint32_t, uint32_t, uint32_t, uint32_t) {}
	void VulkanRenderDevice::BindGraphicsPipeline(const NLS::Render::RHI::GraphicsPipelineDesc& pipelineDesc, const NLS::Render::Resources::BindingSetInstance* bindingSet)
	{
#if NLS_HAS_VULKAN
		auto resolvedDesc = pipelineDesc;
		resolvedDesc.attachmentLayout.colorAttachmentFormats.clear();
		resolvedDesc.attachmentLayout.sampleCount = 1;
		if (m_impl->boundFramebuffer != 0)
		{
			if (const auto framebufferIt = m_impl->framebuffers.find(m_impl->boundFramebuffer); framebufferIt != m_impl->framebuffers.end())
			{
				for (const auto colorTextureId : framebufferIt->second.colorTextureIds)
				{
					if (const auto textureIt = m_impl->textures.find(colorTextureId); textureIt != m_impl->textures.end())
						resolvedDesc.attachmentLayout.colorAttachmentFormats.push_back(textureIt->second.desc.format);
				}

				resolvedDesc.attachmentLayout.hasDepthAttachment = framebufferIt->second.depthTextureId != 0;
				if (resolvedDesc.attachmentLayout.hasDepthAttachment)
				{
					if (const auto depthTextureIt = m_impl->textures.find(framebufferIt->second.depthTextureId); depthTextureIt != m_impl->textures.end())
						resolvedDesc.attachmentLayout.depthAttachmentFormat = depthTextureIt->second.desc.format;
				}
			}
		}

		if (resolvedDesc.attachmentLayout.colorAttachmentFormats.empty())
			resolvedDesc.attachmentLayout.colorAttachmentFormats = { NLS::Render::RHI::TextureFormat::RGBA8 };

		m_impl->currentPipelineDesc = resolvedDesc;
		m_impl->currentBindingSet = bindingSet;
		m_impl->hasLoggedDrawStub = false;

		if (!m_impl->device || resolvedDesc.reflection == nullptr)
			return;

		VkRenderPass targetRenderPass = m_impl->swapchainRenderPass;
		uint32_t colorAttachmentCount = resolvedDesc.attachmentLayout.colorAttachmentFormats.empty()
			? 1u
			: static_cast<uint32_t>(resolvedDesc.attachmentLayout.colorAttachmentFormats.size());
		if (m_impl->boundFramebuffer != 0)
		{
			if (!EnsureFramebufferReady(*m_impl, m_impl->boundFramebuffer))
				return;

			auto framebufferIt = m_impl->framebuffers.find(m_impl->boundFramebuffer);
			if (framebufferIt == m_impl->framebuffers.end() || framebufferIt->second.renderPass == VK_NULL_HANDLE)
				return;

			targetRenderPass = framebufferIt->second.renderPass;
			colorAttachmentCount = framebufferIt->second.drawBufferCount > 0
				? framebufferIt->second.drawBufferCount
				: static_cast<uint32_t>(framebufferIt->second.colorTextureIds.size());
		}

		const auto pipelineKey = BuildPipelineCacheKey(resolvedDesc) + "|rp=" + std::to_string(static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(targetRenderPass)));
		if (m_impl->pipelineCache.find(pipelineKey) != m_impl->pipelineCache.end())
			return;

		PipelineCacheEntry pipelineEntry{};
		std::unordered_map<uint32_t, std::vector<VkDescriptorSetLayoutBinding>> descriptorBindingsBySpace;

		for (const auto& constantBuffer : resolvedDesc.reflection->constantBuffers)
		{
			VkDescriptorSetLayoutBinding binding{};
			binding.binding = constantBuffer.bindingIndex;
			binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			binding.descriptorCount = 1;
			binding.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
			descriptorBindingsBySpace[constantBuffer.bindingSpace].push_back(binding);

			pipelineEntry.descriptorBindings.push_back({
				constantBuffer.name,
				NLS::Render::Resources::ShaderResourceKind::UniformBuffer,
				NLS::Render::Resources::UniformType::UNIFORM_FLOAT,
				constantBuffer.bindingSpace,
				constantBuffer.bindingIndex,
				1u,
				NLS::Render::RHI::TextureDimension::Texture2D
			});
		}

		for (const auto& property : resolvedDesc.reflection->properties)
		{
			VkDescriptorType descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
			bool hasDescriptorType = true;
			if (property.kind == NLS::Render::Resources::ShaderResourceKind::SampledTexture)
				descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			else if (property.kind == NLS::Render::Resources::ShaderResourceKind::Sampler)
				descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
			else
				hasDescriptorType = false;

			if (!hasDescriptorType)
				continue;

			VkDescriptorSetLayoutBinding binding{};
			binding.binding = property.bindingIndex;
			binding.descriptorType = descriptorType;
			binding.descriptorCount = static_cast<uint32_t>((std::max)(1, property.arraySize));
			binding.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
			descriptorBindingsBySpace[property.bindingSpace].push_back(binding);

			pipelineEntry.descriptorBindings.push_back({
				property.name,
				property.kind,
				property.type,
				property.bindingSpace,
				property.bindingIndex,
				static_cast<uint32_t>((std::max)(1, property.arraySize)),
				property.type == NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_CUBE
					? NLS::Render::RHI::TextureDimension::TextureCube
					: NLS::Render::RHI::TextureDimension::Texture2D
			});
		}

		std::vector<uint32_t> sortedSpaces;
		sortedSpaces.reserve(descriptorBindingsBySpace.size());
		for (const auto& [space, _] : descriptorBindingsBySpace)
			sortedSpaces.push_back(space);
		std::sort(sortedSpaces.begin(), sortedSpaces.end());

		for (const auto space : sortedSpaces)
		{
			auto& bindings = descriptorBindingsBySpace[space];
			std::sort(bindings.begin(), bindings.end(), [](const auto& lhs, const auto& rhs)
			{
				if (lhs.binding == rhs.binding)
					return lhs.descriptorType < rhs.descriptorType;
				return lhs.binding < rhs.binding;
			});

			bindings.erase(std::unique(bindings.begin(), bindings.end(), [](const auto& lhs, const auto& rhs)
			{
				return lhs.binding == rhs.binding && lhs.descriptorType == rhs.descriptorType;
			}), bindings.end());

			VkDescriptorSetLayoutCreateInfo layoutInfo{};
			layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
			layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
			layoutInfo.pBindings = bindings.empty() ? nullptr : bindings.data();

			VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
			if (vkCreateDescriptorSetLayout(m_impl->device, &layoutInfo, nullptr, &descriptorSetLayout) != VK_SUCCESS)
			{
				NLS_LOG_ERROR("Failed to create Vulkan descriptor set layout from shader reflection.");
				return;
			}

			pipelineEntry.descriptorSetLayouts.push_back(descriptorSetLayout);
			pipelineEntry.descriptorSetSpaces.push_back(space);
		}

		VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
		pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(pipelineEntry.descriptorSetLayouts.size());
		pipelineLayoutInfo.pSetLayouts = pipelineEntry.descriptorSetLayouts.empty() ? nullptr : pipelineEntry.descriptorSetLayouts.data();

		if (vkCreatePipelineLayout(m_impl->device, &pipelineLayoutInfo, nullptr, &pipelineEntry.pipelineLayout) != VK_SUCCESS)
		{
			NLS_LOG_ERROR("Failed to create Vulkan pipeline layout from shader reflection.");
			for (auto descriptorSetLayout : pipelineEntry.descriptorSetLayouts)
				vkDestroyDescriptorSetLayout(m_impl->device, descriptorSetLayout, nullptr);
			return;
		}

		for (const auto& stage : resolvedDesc.shaderStages)
		{
			if (stage.targetPlatform != NLS::Render::ShaderCompiler::ShaderTargetPlatform::SPIRV || stage.bytecode.empty())
				continue;

			VkShaderModuleCreateInfo moduleInfo{};
			moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			moduleInfo.codeSize = stage.bytecode.size();
			moduleInfo.pCode = reinterpret_cast<const uint32_t*>(stage.bytecode.data());

			VkShaderModule shaderModule = VK_NULL_HANDLE;
			if (vkCreateShaderModule(m_impl->device, &moduleInfo, nullptr, &shaderModule) != VK_SUCCESS)
			{
				NLS_LOG_ERROR("Failed to create Vulkan shader module from compiled SPIR-V.");
				for (auto module : pipelineEntry.shaderModules)
					vkDestroyShaderModule(m_impl->device, module, nullptr);
				for (auto descriptorSetLayout : pipelineEntry.descriptorSetLayouts)
					vkDestroyDescriptorSetLayout(m_impl->device, descriptorSetLayout, nullptr);
				if (pipelineEntry.pipelineLayout != VK_NULL_HANDLE)
					vkDestroyPipelineLayout(m_impl->device, pipelineEntry.pipelineLayout, nullptr);
				return;
			}

			pipelineEntry.shaderModules.push_back(shaderModule);
		}

		std::vector<VkPipelineShaderStageCreateInfo> shaderStageInfos;
		shaderStageInfos.reserve(resolvedDesc.shaderStages.size());
		size_t shaderModuleIndex = 0;
		for (const auto& stage : resolvedDesc.shaderStages)
		{
			if (stage.targetPlatform != NLS::Render::ShaderCompiler::ShaderTargetPlatform::SPIRV || stage.bytecode.empty())
				continue;

			VkPipelineShaderStageCreateInfo shaderStageInfo{};
			shaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			shaderStageInfo.stage = ToVkShaderStage(stage.stage);
			shaderStageInfo.module = pipelineEntry.shaderModules[shaderModuleIndex++];
			shaderStageInfo.pName = stage.entryPoint.empty() ? "main" : stage.entryPoint.c_str();
			shaderStageInfos.push_back(shaderStageInfo);
		}

		const VkVertexInputBindingDescription vertexBindingDescription{
			0,
			sizeof(float) * 14u,
			VK_VERTEX_INPUT_RATE_VERTEX
		};
		const std::array<VkVertexInputAttributeDescription, 5> vertexAttributes{ {
			{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
			{ 1, 0, VK_FORMAT_R32G32_SFLOAT, 12 },
			{ 2, 0, VK_FORMAT_R32G32B32_SFLOAT, 20 },
			{ 3, 0, VK_FORMAT_R32G32B32_SFLOAT, 32 },
			{ 4, 0, VK_FORMAT_R32G32B32_SFLOAT, 44 }
		} };
		const VkPipelineVertexInputStateCreateInfo vertexInputInfo{
			VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			nullptr,
			0,
			1,
			&vertexBindingDescription,
			static_cast<uint32_t>(vertexAttributes.size()),
			vertexAttributes.data()
		};
		const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
			VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			nullptr,
			0,
			ToVkPrimitiveTopology(resolvedDesc.primitiveMode),
			VK_FALSE
		};
		const VkPipelineViewportStateCreateInfo viewportState{
			VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			nullptr,
			0,
			1,
			nullptr,
			1,
			nullptr
		};
		const VkPipelineRasterizationStateCreateInfo rasterizationState{
			VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			nullptr,
			0,
			VK_FALSE,
			VK_FALSE,
			VK_POLYGON_MODE_FILL,
			resolvedDesc.rasterState.culling ? ToVkCullMode(resolvedDesc.rasterState.cullFace) : VK_CULL_MODE_NONE,
			VK_FRONT_FACE_COUNTER_CLOCKWISE,
			VK_FALSE,
			0.0f,
			0.0f,
			0.0f,
			1.0f
		};
		const VkPipelineMultisampleStateCreateInfo multisampleState{
			VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			nullptr,
			0,
			VK_SAMPLE_COUNT_1_BIT,
			VK_FALSE,
			1.0f,
			nullptr,
			VK_FALSE,
			VK_FALSE
		};
		const VkPipelineColorBlendAttachmentState colorBlendAttachment{
			resolvedDesc.blendState.enabled ? VK_TRUE : VK_FALSE,
			resolvedDesc.blendState.enabled ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_ONE,
			resolvedDesc.blendState.enabled ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ZERO,
			VK_BLEND_OP_ADD,
			VK_BLEND_FACTOR_ONE,
			resolvedDesc.blendState.enabled ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ZERO,
			VK_BLEND_OP_ADD,
			resolvedDesc.blendState.colorWrite ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT) : 0u
		};
		std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments(colorAttachmentCount, colorBlendAttachment);
		const VkPipelineColorBlendStateCreateInfo colorBlendState{
			VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			nullptr,
			0,
			VK_FALSE,
			VK_LOGIC_OP_COPY,
			static_cast<uint32_t>(colorBlendAttachments.size()),
			colorBlendAttachments.data(),
			{ 0.0f, 0.0f, 0.0f, 0.0f }
		};
		const std::array<VkDynamicState, 2> dynamicStates{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		const VkPipelineDynamicStateCreateInfo dynamicStateInfo{
			VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
			nullptr,
			0,
			static_cast<uint32_t>(dynamicStates.size()),
			dynamicStates.data()
		};
		const VkPipelineDepthStencilStateCreateInfo depthStencilState{
			VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
			nullptr,
			0,
			resolvedDesc.depthStencilState.depthTest ? VK_TRUE : VK_FALSE,
			resolvedDesc.depthStencilState.depthWrite ? VK_TRUE : VK_FALSE,
			ToVkCompareOp(resolvedDesc.depthStencilState.depthCompare),
			VK_FALSE,
			VK_FALSE,
			{},
			{},
			0.0f,
			1.0f
		};
		const VkGraphicsPipelineCreateInfo graphicsPipelineInfo{
			VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			nullptr,
			0,
			static_cast<uint32_t>(shaderStageInfos.size()),
			shaderStageInfos.data(),
			&vertexInputInfo,
			&inputAssembly,
			nullptr,
			&viewportState,
			&rasterizationState,
			&multisampleState,
			&depthStencilState,
			&colorBlendState,
			&dynamicStateInfo,
			pipelineEntry.pipelineLayout,
			targetRenderPass,
			0,
			VK_NULL_HANDLE,
			-1
		};
		if (vkCreateGraphicsPipelines(m_impl->device, VK_NULL_HANDLE, 1, &graphicsPipelineInfo, nullptr, &pipelineEntry.pipeline) != VK_SUCCESS)
		{
			NLS_LOG_ERROR("Failed to create Vulkan graphics pipeline.");
			for (auto module : pipelineEntry.shaderModules)
				vkDestroyShaderModule(m_impl->device, module, nullptr);
			for (auto descriptorSetLayout : pipelineEntry.descriptorSetLayouts)
				vkDestroyDescriptorSetLayout(m_impl->device, descriptorSetLayout, nullptr);
			if (pipelineEntry.pipelineLayout != VK_NULL_HANDLE)
				vkDestroyPipelineLayout(m_impl->device, pipelineEntry.pipelineLayout, nullptr);
			return;
		}

		m_impl->pipelineCache.emplace(pipelineKey, std::move(pipelineEntry));
#else
		(void)pipelineDesc;
		(void)bindingSet;
#endif
	}
	std::shared_ptr<NLS::Render::RHI::IRHITexture> VulkanRenderDevice::CreateTextureResource(NLS::Render::RHI::TextureDimension dimension)
	{
		NLS::Render::RHI::TextureDesc desc{};
		desc.dimension = dimension;
		auto resource = std::make_shared<VulkanTextureResource>(CreateTexture(), desc, [this](uint32_t id) { DestroyTexture(id); });
#if NLS_HAS_VULKAN
		if (resource)
			m_impl->textureObjects[resource->GetResourceId()] = resource;
#endif
		return resource;
	}
	uint32_t VulkanRenderDevice::CreateTexture()
	{
#if NLS_HAS_VULKAN
		const auto id = m_impl->nextResourceId++;
		m_impl->textures.emplace(id, Impl::TextureResource{});
		return id;
#else
		return 0;
#endif
	}
	void VulkanRenderDevice::DestroyTexture(uint32_t textureId)
	{
#if NLS_HAS_VULKAN
		m_impl->textureObjects.erase(textureId);
		m_impl->uiTexturesUsedThisFrame.erase(textureId);
		if (const auto uiHandleIt = m_impl->uiTextureHandles.find(textureId); uiHandleIt != m_impl->uiTextureHandles.end())
		{
#if NLS_HAS_IMGUI_VULKAN_BACKEND
			if (uiHandleIt->second != VK_NULL_HANDLE)
				ImGui_ImplVulkan_RemoveTexture(uiHandleIt->second);
#endif
			m_impl->uiTextureHandles.erase(uiHandleIt);
		}
		if (const auto found = m_impl->textures.find(textureId); found != m_impl->textures.end())
		{
			if (found->second.view != VK_NULL_HANDLE)
				vkDestroyImageView(m_impl->device, found->second.view, nullptr);
			if (found->second.image != VK_NULL_HANDLE)
				vkDestroyImage(m_impl->device, found->second.image, nullptr);
			if (found->second.memory != VK_NULL_HANDLE)
				vkFreeMemory(m_impl->device, found->second.memory, nullptr);
			m_impl->textures.erase(found);
		}
#endif
	}
	void VulkanRenderDevice::BindTexture(NLS::Render::RHI::TextureDimension, uint32_t textureId)
	{
#if NLS_HAS_VULKAN
		m_impl->boundTexture = textureId;
#endif
	}
	void VulkanRenderDevice::ActivateTexture(uint32_t) {}
	void VulkanRenderDevice::SetupTexture(const NLS::Render::RHI::TextureDesc& desc, const void* data)
	{
#if NLS_HAS_VULKAN
		const auto found = m_impl->textures.find(m_impl->boundTexture);
		if (found == m_impl->textures.end())
			return;

		auto& resource = found->second;
		if (const auto uiHandleIt = m_impl->uiTextureHandles.find(m_impl->boundTexture); uiHandleIt != m_impl->uiTextureHandles.end())
		{
#if NLS_HAS_IMGUI_VULKAN_BACKEND
			if (uiHandleIt->second != VK_NULL_HANDLE)
				ImGui_ImplVulkan_RemoveTexture(uiHandleIt->second);
#endif
			m_impl->uiTextureHandles.erase(uiHandleIt);
		}
		if (resource.view != VK_NULL_HANDLE)
			vkDestroyImageView(m_impl->device, resource.view, nullptr);
		if (resource.image != VK_NULL_HANDLE)
			vkDestroyImage(m_impl->device, resource.image, nullptr);
		if (resource.memory != VK_NULL_HANDLE)
			vkFreeMemory(m_impl->device, resource.memory, nullptr);

		const VkImageCreateInfo imageCreateInfo{
			VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			nullptr,
			GetVkImageCreateFlags(desc.dimension),
			VK_IMAGE_TYPE_2D,
			ToVkFormat(desc.format),
			{ desc.width, desc.height, 1 },
			1,
			NLS::Render::RHI::GetTextureLayerCount(desc.dimension),
			VK_SAMPLE_COUNT_1_BIT,
			VK_IMAGE_TILING_OPTIMAL,
			GetVkImageUsage(desc),
			VK_SHARING_MODE_EXCLUSIVE,
			0,
			nullptr,
			VK_IMAGE_LAYOUT_UNDEFINED
		};

		if (vkCreateImage(m_impl->device, &imageCreateInfo, nullptr, &resource.image) != VK_SUCCESS)
			return;

		VkMemoryRequirements memoryRequirements{};
		vkGetImageMemoryRequirements(m_impl->device, resource.image, &memoryRequirements);

		auto findMemoryType = [this](uint32_t typeBits, VkMemoryPropertyFlags properties)
		{
			VkPhysicalDeviceMemoryProperties memoryProperties{};
			vkGetPhysicalDeviceMemoryProperties(m_impl->physicalDevice, &memoryProperties);
			for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
			{
				if (((typeBits & (1u << i)) != 0u) && (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
					return i;
			}
			return 0u;
		};

		const VkMemoryAllocateInfo allocationInfo{
			VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			nullptr,
			memoryRequirements.size,
			findMemoryType(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
		};

		if (vkAllocateMemory(m_impl->device, &allocationInfo, nullptr, &resource.memory) != VK_SUCCESS)
			return;

		vkBindImageMemory(m_impl->device, resource.image, resource.memory, 0);

		const VkImageAspectFlags aspectMask =
			desc.format == NLS::Render::RHI::TextureFormat::Depth24Stencil8
			? static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
			: VK_IMAGE_ASPECT_COLOR_BIT;
		const VkImageViewCreateInfo imageViewCreateInfo{
			VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			nullptr,
			0,
			resource.image,
			ToVkImageViewType(desc.dimension),
			ToVkFormat(desc.format),
			{},
			{ aspectMask, 0, 1, 0, NLS::Render::RHI::GetTextureLayerCount(desc.dimension) }
		};
		if (vkCreateImageView(m_impl->device, &imageViewCreateInfo, nullptr, &resource.view) != VK_SUCCESS)
			return;

		resource.desc = desc;
		resource.layout = VK_IMAGE_LAYOUT_UNDEFINED;
		resource.initialized = false;

		if (data != nullptr && !IsDepthStencilFormat(desc.format))
		{
			const VkDeviceSize uploadSize =
				static_cast<VkDeviceSize>(desc.width) *
				static_cast<VkDeviceSize>(desc.height) *
				static_cast<VkDeviceSize>(NLS::Render::RHI::GetTextureFormatBytesPerPixel(desc.format)) *
				static_cast<VkDeviceSize>(NLS::Render::RHI::GetTextureLayerCount(desc.dimension));

			VkBuffer stagingBuffer = VK_NULL_HANDLE;
			VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
			const VkBufferCreateInfo stagingBufferInfo{
				VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				nullptr,
				0,
				uploadSize,
				VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
				VK_SHARING_MODE_EXCLUSIVE,
				0,
				nullptr
			};
			if (vkCreateBuffer(m_impl->device, &stagingBufferInfo, nullptr, &stagingBuffer) != VK_SUCCESS)
				return;

			VkMemoryRequirements stagingRequirements{};
			vkGetBufferMemoryRequirements(m_impl->device, stagingBuffer, &stagingRequirements);
			const VkMemoryAllocateInfo stagingAllocationInfo{
				VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				nullptr,
				stagingRequirements.size,
				FindMemoryTypeIndex(
					m_impl->physicalDevice,
					stagingRequirements.memoryTypeBits,
					VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
			};
			if (vkAllocateMemory(m_impl->device, &stagingAllocationInfo, nullptr, &stagingMemory) != VK_SUCCESS)
			{
				vkDestroyBuffer(m_impl->device, stagingBuffer, nullptr);
				return;
			}

			vkBindBufferMemory(m_impl->device, stagingBuffer, stagingMemory, 0);

			void* mappedData = nullptr;
			if (vkMapMemory(m_impl->device, stagingMemory, 0, uploadSize, 0, &mappedData) != VK_SUCCESS)
			{
				vkDestroyBuffer(m_impl->device, stagingBuffer, nullptr);
				vkFreeMemory(m_impl->device, stagingMemory, nullptr);
				return;
			}
			std::memcpy(mappedData, data, static_cast<size_t>(uploadSize));
			vkUnmapMemory(m_impl->device, stagingMemory);

			const auto layerCount = NLS::Render::RHI::GetTextureLayerCount(desc.dimension);
			const auto bytesPerPixel = NLS::Render::RHI::GetTextureFormatBytesPerPixel(desc.format);
			const auto bytesPerLayer = static_cast<VkDeviceSize>(desc.width) * static_cast<VkDeviceSize>(desc.height) * static_cast<VkDeviceSize>(bytesPerPixel);
			std::vector<VkBufferImageCopy> copyRegions;
			copyRegions.reserve(layerCount);
			for (uint32_t layerIndex = 0; layerIndex < layerCount; ++layerIndex)
			{
				copyRegions.push_back({
					bytesPerLayer * static_cast<VkDeviceSize>(layerIndex),
					0,
					0,
					{ aspectMask, 0, layerIndex, 1 },
					{ 0, 0, 0 },
					{ desc.width, desc.height, 1 }
				});
			}

			const bool uploadSucceeded = ExecuteImmediateCommands(*m_impl, [&](VkCommandBuffer commandBuffer)
			{
				if (commandBuffer == VK_NULL_HANDLE)
					return false;

				TransitionTexture(
					*m_impl,
					commandBuffer,
					resource,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					VK_ACCESS_TRANSFER_WRITE_BIT,
					VK_PIPELINE_STAGE_TRANSFER_BIT);

				vkCmdCopyBufferToImage(
					commandBuffer,
					stagingBuffer,
					resource.image,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					static_cast<uint32_t>(copyRegions.size()),
					copyRegions.data());

				TransitionTexture(
					*m_impl,
					commandBuffer,
					resource,
					GetPreferredTextureLayout(desc),
					NLS::Render::RHI::HasUsage(desc.usage, NLS::Render::RHI::TextureUsage::Sampled) ? VK_ACCESS_SHADER_READ_BIT : 0,
					NLS::Render::RHI::HasUsage(desc.usage, NLS::Render::RHI::TextureUsage::Sampled) ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
				return true;
			});

			vkDestroyBuffer(m_impl->device, stagingBuffer, nullptr);
			vkFreeMemory(m_impl->device, stagingMemory, nullptr);
			if (!uploadSucceeded)
				return;
		}

		if (const auto objectIt = m_impl->textureObjects.find(m_impl->boundTexture); objectIt != m_impl->textureObjects.end())
		{
			if (auto textureObject = objectIt->second.lock())
				textureObject->SetDesc(desc);
		}
#endif
	}
	void VulkanRenderDevice::GenerateTextureMipmap(NLS::Render::RHI::TextureDimension) {}
	uint32_t VulkanRenderDevice::CreateFramebuffer()
	{
#if NLS_HAS_VULKAN
		const auto id = m_impl->nextResourceId++;
		m_impl->framebuffers.emplace(id, Impl::FramebufferResource{});
		return id;
#else
		return 0;
#endif
	}
	void VulkanRenderDevice::DestroyFramebuffer(uint32_t framebufferId)
	{
#if NLS_HAS_VULKAN
		if (m_impl->boundFramebuffer == framebufferId)
			m_impl->boundFramebuffer = 0;
		if (const auto found = m_impl->framebuffers.find(framebufferId); found != m_impl->framebuffers.end())
		{
			if (found->second.framebuffer != VK_NULL_HANDLE)
				vkDestroyFramebuffer(m_impl->device, found->second.framebuffer, nullptr);
			if (found->second.renderPass != VK_NULL_HANDLE)
				vkDestroyRenderPass(m_impl->device, found->second.renderPass, nullptr);
			m_impl->framebuffers.erase(found);
		}
#else
		(void)framebufferId;
#endif
	}
	void VulkanRenderDevice::BindFramebuffer(uint32_t framebufferId)
	{
#if NLS_HAS_VULKAN
		m_impl->boundFramebuffer = framebufferId;
#else
		(void)framebufferId;
#endif
	}
	void VulkanRenderDevice::AttachFramebufferColorTexture(uint32_t framebufferId, uint32_t textureId, uint32_t attachmentIndex)
	{
#if NLS_HAS_VULKAN
		auto framebufferIt = m_impl->framebuffers.find(framebufferId);
		if (framebufferIt == m_impl->framebuffers.end())
			return;

		auto& colorTextures = framebufferIt->second.colorTextureIds;
		if (colorTextures.size() <= attachmentIndex)
			colorTextures.resize(attachmentIndex + 1, 0);
		colorTextures[attachmentIndex] = textureId;
		framebufferIt->second.attachmentsDirty = true;
#else
		(void)framebufferId;
		(void)textureId;
		(void)attachmentIndex;
#endif
	}
	void VulkanRenderDevice::AttachFramebufferDepthStencilTexture(uint32_t framebufferId, uint32_t textureId)
	{
#if NLS_HAS_VULKAN
		auto framebufferIt = m_impl->framebuffers.find(framebufferId);
		if (framebufferIt == m_impl->framebuffers.end())
			return;

		framebufferIt->second.depthTextureId = textureId;
		framebufferIt->second.attachmentsDirty = true;
#else
		(void)framebufferId;
		(void)textureId;
#endif
	}
	void VulkanRenderDevice::SetFramebufferDrawBufferCount(uint32_t framebufferId, uint32_t colorAttachmentCount)
	{
#if NLS_HAS_VULKAN
		if (auto framebufferIt = m_impl->framebuffers.find(framebufferId); framebufferIt != m_impl->framebuffers.end())
		{
			framebufferIt->second.drawBufferCount = colorAttachmentCount;
			framebufferIt->second.attachmentsDirty = true;
		}
#else
		(void)framebufferId;
		(void)colorAttachmentCount;
#endif
	}
	void VulkanRenderDevice::BlitDepth(uint32_t sourceFramebufferId, uint32_t destinationFramebufferId, uint32_t width, uint32_t height)
	{
#if NLS_HAS_VULKAN
		const auto sourceFramebufferIt = m_impl->framebuffers.find(sourceFramebufferId);
		const auto destinationFramebufferIt = m_impl->framebuffers.find(destinationFramebufferId);
		if (sourceFramebufferIt == m_impl->framebuffers.end() || destinationFramebufferIt == m_impl->framebuffers.end())
			return;

		const auto sourceTextureIt = m_impl->textures.find(sourceFramebufferIt->second.depthTextureId);
		const auto destinationTextureIt = m_impl->textures.find(destinationFramebufferIt->second.depthTextureId);
		if (sourceTextureIt == m_impl->textures.end() ||
			destinationTextureIt == m_impl->textures.end() ||
			sourceTextureIt->second.image == VK_NULL_HANDLE ||
			destinationTextureIt->second.image == VK_NULL_HANDLE)
		{
			return;
		}

		ExecuteImmediateCommands(*m_impl, [&](VkCommandBuffer commandBuffer)
		{
			if (commandBuffer == VK_NULL_HANDLE)
				return false;

			auto& sourceTexture = sourceTextureIt->second;
			auto& destinationTexture = destinationTextureIt->second;
			const auto sourcePreferredLayout = GetPreferredTextureLayout(sourceTexture.desc);
			const auto destinationPreferredLayout = GetPreferredTextureLayout(destinationTexture.desc);

			TransitionTexture(
				*m_impl,
				commandBuffer,
				sourceTexture,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_ACCESS_TRANSFER_READ_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT);
			TransitionTexture(
				*m_impl,
				commandBuffer,
				destinationTexture,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT);

			const VkImageCopy copyRegion{
				{ static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT), 0, 0, 1 },
				{ 0, 0, 0 },
				{ static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT), 0, 0, 1 },
				{ 0, 0, 0 },
				{ width, height, 1 }
			};
			vkCmdCopyImage(
				commandBuffer,
				sourceTexture.image,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				destinationTexture.image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				1,
				&copyRegion);

			TransitionTexture(
				*m_impl,
				commandBuffer,
				sourceTexture,
				sourcePreferredLayout,
				sourcePreferredLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
				sourcePreferredLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
					? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
					: VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
			TransitionTexture(
				*m_impl,
				commandBuffer,
				destinationTexture,
				destinationPreferredLayout,
				destinationPreferredLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
				destinationPreferredLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
					? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
					: VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
			return true;
		});
#else
		(void)sourceFramebufferId;
		(void)destinationFramebufferId;
		(void)width;
		(void)height;
#endif
	}
	std::shared_ptr<NLS::Render::RHI::IRHIBuffer> VulkanRenderDevice::CreateBufferResource(NLS::Render::RHI::BufferType type)
	{
		auto resource = std::make_shared<VulkanBufferResource>(CreateBuffer(), type, [this](uint32_t id) { DestroyBuffer(id); });
#if NLS_HAS_VULKAN
		if (resource)
			m_impl->bufferObjects[resource->GetResourceId()] = resource;
#endif
		return resource;
	}
	uint32_t VulkanRenderDevice::CreateBuffer()
	{
#if NLS_HAS_VULKAN
		const auto id = m_impl->nextResourceId++;
		m_impl->buffers.emplace(id, Impl::BufferResource{});
		return id;
#else
		return 0;
#endif
	}
	void VulkanRenderDevice::DestroyBuffer(uint32_t bufferId)
	{
#if NLS_HAS_VULKAN
		m_impl->bufferObjects.erase(bufferId);
		if (const auto found = m_impl->buffers.find(bufferId); found != m_impl->buffers.end())
		{
			if (found->second.buffer != VK_NULL_HANDLE)
				vkDestroyBuffer(m_impl->device, found->second.buffer, nullptr);
			if (found->second.memory != VK_NULL_HANDLE)
				vkFreeMemory(m_impl->device, found->second.memory, nullptr);
			m_impl->buffers.erase(found);
		}
#endif
	}
	void VulkanRenderDevice::BindBuffer(NLS::Render::RHI::BufferType type, uint32_t bufferId)
	{
#if NLS_HAS_VULKAN
		m_impl->boundBuffers[type] = bufferId;
#endif
	}
	void VulkanRenderDevice::BindBufferBase(NLS::Render::RHI::BufferType type, uint32_t bindingPoint, uint32_t bufferId)
	{
#if NLS_HAS_VULKAN
		m_impl->boundBuffers[type] = bufferId;
		if (type == NLS::Render::RHI::BufferType::Uniform)
			m_impl->uniformBufferBindings[bindingPoint] = bufferId;
#endif
	}
	void VulkanRenderDevice::SetBufferData(NLS::Render::RHI::BufferType type, size_t size, const void* data, NLS::Render::RHI::BufferUsage)
	{
#if NLS_HAS_VULKAN
		const auto bound = m_impl->boundBuffers.find(type);
		if (bound == m_impl->boundBuffers.end())
			return;

		auto found = m_impl->buffers.find(bound->second);
		if (found == m_impl->buffers.end())
			return;

		auto& resource = found->second;
		if (resource.buffer != VK_NULL_HANDLE)
			vkDestroyBuffer(m_impl->device, resource.buffer, nullptr);
		if (resource.memory != VK_NULL_HANDLE)
			vkFreeMemory(m_impl->device, resource.memory, nullptr);

		const VkBufferCreateInfo createInfo{
			VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			nullptr,
			0,
			static_cast<VkDeviceSize>(size),
			static_cast<VkBufferUsageFlags>(
				type == NLS::Render::RHI::BufferType::Uniform ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT :
				type == NLS::Render::RHI::BufferType::Vertex ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT :
				type == NLS::Render::RHI::BufferType::Index ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT :
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
			VK_SHARING_MODE_EXCLUSIVE,
			0,
			nullptr
		};

		if (vkCreateBuffer(m_impl->device, &createInfo, nullptr, &resource.buffer) != VK_SUCCESS)
			return;

		VkMemoryRequirements memoryRequirements{};
		vkGetBufferMemoryRequirements(m_impl->device, resource.buffer, &memoryRequirements);

		auto findMemoryType = [this](uint32_t typeBits, VkMemoryPropertyFlags properties)
		{
			VkPhysicalDeviceMemoryProperties memoryProperties{};
			vkGetPhysicalDeviceMemoryProperties(m_impl->physicalDevice, &memoryProperties);
			for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
			{
				if (((typeBits & (1u << i)) != 0u) && (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
					return i;
			}
			return 0u;
		};

		const VkMemoryAllocateInfo allocationInfo{
			VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			nullptr,
			memoryRequirements.size,
			findMemoryType(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
		};

		if (vkAllocateMemory(m_impl->device, &allocationInfo, nullptr, &resource.memory) != VK_SUCCESS)
			return;

		vkBindBufferMemory(m_impl->device, resource.buffer, resource.memory, 0);
		resource.size = static_cast<VkDeviceSize>(size);
		if (const auto objectIt = m_impl->bufferObjects.find(bound->second); objectIt != m_impl->bufferObjects.end())
		{
			if (auto bufferObject = objectIt->second.lock())
				bufferObject->SetSize(size);
		}

		if (data != nullptr && size > 0)
		{
			void* mappedData = nullptr;
			if (vkMapMemory(m_impl->device, resource.memory, 0, resource.size, 0, &mappedData) == VK_SUCCESS)
			{
				memcpy(mappedData, data, size);
				vkUnmapMemory(m_impl->device, resource.memory);
			}
		}
#endif
	}
	void VulkanRenderDevice::SetBufferSubData(NLS::Render::RHI::BufferType type, size_t offset, size_t size, const void* data)
	{
#if NLS_HAS_VULKAN
		const auto bound = m_impl->boundBuffers.find(type);
		if (bound == m_impl->boundBuffers.end())
			return;

		auto found = m_impl->buffers.find(bound->second);
		if (found == m_impl->buffers.end() || found->second.memory == VK_NULL_HANDLE || data == nullptr || offset + size > found->second.size)
			return;

		void* mappedData = nullptr;
		if (vkMapMemory(m_impl->device, found->second.memory, offset, size, 0, &mappedData) == VK_SUCCESS)
		{
			std::memcpy(mappedData, data, size);
			vkUnmapMemory(m_impl->device, found->second.memory);
		}
#else
		(void)type;
		(void)offset;
		(void)size;
		(void)data;
#endif
	}
	void* VulkanRenderDevice::GetUITextureHandle(uint32_t textureId) const
	{
#if !NLS_HAS_VULKAN
		(void)textureId;
		return nullptr;
#else
		if (textureId == 0)
			return nullptr;

		auto* impl = m_impl.get();
		auto textureIt = impl->textures.find(textureId);
		if (textureIt == impl->textures.end() || textureIt->second.view == VK_NULL_HANDLE)
			return nullptr;

		impl->uiTexturesUsedThisFrame.insert(textureId);

#if NLS_HAS_IMGUI_VULKAN_BACKEND
		if (const auto existing = impl->uiTextureHandles.find(textureId); existing != impl->uiTextureHandles.end())
			return reinterpret_cast<void*>(existing->second);

		if (impl->uiDescriptorPool == VK_NULL_HANDLE || impl->uiTextureSampler == VK_NULL_HANDLE)
			return nullptr;

		const VkDescriptorSet descriptorSet = ImGui_ImplVulkan_AddTexture(
			impl->uiTextureSampler,
			textureIt->second.view,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		if (descriptorSet == VK_NULL_HANDLE)
			return nullptr;

		impl->uiTextureHandles.emplace(textureId, descriptorSet);
		return reinterpret_cast<void*>(descriptorSet);
#else
		return nullptr;
#endif
#endif
	}

	void VulkanRenderDevice::ReleaseUITextureHandles()
	{
#if NLS_HAS_VULKAN && NLS_HAS_IMGUI_VULKAN_BACKEND
		for (const auto& [_, descriptorSet] : m_impl->uiTextureHandles)
		{
			if (descriptorSet != VK_NULL_HANDLE)
				ImGui_ImplVulkan_RemoveTexture(descriptorSet);
		}
#endif
#if NLS_HAS_VULKAN
		m_impl->uiTextureHandles.clear();
		m_impl->uiTexturesUsedThisFrame.clear();
#endif
	}

	bool VulkanRenderDevice::PrepareUIRender()
	{
#if !NLS_HAS_VULKAN
		return false;
#else
		if (m_impl->swapchainRenderPass == VK_NULL_HANDLE || m_impl->frameContexts.empty())
			return false;

		if (!EnsureFrameRecording(*m_impl))
			return false;

		auto& frame = m_impl->frameContexts[m_impl->currentSwapchainImageIndex % m_impl->frameContexts.size()];
		if (frame.commandBuffer == VK_NULL_HANDLE || frame.framebuffer == VK_NULL_HANDLE)
			return false;

		if (frame.renderPassActive && m_impl->activeRenderTarget != 0)
			EndActiveRenderPass(*m_impl);

		for (const uint32_t textureId : m_impl->uiTexturesUsedThisFrame)
		{
			auto textureIt = m_impl->textures.find(textureId);
			if (textureIt == m_impl->textures.end() || IsDepthStencilFormat(textureIt->second.desc.format))
				continue;

			TransitionTexture(
				*m_impl,
				frame.commandBuffer,
				textureIt->second,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_ACCESS_SHADER_READ_BIT,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		}
		m_impl->uiTexturesUsedThisFrame.clear();

		if (!frame.renderPassActive)
		{
			if (!m_impl->swapchainHasColorContent)
			{
				VkImageMemoryBarrier toTransfer{
					VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					nullptr,
					0,
					VK_ACCESS_TRANSFER_WRITE_BIT,
					frame.imageInitialized ? frame.layout : VK_IMAGE_LAYOUT_UNDEFINED,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					VK_QUEUE_FAMILY_IGNORED,
					VK_QUEUE_FAMILY_IGNORED,
					frame.image,
					{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
				};
				vkCmdPipelineBarrier(
					frame.commandBuffer,
					frame.imageInitialized ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
					VK_PIPELINE_STAGE_TRANSFER_BIT,
					0,
					0, nullptr,
					0, nullptr,
					1, &toTransfer);

				const VkClearColorValue clearValue{ { m_impl->clearColor[0], m_impl->clearColor[1], m_impl->clearColor[2], m_impl->clearColor[3] } };
				const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				vkCmdClearColorImage(frame.commandBuffer, frame.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearValue, 1, &range);
				frame.imageInitialized = true;
				frame.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				m_impl->swapchainHasColorContent = true;
			}

			VkImageMemoryBarrier toColorAttachment{
				VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				nullptr,
				static_cast<VkAccessFlags>(
					frame.layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
						? VK_ACCESS_TRANSFER_WRITE_BIT
						: (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)),
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				frame.imageInitialized ? frame.layout : VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				VK_QUEUE_FAMILY_IGNORED,
				VK_QUEUE_FAMILY_IGNORED,
				frame.image,
				{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
			};
			vkCmdPipelineBarrier(
				frame.commandBuffer,
				frame.imageInitialized ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				0,
				0, nullptr,
				0, nullptr,
				1, &toColorAttachment);

			const VkRenderPassBeginInfo renderPassBeginInfo{
				VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
				nullptr,
				m_impl->swapchainRenderPass,
				frame.framebuffer,
				{ { 0, 0 }, m_impl->swapchainExtent },
				0,
				nullptr
			};
			vkCmdBeginRenderPass(frame.commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
			frame.renderPassActive = true;
			frame.imageInitialized = true;
			frame.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			m_impl->activeRenderTarget = 0;
		}

		m_impl->swapchainHasColorContent = true;
		m_impl->hasPendingCommands = true;
		return true;
#endif
	}
	std::string VulkanRenderDevice::GetVendor() { return m_impl->vendor; }
	std::string VulkanRenderDevice::GetHardware() { return m_impl->hardware; }
	std::string VulkanRenderDevice::GetVersion() { return m_impl->version; }
	std::string VulkanRenderDevice::GetShadingLanguageVersion() { return m_impl->shadingLanguageVersion; }
	NLS::Render::RHI::RHIDeviceCapabilities VulkanRenderDevice::GetCapabilities() const { return m_impl->capabilities; }
	NLS::Render::RHI::NativeRenderDeviceInfo VulkanRenderDevice::GetNativeDeviceInfo() const
	{
		NLS::Render::RHI::NativeRenderDeviceInfo info{};
		info.backend = NLS::Render::RHI::NativeBackendType::Vulkan;
#if NLS_HAS_VULKAN
		info.instance = reinterpret_cast<void*>(m_impl->instance);
		info.physicalDevice = reinterpret_cast<void*>(m_impl->physicalDevice);
		info.device = reinterpret_cast<void*>(m_impl->device);
		info.graphicsQueue = reinterpret_cast<void*>(m_impl->graphicsQueue);
		info.surface = reinterpret_cast<void*>(m_impl->surface);
		info.swapchain = reinterpret_cast<void*>(m_impl->swapchain);
		info.uiRenderPass = reinterpret_cast<void*>(m_impl->swapchainRenderPass);
		info.uiDescriptorPool = reinterpret_cast<void*>(m_impl->uiDescriptorPool);
		info.platformWindow = m_impl->swapchainWindow;
		info.graphicsQueueFamilyIndex = m_impl->graphicsQueueFamilyIndex;
		info.swapchainImageCount = static_cast<uint32_t>(m_impl->swapchainImages.size());
		if (!m_impl->frameContexts.empty())
			info.currentCommandBuffer = reinterpret_cast<void*>(m_impl->frameContexts[m_impl->currentSwapchainImageIndex % m_impl->frameContexts.size()].commandBuffer);
#endif
		return info;
	}
	bool VulkanRenderDevice::IsBackendReady() const { return m_impl->backendReady; }
	bool VulkanRenderDevice::CreateSwapchain(const NLS::Render::RHI::SwapchainDesc& desc)
	{
#if !NLS_HAS_VULKAN
		(void)desc;
		return false;
#else
		if (!m_impl->backendReady || desc.platformWindow == nullptr)
			return false;

		DestroySwapchain();

		auto* window = static_cast<GLFWwindow*>(desc.platformWindow);
		if (glfwCreateWindowSurface(m_impl->instance, window, nullptr, &m_impl->surface) != VK_SUCCESS)
		{
			NLS_LOG_ERROR("Failed to create Vulkan window surface.");
			return false;
		}

		VkBool32 presentSupported = VK_FALSE;
		if (vkGetPhysicalDeviceSurfaceSupportKHR(m_impl->physicalDevice, m_impl->graphicsQueueFamilyIndex, m_impl->surface, &presentSupported) != VK_SUCCESS ||
			presentSupported == VK_FALSE)
		{
			NLS_LOG_ERROR("Selected Vulkan queue family does not support presentation.");
			DestroySwapchain();
			return false;
		}

		uint32_t formatCount = 0;
		vkGetPhysicalDeviceSurfaceFormatsKHR(m_impl->physicalDevice, m_impl->surface, &formatCount, nullptr);
		if (formatCount == 0)
		{
			DestroySwapchain();
			return false;
		}

		std::vector<VkSurfaceFormatKHR> formats(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(m_impl->physicalDevice, m_impl->surface, &formatCount, formats.data());
		const VkSurfaceFormatKHR selectedFormat = formats[0];

		VkSurfaceCapabilitiesKHR surfaceCapabilities{};
		if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_impl->physicalDevice, m_impl->surface, &surfaceCapabilities) != VK_SUCCESS)
		{
			DestroySwapchain();
			return false;
		}

		m_impl->swapchainExtent.width = desc.width > 0 ? desc.width : surfaceCapabilities.currentExtent.width;
		m_impl->swapchainExtent.height = desc.height > 0 ? desc.height : surfaceCapabilities.currentExtent.height;
		m_impl->swapchainFormat = selectedFormat.format;
		m_impl->swapchainImageCount = std::max(surfaceCapabilities.minImageCount, desc.imageCount);
		if (surfaceCapabilities.maxImageCount > 0)
			m_impl->swapchainImageCount = std::min(m_impl->swapchainImageCount, surfaceCapabilities.maxImageCount);
		m_impl->swapchainVsync = desc.vsync;
		m_impl->swapchainWindow = window;
		m_impl->currentSwapchainImageIndex = 0;

		uint32_t presentModeCount = 0;
		vkGetPhysicalDeviceSurfacePresentModesKHR(m_impl->physicalDevice, m_impl->surface, &presentModeCount, nullptr);
		std::vector<VkPresentModeKHR> presentModes(presentModeCount);
		if (presentModeCount > 0)
			vkGetPhysicalDeviceSurfacePresentModesKHR(m_impl->physicalDevice, m_impl->surface, &presentModeCount, presentModes.data());

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

		const VkSwapchainCreateInfoKHR createInfo{
			VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
			nullptr,
			0,
			m_impl->surface,
			m_impl->swapchainImageCount,
			m_impl->swapchainFormat,
			selectedFormat.colorSpace,
			m_impl->swapchainExtent,
			1,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_SHARING_MODE_EXCLUSIVE,
			0,
			nullptr,
			surfaceCapabilities.currentTransform,
			VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
			presentMode,
			VK_TRUE,
			VK_NULL_HANDLE
		};

		if (vkCreateSwapchainKHR(m_impl->device, &createInfo, nullptr, &m_impl->swapchain) != VK_SUCCESS)
		{
			NLS_LOG_ERROR("Failed to create Vulkan swapchain.");
			DestroySwapchain();
			return false;
		}

		uint32_t imageCount = 0;
		vkGetSwapchainImagesKHR(m_impl->device, m_impl->swapchain, &imageCount, nullptr);
		m_impl->swapchainImages.resize(imageCount);
		vkGetSwapchainImagesKHR(m_impl->device, m_impl->swapchain, &imageCount, m_impl->swapchainImages.data());
		m_impl->acquireFences.resize(imageCount, VK_NULL_HANDLE);
		m_impl->frameContexts.resize(imageCount);

		const VkAttachmentDescription colorAttachment{
			0,
			m_impl->swapchainFormat,
			VK_SAMPLE_COUNT_1_BIT,
			VK_ATTACHMENT_LOAD_OP_LOAD,
			VK_ATTACHMENT_STORE_OP_STORE,
			VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			VK_ATTACHMENT_STORE_OP_DONT_CARE,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
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
		if (vkCreateRenderPass(m_impl->device, &renderPassInfo, nullptr, &m_impl->swapchainRenderPass) != VK_SUCCESS)
		{
			DestroySwapchain();
			return false;
		}

		const VkFenceCreateInfo fenceInfo{
			VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
			nullptr,
			VK_FENCE_CREATE_SIGNALED_BIT
		};

		for (auto& fence : m_impl->acquireFences)
		{
			if (vkCreateFence(m_impl->device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
			{
				DestroySwapchain();
				return false;
			}
		}

		const VkCommandBufferAllocateInfo commandBufferInfo{
			VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			nullptr,
			m_impl->commandPool,
			VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			imageCount
		};

		std::vector<VkCommandBuffer> commandBuffers(imageCount, VK_NULL_HANDLE);
		if (vkAllocateCommandBuffers(m_impl->device, &commandBufferInfo, commandBuffers.data()) != VK_SUCCESS)
		{
			DestroySwapchain();
			return false;
		}

		const VkSemaphoreCreateInfo semaphoreInfo{
			VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
			nullptr,
			0
		};

		for (uint32_t i = 0; i < imageCount; ++i)
		{
			m_impl->frameContexts[i].image = m_impl->swapchainImages[i];
			m_impl->frameContexts[i].commandBuffer = commandBuffers[i];
			m_impl->frameContexts[i].fence = m_impl->acquireFences[i];

			const VkImageViewCreateInfo viewInfo{
				VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				nullptr,
				0,
				m_impl->swapchainImages[i],
				VK_IMAGE_VIEW_TYPE_2D,
				m_impl->swapchainFormat,
				{},
				{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
			};

			if (vkCreateImageView(m_impl->device, &viewInfo, nullptr, &m_impl->frameContexts[i].view) != VK_SUCCESS ||
				vkCreateSemaphore(m_impl->device, &semaphoreInfo, nullptr, &m_impl->frameContexts[i].imageAvailableSemaphore) != VK_SUCCESS ||
				vkCreateSemaphore(m_impl->device, &semaphoreInfo, nullptr, &m_impl->frameContexts[i].renderFinishedSemaphore) != VK_SUCCESS)
			{
				DestroySwapchain();
				return false;
			}

			const VkFramebufferCreateInfo framebufferInfo{
				VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
				nullptr,
				0,
				m_impl->swapchainRenderPass,
				1,
				&m_impl->frameContexts[i].view,
				m_impl->swapchainExtent.width,
				m_impl->swapchainExtent.height,
				1
			};
			if (vkCreateFramebuffer(m_impl->device, &framebufferInfo, nullptr, &m_impl->frameContexts[i].framebuffer) != VK_SUCCESS)
			{
				DestroySwapchain();
				return false;
			}
		}

		return true;
#endif
	}

	void VulkanRenderDevice::DestroySwapchain()
	{
#if NLS_HAS_VULKAN
		if (m_impl->device != VK_NULL_HANDLE)
			vkDeviceWaitIdle(m_impl->device);

		for (auto fence : m_impl->acquireFences)
		{
			if (fence != VK_NULL_HANDLE)
				vkDestroyFence(m_impl->device, fence, nullptr);
		}
		m_impl->acquireFences.clear();
		for (auto& frame : m_impl->frameContexts)
		{
			if (frame.framebuffer != VK_NULL_HANDLE)
				vkDestroyFramebuffer(m_impl->device, frame.framebuffer, nullptr);
			if (frame.view != VK_NULL_HANDLE)
				vkDestroyImageView(m_impl->device, frame.view, nullptr);
			if (frame.imageAvailableSemaphore != VK_NULL_HANDLE)
				vkDestroySemaphore(m_impl->device, frame.imageAvailableSemaphore, nullptr);
			if (frame.renderFinishedSemaphore != VK_NULL_HANDLE)
				vkDestroySemaphore(m_impl->device, frame.renderFinishedSemaphore, nullptr);
		}
		m_impl->frameContexts.clear();
		m_impl->swapchainImages.clear();
		if (m_impl->swapchainRenderPass != VK_NULL_HANDLE)
		{
			vkDestroyRenderPass(m_impl->device, m_impl->swapchainRenderPass, nullptr);
			m_impl->swapchainRenderPass = VK_NULL_HANDLE;
		}

		if (m_impl->swapchain != VK_NULL_HANDLE)
		{
			vkDestroySwapchainKHR(m_impl->device, m_impl->swapchain, nullptr);
			m_impl->swapchain = VK_NULL_HANDLE;
		}

		if (m_impl->surface != VK_NULL_HANDLE)
		{
			vkDestroySurfaceKHR(m_impl->instance, m_impl->surface, nullptr);
			m_impl->surface = VK_NULL_HANDLE;
		}
		m_impl->swapchainWindow = nullptr;
		m_impl->hasPendingCommands = false;
		m_impl->isFrameRecording = false;
		m_impl->swapchainImageAcquired = false;
		m_impl->pendingAcquireSemaphore = VK_NULL_HANDLE;
		m_impl->activeRenderTarget = std::numeric_limits<uint32_t>::max();
#endif
	}

	void VulkanRenderDevice::ResizeSwapchain(uint32_t width, uint32_t height)
	{
#if NLS_HAS_VULKAN
		if (m_impl->swapchainWindow == nullptr)
			return;

		NLS::Render::RHI::SwapchainDesc desc{};
		desc.width = width;
		desc.height = height;
		desc.platformWindow = m_impl->swapchainWindow;
		desc.vsync = m_impl->swapchainVsync;
		DestroySwapchain();
		CreateSwapchain(desc);
#else
		(void)width;
		(void)height;
#endif
	}

	void VulkanRenderDevice::PresentSwapchain()
	{
#if NLS_HAS_VULKAN
		if (m_impl->swapchain == VK_NULL_HANDLE || m_impl->frameContexts.empty())
			return;

		auto& frame = m_impl->frameContexts[m_impl->currentSwapchainImageIndex % m_impl->frameContexts.size()];
		if (m_impl->isFrameRecording)
		{
			if (frame.renderPassActive)
			{
				vkCmdEndRenderPass(frame.commandBuffer);
				frame.renderPassActive = false;
				m_impl->activeRenderTarget = std::numeric_limits<uint32_t>::max();
			}

			VkImageMemoryBarrier toPresent{
				VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				nullptr,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
				0,
				frame.imageInitialized ? frame.layout : VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
				VK_QUEUE_FAMILY_IGNORED,
				VK_QUEUE_FAMILY_IGNORED,
				frame.image,
				{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
			};
			vkCmdPipelineBarrier(
				frame.commandBuffer,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
				0,
				0, nullptr,
				0, nullptr,
				1, &toPresent);
			vkEndCommandBuffer(frame.commandBuffer);
			m_impl->isFrameRecording = false;
			frame.layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			frame.imageInitialized = true;
		}

		if (m_impl->hasPendingCommands)
		{
			const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			const uint32_t waitSemaphoreCount = m_impl->pendingAcquireSemaphore != VK_NULL_HANDLE ? 1u : 0u;
			const VkSubmitInfo submitInfo{
				VK_STRUCTURE_TYPE_SUBMIT_INFO,
				nullptr,
				waitSemaphoreCount,
				waitSemaphoreCount == 0 ? nullptr : &m_impl->pendingAcquireSemaphore,
				waitSemaphoreCount == 0 ? nullptr : &waitStage,
				1,
				&frame.commandBuffer,
				1,
				&frame.renderFinishedSemaphore
			};

			if (vkQueueSubmit(m_impl->graphicsQueue, 1, &submitInfo, frame.fence) == VK_SUCCESS)
			{
				m_impl->hasPendingCommands = false;
				m_impl->pendingAcquireSemaphore = VK_NULL_HANDLE;
			}
		}

		const VkPresentInfoKHR presentInfo{
			VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
			nullptr,
			1,
			&frame.renderFinishedSemaphore,
			1,
			&m_impl->swapchain,
			&m_impl->currentSwapchainImageIndex,
			nullptr
		};

		vkQueuePresentKHR(m_impl->graphicsQueue, &presentInfo);
		m_impl->swapchainImageAcquired = false;
		m_impl->pendingAcquireSemaphore = VK_NULL_HANDLE;
		m_impl->swapchainHasColorContent = false;
#endif
	}
}
