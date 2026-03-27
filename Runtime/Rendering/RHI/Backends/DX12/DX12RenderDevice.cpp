#include "Rendering/RHI/Backends/DX12/DX12RenderDevice.h"

#include <Debug/Logger.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Rendering/Resources/BindingSetInstance.h"
#include "Rendering/RHI/BindingPointMap.h"

#if defined(_WIN32)
#ifdef APIENTRY
#undef APIENTRY
#endif
#include <Windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif

namespace NLS::Render::Backend
{
	namespace
	{
		using BindingSetInstance = NLS::Render::Resources::BindingSetInstance;
		using GraphicsPipelineDesc = NLS::Render::RHI::GraphicsPipelineDesc;
		using ShaderStageDesc = NLS::Render::RHI::ShaderStageDesc;

		class DX12TextureResource final : public NLS::Render::RHI::IRHITexture
		{
		public:
			DX12TextureResource(uint32_t id, NLS::Render::RHI::TextureDesc desc, std::function<void(uint32_t)> destroy)
				: m_id(id), m_desc(desc), m_destroy(std::move(destroy))
			{
			}

			~DX12TextureResource() override
			{
				if (m_destroy && m_id != 0)
					m_destroy(m_id);
			}

			NLS::Render::RHI::RHIResourceType GetResourceType() const override { return NLS::Render::RHI::RHIResourceType::Texture; }
			uint32_t GetResourceId() const override { return m_id; }
			NLS::Render::RHI::TextureDimension GetDimension() const override { return m_desc.dimension; }
			const NLS::Render::RHI::TextureDesc& GetDesc() const override { return m_desc; }
			void SetDesc(const NLS::Render::RHI::TextureDesc& desc) override { m_desc = desc; }

		private:
			uint32_t m_id = 0;
			NLS::Render::RHI::TextureDesc m_desc{};
			std::function<void(uint32_t)> m_destroy;
		};

		class DX12BufferResource final : public NLS::Render::RHI::IRHIBuffer
		{
		public:
			DX12BufferResource(uint32_t id, NLS::Render::RHI::BufferType type, std::function<void(uint32_t)> destroy)
				: m_id(id), m_type(type), m_destroy(std::move(destroy))
			{
			}

			~DX12BufferResource() override
			{
				if (m_destroy && m_id != 0)
					m_destroy(m_id);
			}

			NLS::Render::RHI::RHIResourceType GetResourceType() const override { return NLS::Render::RHI::RHIResourceType::Buffer; }
			uint32_t GetResourceId() const override { return m_id; }
			NLS::Render::RHI::BufferType GetBufferType() const override { return m_type; }
			size_t GetSize() const override { return m_size; }
			void SetSize(size_t size) override { m_size = size; }

		private:
			uint32_t m_id = 0;
			NLS::Render::RHI::BufferType m_type = NLS::Render::RHI::BufferType::Uniform;
			size_t m_size = 0;
			std::function<void(uint32_t)> m_destroy;
		};

#if defined(_WIN32)
		struct PipelineCacheEntry
		{
			struct RootConstantBufferBinding
			{
				uint32_t bindingSpace = 0;
				uint32_t bindingIndex = 0;
				UINT rootParameterIndex = 0;
			};

			struct DescriptorTableBinding
			{
				NLS::Render::Resources::ShaderResourceKind kind = NLS::Render::Resources::ShaderResourceKind::SampledTexture;
				uint32_t bindingSpace = 0;
				UINT rootParameterIndex = 0;
				UINT descriptorHeapOffset = 0;
			};

			struct TextureDescriptorBinding
			{
				std::string name;
				uint32_t bindingSpace = 0;
				uint32_t bindingIndex = 0;
				UINT descriptorHeapOffset = 0;
				DXGI_FORMAT fallbackFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
				NLS::Render::RHI::TextureDimension defaultDimension = NLS::Render::RHI::TextureDimension::Texture2D;
			};

			Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
			Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState;
			std::vector<RootConstantBufferBinding> constantBuffers;
			std::vector<DescriptorTableBinding> descriptorTables;
			std::vector<TextureDescriptorBinding> sampledTextures;
		};

		D3D12_PRIMITIVE_TOPOLOGY ToD3DPrimitiveTopology(NLS::Render::Settings::EPrimitiveMode primitiveMode)
		{
			switch (primitiveMode)
			{
			case NLS::Render::Settings::EPrimitiveMode::LINES: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
			case NLS::Render::Settings::EPrimitiveMode::POINTS: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
			case NLS::Render::Settings::EPrimitiveMode::TRIANGLES:
			default:
				return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			}
		}

		D3D12_PRIMITIVE_TOPOLOGY_TYPE ToD3DPrimitiveTopologyType(NLS::Render::Settings::EPrimitiveMode primitiveMode)
		{
			switch (primitiveMode)
			{
			case NLS::Render::Settings::EPrimitiveMode::LINES: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
			case NLS::Render::Settings::EPrimitiveMode::POINTS: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
			case NLS::Render::Settings::EPrimitiveMode::TRIANGLES:
			default:
				return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			}
		}

		D3D12_CULL_MODE ToD3DCullMode(NLS::Render::Settings::ECullFace cullFace)
		{
			switch (cullFace)
			{
			case NLS::Render::Settings::ECullFace::FRONT: return D3D12_CULL_MODE_FRONT;
			case NLS::Render::Settings::ECullFace::BACK: return D3D12_CULL_MODE_BACK;
			case NLS::Render::Settings::ECullFace::FRONT_AND_BACK:
			default:
				return D3D12_CULL_MODE_NONE;
			}
		}

		D3D12_COMPARISON_FUNC ToD3DComparisonFunc(NLS::Render::Settings::EComparaisonAlgorithm algorithm)
		{
			switch (algorithm)
			{
			case NLS::Render::Settings::EComparaisonAlgorithm::LESS: return D3D12_COMPARISON_FUNC_LESS;
			case NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
			case NLS::Render::Settings::EComparaisonAlgorithm::GREATER: return D3D12_COMPARISON_FUNC_GREATER;
			case NLS::Render::Settings::EComparaisonAlgorithm::GREATER_EQUAL: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
			case NLS::Render::Settings::EComparaisonAlgorithm::EQUAL: return D3D12_COMPARISON_FUNC_EQUAL;
			case NLS::Render::Settings::EComparaisonAlgorithm::NOTEQUAL: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
			case NLS::Render::Settings::EComparaisonAlgorithm::ALWAYS: return D3D12_COMPARISON_FUNC_ALWAYS;
			case NLS::Render::Settings::EComparaisonAlgorithm::NEVER:
			default:
				return D3D12_COMPARISON_FUNC_NEVER;
			}
		}

		std::string FormatHRESULT(const HRESULT hr)
		{
			char* messageBuffer = nullptr;
			const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
			const DWORD languageId = MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT);
			const DWORD messageLength = FormatMessageA(
				flags,
				nullptr,
				static_cast<DWORD>(hr),
				languageId,
				reinterpret_cast<LPSTR>(&messageBuffer),
				0,
				nullptr);

			char hexCode[11]{};
			snprintf(hexCode, sizeof(hexCode), "0x%08lX", static_cast<unsigned long>(hr));
			std::string message = "HRESULT=" + std::string(hexCode);
			if (messageLength != 0 && messageBuffer != nullptr)
			{
				std::string systemMessage(messageBuffer, messageLength);
				while (!systemMessage.empty() && (systemMessage.back() == '\r' || systemMessage.back() == '\n' || systemMessage.back() == ' '))
					systemMessage.pop_back();
				message += " (" + systemMessage + ")";
			}

			if (messageBuffer != nullptr)
				LocalFree(messageBuffer);

			return message;
		}

		bool ShouldLogDX12DrawDiagnostics()
		{
			static const bool enabled = []()
			{
				if (const char* value = std::getenv("NLS_LOG_RENDER_DRAW_PATH"); value != nullptr)
					return std::strcmp(value, "1") == 0 || _stricmp(value, "true") == 0;
				return false;
			}();
			return enabled;
		}

		void LogDX12InfoQueueMessages(ID3D12Device* device, const std::string& prefix)
		{
			if (device == nullptr)
				return;

			Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
			if (FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue))) || infoQueue == nullptr)
				return;

			const UINT64 messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
			for (UINT64 messageIndex = 0; messageIndex < messageCount; ++messageIndex)
			{
				SIZE_T messageLength = 0;
				if (FAILED(infoQueue->GetMessage(messageIndex, nullptr, &messageLength)) || messageLength == 0)
					continue;

				std::vector<uint8_t> messageBytes(messageLength);
				auto* message = reinterpret_cast<D3D12_MESSAGE*>(messageBytes.data());
				if (FAILED(infoQueue->GetMessage(messageIndex, message, &messageLength)))
					continue;

				std::string severity;
				switch (message->Severity)
				{
				case D3D12_MESSAGE_SEVERITY_CORRUPTION: severity = "corruption"; break;
				case D3D12_MESSAGE_SEVERITY_ERROR: severity = "error"; break;
				case D3D12_MESSAGE_SEVERITY_WARNING: severity = "warning"; break;
				case D3D12_MESSAGE_SEVERITY_INFO: severity = "info"; break;
				case D3D12_MESSAGE_SEVERITY_MESSAGE:
				default:
					severity = "message";
					break;
				}

				NLS_LOG_WARNING(prefix + " [" + severity + "] " + (message->pDescription != nullptr ? message->pDescription : "<no description>"));
			}

			infoQueue->ClearStoredMessages();
		}

		DXGI_FORMAT ToDxgiSrvFormat(NLS::Render::RHI::TextureFormat format)
		{
			switch (format)
			{
			case NLS::Render::RHI::TextureFormat::RGB8: return DXGI_FORMAT_R8G8B8A8_UNORM;
			case NLS::Render::RHI::TextureFormat::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
			case NLS::Render::RHI::TextureFormat::RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
			case NLS::Render::RHI::TextureFormat::Depth24Stencil8: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			default: return DXGI_FORMAT_R8G8B8A8_UNORM;
			}
		}

		DXGI_FORMAT ToDxgiResourceFormat(NLS::Render::RHI::TextureFormat format)
		{
			switch (format)
			{
			case NLS::Render::RHI::TextureFormat::Depth24Stencil8: return DXGI_FORMAT_R24G8_TYPELESS;
			default: return ToDxgiSrvFormat(format);
			}
		}

		DXGI_FORMAT ToDxgiAttachmentFormat(NLS::Render::RHI::TextureFormat format)
		{
			switch (format)
			{
			case NLS::Render::RHI::TextureFormat::Depth24Stencil8: return DXGI_FORMAT_D24_UNORM_S8_UINT;
			default: return ToDxgiSrvFormat(format);
			}
		}

		D3D12_RESOURCE_FLAGS GetTextureResourceFlags(const NLS::Render::RHI::TextureDesc& desc)
		{
			if (NLS::Render::RHI::HasUsage(desc.usage, NLS::Render::RHI::TextureUsage::DepthStencilAttachment))
				return D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

			if (NLS::Render::RHI::HasUsage(desc.usage, NLS::Render::RHI::TextureUsage::ColorAttachment))
				return D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

			return D3D12_RESOURCE_FLAG_NONE;
		}

		D3D12_SRV_DIMENSION ToD3DSrvDimension(NLS::Render::RHI::TextureDimension dimension)
		{
			return dimension == NLS::Render::RHI::TextureDimension::TextureCube
				? D3D12_SRV_DIMENSION_TEXTURECUBE
				: D3D12_SRV_DIMENSION_TEXTURE2D;
		}

		void FillTextureSrvDesc(
			D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc,
			NLS::Render::RHI::TextureFormat format,
			NLS::Render::RHI::TextureDimension dimension)
		{
			srvDesc = {};
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Format = ToDxgiSrvFormat(format);
			srvDesc.ViewDimension = ToD3DSrvDimension(dimension);
			if (dimension == NLS::Render::RHI::TextureDimension::TextureCube)
			{
				srvDesc.TextureCube.MipLevels = 1;
				srvDesc.TextureCube.MostDetailedMip = 0;
				srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
			}
			else
			{
				srvDesc.Texture2D.MipLevels = 1;
				srvDesc.Texture2D.MostDetailedMip = 0;
				srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
			}
		}

		auto ExecuteImmediateCommandList = [](auto& impl, const std::function<bool(ID3D12GraphicsCommandList*)>& record)
		{
			if (!impl.device || !impl.graphicsQueue || !impl.fence || !impl.fenceEvent)
				return false;

			Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
			Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
			if (FAILED(impl.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
				FAILED(impl.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList))))
			{
				return false;
			}

			if (!record(commandList.Get()) || FAILED(commandList->Close()))
				return false;

			ID3D12CommandList* commandLists[] = { commandList.Get() };
			impl.graphicsQueue->ExecuteCommandLists(1, commandLists);

			const UINT64 fenceValue = ++impl.fenceValue;
			impl.graphicsQueue->Signal(impl.fence.Get(), fenceValue);
			if (impl.fence->GetCompletedValue() < fenceValue)
			{
				impl.fence->SetEventOnCompletion(fenceValue, impl.fenceEvent);
				WaitForSingleObject(impl.fenceEvent, INFINITE);
			}

			return true;
		};

		uint64_t HashBytes(const std::vector<uint8_t>& bytes)
		{
			uint64_t hash = 1469598103934665603ull;
			for (const auto byte : bytes)
			{
				hash ^= static_cast<uint64_t>(byte);
				hash *= 1099511628211ull;
			}
			return hash;
		}

		std::string BuildPipelineCacheKey(const GraphicsPipelineDesc& desc)
		{
			std::string key = std::to_string(static_cast<int>(desc.rasterState.cullFace)) + "|" +
				std::to_string(desc.rasterState.culling ? 1 : 0) + "|" +
				std::to_string(desc.depthStencilState.depthTest ? 1 : 0) + "|" +
				std::to_string(desc.depthStencilState.depthWrite ? 1 : 0) + "|" +
				std::to_string(static_cast<int>(desc.depthStencilState.depthCompare)) + "|" +
				std::to_string(desc.blendState.enabled ? 1 : 0) + "|" +
				std::to_string(desc.layout.uniformBufferBindingCount) + "|" +
				std::to_string(desc.layout.sampledTextureBindingCount) + "|" +
				std::to_string(desc.layout.samplerBindingCount) + "|" +
				std::to_string(desc.layout.storageBufferBindingCount) + "|" +
				std::to_string(static_cast<int>(desc.primitiveMode)) + "|" +
				std::to_string(desc.attachmentLayout.sampleCount) + "|" +
				std::to_string(desc.attachmentLayout.hasDepthAttachment ? 1 : 0);

			for (const auto format : desc.attachmentLayout.colorAttachmentFormats)
				key += "|rtf:" + std::to_string(static_cast<int>(format));
			key += "|dsf:" + std::to_string(static_cast<int>(desc.attachmentLayout.depthAttachmentFormat));

			for (const auto& stage : desc.shaderStages)
			{
				key += "|" + std::to_string(static_cast<int>(stage.stage)) +
					":" + std::to_string(static_cast<int>(stage.targetPlatform)) +
					":" + stage.entryPoint +
					":" + std::to_string(stage.bytecode.size()) +
					":" + std::to_string(HashBytes(stage.bytecode));
			}

			if (desc.reflection != nullptr)
			{
				for (const auto& constantBuffer : desc.reflection->constantBuffers)
				{
					key += "|cb:" + constantBuffer.name +
						":" + std::to_string(constantBuffer.bindingSpace) +
						":" + std::to_string(constantBuffer.bindingIndex);
				}

				for (const auto& property : desc.reflection->properties)
				{
					if (property.kind != NLS::Render::Resources::ShaderResourceKind::SampledTexture &&
						property.kind != NLS::Render::Resources::ShaderResourceKind::Sampler)
					{
						continue;
					}

					key += "|res:" + property.name +
						":" + std::to_string(static_cast<int>(property.kind)) +
						":" + std::to_string(property.bindingSpace) +
						":" + std::to_string(property.bindingIndex);
				}
			}

			return key;
		}

