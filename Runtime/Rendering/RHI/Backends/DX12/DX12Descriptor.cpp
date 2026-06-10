#include "Rendering/RHI/Backends/DX12/DX12Descriptor.h"

#include <algorithm>
#include <bit>
#include <climits>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Debug/Logger.h>
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Backends/DX12/DX12FormatUtils.h"
#include "Rendering/RHI/Backends/DX12/DX12QueueSynchronization.h"
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

		template<typename T>
		void AppendKeyValue(std::string& key, const T value)
		{
			static_assert(std::is_trivially_copyable_v<T>);
			const auto* bytes = reinterpret_cast<const char*>(&value);
			key.append(bytes, sizeof(T));
		}

		void AppendNativeSamplerFloatKeyValue(std::string& key, const float value)
		{
			AppendKeyValue<uint32_t>(key, std::bit_cast<uint32_t>(value == 0.0f ? 0.0f : value));
		}

		void AppendNativeSamplerDescKey(std::string& key, const D3D12_SAMPLER_DESC& desc)
		{
			AppendKeyValue<uint32_t>(key, static_cast<uint32_t>(desc.Filter));
			AppendKeyValue<uint32_t>(key, static_cast<uint32_t>(desc.AddressU));
			AppendKeyValue<uint32_t>(key, static_cast<uint32_t>(desc.AddressV));
			AppendKeyValue<uint32_t>(key, static_cast<uint32_t>(desc.AddressW));
			AppendNativeSamplerFloatKeyValue(key, desc.MipLODBias);
			AppendKeyValue<uint32_t>(key, desc.MaxAnisotropy);
			AppendKeyValue<uint32_t>(key, static_cast<uint32_t>(desc.ComparisonFunc));
			for (const auto value : desc.BorderColor)
				AppendNativeSamplerFloatKeyValue(key, value);
			AppendNativeSamplerFloatKeyValue(key, desc.MinLOD);
			AppendNativeSamplerFloatKeyValue(key, desc.MaxLOD);
		}

		NLS::Render::RHI::DX12::DX12DescriptorRangeCategory ToDX12DescriptorRangeCategory(
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

		const NLS::Render::RHI::RHIBindingLayoutEntry* FindDX12BindingLayoutEntry(
			const NLS::Render::RHI::RHIBindingSetDesc& desc,
			const NLS::Render::RHI::DX12::DX12DescriptorTableRangeDesc& range)
		{
			if (desc.layout == nullptr)
				return nullptr;

			const auto& entries = desc.layout->GetDesc().entries;
			const auto entryIt = std::find_if(
				entries.begin(),
				entries.end(),
				[&range](const NLS::Render::RHI::RHIBindingLayoutEntry& entry)
				{
					return entry.binding == range.binding &&
						entry.registerSpace == range.registerSpace &&
						entry.count == range.descriptorCount &&
						ToDX12DescriptorRangeCategory(entry.type) == range.category;
				});
			return entryIt != entries.end() ? &(*entryIt) : nullptr;
		}

		const NLS::Render::RHI::RHIBindingSetEntry* FindDX12BindingSetEntry(
			const NLS::Render::RHI::RHIBindingSetDesc& desc,
			const NLS::Render::RHI::RHIBindingLayoutEntry& layoutEntry)
		{
			const auto entryIt = std::find_if(
				desc.entries.begin(),
				desc.entries.end(),
				[&layoutEntry](const NLS::Render::RHI::RHIBindingSetEntry& entry)
				{
					return entry.binding == layoutEntry.binding && entry.type == layoutEntry.type;
				});
			return entryIt != desc.entries.end() ? &(*entryIt) : nullptr;
		}

		std::string BuildDX12SamplerDescriptorTableKey(
			const NLS::Render::RHI::RHIBindingSetDesc& desc,
			const std::vector<NLS::Render::RHI::DX12::DX12DescriptorTableDesc>& tables)
		{
			std::string key;
			key.reserve(256u);
			const auto samplerTableCount = static_cast<uint32_t>(std::count_if(
				tables.begin(),
				tables.end(),
				[](const NLS::Render::RHI::DX12::DX12DescriptorTableDesc& table)
				{
					return table.heapKind == NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Sampler;
				}));
			AppendKeyValue<uint32_t>(key, samplerTableCount);

			const NLS::Render::RHI::SamplerDesc defaultSamplerDesc{};
			for (const auto& table : tables)
			{
				if (table.heapKind != NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Sampler)
					continue;

				AppendKeyValue<uint32_t>(key, static_cast<uint32_t>(table.heapKind));
				AppendKeyValue<uint32_t>(key, static_cast<uint32_t>(table.category));
				AppendKeyValue<uint32_t>(key, table.set);
				AppendKeyValue<uint32_t>(key, table.registerSpace);
				AppendKeyValue<uint32_t>(key, static_cast<uint32_t>(table.ranges.size()));

				for (const auto& range : table.ranges)
				{
					AppendKeyValue<uint32_t>(key, static_cast<uint32_t>(range.category));
					AppendKeyValue<uint32_t>(key, range.registerSpace);
					AppendKeyValue<uint32_t>(key, range.binding);
					AppendKeyValue<uint32_t>(key, range.descriptorCount);
					AppendKeyValue<uint32_t>(key, range.elementStride);

					const auto* layoutEntry = FindDX12BindingLayoutEntry(desc, range);
					const auto* boundEntry = layoutEntry != nullptr
						? FindDX12BindingSetEntry(desc, *layoutEntry)
						: nullptr;
					const auto& samplerDesc = boundEntry != nullptr && boundEntry->sampler != nullptr
						? boundEntry->sampler->GetDesc()
						: defaultSamplerDesc;
					const auto nativeSamplerDesc = NLS::Render::RHI::DX12::BuildDX12SamplerDesc(samplerDesc);
					for (uint32_t descriptorIndex = 0u; descriptorIndex < range.descriptorCount; ++descriptorIndex)
						AppendNativeSamplerDescKey(key, nativeSamplerDesc);
				}
			}

			return key;
		}

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

		struct DX12DescriptorInitializationQuarantinedSubmission
		{
			Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
			Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
			Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
			Microsoft::WRL::ComPtr<ID3D12Fence> fence;
			HRESULT signalResult = S_OK;
			HRESULT deviceStatus = S_OK;
		};

		std::mutex& DX12DescriptorInitializationQuarantineMutex()
		{
			static std::mutex mutex;
			return mutex;
		}

		std::vector<DX12DescriptorInitializationQuarantinedSubmission>& DX12DescriptorInitializationQuarantine()
		{
			static std::vector<DX12DescriptorInitializationQuarantinedSubmission> submissions;
			return submissions;
		}

		void QuarantineDX12DescriptorInitializationSubmissionAfterExecute(
			ID3D12Device* device,
			const Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>& heap,
			const Microsoft::WRL::ComPtr<ID3D12CommandAllocator>& allocator,
			const Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>& commandList,
			const Microsoft::WRL::ComPtr<ID3D12Fence>& fence,
			HRESULT signalResult)
		{
			DX12DescriptorInitializationQuarantinedSubmission submission;
			submission.heap = heap;
			submission.allocator = allocator;
			submission.commandList = commandList;
			submission.fence = fence;
			submission.signalResult = signalResult;
			submission.deviceStatus = device != nullptr ? device->GetDeviceRemovedReason() : S_OK;
			const HRESULT deviceStatus = submission.deviceStatus;

			std::lock_guard<std::mutex> lock(DX12DescriptorInitializationQuarantineMutex());
			DX12DescriptorInitializationQuarantine().push_back(std::move(submission));
			NLS::Render::Context::MarkLocatedDriverUnsafeGpuWorkQuarantined(
				"DX12 descriptor heap initialization queued GPU work without a reliable fence; quarantined submitted resources hr=" +
				std::to_string(signalResult));
			if (FAILED(deviceStatus))
			{
				NLS::Render::Context::MarkLocatedDriverDeviceLost(
					"DX12 descriptor heap initialization detected device lost after ExecuteCommandLists; quarantined submitted resources hr=" +
					std::to_string(deviceStatus));
			}
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
							HRESULT signalResult = S_OK;
							{
								NLS::Render::RHI::DX12::ScopedDX12QueueLock queueLock(m_commandQueue);
								ID3D12CommandList* cmdLists[] = { tempCmdList.Get() };
								m_commandQueue->ExecuteCommandLists(1, cmdLists);
								signalResult = m_commandQueue->Signal(fence.Get(), fenceValue);
							}
							if (FAILED(signalResult))
							{
								m_quarantined = true;
								QuarantineDX12DescriptorInitializationSubmissionAfterExecute(
									m_device,
									m_heap,
									allocator,
									tempCmdList,
									fence,
									signalResult);
								const HRESULT deviceStatus = m_device->GetDeviceRemovedReason();
								NLS_LOG_ERROR(
									m_heapDebugName +
									": failed to signal descriptor heap initialization fence after ExecuteCommandLists; quarantined submitted resources hr=" +
									std::to_string(signalResult) +
									" deviceStatus=" +
									std::to_string(deviceStatus));
							}
							else
							{
								if (!WaitForDX12FenceValue(
									fence.Get(),
									fenceValue,
									fenceEvent,
									m_heapDebugName + " descriptor heap initialization"))
								{
									m_quarantined = true;
									QuarantineDX12DescriptorInitializationSubmissionAfterExecute(
										m_device,
										m_heap,
										allocator,
										tempCmdList,
										fence,
										signalResult);
								}
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
		if (m_quarantined)
			return nullptr;
		return m_heap.Get();
	}

	UINT DX12ShaderVisibleDescriptorHeapAllocator::GetDescriptorSize() const
	{
		return m_descriptorSize;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE DX12ShaderVisibleDescriptorHeapAllocator::GetCpuHandle() const
	{
		if (m_quarantined)
		{
			NLS_LOG_ERROR(m_heapDebugName + "::GetCpuHandle: descriptor heap allocator is quarantined");
			return {};
		}
		return m_heap != nullptr ? m_heap->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{};
	}

	D3D12_GPU_DESCRIPTOR_HANDLE DX12ShaderVisibleDescriptorHeapAllocator::GetGpuHandle() const
	{
		if (m_quarantined)
		{
			NLS_LOG_ERROR(m_heapDebugName + "::GetGpuHandle: descriptor heap allocator is quarantined");
			return {};
		}
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
		if (m_quarantined)
		{
			NLS_LOG_ERROR(m_heapDebugName + ": descriptor heap allocator is quarantined");
			return UINT_MAX;
		}
		NLS::Render::RHI::DescriptorAllocationRequest request;
		request.count = count;
		request.lifetime = NLS::Render::RHI::DescriptorAllocationLifetime::Persistent;
		request.debugName = m_heapDebugName;
		const auto allocation = m_rangeAllocator.Allocate(request);
		if (allocation.IsValid())
			return static_cast<UINT>(allocation.offset);

		const auto stats = m_rangeAllocator.GetStats();
		NLS_LOG_ERROR(
			m_heapDebugName +
			": Out of descriptors! requested=" + std::to_string(count) +
			" persistentUsed=" + std::to_string(stats.persistentUsed) +
			" persistentPeak=" + std::to_string(stats.persistentPeak) +
			" persistentCapacity=" + std::to_string(stats.persistentCapacity) +
			" released=" + std::to_string(stats.persistentReleased) +
			" failures=" + std::to_string(stats.allocationFailures));
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

	struct DX12SamplerDescriptorTableCache::Allocation::Impl
	{
		struct Entry
		{
			UINT offset = UINT_MAX;
			UINT count = 0u;
			uint32_t refCount = 0u;
		};

		std::mutex mutex;
		std::shared_ptr<DX12ShaderVisibleDescriptorHeapAllocator> allocator;
		std::unordered_map<std::string, Entry> entries;
	};

	struct DX12SamplerDescriptorTableCache::Impl final
		: public DX12SamplerDescriptorTableCache::Allocation::Impl
	{
	};

	DX12SamplerDescriptorTableCache::Allocation::Allocation(
		std::shared_ptr<Impl> impl,
		std::string key,
		UINT offset,
		UINT count)
		: m_impl(std::move(impl))
		, m_key(std::move(key))
		, m_offset(offset)
		, m_count(count)
	{
	}

	DX12SamplerDescriptorTableCache::Allocation::~Allocation()
	{
		if (m_impl == nullptr || m_offset == UINT_MAX || m_count == 0u)
			return;

		std::lock_guard<std::mutex> lock(m_impl->mutex);
		auto entryIt = m_impl->entries.find(m_key);
		if (entryIt == m_impl->entries.end())
			return;

		auto& entry = entryIt->second;
		if (entry.refCount > 1u)
		{
			--entry.refCount;
			return;
		}

		if (m_impl->allocator != nullptr)
			m_impl->allocator->Free(entry.offset, entry.count);
		m_impl->entries.erase(entryIt);
	}

	UINT DX12SamplerDescriptorTableCache::Allocation::GetOffset() const
	{
		return m_offset;
	}

	UINT DX12SamplerDescriptorTableCache::Allocation::GetCount() const
	{
		return m_count;
	}

	DX12SamplerDescriptorTableCache::DX12SamplerDescriptorTableCache(
		std::shared_ptr<DX12ShaderVisibleDescriptorHeapAllocator> allocator)
		: m_impl(std::make_shared<Impl>())
	{
		m_impl->allocator = std::move(allocator);
	}

	DX12SamplerDescriptorTableCache::~DX12SamplerDescriptorTableCache() = default;

	std::shared_ptr<DX12SamplerDescriptorTableCache::Allocation> DX12SamplerDescriptorTableCache::Acquire(
		const NLS::Render::RHI::RHIBindingSetDesc& desc,
		const std::vector<NLS::Render::RHI::DX12::DX12DescriptorTableDesc>& tables,
		UINT descriptorCount,
		const std::function<void(D3D12_CPU_DESCRIPTOR_HANDLE, UINT)>& writeDescriptors)
	{
		if (m_impl == nullptr || m_impl->allocator == nullptr || descriptorCount == 0u)
			return nullptr;
		if (m_impl->allocator->GetHeap() == nullptr)
			return nullptr;

		auto key = BuildDX12SamplerDescriptorTableKey(desc, tables);
		std::lock_guard<std::mutex> lock(m_impl->mutex);
		if (auto found = m_impl->entries.find(key); found != m_impl->entries.end())
		{
			++found->second.refCount;
			return std::shared_ptr<Allocation>(
				new Allocation(m_impl, std::move(key), found->second.offset, found->second.count));
		}

		const UINT offset = m_impl->allocator->Allocate(descriptorCount);
		if (offset == UINT_MAX)
			return nullptr;

		D3D12_CPU_DESCRIPTOR_HANDLE baseCpuHandle = m_impl->allocator->GetCpuHandle();
		const UINT descriptorSize = m_impl->allocator->GetDescriptorSize();
		baseCpuHandle.ptr += static_cast<SIZE_T>(offset) * descriptorSize;
		writeDescriptors(baseCpuHandle, descriptorSize);

		m_impl->entries.emplace(key, Allocation::Impl::Entry{ offset, descriptorCount, 1u });
		return std::shared_ptr<Allocation>(
			new Allocation(m_impl, std::move(key), offset, descriptorCount));
	}

	UINT DX12SamplerDescriptorTableCache::GetDescriptorSize() const
	{
		return m_impl != nullptr && m_impl->allocator != nullptr
			? m_impl->allocator->GetDescriptorSize()
			: 0u;
	}

	ID3D12DescriptorHeap* DX12SamplerDescriptorTableCache::GetHeap() const
	{
		return m_impl != nullptr && m_impl->allocator != nullptr
			? m_impl->allocator->GetHeap()
			: nullptr;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE DX12SamplerDescriptorTableCache::GetGpuHandle(
		UINT offset,
		UINT descriptorIndex) const
	{
		if (m_impl == nullptr || m_impl->allocator == nullptr)
			return {};

		D3D12_GPU_DESCRIPTOR_HANDLE handle = m_impl->allocator->GetGpuHandle();
		if (handle.ptr == 0)
			return {};

		handle.ptr += static_cast<UINT64>(offset + descriptorIndex) * m_impl->allocator->GetDescriptorSize();
		return handle;
	}

	NativeDX12BindingSet::NativeDX12BindingSet(
		ID3D12Device* device,
		NLS::Render::RHI::RHIBindingSetDesc desc,
		std::shared_ptr<DX12ShaderVisibleDescriptorHeapAllocator> resourceHeapAllocator,
		std::shared_ptr<DX12ShaderVisibleDescriptorHeapAllocator> samplerHeapAllocator,
		std::shared_ptr<DX12SamplerDescriptorTableCache> samplerDescriptorTableCache)
		: m_device(device)
		, m_desc(std::move(desc))
		, m_resourceHeapAllocator(std::move(resourceHeapAllocator))
		, m_samplerHeapAllocator(std::move(samplerHeapAllocator))
		, m_samplerDescriptorTableCache(std::move(samplerDescriptorTableCache))
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
			if (m_samplerDescriptorTableCache != nullptr && m_samplerDescriptorTableCache->GetHeap() != nullptr)
			{
				m_samplerDescriptorTableAllocation = m_samplerDescriptorTableCache->Acquire(
					m_desc,
					tables,
					m_samplerDescriptorCount,
					[this, &tables](D3D12_CPU_DESCRIPTOR_HANDLE baseCpuHandle, UINT descriptorSize)
					{
						WriteSamplerDescriptorTables(tables, baseCpuHandle, descriptorSize);
					});
				if (m_samplerDescriptorTableAllocation == nullptr)
				{
					NLS_LOG_ERROR("NativeDX12BindingSet: failed to allocate sampler descriptors");
					return;
				}

				m_samplerDescriptorOffset = m_samplerDescriptorTableAllocation->GetOffset();
				m_samplerDescriptorSize = m_samplerDescriptorTableCache->GetDescriptorSize();
			}
			else if (m_samplerHeapAllocator == nullptr || m_samplerHeapAllocator->GetHeap() == nullptr)
			{
				NLS_LOG_ERROR("NativeDX12BindingSet: sampler descriptor heap allocator is null");
				return;
			}
			else
			{
				m_samplerDescriptorOffset = m_samplerHeapAllocator->Allocate(m_samplerDescriptorCount);
				if (m_samplerDescriptorOffset == UINT_MAX)
				{
					NLS_LOG_ERROR("NativeDX12BindingSet: failed to allocate sampler descriptors");
					return;
				}

				m_samplerDescriptorSize = m_samplerHeapAllocator->GetDescriptorSize();
			}
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
			const D3D12_GPU_DESCRIPTOR_HANDLE tableGpuHandle = usesSamplerHeap
				? ComputeSamplerGpuHandle(samplerCursor)
				: ComputeResourceGpuHandle(resourceCursor);
			if (tableGpuHandle.ptr == 0)
			{
				NLS_LOG_ERROR("NativeDX12BindingSet: descriptor table GPU handle is zero");
				return;
			}

			m_descriptorTables.push_back({ table.set, table.heapKind, tableGpuHandle });

			if (usesSamplerHeap && m_samplerDescriptorTableAllocation != nullptr)
			{
				samplerCursor += tableDescriptorCount;
				continue;
			}

			D3D12_CPU_DESCRIPTOR_HANDLE tableCpuHandle = usesSamplerHeap
				? ComputeSamplerCpuHandle(samplerCursor)
				: ComputeResourceCpuHandle(resourceCursor);
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

		m_valid = true;
	}
#endif

	NativeDX12BindingSet::~NativeDX12BindingSet()
	{
#if defined(_WIN32)
		if (m_resourceHeapAllocator != nullptr && m_resourceDescriptorOffset != UINT_MAX && m_resourceDescriptorCount > 0)
			m_resourceHeapAllocator->Free(m_resourceDescriptorOffset, m_resourceDescriptorCount);
		if (m_samplerDescriptorTableAllocation == nullptr &&
			m_samplerHeapAllocator != nullptr &&
			m_samplerDescriptorOffset != UINT_MAX &&
			m_samplerDescriptorCount > 0)
		{
			m_samplerHeapAllocator->Free(m_samplerDescriptorOffset, m_samplerDescriptorCount);
		}
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
		if (!m_valid)
			return {};

		auto* self = const_cast<NativeDX12BindingSet*>(this);
		return {
			NLS::Render::RHI::BackendType::DX12,
			static_cast<IDX12BindingSetAccess*>(self)
		};
#else
		return {};
#endif
	}

	NLS::Render::RHI::NativeHandle NativeDX12BindingSet::GetNativeDescriptorHeapCompatibilityHandle(
		const uint32_t heapClass) const
	{
#if defined(_WIN32)
		if (!m_valid)
			return {};

		const auto heapKind = heapClass == 1u
			? NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Sampler
			: NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Resource;
		return {
			NLS::Render::RHI::BackendType::DX12,
			GetDescriptorHeap(heapKind)
		};
#else
		(void)heapClass;
		return {};
#endif
	}

	bool NativeDX12BindingSet::IsValid() const
	{
		return m_valid;
	}

#if defined(_WIN32)
	D3D12_GPU_DESCRIPTOR_HANDLE NativeDX12BindingSet::GetGPUHandle(
		uint32_t set,
		NLS::Render::RHI::DX12::DX12DescriptorHeapKind heapKind) const
	{
		if (!m_valid)
			return {};

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
		if (!m_valid)
			return nullptr;

		if (heapKind == NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Sampler)
		{
			if (m_samplerDescriptorTableCache != nullptr && m_samplerDescriptorTableAllocation != nullptr)
				return m_samplerDescriptorTableCache->GetHeap();
			return m_samplerHeapAllocator != nullptr ? m_samplerHeapAllocator->GetHeap() : nullptr;
		}

		return m_resourceHeapAllocator != nullptr ? m_resourceHeapAllocator->GetHeap() : nullptr;
	}
#endif

	bool NativeDX12BindingSet::IsCompatibleWithDescriptorTable(
		const NLS::Render::RHI::DX12::DX12DescriptorTableDesc& table) const
	{
		if (!m_valid)
			return false;

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
	const NLS::Render::RHI::RHIBindingLayoutEntry* NativeDX12BindingSet::FindLayoutEntry(
		const NLS::Render::RHI::DX12::DX12DescriptorTableRangeDesc& range) const
	{
		return FindDX12BindingLayoutEntry(m_desc, range);
	}

	const NLS::Render::RHI::RHIBindingSetEntry* NativeDX12BindingSet::FindBoundEntry(
		const NLS::Render::RHI::RHIBindingLayoutEntry& layoutEntry) const
	{
		return FindDX12BindingSetEntry(m_desc, layoutEntry);
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
		if (handle.ptr == 0)
			return {};
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
		if (m_samplerDescriptorTableCache != nullptr && m_samplerDescriptorTableAllocation != nullptr)
			return m_samplerDescriptorTableCache->GetGpuHandle(m_samplerDescriptorOffset, descriptorIndex);

		D3D12_GPU_DESCRIPTOR_HANDLE handle = m_samplerHeapAllocator->GetGpuHandle();
		if (handle.ptr == 0)
			return {};
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

	void NativeDX12BindingSet::WriteSamplerDescriptorTables(
		const std::vector<NLS::Render::RHI::DX12::DX12DescriptorTableDesc>& tables,
		D3D12_CPU_DESCRIPTOR_HANDLE baseCpuHandle,
		UINT descriptorSize) const
	{
		UINT samplerCursor = 0u;
		for (const auto& table : tables)
		{
			UINT tableDescriptorCount = 0u;
			for (const auto& range : table.ranges)
				tableDescriptorCount += range.descriptorCount;

			if (table.heapKind != NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Sampler)
				continue;

			D3D12_CPU_DESCRIPTOR_HANDLE destHandle = baseCpuHandle;
			destHandle.ptr += static_cast<SIZE_T>(samplerCursor) * descriptorSize;
			for (const auto& range : table.ranges)
			{
				const auto* layoutEntry = FindLayoutEntry(range);
				const auto* boundEntry = layoutEntry != nullptr ? FindBoundEntry(*layoutEntry) : nullptr;
				for (uint32_t descriptorIndex = 0u; descriptorIndex < range.descriptorCount; ++descriptorIndex)
				{
					WriteSamplerDescriptor(boundEntry, destHandle);
					destHandle.ptr += descriptorSize;
				}
			}

			samplerCursor += tableDescriptorCount;
		}
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
