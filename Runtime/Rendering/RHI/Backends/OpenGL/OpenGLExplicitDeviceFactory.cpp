#include "Rendering/RHI/Backends/OpenGL/OpenGLExplicitDeviceFactory.h"

#if defined(_WIN32)
// Include windows.h first at the top, before glad/glfw
#include <windows.h>

// Immediately undefine CreateSemaphore macro to prevent conflict with our method name
// This must be BEFORE any other includes that might include windows.h
#ifdef CreateSemaphore
#undef CreateSemaphore
#endif
#endif

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#if defined(_WIN32)
// Re-include windows.h after GLFW to ensure we have the real Windows API available
// (GLFW may have included windows.h internally)
#include <windows.h>
#endif

#include "Debug/Logger.h"

namespace NLS::Render::Backend
{
	namespace
	{
		GLenum ToGLComparisonFunc(const NLS::Render::Settings::EComparaisonAlgorithm algorithm)
		{
			return GL_NEVER + static_cast<GLint>(algorithm);
		}

		GLenum ToGLStencilOp(const NLS::Render::Settings::EOperation operation)
		{
			switch (operation)
			{
			case NLS::Render::Settings::EOperation::KEEP: return GL_KEEP;
			case NLS::Render::Settings::EOperation::ZERO: return GL_ZERO;
			case NLS::Render::Settings::EOperation::REPLACE: return GL_REPLACE;
			case NLS::Render::Settings::EOperation::INCREMENT: return GL_INCR;
			case NLS::Render::Settings::EOperation::INCREMENT_WRAP: return GL_INCR_WRAP;
			case NLS::Render::Settings::EOperation::DECREMENT: return GL_DECR;
			case NLS::Render::Settings::EOperation::DECREMENT_WRAP: return GL_DECR_WRAP;
			case NLS::Render::Settings::EOperation::INVERT: return GL_INVERT;
			default: return GL_KEEP;
			}
		}

		class OpenGLAdapter final : public RHI::RHIAdapter
		{
		public:
			OpenGLAdapter(const std::string& vendor, const std::string& hardware)
				: m_vendor(vendor), m_hardware(hardware)
			{}

			std::string_view GetDebugName() const override { return "OpenGLAdapter"; }
			RHI::NativeBackendType GetBackendType() const override { return RHI::NativeBackendType::OpenGL; }
			std::string_view GetVendor() const override { return m_vendor; }
			std::string_view GetHardware() const override { return m_hardware; }

		private:
			std::string m_vendor;
			std::string m_hardware;
		};
	}

	// ============================================================================
	// OpenGLCommandBuffer Implementation
	// ============================================================================

	OpenGLCommandBuffer::OpenGLCommandBuffer(void* platformWindow, std::string debugName)
		: m_platformWindow(platformWindow)
		, m_debugName(std::move(debugName))
	{
	}

	void OpenGLCommandBuffer::Begin()
	{
		m_recording = true;
		Reset();
	}

	void OpenGLCommandBuffer::End()
	{
		m_recording = false;
		Execute();
	}

	void OpenGLCommandBuffer::Reset()
	{
		m_recording = false;
		m_viewportSet = false;
		m_scissorSet = false;
		m_graphicsPipeline.reset();
		m_bindingSet.reset();
		m_vertexBufferBound = false;
		m_indexBufferBound = false;
		m_pendingDrawType = DrawType::None;
	}

	void OpenGLCommandBuffer::BeginRenderPass(const RHI::RHIRenderPassDesc& desc)
	{
		// Bind default framebuffer (0) for swapchain rendering
		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		// Handle clear operations
		for (size_t i = 0; i < desc.colorAttachments.size(); ++i)
		{
			const auto& att = desc.colorAttachments[i];
			if (att.loadOp == RHI::LoadOp::Clear)
			{
				glClearColor(att.clearValue.r, att.clearValue.g, att.clearValue.b, att.clearValue.a);
				glClear(GL_COLOR_BUFFER_BIT);
			}
		}

		if (desc.depthStencilAttachment.has_value())
		{
			const auto& dsAtt = desc.depthStencilAttachment.value();
			GLenum clearMask = 0;
			if (dsAtt.depthLoadOp == RHI::LoadOp::Clear)
				clearMask |= GL_DEPTH_BUFFER_BIT;
			if (dsAtt.stencilLoadOp == RHI::LoadOp::Clear)
				clearMask |= GL_STENCIL_BUFFER_BIT;

			if (clearMask != 0)
			{
				glClearDepth(dsAtt.clearValue.depth);
				glClearStencil(static_cast<GLint>(dsAtt.clearValue.stencil));
				glClear(clearMask);
			}
		}
	}

	void OpenGLCommandBuffer::EndRenderPass()
	{
		// OpenGL doesn't need explicit end render pass
	}

	void OpenGLCommandBuffer::SetViewport(const RHI::RHIViewport& viewport)
	{
		m_viewport = viewport;
		m_viewportSet = true;
		glViewport(
			static_cast<GLint>(viewport.x),
			static_cast<GLint>(viewport.y),
			static_cast<GLsizei>(viewport.width),
			static_cast<GLsizei>(viewport.height));
	}

