#include "Rendering/RHI/Backends/DX12/DX12Pipeline.h"

#include <algorithm>
#include <climits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <Debug/Logger.h>
#include "Rendering/RHI/Backends/DX12/DX12DebugNameUtils.h"
#include "Rendering/RHI/Backends/DX12/DX12FormatUtils.h"
#include "Rendering/RHI/Backends/DX12/DX12GraphicsPipelineUtils.h"
#include "Rendering/RHI/Backends/DX12/DX12PipelineCacheBlobUtils.h"
#include "Rendering/RHI/Utils/PipelineCache/PipelineCache.h"

namespace NLS::Render::Backend
{
#if defined(_WIN32)
	namespace
	{
		D3D12_COMPARISON_FUNC ToD3D12ComparisonFunc(NLS::Render::Settings::EComparaisonAlgorithm algorithm)
		{
			switch (algorithm)
			{
			case NLS::Render::Settings::EComparaisonAlgorithm::NEVER: return D3D12_COMPARISON_FUNC_NEVER;
			case NLS::Render::Settings::EComparaisonAlgorithm::LESS: return D3D12_COMPARISON_FUNC_LESS;
			case NLS::Render::Settings::EComparaisonAlgorithm::EQUAL: return D3D12_COMPARISON_FUNC_EQUAL;
			case NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
			case NLS::Render::Settings::EComparaisonAlgorithm::GREATER: return D3D12_COMPARISON_FUNC_GREATER;
			case NLS::Render::Settings::EComparaisonAlgorithm::NOTEQUAL: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
			case NLS::Render::Settings::EComparaisonAlgorithm::GREATER_EQUAL: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
			case NLS::Render::Settings::EComparaisonAlgorithm::ALWAYS:
			default:
				return D3D12_COMPARISON_FUNC_ALWAYS;
			}
		}

		D3D12_STENCIL_OP ToD3D12StencilOp(NLS::Render::Settings::EOperation operation)
		{
			switch (operation)
			{
			case NLS::Render::Settings::EOperation::KEEP: return D3D12_STENCIL_OP_KEEP;
			case NLS::Render::Settings::EOperation::ZERO: return D3D12_STENCIL_OP_ZERO;
			case NLS::Render::Settings::EOperation::REPLACE: return D3D12_STENCIL_OP_REPLACE;
			case NLS::Render::Settings::EOperation::INCREMENT: return D3D12_STENCIL_OP_INCR_SAT;
			case NLS::Render::Settings::EOperation::INCREMENT_WRAP: return D3D12_STENCIL_OP_INCR;
			case NLS::Render::Settings::EOperation::DECREMENT: return D3D12_STENCIL_OP_DECR_SAT;
			case NLS::Render::Settings::EOperation::DECREMENT_WRAP: return D3D12_STENCIL_OP_DECR;
			case NLS::Render::Settings::EOperation::INVERT: return D3D12_STENCIL_OP_INVERT;
			default: return D3D12_STENCIL_OP_KEEP;
			}
		}

		DXGI_FORMAT ToD3D12Format(NLS::Render::RHI::TextureFormat format)
		{
			const DXGI_FORMAT dxgiFormat = NLS::Render::RHI::DX12::ToDXGIFormat(format);
			return dxgiFormat != DXGI_FORMAT_UNKNOWN ? dxgiFormat : DXGI_FORMAT_R8G8B8A8_UNORM;
		}

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

			const auto wideName = Utf8ToWideString(debugName);
			if (!wideName.empty())
				object->SetName(wideName.c_str());
		}

		std::string PipelineKeyLabel(const NLS::Render::RHI::PipelineCacheKey& key)
		{
			std::ostringstream stream;
			stream << std::hex << key.hash;
			if (!key.stableDebugName.empty())
				stream << ":" << key.stableDebugName;
			return stream.str();
		}

