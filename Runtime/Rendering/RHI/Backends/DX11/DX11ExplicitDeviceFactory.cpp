#include "Rendering/RHI/Backends/DX11/DX11ExplicitDeviceFactory.h"

#if defined(_WIN32)
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif

#include "Debug/Logger.h"
#include "Rendering/Data/PipelineState.h"

namespace NLS::Render::Backend
{
	namespace
	{
		class DX11Adapter final : public RHI::RHIAdapter
		{
		public:
			DX11Adapter(const std::string& vendor, const std::string& hardware)
				: m_vendor(vendor), m_hardware(hardware)
			{}

			std::string_view GetDebugName() const override { return "DX11Adapter"; }
			RHI::NativeBackendType GetBackendType() const override { return RHI::NativeBackendType::DX11; }
			std::string_view GetVendor() const override { return m_vendor; }
			std::string_view GetHardware() const override { return m_hardware; }

		private:
			std::string m_vendor;
			std::string m_hardware;
		};
	}

	// ============================================================================
	// DX11 Helper Functions
	// ============================================================================

	namespace
	{
		DXGI_FORMAT ToDxgiFormat(NLS::Render::RHI::TextureFormat format)
		{
			switch (format)
			{
			case NLS::Render::RHI::TextureFormat::RGB8: return DXGI_FORMAT_R8G8B8A8_UNORM;
			case NLS::Render::RHI::TextureFormat::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
			case NLS::Render::RHI::TextureFormat::RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
			case NLS::Render::RHI::TextureFormat::Depth24Stencil8: return DXGI_FORMAT_D24_UNORM_S8_UINT;
			default: return DXGI_FORMAT_R8G8B8A8_UNORM;
			}
		}

		D3D11_COMPARISON_FUNC ToD3D11ComparisonFunc(NLS::Render::Settings::EComparaisonAlgorithm algorithm)
		{
			switch (algorithm)
			{
			case NLS::Render::Settings::EComparaisonAlgorithm::NEVER: return D3D11_COMPARISON_NEVER;
			case NLS::Render::Settings::EComparaisonAlgorithm::LESS: return D3D11_COMPARISON_LESS;
			case NLS::Render::Settings::EComparaisonAlgorithm::EQUAL: return D3D11_COMPARISON_EQUAL;
			case NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL: return D3D11_COMPARISON_LESS_EQUAL;
			case NLS::Render::Settings::EComparaisonAlgorithm::GREATER: return D3D11_COMPARISON_GREATER;
			case NLS::Render::Settings::EComparaisonAlgorithm::NOTEQUAL: return D3D11_COMPARISON_NOT_EQUAL;
			case NLS::Render::Settings::EComparaisonAlgorithm::GREATER_EQUAL: return D3D11_COMPARISON_GREATER_EQUAL;
			case NLS::Render::Settings::EComparaisonAlgorithm::ALWAYS:
			default:
				return D3D11_COMPARISON_ALWAYS;
			}
		}

		D3D11_STENCIL_OP ToD3D11StencilOp(NLS::Render::Settings::EOperation operation)
		{
			switch (operation)
			{
			case NLS::Render::Settings::EOperation::KEEP: return D3D11_STENCIL_OP_KEEP;
			case NLS::Render::Settings::EOperation::ZERO: return D3D11_STENCIL_OP_ZERO;
			case NLS::Render::Settings::EOperation::REPLACE: return D3D11_STENCIL_OP_REPLACE;
			case NLS::Render::Settings::EOperation::INCREMENT: return D3D11_STENCIL_OP_INCR_SAT;
			case NLS::Render::Settings::EOperation::INCREMENT_WRAP: return D3D11_STENCIL_OP_INCR;
			case NLS::Render::Settings::EOperation::DECREMENT: return D3D11_STENCIL_OP_DECR_SAT;
			case NLS::Render::Settings::EOperation::DECREMENT_WRAP: return D3D11_STENCIL_OP_DECR;
			case NLS::Render::Settings::EOperation::INVERT: return D3D11_STENCIL_OP_INVERT;
			default: return D3D11_STENCIL_OP_KEEP;
			}
		}
	} // anonymous namespace

	// ============================================================================
	// DX11ShaderModule Implementation
	// ============================================================================

	DX11ShaderModule::DX11ShaderModule(
		Microsoft::WRL::ComPtr<ID3D11VertexShader> vs,
		Microsoft::WRL::ComPtr<ID3D11PixelShader> ps,
		Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout,
		const RHI::RHIShaderModuleDesc& desc)
		: m_vertexShader(vs)
		, m_pixelShader(ps)
		, m_inputLayout(inputLayout)
		, m_desc(desc)
	{
	}

	// ============================================================================
	// DX11GraphicsPipeline Implementation
	// ============================================================================

	DX11GraphicsPipeline::DX11GraphicsPipeline(
		Microsoft::WRL::ComPtr<ID3D11VertexShader> vs,
		Microsoft::WRL::ComPtr<ID3D11PixelShader> ps,
		Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout,
		Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerState,
		Microsoft::WRL::ComPtr<ID3D11BlendState> blendState,
		Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthStencilState,
		const RHI::RHIGraphicsPipelineDesc& desc)
		: m_vertexShader(vs)
		, m_pixelShader(ps)
		, m_inputLayout(inputLayout)
		, m_rasterizerState(rasterizerState)
		, m_blendState(blendState)
		, m_depthStencilState(depthStencilState)
		, m_desc(desc)
	{
		m_stencilRef = desc.depthStencilState.stencilReference;
	}

	// ============================================================================
	// DX11CommandBuffer Implementation - Direct D3D11 recording
	// ============================================================================

	DX11CommandBuffer::DX11CommandBuffer(Microsoft::WRL::ComPtr<ID3D11Device> device, Microsoft::WRL::ComPtr<ID3D11DeviceContext> context, std::string debugName)
		: m_device(device)
		, m_context(context)
		, m_debugName(std::move(debugName))
	{
	}

	void DX11CommandBuffer::Begin()
	{
		m_recording = true;
		Reset();
	}

	void DX11CommandBuffer::End()
	{
		m_recording = false;
		Execute();
	}

	void DX11CommandBuffer::Reset()
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

