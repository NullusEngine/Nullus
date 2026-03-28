#include "Rendering/Resources/Material.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <vector>

#include "Debug/Logger.h"
#include "Math/Vector2.h"
#include "Math/Vector3.h"
#include "Math/Vector4.h"
#include "Math/Matrix4.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/RHI/BindingPointMap.h"
#include "Rendering/RHI/ExplicitRHICompat.h"
#include "Rendering/Resources/Loaders/TextureLoader.h"
#include "Rendering/Resources/TextureCube.h"

namespace
{
	using ShaderSourceLanguage = NLS::Render::ShaderCompiler::ShaderSourceLanguage;
	using ShaderResourceKind = NLS::Render::Resources::ShaderResourceKind;
	using UniformType = NLS::Render::Resources::UniformType;

	bool CanUseCompatibilityRecordedFallback(const std::shared_ptr<NLS::Render::RHI::RHIDevice>& device)
	{
		if (device == nullptr)
			return true;

		const auto nativeBackend = device->GetNativeDeviceInfo().backend;
		switch (nativeBackend)
		{
		case NLS::Render::RHI::NativeBackendType::DX12:
		case NLS::Render::RHI::NativeBackendType::Vulkan:
		case NLS::Render::RHI::NativeBackendType::Metal:
			return false;
		case NLS::Render::RHI::NativeBackendType::None:
		case NLS::Render::RHI::NativeBackendType::OpenGL:
		case NLS::Render::RHI::NativeBackendType::DX11:
		default:
			return true;
		}
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

    bool ShouldLogMaterialBindingDiagnostics()
    {
        static const bool enabled = []()
        {
            if (const char* value = std::getenv("NLS_LOG_MATERIAL_BINDINGS"); value != nullptr)
                return std::strcmp(value, "1") == 0 || _stricmp(value, "true") == 0;
            return false;
        }();
        return enabled;
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

	uint32_t MapBindingSpaceToSetIndex(const uint32_t bindingSpace)
	{
		switch (bindingSpace)
		{
		case NLS::Render::RHI::BindingPointMap::kFrameBindingSpace: return 0u;
		case NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace: return 1u;
		case NLS::Render::RHI::BindingPointMap::kObjectBindingSpace: return 2u;
		case NLS::Render::RHI::BindingPointMap::kPassBindingSpace: return 3u;
		default: return bindingSpace;
		}
	}

	NLS::Render::RHI::BindingType ToBindingType(const ShaderResourceKind kind)
	{
		switch (kind)
		{
		case ShaderResourceKind::UniformBuffer: return NLS::Render::RHI::BindingType::UniformBuffer;
		case ShaderResourceKind::StructuredBuffer:
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
		const NLS::Render::RHI::ShaderStageMask stageMask)
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
		layoutDesc.entries.push_back(std::move(entry));
	}
}

namespace NLS::Render::Resources
{
	Material::Material(Shader* p_shader)
	{
		SetShader(p_shader);
	}

	void Material::SetShader(Shader* p_shader)
	{
		m_shader = p_shader;
		m_explicitBindingLayout.reset();
		m_explicitBindingSet.reset();
		m_explicitPipelineLayout.reset();
		m_explicitBindingLayoutBackend = RHI::NativeBackendType::None;
		m_explicitBindingSetBackend = RHI::NativeBackendType::None;
		m_explicitPipelineLayoutBackend = RHI::NativeBackendType::None;
		m_explicitBindingLayoutDirty = true;
		m_explicitBindingSetDirty = true;

		if (m_shader)
			FillUniform();
		else
		{
			m_parameterBlock.Clear();
			m_bindingLayout.bindings.clear();
			m_bindingSet.Clear();
			m_materialConstantBuffers.clear();
		}
	}

	void Material::FillUniform()
	{
		m_parameterBlock.Clear();
		m_bindingLayout.bindings.clear();
		m_bindingSet.Clear();
		m_materialConstantBuffers.clear();
		m_explicitBindingLayout.reset();
		m_explicitBindingSet.reset();
		m_explicitPipelineLayout.reset();
		m_explicitBindingLayoutBackend = RHI::NativeBackendType::None;
		m_explicitBindingSetBackend = RHI::NativeBackendType::None;
		m_explicitPipelineLayoutBackend = RHI::NativeBackendType::None;
		m_explicitBindingLayoutDirty = true;
		m_explicitBindingSetDirty = true;

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

		m_explicitBindingSet.reset();
		m_explicitBindingSetDirty = true;
		return true;
	}

	void Material::SyncParameterLayout()
	{
		FillUniform();
	}

	void Material::RebuildBindingLayout()
	{
		m_bindingLayout.bindings.clear();
		m_explicitBindingLayout.reset();
		m_explicitBindingSet.reset();
		m_explicitPipelineLayout.reset();
		m_explicitBindingLayoutBackend = RHI::NativeBackendType::None;
		m_explicitBindingSetBackend = RHI::NativeBackendType::None;
		m_explicitPipelineLayoutBackend = RHI::NativeBackendType::None;
		m_explicitBindingLayoutDirty = true;
		m_explicitBindingSetDirty = true;

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
				constantBuffer.bindingIndex,
				static_cast<int32_t>(constantBuffer.bindingIndex)
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
				property.bindingIndex,
				static_cast<int32_t>(property.bindingIndex)
			});
		}