	void OpenGLCommandBuffer::SetScissor(const RHI::RHIRect2D& rect)
	{
		m_scissor = rect;
		m_scissorSet = true;
		glEnable(GL_SCISSOR_TEST);
		glScissor(
			static_cast<GLint>(rect.x),
			static_cast<GLint>(rect.y),
			static_cast<GLsizei>(rect.width),
			static_cast<GLsizei>(rect.height));
	}

	void OpenGLCommandBuffer::BindGraphicsPipeline(const std::shared_ptr<RHI::RHIGraphicsPipeline>& pipeline)
	{
		m_graphicsPipeline = pipeline;

		if (pipeline == nullptr)
			return;

		auto* glPipeline = dynamic_cast<OpenGLGraphicsPipeline*>(pipeline.get());
		if (glPipeline == nullptr)
		{
			NLS_LOG_WARNING("OpenGLCommandBuffer::BindGraphicsPipeline: invalid pipeline type");
			return;
		}

		// Bind shader program
		glUseProgram(glPipeline->GetProgramId());

		// Bind VAO
		glBindVertexArray(glPipeline->GetVAOId());

		// Apply rasterizer state from pipeline desc
		const auto& desc = glPipeline->GetDesc();
		if (desc.rasterState.cullEnabled)
		{
			glEnable(GL_CULL_FACE);
			if (desc.rasterState.cullFace == Settings::ECullFace::FRONT)
				glCullFace(GL_FRONT);
			else if (desc.rasterState.cullFace == Settings::ECullFace::BACK)
				glCullFace(GL_BACK);
			else
				glCullFace(GL_FRONT_AND_BACK);
		}
		else
		{
			glDisable(GL_CULL_FACE);
		}

		// Wireframe mode
		if (desc.rasterState.wireframe)
			glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		else
			glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

		// Apply blend state
		if (desc.blendState.enabled)
		{
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			if (!desc.blendState.colorWrite)
				glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
			else
				glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		}
		else
		{
			glDisable(GL_BLEND);
		}

		// Apply depth state
		if (desc.depthStencilState.depthTest)
		{
			glEnable(GL_DEPTH_TEST);
			glDepthFunc(ToGLComparisonFunc(desc.depthStencilState.depthCompare));
		}
		else
		{
			glDisable(GL_DEPTH_TEST);
		}

		if (desc.depthStencilState.depthWrite)
			glDepthMask(GL_TRUE);
		else
			glDepthMask(GL_FALSE);

		if (desc.depthStencilState.stencilTest)
		{
			glEnable(GL_STENCIL_TEST);
			glStencilMask(static_cast<GLuint>(desc.depthStencilState.stencilWriteMask));
			glStencilFunc(
				ToGLComparisonFunc(desc.depthStencilState.stencilCompare),
				static_cast<GLint>(desc.depthStencilState.stencilReference),
				static_cast<GLuint>(desc.depthStencilState.stencilReadMask));
			glStencilOp(
				ToGLStencilOp(desc.depthStencilState.stencilFailOp),
				ToGLStencilOp(desc.depthStencilState.stencilDepthFailOp),
				ToGLStencilOp(desc.depthStencilState.stencilPassOp));
		}
		else
		{
			glDisable(GL_STENCIL_TEST);
		}
	}

	void OpenGLCommandBuffer::BindComputePipeline(const std::shared_ptr<RHI::RHIComputePipeline>& pipeline)
	{
		// Tier B - compute not supported via legacy path
	}

	void OpenGLCommandBuffer::BindBindingSet(uint32_t setIndex, const std::shared_ptr<RHI::RHIBindingSet>& bindingSet)
	{
		m_bindingSet = bindingSet;
		// Tier B - binding set handling via legacy device
	}

	void OpenGLCommandBuffer::PushConstants(RHI::ShaderStageMask stageMask, uint32_t offset, uint32_t size, const void* data)
	{
		// Tier B - legacy device doesn't have push constants
		// Could potentially use uniforms, but this is a shim
	}

	void OpenGLCommandBuffer::BindVertexBuffer(uint32_t slot, const RHI::RHIVertexBufferView& view)
	{
		m_vertexBufferView = view;
		m_vertexBufferBound = true;

		if (view.buffer == nullptr)
			return;

		// Get OpenGL buffer ID from the RHIBuffer
		auto* glBuffer = dynamic_cast<OpenGLBuffer*>(view.buffer.get());
		if (glBuffer == nullptr)
			return;

		glBindVertexBuffer(slot, glBuffer->GetBufferId(), view.offset, view.stride);
	}

