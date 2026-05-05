#include "Rendering/RHI/Backends/DX12/DX12Command.h"

#include "Profiling/Profiler.h"
#include "Rendering/RHI/Backends/DX12/DX12RenderPassUtils.h"
#include "Rendering/RHI/Backends/DX12/DX12Resource.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <Debug/Logger.h>

#if defined(_WIN32)
#include <Windows.h>
#include <d3d12.h>
#endif

namespace NLS::Render::Backend
{
	namespace
	{
#if defined(_WIN32)
		std::wstring Utf8ToWideString(const std::string& value)
		{
			if (value.empty())
				return {};

			const int requiredChars = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
			if (requiredChars <= 1)
				return {};

			std::wstring wide(static_cast<size_t>(requiredChars), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, wide.data(), requiredChars);
			wide.resize(static_cast<size_t>(requiredChars - 1));
			return wide;
		}

		void SetDx12ObjectName(ID3D12Object* object, const std::string& debugName)
		{
			if (object == nullptr || debugName.empty())
				return;

			const std::wstring wideName = Utf8ToWideString(debugName);
			if (!wideName.empty())
				object->SetName(wideName.c_str());
		}

		IDX12BindingSetAccess* ResolveDX12BindingSetAccess(
			const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& bindingSet)
		{
			auto current = bindingSet;
			while (current != nullptr)
			{
				const auto nativeHandle = current->GetNativeBindingSetHandle();
				if (nativeHandle.backend == NLS::Render::RHI::BackendType::DX12 &&
					nativeHandle.handle != nullptr)
				{
					return static_cast<IDX12BindingSetAccess*>(nativeHandle.handle);
				}

				current = current->GetWrappedBindingSetShared();
			}
			return nullptr;
		}

		const char* ToDescriptorHeapKindName(const NLS::Render::RHI::DX12::DX12DescriptorHeapKind heapKind)
		{
			switch (heapKind)
			{
			case NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Resource:
				return "Resource";
			case NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Sampler:
				return "Sampler";
			}
			return "Unknown";
		}
#endif
	}

