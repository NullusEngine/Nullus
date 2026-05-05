#include "Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.h"
#include "Rendering/RHI/Backends/DX12/DX12Command.h"
#include "Rendering/RHI/Backends/DX12/DX12Descriptor.h"
#include "Rendering/RHI/Backends/DX12/DX12Device.h"
#include "Rendering/RHI/Backends/DX12/DX12Pipeline.h"
#include "Rendering/RHI/Backends/DX12/DX12Queue.h"
#include "Rendering/RHI/Backends/DX12/DX12ReadbackUtils.h"
#include "Rendering/RHI/Backends/DX12/DX12Resource.h"
#include "Rendering/RHI/Backends/DX12/DX12Swapchain.h"
#include "Rendering/RHI/Backends/DX12/DX12Synchronization.h"

#include <array>
#include <memory>
#include <string>

#include "Profiling/Profiler.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"

#if defined(_WIN32)
#include <dxgi1_6.h>
#undef CreateSemaphore  // Windows macro conflicts with RHIDevice::CreateSemaphore
#include <d3d12.h>
#include <wrl/client.h>
#endif

namespace NLS::Render::Backend
{
#if defined(_WIN32)
	using Microsoft::WRL::ComPtr;
#endif

	namespace
	{
		constexpr uint32_t kDx12ShaderVisibleResourceDescriptorCapacity = 65536u;
		constexpr uint32_t kDx12ShaderVisibleSamplerDescriptorCapacity = 2048u;

		class NativeDX12Adapter final : public NLS::Render::RHI::RHIAdapter
		{
		public:
			NativeDX12Adapter(const std::string& vendor, const std::string& hardware)
				: m_vendor(vendor)
				, m_hardware(hardware)
			{
			}

			std::string_view GetDebugName() const override { return "NativeDX12Adapter"; }
			NLS::Render::RHI::NativeBackendType GetBackendType() const override { return NLS::Render::RHI::NativeBackendType::DX12; }
			std::string_view GetVendor() const override { return m_vendor; }
			std::string_view GetHardware() const override { return m_hardware; }

		private:
			std::string m_vendor;
			std::string m_hardware;
		};

		class NativeDX12ExplicitDevice final : public NLS::Render::RHI::RHIDevice
		{
		public:
			NativeDX12ExplicitDevice(
				ID3D12Device* device,
				ID3D12CommandQueue* graphicsQueue,
				ID3D12CommandQueue* computeQueue,
				IDXGIFactory6* factory,
				IDXGIAdapter1* adapter,
				const NLS::Render::RHI::RHIDeviceCapabilities& capabilities,
				const std::string& vendor,
				const std::string& hardware)
				: m_device(device)
				, m_graphicsQueue(graphicsQueue)
				, m_computeQueue(computeQueue)
				, m_factory(factory)
				, m_adapter(adapter)
				, m_capabilities(capabilities)
				, m_rhiAdapter(std::make_shared<NativeDX12Adapter>(vendor, hardware))
				, m_resourceHeapAllocator(std::make_unique<DX12ShaderVisibleDescriptorHeapAllocator>(
					device,
					graphicsQueue,
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
					kDx12ShaderVisibleResourceDescriptorCapacity,
					"DX12ResourceHeapAllocator"))
				, m_samplerHeapAllocator(std::make_unique<DX12ShaderVisibleDescriptorHeapAllocator>(
					device,
					graphicsQueue,
					D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
					kDx12ShaderVisibleSamplerDescriptorCapacity,
					"DX12SamplerHeapAllocator"))
			{
			}

			~NativeDX12ExplicitDevice()
			{
				m_samplerHeapAllocator.reset();
				m_resourceHeapAllocator.reset();
			}

			std::string_view GetDebugName() const override { return "NativeDX12ExplicitDevice"; }
			const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_rhiAdapter; }
			const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
			NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override
			{
				NLS::Render::RHI::NativeRenderDeviceInfo info{};
				info.backend = NLS::Render::RHI::NativeBackendType::DX12;
#if defined(_WIN32)
				info.device = m_device.Get();
				info.graphicsQueue = m_graphicsQueue.Get();
				info.swapchain = m_swapchain.Get();
				info.nativeWindowHandle = m_nativeWindowHandle;
#endif
				return info;
			}
			bool IsBackendReady() const override { return m_device != nullptr; }

