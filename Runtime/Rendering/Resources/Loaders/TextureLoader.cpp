#include "Rendering/Resources/Loaders/TextureLoader.h"

#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/Resources/TextureCube.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/NativeArtifactContainer.h"

#include <Debug/Logger.h>
#include <Image.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <optional>
#include <algorithm>
#include <vector>

namespace
{
bool ShouldTryLoadTextureArtifact(const std::string& path)
{
	if (NLS::Core::Assets::TryMakePortableContentArtifactPath(path).empty())
		return false;

	return NLS::Core::Assets::ReadNativeArtifactPayloadPrefixFromFile(
		path,
		NLS::Core::Assets::ArtifactType::Texture,
		4u,
		0u).has_value();
}

std::chrono::microseconds NonZeroElapsedMicros(
	const std::chrono::steady_clock::time_point begin,
	const std::chrono::steady_clock::time_point end)
{
	auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - begin);
	if (elapsed.count() == 0)
		elapsed = std::chrono::microseconds(1);
	return elapsed;
}

size_t TextureArtifactPixelByteCount(const NLS::Render::Assets::TextureArtifactData& artifact)
{
	size_t byteCount = 0u;
	for (const auto& mip : artifact.mips)
		byteCount += mip.PixelSize();
	return byteCount;
}
}

namespace NLS::Render::Resources::Loaders
{
Texture2D* TextureLoader::Create(const std::string& p_filepath, Settings::ETextureFilteringMode p_firstFilter, Settings::ETextureFilteringMode p_secondFilter, bool p_generateMipmap)
{
	if (!ShouldTryLoadTextureArtifact(p_filepath))
		return nullptr;

	if (auto artifact = NLS::Render::Assets::LoadTextureArtifact(p_filepath))
	{
		Texture2D* texture = new Texture2D(Texture2D::SkipInitialTextureTag {});
		texture->firstFilter = p_firstFilter;
		texture->secondFilter = p_secondFilter;
		texture->isMimapped = artifact->mips.size() > 1u || p_generateMipmap;
		if (!texture->SetTextureResource(*artifact))
		{
			if (NLS::Render::RHI::IsTextureFormatCompressed(artifact->format))
			{
				NLS_LOG_WARNING(
					"TextureLoader: unsupported compressed texture artifact \"" +
					p_filepath +
					"\" for current runtime backend");
			}
			delete texture;
			return nullptr;
		}
		texture->path = p_filepath;
		return texture;
	}

	return nullptr;
}

Texture2D* TextureLoader::CreateFromImageFile(const std::string& p_filepath, Settings::ETextureFilteringMode p_firstFilter, Settings::ETextureFilteringMode p_secondFilter, bool p_generateMipmap)
{
	Image image(p_filepath, true);
	if (image.GetData() == nullptr)
		return nullptr;

	Texture2D* texture = CreateFromImage(&image, p_firstFilter, p_secondFilter, p_generateMipmap);
	if (texture)
	{
		texture->path = p_filepath;
	}

	return texture;
}

TextureCube* TextureLoader::CreateCubeMap(const std::vector<std::string>& filePaths)
{
	(void)filePaths;
	return nullptr;
}

Texture2D* TextureLoader::CreatePixel(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	std::array<uint8_t, 4> colorData = { r, g, b, a };

	Image image(1, 1, 4);
	image.SetData(colorData.data());

	return CreateFromImage(
		&image,
		Settings::ETextureFilteringMode::NEAREST,
		Settings::ETextureFilteringMode::NEAREST,
		false
	);
}

Texture2D* TextureLoader::CreateFromMemory(uint8_t* p_data, uint32_t p_width, uint32_t p_height, Settings::ETextureFilteringMode p_firstFilter, Settings::ETextureFilteringMode p_secondFilter, bool p_generateMipmap)
{
	Image image(p_width, p_height, 4);
	image.SetData(p_data);

	return CreateFromImage(&image, p_firstFilter, p_secondFilter, p_generateMipmap);
}

Texture2D* TextureLoader::CreateFromRgba8Memory(
	const void* p_data,
	const size_t p_dataSize,
	const uint32_t p_width,
	const uint32_t p_height,
	const Settings::ETextureFilteringMode p_firstFilter,
	const Settings::ETextureFilteringMode p_secondFilter,
	const bool p_generateMipmap)
{
	Texture2D* texture = new Texture2D(Texture2D::SkipInitialTextureTag {});
	texture->isMimapped = p_generateMipmap;
	texture->firstFilter = p_firstFilter;
	texture->secondFilter = p_secondFilter;
	if (!texture->SetRgba8TextureResource(p_data, p_dataSize, p_width, p_height))
	{
		delete texture;
		return nullptr;
	}
	return texture;
}

Texture2D* TextureLoader::CreateFromImage(const Image* image, Settings::ETextureFilteringMode p_firstFilter, Settings::ETextureFilteringMode p_secondFilter, bool p_generateMipmap)
{
	Texture2D* texture = new Texture2D();

	texture->isMimapped = p_generateMipmap;
	texture->firstFilter = p_firstFilter;
	texture->secondFilter = p_secondFilter;
	texture->SetTextureResource(image);

	return texture;
}

Texture2D* TextureLoader::CreateFromArtifact(
	const NLS::Render::Assets::TextureArtifactData& artifact,
	Settings::ETextureFilteringMode p_firstFilter,
	Settings::ETextureFilteringMode p_secondFilter,
	bool p_generateMipmap)
{
	const auto begin = std::chrono::steady_clock::now();
	Texture2D* texture = new Texture2D(Texture2D::SkipInitialTextureTag {});
	texture->firstFilter = p_firstFilter;
	texture->secondFilter = p_secondFilter;
	texture->isMimapped = artifact.mips.size() > 1u || p_generateMipmap;
	if (!texture->SetTextureResource(artifact))
	{
		delete texture;
		NLS::Core::Assets::RecordArtifactLoadTelemetry({
			NLS::Core::Assets::ArtifactLoadTelemetryStage::RuntimeResourceCreation,
			NonZeroElapsedMicros(begin, std::chrono::steady_clock::now()),
			TextureArtifactPixelByteCount(artifact),
			{}
		});
		return nullptr;
	}

	NLS::Core::Assets::RecordArtifactLoadTelemetry({
		NLS::Core::Assets::ArtifactLoadTelemetryStage::RuntimeResourceCreation,
		NonZeroElapsedMicros(begin, std::chrono::steady_clock::now()),
		TextureArtifactPixelByteCount(artifact),
		{}
	});
	return texture;
}

void TextureLoader::Reload(Texture2D* p_texture, const std::string& p_filePath, Settings::ETextureFilteringMode p_firstFilter, Settings::ETextureFilteringMode p_secondFilter, bool p_generateMipmap)
{
	if (!p_texture)
	{
		return;
	}

	Texture2D* newTexture = Create(p_filePath, p_firstFilter, p_secondFilter, p_generateMipmap);

	if (newTexture)
	{
		p_texture->ReloadFrom(*newTexture);
		delete newTexture;
	}
}

bool TextureLoader::Destroy(Texture2D*& p_textureInstance)
{
	if (p_textureInstance)
	{
		delete p_textureInstance;
		p_textureInstance = nullptr;
		return true;
	}

	return false;
}
}
