#include "Rendering/ShaderCompiler/ShaderAsset.h"

#include <sstream>
#include <utility>

namespace NLS::Render::ShaderCompiler
{
	ShaderAsset::ShaderAsset(std::string sourcePath)
		: m_sourcePath(std::move(sourcePath))
	{
	}

	void ShaderAsset::SetSourcePath(std::string sourcePath)
	{
		m_sourcePath = std::move(sourcePath);
	}

	const std::string& ShaderAsset::GetSourcePath() const
	{
		return m_sourcePath;
	}

	void ShaderAsset::SetEntryPoint(ShaderStage stage, std::string entryPoint)
	{
		m_entryPoints[stage] = std::move(entryPoint);
	}

	std::string ShaderAsset::GetEntryPoint(ShaderStage stage) const
	{
		const auto found = m_entryPoints.find(stage);
		return found != m_entryPoints.end() ? found->second : std::string();
	}

	void ShaderAsset::SetTargetProfile(ShaderStage stage, std::string targetProfile)
	{
		m_targetProfiles[stage] = std::move(targetProfile);
	}

	std::string ShaderAsset::GetTargetProfile(ShaderStage stage) const
	{
		const auto found = m_targetProfiles.find(stage);
		return found != m_targetProfiles.end() ? found->second : std::string();
	}

	void ShaderAsset::SetCompiledVariant(const ShaderVariantKey& key, ShaderCompilationOutput output)
	{
		m_compiledVariants[BuildVariantCacheKey(key)] = std::move(output);
	}

	const ShaderCompilationOutput* ShaderAsset::FindCompiledVariant(const ShaderVariantKey& key) const
	{
		const auto found = m_compiledVariants.find(BuildVariantCacheKey(key));
		return found != m_compiledVariants.end() ? &found->second : nullptr;
	}

	std::string ShaderAsset::BuildVariantCacheKey(const ShaderVariantKey& key)
	{
		return key.ToString();
	}
}
