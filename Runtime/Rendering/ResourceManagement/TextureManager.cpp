#include "Core/ResourceManagement/TextureManager.h"
#include <Debug/Logger.h>
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/Settings/DriverSettings.h"

#include "Rendering/Assets/TextureArtifact.h"
#include <Rendering/Resources/Loaders/TextureLoader.h>

#include <Filesystem/IniFile.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <exception>
#include <filesystem>
#include <future>
#include <mutex>
#include <memory>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace
{
using ETextureFilteringMode = NLS::Render::Settings::ETextureFilteringMode;
using Texture2D = NLS::Render::Resources::Texture2D;
using TextureCube = NLS::Render::Resources::TextureCube;
using TextureLoader = NLS::Render::Resources::Loaders::TextureLoader;

struct AsyncTextureArtifactRequest
{
	std::string path;
	std::string realPath;
	ETextureFilteringMode minFilter = ETextureFilteringMode::NEAREST;
	ETextureFilteringMode magFilter = ETextureFilteringMode::NEAREST;
	bool mipmap = false;
	std::optional<std::filesystem::file_time_type> writeTime;
	std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
	size_t cancelableInterestCount = 0u;
	bool hasSharedInterest = false;
	std::future<std::optional<NLS::Render::Assets::TextureArtifactData>> future;
};

struct FailedTextureArtifactLoad
{
	std::string realPath;
	std::optional<std::filesystem::file_time_type> writeTime;
	std::string runtimeSignature;
};

std::mutex g_asyncTextureMutex;
std::unordered_map<std::string, AsyncTextureArtifactRequest> g_asyncTextureRequests;
std::unordered_map<std::string, FailedTextureArtifactLoad> g_failedAsyncTextureArtifacts;
std::unordered_set<std::string> g_cancelledAsyncTextureArtifacts;

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
	auto extension = std::filesystem::path(path).extension().string();
	std::transform(
		extension.begin(),
		extension.end(),
		extension.begin(),
		[](const unsigned char character)
		{
			return static_cast<char>(std::tolower(character));
		});
	return extension == ".ntex";
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
std::string TextureManager::ResolveResourcePath(const std::string& path)
{
	return GetRealPath(path);
}

namespace
{
Texture2D* FindCachedTextureByEquivalentArtifactPath(
	TextureManager& manager,
	const std::string& realPath)
{
	const auto target = NormalizeResolvedArtifactPath(realPath);
	if (target.empty())
		return nullptr;

	for (const auto& [resourcePath, texture] : manager.GetResources())
	{
		if (texture == nullptr)
			continue;

		if (NormalizeResolvedArtifactPath(TextureManager::ResolveResourcePath(resourcePath)) == target ||
			NormalizeResolvedArtifactPath(TextureManager::ResolveResourcePath(texture->path)) == target)
		{
			return texture;
		}
	}
	return nullptr;
}

auto FindAsyncTextureRequestByEquivalentArtifactPath(
	std::unordered_map<std::string, AsyncTextureArtifactRequest>& requests,
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

auto FindFailedTextureLoadByEquivalentArtifactPath(
	std::unordered_map<std::string, FailedTextureArtifactLoad>& failures,
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

void EraseCancelledTextureArtifactByEquivalentPath(
	std::unordered_set<std::string>& cancelledArtifacts,
	const std::string& path,
	const std::string& realPath)
{
	cancelledArtifacts.erase(path);
	for (auto it = cancelledArtifacts.begin(); it != cancelledArtifacts.end();)
	{
		if (ArtifactPathMatchesResolvedPath(TextureManager::ResolveResourcePath(*it), realPath))
			it = cancelledArtifacts.erase(it);
		else
			++it;
	}
}
}

Texture2D* TextureManager::CreateResource(const std::string& path)
{
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

void TextureManager::ReloadResource(Texture2D* resource, const std::string& path)
{
	std::string realPath = GetRealPath(path);

	auto [min, mag, mipmap] = GetTextureMetadata(realPath);

	TextureLoader::Reload(resource, realPath, min, mag, mipmap);
}

Texture2D* TextureManager::RequestAsyncArtifact(const std::string& path, const bool cancelableInterest)
{
	if (auto* cached = GetResource(path, false))
		return cached;

	const auto realPath = GetRealPath(path);
	if (auto* cached = FindCachedTextureByEquivalentArtifactPath(*this, realPath))
		return cached;
	if (!IsTextureArtifactPath(realPath))
		return nullptr;

	const auto writeTime = TryGetLastWriteTime(realPath);
	const auto runtimeSignature = CurrentTextureRuntimeSignature();
	auto [min, mag, mipmap] = GetTextureMetadata(realPath);
	AsyncTextureArtifactRequest request;
	request.path = path;
	request.realPath = realPath;
	request.minFilter = min;
	request.magFilter = mag;
	request.mipmap = mipmap;
	request.writeTime = writeTime;
	request.cancelableInterestCount = cancelableInterest ? 1u : 0u;
	request.hasSharedInterest = !cancelableInterest;
	auto cancellationFlag = request.cancelled;
	{
		std::lock_guard lock(g_asyncTextureMutex);
		if (auto existing = FindAsyncTextureRequestByEquivalentArtifactPath(g_asyncTextureRequests, path, realPath);
			existing != g_asyncTextureRequests.end())
		{
			if (cancelableInterest)
				++existing->second.cancelableInterestCount;
			else
				existing->second.hasSharedInterest = true;
			return nullptr;
		}
		auto failed = FindFailedTextureLoadByEquivalentArtifactPath(g_failedAsyncTextureArtifacts, path, realPath);
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
		EraseCancelledTextureArtifactByEquivalentPath(g_cancelledAsyncTextureArtifacts, path, realPath);
		g_asyncTextureRequests.emplace(path, std::move(request));
	}

	std::future<std::optional<NLS::Render::Assets::TextureArtifactData>> future;
	try
	{
		future = std::async(
			std::launch::async,
			[realPath, cancellationFlag]()
			{
				return NLS::Render::Assets::LoadTextureArtifact(realPath, cancellationFlag.get());
			});
	}
	catch (...)
	{
		std::lock_guard lock(g_asyncTextureMutex);
		g_asyncTextureRequests.erase(path);
		g_failedAsyncTextureArtifacts[path] = { realPath, writeTime, runtimeSignature };
		return nullptr;
	}

	std::lock_guard lock(g_asyncTextureMutex);
	auto found = g_asyncTextureRequests.find(path);
	if (found != g_asyncTextureRequests.end())
		found->second.future = std::move(future);
	return nullptr;
}

void TextureManager::CancelAsyncArtifact(const std::string& path)
{
	if (path.empty())
		return;

	const auto realPath = GetRealPath(path);
	std::lock_guard lock(g_asyncTextureMutex);
	if (auto found = FindAsyncTextureRequestByEquivalentArtifactPath(g_asyncTextureRequests, path, realPath);
		found != g_asyncTextureRequests.end())
	{
		if (found->second.cancelableInterestCount > 0u)
			--found->second.cancelableInterestCount;
		if (found->second.cancelableInterestCount == 0u && !found->second.hasSharedInterest)
		{
			if (found->second.cancelled)
				found->second.cancelled->store(true, std::memory_order_release);
			g_cancelledAsyncTextureArtifacts.insert(found->second.path);
		}
		return;
	}
	if (auto failed = FindFailedTextureLoadByEquivalentArtifactPath(g_failedAsyncTextureArtifacts, path, realPath);
		failed != g_failedAsyncTextureArtifacts.end())
	{
		g_failedAsyncTextureArtifacts.erase(failed);
	}
}

bool TextureManager::IsAsyncArtifactLoadPending(const std::string& path) const
{
	const auto realPath = GetRealPath(path);
	std::lock_guard lock(g_asyncTextureMutex);
	return FindAsyncTextureRequestByEquivalentArtifactPath(
		g_asyncTextureRequests,
		path,
		realPath) != g_asyncTextureRequests.end();
}

bool TextureManager::IsAsyncArtifactLoadFailed(const std::string& path) const
{
	const auto realPath = GetRealPath(path);
	const auto writeTime = TryGetLastWriteTime(realPath);
	const auto runtimeSignature = CurrentTextureRuntimeSignature();
	std::lock_guard lock(g_asyncTextureMutex);
	auto failed = FindFailedTextureLoadByEquivalentArtifactPath(g_failedAsyncTextureArtifacts, path, realPath);
	return failed != g_failedAsyncTextureArtifacts.end() &&
		ArtifactPathMatchesResolvedPath(failed->second.realPath, realPath) &&
		failed->second.writeTime == writeTime &&
		failed->second.runtimeSignature == runtimeSignature;
}

namespace
{
bool TextureRequestMatchesAnyTrackedPath(
	const AsyncTextureArtifactRequest& request,
	const std::unordered_set<std::string>& paths)
{
	for (const auto& path : paths)
	{
		if (path == request.path ||
			ArtifactPathMatchesResolvedPath(TextureManager::ResolveResourcePath(path), request.realPath))
		{
			return true;
		}
	}
	return false;
}

void PumpAsyncTextureArtifactLoads(
	TextureManager& manager,
	const size_t maxCompletions,
	const std::unordered_set<std::string>* paths)
{
	size_t completedCount = 0u;

	while (completedCount < maxCompletions)
	{
		AsyncTextureArtifactRequest request;
		{
			std::lock_guard lock(g_asyncTextureMutex);
			auto found = std::find_if(
				g_asyncTextureRequests.begin(),
				g_asyncTextureRequests.end(),
				[paths](auto& entry)
				{
					if (paths != nullptr && !TextureRequestMatchesAnyTrackedPath(entry.second, *paths))
						return false;
					return entry.second.future.valid() &&
						entry.second.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
				});
			if (found == g_asyncTextureRequests.end())
				return;

			request = std::move(found->second);
			g_asyncTextureRequests.erase(found);
		}

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
			g_failedAsyncTextureArtifacts[request.path] =
				{ request.realPath, request.writeTime, CurrentTextureRuntimeSignature() };
			++completedCount;
			continue;
		}
		catch (...)
		{
			NLS_LOG_ERROR("Async texture artifact load failed: " + request.realPath + " error=unknown");
			std::lock_guard lock(g_asyncTextureMutex);
			g_failedAsyncTextureArtifacts[request.path] =
				{ request.realPath, request.writeTime, CurrentTextureRuntimeSignature() };
			++completedCount;
			continue;
		}
		{
			std::lock_guard lock(g_asyncTextureMutex);
			auto cancelled = std::find_if(
				g_cancelledAsyncTextureArtifacts.begin(),
				g_cancelledAsyncTextureArtifacts.end(),
				[&request](const auto& path)
				{
					return path == request.path ||
						ArtifactPathMatchesResolvedPath(TextureManager::ResolveResourcePath(path), request.realPath);
				});
			if (cancelled != g_cancelledAsyncTextureArtifacts.end())
			{
				g_cancelledAsyncTextureArtifacts.erase(cancelled);
				++completedCount;
				continue;
			}
		}
		if (artifact.has_value())
		{
			if (FindCachedTextureByEquivalentArtifactPath(manager, request.realPath) != nullptr)
			{
				std::lock_guard lock(g_asyncTextureMutex);
				g_failedAsyncTextureArtifacts.erase(request.path);
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
				if (texture)
				{
					texture->path = request.path;
					manager.RegisterResource(request.path, texture);
					texture = nullptr;
					std::lock_guard lock(g_asyncTextureMutex);
					g_failedAsyncTextureArtifacts.erase(request.path);
				}
				else
				{
					std::lock_guard lock(g_asyncTextureMutex);
					g_failedAsyncTextureArtifacts[request.path] =
						{ request.realPath, request.writeTime, CurrentTextureRuntimeSignature() };
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
				g_failedAsyncTextureArtifacts[request.path] =
					{ request.realPath, request.writeTime, CurrentTextureRuntimeSignature() };
			}
			catch (...)
			{
				if (texture)
					TextureLoader::Destroy(texture);
				NLS_LOG_ERROR("Async texture runtime creation failed: " + request.realPath + " error=unknown");
				std::lock_guard lock(g_asyncTextureMutex);
				g_failedAsyncTextureArtifacts[request.path] =
					{ request.realPath, request.writeTime, CurrentTextureRuntimeSignature() };
			}
		}
		else
		{
			std::lock_guard lock(g_asyncTextureMutex);
			g_failedAsyncTextureArtifacts[request.path] =
				{ request.realPath, request.writeTime, CurrentTextureRuntimeSignature() };
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
	const size_t maxCompletions)
{
	if (paths.empty())
		return;
	PumpAsyncTextureArtifactLoads(*this, maxCompletions, &paths);
}

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
