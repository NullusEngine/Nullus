#include "Core/ResourceManagement/TextureManager.h"
#include "Rendering/Settings/DriverSettings.h"

#include "Rendering/Assets/TextureArtifact.h"
#include <Rendering/Resources/Loaders/TextureLoader.h>

#include <Filesystem/IniFile.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <future>
#include <mutex>
#include <optional>
#include <tuple>
#include <unordered_map>

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
	std::future<std::optional<NLS::Render::Assets::TextureArtifactData>> future;
};

std::mutex g_asyncTextureMutex;
std::unordered_map<std::string, AsyncTextureArtifactRequest> g_asyncTextureRequests;

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
}

namespace NLS::Core::ResourceManagement
{
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

Texture2D* TextureManager::RequestAsyncArtifact(const std::string& path)
{
	if (auto* cached = GetResource(path, false))
		return cached;

	const auto realPath = GetRealPath(path);
	if (!IsTextureArtifactPath(realPath))
		return nullptr;

	{
		std::lock_guard lock(g_asyncTextureMutex);
		if (g_asyncTextureRequests.find(path) != g_asyncTextureRequests.end())
			return nullptr;
	}

	auto [min, mag, mipmap] = GetTextureMetadata(realPath);
	AsyncTextureArtifactRequest request;
	request.path = path;
	request.realPath = realPath;
	request.minFilter = min;
	request.magFilter = mag;
	request.mipmap = mipmap;
	request.future = std::async(
		std::launch::async,
		[realPath]()
		{
			return NLS::Render::Assets::LoadTextureArtifact(realPath);
		});

	std::lock_guard lock(g_asyncTextureMutex);
	g_asyncTextureRequests.emplace(path, std::move(request));
	return nullptr;
}

void TextureManager::PumpAsyncLoads(const size_t maxCompletions)
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
				[](auto& entry)
				{
					return entry.second.future.valid() &&
						entry.second.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
				});
			if (found == g_asyncTextureRequests.end())
				return;

			request = std::move(found->second);
			g_asyncTextureRequests.erase(found);
		}

		auto artifact = request.future.get();
		if (artifact.has_value())
		{
			if (GetResource(request.path, false) != nullptr)
			{
				++completedCount;
				continue;
			}

			if (auto* texture = TextureLoader::CreateFromArtifact(
					*artifact,
					request.minFilter,
					request.magFilter,
					request.mipmap))
			{
				texture->path = request.path;
				RegisterResource(request.path, texture);
			}
		}

		++completedCount;
	}
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
