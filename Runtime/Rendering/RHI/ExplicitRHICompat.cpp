#include "Rendering/RHI/ExplicitRHICompat.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <type_traits>
#include <utility>
#include <variant>

#include "Debug/Logger.h"
#include "Rendering/Settings/ERenderingCapability.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/RHI/BindingPointMap.h"
#include "Rendering/RHI/IRenderDevice.h"
#include "Rendering/Resources/BindingSetInstance.h"
#include "Rendering/Resources/IMesh.h"

namespace NLS::Render::RHI
{
	namespace
	{
		PrimitiveTopology ToPrimitiveTopology(const NLS::Render::Settings::EPrimitiveMode primitiveMode)
		{
			switch (primitiveMode)
			{
			case NLS::Render::Settings::EPrimitiveMode::LINES: return PrimitiveTopology::LineList;
			case NLS::Render::Settings::EPrimitiveMode::POINTS: return PrimitiveTopology::PointList;
			case NLS::Render::Settings::EPrimitiveMode::TRIANGLES:
			default:
				return PrimitiveTopology::TriangleList;
			}
		}

		NLS::Render::Settings::EPrimitiveMode ToPrimitiveMode(const PrimitiveTopology topology)
		{
			switch (topology)
			{
			case PrimitiveTopology::LineList: return NLS::Render::Settings::EPrimitiveMode::LINES;
			case PrimitiveTopology::PointList: return NLS::Render::Settings::EPrimitiveMode::POINTS;
			case PrimitiveTopology::TriangleList:
			default:
				return NLS::Render::Settings::EPrimitiveMode::TRIANGLES;
			}
		}

		NLS::Render::ShaderCompiler::ShaderTargetPlatform ToShaderTargetPlatform(const NativeBackendType backend)
		{
			switch (backend)
			{
			case NativeBackendType::DX12: return NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL;
			case NativeBackendType::Vulkan: return NLS::Render::ShaderCompiler::ShaderTargetPlatform::SPIRV;
			case NativeBackendType::OpenGL: return NLS::Render::ShaderCompiler::ShaderTargetPlatform::GLSL;
			default: return NLS::Render::ShaderCompiler::ShaderTargetPlatform::Unknown;
			}
		}

		ShaderStageDesc BuildLegacyShaderStageDesc(const RHIShaderModuleDesc& desc)
		{
			ShaderStageDesc stageDesc;
			stageDesc.stage = desc.stage;
			stageDesc.targetPlatform = ToShaderTargetPlatform(desc.targetBackend);
			stageDesc.entryPoint = desc.entryPoint;
			stageDesc.bytecode = desc.bytecode;
			return stageDesc;
		}

		GraphicsPipelineDesc BuildLegacyGraphicsPipelineDesc(const RHIGraphicsPipelineDesc& desc)
		{
			GraphicsPipelineDesc legacyDesc;
			legacyDesc.reflection = desc.reflection;
			legacyDesc.primitiveMode = ToPrimitiveMode(desc.primitiveTopology);
			legacyDesc.attachmentLayout.colorAttachmentFormats = desc.renderTargetLayout.colorFormats;
			legacyDesc.attachmentLayout.depthAttachmentFormat = desc.renderTargetLayout.depthFormat;
			legacyDesc.attachmentLayout.hasDepthAttachment = desc.renderTargetLayout.hasDepth;
			legacyDesc.attachmentLayout.sampleCount = desc.renderTargetLayout.sampleCount;
			legacyDesc.rasterState.culling = desc.rasterState.cullEnabled;
			legacyDesc.rasterState.cullFace = desc.rasterState.cullFace;
			legacyDesc.depthStencilState.depthTest = desc.depthStencilState.depthTest;
			legacyDesc.depthStencilState.depthWrite = desc.depthStencilState.depthWrite;
			legacyDesc.depthStencilState.depthCompare = desc.depthStencilState.depthCompare;
			legacyDesc.blendState.enabled = desc.blendState.enabled;
			legacyDesc.blendState.colorWrite = desc.blendState.colorWrite;

			if (desc.vertexShader != nullptr)
				legacyDesc.shaderStages.push_back(BuildLegacyShaderStageDesc(desc.vertexShader->GetDesc()));
			if (desc.fragmentShader != nullptr)
				legacyDesc.shaderStages.push_back(BuildLegacyShaderStageDesc(desc.fragmentShader->GetDesc()));

			if (desc.reflection != nullptr)
			{
				for (const auto& constantBuffer : desc.reflection->constantBuffers)
					++legacyDesc.layout.uniformBufferBindingCount;

				for (const auto& property : desc.reflection->properties)
				{
					switch (property.kind)
					{
					case NLS::Render::Resources::ShaderResourceKind::SampledTexture:
						++legacyDesc.layout.sampledTextureBindingCount;
						break;
					case NLS::Render::Resources::ShaderResourceKind::Sampler:
						++legacyDesc.layout.samplerBindingCount;
						break;
					case NLS::Render::Resources::ShaderResourceKind::StructuredBuffer:
					case NLS::Render::Resources::ShaderResourceKind::StorageBuffer:
						++legacyDesc.layout.storageBufferBindingCount;
						break;
					default:
						break;
					}
				}
			}

			return legacyDesc;
		}

		BufferType InferLegacyBufferType(const RHIBufferDesc& desc)
		{
			if (HasBufferUsage(desc.usage, BufferUsageFlags::Vertex))
				return BufferType::Vertex;
			if (HasBufferUsage(desc.usage, BufferUsageFlags::Index))
				return BufferType::Index;
			if (HasBufferUsage(desc.usage, BufferUsageFlags::Uniform))
				return BufferType::Uniform;
			return BufferType::ShaderStorage;
		}

		BufferUsage InferLegacyBufferUsage(const MemoryUsage memoryUsage)
		{
			switch (memoryUsage)
			{
			case MemoryUsage::CPUToGPU: return BufferUsage::DynamicDraw;
			case MemoryUsage::GPUToCPU: return BufferUsage::StreamDraw;
			case MemoryUsage::GPUOnly:
			default:
				return BufferUsage::StaticDraw;
			}
		}

		BufferUsageFlags InferBufferUsageFlags(const BufferType bufferType)
		{
			switch (bufferType)
			{
			case BufferType::Vertex: return BufferUsageFlags::Vertex;
			case BufferType::Index: return BufferUsageFlags::Index;
			case BufferType::Uniform: return BufferUsageFlags::Uniform;
			case BufferType::ShaderStorage:
			default:
				return BufferUsageFlags::Storage;
			}
		}

