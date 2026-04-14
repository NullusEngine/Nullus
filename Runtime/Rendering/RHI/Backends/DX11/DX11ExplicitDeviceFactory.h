#pragma once

#include <memory>
#include <vector>
#include <optional>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHISwapchain.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHIResource.h"

#if defined(_WIN32)
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

// Undefine Windows macro that conflicts with RHIDevice::CreateSemaphore
#ifdef CreateSemaphore
#undef CreateSemaphore
#endif
#endif

namespace NLS::Render::RHI
{
	class RHIDevice;
	class DX11Buffer;
	class DX11Texture;
	class DX11TextureView;
}

namespace NLS::Render::Backend
{
	// DX11 Shader Module - wraps D3D11 vertex/pixel shaders
	class DX11ShaderModule final : public NLS::Render::RHI::RHIShaderModule
	{
	public:
		DX11ShaderModule(Microsoft::WRL::ComPtr<ID3D11VertexShader> vs, Microsoft::WRL::ComPtr<ID3D11PixelShader> ps, Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout, const NLS::Render::RHI::RHIShaderModuleDesc& desc);
		~DX11ShaderModule() override = default;

		std::string_view GetDebugName() const override { return m_desc.debugName; }
		const NLS::Render::RHI::RHIShaderModuleDesc& GetDesc() const override { return m_desc; }

		ID3D11VertexShader* GetVertexShader() const { return m_vertexShader.Get(); }
		ID3D11PixelShader* GetPixelShader() const { return m_pixelShader.Get(); }
		ID3D11InputLayout* GetInputLayout() const { return m_inputLayout.Get(); }

	private:
		Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
		Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
		Microsoft::WRL::ComPtr<ID3D11InputLayout> m_inputLayout;
		NLS::Render::RHI::RHIShaderModuleDesc m_desc;
	};

	// DX11 Graphics Pipeline - wraps D3D11 pipeline state
	class DX11GraphicsPipeline final : public NLS::Render::RHI::RHIGraphicsPipeline
	{
	public:
		DX11GraphicsPipeline(
			Microsoft::WRL::ComPtr<ID3D11VertexShader> vs,
			Microsoft::WRL::ComPtr<ID3D11PixelShader> ps,
			Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout,
			Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerState,
			Microsoft::WRL::ComPtr<ID3D11BlendState> blendState,
			Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthStencilState,
			const NLS::Render::RHI::RHIGraphicsPipelineDesc& desc);
		~DX11GraphicsPipeline() override = default;

		std::string_view GetDebugName() const override { return m_desc.debugName; }
		const NLS::Render::RHI::RHIGraphicsPipelineDesc& GetDesc() const override { return m_desc; }
		uint64_t GetPipelineHandle() const override { return reinterpret_cast<uint64_t>(m_rasterizerState.Get()); }

		ID3D11VertexShader* GetVertexShader() const { return m_vertexShader.Get(); }
		ID3D11PixelShader* GetPixelShader() const { return m_pixelShader.Get(); }
		ID3D11InputLayout* GetInputLayout() const { return m_inputLayout.Get(); }
		ID3D11RasterizerState* GetRasterizerState() const { return m_rasterizerState.Get(); }
		ID3D11BlendState* GetBlendState() const { return m_blendState.Get(); }
		ID3D11DepthStencilState* GetDepthStencilState() const { return m_depthStencilState.Get(); }

		const float* GetBlendFactor() const { return m_blendFactor; }
		UINT GetStencilRef() const { return m_stencilRef; }

	private:
		Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
		Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
		Microsoft::WRL::ComPtr<ID3D11InputLayout> m_inputLayout;
		Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizerState;
		Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendState;
		Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthStencilState;
		NLS::Render::RHI::RHIGraphicsPipelineDesc m_desc;
		float m_blendFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		UINT m_stencilRef = 0;
	};

	// DX11 Command Buffer - Tier A implementation with direct D3D11 command recording
	class DX11CommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
	{
	public:
		DX11CommandBuffer(Microsoft::WRL::ComPtr<ID3D11Device> device, Microsoft::WRL::ComPtr<ID3D11DeviceContext> context, std::string debugName);
		~DX11CommandBuffer() override = default;

		// RHIObject
		std::string_view GetDebugName() const override { return m_debugName; }

		// RHICommandBuffer interface - direct D3D11 recording
		void Begin() override;
		void End() override;
		void Reset() override;
		bool IsRecording() const override { return m_recording; }
		void* GetNativeCommandBuffer() const override { return m_context.Get(); }

