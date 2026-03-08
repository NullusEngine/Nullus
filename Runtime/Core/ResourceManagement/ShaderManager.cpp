#include "Core/ResourceManagement/ShaderManager.h"

NLS::Render::Resources::Shader* NLS::Core::ResourceManagement::ShaderManager::CreateResource(const std::string & p_path)
{
	std::string realPath = GetRealPath(p_path);
	NLS::Render::Resources::Shader* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(realPath);
	// Do NOT rewrite const members with offsetof/reinterpret_cast (UB on non-standard-layout types).
	// Keep loader path as-is to avoid corrupting shader object state.
	return shader;
}

void NLS::Core::ResourceManagement::ShaderManager::DestroyResource(NLS::Render::Resources::Shader* p_resource)
{
	NLS::Render::Resources::Loaders::ShaderLoader::Destroy(p_resource);
}

void NLS::Core::ResourceManagement::ShaderManager::ReloadResource(NLS::Render::Resources::Shader* p_resource, const std::string& p_path)
{
	NLS::Render::Resources::Loaders::ShaderLoader::Recompile(*p_resource, p_path);
}