#endif
	}

	struct DX12RenderDevice::Impl
	{
#if defined(_WIN32)
		struct BufferResource
		{
			Microsoft::WRL::ComPtr<ID3D12Resource> resource;
			size_t size = 0;
		};

		struct TextureResource
		{
			Microsoft::WRL::ComPtr<ID3D12Resource> resource;
			NLS::Render::RHI::TextureDesc desc{};
			D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
		};

		struct FrameContext
		{
			Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
			Microsoft::WRL::ComPtr<ID3D12Resource> backBuffer;
			D3D12_RESOURCE_STATES backBufferState = D3D12_RESOURCE_STATE_PRESENT;
		};

		struct FramebufferResource
		{
			std::vector<uint32_t> colorTextureIds;
			uint32_t depthTextureId = 0;
			uint32_t drawBufferCount = 0;
			Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
			Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap;
			bool descriptorsDirty = true;
			NLS::Render::RHI::TextureFormat colorFormat = NLS::Render::RHI::TextureFormat::RGBA8;
		};

		Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
		Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
		Microsoft::WRL::ComPtr<ID3D12Device> device;
		Microsoft::WRL::ComPtr<ID3D12CommandQueue> graphicsQueue;
		Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
		Microsoft::WRL::ComPtr<ID3D12Fence> fence;
		HANDLE fenceEvent = nullptr;
		UINT64 fenceValue = 0;
		UINT rtvDescriptorSize = 0;
		UINT srvDescriptorSize = 0;
		Microsoft::WRL::ComPtr<ID3D12Resource> depthBuffer;
		Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain;
		HWND swapchainWindow = nullptr;
		uint32_t swapchainWidth = 0;
		uint32_t swapchainHeight = 0;
		uint32_t swapchainImageCount = 2;
		bool swapchainVsync = true;
		std::vector<FrameContext> frameContexts;
		D3D12_VIEWPORT viewport{ 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
		D3D12_RECT scissorRect{ 0, 0, 1, 1 };
		FLOAT clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
		float clearDepth = 1.0f;
		bool hasPendingCommands = false;
		bool isFrameRecording = false;
		std::unordered_map<uint32_t, BufferResource> buffers;
		std::unordered_map<uint32_t, TextureResource> textures;
		std::unordered_map<uint32_t, FramebufferResource> framebuffers;
		std::unordered_map<uint32_t, std::weak_ptr<NLS::Render::RHI::IRHITexture>> textureObjects;
		std::unordered_map<uint32_t, std::weak_ptr<NLS::Render::RHI::IRHIBuffer>> bufferObjects;
		std::unordered_map<std::string, PipelineCacheEntry> pipelineCache;
		std::unordered_map<NLS::Render::RHI::BufferType, uint32_t> boundBuffers;
		std::unordered_map<uint32_t, uint32_t> uniformBufferBindings;
		NLS::Render::RHI::GraphicsPipelineDesc currentPipelineDesc{};
		const NLS::Render::Resources::BindingSetInstance* currentBindingSet = nullptr;
		uint32_t boundFramebuffer = 0;
		uint32_t boundTexture = 0;
		uint32_t nextResourceId = 1;
		bool hasLoggedDrawStub = false;
		uint32_t pendingInstanceCount = 1u;
#endif
		std::string vendor = "DX12";
		std::string hardware = "Stub";
		std::string version = "Stub";
		std::string shadingLanguageVersion = "DXIL";
		NLS::Render::RHI::RHIDeviceCapabilities capabilities{};
		bool backendReady = false;
	};

#if defined(_WIN32)
	namespace
	{
		D3D12_RESOURCE_STATES GetInitialTextureState(const NLS::Render::RHI::TextureDesc& desc)
		{
			if (NLS::Render::RHI::HasUsage(desc.usage, NLS::Render::RHI::TextureUsage::DepthStencilAttachment))
				return D3D12_RESOURCE_STATE_DEPTH_WRITE;

			if (NLS::Render::RHI::HasUsage(desc.usage, NLS::Render::RHI::TextureUsage::ColorAttachment))
				return D3D12_RESOURCE_STATE_RENDER_TARGET;

			if (NLS::Render::RHI::HasUsage(desc.usage, NLS::Render::RHI::TextureUsage::Sampled))
				return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

			return D3D12_RESOURCE_STATE_COMMON;
		}

		bool EnsureCommandListRecording(DX12RenderDevice::Impl& impl)
		{
			if (impl.isFrameRecording)
				return impl.commandList != nullptr;

			if (!impl.swapchain || !impl.commandList || impl.frameContexts.empty())
				return false;

			const UINT frameIndex = impl.swapchain->GetCurrentBackBufferIndex();
			auto& frame = impl.frameContexts[frameIndex];
			if (!frame.commandAllocator)
				return false;

			frame.commandAllocator->Reset();
			if (FAILED(impl.commandList->Reset(frame.commandAllocator.Get(), nullptr)))
				return false;

			impl.isFrameRecording = true;
			return true;
		}

		void TransitionResource(
			ID3D12GraphicsCommandList* commandList,
			ID3D12Resource* resource,
			D3D12_RESOURCE_STATES& currentState,
			D3D12_RESOURCE_STATES targetState)
		{
			if (!commandList || !resource || currentState == targetState)
				return;

			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = resource;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = currentState;
			barrier.Transition.StateAfter = targetState;
			commandList->ResourceBarrier(1, &barrier);
			currentState = targetState;
		}

		auto TransitionBindingSetTexturesToShaderRead = [](auto& impl, ID3D12GraphicsCommandList* commandList, const PipelineCacheEntry& pipelineEntry)
		{
			if (commandList == nullptr || impl.currentBindingSet == nullptr)
				return;

			for (const auto& textureBinding : pipelineEntry.sampledTextures)
			{
				const auto* entry = impl.currentBindingSet->Find(textureBinding.name);
				if (entry == nullptr || entry->textureResource == nullptr)
					continue;

				const auto textureIt = impl.textures.find(entry->textureResource->GetResourceId());
				if (textureIt == impl.textures.end() || !textureIt->second.resource)
					continue;

				TransitionResource(
					commandList,
					textureIt->second.resource.Get(),
					textureIt->second.state,
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}
		};

		auto ResolveUniformBufferId = [](const auto& impl, const NLS::Render::Resources::BindingSetInstance* bindingSet, const uint32_t bindingSpace, const uint32_t bindingIndex)
		{
			if (bindingSet != nullptr)
			{
				for (const auto& entry : bindingSet->Entries())
				{
					if (entry.kind != NLS::Render::Resources::ShaderResourceKind::UniformBuffer ||
						entry.bindingSpace != bindingSpace ||
						entry.bindingIndex != bindingIndex ||
						entry.bufferResource == nullptr)
					{
						continue;
					}

					return entry.bufferResource->GetResourceId();
				}
			}

			const auto bindingPoint = NLS::Render::RHI::BindingPointMap::GetUniformBufferBindingPoint(bindingSpace, bindingIndex);
			if (const auto bindingIt = impl.uniformBufferBindings.find(bindingPoint); bindingIt != impl.uniformBufferBindings.end())
				return bindingIt->second;

			return 0u;
		};

		void EnsureFramebufferDescriptors(DX12RenderDevice::Impl& impl, uint32_t framebufferId)
		{
			auto framebufferIt = impl.framebuffers.find(framebufferId);
			if (framebufferIt == impl.framebuffers.end() || !impl.device)
				return;

			auto& framebuffer = framebufferIt->second;
			if (!framebuffer.descriptorsDirty)
				return;

			const auto colorCount = static_cast<UINT>(framebuffer.colorTextureIds.size());
			if (colorCount > 0)
			{
				D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
				rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
				rtvHeapDesc.NumDescriptors = colorCount;
				rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
				impl.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&framebuffer.rtvHeap));

				if (framebuffer.rtvHeap)
				{
					auto rtvHandle = framebuffer.rtvHeap->GetCPUDescriptorHandleForHeapStart();
					for (UINT i = 0; i < colorCount; ++i)
					{
						const auto textureIt = impl.textures.find(framebuffer.colorTextureIds[i]);
						if (textureIt == impl.textures.end() || !textureIt->second.resource)
							continue;

						D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
						rtvDesc.Format = ToDxgiAttachmentFormat(textureIt->second.desc.format);
						rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
						impl.device->CreateRenderTargetView(textureIt->second.resource.Get(), &rtvDesc, rtvHandle);
						framebuffer.colorFormat = textureIt->second.desc.format;
						rtvHandle.ptr += impl.rtvDescriptorSize;
					}
				}
			}

			if (framebuffer.depthTextureId != 0)
			{
				const auto textureIt = impl.textures.find(framebuffer.depthTextureId);
				if (textureIt != impl.textures.end() && textureIt->second.resource)
				{
					D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
					dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
					dsvHeapDesc.NumDescriptors = 1;
					dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
					impl.device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&framebuffer.dsvHeap));

					if (framebuffer.dsvHeap)
					{
						D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
						dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
						dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
						impl.device->CreateDepthStencilView(textureIt->second.resource.Get(), &dsvDesc, framebuffer.dsvHeap->GetCPUDescriptorHandleForHeapStart());
					}
				}
			}

			framebuffer.descriptorsDirty = false;
		}
	}
#endif

	DX12RenderDevice::DX12RenderDevice() : m_impl(std::make_unique<Impl>())
	{
	}

	DX12RenderDevice::~DX12RenderDevice()
	{
#if defined(_WIN32)
		DestroySwapchain();
		if (m_impl->fenceEvent != nullptr)
		{
			CloseHandle(m_impl->fenceEvent);
			m_impl->fenceEvent = nullptr;
		}
#endif
	}

	std::optional<NLS::Render::Data::PipelineState> DX12RenderDevice::Init(const NLS::Render::Settings::DriverSettings& settings)
	{
#if !defined(_WIN32)
		NLS_LOG_WARNING("DX12 backend is only supported on Windows. Falling back to stub mode.");
		return NLS::Render::Data::PipelineState{};
#else
		UINT factoryFlags = 0;
#if defined(_DEBUG)
		if (settings.debugMode)
		{
			Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
			if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
			{
				debugController->EnableDebugLayer();
				factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
			}
		}
#endif

		if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_impl->factory))))
		{
			NLS_LOG_ERROR("Failed to create DXGI factory.");
			return std::nullopt;
		}

		for (UINT adapterIndex = 0; ; ++adapterIndex)
		{
			Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
			if (m_impl->factory->EnumAdapters1(adapterIndex, &candidate) == DXGI_ERROR_NOT_FOUND)
				break;

			DXGI_ADAPTER_DESC1 adapterDesc{};
			candidate->GetDesc1(&adapterDesc);
			if ((adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
				continue;

			if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
			{
				m_impl->adapter = candidate;
				m_impl->hardware.assign(adapterDesc.Description, adapterDesc.Description + wcslen(adapterDesc.Description));
				break;
			}
		}

		if (!m_impl->adapter)
		{
			NLS_LOG_ERROR("Failed to find a suitable DX12 adapter.");
			return std::nullopt;
		}

		if (FAILED(D3D12CreateDevice(m_impl->adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_impl->device))))
		{
			NLS_LOG_ERROR("Failed to create DX12 device.");
			return std::nullopt;
		}

		const D3D12_COMMAND_QUEUE_DESC queueDesc{
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			0,
			D3D12_COMMAND_QUEUE_FLAG_NONE,
			0
		};

		if (FAILED(m_impl->device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_impl->graphicsQueue))))
		{
			NLS_LOG_ERROR("Failed to create DX12 graphics command queue.");
			return std::nullopt;
		}

		if (FAILED(m_impl->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_impl->fence))))
		{
			NLS_LOG_ERROR("Failed to create DX12 fence.");
			return std::nullopt;
		}

		m_impl->fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (m_impl->fenceEvent == nullptr)
		{
			NLS_LOG_ERROR("Failed to create DX12 fence event.");
			return std::nullopt;
		}

		m_impl->vendor = "DX12";
		m_impl->version = "FeatureLevel11_0+";
		m_impl->shadingLanguageVersion = "DXIL";
		m_impl->capabilities.backendReady = true;
		m_impl->capabilities.supportsGraphics = true;
		m_impl->capabilities.supportsCompute = true;
		m_impl->capabilities.supportsSwapchain = true;
		m_impl->capabilities.supportsFramebufferBlit = true;
		m_impl->capabilities.supportsDepthBlit = true;
		m_impl->capabilities.supportsCurrentSceneRenderer = true;
		m_impl->capabilities.supportsOffscreenFramebuffers = true;
		m_impl->capabilities.supportsFramebufferReadback = true;
		m_impl->capabilities.supportsUITextureHandles = true;
		m_impl->capabilities.supportsCubemaps = true;
		m_impl->capabilities.supportsMultiRenderTargets = true;
		m_impl->capabilities.maxTextureDimension2D = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
		m_impl->capabilities.maxColorAttachments = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT;
		m_impl->backendReady = true;

		NLS_LOG_INFO("Initialized DX12 backend on adapter: " + m_impl->hardware);
		return NLS::Render::Data::PipelineState{};
