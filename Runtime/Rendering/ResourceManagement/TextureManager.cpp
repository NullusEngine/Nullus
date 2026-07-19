#include "Core/ResourceManagement/TextureManager.h"
#include <Debug/Logger.h>
#include "Assets/ArtifactManifest.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/Settings/DriverSettings.h"

#include "Assets/NativeArtifactContainer.h"
#include "Rendering/Assets/TextureArtifact.h"
#include <Jobs/BackgroundJobQueue.h>
#include <Jobs/JobSystem.h>
#include <Profiling/PerformanceStageStats.h>
#include <Rendering/Resources/Loaders/TextureLoader.h>

#include <Filesystem/IniFile.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <functional>
#include <fstream>
#include <future>
#include <mutex>
#include <memory>
#include <optional>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
using ETextureFilteringMode = NLS::Render::Settings::ETextureFilteringMode;
using Texture2D = NLS::Render::Resources::Texture2D;
using TextureCube = NLS::Render::Resources::TextureCube;
using TextureLoader = NLS::Render::Resources::Loaders::TextureLoader;

struct AsyncTextureArtifactRequest
{
    const NLS::Core::ResourceManagement::TextureManager* owner = nullptr;
    std::string path;
    std::string realPath;
	ETextureFilteringMode minFilter = ETextureFilteringMode::NEAREST;
	ETextureFilteringMode magFilter = ETextureFilteringMode::NEAREST;
		bool mipmap = false;
		std::optional<std::filesystem::file_time_type> writeTime;
				std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
				size_t cancelableInterestCount = 0u;
				size_t sharedInterestCount = 0u;
				bool retryCancelledCompletion = false;
				NLS::Base::Jobs::JobHandle jobHandle;
			std::future<std::optional<NLS::Render::Assets::TextureArtifactData>> future;
	};

struct FailedTextureArtifactLoad
{
	const NLS::Core::ResourceManagement::TextureManager* owner = nullptr;
	std::string realPath;
	std::optional<std::filesystem::file_time_type> writeTime;
	std::string runtimeSignature;
};

struct AsyncTextureArtifactStateKey
{
	const NLS::Core::ResourceManagement::TextureManager* owner = nullptr;
	std::string path;
	std::string realPath;

	[[nodiscard]] bool operator==(const AsyncTextureArtifactStateKey& other) const
	{
		return owner == other.owner && path == other.path;
	}
};

struct AsyncTextureArtifactStateKeyHash
{
	[[nodiscard]] size_t operator()(const AsyncTextureArtifactStateKey& key) const
	{
		return std::hash<const NLS::Core::ResourceManagement::TextureManager*>{}(key.owner) ^
			(std::hash<std::string>{}(key.path) << 1u);
	}
};

std::mutex g_asyncTextureMutex;
std::condition_variable g_asyncTextureCondition;
std::unordered_map<AsyncTextureArtifactStateKey, AsyncTextureArtifactRequest, AsyncTextureArtifactStateKeyHash> g_asyncTextureRequests;
std::unordered_map<AsyncTextureArtifactStateKey, AsyncTextureArtifactRequest*, AsyncTextureArtifactStateKeyHash> g_completingAsyncTextureRequests;
std::unordered_map<AsyncTextureArtifactStateKey, FailedTextureArtifactLoad, AsyncTextureArtifactStateKeyHash> g_failedAsyncTextureArtifacts;
std::unordered_set<AsyncTextureArtifactStateKey, AsyncTextureArtifactStateKeyHash> g_cancelledAsyncTextureArtifacts;
std::atomic_size_t g_activeTextureArtifactWorkers {0u};
#if defined(NLS_ENABLE_TEST_HOOKS)
std::function<void()> g_beforeAsyncTextureCompletionForTesting;
#endif
constexpr size_t kMaxPendingAsyncTextureArtifactRequests = 32u;
constexpr size_t kMaxQueuedAsyncTextureArtifactRequests = 256u;

struct TrackedTextureArtifactPaths
{
	std::unordered_set<std::string> sourcePaths;
	std::unordered_set<std::string> normalizedRealPaths;
};

ETextureFilteringMode ParseTextureFilteringMode(int value)
{
	switch (value)
	{
	case 0x2600: return ETextureFilteringMode::NEAREST;
	case 0x2601: return ETextureFilteringMode::LINEAR;
	case 0x2700: return ETextureFilteringMode::NEAREST_MIPMAP_NEAREST;
	case 0x2701: return ETextureFilteringMode::LINEAR_MIPMAP_NEAREST;
	case 0x2702: return ETextureFilteringMode::NEAREST_MIPMAP_LINEAR;
	case 0x2703: return ETextureFilteringMode::LINEAR_MIPMAP_LINEAR;
	default: return static_cast<ETextureFilteringMode>(value);
	}
}

std::tuple<ETextureFilteringMode, ETextureFilteringMode, bool> GetTextureMetadata(const std::string& path)
{
	auto metaFile = NLS::Filesystem::IniFile(path + ".meta");

	auto min = metaFile.GetOrDefault("MIN_FILTER", static_cast<int>(ETextureFilteringMode::LINEAR_MIPMAP_LINEAR));
	auto mag = metaFile.GetOrDefault("MAG_FILTER", static_cast<int>(ETextureFilteringMode::LINEAR));
	auto mipmap = metaFile.GetOrDefault("ENABLE_MIPMAPPING", true);

	return { ParseTextureFilteringMode(min), ParseTextureFilteringMode(mag), mipmap };
}

bool IsTextureArtifactPath(const std::string& path)
{
	return NLS::Core::Assets::ReadNativeArtifactPayloadPrefixFromFile(
		path,
		NLS::Core::Assets::ArtifactType::Texture,
		4u,
		0u).has_value();
}