		m_materialLayout.bindings = m_bindingLayout;
	}

	void Material::RebuildBindingSet() const
	{
		m_explicitBindingSet.reset();
		m_explicitPipelineLayout.reset();
		m_explicitPipelineLayoutBackend = RHI::NativeBackendType::None;
		m_explicitBindingSetDirty = true;
		m_bindingSet.SetLayout(m_bindingLayout);

		for (const auto& binding : m_bindingLayout.bindings)
		{
			if (binding.kind == ShaderResourceKind::Sampler)
			{
				m_bindingSet.SetSampler(binding.name, BuildDefaultSamplerDesc(binding.name));
				continue;
			}

			const auto* parameter = m_parameterBlock.TryGet(binding.name);
			if (!parameter)
				continue;

			switch (binding.type)
			{
			case UniformType::UNIFORM_SAMPLER_2D:
				if (parameter->type() == typeid(Texture2D*))
				{
					auto* texture = std::any_cast<Texture2D*>(*parameter);
					m_bindingSet.SetTexture(binding.name, texture != nullptr ? texture : GetDefaultWhiteTexture2D());
				}
				break;
			case UniformType::UNIFORM_SAMPLER_CUBE:
				if (parameter->type() == typeid(TextureCube*))
					m_bindingSet.SetTexture(binding.name, std::any_cast<TextureCube*>(*parameter));
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

			auto& bufferState = m_materialConstantBuffers[constantBuffer.name];
			if (!bufferState.buffer || bufferState.size != constantBuffer.byteSize || bufferState.bindingPoint != bindingPoint)
			{
				bufferState.buffer = std::make_unique<NLS::Render::Buffers::UniformBuffer>(
					constantBuffer.byteSize,
					bindingPoint,
					0,
					Settings::EAccessSpecifier::STREAM_DRAW);
				bufferState.size = constantBuffer.byteSize;
				bufferState.bindingPoint = bindingPoint;
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
			bufferState.buffer->Bind(bindingPoint);

			m_bindingSet.SetBuffer(constantBuffer.name, bufferState.buffer->GetRHIBuffer());
		}
	}

	Shader*& Material::GetShader()
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

	void Material::SetBlendable(bool p_transparent) { m_blendable = p_transparent; }
	void Material::SetBackfaceCulling(bool p_backfaceCulling) { m_backfaceCulling = p_backfaceCulling; }
	void Material::SetFrontfaceCulling(bool p_frontfaceCulling) { m_frontfaceCulling = p_frontfaceCulling; }
	void Material::SetDepthTest(bool p_depthTest) { m_depthTest = p_depthTest; }
	void Material::SetDepthWriting(bool p_depthWriting) { m_depthWriting = p_depthWriting; }
	void Material::SetColorWriting(bool p_colorWriting) { m_colorWriting = p_colorWriting; }
	void Material::SetGPUInstances(int p_instances) { m_gpuInstances = p_instances; }
	bool Material::IsBlendable() const { return m_blendable; }
	bool Material::HasBackfaceCulling() const { return m_backfaceCulling; }
	bool Material::HasFrontfaceCulling() const { return m_frontfaceCulling; }
	bool Material::HasDepthTest() const { return m_depthTest; }
	bool Material::HasDepthWriting() const { return m_depthWriting; }
	bool Material::HasColorWriting() const { return m_colorWriting; }
	int Material::GetGPUInstances() const { return m_gpuInstances; }

	const Data::StateMask Material::GenerateStateMask() const
	{
		Data::StateMask stateMask;
		stateMask.depthWriting = m_depthWriting;
		stateMask.colorWriting = m_colorWriting;
		stateMask.blendable = m_blendable;
		stateMask.depthTest = m_depthTest;
		stateMask.frontfaceCulling = m_frontfaceCulling;
		stateMask.backfaceCulling = m_backfaceCulling;
		return stateMask;
	}

	RHI::GraphicsPipelineDesc Material::BuildGraphicsPipelineDesc() const
	{
		RHI::GraphicsPipelineDesc desc;
		desc.blendState.enabled = m_blendable;
		desc.blendState.colorWrite = m_colorWriting;
		desc.depthStencilState.depthTest = m_depthTest;
		desc.depthStencilState.depthWrite = m_depthWriting;
		desc.attachmentLayout.colorAttachmentFormats = { RHI::TextureFormat::RGBA8 };
		desc.attachmentLayout.depthAttachmentFormat = RHI::TextureFormat::Depth24Stencil8;
		desc.attachmentLayout.sampleCount = 1;
		desc.attachmentLayout.hasDepthAttachment = true;
		desc.rasterState.culling = m_backfaceCulling || m_frontfaceCulling;
		desc.rasterState.cullFace = m_backfaceCulling && m_frontfaceCulling
			? Settings::ECullFace::FRONT_AND_BACK
			: m_frontfaceCulling ? Settings::ECullFace::FRONT : Settings::ECullFace::BACK;
		desc.gpuInstances = m_gpuInstances;
		desc.reflection = m_shader ? &m_shader->GetReflection() : nullptr;

		if (m_shader)
		{
			for (const auto targetPlatform : {
				ShaderCompiler::ShaderTargetPlatform::DXIL,
				ShaderCompiler::ShaderTargetPlatform::SPIRV,
				ShaderCompiler::ShaderTargetPlatform::GLSL })
			{
				for (const auto stage : { ShaderCompiler::ShaderStage::Vertex, ShaderCompiler::ShaderStage::Pixel })
				{
					if (const auto* artifact = m_shader->FindCompiledArtifact(stage, targetPlatform); artifact != nullptr)
					{
						desc.shaderStages.push_back({
							static_cast<RHI::ShaderStage>(artifact->stage),
							artifact->targetPlatform,
							artifact->entryPoint,
							artifact->output.bytecode
						});
					}
				}
			}
		}

		if (m_shader)
		{
			for (const auto& constantBuffer : m_shader->GetReflection().constantBuffers)
				++desc.layout.uniformBufferBindingCount;

			for (const auto& property : m_shader->GetReflection().properties)
			{
				switch (property.kind)
				{
				case ShaderResourceKind::SampledTexture:
					++desc.layout.sampledTextureBindingCount;
					break;
				case ShaderResourceKind::Sampler:
					++desc.layout.samplerBindingCount;
					break;
				case ShaderResourceKind::StructuredBuffer:
				case ShaderResourceKind::StorageBuffer:
					++desc.layout.storageBufferBindingCount;
					break;
				default:
					break;
				}
			}
		}

		return desc;
	}

	RHI::RHIGraphicsPipelineDesc Material::BuildExplicitGraphicsPipelineDesc(
		const std::shared_ptr<RHI::RHIPipelineLayout>& pipelineLayout,
		const std::shared_ptr<RHI::RHIShaderModule>& vertexShader,
		const std::shared_ptr<RHI::RHIShaderModule>& fragmentShader,
		Settings::EPrimitiveMode primitiveMode,
		Settings::EComparaisonAlgorithm depthCompare) const
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
		desc.depthStencilState.depthTest = m_depthTest;
		desc.depthStencilState.depthWrite = m_depthWriting;
		desc.depthStencilState.depthCompare = depthCompare;
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
		return desc;
	}

	Material::ExplicitPipelineState Material::BuildExplicitPipelineState(
		const std::shared_ptr<RHI::RHIPipelineLayout>& pipelineLayout,
		const std::shared_ptr<RHI::RHIShaderModule>& vertexShader,
		const std::shared_ptr<RHI::RHIShaderModule>& fragmentShader,
		Settings::EPrimitiveMode primitiveMode,
		Settings::EComparaisonAlgorithm depthCompare) const
	{
		ExplicitPipelineState state;
		state.pipelineLayout = pipelineLayout;
		state.vertexShader = vertexShader;
		state.fragmentShader = fragmentShader;

		if (state.IsComplete())
		{
			state.pipelineDesc = BuildExplicitGraphicsPipelineDesc(
				state.pipelineLayout,
				state.vertexShader,
				state.fragmentShader,
				primitiveMode,
				depthCompare);
		}

		return state;
	}

	Material::ExplicitPipelineState Material::BuildExplicitPipelineState(
		const std::shared_ptr<RHI::RHIDevice>& device,
		Settings::EPrimitiveMode primitiveMode,
		Settings::EComparaisonAlgorithm depthCompare) const
	{
		const auto pipelineLayout = GetExplicitPipelineLayout(device);
		const auto vertexShader = m_shader != nullptr
			? m_shader->GetOrCreateExplicitShaderModule(device, NLS::Render::ShaderCompiler::ShaderStage::Vertex)
			: nullptr;
		const auto fragmentShader = m_shader != nullptr
			? m_shader->GetOrCreateExplicitShaderModule(device, NLS::Render::ShaderCompiler::ShaderStage::Pixel)
			: nullptr;
		return BuildExplicitPipelineState(
			pipelineLayout,
			vertexShader,
			fragmentShader,
			primitiveMode,
			depthCompare);
	}

	std::shared_ptr<RHI::RHIGraphicsPipeline> Material::BuildRecordedGraphicsPipeline(
		const std::shared_ptr<RHI::RHIDevice>& device,
		Settings::EPrimitiveMode primitiveMode,
		Settings::EComparaisonAlgorithm depthCompare) const
	{
		const auto explicitState = BuildExplicitPipelineState(device, primitiveMode, depthCompare);
		if (device != nullptr && explicitState.IsComplete())
			return device->CreateGraphicsPipeline(explicitState.pipelineDesc);
		if (!CanUseCompatibilityRecordedFallback(device))
			return nullptr;

		auto legacyDesc = BuildGraphicsPipelineDesc();
		legacyDesc.primitiveMode = primitiveMode;
		legacyDesc.depthStencilState.depthCompare = depthCompare;
		return RHI::CreateCompatibilityGraphicsPipeline(legacyDesc);
	}

	std::shared_ptr<RHI::RHIBindingSet> Material::GetRecordedBindingSet(const std::shared_ptr<RHI::RHIDevice>& device) const
	{
		if (const auto explicitBindingSet = GetExplicitBindingSet(device); explicitBindingSet != nullptr)
			return explicitBindingSet;
		if (!CanUseCompatibilityRecordedFallback(device))
			return nullptr;

		return RHI::WrapCompatibilityBindingSet(&GetBindingSetInstance());
	}

	MaterialParameterBlock& Material::GetParameterBlock()
	{
		return m_parameterBlock;
	}

	const MaterialParameterBlock& Material::GetParameterBlock() const
	{
		return m_parameterBlock;
	}

	const ResourceBindingLayout& Material::GetBindingLayout() const
	{
		return m_bindingLayout;
	}

	const MaterialLayout& Material::GetMaterialLayout() const
	{
		return m_materialLayout;
	}

	const BindingSetInstance& Material::GetBindingSetInstance() const
	{
		RebuildBindingSet();
		return m_bindingSet;
	}

	const MaterialResourceSet& Material::GetBindingSet() const
	{
		RebuildBindingSet();
		return m_bindingSet;
	}

	const std::shared_ptr<RHI::RHIBindingLayout>& Material::GetExplicitBindingLayout(const std::shared_ptr<RHI::RHIDevice>& device) const
	{
		const auto backend = device != nullptr ? device->GetNativeDeviceInfo().backend : RHI::NativeBackendType::None;
		if (!m_explicitBindingLayoutDirty &&
			m_explicitBindingLayout != nullptr &&
			m_explicitBindingLayoutBackend == backend)
		{
			return m_explicitBindingLayout;
		}

		m_explicitBindingLayout.reset();
		m_explicitBindingLayoutBackend = backend;
		if (!m_shader)
		{
			m_explicitBindingLayoutDirty = false;
			return m_explicitBindingLayout;
		}

		RHI::RHIBindingLayoutDesc layoutDesc;
		layoutDesc.debugName = path.empty() ? "MaterialBindingLayout" : path + ":MaterialBindingLayout";

		for (const auto& constantBuffer : m_shader->GetReflection().constantBuffers)
		{
			if (constantBuffer.bindingSpace != NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace)
				continue;

			UpsertBindingLayoutEntry(
				layoutDesc,
				constantBuffer.name,
				RHI::BindingType::UniformBuffer,
				MapBindingSpaceToSetIndex(constantBuffer.bindingSpace),
				constantBuffer.bindingIndex,
				1u,
				ToShaderStageMask(constantBuffer.stage));
		}

		for (const auto& property : m_shader->GetReflection().properties)
		{
			if (!ShouldExposeToMaterial(property) ||
				(property.kind != ShaderResourceKind::SampledTexture &&
				 property.kind != ShaderResourceKind::Sampler))
			{
				continue;
			}

			UpsertBindingLayoutEntry(
				layoutDesc,
				property.name,
				ToBindingType(property.kind),
				MapBindingSpaceToSetIndex(property.bindingSpace),
				property.bindingIndex,
				property.arraySize > 0 ? static_cast<uint32_t>(property.arraySize) : 1u,
				ToShaderStageMask(property.stage));
		}

		if (device != nullptr)
			m_explicitBindingLayout = device->CreateBindingLayout(layoutDesc);
		m_explicitBindingLayoutDirty = false;
		return m_explicitBindingLayout;
	}

	const std::shared_ptr<RHI::RHIBindingSet>& Material::GetExplicitBindingSet(const std::shared_ptr<RHI::RHIDevice>& device) const
	{
		const auto backend = device != nullptr ? device->GetNativeDeviceInfo().backend : RHI::NativeBackendType::None;
		if (!m_explicitBindingSetDirty &&
			m_explicitBindingSet != nullptr &&
			m_explicitBindingSetBackend == backend)
		{
			return m_explicitBindingSet;
		}

		RebuildBindingSet();

		const auto& explicitLayout = GetExplicitBindingLayout(device);
		m_explicitBindingSet.reset();
		m_explicitBindingSetBackend = backend;
		if (device == nullptr || explicitLayout == nullptr)
		{
			m_explicitBindingSetDirty = false;
			return m_explicitBindingSet;
		}

		RHI::RHIBindingSetDesc bindingSetDesc;
		bindingSetDesc.layout = explicitLayout;
		bindingSetDesc.debugName = path.empty() ? "MaterialBindingSet" : path + ":MaterialBindingSet";

		for (const auto& entry : explicitLayout->GetDesc().entries)
		{
			RHI::RHIBindingSetEntry bindingEntry;
			bindingEntry.binding = entry.binding;
			bindingEntry.type = entry.type;

			switch (entry.type)
			{
			case RHI::BindingType::UniformBuffer:
			case RHI::BindingType::StorageBuffer:
			{
				auto bufferState = m_materialConstantBuffers.find(entry.name);
				if (bufferState == m_materialConstantBuffers.end() || !bufferState->second.buffer)
				{
					if (ShouldLogMaterialBindingDiagnostics() && m_shader != nullptr && m_shader->path.find("Skybox") != std::string::npos)
					{
						NLS_LOG_INFO(
							"[SkyboxMaterial] Missing explicit buffer for entry \"" + entry.name +
							"\". known buffers=" + std::to_string(m_materialConstantBuffers.size()));
					}
					continue;
				}

				bindingEntry.buffer = bufferState->second.buffer->CreateExplicitSnapshotBuffer(entry.name + "Snapshot");
				if (bindingEntry.buffer == nullptr)
					bindingEntry.buffer = bufferState->second.buffer->GetExplicitRHIBufferHandle();
				if (bindingEntry.buffer == nullptr)
					continue;
				bindingEntry.bufferRange = bufferState->second.size;
				break;
			}
			case RHI::BindingType::Texture:
			case RHI::BindingType::RWTexture:
			{
				const auto* parameter = m_parameterBlock.TryGet(entry.name);
				if (parameter == nullptr)
				{
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
					texture = std::any_cast<TextureCube*>(*parameter);
					break;
				default:
					break;
				}

				if (texture == nullptr || !texture->GetExplicitRHITextureHandle())
				{
					if (ShouldLogMaterialBindingDiagnostics() && m_shader != nullptr && m_shader->path.find("Skybox") != std::string::npos)
						NLS_LOG_INFO("[SkyboxMaterial] Texture entry \"" + entry.name + "\" resolved to null texture");
					continue;
				}

				bindingEntry.textureView = texture->GetOrCreateExplicitTextureView(entry.name + "View");
				if (bindingEntry.textureView == nullptr)
					continue;
				break;
			}
			case RHI::BindingType::Sampler:
			{
				const auto defaultSampler = BuildDefaultSamplerDesc(entry.name);
				const auto* sampler = m_bindingSet.GetSampler(entry.name);
				bindingEntry.sampler = device->CreateSampler(
					sampler != nullptr ? *sampler : defaultSampler,
					entry.name + "Sampler");
				break;
			}
			default:
				continue;
			}

			bindingSetDesc.entries.push_back(std::move(bindingEntry));
		}

		if (ShouldLogMaterialBindingDiagnostics() && m_shader != nullptr && m_shader->path.find("Skybox") != std::string::npos)
			NLS_LOG_INFO("[SkyboxMaterial] Explicit binding set entry count = " + std::to_string(bindingSetDesc.entries.size()));

		m_explicitBindingSet = device->CreateBindingSet(bindingSetDesc);
		m_explicitBindingSetDirty = false;
		return m_explicitBindingSet;
	}

	const std::shared_ptr<RHI::RHIPipelineLayout>& Material::GetExplicitPipelineLayout(const std::shared_ptr<RHI::RHIDevice>& device) const
	{
		const auto backend = device != nullptr ? device->GetNativeDeviceInfo().backend : RHI::NativeBackendType::None;
		if (m_explicitPipelineLayout != nullptr && m_explicitPipelineLayoutBackend == backend)
			return m_explicitPipelineLayout;

		m_explicitPipelineLayout.reset();
		m_explicitPipelineLayoutBackend = backend;
		if (device == nullptr)
			return m_explicitPipelineLayout;

		const auto& materialLayout = GetExplicitBindingLayout(device);
		if (materialLayout == nullptr)
			return m_explicitPipelineLayout;

		RHI::RHIPipelineLayoutDesc pipelineLayoutDesc;
		pipelineLayoutDesc.bindingLayouts.push_back(materialLayout);
		pipelineLayoutDesc.debugName = path.empty() ? "MaterialPipelineLayout" : path + ":MaterialPipelineLayout";
		m_explicitPipelineLayout = device->CreatePipelineLayout(pipelineLayoutDesc);
		return m_explicitPipelineLayout;
	}

	std::map<std::string, std::any>& Material::GetUniformsData()
	{
		return m_parameterBlock.Data();
	}
}
