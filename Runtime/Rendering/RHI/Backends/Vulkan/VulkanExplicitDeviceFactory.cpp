#include "Rendering/RHI/Backends/Vulkan/VulkanExplicitDeviceFactory.h"

#include <array>
#include <string>
#include <vector>

#include <Debug/Logger.h>
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHISync.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHIPipeline.h"

#if NLS_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace NLS::Render::Backend
{
	namespace
	{
#if NLS_HAS_VULKAN
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
#endif

		class NativeVulkanFence final : public NLS::Render::RHI::RHIFence
		{
		public:
			NativeVulkanFence(VkDevice device, VkFence fence, const std::string& debugName)
				: m_device(device)
				, m_fence(fence)
				, m_debugName(debugName)
			{
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
				VkResult result = vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, timeoutNanoseconds / 1000000);
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

			void* GetNativeSemaphoreHandle() override { return reinterpret_cast<void*>(m_semaphore); }

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
		class NativeVulkanSwapchain;
		class NativeVulkanTexture;

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
			void Present(const NLS::Render::RHI::RHIPresentDesc& presentDesc) override
			{
#if NLS_HAS_VULKAN
				if (m_queue == nullptr || presentDesc.swapchain == nullptr)
					return;

				VkSwapchainKHR swapchain = reinterpret_cast<VkSwapchainKHR>(presentDesc.swapchain->GetNativeSwapchainHandle());
				if (swapchain == VK_NULL_HANDLE)
					return;

				VkPresentInfoKHR presentInfo{};
				presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
				presentInfo.swapchainCount = 1;
				presentInfo.pSwapchains = &swapchain;
				presentInfo.pImageIndices = &presentDesc.imageIndex;

				// Handle wait semaphores if any
				std::vector<VkSemaphore> waitSemaphores;
				if (!presentDesc.waitSemaphores.empty())
				{
					for (const auto& sem : presentDesc.waitSemaphores)
					{
						if (sem != nullptr)
						{
							VkSemaphore vkSem = reinterpret_cast<VkSemaphore>(sem->GetNativeSemaphoreHandle());
							if (vkSem != VK_NULL_HANDLE)
								waitSemaphores.push_back(vkSem);
						}
					}
				}

				// Add UI signal semaphore to wait semaphores - ensures UI rendering completes before present
				if (presentDesc.uiSignalSemaphore != nullptr)
				{
					VkSemaphore uiSem = reinterpret_cast<VkSemaphore>(presentDesc.uiSignalSemaphore);
					if (uiSem != VK_NULL_HANDLE)
						waitSemaphores.push_back(uiSem);
				}

				if (!waitSemaphores.empty())
				{
					presentInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
					presentInfo.pWaitSemaphores = waitSemaphores.data();
				}

				vkQueuePresentKHR(m_queue, &presentInfo);
#endif
			}

#if NLS_HAS_VULKAN
			VkQueue GetQueue() const { return m_queue; }
#endif

		private:
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
				vkResetCommandBuffer(m_commandBuffer, 0);
#endif
			}
			bool IsRecording() const override { return m_recording; }
			void* GetNativeCommandBuffer() const override
			{
#if NLS_HAS_VULKAN
				return reinterpret_cast<void*>(m_commandBuffer);
#else
				return nullptr;
#endif
			}

			void BeginRenderPass(const NLS::Render::RHI::RHIRenderPassDesc& desc) override
			{
#if NLS_HAS_VULKAN
				if (m_commandBuffer == nullptr)
					return;

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
				else
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
								barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
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
							barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
							barrier.subresourceRange.baseMipLevel = 0;
							barrier.subresourceRange.levelCount = 1;
							barrier.subresourceRange.baseArrayLayer = 0;
							barrier.subresourceRange.layerCount = 1;
							vkCmdPipelineBarrier(m_commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
						}
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
				if (m_dynamicRenderingEnabled)
				{
					vkCmdEndRendering(m_commandBuffer);
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
				vp.y = viewport.y;
				vp.width = viewport.width;
				vp.height = viewport.height;
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
			void BindComputePipeline(const std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>& pipeline) override
			{
#if NLS_HAS_VULKAN
				if (m_commandBuffer == nullptr || pipeline == nullptr)
					return;
				m_boundComputePipeline = pipeline;
#endif
			}
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
				if (m_commandBuffer != nullptr)
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
					vkBarrier.oldLayout = ToVkImageLayout(tb.before);
					vkBarrier.newLayout = ToVkImageLayout(tb.after);
					vkBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					vkBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					vkBarrier.image = image;
					vkBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
					vkBarrier.subresourceRange.baseMipLevel = tb.subresourceRange.baseMipLevel;
					vkBarrier.subresourceRange.levelCount = tb.subresourceRange.mipLevelCount;
					vkBarrier.subresourceRange.baseArrayLayer = tb.subresourceRange.baseArrayLayer;
					vkBarrier.subresourceRange.layerCount = tb.subresourceRange.arrayLayerCount;
					imageBarriers.push_back(vkBarrier);
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
#endif
			std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> m_boundPipeline;
			std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> m_boundComputePipeline;

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
				default:
					return VK_IMAGE_LAYOUT_GENERAL;
				}
			}
#endif
		};

		void NativeVulkanQueue::Submit(const NLS::Render::RHI::RHISubmitDesc& submitDesc)
		{
#if NLS_HAS_VULKAN
			if (m_device == nullptr || m_queue == nullptr)
				return;

			std::vector<VkCommandBuffer> vkCommandBuffers;
			vkCommandBuffers.reserve(submitDesc.commandBuffers.size());

			for (const auto& cmdBuffer : submitDesc.commandBuffers)
			{
				if (cmdBuffer == nullptr)
					continue;
				auto* nativeCmdBuffer = dynamic_cast<NativeVulkanCommandBuffer*>(cmdBuffer.get());
				if (nativeCmdBuffer != nullptr)
					vkCommandBuffers.push_back(nativeCmdBuffer->GetCommandBuffer());
			}

			if (vkCommandBuffers.empty())
				return;

			VkSubmitInfo submitInfo{};
			submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			submitInfo.commandBufferCount = static_cast<uint32_t>(vkCommandBuffers.size());
			submitInfo.pCommandBuffers = vkCommandBuffers.data();

			VkResult result = vkQueueSubmit(m_queue, 1, &submitInfo, VK_NULL_HANDLE);
			(void)result;
#endif
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
			NativeVulkanSwapchainTexture(VkDevice device, VkImage image, VkFormat format)
				: m_device(device)
				, m_image(image)
				, m_format(format)
			{
				m_desc.format = NLS::Render::RHI::TextureFormat::RGBA8;
				m_desc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
				m_desc.extent.width = 0;
				m_desc.extent.height = 0;
				m_desc.extent.depth = 1;
				m_desc.arrayLayers = 1;
				m_desc.mipLevels = 1;
				m_desc.usage = NLS::Render::RHI::TextureUsageFlags::ColorAttachment;
			}

			std::string_view GetDebugName() const override { return "SwapchainTexture"; }
			const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
			NLS::Render::RHI::ResourceState GetState() const override { return NLS::Render::RHI::ResourceState::RenderTarget; }
			NLS::Render::RHI::NativeHandle GetNativeImageHandle() override { return { NLS::Render::RHI::BackendType::Vulkan, reinterpret_cast<void*>(m_image) }; }

		private:
			VkDevice m_device = nullptr;
			VkImage m_image = VK_NULL_HANDLE;
			VkFormat m_format = VK_FORMAT_UNDEFINED;
			NLS::Render::RHI::RHITextureDesc m_desc{};
		};

		// Helper class to wrap a swapchain backbuffer as a texture view
		class NativeVulkanSwapchainBackbufferView final : public NLS::Render::RHI::RHITextureView
		{
		public:
			NativeVulkanSwapchainBackbufferView(VkDevice device, VkImage image, VkImageView imageView, VkFormat format)
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
				m_texture = std::make_shared<NativeVulkanSwapchainTexture>(device, image, format);
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
			NativeVulkanSwapchain(VkDevice device, VkSwapchainKHR swapchain, VkSurfaceKHR surface, const NLS::Render::RHI::SwapchainDesc& desc)
				: m_device(device)
				, m_swapchain(swapchain)
				, m_surface(surface)
				, m_desc(desc)
				, m_imageCount(desc.imageCount > 0 ? desc.imageCount : 2)
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

				// Wait on fence if provided (same pattern as DX12)
				if (signalFence != nullptr)
					signalFence->Wait(5000000000ULL); // 5 second timeout

				// Get Vulkan semaphore handle if provided
				VkSemaphore vkSemaphore = VK_NULL_HANDLE;
				if (signalSemaphore != nullptr)
				{
					auto* nativeSemaphore = static_cast<NativeVulkanSemaphore*>(signalSemaphore.get());
					if (nativeSemaphore != nullptr)
						vkSemaphore = nativeSemaphore->GetSemaphore();
				}

				// Actually acquire the next image from the swapchain
				uint32_t imageIndex = 0;
				VkResult result = vkAcquireNextImageKHR(
					m_device,
					m_swapchain,
					UINT64_MAX, // timeout - infinite
					vkSemaphore,
					VK_NULL_HANDLE, // no fence in AcquireNextImage (we use the one in Driver)
					&imageIndex);

				if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
					return std::nullopt;

				NLS::Render::RHI::RHIAcquiredImage image;
				image.imageIndex = imageIndex;
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
			void Resize(uint32_t width, uint32_t height) override
			{
#if NLS_HAS_VULKAN
				if (m_device == nullptr || m_surface == nullptr)
					return;

				// Get surface capabilities
				VkSurfaceCapabilitiesKHR surfaceCapabilities{};
				if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &surfaceCapabilities) != VK_SUCCESS)
					return;

				// Get surface formats
				uint32_t formatCount = 0;
				if (vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, nullptr) != VK_SUCCESS || formatCount == 0)
					return;
				std::vector<VkSurfaceFormatKHR> formats(formatCount);
				if (vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, formats.data()) != VK_SUCCESS)
					return;

				// Get present modes
				uint32_t presentModeCount = 0;
				if (vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &presentModeCount, nullptr) != VK_SUCCESS)
					return;
				std::vector<VkPresentModeKHR> presentModes(presentModeCount);
				if (vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &presentModeCount, presentModes.data()) != VK_SUCCESS)
					return;

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
				createInfo.imageFormat = formats[0].format;
				createInfo.imageColorSpace = formats[0].colorSpace;
				createInfo.imageExtent = extent;
				createInfo.imageArrayLayers = 1;
				createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
				createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
				createInfo.preTransform = surfaceCapabilities.currentTransform;
				createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
				createInfo.presentMode = presentMode;
				createInfo.clipped = VK_TRUE;
				createInfo.oldSwapchain = oldSwapchain;

				VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
				if (vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &newSwapchain) != VK_SUCCESS)
					return;

				m_swapchain = newSwapchain;
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
#endif
			}

			void* GetNativeSwapchainHandle() override { return reinterpret_cast<void*>(m_swapchain); }