#endif
	}

	void DX12RenderDevice::Clear(bool colorBuffer, bool, bool)
	{
#if defined(_WIN32)
		if (!m_impl->commandList)
			return;

		if (!EnsureCommandListRecording(*m_impl))
			return;

		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{};
		D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};
		bool hasDepth = false;

		if (m_impl->boundFramebuffer != 0)
		{
			EnsureFramebufferDescriptors(*m_impl, m_impl->boundFramebuffer);
			auto framebufferIt = m_impl->framebuffers.find(m_impl->boundFramebuffer);
			if (framebufferIt == m_impl->framebuffers.end() || !framebufferIt->second.rtvHeap)
				return;

			rtvHandle = framebufferIt->second.rtvHeap->GetCPUDescriptorHandleForHeapStart();

			if (const auto textureIt = m_impl->textures.find(framebufferIt->second.colorTextureIds.empty() ? 0u : framebufferIt->second.colorTextureIds[0]);
				textureIt != m_impl->textures.end() && textureIt->second.resource)
			{
				TransitionResource(m_impl->commandList.Get(), textureIt->second.resource.Get(), textureIt->second.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
			}

			if (framebufferIt->second.depthTextureId != 0)
			{
				if (const auto depthIt = m_impl->textures.find(framebufferIt->second.depthTextureId);
					depthIt != m_impl->textures.end() && depthIt->second.resource && framebufferIt->second.dsvHeap)
				{
					TransitionResource(m_impl->commandList.Get(), depthIt->second.resource.Get(), depthIt->second.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
					dsvHandle = framebufferIt->second.dsvHeap->GetCPUDescriptorHandleForHeapStart();
					hasDepth = true;
				}
			}
		}
		else
		{
			if (!m_impl->swapchain || m_impl->frameContexts.empty())
				return;

			const UINT frameIndex = m_impl->swapchain->GetCurrentBackBufferIndex();
			auto& frame = m_impl->frameContexts[frameIndex];
			if (!frame.backBuffer)
				return;

			TransitionResource(m_impl->commandList.Get(), frame.backBuffer.Get(), frame.backBufferState, D3D12_RESOURCE_STATE_RENDER_TARGET);

			rtvHandle = m_impl->rtvHeap->GetCPUDescriptorHandleForHeapStart();
			rtvHandle.ptr += static_cast<SIZE_T>(frameIndex) * m_impl->rtvDescriptorSize;
			if (m_impl->dsvHeap)
			{
				dsvHandle = m_impl->dsvHeap->GetCPUDescriptorHandleForHeapStart();
				hasDepth = m_impl->depthBuffer != nullptr;
			}
		}

		m_impl->commandList->RSSetViewports(1, &m_impl->viewport);
		m_impl->commandList->RSSetScissorRects(1, &m_impl->scissorRect);
		m_impl->commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, hasDepth ? &dsvHandle : nullptr);
		if (colorBuffer)
			m_impl->commandList->ClearRenderTargetView(rtvHandle, m_impl->clearColor, 0, nullptr);
		if (hasDepth)
			m_impl->commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, m_impl->clearDepth, 0, 0, nullptr);
		m_impl->hasPendingCommands = true;
#endif
	}
	void DX12RenderDevice::ReadPixels(
		uint32_t x,
		uint32_t y,
		uint32_t width,
		uint32_t height,
		NLS::Render::Settings::EPixelDataFormat format,
		NLS::Render::Settings::EPixelDataType,
		void* data)
	{
#if defined(_WIN32)
		if (!data || width == 0 || height == 0 || !m_impl->device || !m_impl->graphicsQueue || !m_impl->commandList)
			return;

		ID3D12Resource* sourceResource = nullptr;
		D3D12_RESOURCE_STATES* sourceState = nullptr;
		NLS::Render::RHI::TextureDesc sourceDesc{};

		if (m_impl->boundFramebuffer != 0)
		{
			auto framebufferIt = m_impl->framebuffers.find(m_impl->boundFramebuffer);
			if (framebufferIt == m_impl->framebuffers.end() || framebufferIt->second.colorTextureIds.empty())
				return;

			auto textureIt = m_impl->textures.find(framebufferIt->second.colorTextureIds[0]);
			if (textureIt == m_impl->textures.end() || !textureIt->second.resource)
				return;

			sourceResource = textureIt->second.resource.Get();
			sourceState = &textureIt->second.state;
			sourceDesc = textureIt->second.desc;
		}
		else
		{
			if (!m_impl->swapchain || m_impl->frameContexts.empty())
				return;

			const UINT frameIndex = m_impl->swapchain->GetCurrentBackBufferIndex();
			auto& frame = m_impl->frameContexts[frameIndex];
			if (!frame.backBuffer)
				return;

			sourceResource = frame.backBuffer.Get();
			sourceState = &frame.backBufferState;
			sourceDesc.width = static_cast<uint16_t>(m_impl->swapchainWidth);
			sourceDesc.height = static_cast<uint16_t>(m_impl->swapchainHeight);
			sourceDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
		}

		if (!EnsureCommandListRecording(*m_impl))
			return;

		TransitionResource(m_impl->commandList.Get(), sourceResource, *sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);

		D3D12_RESOURCE_DESC resourceDesc = sourceResource->GetDesc();
		UINT64 totalBytes = 0;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
		UINT numRows = 0;
		UINT64 rowSizeInBytes = 0;
		m_impl->device->GetCopyableFootprints(&resourceDesc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);

		Microsoft::WRL::ComPtr<ID3D12Resource> readbackBuffer;
		const D3D12_HEAP_PROPERTIES heapProperties{
			D3D12_HEAP_TYPE_READBACK,
			D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
			D3D12_MEMORY_POOL_UNKNOWN,
			1,
			1
		};
		const D3D12_RESOURCE_DESC bufferDesc{
			D3D12_RESOURCE_DIMENSION_BUFFER,
			0,
			totalBytes,
			1,
			1,
			1,
			DXGI_FORMAT_UNKNOWN,
			{ 1, 0 },
			D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
			D3D12_RESOURCE_FLAG_NONE
		};

		if (FAILED(m_impl->device->CreateCommittedResource(
			&heapProperties,
			D3D12_HEAP_FLAG_NONE,
			&bufferDesc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(&readbackBuffer))))
		{
			return;
		}

		D3D12_TEXTURE_COPY_LOCATION srcLocation{};
		srcLocation.pResource = sourceResource;
		srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		srcLocation.SubresourceIndex = 0;

		D3D12_TEXTURE_COPY_LOCATION dstLocation{};
		dstLocation.pResource = readbackBuffer.Get();
		dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dstLocation.PlacedFootprint = footprint;

		m_impl->commandList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
		m_impl->commandList->Close();

		ID3D12CommandList* commandLists[] = { m_impl->commandList.Get() };
		m_impl->graphicsQueue->ExecuteCommandLists(1, commandLists);

		const UINT64 fenceValue = ++m_impl->fenceValue;
		m_impl->graphicsQueue->Signal(m_impl->fence.Get(), fenceValue);
		if (m_impl->fence->GetCompletedValue() < fenceValue)
		{
			m_impl->fence->SetEventOnCompletion(fenceValue, m_impl->fenceEvent);
			WaitForSingleObject(m_impl->fenceEvent, INFINITE);
		}

		const D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(totalBytes) };
		void* mappedData = nullptr;
		if (SUCCEEDED(readbackBuffer->Map(0, &readRange, &mappedData)))
		{
			const auto* bytes = static_cast<const uint8_t*>(mappedData);
			const auto sampleX = (std::min)(x, static_cast<uint32_t>(sourceDesc.width > 0 ? sourceDesc.width - 1 : 0));
			const auto sampleYFromBottom = (std::min)(y, static_cast<uint32_t>(sourceDesc.height > 0 ? sourceDesc.height - 1 : 0));
			const auto sampleY = sourceDesc.height > 0
				? static_cast<uint32_t>(sourceDesc.height - 1u) - sampleYFromBottom
				: 0u;
			const auto pixelOffset = static_cast<size_t>(sampleY) * footprint.Footprint.RowPitch + static_cast<size_t>(sampleX) * 4u;

			if (format == NLS::Render::Settings::EPixelDataFormat::RGB)
			{
				auto* output = static_cast<uint8_t*>(data);
				output[0] = bytes[pixelOffset + 0];
				output[1] = bytes[pixelOffset + 1];
				output[2] = bytes[pixelOffset + 2];
			}
			else
			{
				std::memcpy(data, bytes + pixelOffset, (std::min)(static_cast<size_t>(4), static_cast<size_t>(width * height * 4)));
			}

			readbackBuffer->Unmap(0, nullptr);
		}

		m_impl->isFrameRecording = false;
		m_impl->hasPendingCommands = false;
#else
		(void)x;
		(void)y;
		(void)width;
		(void)height;
		(void)format;
		(void)data;
