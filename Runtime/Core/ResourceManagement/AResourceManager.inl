#pragma once

#include <algorithm>
#include <filesystem>

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

			const auto alreadyRegistered = std::find_if(
				m_resources.begin(),
				m_resources.end(),
				[newResource](const auto& resource)
				{
					return resource.second == newResource;
				});
			if (alreadyRegistered != m_resources.end())
				return alreadyRegistered->second;

			return RegisterResource(p_path, newResource);
		}
	}

	template<typename T>
	inline void AResourceManager<T>::UnloadResource(const std::string & p_path)
	{
		if (auto resource = GetResource(p_path, false); resource)
		{
			DestroyResource(resource);
			UnregisterResource(p_path);
		}
	}

	template<typename T>
	inline bool AResourceManager<T>::MoveResource(const std::string & p_previousPath, const std::string & p_newPath)
	{
		if (IsResourceRegistered(p_previousPath) && !IsResourceRegistered(p_newPath))
		{
			T* toMove = m_resources.at(p_previousPath);
			UnregisterResource(p_previousPath);
			RegisterResource(p_newPath, toMove);
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
		return m_resources.find(p_path) != m_resources.end();
	}

	template<typename T>
	inline void AResourceManager<T>::UnloadResources()
	{
		for (auto&[key, value] : m_resources)
			DestroyResource(value);

		m_resources.clear();
	}

	template<typename T>
	inline T* AResourceManager<T>::RegisterResource(const std::string& p_path, T* p_instance)
	{
		if (auto resource = GetResource(p_path, false); resource)
			DestroyResource(resource);

		m_resources[p_path] = p_instance;

		return p_instance;
	}

	template<typename T>
	inline void AResourceManager<T>::UnregisterResource(const std::string & p_path)
	{
		m_resources.erase(p_path);
	}

	template<typename T>
	inline T* AResourceManager<T>::GetResource(const std::string& p_path, bool p_tryToLoadIfNotFound)
	{
		if (auto resource = m_resources.find(p_path); resource != m_resources.end())
		{
			return resource->second;
		}
		else if (p_tryToLoadIfNotFound)
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
	inline std::unordered_map<std::string, T*>& AResourceManager<T>::GetResources()
	{
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