#if NLS_HAS_VULKAN
			VkSwapchainKHR GetSwapchain() const { return m_swapchain; }
			VkSurfaceKHR GetSurface() const { return m_surface; }
			VkDevice GetDevice() const { return m_device; }
			VkPhysicalDevice GetPhysicalDevice() const { return m_physicalDevice; }
			void SetPhysicalDevice(VkPhysicalDevice physicalDevice) { m_physicalDevice = physicalDevice; }
#endif

		private:
			VkDevice m_device = nullptr;
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
							m_device, m_swapchainImages[i], imageView, m_imageFormat);
						m_backbufferViews.push_back(view);
					}
				}
#endif
			}
		};

		class NativeVulkanBuffer final : public NLS::Render::RHI::RHIBuffer
		{
		public:
			NativeVulkanBuffer(VkDevice device, VkPhysicalDevice physicalDevice, const NLS::Render::RHI::RHIBufferDesc& desc, const void* initialData)
				: m_device(device)
				, m_desc(desc)
			{
#if NLS_HAS_VULKAN
				if (device == nullptr)
					return;

				VkBufferCreateInfo bufferInfo{};
				bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
				bufferInfo.size = desc.size;
				bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
					VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
				bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

				VkResult result = vkCreateBuffer(device, &bufferInfo, nullptr, &m_buffer);
				if (result != VK_SUCCESS)
					return;

				// Allocate memory for the buffer
				VkMemoryRequirements memRequirements;
				vkGetBufferMemoryRequirements(device, m_buffer, &memRequirements);

				VkMemoryAllocateInfo allocInfo{};
				allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
				allocInfo.allocationSize = memRequirements.size;
				allocInfo.memoryTypeIndex = FindMemoryType(physicalDevice, memRequirements, desc.memoryUsage);

				result = vkAllocateMemory(device, &allocInfo, nullptr, &m_memory);
				if (result != VK_SUCCESS)
					return;

				vkBindBufferMemory(device, m_buffer, m_memory, 0);

				// Copy initial data if provided
				if (initialData != nullptr)
				{
					void* mappedData = nullptr;
					vkMapMemory(device, m_memory, 0, desc.size, 0, &mappedData);
					if (mappedData != nullptr)
					{
						std::memcpy(mappedData, initialData, desc.size);
						vkUnmapMemory(device, m_memory);
					}
				}
#endif
			}

			~NativeVulkanBuffer()
			{
#if NLS_HAS_VULKAN
				if (m_device != nullptr)
				{
					if (m_buffer != VK_NULL_HANDLE)
						vkDestroyBuffer(m_device, m_buffer, nullptr);
					if (m_memory != VK_NULL_HANDLE)
						vkFreeMemory(m_device, m_memory, nullptr);
				}
#endif
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHIBufferDesc& GetDesc() const override { return m_desc; }
			NLS::Render::RHI::ResourceState GetState() const override { return m_state; }
			uint64_t GetGPUAddress() const override { return 0; } // Vulkan doesn't use GPU addresses like DX12
			NLS::Render::RHI::NativeHandle GetNativeBufferHandle() override { return { NLS::Render::RHI::BackendType::Vulkan, reinterpret_cast<void*>(m_buffer) }; }

		private:
			VkDevice m_device = nullptr;
			NLS::Render::RHI::RHIBufferDesc m_desc{};
			NLS::Render::RHI::ResourceState m_state = NLS::Render::RHI::ResourceState::Unknown;
#if NLS_HAS_VULKAN
			VkBuffer m_buffer = VK_NULL_HANDLE;
			VkDeviceMemory m_memory = VK_NULL_HANDLE;
#endif
		};

		class NativeVulkanTexture final : public NLS::Render::RHI::RHITexture
		{
		public:
			NativeVulkanTexture(VkDevice device, VkPhysicalDevice physicalDevice, const NLS::Render::RHI::RHITextureDesc& desc, const void*)
				: m_device(device)
				, m_desc(desc)
			{
#if NLS_HAS_VULKAN
				if (device == nullptr)
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
				imageInfo.arrayLayers = (desc.dimension == NLS::Render::RHI::TextureDimension::TextureCube) ? 6 : 1;
				imageInfo.mipLevels = desc.mipLevels;
				imageInfo.format = ToVkFormat(desc.format);
				imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
				imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				imageInfo.usage = ToVkImageUsage(desc.usage);
				imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
				imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

				if (desc.dimension == NLS::Render::RHI::TextureDimension::TextureCube)
					imageInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

				VkResult result = vkCreateImage(device, &imageInfo, nullptr, &m_image);
				if (result != VK_SUCCESS)
					return;

				// Allocate memory
				VkMemoryRequirements memRequirements;
				vkGetImageMemoryRequirements(device, m_image, &memRequirements);

				VkMemoryAllocateInfo allocInfo{};
				allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
				allocInfo.allocationSize = memRequirements.size;
				allocInfo.memoryTypeIndex = FindMemoryType(physicalDevice, memRequirements, desc.memoryUsage);

				result = vkAllocateMemory(device, &allocInfo, nullptr, &m_memory);
				if (result != VK_SUCCESS)
					return;

				vkBindImageMemory(device, m_image, m_memory, 0);
#endif
			}

			~NativeVulkanTexture()
			{
#if NLS_HAS_VULKAN
				if (m_device != nullptr)
				{
					if (m_image != VK_NULL_HANDLE)
						vkDestroyImage(m_device, m_image, nullptr);
					if (m_memory != VK_NULL_HANDLE)
						vkFreeMemory(m_device, m_memory, nullptr);
				}
#endif
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
			NLS::Render::RHI::ResourceState GetState() const override { return m_state; }
			NLS::Render::RHI::NativeHandle GetNativeImageHandle() override { return { NLS::Render::RHI::BackendType::Vulkan, reinterpret_cast<void*>(m_image) }; }

#if NLS_HAS_VULKAN
			static VkFormat ToVkFormat(NLS::Render::RHI::TextureFormat format)
			{
				switch (format)
				{
				case NLS::Render::RHI::TextureFormat::RGBA8:
				case NLS::Render::RHI::TextureFormat::RGB8: return VK_FORMAT_R8G8B8A8_UNORM;
				case NLS::Render::RHI::TextureFormat::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
				case NLS::Render::RHI::TextureFormat::Depth24Stencil8: return VK_FORMAT_D24_UNORM_S8_UINT;
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
			NLS::Render::RHI::RHITextureDesc m_desc{};
			NLS::Render::RHI::ResourceState m_state = NLS::Render::RHI::ResourceState::Unknown;
#if NLS_HAS_VULKAN
			VkImage m_image = VK_NULL_HANDLE;
			VkDeviceMemory m_memory = VK_NULL_HANDLE;
#endif
		};

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
				viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
				viewInfo.format = ToVkFormat(texture->GetDesc().format);
				viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				viewInfo.subresourceRange.baseMipLevel = desc.subresourceRange.baseMipLevel;
				viewInfo.subresourceRange.levelCount = desc.subresourceRange.mipLevelCount;
				viewInfo.subresourceRange.baseArrayLayer = desc.subresourceRange.baseArrayLayer;
				viewInfo.subresourceRange.layerCount = desc.subresourceRange.arrayLayerCount;

				vkCreateImageView(device, &viewInfo, nullptr, &m_imageView);
#endif
			}

			~NativeVulkanTextureView()
			{
#if NLS_HAS_VULKAN
				if (m_device != nullptr && m_imageView != VK_NULL_HANDLE)
					vkDestroyImageView(m_device, m_imageView, nullptr);
#endif
			}

#if NLS_HAS_VULKAN
			static VkFormat ToVkFormat(NLS::Render::RHI::TextureFormat format)
			{
				switch (format)
				{
				case NLS::Render::RHI::TextureFormat::RGBA8:
				case NLS::Render::RHI::TextureFormat::RGB8: return VK_FORMAT_R8G8B8A8_UNORM;
				case NLS::Render::RHI::TextureFormat::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
				case NLS::Render::RHI::TextureFormat::Depth24Stencil8: return VK_FORMAT_D24_UNORM_S8_UINT;
				default: return VK_FORMAT_R8G8B8A8_UNORM;
				}
			}
#endif

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
			const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override { return m_texture; }
			NLS::Render::RHI::NativeHandle GetNativeShaderResourceView() override { return { NLS::Render::RHI::BackendType::Vulkan, reinterpret_cast<void*>(m_imageView) }; }

#if NLS_HAS_VULKAN
			VkImageView GetImageView() const { return m_imageView; }
#endif

		private:
			VkDevice m_device = nullptr;
			std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
			NLS::Render::RHI::RHITextureViewDesc m_desc{};
#if NLS_HAS_VULKAN
			VkImageView m_imageView = VK_NULL_HANDLE;
#endif
		};

		class NativeVulkanSampler final : public NLS::Render::RHI::RHISampler
		{
		public:
			NativeVulkanSampler(VkDevice device, const NLS::Render::RHI::SamplerDesc& desc, const std::string& debugName)
				: m_desc(desc)
				, m_debugName(debugName)
			{
#if NLS_HAS_VULKAN
				if (device == nullptr)
					return;

				VkSamplerCreateInfo samplerInfo{};
				samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
				samplerInfo.magFilter = ToVkFilter(desc.minFilter);
				samplerInfo.minFilter = ToVkFilter(desc.minFilter);
				samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
				samplerInfo.addressModeU = ToVkSamplerAddressMode(desc.wrapU);
				samplerInfo.addressModeV = ToVkSamplerAddressMode(desc.wrapV);
				samplerInfo.addressModeW = ToVkSamplerAddressMode(desc.wrapW);
				samplerInfo.mipLodBias = 0;
				samplerInfo.maxAnisotropy = 1.0f;
				samplerInfo.compareEnable = VK_FALSE;
				samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
				samplerInfo.minLod = 0;
				samplerInfo.maxLod = VK_LOD_CLAMP_NONE;

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
					VkDescriptorSetLayoutBinding binding{};
					binding.binding = entry.binding;
					binding.descriptorCount = entry.count;
					binding.stageFlags = ToVkShaderStageFlags(entry.stageMask);

					switch (entry.type)
					{
					case NLS::Render::RHI::BindingType::UniformBuffer:
						binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
						break;
					case NLS::Render::RHI::BindingType::StorageBuffer:
						binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
						break;
					case NLS::Render::RHI::BindingType::Texture:
						binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
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

					vkBindings.push_back(binding);
				}

				VkDescriptorSetLayoutCreateInfo layoutInfo{};
				layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
				layoutInfo.bindingCount = static_cast<uint32_t>(vkBindings.size());
				layoutInfo.pBindings = vkBindings.data();

				vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout);
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
#endif

		private:
			VkDevice m_device = nullptr;
			NLS::Render::RHI::RHIBindingLayoutDesc m_desc;
#if NLS_HAS_VULKAN
			VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
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

				// Calculate pool sizes based on actual binding types
				std::vector<VkDescriptorPoolSize> poolSizes;
				for (const auto& entry : m_desc.entries)
				{
					VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; // Default
					switch (entry.type)
					{
					case NLS::Render::RHI::BindingType::UniformBuffer:
						type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
						break;
					case NLS::Render::RHI::BindingType::StorageBuffer:
						type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
						break;
					case NLS::Render::RHI::BindingType::Texture:
						type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
						break;
					case NLS::Render::RHI::BindingType::Sampler:
						type = VK_DESCRIPTOR_TYPE_SAMPLER;
						break;
					}

					// Check if we already have this type
					bool found = false;
					for (auto& ps : poolSizes)
					{
						if (ps.type == type)
						{
							ps.descriptorCount += 1;
							found = true;
							break;
						}
					}
					if (!found)
					{
						poolSizes.push_back({ type, 1 });
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

				std::vector<VkWriteDescriptorSet> writes;
				writes.reserve(m_desc.entries.size());

				for (const auto& entry : m_desc.entries)
				{
					VkWriteDescriptorSet write{};
					write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
					write.dstSet = m_descriptorSet;
					write.dstBinding = entry.binding;
					write.descriptorCount = 1;

					switch (entry.type)
					{
					case NLS::Render::RHI::BindingType::UniformBuffer:
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
							write.pBufferInfo = &bufferInfo;
							writes.push_back(write);
						}
						break;
					case NLS::Render::RHI::BindingType::Texture:
						if (entry.textureView != nullptr)
						{
							write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
							VkDescriptorImageInfo imageInfo{};
							auto srvHandle = entry.textureView->GetNativeShaderResourceView();
							imageInfo.imageView = (srvHandle.backend == NLS::Render::RHI::BackendType::Vulkan) ? static_cast<VkImageView>(srvHandle.handle) : VK_NULL_HANDLE;
							if (entry.sampler != nullptr)
							{
								auto smpHandle = entry.sampler->GetNativeSamplerHandle();
								imageInfo.sampler = (smpHandle.backend == NLS::Render::RHI::BackendType::Vulkan) ? static_cast<VkSampler>(smpHandle.handle) : VK_NULL_HANDLE;
							}
							imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
							write.pImageInfo = &imageInfo;
							writes.push_back(write);
						}
						break;
					case NLS::Render::RHI::BindingType::RWTexture:
						if (entry.textureView != nullptr)
						{
							write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
							VkDescriptorImageInfo imageInfo{};
							imageInfo.imageView = reinterpret_cast<VkImageView>(entry.textureView->GetNativeShaderResourceView().handle);
							imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
							write.pImageInfo = &imageInfo;
							writes.push_back(write);
						}
						break;
					default:
						break;
					}
				}

				if (!writes.empty())
					vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
#endif
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHIBindingSetDesc& GetDesc() const override { return m_desc; }

#if NLS_HAS_VULKAN
			VkDescriptorSet GetDescriptorSet() const { return m_descriptorSet; }
#endif

		private:
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

				std::vector<VkDescriptorSetLayout> setLayouts;
				setLayouts.reserve(m_desc.bindingLayouts.size());
				for (const auto& layout : m_desc.bindingLayouts)
				{
					if (layout != nullptr)
					{
						auto* nativeLayout = dynamic_cast<NativeVulkanBindingLayout*>(layout.get());
						if (nativeLayout != nullptr)
							setLayouts.push_back(nativeLayout->GetDescriptorSetLayout());
					}
				}

				std::vector<VkPushConstantRange> pushConstantRanges;
				pushConstantRanges.reserve(m_desc.pushConstants.size());
				for (const auto& pc : m_desc.pushConstants)
				{
					VkPushConstantRange range{};
					range.stageFlags = ToVkShaderStageFlags(pc.stageMask);
					range.offset = pc.offset;
					range.size = pc.size;
					pushConstantRanges.push_back(range);
				}

				VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
				pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
				pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
				pipelineLayoutInfo.pSetLayouts = setLayouts.data();
				pipelineLayoutInfo.pushConstantRangeCount = static_cast<uint32_t>(pushConstantRanges.size());
				pipelineLayoutInfo.pPushConstantRanges = pushConstantRanges.data();

				vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout);
#endif
			}

			~NativeVulkanPipelineLayout()
			{
#if NLS_HAS_VULKAN
				if (m_device != nullptr && m_pipelineLayout != VK_NULL_HANDLE)
					vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
#endif
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHIPipelineLayoutDesc& GetDesc() const override { return m_desc; }

#if NLS_HAS_VULKAN
			VkPipelineLayout GetPipelineLayout() const { return m_pipelineLayout; }
#endif

		private:
			VkDevice m_device = nullptr;
			NLS::Render::RHI::RHIPipelineLayoutDesc m_desc;
#if NLS_HAS_VULKAN
			VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
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

			// Extract and store the pipeline layout
			if (pipeline->GetDesc().pipelineLayout != nullptr)
			{
				auto* nativePipelineLayout = dynamic_cast<NativeVulkanPipelineLayout*>(pipeline->GetDesc().pipelineLayout.get());
				if (nativePipelineLayout != nullptr)
					m_boundPipelineLayout = nativePipelineLayout->GetPipelineLayout();
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

			auto* nativeBindingSet = dynamic_cast<NativeVulkanBindingSet*>(bindingSet.get());
			if (nativeBindingSet == nullptr || m_boundPipelineLayout == VK_NULL_HANDLE)
				return;

			VkDescriptorSet vkDescriptorSet = nativeBindingSet->GetDescriptorSet();
			if (vkDescriptorSet == VK_NULL_HANDLE)
			{
				return;
			}
			vkCmdBindDescriptorSets(
				m_commandBuffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				m_boundPipelineLayout,
				setIndex,
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
			explicit NativeVulkanGraphicsPipeline(VkDevice device, NLS::Render::RHI::RHIGraphicsPipelineDesc desc)
				: m_device(device)
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
					attr.format = VK_FORMAT_R32G32B32A32_SFLOAT; // TODO: derive from elementSize
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
				VkCullModeFlags vkCullMode = m_desc.rasterState.cullEnabled ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
				VkFrontFace vkFrontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
				switch (m_desc.rasterState.cullFace)
				{
				case NLS::Render::Settings::ECullFace::FRONT: vkCullMode = VK_CULL_MODE_FRONT_BIT; break;
				case NLS::Render::Settings::ECullFace::BACK: vkCullMode = VK_CULL_MODE_BACK_BIT; break;
				case NLS::Render::Settings::ECullFace::FRONT_AND_BACK: vkCullMode = VK_CULL_MODE_FRONT_AND_BACK; break;
				}

				VkPipelineRasterizationStateCreateInfo rasterizer{};
				rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
				rasterizer.depthClampEnable = VK_FALSE;
				rasterizer.rasterizerDiscardEnable = VK_FALSE;
				rasterizer.polygonMode = m_desc.rasterState.wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
				rasterizer.cullMode = vkCullMode;
				rasterizer.frontFace = vkFrontFace;
				rasterizer.depthBiasEnable = VK_FALSE;

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

				// Color blend state
				VkPipelineColorBlendAttachmentState colorBlendAttachment{};
				colorBlendAttachment.colorWriteMask = m_desc.blendState.colorWrite ? 0xF : 0x0;
				colorBlendAttachment.blendEnable = m_desc.blendState.enabled ? VK_TRUE : VK_FALSE;
				colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
				colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
				colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

				VkPipelineColorBlendStateCreateInfo colorBlending{};
				colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
				colorBlending.logicOpEnable = VK_FALSE;
				colorBlending.attachmentCount = 1;
				colorBlending.pAttachments = &colorBlendAttachment;

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
				case NLS::Render::RHI::TextureFormat::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
				case NLS::Render::RHI::TextureFormat::RGB8: return VK_FORMAT_R8G8B8_UNORM;
				case NLS::Render::RHI::TextureFormat::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
				case NLS::Render::RHI::TextureFormat::Depth24Stencil8: return VK_FORMAT_D24_UNORM_S8_UINT;
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
				for (size_t i = 0; i < m_desc.renderTargetLayout.colorFormats.size(); ++i)
				{
					VkAttachmentDescription colorAtt{};
					colorAtt.format = ToVkFormat(m_desc.renderTargetLayout.colorFormats[i]);
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
				bool hasDepth = m_desc.renderTargetLayout.hasDepth;
				if (hasDepth)
				{
					VkAttachmentDescription depthAtt{};
					depthAtt.format = ToVkFormat(m_desc.renderTargetLayout.depthFormat);
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
			NLS::Render::RHI::RHIGraphicsPipelineDesc m_desc;
#if NLS_HAS_VULKAN
			VkPipeline m_pipeline = VK_NULL_HANDLE;
#endif
		};

		class NativeVulkanComputePipeline final : public NLS::Render::RHI::RHIComputePipeline
		{
		public:
			explicit NativeVulkanComputePipeline(NLS::Render::RHI::RHIComputePipelineDesc desc)
				: m_desc(std::move(desc))
			{
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHIComputePipelineDesc& GetDesc() const override { return m_desc; }

		private:
			NLS::Render::RHI::RHIComputePipelineDesc m_desc;
		};

		class NativeVulkanExplicitDevice final : public NLS::Render::RHI::RHIDevice
		{
		public:
			~NativeVulkanExplicitDevice() { DestroyUIResources(); }

			NativeVulkanExplicitDevice(
				VkInstance instance,
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
				, m_physicalDevice(physicalDevice)
				, m_device(device)
				, m_graphicsQueue(graphicsQueue)
				, m_surface(surface)
				, m_swapchain(swapchain)
				, m_graphicsQueueFamilyIndex(graphicsQueueFamilyIndex)
				, m_capabilities(capabilities)
				, m_rhiAdapter(std::make_shared<NativeVulkanAdapter>(vendor, hardware))
				, m_dynamicRenderingEnabled(dynamicRenderingEnabled)
			{
				CreateUIResources();
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
			bool IsBackendReady() const override { return m_device != nullptr; }

			std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType queueType) override
			{
				const auto queueIndex = static_cast<size_t>(queueType);
				if (m_queues[queueIndex] == nullptr)
					m_queues[queueIndex] = std::make_shared<NativeVulkanQueue>(m_device, m_graphicsQueue, "GraphicsQueue");
				return m_queues[queueIndex];
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
				VkSurfaceFormatKHR selectedFormat = formats[0];

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
				createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
				createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
				createInfo.preTransform = surfaceCapabilities.currentTransform;
				createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
				createInfo.presentMode = presentMode;
				createInfo.clipped = VK_TRUE;
				createInfo.oldSwapchain = VK_NULL_HANDLE;

				VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
				if (vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &newSwapchain) != VK_SUCCESS)
					return nullptr;

				// Update stored swapchain
				m_swapchain = newSwapchain;

				// Create SwapchainDesc for the NativeVulkanSwapchain
				NLS::Render::RHI::SwapchainDesc swapchainDesc = desc;
				swapchainDesc.width = extent.width;
				swapchainDesc.height = extent.height;
				swapchainDesc.imageCount = imageCount;

				auto swapchain = std::make_shared<NativeVulkanSwapchain>(m_device, newSwapchain, m_surface, swapchainDesc);
				swapchain->SetPhysicalDevice(m_physicalDevice);

				return swapchain;
#else
				return nullptr;
#endif
			}

			std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(const NLS::Render::RHI::RHIBufferDesc& desc, const void* initialData) override;
			std::shared_ptr<NLS::Render::RHI::RHITexture> CreateTexture(const NLS::Render::RHI::RHITextureDesc& desc, const void* initialData) override;
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

		private:
			VkInstance m_instance = nullptr;
			VkPhysicalDevice m_physicalDevice = nullptr;
			VkDevice m_device = nullptr;
			VkQueue m_graphicsQueue = nullptr;
			VkSurfaceKHR m_surface = nullptr;
			VkSwapchainKHR m_swapchain = nullptr;
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

			// From RHIDevice
			void SetCurrentCommandBuffer(void* commandBuffer) override { m_currentCommandBuffer = commandBuffer; }
		};

		void NativeVulkanExplicitDevice::CreateUIResources()
		{
#if NLS_HAS_VULKAN && NLS_HAS_IMGUI_VULKAN_BACKEND
			if (m_device == nullptr)
				return;

			// Create UI render pass
			const VkAttachmentDescription colorAttachment{
				0,
				VK_FORMAT_B8G8R8A8_UNORM,  // Default format, matching swapchain
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
		std::shared_ptr<NLS::Render::RHI::RHIBuffer> NativeVulkanExplicitDevice::CreateBuffer(const NLS::Render::RHI::RHIBufferDesc& desc, const void* initialData)
		{
			if (m_device == nullptr)
				return nullptr;
			return std::make_shared<NativeVulkanBuffer>(m_device, m_physicalDevice, desc, initialData);
		}

		std::shared_ptr<NLS::Render::RHI::RHITexture> NativeVulkanExplicitDevice::CreateTexture(const NLS::Render::RHI::RHITextureDesc& desc, const void* initialData)
		{
#if NLS_HAS_VULKAN
			if (m_device == nullptr)
				return nullptr;
			return std::make_shared<NativeVulkanTexture>(m_device, m_physicalDevice, desc, initialData);
#else
			return nullptr;
#endif
		}

		std::shared_ptr<NLS::Render::RHI::RHITextureView> NativeVulkanExplicitDevice::CreateTextureView(const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture, const NLS::Render::RHI::RHITextureViewDesc& desc)
		{
#if NLS_HAS_VULKAN
			if (texture == nullptr)
				return nullptr;
			return std::make_shared<NativeVulkanTextureView>(m_device, texture, desc);
#else
			return nullptr;
#endif
		}

		std::shared_ptr<NLS::Render::RHI::RHISampler> NativeVulkanExplicitDevice::CreateSampler(const NLS::Render::RHI::SamplerDesc& desc, std::string debugName)
		{
#if NLS_HAS_VULKAN
			if (m_device == nullptr)
				return nullptr;
			return std::make_shared<NativeVulkanSampler>(m_device, desc, debugName.empty() ? "Sampler" : debugName);
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
			return std::make_shared<NativeVulkanGraphicsPipeline>(m_device, desc);
		}

		std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> NativeVulkanExplicitDevice::CreateComputePipeline(const NLS::Render::RHI::RHIComputePipelineDesc& desc)
		{
			return std::make_shared<NativeVulkanComputePipeline>(desc);
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
			if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &readbackBuffer) != VK_SUCCESS)
				return;

			VkMemoryRequirements memoryRequirements{};
			vkGetBufferMemoryRequirements(m_device, readbackBuffer, &memoryRequirements);
			const VkMemoryAllocateInfo allocationInfo{
				VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				nullptr,
				memoryRequirements.size,
				FindMemoryType(m_physicalDevice, memoryRequirements, NLS::Render::RHI::MemoryUsage::GPUToCPU)
			};
			if (vkAllocateMemory(m_device, &allocationInfo, nullptr, &readbackMemory) != VK_SUCCESS)
			{
				vkDestroyBuffer(m_device, readbackBuffer, nullptr);
				return;
			}
			vkBindBufferMemory(m_device, readbackBuffer, readbackMemory, 0);

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
				vkDestroyBuffer(m_device, readbackBuffer, nullptr);
				vkFreeMemory(m_device, readbackMemory, nullptr);
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
				vkDestroyBuffer(m_device, readbackBuffer, nullptr);
				vkFreeMemory(m_device, readbackMemory, nullptr);
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
				vkDestroyBuffer(m_device, readbackBuffer, nullptr);
				vkFreeMemory(m_device, readbackMemory, nullptr);
				return;
			}

			// Transition image to TRANSFER_SRC
			const VkImageMemoryBarrier transitionBarrier{
				VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				nullptr,
				0,  // srcAccessMask
				VK_ACCESS_TRANSFER_READ_BIT,  // dstAccessMask
				VK_IMAGE_LAYOUT_UNDEFINED,  // oldLayout
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

			if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
			{
				vkFreeCommandBuffers(m_device, commandPool, 1, &commandBuffer);
				vkDestroyCommandPool(m_device, commandPool, nullptr);
				vkDestroyBuffer(m_device, readbackBuffer, nullptr);
				vkFreeMemory(m_device, readbackMemory, nullptr);
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
				vkDestroyBuffer(m_device, readbackBuffer, nullptr);
				vkFreeMemory(m_device, readbackMemory, nullptr);
				return;
			}

			// Wait for completion
			vkQueueWaitIdle(m_graphicsQueue);

			// Map memory and copy data
			void* mappedData = nullptr;
			if (vkMapMemory(m_device, readbackMemory, 0, readbackSize, 0, &mappedData) == VK_SUCCESS)
			{
				if (format == NLS::Render::Settings::EPixelDataFormat::RGB && bytesPerPixel >= 3)
				{
					// RGB format: expand from source format to RGB
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
				vkUnmapMemory(m_device, readbackMemory);
			}

			// Cleanup
			vkFreeCommandBuffers(m_device, commandPool, 1, &commandBuffer);
			vkDestroyCommandPool(m_device, commandPool, nullptr);
			vkDestroyBuffer(m_device, readbackBuffer, nullptr);
			vkFreeMemory(m_device, readbackMemory, nullptr);
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

		VkInstance instance = VK_NULL_HANDLE;
		if (vkCreateInstance(&instanceCreateInfo, nullptr, &instance) != VK_SUCCESS)
		{
			NLS_LOG_ERROR("CreateVulkanRhiDevice: failed to create Vulkan instance");
			return nullptr;
		}

		// Create surface from window
		VkSurfaceKHR surface = VK_NULL_HANDLE;
		GLFWwindow* window = static_cast<GLFWwindow*>(platformWindow);
		if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS)
		{
			NLS_LOG_ERROR("CreateVulkanRhiDevice: failed to create Vulkan surface");
			vkDestroyInstance(instance, nullptr);
			return nullptr;
		}

		// Enumerate and select physical device
		uint32_t physicalDeviceCount = 0;
		if (vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, nullptr) != VK_SUCCESS || physicalDeviceCount == 0)
		{
			NLS_LOG_ERROR("CreateVulkanRhiDevice: failed to enumerate physical devices");
			vkDestroySurfaceKHR(instance, surface, nullptr);
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

			for (uint32_t queueFamilyIndex = 0; queueFamilyIndex < queueFamilyCount; ++queueFamilyIndex)
			{
				if ((queueFamilies[queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
				{
					physicalDevice = device;
					graphicsQueueFamilyIndex = queueFamilyIndex;
					break;
				}
			}

			if (physicalDevice != VK_NULL_HANDLE)
				break;
		}

		if (physicalDevice == VK_NULL_HANDLE)
		{
			NLS_LOG_ERROR("CreateVulkanRhiDevice: failed to find a Vulkan graphics queue family");
			vkDestroySurfaceKHR(instance, surface, nullptr);
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

		// Enable VK_KHR_dynamic_rendering for render passless rendering
		constexpr const char* deviceExtensions[] = {
			VK_KHR_SWAPCHAIN_EXTENSION_NAME,
			VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME
		};

		// Check if device supports dynamic rendering
		VkPhysicalDeviceDynamicRenderingFeatures dynamicRenderingFeatures{
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
			nullptr,
			VK_FALSE  // will be set to VK_TRUE if supported
		};

		VkPhysicalDeviceFeatures2 deviceFeatures2{
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
			&dynamicRenderingFeatures,
			{}
		};

		// Query dynamic rendering support
		vkGetPhysicalDeviceFeatures2(physicalDevice, &deviceFeatures2);

		const VkDeviceCreateInfo deviceCreateInfo{
			VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			dynamicRenderingFeatures.dynamicRendering ? &dynamicRenderingFeatures : nullptr,
			0,
			1,
			&queueCreateInfo,
			0,
			nullptr,
			static_cast<uint32_t>(dynamicRenderingFeatures.dynamicRendering ? 2 : 1),
			deviceExtensions,
			nullptr
		};

		VkDevice device = VK_NULL_HANDLE;
		if (vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &device) != VK_SUCCESS)
		{
			NLS_LOG_ERROR("CreateVulkanRhiDevice: failed to create Vulkan logical device");
			vkDestroySurfaceKHR(instance, surface, nullptr);
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
		capabilities.supportsCompute = false;
		capabilities.supportsSwapchain = true;
		capabilities.supportsFramebufferReadback = true;
		capabilities.supportsEditorPickingReadback = true;
		capabilities.maxTextureDimension2D = 4096;  // Conservative default
		capabilities.maxColorAttachments = 8;

		// Get vendor/hardware info
		VkPhysicalDeviceProperties properties;
		vkGetPhysicalDeviceProperties(physicalDevice, &properties);
		std::string vendor = "Unknown";
		std::string hardware = std::string(properties.deviceName);

		NLS_LOG_INFO("CreateVulkanRhiDevice: created Vulkan device directly, vendor=" + vendor + ", device=" + hardware);

		return std::make_shared<NativeVulkanExplicitDevice>(
			instance,
			physicalDevice,
			device,
			graphicsQueue,
			surface,
			VK_NULL_HANDLE,  // No initial swapchain, created later via CreateSwapchain
			graphicsQueueFamilyIndex,
			capabilities,
			vendor,
			hardware,
			dynamicRenderingFeatures.dynamicRendering == VK_TRUE);
	}
#else
	std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateVulkanRhiDevice(void* /*platformWindow*/)
	{
		NLS_LOG_WARNING("CreateVulkanRhiDevice: Vulkan not available at build time");
		return nullptr;
	}
#endif
}
