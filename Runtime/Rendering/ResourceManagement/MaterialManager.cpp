#include "Core/ResourceManagement/MaterialManager.h"

#include <Debug/Logger.h>
#include <Jobs/BackgroundJobQueue.h>
#include <Jobs/JobSystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <Rendering/Resources/Texture.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
thread_local NLS::Core::ResourceManagement::ResourceLoadProgressCallback g_resourceLoadProgressCallback;

struct AsyncMaterialArtifactRequest
{
    const NLS::Core::ResourceManagement::MaterialManager* owner = nullptr;
    std::string path;
	std::string realPath;
	std::optional<std::filesystem::file_time_type> writeTime;
			std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
			size_t cancelableInterestCount = 0u;
			size_t sharedInterestCount = 0u;
			bool retryCancelledCompletion = false;
			NLS::Base::Jobs::JobHandle jobHandle;
		std::future<std::string> future;
	};

struct FailedMaterialArtifactLoad
{
	const NLS::Core::ResourceManagement::MaterialManager* owner = nullptr;
	std::string realPath;
	std::optional<std::filesystem::file_time_type> writeTime;
};

struct AsyncMaterialArtifactStateKey
{
	const NLS::Core::ResourceManagement::MaterialManager* owner = nullptr;
	std::string path;
	std::string realPath;

	[[nodiscard]] bool operator==(const AsyncMaterialArtifactStateKey& other) const
	{
		return owner == other.owner && path == other.path;
	}
};

struct AsyncMaterialArtifactStateKeyHash
{
	[[nodiscard]] size_t operator()(const AsyncMaterialArtifactStateKey& key) const
	{
		return std::hash<const NLS::Core::ResourceManagement::MaterialManager*>{}(key.owner) ^
			(std::hash<std::string>{}(key.path) << 1u);
	}
};

std::mutex g_asyncMaterialMutex;
std::unordered_map<AsyncMaterialArtifactStateKey, AsyncMaterialArtifactRequest, AsyncMaterialArtifactStateKeyHash> g_asyncMaterialRequests;
std::unordered_map<AsyncMaterialArtifactStateKey, FailedMaterialArtifactLoad, AsyncMaterialArtifactStateKeyHash> g_failedAsyncMaterialArtifacts;
std::unordered_set<AsyncMaterialArtifactStateKey, AsyncMaterialArtifactStateKeyHash> g_cancelledAsyncMaterialArtifacts;
std::atomic_size_t g_activeMaterialArtifactWorkers {0u};
constexpr size_t kMaxPendingAsyncMaterialArtifactRequests = 8u;
constexpr size_t kMaxQueuedAsyncMaterialArtifactRequests = 256u;

struct TrackedMaterialArtifactPaths
{
	std::unordered_set<std::string> sourcePaths;
	std::unordered_set<std::string> normalizedRealPaths;
};

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
    std::unordered_map<AsyncMaterialArtifactStateKey, AsyncMaterialArtifactRequest, AsyncMaterialArtifactStateKeyHash>& requests,
    const MaterialManager& manager,
    const std::string& path,
    const std::string& realPath)
{
    return std::find_if(
        requests.begin(),
        requests.end(),
        [&manager, &path, &realPath](const auto& entry)
        {
            return entry.second.owner == &manager &&
                (entry.first.path == path ||
                 ArtifactPathMatchesResolvedPath(entry.second.realPath, realPath));
        });
}

auto FindFailedMaterialLoadByEquivalentArtifactPath(
	std::unordered_map<AsyncMaterialArtifactStateKey, FailedMaterialArtifactLoad, AsyncMaterialArtifactStateKeyHash>& failures,
	const MaterialManager& manager,
	const std::string& path,
	const std::string& realPath)
{
	return std::find_if(
		failures.begin(),
		failures.end(),
		[&manager, &path, &realPath](const auto& entry)
		{
			return entry.second.owner == &manager &&
				(entry.first.path == path ||
				 ArtifactPathMatchesResolvedPath(entry.second.realPath, realPath));
	});
}

