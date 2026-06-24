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

        const auto nativeBackend = device->GetNativeDeviceInfo().backend;
        if (nativeBackend != NLS::Render::RHI::NativeBackendType::None)
            return nativeBackend;

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
		, m_runtimeData(std::make_shared<RuntimeData>())
	{
	}

	Shader::~Shader() = default;

	std::optional<UniformInfo> Shader::GetUniformInfo(const std::string& p_name) const
	{
		const auto data = GetRuntimeData();
		const auto found = std::find_if(data->uniforms.begin(), data->uniforms.end(), [&p_name](const UniformInfo& element)
		{
			return p_name == element.name;
		});

		return found != data->uniforms.end()
			? std::optional<UniformInfo>(*found)
			: std::nullopt;
	}

	ShaderReflection Shader::GetReflection() const
	{
		return GetRuntimeData()->reflection;
	}

	std::vector<ShaderParameterStruct> Shader::GetParameterStructs() const
	{
		return GetRuntimeData()->parameterStructs;
	}

	std::shared_ptr<const ShaderReflection> Shader::GetReflectionSnapshot() const
	{
		const auto data = GetRuntimeData();
		return std::shared_ptr<const ShaderReflection>(data, &data->reflection);
	}

	bool Shader::HasParameterStructs() const
	{
		return !GetRuntimeData()->parameterStructs.empty();
	}

	ShaderCompiler::ShaderSourceLanguage Shader::GetSourceLanguage() const
	{
		return m_sourceLanguage;
	}

	std::optional<NLS::Render::ShaderLab::ShaderLabPassState> Shader::GetShaderLabPassState() const
	{
		return GetRuntimeData()->shaderLabPassState;
	}

	std::string Shader::GetShaderLabLightMode() const
	{
		return GetRuntimeData()->shaderLabLightMode;
	}

	std::string Shader::GetImportedArtifactSourcePath() const
	{
		return GetRuntimeData()->importedArtifactSourcePath;
	}

	std::string Shader::GetImportedArtifactSubAssetKey() const
	{
		return GetRuntimeData()->importedArtifactSubAssetKey;
	}

	std::vector<ShaderCompiledArtifact> Shader::GetCompiledArtifacts() const
	{
		return GetRuntimeData()->compiledArtifacts;
	}

	uint64_t Shader::GetGeneration() const
	{
		return GetRuntimeData()->generation;
	}

	uint64_t Shader::GetInstanceId() const
	{
		return m_instanceId;
	}

	std::shared_ptr<RHI::RHIShaderModule> Shader::GetOrCreateExplicitShaderModule(
		const std::shared_ptr<RHI::RHIDevice>& device,
		ShaderCompiler::ShaderStage stage,
		uint64_t keywordHash) const
	{
		if (device == nullptr)
			return nullptr;

		const auto backend = ResolveDeviceBackendType(device);
		const auto data = GetRuntimeData();
		const auto cacheKey = std::make_tuple(ResolveDeviceCacheIdentity(device), backend, stage, keywordHash, data->generation);
		{
			std::lock_guard lock(m_runtimeMutex);
			if (const auto found = m_explicitShaderModules.find(cacheKey); found != m_explicitShaderModules.end())
				return found->second;
		}

		const auto targetPlatform = ToTargetPlatform(backend);
		if (targetPlatform == ShaderCompiler::ShaderTargetPlatform::Unknown)
			return nullptr;

		const auto artifact = std::find_if(data->compiledArtifacts.begin(), data->compiledArtifacts.end(), [stage, targetPlatform, keywordHash](const ShaderCompiledArtifact& candidate)
		{
			return candidate.stage == stage &&
				candidate.targetPlatform == targetPlatform &&
				candidate.keywordHash == keywordHash;
		});
		if (artifact == data->compiledArtifacts.end())
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
		desc.debugName = path + ":" + artifact->entryPoint + ":" + std::to_string(artifact->keywordHash);

		auto module = device->CreateShaderModule(desc);
		std::lock_guard lock(m_runtimeMutex);
		m_explicitShaderModules[cacheKey] = module;
		return module;
	}

	std::vector<UniformInfo> Shader::BuildUniformInfosFromReflection(const ShaderReflection& reflection)
	{
		std::vector<UniformInfo> uniforms;

		for (const auto& property : reflection.properties)
		{
			if (property.kind != ShaderResourceKind::Value && property.kind != ShaderResourceKind::SampledTexture)
				continue;

			const auto defaultValue = CreateDefaultValue(property.type);
			if (!defaultValue.has_value())
				continue;

			uniforms.push_back({
				property.type,
				property.name,
				property.location,
				defaultValue
			});
		}

		return uniforms;
	}

	std::shared_ptr<const Shader::RuntimeData> Shader::GetRuntimeData() const
	{
		std::lock_guard lock(m_runtimeMutex);
		return m_runtimeData;
	}

	Shader::RuntimeDataSnapshot Shader::GetRuntimeDataSnapshot() const
	{
		const auto data = GetRuntimeData();
		return {
			data->reflection,
			data->parameterStructs,
			data->compiledArtifacts,
			data->importedArtifactSourcePath,
			data->importedArtifactSubAssetKey,
			data->shaderLabLightMode,
			data->shaderLabPassState
		};
	}

	void Shader::ReplaceRuntimeData(RuntimeDataSnapshot snapshot)
	{
		SetRuntimeData(
			std::move(snapshot.reflection),
			std::move(snapshot.parameterStructs),
			std::move(snapshot.compiledArtifacts),
			std::move(snapshot.importedArtifactSourcePath),
			std::move(snapshot.importedArtifactSubAssetKey),
			std::move(snapshot.shaderLabLightMode),
			std::move(snapshot.shaderLabPassState));
	}

	void Shader::SetRuntimeData(
		ShaderReflection reflection,
		std::vector<ShaderParameterStruct> parameterStructs,
		std::vector<ShaderCompiledArtifact> compiledArtifacts,
		std::string importedArtifactSourcePath,
		std::string importedArtifactSubAssetKey,
		std::string shaderLabLightMode,
		std::optional<ShaderLab::ShaderLabPassState> shaderLabPassState)
	{
		auto next = std::make_shared<RuntimeData>();
		next->reflection = std::move(reflection);
		next->parameterStructs = std::move(parameterStructs);
		next->compiledArtifacts = std::move(compiledArtifacts);
		next->importedArtifactSourcePath = std::move(importedArtifactSourcePath);
		next->importedArtifactSubAssetKey = std::move(importedArtifactSubAssetKey);
		next->shaderLabLightMode = std::move(shaderLabLightMode);
		next->shaderLabPassState = std::move(shaderLabPassState);
		next->uniforms = BuildUniformInfosFromReflection(next->reflection);

		std::lock_guard lock(m_runtimeMutex);
		next->generation = m_runtimeData ? m_runtimeData->generation + 1u : 1u;
		m_runtimeData = std::move(next);
		m_explicitShaderModules.clear();
	}

	void Shader::SetReflection(ShaderReflection reflection)
	{
		auto snapshot = GetRuntimeDataSnapshot();
		snapshot.reflection = std::move(reflection);
		ReplaceRuntimeData(std::move(snapshot));
	}

	void Shader::SetParameterStructs(std::vector<ShaderParameterStruct> parameterStructs)
	{
		auto snapshot = GetRuntimeDataSnapshot();
		snapshot.parameterStructs = std::move(parameterStructs);
		ReplaceRuntimeData(std::move(snapshot));
	}

	void Shader::SetCompiledArtifact(ShaderCompiledArtifact artifact)
	{
		auto snapshot = GetRuntimeDataSnapshot();
		const auto found = std::find_if(snapshot.compiledArtifacts.begin(), snapshot.compiledArtifacts.end(), [&artifact](const ShaderCompiledArtifact& existing)
		{
			return existing.stage == artifact.stage &&
				existing.targetPlatform == artifact.targetPlatform &&
				existing.keywordHash == artifact.keywordHash;
		});

		if (found != snapshot.compiledArtifacts.end())
			*found = std::move(artifact);
		else
			snapshot.compiledArtifacts.push_back(std::move(artifact));
		ReplaceRuntimeData(std::move(snapshot));
	}

	void Shader::ClearCompiledArtifacts()
	{
		auto snapshot = GetRuntimeDataSnapshot();
		snapshot.compiledArtifacts.clear();
		ReplaceRuntimeData(std::move(snapshot));
	}

	void Shader::SetImportedArtifactIdentity(std::string sourcePath, std::string subAssetKey)
	{
		auto snapshot = GetRuntimeDataSnapshot();
		snapshot.importedArtifactSourcePath = std::move(sourcePath);
		snapshot.importedArtifactSubAssetKey = std::move(subAssetKey);
		ReplaceRuntimeData(std::move(snapshot));
	}

	void Shader::SetShaderLabPassState(ShaderLab::ShaderLabPassState state)
	{
		auto snapshot = GetRuntimeDataSnapshot();
		snapshot.shaderLabPassState = std::move(state);
		ReplaceRuntimeData(std::move(snapshot));
	}

	void Shader::ClearShaderLabPassState()
	{
		auto snapshot = GetRuntimeDataSnapshot();
		if (!snapshot.shaderLabPassState.has_value())
			return;

		snapshot.shaderLabPassState.reset();
		ReplaceRuntimeData(std::move(snapshot));
	}