			std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType queueType) override
			{
				if (queueType == NLS::Render::RHI::QueueType::Compute && m_computeQueue == nullptr)
				{
					if (m_queues[static_cast<size_t>(NLS::Render::RHI::QueueType::Graphics)] == nullptr)
					{
						m_queues[static_cast<size_t>(NLS::Render::RHI::QueueType::Graphics)] =
							std::make_shared<NativeDX12Queue>(
								m_device.Get(),
								m_graphicsQueue.Get(),
								NLS::Render::RHI::QueueType::Graphics,
								"GraphicsQueue");
					}
					m_queues[static_cast<size_t>(queueType)] =
						m_queues[static_cast<size_t>(NLS::Render::RHI::QueueType::Graphics)];
					return m_queues[static_cast<size_t>(queueType)];
				}

				const auto queueIndex = static_cast<size_t>(queueType);
				if (m_queues[queueIndex] == nullptr)
				{
					ID3D12CommandQueue* nativeQueue = m_graphicsQueue.Get();
					std::string debugName = "GraphicsQueue";
					NLS::Render::RHI::QueueType resolvedQueueType = NLS::Render::RHI::QueueType::Graphics;
					if (queueType == NLS::Render::RHI::QueueType::Compute && m_computeQueue != nullptr)
					{
						nativeQueue = m_computeQueue.Get();
						debugName = "ComputeQueue";
						resolvedQueueType = NLS::Render::RHI::QueueType::Compute;
					}
					m_queues[queueIndex] = std::make_shared<NativeDX12Queue>(
						m_device.Get(),
						nativeQueue,
						resolvedQueueType,
						debugName);
					InitializeTimelineGpuProfilerIfNeeded();
				}
				return m_queues[queueIndex];
			}

			std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(const NLS::Render::RHI::SwapchainDesc& desc) override
			{
#if defined(_WIN32)
				auto swapchain = CreateNativeDX12Swapchain(
					m_factory.Get(),
					m_device.Get(),
					m_graphicsQueue.Get(),
					desc);
				if (swapchain != nullptr)
				{
					m_nativeWindowHandle = desc.nativeWindowHandle;
					m_swapchain = static_cast<IDXGISwapChain3*>(swapchain->GetNativeSwapchainHandle());
				}
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
#if defined(_WIN32)
				InitializeTimelineGpuProfilerIfNeeded();
				const bool useDedicatedComputeQueue =
					queueType == NLS::Render::RHI::QueueType::Compute &&
					m_computeQueue != nullptr;
				ID3D12CommandQueue* nativeQueue = useDedicatedComputeQueue
					? m_computeQueue.Get()
					: m_graphicsQueue.Get();
				const D3D12_COMMAND_LIST_TYPE commandListType = useDedicatedComputeQueue
					? D3D12_COMMAND_LIST_TYPE_COMPUTE
					: D3D12_COMMAND_LIST_TYPE_DIRECT;
				return std::make_shared<NativeDX12CommandPool>(
					m_device.Get(),
					nativeQueue,
					queueType,
					commandListType,
					debugName.empty() ? "CommandPool" : debugName);
#else
				return nullptr;
#endif
			}
			std::shared_ptr<NLS::Render::RHI::RHIFence> CreateFence(std::string debugName) override
			{
#if defined(_WIN32)
				return std::make_shared<NativeDX12Fence>(m_device.Get(), debugName.empty() ? "Fence" : debugName);
#else
				return nullptr;
#endif
			}
			std::shared_ptr<NLS::Render::RHI::RHISemaphore> CreateSemaphore(std::string debugName) override
			{
#if defined(_WIN32)
				return std::make_shared<NativeDX12Semaphore>(m_device.Get(), debugName.empty() ? "Semaphore" : debugName);
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
			void InitializeTimelineGpuProfilerIfNeeded()
			{
#if defined(_WIN32)
				if (m_gpuProfilerInitialized || m_device == nullptr || m_graphicsQueue == nullptr)
					return;

				NLS::Base::Profiling::ProfilerGpuContextEvent event;
				event.nativeDevice = m_device.Get();
				event.nativeCommandQueues.push_back(m_graphicsQueue.Get());
				if (m_computeQueue != nullptr)
					event.nativeCommandQueues.push_back(m_computeQueue.Get());
				event.frameLatency = 2u;
				NLS::Base::Profiling::Profiler::InitializeGpuContext(event);
				m_gpuProfilerInitialized = true;
#endif
			}

			Microsoft::WRL::ComPtr<ID3D12Device> m_device;
			Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_graphicsQueue;
			Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_computeQueue;
			Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
			Microsoft::WRL::ComPtr<IDXGIAdapter1> m_adapter;
			Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapchain;
			void* m_nativeWindowHandle = nullptr;
			NLS::Render::RHI::RHIDeviceCapabilities m_capabilities{};
			std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_rhiAdapter;
			std::array<std::shared_ptr<NLS::Render::RHI::RHIQueue>, 3> m_queues{};
			std::unique_ptr<DX12ShaderVisibleDescriptorHeapAllocator> m_resourceHeapAllocator;
			std::unique_ptr<DX12ShaderVisibleDescriptorHeapAllocator> m_samplerHeapAllocator;
			bool m_gpuProfilerInitialized = false;
		};

		// NativeDX12ExplicitDevice method implementations
		std::shared_ptr<NLS::Render::RHI::RHIBuffer> NativeDX12ExplicitDevice::CreateBuffer(const NLS::Render::RHI::RHIBufferDesc& desc, const void* initialData)
		{
			if (m_device == nullptr)
				return nullptr;
			return std::make_shared<NativeDX12Buffer>(m_device.Get(), m_graphicsQueue.Get(), desc, initialData);
		}

