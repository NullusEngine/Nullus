#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Rendering/RHI/Backends/DX12/DX12Command.h"
#include "Rendering/RHI/Backends/DX12/DX12PipelineLayoutUtils.h"
#include "Rendering/RHI/Core/RHIBinding.h"

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
#endif

	private:
		ID3D12Device* m_device = nullptr;
		ID3D12CommandQueue* m_commandQueue = nullptr;
		std::string m_heapDebugName;
#if defined(_WIN32)
		D3D12_DESCRIPTOR_HEAP_TYPE m_heapType = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		UINT m_descriptorCapacity = 0;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
		UINT m_descriptorSize = 0;
		std::mutex m_mutex;
		std::vector<std::pair<UINT, UINT>> m_freeDescriptors;
#endif
	};

	class NativeDX12BindingSet final
		: public NLS::Render::RHI::RHIBindingSet
		, public IDX12BindingSetAccess
	{
	public:
#if defined(_WIN32)
		explicit NativeDX12BindingSet(
			ID3D12Device* device,
			NLS::Render::RHI::RHIBindingSetDesc desc,
			DX12ShaderVisibleDescriptorHeapAllocator* resourceHeapAllocator,
			DX12ShaderVisibleDescriptorHeapAllocator* samplerHeapAllocator);
#endif
		~NativeDX12BindingSet() override;

		std::string_view GetDebugName() const override;
		const NLS::Render::RHI::RHIBindingSetDesc& GetDesc() const override;
		NLS::Render::RHI::NativeHandle GetNativeBindingSetHandle() const override;

#if defined(_WIN32)
		D3D12_GPU_DESCRIPTOR_HANDLE GetGPUHandle(
			uint32_t set,
			NLS::Render::RHI::DX12::DX12DescriptorHeapKind heapKind) const override;
		ID3D12DescriptorHeap* GetDescriptorHeap(
			NLS::Render::RHI::DX12::DX12DescriptorHeapKind heapKind) const override;
#endif

	private:
#if defined(_WIN32)
		struct DescriptorTableBinding
		{
			uint32_t set = 0;
			NLS::Render::RHI::DX12::DX12DescriptorHeapKind heapKind =
				NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Resource;
			D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = {};
		};

		static D3D12_FILTER ToD3D12Filter(
			NLS::Render::RHI::TextureFilter minFilter,
			NLS::Render::RHI::TextureFilter magFilter);
		static D3D12_TEXTURE_ADDRESS_MODE ToD3D12AddressMode(NLS::Render::RHI::TextureWrap wrap);
		static D3D12_SAMPLER_DESC BuildSamplerDesc(const NLS::Render::RHI::SamplerDesc& desc);
		static NLS::Render::RHI::DX12::DX12DescriptorRangeCategory ToRangeCategory(
			NLS::Render::RHI::BindingType type);

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
		void WriteNullStructuredBufferDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE destination) const;
		void WriteNullStorageBufferDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE destination) const;
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
#endif

		ID3D12Device* m_device = nullptr;
		NLS::Render::RHI::RHIBindingSetDesc m_desc;
		DX12ShaderVisibleDescriptorHeapAllocator* m_resourceHeapAllocator = nullptr;
		DX12ShaderVisibleDescriptorHeapAllocator* m_samplerHeapAllocator = nullptr;
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