		TextureUsageFlags InferTextureUsageFlags(const TextureUsage usage)
		{
			TextureUsageFlags result = TextureUsageFlags::None;
			if (HasUsage(usage, TextureUsage::Sampled))
				result = result | TextureUsageFlags::Sampled;
			if (HasUsage(usage, TextureUsage::ColorAttachment))
				result = result | TextureUsageFlags::ColorAttachment;
			if (HasUsage(usage, TextureUsage::DepthStencilAttachment))
				result = result | TextureUsageFlags::DepthStencilAttachment;
			if (HasUsage(usage, TextureUsage::Storage))
				result = result | TextureUsageFlags::Storage;
			return result;
		}

		RHIBufferDesc BuildCompatibilityBufferDesc(const std::shared_ptr<IRHIBuffer>& legacyBuffer, std::string debugName)
		{
			RHIBufferDesc desc;
			if (legacyBuffer != nullptr)
			{
				desc.size = legacyBuffer->GetSize();
				desc.usage = InferBufferUsageFlags(legacyBuffer->GetBufferType());
			}
			desc.memoryUsage = MemoryUsage::GPUOnly;
			desc.debugName = std::move(debugName);
			return desc;
		}

		RHITextureDesc BuildCompatibilityTextureDesc(const std::shared_ptr<IRHITexture>& legacyTexture, std::string debugName)
		{
			RHITextureDesc desc;
			if (legacyTexture != nullptr)
			{
				const auto& legacyDesc = legacyTexture->GetDesc();
				desc.extent.width = legacyDesc.width;
				desc.extent.height = legacyDesc.height;
				desc.dimension = legacyDesc.dimension;
				desc.format = legacyDesc.format;
				desc.arrayLayers = GetTextureLayerCount(legacyDesc.dimension);
				desc.usage = InferTextureUsageFlags(legacyDesc.usage);
			}
			desc.memoryUsage = MemoryUsage::GPUOnly;
			desc.debugName = std::move(debugName);
			return desc;
		}

		uint32_t MapSetIndexToLegacyBindingSpace(const uint32_t setIndex)
		{
			switch (setIndex)
			{
			case 0u: return BindingPointMap::kFrameBindingSpace;
			case 1u: return BindingPointMap::kMaterialBindingSpace;
			case 2u: return BindingPointMap::kObjectBindingSpace;
			case 3u: return BindingPointMap::kPassBindingSpace;
			default: return setIndex;
			}
		}

		NLS::Render::Resources::ShaderResourceKind ToLegacyShaderResourceKind(const BindingType bindingType)
		{
			switch (bindingType)
			{
			case BindingType::UniformBuffer: return NLS::Render::Resources::ShaderResourceKind::UniformBuffer;
			case BindingType::StorageBuffer: return NLS::Render::Resources::ShaderResourceKind::StorageBuffer;
			case BindingType::Texture: return NLS::Render::Resources::ShaderResourceKind::SampledTexture;
			case BindingType::RWTexture: return NLS::Render::Resources::ShaderResourceKind::StorageBuffer;
			case BindingType::Sampler:
			default:
				return NLS::Render::Resources::ShaderResourceKind::Sampler;
			}
		}

		class CompatibilityBuffer;
		class CompatibilityTexture;

		const IRHIBuffer* ExtractLegacyBufferResource(const std::shared_ptr<RHIBuffer>& buffer);
		const IRHITexture* ExtractLegacyTextureResource(const std::shared_ptr<RHITexture>& texture);
		const IRHITexture* ExtractLegacyTextureResource(const std::shared_ptr<RHITextureView>& textureView);
		std::unique_ptr<NLS::Render::Resources::BindingSetInstance> BuildCompatibilityLegacyBindingSet(const RHIBindingSetDesc& desc);

		class CompatibilityAdapter final : public RHIAdapter
		{
		public:
			explicit CompatibilityAdapter(IRenderDevice& renderDevice)
				: m_backendType(renderDevice.GetNativeDeviceInfo().backend)
				, m_vendor(renderDevice.GetVendor())
				, m_hardware(renderDevice.GetHardware())
			{
			}

			std::string_view GetDebugName() const override { return "CompatibilityAdapter"; }
			NativeBackendType GetBackendType() const override { return m_backendType; }
			std::string_view GetVendor() const override { return m_vendor; }
			std::string_view GetHardware() const override { return m_hardware; }

		private:
			NativeBackendType m_backendType = NativeBackendType::None;
			std::string m_vendor;
			std::string m_hardware;
		};

		class CompatibilityBuffer final : public RHIBuffer
		{
		public:
			CompatibilityBuffer(RHIBufferDesc desc, std::shared_ptr<IRHIBuffer> buffer)
				: m_desc(std::move(desc))
				, m_buffer(std::move(buffer))
			{
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const RHIBufferDesc& GetDesc() const override { return m_desc; }
			ResourceState GetState() const override { return m_state; }
			const std::shared_ptr<IRHIBuffer>& GetLegacyBuffer() const { return m_buffer; }

		private:
			RHIBufferDesc m_desc;
			std::shared_ptr<IRHIBuffer> m_buffer;
			ResourceState m_state = ResourceState::Unknown;
		};

		class CompatibilityTexture final : public RHITexture
		{
		public:
			CompatibilityTexture(RHITextureDesc desc, std::shared_ptr<IRHITexture> texture)
				: m_desc(std::move(desc))
				, m_texture(std::move(texture))
			{
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const RHITextureDesc& GetDesc() const override { return m_desc; }
			ResourceState GetState() const override { return m_state; }
			const std::shared_ptr<IRHITexture>& GetLegacyTexture() const { return m_texture; }

		private:
			RHITextureDesc m_desc;
			std::shared_ptr<IRHITexture> m_texture;
			ResourceState m_state = ResourceState::Unknown;
		};

		class CompatibilityTextureView final : public RHITextureView
		{
		public:
			CompatibilityTextureView(std::shared_ptr<RHITexture> texture, RHITextureViewDesc desc)
				: m_texture(std::move(texture))
				, m_desc(std::move(desc))
			{
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const RHITextureViewDesc& GetDesc() const override { return m_desc; }
			const std::shared_ptr<RHITexture>& GetTexture() const override { return m_texture; }

		private:
			std::shared_ptr<RHITexture> m_texture;
			RHITextureViewDesc m_desc;
		};

		class CompatibilitySampler final : public RHISampler
		{
		public:
			CompatibilitySampler(SamplerDesc desc, std::string debugName)
				: m_desc(std::move(desc))
				, m_debugName(std::move(debugName))
			{
			}

			std::string_view GetDebugName() const override { return m_debugName; }
			const SamplerDesc& GetDesc() const override { return m_desc; }

		private:
			SamplerDesc m_desc;
			std::string m_debugName;
		};