	void DX11CommandBuffer::BeginRenderPass(const RHI::RHIRenderPassDesc& desc)
	{
		if (m_context == nullptr)
			return;

		// Collect render target views
		std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> rtvHandles;
		Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsvHandle;

		for (const auto& colorAtt : desc.colorAttachments)
		{
			if (colorAtt.view != nullptr)
			{
				auto nativeHandle = colorAtt.view->GetNativeRenderTargetView();
				if (nativeHandle.backend == RHI::BackendType::DX11 && nativeHandle.handle != nullptr)
				{
					rtvHandles.push_back(static_cast<ID3D11RenderTargetView*>(nativeHandle.handle));
				}
			}
		}

		// Get depth stencil view
		if (desc.depthStencilAttachment.has_value() && desc.depthStencilAttachment->view != nullptr)
		{
			auto nativeHandle = desc.depthStencilAttachment->view->GetNativeDepthStencilView();
			if (nativeHandle.backend == RHI::BackendType::DX11 && nativeHandle.handle != nullptr)
			{
				dsvHandle = static_cast<ID3D11DepthStencilView*>(nativeHandle.handle);
			}
		}

		// Set render targets
		if (!rtvHandles.empty() || dsvHandle != nullptr)
		{
			std::vector<ID3D11RenderTargetView*> rtvPtrs;
			for (auto& rtv : rtvHandles)
				rtvPtrs.push_back(rtv.Get());

			m_context->OMSetRenderTargets(
				static_cast<UINT>(rtvPtrs.size()),
				rtvPtrs.empty() ? nullptr : rtvPtrs.data(),
				dsvHandle.Get());
		}

		// Handle clear if needed
		for (size_t i = 0; i < desc.colorAttachments.size() && i < rtvHandles.size(); ++i)
		{
			if (desc.colorAttachments[i].loadOp == RHI::LoadOp::Clear)
			{
				const auto& cv = desc.colorAttachments[i].clearValue;
				float color[4] = {cv.r, cv.g, cv.b, cv.a};
				m_context->ClearRenderTargetView(
					rtvHandles[i].Get(),
					color);
			}
		}

		if (dsvHandle != nullptr && desc.depthStencilAttachment.has_value())
		{
			const auto& dsAtt = desc.depthStencilAttachment.value();
			if (dsAtt.depthLoadOp == RHI::LoadOp::Clear)
			{
				m_context->ClearDepthStencilView(
					dsvHandle.Get(),
					D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
					dsAtt.clearValue.depth,
					static_cast<UINT8>(dsAtt.clearValue.stencil));
			}
		}
	}

	void DX11CommandBuffer::EndRenderPass()
	{
		// DX11 end render pass - unbind render targets
	}

	void DX11CommandBuffer::SetViewport(const RHI::RHIViewport& viewport)
	{
		m_viewport = viewport;
		m_viewportSet = true;
		if (m_context)
		{
			D3D11_VIEWPORT vp = {};
			vp.TopLeftX = viewport.x;
			vp.TopLeftY = viewport.y;
			vp.Width = viewport.width;
			vp.Height = viewport.height;
			vp.MinDepth = viewport.minDepth;
			vp.MaxDepth = viewport.maxDepth;
			m_context->RSSetViewports(1, &vp);
		}
	}

	void DX11CommandBuffer::SetScissor(const RHI::RHIRect2D& rect)
	{
		m_scissor = rect;
		m_scissorSet = true;
		if (m_context)
		{
			D3D11_RECT scissor = { rect.x, rect.y, static_cast<LONG>(rect.x + rect.width), static_cast<LONG>(rect.y + rect.height) };
			m_context->RSSetScissorRects(1, &scissor);
		}
	}

	void DX11CommandBuffer::BindGraphicsPipeline(const std::shared_ptr<RHI::RHIGraphicsPipeline>& pipeline)
	{
		m_graphicsPipeline = pipeline;

		if (pipeline == nullptr || m_context == nullptr)
			return;

		auto* nativePipeline = dynamic_cast<DX11GraphicsPipeline*>(pipeline.get());
		if (nativePipeline == nullptr)
		{
			NLS_LOG_WARNING("DX11CommandBuffer::BindGraphicsPipeline: invalid pipeline type");
			return;
		}

		// Bind shaders
		if (nativePipeline->GetVertexShader())
			m_context->VSSetShader(nativePipeline->GetVertexShader(), nullptr, 0);
		if (nativePipeline->GetPixelShader())
			m_context->PSSetShader(nativePipeline->GetPixelShader(), nullptr, 0);

		// Bind input layout
		if (nativePipeline->GetInputLayout())
			m_context->IASetInputLayout(nativePipeline->GetInputLayout());

		// Bind rasterizer state
		if (nativePipeline->GetRasterizerState())
			m_context->RSSetState(nativePipeline->GetRasterizerState());

		// Bind blend state
		if (nativePipeline->GetBlendState())
		{
			float blendFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
			m_context->OMSetBlendState(nativePipeline->GetBlendState(), blendFactor, 0xFFFFFFFF);
		}

		// Bind depth stencil state
		if (nativePipeline->GetDepthStencilState())
			m_context->OMSetDepthStencilState(nativePipeline->GetDepthStencilState(), nativePipeline->GetStencilRef());
	}

	void DX11CommandBuffer::BindComputePipeline(const std::shared_ptr<RHI::RHIComputePipeline>& pipeline)
	{
		// Compute not supported in DX11
	}

	void DX11CommandBuffer::BindBindingSet(uint32_t setIndex, const std::shared_ptr<RHI::RHIBindingSet>& bindingSet)
	{
		m_bindingSet = bindingSet;
		// TODO: Update descriptor tables
	}

	void DX11CommandBuffer::PushConstants(RHI::ShaderStageMask stageMask, uint32_t offset, uint32_t size, const void* data)
	{
		// TODO: Use dynamic buffers for push constants
	}

	void DX11CommandBuffer::BindVertexBuffer(uint32_t slot, const RHI::RHIVertexBufferView& view)
	{
		m_vertexBufferView = view;
		m_vertexBufferBound = true;
		if (m_context && view.buffer)
		{
			ID3D11Buffer* buffers[] = { static_cast<ID3D11Buffer*>(view.buffer->GetNativeBufferHandle().handle) };
			UINT strides[] = { view.stride };
			UINT offsets[] = { static_cast<UINT>(view.offset) };
			m_context->IASetVertexBuffers(slot, 1, buffers, strides, offsets);
		}
	}

	void DX11CommandBuffer::BindIndexBuffer(const RHI::RHIIndexBufferView& view)
	{
		m_indexBufferView = view;
		m_indexBufferBound = true;
		if (m_context && view.buffer)
		{
			DXGI_FORMAT format = (view.indexType == RHI::IndexType::UInt16) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
			ID3D11Buffer* buffer = static_cast<ID3D11Buffer*>(view.buffer->GetNativeBufferHandle().handle);
			m_context->IASetIndexBuffer(buffer, format, static_cast<UINT>(view.offset));
		}
	}

	void DX11CommandBuffer::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
	{
		if (!m_recording)
		{
			ExecuteDirectDraw(vertexCount, instanceCount, firstVertex, firstInstance);
			return;
		}

		m_pendingDrawType = DrawType::Draw;
		m_pendingVertexCount = vertexCount;
		m_pendingInstanceCount = instanceCount;
		m_pendingFirstVertex = firstVertex;
		m_pendingFirstInstance = firstInstance;
	}

