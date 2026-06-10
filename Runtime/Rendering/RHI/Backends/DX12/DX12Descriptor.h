#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Rendering/RHI/Backends/DX12/DX12Access.h"
#include "Rendering/RHI/Backends/DX12/DX12PipelineLayoutUtils.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Utils/DescriptorAllocator/DescriptorAllocator.h"

struct ID3D12CommandQueue;
struct ID3D12DescriptorHeap;
struct ID3D12Device;

#if defined(_WIN32)
#include <d3d12.h>
#include <wrl/client.h>
#undef CreateSemaphore
#endif

namespace NLS::Render::Backend
{
	class DX12SamplerDescriptorTableCache;

	class DX12ShaderVisibleDescriptorHeapAllocator
	{
	public:
#if defined(_WIN32)
		DX12ShaderVisibleDescriptorHeapAllocator(
			ID3D12Device* device,
			ID3D12CommandQueue* commandQueue,
			D3D12_DESCRIPTOR_HEAP_TYPE heapType,
			UINT descriptorCapacity,
			const char* heapDebugName);

		ID3D12DescriptorHeap* GetHeap() const;
		UINT GetDescriptorSize() const;
		D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle() const;
		D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle() const;
		UINT Allocate(UINT count = 1);
		void Free(UINT offset, UINT count = 1);
		NLS::Render::RHI::DescriptorAllocatorStats GetStats() const;
#endif

	private:
		ID3D12Device* m_device = nullptr;
		ID3D12CommandQueue* m_commandQueue = nullptr;
		std::string m_heapDebugName;
		NLS::Render::RHI::DescriptorRangeAllocator m_rangeAllocator;
#if defined(_WIN32)
		D3D12_DESCRIPTOR_HEAP_TYPE m_heapType = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		UINT m_descriptorCapacity = 0;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
		UINT m_descriptorSize = 0;
		bool m_quarantined = false;
#endif
	};

#if defined(_WIN32)
	class DX12SamplerDescriptorTableCache
	{
	public:
		class Allocation
		{
		public:
			~Allocation();
			Allocation(const Allocation&) = delete;
			Allocation& operator=(const Allocation&) = delete;
			Allocation(Allocation&&) = delete;
			Allocation& operator=(Allocation&&) = delete;

			UINT GetOffset() const;
			UINT GetCount() const;

		private:
			friend class DX12SamplerDescriptorTableCache;
			struct Impl;

			Allocation(std::shared_ptr<Impl> impl, std::string key, UINT offset, UINT count);

			std::shared_ptr<Impl> m_impl;
			std::string m_key;
			UINT m_offset = UINT_MAX;
			UINT m_count = 0u;
		};

		explicit DX12SamplerDescriptorTableCache(
			std::shared_ptr<DX12ShaderVisibleDescriptorHeapAllocator> allocator);
		~DX12SamplerDescriptorTableCache();

		std::shared_ptr<Allocation> Acquire(
			const NLS::Render::RHI::RHIBindingSetDesc& desc,
			const std::vector<NLS::Render::RHI::DX12::DX12DescriptorTableDesc>& tables,
			UINT descriptorCount,
			const std::function<void(D3D12_CPU_DESCRIPTOR_HANDLE, UINT)>& writeDescriptors);
		UINT GetDescriptorSize() const;
		ID3D12DescriptorHeap* GetHeap() const;
		D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle(UINT offset, UINT descriptorIndex) const;

	private:
		struct Impl;
		std::shared_ptr<Impl> m_impl;
	};
#endif

