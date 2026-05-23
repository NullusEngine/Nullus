#include "Rendering/RHI/Backends/DX12/DX12Descriptor.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <utility>

#include <Debug/Logger.h>
#include "Rendering/RHI/Backends/DX12/DX12FormatUtils.h"
#include "Rendering/RHI/Backends/DX12/DX12ReadbackUtils.h"
#include "Rendering/RHI/Backends/DX12/DX12SamplerUtils.h"
#include "Rendering/RHI/Backends/DX12/DX12TextureViewUtils.h"
#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/RHI/Core/RHIResource.h"

#if defined(_WIN32)
#include <Windows.h>
#undef CreateSemaphore
#endif

namespace NLS::Render::Backend
{
#if defined(_WIN32)
	namespace
	{
		constexpr uint64_t kDX12DescriptorFenceWaitTimeoutNanoseconds = 5'000'000'000ull;

		UINT64 AlignUp(UINT64 value, UINT64 alignment)
		{
			if (alignment == 0)
				return value;

			return (value + (alignment - 1u)) & ~(alignment - 1u);
		}

		bool WaitForDX12FenceValue(
			ID3D12Fence* fence,
			UINT64 fenceValue,
			HANDLE fenceEvent,
			const std::string& context)
		{
			if (fence == nullptr || fenceEvent == nullptr || fenceValue == 0u)
			{
				NLS_LOG_ERROR(context + ": invalid fence wait request");
				return false;
			}
			if (fence->GetCompletedValue() >= fenceValue)
				return true;

			const HRESULT setEventResult = fence->SetEventOnCompletion(fenceValue, fenceEvent);
			if (FAILED(setEventResult))
			{
				NLS_LOG_ERROR(
					context +
					": failed to set fence completion event hr=" +
					std::to_string(setEventResult) +
					" value=" +
					std::to_string(fenceValue));
				return false;
			}

			const DWORD waitTimeoutMs =
				NLS::Render::RHI::DX12::ConvertDX12WaitTimeoutNanosecondsToMilliseconds(
					kDX12DescriptorFenceWaitTimeoutNanoseconds);
			const DWORD waitResult = WaitForSingleObject(fenceEvent, waitTimeoutMs);
			if (waitResult != WAIT_OBJECT_0)
			{
				NLS_LOG_ERROR(
					context +
					": timed out waiting for fence value=" +
					std::to_string(fenceValue) +
					" completed=" +
					std::to_string(fence->GetCompletedValue()));
				return false;
			}
			return true;
		}
	}

