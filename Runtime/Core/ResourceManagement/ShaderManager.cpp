#include "Core/ResourceManagement/ShaderManager.h"

NLS::Rendering::Resources::Shader* NLS::Core::ResourceManagement::ShaderManager::CreateResource(const std::string & p_path)
{
	std::string realPath = GetRealPath(p_path);
	NLS::Rendering::Resources::Shader* shader = NLS::Rendering::Resources::Loaders::ShaderLoader::Create(realPath);
	if (shader)
		*reinterpret_cast<std::string*>(reinterpret_cast<char*>(shader) + offsetof(NLS::Rendering::Resources::Shader, path)) = p_path; // Force the resource path to fit the given path

	return shader;
}

void NLS::Core::ResourceManagement::ShaderManager::DestroyResource(NLS::Rendering::Resources::Shader* p_resource)
{
	NLS::Rendering::Resources::Loaders::ShaderLoader::Destroy(p_resource);
}

void NLS::Core::ResourceManagement::ShaderManager::ReloadResource(NLS::Rendering::Resources::Shader* p_resource, const std::string& p_path)
{
	NLS::Rendering::Resources::Loaders::ShaderLoader::Recompile(*p_resource, p_path);
}
