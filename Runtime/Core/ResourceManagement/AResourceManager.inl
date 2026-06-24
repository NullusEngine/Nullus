#pragma once

#include <algorithm>
#include <filesystem>

#include "Assets/ArtifactLoadTelemetry.h"
#include "Core/ResourceManagement/AResourceManager.h"
namespace NLS::Core::ResourceManagement
{
	template<typename T>
	inline T* AResourceManager<T>::LoadResource(const std::string & p_path)
	{
		if (auto resource = GetResource(p_path, false); resource)
			return resource;
		else
		{
			const auto* resourceType = GetResourceTypeName();
			ResourceLoadProgressScope::Report({
				resourceType,
				p_path,
				std::string("Loading ") + resourceType + ": " + p_path,
				false,
				false
			});
			auto newResource = CreateResource(p_path);
			ResourceLoadProgressScope::Report({
				resourceType,
				p_path,
				std::string(newResource ? "Loaded " : "Failed to load ") + resourceType + ": " + p_path,
				true,
				newResource != nullptr
			});
			if (!newResource)
				return nullptr;

			T* registeredResource = nullptr;
			bool destroyNewResource = false;
			bool insertedNewResource = false;
			{
				std::lock_guard lock(m_resourcesMutex);
				if (auto resource = m_resources.find(p_path); resource != m_resources.end())
				{
					registeredResource = resource->second;
					destroyNewResource = registeredResource != newResource;
				}
				else
				{
					const auto alreadyRegistered = std::find_if(
						m_resources.begin(),
						m_resources.end(),
						[newResource](const auto& resource)
						{
							return resource.second == newResource;
						});
					if (alreadyRegistered != m_resources.end())
					{
						registeredResource = alreadyRegistered->second;
					}
					else
					{
						m_resources[p_path] = newResource;
						registeredResource = newResource;
						insertedNewResource = true;
					}
				}
				if (insertedNewResource)
					OnResourceRegistered(p_path, registeredResource);
			}

			if (destroyNewResource)
				DestroyResource(newResource);

			return registeredResource;
		}
	}

	template<typename T>
	inline void AResourceManager<T>::UnloadResource(const std::string & p_path)
	{
		T* resource = nullptr;
		{
			std::lock_guard lock(m_resourcesMutex);
			if (auto found = m_resources.find(p_path); found != m_resources.end())
			{
				resource = found->second;
				m_resources.erase(found);
				m_lifetimeManagedResources.erase(ResourceLifetimeRegistry::NormalizeResourcePath(p_path));
				OnResourceUnregistered(p_path, resource);
			}
		}

		if (resource)
			DestroyResourceForPath(p_path, resource);
	}

	template<typename T>
	inline bool AResourceManager<T>::MoveResource(const std::string & p_previousPath, const std::string & p_newPath)
	{
		std::lock_guard lock(m_resourcesMutex);
		const auto previous = m_resources.find(p_previousPath);
		if (previous != m_resources.end() && m_resources.find(p_newPath) == m_resources.end())
		{
			auto* resource = previous->second;
			m_resources[p_newPath] = resource;
			m_resources.erase(previous);
			const auto previousNormalizedPath = ResourceLifetimeRegistry::NormalizeResourcePath(p_previousPath);
			if (m_lifetimeManagedResources.erase(previousNormalizedPath) > 0u)
				m_lifetimeManagedResources.insert(ResourceLifetimeRegistry::NormalizeResourcePath(p_newPath));
			OnResourceMoved(p_previousPath, p_newPath, resource);
			return true;
		}

		return false;
	}

	template<typename T>
	inline void AResourceManager<T>::ReloadResource(const std::string& p_path)
	{
		if (auto resource = GetResource(p_path, false); resource)
		{
			ReloadResource(resource, p_path);
		}
	}

	template<typename T>
	inline bool AResourceManager<T>::IsResourceRegistered(const std::string & p_path)
	{
		std::lock_guard lock(m_resourcesMutex);
		return m_resources.find(p_path) != m_resources.end();
	}

	template<typename T>
	inline void AResourceManager<T>::UnloadResources()
	{
		std::unordered_map<std::string, T*> resources;
		{
			std::lock_guard lock(m_resourcesMutex);
			resources = std::move(m_resources);
			m_resources.clear();
			m_lifetimeManagedResources.clear();
			OnAllResourcesUnregistered();
		}

		for (auto& [key, value] : resources)
			DestroyResourceForPath(key, value);
	}