		class CompatibilityBindingLayout final : public RHIBindingLayout
		{
		public:
			explicit CompatibilityBindingLayout(RHIBindingLayoutDesc desc)
				: m_desc(std::move(desc))
			{
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const RHIBindingLayoutDesc& GetDesc() const override { return m_desc; }

		private:
			RHIBindingLayoutDesc m_desc;
		};

		class CompatibilityBindingSet final : public RHIBindingSet
		{
		public:
			explicit CompatibilityBindingSet(RHIBindingSetDesc desc)
				: m_desc(std::move(desc))
			{
				m_ownedLegacyBindingSet = BuildCompatibilityLegacyBindingSet(m_desc);
				m_legacyBindingSet = m_ownedLegacyBindingSet.get();
			}

			explicit CompatibilityBindingSet(const NLS::Render::Resources::BindingSetInstance* legacyBindingSet)
				: m_legacyBindingSet(legacyBindingSet)
			{
				m_desc.debugName = "CompatibilityBindingSet";
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const RHIBindingSetDesc& GetDesc() const override { return m_desc; }
			const NLS::Render::Resources::BindingSetInstance* GetLegacyBindingSet() const { return m_legacyBindingSet; }

		private:
			RHIBindingSetDesc m_desc;
			std::unique_ptr<NLS::Render::Resources::BindingSetInstance> m_ownedLegacyBindingSet;
			const NLS::Render::Resources::BindingSetInstance* m_legacyBindingSet = nullptr;
		};

		const IRHIBuffer* ExtractLegacyBufferResource(const std::shared_ptr<RHIBuffer>& buffer)
		{
			const auto* compatibilityBuffer = buffer != nullptr
				? dynamic_cast<const CompatibilityBuffer*>(buffer.get())
				: nullptr;
			return compatibilityBuffer != nullptr && compatibilityBuffer->GetLegacyBuffer() != nullptr
				? compatibilityBuffer->GetLegacyBuffer().get()
				: nullptr;
		}

		const IRHITexture* ExtractLegacyTextureResource(const std::shared_ptr<RHITexture>& texture)
		{
			const auto* compatibilityTexture = texture != nullptr
				? dynamic_cast<const CompatibilityTexture*>(texture.get())
				: nullptr;
			return compatibilityTexture != nullptr && compatibilityTexture->GetLegacyTexture() != nullptr
				? compatibilityTexture->GetLegacyTexture().get()
				: nullptr;
		}

		const IRHITexture* ExtractLegacyTextureResource(const std::shared_ptr<RHITextureView>& textureView)
		{
			return textureView != nullptr
				? ExtractLegacyTextureResource(textureView->GetTexture())
				: nullptr;
		}

		std::unique_ptr<NLS::Render::Resources::BindingSetInstance> BuildCompatibilityLegacyBindingSet(const RHIBindingSetDesc& desc)
		{
			if (desc.layout == nullptr)
				return nullptr;

			auto legacyBindingSet = std::make_unique<NLS::Render::Resources::BindingSetInstance>();

			NLS::Render::Resources::ResourceBindingLayout legacyLayout;
			legacyLayout.bindings.reserve(desc.layout->GetDesc().entries.size());
			for (const auto& entry : desc.layout->GetDesc().entries)
			{
				legacyLayout.bindings.push_back({
					entry.name,
					ToLegacyShaderResourceKind(entry.type),
					NLS::Render::Resources::UniformType::UNIFORM_FLOAT,
					MapSetIndexToLegacyBindingSpace(entry.set),
					entry.binding,
					static_cast<int32_t>(entry.binding)
				});
			}

			legacyBindingSet->SetLayout(legacyLayout);

			for (const auto& entry : desc.entries)
			{
				const auto layoutEntry = std::find_if(
					desc.layout->GetDesc().entries.begin(),
					desc.layout->GetDesc().entries.end(),
					[&entry](const RHIBindingLayoutEntry& candidate)
					{
						return candidate.binding == entry.binding && candidate.type == entry.type;
					});
				if (layoutEntry == desc.layout->GetDesc().entries.end())
					continue;

				switch (entry.type)
				{
				case BindingType::UniformBuffer:
				case BindingType::StorageBuffer:
					if (const auto* legacyBuffer = ExtractLegacyBufferResource(entry.buffer))
						legacyBindingSet->SetBuffer(layoutEntry->name, legacyBuffer);
					break;
				case BindingType::Texture:
				case BindingType::RWTexture:
					if (const auto* legacyTexture = ExtractLegacyTextureResource(entry.textureView))
						legacyBindingSet->SetResource(layoutEntry->name, legacyTexture);
					break;
				case BindingType::Sampler:
					if (entry.sampler != nullptr)
						legacyBindingSet->SetSampler(layoutEntry->name, entry.sampler->GetDesc());
					break;
				default:
					break;
				}
			}

			return legacyBindingSet;
		}

		std::unique_ptr<NLS::Render::Resources::BindingSetInstance> MergeLegacyBindingSets(
			const std::array<std::shared_ptr<RHIBindingSet>, kRHIMaxBindingSets>& bindingSets)
		{
			auto mergedBindingSet = std::make_unique<NLS::Render::Resources::BindingSetInstance>();
			NLS::Render::Resources::ResourceBindingLayout mergedLayout;

			for (const auto& bindingSet : bindingSets)
			{
				const auto* compatibilityBindingSet = bindingSet != nullptr
					? dynamic_cast<const CompatibilityBindingSet*>(bindingSet.get())
					: nullptr;
				const auto* legacyBindingSet = compatibilityBindingSet != nullptr
					? compatibilityBindingSet->GetLegacyBindingSet()
					: nullptr;
				if (legacyBindingSet == nullptr)
					continue;

				for (const auto& entry : legacyBindingSet->Entries())
				{
					mergedLayout.bindings.push_back({
						entry.name,
						entry.kind,
						NLS::Render::Resources::UniformType::UNIFORM_FLOAT,
						entry.bindingSpace,
						entry.bindingIndex,
						entry.slot
					});
				}
			}

			mergedBindingSet->SetLayout(mergedLayout);

			for (const auto& bindingSet : bindingSets)
			{
				const auto* compatibilityBindingSet = bindingSet != nullptr
					? dynamic_cast<const CompatibilityBindingSet*>(bindingSet.get())
					: nullptr;
				const auto* legacyBindingSet = compatibilityBindingSet != nullptr
					? compatibilityBindingSet->GetLegacyBindingSet()
					: nullptr;
				if (legacyBindingSet == nullptr)
					continue;

				for (const auto& entry : legacyBindingSet->Entries())
				{
					if (entry.hasSampler)
						mergedBindingSet->SetSampler(entry.name, entry.sampler);
					else if (entry.bufferResource != nullptr)
						mergedBindingSet->SetBuffer(entry.name, entry.bufferResource);
					else if (entry.texture != nullptr)
						mergedBindingSet->SetTexture(entry.name, entry.texture);
					else if (entry.resource != nullptr)
						mergedBindingSet->SetResource(entry.name, entry.resource);
				}
			}

			return mergedBindingSet;
		}