	DX12ShaderVisibleDescriptorHeapAllocator::DX12ShaderVisibleDescriptorHeapAllocator(
		ID3D12Device* device,
		ID3D12CommandQueue* commandQueue,
		D3D12_DESCRIPTOR_HEAP_TYPE heapType,
		UINT descriptorCapacity,
		const char* heapDebugName)
		: m_device(device)
		, m_commandQueue(commandQueue)
		, m_heapDebugName(heapDebugName != nullptr ? heapDebugName : "DX12DescriptorHeap")
		, m_heapType(heapType)
		, m_descriptorCapacity(descriptorCapacity)
	{
		NLS::Render::RHI::DescriptorRangeAllocatorDesc rangeDesc;
		rangeDesc.transientCapacity = m_descriptorCapacity > 0u ? m_descriptorCapacity : 1u;
		rangeDesc.persistentCapacity = m_descriptorCapacity;
		rangeDesc.boundPersistentCapacity = true;
		rangeDesc.debugName = m_heapDebugName;
		m_rangeAllocator.Configure(rangeDesc);

		if (m_device == nullptr)
			return;

		D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
		heapDesc.Type = m_heapType;
		heapDesc.NumDescriptors = m_descriptorCapacity;
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		heapDesc.NodeMask = 0;
		if (FAILED(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_heap))))
		{
			NLS_LOG_ERROR(m_heapDebugName + ": Failed to create descriptor heap");
			return;
		}

		m_descriptorSize = m_device->GetDescriptorHandleIncrementSize(m_heapType);

		D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = m_heap->GetGPUDescriptorHandleForHeapStart();
		if (m_commandQueue != nullptr)
		{
			Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
			Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> tempCmdList;
			if (SUCCEEDED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
				SUCCEEDED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&tempCmdList))))
			{
				const HRESULT closeResult = tempCmdList->Close();
				if (FAILED(closeResult))
				{
					NLS_LOG_ERROR(
						m_heapDebugName +
						": failed to close descriptor heap initialization command list hr=" +
						std::to_string(closeResult));
				}
				else
				{
					ID3D12CommandList* cmdLists[] = { tempCmdList.Get() };
					m_commandQueue->ExecuteCommandLists(1, cmdLists);

					Microsoft::WRL::ComPtr<ID3D12Fence> fence;
					const HRESULT createFenceResult = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
					if (FAILED(createFenceResult))
					{
						NLS_LOG_ERROR(
							m_heapDebugName +
							": failed to create descriptor heap initialization fence hr=" +
							std::to_string(createFenceResult));
					}
					else
					{
						HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
						if (fenceEvent == nullptr)
						{
							NLS_LOG_ERROR(m_heapDebugName + ": failed to create descriptor heap initialization fence event");
						}
						else
						{
							constexpr UINT64 fenceValue = 1u;
							const HRESULT signalResult = m_commandQueue->Signal(fence.Get(), fenceValue);
							if (FAILED(signalResult))
							{
								NLS_LOG_ERROR(
									m_heapDebugName +
									": failed to signal descriptor heap initialization fence hr=" +
									std::to_string(signalResult));
							}
							else
							{
								(void)WaitForDX12FenceValue(
									fence.Get(),
									fenceValue,
									fenceEvent,
									m_heapDebugName + " descriptor heap initialization");
							}
							CloseHandle(fenceEvent);
						}
					}
				}

				gpuHandle = m_heap->GetGPUDescriptorHandleForHeapStart();
			}
		}

		if (gpuHandle.ptr == 0)
		{
			NLS_LOG_ERROR(m_heapDebugName + ": GPU handle is zero after initialization!");
		}
	}

	ID3D12DescriptorHeap* DX12ShaderVisibleDescriptorHeapAllocator::GetHeap() const
	{
		return m_heap.Get();
	}

	UINT DX12ShaderVisibleDescriptorHeapAllocator::GetDescriptorSize() const
	{
		return m_descriptorSize;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE DX12ShaderVisibleDescriptorHeapAllocator::GetCpuHandle() const
	{
		return m_heap != nullptr ? m_heap->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{};
	}

	D3D12_GPU_DESCRIPTOR_HANDLE DX12ShaderVisibleDescriptorHeapAllocator::GetGpuHandle() const
	{
		if (m_heap == nullptr)
		{
			NLS_LOG_ERROR(m_heapDebugName + "::GetGpuHandle: heap is null");
			return {};
		}

		D3D12_GPU_DESCRIPTOR_HANDLE handle = m_heap->GetGPUDescriptorHandleForHeapStart();
		if (handle.ptr == 0)
		{
			NLS_LOG_ERROR(m_heapDebugName + "::GetGpuHandle: GPU handle is zero");
			return {};
		}

		return handle;
	}

	UINT DX12ShaderVisibleDescriptorHeapAllocator::Allocate(UINT count)
	{
		NLS::Render::RHI::DescriptorAllocationRequest request;
		request.count = count;
		request.lifetime = NLS::Render::RHI::DescriptorAllocationLifetime::Persistent;
		request.debugName = m_heapDebugName;
		const auto allocation = m_rangeAllocator.Allocate(request);
		if (allocation.IsValid())
			return static_cast<UINT>(allocation.offset);

		NLS_LOG_ERROR(m_heapDebugName + ": Out of descriptors!");
		return UINT_MAX;
	}

	void DX12ShaderVisibleDescriptorHeapAllocator::Free(UINT offset, UINT count)
	{
		NLS::Render::RHI::DescriptorAllocation allocation;
		allocation.offset = offset;
		allocation.count = count;
		allocation.lifetime = NLS::Render::RHI::DescriptorAllocationLifetime::Persistent;
		allocation.debugName = m_heapDebugName;
		m_rangeAllocator.Release(allocation);
	}

	NLS::Render::RHI::DescriptorAllocatorStats DX12ShaderVisibleDescriptorHeapAllocator::GetStats() const
	{
		return m_rangeAllocator.GetStats();
	}

	NativeDX12BindingSet::NativeDX12BindingSet(
		ID3D12Device* device,
		NLS::Render::RHI::RHIBindingSetDesc desc,
		DX12ShaderVisibleDescriptorHeapAllocator* resourceHeapAllocator,
		DX12ShaderVisibleDescriptorHeapAllocator* samplerHeapAllocator)
		: m_device(device)
		, m_desc(std::move(desc))
		, m_resourceHeapAllocator(resourceHeapAllocator)
		, m_samplerHeapAllocator(samplerHeapAllocator)
	{
		if (m_device == nullptr || m_desc.layout == nullptr)
		{
			NLS_LOG_ERROR("NativeDX12BindingSet: device or layout is null");
			return;
		}

		NLS::Render::RHI::RHIPipelineLayoutDesc pipelineLayoutDesc;
		pipelineLayoutDesc.bindingLayouts.push_back(m_desc.layout);
		const auto tables = NLS::Render::RHI::DX12::BuildDX12DescriptorTableDescs(pipelineLayoutDesc);
		m_descriptorTableDescs = tables;

		m_resourceDescriptorCount = 0;
		m_samplerDescriptorCount = 0;
		for (const auto& table : tables)
		{
			for (const auto& range : table.ranges)
			{
				if (table.heapKind == NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Sampler)
					m_samplerDescriptorCount += range.descriptorCount;
				else
					m_resourceDescriptorCount += range.descriptorCount;
			}
		}

		if (m_resourceDescriptorCount > 0)
		{
			if (m_resourceHeapAllocator == nullptr || m_resourceHeapAllocator->GetHeap() == nullptr)
			{
				NLS_LOG_ERROR("NativeDX12BindingSet: resource descriptor heap allocator is null");
				return;
			}

			m_resourceDescriptorOffset = m_resourceHeapAllocator->Allocate(m_resourceDescriptorCount);
			if (m_resourceDescriptorOffset == UINT_MAX)
			{
				NLS_LOG_ERROR("NativeDX12BindingSet: failed to allocate resource descriptors");
				return;
			}

			m_resourceDescriptorSize = m_resourceHeapAllocator->GetDescriptorSize();
		}

		if (m_samplerDescriptorCount > 0)
		{
			if (m_samplerHeapAllocator == nullptr || m_samplerHeapAllocator->GetHeap() == nullptr)
			{
				NLS_LOG_ERROR("NativeDX12BindingSet: sampler descriptor heap allocator is null");
				return;
			}

			m_samplerDescriptorOffset = m_samplerHeapAllocator->Allocate(m_samplerDescriptorCount);
			if (m_samplerDescriptorOffset == UINT_MAX)
			{
				NLS_LOG_ERROR("NativeDX12BindingSet: failed to allocate sampler descriptors");
				return;
			}

			m_samplerDescriptorSize = m_samplerHeapAllocator->GetDescriptorSize();
		}

		UINT resourceCursor = 0;
		UINT samplerCursor = 0;
		for (const auto& table : tables)
		{
			UINT tableDescriptorCount = 0;
			for (const auto& range : table.ranges)
				tableDescriptorCount += range.descriptorCount;

			if (tableDescriptorCount == 0)
				continue;

			const bool usesSamplerHeap =
				table.heapKind == NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Sampler;
			D3D12_CPU_DESCRIPTOR_HANDLE tableCpuHandle = usesSamplerHeap
				? ComputeSamplerCpuHandle(samplerCursor)
				: ComputeResourceCpuHandle(resourceCursor);
			const D3D12_GPU_DESCRIPTOR_HANDLE tableGpuHandle = usesSamplerHeap
				? ComputeSamplerGpuHandle(samplerCursor)
				: ComputeResourceGpuHandle(resourceCursor);

			m_descriptorTables.push_back({ table.set, table.heapKind, tableGpuHandle });

			D3D12_CPU_DESCRIPTOR_HANDLE destHandle = tableCpuHandle;
			for (const auto& range : table.ranges)
			{
				const auto* layoutEntry = FindLayoutEntry(range);
				const auto* boundEntry = layoutEntry != nullptr ? FindBoundEntry(*layoutEntry) : nullptr;
				for (uint32_t descriptorIndex = 0; descriptorIndex < range.descriptorCount; ++descriptorIndex)
				{
					if (usesSamplerHeap)
						WriteSamplerDescriptor(boundEntry, destHandle);
					else
						WriteResourceDescriptor(layoutEntry, boundEntry, destHandle);

					destHandle.ptr += usesSamplerHeap ? m_samplerDescriptorSize : m_resourceDescriptorSize;
				}
			}

			if (usesSamplerHeap)
				samplerCursor += tableDescriptorCount;
			else
				resourceCursor += tableDescriptorCount;
		}
	}
#endif

	NativeDX12BindingSet::~NativeDX12BindingSet()
	{
#if defined(_WIN32)
		if (m_resourceHeapAllocator != nullptr && m_resourceDescriptorOffset != UINT_MAX && m_resourceDescriptorCount > 0)
			m_resourceHeapAllocator->Free(m_resourceDescriptorOffset, m_resourceDescriptorCount);
		if (m_samplerHeapAllocator != nullptr && m_samplerDescriptorOffset != UINT_MAX && m_samplerDescriptorCount > 0)
			m_samplerHeapAllocator->Free(m_samplerDescriptorOffset, m_samplerDescriptorCount);
#endif
	}

	std::string_view NativeDX12BindingSet::GetDebugName() const
	{
		return m_desc.debugName;
	}

	const NLS::Render::RHI::RHIBindingSetDesc& NativeDX12BindingSet::GetDesc() const
	{
		return m_desc;
	}

	NLS::Render::RHI::NativeHandle NativeDX12BindingSet::GetNativeBindingSetHandle() const
	{
#if defined(_WIN32)
		auto* self = const_cast<NativeDX12BindingSet*>(this);
		return {
			NLS::Render::RHI::BackendType::DX12,
			static_cast<IDX12BindingSetAccess*>(self)
		};
#else
		return {};
#endif
	}

#if defined(_WIN32)
	D3D12_GPU_DESCRIPTOR_HANDLE NativeDX12BindingSet::GetGPUHandle(
		uint32_t set,
		NLS::Render::RHI::DX12::DX12DescriptorHeapKind heapKind) const
	{
		const auto tableIt = std::find_if(
			m_descriptorTables.begin(),
			m_descriptorTables.end(),
			[set, heapKind](const DescriptorTableBinding& table)
			{
				return table.set == set && table.heapKind == heapKind;
			});
		return tableIt != m_descriptorTables.end() ? tableIt->gpuHandle : D3D12_GPU_DESCRIPTOR_HANDLE{};
	}

	ID3D12DescriptorHeap* NativeDX12BindingSet::GetDescriptorHeap(
		NLS::Render::RHI::DX12::DX12DescriptorHeapKind heapKind) const
	{
		return heapKind == NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Sampler
			? (m_samplerHeapAllocator != nullptr ? m_samplerHeapAllocator->GetHeap() : nullptr)
			: (m_resourceHeapAllocator != nullptr ? m_resourceHeapAllocator->GetHeap() : nullptr);
	}
#endif

	bool NativeDX12BindingSet::IsCompatibleWithDescriptorTable(
		const NLS::Render::RHI::DX12::DX12DescriptorTableDesc& table) const
	{
		const auto found = std::find_if(
			m_descriptorTableDescs.begin(),
			m_descriptorTableDescs.end(),
			[&table](const auto& candidate)
			{
				return candidate.set == table.set &&
					candidate.heapKind == table.heapKind;
			});
		return found != m_descriptorTableDescs.end() &&
			NLS::Render::RHI::DX12::AreDX12DescriptorTablesCompatible(table, *found);
	}

#if defined(_WIN32)
	NLS::Render::RHI::DX12::DX12DescriptorRangeCategory NativeDX12BindingSet::ToRangeCategory(
		NLS::Render::RHI::BindingType type)
	{
		switch (type)
		{
		case NLS::Render::RHI::BindingType::UniformBuffer:
			return NLS::Render::RHI::DX12::DX12DescriptorRangeCategory::ConstantBuffer;
		case NLS::Render::RHI::BindingType::StructuredBuffer:
		case NLS::Render::RHI::BindingType::Texture:
			return NLS::Render::RHI::DX12::DX12DescriptorRangeCategory::ShaderResource;
		case NLS::Render::RHI::BindingType::StorageBuffer:
		case NLS::Render::RHI::BindingType::RWTexture:
			return NLS::Render::RHI::DX12::DX12DescriptorRangeCategory::UnorderedAccess;
		case NLS::Render::RHI::BindingType::Sampler:
		default:
			return NLS::Render::RHI::DX12::DX12DescriptorRangeCategory::Sampler;
		}
	}

	const NLS::Render::RHI::RHIBindingLayoutEntry* NativeDX12BindingSet::FindLayoutEntry(
		const NLS::Render::RHI::DX12::DX12DescriptorTableRangeDesc& range) const
	{
		const auto& entries = m_desc.layout->GetDesc().entries;
		const auto entryIt = std::find_if(
			entries.begin(),
			entries.end(),
			[&range](const NLS::Render::RHI::RHIBindingLayoutEntry& entry)
			{
				return entry.binding == range.binding &&
					entry.registerSpace == range.registerSpace &&
					entry.count == range.descriptorCount &&
					ToRangeCategory(entry.type) == range.category;
			});
		return entryIt != entries.end() ? &(*entryIt) : nullptr;
	}

	const NLS::Render::RHI::RHIBindingSetEntry* NativeDX12BindingSet::FindBoundEntry(
		const NLS::Render::RHI::RHIBindingLayoutEntry& layoutEntry) const
	{
		const auto entryIt = std::find_if(
			m_desc.entries.begin(),
			m_desc.entries.end(),
			[&layoutEntry](const NLS::Render::RHI::RHIBindingSetEntry& entry)
			{
				return entry.binding == layoutEntry.binding && entry.type == layoutEntry.type;
			});
		return entryIt != m_desc.entries.end() ? &(*entryIt) : nullptr;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE NativeDX12BindingSet::ComputeResourceCpuHandle(UINT descriptorIndex) const
	{
		D3D12_CPU_DESCRIPTOR_HANDLE handle = m_resourceHeapAllocator->GetCpuHandle();
		handle.ptr += static_cast<SIZE_T>(m_resourceDescriptorOffset + descriptorIndex) * m_resourceDescriptorSize;
		return handle;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE NativeDX12BindingSet::ComputeResourceGpuHandle(UINT descriptorIndex) const
	{
		D3D12_GPU_DESCRIPTOR_HANDLE handle = m_resourceHeapAllocator->GetGpuHandle();
		handle.ptr += static_cast<UINT64>(m_resourceDescriptorOffset + descriptorIndex) * m_resourceDescriptorSize;
		return handle;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE NativeDX12BindingSet::ComputeSamplerCpuHandle(UINT descriptorIndex) const
	{
		D3D12_CPU_DESCRIPTOR_HANDLE handle = m_samplerHeapAllocator->GetCpuHandle();
		handle.ptr += static_cast<SIZE_T>(m_samplerDescriptorOffset + descriptorIndex) * m_samplerDescriptorSize;
		return handle;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE NativeDX12BindingSet::ComputeSamplerGpuHandle(UINT descriptorIndex) const
	{
		D3D12_GPU_DESCRIPTOR_HANDLE handle = m_samplerHeapAllocator->GetGpuHandle();
		handle.ptr += static_cast<UINT64>(m_samplerDescriptorOffset + descriptorIndex) * m_samplerDescriptorSize;
		return handle;
	}

	void NativeDX12BindingSet::WriteSamplerDescriptor(
		const NLS::Render::RHI::RHIBindingSetEntry* boundEntry,
		D3D12_CPU_DESCRIPTOR_HANDLE destination) const
	{
		const auto defaultSamplerDesc = NLS::Render::RHI::SamplerDesc{};
		const auto& samplerDesc = boundEntry != nullptr && boundEntry->sampler != nullptr
			? boundEntry->sampler->GetDesc()
			: defaultSamplerDesc;
		const auto nativeSamplerDesc = NLS::Render::RHI::DX12::BuildDX12SamplerDesc(samplerDesc);
		m_device->CreateSampler(&nativeSamplerDesc, destination);
	}

	void NativeDX12BindingSet::WriteNullStructuredBufferDescriptor(
		D3D12_CPU_DESCRIPTOR_HANDLE destination,
		const uint32_t elementStride) const
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = 1;
		srvDesc.Buffer.StructureByteStride = elementStride != 0u ? elementStride : sizeof(uint32_t);
		srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
		m_device->CreateShaderResourceView(nullptr, &srvDesc, destination);
	}

	void NativeDX12BindingSet::WriteNullStorageBufferDescriptor(
		D3D12_CPU_DESCRIPTOR_HANDLE destination,
		const uint32_t elementStride) const
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.FirstElement = 0;
		uavDesc.Buffer.NumElements = 1;
		uavDesc.Buffer.StructureByteStride = elementStride != 0u ? elementStride : sizeof(uint32_t);
		uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
		m_device->CreateUnorderedAccessView(nullptr, nullptr, &uavDesc, destination);
	}

	void NativeDX12BindingSet::WriteResourceDescriptor(
		const NLS::Render::RHI::RHIBindingLayoutEntry* layoutEntry,
		const NLS::Render::RHI::RHIBindingSetEntry* boundEntry,
		D3D12_CPU_DESCRIPTOR_HANDLE destination) const
	{
		if (layoutEntry == nullptr)
			return;

		switch (layoutEntry->type)
		{
		case NLS::Render::RHI::BindingType::UniformBuffer:
		{
			if (boundEntry == nullptr || boundEntry->buffer == nullptr)
			{
				m_device->CreateConstantBufferView(nullptr, destination);
				return;
			}

			const auto bufferHandle = boundEntry->buffer->GetNativeBufferHandle();
			auto* resource = bufferHandle.backend == NLS::Render::RHI::BackendType::DX12
				? static_cast<ID3D12Resource*>(bufferHandle.handle)
				: nullptr;
			if (resource == nullptr)
			{
				m_device->CreateConstantBufferView(nullptr, destination);
				return;
			}

			const UINT64 bufferSize = boundEntry->bufferRange > 0
				? boundEntry->bufferRange
				: (boundEntry->buffer->GetDesc().size > boundEntry->bufferOffset
					? boundEntry->buffer->GetDesc().size - boundEntry->bufferOffset
					: 0u);
			if (bufferSize == 0)
			{
				m_device->CreateConstantBufferView(nullptr, destination);
				return;
			}

			D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
			cbvDesc.BufferLocation = resource->GetGPUVirtualAddress() + boundEntry->bufferOffset;
			cbvDesc.SizeInBytes = static_cast<UINT>(AlignUp(bufferSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT));
			m_device->CreateConstantBufferView(&cbvDesc, destination);
			return;
		}
		case NLS::Render::RHI::BindingType::StructuredBuffer:
		case NLS::Render::RHI::BindingType::StorageBuffer:
		{
			const auto writeNullDescriptor = [&]()
			{
				if (layoutEntry->type == NLS::Render::RHI::BindingType::StructuredBuffer)
					WriteNullStructuredBufferDescriptor(destination, layoutEntry->elementStride);
				else
					WriteNullStorageBufferDescriptor(destination, layoutEntry->elementStride);
			};

			if (boundEntry == nullptr || boundEntry->buffer == nullptr)
			{
				writeNullDescriptor();
				return;
			}

			const auto bufferHandle = boundEntry->buffer->GetNativeBufferHandle();
			auto* resource = bufferHandle.backend == NLS::Render::RHI::BackendType::DX12
				? static_cast<ID3D12Resource*>(bufferHandle.handle)
				: nullptr;
			if (resource == nullptr)
			{
				writeNullDescriptor();
				return;
			}

			const UINT64 bufferSize = boundEntry->bufferRange > 0
				? boundEntry->bufferRange
				: (boundEntry->buffer->GetDesc().size > boundEntry->bufferOffset
					? boundEntry->buffer->GetDesc().size - boundEntry->bufferOffset
					: 0u);
			if (bufferSize == 0)
			{
				writeNullDescriptor();
				return;
			}

			const auto elementStride =
				boundEntry->elementStride != 0u
					? boundEntry->elementStride
					: layoutEntry->elementStride != 0u
						? layoutEntry->elementStride
						: sizeof(uint32_t);
			if (elementStride == 0u ||
				(boundEntry->bufferOffset % elementStride) != 0u ||
				(bufferSize % elementStride) != 0u)
			{
				NLS_LOG_ERROR(
					"NativeDX12BindingSet: rejected unaligned structured/storage buffer range for binding " +
					std::to_string(layoutEntry->binding) +
					" offset=" + std::to_string(boundEntry->bufferOffset) +
					" range=" + std::to_string(bufferSize) +
					" stride=" + std::to_string(elementStride));
				writeNullDescriptor();
				return;
			}
			const UINT firstElement = static_cast<UINT>(boundEntry->bufferOffset / elementStride);
			const UINT numElements = static_cast<UINT>(bufferSize / elementStride);
			if (numElements == 0u)
			{
				writeNullDescriptor();
				return;
			}

			if (layoutEntry->type == NLS::Render::RHI::BindingType::StructuredBuffer)
			{
				D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
				srvDesc.Format = DXGI_FORMAT_UNKNOWN;
				srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
				srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srvDesc.Buffer.FirstElement = firstElement;
				srvDesc.Buffer.NumElements = numElements;
				srvDesc.Buffer.StructureByteStride = elementStride;
				srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
				m_device->CreateShaderResourceView(resource, &srvDesc, destination);
				return;
			}

			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = DXGI_FORMAT_UNKNOWN;
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
			uavDesc.Buffer.FirstElement = firstElement;
			uavDesc.Buffer.NumElements = numElements;
			uavDesc.Buffer.StructureByteStride = elementStride;
			uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
			m_device->CreateUnorderedAccessView(resource, nullptr, &uavDesc, destination);
			return;
		}
		case NLS::Render::RHI::BindingType::Texture:
		case NLS::Render::RHI::BindingType::RWTexture:
		{
			ID3D12Resource* resource = nullptr;
			const auto* viewDesc = boundEntry != nullptr && boundEntry->textureView != nullptr
				? &boundEntry->textureView->GetDesc()
				: nullptr;
			const auto texture = boundEntry != nullptr && boundEntry->textureView != nullptr
				? boundEntry->textureView->GetTexture()
				: nullptr;

			if (viewDesc != nullptr && texture != nullptr)
			{
				const auto textureHandle = texture->GetNativeImageHandle();
				resource = textureHandle.backend == NLS::Render::RHI::BackendType::DX12
					? static_cast<ID3D12Resource*>(textureHandle.handle)
					: nullptr;
			}

			if (resource != nullptr &&
				viewDesc != nullptr &&
				texture != nullptr)
			{
				if (layoutEntry->type == NLS::Render::RHI::BindingType::RWTexture)
				{
					const auto descriptors = NLS::Render::RHI::DX12::BuildDX12TextureViewDescriptorSet(texture->GetDesc(), *viewDesc);
					D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
					uavDesc.Format = descriptors.hasSrv ? descriptors.srvDesc.Format : DXGI_FORMAT_R8G8B8A8_UNORM;
					uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
					uavDesc.Texture2D.MipSlice = viewDesc->subresourceRange.baseMipLevel;
					m_device->CreateUnorderedAccessView(resource, nullptr, &uavDesc, destination);
					return;
				}

				const auto descriptors = NLS::Render::RHI::DX12::BuildDX12TextureViewDescriptorSet(texture->GetDesc(), *viewDesc);
				if (descriptors.hasSrv)
				{
					m_device->CreateShaderResourceView(resource, &descriptors.srvDesc, destination);
					return;
				}
			}

			if (layoutEntry->type == NLS::Render::RHI::BindingType::RWTexture)
			{
				D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
				uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
				m_device->CreateUnorderedAccessView(nullptr, nullptr, &uavDesc, destination);
				return;
			}

			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			m_device->CreateShaderResourceView(nullptr, &srvDesc, destination);
			return;
		}
		case NLS::Render::RHI::BindingType::Sampler:
		default:
			return;
		}
	}
#endif

	NativeDX12BindingLayout::NativeDX12BindingLayout(NLS::Render::RHI::RHIBindingLayoutDesc desc)
		: m_desc(std::move(desc))
	{
	}

	std::string_view NativeDX12BindingLayout::GetDebugName() const
	{
		return m_desc.debugName;
	}

	const NLS::Render::RHI::RHIBindingLayoutDesc& NativeDX12BindingLayout::GetDesc() const
	{
		return m_desc;
	}
}