#if defined(NLS_ENABLE_TEST_HOOKS)
	const ShaderCompiledArtifact* Shader::FindCompiledArtifact(
		ShaderCompiler::ShaderStage stage,
		ShaderCompiler::ShaderTargetPlatform targetPlatform,
		uint64_t keywordHash) const
	{
		const auto data = GetRuntimeData();
		const auto found = std::find_if(data->compiledArtifacts.begin(), data->compiledArtifacts.end(), [stage, targetPlatform, keywordHash](const ShaderCompiledArtifact& artifact)
		{
			return artifact.stage == stage &&
				artifact.targetPlatform == targetPlatform &&
				artifact.keywordHash == keywordHash;
		});

		if (found != data->compiledArtifacts.end())
			return &*found;

		return nullptr;
	}

	Shader* Shader::CreateForTesting(
		const std::string& path,
		ShaderCompiler::ShaderSourceLanguage sourceLanguage)
	{
		return new Shader(path, sourceLanguage);
	}

	void Shader::DestroyForTesting(Shader*& shader)
	{
		delete shader;
		shader = nullptr;
	}

	void Shader::SetShaderLabPassStateForTesting(ShaderLab::ShaderLabPassState state)
	{
		SetShaderLabPassState(std::move(state));
	}

	void Shader::SetImportedShaderLabPassForTesting(
		std::string sourcePath,
		std::string subAssetKey,
		std::string lightMode,
		ShaderLab::ShaderLabPassState state)
	{
		auto snapshot = GetRuntimeDataSnapshot();
		snapshot.importedArtifactSourcePath = std::move(sourcePath);
		snapshot.importedArtifactSubAssetKey = std::move(subAssetKey);
		snapshot.shaderLabLightMode = std::move(lightMode);
		snapshot.shaderLabPassState = std::move(state);
		ReplaceRuntimeData(std::move(snapshot));
	}

	void Shader::SetReflectionForTesting(ShaderReflection reflection)
	{
		SetReflection(std::move(reflection));
	}

	void Shader::SetParameterStructsForTesting(std::vector<ShaderParameterStruct> parameterStructs)
	{
		SetParameterStructs(std::move(parameterStructs));
	}

	void Shader::ReplaceRuntimeDataForTesting(const Shader& source)
	{
		ReplaceRuntimeData(source.GetRuntimeDataSnapshot());
	}

	size_t Shader::GetRetiredRuntimeDataCountForTesting() const
	{
		return 0u;
	}
#endif
}