		void ApplyLegacyGraphicsState(IRenderDevice& renderDevice, const GraphicsPipelineDesc& pipelineDesc)
		{
			renderDevice.SetColorWriting(
				pipelineDesc.blendState.colorWrite,
				pipelineDesc.blendState.colorWrite,
				pipelineDesc.blendState.colorWrite,
				pipelineDesc.blendState.colorWrite);
			renderDevice.SetDepthWriting(pipelineDesc.depthStencilState.depthWrite);
			renderDevice.SetCapability(NLS::Render::Settings::ERenderingCapability::BLEND, pipelineDesc.blendState.enabled);
			renderDevice.SetCapability(NLS::Render::Settings::ERenderingCapability::CULL_FACE, pipelineDesc.rasterState.culling);
			renderDevice.SetCapability(NLS::Render::Settings::ERenderingCapability::DEPTH_TEST, pipelineDesc.depthStencilState.depthTest);
			renderDevice.SetCapability(NLS::Render::Settings::ERenderingCapability::SCISSOR_TEST, false);
			renderDevice.SetDepthAlgorithm(pipelineDesc.depthStencilState.depthCompare);
			if (pipelineDesc.rasterState.culling)
				renderDevice.SetCullFace(pipelineDesc.rasterState.cullFace);
		}

		class CompatibilityPipelineLayout final : public RHIPipelineLayout
		{
		public:
			explicit CompatibilityPipelineLayout(RHIPipelineLayoutDesc desc)
				: m_desc(std::move(desc))
			{
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const RHIPipelineLayoutDesc& GetDesc() const override { return m_desc; }

		private:
			RHIPipelineLayoutDesc m_desc;
		};

		class CompatibilityShaderModule final : public RHIShaderModule
		{
		public:
			explicit CompatibilityShaderModule(RHIShaderModuleDesc desc)
				: m_desc(std::move(desc))
			{
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const RHIShaderModuleDesc& GetDesc() const override { return m_desc; }

		private:
			RHIShaderModuleDesc m_desc;
		};

		class CompatibilityGraphicsPipeline final : public RHIGraphicsPipeline
		{
		public:
			explicit CompatibilityGraphicsPipeline(RHIGraphicsPipelineDesc desc)
				: m_desc(std::move(desc))
			{
				if (m_desc.reflection != nullptr)
					m_legacyDesc = BuildLegacyGraphicsPipelineDesc(m_desc);
			}

			explicit CompatibilityGraphicsPipeline(GraphicsPipelineDesc legacyDesc)
				: m_legacyDesc(std::move(legacyDesc))
			{
				m_desc.debugName = "CompatibilityLegacyGraphicsPipeline";
				m_desc.primitiveTopology = ToPrimitiveTopology(m_legacyDesc->primitiveMode);
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const RHIGraphicsPipelineDesc& GetDesc() const override { return m_desc; }
			const GraphicsPipelineDesc* GetLegacyDesc() const
			{
				return m_legacyDesc.has_value() ? &m_legacyDesc.value() : nullptr;
			}

		private:
			RHIGraphicsPipelineDesc m_desc;
			std::optional<GraphicsPipelineDesc> m_legacyDesc;
		};

		class CompatibilityComputePipeline final : public RHIComputePipeline
		{
		public:
			explicit CompatibilityComputePipeline(RHIComputePipelineDesc desc)
				: m_desc(std::move(desc))
			{
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const RHIComputePipelineDesc& GetDesc() const override { return m_desc; }

		private:
			RHIComputePipelineDesc m_desc;
		};

		class CompatibilityFence final : public RHIFence
		{
		public:
			explicit CompatibilityFence(std::string debugName) : m_debugName(std::move(debugName)) {}

			std::string_view GetDebugName() const override { return m_debugName; }
			bool IsSignaled() const override { return m_signaled; }
			void Reset() override { m_signaled = false; }
			bool Wait(uint64_t) override { return true; }
			void Signal() { m_signaled = true; }

		private:
			std::string m_debugName;
			bool m_signaled = true;
		};

		class CompatibilitySemaphore final : public RHISemaphore
		{
		public:
			explicit CompatibilitySemaphore(std::string debugName) : m_debugName(std::move(debugName)) {}

			std::string_view GetDebugName() const override { return m_debugName; }
			bool IsSignaled() const override { return m_signaled; }
			void Reset() override { m_signaled = false; }
			void Signal() { m_signaled = true; }

		private:
			std::string m_debugName;
			bool m_signaled = false;
		};

		struct BeginRenderPassCommand
		{
			RHIRenderPassDesc desc;
		};

		struct EndRenderPassCommand {};
		struct SetViewportCommand { RHIViewport viewport; };
		struct SetScissorCommand { RHIRect2D rect; };
		struct BindGraphicsPipelineCommand { std::shared_ptr<RHIGraphicsPipeline> pipeline; };
		struct BindComputePipelineCommand { std::shared_ptr<RHIComputePipeline> pipeline; };
		struct BindBindingSetCommand { uint32_t setIndex = 0; std::shared_ptr<RHIBindingSet> bindingSet; };
		struct PushConstantsCommand { ShaderStageMask stageMask = ShaderStageMask::None; uint32_t offset = 0; std::vector<uint8_t> data; };
		struct BindVertexBufferCommand { uint32_t slot = 0; RHIVertexBufferView view; };
		struct BindIndexBufferCommand { RHIIndexBufferView view; };
		struct DrawCommand { uint32_t vertexCount = 0; uint32_t instanceCount = 1; uint32_t firstVertex = 0; uint32_t firstInstance = 0; };
		struct DrawIndexedCommand { uint32_t indexCount = 0; uint32_t instanceCount = 1; uint32_t firstIndex = 0; int32_t vertexOffset = 0; uint32_t firstInstance = 0; };
		struct DispatchCommand { uint32_t groupCountX = 1; uint32_t groupCountY = 1; uint32_t groupCountZ = 1; };
		struct CopyBufferCommand { std::shared_ptr<RHIBuffer> source; std::shared_ptr<RHIBuffer> destination; RHIBufferCopyRegion region; };
		struct CopyBufferToTextureCommand { RHIBufferToTextureCopyDesc desc; };
		struct CopyTextureCommand { RHITextureCopyDesc desc; };
		struct BarrierCommand { RHIBarrierDesc desc; };

