#include "Rendering/RHI/Backends/DX12/DX12Command.h"

#include "Profiling/Profiler.h"
#include "Rendering/RHI/Backends/DX12/DX12DebugNameUtils.h"
#include "Rendering/RHI/Backends/DX12/DX12FormatUtils.h"
#include "Rendering/RHI/Backends/DX12/DX12InfoQueueUtils.h"
#include "Rendering/RHI/Backends/DX12/DX12RenderPassUtils.h"
#include "Rendering/RHI/Backends/DX12/DX12Resource.h"
#include "Rendering/RHI/Backends/DX12/DX12TextureViewUtils.h"

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

		constexpr UINT kPixEventAnsiVersion = 1u;

		void BeginDx12DebugEvent(ID3D12GraphicsCommandList* commandList, const std::string& label)
		{
			if (commandList == nullptr || label.empty())
				return;

			commandList->BeginEvent(
				kPixEventAnsiVersion,
				label.c_str(),
				static_cast<UINT>(label.size() + 1u));
		}

		void EndDx12DebugEvent(ID3D12GraphicsCommandList* commandList)
		{
			if (commandList == nullptr)
				return;

			commandList->EndEvent();
		}

		NLS::Render::RHI::DX12::ScopedDx12InfoQueueMessageFilter MakeBackbufferClearValueWarningFilter(
			ID3D12Device* device,
			const bool shouldFilter)
		{
			return NLS::Render::RHI::DX12::ScopedDx12InfoQueueMessageFilter(
				shouldFilter ? device : nullptr,
				D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE);
		}

		void AppendDX12TextureTransitionBarriers(
			std::vector<D3D12_RESOURCE_BARRIER>& barriers,
			ID3D12Resource* resource,
			const NLS::Render::RHI::RHITextureDesc& textureDesc,
			const NLS::Render::RHI::RHISubresourceRange& subresourceRange,
			const D3D12_RESOURCE_STATES stateBefore,
			const D3D12_RESOURCE_STATES stateAfter)
		{
			if (resource == nullptr)
				return;

			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = resource;
			barrier.Transition.StateBefore = stateBefore;
			barrier.Transition.StateAfter = stateAfter;

			if (NLS::Render::RHI::DX12::DoesDX12BarrierRangeCoverWholeTexture(textureDesc, subresourceRange))
			{
				barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				barriers.push_back(barrier);
				return;
			}

			const auto subresourceIndices =
				NLS::Render::RHI::DX12::BuildDX12BarrierSubresourceIndices(textureDesc, subresourceRange);
			for (const auto subresourceIndex : subresourceIndices)
			{
				barrier.Transition.Subresource = subresourceIndex;
				barriers.push_back(barrier);
			}
		}

		bool IsLegalCpuVisibleBufferState(
			const NLS::Render::RHI::RHIBuffer& buffer,
			const NLS::Render::RHI::ResourceState state)
		{
			const auto memoryUsage = buffer.GetDesc().memoryUsage;
			if (memoryUsage == NLS::Render::RHI::MemoryUsage::CPUToGPU)
				return state == NLS::Render::RHI::ResourceState::Unknown ||
					state == NLS::Render::RHI::ResourceState::GenericRead;
			if (memoryUsage == NLS::Render::RHI::MemoryUsage::GPUToCPU)
				return state == NLS::Render::RHI::ResourceState::Unknown ||
					state == NLS::Render::RHI::ResourceState::CopyDst;
			return true;
		}

		bool IsLegalCpuVisibleBufferTransition(
			const NLS::Render::RHI::RHIBuffer& buffer,
			const NLS::Render::RHI::ResourceState before,
			const NLS::Render::RHI::ResourceState after)
		{
			return IsLegalCpuVisibleBufferState(buffer, before) &&
				IsLegalCpuVisibleBufferState(buffer, after);
		}

		bool IsD3D12CommonPromotionAllowedForTexture(const NLS::Render::RHI::ResourceState state)
		{
			constexpr uint32_t promotableStates =
				static_cast<uint32_t>(NLS::Render::RHI::ResourceState::CopySrc) |
				static_cast<uint32_t>(NLS::Render::RHI::ResourceState::CopyDst) |
				static_cast<uint32_t>(NLS::Render::RHI::ResourceState::ShaderRead);
			const auto stateMask = static_cast<uint32_t>(state);
			return stateMask != 0u && (stateMask & ~promotableStates) == 0u;
		}

		bool ShouldFilterUnresolvedPartialTextureBarrier(
			const NLS::Render::RHI::RHITexture& texture,
			const NLS::Render::RHI::RHITextureBarrier& barrier)
		{
			const bool coversWholeTexture =
				NLS::Render::RHI::DX12::DoesDX12BarrierRangeCoverWholeTexture(
					texture.GetDesc(),
					barrier.subresourceRange);
			return barrier.before == NLS::Render::RHI::ResourceState::Unknown &&
				!coversWholeTexture;
		}

		bool IsValidDX12BufferCopyEndpoint(
			const NLS::Render::RHI::RHIBuffer& source,
			const NLS::Render::RHI::RHIBuffer& destination,
			const std::string& debugName)
		{
			const auto sourceMemory = source.GetDesc().memoryUsage;
			const auto destinationMemory = destination.GetDesc().memoryUsage;
			if (destinationMemory == NLS::Render::RHI::MemoryUsage::CPUToGPU)
			{
				NLS_LOG_ERROR("NativeDX12CommandBuffer::CopyBuffer rejected CPUToGPU destination: " + debugName);
				return false;
			}
			if (sourceMemory == NLS::Render::RHI::MemoryUsage::GPUToCPU)
			{
				NLS_LOG_ERROR("NativeDX12CommandBuffer::CopyBuffer rejected GPUToCPU source: " + debugName);
				return false;
			}
			return true;
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
		while (!m_debugEventScopeStack.empty())
		{
			EndDx12DebugEvent(m_commandList.Get());
			m_debugEventScopeStack.pop_back();
		}

		while (!m_gpuProfileScopeStack.empty())
		{
			auto event = std::move(m_gpuProfileScopeStack.back());
			m_gpuProfileScopeStack.pop_back();
			NLS::Base::Profiling::Profiler::EndGpuScope(event);
		}
#endif
	}

	void NativeDX12CommandBuffer::ClearBoundPipelineState()
	{
		m_boundPipeline.reset();
		m_boundComputePipeline.reset();
		m_boundDescriptorTables.clear();
		m_boundPushConstantRootParameters.clear();
		m_initializedRootDescriptorTables.clear();
		m_boundBindingSets.clear();
#if defined(_WIN32)
		m_bindingComputePipeline = false;
#endif
	}