#endif
	}
	void DX12RenderDevice::DrawElements(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t indexCount)
	{
#if defined(_WIN32)
		const uint32_t instanceCount = (std::max)(1u, m_impl->pendingInstanceCount);
		m_impl->pendingInstanceCount = 1u;
		if (ShouldLogDX12DrawDiagnostics())
			NLS_LOG_INFO("[DX12Draw] DrawElements requested, indexCount=" + std::to_string(indexCount) + ", instanceCount=" + std::to_string(instanceCount));
		const auto hasVertexDxil = std::any_of(m_impl->currentPipelineDesc.shaderStages.begin(), m_impl->currentPipelineDesc.shaderStages.end(), [](const NLS::Render::RHI::ShaderStageDesc& stage)
		{
			return stage.stage == NLS::Render::RHI::ShaderStage::Vertex &&
				stage.targetPlatform == NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL &&
				!stage.bytecode.empty();
		});
		const auto hasPixelDxil = std::any_of(m_impl->currentPipelineDesc.shaderStages.begin(), m_impl->currentPipelineDesc.shaderStages.end(), [](const NLS::Render::RHI::ShaderStageDesc& stage)
		{
			return stage.stage == NLS::Render::RHI::ShaderStage::Fragment &&
				stage.targetPlatform == NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL &&
				!stage.bytecode.empty();
		});
		if (!hasVertexDxil || !hasPixelDxil)
		{
			if (!m_impl->hasLoggedDrawStub)
			{
				NLS_LOG_WARNING("DX12 draw skipped because the current material does not provide complete DXIL shader stages yet.");
				m_impl->hasLoggedDrawStub = true;
			}
			return;
		}

		if (m_impl->boundBuffers.find(NLS::Render::RHI::BufferType::Vertex) == m_impl->boundBuffers.end() ||
			m_impl->boundBuffers.find(NLS::Render::RHI::BufferType::Index) == m_impl->boundBuffers.end())
		{
			if (!m_impl->hasLoggedDrawStub)
			{
				NLS_LOG_WARNING("DX12 draw skipped because vertex/index buffers are not both bound yet.");
				m_impl->hasLoggedDrawStub = true;
			}
			return;
		}

		if (!m_impl->isFrameRecording || !m_impl->commandList)
		{
			if (!m_impl->hasPendingCommands && EnsureCommandListRecording(*m_impl))
			{
				if (ShouldLogDX12DrawDiagnostics())
					NLS_LOG_INFO("[DX12Draw] DrawElements recovered by reopening the frame command list.");
			}
			else if (!m_impl->hasLoggedDrawStub)
			{
				NLS_LOG_WARNING("DX12 draw skipped because no active frame command list is recording.");
				m_impl->hasLoggedDrawStub = true;
			}
			if (!m_impl->isFrameRecording || !m_impl->commandList)
				return;
		}

		const auto vertexBufferId = m_impl->boundBuffers[NLS::Render::RHI::BufferType::Vertex];
		const auto indexBufferId = m_impl->boundBuffers[NLS::Render::RHI::BufferType::Index];
		const auto vertexBufferIt = m_impl->buffers.find(vertexBufferId);
		const auto indexBufferIt = m_impl->buffers.find(indexBufferId);
		if (vertexBufferIt == m_impl->buffers.end() || indexBufferIt == m_impl->buffers.end())
			return;

		const auto pipelineKey = BuildPipelineCacheKey(m_impl->currentPipelineDesc);
		auto pipelineIt = m_impl->pipelineCache.find(pipelineKey);
		if (pipelineIt == m_impl->pipelineCache.end())
		{
			const auto vertexStage = std::find_if(m_impl->currentPipelineDesc.shaderStages.begin(), m_impl->currentPipelineDesc.shaderStages.end(), [](const ShaderStageDesc& stage)
			{
				return stage.stage == NLS::Render::RHI::ShaderStage::Vertex &&
					stage.targetPlatform == NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL;
			});
			const auto pixelStage = std::find_if(m_impl->currentPipelineDesc.shaderStages.begin(), m_impl->currentPipelineDesc.shaderStages.end(), [](const ShaderStageDesc& stage)
			{
				return stage.stage == NLS::Render::RHI::ShaderStage::Fragment &&
					stage.targetPlatform == NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL;
			});
			if (vertexStage == m_impl->currentPipelineDesc.shaderStages.end() || pixelStage == m_impl->currentPipelineDesc.shaderStages.end())
				return;

			PipelineCacheEntry pipelineEntry{};
			std::vector<D3D12_ROOT_PARAMETER> rootParameters;
			std::vector<std::vector<D3D12_DESCRIPTOR_RANGE>> descriptorRangesByTable;
			std::vector<D3D12_STATIC_SAMPLER_DESC> staticSamplers;
			rootParameters.reserve(m_impl->currentPipelineDesc.reflection ? m_impl->currentPipelineDesc.reflection->constantBuffers.size() + 4u : 4u);

			if (m_impl->currentPipelineDesc.reflection != nullptr)
			{
				auto constantBuffers = m_impl->currentPipelineDesc.reflection->constantBuffers;
				std::sort(constantBuffers.begin(), constantBuffers.end(), [](const auto& lhs, const auto& rhs)
				{
					return lhs.bindingSpace == rhs.bindingSpace
						? lhs.bindingIndex < rhs.bindingIndex
						: lhs.bindingSpace < rhs.bindingSpace;
				});

				for (const auto& constantBuffer : constantBuffers)
				{
					D3D12_ROOT_PARAMETER parameter{};
					parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
					parameter.Descriptor.ShaderRegister = constantBuffer.bindingIndex;
					parameter.Descriptor.RegisterSpace = constantBuffer.bindingSpace;
					parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

					pipelineEntry.constantBuffers.push_back({
						constantBuffer.bindingSpace,
						constantBuffer.bindingIndex,
						static_cast<UINT>(rootParameters.size())
					});
					rootParameters.push_back(parameter);
				}

				std::unordered_map<uint32_t, std::vector<NLS::Render::Resources::ShaderPropertyDesc>> texturesBySpace;
				for (const auto& property : m_impl->currentPipelineDesc.reflection->properties)
				{
					if (property.kind == NLS::Render::Resources::ShaderResourceKind::SampledTexture)
						texturesBySpace[property.bindingSpace].push_back(property);
					else if (property.kind == NLS::Render::Resources::ShaderResourceKind::Sampler)
					{
						D3D12_STATIC_SAMPLER_DESC staticSampler{};
						staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
						staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
						staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
						staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
						staticSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
						staticSampler.MaxLOD = D3D12_FLOAT32_MAX;
						staticSampler.ShaderRegister = property.bindingIndex;
						staticSampler.RegisterSpace = property.bindingSpace;
						staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
						staticSamplers.push_back(staticSampler);
					}
				}

				UINT descriptorHeapOffset = 0;
				std::vector<uint32_t> sortedTextureSpaces;
				sortedTextureSpaces.reserve(texturesBySpace.size());
				for (const auto& [space, _] : texturesBySpace)
					sortedTextureSpaces.push_back(space);
				std::sort(sortedTextureSpaces.begin(), sortedTextureSpaces.end());

				for (const auto space : sortedTextureSpaces)
				{
					auto& properties = texturesBySpace[space];
					std::sort(properties.begin(), properties.end(), [](const auto& lhs, const auto& rhs)
					{
						return lhs.bindingIndex < rhs.bindingIndex;
					});

					descriptorRangesByTable.emplace_back();
					auto& tableRanges = descriptorRangesByTable.back();
					tableRanges.reserve(properties.size());
					const auto tableStartOffset = descriptorHeapOffset;

					for (const auto& property : properties)
					{
						D3D12_DESCRIPTOR_RANGE range{};
						range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
						range.NumDescriptors = (std::max)(1, property.arraySize);
						range.BaseShaderRegister = property.bindingIndex;
						range.RegisterSpace = property.bindingSpace;
						range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
						tableRanges.push_back(range);

						pipelineEntry.sampledTextures.push_back({
							property.name,
							property.bindingSpace,
							property.bindingIndex,
							descriptorHeapOffset,
							DXGI_FORMAT_R8G8B8A8_UNORM,
							property.type == NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_CUBE
								? NLS::Render::RHI::TextureDimension::TextureCube
								: NLS::Render::RHI::TextureDimension::Texture2D
						});
						descriptorHeapOffset += range.NumDescriptors;
					}

					D3D12_ROOT_PARAMETER parameter{};
					parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
					parameter.DescriptorTable.NumDescriptorRanges = static_cast<UINT>(tableRanges.size());
					parameter.DescriptorTable.pDescriptorRanges = tableRanges.data();
					parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

					pipelineEntry.descriptorTables.push_back({
						NLS::Render::Resources::ShaderResourceKind::SampledTexture,
						space,
						static_cast<UINT>(rootParameters.size()),
						tableStartOffset
					});
					rootParameters.push_back(parameter);
				}
			}

			D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
			rootSignatureDesc.NumParameters = static_cast<UINT>(rootParameters.size());
			rootSignatureDesc.pParameters = rootParameters.empty() ? nullptr : rootParameters.data();
			rootSignatureDesc.NumStaticSamplers = static_cast<UINT>(staticSamplers.size());
			rootSignatureDesc.pStaticSamplers = staticSamplers.empty() ? nullptr : staticSamplers.data();
			rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

			Microsoft::WRL::ComPtr<ID3DBlob> serializedRootSignature;
			Microsoft::WRL::ComPtr<ID3DBlob> rootSignatureError;
			if (FAILED(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serializedRootSignature, &rootSignatureError)))
			{
				if (rootSignatureError)
					NLS_LOG_ERROR("DX12 root signature serialization failed: " + std::string(static_cast<const char*>(rootSignatureError->GetBufferPointer()), rootSignatureError->GetBufferSize()));
				return;
			}

			if (FAILED(m_impl->device->CreateRootSignature(
				0,
				serializedRootSignature->GetBufferPointer(),
				serializedRootSignature->GetBufferSize(),
				IID_PPV_ARGS(&pipelineEntry.rootSignature))))
			{
				NLS_LOG_ERROR("Failed to create DX12 root signature.");
				return;
			}

			static const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
			};

			D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
			psoDesc.pRootSignature = pipelineEntry.rootSignature.Get();
			psoDesc.VS = { vertexStage->bytecode.data(), vertexStage->bytecode.size() };
			psoDesc.PS = { pixelStage->bytecode.data(), pixelStage->bytecode.size() };
			D3D12_BLEND_DESC blendDesc{};
			blendDesc.AlphaToCoverageEnable = FALSE;
			blendDesc.IndependentBlendEnable = FALSE;
			D3D12_RENDER_TARGET_BLEND_DESC renderTargetBlendDesc{};
			renderTargetBlendDesc.BlendEnable = m_impl->currentPipelineDesc.blendState.enabled;
			renderTargetBlendDesc.LogicOpEnable = FALSE;
			renderTargetBlendDesc.SrcBlend = m_impl->currentPipelineDesc.blendState.enabled
				? D3D12_BLEND_SRC_ALPHA
				: D3D12_BLEND_ONE;
			renderTargetBlendDesc.DestBlend = m_impl->currentPipelineDesc.blendState.enabled
				? D3D12_BLEND_INV_SRC_ALPHA
				: D3D12_BLEND_ZERO;
			renderTargetBlendDesc.BlendOp = D3D12_BLEND_OP_ADD;
			renderTargetBlendDesc.SrcBlendAlpha = m_impl->currentPipelineDesc.blendState.enabled
				? D3D12_BLEND_ONE
				: D3D12_BLEND_ONE;
			renderTargetBlendDesc.DestBlendAlpha = m_impl->currentPipelineDesc.blendState.enabled
				? D3D12_BLEND_INV_SRC_ALPHA
				: D3D12_BLEND_ZERO;
			renderTargetBlendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			renderTargetBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
			renderTargetBlendDesc.RenderTargetWriteMask = m_impl->currentPipelineDesc.blendState.colorWrite ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;
			for (auto& renderTarget : blendDesc.RenderTarget)
				renderTarget = renderTargetBlendDesc;
			psoDesc.BlendState = blendDesc;
			psoDesc.SampleMask = UINT_MAX;
			D3D12_RASTERIZER_DESC rasterizerDesc{};
			rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
			rasterizerDesc.CullMode = m_impl->currentPipelineDesc.rasterState.culling
				? ToD3DCullMode(m_impl->currentPipelineDesc.rasterState.cullFace)
				: D3D12_CULL_MODE_NONE;
			rasterizerDesc.FrontCounterClockwise = TRUE;
			rasterizerDesc.DepthClipEnable = TRUE;
			rasterizerDesc.MultisampleEnable = FALSE;
			rasterizerDesc.AntialiasedLineEnable = FALSE;
			rasterizerDesc.ForcedSampleCount = 0;
			rasterizerDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
			psoDesc.RasterizerState = rasterizerDesc;
			D3D12_DEPTH_STENCIL_DESC depthStencilDesc{};
			depthStencilDesc.DepthEnable = m_impl->currentPipelineDesc.depthStencilState.depthTest;
			depthStencilDesc.DepthWriteMask = m_impl->currentPipelineDesc.depthStencilState.depthWrite
				? D3D12_DEPTH_WRITE_MASK_ALL
				: D3D12_DEPTH_WRITE_MASK_ZERO;
			depthStencilDesc.DepthFunc = ToD3DComparisonFunc(m_impl->currentPipelineDesc.depthStencilState.depthCompare);
			depthStencilDesc.StencilEnable = FALSE;
			depthStencilDesc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
			depthStencilDesc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
			depthStencilDesc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
			depthStencilDesc.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
			psoDesc.DepthStencilState = depthStencilDesc;
			psoDesc.InputLayout = { inputLayout, static_cast<UINT>(_countof(inputLayout)) };
			psoDesc.PrimitiveTopologyType = ToD3DPrimitiveTopologyType(m_impl->currentPipelineDesc.primitiveMode);
			psoDesc.NumRenderTargets = static_cast<UINT>((std::min)(
				m_impl->currentPipelineDesc.attachmentLayout.colorAttachmentFormats.size(),
				static_cast<size_t>(D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)));
			for (UINT renderTargetIndex = 0; renderTargetIndex < psoDesc.NumRenderTargets; ++renderTargetIndex)
			{
				psoDesc.RTVFormats[renderTargetIndex] = ToDxgiAttachmentFormat(
					m_impl->currentPipelineDesc.attachmentLayout.colorAttachmentFormats[renderTargetIndex]);
			}
			psoDesc.DSVFormat = m_impl->currentPipelineDesc.attachmentLayout.hasDepthAttachment
				? ToDxgiAttachmentFormat(m_impl->currentPipelineDesc.attachmentLayout.depthAttachmentFormat)
				: DXGI_FORMAT_UNKNOWN;
			psoDesc.SampleDesc.Count = m_impl->currentPipelineDesc.attachmentLayout.sampleCount;

			const HRESULT createPsoHr = m_impl->device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineEntry.pipelineState));
			if (FAILED(createPsoHr))
			{
				NLS_LOG_ERROR("Failed to create DX12 graphics pipeline state: " + FormatHRESULT(createPsoHr));
				NLS_LOG_WARNING(
					"DX12 pipeline summary: RTCount=" + std::to_string(psoDesc.NumRenderTargets) +
					", HasDepth=" + std::to_string(psoDesc.DSVFormat != DXGI_FORMAT_UNKNOWN ? 1 : 0) +
					", PrimitiveTopologyType=" + std::to_string(static_cast<int>(psoDesc.PrimitiveTopologyType)) +
					", SampleCount=" + std::to_string(psoDesc.SampleDesc.Count));
				LogDX12InfoQueueMessages(m_impl->device.Get(), "DX12 pipeline creation");
				return;
			}

			pipelineIt = m_impl->pipelineCache.emplace(pipelineKey, std::move(pipelineEntry)).first;
		}

		std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvHandles;
		D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};
		if (m_impl->boundFramebuffer != 0)
		{
			EnsureFramebufferDescriptors(*m_impl, m_impl->boundFramebuffer);
			auto framebufferIt = m_impl->framebuffers.find(m_impl->boundFramebuffer);
			if (framebufferIt == m_impl->framebuffers.end() || !framebufferIt->second.rtvHeap)
				return;
			const auto renderTargetCount = framebufferIt->second.drawBufferCount > 0
				? framebufferIt->second.drawBufferCount
				: static_cast<uint32_t>(framebufferIt->second.colorTextureIds.size());
			rtvHandles.reserve(renderTargetCount);
			auto rtvHandle = framebufferIt->second.rtvHeap->GetCPUDescriptorHandleForHeapStart();
			for (uint32_t renderTargetIndex = 0; renderTargetIndex < renderTargetCount; ++renderTargetIndex)
			{
				rtvHandles.push_back(rtvHandle);
				rtvHandle.ptr += m_impl->rtvDescriptorSize;
			}
			if (framebufferIt->second.dsvHeap)
				dsvHandle = framebufferIt->second.dsvHeap->GetCPUDescriptorHandleForHeapStart();
		}
		else
		{
			const UINT frameIndex = m_impl->swapchain->GetCurrentBackBufferIndex();
			auto rtvHandle = m_impl->rtvHeap->GetCPUDescriptorHandleForHeapStart();
			rtvHandle.ptr += static_cast<SIZE_T>(frameIndex) * m_impl->rtvDescriptorSize;
			rtvHandles.push_back(rtvHandle);
			dsvHandle = m_impl->dsvHeap->GetCPUDescriptorHandleForHeapStart();
		}

		m_impl->commandList->SetGraphicsRootSignature(pipelineIt->second.rootSignature.Get());
		m_impl->commandList->SetPipelineState(pipelineIt->second.pipelineState.Get());
		ID3D12DescriptorHeap* descriptorHeaps[] = { m_impl->srvHeap.Get() };
		m_impl->commandList->SetDescriptorHeaps(1, descriptorHeaps);
		m_impl->commandList->RSSetViewports(1, &m_impl->viewport);
		m_impl->commandList->RSSetScissorRects(1, &m_impl->scissorRect);
		m_impl->commandList->OMSetRenderTargets(
			static_cast<UINT>(rtvHandles.size()),
			rtvHandles.data(),
			FALSE,
			dsvHandle.ptr != 0 ? &dsvHandle : nullptr);
		m_impl->commandList->IASetPrimitiveTopology(ToD3DPrimitiveTopology(primitiveMode));

		D3D12_VERTEX_BUFFER_VIEW vbView{};
		vbView.BufferLocation = vertexBufferIt->second.resource->GetGPUVirtualAddress();
		vbView.SizeInBytes = static_cast<UINT>(vertexBufferIt->second.size);
		vbView.StrideInBytes = sizeof(float) * 14;
		m_impl->commandList->IASetVertexBuffers(0, 1, &vbView);

		D3D12_INDEX_BUFFER_VIEW ibView{};
		ibView.BufferLocation = indexBufferIt->second.resource->GetGPUVirtualAddress();
		ibView.SizeInBytes = static_cast<UINT>(indexBufferIt->second.size);
		ibView.Format = DXGI_FORMAT_R32_UINT;
		m_impl->commandList->IASetIndexBuffer(&ibView);

		for (const auto& constantBufferBinding : pipelineIt->second.constantBuffers)
		{
			const auto bufferId = ResolveUniformBufferId(
				*m_impl,
				m_impl->currentBindingSet,
				constantBufferBinding.bindingSpace,
				constantBufferBinding.bindingIndex);
			if (bufferId != 0u)
			{
				if (const auto bufferIt = m_impl->buffers.find(bufferId); bufferIt != m_impl->buffers.end() && bufferIt->second.resource)
				{
					m_impl->commandList->SetGraphicsRootConstantBufferView(
						constantBufferBinding.rootParameterIndex,
						bufferIt->second.resource->GetGPUVirtualAddress());
				}
			}
		}

		TransitionBindingSetTexturesToShaderRead(*m_impl, m_impl->commandList.Get(), pipelineIt->second);

		if (m_impl->currentBindingSet != nullptr)
		{
			for (const auto& textureBinding : pipelineIt->second.sampledTextures)
			{
				D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_impl->srvHeap->GetCPUDescriptorHandleForHeapStart();
				cpuHandle.ptr += static_cast<SIZE_T>(textureBinding.descriptorHeapOffset) * m_impl->srvDescriptorSize;

				const auto* entry = m_impl->currentBindingSet->Find(textureBinding.name);
				if (entry != nullptr && entry->textureResource != nullptr)
				{
					const auto textureIt = m_impl->textures.find(entry->textureResource->GetResourceId());
					if (textureIt != m_impl->textures.end() && textureIt->second.resource)
					{
						D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
						FillTextureSrvDesc(srvDesc, entry->textureResource->GetDesc().format, entry->textureResource->GetDimension());
						m_impl->device->CreateShaderResourceView(textureIt->second.resource.Get(), &srvDesc, cpuHandle);
					}
					else
					{
						D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc{};
						FillTextureSrvDesc(nullSrvDesc, NLS::Render::RHI::TextureFormat::RGBA8, textureBinding.defaultDimension);
						m_impl->device->CreateShaderResourceView(nullptr, &nullSrvDesc, cpuHandle);
					}
				}
				else
				{
					D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc{};
					FillTextureSrvDesc(nullSrvDesc, NLS::Render::RHI::TextureFormat::RGBA8, textureBinding.defaultDimension);
					m_impl->device->CreateShaderResourceView(nullptr, &nullSrvDesc, cpuHandle);
				}
			}
		}

		for (const auto& descriptorTable : pipelineIt->second.descriptorTables)
		{
			D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = m_impl->srvHeap->GetGPUDescriptorHandleForHeapStart();
			gpuHandle.ptr += static_cast<SIZE_T>(descriptorTable.descriptorHeapOffset) * m_impl->srvDescriptorSize;
			m_impl->commandList->SetGraphicsRootDescriptorTable(descriptorTable.rootParameterIndex, gpuHandle);
		}
		m_impl->commandList->DrawIndexedInstanced(indexCount, instanceCount, 0, 0, 0);
		m_impl->hasPendingCommands = true;