bool HasResolvedTextureDependencies(const NLS::Render::Resources::Material& material)
{
	const auto& texturePaths = material.GetTextureResourcePaths();
	if (texturePaths.empty())
		return true;

	const auto& uniforms = material.GetUniformsData();
	for (const auto& [name, path] : texturePaths)
	{
		if (path.empty())
			continue;

		const auto uniform = uniforms.find(name);
		if (uniform == uniforms.end())
			return false;

		const auto* texture = std::any_cast<NLS::Render::Resources::Texture2D*>(&uniform->second);
		if (texture == nullptr || *texture == nullptr)
			return false;
	}
	return true;
}

void EraseCancelledMaterialArtifactByEquivalentPath(
	std::unordered_set<AsyncMaterialArtifactStateKey, AsyncMaterialArtifactStateKeyHash>& cancelledArtifacts,
	const MaterialManager& manager,
	const std::string& path,
	const std::string& realPath)
{
	for (auto it = cancelledArtifacts.begin(); it != cancelledArtifacts.end();)
	{
		if (it->owner == &manager &&
			(it->path == path ||
			 ArtifactPathMatchesResolvedPath(it->realPath, realPath)))
		{
			it = cancelledArtifacts.erase(it);
		}
		else
			++it;
	}
}

bool MaterialRequestMatchesAnyTrackedPath(
	const AsyncMaterialArtifactRequest& request,
	const TrackedMaterialArtifactPaths& paths)
{
	if (paths.sourcePaths.find(request.path) != paths.sourcePaths.end())
		return true;
	const auto normalizedRequestPath = NormalizeResolvedArtifactPath(request.realPath);
	return !normalizedRequestPath.empty() &&
		paths.normalizedRealPaths.find(normalizedRequestPath) != paths.normalizedRealPaths.end();
}

TrackedMaterialArtifactPaths BuildTrackedMaterialArtifactPaths(const std::unordered_set<std::string>& paths)
{
	TrackedMaterialArtifactPaths tracked;
	tracked.sourcePaths = paths;
	tracked.normalizedRealPaths.reserve(paths.size());
	for (const auto& path : paths)
	{
		const auto normalized = NormalizeResolvedArtifactPath(MaterialManager::ResolveResourcePath(path));
		if (!normalized.empty())
			tracked.normalizedRealPaths.insert(normalized);
	}
	return tracked;
}

class MaterialArtifactWorkerScope
{
public:
	MaterialArtifactWorkerScope() { g_activeMaterialArtifactWorkers.fetch_add(1u, std::memory_order_acq_rel); }
	~MaterialArtifactWorkerScope() { g_activeMaterialArtifactWorkers.fetch_sub(1u, std::memory_order_acq_rel); }
};

struct MaterialArtifactJobPayload
{
	std::string realPath;
	std::shared_ptr<std::atomic_bool> cancellationFlag;
	std::promise<std::string> promise;
};

struct MaterialArtifactLoadSubmission
{
	NLS::Base::Jobs::JobHandle handle;
	std::future<std::string> future;
};

void RunMaterialArtifactJob(void* userData)
{
	std::unique_ptr<MaterialArtifactJobPayload> payload(static_cast<MaterialArtifactJobPayload*>(userData));
	MaterialArtifactWorkerScope workerScope;
	try
	{
		if (payload->cancellationFlag && payload->cancellationFlag->load(std::memory_order_acquire))
		{
			payload->promise.set_value({});
			return;
		}
		auto materialPayload = MaterialLoader::ReadSerializedPayload(payload->realPath);
		if (payload->cancellationFlag && payload->cancellationFlag->load(std::memory_order_acquire))
		{
			payload->promise.set_value({});
			return;
		}
		payload->promise.set_value(std::move(materialPayload));
	}
	catch (...)
	{
		try
		{
			payload->promise.set_exception(std::current_exception());
		}
		catch (...)
		{
		}
	}
}

void CancelMaterialArtifactJob(void* userData)
{
	std::unique_ptr<MaterialArtifactJobPayload> payload(static_cast<MaterialArtifactJobPayload*>(userData));
	if (payload->cancellationFlag)
		payload->cancellationFlag->store(true, std::memory_order_release);
	try
	{
		payload->promise.set_value({});
	}
	catch (...)
	{
	}
}