	NativeDX12CommandBuffer::NativeDX12CommandBuffer(
		ID3D12Device* device,
		ID3D12CommandQueue* queue,
#if defined(_WIN32)
		D3D12_COMMAND_LIST_TYPE commandListType,
#else
		int commandListType,
#endif
		const std::string& debugName)
		: m_debugName(debugName)
#if defined(_WIN32)
		, m_device(device)
		, m_queue(queue)
		, m_commandListType(commandListType)
#endif
	{
#if defined(_WIN32)
		if (device != nullptr)
		{
			const HRESULT allocatorHr = device->CreateCommandAllocator(m_commandListType, IID_PPV_ARGS(&m_allocator));
			if (FAILED(allocatorHr))
			{
				NLS_LOG_ERROR("NativeDX12CommandBuffer: CreateCommandAllocator failed hr=" + std::to_string(allocatorHr) + " name=" + m_debugName);
			}
			const HRESULT commandListHr = device->CreateCommandList(0, m_commandListType, m_allocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList));
			if (FAILED(commandListHr))
			{
				NLS_LOG_ERROR("NativeDX12CommandBuffer: CreateCommandList failed hr=" + std::to_string(commandListHr) + " name=" + m_debugName);
			}
			SetDx12ObjectName(m_allocator.Get(), m_debugName + "_Allocator");
			SetDx12ObjectName(m_commandList.Get(), m_debugName);
			if (m_commandList != nullptr)
				m_commandList->Close();
		}
#else
		(void)device;
		(void)queue;
		(void)commandListType;
#endif
	}

	NativeDX12CommandBuffer::~NativeDX12CommandBuffer() = default;

	std::string_view NativeDX12CommandBuffer::GetDebugName() const
	{
		return m_debugName;
	}

	void NativeDX12CommandBuffer::EndPendingGpuProfileScopes()
	{
#if defined(_WIN32)
		while (!m_gpuProfileScopeStack.empty())
		{
			auto event = std::move(m_gpuProfileScopeStack.back());
			m_gpuProfileScopeStack.pop_back();
			NLS::Base::Profiling::Profiler::EndGpuScope(event);
		}
#endif
	}

	void NativeDX12CommandBuffer::Begin()
	{
#if defined(_WIN32)
		if (m_allocator == nullptr || m_commandList == nullptr)
		{
			NLS_LOG_ERROR("NativeDX12CommandBuffer::Begin failed: allocator or command list is null name=" + m_debugName);
			return;
		}

		if (m_allocator != nullptr && m_commandList != nullptr)
		{
			const HRESULT allocatorHr = m_allocator->Reset();
			if (FAILED(allocatorHr))
			{
				NLS_LOG_ERROR("NativeDX12CommandBuffer::Begin failed: allocator reset hr=" + std::to_string(allocatorHr) + " name=" + m_debugName);
				return;
			}
			const HRESULT commandListHr = m_commandList->Reset(m_allocator.Get(), nullptr);
			if (FAILED(commandListHr))
			{
				NLS_LOG_ERROR("NativeDX12CommandBuffer::Begin failed: command list reset hr=" + std::to_string(commandListHr) + " name=" + m_debugName);
				return;
			}
			m_currentResourceDescriptorHeap = nullptr;
			m_currentSamplerDescriptorHeap = nullptr;
			m_boundDescriptorTables.clear();
			m_initializedRootDescriptorTables.clear();
			m_boundBindingSets.clear();
			m_recordedBindingSetKeepAlive.clear();
			m_recordedPipelineKeepAlive.clear();
			m_recordedComputePipelineKeepAlive.clear();
			EndPendingGpuProfileScopes();
			m_bindingComputePipeline = false;
			m_recording = true;
		}
#endif
	}

	void NativeDX12CommandBuffer::End()
	{
#if defined(_WIN32)
		if (m_commandList != nullptr)
		{
			m_commandList->Close();
			m_recording = false;
		}
#endif
	}

	void NativeDX12CommandBuffer::Reset()
	{
#if defined(_WIN32)
		m_activeRenderPassTransitions.clear();
		m_currentResourceDescriptorHeap = nullptr;
		m_currentSamplerDescriptorHeap = nullptr;
		m_boundDescriptorTables.clear();
		m_initializedRootDescriptorTables.clear();
		m_boundBindingSets.clear();
		m_recordedBindingSetKeepAlive.clear();
		m_recordedPipelineKeepAlive.clear();
		m_recordedComputePipelineKeepAlive.clear();
		EndPendingGpuProfileScopes();
		m_bindingComputePipeline = false;
		m_boundPipeline.reset();
		m_boundComputePipeline.reset();

		if (m_allocator == nullptr || m_commandList == nullptr)
			return;

		const HRESULT allocatorHr = m_allocator->Reset();
		if (FAILED(allocatorHr))
		{
			NLS_LOG_ERROR("NativeDX12CommandBuffer::Reset failed: allocator reset hr=" + std::to_string(allocatorHr) + " name=" + m_debugName);
			return;
		}

		const HRESULT commandListHr = m_commandList->Reset(m_allocator.Get(), nullptr);
		if (FAILED(commandListHr))
		{
			NLS_LOG_ERROR("NativeDX12CommandBuffer::Reset failed: command list reset hr=" + std::to_string(commandListHr) + " name=" + m_debugName);
			return;
		}

		m_commandList->Close();
		m_recording = false;
#endif
	}

	bool NativeDX12CommandBuffer::IsRecording() const
	{
		return m_recording;
	}

	void* NativeDX12CommandBuffer::GetNativeCommandBuffer() const
	{
#if defined(_WIN32)
		return m_commandList.Get();
#else
		return nullptr;
#endif
	}

	void NativeDX12CommandBuffer::BeginGpuProfileScope(
		const std::string_view name,
		const std::string_view sourceFunction)
	{
#if defined(_WIN32)
		auto event = NLS::Base::Profiling::Profiler::BeginGpuScope(m_commandList.Get(), name, sourceFunction);
		if (event.active)
			m_gpuProfileScopeStack.push_back(std::move(event));
#else
		(void)name;
		(void)sourceFunction;
#endif
	}

	void NativeDX12CommandBuffer::EndGpuProfileScope()
	{
#if defined(_WIN32)
		if (m_gpuProfileScopeStack.empty())
			return;
		auto event = std::move(m_gpuProfileScopeStack.back());
		m_gpuProfileScopeStack.pop_back();
		NLS::Base::Profiling::Profiler::EndGpuScope(event);
#endif
	}

	void NativeDX12CommandBuffer::BeginRenderPass(const NLS::Render::RHI::RHIRenderPassDesc& desc)
	{
#if defined(_WIN32)
		if (m_commandList == nullptr)
			return;

		m_activeRenderPassTransitions.clear();
		const auto clearPlan = NLS::Render::RHI::DX12::BuildDX12RenderPassClearPlan(desc);
		const bool isBackbufferPass = desc.debugName == "BackbufferRenderPass";
		for (const auto& colorAttachment : desc.colorAttachments)
		{
			if (colorAttachment.view == nullptr)
				continue;

			const auto& texture = colorAttachment.view->GetTexture();
			if (texture == nullptr)
				continue;

			const auto textureHandle = texture->GetNativeImageHandle();
			auto* resource = textureHandle.backend == NLS::Render::RHI::BackendType::DX12
				? static_cast<ID3D12Resource*>(textureHandle.handle)
				: nullptr;
			if (resource == nullptr)
				continue;

			D3D12_RESOURCE_STATES stateBefore = isBackbufferPass
				? D3D12_RESOURCE_STATE_PRESENT
				: ToD3D12ResourceState(texture->GetState());
			if (!isBackbufferPass && stateBefore == D3D12_RESOURCE_STATE_COMMON && texture->GetState() == NLS::Render::RHI::ResourceState::Unknown)
				stateBefore = D3D12_RESOURCE_STATE_COMMON;
			const D3D12_RESOURCE_STATES stateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			const D3D12_RESOURCE_STATES stateAfterPass = isBackbufferPass
				? D3D12_RESOURCE_STATE_PRESENT
				: D3D12_RESOURCE_STATE_COMMON;

			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = resource;
			barrier.Transition.StateBefore = stateBefore;
			barrier.Transition.StateAfter = stateAfter;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			m_commandList->ResourceBarrier(1, &barrier);

			m_activeRenderPassTransitions.push_back({
				resource,
				stateAfter,
				stateAfterPass,
				texture.get(),
				isBackbufferPass
					? NLS::Render::RHI::ResourceState::Present
					: NLS::Render::RHI::ResourceState::Unknown
			});
			if (auto* nativeTexture = dynamic_cast<NativeDX12Texture*>(texture.get()))
				nativeTexture->SetState(NLS::Render::RHI::ResourceState::RenderTarget);
		}

		if (desc.depthStencilAttachment.has_value() && desc.depthStencilAttachment->view != nullptr)
		{
			const auto& texture = desc.depthStencilAttachment->view->GetTexture();
			if (texture != nullptr)
			{
				const auto textureHandle = texture->GetNativeImageHandle();
				auto* resource = textureHandle.backend == NLS::Render::RHI::BackendType::DX12
					? static_cast<ID3D12Resource*>(textureHandle.handle)
					: nullptr;
				if (resource != nullptr)
				{
					D3D12_RESOURCE_BARRIER barrier{};
					barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
					barrier.Transition.pResource = resource;
					const D3D12_RESOURCE_STATES stateBefore = ToD3D12ResourceState(texture->GetState());
					barrier.Transition.StateBefore = stateBefore;
					barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
					barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
					m_commandList->ResourceBarrier(1, &barrier);

					m_activeRenderPassTransitions.push_back({
						resource,
						D3D12_RESOURCE_STATE_DEPTH_WRITE,
						D3D12_RESOURCE_STATE_COMMON,
						texture.get(),
						NLS::Render::RHI::ResourceState::Unknown
					});
					if (auto* nativeTexture = dynamic_cast<NativeDX12Texture*>(texture.get()))
						nativeTexture->SetState(NLS::Render::RHI::ResourceState::DepthWrite);
				}
			}
		}

		std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvHandles;
		rtvHandles.reserve(desc.colorAttachments.size());
		D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {};
		std::vector<bool> hasRtvHandle;
		hasRtvHandle.reserve(desc.colorAttachments.size());
		bool hasDSV = false;

		for (const auto& colorAttachment : desc.colorAttachments)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{};
			bool hasHandle = false;

			if (colorAttachment.view != nullptr)
			{
				NLS::Render::RHI::NativeHandle rtvHandleNative = colorAttachment.view->GetNativeRenderTargetView();
				void* rtv = rtvHandleNative.handle;
				if (rtv != nullptr)
				{
					rtvHandle.ptr = reinterpret_cast<UINT64>(rtv);
					hasHandle = true;
				}
			}

			rtvHandles.push_back(rtvHandle);
			hasRtvHandle.push_back(hasHandle);
		}

		if (desc.depthStencilAttachment.has_value() && desc.depthStencilAttachment->view != nullptr)
		{
			NLS::Render::RHI::NativeHandle dsvHandleNative = desc.depthStencilAttachment->view->GetNativeDepthStencilView();
			void* dsv = dsvHandleNative.handle;
			if (dsv != nullptr)
			{
				dsvHandle.ptr = reinterpret_cast<UINT64>(dsv);
				hasDSV = true;
			}
		}

		D3D12_RECT fullRect{};
		fullRect.left = 0;
		fullRect.top = 0;
		fullRect.right = desc.renderArea.width > 0 ? desc.renderArea.width : 1920;
		fullRect.bottom = desc.renderArea.height > 0 ? desc.renderArea.height : 1080;
		m_commandList->RSSetScissorRects(1, &fullRect);

		std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> boundRtvHandles;
		boundRtvHandles.reserve(rtvHandles.size());
		for (size_t attachmentIndex = 0; attachmentIndex < rtvHandles.size(); ++attachmentIndex)
		{
			if (hasRtvHandle[attachmentIndex])
				boundRtvHandles.push_back(rtvHandles[attachmentIndex]);
		}

		if (!boundRtvHandles.empty() && hasDSV)
			m_commandList->OMSetRenderTargets(static_cast<UINT>(boundRtvHandles.size()), boundRtvHandles.data(), FALSE, &dsvHandle);
		else if (!boundRtvHandles.empty())
			m_commandList->OMSetRenderTargets(static_cast<UINT>(boundRtvHandles.size()), boundRtvHandles.data(), FALSE, nullptr);

		for (const uint32_t colorAttachmentIndex : clearPlan.colorAttachmentIndices)
		{
			if (colorAttachmentIndex >= rtvHandles.size() || !hasRtvHandle[colorAttachmentIndex])
				continue;

			const auto& clearValue = desc.colorAttachments[colorAttachmentIndex].clearValue;
			const FLOAT color[] = {
				clearValue.r,
				clearValue.g,
				clearValue.b,
				clearValue.a
			};
			m_commandList->ClearRenderTargetView(rtvHandles[colorAttachmentIndex], color, 1, &fullRect);
		}

		if (hasDSV && (clearPlan.clearDepth || clearPlan.clearStencil) && desc.depthStencilAttachment.has_value())
		{
			D3D12_CLEAR_FLAGS clearFlags = static_cast<D3D12_CLEAR_FLAGS>(0);
			if (clearPlan.clearDepth)
				clearFlags |= D3D12_CLEAR_FLAG_DEPTH;
			if (clearPlan.clearStencil)
				clearFlags |= D3D12_CLEAR_FLAG_STENCIL;

			const auto& clearValue = desc.depthStencilAttachment->clearValue;
			m_commandList->ClearDepthStencilView(
				dsvHandle,
				clearFlags,
				clearValue.depth,
				static_cast<UINT8>(clearValue.stencil),
				1,
				&fullRect);
		}
