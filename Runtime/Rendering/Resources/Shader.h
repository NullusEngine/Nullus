
#pragma once

#include <map>
#include <memory>
#include <tuple>
#include <vector>

#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/Resources/UniformInfo.h"
#include "Rendering/Resources/ShaderReflection.h"
#include "Rendering/ShaderCompiler/ShaderCompilationTypes.h"
#include "RenderDef.h"

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

	class NLS_RENDER_API Shader
	{
		friend class Loaders::ShaderLoader;

	public:
		const UniformInfo* GetUniformInfo(const std::string& p_name) const;
		const ShaderReflection& GetReflection() const;
		ShaderCompiler::ShaderSourceLanguage GetSourceLanguage() const;
		const ShaderCompiledArtifact* FindCompiledArtifact(
			ShaderCompiler::ShaderStage stage,
			ShaderCompiler::ShaderTargetPlatform targetPlatform) const;
		uint64_t GetGeneration() const;
		std::shared_ptr<RHI::RHIShaderModule> GetOrCreateExplicitShaderModule(
			const std::shared_ptr<RHI::RHIDevice>& device,
			ShaderCompiler::ShaderStage stage) const;

	private:
		Shader(const std::string p_path, ShaderCompiler::ShaderSourceLanguage p_sourceLanguage = ShaderCompiler::ShaderSourceLanguage::HLSL);
		~Shader();
		void RebuildUniformInfosFromReflection();
		void SetReflection(ShaderReflection reflection);
		void SetCompiledArtifact(ShaderCompiledArtifact artifact);
		void ClearCompiledArtifacts();

	public:
		const std::string path;

	private:
		std::vector<UniformInfo> m_uniforms;
		ShaderCompiler::ShaderSourceLanguage m_sourceLanguage;
		ShaderReflection m_reflection;
		std::vector<ShaderCompiledArtifact> m_compiledArtifacts;
		uint64_t m_generation = 0u;
		mutable std::map<std::tuple<RHI::NativeBackendType, ShaderCompiler::ShaderStage, uint64_t>, std::shared_ptr<RHI::RHIShaderModule>> m_explicitShaderModules;
	};
}