		void BeginRenderPass(const NLS::Render::RHI::RHIRenderPassDesc& desc) override;
		void EndRenderPass() override;
		void SetViewport(const NLS::Render::RHI::RHIViewport& viewport) override;
		void SetScissor(const NLS::Render::RHI::RHIRect2D& rect) override;
		void BindGraphicsPipeline(const std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>& pipeline) override;
		void BindComputePipeline(const std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>& pipeline) override;
		void BindBindingSet(uint32_t setIndex, const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& bindingSet) override;
		void PushConstants(NLS::Render::RHI::ShaderStageMask stageMask, uint32_t offset, uint32_t size, const void* data) override;
		void BindVertexBuffer(uint32_t slot, const NLS::Render::RHI::RHIVertexBufferView& view) override;
		void BindIndexBuffer(const NLS::Render::RHI::RHIIndexBufferView& view) override;
		void Draw(uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0) override;
		void DrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, int32_t vertexOffset = 0, uint32_t firstInstance = 0) override;
		void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;
		void CopyBuffer(const std::shared_ptr<NLS::Render::RHI::RHIBuffer>& source, const std::shared_ptr<NLS::Render::RHI::RHIBuffer>& destination, const NLS::Render::RHI::RHIBufferCopyRegion& region) override;
		void CopyBufferToTexture(const NLS::Render::RHI::RHIBufferToTextureCopyDesc& desc) override;
		void CopyTexture(const NLS::Render::RHI::RHITextureCopyDesc& desc) override;
		void Barrier(const NLS::Render::RHI::RHIBarrierDesc& barrier) override;

