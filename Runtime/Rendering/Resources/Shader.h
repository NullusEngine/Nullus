
#pragma once

#include <map>
#include <memory>
#include <tuple>
#include <vector>

#include "Object/Object.h"
#include "Reflection/Macros.h"
#include "Rendering/RHI/RHITypes.h"
#include "Rendering/Resources/UniformInfo.h"
#include "Rendering/Resources/ShaderReflection.h"
#include "Rendering/Resources/ShaderParameterStruct.h"
#include "Rendering/ShaderCompiler/ShaderCompilationTypes.h"
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

		const UniformInfo* GetUniformInfo(const std::string& p_name) const;
		const ShaderReflection& GetReflection() const;
		const std::vector<ShaderParameterStruct>& GetParameterStructs() const;
		bool HasParameterStructs() const;
		ShaderCompiler::ShaderSourceLanguage GetSourceLanguage() const;
		const ShaderCompiledArtifact* FindCompiledArtifact(
			ShaderCompiler::ShaderStage stage,
			ShaderCompiler::ShaderTargetPlatform targetPlatform) const;
		uint64_t GetInstanceId() const;
		uint64_t GetGeneration() const;
		std::shared_ptr<RHI::RHIShaderModule> GetOrCreateExplicitShaderModule(
			const std::shared_ptr<RHI::RHIDevice>& device,
			ShaderCompiler::ShaderStage stage) const;
#if defined(NLS_ENABLE_TEST_HOOKS)
		void SetReflectionForTesting(ShaderReflection reflection);
#endif

	private:
		Shader(const std::string p_path, ShaderCompiler::ShaderSourceLanguage p_sourceLanguage = ShaderCompiler::ShaderSourceLanguage::HLSL);
		~Shader();
		void RebuildUniformInfosFromReflection();
		void SetReflection(ShaderReflection reflection);
		void SetParameterStructs(std::vector<ShaderParameterStruct> parameterStructs);
		void SetCompiledArtifact(ShaderCompiledArtifact artifact);
		void ClearCompiledArtifacts();

	public:
		std::string path;

	private:
		std::vector<UniformInfo> m_uniforms;
		ShaderCompiler::ShaderSourceLanguage m_sourceLanguage;
		uint64_t m_instanceId = 0u;
		ShaderReflection m_reflection;
		std::vector<ShaderParameterStruct> m_parameterStructs;
		std::vector<ShaderCompiledArtifact> m_compiledArtifacts;
		uint64_t m_generation = 0u;
		mutable std::map<
			std::tuple<uint64_t, RHI::NativeBackendType, ShaderCompiler::ShaderStage, uint64_t>,
			std::shared_ptr<RHI::RHIShaderModule>> m_explicitShaderModules;
	};
}
