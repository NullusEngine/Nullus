#include "Core/ResourceManagement/MaterialManager.h"

#include <Debug/Logger.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace
{
thread_local NLS::Core::ResourceManagement::ResourceLoadProgressCallback g_resourceLoadProgressCallback;

struct AsyncMaterialArtifactRequest
{
	std::string path;
	std::string realPath;
	std::optional<std::filesystem::file_time_type> writeTime;
	std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
	size_t cancelableInterestCount = 0u;
	bool hasSharedInterest = false;
	std::future<std::string> future;
};

struct FailedMaterialArtifactLoad
{
	std::string realPath;
	std::optional<std::filesystem::file_time_type> writeTime;
};

std::mutex g_asyncMaterialMutex;
std::unordered_map<std::string, AsyncMaterialArtifactRequest> g_asyncMaterialRequests;
std::unordered_map<std::string, FailedMaterialArtifactLoad> g_failedAsyncMaterialArtifacts;
std::unordered_set<std::string> g_cancelledAsyncMaterialArtifacts;

bool IsMaterialArtifactPath(const std::string& path)
{
	auto extension = std::filesystem::path(path).extension().string();
	std::transform(
		extension.begin(),
		extension.end(),
		extension.begin(),
		[](const unsigned char character)
		{
			return static_cast<char>(std::tolower(character));
		});
	return extension == ".nmat";
}

std::optional<std::filesystem::file_time_type> TryGetLastWriteTime(const std::string& path)
{
	std::error_code error;
	auto writeTime = std::filesystem::last_write_time(path, error);
	if (error)
		return std::nullopt;
	return writeTime;
}

std::string NormalizeResolvedArtifactPath(std::string path)
{
	if (path.empty())
		return {};

	std::replace(path.begin(), path.end(), '\\', '/');
	return std::filesystem::path(path).lexically_normal().generic_string();
}

bool ArtifactPathMatchesResolvedPath(
	const std::string& candidateRealPath,
	const std::string& targetRealPath)
{
	return NormalizeResolvedArtifactPath(candidateRealPath) == NormalizeResolvedArtifactPath(targetRealPath);
}
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

namespace
{
Material* FindCachedMaterialByEquivalentArtifactPath(
	MaterialManager& manager,
	const std::string& realPath)
{
	const auto target = NormalizeResolvedArtifactPath(realPath);
	if (target.empty())
		return nullptr;

	for (const auto& [resourcePath, material] : manager.GetResources())
	{
		if (material == nullptr)
			continue;

		if (NormalizeResolvedArtifactPath(MaterialManager::ResolveResourcePath(resourcePath)) == target ||
			NormalizeResolvedArtifactPath(MaterialManager::ResolveResourcePath(material->path)) == target)
		{
			return material;
		}
	}
	return nullptr;
}

auto FindAsyncMaterialRequestByEquivalentArtifactPath(
	std::unordered_map<std::string, AsyncMaterialArtifactRequest>& requests,
	const std::string& path,
	const std::string& realPath)
{
	auto exact = requests.find(path);
	if (exact != requests.end())
		return exact;

	return std::find_if(
		requests.begin(),
		requests.end(),
		[&realPath](const auto& entry)
		{
			return ArtifactPathMatchesResolvedPath(entry.second.realPath, realPath);
		});
}

auto FindFailedMaterialLoadByEquivalentArtifactPath(
	std::unordered_map<std::string, FailedMaterialArtifactLoad>& failures,
	const std::string& path,
	const std::string& realPath)
{
	auto exact = failures.find(path);
	if (exact != failures.end())
		return exact;

	return std::find_if(
		failures.begin(),
		failures.end(),
		[&realPath](const auto& entry)
		{
			return ArtifactPathMatchesResolvedPath(entry.second.realPath, realPath);
		});
}

void EraseCancelledMaterialArtifactByEquivalentPath(
	std::unordered_set<std::string>& cancelledArtifacts,
	const std::string& path,
	const std::string& realPath)
{
	cancelledArtifacts.erase(path);
	for (auto it = cancelledArtifacts.begin(); it != cancelledArtifacts.end();)
	{
		if (ArtifactPathMatchesResolvedPath(MaterialManager::ResolveResourcePath(*it), realPath))
			it = cancelledArtifacts.erase(it);
		else
			++it;
	}
}
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
		material->path = path;

	return material;
}

Material* MaterialManager::PrewarmArtifact(const std::string& path)
{
	if (auto* cached = GetResource(path, false))
		return cached;
	const auto realPath = ResolveResourcePath(path);
	if (auto* cached = FindCachedMaterialByEquivalentArtifactPath(*this, realPath))
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
	const auto realPath = ResolveResourcePath(path);
	if (auto* cached = FindCachedMaterialByEquivalentArtifactPath(*this, realPath))
		return cached;

	auto* loaded = CreateResource(path, {false, true});
	if (loaded && loaded->IsValid())
		return RegisterResource(path, loaded);

	if (loaded)
		DestroyResource(loaded);
	return nullptr;
}

Material* MaterialManager::RequestAsyncArtifact(const std::string& path, const bool cancelableInterest)
{
	if (auto* cached = GetResource(path, false))
		return cached;

	const auto realPath = ResolveResourcePath(path);
	if (auto* cached = FindCachedMaterialByEquivalentArtifactPath(*this, realPath))
		return cached;
	if (!IsMaterialArtifactPath(realPath))
		return nullptr;

	const auto writeTime = TryGetLastWriteTime(realPath);
	AsyncMaterialArtifactRequest request;
	request.path = path;
	request.realPath = realPath;
	request.writeTime = writeTime;
	request.cancelableInterestCount = cancelableInterest ? 1u : 0u;
	request.hasSharedInterest = !cancelableInterest;
	auto cancellationFlag = request.cancelled;
	{
		std::lock_guard lock(g_asyncMaterialMutex);
		if (auto existing = FindAsyncMaterialRequestByEquivalentArtifactPath(g_asyncMaterialRequests, path, realPath);
			existing != g_asyncMaterialRequests.end())
		{
			if (cancelableInterest)
				++existing->second.cancelableInterestCount;
			else
				existing->second.hasSharedInterest = true;
			return nullptr;
		}
		auto failed = FindFailedMaterialLoadByEquivalentArtifactPath(g_failedAsyncMaterialArtifacts, path, realPath);
		if (failed != g_failedAsyncMaterialArtifacts.end())
		{
			if (ArtifactPathMatchesResolvedPath(failed->second.realPath, realPath) &&
				failed->second.writeTime == writeTime)
			{
				return nullptr;
			}
			g_failedAsyncMaterialArtifacts.erase(failed);
		}
		EraseCancelledMaterialArtifactByEquivalentPath(g_cancelledAsyncMaterialArtifacts, path, realPath);
		g_asyncMaterialRequests.emplace(path, std::move(request));
	}

	std::future<std::string> future;
	try
	{
		future = std::async(
			std::launch::async,
			[realPath, cancellationFlag]()
			{
				if (cancellationFlag && cancellationFlag->load(std::memory_order_acquire))
					return std::string {};
				auto payload = MaterialLoader::ReadSerializedPayload(realPath);
				if (cancellationFlag && cancellationFlag->load(std::memory_order_acquire))
					return std::string {};
				return payload;
			});
	}
	catch (...)
	{
		std::lock_guard lock(g_asyncMaterialMutex);
		g_asyncMaterialRequests.erase(path);
		g_failedAsyncMaterialArtifacts[path] = { realPath, writeTime };
		return nullptr;
	}

	std::lock_guard lock(g_asyncMaterialMutex);
	auto found = g_asyncMaterialRequests.find(path);
	if (found != g_asyncMaterialRequests.end())
		found->second.future = std::move(future);
	return nullptr;
}

void MaterialManager::CancelAsyncArtifact(const std::string& path)
{
	if (path.empty())
		return;

	const auto realPath = ResolveResourcePath(path);
	std::lock_guard lock(g_asyncMaterialMutex);
	if (auto found = FindAsyncMaterialRequestByEquivalentArtifactPath(g_asyncMaterialRequests, path, realPath);
		found != g_asyncMaterialRequests.end())
	{
		if (found->second.cancelableInterestCount > 0u)
			--found->second.cancelableInterestCount;
		return;
	}
	if (auto failed = FindFailedMaterialLoadByEquivalentArtifactPath(g_failedAsyncMaterialArtifacts, path, realPath);
		failed != g_failedAsyncMaterialArtifacts.end())
	{
		g_failedAsyncMaterialArtifacts.erase(failed);
	}
}

bool MaterialManager::IsAsyncArtifactLoadPending(const std::string& path) const
{
	const auto realPath = ResolveResourcePath(path);
	std::lock_guard lock(g_asyncMaterialMutex);
	return FindAsyncMaterialRequestByEquivalentArtifactPath(
		g_asyncMaterialRequests,
		path,
		realPath) != g_asyncMaterialRequests.end();
}

bool MaterialManager::IsAsyncArtifactLoadFailed(const std::string& path) const
{
	const auto realPath = ResolveResourcePath(path);
	const auto writeTime = TryGetLastWriteTime(realPath);
	std::lock_guard lock(g_asyncMaterialMutex);
	auto failed = FindFailedMaterialLoadByEquivalentArtifactPath(g_failedAsyncMaterialArtifacts, path, realPath);
	return failed != g_failedAsyncMaterialArtifacts.end() &&
		ArtifactPathMatchesResolvedPath(failed->second.realPath, realPath) &&
		failed->second.writeTime == writeTime;
}

void MaterialManager::PumpAsyncLoads(const size_t maxCompletions)
{
	size_t completedCount = 0u;

	while (completedCount < maxCompletions)
	{
		AsyncMaterialArtifactRequest request;
		{
			std::lock_guard lock(g_asyncMaterialMutex);
			auto found = std::find_if(
				g_asyncMaterialRequests.begin(),
				g_asyncMaterialRequests.end(),
				[](auto& entry)
				{
					return entry.second.future.valid() &&
						entry.second.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
				});
			if (found == g_asyncMaterialRequests.end())
				return;

			request = std::move(found->second);
			g_asyncMaterialRequests.erase(found);
		}

		std::string xml;
		try
		{
			xml = request.future.get();
		}
		catch (const std::exception& exception)
		{
			NLS_LOG_ERROR(
				std::string("Async material artifact load failed: ") +
				request.realPath +
				" error=" +
				exception.what());
			std::lock_guard lock(g_asyncMaterialMutex);
			g_failedAsyncMaterialArtifacts[request.path] =
				{ request.realPath, request.writeTime };
			++completedCount;
			continue;
		}
		catch (...)
		{
			NLS_LOG_ERROR("Async material artifact load failed: " + request.realPath + " error=unknown");
			std::lock_guard lock(g_asyncMaterialMutex);
			g_failedAsyncMaterialArtifacts[request.path] =
				{ request.realPath, request.writeTime };
			++completedCount;
			continue;
		}
		{
			std::lock_guard lock(g_asyncMaterialMutex);
			auto cancelled = std::find_if(
				g_cancelledAsyncMaterialArtifacts.begin(),
				g_cancelledAsyncMaterialArtifacts.end(),
				[&request](const auto& path)
				{
					return path == request.path ||
						ArtifactPathMatchesResolvedPath(MaterialManager::ResolveResourcePath(path), request.realPath);
				});
			if (cancelled != g_cancelledAsyncMaterialArtifacts.end())
			{
				g_cancelledAsyncMaterialArtifacts.erase(cancelled);
				++completedCount;
				continue;
			}
		}
		if (!xml.empty())
		{
			if (FindCachedMaterialByEquivalentArtifactPath(*this, request.realPath) != nullptr)
			{
				std::lock_guard lock(g_asyncMaterialMutex);
				g_failedAsyncMaterialArtifacts.erase(request.path);
				++completedCount;
				continue;
			}

			Material* material = nullptr;
			try
			{
				material = MaterialLoader::CreateFromSerializedPayload(
					request.realPath,
					xml,
					{ false, true });
				if (material)
				{
					material->path = request.path;
					RegisterResource(request.path, material);
					material = nullptr;
					std::lock_guard lock(g_asyncMaterialMutex);
					g_failedAsyncMaterialArtifacts.erase(request.path);
				}
				else
				{
					std::lock_guard lock(g_asyncMaterialMutex);
					g_failedAsyncMaterialArtifacts[request.path] =
						{ request.realPath, request.writeTime };
				}
			}
			catch (const std::exception& exception)
			{
				if (material)
					MaterialLoader::Destroy(material);
				NLS_LOG_ERROR(
					std::string("Async material runtime creation failed: ") +
					request.realPath +
					" error=" +
					exception.what());
				std::lock_guard lock(g_asyncMaterialMutex);
				g_failedAsyncMaterialArtifacts[request.path] =
					{ request.realPath, request.writeTime };
			}
			catch (...)
			{
				if (material)
					MaterialLoader::Destroy(material);
				NLS_LOG_ERROR("Async material runtime creation failed: " + request.realPath + " error=unknown");
				std::lock_guard lock(g_asyncMaterialMutex);
				g_failedAsyncMaterialArtifacts[request.path] =
					{ request.realPath, request.writeTime };
			}
		}
		else
		{
			std::lock_guard lock(g_asyncMaterialMutex);
			g_failedAsyncMaterialArtifacts[request.path] =
				{ request.realPath, request.writeTime };
		}

		++completedCount;
	}
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
