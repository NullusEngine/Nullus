#include "Rendering/RHI/Backends/Vulkan/VulkanExplicitDeviceFactory.h"

#include "Rendering/RHI/Backends/OpenGL/Compat/ExplicitRHICompat.h"
#include "Rendering/RHI/IRenderDevice.h"

namespace NLS::Render::Backend
{
	namespace
	{
		class VulkanFence final : public NLS::Render::RHI::RHIFence
		{
		public:
			explicit VulkanFence(std::shared_ptr<NLS::Render::RHI::RHIFence> innerFence)
				: m_innerFence(std::move(innerFence))
			{
			}

			std::string_view GetDebugName() const override { return m_innerFence->GetDebugName(); }
			bool IsSignaled() const override { return m_innerFence->IsSignaled(); }
			void Reset() override { m_innerFence->Reset(); }
			bool Wait(uint64_t timeoutNanoseconds) override { return m_innerFence->Wait(timeoutNanoseconds); }
			const std::shared_ptr<NLS::Render::RHI::RHIFence>& GetInnerFence() const { return m_innerFence; }

		private:
			std::shared_ptr<NLS::Render::RHI::RHIFence> m_innerFence;
		};

		class VulkanSemaphore final : public NLS::Render::RHI::RHISemaphore
		{
		public:
			explicit VulkanSemaphore(std::shared_ptr<NLS::Render::RHI::RHISemaphore> innerSemaphore)
				: m_innerSemaphore(std::move(innerSemaphore))
			{
			}

			std::string_view GetDebugName() const override { return m_innerSemaphore->GetDebugName(); }
			bool IsSignaled() const override { return m_innerSemaphore->IsSignaled(); }
			void Reset() override { m_innerSemaphore->Reset(); }
			const std::shared_ptr<NLS::Render::RHI::RHISemaphore>& GetInnerSemaphore() const { return m_innerSemaphore; }

		private:
			std::shared_ptr<NLS::Render::RHI::RHISemaphore> m_innerSemaphore;
		};

		class VulkanCommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
		{
		public:
			explicit VulkanCommandBuffer(std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> innerCommandBuffer)
				: m_innerCommandBuffer(std::move(innerCommandBuffer))
			{
			}

			std::string_view GetDebugName() const override { return m_innerCommandBuffer->GetDebugName(); }
			void Begin() override { m_innerCommandBuffer->Begin(); }
			void End() override { m_innerCommandBuffer->End(); }
			void Reset() override { m_innerCommandBuffer->Reset(); }
			bool IsRecording() const override { return m_innerCommandBuffer->IsRecording(); }
			void BeginRenderPass(const NLS::Render::RHI::RHIRenderPassDesc& desc) override { m_innerCommandBuffer->BeginRenderPass(desc); }
			void EndRenderPass() override { m_innerCommandBuffer->EndRenderPass(); }
			void SetViewport(const NLS::Render::RHI::RHIViewport& viewport) override { m_innerCommandBuffer->SetViewport(viewport); }
			void SetScissor(const NLS::Render::RHI::RHIRect2D& rect) override { m_innerCommandBuffer->SetScissor(rect); }
			void BindGraphicsPipeline(const std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>& pipeline) override { m_innerCommandBuffer->BindGraphicsPipeline(pipeline); }
			void BindComputePipeline(const std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>& pipeline) override { m_innerCommandBuffer->BindComputePipeline(pipeline); }
			void BindBindingSet(uint32_t setIndex, const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& bindingSet) override { m_innerCommandBuffer->BindBindingSet(setIndex, bindingSet); }
			void PushConstants(NLS::Render::RHI::ShaderStageMask stageMask, uint32_t offset, uint32_t size, const void* data) override { m_innerCommandBuffer->PushConstants(stageMask, offset, size, data); }
			void BindVertexBuffer(uint32_t slot, const NLS::Render::RHI::RHIVertexBufferView& view) override { m_innerCommandBuffer->BindVertexBuffer(slot, view); }
			void BindIndexBuffer(const NLS::Render::RHI::RHIIndexBufferView& view) override { m_innerCommandBuffer->BindIndexBuffer(view); }
			void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override { m_innerCommandBuffer->Draw(vertexCount, instanceCount, firstVertex, firstInstance); }
			void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override { m_innerCommandBuffer->DrawIndexed(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance); }
			void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override { m_innerCommandBuffer->Dispatch(groupCountX, groupCountY, groupCountZ); }
			void CopyBuffer(const std::shared_ptr<NLS::Render::RHI::RHIBuffer>& source, const std::shared_ptr<NLS::Render::RHI::RHIBuffer>& destination, const NLS::Render::RHI::RHIBufferCopyRegion& region) override { m_innerCommandBuffer->CopyBuffer(source, destination, region); }
			void CopyBufferToTexture(const NLS::Render::RHI::RHIBufferToTextureCopyDesc& desc) override { m_innerCommandBuffer->CopyBufferToTexture(desc); }
			void CopyTexture(const NLS::Render::RHI::RHITextureCopyDesc& desc) override { m_innerCommandBuffer->CopyTexture(desc); }
			void Barrier(const NLS::Render::RHI::RHIBarrierDesc& barrier) override { m_innerCommandBuffer->Barrier(barrier); }
			const std::shared_ptr<NLS::Render::RHI::RHICommandBuffer>& GetInnerCommandBuffer() const { return m_innerCommandBuffer; }

