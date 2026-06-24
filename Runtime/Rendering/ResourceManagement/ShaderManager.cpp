#include "Core/ResourceManagement/ShaderManager.h"

#include "Assets/ArtifactManifest.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ServiceLocator.h"
#include "Rendering/Resources/Material.h"

#include <algorithm>
#include <filesystem>
#include <vector>

namespace NLS::Core::ResourceManagement
{
using Shader = Render::Resources::Shader;
using ShaderLoader = Render::Resources::Loaders::ShaderLoader;

namespace
{
	std::string ToLowerGenericPath(const std::string& path)
	{
		auto value = std::filesystem::path(path).generic_string();
		std::transform(
			value.begin(),
			value.end(),
			value.begin(),
			[](const unsigned char character)
			{
				return static_cast<char>(std::tolower(character));
			});
		return value;
	}

	bool IsBuiltInHlslShaderPath(
		const std::string& requestedPath,
		const std::string& resolvedPath,
		const std::string& engineAssetsPath)
	{
		if (ToLowerGenericPath(requestedPath).rfind(":shaders/", 0u) == 0u)
			return true;

		auto canonicalPath = [](std::filesystem::path path)
		{
			std::error_code error;
			path = std::filesystem::weakly_canonical(path, error);
			if (error)
				path = std::filesystem::absolute(path).lexically_normal();
			return ToLowerGenericPath(path.generic_string());
		};
		auto isInsideOrEqual = [](const std::string& path, const std::string& root)
		{
			return path == root ||
				(path.size() > root.size() &&
					path.rfind(root, 0u) == 0u &&
					(path[root.size()] == '/' || path[root.size()] == '\\'));
		};

		const auto resolved = canonicalPath(resolvedPath);
		std::vector<std::filesystem::path> allowedRoots;
		if (!engineAssetsPath.empty())
			allowedRoots.emplace_back(engineAssetsPath);
		allowedRoots.emplace_back("App/Assets/Engine/Shaders");
		allowedRoots.emplace_back("App/Assets/Editor/Shaders");
		allowedRoots.emplace_back("EngineAssets/Shaders");

		for (const auto& root : allowedRoots)
		{
			auto rootPath = root;
			if (rootPath.is_relative())
				rootPath = std::filesystem::absolute(rootPath);
			const auto canonicalRoot = canonicalPath(rootPath);
			if (isInsideOrEqual(resolved, canonicalRoot))
				return true;
		}

		return false;
	}
}

Shader* ShaderManager::CreateResource(const std::string& path)
{
	const auto portablePath = std::filesystem::path(path).generic_string();
	const auto portableArtifactPath = NLS::Core::Assets::TryMakePortableContentArtifactPath(portablePath);
	if (!portableArtifactPath.empty() &&
		!NLS::Core::Assets::IsRuntimeArtifactPathAuthorized(portableArtifactPath))
	{
		return nullptr;
	}

	std::string realPath = GetRealPath(path);
	const auto extension = ToLowerGenericPath(std::filesystem::path(realPath).extension().generic_string());
	Shader* shader = extension == ".hlsl" && IsBuiltInHlslShaderPath(path, realPath, GetEngineAssetsPath())
		? ShaderLoader::CreateBuiltInHlsl(realPath, GetProjectAssetsPath())
		: ShaderLoader::Create(realPath, GetProjectAssetsPath());

	// Do NOT rewrite const members with offsetof/reinterpret_cast (UB on non-standard-layout types).
	// Keep loader path as-is to avoid corrupting shader object state.
	return shader;
}

void ShaderManager::DestroyResource(Shader* resource)
{
	ShaderLoader::Destroy(resource);
}

void ShaderManager::OnResourceUnregistered(const std::string&, Shader* resource)
{
	if (resource == nullptr)
		return;

	NLS::Render::Resources::Material::ClearShaderReferencesFromLiveMaterials(resource);
}

void ShaderManager::ReloadResource(Shader* resource, const std::string& path)
{
	const auto realPath = GetRealPath(path);
	const auto extension = ToLowerGenericPath(std::filesystem::path(realPath).extension().generic_string());
	if (extension == ".hlsl" && IsBuiltInHlslShaderPath(path, realPath, GetEngineAssetsPath()))
	{
		ShaderLoader::RecompileBuiltInHlsl(*resource, realPath, GetProjectAssetsPath());
		return;
	}

	ShaderLoader::Recompile(*resource, realPath, GetProjectAssetsPath());
}

void ShaderManager::ProvideAssetPaths(const std::string& p_projectAssetsPath, const std::string& p_engineAssetsPath)
{
	AResourceManager<Shader>::ProvideAssetPaths(p_projectAssetsPath, p_engineAssetsPath);
	ShaderLoader::SetDefaultProjectAssetsPath(p_projectAssetsPath);
}

const std::string& ShaderManager::ProjectAssetsRoot()
{
	return GetProjectAssetsPath();
}
}