#endif
	}
	void DX12RenderDevice::DrawElementsInstanced(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t indexCount, uint32_t instances)
	{
		m_impl->pendingInstanceCount = instances;
		DrawElements(primitiveMode, indexCount);
	}
	void DX12RenderDevice::DrawArrays(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t vertexCount)
	{
#if defined(_WIN32)
		const uint32_t instanceCount = (std::max)(1u, m_impl->pendingInstanceCount);
		m_impl->pendingInstanceCount = 1u;
		if (ShouldLogDX12DrawDiagnostics())
			NLS_LOG_INFO("[DX12Draw] DrawArrays requested, vertexCount=" + std::to_string(vertexCount) + ", instanceCount=" + std::to_string(instanceCount));
		const auto hasVertexDxil = std::any_of(m_impl->currentPipelineDesc.shaderStages.begin(), m_impl->currentPipelineDesc.shaderStages.end(), [](const NLS::Render::RHI::ShaderStageDesc& stage)
		{
			return stage.stage == NLS::Render::RHI::ShaderStage::Vertex &&
				stage.targetPlatform == NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL &&
				!stage.bytecode.empty();
		});
		const auto hasPixelDxil = std::any_of(m_impl->currentPipelineDesc.shaderStages.begin(), m_impl->currentPipelineDesc.shaderStages.end(), [](const NLS::Render::RHI::ShaderStageDesc& stage)
		{
			return stage.stage == NLS::Render::RHI::ShaderStage::Fragment &&
				stage.targetPlatform == NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL &&
				!stage.bytecode.empty();
		});
		if (!hasVertexDxil || !hasPixelDxil)
		{
			if (!m_impl->hasLoggedDrawStub)
			{
				NLS_LOG_WARNING("DX12 draw skipped because the current material does not provide complete DXIL shader stages yet.");
				m_impl->hasLoggedDrawStub = true;
			}
			return;
		}

		if (m_impl->boundBuffers.find(NLS::Render::RHI::BufferType::Vertex) == m_impl->boundBuffers.end())
		{
			if (!m_impl->hasLoggedDrawStub)
			{
				NLS_LOG_WARNING("DX12 draw skipped because no vertex buffer is bound yet.");
				m_impl->hasLoggedDrawStub = true;
			}
			return;
		}

		if (!m_impl->isFrameRecording || !m_impl->commandList)
		{
			if (!m_impl->hasPendingCommands && EnsureCommandListRecording(*m_impl))
			{
				if (ShouldLogDX12DrawDiagnostics())
					NLS_LOG_INFO("[DX12Draw] DrawArrays recovered by reopening the frame command list.");
			}
			else if (!m_impl->hasLoggedDrawStub)
			{
				NLS_LOG_WARNING("DX12 draw skipped because no active frame command list is recording.");
				m_impl->hasLoggedDrawStub = true;
			}
			if (!m_impl->isFrameRecording || !m_impl->commandList)
				return;
		}

		const auto vertexBufferId = m_impl->boundBuffers[NLS::Render::RHI::BufferType::Vertex];
		const auto vertexBufferIt = m_impl->buffers.find(vertexBufferId);
		if (vertexBufferIt == m_impl->buffers.end())
			return;

		const auto pipelineKey = BuildPipelineCacheKey(m_impl->currentPipelineDesc);
		const auto pipelineIt = m_impl->pipelineCache.find(pipelineKey);
		if (pipelineIt == m_impl->pipelineCache.end())
			return;

		std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvHandles;
		D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};
		if (m_impl->boundFramebuffer != 0)
		{
			EnsureFramebufferDescriptors(*m_impl, m_impl->boundFramebuffer);
			auto framebufferIt = m_impl->framebuffers.find(m_impl->boundFramebuffer);
			if (framebufferIt == m_impl->framebuffers.end() || !framebufferIt->second.rtvHeap)
				return;
			const auto renderTargetCount = framebufferIt->second.drawBufferCount > 0
				? framebufferIt->second.drawBufferCount
				: static_cast<uint32_t>(framebufferIt->second.colorTextureIds.size());
			rtvHandles.reserve(renderTargetCount);
			auto rtvHandle = framebufferIt->second.rtvHeap->GetCPUDescriptorHandleForHeapStart();
			for (uint32_t renderTargetIndex = 0; renderTargetIndex < renderTargetCount; ++renderTargetIndex)
			{
				rtvHandles.push_back(rtvHandle);
				rtvHandle.ptr += m_impl->rtvDescriptorSize;
			}
			if (framebufferIt->second.dsvHeap)
				dsvHandle = framebufferIt->second.dsvHeap->GetCPUDescriptorHandleForHeapStart();
		}
		else
		{
			const UINT frameIndex = m_impl->swapchain->GetCurrentBackBufferIndex();
			auto rtvHandle = m_impl->rtvHeap->GetCPUDescriptorHandleForHeapStart();
			rtvHandle.ptr += static_cast<SIZE_T>(frameIndex) * m_impl->rtvDescriptorSize;
			rtvHandles.push_back(rtvHandle);
			dsvHandle = m_impl->dsvHeap->GetCPUDescriptorHandleForHeapStart();
		}

		m_impl->commandList->SetGraphicsRootSignature(pipelineIt->second.rootSignature.Get());
		m_impl->commandList->SetPipelineState(pipelineIt->second.pipelineState.Get());
		ID3D12DescriptorHeap* descriptorHeaps[] = { m_impl->srvHeap.Get() };
		m_impl->commandList->SetDescriptorHeaps(1, descriptorHeaps);
		m_impl->commandList->RSSetViewports(1, &m_impl->viewport);
		m_impl->commandList->RSSetScissorRects(1, &m_impl->scissorRect);
		m_impl->commandList->OMSetRenderTargets(
			static_cast<UINT>(rtvHandles.size()),
			rtvHandles.data(),
			FALSE,
			dsvHandle.ptr != 0 ? &dsvHandle : nullptr);
		m_impl->commandList->IASetPrimitiveTopology(ToD3DPrimitiveTopology(primitiveMode));

		D3D12_VERTEX_BUFFER_VIEW vbView{};
		vbView.BufferLocation = vertexBufferIt->second.resource->GetGPUVirtualAddress();
		vbView.SizeInBytes = static_cast<UINT>(vertexBufferIt->second.size);
		vbView.StrideInBytes = sizeof(float) * 14;
		m_impl->commandList->IASetVertexBuffers(0, 1, &vbView);
		for (const auto& constantBufferBinding : pipelineIt->second.constantBuffers)
		{
			const auto bufferId = ResolveUniformBufferId(
				*m_impl,
				m_impl->currentBindingSet,
				constantBufferBinding.bindingSpace,
				constantBufferBinding.bindingIndex);
			if (bufferId != 0u)
			{
				if (const auto bufferIt = m_impl->buffers.find(bufferId); bufferIt != m_impl->buffers.end() && bufferIt->second.resource)
				{
					m_impl->commandList->SetGraphicsRootConstantBufferView(
						constantBufferBinding.rootParameterIndex,
						bufferIt->second.resource->GetGPUVirtualAddress());
				}
			}
		}

		TransitionBindingSetTexturesToShaderRead(*m_impl, m_impl->commandList.Get(), pipelineIt->second);

		if (m_impl->currentBindingSet != nullptr)
		{
			for (const auto& textureBinding : pipelineIt->second.sampledTextures)
			{
				D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_impl->srvHeap->GetCPUDescriptorHandleForHeapStart();
				cpuHandle.ptr += static_cast<SIZE_T>(textureBinding.descriptorHeapOffset) * m_impl->srvDescriptorSize;

				const auto* entry = m_impl->currentBindingSet->Find(textureBinding.name);
				if (entry != nullptr && entry->textureResource != nullptr)
				{
					const auto textureIt = m_impl->textures.find(entry->textureResource->GetResourceId());
					if (textureIt != m_impl->textures.end() && textureIt->second.resource)
					{
						D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
						FillTextureSrvDesc(srvDesc, entry->textureResource->GetDesc().format, entry->textureResource->GetDimension());
						m_impl->device->CreateShaderResourceView(textureIt->second.resource.Get(), &srvDesc, cpuHandle);
					}
					else
					{
						D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc{};
						FillTextureSrvDesc(nullSrvDesc, NLS::Render::RHI::TextureFormat::RGBA8, textureBinding.defaultDimension);
						m_impl->device->CreateShaderResourceView(nullptr, &nullSrvDesc, cpuHandle);
					}
				}
				else
				{
					D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc{};
					FillTextureSrvDesc(nullSrvDesc, NLS::Render::RHI::TextureFormat::RGBA8, textureBinding.defaultDimension);
					m_impl->device->CreateShaderResourceView(nullptr, &nullSrvDesc, cpuHandle);
				}
			}
		}

		for (const auto& descriptorTable : pipelineIt->second.descriptorTables)
		{
			D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = m_impl->srvHeap->GetGPUDescriptorHandleForHeapStart();
			gpuHandle.ptr += static_cast<SIZE_T>(descriptorTable.descriptorHeapOffset) * m_impl->srvDescriptorSize;
			m_impl->commandList->SetGraphicsRootDescriptorTable(descriptorTable.rootParameterIndex, gpuHandle);
		}
		m_impl->commandList->DrawInstanced(vertexCount, instanceCount, 0, 0);
		m_impl->hasPendingCommands = true;