		using CompatibilityCommand = std::variant<
			BeginRenderPassCommand,
			EndRenderPassCommand,
			SetViewportCommand,
			SetScissorCommand,
			BindGraphicsPipelineCommand,
			BindComputePipelineCommand,
			BindBindingSetCommand,
			PushConstantsCommand,
			BindVertexBufferCommand,
			BindIndexBufferCommand,
			DrawCommand,
			DrawIndexedCommand,
			DispatchCommand,
			CopyBufferCommand,
			CopyBufferToTextureCommand,
			CopyTextureCommand,
			BarrierCommand>;

		class CompatibilityCommandBuffer final : public RHICommandBuffer
		{
		public:
			explicit CompatibilityCommandBuffer(std::string debugName) : m_debugName(std::move(debugName)) {}

			std::string_view GetDebugName() const override { return m_debugName; }
			void Begin() override
			{
				m_commands.clear();
				m_recording = true;
			}

			void End() override { m_recording = false; }
			void Reset() override
			{
				m_recording = false;
				m_commands.clear();
			}

			bool IsRecording() const override { return m_recording; }
			void BeginRenderPass(const RHIRenderPassDesc& desc) override { m_commands.emplace_back(BeginRenderPassCommand{ desc }); }
			void EndRenderPass() override { m_commands.emplace_back(EndRenderPassCommand{}); }
			void SetViewport(const RHIViewport& viewport) override { m_commands.emplace_back(SetViewportCommand{ viewport }); }
			void SetScissor(const RHIRect2D& rect) override { m_commands.emplace_back(SetScissorCommand{ rect }); }
			void BindGraphicsPipeline(const std::shared_ptr<RHIGraphicsPipeline>& pipeline) override { m_commands.emplace_back(BindGraphicsPipelineCommand{ pipeline }); }
			void BindComputePipeline(const std::shared_ptr<RHIComputePipeline>& pipeline) override { m_commands.emplace_back(BindComputePipelineCommand{ pipeline }); }
			void BindBindingSet(uint32_t setIndex, const std::shared_ptr<RHIBindingSet>& bindingSet) override { m_commands.emplace_back(BindBindingSetCommand{ setIndex, bindingSet }); }
			void PushConstants(ShaderStageMask stageMask, uint32_t offset, uint32_t size, const void* data) override
			{
				PushConstantsCommand command;
				command.stageMask = stageMask;
				command.offset = offset;
				command.data.resize(size);
				if (size > 0 && data != nullptr)
					std::memcpy(command.data.data(), data, size);
				m_commands.emplace_back(std::move(command));
			}
			void BindVertexBuffer(uint32_t slot, const RHIVertexBufferView& view) override { m_commands.emplace_back(BindVertexBufferCommand{ slot, view }); }
			void BindIndexBuffer(const RHIIndexBufferView& view) override { m_commands.emplace_back(BindIndexBufferCommand{ view }); }
			void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override
			{
				m_commands.emplace_back(DrawCommand{ vertexCount, instanceCount, firstVertex, firstInstance });
			}
			void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override
			{
				m_commands.emplace_back(DrawIndexedCommand{ indexCount, instanceCount, firstIndex, vertexOffset, firstInstance });
			}
			void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override
			{
				m_commands.emplace_back(DispatchCommand{ groupCountX, groupCountY, groupCountZ });
			}
			void CopyBuffer(const std::shared_ptr<RHIBuffer>& source, const std::shared_ptr<RHIBuffer>& destination, const RHIBufferCopyRegion& region) override
			{
				m_commands.emplace_back(CopyBufferCommand{ source, destination, region });
			}
			void CopyBufferToTexture(const RHIBufferToTextureCopyDesc& desc) override { m_commands.emplace_back(CopyBufferToTextureCommand{ desc }); }
			void CopyTexture(const RHITextureCopyDesc& desc) override { m_commands.emplace_back(CopyTextureCommand{ desc }); }
			void Barrier(const RHIBarrierDesc& barrier) override { m_commands.emplace_back(BarrierCommand{ barrier }); }

			void Execute(IRenderDevice& renderDevice) const;

		private:
			std::string m_debugName;
			bool m_recording = false;
			std::vector<CompatibilityCommand> m_commands;
		};

		class CompatibilityCommandPool final : public RHICommandPool
		{
		public:
			CompatibilityCommandPool(QueueType queueType, std::string debugName)
				: m_queueType(queueType)
				, m_debugName(std::move(debugName))
			{
			}

			std::string_view GetDebugName() const override { return m_debugName; }
			QueueType GetQueueType() const override { return m_queueType; }
			std::shared_ptr<RHICommandBuffer> CreateCommandBuffer(std::string debugName) override
			{
				return std::make_shared<CompatibilityCommandBuffer>(std::move(debugName));
			}
			void Reset() override {}

		private:
			QueueType m_queueType = QueueType::Graphics;
			std::string m_debugName;
		};

		class CompatibilitySwapchain final : public RHISwapchain
		{
		public:
			CompatibilitySwapchain(IRenderDevice& renderDevice, SwapchainDesc desc)
				: m_renderDevice(renderDevice)
				, m_desc(std::move(desc))
			{
				m_created = m_renderDevice.CreateSwapchain(m_desc);
				m_imageCount = m_desc.imageCount != 0 ? m_desc.imageCount : 2u;
			}

			~CompatibilitySwapchain() override
			{
				if (m_created)
					m_renderDevice.DestroySwapchain();
			}

			std::string_view GetDebugName() const override { return "CompatibilitySwapchain"; }
			const SwapchainDesc& GetDesc() const override { return m_desc; }
			uint32_t GetImageCount() const override { return m_imageCount; }
			std::optional<RHIAcquiredImage> AcquireNextImage(const std::shared_ptr<RHISemaphore>&, const std::shared_ptr<RHIFence>&) override
			{
				RHIAcquiredImage image;
				image.imageIndex = m_nextImageIndex++ % m_imageCount;
				return image;
			}
			void Resize(uint32_t width, uint32_t height) override
			{
				m_desc.width = width;
				m_desc.height = height;
				m_renderDevice.ResizeSwapchain(width, height);
			}

		private:
			IRenderDevice& m_renderDevice;
			SwapchainDesc m_desc{};
			uint32_t m_imageCount = 0;
			uint32_t m_nextImageIndex = 0;
			bool m_created = false;
		};

		class CompatibilityQueue final : public RHIQueue
		{
		public:
			CompatibilityQueue(IRenderDevice& renderDevice, QueueType type)
				: m_renderDevice(renderDevice)
				, m_type(type)
			{
			}

			std::string_view GetDebugName() const override { return "CompatibilityQueue"; }
			QueueType GetType() const override { return m_type; }
			void Submit(const RHISubmitDesc& submitDesc) override;
			void Present(const RHIPresentDesc& presentDesc) override;

		private:
			IRenderDevice& m_renderDevice;
			QueueType m_type = QueueType::Graphics;
		};

