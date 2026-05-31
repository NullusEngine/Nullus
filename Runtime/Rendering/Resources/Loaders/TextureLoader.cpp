#include "Rendering/Resources/Loaders/TextureLoader.h"

#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/Resources/TextureCube.h"

#include <Debug/Logger.h>
#include <Image.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <algorithm>
#include <vector>

namespace
{
std::vector<uint8_t> ReadBinaryFile(const std::string& path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
		return {};

	return {
		std::istreambuf_iterator<char>(input),
		std::istreambuf_iterator<char>()
	};
}

std::optional<std::vector<uint8_t>> ExtractImportedTexturePayload(const std::string& path)
{
	if (std::filesystem::path(path).extension() != ".ntex")
		return std::nullopt;

	auto bytes = ReadBinaryFile(path);
	if (bytes.empty())
		return std::vector<uint8_t> {};

	const std::string marker = "PAYLOAD_BEGIN\n";
	const auto found = std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end());
	if (found == bytes.end())
		return std::vector<uint8_t> {};

	const auto payloadBegin = found + static_cast<std::ptrdiff_t>(marker.size());
	return std::vector<uint8_t>(payloadBegin, bytes.end());
}
}

namespace NLS::Render::Resources::Loaders
{
Texture2D* TextureLoader::Create(const std::string& p_filepath, Settings::ETextureFilteringMode p_firstFilter, Settings::ETextureFilteringMode p_secondFilter, bool p_generateMipmap)
{
	if (std::filesystem::path(p_filepath).extension() == ".ntex")
	{
		if (auto artifact = NLS::Render::Assets::LoadTextureArtifact(p_filepath))
		{
			Texture2D* texture = new Texture2D;
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

		const auto importedTexturePayload = ExtractImportedTexturePayload(p_filepath);
		if (!importedTexturePayload.has_value() || importedTexturePayload->empty())
			return nullptr;

		Image image(importedTexturePayload->data(), importedTexturePayload->size(), true);
		if (image.GetData() == nullptr)
			return nullptr;

		Texture2D* texture = CreateFromImage(&image, p_firstFilter, p_secondFilter, p_generateMipmap);
		if (texture)
			texture->path = p_filepath;
		return texture;
	}

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
	if (filePaths.size() != 6)
	{
		return nullptr;
	}

	std::vector<Image> images;
	images.reserve(6);
	for (size_t i = 0; i < 6; ++i)
	{
		images.push_back(Image(filePaths[i], false));
		if (images.back().GetData() == nullptr)
		{
			return nullptr;
		}
	}

	auto cubeMap = new TextureCube();
	if (!cubeMap->SetTextureResource({&images[0], &images[1], &images[2], &images[3], &images[4], &images[5]}))
	{
		delete cubeMap;
		return nullptr;
	}

	return cubeMap;
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

Texture2D* TextureLoader::CreateFromImage(const Image* image, Settings::ETextureFilteringMode p_firstFilter, Settings::ETextureFilteringMode p_secondFilter, bool p_generateMipmap)
{
	Texture2D* texture = new Texture2D;

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
	Texture2D* texture = new Texture2D;
	texture->firstFilter = p_firstFilter;
	texture->secondFilter = p_secondFilter;
	texture->isMimapped = artifact.mips.size() > 1u || p_generateMipmap;
	if (!texture->SetTextureResource(artifact))
	{
		delete texture;
		return nullptr;
	}

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
