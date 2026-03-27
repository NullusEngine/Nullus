#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Rendering/RenderDef.h"
#include "Rendering/ShaderCompiler/ShaderCompilationTypes.h"
#include "Rendering/ShaderCompiler/ShaderVariantKey.h"

namespace NLS::Render::ShaderCompiler
{
	class NLS_RENDER_API ShaderAsset
	{
	public:
		explicit ShaderAsset(std::string sourcePath = {});

		void SetSourcePath(std::string sourcePath);
		const std::string& GetSourcePath() const;

		void SetEntryPoint(ShaderStage stage, std::string entryPoint);
		std::string GetEntryPoint(ShaderStage stage) const;

		void SetTargetProfile(ShaderStage stage, std::string targetProfile);
		std::string GetTargetProfile(ShaderStage stage) const;

		void SetCompiledVariant(const ShaderVariantKey& key, ShaderCompilationOutput output);
		const ShaderCompilationOutput* FindCompiledVariant(const ShaderVariantKey& key) const;

	private:
		static std::string BuildVariantCacheKey(const ShaderVariantKey& key);

	private:
		std::string m_sourcePath;
		std::unordered_map<ShaderStage, std::string> m_entryPoints;
		std::unordered_map<ShaderStage, std::string> m_targetProfiles;
		std::unordered_map<std::string, ShaderCompilationOutput> m_compiledVariants;
	};
}