		class CompatibilityDevice final : public RHIDevice
		{
		public:
			explicit CompatibilityDevice(IRenderDevice& renderDevice);

			std::string_view GetDebugName() const override { return "CompatibilityExplicitDevice"; }
			const std::shared_ptr<RHIAdapter>& GetAdapter() const override { return m_adapter; }
			const RHIDeviceCapabilities& GetCapabilities() const override;
			NativeRenderDeviceInfo GetNativeDeviceInfo() const override { return m_renderDevice.GetNativeDeviceInfo(); }
			bool IsBackendReady() const override { return m_renderDevice.IsBackendReady(); }
			std::shared_ptr<RHIQueue> GetQueue(QueueType queueType) override;
			std::shared_ptr<RHISwapchain> CreateSwapchain(const SwapchainDesc& desc) override;
			std::shared_ptr<RHIBuffer> CreateBuffer(const RHIBufferDesc& desc, const void* initialData = nullptr) override;
			std::shared_ptr<RHITexture> CreateTexture(const RHITextureDesc& desc, const void* initialData = nullptr) override;
			std::shared_ptr<RHITextureView> CreateTextureView(const std::shared_ptr<RHITexture>& texture, const RHITextureViewDesc& desc) override;
			std::shared_ptr<RHISampler> CreateSampler(const SamplerDesc& desc, std::string debugName = {}) override;
			std::shared_ptr<RHIBindingLayout> CreateBindingLayout(const RHIBindingLayoutDesc& desc) override;
			std::shared_ptr<RHIBindingSet> CreateBindingSet(const RHIBindingSetDesc& desc) override;
			std::shared_ptr<RHIPipelineLayout> CreatePipelineLayout(const RHIPipelineLayoutDesc& desc) override;
			std::shared_ptr<RHIShaderModule> CreateShaderModule(const RHIShaderModuleDesc& desc) override;
			std::shared_ptr<RHIGraphicsPipeline> CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc) override;
			std::shared_ptr<RHIComputePipeline> CreateComputePipeline(const RHIComputePipelineDesc& desc) override;
			std::shared_ptr<RHICommandPool> CreateCommandPool(QueueType queueType, std::string debugName = {}) override;
			std::shared_ptr<RHIFence> CreateFence(std::string debugName = {}) override;
			std::shared_ptr<RHISemaphore> CreateSemaphore(std::string debugName = {}) override;

		private:
			IRenderDevice& m_renderDevice;
			std::shared_ptr<RHIAdapter> m_adapter;
			mutable RHIDeviceCapabilities m_cachedCapabilities{};
			std::array<std::shared_ptr<RHIQueue>, 3> m_queues{};
		};
	}