		private:
			std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> m_innerCommandBuffer;
		};

		class VulkanCommandPool final : public NLS::Render::RHI::RHICommandPool
		{
		public:
			explicit VulkanCommandPool(std::shared_ptr<NLS::Render::RHI::RHICommandPool> innerCommandPool)
				: m_innerCommandPool(std::move(innerCommandPool))
			{
			}

			std::string_view GetDebugName() const override { return m_innerCommandPool->GetDebugName(); }
			NLS::Render::RHI::QueueType GetQueueType() const override { return m_innerCommandPool->GetQueueType(); }
			std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> CreateCommandBuffer(std::string debugName) override
			{
				return std::make_shared<VulkanCommandBuffer>(m_innerCommandPool->CreateCommandBuffer(std::move(debugName)));
			}
			void Reset() override { m_innerCommandPool->Reset(); }

		private:
			std::shared_ptr<NLS::Render::RHI::RHICommandPool> m_innerCommandPool;
		};

		class VulkanSwapchain final : public NLS::Render::RHI::RHISwapchain
		{
		public:
			explicit VulkanSwapchain(std::shared_ptr<NLS::Render::RHI::RHISwapchain> innerSwapchain)
				: m_innerSwapchain(std::move(innerSwapchain))
			{
			}

			std::string_view GetDebugName() const override { return m_innerSwapchain->GetDebugName(); }
			const NLS::Render::RHI::SwapchainDesc& GetDesc() const override { return m_innerSwapchain->GetDesc(); }
			uint32_t GetImageCount() const override { return m_innerSwapchain->GetImageCount(); }
			std::optional<NLS::Render::RHI::RHIAcquiredImage> AcquireNextImage(
				const std::shared_ptr<NLS::Render::RHI::RHISemaphore>& signalSemaphore,
				const std::shared_ptr<NLS::Render::RHI::RHIFence>& signalFence) override
			{
				const auto vulkanSemaphore = std::dynamic_pointer_cast<VulkanSemaphore>(signalSemaphore);
				const auto vulkanFence = std::dynamic_pointer_cast<VulkanFence>(signalFence);
				return m_innerSwapchain->AcquireNextImage(
					vulkanSemaphore != nullptr ? vulkanSemaphore->GetInnerSemaphore() : signalSemaphore,
					vulkanFence != nullptr ? vulkanFence->GetInnerFence() : signalFence);
			}
			void Resize(uint32_t width, uint32_t height) override { m_innerSwapchain->Resize(width, height); }
			const std::shared_ptr<NLS::Render::RHI::RHISwapchain>& GetInnerSwapchain() const { return m_innerSwapchain; }

		private:
			std::shared_ptr<NLS::Render::RHI::RHISwapchain> m_innerSwapchain;
		};

		class VulkanQueue final : public NLS::Render::RHI::RHIQueue
		{
		public:
			explicit VulkanQueue(std::shared_ptr<NLS::Render::RHI::RHIQueue> innerQueue)
				: m_innerQueue(std::move(innerQueue))
			{
			}

