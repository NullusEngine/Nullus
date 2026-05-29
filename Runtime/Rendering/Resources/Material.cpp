#include "Rendering/Resources/Material.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <vector>

#include "Base/Image.h"
#include "Debug/Logger.h"
#include "Math/Vector2.h"
#include "Math/Vector3.h"
#include "Math/Vector4.h"
#include "Math/Matrix4.h"
#include "Rendering/Buffers/UniformBuffer.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/RHI/BindingPointMap.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/RHI/Core/RHIPipelineStateUtils.h"
#include "Rendering/RHI/Utils/PipelineCache/PipelineCache.h"
#include "Rendering/Resources/Loaders/TextureLoader.h"
#include "Rendering/Resources/MaterialResourceSet.h"
#include "Rendering/Resources/ShaderBindingLayoutUtils.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/Resources/IndexedObjectDataShaderSupport.h"
#include "Rendering/Resources/ShaderParameterStruct.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/Resources/TextureCube.h"

namespace
{
	using ShaderSourceLanguage = NLS::Render::ShaderCompiler::ShaderSourceLanguage;
	using ShaderResourceKind = NLS::Render::Resources::ShaderResourceKind;
	using UniformType = NLS::Render::Resources::UniformType;

	uint64_t NextMaterialInstanceId()
	{
		static std::atomic<uint64_t> nextInstanceId { 1u };
		auto instanceId = nextInstanceId.fetch_add(1u, std::memory_order_relaxed);
		if (instanceId == 0u)
			instanceId = nextInstanceId.fetch_add(1u, std::memory_order_relaxed);
		return instanceId;
	}

	NLS::Render::RHI::SamplerDesc BuildDefaultSamplerDesc(const std::string& bindingName)
	{
		auto toLower = bindingName;
		std::transform(toLower.begin(), toLower.end(), toLower.begin(), [](unsigned char value)
		{
			return static_cast<char>(std::tolower(value));
		});

		NLS::Render::RHI::SamplerDesc desc{};
		desc.minFilter = toLower.find("point") != std::string::npos
			? NLS::Render::RHI::TextureFilter::Nearest
			: NLS::Render::RHI::TextureFilter::Linear;
		desc.magFilter = desc.minFilter;

		const auto wrapMode = toLower.find("clamp") != std::string::npos
			? NLS::Render::RHI::TextureWrap::ClampToEdge
			: NLS::Render::RHI::TextureWrap::Repeat;
		desc.wrapU = wrapMode;
		desc.wrapV = wrapMode;
		desc.wrapW = wrapMode;
		return desc;
	}

	NLS::Render::Resources::Texture2D* GetDefaultWhiteTexture2D()
	{
		static NLS::Render::Resources::Texture2D* texture = []()
		{
			auto* created = NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(255, 255, 255, 255);
			if (created != nullptr)
				created->path = ":Generated/DefaultWhiteTexture";
			return created;
		}();
		return texture;
	}

	NLS::Render::Resources::TextureCube* GetDefaultWhiteTextureCube()
	{
		static NLS::Render::Resources::TextureCube* texture = []() -> NLS::Render::Resources::TextureCube*
		{
			auto* cubeMap = new NLS::Render::Resources::TextureCube();
			std::vector<uint8_t> whitePixel(4, 255);
			std::vector<NLS::Image> images;
			images.reserve(6);
			for (int i = 0; i < 6; ++i)
			{
				images.emplace_back(1, 1, 4);
				images.back().SetData(whitePixel.data());
			}
			std::vector<const NLS::Image*> imagePtrs;
			imagePtrs.reserve(6);
			for (auto& img : images)
				imagePtrs.push_back(&img);
			if (!cubeMap->SetTextureResource(imagePtrs))
			{
				delete cubeMap;
				return nullptr;
			}
			return cubeMap;
		}();
		return texture;
	}

	std::any CreateDefaultMaterialValue(UniformType type)
	{
		switch (type)
		{
		case UniformType::UNIFORM_BOOL: return std::make_any<bool>(false);
		case UniformType::UNIFORM_INT: return std::make_any<int>(0);
		case UniformType::UNIFORM_FLOAT: return std::make_any<float>(0.0f);
		case UniformType::UNIFORM_FLOAT_VEC2: return std::make_any<NLS::Maths::Vector2>(NLS::Maths::Vector2::Zero);
		case UniformType::UNIFORM_FLOAT_VEC3: return std::make_any<NLS::Maths::Vector3>(NLS::Maths::Vector3::Zero);
		case UniformType::UNIFORM_FLOAT_VEC4: return std::make_any<NLS::Maths::Vector4>(NLS::Maths::Vector4::Zero);
		case UniformType::UNIFORM_FLOAT_MAT4: return std::make_any<NLS::Maths::Matrix4>(NLS::Maths::Matrix4::Identity);
		case UniformType::UNIFORM_SAMPLER_2D: return std::make_any<NLS::Render::Resources::Texture2D*>(nullptr);
		case UniformType::UNIFORM_SAMPLER_CUBE: return std::make_any<NLS::Render::Resources::TextureCube*>(nullptr);
		default: return {};
		}
	}

	bool ShouldExposeToMaterial(const NLS::Render::Resources::ShaderPropertyDesc& property)
	{
		return property.bindingSpace == NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace &&
			(property.kind == ShaderResourceKind::Value ||
			 property.kind == ShaderResourceKind::SampledTexture ||
			 property.kind == ShaderResourceKind::Sampler);
	}

	bool IsMaterialTextureBindingName(const std::string& name)
	{
		return name.size() >= 3u && name.rfind("Map") == name.size() - 3u;
	}

