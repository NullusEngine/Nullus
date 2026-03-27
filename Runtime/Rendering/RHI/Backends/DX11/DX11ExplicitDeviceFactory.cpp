#include "Rendering/RHI/Backends/DX11/DX11ExplicitDeviceFactory.h"

#include <utility>

#include "Rendering/RHI/Backends/OpenGL/Compat/ExplicitRHICompat.h"
#include "Rendering/RHI/Core/RHIDevice.h"

namespace NLS::Render::Backend
{
	namespace
	{
		class DX11CompatibilityAdapter final : public NLS::Render::RHI::RHIAdapter
		{
		public:
			explicit DX11CompatibilityAdapter(std::shared_ptr<NLS::Render::RHI::RHIAdapter> inner)
				: m_inner(std::move(inner))
			{
			}

			std::string_view GetDebugName() const override { return "DX11CompatibilityAdapter"; }
			NLS::Render::RHI::NativeBackendType GetBackendType() const override { return NLS::Render::RHI::NativeBackendType::None; }
			std::string_view GetVendor() const override { return m_inner != nullptr ? m_inner->GetVendor() : std::string_view{}; }
			std::string_view GetHardware() const override { return m_inner != nullptr ? m_inner->GetHardware() : std::string_view{}; }

		private:
			std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_inner;
		};

		class DX11CompatibilityExplicitDevice final : public NLS::Render::RHI::RHIDevice
		{
		public:
			explicit DX11CompatibilityExplicitDevice(std::shared_ptr<NLS::Render::RHI::RHIDevice> inner)
				: m_inner(std::move(inner))
				, m_adapter(std::make_shared<DX11CompatibilityAdapter>(m_inner != nullptr ? m_inner->GetAdapter() : nullptr))
			{
			}

			std::string_view GetDebugName() const override { return "DX11CompatibilityExplicitDevice"; }
			const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_adapter; }
			const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_inner->GetCapabilities(); }
			NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override { return {}; }
			bool IsBackendReady() const override { return m_inner->IsBackendReady(); }
			std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType queueType) override { return m_inner->GetQueue(queueType); }
			std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(const NLS::Render::RHI::SwapchainDesc& desc) override { return m_inner->CreateSwapchain(desc); }
			std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(const NLS::Render::RHI::RHIBufferDesc& desc, const void* initialData = nullptr) override { return m_inner->CreateBuffer(desc, initialData); }
			std::shared_ptr<NLS::Render::RHI::RHITexture> CreateTexture(const NLS::Render::RHI::RHITextureDesc& desc, const void* initialData = nullptr) override { return m_inner->CreateTexture(desc, initialData); }
			std::shared_ptr<NLS::Render::RHI::RHITextureView> CreateTextureView(const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture, const NLS::Render::RHI::RHITextureViewDesc& desc) override { return m_inner->CreateTextureView(texture, desc); }
			std::shared_ptr<NLS::Render::RHI::RHISampler> CreateSampler(const NLS::Render::RHI::SamplerDesc& desc, std::string debugName = {}) override { return m_inner->CreateSampler(desc, std::move(debugName)); }
			std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateBindingLayout(const NLS::Render::RHI::RHIBindingLayoutDesc& desc) override { return m_inner->CreateBindingLayout(desc); }
			std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateBindingSet(const NLS::Render::RHI::RHIBindingSetDesc& desc) override { return m_inner->CreateBindingSet(desc); }
			std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> CreatePipelineLayout(const NLS::Render::RHI::RHIPipelineLayoutDesc& desc) override { return m_inner->CreatePipelineLayout(desc); }
			std::shared_ptr<NLS::Render::RHI::RHIShaderModule> CreateShaderModule(const NLS::Render::RHI::RHIShaderModuleDesc& desc) override { return m_inner->CreateShaderModule(desc); }
			std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> CreateGraphicsPipeline(const NLS::Render::RHI::RHIGraphicsPipelineDesc& desc) override { return m_inner->CreateGraphicsPipeline(desc); }
			std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> CreateComputePipeline(const NLS::Render::RHI::RHIComputePipelineDesc& desc) override { return m_inner->CreateComputePipeline(desc); }
			std::shared_ptr<NLS::Render::RHI::RHICommandPool> CreateCommandPool(NLS::Render::RHI::QueueType queueType, std::string debugName = {}) override { return m_inner->CreateCommandPool(queueType, std::move(debugName)); }
			std::shared_ptr<NLS::Render::RHI::RHIFence> CreateFence(std::string debugName = {}) override { return m_inner->CreateFence(std::move(debugName)); }
			std::shared_ptr<NLS::Render::RHI::RHISemaphore> CreateSemaphore(std::string debugName = {}) override { return m_inner->CreateSemaphore(std::move(debugName)); }

		private:
			std::shared_ptr<NLS::Render::RHI::RHIDevice> m_inner;
			std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
		};
	}

	std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateDX11ExplicitDevice(NLS::Render::RHI::IRenderDevice& renderDevice)
	{
		return std::make_shared<DX11CompatibilityExplicitDevice>(NLS::Render::RHI::CreateCompatibilityExplicitDevice(renderDevice));
	}
}