			std::string_view GetDebugName() const override { return m_innerQueue->GetDebugName(); }
			NLS::Render::RHI::QueueType GetType() const override { return m_innerQueue->GetType(); }
			void Submit(const NLS::Render::RHI::RHISubmitDesc& submitDesc) override
			{
				NLS::Render::RHI::RHISubmitDesc innerDesc{};
				innerDesc.commandBuffers.reserve(submitDesc.commandBuffers.size());
				for (const auto& commandBuffer : submitDesc.commandBuffers)
				{
					if (const auto vulkanCommandBuffer = std::dynamic_pointer_cast<VulkanCommandBuffer>(commandBuffer))
						innerDesc.commandBuffers.push_back(vulkanCommandBuffer->GetInnerCommandBuffer());
					else
						innerDesc.commandBuffers.push_back(commandBuffer);
				}
				innerDesc.waitSemaphores.reserve(submitDesc.waitSemaphores.size());
				for (const auto& semaphore : submitDesc.waitSemaphores)
				{
					if (const auto vulkanSemaphore = std::dynamic_pointer_cast<VulkanSemaphore>(semaphore))
						innerDesc.waitSemaphores.push_back(vulkanSemaphore->GetInnerSemaphore());
					else
						innerDesc.waitSemaphores.push_back(semaphore);
				}
				innerDesc.signalSemaphores.reserve(submitDesc.signalSemaphores.size());
				for (const auto& semaphore : submitDesc.signalSemaphores)
				{
					if (const auto vulkanSemaphore = std::dynamic_pointer_cast<VulkanSemaphore>(semaphore))
						innerDesc.signalSemaphores.push_back(vulkanSemaphore->GetInnerSemaphore());
					else
						innerDesc.signalSemaphores.push_back(semaphore);
				}
				if (const auto vulkanFence = std::dynamic_pointer_cast<VulkanFence>(submitDesc.signalFence))
					innerDesc.signalFence = vulkanFence->GetInnerFence();
				else
					innerDesc.signalFence = submitDesc.signalFence;
				m_innerQueue->Submit(innerDesc);
			}
			void Present(const NLS::Render::RHI::RHIPresentDesc& presentDesc) override
			{
				NLS::Render::RHI::RHIPresentDesc innerDesc{};
				innerDesc.imageIndex = presentDesc.imageIndex;
				if (const auto vulkanSwapchain = std::dynamic_pointer_cast<VulkanSwapchain>(presentDesc.swapchain))
					innerDesc.swapchain = vulkanSwapchain->GetInnerSwapchain();
				else
					innerDesc.swapchain = presentDesc.swapchain;
				innerDesc.waitSemaphores.reserve(presentDesc.waitSemaphores.size());
				for (const auto& semaphore : presentDesc.waitSemaphores)
				{
					if (const auto vulkanSemaphore = std::dynamic_pointer_cast<VulkanSemaphore>(semaphore))
						innerDesc.waitSemaphores.push_back(vulkanSemaphore->GetInnerSemaphore());
					else
						innerDesc.waitSemaphores.push_back(semaphore);
				}
				m_innerQueue->Present(innerDesc);
			}

		private:
			std::shared_ptr<NLS::Render::RHI::RHIQueue> m_innerQueue;
		};

		class VulkanAdapter final : public NLS::Render::RHI::RHIAdapter
		{
		public:
			explicit VulkanAdapter(NLS::Render::RHI::IRenderDevice& renderDevice)
				: m_vendor(renderDevice.GetVendor())
				, m_hardware(renderDevice.GetHardware())
			{
			}

			std::string_view GetDebugName() const override { return "VulkanAdapter"; }
			NLS::Render::RHI::NativeBackendType GetBackendType() const override { return NLS::Render::RHI::NativeBackendType::Vulkan; }
			std::string_view GetVendor() const override { return m_vendor; }
			std::string_view GetHardware() const override { return m_hardware; }

		private:
			std::string m_vendor;
			std::string m_hardware;
		};

		class VulkanExplicitDevice final : public NLS::Render::RHI::RHIDevice
		{
		public:
			explicit VulkanExplicitDevice(NLS::Render::RHI::IRenderDevice& renderDevice)
				: m_nativeInfo(renderDevice.GetNativeDeviceInfo())
				, m_capabilities(renderDevice.GetCapabilities())
				, m_adapter(std::make_shared<VulkanAdapter>(renderDevice))
				, m_innerDevice(NLS::Render::RHI::CreateCompatibilityExplicitDevice(renderDevice))
			{
			}