std::optional<std::filesystem::file_time_type> TryGetLastWriteTime(const std::string& path)
{
	std::error_code error;
	auto writeTime = std::filesystem::last_write_time(path, error);
	if (error)
		return std::nullopt;
	return writeTime;
}

std::string CurrentTextureRuntimeSignature()
{
	auto* driver = NLS::Render::Context::TryGetLocatedDriver();
	if (driver == nullptr)
		return "driver:none";

	auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(*driver);
	if (device == nullptr)
		return "device:none";

	const auto nativeInfo = device->GetNativeDeviceInfo();
	const auto& capabilities = device->GetCapabilities();
	std::string signature =
		"backend=" + std::to_string(static_cast<uint32_t>(nativeInfo.backend)) +
		"|ready=" + std::to_string(device->IsBackendReady() ? 1u : 0u);
	for (const auto format : {
		NLS::Render::RHI::TextureFormat::BC1,
		NLS::Render::RHI::TextureFormat::BC3,
		NLS::Render::RHI::TextureFormat::BC5,
		NLS::Render::RHI::TextureFormat::BC7 })
	{
		const auto& capability = capabilities.GetTextureFormatCapability(format);
		signature +=
			"|" + std::to_string(static_cast<uint32_t>(format)) +
			":" + std::to_string(capability.sampled ? 1u : 0u) +
			std::to_string(capability.upload ? 1u : 0u) +
			std::to_string(capability.supportsSrgbView ? 1u : 0u);
	}
	return signature;
}