		// Execute accumulated commands
		void Execute();
		void ExecuteDirectDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance);
		void ExecuteDirectDrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance);

	private:
		std::string m_debugName;
		bool m_recording = false;
		Microsoft::WRL::ComPtr<ID3D11Device> m_device;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;

		// Accumulated state for deferred execution
		NLS::Render::RHI::RHIViewport m_viewport;
		bool m_viewportSet = false;
		NLS::Render::RHI::RHIRect2D m_scissor;
		bool m_scissorSet = false;
		std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> m_graphicsPipeline;
		std::shared_ptr<NLS::Render::RHI::RHIBindingSet> m_bindingSet;
		NLS::Render::RHI::RHIVertexBufferView m_vertexBufferView;
		bool m_vertexBufferBound = false;
		std::optional<NLS::Render::RHI::RHIIndexBufferView> m_indexBufferView;
		bool m_indexBufferBound = false;

		// Pending draw call
		enum class DrawType { None, Draw, DrawIndexed };
		DrawType m_pendingDrawType = DrawType::None;
		uint32_t m_pendingVertexCount = 0;
		uint32_t m_pendingInstanceCount = 1;
		uint32_t m_pendingFirstVertex = 0;
		uint32_t m_pendingFirstIndex = 0;
		int32_t m_pendingVertexOffset = 0;
		uint32_t m_pendingFirstInstance = 0;
	};

	// DX11 Command Pool
	class DX11CommandPool final : public NLS::Render::RHI::RHICommandPool
	{
	public:
		DX11CommandPool(Microsoft::WRL::ComPtr<ID3D11Device> device, Microsoft::WRL::ComPtr<ID3D11DeviceContext> context, NLS::Render::RHI::QueueType queueType, std::string debugName);
		~DX11CommandPool() override = default;

		std::string_view GetDebugName() const override { return m_debugName; }
		NLS::Render::RHI::QueueType GetQueueType() const override { return m_queueType; }
		std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> CreateCommandBuffer(std::string debugName = {}) override;
		void Reset() override;

	private:
		std::string m_debugName;
		NLS::Render::RHI::QueueType m_queueType;
		Microsoft::WRL::ComPtr<ID3D11Device> m_device;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
	};

	// DX11 Buffer implementation
	class DX11Buffer final : public NLS::Render::RHI::RHIBuffer
	{
	public:
		DX11Buffer(Microsoft::WRL::ComPtr<ID3D11Buffer> buffer, const NLS::Render::RHI::RHIBufferDesc& desc);
		~DX11Buffer() override = default;

		std::string_view GetDebugName() const override { return m_debugName; }
		const NLS::Render::RHI::RHIBufferDesc& GetDesc() const override { return m_desc; }
		NLS::Render::RHI::ResourceState GetState() const override { return m_state; }
		uint64_t GetGPUAddress() const override { return 0; } // DX11 has no GPU virtual addressing
		NLS::Render::RHI::NativeHandle GetNativeBufferHandle() override { return { NLS::Render::RHI::BackendType::DX11, m_buffer.Get() }; }

		ID3D11Buffer* GetBuffer() const { return m_buffer.Get(); }

	private:
		std::string m_debugName;
		Microsoft::WRL::ComPtr<ID3D11Buffer> m_buffer;
		NLS::Render::RHI::RHIBufferDesc m_desc;
		NLS::Render::RHI::ResourceState m_state = NLS::Render::RHI::ResourceState::Unknown;
	};

	// DX11 Texture implementation
	class DX11Texture final : public NLS::Render::RHI::RHITexture
	{
	public:
		DX11Texture(Microsoft::WRL::ComPtr<ID3D11Texture2D> texture, const NLS::Render::RHI::RHITextureDesc& desc);
		~DX11Texture() override = default;

		std::string_view GetDebugName() const override { return m_debugName; }
		const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
		NLS::Render::RHI::ResourceState GetState() const override { return m_state; }
		NLS::Render::RHI::NativeHandle GetNativeImageHandle() override { return { NLS::Render::RHI::BackendType::DX11, m_texture.Get() }; }

		ID3D11Texture2D* GetTexture() const { return m_texture.Get(); }

	private:
		std::string m_debugName;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
		NLS::Render::RHI::RHITextureDesc m_desc;
		NLS::Render::RHI::ResourceState m_state = NLS::Render::RHI::ResourceState::Unknown;
	};

	// DX11 Texture View implementation
	class DX11TextureView final : public NLS::Render::RHI::RHITextureView
	{
	public:
		DX11TextureView(
			Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv,
			Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv,
			Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv,
			const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
			const NLS::Render::RHI::RHITextureViewDesc& desc);
		~DX11TextureView() override = default;

		std::string_view GetDebugName() const override { return m_debugName; }
		const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
		const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override { return m_texture; }

		NLS::Render::RHI::NativeHandle GetNativeRenderTargetView() override
		{
			return { NLS::Render::RHI::BackendType::DX11, m_rtv.Get() };
		}
		NLS::Render::RHI::NativeHandle GetNativeDepthStencilView() override
		{
			return { NLS::Render::RHI::BackendType::DX11, m_dsv.Get() };
		}
		NLS::Render::RHI::NativeHandle GetNativeShaderResourceView() override
		{
			return { NLS::Render::RHI::BackendType::DX11, m_srv.Get() };
		}

	private:
		std::string m_debugName;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
		Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
		Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_dsv;
		std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
		NLS::Render::RHI::RHITextureViewDesc m_desc;
	};

	// DX11 Queue
	class DX11Queue final : public NLS::Render::RHI::RHIQueue
	{
	public:
		DX11Queue(Microsoft::WRL::ComPtr<ID3D11DeviceContext> context, Microsoft::WRL::ComPtr<IDXGISwapChain> swapchain, NLS::Render::RHI::QueueType queueType, std::string debugName);
		~DX11Queue() override = default;

		std::string_view GetDebugName() const override { return m_debugName; }
		NLS::Render::RHI::QueueType GetType() const override { return m_queueType; }
		void Submit(const NLS::Render::RHI::RHISubmitDesc& submitDesc) override;
		void Present(const NLS::Render::RHI::RHIPresentDesc& presentDesc) override;

	private:
		std::string m_debugName;
		NLS::Render::RHI::QueueType m_queueType;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
		Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapchain;
	};

	// ============================================================================
	// DX11 Formal RHI Synchronization Primitives
	// ============================================================================

	// DX11 Fence - CPU-based synchronization using Windows events
	class NativeDX11Fence final : public NLS::Render::RHI::RHIFence
	{
	public:
		NativeDX11Fence(const std::string& debugName);
		~NativeDX11Fence() override;

		std::string_view GetDebugName() const override { return m_debugName; }
		bool IsSignaled() const override;
		void Reset() override;
		bool Wait(uint64_t timeoutNanoseconds = 0) override;
		void* GetNativeFenceHandle() override { return reinterpret_cast<void*>(m_event); }

	private:
		std::string m_debugName;
		HANDLE m_event = nullptr;
		UINT64 m_fenceValue = 0;
	};

	// DX11 Semaphore - CPU-based synchronization using Windows events
	class NativeDX11Semaphore final : public NLS::Render::RHI::RHISemaphore
	{
	public:
		NativeDX11Semaphore(const std::string& debugName);
		~NativeDX11Semaphore() override;

		std::string_view GetDebugName() const override { return m_debugName; }
		bool IsSignaled() const override;
		void Reset() override;
		void* GetNativeSemaphoreHandle() override { return reinterpret_cast<void*>(m_event); }

		// Signal the semaphore (called when work completes)
		void Signal();

	private:
		std::string m_debugName;
		HANDLE m_event = nullptr;
		bool m_signaled = false;
	};

	// DX11 Swapchain - wraps IDXGISwapChain for DX11 presentation
	class NativeDX11Swapchain final : public NLS::Render::RHI::RHISwapchain
	{
	public:
		NativeDX11Swapchain(Microsoft::WRL::ComPtr<IDXGISwapChain> swapchain, const NLS::Render::RHI::SwapchainDesc& desc);
		~NativeDX11Swapchain() override;

		// RHIObject
		std::string_view GetDebugName() const override { return m_debugName; }

		// RHISwapchain interface
		const NLS::Render::RHI::SwapchainDesc& GetDesc() const override { return m_desc; }
		uint32_t GetImageCount() const override { return m_desc.imageCount; }
		std::optional<NLS::Render::RHI::RHIAcquiredImage> AcquireNextImage(
			const std::shared_ptr<NLS::Render::RHI::RHISemaphore>& signalSemaphore,
			const std::shared_ptr<NLS::Render::RHI::RHIFence>& signalFence) override;
		void Resize(uint32_t width, uint32_t height) override;
		void* GetNativeSwapchainHandle() override { return m_swapchain.Get(); }

		IDXGISwapChain* GetSwapchain() const { return m_swapchain.Get(); }

	private:
		std::string m_debugName;
		Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapchain;
		NLS::Render::RHI::SwapchainDesc m_desc{};
		uint32_t m_currentImageIndex = 0;
	};

	// ============================================================================
	// DX11 Formal RHI Device - Tier A implementation
	// ============================================================================

	// DX11 formal RHI device with direct D3D11 device access
	class DX11ExplicitDevice final : public NLS::Render::RHI::RHIDevice
	{
	public:
		explicit DX11ExplicitDevice(
			Microsoft::WRL::ComPtr<ID3D11Device> device,
			Microsoft::WRL::ComPtr<ID3D11DeviceContext> context,
			std::shared_ptr<NLS::Render::RHI::RHIAdapter> adapter,
			const NLS::Render::RHI::RHIDeviceCapabilities& capabilities);
		~DX11ExplicitDevice() override;

		// RHIDevice interface - Tier A implementation
		std::string_view GetDebugName() const override { return "DX11ExplicitDevice"; }
		const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_adapter; }
		const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override;
		NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override;
		bool IsBackendReady() const override;
		std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType queueType) override;
		std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(const NLS::Render::RHI::SwapchainDesc& desc) override;
		std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(const NLS::Render::RHI::RHIBufferDesc& desc, const void* initialData = nullptr) override;
		std::shared_ptr<NLS::Render::RHI::RHITexture> CreateTexture(const NLS::Render::RHI::RHITextureDesc& desc, const void* initialData = nullptr) override;
		std::shared_ptr<NLS::Render::RHI::RHITextureView> CreateTextureView(const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture, const NLS::Render::RHI::RHITextureViewDesc& desc) override;
		std::shared_ptr<NLS::Render::RHI::RHISampler> CreateSampler(const NLS::Render::RHI::SamplerDesc& desc, std::string debugName = {}) override;
		std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateBindingLayout(const NLS::Render::RHI::RHIBindingLayoutDesc& desc) override;
		std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateBindingSet(const NLS::Render::RHI::RHIBindingSetDesc& desc) override;
		std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> CreatePipelineLayout(const NLS::Render::RHI::RHIPipelineLayoutDesc& desc) override;
		std::shared_ptr<NLS::Render::RHI::RHIShaderModule> CreateShaderModule(const NLS::Render::RHI::RHIShaderModuleDesc& desc) override;
		std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> CreateGraphicsPipeline(const NLS::Render::RHI::RHIGraphicsPipelineDesc& desc) override;
		std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> CreateComputePipeline(const NLS::Render::RHI::RHIComputePipelineDesc& desc) override;
		std::shared_ptr<NLS::Render::RHI::RHICommandPool> CreateCommandPool(NLS::Render::RHI::QueueType queueType, std::string debugName = {}) override;
		std::shared_ptr<NLS::Render::RHI::RHIFence> CreateFence(std::string debugName = {}) override;
		std::shared_ptr<NLS::Render::RHI::RHISemaphore> CreateSemaphore(std::string debugName = {}) override;

		// Readback support - read pixels from a texture
		void ReadPixels(
		    const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
		    uint32_t x,
		    uint32_t y,
		    uint32_t width,
		    uint32_t height,
		    NLS::Render::Settings::EPixelDataFormat format,
		    NLS::Render::Settings::EPixelDataType type,
		    void* data) override;

		// UI rendering support
		bool PrepareUIRender() override;
		void ReleaseUITextureHandles() override;

		// Platform window for swapchain creation
		void SetPlatformWindow(void* window) { m_platformWindow = window; }
		void* GetPlatformWindow() const { return m_platformWindow; }

	private:
		Microsoft::WRL::ComPtr<ID3D11Device> m_device;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
		std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
		NLS::Render::RHI::RHIDeviceCapabilities m_capabilities{};
		std::shared_ptr<DX11Queue> m_graphicsQueue;
		Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapchain;
		void* m_platformWindow = nullptr;
	};

	// Direct creation - creates DX11 Tier A device without going through IRenderDevice
	NLS_RENDER_API std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateDX11RhiDevice(
		void* platformWindow,
		uint32_t width = 1920,
		uint32_t height = 1080);
}
