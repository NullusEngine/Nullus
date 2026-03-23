#include "Core/ResourceManagement/ShaderManager.h"

namespace NLS::Core::ResourceManagement
{
using Shader = Render::Resources::Shader;
using ShaderLoader = Render::Resources::Loaders::ShaderLoader;

Shader* ShaderManager::CreateResource(const std::string& path)
{
	std::string realPath = GetRealPath(path);
	Shader* shader = ShaderLoader::Create(realPath);

	// Do NOT rewrite const members with offsetof/reinterpret_cast (UB on non-standard-layout types).
	// Keep loader path as-is to avoid corrupting shader object state.
	return shader;
}

void ShaderManager::DestroyResource(Shader* resource)
{
	ShaderLoader::Destroy(resource);
}

void ShaderManager::ReloadResource(Shader* resource, const std::string& path)
{
	ShaderLoader::Recompile(*resource, path);
}
}
