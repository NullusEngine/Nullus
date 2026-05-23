#include "Core/ResourceManagement/MaterialManager.h"

namespace
{
thread_local NLS::Core::ResourceManagement::ResourceLoadProgressCallback g_resourceLoadProgressCallback;
}

namespace NLS::Core::ResourceManagement
{
using Material = Render::Resources::Material;
using MaterialLoader = Render::Resources::Loaders::MaterialLoader;

ResourceLoadProgressScope::ResourceLoadProgressScope(ResourceLoadProgressCallback callback)
    : m_previousCallback(std::move(g_resourceLoadProgressCallback))
{
    g_resourceLoadProgressCallback = std::move(callback);
}

ResourceLoadProgressScope::~ResourceLoadProgressScope()
{
    g_resourceLoadProgressCallback = std::move(m_previousCallback);
}

void ResourceLoadProgressScope::Report(const ResourceLoadProgress& progress)
{
    if (g_resourceLoadProgressCallback)
        g_resourceLoadProgressCallback(progress);
}

std::string MaterialManager::ResolveResourcePath(const std::string& path)
{
	return GetRealPath(path);
}

Material* MaterialManager::CreateResource(const std::string& path)
{
	return CreateResource(path, {});
}

Material* MaterialManager::CreateResource(
	const std::string& path,
	const NLS::Render::Resources::Loaders::MaterialLoader::LoadOptions& options)
{
	std::string realPath = ResolveResourcePath(path);

	Material* material = MaterialLoader::Create(realPath, options);
	if (material)
	{
		*reinterpret_cast<std::string*>(reinterpret_cast<char*>(material) + offsetof(Material, path)) = path;
	}

	return material;
}

Material* MaterialManager::PrewarmArtifact(const std::string& path)
{
	if (auto* cached = GetResource(path, false))
		return cached;

	auto* prewarmed = CreateResource(path, {false, false});
	if (prewarmed && prewarmed->IsValid())
		return RegisterResource(path, prewarmed);

	if (prewarmed)
		DestroyResource(prewarmed);
	return nullptr;
}

Material* MaterialManager::LoadArtifactWithoutTextures(const std::string& path)
{
	if (auto* cached = GetResource(path, false))
		return cached;

	auto* loaded = CreateResource(path, {false, true});
	if (loaded && loaded->IsValid())
		return RegisterResource(path, loaded);

	if (loaded)
		DestroyResource(loaded);
	return nullptr;
}

void MaterialManager::DestroyResource(Material* resource)
{
	MaterialLoader::Destroy(resource);
}

void MaterialManager::ReloadResource(Material* resource, const std::string& path)
{
	std::string realPath = ResolveResourcePath(path);
	MaterialLoader::Reload(*resource, realPath);
}
}