#if defined(_WIN32)
	bool NativeDX12CommandBuffer::IsBoundGraphicsPipelineNativeValid() const
	{
		if (m_boundPipeline == nullptr)
			return false;

		const auto* nativePipeline = dynamic_cast<const IDX12GraphicsPipelineAccess*>(m_boundPipeline.get());
		return nativePipeline != nullptr &&
			nativePipeline->GetRootSignature() != nullptr &&
			nativePipeline->GetPipelineState() != nullptr;
	}

	bool NativeDX12CommandBuffer::IsBoundComputePipelineNativeValid() const
	{
		if (m_boundComputePipeline == nullptr)
			return false;

		const auto* nativePipeline = dynamic_cast<const IDX12ComputePipelineAccess*>(m_boundComputePipeline.get());
		return nativePipeline != nullptr &&
			nativePipeline->GetRootSignature() != nullptr &&
			nativePipeline->GetPipelineState() != nullptr;
	}

	bool NativeDX12CommandBuffer::HasInitializedRequiredRootDescriptorTables(std::string_view operationName) const
	{
		bool allRequiredTablesInitialized = true;
		for (size_t rootParameterIndex = 0; rootParameterIndex < m_boundDescriptorTables.size(); ++rootParameterIndex)
		{
			if (rootParameterIndex < m_initializedRootDescriptorTables.size() &&
				m_initializedRootDescriptorTables[rootParameterIndex])
			{
				continue;
			}

			allRequiredTablesInitialized = false;
			const auto& table = m_boundDescriptorTables[rootParameterIndex];
			NLS_LOG_ERROR(
				"NativeDX12CommandBuffer::" + std::string(operationName) +
				" missing root descriptor table: commandList=" + m_debugName +
				" rootIndex=" + std::to_string(rootParameterIndex) +
				" set=" + std::to_string(table.set) +
				" heap=" + ToDescriptorHeapKindName(table.heapKind));
		}
		return allRequiredTablesInitialized;
	}