	class NativeDX12BindingSet final
		: public NLS::Render::RHI::RHIBindingSet
		, public IDX12BindingSetAccess
	{
	public:
#if defined(_WIN32)
		explicit NativeDX12BindingSet(
			ID3D12Device* device,
			NLS::Render::RHI::RHIBindingSetDesc desc,
			std::shared_ptr<DX12ShaderVisibleDescriptorHeapAllocator> resourceHeapAllocator,
			std::shared_ptr<DX12ShaderVisibleDescriptorHeapAllocator> samplerHeapAllocator,
			std::shared_ptr<DX12SamplerDescriptorTableCache> samplerDescriptorTableCache);
#endif
		~NativeDX12BindingSet() override;

		std::string_view GetDebugName() const override;
		const NLS::Render::RHI::RHIBindingSetDesc& GetDesc() const override;
		NLS::Render::RHI::NativeHandle GetNativeBindingSetHandle() const override;
		NLS::Render::RHI::NativeHandle GetNativeDescriptorHeapCompatibilityHandle(uint32_t heapClass) const override;
		bool IsValid() const;

#if defined(_WIN32)
		D3D12_GPU_DESCRIPTOR_HANDLE GetGPUHandle(
			uint32_t set,
			NLS::Render::RHI::DX12::DX12DescriptorHeapKind heapKind) const override;
		ID3D12DescriptorHeap* GetDescriptorHeap(
			NLS::Render::RHI::DX12::DX12DescriptorHeapKind heapKind) const override;
#endif
		bool IsCompatibleWithDescriptorTable(
			const NLS::Render::RHI::DX12::DX12DescriptorTableDesc& table) const override;

	private:
#if defined(_WIN32)
		struct DescriptorTableBinding
		{
			uint32_t set = 0;
			NLS::Render::RHI::DX12::DX12DescriptorHeapKind heapKind =
				NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Resource;
			D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = {};
		};

		const NLS::Render::RHI::RHIBindingLayoutEntry* FindLayoutEntry(
			const NLS::Render::RHI::DX12::DX12DescriptorTableRangeDesc& range) const;
		const NLS::Render::RHI::RHIBindingSetEntry* FindBoundEntry(
			const NLS::Render::RHI::RHIBindingLayoutEntry& layoutEntry) const;
		D3D12_CPU_DESCRIPTOR_HANDLE ComputeResourceCpuHandle(UINT descriptorIndex) const;
		D3D12_GPU_DESCRIPTOR_HANDLE ComputeResourceGpuHandle(UINT descriptorIndex) const;
		D3D12_CPU_DESCRIPTOR_HANDLE ComputeSamplerCpuHandle(UINT descriptorIndex) const;
		D3D12_GPU_DESCRIPTOR_HANDLE ComputeSamplerGpuHandle(UINT descriptorIndex) const;
		void WriteSamplerDescriptor(
			const NLS::Render::RHI::RHIBindingSetEntry* boundEntry,
			D3D12_CPU_DESCRIPTOR_HANDLE destination) const;
		void WriteSamplerDescriptorTables(
			const std::vector<NLS::Render::RHI::DX12::DX12DescriptorTableDesc>& tables,
			D3D12_CPU_DESCRIPTOR_HANDLE baseCpuHandle,
			UINT descriptorSize) const;
		void WriteNullStructuredBufferDescriptor(
			D3D12_CPU_DESCRIPTOR_HANDLE destination,
			uint32_t elementStride) const;
		void WriteNullStorageBufferDescriptor(
			D3D12_CPU_DESCRIPTOR_HANDLE destination,
			uint32_t elementStride) const;
		void WriteResourceDescriptor(
			const NLS::Render::RHI::RHIBindingLayoutEntry* layoutEntry,
			const NLS::Render::RHI::RHIBindingSetEntry* boundEntry,
			D3D12_CPU_DESCRIPTOR_HANDLE destination) const;

		std::vector<DescriptorTableBinding> m_descriptorTables;
		UINT m_resourceDescriptorOffset = UINT_MAX;
		UINT m_resourceDescriptorCount = 0;
		UINT m_resourceDescriptorSize = 0;
		UINT m_samplerDescriptorOffset = UINT_MAX;
		UINT m_samplerDescriptorCount = 0;
		UINT m_samplerDescriptorSize = 0;
		std::shared_ptr<DX12SamplerDescriptorTableCache::Allocation> m_samplerDescriptorTableAllocation;
#endif

		std::vector<NLS::Render::RHI::DX12::DX12DescriptorTableDesc> m_descriptorTableDescs;
		ID3D12Device* m_device = nullptr;
		NLS::Render::RHI::RHIBindingSetDesc m_desc;
		std::shared_ptr<DX12ShaderVisibleDescriptorHeapAllocator> m_resourceHeapAllocator;
		std::shared_ptr<DX12ShaderVisibleDescriptorHeapAllocator> m_samplerHeapAllocator;
		std::shared_ptr<DX12SamplerDescriptorTableCache> m_samplerDescriptorTableCache;
		bool m_valid = false;
	};

	class NativeDX12BindingLayout final : public NLS::Render::RHI::RHIBindingLayout
	{
	public:
		explicit NativeDX12BindingLayout(NLS::Render::RHI::RHIBindingLayoutDesc desc);

		std::string_view GetDebugName() const override;
		const NLS::Render::RHI::RHIBindingLayoutDesc& GetDesc() const override;

	private:
		NLS::Render::RHI::RHIBindingLayoutDesc m_desc;
	};
}
