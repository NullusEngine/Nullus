#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Profiling/Profiler.h"
#include "Rendering/RHI/Backends/DX12/DX12Access.h"
#include "Rendering/RHI/Backends/DX12/DX12PipelineLayoutUtils.h"
#include "Rendering/RHI/Core/RHICommand.h"

struct ID3D12CommandQueue;
struct ID3D12DescriptorHeap;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12PipelineState;
struct ID3D12Resource;
struct ID3D12RootSignature;

#if defined(_WIN32)
#include <d3d12.h>
#include <wrl/client.h>
#undef CreateSemaphore
#endif

namespace NLS::Render::Backend
{
	class NativeDX12CommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
	{
	public:
#if defined(_WIN32)
		NativeDX12CommandBuffer(
			ID3D12Device* device,
			ID3D12CommandQueue* queue,
			D3D12_COMMAND_LIST_TYPE commandListType,
			const std::string& debugName);
#else
		NativeDX12CommandBuffer(
			ID3D12Device* device,
			ID3D12CommandQueue* queue,
			int commandListType,
			const std::string& debugName);
#endif
		~NativeDX12CommandBuffer() override;

		std::string_view GetDebugName() const override;
		void Begin() override;
		void End() override;
		void Reset() override;
		bool IsRecording() const override;
		NLS::Render::RHI::NativeHandle GetNativeCommandBuffer() const override;
		void BeginGpuProfileScope(std::string_view name, std::string_view sourceFunction) override;
		void EndGpuProfileScope() override;
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
		void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override;
		void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override;
		NLS::Render::RHI::RHICommandRecordingResult DrawChecked(
			uint32_t vertexCount,
			uint32_t instanceCount,
			uint32_t firstVertex,
			uint32_t firstInstance) override;
		NLS::Render::RHI::RHICommandRecordingResult DrawIndexedChecked(
			uint32_t indexCount,
			uint32_t instanceCount,
			uint32_t firstIndex,
			int32_t vertexOffset,
			uint32_t firstInstance) override;
		void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;
		void CopyBuffer(
			const std::shared_ptr<NLS::Render::RHI::RHIBuffer>& source,
			const std::shared_ptr<NLS::Render::RHI::RHIBuffer>& destination,
			const NLS::Render::RHI::RHIBufferCopyRegion& region) override;
		void CopyBufferToTexture(const NLS::Render::RHI::RHIBufferToTextureCopyDesc& desc) override;
		void CopyTexture(const NLS::Render::RHI::RHITextureCopyDesc& desc) override;
		NLS::Render::RHI::RHIBarrierDesc FilterBarrierDesc(const NLS::Render::RHI::RHIBarrierDesc& barrier) const override;
		bool IsClosedForSubmission() const override;
		NLS::Render::RHI::RHICommandRecordingResult BarrierChecked(const NLS::Render::RHI::RHIBarrierDesc& barrier) override;
		void Barrier(const NLS::Render::RHI::RHIBarrierDesc& barrier) override;
		bool IsChildCommandBuffer() const override;
		bool CanExecuteChildCommandBuffers() const override;
		NLS::Render::RHI::RHICommandRecordingResult ExecuteChildCommandBuffer(
			const std::shared_ptr<NLS::Render::RHI::RHICommandBuffer>& childCommandBuffer) override;

#if defined(_WIN32)
		ID3D12GraphicsCommandList* GetCommandList() const;
		static DXGI_FORMAT ToDXGIFormat(NLS::Render::RHI::TextureFormat format);
		static uint32_t GetBytesPerPixel(DXGI_FORMAT format);
		static D3D12_RESOURCE_STATES ToD3D12ResourceState(NLS::Render::RHI::ResourceState state);
#endif

	private:
		void EndPendingGpuProfileScopes();
		void ClearBoundPipelineState();
#if defined(_WIN32)
		bool IsBoundGraphicsPipelineNativeValid() const;
		bool IsBoundComputePipelineNativeValid() const;
		bool FlushBoundDescriptorTables();
		bool FlushBoundDescriptorTablesIfDirty();
		bool HasInitializedRequiredRootDescriptorTables(std::string_view operationName) const;
		bool ValidateDescriptorTableBindings(std::string_view operationName) const;
		void SetDescriptorTableBindingError(std::string message);
		void ClearDescriptorTableBindingError();
		void RemoveBoundBindingSet(uint32_t setIndex);
		void InvalidateParentStateAfterChildExecution();
#endif

