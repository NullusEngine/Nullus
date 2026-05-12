#pragma once

#include <cstdint>
#include <vector>

#include "Rendering/RHI/Backends/DX12/DX12PipelineLayoutUtils.h"

struct ID3D12DescriptorHeap;
struct ID3D12PipelineState;
struct ID3D12RootSignature;

#if defined(_WIN32)
#include <d3d12.h>
#undef CreateSemaphore
#endif

namespace NLS::Render::Backend
{
	class IDX12BindingSetAccess
	{
	public:
		virtual ~IDX12BindingSetAccess() = default;
#if defined(_WIN32)
		virtual D3D12_GPU_DESCRIPTOR_HANDLE GetGPUHandle(
			uint32_t set,
			NLS::Render::RHI::DX12::DX12DescriptorHeapKind heapKind) const = 0;
		virtual ID3D12DescriptorHeap* GetDescriptorHeap(
			NLS::Render::RHI::DX12::DX12DescriptorHeapKind heapKind) const = 0;
#endif
	};

	class IDX12PipelineLayoutAccess
	{
	public:
		virtual ~IDX12PipelineLayoutAccess() = default;
#if defined(_WIN32)
		virtual ID3D12RootSignature* GetRootSignature() const = 0;
#endif
		virtual const std::vector<NLS::Render::RHI::DX12::DX12DescriptorTableDesc>& GetDescriptorTables() const = 0;
	};

	class IDX12GraphicsPipelineAccess
	{
	public:
		virtual ~IDX12GraphicsPipelineAccess() = default;
#if defined(_WIN32)
		virtual ID3D12PipelineState* GetPipelineState() const = 0;
		virtual ID3D12RootSignature* GetRootSignature() const = 0;
#endif
	};

	class IDX12ComputePipelineAccess
	{
	public:
		virtual ~IDX12ComputePipelineAccess() = default;
#if defined(_WIN32)
		virtual ID3D12PipelineState* GetPipelineState() const = 0;
		virtual ID3D12RootSignature* GetRootSignature() const = 0;
#endif
	};
}