MaterialArtifactLoadSubmission StartMaterialArtifactLoad(const AsyncMaterialArtifactRequest& request)
{
	auto payload = std::make_unique<MaterialArtifactJobPayload>();
	payload->realPath = request.realPath;
	payload->cancellationFlag = request.cancelled;
	auto future = payload->promise.get_future();

	NLS::Base::Jobs::BackgroundJobDesc desc;
	desc.function = RunMaterialArtifactJob;
	desc.userData = payload.get();
	desc.cancelFunction = CancelMaterialArtifactJob;
	desc.cancelUserData = payload.get();
	desc.debugName = "MaterialManager::AsyncArtifactLoad";

	const auto handle = NLS::Base::Jobs::ScheduleBackgroundJob(desc);
	if (handle.id == 0u)
		return {};

	(void)payload.release();
	return {handle, std::move(future)};
}

size_t CountActiveMaterialRequests()
{
	return static_cast<size_t>(std::count_if(
		g_asyncMaterialRequests.begin(),
		g_asyncMaterialRequests.end(),
			[](const auto& entry)
			{
				if (!entry.second.future.valid())
					return false;
				if (entry.second.jobHandle.id != 0u &&
					NLS::Base::Jobs::IsCompleted(entry.second.jobHandle))
				{
					return false;
				}
				return entry.second.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
			}));
}

size_t CountQueuedMaterialRequestsForOwner(const MaterialManager& manager)
{
	return static_cast<size_t>(std::count_if(
		g_asyncMaterialRequests.begin(),
		g_asyncMaterialRequests.end(),
		[&manager](const auto& entry)
		{
			return entry.second.owner == &manager;
		}));
}

void PromoteQueuedMaterialArtifactLoads(
	MaterialManager& manager,
	const TrackedMaterialArtifactPaths* paths)
{
	for (;;)
	{
		if (CountActiveMaterialRequests() >= kMaxPendingAsyncMaterialArtifactRequests)
			return;

		auto found = std::find_if(
			g_asyncMaterialRequests.begin(),
			g_asyncMaterialRequests.end(),
			[&manager, paths](auto& entry)
			{
				if (entry.second.owner != &manager || entry.second.future.valid())
					return false;
				return paths == nullptr || MaterialRequestMatchesAnyTrackedPath(entry.second, *paths);
			});
		if (found == g_asyncMaterialRequests.end())
			return;

			try
			{
					auto load = StartMaterialArtifactLoad(found->second);
					if (!load.future.valid())
					{
						if (NLS::Base::Jobs::IsJobSystemInitialized())
						{
							g_failedAsyncMaterialArtifacts[found->first] =
								{ found->second.owner, found->second.realPath, found->second.writeTime };
							g_asyncMaterialRequests.erase(found);
							continue;
						}
						return;
					}
					found->second.jobHandle = load.handle;
					found->second.future = std::move(load.future);
			}
		catch (...)
		{
			g_failedAsyncMaterialArtifacts[found->first] =
				{ found->second.owner, found->second.realPath, found->second.writeTime };
			g_asyncMaterialRequests.erase(found);
		}
	}
}