	void ApplyPipelineStateOverrides(
		NLS::Render::RHI::RHIGraphicsPipelineDesc& desc,
		const NLS::Render::Resources::MaterialPipelineStateOverrides& overrides)
	{
		const auto colorFormats = overrides.GetColorFormats();
		if (overrides.HasColorFormatsOverride())
		{
			desc.renderTargetLayout.colorFormats.assign(
				colorFormats.begin(),
				colorFormats.end());
			const size_t renderTargetCount = std::max<size_t>(1u, desc.renderTargetLayout.colorFormats.size());
			const auto templateTarget = desc.blendState.renderTargets.empty()
				? NLS::Render::RHI::RHIRenderTargetBlendStateDesc{}
				: desc.blendState.renderTargets.front();
			desc.blendState.renderTargets.assign(renderTargetCount, templateTarget);
		}
		if (overrides.colorWrite.has_value())
		{
			desc.blendState.colorWrite = *overrides.colorWrite;
			const auto writeMask = *overrides.colorWrite
				? NLS::Render::RHI::RHIColorWriteMask::All
				: NLS::Render::RHI::RHIColorWriteMask::None;
			if (desc.blendState.renderTargets.empty())
				desc.blendState.renderTargets.resize(std::max<size_t>(1u, desc.renderTargetLayout.colorFormats.size()));
			for (auto& target : desc.blendState.renderTargets)
				target.colorWriteMask = writeMask;
		}
		if (overrides.blending.has_value())
		{
			desc.blendState.enabled = *overrides.blending;
			if (desc.blendState.renderTargets.empty())
				desc.blendState.renderTargets.resize(std::max<size_t>(1u, desc.renderTargetLayout.colorFormats.size()));
			for (auto& target : desc.blendState.renderTargets)
				target.blendEnable = *overrides.blending;
		}
		if (overrides.depthWrite.has_value())
			desc.depthStencilState.depthWrite = *overrides.depthWrite;
		if (overrides.depthTest.has_value())
			desc.depthStencilState.depthTest = *overrides.depthTest;
		if (overrides.hasDepthAttachment.has_value())
			desc.renderTargetLayout.hasDepth = *overrides.hasDepthAttachment;
		if (overrides.culling.has_value())
			desc.rasterState.cullEnabled = *overrides.culling;
		if (overrides.cullFace.has_value())
			desc.rasterState.cullFace = *overrides.cullFace;

		if (!desc.depthStencilState.depthTest &&
			!desc.depthStencilState.depthWrite &&
			!desc.depthStencilState.stencilTest)
		{
			desc.renderTargetLayout.hasDepth = false;
		}
	}

    bool ShouldLogMaterialBindingDiagnostics()
    {
        return NLS::Render::Settings::GetThreadDiagnosticsSettings().logMaterialBindings;
    }

    NLS::Render::RHI::NativeBackendType ResolveDeviceBackendType(
        const std::shared_ptr<NLS::Render::RHI::RHIDevice>& device)
    {
        if (device == nullptr)
            return NLS::Render::RHI::NativeBackendType::None;

        const auto& adapter = device->GetAdapter();
        return adapter != nullptr
            ? adapter->GetBackendType()
            : NLS::Render::RHI::NativeBackendType::None;
    }

	template<typename T>
	void CopyValueBytes(uint8_t* destination, uint32_t destinationSize, const T& value)
	{
		const auto bytesToCopy = std::min<uint32_t>(destinationSize, static_cast<uint32_t>(sizeof(T)));
		std::memcpy(destination, &value, bytesToCopy);
	}

	bool CopyParameterValueToBuffer(const std::any& value, UniformType type, uint8_t* destination, uint32_t destinationSize)
	{
		switch (type)
		{
		case UniformType::UNIFORM_BOOL:
		case UniformType::UNIFORM_INT:
		{
			int32_t encodedValue = 0;
			if (value.type() == typeid(bool))
				encodedValue = std::any_cast<bool>(value) ? 1 : 0;
			else if (value.type() == typeid(int))
				encodedValue = std::any_cast<int>(value);
			else
				return false;

			CopyValueBytes(destination, destinationSize, encodedValue);
			return true;
		}
		case UniformType::UNIFORM_FLOAT:
			if (value.type() == typeid(float))
			{
				CopyValueBytes(destination, destinationSize, std::any_cast<float>(value));
				return true;
			}
			return false;
		case UniformType::UNIFORM_FLOAT_VEC2:
			if (value.type() == typeid(NLS::Maths::Vector2))
			{
				CopyValueBytes(destination, destinationSize, std::any_cast<NLS::Maths::Vector2>(value));
				return true;
			}
			return false;
		case UniformType::UNIFORM_FLOAT_VEC3:
			if (value.type() == typeid(NLS::Maths::Vector3))
			{
				CopyValueBytes(destination, destinationSize, std::any_cast<NLS::Maths::Vector3>(value));
				return true;
			}
			return false;
		case UniformType::UNIFORM_FLOAT_VEC4:
			if (value.type() == typeid(NLS::Maths::Vector4))
			{
				CopyValueBytes(destination, destinationSize, std::any_cast<NLS::Maths::Vector4>(value));
				return true;
			}
			return false;
		case UniformType::UNIFORM_FLOAT_MAT4:
			if (value.type() == typeid(NLS::Maths::Matrix4))
			{
				CopyValueBytes(destination, destinationSize, std::any_cast<NLS::Maths::Matrix4>(value));
				return true;
			}
			return false;
		default:
			return false;
		}
	}

	std::vector<uint8_t> BuildMaterialConstantBufferData(
		const NLS::Render::Resources::ShaderConstantBufferDesc& constantBuffer,
		const NLS::Render::Resources::MaterialParameterBlock& parameterBlock)
	{
		std::vector<uint8_t> bufferData(constantBuffer.byteSize, 0u);

		for (const auto& member : constantBuffer.members)
		{
			if (member.byteOffset >= bufferData.size() ||
				member.byteOffset + member.byteSize > bufferData.size())
			{
				continue;
			}

			const auto* parameter = parameterBlock.TryGet(member.name);
			if (parameter == nullptr)
				continue;

			CopyParameterValueToBuffer(
				*parameter,
				member.type,
				bufferData.data() + member.byteOffset,
				member.byteSize);
		}

		return bufferData;
	}

	NLS::Render::RHI::ShaderStageMask ToShaderStageMask(const NLS::Render::ShaderCompiler::ShaderStage stage)
	{
		switch (stage)
		{
		case NLS::Render::ShaderCompiler::ShaderStage::Vertex: return NLS::Render::RHI::ShaderStageMask::Vertex;
		case NLS::Render::ShaderCompiler::ShaderStage::Compute: return NLS::Render::RHI::ShaderStageMask::Compute;
		case NLS::Render::ShaderCompiler::ShaderStage::Pixel:
		default:
			return NLS::Render::RHI::ShaderStageMask::Fragment;
		}
	}

	NLS::Render::RHI::BindingType ToBindingType(const ShaderResourceKind kind)
	{
		switch (kind)
		{
		case ShaderResourceKind::UniformBuffer: return NLS::Render::RHI::BindingType::UniformBuffer;
		case ShaderResourceKind::StructuredBuffer: return NLS::Render::RHI::BindingType::StructuredBuffer;
		case ShaderResourceKind::StorageBuffer: return NLS::Render::RHI::BindingType::StorageBuffer;
		case ShaderResourceKind::SampledTexture: return NLS::Render::RHI::BindingType::Texture;
		case ShaderResourceKind::Sampler:
		default:
			return NLS::Render::RHI::BindingType::Sampler;
		}
	}