		void LogDx12DebugMessages(ID3D12Device* device, const std::string& context)
		{
			if (device == nullptr)
				return;

			Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
			if (FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue))) || infoQueue == nullptr)
				return;

			const UINT64 messageCount = infoQueue->GetNumStoredMessages();
			if (messageCount == 0)
				return;

			const UINT64 firstMessage = messageCount > 8 ? messageCount - 8 : 0;
			for (UINT64 messageIndex = firstMessage; messageIndex < messageCount; ++messageIndex)
			{
				SIZE_T messageSize = 0;
				if (FAILED(infoQueue->GetMessage(messageIndex, nullptr, &messageSize)) || messageSize == 0)
					continue;

				std::vector<char> messageBytes(messageSize);
				auto* message = reinterpret_cast<D3D12_MESSAGE*>(messageBytes.data());
				if (FAILED(infoQueue->GetMessage(messageIndex, message, &messageSize)))
					continue;

				NLS_LOG_ERROR(
					context +
					": D3D12 message id=" + std::to_string(message->ID) +
					" severity=" + std::to_string(static_cast<int>(message->Severity)) +
					" text=" + std::string(message->pDescription));
			}

			infoQueue->ClearStoredMessages();
		}
	}
#endif

	NativeDX12PipelineLayout::NativeDX12PipelineLayout(
		ID3D12Device* device,
		NLS::Render::RHI::RHIPipelineLayoutDesc desc)
		: m_device(device)
		, m_desc(std::move(desc))
	{
#if defined(_WIN32)
		if (m_device != nullptr)
		{
			const auto ownedRootParameters = NLS::Render::RHI::DX12::BuildDX12OwnedRootParameters(m_desc);
			m_descriptorTables = ownedRootParameters.descriptorTables;
			m_pushConstantRootParameterOffset = ownedRootParameters.pushConstantRootParameterOffset;
			m_pushConstantRootParameters = ownedRootParameters.pushConstantRootParameters;

			D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
			rootSigDesc.NumParameters = static_cast<UINT>(ownedRootParameters.rootParameters.size());
			rootSigDesc.pParameters = ownedRootParameters.rootParameters.empty() ? nullptr : ownedRootParameters.rootParameters.data();
			rootSigDesc.NumStaticSamplers = 0;
			rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

			ID3DBlob* signatureBlob = nullptr;
			ID3DBlob* errorBlob = nullptr;
			HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &errorBlob);
			if (FAILED(hr))
			{
				if (errorBlob != nullptr)
				{
					NLS_LOG_ERROR("NativeDX12PipelineLayout: D3D12SerializeRootSignature failed: " + std::string(static_cast<char*>(errorBlob->GetBufferPointer())));
					errorBlob->Release();
				}
				return;
			}

			hr = m_device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature));
			signatureBlob->Release();
			if (FAILED(hr))
			{
				NLS_LOG_ERROR("NativeDX12PipelineLayout: CreateRootSignature failed with hr=" + std::to_string(hr));
				return;
			}
			SetDx12ObjectName(m_rootSignature.Get(), "DX12 RootSignature \"" + m_desc.debugName + "\"");
		}
#endif
	}

	NativeDX12PipelineLayout::~NativeDX12PipelineLayout() = default;

	std::string_view NativeDX12PipelineLayout::GetDebugName() const { return m_desc.debugName; }
	const NLS::Render::RHI::RHIPipelineLayoutDesc& NativeDX12PipelineLayout::GetDesc() const { return m_desc; }
	bool NativeDX12PipelineLayout::IsValid() const
	{
#if defined(_WIN32)
		return m_rootSignature != nullptr;
#else
		return true;
#endif
	}
#if defined(_WIN32)
	ID3D12RootSignature* NativeDX12PipelineLayout::GetRootSignature() const { return m_rootSignature.Get(); }