		std::shared_ptr<NLS::Render::RHI::RHITexture> NativeDX12ExplicitDevice::CreateTexture(const NLS::Render::RHI::RHITextureDesc& desc, const void* initialData)
		{
#if defined(_WIN32)
			return CreateNativeDX12Texture(m_device.Get(), m_graphicsQueue.Get(), desc, initialData);
#else
			return nullptr;
#endif
		}

		std::shared_ptr<NLS::Render::RHI::RHITextureView> NativeDX12ExplicitDevice::CreateTextureView(const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture, const NLS::Render::RHI::RHITextureViewDesc& desc)
		{
			if (texture == nullptr)
				return nullptr;
			return std::make_shared<NativeDX12TextureView>(m_device.Get(), texture, desc);
		}

		std::shared_ptr<NLS::Render::RHI::RHISampler> NativeDX12ExplicitDevice::CreateSampler(const NLS::Render::RHI::SamplerDesc& desc, std::string debugName)
		{
#if defined(_WIN32)
			if (m_device == nullptr)
				return nullptr;
			return std::make_shared<NativeDX12Sampler>(m_device.Get(), desc, debugName.empty() ? "Sampler" : debugName);
#else
			return nullptr;
#endif
		}

		std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> NativeDX12ExplicitDevice::CreateBindingLayout(const NLS::Render::RHI::RHIBindingLayoutDesc& desc)
		{
			return std::make_shared<NativeDX12BindingLayout>(desc);
		}

		std::shared_ptr<NLS::Render::RHI::RHIBindingSet> NativeDX12ExplicitDevice::CreateBindingSet(const NLS::Render::RHI::RHIBindingSetDesc& desc)
		{
#if defined(_WIN32)
			return std::make_shared<NativeDX12BindingSet>(
				m_device.Get(),
				desc,
				m_resourceHeapAllocator.get(),
				m_samplerHeapAllocator.get());
#else
			return nullptr;
#endif
		}

		std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> NativeDX12ExplicitDevice::CreatePipelineLayout(const NLS::Render::RHI::RHIPipelineLayoutDesc& desc)
		{
			return std::make_shared<NativeDX12PipelineLayout>(m_device.Get(), desc);
		}

		std::shared_ptr<NLS::Render::RHI::RHIShaderModule> NativeDX12ExplicitDevice::CreateShaderModule(const NLS::Render::RHI::RHIShaderModuleDesc& desc)
		{
			return std::make_shared<NativeDX12ShaderModule>(desc);
		}

		std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> NativeDX12ExplicitDevice::CreateGraphicsPipeline(const NLS::Render::RHI::RHIGraphicsPipelineDesc& desc)
		{
			return std::make_shared<NativeDX12GraphicsPipeline>(m_device.Get(), desc);
		}

		std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> NativeDX12ExplicitDevice::CreateComputePipeline(const NLS::Render::RHI::RHIComputePipelineDesc& desc)
		{
			return std::make_shared<NativeDX12ComputePipeline>(m_device.Get(), desc);
		}

		void NativeDX12ExplicitDevice::ReadPixels(
		    const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
		    uint32_t x,
		    uint32_t y,
		    uint32_t width,
		    uint32_t height,
		    NLS::Render::Settings::EPixelDataFormat format,
		    NLS::Render::Settings::EPixelDataType type,
		    void* data)
		{
#if defined(_WIN32)
			NLS::Render::RHI::DX12::ExecuteDX12ReadPixels(
				m_device.Get(),
				m_graphicsQueue.Get(),
				texture,
				x,
				y,
				width,
				height,
				format,
				type,
				data);
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


	std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateNativeDX12ExplicitDevice(
		ID3D12Device* device,
		ID3D12CommandQueue* graphicsQueue,
		ID3D12CommandQueue* computeQueue,
		IDXGIFactory6* factory,
		IDXGIAdapter1* adapter,
		const NLS::Render::RHI::RHIDeviceCapabilities& capabilities,
		const std::string& vendor,
		const std::string& hardware)
	{
		return std::make_shared<NativeDX12ExplicitDevice>(
			device,
			graphicsQueue,
			computeQueue,
			factory,
			adapter,
			capabilities,
			vendor,
			hardware);
	}

	std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateDX12RhiDevice(bool debugMode)
	{
		const DX12DeviceResources resources = CreateDX12DeviceResources(debugMode);
		if (!resources.IsValid())
			return nullptr;

#if defined(_WIN32)
		return CreateNativeDX12ExplicitDevice(
			resources.device.Get(),
			resources.graphicsQueue.Get(),
			resources.computeQueue.Get(),
			resources.factory.Get(),
			resources.adapter.Get(),
			resources.capabilities,
			resources.vendor,
			resources.hardware);
#else
		return nullptr;
#endif
	}
}
