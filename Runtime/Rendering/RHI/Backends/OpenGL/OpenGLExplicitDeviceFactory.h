#pragma once

#include <memory>
#include <vector>
#include <optional>

#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHISwapchain.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/RHI/Core/RHIBinding.h"

struct GLFWwindow;

namespace NLS::Render::Backend
{
	// Forward declarations
	class OpenGLExplicitDevice;

	// Typed NativeHandle variants for OpenGL backend
	// OpenGL is Tier B - uses compatibility wrappers, these are placeholders for type safety
	struct OpenGLBufferHandle
	{
		uint32_t id = 0;
	};

	struct OpenGLImageHandle
	{
		uint32_t id = 0;
	};

	struct OpenGLSamplerHandle
	{
		uint32_t id = 0;
	};

	// OpenGL Command Buffer shim
	// Wraps legacy IRenderDevice immediate-mode calls in formal RHI command buffer interface
	// Since OpenGL is inherently immediate-mode, this records and replays via legacy device
	class OpenGLCommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
	{
	public:
		OpenGLCommandBuffer(void* platformWindow, std::string debugName);
		~OpenGLCommandBuffer() override = default;

		// RHIObject
		std::string_view GetDebugName() const override { return m_debugName; }

		// RHICommandBuffer interface - minimal shim for Tier B
		void Begin() override;
		void End() override;
		void Reset() override;
		bool IsRecording() const override { return m_recording; }
		// OpenGL doesn't have a native command buffer in the Vulkan sense - returns nullptr
		void* GetNativeCommandBuffer() const override { return nullptr; }

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

		// Execute accumulated commands via legacy device
		void Execute();

	private:
		std::string m_debugName;
		bool m_recording = false;

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

		void* m_platformWindow = nullptr;
	};

	// OpenGL Command Pool
	class OpenGLCommandPool final : public NLS::Render::RHI::RHICommandPool
	{
	public:
		OpenGLCommandPool(void* platformWindow, NLS::Render::RHI::QueueType queueType, std::string debugName);
		~OpenGLCommandPool() override = default;

		std::string_view GetDebugName() const override { return m_debugName; }
		NLS::Render::RHI::QueueType GetQueueType() const override { return m_queueType; }
		std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> CreateCommandBuffer(std::string debugName = {}) override;
		void Reset() override;

	private:
		std::string m_debugName;
		NLS::Render::RHI::QueueType m_queueType;
		void* m_platformWindow = nullptr;
	};

	// OpenGL Queue
	class OpenGLQueue final : public NLS::Render::RHI::RHIQueue
	{
	public:
		OpenGLQueue(void* platformWindow, NLS::Render::RHI::QueueType queueType, std::string debugName);
		~OpenGLQueue() override = default;

		std::string_view GetDebugName() const override { return m_debugName; }
		NLS::Render::RHI::QueueType GetType() const override { return m_queueType; }
		void Submit(const NLS::Render::RHI::RHISubmitDesc& submitDesc) override;
		void Present(const NLS::Render::RHI::RHIPresentDesc& presentDesc) override;

	private:
		std::string m_debugName;
		NLS::Render::RHI::QueueType m_queueType;
		void* m_platformWindow = nullptr;
	};

	// ============================================================================
	// OpenGL Formal RHI Resources
	// ============================================================================

	// OpenGL Shader Module - wraps compiled GLSL shader program
	class OpenGLShaderModule final : public NLS::Render::RHI::RHIShaderModule
	{
	public:
		OpenGLShaderModule(uint32_t programId, const NLS::Render::RHI::RHIShaderModuleDesc& desc);
		~OpenGLShaderModule() override;

		std::string_view GetDebugName() const override { return m_desc.debugName; }
		const NLS::Render::RHI::RHIShaderModuleDesc& GetDesc() const override { return m_desc; }

		uint32_t GetProgramId() const { return m_programId; }

	private:
		uint32_t m_programId = 0;
		NLS::Render::RHI::RHIShaderModuleDesc m_desc{};
	};

	// OpenGL Graphics Pipeline - wraps VAO and pipeline state
	class OpenGLGraphicsPipeline final : public NLS::Render::RHI::RHIGraphicsPipeline
	{
	public:
		OpenGLGraphicsPipeline(
			uint32_t programId,
			uint32_t vaoId,
			const NLS::Render::RHI::RHIGraphicsPipelineDesc& desc);
		~OpenGLGraphicsPipeline() override;