#else
		(void)desc;
#endif
	}

	void NativeDX12CommandBuffer::EndRenderPass()
	{
#if defined(_WIN32)
		if (m_commandList == nullptr || m_activeRenderPassTransitions.empty())
			return;

		std::vector<D3D12_RESOURCE_BARRIER> barriers;
		barriers.reserve(m_activeRenderPassTransitions.size());
		for (const auto& transition : m_activeRenderPassTransitions)
		{
			if (transition.resource == nullptr)
				continue;
			if (transition.stateAfterBegin == transition.stateAfterEnd)
				continue;

			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = transition.resource;
			barrier.Transition.StateBefore = transition.stateAfterBegin;
			barrier.Transition.StateAfter = transition.stateAfterEnd;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barriers.push_back(barrier);
		}

		if (!barriers.empty())
			m_commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
		for (const auto& transition : m_activeRenderPassTransitions)
		{
			if (auto* nativeTexture = dynamic_cast<NativeDX12Texture*>(transition.texture))
				nativeTexture->SetState(transition.textureStateAfterEnd);
		}
		m_activeRenderPassTransitions.clear();
#endif
	}

	void NativeDX12CommandBuffer::SetViewport(const NLS::Render::RHI::RHIViewport& viewport)
	{
#if defined(_WIN32)
		if (m_commandList == nullptr)
			return;
		D3D12_VIEWPORT vp{};
		vp.Width = viewport.width;
		vp.Height = viewport.height;
		vp.MinDepth = viewport.minDepth;
		vp.MaxDepth = viewport.maxDepth;
		m_commandList->RSSetViewports(1, &vp);
#else
		(void)viewport;
#endif
	}

	void NativeDX12CommandBuffer::SetScissor(const NLS::Render::RHI::RHIRect2D& rect)
	{
#if defined(_WIN32)
		if (m_commandList == nullptr)
			return;
		D3D12_RECT scissor{};
		scissor.left = rect.x;
		scissor.top = rect.y;
		scissor.right = rect.x + rect.width;
		scissor.bottom = rect.y + rect.height;
		m_commandList->RSSetScissorRects(1, &scissor);
#else
		(void)rect;
#endif
	}

	void NativeDX12CommandBuffer::BindGraphicsPipeline(const std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>& pipeline)
	{
#if defined(_WIN32)
		if (m_commandList == nullptr || pipeline == nullptr)
			return;

		auto* nativePipeline = dynamic_cast<IDX12GraphicsPipelineAccess*>(pipeline.get());
		if (nativePipeline == nullptr)
			return;

		ID3D12RootSignature* rootSig = nativePipeline->GetRootSignature();
		ID3D12PipelineState* pso = nativePipeline->GetPipelineState();

		if (rootSig != nullptr)
			m_commandList->SetGraphicsRootSignature(rootSig);
		if (pso != nullptr)
			m_commandList->SetPipelineState(pso);

		D3D12_PRIMITIVE_TOPOLOGY primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		switch (pipeline->GetDesc().primitiveTopology)
		{
		case NLS::Render::RHI::PrimitiveTopology::PointList:
			primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
			break;
		case NLS::Render::RHI::PrimitiveTopology::LineList:
			primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
			break;
		case NLS::Render::RHI::PrimitiveTopology::TriangleList:
		default:
			primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			break;
		}
		m_commandList->IASetPrimitiveTopology(primitiveTopology);
		if (pipeline->GetDesc().depthStencilState.stencilTest)
			m_commandList->OMSetStencilRef(pipeline->GetDesc().depthStencilState.stencilReference);

		m_boundDescriptorTables.clear();
		m_boundBindingSets.clear();
		if (pipeline->GetDesc().pipelineLayout != nullptr)
		{
			auto* nativePipelineLayout = dynamic_cast<IDX12PipelineLayoutAccess*>(pipeline->GetDesc().pipelineLayout.get());
			if (nativePipelineLayout != nullptr)
				m_boundDescriptorTables = nativePipelineLayout->GetDescriptorTables();
		}
		m_initializedRootDescriptorTables.assign(m_boundDescriptorTables.size(), false);

		m_boundPipeline = pipeline;
		m_boundComputePipeline.reset();
		m_bindingComputePipeline = false;
		m_recordedPipelineKeepAlive.push_back(pipeline);
#else
		(void)pipeline;
#endif
	}

	void NativeDX12CommandBuffer::BindComputePipeline(const std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>& pipeline)
	{
#if defined(_WIN32)
		if (m_commandList == nullptr || pipeline == nullptr)
			return;

		auto* nativePipeline = dynamic_cast<IDX12ComputePipelineAccess*>(pipeline.get());
		if (nativePipeline == nullptr)
			return;

		ID3D12RootSignature* rootSig = nativePipeline->GetRootSignature();
		ID3D12PipelineState* pso = nativePipeline->GetPipelineState();

		if (rootSig != nullptr)
			m_commandList->SetComputeRootSignature(rootSig);
		if (pso != nullptr)
			m_commandList->SetPipelineState(pso);

		m_boundDescriptorTables.clear();
		m_boundBindingSets.clear();
		if (pipeline->GetDesc().pipelineLayout != nullptr)
		{
			auto* nativePipelineLayout = dynamic_cast<IDX12PipelineLayoutAccess*>(pipeline->GetDesc().pipelineLayout.get());
			if (nativePipelineLayout != nullptr)
				m_boundDescriptorTables = nativePipelineLayout->GetDescriptorTables();
		}
		m_initializedRootDescriptorTables.assign(m_boundDescriptorTables.size(), false);

		m_boundPipeline.reset();
		m_boundComputePipeline = pipeline;
		m_bindingComputePipeline = true;
		m_recordedComputePipelineKeepAlive.push_back(pipeline);
#else
		(void)pipeline;
#endif
	}

	void NativeDX12CommandBuffer::BindBindingSet(uint32_t setIndex, const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& bindingSet)
	{
#if defined(_WIN32)
		if (m_commandList == nullptr || bindingSet == nullptr)
			return;

		auto* nativeBindingSet = ResolveDX12BindingSetAccess(bindingSet);
		if (nativeBindingSet == nullptr)
			return;

		if (m_boundDescriptorTables.empty())
			return;

		const auto existingBindingSetIt = std::find_if(
			m_boundBindingSets.begin(),
			m_boundBindingSets.end(),
			[setIndex](const auto& boundSet)
			{
				return boundSet.first == setIndex;
			});
		if (existingBindingSetIt != m_boundBindingSets.end())
			existingBindingSetIt->second = bindingSet;
		else
			m_boundBindingSets.emplace_back(setIndex, bindingSet);

		m_recordedBindingSetKeepAlive.push_back(bindingSet);

		ID3D12DescriptorHeap* desiredResourceHeap = nullptr;
		ID3D12DescriptorHeap* desiredSamplerHeap = nullptr;
		for (const auto& [boundSetIndex, boundSet] : m_boundBindingSets)
		{
			auto* boundNativeBindingSet = ResolveDX12BindingSetAccess(boundSet);
			if (boundNativeBindingSet == nullptr)
				continue;

			if (desiredResourceHeap == nullptr)
			{
				desiredResourceHeap =
					boundNativeBindingSet->GetDescriptorHeap(NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Resource);
			}
			if (desiredSamplerHeap == nullptr)
			{
				desiredSamplerHeap =
					boundNativeBindingSet->GetDescriptorHeap(NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Sampler);
			}
		}

		m_currentResourceDescriptorHeap = desiredResourceHeap;
		m_currentSamplerDescriptorHeap = desiredSamplerHeap;

		ID3D12DescriptorHeap* activeHeaps[2] = {};
		UINT activeHeapCount = 0;
		if (m_currentResourceDescriptorHeap != nullptr)
			activeHeaps[activeHeapCount++] = m_currentResourceDescriptorHeap;
		if (m_currentSamplerDescriptorHeap != nullptr)
			activeHeaps[activeHeapCount++] = m_currentSamplerDescriptorHeap;
		if (activeHeapCount > 0)
			m_commandList->SetDescriptorHeaps(activeHeapCount, activeHeaps);

		for (const auto& [boundSetIndex, boundSet] : m_boundBindingSets)
		{
			auto* boundNativeBindingSet = ResolveDX12BindingSetAccess(boundSet);
			if (boundNativeBindingSet == nullptr)
				continue;

			for (UINT rootParameterIndex = 0; rootParameterIndex < m_boundDescriptorTables.size(); ++rootParameterIndex)
			{
				const auto& table = m_boundDescriptorTables[rootParameterIndex];
				if (table.set != boundSetIndex)
					continue;

				const auto gpuHandle = boundNativeBindingSet->GetGPUHandle(table.set, table.heapKind);
				if (gpuHandle.ptr == 0)
					continue;

				if (m_bindingComputePipeline)
					m_commandList->SetComputeRootDescriptorTable(rootParameterIndex, gpuHandle);
				else
					m_commandList->SetGraphicsRootDescriptorTable(rootParameterIndex, gpuHandle);
				if (rootParameterIndex < m_initializedRootDescriptorTables.size())
					m_initializedRootDescriptorTables[rootParameterIndex] = true;
			}
		}
#else
		(void)setIndex;
		(void)bindingSet;
#endif
	}

	void NativeDX12CommandBuffer::PushConstants(
		NLS::Render::RHI::ShaderStageMask,
		uint32_t offset,
		uint32_t size,
		const void* data)
	{
#if defined(_WIN32)
		if (m_commandList == nullptr)
			return;
		m_commandList->SetGraphicsRoot32BitConstants(0, size / sizeof(uint32_t), data, offset / sizeof(uint32_t));
#else
		(void)offset;
		(void)size;
		(void)data;
#endif
	}

	void NativeDX12CommandBuffer::BindVertexBuffer(uint32_t slot, const NLS::Render::RHI::RHIVertexBufferView& view)
	{
#if defined(_WIN32)
		if (m_commandList == nullptr || view.buffer == nullptr)
			return;
		D3D12_VERTEX_BUFFER_VIEW vbView{};
		vbView.BufferLocation = view.buffer->GetGPUAddress() + view.offset;
		vbView.SizeInBytes = static_cast<UINT>(view.buffer->GetDesc().size - view.offset);
		vbView.StrideInBytes = view.stride;
		m_commandList->IASetVertexBuffers(slot, 1, &vbView);
#else
		(void)slot;
		(void)view;
#endif
	}

	void NativeDX12CommandBuffer::BindIndexBuffer(const NLS::Render::RHI::RHIIndexBufferView& view)
	{
#if defined(_WIN32)
		if (m_commandList == nullptr || view.buffer == nullptr)
			return;
		D3D12_INDEX_BUFFER_VIEW ibView{};
		ibView.BufferLocation = view.buffer->GetGPUAddress() + view.offset;
		ibView.SizeInBytes = static_cast<UINT>(view.buffer->GetDesc().size - view.offset);
		ibView.Format = view.indexType == NLS::Render::RHI::IndexType::UInt16
			? DXGI_FORMAT_R16_UINT
			: DXGI_FORMAT_R32_UINT;
		m_commandList->IASetIndexBuffer(&ibView);
#else
		(void)view;
#endif
	}

	void NativeDX12CommandBuffer::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
	{
#if defined(_WIN32)
		for (size_t rootParameterIndex = 0; rootParameterIndex < m_boundDescriptorTables.size(); ++rootParameterIndex)
		{
			if (rootParameterIndex < m_initializedRootDescriptorTables.size() &&
				m_initializedRootDescriptorTables[rootParameterIndex])
			{
				continue;
			}

			const auto& table = m_boundDescriptorTables[rootParameterIndex];
			NLS_LOG_ERROR(
				"NativeDX12CommandBuffer::Draw missing root descriptor table: commandList=" + m_debugName +
				" rootIndex=" + std::to_string(rootParameterIndex) +
				" set=" + std::to_string(table.set) +
				" heap=" + ToDescriptorHeapKindName(table.heapKind));
		}
		if (m_commandList != nullptr)
			m_commandList->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
#else
		(void)vertexCount;
		(void)instanceCount;
		(void)firstVertex;
		(void)firstInstance;
#endif
	}

	void NativeDX12CommandBuffer::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
	{
#if defined(_WIN32)
		if (m_commandList == nullptr || m_boundPipeline == nullptr)
			return;
		for (size_t rootParameterIndex = 0; rootParameterIndex < m_boundDescriptorTables.size(); ++rootParameterIndex)
		{
			if (rootParameterIndex < m_initializedRootDescriptorTables.size() &&
				m_initializedRootDescriptorTables[rootParameterIndex])
			{
				continue;
			}

			const auto& table = m_boundDescriptorTables[rootParameterIndex];
			NLS_LOG_ERROR(
				"NativeDX12CommandBuffer::DrawIndexed missing root descriptor table: commandList=" + m_debugName +
				" rootIndex=" + std::to_string(rootParameterIndex) +
				" set=" + std::to_string(table.set) +
				" heap=" + ToDescriptorHeapKindName(table.heapKind));
		}
		m_commandList->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
#else
		(void)indexCount;
		(void)instanceCount;
		(void)firstIndex;
		(void)vertexOffset;
		(void)firstInstance;
#endif
	}

	void NativeDX12CommandBuffer::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
	{
#if defined(_WIN32)
		for (size_t rootParameterIndex = 0; rootParameterIndex < m_boundDescriptorTables.size(); ++rootParameterIndex)
		{
			if (rootParameterIndex < m_initializedRootDescriptorTables.size() &&
				m_initializedRootDescriptorTables[rootParameterIndex])
			{
				continue;
			}

			const auto& table = m_boundDescriptorTables[rootParameterIndex];
			NLS_LOG_ERROR(
				"NativeDX12CommandBuffer::Dispatch missing root descriptor table: commandList=" + m_debugName +
				" rootIndex=" + std::to_string(rootParameterIndex) +
				" set=" + std::to_string(table.set) +
				" heap=" + ToDescriptorHeapKindName(table.heapKind));
		}
		if (m_commandList != nullptr && m_boundComputePipeline != nullptr)
			m_commandList->Dispatch(groupCountX, groupCountY, groupCountZ);
#else
		(void)groupCountX;
		(void)groupCountY;
		(void)groupCountZ;
#endif
	}

	void NativeDX12CommandBuffer::CopyBuffer(
		const std::shared_ptr<NLS::Render::RHI::RHIBuffer>& source,
		const std::shared_ptr<NLS::Render::RHI::RHIBuffer>& destination,
		const NLS::Render::RHI::RHIBufferCopyRegion& region)
	{
#if defined(_WIN32)
		if (m_commandList == nullptr || source == nullptr || destination == nullptr)
			return;

		auto srcHandle = source->GetNativeBufferHandle();
		auto dstHandle = destination->GetNativeBufferHandle();
		if (srcHandle.backend != NLS::Render::RHI::BackendType::DX12 || dstHandle.backend != NLS::Render::RHI::BackendType::DX12)
			return;

		auto* srcResource = static_cast<ID3D12Resource*>(srcHandle.handle);
		auto* dstResource = static_cast<ID3D12Resource*>(dstHandle.handle);
		if (srcResource == nullptr || dstResource == nullptr)
			return;

		m_commandList->CopyBufferRegion(dstResource, region.dstOffset, srcResource, region.srcOffset, region.size);
#else
		(void)source;
		(void)destination;
		(void)region;
#endif
	}

	void NativeDX12CommandBuffer::CopyBufferToTexture(const NLS::Render::RHI::RHIBufferToTextureCopyDesc& desc)
	{
#if defined(_WIN32)
		if (m_commandList == nullptr || desc.source == nullptr || desc.destination == nullptr)
			return;

		auto srcHandle = desc.source->GetNativeBufferHandle();
		auto* srcResource = srcHandle.backend == NLS::Render::RHI::BackendType::DX12
			? static_cast<ID3D12Resource*>(srcHandle.handle)
			: nullptr;
		auto dstHandle = desc.destination->GetNativeImageHandle();
		auto* dstResource = dstHandle.backend == NLS::Render::RHI::BackendType::DX12
			? static_cast<ID3D12Resource*>(dstHandle.handle)
			: nullptr;
		if (srcResource == nullptr || dstResource == nullptr)
			return;

		DXGI_FORMAT format = ToDXGIFormat(desc.destination->GetDesc().format);
		const uint32_t bytesPerPixel = GetBytesPerPixel(format);

		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
		footprint.Offset = desc.bufferOffset;
		footprint.Footprint.Format = format;
		footprint.Footprint.Width = desc.extent.width;
		footprint.Footprint.Height = desc.extent.height;
		footprint.Footprint.Depth = 1;
		footprint.Footprint.RowPitch = desc.extent.width * bytesPerPixel;

		D3D12_TEXTURE_COPY_LOCATION srcLocation{};
		srcLocation.pResource = srcResource;
		srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		srcLocation.PlacedFootprint = footprint;

		D3D12_TEXTURE_COPY_LOCATION dstLocation{};
		dstLocation.pResource = dstResource;
		dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dstLocation.SubresourceIndex = desc.mipLevel;

		D3D12_BOX srcBox{};
		srcBox.left = 0;
		srcBox.top = 0;
		srcBox.front = 0;
		srcBox.right = desc.extent.width;
		srcBox.bottom = desc.extent.height;
		srcBox.back = 1;

		m_commandList->CopyTextureRegion(&dstLocation, desc.textureOffset.x, desc.textureOffset.y, desc.textureOffset.z, &srcLocation, &srcBox);
#else
		(void)desc;
#endif
	}

	void NativeDX12CommandBuffer::CopyTexture(const NLS::Render::RHI::RHITextureCopyDesc& desc)
	{
#if defined(_WIN32)
		if (m_commandList == nullptr || desc.source == nullptr || desc.destination == nullptr)
			return;

		auto srcHandle = desc.source->GetNativeImageHandle();
		auto* srcResource = srcHandle.backend == NLS::Render::RHI::BackendType::DX12
			? static_cast<ID3D12Resource*>(srcHandle.handle)
			: nullptr;
		auto dstHandle = desc.destination->GetNativeImageHandle();
		auto* dstResource = dstHandle.backend == NLS::Render::RHI::BackendType::DX12
			? static_cast<ID3D12Resource*>(dstHandle.handle)
			: nullptr;
		if (srcResource == nullptr || dstResource == nullptr)
			return;

		D3D12_TEXTURE_COPY_LOCATION srcLocation{};
		srcLocation.pResource = srcResource;
		srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		srcLocation.SubresourceIndex = desc.sourceRange.baseMipLevel;

		D3D12_TEXTURE_COPY_LOCATION dstLocation{};
		dstLocation.pResource = dstResource;
		dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dstLocation.SubresourceIndex = desc.destinationRange.baseMipLevel;

		D3D12_BOX srcBox{};
		srcBox.left = desc.sourceOffset.x;
		srcBox.top = desc.sourceOffset.y;
		srcBox.front = desc.sourceOffset.z;
		srcBox.right = desc.sourceOffset.x + desc.extent.width;
		srcBox.bottom = desc.sourceOffset.y + desc.extent.height;
		srcBox.back = desc.sourceOffset.z + desc.extent.depth;

		m_commandList->CopyTextureRegion(&dstLocation, desc.destinationOffset.x, desc.destinationOffset.y, desc.destinationOffset.z, &srcLocation, &srcBox);
#else
		(void)desc;
#endif
	}

	void NativeDX12CommandBuffer::Barrier(const NLS::Render::RHI::RHIBarrierDesc& barrierDesc)
	{
#if defined(_WIN32)
		if (m_commandList == nullptr)
			return;

		std::vector<D3D12_RESOURCE_BARRIER> barriers;

		for (const auto& bufferBarrier : barrierDesc.bufferBarriers)
		{
			if (bufferBarrier.buffer == nullptr)
				continue;

			auto bufferHandle = bufferBarrier.buffer->GetNativeBufferHandle();
			auto* resource = bufferHandle.backend == NLS::Render::RHI::BackendType::DX12
				? static_cast<ID3D12Resource*>(bufferHandle.handle)
				: nullptr;
			if (resource == nullptr)
				continue;

			const auto stateBefore = ToD3D12ResourceState(bufferBarrier.before);
			const auto stateAfter = ToD3D12ResourceState(bufferBarrier.after);
			D3D12_RESOURCE_BARRIER barrier{};
			if (stateBefore == stateAfter)
			{
				if ((stateAfter & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) == 0)
					continue;
				barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
				barrier.UAV.pResource = resource;
				barriers.push_back(barrier);
				continue;
			}

			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = resource;
			barrier.Transition.StateBefore = ToD3D12ResourceState(bufferBarrier.before);
			barrier.Transition.StateAfter = ToD3D12ResourceState(bufferBarrier.after);
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barriers.push_back(barrier);
		}

		for (const auto& textureBarrier : barrierDesc.textureBarriers)
		{
			if (textureBarrier.texture == nullptr)
				continue;

			auto textureHandle = textureBarrier.texture->GetNativeImageHandle();
			auto* resource = textureHandle.backend == NLS::Render::RHI::BackendType::DX12
				? static_cast<ID3D12Resource*>(textureHandle.handle)
				: nullptr;
			if (resource == nullptr)
				continue;

			const auto stateBefore = ToD3D12ResourceState(textureBarrier.before);
			const auto stateAfter = ToD3D12ResourceState(textureBarrier.after);
			D3D12_RESOURCE_BARRIER barrier{};
			if (stateBefore == stateAfter)
			{
				if ((stateAfter & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) == 0)
					continue;
				barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
				barrier.UAV.pResource = resource;
				barriers.push_back(barrier);
				continue;
			}

			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = resource;
			barrier.Transition.StateBefore = ToD3D12ResourceState(textureBarrier.before);
			barrier.Transition.StateAfter = ToD3D12ResourceState(textureBarrier.after);
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barriers.push_back(barrier);
			if (auto* nativeTexture = dynamic_cast<NativeDX12Texture*>(textureBarrier.texture.get()))
				nativeTexture->SetState(textureBarrier.after);
		}

		if (!barriers.empty())
			m_commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
#else
		(void)barrierDesc;
#endif
	}