#endif
	const std::vector<NLS::Render::RHI::DX12::DX12DescriptorTableDesc>& NativeDX12PipelineLayout::GetDescriptorTables() const { return m_descriptorTables; }
	uint32_t NativeDX12PipelineLayout::GetPushConstantRootParameterOffset() const { return m_pushConstantRootParameterOffset; }
	const std::vector<NLS::Render::RHI::DX12::DX12PushConstantRootParameterDesc>& NativeDX12PipelineLayout::GetPushConstantRootParameters() const { return m_pushConstantRootParameters; }

	NativeDX12ShaderModule::NativeDX12ShaderModule(NLS::Render::RHI::RHIShaderModuleDesc desc)
		: m_desc(std::move(desc))
	{
	}

	std::string_view NativeDX12ShaderModule::GetDebugName() const { return m_desc.debugName; }
	const NLS::Render::RHI::RHIShaderModuleDesc& NativeDX12ShaderModule::GetDesc() const { return m_desc; }

	NativeDX12GraphicsPipeline::NativeDX12GraphicsPipeline(
		ID3D12Device* device,
		NLS::Render::RHI::RHIGraphicsPipelineDesc desc)
		: m_device(device)
		, m_desc(std::move(desc))
	{
#if defined(_WIN32)
		if (m_device == nullptr)
			return;

		bool usedLayoutRootSignature = false;
		if (m_desc.pipelineLayout != nullptr)
		{
			auto* nativeLayout = dynamic_cast<NativeDX12PipelineLayout*>(m_desc.pipelineLayout.get());
			if (nativeLayout != nullptr)
			{
				ID3D12RootSignature* layoutRootSig = nativeLayout->GetRootSignature();
				if (layoutRootSig != nullptr)
				{
					m_rootSignature = layoutRootSig;
					layoutRootSig->AddRef();
					usedLayoutRootSignature = true;
				}
			}
		}

		if (m_desc.pipelineLayout != nullptr && !usedLayoutRootSignature)
		{
			NLS_LOG_ERROR("NativeDX12GraphicsPipeline: pipeline layout has no valid native root signature: " + m_desc.debugName);
			return;
		}

		if (!usedLayoutRootSignature)
			CreateDefaultRootSignature();

		if (m_rootSignature != nullptr)
			CreatePipelineState();
#endif
	}

	NativeDX12GraphicsPipeline::~NativeDX12GraphicsPipeline()
	{
#if defined(_WIN32)
		if (m_pipelineState != nullptr)
			m_pipelineState->Release();
		if (m_rootSignature != nullptr)
			m_rootSignature->Release();
#endif
	}

	std::string_view NativeDX12GraphicsPipeline::GetDebugName() const { return m_desc.debugName; }
	const NLS::Render::RHI::RHIGraphicsPipelineDesc& NativeDX12GraphicsPipeline::GetDesc() const { return m_desc; }
	bool NativeDX12GraphicsPipeline::IsValid() const
	{
#if defined(_WIN32)
		return m_rootSignature != nullptr && m_pipelineState != nullptr;
#else
		return true;
#endif
	}
#if defined(_WIN32)
	ID3D12PipelineState* NativeDX12GraphicsPipeline::GetPipelineState() const { return m_pipelineState; }
	ID3D12RootSignature* NativeDX12GraphicsPipeline::GetRootSignature() const { return m_rootSignature; }
#endif

	void NativeDX12GraphicsPipeline::CreateDefaultRootSignature()
	{
#if defined(_WIN32)
		if (m_device == nullptr)
			return;

		D3D12_DESCRIPTOR_RANGE range = {};
		range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
		range.NumDescriptors = 1;
		range.BaseShaderRegister = 0;
		range.RegisterSpace = 0;
		range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_ROOT_PARAMETER rootParam = {};
		rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParam.DescriptorTable.NumDescriptorRanges = 1;
		rootParam.DescriptorTable.pDescriptorRanges = &range;
		rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
		rootSigDesc.NumParameters = 1;
		rootSigDesc.pParameters = &rootParam;
		rootSigDesc.NumStaticSamplers = 0;
		rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

		Microsoft::WRL::ComPtr<ID3DBlob> signature;
		Microsoft::WRL::ComPtr<ID3DBlob> error;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
		if (FAILED(hr))
		{
			if (error != nullptr)
			{
				NLS_LOG_ERROR("NativeDX12GraphicsPipeline: D3D12SerializeRootSignature failed: " + std::string(static_cast<char*>(error->GetBufferPointer())));
			}
			return;
		}

		hr = m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature));
		if (FAILED(hr))
		{
			NLS_LOG_ERROR("NativeDX12GraphicsPipeline: CreateRootSignature failed with hr=" + std::to_string(hr));
			return;
		}
		SetDx12ObjectName(m_rootSignature, "DX12 RootSignature \"" + m_desc.debugName + "\" default");
