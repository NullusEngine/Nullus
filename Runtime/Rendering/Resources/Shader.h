
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <vector>

#include "Object/Object.h"
#include "Reflection/Macros.h"
#include "Rendering/RHI/RHITypes.h"
#include "Rendering/Resources/UniformInfo.h"
#include "Rendering/Resources/ShaderReflection.h"
#include "Rendering/Resources/ShaderParameterStruct.h"
#include "Rendering/ShaderCompiler/ShaderCompilationTypes.h"
#include "Rendering/ShaderLab/ShaderLabTypes.h"
#include "Resources/Shader.generated.h"
#include "RenderDef.h"

namespace NLS::Render::RHI
{
	class RHIDevice;
	class RHIShaderModule;
}

namespace NLS::Render::Resources
{
	namespace Loaders { class ShaderLoader; }

	struct NLS_RENDER_API ShaderCompiledArtifact
	{
		ShaderCompiler::ShaderStage stage = ShaderCompiler::ShaderStage::Vertex;
		ShaderCompiler::ShaderTargetPlatform targetPlatform = ShaderCompiler::ShaderTargetPlatform::Unknown;
		std::string entryPoint;
		std::string targetProfile;
		ShaderCompiler::ShaderCompilationOutput output;
		uint64_t keywordHash = 0u;
	};

	NLS_RENDER_API std::string BuildShaderArtifactToolchainFingerprint(
		ShaderCompiler::ShaderTargetPlatform targetPlatform,
		std::string_view targetProfile,
		std::string_view entryPoint,
		const ShaderCompiler::ShaderCompilationOutput& output);

	CLASS(NLS_RENDER_API Shader) : public NLS::NamedObject
	{
		friend class Loaders::ShaderLoader;

	public:
		GENERATED_BODY()

		std::optional<UniformInfo> GetUniformInfo(const std::string& p_name) const;
		ShaderReflection GetReflection() const;
		std::vector<ShaderParameterStruct> GetParameterStructs() const;
		std::shared_ptr<const ShaderReflection> GetReflectionSnapshot() const;
		bool HasParameterStructs() const;
		ShaderCompiler::ShaderSourceLanguage GetSourceLanguage() const;
		std::optional<NLS::Render::ShaderLab::ShaderLabPassState> GetShaderLabPassState() const;
		std::string GetShaderLabLightMode() const;
		std::string GetImportedArtifactSourcePath() const;
		std::string GetImportedArtifactSubAssetKey() const;
		std::vector<ShaderCompiledArtifact> GetCompiledArtifacts() const;
		uint64_t GetInstanceId() const;
		uint64_t GetGeneration() const;
		std::shared_ptr<RHI::RHIShaderModule> GetOrCreateExplicitShaderModule(
			const std::shared_ptr<RHI::RHIDevice>& device,
			ShaderCompiler::ShaderStage stage,
			uint64_t keywordHash = 0u) const;
#if defined(NLS_ENABLE_TEST_HOOKS)
		static Shader* CreateForTesting(
			const std::string& path,
			ShaderCompiler::ShaderSourceLanguage sourceLanguage = ShaderCompiler::ShaderSourceLanguage::HLSL);
		static void DestroyForTesting(Shader*& shader);
		void SetShaderLabPassStateForTesting(ShaderLab::ShaderLabPassState state);
		void SetImportedShaderLabPassForTesting(
			std::string sourcePath,
			std::string subAssetKey,
			std::string lightMode,
			ShaderLab::ShaderLabPassState state);
		void SetReflectionForTesting(ShaderReflection reflection);
		void SetParameterStructsForTesting(std::vector<ShaderParameterStruct> parameterStructs);
		void ReplaceRuntimeDataForTesting(const Shader& source);
		const ShaderCompiledArtifact* FindCompiledArtifact(
			ShaderCompiler::ShaderStage stage,
			ShaderCompiler::ShaderTargetPlatform targetPlatform,
			uint64_t keywordHash = 0u) const;
		size_t GetRetiredRuntimeDataCountForTesting() const;
#endif

	private:
		struct RuntimeDataSnapshot
		{
			ShaderReflection reflection;
			std::vector<ShaderParameterStruct> parameterStructs;
			std::vector<ShaderCompiledArtifact> compiledArtifacts;
			std::string importedArtifactSourcePath;
			std::string importedArtifactSubAssetKey;
			std::string shaderLabLightMode;
			std::optional<ShaderLab::ShaderLabPassState> shaderLabPassState;
		};
		struct RuntimeData
		{
			std::vector<UniformInfo> uniforms;
			ShaderReflection reflection;
			std::vector<ShaderParameterStruct> parameterStructs;
			std::vector<ShaderCompiledArtifact> compiledArtifacts;
			std::string importedArtifactSourcePath;
			std::string importedArtifactSubAssetKey;
			std::string shaderLabLightMode;
			std::optional<ShaderLab::ShaderLabPassState> shaderLabPassState;
			uint64_t generation = 0u;
		};

		Shader(const std::string p_path, ShaderCompiler::ShaderSourceLanguage p_sourceLanguage = ShaderCompiler::ShaderSourceLanguage::HLSL);
		~Shader();
		static std::vector<UniformInfo> BuildUniformInfosFromReflection(const ShaderReflection& reflection);
		std::shared_ptr<const RuntimeData> GetRuntimeData() const;
		RuntimeDataSnapshot GetRuntimeDataSnapshot() const;
		void ReplaceRuntimeData(RuntimeDataSnapshot snapshot);
		void SetRuntimeData(
			ShaderReflection reflection,
			std::vector<ShaderParameterStruct> parameterStructs,
			std::vector<ShaderCompiledArtifact> compiledArtifacts,
			std::string importedArtifactSourcePath = {},
			std::string importedArtifactSubAssetKey = {},
			std::string shaderLabLightMode = {},
			std::optional<ShaderLab::ShaderLabPassState> shaderLabPassState = std::nullopt);
		void SetReflection(ShaderReflection reflection);
		void SetParameterStructs(std::vector<ShaderParameterStruct> parameterStructs);
		void SetCompiledArtifact(ShaderCompiledArtifact artifact);
		void ClearCompiledArtifacts();
		void SetImportedArtifactIdentity(std::string sourcePath, std::string subAssetKey);
		void SetShaderLabPassState(ShaderLab::ShaderLabPassState state);
		void ClearShaderLabPassState();

	public:
		std::string path;

	private:
		ShaderCompiler::ShaderSourceLanguage m_sourceLanguage;
		uint64_t m_instanceId = 0u;
		std::shared_ptr<const RuntimeData> m_runtimeData;
		mutable std::recursive_mutex m_runtimeMutex;
		mutable std::map<
			std::tuple<uint64_t, RHI::NativeBackendType, ShaderCompiler::ShaderStage, uint64_t, uint64_t>,
			std::shared_ptr<RHI::RHIShaderModule>> m_explicitShaderModules;
	};
}