#if defined(_WIN32)
	ID3D12GraphicsCommandList* NativeDX12CommandBuffer::GetCommandList() const
	{
		return m_commandList.Get();
	}

	DXGI_FORMAT NativeDX12CommandBuffer::ToDXGIFormat(NLS::Render::RHI::TextureFormat format)
	{
		switch (format)
		{
		case NLS::Render::RHI::TextureFormat::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case NLS::Render::RHI::TextureFormat::RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
		case NLS::Render::RHI::TextureFormat::Depth24Stencil8: return DXGI_FORMAT_D24_UNORM_S8_UINT;
		case NLS::Render::RHI::TextureFormat::RGB8: return DXGI_FORMAT_R8G8B8A8_UNORM;
		default: return DXGI_FORMAT_R8G8B8A8_UNORM;
		}
	}

	uint32_t NativeDX12CommandBuffer::GetBytesPerPixel(DXGI_FORMAT format)
	{
		switch (format)
		{
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UINT:
		case DXGI_FORMAT_R8G8B8A8_SNORM:
		case DXGI_FORMAT_R8G8B8A8_SINT:
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
			return 4;
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_UNORM:
		case DXGI_FORMAT_R16G16B16A16_UINT:
		case DXGI_FORMAT_R16G16B16A16_SNORM:
		case DXGI_FORMAT_R16G16B16A16_SINT:
			return 8;
		default:
			return 4;
		}
	}

	D3D12_RESOURCE_STATES NativeDX12CommandBuffer::ToD3D12ResourceState(NLS::Render::RHI::ResourceState state)
	{
		D3D12_RESOURCE_STATES result = D3D12_RESOURCE_STATE_COMMON;
		if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::CopySrc))
			result |= D3D12_RESOURCE_STATE_COPY_SOURCE;
		if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::CopyDst))
			result |= D3D12_RESOURCE_STATE_COPY_DEST;
		if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::VertexBuffer))
			result |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
		if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::IndexBuffer))
			result |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
		if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::UniformBuffer))
			result |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
		if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::ShaderRead))
			result |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::ShaderWrite))
			result |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::RenderTarget))
			result |= D3D12_RESOURCE_STATE_RENDER_TARGET;
		if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::DepthRead))
			result |= D3D12_RESOURCE_STATE_DEPTH_READ;
		if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::DepthWrite))
			result |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
		if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::Present))
			result |= D3D12_RESOURCE_STATE_PRESENT;
		return result;
	}
#endif

	NativeDX12CommandPool::NativeDX12CommandPool(
		ID3D12Device* device,
		ID3D12CommandQueue* queue,
		NLS::Render::RHI::QueueType queueType,
#if defined(_WIN32)
		D3D12_COMMAND_LIST_TYPE commandListType,
#else
		int commandListType,
#endif
		const std::string& debugName)
		: m_device(device)
		, m_queue(queue)
		, m_queueType(queueType)
		, m_commandListType(commandListType)
		, m_debugName(debugName)
	{
	}

	std::string_view NativeDX12CommandPool::GetDebugName() const
	{
		return m_debugName;
	}

	NLS::Render::RHI::QueueType NativeDX12CommandPool::GetQueueType() const
	{
		return m_queueType;
	}

	std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> NativeDX12CommandPool::CreateCommandBuffer(std::string debugName)
	{
		return std::make_shared<NativeDX12CommandBuffer>(
			m_device,
			m_queue,
			m_commandListType,
			debugName.empty() ? m_debugName : debugName);
	}

	void NativeDX12CommandPool::Reset()
	{
	}
}