#endif
	}
	void DX12RenderDevice::DrawArraysInstanced(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t vertexCount, uint32_t instances)
	{
		m_impl->pendingInstanceCount = instances;
		DrawArrays(primitiveMode, vertexCount);
	}
	void DX12RenderDevice::SetClearColor(float red, float green, float blue, float alpha)
	{
#if defined(_WIN32)
		m_impl->clearColor[0] = red;
		m_impl->clearColor[1] = green;
		m_impl->clearColor[2] = blue;
		m_impl->clearColor[3] = alpha;
#endif
	}
	void DX12RenderDevice::SetRasterizationLinesWidth(float) {}
	void DX12RenderDevice::SetRasterizationMode(NLS::Render::Settings::ERasterizationMode) {}
	void DX12RenderDevice::SetCapability(NLS::Render::Settings::ERenderingCapability, bool) {}
	bool DX12RenderDevice::GetCapability(NLS::Render::Settings::ERenderingCapability) { return false; }
	void DX12RenderDevice::SetStencilAlgorithm(NLS::Render::Settings::EComparaisonAlgorithm, int32_t, uint32_t) {}
	void DX12RenderDevice::SetDepthAlgorithm(NLS::Render::Settings::EComparaisonAlgorithm) {}
	void DX12RenderDevice::SetStencilMask(uint32_t) {}
	void DX12RenderDevice::SetStencilOperations(NLS::Render::Settings::EOperation, NLS::Render::Settings::EOperation, NLS::Render::Settings::EOperation) {}
	void DX12RenderDevice::SetCullFace(NLS::Render::Settings::ECullFace) {}
	void DX12RenderDevice::SetDepthWriting(bool) {}
	void DX12RenderDevice::SetColorWriting(bool, bool, bool, bool) {}
	void DX12RenderDevice::SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
	{
#if defined(_WIN32)
		m_impl->viewport = D3D12_VIEWPORT{
			static_cast<float>(x),
			static_cast<float>(y),
			static_cast<float>(width),
			static_cast<float>(height),
			0.0f,
			1.0f
		};
		m_impl->scissorRect = D3D12_RECT{
			static_cast<LONG>(x),
			static_cast<LONG>(y),
			static_cast<LONG>(x + width),
			static_cast<LONG>(y + height)
		};
#endif
	}
	void DX12RenderDevice::BindGraphicsPipeline(const NLS::Render::RHI::GraphicsPipelineDesc& pipelineDesc, const NLS::Render::Resources::BindingSetInstance* bindingSet)
	{
#if defined(_WIN32)
		auto resolvedDesc = pipelineDesc;
		resolvedDesc.attachmentLayout.colorAttachmentFormats.clear();
		resolvedDesc.attachmentLayout.sampleCount = 1;

		if (m_impl->boundFramebuffer != 0)
		{
			if (const auto framebufferIt = m_impl->framebuffers.find(m_impl->boundFramebuffer); framebufferIt != m_impl->framebuffers.end())
			{
				for (const auto colorTextureId : framebufferIt->second.colorTextureIds)
				{
					if (const auto textureIt = m_impl->textures.find(colorTextureId); textureIt != m_impl->textures.end())
						resolvedDesc.attachmentLayout.colorAttachmentFormats.push_back(textureIt->second.desc.format);
				}

				resolvedDesc.attachmentLayout.hasDepthAttachment = framebufferIt->second.depthTextureId != 0;
				if (resolvedDesc.attachmentLayout.hasDepthAttachment)
				{
					if (const auto depthTextureIt = m_impl->textures.find(framebufferIt->second.depthTextureId); depthTextureIt != m_impl->textures.end())
						resolvedDesc.attachmentLayout.depthAttachmentFormat = depthTextureIt->second.desc.format;
				}
			}
		}

		if (resolvedDesc.attachmentLayout.colorAttachmentFormats.empty())
			resolvedDesc.attachmentLayout.colorAttachmentFormats = { NLS::Render::RHI::TextureFormat::RGBA8 };

		m_impl->currentPipelineDesc = resolvedDesc;
		m_impl->currentBindingSet = bindingSet;
		m_impl->hasLoggedDrawStub = false;

		if (!m_impl->device || resolvedDesc.reflection == nullptr)
			return;

		const auto pipelineKey = BuildPipelineCacheKey(resolvedDesc);
		if (m_impl->pipelineCache.find(pipelineKey) != m_impl->pipelineCache.end())
			return;

		const auto vertexStage = std::find_if(resolvedDesc.shaderStages.begin(), resolvedDesc.shaderStages.end(), [](const ShaderStageDesc& stage)
		{
			return stage.stage == NLS::Render::RHI::ShaderStage::Vertex &&
				stage.targetPlatform == NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL;
		});
		const auto pixelStage = std::find_if(resolvedDesc.shaderStages.begin(), resolvedDesc.shaderStages.end(), [](const ShaderStageDesc& stage)
		{
			return stage.stage == NLS::Render::RHI::ShaderStage::Fragment &&
				stage.targetPlatform == NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL;
		});
		if (vertexStage == resolvedDesc.shaderStages.end() || pixelStage == resolvedDesc.shaderStages.end())
			return;

		PipelineCacheEntry pipelineEntry{};
		std::vector<D3D12_ROOT_PARAMETER> rootParameters;
		std::vector<std::vector<D3D12_DESCRIPTOR_RANGE>> descriptorRangesByTable;
		std::vector<D3D12_STATIC_SAMPLER_DESC> staticSamplers;
		rootParameters.reserve(resolvedDesc.reflection ? resolvedDesc.reflection->constantBuffers.size() + 4u : 4u);

		if (resolvedDesc.reflection != nullptr)
		{
			auto constantBuffers = resolvedDesc.reflection->constantBuffers;
			std::sort(constantBuffers.begin(), constantBuffers.end(), [](const auto& lhs, const auto& rhs)
			{
				return lhs.bindingSpace == rhs.bindingSpace
					? lhs.bindingIndex < rhs.bindingIndex
					: lhs.bindingSpace < rhs.bindingSpace;
			});

			for (const auto& constantBuffer : constantBuffers)
			{
				D3D12_ROOT_PARAMETER parameter{};
				parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
				parameter.Descriptor.ShaderRegister = constantBuffer.bindingIndex;
				parameter.Descriptor.RegisterSpace = constantBuffer.bindingSpace;
				parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

				pipelineEntry.constantBuffers.push_back({
					constantBuffer.bindingSpace,
					constantBuffer.bindingIndex,
					static_cast<UINT>(rootParameters.size())
				});
				rootParameters.push_back(parameter);
			}

			std::unordered_map<uint32_t, std::vector<NLS::Render::Resources::ShaderPropertyDesc>> texturesBySpace;
			for (const auto& property : resolvedDesc.reflection->properties)
			{
				if (property.kind == NLS::Render::Resources::ShaderResourceKind::SampledTexture)
					texturesBySpace[property.bindingSpace].push_back(property);
				else if (property.kind == NLS::Render::Resources::ShaderResourceKind::Sampler)
				{
					D3D12_STATIC_SAMPLER_DESC staticSampler{};
					staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
					staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
					staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
					staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
					staticSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
					staticSampler.MaxLOD = D3D12_FLOAT32_MAX;
					staticSampler.ShaderRegister = property.bindingIndex;
					staticSampler.RegisterSpace = property.bindingSpace;
					staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
					staticSamplers.push_back(staticSampler);
				}
			}

			UINT descriptorHeapOffset = 0;
			std::vector<uint32_t> sortedTextureSpaces;
			sortedTextureSpaces.reserve(texturesBySpace.size());
			for (const auto& [space, _] : texturesBySpace)
				sortedTextureSpaces.push_back(space);
			std::sort(sortedTextureSpaces.begin(), sortedTextureSpaces.end());

			for (const auto space : sortedTextureSpaces)
			{
				auto& properties = texturesBySpace[space];
				std::sort(properties.begin(), properties.end(), [](const auto& lhs, const auto& rhs)
				{
					return lhs.bindingIndex < rhs.bindingIndex;
				});

				descriptorRangesByTable.emplace_back();
				auto& tableRanges = descriptorRangesByTable.back();
				tableRanges.reserve(properties.size());
				const auto tableStartOffset = descriptorHeapOffset;

				for (const auto& property : properties)
				{
					D3D12_DESCRIPTOR_RANGE range{};
					range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
					range.NumDescriptors = (std::max)(1, property.arraySize);
					range.BaseShaderRegister = property.bindingIndex;
					range.RegisterSpace = property.bindingSpace;
					range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
					tableRanges.push_back(range);

					pipelineEntry.sampledTextures.push_back({
						property.name,
						property.bindingSpace,
						property.bindingIndex,
						descriptorHeapOffset,
						DXGI_FORMAT_R8G8B8A8_UNORM,
						property.type == NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_CUBE
							? NLS::Render::RHI::TextureDimension::TextureCube
							: NLS::Render::RHI::TextureDimension::Texture2D
					});
					descriptorHeapOffset += range.NumDescriptors;
				}

				D3D12_ROOT_PARAMETER parameter{};
				parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
				parameter.DescriptorTable.NumDescriptorRanges = static_cast<UINT>(tableRanges.size());
				parameter.DescriptorTable.pDescriptorRanges = tableRanges.data();
				parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

				pipelineEntry.descriptorTables.push_back({
					NLS::Render::Resources::ShaderResourceKind::SampledTexture,
					space,
					static_cast<UINT>(rootParameters.size()),
					tableStartOffset
				});
				rootParameters.push_back(parameter);
			}
		}

		D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
		rootSignatureDesc.NumParameters = static_cast<UINT>(rootParameters.size());
		rootSignatureDesc.pParameters = rootParameters.empty() ? nullptr : rootParameters.data();
		rootSignatureDesc.NumStaticSamplers = static_cast<UINT>(staticSamplers.size());
		rootSignatureDesc.pStaticSamplers = staticSamplers.empty() ? nullptr : staticSamplers.data();
		rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

		Microsoft::WRL::ComPtr<ID3DBlob> serializedRootSignature;
		Microsoft::WRL::ComPtr<ID3DBlob> rootSignatureError;
		if (FAILED(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serializedRootSignature, &rootSignatureError)))
		{
			if (rootSignatureError)
				NLS_LOG_ERROR("DX12 root signature serialization failed: " + std::string(static_cast<const char*>(rootSignatureError->GetBufferPointer()), rootSignatureError->GetBufferSize()));
			return;
		}

		if (FAILED(m_impl->device->CreateRootSignature(
			0,
			serializedRootSignature->GetBufferPointer(),
			serializedRootSignature->GetBufferSize(),
			IID_PPV_ARGS(&pipelineEntry.rootSignature))))
		{
			NLS_LOG_ERROR("Failed to create DX12 root signature.");
			return;
		}

		static const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
		};

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.pRootSignature = pipelineEntry.rootSignature.Get();
		psoDesc.VS = { vertexStage->bytecode.data(), vertexStage->bytecode.size() };
		psoDesc.PS = { pixelStage->bytecode.data(), pixelStage->bytecode.size() };
		D3D12_BLEND_DESC blendDesc{};
		blendDesc.AlphaToCoverageEnable = FALSE;
		blendDesc.IndependentBlendEnable = FALSE;
		D3D12_RENDER_TARGET_BLEND_DESC renderTargetBlendDesc{};
		renderTargetBlendDesc.BlendEnable = resolvedDesc.blendState.enabled;
		renderTargetBlendDesc.LogicOpEnable = FALSE;
		renderTargetBlendDesc.SrcBlend = resolvedDesc.blendState.enabled
			? D3D12_BLEND_SRC_ALPHA
			: D3D12_BLEND_ONE;
		renderTargetBlendDesc.DestBlend = resolvedDesc.blendState.enabled
			? D3D12_BLEND_INV_SRC_ALPHA
			: D3D12_BLEND_ZERO;
		renderTargetBlendDesc.BlendOp = D3D12_BLEND_OP_ADD;
		renderTargetBlendDesc.SrcBlendAlpha = resolvedDesc.blendState.enabled
			? D3D12_BLEND_ONE
			: D3D12_BLEND_ONE;
		renderTargetBlendDesc.DestBlendAlpha = resolvedDesc.blendState.enabled
			? D3D12_BLEND_INV_SRC_ALPHA
			: D3D12_BLEND_ZERO;
		renderTargetBlendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		renderTargetBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
		renderTargetBlendDesc.RenderTargetWriteMask = resolvedDesc.blendState.colorWrite ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;
		for (auto& renderTarget : blendDesc.RenderTarget)
			renderTarget = renderTargetBlendDesc;
		psoDesc.BlendState = blendDesc;
		psoDesc.SampleMask = UINT_MAX;
		D3D12_RASTERIZER_DESC rasterizerDesc{};
		rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
		rasterizerDesc.CullMode = resolvedDesc.rasterState.culling
			? ToD3DCullMode(resolvedDesc.rasterState.cullFace)
			: D3D12_CULL_MODE_NONE;
		rasterizerDesc.FrontCounterClockwise = TRUE;
		rasterizerDesc.DepthClipEnable = TRUE;
		rasterizerDesc.MultisampleEnable = FALSE;
		rasterizerDesc.AntialiasedLineEnable = FALSE;
		rasterizerDesc.ForcedSampleCount = 0;
		rasterizerDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
		psoDesc.RasterizerState = rasterizerDesc;
		D3D12_DEPTH_STENCIL_DESC depthStencilDesc{};
		depthStencilDesc.DepthEnable = resolvedDesc.depthStencilState.depthTest;
		depthStencilDesc.DepthWriteMask = resolvedDesc.depthStencilState.depthWrite
			? D3D12_DEPTH_WRITE_MASK_ALL
			: D3D12_DEPTH_WRITE_MASK_ZERO;
		depthStencilDesc.DepthFunc = ToD3DComparisonFunc(resolvedDesc.depthStencilState.depthCompare);
		depthStencilDesc.StencilEnable = FALSE;
		depthStencilDesc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
		depthStencilDesc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
		depthStencilDesc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		depthStencilDesc.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		psoDesc.DepthStencilState = depthStencilDesc;
		psoDesc.InputLayout = { inputLayout, static_cast<UINT>(_countof(inputLayout)) };
		psoDesc.PrimitiveTopologyType = ToD3DPrimitiveTopologyType(resolvedDesc.primitiveMode);
		psoDesc.NumRenderTargets = static_cast<UINT>((std::min)(
			resolvedDesc.attachmentLayout.colorAttachmentFormats.size(),
			static_cast<size_t>(D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)));
		for (UINT renderTargetIndex = 0; renderTargetIndex < psoDesc.NumRenderTargets; ++renderTargetIndex)
			psoDesc.RTVFormats[renderTargetIndex] = ToDxgiAttachmentFormat(resolvedDesc.attachmentLayout.colorAttachmentFormats[renderTargetIndex]);
		psoDesc.DSVFormat = resolvedDesc.attachmentLayout.hasDepthAttachment
			? ToDxgiAttachmentFormat(resolvedDesc.attachmentLayout.depthAttachmentFormat)
			: DXGI_FORMAT_UNKNOWN;
		psoDesc.SampleDesc.Count = resolvedDesc.attachmentLayout.sampleCount;

		const HRESULT createPsoHr = m_impl->device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineEntry.pipelineState));
		if (FAILED(createPsoHr))
		{
			NLS_LOG_ERROR("Failed to create DX12 graphics pipeline state: " + FormatHRESULT(createPsoHr));
			NLS_LOG_WARNING(
				"DX12 pipeline summary: RTCount=" + std::to_string(psoDesc.NumRenderTargets) +
				", HasDepth=" + std::to_string(psoDesc.DSVFormat != DXGI_FORMAT_UNKNOWN ? 1 : 0) +
				", PrimitiveTopologyType=" + std::to_string(static_cast<int>(psoDesc.PrimitiveTopologyType)) +
				", SampleCount=" + std::to_string(psoDesc.SampleDesc.Count));
			LogDX12InfoQueueMessages(m_impl->device.Get(), "DX12 pipeline creation");
			return;
		}

		m_impl->pipelineCache.emplace(pipelineKey, std::move(pipelineEntry));
#else
		(void)pipelineDesc;
		(void)bindingSet;