std::string NormalizeResolvedArtifactPath(std::string path)
{
	if (path.empty())
		return {};

	try
	{
		std::replace(path.begin(), path.end(), '\\', '/');
		return std::filesystem::path(path).lexically_normal().generic_string();
	}
	catch (const std::filesystem::filesystem_error&)
	{
		return {};
	}
	catch (const std::system_error&)
	{
		return {};
	}
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
std::string TextureManager::ResolveResourcePath(const std::string& path)
{
	return GetRealPath(path);
}

namespace
{
bool TextureRequestMatchesAnyTrackedPath(
	const AsyncTextureArtifactRequest& request,
	const TrackedTextureArtifactPaths& paths);

auto FindAsyncTextureRequestByEquivalentArtifactPath(
    std::unordered_map<AsyncTextureArtifactStateKey, AsyncTextureArtifactRequest, AsyncTextureArtifactStateKeyHash>& requests,
    const TextureManager& manager,
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

auto FindCompletingTextureRequestByEquivalentArtifactPath(
	std::unordered_map<AsyncTextureArtifactStateKey, AsyncTextureArtifactRequest*, AsyncTextureArtifactStateKeyHash>& requests,
	const TextureManager& manager,
	const std::string& path,
	const std::string& realPath)
{
	return std::find_if(
		requests.begin(),
		requests.end(),
		[&manager, &path, &realPath](const auto& entry)
		{
			const auto* request = entry.second;
			return request != nullptr &&
				request->owner == &manager &&
				(entry.first.path == path ||
				 ArtifactPathMatchesResolvedPath(request->realPath, realPath));
		});
}

auto FindFailedTextureLoadByEquivalentArtifactPath(
	std::unordered_map<AsyncTextureArtifactStateKey, FailedTextureArtifactLoad, AsyncTextureArtifactStateKeyHash>& failures,
	const TextureManager& manager,
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

void EraseCancelledTextureArtifactByEquivalentPath(
	std::unordered_set<AsyncTextureArtifactStateKey, AsyncTextureArtifactStateKeyHash>& cancelledArtifacts,
	const TextureManager& manager,
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

bool ConsumeCancelledTextureArtifactByEquivalentPath(const AsyncTextureArtifactRequest& request)
{
	auto cancelled = std::find_if(
		g_cancelledAsyncTextureArtifacts.begin(),
		g_cancelledAsyncTextureArtifacts.end(),
		[&request](const auto& entry)
		{
			return entry.owner == request.owner &&
				(entry.path == request.path ||
				 ArtifactPathMatchesResolvedPath(entry.realPath, request.realPath));
		});
	if (cancelled == g_cancelledAsyncTextureArtifacts.end())
		return false;
	g_cancelledAsyncTextureArtifacts.erase(cancelled);
	return true;
}

void EraseCompletingTextureRequestLocked(const AsyncTextureArtifactRequest& request)
{
	for (auto it = g_completingAsyncTextureRequests.begin(); it != g_completingAsyncTextureRequests.end(); ++it)
	{
		if (it->second == &request)
		{
			g_completingAsyncTextureRequests.erase(it);
			return;
		}
	}
}

void RecordTextureCompletionFailureUnlessCancelled(const AsyncTextureArtifactRequest& request)
{
	EraseCompletingTextureRequestLocked(request);
	if (ConsumeCancelledTextureArtifactByEquivalentPath(request))
	{
		g_failedAsyncTextureArtifacts.erase({request.owner, request.path});
		return;
	}
	g_failedAsyncTextureArtifacts[{request.owner, request.path}] =
		{request.owner, request.realPath, request.writeTime, CurrentTextureRuntimeSignature()};
}

class CompletingTextureRequestScope
{
public:
	explicit CompletingTextureRequestScope(AsyncTextureArtifactRequest& request)
		: m_request(&request)
	{
	}

	~CompletingTextureRequestScope()
	{
		{
			std::lock_guard lock(g_asyncTextureMutex);
			EraseCompletingTextureRequestLocked(*m_request);
		}
		g_asyncTextureCondition.notify_all();
	}

	CompletingTextureRequestScope(const CompletingTextureRequestScope&) = delete;
	CompletingTextureRequestScope& operator=(const CompletingTextureRequestScope&) = delete;

private:
	AsyncTextureArtifactRequest* m_request = nullptr;
};

class TextureArtifactWorkerScope
{
public:
	TextureArtifactWorkerScope() { g_activeTextureArtifactWorkers.fetch_add(1u, std::memory_order_acq_rel); }
	~TextureArtifactWorkerScope() { g_activeTextureArtifactWorkers.fetch_sub(1u, std::memory_order_acq_rel); }
};

struct TextureArtifactJobPayload
{
	std::string realPath;
	std::shared_ptr<std::atomic_bool> cancellationFlag;
	std::promise<std::optional<NLS::Render::Assets::TextureArtifactData>> promise;
};

struct TextureArtifactLoadSubmission
{
	NLS::Base::Jobs::JobHandle handle;
	std::future<std::optional<NLS::Render::Assets::TextureArtifactData>> future;
};

void RunTextureArtifactJob(void* userData)
{
	std::unique_ptr<TextureArtifactJobPayload> payload(static_cast<TextureArtifactJobPayload*>(userData));
	TextureArtifactWorkerScope workerScope;
	try
	{
		payload->promise.set_value(NLS::Render::Assets::LoadTextureArtifact(
			payload->realPath,
			payload->cancellationFlag.get()));
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

void CancelTextureArtifactJob(void* userData)
{
	std::unique_ptr<TextureArtifactJobPayload> payload(static_cast<TextureArtifactJobPayload*>(userData));
	if (payload->cancellationFlag)
		payload->cancellationFlag->store(true, std::memory_order_release);
	try
	{
		payload->promise.set_value(std::nullopt);
	}
	catch (...)
	{
	}
}

TextureArtifactLoadSubmission StartTextureArtifactLoad(
	const AsyncTextureArtifactRequest& request)
{
	auto payload = std::make_unique<TextureArtifactJobPayload>();
	payload->realPath = request.realPath;
	payload->cancellationFlag = request.cancelled;
	auto future = payload->promise.get_future();

	NLS::Base::Jobs::BackgroundJobDesc desc;
	desc.function = RunTextureArtifactJob;
	desc.userData = payload.get();
	desc.cancelFunction = CancelTextureArtifactJob;
	desc.cancelUserData = payload.get();
	desc.priority = NLS::Base::Jobs::JobPriority::High;
	desc.debugName = "TextureManager::AsyncArtifactLoad";

	const auto handle = NLS::Base::Jobs::ScheduleBackgroundJob(desc);
	if (handle.id == 0u)
		return {};

	(void)payload.release();
	return {handle, std::move(future)};
}

size_t CountActiveTextureRequests()
{
	return static_cast<size_t>(std::count_if(
		g_asyncTextureRequests.begin(),
		g_asyncTextureRequests.end(),
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

size_t CountQueuedTextureRequestsForOwner(const TextureManager& manager)
{
	const auto queuedCount = static_cast<size_t>(std::count_if(
		g_asyncTextureRequests.begin(),
		g_asyncTextureRequests.end(),
		[&manager](const auto& entry)
		{
			return entry.second.owner == &manager;
		}));
	const auto completingCount = static_cast<size_t>(std::count_if(
		g_completingAsyncTextureRequests.begin(),
		g_completingAsyncTextureRequests.end(),
		[&manager](const auto& entry)
		{
			return entry.second != nullptr && entry.second->owner == &manager;
		}));
	return queuedCount + completingCount;
}

void PromoteQueuedTextureArtifactLoads(
	TextureManager& manager,
	const TrackedTextureArtifactPaths* paths)
{
	for (;;)
	{
		if (CountActiveTextureRequests() >= kMaxPendingAsyncTextureArtifactRequests)
			return;

		auto found = std::find_if(
			g_asyncTextureRequests.begin(),
			g_asyncTextureRequests.end(),
			[&manager, paths](auto& entry)
			{
				if (entry.second.owner != &manager || entry.second.future.valid())
					return false;
				return paths == nullptr || TextureRequestMatchesAnyTrackedPath(entry.second, *paths);
			});
		if (found == g_asyncTextureRequests.end())
			return;

			try
			{
					auto load = StartTextureArtifactLoad(found->second);
					if (!load.future.valid())
					{
						if (NLS::Base::Jobs::IsJobSystemInitialized())
						{
							g_failedAsyncTextureArtifacts[found->first] =
								{ found->second.owner, found->second.realPath, found->second.writeTime, CurrentTextureRuntimeSignature() };
							g_asyncTextureRequests.erase(found);
							continue;
						}
						return;
					}
					found->second.jobHandle = load.handle;
					found->second.future = std::move(load.future);
			}
		catch (...)
		{
			g_failedAsyncTextureArtifacts[found->first] =
				{ found->second.owner, found->second.realPath, found->second.writeTime, CurrentTextureRuntimeSignature() };
			g_asyncTextureRequests.erase(found);
		}
	}
}

void ClearAsyncTextureArtifactStateForOwner(const TextureManager& manager)
{
	std::vector<AsyncTextureArtifactRequest> removedRequests;
	{
		std::unique_lock lock(g_asyncTextureMutex);
		for (auto it = g_asyncTextureRequests.begin(); it != g_asyncTextureRequests.end();)
		{
			if (it->second.owner == &manager)
			{
				if (it->second.cancelled)
					it->second.cancelled->store(true, std::memory_order_release);
				removedRequests.push_back(std::move(it->second));
				it = g_asyncTextureRequests.erase(it);
			}
			else
				++it;
		}
		for (const auto& [_, request] : g_completingAsyncTextureRequests)
		{
			if (request == nullptr || request->owner != &manager)
				continue;
			if (request->cancelled)
				request->cancelled->store(true, std::memory_order_release);
			request->cancelableInterestCount = 0u;
			request->sharedInterestCount = 0u;
			g_cancelledAsyncTextureArtifacts.insert({request->owner, request->path, request->realPath});
		}
		g_asyncTextureCondition.wait(lock, [&manager]()
		{
			return std::none_of(
				g_completingAsyncTextureRequests.begin(),
				g_completingAsyncTextureRequests.end(),
				[&manager](const auto& entry)
				{
					return entry.second != nullptr && entry.second->owner == &manager;
				});
		});
		for (auto it = g_failedAsyncTextureArtifacts.begin(); it != g_failedAsyncTextureArtifacts.end();)
		{
			if (it->second.owner == &manager)
				it = g_failedAsyncTextureArtifacts.erase(it);
			else
				++it;
		}
		for (auto it = g_cancelledAsyncTextureArtifacts.begin(); it != g_cancelledAsyncTextureArtifacts.end();)
		{
			if (it->owner == &manager)
				it = g_cancelledAsyncTextureArtifacts.erase(it);
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

TextureManager::~TextureManager()
{
	ClearAsyncTextureArtifactStateForOwner(*this);
}

Texture2D* TextureManager::CreateResource(const std::string& path)
{
	const auto portablePath = std::filesystem::path(path).generic_string();
	const auto portableArtifactPath = NLS::Core::Assets::TryMakePortableContentArtifactPath(portablePath);
	if (!portableArtifactPath.empty() &&
		!NLS::Core::Assets::IsRuntimeArtifactPathAuthorized(portableArtifactPath))
	{
		return nullptr;
	}

	std::string realPath = GetRealPath(path);

	auto [min, mag, mipmap] = GetTextureMetadata(realPath);

	Texture2D* texture = TextureLoader::Create(realPath, min, mag, mipmap);
	if (texture)
	{
		texture->path = path;
	}

	return texture;
}

void TextureManager::DestroyResource(Texture2D* resource)
{
	TextureLoader::Destroy(resource);
}

Texture2D* TextureManager::RegisterResource(const std::string& path, Texture2D* instance)
{
	InvalidateArtifactLookupIndex();
	auto* registered = AResourceManager<Texture2D>::RegisterResource(path, instance);
	InvalidateArtifactLookupIndex();
	return registered;
}

void TextureManager::UnloadResource(const std::string& path)
{
	InvalidateArtifactLookupIndex();
	AResourceManager<Texture2D>::UnloadResource(path);
	InvalidateArtifactLookupIndex();
}

bool TextureManager::MoveResource(const std::string& previousPath, const std::string& newPath)
{
	InvalidateArtifactLookupIndex();
	const bool moved = AResourceManager<Texture2D>::MoveResource(previousPath, newPath);
	if (moved)
		InvalidateArtifactLookupIndex();
	return moved;
}

void TextureManager::UnloadResources()
{
	InvalidateArtifactLookupIndex();
	AResourceManager<Texture2D>::UnloadResources();
	InvalidateArtifactLookupIndex();
}

void TextureManager::UnregisterResource(const std::string& path)
{
	InvalidateArtifactLookupIndex();
	AResourceManager<Texture2D>::UnregisterResource(path);
	InvalidateArtifactLookupIndex();
}

Texture2D* TextureManager::FindCachedArtifactResourceByResolvedPath(const std::string& realPath) const
{
	const auto target = NormalizeResolvedArtifactPath(realPath);
	if (target.empty())
		return nullptr;

	EnsureArtifactLookupIndex();
	std::lock_guard lock(m_artifactLookupIndexMutex);
	if (auto found = m_texturesByNormalizedArtifactPath.find(target);
		found != m_texturesByNormalizedArtifactPath.end())
	{
		return found->second;
	}
	return nullptr;
}

void TextureManager::InvalidateArtifactLookupIndex() const
{
	std::lock_guard lock(m_artifactLookupIndexMutex);
	m_artifactLookupIndexDirty = true;
	m_artifactLookupIndexedResourceCount = 0u;
	++m_artifactLookupGeneration;
	m_texturesByNormalizedArtifactPath.clear();
}

void TextureManager::EnsureArtifactLookupIndex() const
{
	for (;;)
	{
		uint64_t generationBefore = 0u;
		{
			std::lock_guard lock(m_artifactLookupIndexMutex);
			if (!m_artifactLookupIndexDirty &&
				m_artifactLookupIndexedGeneration == m_artifactLookupGeneration)
			{
				return;
			}
			generationBefore = m_artifactLookupGeneration;
		}

		const auto resources = GetResources();
		std::lock_guard lock(m_artifactLookupIndexMutex);
		if (!m_artifactLookupIndexDirty &&
			m_artifactLookupIndexedResourceCount == resources.size() &&
			m_artifactLookupIndexedGeneration == m_artifactLookupGeneration)
		{
			return;
		}
		if (generationBefore != m_artifactLookupGeneration)
			continue;

		m_texturesByNormalizedArtifactPath.clear();
		m_texturesByNormalizedArtifactPath.reserve(resources.size() * 2u);
		for (const auto& [resourcePath, texture] : resources)
		{
			if (texture == nullptr)
				continue;
			IndexTextureArtifactPath(resourcePath, texture);
			IndexTextureArtifactPath(texture->path, texture);
		}
		m_artifactLookupIndexedResourceCount = resources.size();
		m_artifactLookupIndexedGeneration = m_artifactLookupGeneration;
		m_artifactLookupIndexDirty = false;
#if defined(NLS_ENABLE_TEST_HOOKS)
		++m_artifactLookupIndexRebuildCountForTesting;
#endif
		return;
	}
}

void TextureManager::IndexTextureArtifactPath(const std::string& path, Texture2D* texture) const
{
	if (texture == nullptr || path.empty())
		return;

	const auto key = NormalizeResolvedArtifactPath(ResolveResourcePath(path));
	if (!key.empty())
		m_texturesByNormalizedArtifactPath.try_emplace(key, texture);
}

#if defined(NLS_ENABLE_TEST_HOOKS)
void TextureManager::ClearArtifactLookupIndexForTesting() const
{
	std::lock_guard lock(m_artifactLookupIndexMutex);
	m_artifactLookupIndexDirty = true;
	m_artifactLookupIndexedResourceCount = 0u;
	m_artifactLookupIndexedGeneration = 0u;
	m_texturesByNormalizedArtifactPath.clear();
	m_artifactLookupIndexRebuildCountForTesting = 0u;
}

size_t TextureManager::GetArtifactLookupIndexRebuildCountForTesting() const
{
	std::lock_guard lock(m_artifactLookupIndexMutex);
	return m_artifactLookupIndexRebuildCountForTesting;
}
#endif

void TextureManager::ReloadResource(Texture2D* resource, const std::string& path)
{
	std::string realPath = GetRealPath(path);

	auto [min, mag, mipmap] = GetTextureMetadata(realPath);

	TextureLoader::Reload(resource, realPath, min, mag, mipmap);
}

Texture2D* TextureManager::GetArtifactResource(const std::string& path, const bool tryToLoadIfNotFound)
{
	NLS::Base::Profiling::PerformanceStageScope resolveScope(
		NLS::Base::Profiling::PerformanceStageDomain::Prefab,
		"PrewarmTextureArtifactResolve",
		NLS::Base::Profiling::PerformanceStageThread::Main);
	resolveScope.AddCounter("requestCount");

	if (auto* cached = GetResource(path, false))
	{
		resolveScope.AddCounter("requestedPathCacheHitCount");
		return cached;
	}

	const auto realPath = GetRealPath(path);
	if (auto* cached = FindCachedArtifactResourceByResolvedPath(realPath))
	{
		resolveScope.AddCounter("resolvedPathCacheHitCount");
		return cached;
	}

	if (!tryToLoadIfNotFound)
	{
		resolveScope.AddCounter("loadDisabledMissCount");
		return nullptr;
	}

	resolveScope.AddCounter("createAttemptCount");
	auto* resource = GetResource(path, true);
	resolveScope.AddCounter(resource != nullptr ? "createSuccessCount" : "createFailureCount");
	return resource;
}

Texture2D* TextureManager::RequestAsyncArtifact(const std::string& path, const bool cancelableInterest)
{
		if (auto* cached = GetResource(path, false);
			cached != nullptr && cached->GetTextureHandle() != nullptr)
		{
			return cached;
		}

		const auto realPath = GetRealPath(path);
		if (auto* cached = FindCachedArtifactResourceByResolvedPath(realPath);
			cached != nullptr && cached->GetTextureHandle() != nullptr)
		{
			return cached;
		}
	if (!IsTextureArtifactPath(realPath))
		return nullptr;

		const auto writeTime = TryGetLastWriteTime(realPath);
		const auto runtimeSignature = CurrentTextureRuntimeSignature();
		auto [min, mag, mipmap] = GetTextureMetadata(realPath);
		AsyncTextureArtifactRequest request;
		request.owner = this;
		request.path = path;
		request.realPath = realPath;
		request.minFilter = min;
		request.magFilter = mag;
		request.mipmap = mipmap;
		request.writeTime = writeTime;
		request.cancelableInterestCount = cancelableInterest ? 1u : 0u;
		request.sharedInterestCount = cancelableInterest ? 0u : 1u;
		{
			std::lock_guard lock(g_asyncTextureMutex);
				if (auto existing = FindAsyncTextureRequestByEquivalentArtifactPath(g_asyncTextureRequests, *this, path, realPath);
					existing != g_asyncTextureRequests.end())
			{
				if (cancelableInterest)
					++existing->second.cancelableInterestCount;
				else
					++existing->second.sharedInterestCount;
				EraseCancelledTextureArtifactByEquivalentPath(
					g_cancelledAsyncTextureArtifacts,
					*this,
					path,
					realPath);
				if (existing->second.cancelled)
					existing->second.cancelled->store(false, std::memory_order_release);
				existing->second.retryCancelledCompletion = true;
					return nullptr;
				}
				if (auto completing = FindCompletingTextureRequestByEquivalentArtifactPath(
						g_completingAsyncTextureRequests,
						*this,
						path,
						realPath);
					completing != g_completingAsyncTextureRequests.end())
				{
					auto& completingRequest = *completing->second;
					if (cancelableInterest)
						++completingRequest.cancelableInterestCount;
					else
						++completingRequest.sharedInterestCount;
					EraseCancelledTextureArtifactByEquivalentPath(
						g_cancelledAsyncTextureArtifacts,
						*this,
						path,
						realPath);
					if (completingRequest.cancelled)
						completingRequest.cancelled->store(false, std::memory_order_release);
					completingRequest.retryCancelledCompletion = true;
					return nullptr;
				}
			auto failed = FindFailedTextureLoadByEquivalentArtifactPath(g_failedAsyncTextureArtifacts, *this, path, realPath);
		if (failed != g_failedAsyncTextureArtifacts.end())
		{
			if (ArtifactPathMatchesResolvedPath(failed->second.realPath, realPath) &&
				failed->second.writeTime == writeTime &&
				failed->second.runtimeSignature == runtimeSignature)
			{
				return nullptr;
			}
			g_failedAsyncTextureArtifacts.erase(failed);
		}
		EraseCancelledTextureArtifactByEquivalentPath(g_cancelledAsyncTextureArtifacts, *this, path, realPath);
		if (CountQueuedTextureRequestsForOwner(*this) >= kMaxQueuedAsyncTextureArtifactRequests)
			return nullptr;
		if (CountActiveTextureRequests() < kMaxPendingAsyncTextureArtifactRequests)
		{
			try
			{
					auto load = StartTextureArtifactLoad(request);
					if (load.future.valid())
					{
						request.jobHandle = load.handle;
						request.future = std::move(load.future);
					}
					else if (NLS::Base::Jobs::IsJobSystemInitialized())
					{
						g_failedAsyncTextureArtifacts[{ this, path }] = { this, realPath, writeTime, runtimeSignature };
						return nullptr;
					}
				}
			catch (...)
			{
				g_failedAsyncTextureArtifacts[{ this, path }] = { this, realPath, writeTime, runtimeSignature };
				return nullptr;
			}
			}
			g_asyncTextureRequests.emplace(AsyncTextureArtifactStateKey{ this, path }, std::move(request));
		}

		return nullptr;
	}

void TextureManager::CancelAsyncArtifact(const std::string& path, const bool cancelableInterest)
{
	if (path.empty())
		return;

	const auto realPath = GetRealPath(path);
	std::lock_guard lock(g_asyncTextureMutex);
	if (auto found = FindAsyncTextureRequestByEquivalentArtifactPath(g_asyncTextureRequests, *this, path, realPath);
			found != g_asyncTextureRequests.end())
	{
		if (cancelableInterest && found->second.cancelableInterestCount > 0u)
			--found->second.cancelableInterestCount;
		if (!cancelableInterest && found->second.sharedInterestCount > 0u)
			--found->second.sharedInterestCount;
		if (found->second.cancelableInterestCount == 0u && found->second.sharedInterestCount == 0u)
		{
			if (found->second.cancelled)
				found->second.cancelled->store(true, std::memory_order_release);
			if (!found->second.future.valid())
			{
				g_asyncTextureRequests.erase(found);
				return;
			}
			g_cancelledAsyncTextureArtifacts.insert({ found->second.owner, found->second.path, found->second.realPath });
		}
			return;
		}
		if (auto completing = FindCompletingTextureRequestByEquivalentArtifactPath(
				g_completingAsyncTextureRequests,
				*this,
				path,
				realPath);
			completing != g_completingAsyncTextureRequests.end())
		{
			auto& completingRequest = *completing->second;
			if (cancelableInterest && completingRequest.cancelableInterestCount > 0u)
				--completingRequest.cancelableInterestCount;
			if (!cancelableInterest && completingRequest.sharedInterestCount > 0u)
				--completingRequest.sharedInterestCount;
			if (completingRequest.cancelableInterestCount == 0u && completingRequest.sharedInterestCount == 0u)
			{
				if (completingRequest.cancelled)
					completingRequest.cancelled->store(true, std::memory_order_release);
				g_cancelledAsyncTextureArtifacts.insert({
					completingRequest.owner,
					completingRequest.path,
					completingRequest.realPath});
				if (auto failed = FindFailedTextureLoadByEquivalentArtifactPath(
						g_failedAsyncTextureArtifacts,
						*this,
						path,
						realPath);
					failed != g_failedAsyncTextureArtifacts.end())
				{
					g_failedAsyncTextureArtifacts.erase(failed);
				}
			}
			return;
		}
		if (auto failed = FindFailedTextureLoadByEquivalentArtifactPath(g_failedAsyncTextureArtifacts, *this, path, realPath);
		failed != g_failedAsyncTextureArtifacts.end())
	{
		g_failedAsyncTextureArtifacts.erase(failed);
	}
}

bool TextureManager::IsAsyncArtifactLoadPending(const std::string& path) const
{
		const auto realPath = GetRealPath(path);
		std::lock_guard lock(g_asyncTextureMutex);
		if (FindAsyncTextureRequestByEquivalentArtifactPath(
			g_asyncTextureRequests,
			*this,
			path,
			realPath) != g_asyncTextureRequests.end())
		{
			return true;
		}
		return FindCompletingTextureRequestByEquivalentArtifactPath(
			g_completingAsyncTextureRequests,
			*this,
			path,
			realPath) != g_completingAsyncTextureRequests.end();
	}

bool TextureManager::IsAsyncArtifactLoadFailed(const std::string& path) const
{
	const auto realPath = GetRealPath(path);
	const auto writeTime = TryGetLastWriteTime(realPath);
	const auto runtimeSignature = CurrentTextureRuntimeSignature();
	std::lock_guard lock(g_asyncTextureMutex);
	auto failed = FindFailedTextureLoadByEquivalentArtifactPath(g_failedAsyncTextureArtifacts, *this, path, realPath);
	return failed != g_failedAsyncTextureArtifacts.end() &&
		ArtifactPathMatchesResolvedPath(failed->second.realPath, realPath) &&
		failed->second.writeTime == writeTime &&
		failed->second.runtimeSignature == runtimeSignature;
}

namespace
{
bool TextureRequestMatchesAnyTrackedPath(
	const AsyncTextureArtifactRequest& request,
	const TrackedTextureArtifactPaths& paths)
{
	if (paths.sourcePaths.find(request.path) != paths.sourcePaths.end())
		return true;
	const auto normalizedRequestPath = NormalizeResolvedArtifactPath(request.realPath);
	return !normalizedRequestPath.empty() &&
		paths.normalizedRealPaths.find(normalizedRequestPath) != paths.normalizedRealPaths.end();
}

TrackedTextureArtifactPaths BuildTrackedTextureArtifactPaths(const std::unordered_set<std::string>& paths)
{
	TrackedTextureArtifactPaths tracked;
	tracked.sourcePaths = paths;
	tracked.normalizedRealPaths.reserve(paths.size());
	for (const auto& path : paths)
	{
		const auto normalized = NormalizeResolvedArtifactPath(TextureManager::ResolveResourcePath(path));
		if (!normalized.empty())
			tracked.normalizedRealPaths.insert(normalized);
	}
	return tracked;
}

void PumpAsyncTextureArtifactLoads(
	TextureManager& manager,
	const size_t maxCompletions,
	const TrackedTextureArtifactPaths* paths,
	const std::function<bool()>& shouldStop = {})
{
	size_t completedCount = 0u;

	while (completedCount < maxCompletions)
	{
		if (completedCount > 0u && shouldStop && shouldStop())
			return;

			AsyncTextureArtifactRequest request;
			CompletingTextureRequestScope completingScope(request);
#if defined(NLS_ENABLE_TEST_HOOKS)
			std::function<void()> beforeCompletionForTesting;
#endif
			{
				std::lock_guard lock(g_asyncTextureMutex);
			PromoteQueuedTextureArtifactLoads(manager, paths);
			auto found = std::find_if(
				g_asyncTextureRequests.begin(),
				g_asyncTextureRequests.end(),
                    [&manager, paths](auto& entry)
                    {
                        if (entry.second.owner != &manager)
                            return false;
                        if (paths != nullptr && !TextureRequestMatchesAnyTrackedPath(entry.second, *paths))
                            return false;
					return entry.second.future.valid() &&
						entry.second.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
				});
			if (found == g_asyncTextureRequests.end())
				return;

				request = std::move(found->second);
				g_asyncTextureRequests.erase(found);
				g_completingAsyncTextureRequests.emplace(
					AsyncTextureArtifactStateKey{request.owner, request.path, request.realPath},
					&request);
#if defined(NLS_ENABLE_TEST_HOOKS)
				beforeCompletionForTesting = g_beforeAsyncTextureCompletionForTesting;
#endif
			}

#if defined(NLS_ENABLE_TEST_HOOKS)
			if (beforeCompletionForTesting)
				beforeCompletionForTesting();
#endif

			std::optional<NLS::Render::Assets::TextureArtifactData> artifact;
		try
		{
			artifact = request.future.get();
		}
		catch (const std::exception& exception)
		{
			NLS_LOG_ERROR(
				std::string("Async texture artifact load failed: ") +
				request.realPath +
				" error=" +
				exception.what());
			std::lock_guard lock(g_asyncTextureMutex);
			RecordTextureCompletionFailureUnlessCancelled(request);
			++completedCount;
			continue;
		}
		catch (...)
		{
			NLS_LOG_ERROR("Async texture artifact load failed: " + request.realPath + " error=unknown");
			std::lock_guard lock(g_asyncTextureMutex);
			RecordTextureCompletionFailureUnlessCancelled(request);
			++completedCount;
			continue;
		}
			{
				std::lock_guard lock(g_asyncTextureMutex);
				if (ConsumeCancelledTextureArtifactByEquivalentPath(request))
				{
					EraseCompletingTextureRequestLocked(request);
					++completedCount;
					continue;
			}
		}
		if (artifact.has_value())
		{
					if (auto* existing = manager.GetArtifactResource(request.path, false);
						existing != nullptr && existing->GetTextureHandle() != nullptr)
					{
						std::lock_guard lock(g_asyncTextureMutex);
						EraseCompletingTextureRequestLocked(request);
						ConsumeCancelledTextureArtifactByEquivalentPath(request);
						g_failedAsyncTextureArtifacts.erase({ request.owner, request.path });
				++completedCount;
				continue;
			}

			Texture2D* texture = nullptr;
			try
			{
				texture = TextureLoader::CreateFromArtifact(
					*artifact,
					request.minFilter,
					request.magFilter,
					request.mipmap);
					if (texture && texture->GetTextureHandle() != nullptr)
					{
						texture->path = request.path;
						bool cancelledBeforeRegistration = false;
						{
							std::lock_guard lock(g_asyncTextureMutex);
							cancelledBeforeRegistration = ConsumeCancelledTextureArtifactByEquivalentPath(request);
							EraseCompletingTextureRequestLocked(request);
							if (!cancelledBeforeRegistration)
							{
								manager.RegisterResource(request.path, texture);
								texture = nullptr;
								g_failedAsyncTextureArtifacts.erase({ request.owner, request.path });
							}
						}
						if (cancelledBeforeRegistration && texture)
						{
							TextureLoader::Destroy(texture);
							texture = nullptr;
						}
					}
				else
				{
					const bool missingGpuHandle = texture != nullptr;
					if (texture)
					{
						TextureLoader::Destroy(texture);
						texture = nullptr;
					}
						std::lock_guard lock(g_asyncTextureMutex);
						EraseCompletingTextureRequestLocked(request);
						if (ConsumeCancelledTextureArtifactByEquivalentPath(request))
						{
							g_failedAsyncTextureArtifacts.erase({ request.owner, request.path });
						}
						else if (!missingGpuHandle &&
							request.retryCancelledCompletion &&
						request.cancelableInterestCount + request.sharedInterestCount > 0u)
					{
						request.cancelled = std::make_shared<std::atomic_bool>(false);
						request.jobHandle = {};
						request.future = {};
						request.retryCancelledCompletion = false;
						g_failedAsyncTextureArtifacts.erase({ request.owner, request.path });
						g_asyncTextureRequests.emplace(
							AsyncTextureArtifactStateKey{ request.owner, request.path },
							std::move(request));
					}
					else
					{
						g_failedAsyncTextureArtifacts[{ request.owner, request.path }] =
							{ request.owner, request.realPath, request.writeTime, CurrentTextureRuntimeSignature() };
					}
				}
			}
			catch (const std::exception& exception)
			{
				if (texture)
					TextureLoader::Destroy(texture);
				NLS_LOG_ERROR(
					std::string("Async texture runtime creation failed: ") +
					request.realPath +
					" error=" +
					exception.what());
				std::lock_guard lock(g_asyncTextureMutex);
				RecordTextureCompletionFailureUnlessCancelled(request);
			}
			catch (...)
			{
				if (texture)
					TextureLoader::Destroy(texture);
				NLS_LOG_ERROR("Async texture runtime creation failed: " + request.realPath + " error=unknown");
				std::lock_guard lock(g_asyncTextureMutex);
				RecordTextureCompletionFailureUnlessCancelled(request);
			}
		}
			else
				{
					std::lock_guard lock(g_asyncTextureMutex);
					EraseCompletingTextureRequestLocked(request);
					if (ConsumeCancelledTextureArtifactByEquivalentPath(request))
					{
						g_failedAsyncTextureArtifacts.erase({ request.owner, request.path });
					}
					else if (request.retryCancelledCompletion &&
						request.cancelableInterestCount + request.sharedInterestCount > 0u)
				{
					request.cancelled = std::make_shared<std::atomic_bool>(false);
					request.jobHandle = {};
					request.future = {};
					request.retryCancelledCompletion = false;
					g_failedAsyncTextureArtifacts.erase({ request.owner, request.path });
					g_asyncTextureRequests.emplace(
						AsyncTextureArtifactStateKey{ request.owner, request.path },
						std::move(request));
				}
				else
				{
					g_failedAsyncTextureArtifacts[{ request.owner, request.path }] =
						{ request.owner, request.realPath, request.writeTime, CurrentTextureRuntimeSignature() };
				}
			}

		++completedCount;
	}
}
}

void TextureManager::PumpAsyncLoads(const size_t maxCompletions)
{
	PumpAsyncTextureArtifactLoads(*this, maxCompletions, nullptr);
}

void TextureManager::PumpAsyncLoadsForPaths(
	const std::unordered_set<std::string>& paths,
	const size_t maxCompletions,
	const std::function<bool()>& shouldStop)
{
	if (paths.empty())
		return;
	const auto trackedPaths = BuildTrackedTextureArtifactPaths(paths);
	PumpAsyncTextureArtifactLoads(*this, maxCompletions, &trackedPaths, shouldStop);
}

AsyncArtifactRequestDiagnostics TextureManager::GetAsyncArtifactRequestDiagnostics()
{
	std::lock_guard lock(g_asyncTextureMutex);
	AsyncArtifactRequestDiagnostics diagnostics;
	diagnostics.totalRequests = g_asyncTextureRequests.size() + g_completingAsyncTextureRequests.size();
	diagnostics.activeRequests = CountActiveTextureRequests();
	diagnostics.failedRequests = g_failedAsyncTextureArtifacts.size();
	diagnostics.maxActiveRequests = kMaxPendingAsyncTextureArtifactRequests;
	for (const auto& [_, request] : g_asyncTextureRequests)
	{
		if (!request.future.valid())
		{
			++diagnostics.queuedRequests;
			continue;
		}
		if (request.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
			++diagnostics.readyRequests;
	}
	diagnostics.readyRequests += g_completingAsyncTextureRequests.size();
	return diagnostics;
}

#if defined(NLS_ENABLE_TEST_HOOKS)
void TextureManager::ClearAsyncArtifactRequestStateForTesting()
{
		std::vector<AsyncTextureArtifactRequest> removedRequests;
		{
			std::unique_lock lock(g_asyncTextureMutex);
		removedRequests.reserve(g_asyncTextureRequests.size());
		for (auto& [path, request] : g_asyncTextureRequests)
		{
			if (request.cancelled)
				request.cancelled->store(true, std::memory_order_release);
			removedRequests.push_back(std::move(request));
		}
			g_asyncTextureRequests.clear();
			for (const auto& [_, request] : g_completingAsyncTextureRequests)
			{
				if (request == nullptr)
					continue;
				if (request->cancelled)
					request->cancelled->store(true, std::memory_order_release);
				request->cancelableInterestCount = 0u;
				request->sharedInterestCount = 0u;
				g_cancelledAsyncTextureArtifacts.insert({request->owner, request->path, request->realPath});
			}
			g_asyncTextureCondition.wait(lock, []()
			{
				return g_completingAsyncTextureRequests.empty();
			});
			g_failedAsyncTextureArtifacts.clear();
			g_cancelledAsyncTextureArtifacts.clear();
			g_beforeAsyncTextureCompletionForTesting = {};
	}
	for (auto& request : removedRequests)
	{
		if (request.jobHandle.id != 0u)
			NLS::Base::Jobs::Complete(request.jobHandle);
	}
}

bool TextureManager::WaitForAsyncArtifactWorkersForTesting(const uint32_t timeoutMilliseconds)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
	while (g_activeTextureArtifactWorkers.load(std::memory_order_acquire) != 0u)
	{
		if (std::chrono::steady_clock::now() >= deadline)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return true;
}

size_t TextureManager::GetPendingAsyncArtifactRequestCountForTesting()
{
	std::lock_guard lock(g_asyncTextureMutex);
	return static_cast<size_t>(std::count_if(
			g_asyncTextureRequests.begin(),
		g_asyncTextureRequests.end(),
		[](const auto& entry)
		{
				return entry.second.future.valid();
				})) + g_completingAsyncTextureRequests.size();
}

size_t TextureManager::GetMaxPendingAsyncArtifactRequestCountForTesting()
{
	return kMaxPendingAsyncTextureArtifactRequests;
}

size_t TextureManager::GetTotalAsyncArtifactRequestCountForTesting()
{
	std::lock_guard lock(g_asyncTextureMutex);
	return g_asyncTextureRequests.size() + g_completingAsyncTextureRequests.size();
}

size_t TextureManager::GetFailedAsyncArtifactRequestCountForTesting()
{
	std::lock_guard lock(g_asyncTextureMutex);
	return g_failedAsyncTextureArtifacts.size();
}

void TextureManager::SetBeforeAsyncArtifactCompletionForTesting(std::function<void()> callback)
{
	std::lock_guard lock(g_asyncTextureMutex);
	g_beforeAsyncTextureCompletionForTesting = std::move(callback);
}
#endif

TextureCube* TextureManager::CreateCubeMap(const std::vector<std::string>& filePaths)
{
	if (filePaths.size() != 6)
	{
		return nullptr;
	}

	std::vector<std::string> realPaths =
	{
		GetRealPath(filePaths[0]),
		GetRealPath(filePaths[1]),
		GetRealPath(filePaths[2]),
		GetRealPath(filePaths[3]),
		GetRealPath(filePaths[4]),
		GetRealPath(filePaths[5])
	};

	return TextureLoader::CreateCubeMap(realPaths);
}
}