		std::string m_debugName;
		bool m_recording = false;
		bool m_isChildCommandBuffer = false;
		bool m_hasClosedRecording = false;
		bool m_childRecordingValid = true;
#if defined(_WIN32)
		ID3D12Device* m_device = nullptr;
		ID3D12CommandQueue* m_queue = nullptr;
		D3D12_COMMAND_LIST_TYPE m_commandListType = D3D12_COMMAND_LIST_TYPE_DIRECT;
		Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_allocator;
		Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
		ID3D12DescriptorHeap* m_currentResourceDescriptorHeap = nullptr;
		ID3D12DescriptorHeap* m_currentSamplerDescriptorHeap = nullptr;
		bool m_descriptorHeapsSet = false;
		bool m_descriptorTablesDirty = false;
		std::string m_descriptorTableBindingError;
		struct PlannedRootDescriptorTable
		{
			UINT rootParameterIndex = 0u;
			D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle{};
			NLS::Render::RHI::DX12::DX12DescriptorHeapKind heapKind =
				NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Resource;
		};
		std::vector<PlannedRootDescriptorTable> m_plannedRootDescriptorTables;
		struct ActiveRenderPassTransition
		{
			ID3D12Resource* resource = nullptr;
			D3D12_RESOURCE_STATES stateAfterBegin = D3D12_RESOURCE_STATE_COMMON;
			D3D12_RESOURCE_STATES stateAfterEnd = D3D12_RESOURCE_STATE_COMMON;
			NLS::Render::RHI::RHITexture* texture = nullptr;
			NLS::Render::RHI::ResourceState textureStateAfterEnd = NLS::Render::RHI::ResourceState::Unknown;
			NLS::Render::RHI::RHISubresourceRange subresourceRange{};
			bool coversWholeTexture = true;
		};
		enum class DebugEventScopeKind : uint8_t
		{
			GpuProfile,
			RenderPass
		};
		struct DebugEventScope
		{
			DebugEventScopeKind kind = DebugEventScopeKind::GpuProfile;
			std::string label;
		};
		std::vector<ActiveRenderPassTransition> m_activeRenderPassTransitions;
		std::unordered_set<const NLS::Render::RHI::RHITexture*> m_partialTextureStateDirty;
		std::vector<DebugEventScope> m_debugEventScopeStack;
#endif
		std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> m_boundPipeline;
		std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> m_boundComputePipeline;
		std::vector<NLS::Render::RHI::DX12::DX12DescriptorTableDesc> m_boundDescriptorTables;
		std::vector<NLS::Render::RHI::DX12::DX12PushConstantRootParameterDesc> m_boundPushConstantRootParameters;
		std::vector<bool> m_initializedRootDescriptorTables;
		std::vector<std::pair<uint32_t, std::shared_ptr<NLS::Render::RHI::RHIBindingSet>>> m_boundBindingSets;
		std::vector<std::shared_ptr<NLS::Render::RHI::RHITextureView>> m_recordedTextureViewKeepAlive;
		std::vector<std::shared_ptr<NLS::Render::RHI::RHIBindingSet>> m_recordedBindingSetKeepAlive;
		std::vector<std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>> m_recordedPipelineKeepAlive;
		std::vector<std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>> m_recordedComputePipelineKeepAlive;
		std::vector<std::shared_ptr<NLS::Render::RHI::RHIBuffer>> m_recordedBufferKeepAlive;
		std::vector<NLS::Base::Profiling::ProfilerGpuScopeEvent> m_gpuProfileScopeStack;
#if defined(_WIN32)
		bool m_bindingComputePipeline = false;
#endif
	};

	class NativeDX12CommandPool final : public NLS::Render::RHI::RHICommandPool
	{
	public:
#if defined(_WIN32)
		NativeDX12CommandPool(
			ID3D12Device* device,
			ID3D12CommandQueue* queue,
			NLS::Render::RHI::QueueType queueType,
			D3D12_COMMAND_LIST_TYPE commandListType,
			const std::string& debugName);
#else
		NativeDX12CommandPool(
			ID3D12Device* device,
			ID3D12CommandQueue* queue,
			NLS::Render::RHI::QueueType queueType,
			int commandListType,
			const std::string& debugName);
#endif

		std::string_view GetDebugName() const override;
		NLS::Render::RHI::QueueType GetQueueType() const override;
		std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> CreateCommandBuffer(std::string debugName) override;
		std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> CreateChildCommandBuffer(std::string debugName) override;
		void Reset() override;

	private:
		ID3D12Device* m_device = nullptr;
		ID3D12CommandQueue* m_queue = nullptr;
		NLS::Render::RHI::QueueType m_queueType = NLS::Render::RHI::QueueType::Graphics;
#if defined(_WIN32)
		D3D12_COMMAND_LIST_TYPE m_commandListType = D3D12_COMMAND_LIST_TYPE_DIRECT;
#else
		int m_commandListType = 0;
#endif
		std::string m_debugName;
	};
}