	void UpsertBindingLayoutEntry(
		NLS::Render::RHI::RHIBindingLayoutDesc& layoutDesc,
		const std::string& name,
		const NLS::Render::RHI::BindingType bindingType,
		const uint32_t setIndex,
		const uint32_t bindingIndex,
		const uint32_t count,
		const NLS::Render::RHI::ShaderStageMask stageMask,
		const uint32_t elementStride = 0u)
	{
		auto existingEntry = std::find_if(
			layoutDesc.entries.begin(),
			layoutDesc.entries.end(),
			[&](const NLS::Render::RHI::RHIBindingLayoutEntry& candidate)
			{
				return candidate.set == setIndex &&
					candidate.binding == bindingIndex &&
					candidate.type == bindingType;
			});
		if (existingEntry != layoutDesc.entries.end())
		{
			existingEntry->stageMask = existingEntry->stageMask | stageMask;
			existingEntry->count = std::max(existingEntry->count, count);
			existingEntry->elementStride = std::max(existingEntry->elementStride, elementStride);
			if (existingEntry->name.empty())
				existingEntry->name = name;
			return;
		}

		NLS::Render::RHI::RHIBindingLayoutEntry entry;
		entry.name = name;
		entry.type = bindingType;
		entry.set = setIndex;
		entry.binding = bindingIndex;
		entry.count = std::max(1u, count);
		entry.stageMask = stageMask;
		entry.elementStride = elementStride;
		layoutDesc.entries.push_back(std::move(entry));
	}

	uint32_t ResolveResourceElementStride(
		const NLS::Render::RHI::BindingType bindingType,
		const uint32_t reflectedByteSize)
	{
		if (bindingType != NLS::Render::RHI::BindingType::StructuredBuffer &&
			bindingType != NLS::Render::RHI::BindingType::StorageBuffer)
		{
			return 0u;
		}

		return reflectedByteSize != 0u ? reflectedByteSize : sizeof(uint32_t);
	}

	std::vector<NLS::Render::RHI::RHIBindingLayoutDesc> BuildExplicitBindingLayoutDescs(
		const NLS::Render::Resources::Shader& shader,
		std::string_view debugNamePrefix)
	{
		if (shader.HasParameterStructs())
			return NLS::Render::Resources::BuildBindingLayoutDescsFromShaderParameters(
				shader.GetParameterStructs(),
				debugNamePrefix);

		return NLS::Render::Resources::BuildExplicitBindingLayoutDescsBySet(
			shader.GetReflection(),
			debugNamePrefix);
	}

}

namespace NLS::Render::Resources
{
	struct Material::MaterialRuntimeState
	{
		struct MaterialConstantBufferState
		{
			std::unique_ptr<NLS::Render::Buffers::UniformBuffer> buffer;
			uint32_t size = 0;
		};

		MaterialResourceSet bindingSet;
		std::map<std::string, MaterialConstantBufferState> materialConstantBuffers;
		std::shared_ptr<RHI::RHIBindingLayout> explicitBindingLayout;
		std::shared_ptr<RHI::RHIBindingSet> explicitBindingSet;
		std::shared_ptr<RHI::RHIPipelineLayout> explicitPipelineLayout;
		std::vector<MaterialBindingDiagnostic> explicitBindingDiagnostics;
		RHI::NativeBackendType explicitBindingLayoutBackend = RHI::NativeBackendType::None;
		RHI::NativeBackendType explicitBindingSetBackend = RHI::NativeBackendType::None;
		RHI::NativeBackendType explicitPipelineLayoutBackend = RHI::NativeBackendType::None;
		uint64_t shaderGeneration = 0u;
		uint64_t explicitBindingSetCreationCount = 0u;
		uint64_t explicitSnapshotBufferCreationCount = 0u;
		bool explicitBindingLayoutDirty = true;
		bool explicitBindingSetDirty = true;
	};

	Material::MaterialRuntimeState& Material::GetRuntimeState() const
	{
		if (!m_runtimeState)
			m_runtimeState = std::make_unique<MaterialRuntimeState>();
		return *m_runtimeState;
	}

	void Material::ResetRuntimeState() const
	{
		auto& state = GetRuntimeState();
		state.bindingSet.Clear();
		state.materialConstantBuffers.clear();
		state.explicitBindingLayout.reset();
		state.explicitBindingSet.reset();
		state.explicitPipelineLayout.reset();
		state.explicitBindingDiagnostics.clear();
		state.explicitBindingLayoutBackend = RHI::NativeBackendType::None;
		state.explicitBindingSetBackend = RHI::NativeBackendType::None;
		state.explicitPipelineLayoutBackend = RHI::NativeBackendType::None;
		state.shaderGeneration = m_shader != nullptr ? m_shader->GetGeneration() : 0u;
		state.explicitBindingLayoutDirty = true;
		state.explicitBindingSetDirty = true;
	}

	void Material::EnsureShaderGenerationCacheCurrent() const
	{
		auto& state = GetRuntimeState();
		const auto currentGeneration = m_shader != nullptr ? m_shader->GetGeneration() : 0u;
		if (state.shaderGeneration == currentGeneration)
			return;

		const auto bindingSetCreationCount = state.explicitBindingSetCreationCount;
		const auto snapshotBufferCreationCount = state.explicitSnapshotBufferCreationCount;
		auto* material = const_cast<Material*>(this);
		const auto previousParameters = material->m_parameterBlock.Data();
		const auto previousTextureResourcePaths = material->m_textureResourcePaths;
		const auto previousSamplerOverrides = material->m_samplerOverrides;

		material->FillUniform();
		GetRuntimeState().explicitBindingSetCreationCount = bindingSetCreationCount;
		GetRuntimeState().explicitSnapshotBufferCreationCount = snapshotBufferCreationCount;
		for (const auto& [name, value] : previousParameters)
		{
			if (material->m_parameterBlock.Contains(name))
				material->m_parameterBlock.Set(name, value);
		}
		material->m_textureResourcePaths = previousTextureResourcePaths;
		material->m_samplerOverrides = previousSamplerOverrides;
		material->RebuildBindingSet();
		GetRuntimeState().shaderGeneration = currentGeneration;
	}

	void Material::InvalidateExplicitBindingSetCache() const
	{
		auto& state = GetRuntimeState();
		state.explicitBindingSet.reset();
		state.explicitPipelineLayout.reset();
		state.explicitBindingDiagnostics.clear();
		state.explicitBindingSetBackend = RHI::NativeBackendType::None;
		state.explicitPipelineLayoutBackend = RHI::NativeBackendType::None;
		state.explicitBindingSetDirty = true;
	}

	Material::Material(Shader* p_shader)
		: m_instanceId(NextMaterialInstanceId())
	{
		m_runtimeState = std::make_unique<MaterialRuntimeState>();
		SetShader(p_shader);
	}

	Material::~Material() = default;

	void Material::SetShader(Shader* p_shader)
	{
		m_shader = p_shader;
		++m_renderStateRevision;
		ResetRuntimeState();

		if (m_shader)
			FillUniform();
		else
		{
			m_parameterBlock.Clear();
			m_textureResourcePaths.clear();
			m_samplerOverrides.clear();
			m_bindingLayout.bindings.clear();
		}
	}