#endif
	}

	void NativeDX12GraphicsPipeline::CreatePipelineState()
	{
#if defined(_WIN32)
		if (m_device == nullptr || m_rootSignature == nullptr)
			return;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.pRootSignature = m_rootSignature;
		const auto inputLayout = NLS::Render::RHI::DX12::BuildDX12OwnedInputLayout(m_desc);

		if (m_desc.vertexShader)
		{
			const auto& vsDesc = m_desc.vertexShader->GetDesc();
			if (!vsDesc.bytecode.empty())
			{
				psoDesc.VS.pShaderBytecode = vsDesc.bytecode.data();
				psoDesc.VS.BytecodeLength = vsDesc.bytecode.size();
			}
		}
		if (m_desc.fragmentShader)
		{
			const auto& fsDesc = m_desc.fragmentShader->GetDesc();
			if (!fsDesc.bytecode.empty())
			{
				psoDesc.PS.pShaderBytecode = fsDesc.bytecode.data();
				psoDesc.PS.BytecodeLength = fsDesc.bytecode.size();
			}
		}

		psoDesc.DS.pShaderBytecode = nullptr;
		psoDesc.DS.BytecodeLength = 0;
		psoDesc.HS.pShaderBytecode = nullptr;
		psoDesc.HS.BytecodeLength = 0;
		psoDesc.GS.pShaderBytecode = nullptr;
		psoDesc.GS.BytecodeLength = 0;
		psoDesc.StreamOutput.pSODeclaration = nullptr;
		psoDesc.StreamOutput.NumEntries = 0;
		psoDesc.StreamOutput.pBufferStrides = nullptr;
		psoDesc.StreamOutput.NumStrides = 0;
		psoDesc.StreamOutput.RasterizedStream = 0;

		psoDesc.BlendState = NLS::Render::RHI::DX12::BuildDX12BlendState(m_desc);

		psoDesc.SampleMask = UINT_MAX;

		psoDesc.RasterizerState = NLS::Render::RHI::DX12::BuildDX12RasterizerState(m_desc);

		const bool hasDepthAttachment = m_desc.renderTargetLayout.hasDepth;
		const bool depthTestEnabled = hasDepthAttachment && m_desc.depthStencilState.depthTest;
		const bool depthWriteEnabled = hasDepthAttachment && m_desc.depthStencilState.depthWrite;
		const bool stencilEnabled = hasDepthAttachment && m_desc.depthStencilState.stencilTest;

		psoDesc.DepthStencilState.DepthEnable = depthTestEnabled ? TRUE : FALSE;
		psoDesc.DepthStencilState.DepthWriteMask = depthWriteEnabled ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
		psoDesc.DepthStencilState.DepthFunc = ToD3D12ComparisonFunc(m_desc.depthStencilState.depthCompare);
		psoDesc.DepthStencilState.StencilEnable = stencilEnabled ? TRUE : FALSE;
		psoDesc.DepthStencilState.StencilReadMask = static_cast<UINT8>(m_desc.depthStencilState.stencilReadMask);
		psoDesc.DepthStencilState.StencilWriteMask = static_cast<UINT8>(m_desc.depthStencilState.stencilWriteMask);
		psoDesc.DepthStencilState.FrontFace.StencilFunc = ToD3D12ComparisonFunc(m_desc.depthStencilState.stencilCompare);
		psoDesc.DepthStencilState.FrontFace.StencilDepthFailOp = ToD3D12StencilOp(m_desc.depthStencilState.stencilDepthFailOp);
		psoDesc.DepthStencilState.FrontFace.StencilPassOp = ToD3D12StencilOp(m_desc.depthStencilState.stencilPassOp);
		psoDesc.DepthStencilState.FrontFace.StencilFailOp = ToD3D12StencilOp(m_desc.depthStencilState.stencilFailOp);
		psoDesc.DepthStencilState.BackFace = psoDesc.DepthStencilState.FrontFace;

		psoDesc.InputLayout.pInputElementDescs = inputLayout.elements.empty() ? nullptr : inputLayout.elements.data();
		psoDesc.InputLayout.NumElements = static_cast<UINT>(inputLayout.elements.size());

		D3D12_PRIMITIVE_TOPOLOGY_TYPE topoType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		switch (m_desc.primitiveTopology)
		{
		case NLS::Render::RHI::PrimitiveTopology::PointList: topoType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT; break;
		case NLS::Render::RHI::PrimitiveTopology::LineList: topoType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; break;
		case NLS::Render::RHI::PrimitiveTopology::TriangleList: topoType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; break;
		}
		psoDesc.PrimitiveTopologyType = topoType;
		const auto colorFormatCount = std::min<size_t>(m_desc.renderTargetLayout.colorFormats.size(), 8u);
		psoDesc.NumRenderTargets = colorFormatCount > 0u ? static_cast<UINT>(colorFormatCount) : 1u;
		if (colorFormatCount == 0u)
		{
			psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		}
		else
		{
			for (size_t colorFormatIndex = 0; colorFormatIndex < colorFormatCount; ++colorFormatIndex)
				psoDesc.RTVFormats[colorFormatIndex] = ToD3D12Format(m_desc.renderTargetLayout.colorFormats[colorFormatIndex]);
		}
		psoDesc.DSVFormat = hasDepthAttachment
			? ToD3D12Format(m_desc.renderTargetLayout.depthFormat)
			: DXGI_FORMAT_UNKNOWN;
		psoDesc.SampleDesc.Count = 1;
		psoDesc.SampleDesc.Quality = 0;
		psoDesc.NodeMask = 0;
		const auto cacheKey = NLS::Render::RHI::BuildGraphicsPipelineCacheKey(m_desc);
		const auto pipelineDebugLabel = NLS::Render::RHI::DX12::BuildDX12GraphicsPipelineDebugLabel(m_desc, PipelineKeyLabel(cacheKey));
		const auto cacheBlobPaths = NLS::Render::RHI::DX12::BuildDX12PipelineCacheBlobPaths(
			cacheKey,
			"graphics",
			NLS::Render::RHI::DX12::GetDX12PipelineCacheBlobRoot());
		const auto cachedBlob = NLS::Render::RHI::DX12::ReadDX12PipelineCacheBlob(cacheBlobPaths.blobPath);
		psoDesc.CachedPSO = NLS::Render::RHI::DX12::BuildDX12CachedPipelineStateView(cachedBlob);
		const bool usedCachedBlob = psoDesc.CachedPSO.pCachedBlob != nullptr && psoDesc.CachedPSO.CachedBlobSizeInBytes != 0u;
		psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

		HRESULT hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState));
		if (NLS::Render::RHI::DX12::ShouldRetryDX12PipelineCreationWithoutCachedBlob(usedCachedBlob, hr))
		{
			psoDesc.CachedPSO = {};
			hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState));
		}
		if (FAILED(hr))
		{
			std::ostringstream diagnostics;
			diagnostics
				<< "NativeDX12GraphicsPipeline: CreateGraphicsPipelineState failed"
				<< " hr=" << hr
				<< " debugName=\"" << m_desc.debugName << "\""
				<< " rootSignature=" << (m_rootSignature != nullptr ? "valid" : "null")
				<< " pipelineLayout=" << (m_desc.pipelineLayout != nullptr ? "set" : "null")
				<< " vsModule=" << (m_desc.vertexShader != nullptr ? "set" : "null")
				<< " psModule=" << (m_desc.fragmentShader != nullptr ? "set" : "null")
				<< " vsName=\"" << (m_desc.vertexShader != nullptr ? std::string(m_desc.vertexShader->GetDebugName()) : std::string{}) << "\""
				<< " psName=\"" << (m_desc.fragmentShader != nullptr ? std::string(m_desc.fragmentShader->GetDebugName()) : std::string{}) << "\""
				<< " vsBytes=" << psoDesc.VS.BytecodeLength
				<< " psBytes=" << psoDesc.PS.BytecodeLength
				<< " inputElements=" << psoDesc.InputLayout.NumElements
				<< " vertexBuffers=" << m_desc.vertexBuffers.size()
				<< " vertexAttributes=" << m_desc.vertexAttributes.size()
				<< " numRT=" << psoDesc.NumRenderTargets
				<< " hasDepth=" << (hasDepthAttachment ? "true" : "false")
				<< " dsvFormat=" << static_cast<int>(psoDesc.DSVFormat)
				<< " sampleCount=" << psoDesc.SampleDesc.Count
				<< " primitiveTopologyType=" << static_cast<int>(psoDesc.PrimitiveTopologyType);
			for (UINT formatIndex = 0; formatIndex < psoDesc.NumRenderTargets && formatIndex < 8u; ++formatIndex)
				diagnostics << " rtv" << formatIndex << "=" << static_cast<int>(psoDesc.RTVFormats[formatIndex]);
			for (size_t attributeIndex = 0; attributeIndex < m_desc.vertexAttributes.size(); ++attributeIndex)
			{
				const auto& attribute = m_desc.vertexAttributes[attributeIndex];
				diagnostics
					<< " attr" << attributeIndex
					<< "{loc=" << attribute.location
					<< ",binding=" << attribute.binding
					<< ",offset=" << attribute.offset
					<< ",size=" << attribute.elementSize
					<< "}";
			}
			NLS_LOG_ERROR(diagnostics.str());
			LogDx12DebugMessages(m_device, "NativeDX12GraphicsPipeline");
			return;
		}
		SetDx12ObjectName(m_pipelineState, pipelineDebugLabel);

		Microsoft::WRL::ComPtr<ID3DBlob> createdBlob;
		if (SUCCEEDED(m_pipelineState->GetCachedBlob(&createdBlob)) && createdBlob != nullptr)
		{
			NLS::Render::RHI::DX12::WriteDX12PipelineCacheBlobAtomically(
				cacheBlobPaths.blobPath,
				createdBlob->GetBufferPointer(),
				createdBlob->GetBufferSize());
		}