#endif
	}
	std::shared_ptr<NLS::Render::RHI::IRHITexture> DX12RenderDevice::CreateTextureResource(NLS::Render::RHI::TextureDimension dimension)
	{
		NLS::Render::RHI::TextureDesc desc{};
		desc.dimension = dimension;
		auto resource = std::make_shared<DX12TextureResource>(CreateTexture(), desc, [this](uint32_t id) { DestroyTexture(id); });
#if defined(_WIN32)
		if (resource)
			m_impl->textureObjects[resource->GetResourceId()] = resource;
#endif
		return resource;
	}
	uint32_t DX12RenderDevice::CreateTexture()
	{
#if defined(_WIN32)
		const auto id = m_impl->nextResourceId++;
		m_impl->textures.emplace(id, Impl::TextureResource{});
		return id;
#else
		return 0;
#endif
	}
	void DX12RenderDevice::DestroyTexture(uint32_t textureId)
	{
#if defined(_WIN32)
		m_impl->textureObjects.erase(textureId);
		m_impl->textures.erase(textureId);
#endif
	}
	void DX12RenderDevice::BindTexture(NLS::Render::RHI::TextureDimension, uint32_t textureId)
	{
#if defined(_WIN32)
		m_impl->boundTexture = textureId;
#endif
	}
	void DX12RenderDevice::ActivateTexture(uint32_t) {}
	void DX12RenderDevice::SetupTexture(const NLS::Render::RHI::TextureDesc& desc, const void* data)
	{
#if defined(_WIN32)
		const auto found = m_impl->textures.find(m_impl->boundTexture);
		if (found == m_impl->textures.end() || !m_impl->device)
			return;

		auto& resource = found->second;
		resource.resource.Reset();

		const bool isDepthTexture = desc.format == NLS::Render::RHI::TextureFormat::Depth24Stencil8;
		const auto layerCount = static_cast<UINT16>(NLS::Render::RHI::GetTextureLayerCount(desc.dimension));
		const auto initialState =
			(data != nullptr && !isDepthTexture)
			? D3D12_RESOURCE_STATE_COPY_DEST
			: GetInitialTextureState(desc);

		const D3D12_RESOURCE_DESC resourceDesc{
			D3D12_RESOURCE_DIMENSION_TEXTURE2D,
			0,
			desc.width,
			desc.height,
			layerCount,
			1,
			ToDxgiResourceFormat(desc.format),
			{ 1, 0 },
			D3D12_TEXTURE_LAYOUT_UNKNOWN,
			GetTextureResourceFlags(desc)
		};

		const D3D12_HEAP_PROPERTIES heapProperties{
			D3D12_HEAP_TYPE_DEFAULT,
			D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
			D3D12_MEMORY_POOL_UNKNOWN,
			1,
			1
		};

		D3D12_CLEAR_VALUE clearValue{};
		clearValue.Format = ToDxgiAttachmentFormat(desc.format);
		if (isDepthTexture)
			clearValue.DepthStencil = { 1.0f, 0 };

		const bool hasAttachmentUsage =
			NLS::Render::RHI::HasUsage(desc.usage, NLS::Render::RHI::TextureUsage::ColorAttachment) ||
			NLS::Render::RHI::HasUsage(desc.usage, NLS::Render::RHI::TextureUsage::DepthStencilAttachment);
		if (FAILED(m_impl->device->CreateCommittedResource(
			&heapProperties,
			D3D12_HEAP_FLAG_NONE,
			&resourceDesc,
			initialState,
			hasAttachmentUsage ? &clearValue : nullptr,
			IID_PPV_ARGS(&resource.resource))))
		{
			return;
		}

		resource.desc = desc;
		resource.state = initialState;

		if (data != nullptr && !isDepthTexture)
		{
			const auto bytesPerPixel = static_cast<size_t>(NLS::Render::RHI::GetTextureFormatBytesPerPixel(desc.format));
			const auto rowSizeInBytes = static_cast<size_t>(desc.width) * bytesPerPixel;
			const auto layerSizeInBytes = rowSizeInBytes * static_cast<size_t>(desc.height);
			UINT64 uploadSize = 0;
			m_impl->device->GetCopyableFootprints(&resourceDesc, 0, layerCount, 0, nullptr, nullptr, nullptr, &uploadSize);

			Microsoft::WRL::ComPtr<ID3D12Resource> uploadResource;
			const D3D12_HEAP_PROPERTIES uploadHeapProperties{
				D3D12_HEAP_TYPE_UPLOAD,
				D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
				D3D12_MEMORY_POOL_UNKNOWN,
				1,
				1
			};
			const D3D12_RESOURCE_DESC uploadDesc{
				D3D12_RESOURCE_DIMENSION_BUFFER,
				0,
				uploadSize,
				1,
				1,
				1,
				DXGI_FORMAT_UNKNOWN,
				{ 1, 0 },
				D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
				D3D12_RESOURCE_FLAG_NONE
			};
			if (FAILED(m_impl->device->CreateCommittedResource(
				&uploadHeapProperties,
				D3D12_HEAP_FLAG_NONE,
				&uploadDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(&uploadResource))))
			{
				return;
			}

			std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(layerCount);
			std::vector<UINT> numRows(layerCount);
			std::vector<UINT64> rowSizesInBytes(layerCount);
			UINT64 totalBytes = 0;
			m_impl->device->GetCopyableFootprints(
				&resourceDesc,
				0,
				layerCount,
				0,
				footprints.data(),
				numRows.data(),
				rowSizesInBytes.data(),
				&totalBytes);

			void* mappedData = nullptr;
			const D3D12_RANGE readRange{ 0, 0 };
			if (FAILED(uploadResource->Map(0, &readRange, &mappedData)))
				return;

			const auto* sourceBytes = static_cast<const uint8_t*>(data);
			auto* destinationBytes = static_cast<uint8_t*>(mappedData);
			for (UINT layerIndex = 0; layerIndex < layerCount; ++layerIndex)
			{
				const auto& footprint = footprints[layerIndex];
				const auto* layerSource = sourceBytes + layerSizeInBytes * static_cast<size_t>(layerIndex);
				for (UINT rowIndex = 0; rowIndex < numRows[layerIndex]; ++rowIndex)
				{
					std::memcpy(
						destinationBytes + footprint.Offset + static_cast<size_t>(rowIndex) * footprint.Footprint.RowPitch,
						layerSource + rowSizeInBytes * static_cast<size_t>(rowIndex),
						rowSizeInBytes);
				}
			}
			uploadResource->Unmap(0, nullptr);

			if (!ExecuteImmediateCommandList(*m_impl, [&](ID3D12GraphicsCommandList* commandList)
			{
				if (commandList == nullptr)
					return false;

				for (UINT layerIndex = 0; layerIndex < layerCount; ++layerIndex)
				{
					D3D12_TEXTURE_COPY_LOCATION dstLocation{};
					dstLocation.pResource = resource.resource.Get();
					dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					dstLocation.SubresourceIndex = layerIndex;

					D3D12_TEXTURE_COPY_LOCATION srcLocation{};
					srcLocation.pResource = uploadResource.Get();
					srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
					srcLocation.PlacedFootprint = footprints[layerIndex];

					commandList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
				}

				TransitionResource(commandList, resource.resource.Get(), resource.state, GetInitialTextureState(desc));
				return true;
			}))
			{
				return;
			}
		}

		if (const auto objectIt = m_impl->textureObjects.find(m_impl->boundTexture); objectIt != m_impl->textureObjects.end())
		{
			if (auto textureObject = objectIt->second.lock())
				textureObject->SetDesc(desc);
		}
#endif
	}
	void DX12RenderDevice::GenerateTextureMipmap(NLS::Render::RHI::TextureDimension) {}
	uint32_t DX12RenderDevice::CreateFramebuffer()
	{
#if defined(_WIN32)
		const auto id = m_impl->nextResourceId++;
		m_impl->framebuffers.emplace(id, Impl::FramebufferResource{});
		return id;
#else
		return 0;
#endif
	}
	void DX12RenderDevice::DestroyFramebuffer(uint32_t framebufferId)
	{
#if defined(_WIN32)
		if (m_impl->boundFramebuffer == framebufferId)
			m_impl->boundFramebuffer = 0;
		m_impl->framebuffers.erase(framebufferId);
#else
		(void)framebufferId;
#endif
	}
	void DX12RenderDevice::BindFramebuffer(uint32_t framebufferId)
	{
#if defined(_WIN32)
		m_impl->boundFramebuffer = framebufferId;
#else
		(void)framebufferId;
#endif
	}
	void DX12RenderDevice::AttachFramebufferColorTexture(uint32_t framebufferId, uint32_t textureId, uint32_t attachmentIndex)
	{
#if defined(_WIN32)
		auto framebufferIt = m_impl->framebuffers.find(framebufferId);
		if (framebufferIt == m_impl->framebuffers.end())
			return;

		auto& colorTextures = framebufferIt->second.colorTextureIds;
		if (attachmentIndex >= colorTextures.size())
			colorTextures.resize(static_cast<size_t>(attachmentIndex) + 1u, 0u);
		colorTextures[attachmentIndex] = textureId;
		framebufferIt->second.descriptorsDirty = true;
#else
		(void)framebufferId;
		(void)textureId;
		(void)attachmentIndex;
#endif
	}
	void DX12RenderDevice::AttachFramebufferDepthStencilTexture(uint32_t framebufferId, uint32_t textureId)
	{
#if defined(_WIN32)
		auto framebufferIt = m_impl->framebuffers.find(framebufferId);
		if (framebufferIt == m_impl->framebuffers.end())
			return;

		framebufferIt->second.depthTextureId = textureId;
		framebufferIt->second.descriptorsDirty = true;
#else
		(void)framebufferId;
		(void)textureId;
#endif
	}
	void DX12RenderDevice::SetFramebufferDrawBufferCount(uint32_t framebufferId, uint32_t colorAttachmentCount)
	{
#if defined(_WIN32)
		if (auto framebufferIt = m_impl->framebuffers.find(framebufferId); framebufferIt != m_impl->framebuffers.end())
			framebufferIt->second.drawBufferCount = colorAttachmentCount;
#else
		(void)framebufferId;
		(void)colorAttachmentCount;
#endif
	}
	void DX12RenderDevice::BlitDepth(uint32_t sourceFramebufferId, uint32_t destinationFramebufferId, uint32_t, uint32_t)
	{
#if defined(_WIN32)
		const auto sourceFramebufferIt = m_impl->framebuffers.find(sourceFramebufferId);
		const auto destinationFramebufferIt = m_impl->framebuffers.find(destinationFramebufferId);
		if (sourceFramebufferIt == m_impl->framebuffers.end() || destinationFramebufferIt == m_impl->framebuffers.end())
			return;

		const auto sourceTextureIt = m_impl->textures.find(sourceFramebufferIt->second.depthTextureId);
		const auto destinationTextureIt = m_impl->textures.find(destinationFramebufferIt->second.depthTextureId);
		if (sourceTextureIt == m_impl->textures.end() ||
			destinationTextureIt == m_impl->textures.end() ||
			!sourceTextureIt->second.resource ||
			!destinationTextureIt->second.resource)
		{
			return;
		}

		ExecuteImmediateCommandList(*m_impl, [&](ID3D12GraphicsCommandList* commandList)
		{
			if (commandList == nullptr)
				return false;

			auto& sourceTexture = sourceTextureIt->second;
			auto& destinationTexture = destinationTextureIt->second;
			const auto sourceStateBeforeCopy = sourceTexture.state;
			const auto destinationStateBeforeCopy = destinationTexture.state;

			TransitionResource(commandList, sourceTexture.resource.Get(), sourceTexture.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
			TransitionResource(commandList, destinationTexture.resource.Get(), destinationTexture.state, D3D12_RESOURCE_STATE_COPY_DEST);

			D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
			sourceLocation.pResource = sourceTexture.resource.Get();
			sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			sourceLocation.SubresourceIndex = 0;

			D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
			destinationLocation.pResource = destinationTexture.resource.Get();
			destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			destinationLocation.SubresourceIndex = 0;

			commandList->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, nullptr);

			TransitionResource(commandList, sourceTexture.resource.Get(), sourceTexture.state, sourceStateBeforeCopy);
			TransitionResource(commandList, destinationTexture.resource.Get(), destinationTexture.state, destinationStateBeforeCopy);
			return true;
		});
#else
		(void)sourceFramebufferId;
		(void)destinationFramebufferId;
#endif
	}
	std::shared_ptr<NLS::Render::RHI::IRHIBuffer> DX12RenderDevice::CreateBufferResource(NLS::Render::RHI::BufferType type)
	{
		auto resource = std::make_shared<DX12BufferResource>(CreateBuffer(), type, [this](uint32_t id) { DestroyBuffer(id); });
#if defined(_WIN32)
		if (resource)
			m_impl->bufferObjects[resource->GetResourceId()] = resource;
#endif
		return resource;
	}
	uint32_t DX12RenderDevice::CreateBuffer()
	{
#if defined(_WIN32)
		const auto id = m_impl->nextResourceId++;
		m_impl->buffers.emplace(id, Impl::BufferResource{});
		return id;
#else
		return 0;
#endif
	}
	void DX12RenderDevice::DestroyBuffer(uint32_t bufferId)
	{
#if defined(_WIN32)
		m_impl->bufferObjects.erase(bufferId);
		m_impl->buffers.erase(bufferId);
#endif
	}
	void DX12RenderDevice::BindBuffer(NLS::Render::RHI::BufferType type, uint32_t bufferId)
	{
#if defined(_WIN32)
		m_impl->boundBuffers[type] = bufferId;
#endif
	}
	void DX12RenderDevice::BindBufferBase(NLS::Render::RHI::BufferType type, uint32_t bindingPoint, uint32_t bufferId)
	{
#if defined(_WIN32)
		m_impl->boundBuffers[type] = bufferId;
		if (type == NLS::Render::RHI::BufferType::Uniform)
			m_impl->uniformBufferBindings[bindingPoint] = bufferId;
#endif
	}
	void DX12RenderDevice::SetBufferData(NLS::Render::RHI::BufferType type, size_t size, const void* data, NLS::Render::RHI::BufferUsage)
	{
#if defined(_WIN32)
		const auto bound = m_impl->boundBuffers.find(type);
		if (bound == m_impl->boundBuffers.end() || !m_impl->device)
			return;

		auto found = m_impl->buffers.find(bound->second);
		if (found == m_impl->buffers.end())
			return;

		const auto allocationSize = type == NLS::Render::RHI::BufferType::Uniform
			? (size + 255u) & ~size_t(255u)
			: size;

		const D3D12_HEAP_PROPERTIES heapProperties{
			D3D12_HEAP_TYPE_UPLOAD,
			D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
			D3D12_MEMORY_POOL_UNKNOWN,
			1,
			1
		};

		const D3D12_RESOURCE_DESC resourceDesc{
			D3D12_RESOURCE_DIMENSION_BUFFER,
			0,
			static_cast<UINT64>(allocationSize),
			1,
			1,
			1,
			DXGI_FORMAT_UNKNOWN,
			{ 1, 0 },
			D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
			D3D12_RESOURCE_FLAG_NONE
		};

		if (FAILED(m_impl->device->CreateCommittedResource(
			&heapProperties,
			D3D12_HEAP_FLAG_NONE,
			&resourceDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&found->second.resource))))
		{
			return;
		}

		found->second.size = size;
		if (const auto objectIt = m_impl->bufferObjects.find(bound->second); objectIt != m_impl->bufferObjects.end())
		{
			if (auto bufferObject = objectIt->second.lock())
				bufferObject->SetSize(size);
		}

		if (data != nullptr && size > 0)
		{
			void* mappedData = nullptr;
			const D3D12_RANGE readRange{ 0, 0 };
			if (SUCCEEDED(found->second.resource->Map(0, &readRange, &mappedData)))
			{
				memcpy(mappedData, data, size);
				found->second.resource->Unmap(0, nullptr);
			}
		}