	void Material::FillUniform()
	{
		m_parameterBlock.Clear();
		m_textureResourcePaths.clear();
		m_samplerOverrides.clear();
		m_bindingLayout.bindings.clear();
		ResetRuntimeState();

		if (!m_shader)
			return;

		for (const auto& property : m_shader->GetReflection().properties)
		{
			if (!ShouldExposeToMaterial(property))
				continue;

			if (const auto* uniformInfo = m_shader->GetUniformInfo(property.name); uniformInfo != nullptr)
			{
				m_parameterBlock.Set(property.name, uniformInfo->defaultValue);
				continue;
			}

			if (const auto defaultValue = CreateDefaultMaterialValue(property.type); defaultValue.has_value())
				m_parameterBlock.Set(property.name, defaultValue);
		}

		RebuildBindingLayout();
		RebuildBindingSet();
	}

	const ShaderPropertyDesc* Material::FindMaterialProperty(const std::string& key) const
	{
		if (m_shader == nullptr)
			return nullptr;

		const auto& properties = m_shader->GetReflection().properties;
		const auto found = std::find_if(
			properties.begin(),
			properties.end(),
			[&key](const ShaderPropertyDesc& property)
			{
				return property.name == key && ShouldExposeToMaterial(property);
			});
		return found != properties.end() ? &*found : nullptr;
	}

	bool Material::EnsureMaterialParameterExists(const std::string& key)
	{
		if (m_parameterBlock.Contains(key))
			return true;

		const auto* property = FindMaterialProperty(key);
		if (property == nullptr)
			return false;

		if (const auto* uniformInfo = m_shader->GetUniformInfo(property->name); uniformInfo != nullptr)
			m_parameterBlock.Set(property->name, uniformInfo->defaultValue);
		else if (const auto defaultValue = CreateDefaultMaterialValue(property->type); defaultValue.has_value())
			m_parameterBlock.Set(property->name, defaultValue);
		else
			return false;

		InvalidateExplicitBindingSetCache();
		return true;
	}

	void Material::SyncParameterLayout()
	{
		FillUniform();
	}

	void Material::RebuildBindingLayout()
	{
		m_bindingLayout.bindings.clear();
		ResetRuntimeState();

		if (!m_shader)
			return;

		for (const auto& constantBuffer : m_shader->GetReflection().constantBuffers)
		{
			if (constantBuffer.bindingSpace != NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace)
				continue;

			m_bindingLayout.bindings.push_back({
				constantBuffer.name,
				ShaderResourceKind::UniformBuffer,
				UniformType::UNIFORM_FLOAT,
				constantBuffer.bindingSpace,
				constantBuffer.bindingIndex
			});
		}

		for (const auto& property : m_shader->GetReflection().properties)
		{
			if (!ShouldExposeToMaterial(property) ||
				(property.kind != ShaderResourceKind::SampledTexture && property.kind != ShaderResourceKind::Sampler))
			{
				continue;
			}

			m_bindingLayout.bindings.push_back({
				property.name,
				property.kind,
				property.type,
				property.bindingSpace,
				property.bindingIndex
			});
		}

		m_materialLayout.bindings = m_bindingLayout;
	}

	void Material::RebuildBindingSet() const
	{
		auto& state = GetRuntimeState();
		EnsureShaderGenerationCacheCurrent();
		InvalidateExplicitBindingSetCache();
		state.bindingSet.SetLayout(m_bindingLayout);

		for (const auto& binding : m_bindingLayout.bindings)
		{
			if (binding.kind == ShaderResourceKind::Sampler)
			{
				const auto override = m_samplerOverrides.find(binding.name);
				state.bindingSet.SetSampler(
					binding.name,
					override != m_samplerOverrides.end()
						? override->second
						: BuildDefaultSamplerDesc(binding.name));
				continue;
			}

			const auto* parameter = m_parameterBlock.TryGet(binding.name);
			if (!parameter && !(binding.type == UniformType::UNIFORM_SAMPLER_2D && IsMaterialTextureBindingName(binding.name)))
				continue;

			switch (binding.type)
			{
			case UniformType::UNIFORM_SAMPLER_2D:
				if (parameter && parameter->type() == typeid(Texture2D*))
				{
					auto* texture = std::any_cast<Texture2D*>(*parameter);
					const auto resolvedTexture = texture != nullptr ? texture : GetDefaultWhiteTexture2D();
					state.bindingSet.SetTexture(
						binding.name,
						resolvedTexture != nullptr ? resolvedTexture->GetTextureHandle() : nullptr);
				}
				else if (IsMaterialTextureBindingName(binding.name))
				{
					auto* texture = GetDefaultWhiteTexture2D();
					state.bindingSet.SetTexture(
						binding.name,
						texture != nullptr ? texture->GetTextureHandle() : nullptr);
				}
				break;
			case UniformType::UNIFORM_SAMPLER_CUBE:
				if (parameter->type() == typeid(TextureCube*))
				{
					const auto* texture = std::any_cast<TextureCube*>(*parameter);
					state.bindingSet.SetTexture(
						binding.name,
						texture != nullptr ? texture->GetTextureHandle() : nullptr);
				}
				break;
			default:
				break;
			}
		}

		if (!m_shader)
			return;

		if (ShouldLogMaterialBindingDiagnostics() && m_shader->path.find("Skybox") != std::string::npos)
		{
			NLS_LOG_INFO("[SkyboxMaterial] Reflection constant buffer count = " + std::to_string(m_shader->GetReflection().constantBuffers.size()));
			for (const auto& constantBuffer : m_shader->GetReflection().constantBuffers)
			{
				NLS_LOG_INFO(
					"[SkyboxMaterial] CBuffer name=" + constantBuffer.name +
					" space=" + std::to_string(constantBuffer.bindingSpace) +
					" binding=" + std::to_string(constantBuffer.bindingIndex) +
					" size=" + std::to_string(constantBuffer.byteSize));
				if (constantBuffer.name == "MaterialConstants")
				{
					for (const auto& member : constantBuffer.members)
					{
						NLS_LOG_INFO(
							"[SkyboxMaterial]   member name=" + member.name +
							" type=" + std::to_string(static_cast<int>(member.type)) +
							" offset=" + std::to_string(member.byteOffset) +
							" size=" + std::to_string(member.byteSize));
					}
				}
			}

			if (const auto* useProceduralSky = m_parameterBlock.TryGet("u_UseProceduralSky"); useProceduralSky != nullptr && useProceduralSky->type() == typeid(bool))
				NLS_LOG_INFO(std::string("[SkyboxMaterial] Parameter u_UseProceduralSky=") + (std::any_cast<bool>(*useProceduralSky) ? "true" : "false"));
			else
				NLS_LOG_INFO("[SkyboxMaterial] Parameter u_UseProceduralSky is missing");
		}

		for (const auto& constantBuffer : m_shader->GetReflection().constantBuffers)
		{
			if (constantBuffer.bindingSpace != NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace ||
				constantBuffer.byteSize == 0)
			{
				continue;
			}

			const auto bindingPoint = NLS::Render::RHI::BindingPointMap::GetUniformBufferBindingPoint(
				constantBuffer.bindingSpace,
				constantBuffer.bindingIndex);

			auto& bufferState = state.materialConstantBuffers[constantBuffer.name];
			if (!bufferState.buffer || bufferState.size != constantBuffer.byteSize)
			{
				bufferState.buffer = std::make_unique<NLS::Render::Buffers::UniformBuffer>(
					constantBuffer.byteSize,
					bindingPoint,
					0,
					Settings::EAccessSpecifier::STREAM_DRAW);
				bufferState.size = constantBuffer.byteSize;
			}

			auto bufferData = BuildMaterialConstantBufferData(constantBuffer, m_parameterBlock);
			if (ShouldLogMaterialBindingDiagnostics() && m_shader->path.find("Skybox") != std::string::npos)
			{
				NLS_LOG_INFO(
					"[SkyboxMaterial] Buffer \"" + constantBuffer.name +
					"\" members=" + std::to_string(constantBuffer.members.size()) +
					" bytes=" + std::to_string(bufferData.size()) +
					" firstInt=" + std::to_string(bufferData.size() >= sizeof(int32_t) ? *reinterpret_cast<const int32_t*>(bufferData.data()) : -1));
			}
			if (!bufferData.empty())
				bufferState.buffer->SetRawData(bufferData.data(), static_cast<uint32_t>(bufferData.size()));

			state.bindingSet.SetBuffer(
				constantBuffer.name,
				bufferState.buffer->GetBufferHandle());
		}
	}

