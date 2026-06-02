#include "Rendering/Resources/Shader.h"

#include <algorithm>
#include <atomic>
#include <string>

#include "Math/Matrix4.h"
#include "Math/Vector2.h"
#include "Math/Vector3.h"
#include "Math/Vector4.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/Resources/TextureCube.h"

namespace
{
	using ShaderResourceKind = NLS::Render::Resources::ShaderResourceKind;
	using UniformType = NLS::Render::Resources::UniformType;

	NLS::Render::ShaderCompiler::ShaderTargetPlatform ToTargetPlatform(const NLS::Render::RHI::NativeBackendType backend)
	{
		switch (backend)
		{
		case NLS::Render::RHI::NativeBackendType::DX12: return NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL;
		case NLS::Render::RHI::NativeBackendType::Vulkan: return NLS::Render::ShaderCompiler::ShaderTargetPlatform::SPIRV;
		case NLS::Render::RHI::NativeBackendType::OpenGL: return NLS::Render::ShaderCompiler::ShaderTargetPlatform::GLSL;
		default: return NLS::Render::ShaderCompiler::ShaderTargetPlatform::Unknown;
		}
	}

	uint64_t NextShaderInstanceId()
	{
		static std::atomic<uint64_t> nextInstanceId { 1u };
		auto instanceId = nextInstanceId.fetch_add(1u, std::memory_order_relaxed);
		if (instanceId == 0u)
			instanceId = nextInstanceId.fetch_add(1u, std::memory_order_relaxed);
		return instanceId;
	}

	NLS::Render::RHI::ShaderStage ToRHIStage(const NLS::Render::ShaderCompiler::ShaderStage stage)
	{
		switch (stage)
		{
		case NLS::Render::ShaderCompiler::ShaderStage::Vertex: return NLS::Render::RHI::ShaderStage::Vertex;
		case NLS::Render::ShaderCompiler::ShaderStage::Compute: return NLS::Render::RHI::ShaderStage::Compute;
		case NLS::Render::ShaderCompiler::ShaderStage::Pixel:
		default:
			return NLS::Render::RHI::ShaderStage::Fragment;
		}
	}

	std::string ToFingerprintLabel(const NLS::Render::ShaderCompiler::ShaderTargetPlatform targetPlatform)
	{
		switch (targetPlatform)
		{
		case NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL: return "DXIL";
		case NLS::Render::ShaderCompiler::ShaderTargetPlatform::SPIRV: return "SPIRV";
		case NLS::Render::ShaderCompiler::ShaderTargetPlatform::GLSL: return "GLSL";
		case NLS::Render::ShaderCompiler::ShaderTargetPlatform::Unknown:
		default:
			return "Unknown";
		}
	}

	std::any CreateDefaultValue(UniformType type)
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

    uint64_t ResolveDeviceCacheIdentity(
        const std::shared_ptr<NLS::Render::RHI::RHIDevice>& device)
    {
        return device != nullptr ? device->GetCacheIdentity() : 0u;
    }
}

namespace NLS::Render::Resources
{
	std::string BuildShaderArtifactToolchainFingerprint(
		ShaderCompiler::ShaderTargetPlatform targetPlatform,
		std::string_view targetProfile,
		std::string_view entryPoint,
		const ShaderCompiler::ShaderCompilationOutput& output)
	{
		std::string fingerprint = ToFingerprintLabel(targetPlatform);
		fingerprint += "|";
		fingerprint += targetProfile;
		fingerprint += "|";
		fingerprint += entryPoint;
		fingerprint += "|";
		fingerprint += output.cacheKey;
		fingerprint += "|";
		fingerprint += output.artifactPath;
		return fingerprint;
	}

	Shader::Shader(const std::string p_path, ShaderCompiler::ShaderSourceLanguage p_sourceLanguage)
		: path(p_path)
		, m_sourceLanguage(p_sourceLanguage)
		, m_instanceId(NextShaderInstanceId())
	{
	}

	Shader::~Shader() = default;

	const UniformInfo* Shader::GetUniformInfo(const std::string& p_name) const
	{
		const auto found = std::find_if(m_uniforms.begin(), m_uniforms.end(), [&p_name](const UniformInfo& element)
		{
			return p_name == element.name;
		});

		return found != m_uniforms.end() ? &*found : nullptr;
	}

	const ShaderReflection& Shader::GetReflection() const
	{
		return m_reflection;
	}

	const std::vector<ShaderParameterStruct>& Shader::GetParameterStructs() const
	{
		return m_parameterStructs;
	}