	void OpenGLCommandBuffer::BindIndexBuffer(const RHI::RHIIndexBufferView& view)
	{
		m_indexBufferView = view;
		m_indexBufferBound = true;

		if (view.buffer == nullptr)
			return;

		auto* glBuffer = dynamic_cast<OpenGLBuffer*>(view.buffer.get());
		if (glBuffer == nullptr)
			return;

		GLenum format = (view.indexType == RHI::IndexType::UInt16) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, glBuffer->GetBufferId());
	}

	void OpenGLCommandBuffer::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
	{
		// OpenGL is inherently immediate-mode, execute draw immediately
		if (instanceCount > 1)
			glDrawArraysInstanced(GL_TRIANGLES, static_cast<GLint>(firstVertex), static_cast<GLsizei>(vertexCount), instanceCount);
		else
			glDrawArrays(GL_TRIANGLES, static_cast<GLint>(firstVertex), static_cast<GLsizei>(vertexCount));
	}

	void OpenGLCommandBuffer::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
	{
		// OpenGL is inherently immediate-mode, execute draw immediately
		GLenum indexType = GL_UNSIGNED_INT; // Default, should match bound index buffer
		if (instanceCount > 1)
			glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(indexCount), indexType,
				reinterpret_cast<void*>(static_cast<uintptr_t>(firstIndex * sizeof(uint32_t))), instanceCount);
		else
			glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indexCount), indexType,
				reinterpret_cast<void*>(static_cast<uintptr_t>(firstIndex * sizeof(uint32_t))));
	}

	void OpenGLCommandBuffer::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
	{
		// Tier B - compute not supported
	}

	void OpenGLCommandBuffer::CopyBuffer(const std::shared_ptr<RHI::RHIBuffer>& source, const std::shared_ptr<RHI::RHIBuffer>& destination, const RHI::RHIBufferCopyRegion& region)
	{
		// Tier B - copy operations would need legacy device support
	}

	void OpenGLCommandBuffer::CopyBufferToTexture(const RHI::RHIBufferToTextureCopyDesc& desc)
	{
		// Tier B - copy operations would need legacy device support
	}

	void OpenGLCommandBuffer::CopyTexture(const RHI::RHITextureCopyDesc& desc)
	{
		// Tier B - copy operations would need legacy device support
	}

	void OpenGLCommandBuffer::Barrier(const RHI::RHIBarrierDesc& barrier)
	{
		// Tier B - memory barriers handled by legacy device
	}

	// ============================================================================
	// OpenGLShaderModule Implementation
	// ============================================================================

	OpenGLShaderModule::OpenGLShaderModule(uint32_t programId, const RHI::RHIShaderModuleDesc& desc)
		: m_programId(programId), m_desc(desc)
	{
	}

	OpenGLShaderModule::~OpenGLShaderModule()
	{
		if (m_programId != 0)
		{
			glDeleteProgram(m_programId);
		}
	}

	// ============================================================================
	// OpenGLGraphicsPipeline Implementation
	// ============================================================================

	OpenGLGraphicsPipeline::OpenGLGraphicsPipeline(
		uint32_t programId,
		uint32_t vaoId,
		const RHI::RHIGraphicsPipelineDesc& desc)
		: m_programId(programId), m_vaoId(vaoId), m_desc(desc)
	{
	}

	OpenGLGraphicsPipeline::~OpenGLGraphicsPipeline()
	{
		if (m_vaoId != 0)
		{
			glDeleteVertexArrays(1, &m_vaoId);
		}
	}

	void OpenGLCommandBuffer::Execute()
	{
		// Execute pending draw call via OpenGL
		if (m_pendingDrawType == DrawType::Draw)
		{
			glDrawArrays(GL_TRIANGLES, static_cast<GLint>(m_pendingFirstVertex), static_cast<GLsizei>(m_pendingVertexCount));
		}
		else if (m_pendingDrawType == DrawType::DrawIndexed)
		{
			glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(m_pendingVertexCount), GL_UNSIGNED_INT, nullptr);
		}
		m_pendingDrawType = DrawType::None;
	}

	// ============================================================================
	// OpenGLCommandPool Implementation
	// ============================================================================

	OpenGLCommandPool::OpenGLCommandPool(void* platformWindow, RHI::QueueType queueType, std::string debugName)
		: m_platformWindow(platformWindow)
		, m_queueType(queueType)
		, m_debugName(std::move(debugName))
	{
	}

	std::shared_ptr<RHI::RHICommandBuffer> OpenGLCommandPool::CreateCommandBuffer(std::string debugName)
	{
		return std::make_shared<OpenGLCommandBuffer>(
			m_platformWindow,
			debugName.empty() ? m_debugName : debugName);
	}

	void OpenGLCommandPool::Reset()
	{
		// No-op for Tier B - legacy device doesn't have pool resets
	}

	// ============================================================================
	// OpenGLQueue Implementation
	// ============================================================================

	OpenGLQueue::OpenGLQueue(void* platformWindow, RHI::QueueType queueType, std::string debugName)
		: m_platformWindow(platformWindow)
		, m_queueType(queueType)
		, m_debugName(std::move(debugName))
	{
	}

	void OpenGLQueue::Submit(const RHI::RHISubmitDesc& submitDesc)
	{
		// Execute all command buffers via OpenGL
		for (const auto& cmdBuffer : submitDesc.commandBuffers)
		{
			if (cmdBuffer == nullptr)
				continue;
			auto* glCmdBuffer = dynamic_cast<OpenGLCommandBuffer*>(cmdBuffer.get());
			if (glCmdBuffer != nullptr)
			{
				glCmdBuffer->Execute();
			}
		}
	}

	void OpenGLQueue::Present(const RHI::RHIPresentDesc& presentDesc)
	{
		// Use NativeOpenGLSwapchain if provided, otherwise fall back to GLFW
		if (presentDesc.swapchain)
		{
			if (auto* nativeSwapchain = dynamic_cast<NativeOpenGLSwapchain*>(presentDesc.swapchain.get()))
			{
				nativeSwapchain->Present();
				return;
			}
		}

		// Fallback to GLFW swap
		if (m_platformWindow != nullptr)
		{
			glfwSwapBuffers(static_cast<GLFWwindow*>(m_platformWindow));
		}
	}

	// ============================================================================
	// OpenGL Formal RHI Resources
	// ============================================================================

	// ---------------------------------------------------------------------------
	// OpenGLTexture
	// ---------------------------------------------------------------------------

	OpenGLTexture::OpenGLTexture(uint32_t textureId, const RHI::RHITextureDesc& desc, std::string debugName)
		: m_textureId(textureId)
		, m_desc(desc)
		, m_debugName(std::move(debugName))
	{
	}

	OpenGLTexture::~OpenGLTexture()
	{
		if (m_textureId != 0)
		{
			GLuint id = m_textureId;
			glDeleteTextures(1, &id);
		}
	}

	RHI::NativeHandle OpenGLTexture::GetNativeImageHandle()
	{
		return { RHI::BackendType::OpenGL, reinterpret_cast<void*>(static_cast<uintptr_t>(m_textureId)) };
	}

	// ---------------------------------------------------------------------------
	// OpenGLBuffer
	// ---------------------------------------------------------------------------

	OpenGLBuffer::OpenGLBuffer(uint32_t bufferId, const RHI::RHIBufferDesc& desc, std::string debugName)
		: m_bufferId(bufferId)
		, m_desc(desc)
		, m_debugName(std::move(debugName))
	{
	}

	OpenGLBuffer::~OpenGLBuffer()
	{
		if (m_bufferId != 0)
		{
			GLuint id = m_bufferId;
			glDeleteBuffers(1, &id);
		}
	}

	RHI::NativeHandle OpenGLBuffer::GetNativeBufferHandle()
	{
		return { RHI::BackendType::OpenGL, reinterpret_cast<void*>(static_cast<uintptr_t>(m_bufferId)) };
	}

	// ---------------------------------------------------------------------------
	// OpenGLTextureView
	// ---------------------------------------------------------------------------

	OpenGLTextureView::OpenGLTextureView(const std::shared_ptr<RHI::RHITexture>& texture, const RHI::RHITextureViewDesc& desc)
		: m_texture(texture)
		, m_desc(desc)
		, m_debugName(desc.debugName.empty() ? "TextureView" : desc.debugName)
	{
	}

	OpenGLTextureView::~OpenGLTextureView()
	{
		// OpenGL doesn't have texture views that need explicit destruction
		// The underlying texture is owned by OpenGLTexture
	}

	RHI::NativeHandle OpenGLTextureView::GetNativeRenderTargetView()
	{
		// For OpenGL, there's no separate RTV - the texture itself is used
		if (m_texture == nullptr)
			return {};
		return m_texture->GetNativeImageHandle();
	}

	RHI::NativeHandle OpenGLTextureView::GetNativeDepthStencilView()
	{
		// For OpenGL, there's no separate DSV - the texture itself is used
		if (m_texture == nullptr)
			return {};
		return m_texture->GetNativeImageHandle();
	}

	RHI::NativeHandle OpenGLTextureView::GetNativeShaderResourceView()
	{
		// For OpenGL, there's no separate SRV - the texture itself is used for sampling
		if (m_texture == nullptr)
			return {};
		return m_texture->GetNativeImageHandle();
	}

	// ---------------------------------------------------------------------------
	// OpenGL Helper Functions
	// ---------------------------------------------------------------------------

	namespace
	{
		inline int32_t ToGLTextureFilter(RHI::TextureFilter filter)
		{
			switch (filter)
			{
			case RHI::TextureFilter::Nearest: return GL_NEAREST;
			case RHI::TextureFilter::Linear: return GL_LINEAR;
			default: return GL_LINEAR;
			}
		}

		inline int32_t ToGLTextureWrap(RHI::TextureWrap wrap)
		{
			switch (wrap)
			{
			case RHI::TextureWrap::ClampToEdge: return GL_CLAMP_TO_EDGE;
			case RHI::TextureWrap::Repeat: return GL_REPEAT;
			default: return GL_REPEAT;
			}
		}
	}

	// ---------------------------------------------------------------------------
	// OpenGLSampler
	// ---------------------------------------------------------------------------

	OpenGLSampler::OpenGLSampler(const RHI::SamplerDesc& desc, std::string debugName)
		: m_desc(desc)
		, m_debugName(std::move(debugName))
	{
		// Generate GL sampler
		glGenSamplers(1, &m_samplerId);
		if (m_samplerId == 0)
			return;

		// Set sampler parameters
		glSamplerParameteri(m_samplerId, GL_TEXTURE_MIN_FILTER, ToGLTextureFilter(desc.minFilter));
		glSamplerParameteri(m_samplerId, GL_TEXTURE_MAG_FILTER, ToGLTextureFilter(desc.magFilter));
		glSamplerParameteri(m_samplerId, GL_TEXTURE_WRAP_S, ToGLTextureWrap(desc.wrapU));
		glSamplerParameteri(m_samplerId, GL_TEXTURE_WRAP_T, ToGLTextureWrap(desc.wrapV));
	}

	OpenGLSampler::~OpenGLSampler()
	{
		if (m_samplerId != 0)
		{
			GLuint id = m_samplerId;
			glDeleteSamplers(1, &id);
		}
	}

	RHI::NativeHandle OpenGLSampler::GetNativeSamplerHandle()
	{
		return { RHI::BackendType::OpenGL, reinterpret_cast<void*>(static_cast<uintptr_t>(m_samplerId)) };
	}

	// ============================================================================
	// NativeOpenGLFence Implementation
	// ============================================================================

	NativeOpenGLFence::NativeOpenGLFence(const std::string& debugName)
		: m_debugName(debugName)
	{
		// Create OpenGL sync object
		m_sync = reinterpret_cast<void*>(glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0));
		if (m_sync == nullptr)
		{
			NLS_LOG_WARNING("NativeOpenGLFence: failed to create GL sync");
		}
	}

	NativeOpenGLFence::~NativeOpenGLFence()
	{
		if (m_sync != nullptr)
		{
			glDeleteSync(reinterpret_cast<GLsync>(m_sync));
			m_sync = nullptr;
		}
	}

	bool NativeOpenGLFence::IsSignaled() const
	{
		if (m_sync == nullptr)
			return true;

		GLint status;
		glGetSynciv(reinterpret_cast<GLsync>(m_sync), GL_SYNC_STATUS, sizeof(GLint), nullptr, &status);
		return status == GL_SIGNALED;
	}

	void NativeOpenGLFence::Reset()
	{
		if (m_sync != nullptr)
		{
			glDeleteSync(reinterpret_cast<GLsync>(m_sync));
			m_sync = nullptr;
		}
		m_sync = reinterpret_cast<void*>(glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0));
	}

	bool NativeOpenGLFence::Wait(uint64_t timeoutNanoseconds)
	{
		if (m_sync == nullptr)
			return true;

		GLenum result = glClientWaitSync(reinterpret_cast<GLsync>(m_sync), GL_SYNC_FLUSH_COMMANDS_BIT, static_cast<GLuint64>(timeoutNanoseconds / 1000000));
		return result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED;
	}

	// ============================================================================
	// NativeOpenGLSemaphore Implementation
	// ============================================================================

	NativeOpenGLSemaphore::NativeOpenGLSemaphore(const std::string& debugName)
		: m_debugName(debugName)
	{
		// Create manual-reset event (non-signaled initially)
		m_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		if (m_event == nullptr)
		{
			NLS_LOG_WARNING("NativeOpenGLSemaphore: failed to create event");
		}
	}

	NativeOpenGLSemaphore::~NativeOpenGLSemaphore()
	{
		if (m_event != nullptr)
		{
			CloseHandle(m_event);
			m_event = nullptr;
		}
	}

	bool NativeOpenGLSemaphore::IsSignaled() const
	{
		return m_signaled;
	}

	void NativeOpenGLSemaphore::Reset()
	{
		m_signaled = false;
		if (m_event != nullptr)
		{
			ResetEvent(m_event);
		}
	}

	void NativeOpenGLSemaphore::Signal()
	{
		m_signaled = true;
		if (m_event != nullptr)
		{
			SetEvent(m_event);
		}
	}

	// ============================================================================
	// NativeOpenGLSwapchain Implementation
	// ============================================================================

	NativeOpenGLSwapchain::NativeOpenGLSwapchain(void* glfwWindow, const RHI::SwapchainDesc& desc)
		: m_window(glfwWindow)
		, m_desc(desc)
		, m_debugName("Swapchain")
	{
	}

	NativeOpenGLSwapchain::~NativeOpenGLSwapchain()
	{
		// GLFW windows are managed externally, don't destroy here
		m_window = nullptr;
	}

	std::optional<RHI::RHIAcquiredImage> NativeOpenGLSwapchain::AcquireNextImage(
		const std::shared_ptr<RHI::RHISemaphore>& signalSemaphore,
		const std::shared_ptr<RHI::RHIFence>& signalFence)
	{
		// OpenGL immediate mode - no actual acquisition needed
		// Signal the semaphore/fence if provided
		if (signalSemaphore)
		{
			if (auto* nativeSem = dynamic_cast<NativeOpenGLSemaphore*>(signalSemaphore.get()))
			{
				nativeSem->Signal();
			}
		}

		if (signalFence)
		{
			signalFence->Reset();
		}

		RHI::RHIAcquiredImage image;
		image.imageIndex = m_currentImageIndex;
		image.suboptimal = false;

		return image;
	}

	void NativeOpenGLSwapchain::Resize(uint32_t width, uint32_t height)
	{
		m_desc.width = width;
		m_desc.height = height;
		// Note: GLFW doesn't support swapchain resize - the window must be recreated
		// This is a limitation of the OpenGL/GLFW approach
		NLS_LOG_WARNING("NativeOpenGLSwapchain: GLFW does not support swapchain resize, window recreation needed");
	}

	void NativeOpenGLSwapchain::Present()
	{
		if (m_window != nullptr)
		{
			glfwSwapBuffers(static_cast<GLFWwindow*>(m_window));
		}
	}

	// ============================================================================
	// OpenGLExplicitDevice Implementation
	// ============================================================================

	OpenGLExplicitDevice::OpenGLExplicitDevice(void* platformWindow)
		: m_platformWindow(platformWindow)
		, m_adapter(std::make_shared<OpenGLAdapter>("Unknown", "OpenGL"))
		, m_graphicsQueue(std::make_shared<OpenGLQueue>(platformWindow, RHI::QueueType::Graphics, "GraphicsQueue"))
	{
		// Note: GLAD initialization is deferred to SetPlatformWindow or CreateSwapchain
		// because the GLFW window is not available at construction time
		m_capabilities.supportsGraphics = true;
		m_capabilities.supportsSwapchain = true;

		NLS_LOG_INFO("OpenGLExplicitDevice: created formal RHI device (GLAD init deferred)");
	}

	void OpenGLExplicitDevice::InitializeGLAD()
	{
		// Check if GLAD is already initialized by checking if glGetString is available
		if (glGetString != nullptr && glGetString(GL_VENDOR) != nullptr)
		{
			// GLAD already initialized (likely by IRenderDevice)
			m_capabilities.supportsCompute = GLAD_GL_VERSION_4_3 != 0;
			NLS_LOG_INFO("OpenGLExplicitDevice: GLAD already initialized");
			return;
		}

		if (m_platformWindow == nullptr)
			return;

		glfwMakeContextCurrent(static_cast<GLFWwindow*>(m_platformWindow));
		if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
		{
			NLS_LOG_ERROR("OpenGLExplicitDevice: failed to initialize GLAD");
			return;
		}
		m_capabilities.supportsCompute = GLAD_GL_VERSION_4_3 != 0;
		NLS_LOG_INFO("OpenGLExplicitDevice: GLAD initialized successfully");
	}

	OpenGLExplicitDevice::~OpenGLExplicitDevice() = default;

	const RHI::RHIDeviceCapabilities& OpenGLExplicitDevice::GetCapabilities() const
	{
		return m_capabilities;
	}

	RHI::NativeRenderDeviceInfo OpenGLExplicitDevice::GetNativeDeviceInfo() const
	{
		RHI::NativeRenderDeviceInfo info = {};
		info.backend = RHI::NativeBackendType::OpenGL;
		info.device = nullptr;
		info.graphicsQueue = m_platformWindow;  // Use GLFW window as queue representation
		return info;
	}

	bool OpenGLExplicitDevice::IsBackendReady() const
	{
		return true;  // Legacy device is already initialized
	}

	std::shared_ptr<RHI::RHIQueue> OpenGLExplicitDevice::GetQueue(RHI::QueueType queueType)
	{
		if (queueType == RHI::QueueType::Graphics)
		{
			return m_graphicsQueue;
		}
		return nullptr;
	}

	std::shared_ptr<RHI::RHISwapchain> OpenGLExplicitDevice::CreateSwapchain(const RHI::SwapchainDesc& desc)
	{
		// Initialize GLAD if not already done
		if (desc.platformWindow != nullptr)
		{
			SetPlatformWindow(desc.platformWindow);
		}
		InitializeGLAD();

		// Create formal OpenGL swapchain wrapping GLFW window
		return std::make_shared<NativeOpenGLSwapchain>(desc.platformWindow, desc);
	}

	std::shared_ptr<RHI::RHIBuffer> OpenGLExplicitDevice::CreateBuffer(const RHI::RHIBufferDesc& desc, const void* initialData)
	{
		// Create GL buffer
		GLuint bufferId = 0;
		glGenBuffers(1, &bufferId);
		if (bufferId == 0)
			return nullptr;

		// Determine GL target based on buffer usage flags
		GLenum target = GL_ARRAY_BUFFER;
		if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(RHI::BufferUsageFlags::Index))
			target = GL_ELEMENT_ARRAY_BUFFER;
		else if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(RHI::BufferUsageFlags::Uniform))
			target = GL_UNIFORM_BUFFER;
		else if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(RHI::BufferUsageFlags::Storage))
			target = GL_SHADER_STORAGE_BUFFER;

		// Bind and set data
		glBindBuffer(target, bufferId);
		GLenum glUsage = (desc.memoryUsage == RHI::MemoryUsage::GPUOnly) ? GL_STATIC_DRAW : GL_DYNAMIC_DRAW;
		glBufferData(target, desc.size, initialData, glUsage);
		glBindBuffer(target, 0);

		auto debugNameStr = desc.debugName.empty() ? "Buffer" : desc.debugName;
		return std::make_shared<OpenGLBuffer>(bufferId, desc, debugNameStr);
	}

	std::shared_ptr<RHI::RHITexture> OpenGLExplicitDevice::CreateTexture(const RHI::RHITextureDesc& desc, const void* initialData)
	{
		// Create GL texture
		GLuint textureId = 0;
		glGenTextures(1, &textureId);
		if (textureId == 0)
			return nullptr;

		// Bind texture and set parameters
		GLenum target = GL_TEXTURE_2D;
		if (desc.dimension == RHI::TextureDimension::TextureCube)
			target = GL_TEXTURE_CUBE_MAP;

		glBindTexture(target, textureId);

		// Set texture parameters
		glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		// Allocate texture storage
		GLint internalFormat = GL_RGBA8;
		switch (desc.format)
		{
		case RHI::TextureFormat::RGB8: internalFormat = GL_RGB8; break;
		case RHI::TextureFormat::RGBA8: internalFormat = GL_RGBA8; break;
		case RHI::TextureFormat::RGBA16F: internalFormat = GL_RGBA16F; break;
		case RHI::TextureFormat::Depth24Stencil8: internalFormat = GL_DEPTH24_STENCIL8; break;
		}

		GLenum glFormat = (desc.format == RHI::TextureFormat::Depth24Stencil8) ? GL_DEPTH_STENCIL : GL_RGBA;
		GLenum glType = (desc.format == RHI::TextureFormat::RGBA16F) ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE;

		glTexImage2D(target, 0, internalFormat, desc.extent.width, desc.extent.height, 0, glFormat, glType, initialData);

		// Generate mipmaps if mip levels > 1
		if (desc.mipLevels > 1)
		{
			glGenerateMipmap(target);
		}

		glBindTexture(target, 0);

		auto debugNameStr = desc.debugName.empty() ? "Texture" : desc.debugName;
		return std::make_shared<OpenGLTexture>(textureId, desc, debugNameStr);
	}

	std::shared_ptr<RHI::RHITextureView> OpenGLExplicitDevice::CreateTextureView(
		const std::shared_ptr<RHI::RHITexture>& texture, const RHI::RHITextureViewDesc& desc)
	{
		if (texture == nullptr)
			return nullptr;
		return std::make_shared<OpenGLTextureView>(texture, desc);
	}

	std::shared_ptr<RHI::RHISampler> OpenGLExplicitDevice::CreateSampler(const RHI::SamplerDesc& desc, std::string debugName)
	{
		auto debugNameStr = debugName.empty() ? "Sampler" : debugName;
		return std::make_shared<OpenGLSampler>(desc, debugNameStr);
	}

	std::shared_ptr<RHI::RHIBindingLayout> OpenGLExplicitDevice::CreateBindingLayout(const RHI::RHIBindingLayoutDesc& desc)
	{
		// Tier B - no formal binding layout implementation
		return nullptr;
	}

	std::shared_ptr<RHI::RHIBindingSet> OpenGLExplicitDevice::CreateBindingSet(const RHI::RHIBindingSetDesc& desc)
	{
		// Tier B - no formal binding set implementation
		return nullptr;
	}

	std::shared_ptr<RHI::RHIPipelineLayout> OpenGLExplicitDevice::CreatePipelineLayout(const RHI::RHIPipelineLayoutDesc& desc)
	{
		// Tier B - no formal pipeline layout implementation
		return nullptr;
	}

	std::shared_ptr<RHI::RHIShaderModule> OpenGLExplicitDevice::CreateShaderModule(const RHI::RHIShaderModuleDesc& desc)
	{
		if (desc.bytecode.empty())
		{
			NLS_LOG_WARNING("OpenGLExplicitDevice::CreateShaderModule: empty bytecode");
			return nullptr;
		}

		// GLSL source code is stored as bytecode in our pipeline
		// The bytecode is actually ASCII GLSL source
		const char* source = reinterpret_cast<const char*>(desc.bytecode.data());
		size_t sourceLen = desc.bytecode.size();

		// Determine shader type based on stage
		GLenum shaderType;
		switch (desc.stage)
		{
		case RHI::ShaderStage::Vertex:
			shaderType = GL_VERTEX_SHADER;
			break;
		case RHI::ShaderStage::Fragment:
			shaderType = GL_FRAGMENT_SHADER;
			break;
		default:
			NLS_LOG_WARNING("OpenGLExplicitDevice::CreateShaderModule: unsupported shader stage");
			return nullptr;
		}

		// Compile vertex shader
		GLuint shader = glCreateShader(shaderType);
		glShaderSource(shader, 1, &source, nullptr);
		glCompileShader(shader);

		GLint compiled = 0;
		glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
		if (!compiled)
		{
			GLint infoLen = 0;
			glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
			if (infoLen > 1)
			{
				std::vector<char> infoLog(infoLen);
				glGetShaderInfoLog(shader, infoLen, nullptr, infoLog.data());
				NLS_LOG_WARNING("OpenGLExplicitDevice::CreateShaderModule: shader compile error: {}", infoLog.data());
			}
			glDeleteShader(shader);
			return nullptr;
		}

		// Create program and attach shader
		GLuint program = glCreateProgram();
		glAttachShader(program, shader);
		glLinkProgram(program);

		// Shader can be deleted after linking
		glDeleteShader(shader);

		GLint linked = 0;
		glGetProgramiv(program, GL_LINK_STATUS, &linked);
		if (!linked)
		{
			GLint infoLen = 0;
			glGetProgramiv(program, GL_INFO_LOG_LENGTH, &infoLen);
			if (infoLen > 1)
			{
				std::vector<char> infoLog(infoLen);
				glGetProgramInfoLog(program, infoLen, nullptr, infoLog.data());
				NLS_LOG_WARNING("OpenGLExplicitDevice::CreateShaderModule: program link error: {}", infoLog.data());
			}
			glDeleteProgram(program);
			return nullptr;
		}

		return std::make_shared<OpenGLShaderModule>(program, desc);
	}

	std::shared_ptr<RHI::RHIGraphicsPipeline> OpenGLExplicitDevice::CreateGraphicsPipeline(const RHI::RHIGraphicsPipelineDesc& desc)
	{
		if (desc.vertexShader == nullptr || desc.fragmentShader == nullptr)
		{
			NLS_LOG_WARNING("OpenGLExplicitDevice::CreateGraphicsPipeline: missing shaders");
			return nullptr;
		}

		auto* vsModule = dynamic_cast<OpenGLShaderModule*>(desc.vertexShader.get());
		auto* fsModule = dynamic_cast<OpenGLShaderModule*>(desc.fragmentShader.get());
		if (vsModule == nullptr || fsModule == nullptr)
		{
			NLS_LOG_WARNING("OpenGLExplicitDevice::CreateGraphicsPipeline: invalid shader module types");
			return nullptr;
		}

		// Create VAO for this pipeline
		GLuint vao = 0;
		glGenVertexArrays(1, &vao);
		if (vao == 0)
		{
			NLS_LOG_WARNING("OpenGLExplicitDevice::CreateGraphicsPipeline: failed to create VAO");
			return nullptr;
		}

		glBindVertexArray(vao);

		// Setup vertex attributes based on the pipeline's vertex attributes
		// Note: We assume the vertex buffer is already bound via BindVertexBuffer
		// The actual VBO binding happens at draw time

		glBindVertexArray(0);

		return std::make_shared<OpenGLGraphicsPipeline>(vsModule->GetProgramId(), vao, desc);
	}

	std::shared_ptr<RHI::RHIComputePipeline> OpenGLExplicitDevice::CreateComputePipeline(const RHI::RHIComputePipelineDesc& desc)
	{
		// Tier B - no formal compute pipeline implementation
		return nullptr;
	}

	std::shared_ptr<RHI::RHICommandPool> OpenGLExplicitDevice::CreateCommandPool(RHI::QueueType queueType, std::string debugName)
	{
		return std::make_shared<OpenGLCommandPool>(
			m_platformWindow,
			queueType,
			debugName.empty() ? "CommandPool" : debugName);
	}

	std::shared_ptr<RHI::RHIFence> OpenGLExplicitDevice::CreateFence(std::string debugName)
	{
		return std::make_shared<NativeOpenGLFence>(debugName.empty() ? "Fence" : debugName);
	}

	std::shared_ptr<RHI::RHISemaphore> OpenGLExplicitDevice::CreateSemaphore(std::string debugName)
	{
		return std::make_shared<NativeOpenGLSemaphore>(debugName.empty() ? "Semaphore" : debugName);
	}

	void OpenGLExplicitDevice::ReadPixels(
	    const std::shared_ptr<RHI::RHITexture>& texture,
	    uint32_t x,
	    uint32_t y,
	    uint32_t width,
	    uint32_t height,
	    NLS::Render::Settings::EPixelDataFormat format,
	    NLS::Render::Settings::EPixelDataType type,
	    void* data)
	{
		if (texture == nullptr || data == nullptr || width == 0 || height == 0)
			return;

		// Get the OpenGL texture ID from the RHITexture
		auto glTexture = dynamic_cast<OpenGLTexture*>(texture.get());
		if (glTexture == nullptr)
			return;

		uint32_t textureId = glTexture->GetTextureId();
		if (textureId == 0)
			return;

		// Save current state
		GLint currentFBO = 0;
		GLint currentReadFBO = 0;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &currentReadFBO);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, textureId);

		// Determine OpenGL format and type
		GLenum glFormat = GL_RGBA;
		GLenum glType = GL_UNSIGNED_BYTE;
		uint32_t bytesPerPixel = 4;

		switch (format)
		{
		case NLS::Render::Settings::EPixelDataFormat::RGB:
			glFormat = GL_RGB;
			bytesPerPixel = 3;
			break;
		case NLS::Render::Settings::EPixelDataFormat::RGBA:
			glFormat = GL_RGBA;
			bytesPerPixel = 4;
			break;
		case NLS::Render::Settings::EPixelDataFormat::DEPTH_COMPONENT:
			glFormat = GL_DEPTH_COMPONENT;
			bytesPerPixel = 4;
			break;
		default:
			glFormat = GL_RGBA;
			bytesPerPixel = 4;
			break;
		}

		switch (type)
		{
		case NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE:
			glType = GL_UNSIGNED_BYTE;
			break;
		case NLS::Render::Settings::EPixelDataType::FLOAT:
			glType = GL_FLOAT;
			bytesPerPixel = (format == NLS::Render::Settings::EPixelDataFormat::RGB) ? 12 : 16;
			break;
		default:
			glType = GL_UNSIGNED_BYTE;
			break;
		}

		// Read pixels
		glReadPixels(x, y, width, height, glFormat, glType, data);

		// Restore previous framebuffer state
		glBindFramebuffer(GL_READ_FRAMEBUFFER, currentReadFBO);
	}

	bool OpenGLExplicitDevice::PrepareUIRender()
	{
		// OpenGL - UI rendering just needs to bind default framebuffer
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		return true;
	}

	void OpenGLExplicitDevice::ReleaseUITextureHandles()
	{
		// No-op for OpenGL explicit device
	}

	std::shared_ptr<RHI::RHIDevice> CreateOpenGLRhiDevice(void* platformWindow)
	{
		// OpenGL Tier A - direct creation with GLFW window
		NLS_LOG_INFO("CreateOpenGLRhiDevice: creating OpenGL formal RHI device");
		return std::make_shared<OpenGLExplicitDevice>(platformWindow);
	}

}