	template<typename T>
	inline T* AResourceManager<T>::RegisterResource(const std::string& p_path, T* p_instance)
	{
		T* previousResource = nullptr;
		{
			std::lock_guard lock(m_resourcesMutex);
			if (auto resource = m_resources.find(p_path); resource != m_resources.end())
				previousResource = resource->second;

			m_resources[p_path] = p_instance;
			m_lifetimeManagedResources.erase(ResourceLifetimeRegistry::NormalizeResourcePath(p_path));
			if (previousResource != nullptr)
				OnResourceUnregistered(p_path, previousResource);
			OnResourceRegistered(p_path, p_instance);
		}

		if (previousResource)
			DestroyResourceForPath(p_path, previousResource);

		return p_instance;
	}

	template<typename T>
	inline ResourceHandle<T> AResourceManager<T>::AcquireResourceHandle(
		ResourceLifetimeRegistry& p_registry,
		const ResourceLifetimeAcquireRequest& p_request)
	{
		const auto resourcePath = p_request.path;
		const auto resourceId = p_registry.Acquire(p_request);
		if (resourceId.normalizedPath.empty())
			return {};
		if (!GetResource(resourcePath, false) && !LoadResource(resourcePath))
		{
			p_registry.Release(resourceId, p_request.ownerToken);
			return {};
		}
		{
			std::lock_guard lock(m_resourcesMutex);
			m_lifetimeManagedResources.insert(ResourceLifetimeRegistry::NormalizeResourcePath(resourcePath));
		}

		return ResourceHandle<T>(
			p_registry,
			resourceId,
			p_request.ownerToken,
			[this, resourcePath](const ResourceId&) -> T*
			{
				return GetResource(resourcePath, false);
			});
	}

	template<typename T>
	inline size_t AResourceManager<T>::TrimUnusedResources(
		ResourceLifetimeRegistry& p_registry,
		ResourceLifetimeResourceType p_type,
		const ResourceLifetimeTrimOptions& p_options)
	{
		size_t unloadedCount = 0u;
		for (const auto& [path, resource] : GetResources())
		{
			(void)resource;
			if (p_registry.HasActiveOwners(p_type, path))
			{
				NLS::Core::Assets::RecordArtifactLoadTelemetry({
					NLS::Core::Assets::ArtifactLoadTelemetryStage::LifetimeTrimSkip,
					{},
					p_registry.GetEstimatedBytes(p_type, path),
					ResourceLifetimeRegistry::NormalizeResourcePath(path) });
			}
		}

		std::unordered_map<std::string, std::vector<std::string>> normalizedResourceIndex;
		{
			std::lock_guard lock(m_resourcesMutex);
			normalizedResourceIndex.reserve(m_resources.size());
			for (auto it = m_resources.begin(); it != m_resources.end(); ++it)
				normalizedResourceIndex[ResourceLifetimeRegistry::NormalizeResourcePath(it->first)].push_back(it->first);
		}

		for (const auto& candidate : p_registry.CollectTrimCandidates(p_options))
		{
			if (candidate.type != p_type)
				continue;

			if (!p_registry.TryBeginEviction(candidate))
				continue;

			std::vector<std::string> registeredPaths;
			{
				std::lock_guard lock(m_resourcesMutex);
				if (const auto found = normalizedResourceIndex.find(candidate.normalizedPath); found != normalizedResourceIndex.end())
				{
					registeredPaths = found->second;
					normalizedResourceIndex.erase(found);
				}
			}

			std::vector<std::pair<std::string, T*>> resourcesToDestroy;
			{
				std::lock_guard lock(m_resourcesMutex);
				for (const auto& registeredPath : registeredPaths)
				{
					if (auto registered = m_resources.find(registeredPath); registered != m_resources.end())
					{
						auto key = registered->first;
						auto* resource = registered->second;
						resourcesToDestroy.emplace_back(key, resource);
						m_resources.erase(registered);
						OnResourceUnregistered(key, resource);
					}
					m_lifetimeManagedResources.erase(ResourceLifetimeRegistry::NormalizeResourcePath(registeredPath));
				}
			}

			if (!p_registry.CompleteEviction(candidate.type, candidate.normalizedPath))
			{
				std::vector<std::pair<std::string, T*>> replacedResourcesToDestroy;
				{
					std::lock_guard lock(m_resourcesMutex);
					for (const auto& [registeredPath, resource] : resourcesToDestroy)
					{
						auto registered = m_resources.find(registeredPath);
						if (registered == m_resources.end())
						{
							m_resources.emplace(registeredPath, resource);
							m_lifetimeManagedResources.insert(ResourceLifetimeRegistry::NormalizeResourcePath(registeredPath));
							OnResourceRegistered(registeredPath, resource);
						}
						else if (registered->second != resource)
						{
							replacedResourcesToDestroy.emplace_back(registeredPath, resource);
						}
					}
				}
				for (const auto& [registeredPath, resource] : replacedResourcesToDestroy)
				{
					if (resource != nullptr)
						DestroyResourceForPath(registeredPath, resource);
				}
				continue;
			}

			if (registeredPaths.empty())
			{
				++unloadedCount;
				continue;
			}

			if (resourcesToDestroy.empty())
				continue;

			for (const auto& [registeredPath, resource] : resourcesToDestroy)
			{
				if (resource == nullptr)
					continue;
				DestroyResourceForPath(registeredPath, resource);
				++unloadedCount;
			}
			NLS::Core::Assets::RecordArtifactLoadTelemetry({
				NLS::Core::Assets::ArtifactLoadTelemetryStage::Eviction,
				{},
				candidate.estimatedBytes,
				candidate.normalizedPath });
		}
		return unloadedCount;
	}