	Shader*& Material::GetShader()
	{
		return m_shader;
	}

	const Shader* Material::GetShader() const
	{
		return m_shader;
	}

	bool Material::HasShader() const
	{
		return m_shader != nullptr;
	}

	bool Material::IsValid() const
	{
		return HasShader();
	}

	void Material::SetBlendable(bool p_transparent)
	{
		if (m_blendable == p_transparent)
			return;
		m_blendable = p_transparent;
		++m_renderStateRevision;
	}

	void Material::SetBackfaceCulling(bool p_backfaceCulling)
	{
		if (m_backfaceCulling == p_backfaceCulling)
			return;
		m_backfaceCulling = p_backfaceCulling;
		++m_renderStateRevision;
	}

	void Material::SetFrontfaceCulling(bool p_frontfaceCulling)
	{
		if (m_frontfaceCulling == p_frontfaceCulling)
			return;
		m_frontfaceCulling = p_frontfaceCulling;
		++m_renderStateRevision;
	}

	void Material::SetDepthTest(bool p_depthTest)
	{
		if (m_depthTest == p_depthTest)
			return;
		m_depthTest = p_depthTest;
		++m_renderStateRevision;
	}

	void Material::SetDepthWriting(bool p_depthWriting)
	{
		if (m_depthWriting == p_depthWriting)
			return;
		m_depthWriting = p_depthWriting;
		++m_renderStateRevision;
	}

	void Material::SetColorWriting(bool p_colorWriting)
	{
		if (m_colorWriting == p_colorWriting)
			return;
		m_colorWriting = p_colorWriting;
		++m_renderStateRevision;
	}

	void Material::SetGPUInstances(int p_instances)
	{
		if (m_gpuInstances == p_instances)
			return;
		m_gpuInstances = p_instances;
		++m_renderStateRevision;
	}
	bool Material::IsBlendable() const { return m_blendable; }
	bool Material::HasBackfaceCulling() const { return m_backfaceCulling; }
	bool Material::HasFrontfaceCulling() const { return m_frontfaceCulling; }
	bool Material::HasDepthTest() const { return m_depthTest; }
	bool Material::HasDepthWriting() const { return m_depthWriting; }
	bool Material::HasColorWriting() const { return m_colorWriting; }
	int Material::GetGPUInstances() const { return m_gpuInstances; }

	const Data::StateMask Material::GenerateStateMask() const
	{
		Data::StateMask stateMask{};
		stateMask.depthWriting = m_depthWriting;
		stateMask.colorWriting = m_colorWriting;
		stateMask.blendable = m_blendable;
		stateMask.depthTest = m_depthTest;
		stateMask.frontfaceCulling = m_frontfaceCulling;
		stateMask.backfaceCulling = m_backfaceCulling;
		return stateMask;
	}