		std::string_view GetDebugName() const override { return m_desc.debugName; }
		const NLS::Render::RHI::RHIGraphicsPipelineDesc& GetDesc() const override { return m_desc; }
		uint64_t GetPipelineHandle() const override { return static_cast<uint64_t>(m_programId); }

		uint32_t GetProgramId() const { return m_programId; }
		uint32_t GetVAOId() const { return m_vaoId; }

	private:
		uint32_t m_programId = 0;
		uint32_t m_vaoId = 0;
		NLS::Render::RHI::RHIGraphicsPipelineDesc m_desc{};
	};

	// OpenGL Texture - wraps GLuint texture ID in formal RHITexture interface
	class OpenGLTexture final : public NLS::Render::RHI::RHITexture
	{
	public:
		OpenGLTexture(uint32_t textureId, const NLS::Render::RHI::RHITextureDesc& desc, std::string debugName);
		~OpenGLTexture() override;

		std::string_view GetDebugName() const override { return m_debugName; }
		const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
		NLS::Render::RHI::ResourceState GetState() const override { return m_state; }
		NLS::Render::RHI::NativeHandle GetNativeImageHandle() override;

		uint32_t GetTextureId() const { return m_textureId; }

	private:
		uint32_t m_textureId = 0;
		NLS::Render::RHI::RHITextureDesc m_desc{};
		NLS::Render::RHI::ResourceState m_state = NLS::Render::RHI::ResourceState::ShaderRead;
		std::string m_debugName;
	};

	// OpenGL Buffer - wraps GLuint buffer ID in formal RHIBuffer interface
	class OpenGLBuffer final : public NLS::Render::RHI::RHIBuffer
	{
	public:
		OpenGLBuffer(uint32_t bufferId, const NLS::Render::RHI::RHIBufferDesc& desc, std::string debugName);
		~OpenGLBuffer() override;

		std::string_view GetDebugName() const override { return m_debugName; }
		const NLS::Render::RHI::RHIBufferDesc& GetDesc() const override { return m_desc; }
		NLS::Render::RHI::ResourceState GetState() const override { return m_state; }
		uint64_t GetGPUAddress() const override { return static_cast<uint64_t>(m_bufferId); }
		NLS::Render::RHI::NativeHandle GetNativeBufferHandle() override;

		uint32_t GetBufferId() const { return m_bufferId; }

	private:
		uint32_t m_bufferId = 0;
		NLS::Render::RHI::RHIBufferDesc m_desc{};
		NLS::Render::RHI::ResourceState m_state = NLS::Render::RHI::ResourceState::ShaderRead;
		std::string m_debugName;
	};

	// OpenGL Texture View - wraps OpenGL texture for shader access
	class OpenGLTextureView final : public NLS::Render::RHI::RHITextureView
	{
	public:
		OpenGLTextureView(const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture, const NLS::Render::RHI::RHITextureViewDesc& desc);
		~OpenGLTextureView() override;

		std::string_view GetDebugName() const override { return m_debugName; }
		const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
		const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override { return m_texture; }
		NLS::Render::RHI::NativeHandle GetNativeRenderTargetView() override;
		NLS::Render::RHI::NativeHandle GetNativeDepthStencilView() override;
		NLS::Render::RHI::NativeHandle GetNativeShaderResourceView() override;

	private:
		std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
		NLS::Render::RHI::RHITextureViewDesc m_desc{};
		std::string m_debugName;
	};

	// OpenGL Sampler - wraps sampler state
	class OpenGLSampler final : public NLS::Render::RHI::RHISampler
	{
	public:
		OpenGLSampler(const NLS::Render::RHI::SamplerDesc& desc, std::string debugName);
		~OpenGLSampler() override;

		std::string_view GetDebugName() const override { return m_debugName; }
		const NLS::Render::RHI::SamplerDesc& GetDesc() const override { return m_desc; }
		NLS::Render::RHI::NativeHandle GetNativeSamplerHandle() override;

		uint32_t GetSamplerId() const { return m_samplerId; }

