#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "Rendering/RHI/Backends/DX12/DX12Access.h"
#include "Rendering/RHI/Backends/DX12/DX12PipelineLayoutUtils.h"
#include "Rendering/RHI/Core/RHIPipeline.h"

struct ID3D12Device;
struct ID3D12PipelineState;
struct ID3D12RootSignature;

#if defined(_WIN32)
#include <d3d12.h>
#include <wrl/client.h>
#undef CreateSemaphore
#endif

namespace NLS::Render::Backend
{
	class NativeDX12PipelineLayout final
		: public NLS::Render::RHI::RHIPipelineLayout
		, public IDX12PipelineLayoutAccess
	{
	public:
		explicit NativeDX12PipelineLayout(ID3D12Device* device, NLS::Render::RHI::RHIPipelineLayoutDesc desc);
		~NativeDX12PipelineLayout() override;

		std::string_view GetDebugName() const override;
		const NLS::Render::RHI::RHIPipelineLayoutDesc& GetDesc() const override;
#if defined(_WIN32)
		ID3D12RootSignature* GetRootSignature() const override;
#endif
		const std::vector<NLS::Render::RHI::DX12::DX12DescriptorTableDesc>& GetDescriptorTables() const override;

	private:
		ID3D12Device* m_device = nullptr;
		NLS::Render::RHI::RHIPipelineLayoutDesc m_desc;
#if defined(_WIN32)
		Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
#endif
		std::vector<NLS::Render::RHI::DX12::DX12DescriptorTableDesc> m_descriptorTables;
	};

	class NativeDX12ShaderModule final : public NLS::Render::RHI::RHIShaderModule
	{
	public:
		explicit NativeDX12ShaderModule(NLS::Render::RHI::RHIShaderModuleDesc desc);

		std::string_view GetDebugName() const override;
		const NLS::Render::RHI::RHIShaderModuleDesc& GetDesc() const override;

	private:
		NLS::Render::RHI::RHIShaderModuleDesc m_desc;
	};

	class NativeDX12GraphicsPipeline final
		: public NLS::Render::RHI::RHIGraphicsPipeline
		, public IDX12GraphicsPipelineAccess
	{
	public:
		explicit NativeDX12GraphicsPipeline(ID3D12Device* device, NLS::Render::RHI::RHIGraphicsPipelineDesc desc);
		~NativeDX12GraphicsPipeline() override;

		std::string_view GetDebugName() const override;
		const NLS::Render::RHI::RHIGraphicsPipelineDesc& GetDesc() const override;
#if defined(_WIN32)
		ID3D12PipelineState* GetPipelineState() const override;
		ID3D12RootSignature* GetRootSignature() const override;
#endif

	private:
		void CreateDefaultRootSignature();
		void CreatePipelineState();

		ID3D12Device* m_device = nullptr;
		NLS::Render::RHI::RHIGraphicsPipelineDesc m_desc;
#if defined(_WIN32)
		ID3D12RootSignature* m_rootSignature = nullptr;
		ID3D12PipelineState* m_pipelineState = nullptr;
#endif
	};

	class NativeDX12ComputePipeline final : public NLS::Render::RHI::RHIComputePipeline
		, public IDX12ComputePipelineAccess
	{
	public:
		explicit NativeDX12ComputePipeline(ID3D12Device* device, NLS::Render::RHI::RHIComputePipelineDesc desc);
		~NativeDX12ComputePipeline() override;

		std::string_view GetDebugName() const override;
		const NLS::Render::RHI::RHIComputePipelineDesc& GetDesc() const override;
#if defined(_WIN32)
		ID3D12PipelineState* GetPipelineState() const override;
		ID3D12RootSignature* GetRootSignature() const override;
#endif

	private:
		void CreatePipelineState();

		ID3D12Device* m_device = nullptr;
		NLS::Render::RHI::RHIComputePipelineDesc m_desc;
#if defined(_WIN32)
		ID3D12RootSignature* m_rootSignature = nullptr;
		ID3D12PipelineState* m_pipelineState = nullptr;
#endif
	};
}