	template<typename T>
	inline void AResourceManager<T>::DestroyResourceForPath(const std::string&, T* p_resource)
	{
		DestroyResource(p_resource);
	}

	template<typename T>
	inline void AResourceManager<T>::UnregisterResource(const std::string & p_path)
	{
		std::lock_guard lock(m_resourcesMutex);
		if (auto found = m_resources.find(p_path); found != m_resources.end())
		{
			auto* resource = found->second;
			m_resources.erase(found);
			OnResourceUnregistered(p_path, resource);
		}
		m_lifetimeManagedResources.erase(ResourceLifetimeRegistry::NormalizeResourcePath(p_path));
	}

	template<typename T>
	inline T* AResourceManager<T>::GetResource(const std::string& p_path, bool p_tryToLoadIfNotFound)
	{
		{
			std::lock_guard lock(m_resourcesMutex);
			if (auto resource = m_resources.find(p_path); resource != m_resources.end())
			{
				return resource->second;
			}
		}
		if (p_tryToLoadIfNotFound)
		{
			return LoadResource(p_path);
		}

		return nullptr;
	}

	template<typename T>
	inline T* AResourceManager<T>::operator[](const std::string & p_path)
	{
		return GetResource(p_path);
	}

	template<typename T>
	inline void AResourceManager<T>::ProvideAssetPaths(const std::string & p_projectAssetsPath, const std::string & p_engineAssetsPath)
	{
		__PROJECT_ASSETS_PATH	= p_projectAssetsPath;
		__ENGINE_ASSETS_PATH	= p_engineAssetsPath;
	}

	template<typename T>
	inline std::unordered_map<std::string, T*> AResourceManager<T>::GetResources() const
	{
		std::lock_guard lock(m_resourcesMutex);
		return m_resources;
	}

	template<typename T>
	std::string AResourceManager<T>::GetRealPath(const std::string& p_path)
	{
		if (p_path.empty())
			return {};

		if (std::filesystem::path(p_path).is_absolute())
			return p_path;

		const auto genericPath = std::filesystem::path(p_path).generic_string();
		if (genericPath == "Library" || genericPath.rfind("Library/", 0) == 0)
		{
			auto projectAssetsPath = __PROJECT_ASSETS_PATH;
			while (!projectAssetsPath.empty() &&
				(projectAssetsPath.back() == '/' || projectAssetsPath.back() == '\\'))
			{
				projectAssetsPath.pop_back();
			}
			const auto projectRoot = std::filesystem::path(projectAssetsPath).parent_path();
			if (!projectRoot.empty())
				return (projectRoot / std::filesystem::path(genericPath)).string();
		}

		std::string result;

		if (p_path[0] == ':') // The path is an engine path
		{
			result = __ENGINE_ASSETS_PATH + std::string(p_path.data() + 1, p_path.data() + p_path.size());
		}
		else // The path is a project path
		{
			result = __PROJECT_ASSETS_PATH + p_path;
		}

		return result;
	}

	template<typename T>
	const std::string& AResourceManager<T>::GetProjectAssetsPath()
	{
		return __PROJECT_ASSETS_PATH;
	}

	template<typename T>
	const std::string& AResourceManager<T>::GetEngineAssetsPath()
	{
		return __ENGINE_ASSETS_PATH;
	}
}