	std::shared_ptr<RHI::RHIGraphicsPipeline> Material::BuildRecordedGraphicsPipeline(
		const std::shared_ptr<RHI::RHIDevice>& device,
        const std::shared_ptr<RHI::PipelineCache>& pipelineCache,
		Settings::EPrimitiveMode primitiveMode,
		const Data::PipelineState& pipelineState,
		MaterialPipelineStateOverrides overrides,
		bool* hasPipelineLayout,
		bool* hasVertexShader,
		bool* hasFragmentShader) const
	{
		const auto pipelineLayout = GetExplicitPipelineLayout(device);
		const auto vertexShader = device != nullptr && m_shader != nullptr
			? m_shader->GetOrCreateExplicitShaderModule(device, NLS::Render::ShaderCompiler::ShaderStage::Vertex)
			: nullptr;
		const auto fragmentShader = device != nullptr && m_shader != nullptr
			? m_shader->GetOrCreateExplicitShaderModule(device, NLS::Render::ShaderCompiler::ShaderStage::Pixel)
			: nullptr;

		if (hasPipelineLayout != nullptr)
			*hasPipelineLayout = pipelineLayout != nullptr;
		if (hasVertexShader != nullptr)
			*hasVertexShader = vertexShader != nullptr;
		if (hasFragmentShader != nullptr)
			*hasFragmentShader = fragmentShader != nullptr;

		if (device != nullptr &&
			pipelineLayout != nullptr &&
			vertexShader != nullptr &&
			fragmentShader != nullptr)
		{
			RHI::RHIGraphicsPipelineDesc desc;
			desc.pipelineLayout = pipelineLayout;
			desc.vertexShader = vertexShader;
			desc.fragmentShader = fragmentShader;
			desc.reflection = m_shader ? &m_shader->GetReflection() : nullptr;
			desc.rasterState.cullEnabled = m_backfaceCulling || m_frontfaceCulling;
			desc.rasterState.cullFace = m_backfaceCulling && m_frontfaceCulling
				? Settings::ECullFace::FRONT_AND_BACK
				: m_frontfaceCulling ? Settings::ECullFace::FRONT : Settings::ECullFace::BACK;
			desc.blendState.enabled = m_blendable;
			desc.blendState.colorWrite = m_colorWriting;
			desc.blendState.renderTargets = { RHI::RHIRenderTargetBlendStateDesc{} };
			desc.blendState.renderTargets[0].blendEnable = m_blendable;
			desc.blendState.renderTargets[0].colorWriteMask = m_colorWriting
				? RHI::RHIColorWriteMask::All
				: RHI::RHIColorWriteMask::None;
			desc.depthStencilState.depthTest = m_depthTest;
			desc.depthStencilState.depthWrite = m_depthWriting;
			switch (primitiveMode)
			{
			case Settings::EPrimitiveMode::LINES:
				desc.primitiveTopology = RHI::PrimitiveTopology::LineList;
				break;
			case Settings::EPrimitiveMode::POINTS:
				desc.primitiveTopology = RHI::PrimitiveTopology::PointList;
				break;
			case Settings::EPrimitiveMode::TRIANGLES:
			default:
				desc.primitiveTopology = RHI::PrimitiveTopology::TriangleList;
				break;
			}
			desc.renderTargetLayout.colorFormats = { RHI::TextureFormat::RGBA8 };
			desc.renderTargetLayout.depthFormat = RHI::TextureFormat::Depth24Stencil8;
			desc.renderTargetLayout.hasDepth = true;
			desc.renderTargetLayout.sampleCount = 1;
			desc.debugName = "RecordedGraphicsPipeline";
			desc.vertexBuffers.push_back({ 0u, static_cast<uint32_t>(sizeof(NLS::Render::Geometry::Vertex)), false });
			desc.vertexAttributes = {
				{ 0u, 0u, 0u, 12u },
				{ 1u, 0u, 12u, 8u },
				{ 2u, 0u, 20u, 12u },
				{ 3u, 0u, 32u, 12u },
				{ 4u, 0u, 44u, 12u }
			};
			RHI::ApplyPipelineStateToGraphicsPipelineDesc(pipelineState, desc);
			ApplyPipelineStateOverrides(desc, overrides);
			if (!desc.renderTargetLayout.hasDepth)
			{
				desc.depthStencilState.depthTest = false;
				desc.depthStencilState.depthWrite = false;
				desc.depthStencilState.stencilTest = false;
			}
			if (pipelineCache == nullptr)
				return nullptr;

			const auto cacheKey = RHI::BuildGraphicsPipelineCacheKey(desc);
			return pipelineCache->GetOrCreateGraphicsPipeline(
				cacheKey,
				[device, desc]()
				{
					return device->CreateGraphicsPipeline(desc);
				},
				RHI::PipelineCacheRequestMode::Runtime);
		}

		return nullptr;
	}

	std::shared_ptr<RHI::RHIBindingSet> Material::GetRecordedBindingSet(const std::shared_ptr<RHI::RHIDevice>& device) const
	{
		return GetExplicitBindingSet(device);
	}

	const MaterialParameterBlock& Material::GetParameterBlock() const
	{
		return m_parameterBlock;
	}

	const MaterialResourceSet& Material::GetBindingSet() const
	{
		RebuildBindingSet();
		return GetRuntimeState().bindingSet;
	}

	const std::shared_ptr<RHI::RHIBindingLayout>& Material::GetExplicitBindingLayout(const std::shared_ptr<RHI::RHIDevice>& device) const
	{
		EnsureShaderGenerationCacheCurrent();
		auto& state = GetRuntimeState();
		const auto backend = ResolveDeviceBackendType(device);
		if (!state.explicitBindingLayoutDirty &&
			state.explicitBindingLayout != nullptr &&
			state.explicitBindingLayoutBackend == backend)
		{
			return state.explicitBindingLayout;
		}

		state.explicitBindingLayout.reset();
		state.explicitBindingLayoutBackend = backend;
		if (!m_shader)
		{
			state.explicitBindingLayoutDirty = false;
			return state.explicitBindingLayout;
		}

		if (!m_shader->HasParameterStructs())
		{
			const auto validation = ValidateShaderBindingReflection(m_shader->GetReflection());
			if (validation.HasErrors())
			{
				for (const auto& diagnostic : validation.diagnostics)
				{
					if (diagnostic.severity == ShaderBindingValidationSeverity::Error)
					{
						state.explicitBindingDiagnostics.push_back({
							MaterialBindingDiagnosticSeverity::Error,
							{},
							diagnostic.message
						});
					}
				}
				state.explicitBindingLayoutDirty = false;
				return state.explicitBindingLayout;
			}
		}

		const auto layoutDescs = BuildExplicitBindingLayoutDescs(
			*m_shader,
			path.empty() ? "Material" : path);
		constexpr uint32_t materialSetIndex = NLS::Render::RHI::BindingPointMap::kMaterialDescriptorSet;
		if (device != nullptr &&
			layoutDescs.size() > materialSetIndex &&
			!layoutDescs[materialSetIndex].entries.empty())
		{
			auto layoutDesc = layoutDescs[materialSetIndex];
			layoutDesc.debugName = path.empty() ? "MaterialBindingLayout" : path + ":MaterialBindingLayout";
			state.explicitBindingLayout = device->CreateBindingLayout(layoutDesc);
		}
		state.explicitBindingLayoutDirty = false;
		return state.explicitBindingLayout;
	}