	void CompatibilityCommandBuffer::Execute(IRenderDevice& renderDevice) const
	{
		std::shared_ptr<RHIGraphicsPipeline> currentGraphicsPipeline;
		std::array<std::shared_ptr<RHIBindingSet>, kRHIMaxBindingSets> currentBindingSets{};
		uint32_t activeFramebufferId = 0;
		bool ownsActiveFramebuffer = false;

		auto destroyActiveFramebuffer = [&]()
		{
			if (ownsActiveFramebuffer && activeFramebufferId != 0)
				renderDevice.DestroyFramebuffer(activeFramebufferId);
			activeFramebufferId = 0;
			ownsActiveFramebuffer = false;
		};

		auto extractLegacyTextureId = [](const std::shared_ptr<RHITextureView>& view) -> uint32_t
		{
			if (view == nullptr)
				return 0;

			const auto texture = view->GetTexture();
			const auto* compatibilityTexture = texture != nullptr
				? dynamic_cast<const CompatibilityTexture*>(texture.get())
				: nullptr;
			return compatibilityTexture != nullptr && compatibilityTexture->GetLegacyTexture() != nullptr
				? compatibilityTexture->GetLegacyTexture()->GetResourceId()
				: 0u;
		};

		for (const auto& command : m_commands)
		{
			std::visit(
				[&](const auto& typedCommand)
				{
					using T = std::decay_t<decltype(typedCommand)>;
					if constexpr (std::is_same_v<T, BeginRenderPassCommand>)
					{
						destroyActiveFramebuffer();

						NLS::Render::RHI::FramebufferDesc framebufferDesc;
						for (const auto& colorAttachment : typedCommand.desc.colorAttachments)
						{
							const auto textureId = extractLegacyTextureId(colorAttachment.view);
							if (textureId != 0)
								framebufferDesc.colorAttachments.push_back({ textureId, colorAttachment.view->GetDesc().format });
						}

						if (typedCommand.desc.depthStencilAttachment.has_value())
						{
							framebufferDesc.depthStencilTextureId = extractLegacyTextureId(typedCommand.desc.depthStencilAttachment->view);
							if (typedCommand.desc.depthStencilAttachment->view != nullptr)
								framebufferDesc.depthStencilFormat = typedCommand.desc.depthStencilAttachment->view->GetDesc().format;
						}
						framebufferDesc.drawBufferCount = static_cast<uint32_t>(framebufferDesc.colorAttachments.size());

						if (!framebufferDesc.colorAttachments.empty() || framebufferDesc.depthStencilTextureId != 0)
						{
							activeFramebufferId = renderDevice.CreateFramebuffer(framebufferDesc);
							ownsActiveFramebuffer = activeFramebufferId != 0;
							renderDevice.BindFramebuffer(activeFramebufferId);
						}
						else
						{
							renderDevice.BindFramebuffer(0);
						}

						const bool clearColor = std::any_of(
							typedCommand.desc.colorAttachments.begin(),
							typedCommand.desc.colorAttachments.end(),
							[](const RHIRenderPassColorAttachmentDesc& attachment)
							{
								return attachment.loadOp == LoadOp::Clear;
							});
						const bool clearDepth = typedCommand.desc.depthStencilAttachment.has_value() &&
							typedCommand.desc.depthStencilAttachment->depthLoadOp == LoadOp::Clear;
						if (clearColor)
						{
							const auto& clear = typedCommand.desc.colorAttachments.front().clearValue;
							renderDevice.SetClearColor(clear.r, clear.g, clear.b, clear.a);
						}
						if (clearColor || clearDepth)
						{
							// Explicit render pass clears must not inherit stale write-mask or scissor state.
							renderDevice.SetColorWriting(true, true, true, true);
							renderDevice.SetDepthWriting(true);
							renderDevice.SetCapability(NLS::Render::Settings::ERenderingCapability::SCISSOR_TEST, false);
							renderDevice.Clear(clearColor, clearDepth, false);
						}
					}
					else if constexpr (std::is_same_v<T, EndRenderPassCommand>)
					{
						destroyActiveFramebuffer();
						renderDevice.BindFramebuffer(0);
					}
					else if constexpr (std::is_same_v<T, SetViewportCommand>)
					{
						renderDevice.SetViewport(
							static_cast<uint32_t>(typedCommand.viewport.x),
							static_cast<uint32_t>(typedCommand.viewport.y),
							static_cast<uint32_t>(typedCommand.viewport.width),
							static_cast<uint32_t>(typedCommand.viewport.height));
					}
					else if constexpr (std::is_same_v<T, SetScissorCommand>)
					{
						(void)typedCommand;
						// Legacy IRenderDevice has no explicit scissor rectangle setter.
						// Recorded passes currently rely on a full-viewport scissor.
						renderDevice.SetCapability(NLS::Render::Settings::ERenderingCapability::SCISSOR_TEST, false);
					}
					else if constexpr (std::is_same_v<T, BindGraphicsPipelineCommand>)
					{
						currentGraphicsPipeline = typedCommand.pipeline;
					}
					else if constexpr (std::is_same_v<T, BindBindingSetCommand>)
					{
						if (typedCommand.setIndex < currentBindingSets.size())
							currentBindingSets[typedCommand.setIndex] = typedCommand.bindingSet;
					}
					else if constexpr (std::is_same_v<T, BindVertexBufferCommand>)
					{
						if (typedCommand.view.buffer != nullptr)
						{
							const auto* legacyBuffer = dynamic_cast<const CompatibilityBuffer*>(typedCommand.view.buffer.get());
							if (legacyBuffer != nullptr && legacyBuffer->GetLegacyBuffer() != nullptr)
								renderDevice.BindBuffer(BufferType::Vertex, legacyBuffer->GetLegacyBuffer()->GetResourceId());
						}
					}
					else if constexpr (std::is_same_v<T, BindIndexBufferCommand>)
					{
						if (typedCommand.view.buffer != nullptr)
						{
							const auto* legacyBuffer = dynamic_cast<const CompatibilityBuffer*>(typedCommand.view.buffer.get());
							if (legacyBuffer != nullptr && legacyBuffer->GetLegacyBuffer() != nullptr)
								renderDevice.BindBuffer(BufferType::Index, legacyBuffer->GetLegacyBuffer()->GetResourceId());
						}
					}
					else if constexpr (std::is_same_v<T, DrawCommand>)
					{
						const auto* legacyPipeline = currentGraphicsPipeline != nullptr
							? dynamic_cast<const CompatibilityGraphicsPipeline*>(currentGraphicsPipeline.get())
							: nullptr;
						const auto mergedBindingSet = MergeLegacyBindingSets(currentBindingSets);
						if (legacyPipeline != nullptr && legacyPipeline->GetLegacyDesc() != nullptr)
						{
							ApplyLegacyGraphicsState(renderDevice, *legacyPipeline->GetLegacyDesc());
							renderDevice.BindGraphicsPipeline(
								*legacyPipeline->GetLegacyDesc(),
								mergedBindingSet.get());
							if (typedCommand.instanceCount > 1)
								renderDevice.DrawArraysInstanced(ToPrimitiveMode(legacyPipeline->GetDesc().primitiveTopology), typedCommand.vertexCount, typedCommand.instanceCount);
							else
								renderDevice.DrawArrays(ToPrimitiveMode(legacyPipeline->GetDesc().primitiveTopology), typedCommand.vertexCount);
						}
					}
					else if constexpr (std::is_same_v<T, DrawIndexedCommand>)
					{
						const auto* legacyPipeline = currentGraphicsPipeline != nullptr
							? dynamic_cast<const CompatibilityGraphicsPipeline*>(currentGraphicsPipeline.get())
							: nullptr;
						const auto mergedBindingSet = MergeLegacyBindingSets(currentBindingSets);
						if (legacyPipeline != nullptr && legacyPipeline->GetLegacyDesc() != nullptr)
						{
							ApplyLegacyGraphicsState(renderDevice, *legacyPipeline->GetLegacyDesc());
							renderDevice.BindGraphicsPipeline(
								*legacyPipeline->GetLegacyDesc(),
								mergedBindingSet.get());
							if (typedCommand.instanceCount > 1)
								renderDevice.DrawElementsInstanced(ToPrimitiveMode(legacyPipeline->GetDesc().primitiveTopology), typedCommand.indexCount, typedCommand.instanceCount);
							else
								renderDevice.DrawElements(ToPrimitiveMode(legacyPipeline->GetDesc().primitiveTopology), typedCommand.indexCount);
						}
					}
					else
					{
					}
				},
				command);
		}

		destroyActiveFramebuffer();
	}

	void CompatibilityQueue::Submit(const RHISubmitDesc& submitDesc)
	{
		for (const auto& semaphore : submitDesc.waitSemaphores)
		{
			if (const auto compatibilitySemaphore = std::dynamic_pointer_cast<CompatibilitySemaphore>(semaphore))
				compatibilitySemaphore->Reset();
		}

		for (const auto& commandBuffer : submitDesc.commandBuffers)
		{
			const auto compatibilityBuffer = std::dynamic_pointer_cast<CompatibilityCommandBuffer>(commandBuffer);
			if (compatibilityBuffer != nullptr)
				compatibilityBuffer->Execute(m_renderDevice);
		}

		for (const auto& semaphore : submitDesc.signalSemaphores)
		{
			if (const auto compatibilitySemaphore = std::dynamic_pointer_cast<CompatibilitySemaphore>(semaphore))
				compatibilitySemaphore->Signal();
		}

		if (const auto compatibilityFence = std::dynamic_pointer_cast<CompatibilityFence>(submitDesc.signalFence))
			compatibilityFence->Signal();
	}

	void CompatibilityQueue::Present(const RHIPresentDesc&)
	{
		m_renderDevice.PresentSwapchain();
	}

	CompatibilityDevice::CompatibilityDevice(IRenderDevice& renderDevice)
		: m_renderDevice(renderDevice)
		, m_adapter(std::make_shared<CompatibilityAdapter>(renderDevice))
	{
	}

	const RHIDeviceCapabilities& CompatibilityDevice::GetCapabilities() const
	{
		m_cachedCapabilities = m_renderDevice.GetCapabilities();
		return m_cachedCapabilities;
	}

	std::shared_ptr<RHIQueue> CompatibilityDevice::GetQueue(QueueType queueType)
	{
		const auto queueIndex = static_cast<size_t>(queueType);
		if (m_queues[queueIndex] == nullptr)
			m_queues[queueIndex] = std::make_shared<CompatibilityQueue>(m_renderDevice, queueType);
		return m_queues[queueIndex];
	}

	std::shared_ptr<RHISwapchain> CompatibilityDevice::CreateSwapchain(const SwapchainDesc& desc)
	{
		return std::make_shared<CompatibilitySwapchain>(m_renderDevice, desc);
	}