	void DX11CommandBuffer::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
	{
		if (!m_recording)
		{
			ExecuteDirectDrawIndexed(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
			return;
		}

		m_pendingDrawType = DrawType::DrawIndexed;
		m_pendingVertexCount = indexCount;
		m_pendingInstanceCount = instanceCount;
		m_pendingFirstIndex = firstIndex;
		m_pendingVertexOffset = vertexOffset;
		m_pendingFirstInstance = firstInstance;
	}

	void DX11CommandBuffer::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
	{
		// Compute not supported in DX11
	}

	void DX11CommandBuffer::CopyBuffer(const std::shared_ptr<RHI::RHIBuffer>& source, const std::shared_ptr<RHI::RHIBuffer>& destination, const RHI::RHIBufferCopyRegion& region)
	{
		if (!m_context)
			return;

		D3D11_BOX srcBox = {};
		srcBox.left = static_cast<UINT>(region.srcOffset);
		srcBox.right = srcBox.left + static_cast<UINT>(region.size);
		srcBox.front = 0;
		srcBox.back = 1;
		srcBox.top = 0;
		srcBox.bottom = 1;

		ID3D11Buffer* srcBuffer = static_cast<ID3D11Buffer*>(source->GetNativeBufferHandle().handle);
		ID3D11Buffer* dstBuffer = static_cast<ID3D11Buffer*>(destination->GetNativeBufferHandle().handle);

		m_context->CopySubresourceRegion(dstBuffer, 0, static_cast<UINT>(region.dstOffset), 0, 0, srcBuffer, 0, &srcBox);
	}

	void DX11CommandBuffer::CopyBufferToTexture(const RHI::RHIBufferToTextureCopyDesc& desc)
	{
		if (!m_context)
			return;

		ID3D11Buffer* buffer = static_cast<ID3D11Buffer*>(desc.source->GetNativeBufferHandle().handle);
		ID3D11Texture2D* texture = static_cast<ID3D11Texture2D*>(desc.destination->GetNativeImageHandle().handle);

		D3D11_BOX srcBox = {};
		srcBox.left = static_cast<UINT>(desc.bufferOffset);
		srcBox.right = srcBox.left + desc.extent.width * 4;  // Assuming RGBA
		srcBox.front = 0;
		srcBox.back = 1;
		srcBox.top = 0;
		srcBox.bottom = desc.extent.height;

		m_context->CopySubresourceRegion(texture, D3D11CalcSubresource(desc.mipLevel, desc.arrayLayer, 1),
			desc.textureOffset.x, desc.textureOffset.y, desc.textureOffset.z,
			buffer, 0, &srcBox);
	}

	void DX11CommandBuffer::CopyTexture(const RHI::RHITextureCopyDesc& desc)
	{
		if (!m_context)
			return;

		ID3D11Texture2D* srcTexture = static_cast<ID3D11Texture2D*>(desc.source->GetNativeImageHandle().handle);
		ID3D11Texture2D* dstTexture = static_cast<ID3D11Texture2D*>(desc.destination->GetNativeImageHandle().handle);

		D3D11_TEXTURE2D_DESC srcDesc;
		srcTexture->GetDesc(&srcDesc);

		D3D11_BOX srcBox = {};
		srcBox.left = desc.sourceOffset.x;
		srcBox.top = desc.sourceOffset.y;
		srcBox.front = desc.sourceOffset.z;
		srcBox.right = srcBox.left + desc.extent.width;
		srcBox.bottom = srcBox.top + desc.extent.height;
		srcBox.back = srcBox.front + desc.extent.depth;

		UINT srcSubresource = D3D11CalcSubresource(desc.sourceRange.baseMipLevel, desc.sourceRange.baseArrayLayer, srcDesc.MipLevels);

		m_context->CopySubresourceRegion(dstTexture,
			D3D11CalcSubresource(desc.destinationRange.baseMipLevel, desc.destinationRange.baseArrayLayer, 1),
			desc.destinationOffset.x, desc.destinationOffset.y, desc.destinationOffset.z,
			srcTexture, srcSubresource, &srcBox);
	}

	void DX11CommandBuffer::Barrier(const RHI::RHIBarrierDesc& barrier)
	{
		// DX11 doesn't have explicit barriers - resource transitions handled via OMBlendState
	}

	void DX11CommandBuffer::ExecuteDirectDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
	{
		if (m_context)
		{
			if (instanceCount > 1)
				m_context->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
			else
				m_context->Draw(vertexCount, firstVertex);
		}
	}

	void DX11CommandBuffer::ExecuteDirectDrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
	{
		if (m_context)
		{
			if (instanceCount > 1)
				m_context->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
			else
				m_context->DrawIndexed(indexCount, firstIndex, vertexOffset);
		}
	}

	void DX11CommandBuffer::Execute()
	{
		if (m_pendingDrawType == DrawType::Draw)
		{
			ExecuteDirectDraw(m_pendingVertexCount, m_pendingInstanceCount, m_pendingFirstVertex, m_pendingFirstInstance);
		}
		else if (m_pendingDrawType == DrawType::DrawIndexed)
		{
			ExecuteDirectDrawIndexed(m_pendingVertexCount, m_pendingInstanceCount, m_pendingFirstIndex, m_pendingVertexOffset, m_pendingFirstInstance);
		}
		m_pendingDrawType = DrawType::None;
	}

	// ============================================================================
	// DX11CommandPool Implementation
	// ============================================================================

	DX11CommandPool::DX11CommandPool(Microsoft::WRL::ComPtr<ID3D11Device> device, Microsoft::WRL::ComPtr<ID3D11DeviceContext> context, RHI::QueueType queueType, std::string debugName)
		: m_device(device)
		, m_context(context)
		, m_queueType(queueType)
		, m_debugName(std::move(debugName))
	{
	}

	std::shared_ptr<RHI::RHICommandBuffer> DX11CommandPool::CreateCommandBuffer(std::string debugName)
	{
		return std::make_shared<DX11CommandBuffer>(
			m_device,
			m_context,
			debugName.empty() ? m_debugName : debugName);
	}

	void DX11CommandPool::Reset()
	{
		// No-op for DX11 - immediate context doesn't need pool reset
	}

	// ============================================================================
	// DX11Queue Implementation
	// ============================================================================

	DX11Queue::DX11Queue(Microsoft::WRL::ComPtr<ID3D11DeviceContext> context, Microsoft::WRL::ComPtr<IDXGISwapChain> swapchain, RHI::QueueType queueType, std::string debugName)
		: m_context(context)
		, m_swapchain(swapchain)
		, m_queueType(queueType)
		, m_debugName(std::move(debugName))
	{
	}

	void DX11Queue::Submit(const RHI::RHISubmitDesc& submitDesc)
	{
		// Execute all command buffers
		for (const auto& cmdBuffer : submitDesc.commandBuffers)
		{
			if (cmdBuffer == nullptr)
				continue;
			auto* dxCmdBuffer = dynamic_cast<DX11CommandBuffer*>(cmdBuffer.get());
			if (dxCmdBuffer != nullptr)
			{
				dxCmdBuffer->Execute();
			}
		}
	}

	void DX11Queue::Present(const RHI::RHIPresentDesc& presentDesc)
	{
		// Try to use NativeDX11Swapchain if provided
		if (presentDesc.swapchain)
		{
			if (auto* nativeSwapchain = dynamic_cast<NativeDX11Swapchain*>(presentDesc.swapchain.get()))
			{
				UINT syncInterval = presentDesc.waitSemaphores.empty() ? 1 : 0;
				UINT presentFlags = 0;
				nativeSwapchain->GetSwapchain()->Present(syncInterval, presentFlags);
				return;
			}
		}

		// Fallback to member swapchain
		if (m_swapchain)
		{
			UINT syncInterval = presentDesc.waitSemaphores.empty() ? 1 : 0;
			UINT presentFlags = 0;
			m_swapchain->Present(syncInterval, presentFlags);
		}
	}

	// ============================================================================
	// DX11Buffer Implementation
	// ============================================================================

	DX11Buffer::DX11Buffer(Microsoft::WRL::ComPtr<ID3D11Buffer> buffer, const NLS::Render::RHI::RHIBufferDesc& desc)
		: m_buffer(buffer), m_desc(desc)
	{
	}

	// ============================================================================
	// DX11Texture Implementation
	// ============================================================================

	DX11Texture::DX11Texture(Microsoft::WRL::ComPtr<ID3D11Texture2D> texture, const NLS::Render::RHI::RHITextureDesc& desc)
		: m_texture(texture), m_desc(desc)
	{
	}

	// ============================================================================
	// DX11TextureView Implementation
	// ============================================================================

	DX11TextureView::DX11TextureView(
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv,
		Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv,
		Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv,
		const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
		const NLS::Render::RHI::RHITextureViewDesc& desc)
		: m_srv(srv), m_rtv(rtv), m_dsv(dsv), m_texture(texture), m_desc(desc)
	{
	}

	// ============================================================================
	// NativeDX11Fence Implementation
	// ============================================================================

	NativeDX11Fence::NativeDX11Fence(const std::string& debugName)
		: m_debugName(debugName)
	{
		// Create auto-reset event (signaled state initially)
		m_event = CreateEvent(nullptr, FALSE, TRUE, nullptr);
		if (m_event == nullptr)
		{
			NLS_LOG_WARNING("NativeDX11Fence: failed to create event");
		}
	}

	NativeDX11Fence::~NativeDX11Fence()
	{
		if (m_event != nullptr)
		{
			CloseHandle(m_event);
			m_event = nullptr;
		}
	}

	bool NativeDX11Fence::IsSignaled() const
	{
		if (m_event != nullptr)
		{
			return WaitForSingleObject(m_event, 0) == WAIT_OBJECT_0;
		}
		return true;
	}

	void NativeDX11Fence::Reset()
	{
		m_fenceValue++;
		if (m_event != nullptr)
		{
			ResetEvent(m_event);
		}
	}

	bool NativeDX11Fence::Wait(uint64_t timeoutNanoseconds)
	{
		if (m_event != nullptr)
		{
			DWORD timeoutMs = (timeoutNanoseconds == 0) ? INFINITE : static_cast<DWORD>(timeoutNanoseconds / 1000000);
			DWORD result = WaitForSingleObject(m_event, timeoutMs);
			return result == WAIT_OBJECT_0;
		}
		return true;
	}

	// ============================================================================
	// NativeDX11Semaphore Implementation
	// ============================================================================

	NativeDX11Semaphore::NativeDX11Semaphore(const std::string& debugName)
		: m_debugName(debugName)
	{
		// Create manual-reset event (non-signaled initially)
		m_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		if (m_event == nullptr)
		{
			NLS_LOG_WARNING("NativeDX11Semaphore: failed to create event");
		}
	}

	NativeDX11Semaphore::~NativeDX11Semaphore()
	{
		if (m_event != nullptr)
		{
			CloseHandle(m_event);
			m_event = nullptr;
		}
	}

	bool NativeDX11Semaphore::IsSignaled() const
	{
		return m_signaled;
	}

	void NativeDX11Semaphore::Reset()
	{
		m_signaled = false;
		if (m_event != nullptr)
		{
			ResetEvent(m_event);
		}
	}

	void NativeDX11Semaphore::Signal()
	{
		m_signaled = true;
		if (m_event != nullptr)
		{
			SetEvent(m_event);
		}
	}

	// ============================================================================
	// NativeDX11Swapchain Implementation
	// ============================================================================

	NativeDX11Swapchain::NativeDX11Swapchain(Microsoft::WRL::ComPtr<IDXGISwapChain> swapchain, const RHI::SwapchainDesc& desc)
		: m_swapchain(swapchain)
		, m_desc(desc)
		, m_debugName("Swapchain")
	{
	}

	NativeDX11Swapchain::~NativeDX11Swapchain()
	{
		m_swapchain.Reset();
	}

	std::optional<RHI::RHIAcquiredImage> NativeDX11Swapchain::AcquireNextImage(
		const std::shared_ptr<RHI::RHISemaphore>& signalSemaphore,
		const std::shared_ptr<RHI::RHIFence>& signalFence)
	{
		// DX11 does async present - we just need to signal the semaphore/fence
		if (signalSemaphore)
		{
			if (auto* nativeSem = dynamic_cast<NativeDX11Semaphore*>(signalSemaphore.get()))
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

	void NativeDX11Swapchain::Resize(uint32_t width, uint32_t height)
	{
		m_desc.width = width;
		m_desc.height = height;

		if (m_swapchain)
		{
			// Resize the swapchain buffers
			// Note: need to release any references to back buffers before resizing
			HRESULT hr = m_swapchain->ResizeBuffers(
				0, // keep current buffer count
				width,
				height,
				DXGI_FORMAT_UNKNOWN, // keep current format
				0);
			if (FAILED(hr))
			{
				NLS_LOG_ERROR("NativeDX11Swapchain: failed to resize swapchain");
			}
		}
	}

	// ============================================================================
	// DX11ExplicitDevice Implementation
	// ============================================================================

	DX11ExplicitDevice::DX11ExplicitDevice(
		Microsoft::WRL::ComPtr<ID3D11Device> device,
		Microsoft::WRL::ComPtr<ID3D11DeviceContext> context,
		std::shared_ptr<RHI::RHIAdapter> adapter,
		const RHI::RHIDeviceCapabilities& capabilities)
		: m_device(device)
		, m_context(context)
		, m_adapter(adapter)
		, m_capabilities(capabilities)
	{
		m_graphicsQueue = std::make_shared<DX11Queue>(m_context, m_swapchain, RHI::QueueType::Graphics, "GraphicsQueue");
		NLS_LOG_INFO("DX11ExplicitDevice: created Tier A formal RHI device");
	}

	DX11ExplicitDevice::~DX11ExplicitDevice() = default;

	const RHI::RHIDeviceCapabilities& DX11ExplicitDevice::GetCapabilities() const
	{
		return m_capabilities;
	}

	RHI::NativeRenderDeviceInfo DX11ExplicitDevice::GetNativeDeviceInfo() const
	{
		RHI::NativeRenderDeviceInfo info{};
		info.backend = RHI::NativeBackendType::DX11;
		info.device = static_cast<void*>(m_device.Get());
		info.graphicsQueue = static_cast<void*>(m_context.Get());
		info.swapchain = static_cast<void*>(m_swapchain.Get());
		return info;
	}

	bool DX11ExplicitDevice::IsBackendReady() const
	{
		// Tier B shim is ready if we have either device or legacy device reference
		return m_device != nullptr && m_context != nullptr;
	}

	std::shared_ptr<RHI::RHIQueue> DX11ExplicitDevice::GetQueue(RHI::QueueType queueType)
	{
		if (queueType == RHI::QueueType::Graphics)
		{
			return m_graphicsQueue;
		}
		return nullptr;
	}

	std::shared_ptr<RHI::RHISwapchain> DX11ExplicitDevice::CreateSwapchain(const RHI::SwapchainDesc& desc)
	{
		// Get DXGI device and adapter
		Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
		Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
		Microsoft::WRL::ComPtr<IDXGIFactory> dxgiFactory;

		 HRESULT hr = m_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
		if (FAILED(hr))
		{
			NLS_LOG_ERROR("DX11ExplicitDevice::CreateSwapchain: failed to get DXGI device");
			return nullptr;
		}

		hr = dxgiDevice->GetAdapter(dxgiAdapter.GetAddressOf());
		if (FAILED(hr))
		{
			NLS_LOG_ERROR("DX11ExplicitDevice::CreateSwapchain: failed to get DXGI adapter");
			return nullptr;
		}

		hr = dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory));
		if (FAILED(hr))
		{
			NLS_LOG_ERROR("DX11ExplicitDevice::CreateSwapchain: failed to get DXGI factory");
			return nullptr;
		}

		// Get HWND from platform window
		HWND hwnd = static_cast<HWND>(desc.platformWindow != nullptr ? desc.platformWindow : m_platformWindow);
		if (hwnd == nullptr)
		{
			NLS_LOG_ERROR("DX11ExplicitDevice::CreateSwapchain: no valid window handle");
			return nullptr;
		}

		// Determine format - only RGBA8 and RGBA16F are defined in TextureFormat enum
		DXGI_FORMAT dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		switch (desc.backbufferFormat)
		{
		case RHI::TextureFormat::RGBA8:
			dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
			break;
		case RHI::TextureFormat::RGBA16F:
			dxgiFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
			break;
		case RHI::TextureFormat::Depth24Stencil8:
			// Not typically used for swapchain, fall back to RGBA8
			dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
			break;
		default:
			dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
			break;
		}

		// Create swapchain descriptor
		DXGI_SWAP_CHAIN_DESC swapDesc{};
		swapDesc.BufferDesc.Width = desc.width > 0 ? desc.width : 1920;
		swapDesc.BufferDesc.Height = desc.height > 0 ? desc.height : 1080;
		swapDesc.BufferDesc.Format = dxgiFormat;
		swapDesc.BufferDesc.RefreshRate.Numerator = desc.vsync ? 60 : 0;
		swapDesc.BufferDesc.RefreshRate.Denominator = desc.vsync ? 1 : 1;
		swapDesc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
		swapDesc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
		swapDesc.SampleDesc.Count = 1;
		swapDesc.SampleDesc.Quality = 0;
		swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_BACK_BUFFER;
		swapDesc.BufferCount = desc.imageCount > 0 ? desc.imageCount : 2;
		swapDesc.OutputWindow = hwnd;
		swapDesc.Windowed = TRUE;
		swapDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		swapDesc.Flags = 0;

		// Create the swapchain
		Microsoft::WRL::ComPtr<IDXGISwapChain> swapchain;
		hr = dxgiFactory->CreateSwapChain(m_device.Get(), &swapDesc, swapchain.GetAddressOf());
		if (FAILED(hr))
		{
			NLS_LOG_ERROR("DX11ExplicitDevice::CreateSwapchain: failed to create swapchain");
			return nullptr;
		}

		// Store the swapchain and update the queue
		m_swapchain = swapchain;

		NLS_LOG_INFO("DX11ExplicitDevice::CreateSwapchain: created DXGI swapchain");

		return std::make_shared<NativeDX11Swapchain>(swapchain, desc);
	}

	std::shared_ptr<RHI::RHIBuffer> DX11ExplicitDevice::CreateBuffer(const RHI::RHIBufferDesc& desc, const void* initialData)
	{
		D3D11_BUFFER_DESC bufferDesc{};
		bufferDesc.ByteWidth = static_cast<UINT>(desc.size);
		bufferDesc.MiscFlags = 0;

		// Set bind flags based on usage
		if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(RHI::BufferUsageFlags::Vertex))
			bufferDesc.BindFlags |= D3D11_BIND_VERTEX_BUFFER;
		if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(RHI::BufferUsageFlags::Index))
			bufferDesc.BindFlags |= D3D11_BIND_INDEX_BUFFER;
		if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(RHI::BufferUsageFlags::Uniform))
			bufferDesc.BindFlags |= D3D11_BIND_CONSTANT_BUFFER;
		if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(RHI::BufferUsageFlags::Storage))
			bufferDesc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

		// Set CPU access flags based on memory usage
		if (desc.memoryUsage == RHI::MemoryUsage::CPUToGPU)
			bufferDesc.CPUAccessFlags |= D3D11_CPU_ACCESS_WRITE;
		if (desc.memoryUsage == RHI::MemoryUsage::GPUToCPU)
			bufferDesc.CPUAccessFlags |= D3D11_CPU_ACCESS_READ;

		D3D11_SUBRESOURCE_DATA initData{};
		initData.pSysMem = initialData;

		Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
		HRESULT hr = m_device->CreateBuffer(
			&bufferDesc,
			initialData ? &initData : nullptr,
			&buffer);

		if (FAILED(hr))
		{
			NLS_LOG_WARNING("DX11ExplicitDevice::CreateBuffer failed");
			return nullptr;
		}

		return std::make_shared<DX11Buffer>(buffer, desc);
	}

	std::shared_ptr<RHI::RHITexture> DX11ExplicitDevice::CreateTexture(const RHI::RHITextureDesc& desc, const void*)
	{
		D3D11_TEXTURE2D_DESC texDesc{};
		texDesc.Width = desc.extent.width;
		texDesc.Height = desc.extent.height;
		texDesc.MipLevels = desc.mipLevels;
		texDesc.ArraySize = desc.arrayLayers;
		texDesc.Format = ToDxgiFormat(desc.format);
		texDesc.SampleDesc.Count = desc.sampleCount;
		texDesc.SampleDesc.Quality = 0;
		texDesc.Usage = D3D11_USAGE_DEFAULT;

		// Set bind flags based on usage
		if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(RHI::TextureUsageFlags::Sampled))
			texDesc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
		if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(RHI::TextureUsageFlags::ColorAttachment))
			texDesc.BindFlags |= D3D11_BIND_RENDER_TARGET;
		if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(RHI::TextureUsageFlags::DepthStencilAttachment))
			texDesc.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
		if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(RHI::TextureUsageFlags::Storage))
			texDesc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

		Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
		HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, &texture);

		if (FAILED(hr))
		{
			NLS_LOG_WARNING("DX11ExplicitDevice::CreateTexture failed");
			return nullptr;
		}

		return std::make_shared<DX11Texture>(texture, desc);
	}

	std::shared_ptr<RHI::RHITextureView> DX11ExplicitDevice::CreateTextureView(
		const std::shared_ptr<RHI::RHITexture>& texture, const RHI::RHITextureViewDesc& desc)
	{
		if (!texture)
			return nullptr;

		auto* dxTexture = dynamic_cast<DX11Texture*>(texture.get());
		if (!dxTexture)
		{
			NLS_LOG_WARNING("DX11ExplicitDevice::CreateTextureView: invalid texture type");
			return nullptr;
		}

		ID3D11Texture2D* d3dTexture = dxTexture->GetTexture();
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
		Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
		Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;

		// Create SRV if requested
		if (desc.viewType == RHI::TextureViewType::Auto ||
			desc.viewType == RHI::TextureViewType::Texture2D)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = ToDxgiFormat(desc.format);
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = desc.subresourceRange.mipLevelCount;
			srvDesc.Texture2D.MostDetailedMip = desc.subresourceRange.baseMipLevel;

			HRESULT hr = m_device->CreateShaderResourceView(d3dTexture, &srvDesc, &srv);
			if (FAILED(hr))
			{
				NLS_LOG_WARNING("DX11ExplicitDevice::CreateTextureView: failed to create SRV");
				return nullptr;
			}
		}

		// Create RTV if color attachment
		if (static_cast<uint32_t>(texture->GetDesc().usage) & static_cast<uint32_t>(RHI::TextureUsageFlags::ColorAttachment))
		{
			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
			rtvDesc.Format = ToDxgiFormat(desc.format);
			rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			rtvDesc.Texture2D.MipSlice = 0;

			HRESULT hr = m_device->CreateRenderTargetView(d3dTexture, &rtvDesc, &rtv);
			if (FAILED(hr))
			{
				NLS_LOG_WARNING("DX11ExplicitDevice::CreateTextureView: failed to create RTV");
				return nullptr;
			}
		}

		// Create DSV if depth stencil attachment
		if (static_cast<uint32_t>(texture->GetDesc().usage) & static_cast<uint32_t>(RHI::TextureUsageFlags::DepthStencilAttachment))
		{
			D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
			dsvDesc.Format = ToDxgiFormat(desc.format);
			dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
			dsvDesc.Texture2D.MipSlice = 0;

			HRESULT hr = m_device->CreateDepthStencilView(d3dTexture, &dsvDesc, &dsv);
			if (FAILED(hr))
			{
				NLS_LOG_WARNING("DX11ExplicitDevice::CreateTextureView: failed to create DSV");
				return nullptr;
			}
		}

		return std::make_shared<DX11TextureView>(srv, rtv, dsv, texture, desc);
	}

	std::shared_ptr<RHI::RHISampler> DX11ExplicitDevice::CreateSampler(const RHI::SamplerDesc& desc, std::string debugName)
	{
		// TODO: Implement DX11 sampler
		return nullptr;
	}

	std::shared_ptr<RHI::RHIBindingLayout> DX11ExplicitDevice::CreateBindingLayout(const RHI::RHIBindingLayoutDesc& desc)
	{
		// TODO: Implement DX11 binding layout
		return nullptr;
	}

	std::shared_ptr<RHI::RHIBindingSet> DX11ExplicitDevice::CreateBindingSet(const RHI::RHIBindingSetDesc& desc)
	{
		// TODO: Implement DX11 binding set
		return nullptr;
	}

	std::shared_ptr<RHI::RHIPipelineLayout> DX11ExplicitDevice::CreatePipelineLayout(const RHI::RHIPipelineLayoutDesc& desc)
	{
		// TODO: Implement DX11 pipeline layout
		return nullptr;
	}

	std::shared_ptr<RHI::RHIShaderModule> DX11ExplicitDevice::CreateShaderModule(const RHI::RHIShaderModuleDesc& desc)
	{
		if (desc.bytecode.empty())
		{
			NLS_LOG_WARNING("DX11ExplicitDevice::CreateShaderModule: empty bytecode");
			return nullptr;
		}

		Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
		Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
		Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;

		if (desc.stage == RHI::ShaderStage::Vertex)
		{
			HRESULT hr = m_device->CreateVertexShader(desc.bytecode.data(), desc.bytecode.size(), nullptr, &vs);
			if (FAILED(hr))
			{
				NLS_LOG_WARNING("DX11ExplicitDevice::CreateShaderModule: failed to create vertex shader");
				return nullptr;
			}
			// Note: Input layout is created in CreateGraphicsPipeline since it requires both bytecode and vertex attribute descriptions
			// For now, pass nullptr for inputLayout - it will be created later in CreateGraphicsPipeline
		}
		else if (desc.stage == RHI::ShaderStage::Fragment)
		{
			HRESULT hr = m_device->CreatePixelShader(desc.bytecode.data(), desc.bytecode.size(), nullptr, &ps);
			if (FAILED(hr))
			{
				NLS_LOG_WARNING("DX11ExplicitDevice::CreateShaderModule: failed to create pixel shader");
				return nullptr;
			}
		}
		else
		{
			NLS_LOG_WARNING("DX11ExplicitDevice::CreateShaderModule: unsupported shader stage");
			return nullptr;
		}

		return std::make_shared<DX11ShaderModule>(vs, ps, inputLayout, desc);
	}

	std::shared_ptr<RHI::RHIGraphicsPipeline> DX11ExplicitDevice::CreateGraphicsPipeline(const RHI::RHIGraphicsPipelineDesc& desc)
	{
		if (desc.vertexShader == nullptr || desc.fragmentShader == nullptr)
		{
			NLS_LOG_WARNING("DX11ExplicitDevice::CreateGraphicsPipeline: missing shaders");
			return nullptr;
		}

		// Get shader modules
		auto* vsModule = dynamic_cast<DX11ShaderModule*>(desc.vertexShader.get());
		auto* psModule = dynamic_cast<DX11ShaderModule*>(desc.fragmentShader.get());
		if (vsModule == nullptr || psModule == nullptr)
		{
			NLS_LOG_WARNING("DX11ExplicitDevice::CreateGraphicsPipeline: invalid shader module types");
			return nullptr;
		}

		// Get vertex shader bytecode for input layout creation
		const auto& vsBytecode = vsModule->GetDesc().bytecode;
		if (vsBytecode.empty())
		{
			NLS_LOG_WARNING("DX11ExplicitDevice::CreateGraphicsPipeline: vertex shader has no bytecode");
			return nullptr;
		}

		// Create input layout from vertex attributes
		Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
		if (!desc.vertexAttributes.empty())
		{
			std::vector<D3D11_INPUT_ELEMENT_DESC> elementDescs;
			elementDescs.reserve(desc.vertexAttributes.size());

			for (const auto& attr : desc.vertexAttributes)
			{
				D3D11_INPUT_ELEMENT_DESC elem = {};
				elem.SemanticName = attr.location == 0 ? "POSITION" : (attr.location == 1 ? "NORMAL" : (attr.location == 2 ? "TEXCOORD" : "COLOR"));
				elem.SemanticIndex = 0;
				elem.AlignedByteOffset = attr.offset;

				// Infer format from elementSize (the only available hint)
				// Common element sizes: 4=float/ubyte4, 8=float2, 12=float3, 16=float4
				switch (attr.elementSize)
				{
				case 4:
					// Could be FLOAT or UBYTE4 - default to FLOAT for positions
					elem.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
					break;
				case 8:
					elem.Format = DXGI_FORMAT_R32G32_FLOAT;
					break;
				case 12:
					elem.Format = DXGI_FORMAT_R32G32B32_FLOAT;
					break;
				case 16:
				default:
					elem.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
					break;
				}

				elem.InputSlot = attr.binding;
				elem.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
				elem.InstanceDataStepRate = 0;
				elementDescs.push_back(elem);
			}

			HRESULT hr = m_device->CreateInputLayout(
				elementDescs.data(),
				static_cast<UINT>(elementDescs.size()),
				vsBytecode.data(),
				vsBytecode.size(),
				&inputLayout);
			if (FAILED(hr))
			{
				NLS_LOG_WARNING("DX11ExplicitDevice::CreateGraphicsPipeline: failed to create input layout");
				return nullptr;
			}
		}

		// Create rasterizer state
		D3D11_RASTERIZER_DESC rasterDesc = {};
		rasterDesc.FillMode = desc.rasterState.wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
		rasterDesc.CullMode = desc.rasterState.cullEnabled
			? (desc.rasterState.cullFace == Settings::ECullFace::FRONT ? D3D11_CULL_FRONT : D3D11_CULL_BACK)
			: D3D11_CULL_NONE;
		rasterDesc.FrontCounterClockwise = TRUE;
		rasterDesc.DepthBias = 0;
		rasterDesc.DepthBiasClamp = 0.0f;
		rasterDesc.SlopeScaledDepthBias = 0.0f;
		rasterDesc.DepthClipEnable = TRUE;
		rasterDesc.ScissorEnable = FALSE;
		rasterDesc.MultisampleEnable = FALSE;
		rasterDesc.AntialiasedLineEnable = FALSE;

		Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerState;
		HRESULT hr = m_device->CreateRasterizerState(&rasterDesc, &rasterizerState);
		if (FAILED(hr))
		{
			NLS_LOG_WARNING("DX11ExplicitDevice::CreateGraphicsPipeline: failed to create rasterizer state");
			return nullptr;
		}

		// Create blend state
		D3D11_BLEND_DESC blendDesc = {};
		blendDesc.AlphaToCoverageEnable = FALSE;
		blendDesc.IndependentBlendEnable = FALSE;
		blendDesc.RenderTarget[0].BlendEnable = desc.blendState.enabled ? TRUE : FALSE;
		blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].RenderTargetWriteMask = desc.blendState.colorWrite ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;

		Microsoft::WRL::ComPtr<ID3D11BlendState> blendState;
		hr = m_device->CreateBlendState(&blendDesc, &blendState);
		if (FAILED(hr))
		{
			NLS_LOG_WARNING("DX11ExplicitDevice::CreateGraphicsPipeline: failed to create blend state");
			return nullptr;
		}

		// Create depth stencil state
		D3D11_DEPTH_STENCIL_DESC dsDesc = {};
		dsDesc.DepthEnable = desc.depthStencilState.depthTest ? TRUE : FALSE;
		dsDesc.DepthWriteMask = desc.depthStencilState.depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
		dsDesc.DepthFunc = ToD3D11ComparisonFunc(desc.depthStencilState.depthCompare);
		dsDesc.StencilEnable = desc.depthStencilState.stencilTest ? TRUE : FALSE;
		dsDesc.StencilReadMask = static_cast<UINT8>(desc.depthStencilState.stencilReadMask);
		dsDesc.StencilWriteMask = static_cast<UINT8>(desc.depthStencilState.stencilWriteMask);
		dsDesc.FrontFace.StencilFunc = ToD3D11ComparisonFunc(desc.depthStencilState.stencilCompare);
		dsDesc.FrontFace.StencilFailOp = ToD3D11StencilOp(desc.depthStencilState.stencilFailOp);
		dsDesc.FrontFace.StencilDepthFailOp = ToD3D11StencilOp(desc.depthStencilState.stencilDepthFailOp);
		dsDesc.FrontFace.StencilPassOp = ToD3D11StencilOp(desc.depthStencilState.stencilPassOp);
		dsDesc.BackFace = dsDesc.FrontFace;

		Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthStencilState;
		hr = m_device->CreateDepthStencilState(&dsDesc, &depthStencilState);
		if (FAILED(hr))
		{
			NLS_LOG_WARNING("DX11ExplicitDevice::CreateGraphicsPipeline: failed to create depth stencil state");
			return nullptr;
		}

		return std::make_shared<DX11GraphicsPipeline>(
			vsModule->GetVertexShader(),
			psModule->GetPixelShader(),
			inputLayout,
			rasterizerState,
			blendState,
			depthStencilState,
			desc);
	}

	std::shared_ptr<RHI::RHIComputePipeline> DX11ExplicitDevice::CreateComputePipeline(const RHI::RHIComputePipelineDesc& desc)
	{
		// Compute not supported in DX11
		return nullptr;
	}

	std::shared_ptr<RHI::RHICommandPool> DX11ExplicitDevice::CreateCommandPool(RHI::QueueType queueType, std::string debugName)
	{
		return std::make_shared<DX11CommandPool>(
			m_device,
			m_context,
			queueType,
			debugName.empty() ? "CommandPool" : debugName);
	}

	std::shared_ptr<RHI::RHIFence> DX11ExplicitDevice::CreateFence(std::string debugName)
	{
		return std::make_shared<NativeDX11Fence>(debugName.empty() ? "Fence" : debugName);
	}

	std::shared_ptr<RHI::RHISemaphore> DX11ExplicitDevice::CreateSemaphore(std::string debugName)
	{
		return std::make_shared<NativeDX11Semaphore>(debugName.empty() ? "Semaphore" : debugName);
	}

	void DX11ExplicitDevice::ReadPixels(
	    const std::shared_ptr<RHI::RHITexture>& texture,
	    uint32_t x,
	    uint32_t y,
	    uint32_t width,
	    uint32_t height,
	    NLS::Render::Settings::EPixelDataFormat format,
	    NLS::Render::Settings::EPixelDataType type,
	    void* data)
	{
#if defined(_WIN32)
		if (texture == nullptr || data == nullptr || width == 0 || height == 0 || m_context == nullptr)
			return;

		// Get the DX11 texture from the RHITexture
		auto dxTexture = dynamic_cast<DX11Texture*>(texture.get());
		if (dxTexture == nullptr)
			return;

		ID3D11Texture2D* srcTexture = dxTexture->GetTexture();
		if (srcTexture == nullptr)
			return;

		// Get texture description
		D3D11_TEXTURE2D_DESC texDesc{};
		srcTexture->GetDesc(&texDesc);

		// Create a staging texture for readback
		D3D11_TEXTURE2D_DESC stagingDesc{};
		stagingDesc.Width = texDesc.Width;
		stagingDesc.Height = texDesc.Height;
		stagingDesc.MipLevels = 1;
		stagingDesc.ArraySize = 1;
		stagingDesc.Format = texDesc.Format;
		stagingDesc.SampleDesc.Count = 1;
		stagingDesc.SampleDesc.Quality = 0;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.BindFlags = 0;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		stagingDesc.MiscFlags = 0;

		Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTexture;
		HRESULT hr = m_device->CreateTexture2D(&stagingDesc, nullptr, stagingTexture.GetAddressOf());
		if (FAILED(hr))
			return;

		// Copy the texture to staging
		m_context->CopySubresourceRegion(stagingTexture.Get(), 0, x, y, 0, srcTexture, 0, nullptr);

		// Map the staging texture
		D3D11_MAPPED_SUBRESOURCE mappedResource{};
		hr = m_context->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mappedResource);
		if (FAILED(hr))
			return;

		// Calculate bytes per pixel
		const uint32_t bytesPerPixel = [format = texDesc.Format]()
		{
			switch (format)
			{
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM:
				return 4u;
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
				return 8u;
			case DXGI_FORMAT_R32G32B32A32_FLOAT:
				return 16u;
			default:
				return 4u;
			}
		}();

		// Copy data row by row (to handle row pitch)
		uint8_t* srcBytes = static_cast<uint8_t*>(mappedResource.pData);
		uint8_t* dstBytes = static_cast<uint8_t*>(data);

		if (format == NLS::Render::Settings::EPixelDataFormat::RGB && bytesPerPixel >= 3)
		{
			// RGB format: expand from source format to RGB
			for (uint32_t row = 0; row < height; ++row)
			{
				for (uint32_t col = 0; col < width; ++col)
				{
					size_t srcIdx = row * mappedResource.RowPitch + col * bytesPerPixel;
					size_t dstIdx = (row * width + col) * 3;
					dstBytes[dstIdx + 0] = srcBytes[srcIdx + 0];
					dstBytes[dstIdx + 1] = srcBytes[srcIdx + 1];
					dstBytes[dstIdx + 2] = srcBytes[srcIdx + 2];
				}
			}
		}
		else
		{
			// Direct copy
			for (uint32_t row = 0; row < height; ++row)
			{
				std::memcpy(dstBytes + row * width * bytesPerPixel,
				            srcBytes + row * mappedResource.RowPitch,
				            width * bytesPerPixel);
			}
		}

		m_context->Unmap(stagingTexture.Get(), 0);
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

	bool DX11ExplicitDevice::PrepareUIRender()
	{
		// UI uses the immediate context directly
		return true;
	}

	void DX11ExplicitDevice::ReleaseUITextureHandles()
	{
		// UI texture handles are managed separately
	}

	std::shared_ptr<RHI::RHIDevice> CreateDX11RhiDevice(void* platformWindow, uint32_t width, uint32_t height)
	{
		// Direct creation of DX11 Tier A device without IRenderDevice
		UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
		deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

		D3D_DRIVER_TYPE driverType = D3D_DRIVER_TYPE_HARDWARE;
		D3D_FEATURE_LEVEL featureLevels[] = {
			D3D_FEATURE_LEVEL_11_1,
			D3D_FEATURE_LEVEL_11_0
		};
		UINT numFeatureLevels = ARRAYSIZE(featureLevels);

		Microsoft::WRL::ComPtr<ID3D11Device> device;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
		D3D_FEATURE_LEVEL featureLevel;

		HRESULT hr = D3D11CreateDevice(
			nullptr,  // Adapter (use default)
			driverType,
			nullptr,  // Module
			deviceFlags,
			featureLevels,
			numFeatureLevels,
			D3D11_SDK_VERSION,
			device.GetAddressOf(),
			&featureLevel,
			context.GetAddressOf());

		if (FAILED(hr))
		{
			NLS_LOG_ERROR("CreateDX11RhiDevice: D3D11CreateDevice failed");
			return nullptr;
		}

		// Get adapter info
		Microsoft::WRL::ComPtr<IDXGIDevice1> dxgiDevice;
		Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
		std::string vendor, hardware;

		if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))))
		{
			if (SUCCEEDED(dxgiDevice->GetAdapter(dxgiAdapter.GetAddressOf())))
			{
				Microsoft::WRL::ComPtr<IDXGIAdapter1> dxgiAdapter1;
				if (SUCCEEDED(dxgiAdapter.As(&dxgiAdapter1)))
				{
					DXGI_ADAPTER_DESC1 desc;
					if (SUCCEEDED(dxgiAdapter1->GetDesc1(&desc)))
					{
						switch (desc.VendorId)
						{
						case 0x10DE: vendor = "NVIDIA"; break;
						case 0x1002: vendor = "AMD"; break;
						case 0x8086: vendor = "Intel"; break;
						default: vendor = "Unknown"; break;
						}
						std::wstring wdesc(desc.Description);
						hardware = std::string(wdesc.begin(), wdesc.end());
					}
				}
			}
		}

		auto adapter = std::make_shared<DX11Adapter>(vendor, hardware);

		// Build capabilities
		RHI::RHIDeviceCapabilities capabilities{};
		capabilities.backendReady = true;
		capabilities.supportsCompute = false;  // DX11 compute requires DirectX 11.1+
		capabilities.supportsFramebufferReadback = true;
		capabilities.maxTextureDimension2D = 8192;  // Conservative for DX11
		capabilities.maxColorAttachments = 8;

		NLS_LOG_INFO("CreateDX11RhiDevice: created DX11 Tier A device directly, vendor=" + vendor);

		// Create and return the explicit device
		auto dxDevice = std::make_shared<DX11ExplicitDevice>(device, context, adapter, capabilities);

		// Store platform window for later swapchain creation
		dxDevice->SetPlatformWindow(platformWindow);

		return dxDevice;
	}
}
