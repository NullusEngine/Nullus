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
#include <cstring>
#include <cstdlib>
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
				bool previewPriority = false;
				bool retryCancelledCompletion = false;
				NLS::Base::Jobs::JobHandle jobHandle;
			std::future<std::optional<NLS::Render::Assets::TextureArtifactData>> future;
			uint64_t runtimeUploadRequestId = 0u;
			std::optional<NLS::Render::Assets::TextureArtifactData> runtimeUploadMetadata;
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
	// Keep a small bounded reserve for visible thumbnail preview dependencies.
	constexpr size_t kMaxPreviewReservedAsyncTextureArtifactRequests = 4u;
	constexpr size_t kMaxQueuedAsyncTextureArtifactRequests = 256u;
	constexpr size_t kMaxTotalAsyncTextureArtifactRequests = 512u;

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
	desc.priority = request.previewPriority
		? NLS::Base::Jobs::JobPriority::High
		: NLS::Base::Jobs::JobPriority::Normal;
	desc.debugName = "TextureManager::AsyncArtifactLoad";

	const auto handle = NLS::Base::Jobs::ScheduleBackgroundJob(desc);
	if (handle.id == 0u)
		return {};

	(void)payload.release();
	return {handle, std::move(future)};
}

std::optional<NLS::Render::Context::TextureRuntimeUploadRequest>
BuildRuntimeTextureUploadRequest(
	const AsyncTextureArtifactRequest& request,
	NLS::Render::Assets::TextureArtifactData& artifact)
{
	if (std::getenv("NLS_DISABLE_RUNTIME_TEXTURE_UPLOAD") != nullptr)
		return std::nullopt;

	if (artifact.width == 0u || artifact.height == 0u || artifact.mips.empty())
		return std::nullopt;

	NLS::Render::Context::TextureRuntimeUploadRequest upload;
	upload.desc.extent.width = artifact.width;
	upload.desc.extent.height = artifact.height;
	upload.desc.extent.depth = artifact.depth;
	upload.desc.dimension = artifact.dimension;
	upload.desc.format = artifact.format;
	upload.desc.colorSpace = artifact.colorSpace == NLS::Render::Assets::TextureArtifactColorSpace::Srgb
		? NLS::Render::RHI::TextureColorSpace::SRGB
		: NLS::Render::RHI::TextureColorSpace::Linear;
	upload.desc.mipLevels = static_cast<uint32_t>(artifact.mips.size());
	upload.desc.usage = NLS::Render::RHI::TextureUsageFlags::Sampled;
	upload.desc.debugName = request.path;
	upload.debugName = request.path;
	upload.mips.reserve(artifact.mips.size());
	for (auto& mip : artifact.mips)
	{
		if (!mip.HasPixels())
			return std::nullopt;
		NLS::Render::Context::TextureRuntimeUploadMip uploadMip;
		uploadMip.level = mip.level;
		uploadMip.width = mip.width;
		uploadMip.height = mip.height;
		uploadMip.rowPitch = mip.rowPitch;
		uploadMip.slicePitch = mip.slicePitch;

		constexpr uint32_t kTextureUploadRowPitchAlignment = 256u;
		if (mip.rowPitch == 0u ||
			mip.PixelSize() < static_cast<size_t>(mip.rowPitch) * mip.height)
			return std::nullopt;

		const uint32_t alignedRowPitch =
			(mip.rowPitch + kTextureUploadRowPitchAlignment - 1u) &
			~(kTextureUploadRowPitchAlignment - 1u);
		if (alignedRowPitch != mip.rowPitch)
		{
			uploadMip.pixels.resize(static_cast<size_t>(alignedRowPitch) * mip.height);
			const auto* source = mip.PixelData();
			for (uint32_t row = 0u; row < mip.height; ++row)
			{
				std::memcpy(
					uploadMip.pixels.data() + static_cast<size_t>(row) * alignedRowPitch,
					source + static_cast<size_t>(row) * mip.rowPitch,
					mip.rowPitch);
			}
			uploadMip.rowPitch = alignedRowPitch;
			uploadMip.slicePitch = alignedRowPitch * mip.height;
		}
		else if (!mip.pixels.empty())
		{
			uploadMip.pixels = std::move(mip.pixels);
		}
		else
		{
			uploadMip.pixels.assign(mip.PixelData(), mip.PixelData() + mip.PixelSize());
		}
		upload.mips.push_back(std::move(uploadMip));
	}
	return upload;
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

bool HasTextureArtifactStartCapacity(const bool previewPriority)
{
	const auto activeCount = CountActiveTextureRequests();
	const auto limit = kMaxPendingAsyncTextureArtifactRequests +
		(previewPriority ? kMaxPreviewReservedAsyncTextureArtifactRequests : 0u);
	return activeCount < limit;
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
		const auto activeCount = CountActiveTextureRequests();
		const bool regularCapacity = activeCount < kMaxPendingAsyncTextureArtifactRequests;
		if (activeCount >= kMaxPendingAsyncTextureArtifactRequests +
			kMaxPreviewReservedAsyncTextureArtifactRequests)
			return;

		auto found = std::find_if(
			g_asyncTextureRequests.begin(),
			g_asyncTextureRequests.end(),
			[&manager, paths](auto& entry)
			{
				if (entry.second.owner != &manager || entry.second.future.valid())
					return false;
				return entry.second.previewPriority &&
					(paths == nullptr || TextureRequestMatchesAnyTrackedPath(entry.second, *paths));
			});
		if (found == g_asyncTextureRequests.end())
		{
			if (!regularCapacity)
				return;
			found = std::find_if(
				g_asyncTextureRequests.begin(),
				g_asyncTextureRequests.end(),
				[&manager, paths](auto& entry)
				{
					if (entry.second.owner != &manager || entry.second.future.valid())
						return false;
					return paths == nullptr || TextureRequestMatchesAnyTrackedPath(entry.second, *paths);
				});
		}
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
				if (it->second.runtimeUploadRequestId != 0u)
				{
					if (auto* driver = NLS::Render::Context::TryGetLocatedDriver(); driver != nullptr)
					{
						NLS::Render::Context::DriverResourceAccess::CancelTextureRuntimeUpload(
							*driver,
							it->second.runtimeUploadRequestId);
					}
				}
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

std::optional<Texture2D*> TextureManager::TryFindCachedArtifactResourceByResolvedPath(
	const std::string& realPath) const
{
	const auto target = NormalizeResolvedArtifactPath(realPath);
	if (target.empty())
		return static_cast<Texture2D*>(nullptr);

	std::unique_lock lock(m_artifactLookupIndexMutex, std::try_to_lock);
	if (!lock.owns_lock())
		return std::nullopt;
	if (m_artifactLookupIndexDirty ||
		m_artifactLookupIndexedGeneration != m_artifactLookupGeneration)
		return std::nullopt;

	const auto found = m_texturesByNormalizedArtifactPath.find(target);
	return found != m_texturesByNormalizedArtifactPath.end()
		? found->second
		: nullptr;
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

void TextureManager::PrewarmArtifactsBatched(const std::vector<std::string>& paths)
{
	if (paths.empty())
		return;
	std::shared_ptr<NLS::Render::RHI::RHIDevice> device;
	if (auto* driver = NLS::Render::Context::TryGetLocatedDriver())
		device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(*driver);
	if (device == nullptr)
		return;

	NLS::Base::Profiling::PerformanceStageScope scope(
		NLS::Base::Profiling::PerformanceStageDomain::Prefab,
		"PrewarmTextureBatched",
		NLS::Base::Profiling::PerformanceStageThread::Main);
	const auto begin = std::chrono::steady_clock::now();
	struct Ready
	{
		std::string path;
		NLS::Render::Assets::TextureArtifactData artifact;
		size_t pixelBytes = 0u;
	};
	constexpr size_t kMaxTexturesPerBatch = 16u;
	constexpr size_t kMaxPixelBytesPerBatch = 128u * 1024u * 1024u;
	std::vector<Ready> ready;
	ready.reserve(kMaxTexturesPerBatch);
	std::unordered_set<std::string> seen;
	size_t batchPixelBytes = 0u;
	size_t loaded = 0u;
	size_t failed = 0u;
	size_t batched = 0u;
	size_t batchCount = 0u;
	size_t batchFailures = 0u;
	// See MeshManager::PrewarmArtifactsPipelined: deferred batches are drained
	// once at the end so the main thread keeps decoding instead of waiting on a
	// fence per batch.
	std::vector<std::string> deferredBatchPaths;
	const auto textureIsUploaded = [this](const std::string& path)
	{
		auto* texture = GetArtifactResource(path, false);
		return texture != nullptr && texture->GetTextureHandle() != nullptr;
	};

	const auto flush = [&]()
	{
		if (ready.empty())
			return;
		const bool batching = device->BeginInitialTextureUploadBatch();
		if (batching)
			++batchCount;
		std::vector<std::unique_ptr<Texture2D>> textures;
		if (batching)
		{
			textures.reserve(ready.size());
			for (const auto& item : ready)
			{
				auto [min, mag, mipmap] = GetTextureMetadata(GetRealPath(item.path));
				NLS::Base::Profiling::PerformanceStageScope createScope(
					NLS::Base::Profiling::PerformanceStageDomain::Prefab,
					"PrewarmTextureGpuCreate",
					NLS::Base::Profiling::PerformanceStageThread::Main);
				textures.emplace_back(TextureLoader::CreateFromArtifact(item.artifact, min, mag, mipmap));
			}
		}
		const bool uploaded = !batching || device->EndInitialTextureUploadBatch();
		if (batching && !uploaded)
		{
			++batchFailures;
			textures.clear();
		}
		for (size_t index = 0u; index < ready.size(); ++index)
		{
			const auto& item = ready[index];
			const auto& path = item.path;
			Texture2D* registered = nullptr;
			if (batching && uploaded && textures[index] &&
				textures[index]->GetTextureHandle() != nullptr)
			{
				textures[index]->path = path;
				registered = RegisterResource(path, textures[index].get());
				if (registered == textures[index].get())
					textures[index].release();
			}
			if (registered == nullptr)
			{
				auto [min, mag, mipmap] = GetTextureMetadata(GetRealPath(path));
				std::unique_ptr<Texture2D> fallback(
					TextureLoader::CreateFromArtifact(item.artifact, min, mag, mipmap));
				if (fallback && fallback->GetTextureHandle() != nullptr)
				{
					fallback->path = path;
					registered = RegisterResource(path, fallback.get());
					if (registered == fallback.get())
						fallback.release();
				}
			}
			if (registered != nullptr)
			{
				++loaded;
				if (batching && uploaded)
				{
					++batched;
					deferredBatchPaths.push_back(path);
				}
			}
			else
				++failed;
		}
		ready.clear();
		batchPixelBytes = 0u;
	};

	for (const auto& path : paths)
	{
		if (path.empty() || GetArtifactResource(path, false) != nullptr)
			continue;
		const auto portable = NLS::Core::Assets::TryMakePortableContentArtifactPath(
			std::filesystem::path(path).generic_string());
		if (!portable.empty() && !NLS::Core::Assets::IsRuntimeArtifactPathAuthorized(portable))
			continue;
		const auto realPath = GetRealPath(path);
		if (!IsTextureArtifactPath(realPath) || !seen.insert(realPath).second)
			continue;
		auto artifact = NLS::Render::Assets::LoadTextureArtifact(realPath);
		if (!artifact.has_value())
			continue;
		size_t pixelBytes = 0u;
		for (const auto& mip : artifact->mips)
			pixelBytes += mip.PixelSize();
		if (!ready.empty() &&
			(ready.size() >= kMaxTexturesPerBatch ||
				pixelBytes > kMaxPixelBytesPerBatch - batchPixelBytes))
			flush();
		ready.push_back({path, std::move(*artifact), pixelBytes});
		batchPixelBytes += pixelBytes;
		if (ready.size() == kMaxTexturesPerBatch ||
			batchPixelBytes >= kMaxPixelBytesPerBatch)
			flush();
	}
	flush();

	if (device != nullptr && !deferredBatchPaths.empty())
	{
		const auto drainBegin = std::chrono::steady_clock::now();
		const size_t deferredBatchFailures = device->WaitForInitialUploads();
		NLS_LOG_INFO("[Startup] Batched texture prewarm deferredUploads=" +
			std::to_string(deferredBatchPaths.size()) + " failedBatches=" +
			std::to_string(deferredBatchFailures) + " drainMs=" +
			std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - drainBegin).count()));
		if (deferredBatchFailures != 0u)
		{
			for (const auto& path : deferredBatchPaths)
			{
				if (textureIsUploaded(path))
					continue;

				UnloadResource(path);
				auto [min, mag, mipmap] = GetTextureMetadata(GetRealPath(path));
				auto artifact = NLS::Render::Assets::LoadTextureArtifact(GetRealPath(path));
				if (!artifact.has_value())
				{
					++failed;
					continue;
				}
				std::unique_ptr<Texture2D> fallback(
					TextureLoader::CreateFromArtifact(*artifact, min, mag, mipmap));
				Texture2D* registered = nullptr;
				if (fallback && fallback->GetTextureHandle() != nullptr)
				{
					fallback->path = path;
					registered = RegisterResource(path, fallback.get());
					if (registered == fallback.get())
						fallback.release();
				}
				if (registered != nullptr)
					++loaded;
				else
					++failed;
			}
		}
	}

	scope.AddCounter("loadedCount", loaded);
	scope.AddCounter("failedCount", failed);
	scope.AddCounter("batchedCount", batched);
	scope.AddCounter("uploadBatchCount", batchCount);
	scope.AddCounter("batchFailureCount", batchFailures);
	NLS_LOG_INFO("[Startup] Batched texture prewarm loaded=" + std::to_string(loaded) +
		" failed=" + std::to_string(failed) + " batched=" + std::to_string(batched) +
		" uploadBatches=" + std::to_string(batchCount) +
		" batchFailures=" + std::to_string(batchFailures) +
		" elapsedMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - begin).count()));
}

Texture2D* TextureManager::RequestAsyncArtifact(
	const std::string& path,
	const bool cancelableInterest)
{
	return RequestAsyncArtifactInternal(path, cancelableInterest, false);
}

Texture2D* TextureManager::RequestAsyncArtifactForPreview(
	const std::string& path,
	const bool cancelableInterest)
{
	return RequestAsyncArtifactInternal(path, cancelableInterest, true);
}

std::optional<Texture2D*> TextureManager::TryGetArtifactResource(const std::string& path)
{
	Texture2D* cached = nullptr;
	if (!TryGetResource(path, cached))
		return std::nullopt;
	if (cached != nullptr)
		return cached;

	const auto realPath = GetRealPath(path);
	// Async completion registers the request spelling, while imported
	// resources can be registered under the resolved artifact spelling. The
	// direct probe avoids turning a dirty/rebuilding secondary index into a
	// false "not ready" result on the thumbnail pump.
	if (!realPath.empty())
	{
		if (!TryGetResource(realPath, cached))
			return std::nullopt;
		if (cached != nullptr)
			return cached;
	}

	return TryFindCachedArtifactResourceByResolvedPath(realPath);
}

std::optional<TextureManager::AsyncPreviewRequestResult>
TextureManager::TryRequestAsyncArtifactForPreview(
	const std::string& path,
	const bool cancelableInterest)
{
	return TryRequestAsyncArtifactInternal(path, cancelableInterest, true);
}

std::optional<TextureManager::AsyncPreviewRequestResult>
TextureManager::TryRequestAsyncArtifact(
	const std::string& path,
	const bool cancelableInterest)
{
	return TryRequestAsyncArtifactInternal(path, cancelableInterest, false);
}

std::optional<TextureManager::AsyncPreviewRequestResult>
TextureManager::TryRequestAsyncArtifactInternal(
	const std::string& path,
	const bool cancelableInterest,
	const bool previewPriority)
{
	AsyncPreviewRequestResult result;
	if (path.empty())
		return result;

	const auto cached = TryGetArtifactResource(path);
	if (!cached.has_value())
		return std::nullopt;
	if (*cached != nullptr && (*cached)->GetTextureHandle() != nullptr)
	{
		result.resource = *cached;
		return result;
	}

	const auto realPath = GetRealPath(path);
	const bool trustedPreviewArtifactPath =
		previewPriority && NLS::Core::Assets::IsContentStorageArtifactPath(path);
	if (!trustedPreviewArtifactPath && !IsTextureArtifactPath(realPath))
		return result;

	const auto writeTime = TryGetLastWriteTime(realPath);
	const auto runtimeSignature = CurrentTextureRuntimeSignature();
	auto [minFilter, magFilter, mipmap] = GetTextureMetadata(realPath);
	AsyncTextureArtifactRequest request;
	request.owner = this;
	request.path = path;
	request.realPath = realPath;
	request.minFilter = minFilter;
	request.magFilter = magFilter;
	request.mipmap = mipmap;
	request.writeTime = writeTime;
	request.cancelableInterestCount = cancelableInterest ? 1u : 0u;
	request.sharedInterestCount = cancelableInterest ? 0u : 1u;
	request.previewPriority = previewPriority;

	std::unique_lock lock(g_asyncTextureMutex, std::try_to_lock);
	if (!lock.owns_lock())
		return std::nullopt;

	if (auto existing = FindAsyncTextureRequestByEquivalentArtifactPath(
			g_asyncTextureRequests,
			*this,
			path,
			realPath);
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
		existing->second.previewPriority = true;
		existing->second.retryCancelledCompletion = true;
		result.pending = true;
		return result;
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
		if (completingRequest.cancelled)
			completingRequest.cancelled->store(false, std::memory_order_release);
		completingRequest.retryCancelledCompletion = true;
		result.pending = true;
		return result;
	}

	const auto failed = FindFailedTextureLoadByEquivalentArtifactPath(
		g_failedAsyncTextureArtifacts,
		*this,
		path,
		realPath);
	if (failed != g_failedAsyncTextureArtifacts.end())
	{
		if (ArtifactPathMatchesResolvedPath(failed->second.realPath, realPath) &&
			failed->second.writeTime == writeTime &&
			failed->second.runtimeSignature == runtimeSignature)
		{
			result.failed = true;
			return result;
		}
		g_failedAsyncTextureArtifacts.erase(failed);
	}

	EraseCancelledTextureArtifactByEquivalentPath(
		g_cancelledAsyncTextureArtifacts,
		*this,
		path,
		realPath);
	if (CountQueuedTextureRequestsForOwner(*this) >= kMaxTotalAsyncTextureArtifactRequests)
		return result;

	if (HasTextureArtifactStartCapacity(true))
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
				g_failedAsyncTextureArtifacts[{ this, path }] = {
					this,
					realPath,
					writeTime,
					runtimeSignature
				};
				result.failed = true;
				return result;
			}
		}
		catch (...)
		{
			g_failedAsyncTextureArtifacts[{ this, path }] = {
				this,
				realPath,
				writeTime,
				runtimeSignature
			};
			result.failed = true;
			return result;
		}
	}

	g_asyncTextureRequests.emplace(
		AsyncTextureArtifactStateKey { this, path },
		std::move(request));
	result.pending = true;
	return result;
}