void PumpAsyncMaterialArtifactLoads(
	MaterialManager& manager,
	const size_t maxCompletions,
	const TrackedMaterialArtifactPaths* paths)
{
	size_t completedCount = 0u;

	while (completedCount < maxCompletions)
	{
		AsyncMaterialArtifactRequest request;
		{
			std::lock_guard lock(g_asyncMaterialMutex);
			PromoteQueuedMaterialArtifactLoads(manager, paths);
			auto found = std::find_if(
				g_asyncMaterialRequests.begin(),
				g_asyncMaterialRequests.end(),
				[&manager, paths](auto& entry)
				{
					if (entry.second.owner != &manager)
						return false;
					if (paths != nullptr && !MaterialRequestMatchesAnyTrackedPath(entry.second, *paths))
						return false;
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
			g_failedAsyncMaterialArtifacts[{ request.owner, request.path }] =
				{ request.owner, request.realPath, request.writeTime };
			++completedCount;
			continue;
		}
		catch (...)
		{
			NLS_LOG_ERROR("Async material artifact load failed: " + request.realPath + " error=unknown");
			std::lock_guard lock(g_asyncMaterialMutex);
			g_failedAsyncMaterialArtifacts[{ request.owner, request.path }] =
				{ request.owner, request.realPath, request.writeTime };
			++completedCount;
			continue;
		}
		{
			std::lock_guard lock(g_asyncMaterialMutex);
			auto cancelled = std::find_if(
				g_cancelledAsyncMaterialArtifacts.begin(),
				g_cancelledAsyncMaterialArtifacts.end(),
					[&request](const auto& entry)
					{
						return entry.owner == request.owner &&
							(entry.path == request.path ||
							 ArtifactPathMatchesResolvedPath(entry.realPath, request.realPath));
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
			if (FindCachedMaterialByEquivalentArtifactPath(manager, request.realPath) != nullptr)
			{
				std::lock_guard lock(g_asyncMaterialMutex);
				g_failedAsyncMaterialArtifacts.erase({ request.owner, request.path });
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
					manager.RegisterResource(request.path, material);
					material = nullptr;
					std::lock_guard lock(g_asyncMaterialMutex);
					g_failedAsyncMaterialArtifacts.erase({ request.owner, request.path });
				}
			else
			{
				std::lock_guard lock(g_asyncMaterialMutex);
				if (request.retryCancelledCompletion &&
					request.cancelableInterestCount + request.sharedInterestCount > 0u)
				{
					request.cancelled = std::make_shared<std::atomic_bool>(false);
					request.jobHandle = {};
					request.future = {};
					request.retryCancelledCompletion = false;
					g_failedAsyncMaterialArtifacts.erase({ request.owner, request.path });
					g_asyncMaterialRequests.emplace(
						AsyncMaterialArtifactStateKey{ request.owner, request.path },
						std::move(request));
				}
				else
				{
					g_failedAsyncMaterialArtifacts[{ request.owner, request.path }] =
						{ request.owner, request.realPath, request.writeTime };
				}
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
				g_failedAsyncMaterialArtifacts[{ request.owner, request.path }] =
					{ request.owner, request.realPath, request.writeTime };
			}
			catch (...)
			{
				if (material)
					MaterialLoader::Destroy(material);
				NLS_LOG_ERROR("Async material runtime creation failed: " + request.realPath + " error=unknown");
				std::lock_guard lock(g_asyncMaterialMutex);
				g_failedAsyncMaterialArtifacts[{ request.owner, request.path }] =
					{ request.owner, request.realPath, request.writeTime };
			}
		}
			else
			{
				std::lock_guard lock(g_asyncMaterialMutex);
				if (request.retryCancelledCompletion &&
					request.cancelableInterestCount + request.sharedInterestCount > 0u)
				{
					request.cancelled = std::make_shared<std::atomic_bool>(false);
					request.jobHandle = {};
					request.future = {};
					request.retryCancelledCompletion = false;
					g_failedAsyncMaterialArtifacts.erase({ request.owner, request.path });
					g_asyncMaterialRequests.emplace(
						AsyncMaterialArtifactStateKey{ request.owner, request.path },
						std::move(request));
				}
				else
				{
					g_failedAsyncMaterialArtifacts[{ request.owner, request.path }] =
						{ request.owner, request.realPath, request.writeTime };
				}
			}

			++completedCount;
		}
	}

void ClearAsyncMaterialArtifactStateForOwner(const MaterialManager& manager)
{
	std::vector<AsyncMaterialArtifactRequest> removedRequests;
	{
		std::lock_guard lock(g_asyncMaterialMutex);
		for (auto it = g_asyncMaterialRequests.begin(); it != g_asyncMaterialRequests.end();)
		{
			if (it->second.owner == &manager)
			{
				if (it->second.cancelled)
					it->second.cancelled->store(true, std::memory_order_release);
				removedRequests.push_back(std::move(it->second));
				it = g_asyncMaterialRequests.erase(it);
			}
			else
				++it;
		}
		for (auto it = g_failedAsyncMaterialArtifacts.begin(); it != g_failedAsyncMaterialArtifacts.end();)
		{
			if (it->second.owner == &manager)
				it = g_failedAsyncMaterialArtifacts.erase(it);
			else
				++it;
		}
		for (auto it = g_cancelledAsyncMaterialArtifacts.begin(); it != g_cancelledAsyncMaterialArtifacts.end();)
		{
			if (it->owner == &manager)
				it = g_cancelledAsyncMaterialArtifacts.erase(it);
			else
				++it;
		}
	}
	for (auto& request : removedRequests)
	{
		if (request.jobHandle.id != 0u)
			NLS::Base::Jobs::Complete(request.jobHandle);
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

MaterialManager::~MaterialManager()
{
	ClearAsyncMaterialArtifactStateForOwner(*this);
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

Material* MaterialManager::PrewarmArtifactWithDependencies(const std::string& path)
{
	if (auto* cached = GetResource(path, false))
	{
		if (!HasResolvedTextureDependencies(*cached))
			NLS::Render::Resources::Loaders::MaterialLoader::Reload(*cached, ResolveResourcePath(path), {true, true});
		return HasResolvedTextureDependencies(*cached) ? cached : nullptr;
	}
	const auto realPath = ResolveResourcePath(path);
	if (auto* cached = FindCachedMaterialByEquivalentArtifactPath(*this, realPath))
	{
		if (!HasResolvedTextureDependencies(*cached))
			NLS::Render::Resources::Loaders::MaterialLoader::Reload(*cached, realPath, {true, true});
		return HasResolvedTextureDependencies(*cached) ? cached : nullptr;
	}

	auto* prewarmed = CreateResource(path, {true, true});
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
		request.owner = this;
		request.path = path;
		request.realPath = realPath;
		request.writeTime = writeTime;
		request.cancelableInterestCount = cancelableInterest ? 1u : 0u;
		request.sharedInterestCount = cancelableInterest ? 0u : 1u;
		{
			std::lock_guard lock(g_asyncMaterialMutex);
			if (auto existing = FindAsyncMaterialRequestByEquivalentArtifactPath(g_asyncMaterialRequests, *this, path, realPath);
				existing != g_asyncMaterialRequests.end())
			{
				if (cancelableInterest)
					++existing->second.cancelableInterestCount;
				else
					++existing->second.sharedInterestCount;
				EraseCancelledMaterialArtifactByEquivalentPath(
					g_cancelledAsyncMaterialArtifacts,
					*this,
					path,
					realPath);
				if (existing->second.cancelled)
					existing->second.cancelled->store(false, std::memory_order_release);
				existing->second.retryCancelledCompletion = true;
				return nullptr;
			}
		auto failed = FindFailedMaterialLoadByEquivalentArtifactPath(g_failedAsyncMaterialArtifacts, *this, path, realPath);
		if (failed != g_failedAsyncMaterialArtifacts.end())
		{
			if (ArtifactPathMatchesResolvedPath(failed->second.realPath, realPath) &&
				failed->second.writeTime == writeTime)
			{
				return nullptr;
			}
			g_failedAsyncMaterialArtifacts.erase(failed);
		}
		EraseCancelledMaterialArtifactByEquivalentPath(g_cancelledAsyncMaterialArtifacts, *this, path, realPath);
		if (CountQueuedMaterialRequestsForOwner(*this) >= kMaxQueuedAsyncMaterialArtifactRequests)
			return nullptr;
		if (CountActiveMaterialRequests() < kMaxPendingAsyncMaterialArtifactRequests)
		{
			try
			{
					auto load = StartMaterialArtifactLoad(request);
					if (load.future.valid())
					{
						request.jobHandle = load.handle;
						request.future = std::move(load.future);
					}
					else if (NLS::Base::Jobs::IsJobSystemInitialized())
					{
						g_failedAsyncMaterialArtifacts[{ this, path }] = { this, realPath, writeTime };
						return nullptr;
					}
				}
			catch (...)
			{
				g_failedAsyncMaterialArtifacts[{ this, path }] = { this, realPath, writeTime };
				return nullptr;
			}
			}
			g_asyncMaterialRequests.emplace(AsyncMaterialArtifactStateKey{ this, path }, std::move(request));
		}

		return nullptr;
	}

void MaterialManager::CancelAsyncArtifact(const std::string& path)
{
	if (path.empty())
		return;

	const auto realPath = ResolveResourcePath(path);
	std::lock_guard lock(g_asyncMaterialMutex);
	if (auto found = FindAsyncMaterialRequestByEquivalentArtifactPath(g_asyncMaterialRequests, *this, path, realPath);
		found != g_asyncMaterialRequests.end())
	{
		if (found->second.cancelableInterestCount > 0u)
			--found->second.cancelableInterestCount;
		if (found->second.cancelableInterestCount == 0u && found->second.sharedInterestCount == 0u)
		{
			if (found->second.cancelled)
				found->second.cancelled->store(true, std::memory_order_release);
			if (!found->second.future.valid())
			{
				g_asyncMaterialRequests.erase(found);
				return;
			}
			g_cancelledAsyncMaterialArtifacts.insert({ found->second.owner, found->second.path, found->second.realPath });
		}
		return;
	}
	if (auto failed = FindFailedMaterialLoadByEquivalentArtifactPath(g_failedAsyncMaterialArtifacts, *this, path, realPath);
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
		*this,
		path,
		realPath) != g_asyncMaterialRequests.end();
}

bool MaterialManager::IsAsyncArtifactLoadFailed(const std::string& path) const
{
	const auto realPath = ResolveResourcePath(path);
	const auto writeTime = TryGetLastWriteTime(realPath);
	std::lock_guard lock(g_asyncMaterialMutex);
	auto failed = FindFailedMaterialLoadByEquivalentArtifactPath(g_failedAsyncMaterialArtifacts, *this, path, realPath);
	return failed != g_failedAsyncMaterialArtifacts.end() &&
		ArtifactPathMatchesResolvedPath(failed->second.realPath, realPath) &&
		failed->second.writeTime == writeTime;
}

#if defined(NLS_ENABLE_TEST_HOOKS)
void MaterialManager::ClearAsyncArtifactRequestStateForTesting()
{
	std::vector<AsyncMaterialArtifactRequest> removedRequests;
	{
		std::lock_guard lock(g_asyncMaterialMutex);
		removedRequests.reserve(g_asyncMaterialRequests.size());
		for (auto& [path, request] : g_asyncMaterialRequests)
		{
			if (request.cancelled)
				request.cancelled->store(true, std::memory_order_release);
			removedRequests.push_back(std::move(request));
		}
		g_asyncMaterialRequests.clear();
		g_failedAsyncMaterialArtifacts.clear();
		g_cancelledAsyncMaterialArtifacts.clear();
	}
	for (auto& request : removedRequests)
	{
		if (request.jobHandle.id != 0u)
			NLS::Base::Jobs::Complete(request.jobHandle);
	}
}

bool MaterialManager::WaitForAsyncArtifactWorkersForTesting(const uint32_t timeoutMilliseconds)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
	while (g_activeMaterialArtifactWorkers.load(std::memory_order_acquire) != 0u)
	{
		if (std::chrono::steady_clock::now() >= deadline)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return true;
}

size_t MaterialManager::GetPendingAsyncArtifactRequestCountForTesting()
{
	std::lock_guard lock(g_asyncMaterialMutex);
	return static_cast<size_t>(std::count_if(
		g_asyncMaterialRequests.begin(),
		g_asyncMaterialRequests.end(),
		[](const auto& entry)
		{
			return entry.second.future.valid();
		}));
}

size_t MaterialManager::GetTotalAsyncArtifactRequestCountForTesting()
{
	std::lock_guard lock(g_asyncMaterialMutex);
	return g_asyncMaterialRequests.size();
}

size_t MaterialManager::GetFailedAsyncArtifactRequestCountForTesting()
{
	std::lock_guard lock(g_asyncMaterialMutex);
	return g_failedAsyncMaterialArtifacts.size();
}
#endif

void MaterialManager::PumpAsyncLoads(const size_t maxCompletions)
{
	PumpAsyncMaterialArtifactLoads(*this, maxCompletions, nullptr);
}

void MaterialManager::PumpAsyncLoadsForPaths(
	const std::unordered_set<std::string>& paths,
	const size_t maxCompletions)
{
	if (paths.empty())
		return;
	const auto trackedPaths = BuildTrackedMaterialArtifactPaths(paths);
	PumpAsyncMaterialArtifactLoads(*this, maxCompletions, &trackedPaths);
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
