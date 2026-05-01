#include "Rendering/RHI/Backends/DX12/DX12Descriptor.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <utility>

#include <Debug/Logger.h>
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
		UINT64 AlignUp(UINT64 value, UINT64 alignment)
		{
			if (alignment == 0)
				return value;

			return (value + (alignment - 1u)) & ~(alignment - 1u);
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
		m_freeDescriptors.push_back({0, m_descriptorCapacity});

		D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = m_heap->GetGPUDescriptorHandleForHeapStart();
		if (m_commandQueue != nullptr)
		{
			Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
			Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> tempCmdList;
			if (SUCCEEDED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
				SUCCEEDED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&tempCmdList))))
			{
				tempCmdList->Close();
				ID3D12CommandList* cmdLists[] = { tempCmdList.Get() };
				m_commandQueue->ExecuteCommandLists(1, cmdLists);

				Microsoft::WRL::ComPtr<ID3D12Fence> fence;
				if (SUCCEEDED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
				{
					HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
					if (fenceEvent != nullptr)
					{
						fence->SetEventOnCompletion(1, fenceEvent);
						m_commandQueue->Signal(fence.Get(), 1);
						WaitForSingleObject(fenceEvent, INFINITE);
						CloseHandle(fenceEvent);
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
		std::lock_guard<std::mutex> lock(m_mutex);
		for (auto it = m_freeDescriptors.begin(); it != m_freeDescriptors.end(); ++it)
		{
			if (it->second >= count)
			{
				const UINT offset = it->first;
				it->first += count;
				it->second -= count;
				if (it->second == 0)
					m_freeDescriptors.erase(it);
				return offset;
			}
		}

		NLS_LOG_ERROR(m_heapDebugName + ": Out of descriptors!");
		return UINT_MAX;
	}

	void DX12ShaderVisibleDescriptorHeapAllocator::Free(UINT offset, UINT count)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_freeDescriptors.push_back({offset, count});
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

	D3D12_FILTER NativeDX12BindingSet::ToD3D12Filter(
		NLS::Render::RHI::TextureFilter minFilter,
		NLS::Render::RHI::TextureFilter magFilter)
	{
		if (minFilter == NLS::Render::RHI::TextureFilter::Nearest &&
			magFilter == NLS::Render::RHI::TextureFilter::Nearest)
		{
			return D3D12_FILTER_MIN_MAG_MIP_POINT;
		}

		if (minFilter == NLS::Render::RHI::TextureFilter::Nearest)
			return D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
		if (magFilter == NLS::Render::RHI::TextureFilter::Nearest)
			return D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT;
		return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	}

	D3D12_TEXTURE_ADDRESS_MODE NativeDX12BindingSet::ToD3D12AddressMode(NLS::Render::RHI::TextureWrap wrap)
	{
		return wrap == NLS::Render::RHI::TextureWrap::ClampToEdge
			? D3D12_TEXTURE_ADDRESS_MODE_CLAMP
			: D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	}

	D3D12_SAMPLER_DESC NativeDX12BindingSet::BuildSamplerDesc(const NLS::Render::RHI::SamplerDesc& desc)
	{
		D3D12_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = ToD3D12Filter(desc.minFilter, desc.magFilter);
		samplerDesc.AddressU = ToD3D12AddressMode(desc.wrapU);
		samplerDesc.AddressV = ToD3D12AddressMode(desc.wrapV);
		samplerDesc.AddressW = ToD3D12AddressMode(desc.wrapW);
		samplerDesc.MinLOD = 0.0f;
		samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		return samplerDesc;
	}

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
		const auto nativeSamplerDesc = BuildSamplerDesc(samplerDesc);
		m_device->CreateSampler(&nativeSamplerDesc, destination);
	}

	void NativeDX12BindingSet::WriteNullStructuredBufferDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE destination) const
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = 1;
		srvDesc.Buffer.StructureByteStride = sizeof(uint32_t);
		srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
		m_device->CreateShaderResourceView(nullptr, &srvDesc, destination);
	}

	void NativeDX12BindingSet::WriteNullStorageBufferDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE destination) const
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.FirstElement = 0;
		uavDesc.Buffer.NumElements = 1;
		uavDesc.Buffer.StructureByteStride = sizeof(uint32_t);
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
					WriteNullStructuredBufferDescriptor(destination);
				else
					WriteNullStorageBufferDescriptor(destination);
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

			const UINT firstElement = static_cast<UINT>(boundEntry->bufferOffset / sizeof(uint32_t));
			const UINT numElements = static_cast<UINT>(bufferSize / sizeof(uint32_t));
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
				srvDesc.Buffer.StructureByteStride = sizeof(uint32_t);
				srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
				m_device->CreateShaderResourceView(resource, &srvDesc, destination);
				return;
			}

			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = DXGI_FORMAT_UNKNOWN;
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
			uavDesc.Buffer.FirstElement = firstElement;
			uavDesc.Buffer.NumElements = numElements;
			uavDesc.Buffer.StructureByteStride = sizeof(uint32_t);
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
				texture != nullptr &&
				viewDesc->format != NLS::Render::RHI::TextureFormat::Depth24Stencil8 &&
				texture->GetDesc().format != NLS::Render::RHI::TextureFormat::Depth24Stencil8)
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