TextureManager::AsyncArtifactLoadProbeResult
TextureManager::TryProbeAsyncArtifactLoad(const std::string& path) const
{
	const auto realPath = GetRealPath(path);
	std::optional<std::filesystem::file_time_type> failedWriteTime;
	std::string failedRealPath;
	std::string failedRuntimeSignature;
	{
		std::unique_lock lock(g_asyncTextureMutex, std::try_to_lock);
		if (!lock.owns_lock())
			return AsyncArtifactLoadProbeResult::Busy;
		if (FindAsyncTextureRequestByEquivalentArtifactPath(
				g_asyncTextureRequests,
				*this,
				path,
				realPath) != g_asyncTextureRequests.end() ||
			FindCompletingTextureRequestByEquivalentArtifactPath(
				g_completingAsyncTextureRequests,
				*this,
				path,
				realPath) != g_completingAsyncTextureRequests.end())
		{
			return AsyncArtifactLoadProbeResult::Pending;
		}

		const auto failed = FindFailedTextureLoadByEquivalentArtifactPath(
			g_failedAsyncTextureArtifacts,
			*this,
			path,
			realPath);
		if (failed == g_failedAsyncTextureArtifacts.end())
			return AsyncArtifactLoadProbeResult::Missing;
		failedRealPath = failed->second.realPath;
		failedWriteTime = failed->second.writeTime;
		failedRuntimeSignature = failed->second.runtimeSignature;
	}

	return ArtifactPathMatchesResolvedPath(failedRealPath, realPath) &&
		failedWriteTime == TryGetLastWriteTime(realPath) &&
		failedRuntimeSignature == CurrentTextureRuntimeSignature()
		? AsyncArtifactLoadProbeResult::Failed
		: AsyncArtifactLoadProbeResult::Missing;
}