	bool Shader::HasParameterStructs() const
	{
		return !m_parameterStructs.empty();
	}

	ShaderCompiler::ShaderSourceLanguage Shader::GetSourceLanguage() const
	{
		return m_sourceLanguage;
	}

	const ShaderCompiledArtifact* Shader::FindCompiledArtifact(ShaderCompiler::ShaderStage stage, ShaderCompiler::ShaderTargetPlatform targetPlatform) const
	{
		const auto found = std::find_if(m_compiledArtifacts.begin(), m_compiledArtifacts.end(), [stage, targetPlatform](const ShaderCompiledArtifact& artifact)
		{
			return artifact.stage == stage && artifact.targetPlatform == targetPlatform;
		});

		return found != m_compiledArtifacts.end() ? &*found : nullptr;
	}

	uint64_t Shader::GetGeneration() const
	{
		return m_generation;
	}

	uint64_t Shader::GetInstanceId() const
	{
		return m_instanceId;
	}

	std::shared_ptr<RHI::RHIShaderModule> Shader::GetOrCreateExplicitShaderModule(
		const std::shared_ptr<RHI::RHIDevice>& device,
		ShaderCompiler::ShaderStage stage) const
	{
		if (device == nullptr)
			return nullptr;

		const auto backend = ResolveDeviceBackendType(device);
		const auto cacheKey = std::make_tuple(ResolveDeviceCacheIdentity(device), backend, stage, m_generation);
		if (const auto found = m_explicitShaderModules.find(cacheKey); found != m_explicitShaderModules.end())
			return found->second;

		const auto targetPlatform = ToTargetPlatform(backend);
		if (targetPlatform == ShaderCompiler::ShaderTargetPlatform::Unknown)
			return nullptr;

		const auto* artifact = FindCompiledArtifact(stage, targetPlatform);
		if (artifact == nullptr)
			return nullptr;

		RHI::RHIShaderModuleDesc desc;
		desc.stage = ToRHIStage(stage);
		desc.targetBackend = backend;
		desc.entryPoint = artifact->entryPoint;
		desc.bytecode = artifact->output.bytecode;
		desc.shaderToolchainFingerprint = BuildShaderArtifactToolchainFingerprint(
			artifact->targetPlatform,
			artifact->targetProfile,
			artifact->entryPoint,
			artifact->output);
		desc.debugName = path + ":" + artifact->entryPoint;

		auto module = device->CreateShaderModule(desc);
		m_explicitShaderModules[cacheKey] = module;
		return module;
	}

	void Shader::RebuildUniformInfosFromReflection()
	{
		m_uniforms.clear();

		for (const auto& property : m_reflection.properties)
		{
			if (property.kind != ShaderResourceKind::Value && property.kind != ShaderResourceKind::SampledTexture)
				continue;

			const auto defaultValue = CreateDefaultValue(property.type);
			if (!defaultValue.has_value())
				continue;

			m_uniforms.push_back({
				property.type,
				property.name,
				property.location,
				defaultValue
			});
		}
	}

	void Shader::SetReflection(ShaderReflection reflection)
	{
		m_reflection = std::move(reflection);
		++m_generation;
		m_explicitShaderModules.clear();
		RebuildUniformInfosFromReflection();
	}

	void Shader::SetParameterStructs(std::vector<ShaderParameterStruct> parameterStructs)
	{
		m_parameterStructs = std::move(parameterStructs);
		++m_generation;
		m_explicitShaderModules.clear();
	}

	void Shader::SetCompiledArtifact(ShaderCompiledArtifact artifact)
	{
		const auto found = std::find_if(m_compiledArtifacts.begin(), m_compiledArtifacts.end(), [&artifact](const ShaderCompiledArtifact& existing)
		{
			return existing.stage == artifact.stage && existing.targetPlatform == artifact.targetPlatform;
		});

		if (found != m_compiledArtifacts.end())
			*found = std::move(artifact);
		else
			m_compiledArtifacts.push_back(std::move(artifact));
		++m_generation;
		m_explicitShaderModules.clear();
	}

	void Shader::ClearCompiledArtifacts()
	{
		m_compiledArtifacts.clear();
		++m_generation;
		m_explicitShaderModules.clear();
	}

#if defined(NLS_ENABLE_TEST_HOOKS)
	void Shader::SetReflectionForTesting(ShaderReflection reflection)
	{
		SetReflection(std::move(reflection));
	}
#endif
}