#endif
	}

	NativeDX12ComputePipeline::NativeDX12ComputePipeline(
		ID3D12Device* device,
		NLS::Render::RHI::RHIComputePipelineDesc desc)
		: m_device(device)
		, m_desc(std::move(desc))
	{
#if defined(_WIN32)
		if (m_device == nullptr)
			return;

		if (m_desc.pipelineLayout != nullptr)
		{
			auto* nativeLayout = dynamic_cast<NativeDX12PipelineLayout*>(m_desc.pipelineLayout.get());
			if (nativeLayout != nullptr)
			{
				ID3D12RootSignature* layoutRootSig = nativeLayout->GetRootSignature();
				if (layoutRootSig != nullptr)
				{
					m_rootSignature = layoutRootSig;
					m_rootSignature->AddRef();
				}
			}
		}

		if (m_desc.pipelineLayout != nullptr && m_rootSignature == nullptr)
		{
			NLS_LOG_ERROR("NativeDX12ComputePipeline: pipeline layout has no valid native root signature: " + m_desc.debugName);
			return;
		}

		if (m_rootSignature != nullptr)
			CreatePipelineState();
#endif
	}

	NativeDX12ComputePipeline::~NativeDX12ComputePipeline()
	{
#if defined(_WIN32)
		if (m_pipelineState != nullptr)
			m_pipelineState->Release();
		if (m_rootSignature != nullptr)
			m_rootSignature->Release();
#endif
	}

	std::string_view NativeDX12ComputePipeline::GetDebugName() const { return m_desc.debugName; }
	const NLS::Render::RHI::RHIComputePipelineDesc& NativeDX12ComputePipeline::GetDesc() const { return m_desc; }
	bool NativeDX12ComputePipeline::IsValid() const
	{
#if defined(_WIN32)
		return m_rootSignature != nullptr && m_pipelineState != nullptr;
#else
		return true;
#endif
	}