Texture2D* TextureManager::RequestAsyncArtifactInternal(
	const std::string& path,
	const bool cancelableInterest,
	const bool previewPriority)
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
	// Thumbnail preparation already resolved content-addressed artifact paths.
	// Avoid a duplicate payload-header read on the UI thread; the asynchronous
	// decoder still validates the artifact before publishing the texture.
	const bool trustedPreviewArtifactPath = previewPriority &&
		NLS::Core::Assets::IsContentStorageArtifactPath(path);
	if (!trustedPreviewArtifactPath && !IsTextureArtifactPath(realPath))
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
		request.previewPriority = previewPriority;
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
				existing->second.previewPriority = existing->second.previewPriority || previewPriority;
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
		const auto queuedRequestCount = CountQueuedTextureRequestsForOwner(*this);
		const auto queuedRequestLimit = previewPriority
			? kMaxTotalAsyncTextureArtifactRequests
			: kMaxQueuedAsyncTextureArtifactRequests;
		if (queuedRequestCount >= queuedRequestLimit)
			return nullptr;
			if (HasTextureArtifactStartCapacity(previewPriority))
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
			if (found->second.runtimeUploadRequestId != 0u)
			{
				if (auto* driver = NLS::Render::Context::TryGetLocatedDriver(); driver != nullptr)
				{
					NLS::Render::Context::DriverResourceAccess::CancelTextureRuntimeUpload(
						*driver,
						found->second.runtimeUploadRequestId);
				}
			}
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
	const std::function<bool()>& shouldStop = {},
	const bool allowReadyCompletionAfterStop = false)
{
	size_t completedCount = 0u;

	// Promotion scans the complete request map and must not run once per
	// completion; with several hundred startup requests that made every pump
	// slice spend seconds rescanning the same active set.
	{
		std::lock_guard lock(g_asyncTextureMutex);
		PromoteQueuedTextureArtifactLoads(manager, paths);
	}

	if (auto* driver = NLS::Render::Context::TryGetLocatedDriver();
		driver != nullptr && NLS::Render::Context::DriverRendererAccess::HasExplicitRHI(*driver))
	{
		std::vector<AsyncTextureArtifactStateKey> runtimeUploadKeys;
		{
			std::lock_guard lock(g_asyncTextureMutex);
			for (const auto& [key, activeRequest] : g_asyncTextureRequests)
			{
				if (activeRequest.owner == &manager && activeRequest.runtimeUploadRequestId != 0u)
					runtimeUploadKeys.push_back(key);
			}
		}

		for (const auto& key : runtimeUploadKeys)
		{
			AsyncTextureArtifactRequest activeRequest;
			{
				std::lock_guard lock(g_asyncTextureMutex);
				const auto found = g_asyncTextureRequests.find(key);
				if (found == g_asyncTextureRequests.end() ||
					found->second.runtimeUploadRequestId == 0u)
				{
					continue;
				}
				activeRequest.owner = found->second.owner;
				activeRequest.path = found->second.path;
				activeRequest.realPath = found->second.realPath;
				activeRequest.minFilter = found->second.minFilter;
				activeRequest.magFilter = found->second.magFilter;
				activeRequest.mipmap = found->second.mipmap;
				activeRequest.writeTime = found->second.writeTime;
				activeRequest.runtimeUploadRequestId = found->second.runtimeUploadRequestId;
				activeRequest.runtimeUploadMetadata = found->second.runtimeUploadMetadata;
			}

			auto uploadResult = NLS::Render::Context::DriverResourceAccess::ConsumeTextureRuntimeUploadResult(
				*driver,
				activeRequest.runtimeUploadRequestId);
			if (!uploadResult.ready)
				continue;

			if (!activeRequest.runtimeUploadMetadata.has_value() ||
				!uploadResult.success ||
				uploadResult.texture == nullptr)
			{
				std::lock_guard lock(g_asyncTextureMutex);
				g_asyncTextureRequests.erase(key);
				g_failedAsyncTextureArtifacts[{ activeRequest.owner, activeRequest.path }] =
					{ activeRequest.owner, activeRequest.realPath, activeRequest.writeTime, CurrentTextureRuntimeSignature() };
				++completedCount;
				continue;
			}

			const auto& metadata = *activeRequest.runtimeUploadMetadata;
			Texture2D* texture = TextureLoader::CreateFromRHIArtifact(
				std::move(uploadResult.texture),
				metadata,
				activeRequest.runtimeUploadMetadata->mips.empty()
					? 1u
					: static_cast<uint32_t>(activeRequest.runtimeUploadMetadata->mips.size()),
				activeRequest.minFilter,
				activeRequest.magFilter,
				activeRequest.mipmap);
			if (texture == nullptr)
			{
				std::lock_guard lock(g_asyncTextureMutex);
				g_asyncTextureRequests.erase(key);
				g_failedAsyncTextureArtifacts[{ activeRequest.owner, activeRequest.path }] =
					{ activeRequest.owner, activeRequest.realPath, activeRequest.writeTime, CurrentTextureRuntimeSignature() };
				++completedCount;
				continue;
			}

			texture->path = activeRequest.path;
			{
				std::lock_guard lock(g_asyncTextureMutex);
				manager.RegisterResource(activeRequest.path, texture);
				g_asyncTextureRequests.erase(key);
				g_failedAsyncTextureArtifacts.erase({ activeRequest.owner, activeRequest.path });
			}
			++completedCount;
			if (completedCount >= maxCompletions)
				return;
		}
	}

	while (completedCount < maxCompletions)
	{
		if (shouldStop && shouldStop() && !allowReadyCompletionAfterStop)
			return;

			AsyncTextureArtifactRequest request;
			CompletingTextureRequestScope completingScope(request);
#if defined(NLS_ENABLE_TEST_HOOKS)
			std::function<void()> beforeCompletionForTesting;
#endif
		{
			std::lock_guard lock(g_asyncTextureMutex);
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
			{
				return;
			}

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
			if (auto* driver = NLS::Render::Context::TryGetLocatedDriver();
				driver != nullptr &&
				NLS::Render::Context::DriverRendererAccess::HasExplicitRHI(*driver) &&
				NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(*driver))
			{
				auto uploadRequest = BuildRuntimeTextureUploadRequest(request, *artifact);
				if (uploadRequest.has_value())
				{
					const uint64_t uploadId =
						NLS::Render::Context::DriverResourceAccess::RequestTextureRuntimeUpload(
							*driver,
							std::move(*uploadRequest));
					if (uploadId != 0u)
					{
						request.runtimeUploadMetadata = *artifact;
						request.runtimeUploadMetadata->backingBytes.reset();
						request.runtimeUploadMetadata->backingStorage.reset();
						request.runtimeUploadMetadata->subresources.clear();
						request.runtimeUploadRequestId = uploadId;
						request.jobHandle = {};
						request.future = {};
						std::lock_guard lock(g_asyncTextureMutex);
						EraseCompletingTextureRequestLocked(request);
						g_asyncTextureRequests.emplace(
							AsyncTextureArtifactStateKey { request.owner, request.path, request.realPath },
							std::move(request));
						++completedCount;
						continue;
					}
				}
			}

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
	const std::function<bool()>& shouldStop,
	const bool allowReadyCompletionAfterStop)
{
	if (paths.empty())
		return;
	const auto trackedPaths = BuildTrackedTextureArtifactPaths(paths);
	PumpAsyncTextureArtifactLoads(
		*this,
		maxCompletions,
		&trackedPaths,
		shouldStop,
		allowReadyCompletionAfterStop);
}

AsyncArtifactRequestDiagnostics TextureManager::GetAsyncArtifactRequestDiagnostics()
{
	std::lock_guard lock(g_asyncTextureMutex);
	AsyncArtifactRequestDiagnostics diagnostics;
	diagnostics.totalRequests = g_asyncTextureRequests.size() + g_completingAsyncTextureRequests.size();
	diagnostics.activeRequests = CountActiveTextureRequests();
	diagnostics.failedRequests = g_failedAsyncTextureArtifacts.size();
	diagnostics.maxActiveRequests =
		kMaxPendingAsyncTextureArtifactRequests + kMaxPreviewReservedAsyncTextureArtifactRequests;
	for (const auto& [_, request] : g_asyncTextureRequests)
	{
		if (request.previewPriority)
		{
			++diagnostics.previewRequests;
			if (!request.future.valid())
				++diagnostics.previewQueuedRequests;
			else if (request.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
				++diagnostics.previewActiveRequests;
		}
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