			std::string_view GetDebugName() const override { return "VulkanExplicitDevice"; }
			const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_adapter; }
			const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
			NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override { return m_nativeInfo; }
			bool IsBackendReady() const override { return m_innerDevice != nullptr && m_innerDevice->IsBackendReady(); }
			std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType queueType) override
			{
				const auto queueIndex = static_cast<size_t>(queueType);
				if (m_queues[queueIndex] == nullptr)
					m_queues[queueIndex] = std::make_shared<VulkanQueue>(m_innerDevice->GetQueue(queueType));
				return m_queues[queueIndex];
			}
			std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(const NLS::Render::RHI::SwapchainDesc& desc) override
			{
				return std::make_shared<VulkanSwapchain>(m_innerDevice->CreateSwapchain(desc));
			}
			std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(const NLS::Render::RHI::RHIBufferDesc& desc, const void* initialData) override { return m_innerDevice->CreateBuffer(desc, initialData); }
			std::shared_ptr<NLS::Render::RHI::RHITexture> CreateTexture(const NLS::Render::RHI::RHITextureDesc& desc, const void* initialData) override { return m_innerDevice->CreateTexture(desc, initialData); }
			std::shared_ptr<NLS::Render::RHI::RHITextureView> CreateTextureView(const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture, const NLS::Render::RHI::RHITextureViewDesc& desc) override { return m_innerDevice->CreateTextureView(texture, desc); }
			std::shared_ptr<NLS::Render::RHI::RHISampler> CreateSampler(const NLS::Render::RHI::SamplerDesc& desc, std::string debugName) override { return m_innerDevice->CreateSampler(desc, std::move(debugName)); }
			std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateBindingLayout(const NLS::Render::RHI::RHIBindingLayoutDesc& desc) override { return m_innerDevice->CreateBindingLayout(desc); }
			std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateBindingSet(const NLS::Render::RHI::RHIBindingSetDesc& desc) override { return m_innerDevice->CreateBindingSet(desc); }
			std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> CreatePipelineLayout(const NLS::Render::RHI::RHIPipelineLayoutDesc& desc) override { return m_innerDevice->CreatePipelineLayout(desc); }
			std::shared_ptr<NLS::Render::RHI::RHIShaderModule> CreateShaderModule(const NLS::Render::RHI::RHIShaderModuleDesc& desc) override { return m_innerDevice->CreateShaderModule(desc); }
			std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> CreateGraphicsPipeline(const NLS::Render::RHI::RHIGraphicsPipelineDesc& desc) override { return m_innerDevice->CreateGraphicsPipeline(desc); }
			std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> CreateComputePipeline(const NLS::Render::RHI::RHIComputePipelineDesc& desc) override { return m_innerDevice->CreateComputePipeline(desc); }
			std::shared_ptr<NLS::Render::RHI::RHICommandPool> CreateCommandPool(NLS::Render::RHI::QueueType queueType, std::string debugName) override
			{
				return std::make_shared<VulkanCommandPool>(m_innerDevice->CreateCommandPool(queueType, std::move(debugName)));
			}
			std::shared_ptr<NLS::Render::RHI::RHIFence> CreateFence(std::string debugName) override
			{
				return std::make_shared<VulkanFence>(m_innerDevice->CreateFence(std::move(debugName)));
			}
			std::shared_ptr<NLS::Render::RHI::RHISemaphore> CreateSemaphore(std::string debugName) override
			{
				return std::make_shared<VulkanSemaphore>(m_innerDevice->CreateSemaphore(std::move(debugName)));
			}

		private:
			NLS::Render::RHI::NativeRenderDeviceInfo m_nativeInfo{};
			NLS::Render::RHI::RHIDeviceCapabilities m_capabilities{};
			std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
			std::shared_ptr<NLS::Render::RHI::RHIDevice> m_innerDevice;
			std::array<std::shared_ptr<NLS::Render::RHI::RHIQueue>, 3> m_queues{};
		};
	}

	std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateVulkanExplicitDevice(NLS::Render::RHI::IRenderDevice& renderDevice)
	{
		return std::make_shared<VulkanExplicitDevice>(renderDevice);
	}
}