#endif
	}
	void DX12RenderDevice::SetBufferSubData(NLS::Render::RHI::BufferType type, size_t offset, size_t size, const void* data)
	{
#if defined(_WIN32)
		const auto bound = m_impl->boundBuffers.find(type);
		if (bound == m_impl->boundBuffers.end())
			return;

		auto found = m_impl->buffers.find(bound->second);
		if (found == m_impl->buffers.end() || !found->second.resource || data == nullptr || offset + size > found->second.size)
			return;

		void* mappedData = nullptr;
		const D3D12_RANGE readRange{ 0, 0 };
		if (SUCCEEDED(found->second.resource->Map(0, &readRange, &mappedData)))
		{
			std::memcpy(static_cast<uint8_t*>(mappedData) + offset, data, size);
			found->second.resource->Unmap(0, nullptr);
		}
#else
		(void)type;
		(void)offset;
		(void)size;
		(void)data;
#endif
	}
	void* DX12RenderDevice::GetUITextureHandle(uint32_t textureId) const
	{
#if defined(_WIN32)
		if (const auto textureIt = m_impl->textures.find(textureId); textureIt != m_impl->textures.end() && textureIt->second.resource)
			return textureIt->second.resource.Get();
#else
		(void)textureId;
#endif
		return nullptr;
	}
	void DX12RenderDevice::ReleaseUITextureHandles() {}
	bool DX12RenderDevice::PrepareUIRender()
	{
#if !defined(_WIN32)
		return false;
#else
		if (!m_impl->device || !m_impl->graphicsQueue || !m_impl->commandList)
			return false;

		if (!m_impl->hasPendingCommands)
			return true;

		for (auto& [textureId, texture] : m_impl->textures)
		{
			(void)textureId;
			if (!texture.resource)
				continue;

			if (texture.state == D3D12_RESOURCE_STATE_RENDER_TARGET)
			{
				TransitionResource(
					m_impl->commandList.Get(),
					texture.resource.Get(),
					texture.state,
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}
		}

		if (m_impl->isFrameRecording && m_impl->swapchain && m_impl->commandList && !m_impl->frameContexts.empty())
		{
			const UINT frameIndex = m_impl->swapchain->GetCurrentBackBufferIndex();
			auto& frame = m_impl->frameContexts[frameIndex];
			if (frame.backBuffer && frame.backBufferState == D3D12_RESOURCE_STATE_RENDER_TARGET)
			{
				D3D12_RESOURCE_BARRIER toPresent{};
				toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				toPresent.Transition.pResource = frame.backBuffer.Get();
				toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
				toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
				m_impl->commandList->ResourceBarrier(1, &toPresent);
				frame.backBufferState = D3D12_RESOURCE_STATE_PRESENT;
			}

			m_impl->commandList->Close();
			m_impl->isFrameRecording = false;
		}

		ID3D12CommandList* commandLists[] = { m_impl->commandList.Get() };
		m_impl->graphicsQueue->ExecuteCommandLists(1, commandLists);

		const UINT64 fenceValue = ++m_impl->fenceValue;
		m_impl->graphicsQueue->Signal(m_impl->fence.Get(), fenceValue);
		if (m_impl->fence->GetCompletedValue() < fenceValue)
		{
			m_impl->fence->SetEventOnCompletion(fenceValue, m_impl->fenceEvent);
			WaitForSingleObject(m_impl->fenceEvent, INFINITE);
		}

		m_impl->hasPendingCommands = false;
		return true;
#endif
	}
	std::string DX12RenderDevice::GetVendor() { return m_impl->vendor; }
	std::string DX12RenderDevice::GetHardware() { return m_impl->hardware; }
	std::string DX12RenderDevice::GetVersion() { return m_impl->version; }
	std::string DX12RenderDevice::GetShadingLanguageVersion() { return m_impl->shadingLanguageVersion; }
	NLS::Render::RHI::RHIDeviceCapabilities DX12RenderDevice::GetCapabilities() const { return m_impl->capabilities; }
	NLS::Render::RHI::NativeRenderDeviceInfo DX12RenderDevice::GetNativeDeviceInfo() const
	{
		NLS::Render::RHI::NativeRenderDeviceInfo info{};
		info.backend = NLS::Render::RHI::NativeBackendType::DX12;
#if defined(_WIN32)
		info.device = m_impl->device.Get();
		info.graphicsQueue = m_impl->graphicsQueue.Get();
		info.swapchain = m_impl->swapchain.Get();
		info.nativeWindowHandle = m_impl->swapchainWindow;
		info.swapchainImageCount = m_impl->swapchainImageCount;
#endif
		return info;
	}
	bool DX12RenderDevice::IsBackendReady() const { return m_impl->backendReady; }
	bool DX12RenderDevice::CreateSwapchain(const NLS::Render::RHI::SwapchainDesc& desc)
	{
#if !defined(_WIN32)
		(void)desc;
		return false;
#else
		if (!m_impl->backendReady || !m_impl->factory || !m_impl->graphicsQueue || desc.nativeWindowHandle == nullptr)
			return false;

		DestroySwapchain();

		DXGI_SWAP_CHAIN_DESC1 swapchainDesc{};
		swapchainDesc.Width = desc.width;
		swapchainDesc.Height = desc.height;
		swapchainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		swapchainDesc.SampleDesc.Count = 1;
		swapchainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		swapchainDesc.BufferCount = desc.imageCount;
		swapchainDesc.Scaling = DXGI_SCALING_STRETCH;
		swapchainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		swapchainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

		Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain1;
		if (FAILED(m_impl->factory->CreateSwapChainForHwnd(
			m_impl->graphicsQueue.Get(),
			static_cast<HWND>(desc.nativeWindowHandle),
			&swapchainDesc,
			nullptr,
			nullptr,
			&swapchain1)))
		{
			NLS_LOG_ERROR("Failed to create DX12 swapchain.");
			return false;
		}

		if (FAILED(swapchain1.As(&m_impl->swapchain)))
		{
			NLS_LOG_ERROR("Failed to query IDXGISwapChain3.");
			return false;
		}

		m_impl->swapchainWindow = static_cast<HWND>(desc.nativeWindowHandle);
		m_impl->swapchainWidth = desc.width;
		m_impl->swapchainHeight = desc.height;
		m_impl->swapchainImageCount = desc.imageCount;
		m_impl->swapchainVsync = desc.vsync;

		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtvHeapDesc.NumDescriptors = desc.imageCount;
		rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		if (FAILED(m_impl->device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_impl->rtvHeap))))
		{
			NLS_LOG_ERROR("Failed to create DX12 RTV heap.");
			DestroySwapchain();
			return false;
		}

		D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
		dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		dsvHeapDesc.NumDescriptors = 1;
		dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		if (FAILED(m_impl->device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_impl->dsvHeap))))
		{
			NLS_LOG_ERROR("Failed to create DX12 DSV heap.");
			DestroySwapchain();
			return false;
		}

		D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
		srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		srvHeapDesc.NumDescriptors = 64;
		srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (FAILED(m_impl->device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_impl->srvHeap))))
		{
			NLS_LOG_ERROR("Failed to create DX12 SRV heap.");
			DestroySwapchain();
			return false;
		}
		m_impl->srvDescriptorSize = m_impl->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc{};
		nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		nullSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		nullSrvDesc.Texture2D.MipLevels = 1;
		m_impl->device->CreateShaderResourceView(nullptr, &nullSrvDesc, m_impl->srvHeap->GetCPUDescriptorHandleForHeapStart());

		m_impl->rtvDescriptorSize = m_impl->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		m_impl->frameContexts.resize(desc.imageCount);
		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_impl->rtvHeap->GetCPUDescriptorHandleForHeapStart();
		for (uint32_t i = 0; i < desc.imageCount; ++i)
		{
			if (FAILED(m_impl->swapchain->GetBuffer(i, IID_PPV_ARGS(&m_impl->frameContexts[i].backBuffer))))
			{
				NLS_LOG_ERROR("Failed to fetch DX12 swapchain backbuffer.");
				DestroySwapchain();
				return false;
			}

			m_impl->device->CreateRenderTargetView(m_impl->frameContexts[i].backBuffer.Get(), nullptr, rtvHandle);
			rtvHandle.ptr += m_impl->rtvDescriptorSize;

			if (FAILED(m_impl->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_impl->frameContexts[i].commandAllocator))))
			{
				NLS_LOG_ERROR("Failed to create DX12 command allocator.");
				DestroySwapchain();
				return false;
			}
		}

		if (FAILED(m_impl->device->CreateCommandList(
			0,
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			m_impl->frameContexts[0].commandAllocator.Get(),
			nullptr,
			IID_PPV_ARGS(&m_impl->commandList))))
		{
			NLS_LOG_ERROR("Failed to create DX12 graphics command list.");
			DestroySwapchain();
			return false;
		}

		m_impl->commandList->Close();
		const D3D12_RESOURCE_DESC depthDesc{
			D3D12_RESOURCE_DIMENSION_TEXTURE2D,
			0,
			desc.width,
			desc.height,
			1,
			1,
			DXGI_FORMAT_D24_UNORM_S8_UINT,
			{ 1, 0 },
			D3D12_TEXTURE_LAYOUT_UNKNOWN,
			D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
		};
		const D3D12_HEAP_PROPERTIES depthHeapProperties{
			D3D12_HEAP_TYPE_DEFAULT,
			D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
			D3D12_MEMORY_POOL_UNKNOWN,
			1,
			1
		};
		const D3D12_CLEAR_VALUE depthClearValue{
			DXGI_FORMAT_D24_UNORM_S8_UINT,
			{ 1.0f, 0 }
		};
		if (FAILED(m_impl->device->CreateCommittedResource(
			&depthHeapProperties,
			D3D12_HEAP_FLAG_NONE,
			&depthDesc,
			D3D12_RESOURCE_STATE_DEPTH_WRITE,
			&depthClearValue,
			IID_PPV_ARGS(&m_impl->depthBuffer))))
		{
			NLS_LOG_ERROR("Failed to create DX12 depth buffer.");
			DestroySwapchain();
			return false;
		}

		D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		m_impl->device->CreateDepthStencilView(m_impl->depthBuffer.Get(), &dsvDesc, m_impl->dsvHeap->GetCPUDescriptorHandleForHeapStart());
		SetViewport(0, 0, desc.width, desc.height);
		return true;
#endif
	}

	void DX12RenderDevice::DestroySwapchain()
	{
#if defined(_WIN32)
		if (m_impl->device && m_impl->graphicsQueue && m_impl->fence && m_impl->fenceEvent)
		{
			const UINT64 fenceValue = ++m_impl->fenceValue;
			m_impl->graphicsQueue->Signal(m_impl->fence.Get(), fenceValue);
			if (m_impl->fence->GetCompletedValue() < fenceValue)
			{
				m_impl->fence->SetEventOnCompletion(fenceValue, m_impl->fenceEvent);
				WaitForSingleObject(m_impl->fenceEvent, INFINITE);
			}
		}

		m_impl->frameContexts.clear();
		m_impl->commandList.Reset();
		m_impl->rtvHeap.Reset();
		m_impl->dsvHeap.Reset();
		m_impl->srvHeap.Reset();
		m_impl->depthBuffer.Reset();
		m_impl->swapchain.Reset();
		m_impl->swapchainWindow = nullptr;
		m_impl->swapchainWidth = 0;
		m_impl->swapchainHeight = 0;
		m_impl->hasPendingCommands = false;
		m_impl->isFrameRecording = false;
#endif
	}

	void DX12RenderDevice::ResizeSwapchain(uint32_t width, uint32_t height)
	{
#if defined(_WIN32)
		if (width == 0 || height == 0)
			return;

		if (!m_impl->swapchain || m_impl->swapchainWindow == nullptr)
			return;

		if (m_impl->swapchainWidth == width && m_impl->swapchainHeight == height)
			return;

		if (m_impl->device && m_impl->graphicsQueue && m_impl->fence && m_impl->fenceEvent)
		{
			const UINT64 fenceValue = ++m_impl->fenceValue;
			m_impl->graphicsQueue->Signal(m_impl->fence.Get(), fenceValue);
			if (m_impl->fence->GetCompletedValue() < fenceValue)
			{
				m_impl->fence->SetEventOnCompletion(fenceValue, m_impl->fenceEvent);
				WaitForSingleObject(m_impl->fenceEvent, INFINITE);
			}
		}

		m_impl->hasPendingCommands = false;
		m_impl->isFrameRecording = false;
		m_impl->commandList.Reset();
		m_impl->depthBuffer.Reset();
		m_impl->rtvHeap.Reset();
		m_impl->dsvHeap.Reset();
		m_impl->srvHeap.Reset();
		for (auto& frame : m_impl->frameContexts)
		{
			frame.backBuffer.Reset();
			frame.commandAllocator.Reset();
			frame.backBufferState = D3D12_RESOURCE_STATE_PRESENT;
		}
		m_impl->frameContexts.clear();

		if (FAILED(m_impl->swapchain->ResizeBuffers(
			m_impl->swapchainImageCount,
			width,
			height,
			DXGI_FORMAT_R8G8B8A8_UNORM,
			0)))
		{
			NLS_LOG_ERROR("Failed to resize DX12 swapchain buffers.");
			DestroySwapchain();
			return;
		}

		m_impl->swapchainWidth = width;
		m_impl->swapchainHeight = height;

		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtvHeapDesc.NumDescriptors = m_impl->swapchainImageCount;
		rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		if (FAILED(m_impl->device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_impl->rtvHeap))))
		{
			NLS_LOG_ERROR("Failed to recreate DX12 RTV heap during resize.");
			DestroySwapchain();
			return;
		}

		D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
		dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		dsvHeapDesc.NumDescriptors = 1;
		dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		if (FAILED(m_impl->device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_impl->dsvHeap))))
		{
			NLS_LOG_ERROR("Failed to recreate DX12 DSV heap during resize.");
			DestroySwapchain();
			return;
		}

		D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
		srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		srvHeapDesc.NumDescriptors = 64;
		srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (FAILED(m_impl->device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_impl->srvHeap))))
		{
			NLS_LOG_ERROR("Failed to recreate DX12 SRV heap during resize.");
			DestroySwapchain();
			return;
		}

		m_impl->rtvDescriptorSize = m_impl->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		m_impl->srvDescriptorSize = m_impl->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc{};
		nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		nullSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		nullSrvDesc.Texture2D.MipLevels = 1;
		m_impl->device->CreateShaderResourceView(nullptr, &nullSrvDesc, m_impl->srvHeap->GetCPUDescriptorHandleForHeapStart());

		m_impl->frameContexts.resize(m_impl->swapchainImageCount);
		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_impl->rtvHeap->GetCPUDescriptorHandleForHeapStart();
		for (uint32_t i = 0; i < m_impl->swapchainImageCount; ++i)
		{
			if (FAILED(m_impl->swapchain->GetBuffer(i, IID_PPV_ARGS(&m_impl->frameContexts[i].backBuffer))))
			{
				NLS_LOG_ERROR("Failed to fetch DX12 backbuffer during resize.");
				DestroySwapchain();
				return;
			}

			m_impl->device->CreateRenderTargetView(m_impl->frameContexts[i].backBuffer.Get(), nullptr, rtvHandle);
			rtvHandle.ptr += m_impl->rtvDescriptorSize;

			if (FAILED(m_impl->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_impl->frameContexts[i].commandAllocator))))
			{
				NLS_LOG_ERROR("Failed to recreate DX12 command allocator during resize.");
				DestroySwapchain();
				return;
			}
		}

		if (FAILED(m_impl->device->CreateCommandList(
			0,
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			m_impl->frameContexts[0].commandAllocator.Get(),
			nullptr,
			IID_PPV_ARGS(&m_impl->commandList))))
		{
			NLS_LOG_ERROR("Failed to recreate DX12 graphics command list during resize.");
			DestroySwapchain();
			return;
		}

		m_impl->commandList->Close();

		const D3D12_RESOURCE_DESC depthDesc{
			D3D12_RESOURCE_DIMENSION_TEXTURE2D,
			0,
			width,
			height,
			1,
			1,
			DXGI_FORMAT_D24_UNORM_S8_UINT,
			{ 1, 0 },
			D3D12_TEXTURE_LAYOUT_UNKNOWN,
			D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
		};
		const D3D12_HEAP_PROPERTIES depthHeapProperties{
			D3D12_HEAP_TYPE_DEFAULT,
			D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
			D3D12_MEMORY_POOL_UNKNOWN,
			1,
			1
		};
		const D3D12_CLEAR_VALUE depthClearValue{
			DXGI_FORMAT_D24_UNORM_S8_UINT,
			{ 1.0f, 0 }
		};
		if (FAILED(m_impl->device->CreateCommittedResource(
			&depthHeapProperties,
			D3D12_HEAP_FLAG_NONE,
			&depthDesc,
			D3D12_RESOURCE_STATE_DEPTH_WRITE,
			&depthClearValue,
			IID_PPV_ARGS(&m_impl->depthBuffer))))
		{
			NLS_LOG_ERROR("Failed to recreate DX12 depth buffer during resize.");
			DestroySwapchain();
			return;
		}

		D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		m_impl->device->CreateDepthStencilView(m_impl->depthBuffer.Get(), &dsvDesc, m_impl->dsvHeap->GetCPUDescriptorHandleForHeapStart());
		SetViewport(0, 0, width, height);
#else
		(void)width;
		(void)height;
#endif
	}

	void DX12RenderDevice::PresentSwapchain()
	{
#if defined(_WIN32)
		if (!m_impl->swapchain)
			return;

		if (m_impl->isFrameRecording && m_impl->commandList && !m_impl->frameContexts.empty())
		{
			const UINT frameIndex = m_impl->swapchain->GetCurrentBackBufferIndex();
			auto& frame = m_impl->frameContexts[frameIndex];
			D3D12_RESOURCE_BARRIER toPresent{};
			toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			toPresent.Transition.pResource = frame.backBuffer.Get();
			toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
			m_impl->commandList->ResourceBarrier(1, &toPresent);
			m_impl->commandList->Close();
			m_impl->isFrameRecording = false;
		}

		if (m_impl->hasPendingCommands && m_impl->commandList)
		{
			ID3D12CommandList* commandLists[] = { m_impl->commandList.Get() };
			m_impl->graphicsQueue->ExecuteCommandLists(1, commandLists);

			const UINT64 fenceValue = ++m_impl->fenceValue;
			m_impl->graphicsQueue->Signal(m_impl->fence.Get(), fenceValue);
			if (m_impl->fence->GetCompletedValue() < fenceValue)
			{
				m_impl->fence->SetEventOnCompletion(fenceValue, m_impl->fenceEvent);
				WaitForSingleObject(m_impl->fenceEvent, INFINITE);
			}

			m_impl->hasPendingCommands = false;
		}

		m_impl->swapchain->Present(m_impl->swapchainVsync ? 1 : 0, 0);
#endif
	}
}