#endif

	void NativeDX12CommandBuffer::Begin()
	{
#if defined(_WIN32)
		if (m_recording)
			EndPendingGpuProfileScopes();

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
			m_boundPushConstantRootParameters.clear();
			m_initializedRootDescriptorTables.clear();
			m_boundBindingSets.clear();
			m_recordedTextureViewKeepAlive.clear();
			m_recordedBindingSetKeepAlive.clear();
			m_recordedPipelineKeepAlive.clear();
			m_recordedComputePipelineKeepAlive.clear();
			m_recordedBufferKeepAlive.clear();
			m_partialTextureStateDirty.clear();
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
			EndPendingGpuProfileScopes();
			m_commandList->Close();
			m_recording = false;
		}
#endif
	}

	void NativeDX12CommandBuffer::Reset()
	{
#if defined(_WIN32)
		if (m_recording)
			EndPendingGpuProfileScopes();

		m_activeRenderPassTransitions.clear();
		m_currentResourceDescriptorHeap = nullptr;
		m_currentSamplerDescriptorHeap = nullptr;
		m_boundDescriptorTables.clear();
		m_boundPushConstantRootParameters.clear();
		m_initializedRootDescriptorTables.clear();
		m_boundBindingSets.clear();
		m_recordedTextureViewKeepAlive.clear();
		m_recordedBindingSetKeepAlive.clear();
		m_recordedPipelineKeepAlive.clear();
		m_recordedComputePipelineKeepAlive.clear();
		m_recordedBufferKeepAlive.clear();
		m_partialTextureStateDirty.clear();
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

	NLS::Render::RHI::NativeHandle NativeDX12CommandBuffer::GetNativeCommandBuffer() const
	{
#if defined(_WIN32)
		return { NLS::Render::RHI::BackendType::DX12, m_commandList.Get() };
#else
		return {};
#endif
	}

	void NativeDX12CommandBuffer::BeginGpuProfileScope(
		const std::string_view name,
		const std::string_view sourceFunction)
	{
#if defined(_WIN32)
		const auto debugLabel = NLS::Render::RHI::DX12::BuildDX12GpuScopeDebugLabel(name, sourceFunction);
		BeginDx12DebugEvent(m_commandList.Get(), debugLabel);
		m_debugEventScopeStack.push_back({ DebugEventScopeKind::GpuProfile, debugLabel });

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
		if (!m_gpuProfileScopeStack.empty())
		{
			auto event = std::move(m_gpuProfileScopeStack.back());
			m_gpuProfileScopeStack.pop_back();
			NLS::Base::Profiling::Profiler::EndGpuScope(event);
		}

		if (!m_debugEventScopeStack.empty())
		{
			auto& scope = m_debugEventScopeStack.back();
			if (scope.kind == DebugEventScopeKind::GpuProfile)
			{
				EndDx12DebugEvent(m_commandList.Get());
				m_debugEventScopeStack.pop_back();
			}
		}
#endif
	}

	void NativeDX12CommandBuffer::BeginRenderPass(const NLS::Render::RHI::RHIRenderPassDesc& desc)
	{
#if defined(_WIN32)
		if (m_commandList == nullptr)
			return;

		const auto debugLabel = NLS::Render::RHI::DX12::BuildDX12GpuScopeDebugLabel(desc.debugName, "BeginRenderPass");
		BeginDx12DebugEvent(m_commandList.Get(), debugLabel);
		m_debugEventScopeStack.push_back({ DebugEventScopeKind::RenderPass, debugLabel });

		m_activeRenderPassTransitions.clear();
		const auto clearPlan = NLS::Render::RHI::DX12::BuildDX12RenderPassClearPlan(desc);
		const bool isBackbufferPass = desc.debugName == "BackbufferRenderPass";
		if (!desc.attachmentsRequireExternalStateTransitions)
		{
			std::vector<D3D12_RESOURCE_BARRIER> beginBarriers;
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
				const auto& subresourceRange = colorAttachment.view->GetDesc().subresourceRange;
				const bool coversWholeTexture =
					NLS::Render::RHI::DX12::DoesDX12BarrierRangeCoverWholeTexture(
						texture->GetDesc(),
						subresourceRange);

				AppendDX12TextureTransitionBarriers(
					beginBarriers,
					resource,
					texture->GetDesc(),
					subresourceRange,
					stateBefore,
					stateAfter);

				m_activeRenderPassTransitions.push_back({
					resource,
					stateAfter,
					stateAfterPass,
					texture.get(),
					isBackbufferPass
						? NLS::Render::RHI::ResourceState::Present
						: NLS::Render::RHI::ResourceState::Unknown,
					subresourceRange,
					coversWholeTexture
				});
				if (auto* nativeTexture = dynamic_cast<NativeDX12Texture*>(texture.get()))
				{
					if (coversWholeTexture)
						nativeTexture->SetState(NLS::Render::RHI::ResourceState::RenderTarget);
					else
					{
						nativeTexture->MarkPartialStateDirty();
						m_partialTextureStateDirty.insert(texture.get());
					}
				}
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
						const D3D12_RESOURCE_STATES stateBefore = ToD3D12ResourceState(texture->GetState());
						const auto depthStencilState = desc.depthStencilAttachment->readOnlyDepthStencil
							? D3D12_RESOURCE_STATE_DEPTH_READ
							: D3D12_RESOURCE_STATE_DEPTH_WRITE;
						const auto textureState = desc.depthStencilAttachment->readOnlyDepthStencil
							? NLS::Render::RHI::ResourceState::DepthRead
							: NLS::Render::RHI::ResourceState::DepthWrite;
						const auto& subresourceRange = desc.depthStencilAttachment->view->GetDesc().subresourceRange;
						const bool coversWholeTexture =
							NLS::Render::RHI::DX12::DoesDX12BarrierRangeCoverWholeTexture(
								texture->GetDesc(),
								subresourceRange);
						AppendDX12TextureTransitionBarriers(
							beginBarriers,
							resource,
							texture->GetDesc(),
							subresourceRange,
							stateBefore,
							depthStencilState);

						m_activeRenderPassTransitions.push_back({
							resource,
							depthStencilState,
							D3D12_RESOURCE_STATE_COMMON,
							texture.get(),
							NLS::Render::RHI::ResourceState::Unknown,
							subresourceRange,
							coversWholeTexture
						});
						if (auto* nativeTexture = dynamic_cast<NativeDX12Texture*>(texture.get()))
						{
							if (coversWholeTexture)
								nativeTexture->SetState(textureState);
							else
							{
								nativeTexture->MarkPartialStateDirty();
								m_partialTextureStateDirty.insert(texture.get());
							}
						}
					}
				}
			}
			if (!beginBarriers.empty())
				m_commandList->ResourceBarrier(static_cast<UINT>(beginBarriers.size()), beginBarriers.data());
		}
		for (const auto& colorAttachment : desc.colorAttachments)
		{
			if (colorAttachment.view != nullptr)
				m_recordedTextureViewKeepAlive.push_back(colorAttachment.view);
		}
		if (desc.depthStencilAttachment.has_value() && desc.depthStencilAttachment->view != nullptr)
			m_recordedTextureViewKeepAlive.push_back(desc.depthStencilAttachment->view);

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
			const auto access = desc.depthStencilAttachment->readOnlyDepthStencil
				? NLS::Render::RHI::RHIDepthStencilViewAccess::ReadOnlyDepthStencil
				: NLS::Render::RHI::RHIDepthStencilViewAccess::ReadWrite;
			NLS::Render::RHI::NativeHandle dsvHandleNative =
				desc.depthStencilAttachment->view->GetNativeDepthStencilView(access);
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

		for (const auto& clearRequest : clearPlan.colorClearRequests)
		{
			const uint32_t colorAttachmentIndex = clearRequest.attachmentIndex;
			if (colorAttachmentIndex >= rtvHandles.size() || !hasRtvHandle[colorAttachmentIndex])
				continue;

			const auto& clearValue = desc.colorAttachments[colorAttachmentIndex].clearValue;
			const FLOAT color[] = {
				clearValue.r,
				clearValue.g,
				clearValue.b,
				clearValue.a
			};
			const auto clearValueWarningFilter = MakeBackbufferClearValueWarningFilter(
				m_device,
				clearRequest.suppressClearValueMismatchWarning);
			const auto clearValueWarningScope = clearRequest.suppressClearValueMismatchWarning
				? std::unique_ptr<NLS::Render::RHI::DX12::ScopedDx12InfoQueueMessageScope>{}
				: std::make_unique<NLS::Render::RHI::DX12::ScopedDx12InfoQueueMessageScope>();
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
		if (m_commandList == nullptr)
			return;

		if (!m_activeRenderPassTransitions.empty())
		{
			std::vector<D3D12_RESOURCE_BARRIER> barriers;
			barriers.reserve(m_activeRenderPassTransitions.size());
			for (const auto& transition : m_activeRenderPassTransitions)
			{
				if (transition.resource == nullptr)
					continue;
				if (transition.stateAfterBegin == transition.stateAfterEnd)
					continue;

				if (transition.texture == nullptr)
					continue;

				AppendDX12TextureTransitionBarriers(
					barriers,
					transition.resource,
					transition.texture->GetDesc(),
					transition.subresourceRange,
					transition.stateAfterBegin,
					transition.stateAfterEnd);
			}

			if (!barriers.empty())
				m_commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
			for (const auto& transition : m_activeRenderPassTransitions)
			{
				if (auto* nativeTexture = dynamic_cast<NativeDX12Texture*>(transition.texture))
				{
					if (transition.coversWholeTexture)
						nativeTexture->SetState(transition.textureStateAfterEnd);
					else
					{
						nativeTexture->MarkPartialStateDirty();
						m_partialTextureStateDirty.insert(transition.texture);
					}
				}
			}
			m_activeRenderPassTransitions.clear();
		}

		if (!m_debugEventScopeStack.empty())
		{
			auto& scope = m_debugEventScopeStack.back();
			if (scope.kind == DebugEventScopeKind::RenderPass)
			{
				EndDx12DebugEvent(m_commandList.Get());
				m_debugEventScopeStack.pop_back();
			}
		}
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
		if (rootSig == nullptr || pso == nullptr)
		{
			NLS_LOG_ERROR("NativeDX12CommandBuffer::BindGraphicsPipeline rejected invalid native pipeline: " + std::string(pipeline->GetDebugName()));
			ClearBoundPipelineState();
			return;
		}

		m_commandList->SetGraphicsRootSignature(rootSig);
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
		m_boundPushConstantRootParameters.clear();
		m_boundBindingSets.clear();
		if (pipeline->GetDesc().pipelineLayout != nullptr)
		{
			auto* nativePipelineLayout = dynamic_cast<IDX12PipelineLayoutAccess*>(pipeline->GetDesc().pipelineLayout.get());
			if (nativePipelineLayout != nullptr)
			{
				m_boundDescriptorTables = nativePipelineLayout->GetDescriptorTables();
				m_boundPushConstantRootParameters = nativePipelineLayout->GetPushConstantRootParameters();
			}
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
		if (rootSig == nullptr || pso == nullptr)
		{
			NLS_LOG_ERROR("NativeDX12CommandBuffer::BindComputePipeline rejected invalid native pipeline: " + std::string(pipeline->GetDebugName()));
			ClearBoundPipelineState();
			return;
		}

		m_commandList->SetComputeRootSignature(rootSig);
		m_commandList->SetPipelineState(pso);

		m_boundDescriptorTables.clear();
		m_boundPushConstantRootParameters.clear();
		m_boundBindingSets.clear();
		if (pipeline->GetDesc().pipelineLayout != nullptr)
		{
			auto* nativePipelineLayout = dynamic_cast<IDX12PipelineLayoutAccess*>(pipeline->GetDesc().pipelineLayout.get());
			if (nativePipelineLayout != nullptr)
			{
				m_boundDescriptorTables = nativePipelineLayout->GetDescriptorTables();
				m_boundPushConstantRootParameters = nativePipelineLayout->GetPushConstantRootParameters();
			}
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
				if (!boundNativeBindingSet->IsCompatibleWithDescriptorTable(table))
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
		NLS::Render::RHI::ShaderStageMask stageMask,
		uint32_t offset,
		uint32_t size,
		const void* data)
	{
#if defined(_WIN32)
		if (m_commandList == nullptr || data == nullptr || size == 0u || (size % sizeof(uint32_t)) != 0u || (offset % sizeof(uint32_t)) != 0u)
			return;

		const uint64_t writeBegin = offset;
		const uint64_t writeEnd = writeBegin + size;
		const auto rootParameterIt = std::find_if(
			m_boundPushConstantRootParameters.begin(),
			m_boundPushConstantRootParameters.end(),
			[stageMask, writeBegin, writeEnd](const auto& rootParameter)
			{
				const auto stageOverlap = rootParameter.stageMask & stageMask;
				if (stageOverlap == NLS::Render::RHI::ShaderStageMask::None)
					return false;

				const uint64_t rangeBegin = rootParameter.offset;
				const uint64_t rangeEnd = rangeBegin + rootParameter.size;
				return writeBegin >= rangeBegin && writeEnd <= rangeEnd;
			});
		if (rootParameterIt == m_boundPushConstantRootParameters.end())
			return;

		const uint32_t destinationOffsetIn32BitValues = (offset - rootParameterIt->offset) / sizeof(uint32_t);
		const uint32_t valueCount = size / sizeof(uint32_t);
		const auto rootParameterIndex = rootParameterIt->rootParameterIndex;
		if (m_bindingComputePipeline)
			m_commandList->SetComputeRoot32BitConstants(rootParameterIndex, valueCount, data, destinationOffsetIn32BitValues);
		else
			m_commandList->SetGraphicsRoot32BitConstants(rootParameterIndex, valueCount, data, destinationOffsetIn32BitValues);
#else
		(void)stageMask;
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
		m_recordedBufferKeepAlive.push_back(view.buffer);
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
		m_recordedBufferKeepAlive.push_back(view.buffer);
#else
		(void)view;
#endif
	}

	void NativeDX12CommandBuffer::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
	{
#if defined(_WIN32)
		if (m_commandList == nullptr || m_boundPipeline == nullptr || !IsBoundGraphicsPipelineNativeValid())
			return;
		if (!HasInitializedRequiredRootDescriptorTables("Draw"))
			return;
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
		if (m_commandList == nullptr || m_boundPipeline == nullptr || !IsBoundGraphicsPipelineNativeValid())
			return;
		if (!HasInitializedRequiredRootDescriptorTables("DrawIndexed"))
			return;
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
		if (m_commandList != nullptr &&
			m_boundComputePipeline != nullptr &&
			IsBoundComputePipelineNativeValid() &&
			HasInitializedRequiredRootDescriptorTables("Dispatch"))
		{
			m_commandList->Dispatch(groupCountX, groupCountY, groupCountZ);
		}
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
		if (!IsValidDX12BufferCopyEndpoint(*source, *destination, m_debugName))
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
		const uint32_t rowPitch = desc.rowPitch != 0u
			? desc.rowPitch
			: desc.extent.width * bytesPerPixel;

		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
		footprint.Offset = desc.bufferOffset;
		footprint.Footprint.Format = format;
		footprint.Footprint.Width = desc.extent.width;
		footprint.Footprint.Height = desc.extent.height;
		footprint.Footprint.Depth = 1;
		footprint.Footprint.RowPitch = rowPitch;

		D3D12_TEXTURE_COPY_LOCATION srcLocation{};
		srcLocation.pResource = srcResource;
		srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		srcLocation.PlacedFootprint = footprint;

		D3D12_TEXTURE_COPY_LOCATION dstLocation{};
		dstLocation.pResource = dstResource;
		dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		const auto& destinationDesc = desc.destination->GetDesc();
		const uint32_t mipLevels = (std::max)(destinationDesc.mipLevels, 1u);
		dstLocation.SubresourceIndex = desc.arrayLayer * mipLevels + desc.mipLevel;

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

	NLS::Render::RHI::RHIBarrierDesc NativeDX12CommandBuffer::FilterBarrierDesc(
		const NLS::Render::RHI::RHIBarrierDesc& barrier) const
	{
#if defined(_WIN32)
		NLS::Render::RHI::RHIBarrierDesc filtered;
		filtered.bufferBarriers = barrier.bufferBarriers;
		filtered.textureBarriers.reserve(barrier.textureBarriers.size());

		for (const auto& textureBarrier : barrier.textureBarriers)
		{
			if (textureBarrier.texture == nullptr)
				continue;

			if (auto* nativeTexture = dynamic_cast<NativeDX12Texture*>(textureBarrier.texture.get());
				nativeTexture != nullptr &&
				ShouldFilterUnresolvedPartialTextureBarrier(*nativeTexture, textureBarrier))
			{
				nativeTexture->MarkPartialStateDirty();
				continue;
			}

			filtered.textureBarriers.push_back(textureBarrier);
		}

		return filtered;
#else
		(void)barrier;
		return {};
#endif
	}

	NLS::Render::RHI::RHICommandRecordingResult NativeDX12CommandBuffer::BarrierChecked(
		const NLS::Render::RHI::RHIBarrierDesc& barrierDesc)
	{
#if defined(_WIN32)
		if (m_commandList == nullptr)
		{
			return {
				NLS::Render::RHI::RHICommandRecordingStatusCode::InvalidArgument,
				"NativeDX12CommandBuffer::Barrier: command list is null"
			};
		}

		const auto filteredBarrierDesc = FilterBarrierDesc(barrierDesc);
		std::vector<D3D12_RESOURCE_BARRIER> barriers;

		for (const auto& bufferBarrier : filteredBarrierDesc.bufferBarriers)
		{
			if (bufferBarrier.buffer == nullptr)
				continue;
			const auto& bufferDesc = bufferBarrier.buffer->GetDesc();
			if (bufferDesc.memoryUsage == NLS::Render::RHI::MemoryUsage::CPUToGPU ||
				bufferDesc.memoryUsage == NLS::Render::RHI::MemoryUsage::GPUToCPU)
			{
				auto effectiveBefore = bufferBarrier.before;
				if (effectiveBefore == NLS::Render::RHI::ResourceState::Unknown)
					effectiveBefore = bufferBarrier.buffer->GetState();
				if (IsLegalCpuVisibleBufferTransition(*bufferBarrier.buffer, effectiveBefore, bufferBarrier.after))
					continue;

				NLS_LOG_ERROR(
					"NativeDX12CommandBuffer::Barrier rejected illegal CPU-visible buffer state transition: " +
					std::string(bufferBarrier.buffer->GetDebugName()));
				continue;
			}

			auto bufferHandle = bufferBarrier.buffer->GetNativeBufferHandle();
			auto* resource = bufferHandle.backend == NLS::Render::RHI::BackendType::DX12
				? static_cast<ID3D12Resource*>(bufferHandle.handle)
				: nullptr;
			if (resource == nullptr)
				continue;

			auto effectiveBefore = bufferBarrier.before;
			if (effectiveBefore == NLS::Render::RHI::ResourceState::Unknown && bufferBarrier.buffer != nullptr)
				effectiveBefore = bufferBarrier.buffer->GetState();
			const auto stateBefore = ToD3D12ResourceState(effectiveBefore);
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
			barrier.Transition.StateBefore = stateBefore;
			barrier.Transition.StateAfter = stateAfter;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barriers.push_back(barrier);
			if (auto* nativeBuffer = dynamic_cast<NativeDX12Buffer*>(bufferBarrier.buffer.get()))
				nativeBuffer->SetState(bufferBarrier.after);
		}

		for (const auto& textureBarrier : filteredBarrierDesc.textureBarriers)
		{
			if (textureBarrier.texture == nullptr)
				continue;

			auto textureHandle = textureBarrier.texture->GetNativeImageHandle();
			auto* resource = textureHandle.backend == NLS::Render::RHI::BackendType::DX12
				? static_cast<ID3D12Resource*>(textureHandle.handle)
				: nullptr;
			if (resource == nullptr)
				continue;

			const bool coversWholeTexture =
				NLS::Render::RHI::DX12::DoesDX12BarrierRangeCoverWholeTexture(
					textureBarrier.texture->GetDesc(),
					textureBarrier.subresourceRange);
			auto effectiveBefore = textureBarrier.before;
			auto* nativeTexture = dynamic_cast<NativeDX12Texture*>(textureBarrier.texture.get());
			const bool wholeTextureStateKnown =
				nativeTexture == nullptr
					? m_partialTextureStateDirty.find(textureBarrier.texture.get()) == m_partialTextureStateDirty.end()
					: !nativeTexture->HasPartialStateDirty();
			if (effectiveBefore == NLS::Render::RHI::ResourceState::Unknown && wholeTextureStateKnown)
			{
				effectiveBefore = textureBarrier.texture->GetState();
			}
			if (effectiveBefore == NLS::Render::RHI::ResourceState::Unknown &&
				!wholeTextureStateKnown &&
				!coversWholeTexture &&
				IsD3D12CommonPromotionAllowedForTexture(textureBarrier.after))
			{
				if (nativeTexture != nullptr)
					nativeTexture->MarkPartialStateDirty();
				m_partialTextureStateDirty.insert(textureBarrier.texture.get());
				continue;
			}
			const auto stateBefore = ToD3D12ResourceState(effectiveBefore);
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
			barrier.Transition.StateBefore = stateBefore;
			barrier.Transition.StateAfter = ToD3D12ResourceState(textureBarrier.after);
			if (coversWholeTexture)
			{
				barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				barriers.push_back(barrier);
			}
			else
			{
				const auto subresourceIndices =
					NLS::Render::RHI::DX12::BuildDX12BarrierSubresourceIndices(
						textureBarrier.texture->GetDesc(),
						textureBarrier.subresourceRange);
				if (subresourceIndices.empty())
					continue;
				for (const auto subresourceIndex : subresourceIndices)
				{
					barrier.Transition.Subresource = subresourceIndex;
					barriers.push_back(barrier);
				}
			}
			if (coversWholeTexture)
			{
				if (nativeTexture != nullptr)
					nativeTexture->SetState(textureBarrier.after);
			}
			else
			{
				if (nativeTexture != nullptr)
					nativeTexture->MarkPartialStateDirty();
				m_partialTextureStateDirty.insert(textureBarrier.texture.get());
			}
		}

		if (!barriers.empty())
			m_commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
		return {};
#else
		(void)barrierDesc;
		return {
			NLS::Render::RHI::RHICommandRecordingStatusCode::BackendFailure,
			"DX12 command barrier is only available on Windows"
		};
#endif
	}

	void NativeDX12CommandBuffer::Barrier(const NLS::Render::RHI::RHIBarrierDesc& barrierDesc)
	{
		(void)BarrierChecked(barrierDesc);
	}

#if defined(_WIN32)
	ID3D12GraphicsCommandList* NativeDX12CommandBuffer::GetCommandList() const
	{
		return m_commandList.Get();
	}

	DXGI_FORMAT NativeDX12CommandBuffer::ToDXGIFormat(NLS::Render::RHI::TextureFormat format)
	{
		const DXGI_FORMAT dxgiFormat = NLS::Render::RHI::DX12::ToDXGIFormat(format);
		return dxgiFormat != DXGI_FORMAT_UNKNOWN ? dxgiFormat : DXGI_FORMAT_R8G8B8A8_UNORM;
	}

	uint32_t NativeDX12CommandBuffer::GetBytesPerPixel(DXGI_FORMAT format)
	{
		const uint32_t bytesPerPixel = NLS::Render::RHI::DX12::GetDXGIFormatBytesPerPixel(format);
		return bytesPerPixel != 0u ? bytesPerPixel : 4u;
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
		if (state == NLS::Render::RHI::ResourceState::GenericRead)
			result = D3D12_RESOURCE_STATE_GENERIC_READ;
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