	const std::shared_ptr<RHI::RHIBindingSet>& Material::GetExplicitBindingSet(const std::shared_ptr<RHI::RHIDevice>& device) const
	{
		EnsureShaderGenerationCacheCurrent();
		auto& state = GetRuntimeState();
		const auto backend = ResolveDeviceBackendType(device);
		if (!state.explicitBindingSetDirty &&
			state.explicitBindingSet != nullptr &&
			state.explicitBindingSetBackend == backend)
		{
			return state.explicitBindingSet;
		}

		RebuildBindingSet();

		const auto& explicitLayout = GetExplicitBindingLayout(device);
		state.explicitBindingSet.reset();
		state.explicitBindingSetBackend = backend;
		if (device == nullptr || explicitLayout == nullptr)
		{
			state.explicitBindingSetDirty = false;
			return state.explicitBindingSet;
		}

		RHI::RHIBindingSetDesc bindingSetDesc;
		bindingSetDesc.layout = explicitLayout;
		bindingSetDesc.debugName = path.empty() ? "MaterialBindingSet" : path + ":MaterialBindingSet";
		if (!state.explicitBindingDiagnostics.empty())
			state.explicitBindingDiagnostics.clear();

		auto addBindingDiagnostic = [&state](MaterialBindingDiagnosticSeverity severity, std::string bindingName, std::string message)
		{
			state.explicitBindingDiagnostics.push_back({
				severity,
				std::move(bindingName),
				std::move(message)
			});
		};

		for (const auto& entry : explicitLayout->GetDesc().entries)
		{
			RHI::RHIBindingSetEntry bindingEntry;
			bindingEntry.binding = entry.binding;
			bindingEntry.type = entry.type;

			switch (entry.type)
			{
			case RHI::BindingType::UniformBuffer:
			case RHI::BindingType::StructuredBuffer:
			case RHI::BindingType::StorageBuffer:
			{
				auto bufferState = state.materialConstantBuffers.find(entry.name);
				if (bufferState == state.materialConstantBuffers.end() || !bufferState->second.buffer)
				{
					addBindingDiagnostic(
						MaterialBindingDiagnosticSeverity::Error,
						entry.name,
						"Material binding \"" + entry.name + "\" is missing required buffer resource.");
					if (ShouldLogMaterialBindingDiagnostics() && m_shader != nullptr && m_shader->path.find("Skybox") != std::string::npos)
					{
						NLS_LOG_INFO(
							"[SkyboxMaterial] Missing explicit buffer for entry \"" + entry.name +
							"\". known buffers=" + std::to_string(state.materialConstantBuffers.size()));
					}
					continue;
				}

				bindingEntry.buffer = bufferState->second.buffer->CreateExplicitSnapshotBuffer(entry.name + "Snapshot");
				if (bindingEntry.buffer != nullptr)
					++state.explicitSnapshotBufferCreationCount;
				if (bindingEntry.buffer == nullptr)
				bindingEntry.buffer = bufferState->second.buffer->GetBufferHandle();
				if (bindingEntry.buffer == nullptr)
				{
					addBindingDiagnostic(
						MaterialBindingDiagnosticSeverity::Error,
						entry.name,
						"Material binding \"" + entry.name + "\" resolved to a null buffer descriptor.");
					continue;
				}
				bindingEntry.bufferRange = bufferState->second.size;
				bindingEntry.elementStride = entry.elementStride;
				break;
			}
			case RHI::BindingType::Texture:
			case RHI::BindingType::RWTexture:
			{
				const auto* parameter = m_parameterBlock.TryGet(entry.name);
				if (parameter == nullptr)
				{
					if (IsMaterialTextureBindingName(entry.name))
					{
						const auto* texture = GetDefaultWhiteTexture2D();
						if (texture != nullptr && texture->GetTextureHandle())
						{
							bindingEntry.textureView = texture->GetOrCreateExplicitTextureView(entry.name + "View");
							if (bindingEntry.textureView != nullptr)
								break;
						}
					}

					addBindingDiagnostic(
						MaterialBindingDiagnosticSeverity::Error,
						entry.name,
						"Material binding \"" + entry.name + "\" is missing required texture parameter.");
					if (ShouldLogMaterialBindingDiagnostics() && m_shader != nullptr && m_shader->path.find("Skybox") != std::string::npos)
						NLS_LOG_INFO("[SkyboxMaterial] Missing texture parameter for entry \"" + entry.name + "\"");
					continue;
				}

				const Texture* texture = nullptr;
				switch (parameter->type() == typeid(Texture2D*) ? UniformType::UNIFORM_SAMPLER_2D :
					parameter->type() == typeid(TextureCube*) ? UniformType::UNIFORM_SAMPLER_CUBE :
					UniformType::UNIFORM_FLOAT)
				{
				case UniformType::UNIFORM_SAMPLER_2D:
				{
					auto* texture2D = std::any_cast<Texture2D*>(*parameter);
					texture = texture2D != nullptr ? texture2D : GetDefaultWhiteTexture2D();
					break;
				}
				case UniformType::UNIFORM_SAMPLER_CUBE:
				{
					auto* textureCube = std::any_cast<TextureCube*>(*parameter);
					texture = textureCube != nullptr ? textureCube : GetDefaultWhiteTextureCube();
					break;
				}
				default:
					break;
				}

				if (texture == nullptr || !texture->GetTextureHandle())
				{
					addBindingDiagnostic(
						MaterialBindingDiagnosticSeverity::Error,
						entry.name,
						"Material binding \"" + entry.name + "\" resolved to a null texture descriptor.");
					if (ShouldLogMaterialBindingDiagnostics() && m_shader != nullptr && m_shader->path.find("Skybox") != std::string::npos)
						NLS_LOG_INFO("[SkyboxMaterial] Texture entry \"" + entry.name + "\" resolved to null texture or null handle, skipping");
					continue;
				}

				bindingEntry.textureView = texture->GetOrCreateExplicitTextureView(entry.name + "View");
				if (bindingEntry.textureView == nullptr)
				{
					addBindingDiagnostic(
						MaterialBindingDiagnosticSeverity::Error,
						entry.name,
						"Material binding \"" + entry.name + "\" resolved to a null texture view descriptor.");
					if (ShouldLogMaterialBindingDiagnostics() && m_shader != nullptr && m_shader->path.find("Skybox") != std::string::npos)
						NLS_LOG_INFO("[SkyboxMaterial] Texture entry \"" + entry.name + "\" GetOrCreateExplicitTextureView returned null, skipping");
					continue;
				}

				if (ShouldLogMaterialBindingDiagnostics() && m_shader != nullptr && m_shader->path.find("Skybox") != std::string::npos)
					NLS_LOG_INFO("[SkyboxMaterial] Texture entry \"" + entry.name + "\" added to binding set");
				break;
			}
			case RHI::BindingType::Sampler:
			{
				const auto defaultSampler = BuildDefaultSamplerDesc(entry.name);
				const auto* sampler = state.bindingSet.GetSampler(entry.name);
				bindingEntry.sampler = device->CreateSampler(
					sampler != nullptr ? *sampler : defaultSampler,
					entry.name + "Sampler");
				if (bindingEntry.sampler == nullptr)
				{
					addBindingDiagnostic(
						MaterialBindingDiagnosticSeverity::Error,
						entry.name,
						"Material binding \"" + entry.name + "\" resolved to a null sampler descriptor.");
					continue;
				}
				break;
			}
			default:
				continue;
			}

			bindingSetDesc.entries.push_back(std::move(bindingEntry));
		}

		if (ShouldLogMaterialBindingDiagnostics() && m_shader != nullptr && m_shader->path.find("Skybox") != std::string::npos)
			NLS_LOG_INFO("[SkyboxMaterial] Explicit binding set entry count = " + std::to_string(bindingSetDesc.entries.size()));

		state.explicitBindingSet = device->CreateBindingSet(bindingSetDesc);
		if (state.explicitBindingSet != nullptr)
			++state.explicitBindingSetCreationCount;
		state.explicitBindingSetDirty = false;
		return state.explicitBindingSet;
	}