#if defined(_WIN32)
	ID3D12PipelineState* NativeDX12ComputePipeline::GetPipelineState() const { return m_pipelineState; }
	ID3D12RootSignature* NativeDX12ComputePipeline::GetRootSignature() const { return m_rootSignature; }
#endif

	void NativeDX12ComputePipeline::CreatePipelineState()
	{
#if defined(_WIN32)
		if (m_device == nullptr || m_rootSignature == nullptr || m_desc.computeShader == nullptr)
			return;

		const auto& shaderDesc = m_desc.computeShader->GetDesc();
		if (shaderDesc.bytecode.empty())
			return;

		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.pRootSignature = m_rootSignature;
		psoDesc.CS.pShaderBytecode = shaderDesc.bytecode.data();
		psoDesc.CS.BytecodeLength = shaderDesc.bytecode.size();
		psoDesc.NodeMask = 0;
		const auto cacheKey = NLS::Render::RHI::BuildComputePipelineCacheKey(m_desc);
		const auto pipelineDebugLabel = NLS::Render::RHI::DX12::BuildDX12ComputePipelineDebugLabel(m_desc, PipelineKeyLabel(cacheKey));
		const auto cacheBlobPaths = NLS::Render::RHI::DX12::BuildDX12PipelineCacheBlobPaths(
			cacheKey,
			"compute",
			NLS::Render::RHI::DX12::GetDX12PipelineCacheBlobRoot());
		const auto cachedBlob = NLS::Render::RHI::DX12::ReadDX12PipelineCacheBlob(cacheBlobPaths.blobPath);
		psoDesc.CachedPSO = NLS::Render::RHI::DX12::BuildDX12CachedPipelineStateView(cachedBlob);
		const bool usedCachedBlob = psoDesc.CachedPSO.pCachedBlob != nullptr && psoDesc.CachedPSO.CachedBlobSizeInBytes != 0u;
		psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

		HRESULT hr = m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState));
		if (NLS::Render::RHI::DX12::ShouldRetryDX12PipelineCreationWithoutCachedBlob(usedCachedBlob, hr))
		{
			psoDesc.CachedPSO = {};
			hr = m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState));
		}
		if (FAILED(hr))
		{
			NLS_LOG_ERROR("NativeDX12ComputePipeline: CreateComputePipelineState failed with hr=" + std::to_string(hr));
			LogDx12DebugMessages(m_device, "NativeDX12ComputePipeline");
			return;
		}
		SetDx12ObjectName(m_pipelineState, pipelineDebugLabel);

		Microsoft::WRL::ComPtr<ID3DBlob> createdBlob;
		if (SUCCEEDED(m_pipelineState->GetCachedBlob(&createdBlob)) && createdBlob != nullptr)
		{
			NLS::Render::RHI::DX12::WriteDX12PipelineCacheBlobAtomically(
				cacheBlobPaths.blobPath,
				createdBlob->GetBufferPointer(),
				createdBlob->GetBufferSize());
		}
#endif
	}
}