	private:
		uint32_t m_samplerId = 0;
		NLS::Render::RHI::SamplerDesc m_desc{};
		std::string m_debugName;
	};

	// ============================================================================
	// OpenGL Formal RHI Synchronization Primitives
	// ============================================================================

	// OpenGL Fence - uses GL sync objects
	class NativeOpenGLFence final : public NLS::Render::RHI::RHIFence
	{
	public:
		NativeOpenGLFence(const std::string& debugName);
		~NativeOpenGLFence() override;

		std::string_view GetDebugName() const override { return m_debugName; }
		bool IsSignaled() const override;
		void Reset() override;
		bool Wait(uint64_t timeoutNanoseconds = 0) override;
		void* GetNativeFenceHandle() override { return m_sync; }

	private:
		std::string m_debugName;
		void* m_sync = nullptr;  // GLsync - stored as void* to avoid glad.h dependency in header
	};

	// OpenGL Semaphore - CPU-based synchronization (OpenGL has no native semaphore)
	class NativeOpenGLSemaphore final : public NLS::Render::RHI::RHISemaphore
	{
	public:
		NativeOpenGLSemaphore(const std::string& debugName);
		~NativeOpenGLSemaphore() override;

		std::string_view GetDebugName() const override { return m_debugName; }
		bool IsSignaled() const override;
		void Reset() override;
		void* GetNativeSemaphoreHandle() override { return nullptr; } // OpenGL has no native semaphore

		// Signal the semaphore
		void Signal();

	private:
		std::string m_debugName;
		void* m_event = nullptr;  // HANDLE - stored as void* to avoid Windows.h dependency in header
		bool m_signaled = false;
	};

	// OpenGL Swapchain - wraps GLFW window for OpenGL presentation
	class NativeOpenGLSwapchain final : public NLS::Render::RHI::RHISwapchain
	{
	public:
		NativeOpenGLSwapchain(void* glfwWindow, const NLS::Render::RHI::SwapchainDesc& desc);
		~NativeOpenGLSwapchain() override;

		// RHIObject
		std::string_view GetDebugName() const override { return m_debugName; }

		// RHISwapchain interface
		const NLS::Render::RHI::SwapchainDesc& GetDesc() const override { return m_desc; }
		uint32_t GetImageCount() const override { return m_desc.imageCount; }
		std::optional<NLS::Render::RHI::RHIAcquiredImage> AcquireNextImage(
			const std::shared_ptr<NLS::Render::RHI::RHISemaphore>& signalSemaphore,
			const std::shared_ptr<NLS::Render::RHI::RHIFence>& signalFence) override;
		void Resize(uint32_t width, uint32_t height) override;
		void* GetNativeSwapchainHandle() override { return m_window; }

		// Present - called by OpenGLQueue::Present
		void Present();

	private:
		std::string m_debugName;
		void* m_window = nullptr;
		NLS::Render::RHI::SwapchainDesc m_desc{};
		uint32_t m_currentImageIndex = 0;
	};

	// OpenGL formal RHI device - Tier A implementation
	// Provides formal RHIDevice interface with direct OpenGL resource management

	// Undefine Windows macro that conflicts with RHIDevice::CreateSemaphore
	#ifdef CreateSemaphore
	#undef CreateSemaphore
	#endif

	class OpenGLExplicitDevice final : public NLS::Render::RHI::RHIDevice
	{
	public:
		// Direct creation with GLFW window
		explicit OpenGLExplicitDevice(void* platformWindow);
		~OpenGLExplicitDevice() override;

		// RHIDevice interface - full implementation
		std::string_view GetDebugName() const override { return "OpenGLExplicitDevice"; }
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

		// Initialize GLAD - called when platform window is available
		void InitializeGLAD();

		// Platform window management
		void SetPlatformWindow(void* window) { m_platformWindow = window; }
		void* GetPlatformWindow() const { return m_platformWindow; }

	private:
		void* m_platformWindow = nullptr;
		std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
		NLS::Render::RHI::RHIDeviceCapabilities m_capabilities{};
		std::shared_ptr<OpenGLQueue> m_graphicsQueue;
	};

	// Direct creation - creates OpenGL Tier A device without IRenderDevice
	std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateOpenGLRhiDevice(void* platformWindow);
}