	std::shared_ptr<RHIBuffer> CompatibilityDevice::CreateBuffer(const RHIBufferDesc& desc, const void* initialData)
	{
		auto legacyBuffer = m_renderDevice.CreateBufferResource(InferLegacyBufferType(desc));
		if (legacyBuffer != nullptr)
		{
			m_renderDevice.BindBuffer(legacyBuffer->GetBufferType(), legacyBuffer->GetResourceId());
			m_renderDevice.SetBufferData(legacyBuffer->GetBufferType(), desc.size, initialData, InferLegacyBufferUsage(desc.memoryUsage));
			legacyBuffer->SetSize(desc.size);
		}
		return std::make_shared<CompatibilityBuffer>(desc, std::move(legacyBuffer));
	}

	std::shared_ptr<RHITexture> CompatibilityDevice::CreateTexture(const RHITextureDesc& desc, const void* initialData)
	{
		auto legacyTexture = m_renderDevice.CreateTextureResource(desc.dimension);
		if (legacyTexture != nullptr)
		{
			TextureDesc legacyDesc;
			legacyDesc.width = static_cast<uint16_t>(desc.extent.width);
			legacyDesc.height = static_cast<uint16_t>(desc.extent.height);
			legacyDesc.dimension = desc.dimension;
			legacyDesc.format = desc.format;
			legacyDesc.usage = TextureUsage::Sampled;
			legacyTexture->SetDesc(legacyDesc);
			m_renderDevice.BindTexture(desc.dimension, legacyTexture->GetResourceId());
			m_renderDevice.SetupTexture(legacyDesc, initialData);
		}
		return std::make_shared<CompatibilityTexture>(desc, std::move(legacyTexture));
	}

	std::shared_ptr<RHITextureView> CompatibilityDevice::CreateTextureView(const std::shared_ptr<RHITexture>& texture, const RHITextureViewDesc& desc)
	{
		return std::make_shared<CompatibilityTextureView>(texture, desc);
	}

	std::shared_ptr<RHISampler> CompatibilityDevice::CreateSampler(const SamplerDesc& desc, std::string debugName)
	{
		return std::make_shared<CompatibilitySampler>(desc, std::move(debugName));
	}

	std::shared_ptr<RHIBindingLayout> CompatibilityDevice::CreateBindingLayout(const RHIBindingLayoutDesc& desc)
	{
		return std::make_shared<CompatibilityBindingLayout>(desc);
	}

	std::shared_ptr<RHIBindingSet> CompatibilityDevice::CreateBindingSet(const RHIBindingSetDesc& desc)
	{
		return std::make_shared<CompatibilityBindingSet>(desc);
	}

	std::shared_ptr<RHIPipelineLayout> CompatibilityDevice::CreatePipelineLayout(const RHIPipelineLayoutDesc& desc)
	{
		return std::make_shared<CompatibilityPipelineLayout>(desc);
	}

	std::shared_ptr<RHIShaderModule> CompatibilityDevice::CreateShaderModule(const RHIShaderModuleDesc& desc)
	{
		return std::make_shared<CompatibilityShaderModule>(desc);
	}

	std::shared_ptr<RHIGraphicsPipeline> CompatibilityDevice::CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc)
	{
		return std::make_shared<CompatibilityGraphicsPipeline>(desc);
	}

	std::shared_ptr<RHIComputePipeline> CompatibilityDevice::CreateComputePipeline(const RHIComputePipelineDesc& desc)
	{
		return std::make_shared<CompatibilityComputePipeline>(desc);
	}

	std::shared_ptr<RHICommandPool> CompatibilityDevice::CreateCommandPool(QueueType queueType, std::string debugName)
	{
		return std::make_shared<CompatibilityCommandPool>(queueType, std::move(debugName));
	}

	std::shared_ptr<RHIFence> CompatibilityDevice::CreateFence(std::string debugName)
	{
		return std::make_shared<CompatibilityFence>(std::move(debugName));
	}

	std::shared_ptr<RHISemaphore> CompatibilityDevice::CreateSemaphore(std::string debugName)
	{
		return std::make_shared<CompatibilitySemaphore>(std::move(debugName));
	}

	std::shared_ptr<RHIDevice> CreateCompatibilityExplicitDevice(IRenderDevice& renderDevice)
	{
		return std::make_shared<CompatibilityDevice>(renderDevice);
	}

	std::shared_ptr<RHISampler> CreateCompatibilitySampler(const SamplerDesc& desc, std::string debugName)
	{
		return std::make_shared<CompatibilitySampler>(desc, std::move(debugName));
	}

	std::shared_ptr<RHIBindingLayout> CreateCompatibilityBindingLayout(const RHIBindingLayoutDesc& desc)
	{
		return std::make_shared<CompatibilityBindingLayout>(desc);
	}

	std::shared_ptr<RHIBindingSet> CreateCompatibilityBindingSet(const RHIBindingSetDesc& desc)
	{
		return std::make_shared<CompatibilityBindingSet>(desc);
	}

	std::shared_ptr<RHIPipelineLayout> CreateCompatibilityPipelineLayout(const RHIPipelineLayoutDesc& desc)
	{
		return std::make_shared<CompatibilityPipelineLayout>(desc);
	}

	std::shared_ptr<RHIGraphicsPipeline> CreateCompatibilityGraphicsPipeline(const GraphicsPipelineDesc& legacyDesc)
	{
		return std::make_shared<CompatibilityGraphicsPipeline>(legacyDesc);
	}

	std::shared_ptr<RHIBindingSet> WrapCompatibilityBindingSet(const NLS::Render::Resources::BindingSetInstance* legacyBindingSet)
	{
		return std::make_shared<CompatibilityBindingSet>(legacyBindingSet);
	}

	std::shared_ptr<RHIBuffer> WrapCompatibilityBuffer(const std::shared_ptr<IRHIBuffer>& legacyBuffer, std::string debugName)
	{
		return std::make_shared<CompatibilityBuffer>(BuildCompatibilityBufferDesc(legacyBuffer, std::move(debugName)), legacyBuffer);
	}

	std::shared_ptr<RHITexture> WrapCompatibilityTexture(const std::shared_ptr<IRHITexture>& legacyTexture, std::string debugName)
	{
		return std::make_shared<CompatibilityTexture>(BuildCompatibilityTextureDesc(legacyTexture, std::move(debugName)), legacyTexture);
	}

	std::shared_ptr<RHITextureView> CreateCompatibilityTextureView(const std::shared_ptr<RHITexture>& texture, const RHITextureViewDesc& desc)
	{
		return std::make_shared<CompatibilityTextureView>(texture, desc);
	}

}