	const std::shared_ptr<RHI::RHIPipelineLayout>& Material::GetExplicitPipelineLayout(const std::shared_ptr<RHI::RHIDevice>& device) const
	{
		EnsureShaderGenerationCacheCurrent();
		auto& state = GetRuntimeState();
		auto backend = ResolveDeviceBackendType(device);
		if (device != nullptr)
		{
			const auto nativeInfo = device->GetNativeDeviceInfo();
			if (nativeInfo.backend != RHI::NativeBackendType::None)
				backend = nativeInfo.backend;
		}
		if (state.explicitPipelineLayout != nullptr && state.explicitPipelineLayoutBackend == backend)
			return state.explicitPipelineLayout;

		state.explicitPipelineLayout.reset();
		state.explicitPipelineLayoutBackend = backend;
		if (device == nullptr)
			return state.explicitPipelineLayout;

		if (m_shader == nullptr)
			return state.explicitPipelineLayout;

		const auto layoutDescs = BuildExplicitBindingLayoutDescs(
			*m_shader,
			path.empty() ? "Material" : path);
		if (layoutDescs.empty())
			return state.explicitPipelineLayout;

		const bool requiresIndexedObjectData = ShaderSupportsIndexedObjectData(*m_shader);
		if (requiresIndexedObjectData && !BackendSupportsIndexedObjectDataPushConstants(backend))
			return state.explicitPipelineLayout;

		RHI::RHIPipelineLayoutDesc pipelineLayoutDesc;
		pipelineLayoutDesc.debugName = path.empty() ? "MaterialPipelineLayout" : path + ":MaterialPipelineLayout";
		if (requiresIndexedObjectData)
		{
			pipelineLayoutDesc.pushConstants.push_back({
				RHI::ShaderStageMask::Vertex,
				0u,
				sizeof(uint32_t),
				1u,
				NLS::Render::RHI::BindingPointMap::kObjectBindingSpace
			});
		}
		pipelineLayoutDesc.bindingLayouts.reserve(layoutDescs.size());
		for (const auto& layoutDesc : layoutDescs)
		{
			if (layoutDesc.entries.empty())
				continue;

			pipelineLayoutDesc.bindingLayouts.push_back(device->CreateBindingLayout(layoutDesc));
		}
		state.explicitPipelineLayout = device->CreatePipelineLayout(pipelineLayoutDesc);
		return state.explicitPipelineLayout;
	}

	const std::vector<MaterialBindingDiagnostic>& Material::GetLastExplicitBindingDiagnostics() const
	{
		return GetRuntimeState().explicitBindingDiagnostics;
	}

	bool Material::HasExplicitBindingErrors() const
	{
		const auto& diagnostics = GetRuntimeState().explicitBindingDiagnostics;
		return std::any_of(
			diagnostics.begin(),
			diagnostics.end(),
			[](const MaterialBindingDiagnostic& diagnostic)
			{
				return diagnostic.severity == MaterialBindingDiagnosticSeverity::Error;
			});
	}

	bool Material::RequiresPassDescriptorSet() const
	{
		EnsureShaderGenerationCacheCurrent();
		if (m_shader == nullptr)
			return false;

		if (m_shader->HasParameterStructs())
		{
			const auto layoutDescs = BuildExplicitBindingLayoutDescs(
				*m_shader,
				path.empty() ? "Material" : path);
			constexpr uint32_t passSetIndex = NLS::Render::RHI::BindingPointMap::kPassDescriptorSet;
			return layoutDescs.size() > passSetIndex && !layoutDescs[passSetIndex].entries.empty();
		}

		const auto& reflection = m_shader->GetReflection();
		for (const auto& constantBuffer : reflection.constantBuffers)
		{
			if (constantBuffer.bindingSpace == NLS::Render::RHI::BindingPointMap::kPassBindingSpace)
				return true;
		}

		for (const auto& property : reflection.properties)
		{
			if (property.bindingSpace == NLS::Render::RHI::BindingPointMap::kPassBindingSpace)
				return true;
		}

		return false;
	}

	void Material::SetRawParameter(const std::string& name, std::any value)
	{
		m_parameterBlock.Set(name, std::move(value));
		InvalidateExplicitBindingSetCache();
	}

	void Material::MarkParametersDirty()
	{
		m_parameterBlock.MarkDirty();
		InvalidateExplicitBindingSetCache();
	}

#if defined(NLS_ENABLE_TEST_HOOKS)
	uint64_t Material::GetCachedShaderGenerationForTesting() const
	{
		return GetRuntimeState().shaderGeneration;
	}
#endif

	void Material::SetTextureResourcePath(const std::string& name, std::string path)
	{
		if (name.empty())
			return;

		if (path.empty())
		{
			ClearTextureResourcePath(name);
			return;
		}

		m_textureResourcePaths[name] = std::move(path);
	}

	void Material::ClearTextureResourcePath(const std::string& name)
	{
		m_textureResourcePaths.erase(name);
	}

	std::string Material::GetTextureResourcePath(const std::string& name) const
	{
		const auto found = m_textureResourcePaths.find(name);
		return found != m_textureResourcePaths.end() ? found->second : std::string {};
	}

	const std::map<std::string, std::string>& Material::GetTextureResourcePaths() const
	{
		return m_textureResourcePaths;
	}

	void Material::SetSamplerOverride(const std::string& name, const RHI::SamplerDesc& sampler)
	{
		if (name.empty())
			return;

		m_samplerOverrides[name] = sampler;
		InvalidateExplicitBindingSetCache();
	}

	void Material::ClearSamplerOverride(const std::string& name)
	{
		if (m_samplerOverrides.erase(name) > 0u)
			InvalidateExplicitBindingSetCache();
	}

	void Material::ClearSamplerOverrides()
	{
		if (m_samplerOverrides.empty())
			return;

		m_samplerOverrides.clear();
		InvalidateExplicitBindingSetCache();
	}

	const RHI::SamplerDesc* Material::GetSamplerOverride(const std::string& name) const
	{
		const auto found = m_samplerOverrides.find(name);
		return found != m_samplerOverrides.end() ? &found->second : nullptr;
	}

	uint64_t Material::GetInstanceId() const
	{
		return m_instanceId;
	}

	uint64_t Material::GetParameterRevision() const
	{
		return m_parameterBlock.GetRevision();
	}

	uint64_t Material::GetRenderStateRevision() const
	{
		return m_renderStateRevision;
	}

	uint64_t Material::GetExplicitBindingSetCreationCount() const
	{
		return GetRuntimeState().explicitBindingSetCreationCount;
	}

	uint64_t Material::GetExplicitSnapshotBufferCreationCount() const
	{
		return GetRuntimeState().explicitSnapshotBufferCreationCount;
	}

	const std::map<std::string, std::any>& Material::GetUniformsData() const
	{
		return m_parameterBlock.Data();
	}
}
