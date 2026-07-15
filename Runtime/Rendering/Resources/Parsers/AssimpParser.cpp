#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/scene.h>
#include <assimp/matrix4x4.h>
#include <assimp/postprocess.h>
#include <assimp/quaternion.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "Debug/Logger.h"
#include "Rendering/Resources/Parsers/AssimpParser.h"

namespace NLS::Render::Resources::Parsers
{
namespace
{
using NLS::Render::Assets::ImportedScene;
using NLS::Render::Assets::ImportedSceneMaterialChannel;
using NLS::Render::Assets::ImportedSceneNamedRecord;
using NLS::Render::Assets::ImportedSceneNode;
using NLS::Render::Assets::ImportedScenePrimitive;
using NLS::Render::Assets::SceneModelSourceFormat;

struct AssimpTextureSlot
{
	aiTextureType type = aiTextureType_NONE;
	const char* channelName = "";
};

struct AssimpRawTextureSlot
{
	const char* propertyName = "";
	const char* channelName = "";
};

struct AssimpDirectionTransforms
{
	aiMatrix4x4 linear;
	aiMatrix4x4 normal;
};

struct FbxTexture
{
	uint64_t id = 0u;
	std::string name;
	std::string relativeFilename;
	std::string filename;
};

struct FbxConnection
{
	uint64_t sourceId = 0u;
	uint64_t destinationId = 0u;
	std::string propertyName;
};

struct FbxMaterialGraph
{
	std::unordered_map<uint64_t, FbxTexture> textures;
	std::unordered_map<uint64_t, std::string> materialNames;
	std::vector<FbxConnection> connections;
};

constexpr char kFbx3dsMaxBumpMapProperty[] = "3dsMax|Parameters|bump_map";
constexpr char kFbx3dsMaxBump2dConnectionPrefix[] = "3dsMax|ai_bump2d Parameters/Connections|";
constexpr char kFbxShaderPropertySuffix[] = ".shader";

std::string IndexedKey(const char* prefix, const uint32_t index)
{
	return std::string(prefix) + "/" + std::to_string(index);
}

constexpr int64_t kAssimpImportTimingLogThresholdMilliseconds = 100;
constexpr double kAssimpUnitScaleEpsilon = 1e-4;
constexpr float kAssimpDirectionLengthEpsilon = 1e-8f;

int64_t MillisecondsSince(const std::chrono::steady_clock::time_point start)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - start).count();
}

std::string LowerExtension(const std::filesystem::path& path)
{
	auto extension = path.extension().string();
	std::transform(
		extension.begin(),
		extension.end(),
		extension.begin(),
		[](const unsigned char character)
		{
			return static_cast<char>(std::tolower(character));
		});
	return extension;
}

SceneModelSourceFormat SourceFormatForPath(const std::filesystem::path& path)
{
	const auto extension = LowerExtension(path);
	if (extension == ".fbx")
		return SceneModelSourceFormat::Fbx;
	if (extension == ".obj")
		return SceneModelSourceFormat::Obj;
	return SceneModelSourceFormat::Gltf;
}

void ConfigureAssimpImporterForEditorCache(
	Assimp::Importer& importer,
	const std::filesystem::path& sourcePath)
{
	const auto extension = LowerExtension(sourcePath);
	if (extension != ".fbx")
		return;

	importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_CAMERAS, false);
	importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_LIGHTS, false);
	importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_ANIMATIONS, false);
	importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_WEIGHTS, false);
	importer.SetPropertyBool(AI_CONFIG_IMPORT_NO_SKELETON_MESHES, true);
	importer.SetPropertyInteger(AI_CONFIG_FAVOUR_SPEED, 1);
}

void LogAssimpImportTiming(
	const std::string& sourcePath,
	const char* stage,
	const int64_t elapsedMilliseconds,
	const uint32_t meshCount,
	const uint32_t materialCount)
{
	if (elapsedMilliseconds < kAssimpImportTimingLogThresholdMilliseconds)
		return;

	NLS_LOG_INFO(
		"[AssetImport][Assimp] " +
		std::string(stage) +
		" " +
		std::to_string(elapsedMilliseconds) +
		"ms meshes=" +
		std::to_string(meshCount) +
		" materials=" +
		std::to_string(materialCount) +
		" source=" +
		sourcePath);
}

std::string AiStringToStdString(const aiString& value)
{
	return value.length == 0u ? std::string{} : std::string(value.C_Str());
}

std::string_view TrimAsciiView(std::string_view value)
{
	const auto isSpace = [](const unsigned char character)
	{
		return std::isspace(character) != 0;
	};
	const auto first = std::find_if(value.begin(), value.end(), [&isSpace](const char character)
	{
		return !isSpace(static_cast<unsigned char>(character));
	});
	if (first == value.end())
		return {};
	const auto last = std::find_if(value.rbegin(), value.rend(), [&isSpace](const char character)
	{
		return !isSpace(static_cast<unsigned char>(character));
	}).base();
	return value.substr(
		static_cast<size_t>(std::distance(value.begin(), first)),
		static_cast<size_t>(std::distance(first, last)));
}

bool StartsWith(const std::string_view value, const std::string_view prefix)
{
	return value.size() >= prefix.size() && value.substr(0u, prefix.size()) == prefix;
}

std::string UnquoteFbxAscii(std::string_view value)
{
	value = TrimAsciiView(value);
	if (value.size() >= 2u && value.front() == '"' && value.back() == '"')
		value = value.substr(1u, value.size() - 2u);
	return std::string(value);
}

std::vector<std::string> SplitFbxAsciiFields(const std::string_view value)
{
	std::vector<std::string> fields;
	std::string field;
	bool inQuotes = false;
	for (const char character : value)
	{
		if (character == '"')
			inQuotes = !inQuotes;
		if (character == ',' && !inQuotes)
		{
			fields.emplace_back(TrimAsciiView(field));
			field.clear();
			continue;
		}
		field.push_back(character);
	}
	fields.emplace_back(TrimAsciiView(field));
	return fields;
}

std::optional<uint64_t> ParseFbxAsciiId(const std::string_view value)
{
	const auto trimmed = TrimAsciiView(value);
	if (trimmed.empty())
		return std::nullopt;
	int64_t signedValue = 0;
	const auto [signedEnd, signedError] = std::from_chars(
		trimmed.data(),
		trimmed.data() + trimmed.size(),
		signedValue);
	if (signedError == std::errc{} && signedEnd == trimmed.data() + trimmed.size())
		return static_cast<uint64_t>(signedValue);

	uint64_t unsignedValue = 0u;
	const auto [unsignedEnd, unsignedError] = std::from_chars(
		trimmed.data(),
		trimmed.data() + trimmed.size(),
		unsignedValue);
	if (unsignedError == std::errc{} && unsignedEnd == trimmed.data() + trimmed.size())
		return unsignedValue;
	return std::nullopt;
}

bool ParseFbxAsciiObjectHeader(
	const std::string_view line,
	const std::string_view prefix,
	uint64_t& id,
	std::string& name)
{
	if (!StartsWith(line, prefix))
		return false;
	const auto fields = SplitFbxAsciiFields(line.substr(prefix.size()));
	if (fields.size() < 2u)
		return false;
	const auto parsedId = ParseFbxAsciiId(fields[0]);
	if (!parsedId)
		return false;
	id = *parsedId;
	name = UnquoteFbxAscii(fields[1]);
	return true;
}

int FbxAsciiBraceDelta(const std::string_view line)
{
	int delta = 0;
	bool inQuotes = false;
	for (const char character : line)
	{
		if (character == '"')
			inQuotes = !inQuotes;
		else if (!inQuotes && character == '{')
			++delta;
		else if (!inQuotes && character == '}')
			--delta;
	}
	return delta;
}

std::string ParseFbxAsciiStringProperty(const std::string_view line)
{
	const auto separator = line.find(':');
	return separator == std::string::npos ? std::string{} : UnquoteFbxAscii(line.substr(separator + 1u));
}

bool ReadFbxAsciiMaterialGraph(const std::filesystem::path& sourcePath, FbxMaterialGraph& graph)
{
	std::ifstream input(sourcePath, std::ios::binary);
	if (!input)
		return false;

	std::optional<FbxTexture> activeTexture;
	int textureBraceDepth = 0;
	std::string rawLine;
	while (std::getline(input, rawLine))
	{
		const auto line = TrimAsciiView(rawLine);
		if (activeTexture)
		{
			if (StartsWith(line, "RelativeFilename:"))
				activeTexture->relativeFilename = ParseFbxAsciiStringProperty(line);
			else if (StartsWith(line, "FileName:") || StartsWith(line, "Filename:"))
				activeTexture->filename = ParseFbxAsciiStringProperty(line);

			textureBraceDepth += FbxAsciiBraceDelta(line);
			if (textureBraceDepth <= 0)
			{
				graph.textures[activeTexture->id] = std::move(*activeTexture);
				activeTexture.reset();
			}
			continue;
		}

		FbxTexture texture;
		if (ParseFbxAsciiObjectHeader(line, "Texture:", texture.id, texture.name))
		{
			activeTexture = std::move(texture);
			textureBraceDepth = FbxAsciiBraceDelta(line);
			continue;
		}

		uint64_t materialId = 0u;
		std::string materialName;
		if (ParseFbxAsciiObjectHeader(line, "Material:", materialId, materialName))
		{
			graph.materialNames.emplace(materialId, std::move(materialName));
			continue;
		}

		if (StartsWith(line, "C:"))
		{
			const auto fields = SplitFbxAsciiFields(line.substr(2u));
			if (fields.size() < 3u || UnquoteFbxAscii(fields[0]) != "OP")
				continue;
			const auto sourceId = ParseFbxAsciiId(fields[1]);
			const auto destinationId = ParseFbxAsciiId(fields[2]);
			if (!sourceId || !destinationId)
				continue;
			graph.connections.push_back({
				*sourceId,
				*destinationId,
				fields.size() >= 4u ? UnquoteFbxAscii(fields[3]) : std::string{}
			});
		}
	}

	return !graph.textures.empty() && !graph.materialNames.empty() && !graph.connections.empty();
}

struct FbxBinaryProperty
{
	std::optional<uint64_t> integer;
	std::string text;
};

struct FbxBinaryNodeHeader
{
	uint64_t endOffset = 0u;
	uint64_t propertyCount = 0u;
	uint64_t propertyEnd = 0u;
	std::string name;
};

enum class FbxBinaryNodeResult
{
	Node,
	NullRecord,
	Error
};

class FbxBinaryReader
{
public:
	explicit FbxBinaryReader(const std::filesystem::path& sourcePath)
		: m_input(sourcePath, std::ios::binary)
	{
		if (!m_input)
			return;
		m_input.seekg(0, std::ios::end);
		const auto end = m_input.tellg();
		if (end < 0)
			return;
		m_size = static_cast<uint64_t>(end);
		m_input.seekg(0, std::ios::beg);
	}

	bool Read(FbxMaterialGraph& graph)
	{
		constexpr std::array<unsigned char, 23u> signature = {
			'K', 'a', 'y', 'd', 'a', 'r', 'a', ' ', 'F', 'B', 'X', ' ', 'B', 'i', 'n', 'a', 'r', 'y',
			' ', ' ', 0u, 0x1au, 0u
		};
		std::array<unsigned char, signature.size()> header = {};
		if (!ReadBytes(header.data(), header.size()) || header != signature)
			return false;

		uint32_t version = 0u;
		if (!ReadUint32(version))
			return false;
		m_wideOffsets = version >= 7500u;

		while (Position() < m_size)
		{
			FbxBinaryNodeHeader node;
			const auto result = ReadNodeHeader(m_size, node);
			if (result == FbxBinaryNodeResult::NullRecord)
				break;
			if (result == FbxBinaryNodeResult::Error)
				return false;

			if (!Seek(node.propertyEnd))
				return false;
			if (node.name == "Objects")
			{
				if (!ReadObjects(node.endOffset, graph))
					return false;
			}
			else if (node.name == "Connections")
			{
				if (!ReadConnections(node.endOffset, graph))
					return false;
			}
			else if (!Seek(node.endOffset))
			{
				return false;
			}
		}

		return !graph.textures.empty() && !graph.materialNames.empty() && !graph.connections.empty();
	}

private:
	static constexpr uint32_t kMaximumStringLength = 1024u * 1024u;

	uint64_t Position()
	{
		const auto position = m_input.tellg();
		return position < 0 ? m_size + 1u : static_cast<uint64_t>(position);
	}

	bool Seek(const uint64_t position)
	{
		if (position > m_size)
			return false;
		m_input.clear();
		m_input.seekg(static_cast<std::streamoff>(position), std::ios::beg);
		return static_cast<bool>(m_input);
	}

	bool ReadBytes(void* destination, const size_t size)
	{
		const auto position = Position();
		if (position > m_size || size > m_size - position)
			return false;
		m_input.read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
		return static_cast<size_t>(m_input.gcount()) == size;
	}

	bool ReadBytes(void* destination, const size_t size, const uint64_t boundary)
	{
		const auto position = Position();
		if (boundary > m_size || position > boundary || size > boundary - position)
			return false;
		m_input.read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
		return static_cast<size_t>(m_input.gcount()) == size;
	}

	bool ReadUint8(uint8_t& value)
	{
		return ReadBytes(&value, sizeof(value));
	}

	bool ReadUint8(uint8_t& value, const uint64_t boundary)
	{
		return ReadBytes(&value, sizeof(value), boundary);
	}

	bool ReadUint16(uint16_t& value)
	{
		std::array<unsigned char, 2u> bytes = {};
		if (!ReadBytes(bytes.data(), bytes.size()))
			return false;
		value = static_cast<uint16_t>(bytes[0]) |
			(static_cast<uint16_t>(bytes[1]) << 8u);
		return true;
	}

	bool ReadUint16(uint16_t& value, const uint64_t boundary)
	{
		std::array<unsigned char, 2u> bytes = {};
		if (!ReadBytes(bytes.data(), bytes.size(), boundary))
			return false;
		value = static_cast<uint16_t>(bytes[0]) |
			(static_cast<uint16_t>(bytes[1]) << 8u);
		return true;
	}

	bool ReadUint32(uint32_t& value)
	{
		std::array<unsigned char, 4u> bytes = {};
		if (!ReadBytes(bytes.data(), bytes.size()))
			return false;
		value = static_cast<uint32_t>(bytes[0]) |
			(static_cast<uint32_t>(bytes[1]) << 8u) |
			(static_cast<uint32_t>(bytes[2]) << 16u) |
			(static_cast<uint32_t>(bytes[3]) << 24u);
		return true;
	}

	bool ReadUint32(uint32_t& value, const uint64_t boundary)
	{
		std::array<unsigned char, 4u> bytes = {};
		if (!ReadBytes(bytes.data(), bytes.size(), boundary))
			return false;
		value = static_cast<uint32_t>(bytes[0]) |
			(static_cast<uint32_t>(bytes[1]) << 8u) |
			(static_cast<uint32_t>(bytes[2]) << 16u) |
			(static_cast<uint32_t>(bytes[3]) << 24u);
		return true;
	}

	bool ReadUint64(uint64_t& value)
	{
		std::array<unsigned char, 8u> bytes = {};
		if (!ReadBytes(bytes.data(), bytes.size()))
			return false;
		value = 0u;
		for (size_t index = 0u; index < bytes.size(); ++index)
			value |= static_cast<uint64_t>(bytes[index]) << (index * 8u);
		return true;
	}

	bool ReadUint64(uint64_t& value, const uint64_t boundary)
	{
		std::array<unsigned char, 8u> bytes = {};
		if (!ReadBytes(bytes.data(), bytes.size(), boundary))
			return false;
		value = 0u;
		for (size_t index = 0u; index < bytes.size(); ++index)
			value |= static_cast<uint64_t>(bytes[index]) << (index * 8u);
		return true;
	}

	bool ReadString(const uint32_t length, std::string& value)
	{
		const auto position = Position();
		if (position > m_size || length > m_size - position)
			return false;
		value.resize(length);
		return length == 0u || ReadBytes(value.data(), length);
	}

	bool ReadPropertyString(const uint32_t length, std::string& value, const uint64_t propertyEnd)
	{
		if (length > kMaximumStringLength)
			return false;
		const auto position = Position();
		if (position > propertyEnd || length > propertyEnd - position)
			return false;
		value.resize(length);
		return length == 0u || ReadBytes(value.data(), length, propertyEnd);
	}

	FbxBinaryNodeResult ReadNodeHeader(const uint64_t parentEnd, FbxBinaryNodeHeader& node)
	{
		const auto start = Position();
		const uint64_t headerSize = m_wideOffsets ? 25u : 13u;
		if (start > parentEnd || headerSize > parentEnd - start)
			return FbxBinaryNodeResult::Error;

		uint64_t propertyListLength = 0u;
		uint8_t nameLength = 0u;
		if (m_wideOffsets)
		{
			if (!ReadUint64(node.endOffset) ||
				!ReadUint64(node.propertyCount) ||
				!ReadUint64(propertyListLength) ||
				!ReadUint8(nameLength))
				return FbxBinaryNodeResult::Error;
		}
		else
		{
			uint32_t endOffset = 0u;
			uint32_t propertyCount = 0u;
			uint32_t narrowPropertyListLength = 0u;
			if (!ReadUint32(endOffset) ||
				!ReadUint32(propertyCount) ||
				!ReadUint32(narrowPropertyListLength) ||
				!ReadUint8(nameLength))
				return FbxBinaryNodeResult::Error;
			node.endOffset = endOffset;
			node.propertyCount = propertyCount;
			propertyListLength = narrowPropertyListLength;
		}

		if (node.endOffset == 0u)
			return FbxBinaryNodeResult::NullRecord;
		if (node.endOffset <= start || node.endOffset > parentEnd || nameLength > node.endOffset - Position())
			return FbxBinaryNodeResult::Error;
		if (!ReadString(nameLength, node.name))
			return FbxBinaryNodeResult::Error;
		const auto propertyStart = Position();
		if (propertyStart > node.endOffset || propertyListLength > node.endOffset - propertyStart)
			return FbxBinaryNodeResult::Error;
		node.propertyEnd = propertyStart + propertyListLength;
		return FbxBinaryNodeResult::Node;
	}

	bool ReadProperty(const uint64_t propertyEnd, FbxBinaryProperty& property)
	{
		uint8_t type = 0u;
		if (!ReadUint8(type, propertyEnd))
			return false;

		switch (static_cast<char>(type))
		{
		case 'Y':
		{
			uint16_t value = 0u;
			if (!ReadUint16(value, propertyEnd))
				return false;
			property.integer = static_cast<uint64_t>(static_cast<int16_t>(value));
			return true;
		}
		case 'C':
		{
			uint8_t value = 0u;
			return ReadUint8(value, propertyEnd);
		}
		case 'I':
		{
			uint32_t value = 0u;
			if (!ReadUint32(value, propertyEnd))
				return false;
			property.integer = static_cast<uint64_t>(static_cast<int32_t>(value));
			return true;
		}
		case 'L':
		{
			uint64_t value = 0u;
			if (!ReadUint64(value, propertyEnd))
				return false;
			property.integer = static_cast<uint64_t>(static_cast<int64_t>(value));
			return true;
		}
		case 'F':
		{
			uint32_t ignored = 0u;
			return ReadUint32(ignored, propertyEnd);
		}
		case 'D':
		{
			uint64_t ignored = 0u;
			return ReadUint64(ignored, propertyEnd);
		}
		case 'S':
		case 'R':
		{
			uint32_t length = 0u;
			if (!ReadUint32(length, propertyEnd))
				return false;
			if (type == static_cast<uint8_t>('S'))
				return ReadPropertyString(length, property.text, propertyEnd);
			const auto position = Position();
			if (position > propertyEnd || length > propertyEnd - position)
				return false;
			return Seek(position + length);
		}
		case 'f':
		case 'd':
		case 'l':
		case 'i':
		case 'b':
		case 'c':
		{
			uint32_t arrayLength = 0u;
			uint32_t encoding = 0u;
			uint32_t compressedLength = 0u;
			if (!ReadUint32(arrayLength, propertyEnd) ||
				!ReadUint32(encoding, propertyEnd) ||
				!ReadUint32(compressedLength, propertyEnd))
				return false;
			(void)arrayLength;
			(void)encoding;
			const auto position = Position();
			if (position > propertyEnd || compressedLength > propertyEnd - position)
				return false;
			return Seek(position + compressedLength);
		}
		default:
			return false;
		}
	}

	bool ReadProperties(
		const FbxBinaryNodeHeader& node,
		const size_t maximumCount,
		std::vector<FbxBinaryProperty>& properties)
	{
		const auto count = static_cast<size_t>(std::min<uint64_t>(node.propertyCount, maximumCount));
		properties.reserve(count);
		for (size_t index = 0u; index < count; ++index)
		{
			FbxBinaryProperty property;
			if (!ReadProperty(node.propertyEnd, property))
				return false;
			properties.push_back(std::move(property));
		}
		return Seek(node.propertyEnd);
	}

	bool ReadTextureChildren(const uint64_t parentEnd, const uint64_t textureId, FbxMaterialGraph& graph)
	{
		while (Position() < parentEnd)
		{
			FbxBinaryNodeHeader child;
			const auto result = ReadNodeHeader(parentEnd, child);
			if (result == FbxBinaryNodeResult::NullRecord)
				break;
			if (result == FbxBinaryNodeResult::Error)
				return false;

			if (child.name == "RelativeFilename" || child.name == "FileName" || child.name == "Filename")
			{
				std::vector<FbxBinaryProperty> properties;
				if (!ReadProperties(child, 1u, properties))
					return false;
				if (!properties.empty())
				{
					auto& texture = graph.textures[textureId];
					if (child.name == "RelativeFilename")
						texture.relativeFilename = properties.front().text;
					else
						texture.filename = properties.front().text;
				}
			}
			if (!Seek(child.endOffset))
				return false;
		}
		return Seek(parentEnd);
	}

	bool ReadObjects(const uint64_t parentEnd, FbxMaterialGraph& graph)
	{
		while (Position() < parentEnd)
		{
			FbxBinaryNodeHeader child;
			const auto result = ReadNodeHeader(parentEnd, child);
			if (result == FbxBinaryNodeResult::NullRecord)
				break;
			if (result == FbxBinaryNodeResult::Error)
				return false;

			if (child.name == "Material" || child.name == "Texture")
			{
				std::vector<FbxBinaryProperty> properties;
				if (!ReadProperties(child, 2u, properties))
					return false;
				if (properties.size() >= 2u && properties[0].integer)
				{
					const auto id = *properties[0].integer;
					if (child.name == "Material")
					{
						graph.materialNames.emplace(id, properties[1].text);
					}
					else
					{
						auto& texture = graph.textures[id];
						texture.id = id;
						texture.name = properties[1].text;
						if (!ReadTextureChildren(child.endOffset, id, graph))
							return false;
						continue;
					}
				}
			}
			if (!Seek(child.endOffset))
				return false;
		}
		return Seek(parentEnd);
	}

	bool ReadConnections(const uint64_t parentEnd, FbxMaterialGraph& graph)
	{
		while (Position() < parentEnd)
		{
			FbxBinaryNodeHeader child;
			const auto result = ReadNodeHeader(parentEnd, child);
			if (result == FbxBinaryNodeResult::NullRecord)
				break;
			if (result == FbxBinaryNodeResult::Error)
				return false;

			if (child.name == "C")
			{
				std::vector<FbxBinaryProperty> properties;
				if (!ReadProperties(child, 4u, properties))
					return false;
				if (properties.size() >= 3u &&
					properties[0].text == "OP" &&
					properties[1].integer &&
					properties[2].integer)
				{
					graph.connections.push_back({
						*properties[1].integer,
						*properties[2].integer,
						properties.size() >= 4u ? properties[3].text : std::string{}
					});
				}
			}
			if (!Seek(child.endOffset))
				return false;
		}
		return Seek(parentEnd);
	}

	std::ifstream m_input;
	uint64_t m_size = 0u;
	bool m_wideOffsets = false;
};

bool ReadFbxMaterialGraph(const std::filesystem::path& sourcePath, FbxMaterialGraph& graph)
{
	std::ifstream input(sourcePath, std::ios::binary);
	if (!input)
		return false;
	std::array<char, 18u> header = {};
	input.read(header.data(), static_cast<std::streamsize>(header.size()));
	const bool isBinary = input.gcount() == static_cast<std::streamsize>(header.size()) &&
		std::string_view(header.data(), header.size()) == "Kaydara FBX Binary";
	input.close();

	if (isBinary)
		return FbxBinaryReader(sourcePath).Read(graph);
	return ReadFbxAsciiMaterialGraph(sourcePath, graph);
}

std::string FbxObjectDisplayName(const std::string& name)
{
	const auto separator = name.find("::");
	const auto begin = separator == std::string::npos ? 0u : separator + 2u;
	const auto end = name.find('\0', begin);
	return name.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

bool Is3dsMaxBump2dShaderProperty(const std::string& propertyName)
{
	const auto property = std::string_view(propertyName);
	const auto suffix = std::string_view(kFbxShaderPropertySuffix);
	return StartsWith(property, kFbx3dsMaxBump2dConnectionPrefix) &&
		property.size() >= suffix.size() &&
		property.substr(property.size() - suffix.size()) == suffix;
}

void AddUniqueDependency(std::vector<std::string>* dependencies, const std::string& path);
ImportedSceneMaterialChannel* FindChannel(ImportedSceneNamedRecord& material, const std::string& name);
ImportedSceneMaterialChannel& EnsureChannel(ImportedSceneNamedRecord& material, std::string name);
void AddTextureKeyChannel(
	ImportedSceneNamedRecord& material,
	std::string_view channelName,
	std::string textureKey);

ImportedSceneNamedRecord* FindFbxMaterial(
	const FbxMaterialGraph& graph,
	const uint64_t materialId,
	const std::unordered_map<std::string, ImportedSceneNamedRecord*>& materialsByName,
	const std::unordered_set<std::string>& ambiguousMaterialNames,
	const std::filesystem::path& sourcePath)
{
	const auto materialName = graph.materialNames.find(materialId);
	if (materialName == graph.materialNames.end())
		return nullptr;

	const auto targetName = FbxObjectDisplayName(materialName->second);
	if (ambiguousMaterialNames.find(targetName) != ambiguousMaterialNames.end())
	{
		NLS_LOG_WARNING(
			"[AssetImport][Assimp] Skipped FBX bump2d texture recovery for ambiguous material name '" +
			targetName + "' in " + sourcePath.string());
		return nullptr;
	}
	const auto material = materialsByName.find(targetName);
	return material != materialsByName.end() ? material->second : nullptr;
}

void RecoverFbxBump2dTextureChains(
	const std::filesystem::path& sourcePath,
	ImportedScene& scene,
	std::vector<std::string>* externalDependencies)
{
	FbxMaterialGraph graph;
	if (!ReadFbxMaterialGraph(sourcePath, graph))
		return;

	std::unordered_map<uint64_t, std::vector<const FbxConnection*>> shaderConnectionsByDestination;
	shaderConnectionsByDestination.reserve(graph.connections.size());
	for (const auto& connection : graph.connections)
	{
		if (Is3dsMaxBump2dShaderProperty(connection.propertyName))
			shaderConnectionsByDestination[connection.destinationId].push_back(&connection);
	}

	std::unordered_map<std::string, ImportedSceneNamedRecord*> materialsByName;
	std::unordered_set<std::string> ambiguousMaterialNames;
	materialsByName.reserve(scene.materials.size());
	for (auto& material : scene.materials)
	{
		const auto [found, inserted] = materialsByName.emplace(material.name, &material);
		if (!inserted)
		{
			found->second = nullptr;
			ambiguousMaterialNames.emplace(material.name);
		}
	}

	std::unordered_map<std::string, std::string> textureKeysByUri;
	textureKeysByUri.reserve(scene.textures.size());
	for (const auto& texture : scene.textures)
	{
		if (!texture.uri.empty())
			textureKeysByUri.emplace(texture.uri, texture.sourceKey);
	}

	for (const auto& materialConnection : graph.connections)
	{
		if (materialConnection.propertyName != kFbx3dsMaxBumpMapProperty)
			continue;
		const auto intermediateTexture = graph.textures.find(materialConnection.sourceId);
		if (intermediateTexture == graph.textures.end())
			continue;
		if (!intermediateTexture->second.relativeFilename.empty() || !intermediateTexture->second.filename.empty())
			continue;

		const FbxTexture* fileTexture = nullptr;
		const std::string* uri = nullptr;
		const auto nestedConnections = shaderConnectionsByDestination.find(materialConnection.sourceId);
		if (nestedConnections == shaderConnectionsByDestination.end())
			continue;
		for (const auto* nested : nestedConnections->second)
		{
			const auto candidate = graph.textures.find(nested->sourceId);
			if (candidate == graph.textures.end())
				continue;
			const auto& candidateUri = !candidate->second.relativeFilename.empty()
				? candidate->second.relativeFilename
				: candidate->second.filename;
			if (candidateUri.empty())
				continue;
			fileTexture = &candidate->second;
			uri = &candidateUri;
			break;
		}
		if (!fileTexture || !uri)
			continue;

		auto* material = FindFbxMaterial(
			graph,
			materialConnection.destinationId,
			materialsByName,
			ambiguousMaterialNames,
			sourcePath);
		if (!material)
			continue;
		const bool hasExplicitNormal = std::any_of(
			material->materialChannels.begin(),
			material->materialChannels.end(),
			[](const ImportedSceneMaterialChannel& channel)
			{
				return channel.name == "normal" && !channel.textureKey.empty();
			});
		if (hasExplicitNormal)
			continue;

		auto texture = textureKeysByUri.find(*uri);
		if (texture == textureKeysByUri.end())
		{
			ImportedSceneNamedRecord recoveredTexture;
			recoveredTexture.sourceKey = "parser/fbx/texture/" + std::to_string(fileTexture->id);
			recoveredTexture.name = FbxObjectDisplayName(fileTexture->name);
			recoveredTexture.uri = *uri;
			const auto textureKey = recoveredTexture.sourceKey;
			scene.textures.push_back(std::move(recoveredTexture));
			texture = textureKeysByUri.emplace(*uri, textureKey).first;
		}

		AddTextureKeyChannel(*material, "bump", texture->second);
		AddUniqueDependency(externalDependencies, *uri);
	}
}

std::string MaterialName(const aiMaterial& material, const uint32_t index)
{
	aiString name;
	if (material.Get(AI_MATKEY_NAME, name) == AI_SUCCESS && name.length > 0u)
		return name.C_Str();
	return "Material " + std::to_string(index);
}

std::string MeshName(const aiMesh& mesh, const uint32_t index)
{
	if (mesh.mName.length > 0u)
		return mesh.mName.C_Str();
	return "Mesh " + std::to_string(index);
}

std::string NodeName(const aiNode& node, const uint32_t index)
{
	if (node.mName.length > 0u)
		return node.mName.C_Str();
	return "Node " + std::to_string(index);
}

bool NearlyEqual(const double left, const double right, const double epsilon = kAssimpUnitScaleEpsilon)
{
	return std::fabs(left - right) <= epsilon;
}

bool IsFiniteVector(const aiVector3D& value)
{
	return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

float LengthSquared(const aiVector3D& value)
{
	return value.x * value.x + value.y * value.y + value.z * value.z;
}

aiVector3D NormalizeDirection(const aiVector3D& value)
{
	if (!IsFiniteVector(value))
		return {};

	const float lengthSquared = LengthSquared(value);
	if (!std::isfinite(lengthSquared) || lengthSquared <= kAssimpDirectionLengthEpsilon)
		return {};

	const float inverseLength = 1.0f / std::sqrt(lengthSquared);
	return {value.x * inverseLength, value.y * inverseLength, value.z * inverseLength};
}

aiVector3D TransformLinearDirection(const aiMatrix4x4& matrix, const aiVector3D& direction)
{
	return {
		matrix.a1 * direction.x + matrix.a2 * direction.y + matrix.a3 * direction.z,
		matrix.b1 * direction.x + matrix.b2 * direction.y + matrix.b3 * direction.z,
		matrix.c1 * direction.x + matrix.c2 * direction.y + matrix.c3 * direction.z
	};
}

aiMatrix4x4 LinearOnlyMatrix(aiMatrix4x4 matrix)
{
	matrix.a4 = 0.0f;
	matrix.b4 = 0.0f;
	matrix.c4 = 0.0f;
	matrix.d1 = 0.0f;
	matrix.d2 = 0.0f;
	matrix.d3 = 0.0f;
	matrix.d4 = 1.0f;
	return matrix;
}

AssimpDirectionTransforms BuildDirectionTransforms(const aiMatrix4x4& matrix)
{
	AssimpDirectionTransforms transforms;
	transforms.linear = LinearOnlyMatrix(matrix);
	transforms.normal = transforms.linear;
	transforms.normal.Inverse();
	transforms.normal.Transpose();
	return transforms;
}

bool ShouldCopyDirectionStreams(const aiMatrix4x4& matrix)
{
	const bool diagonalOnly =
		NearlyEqual(matrix.a2, 0.0) &&
		NearlyEqual(matrix.a3, 0.0) &&
		NearlyEqual(matrix.b1, 0.0) &&
		NearlyEqual(matrix.b3, 0.0) &&
		NearlyEqual(matrix.c1, 0.0) &&
		NearlyEqual(matrix.c2, 0.0);
	if (!diagonalOnly)
		return false;

	const double x = matrix.a1;
	const double y = matrix.b2;
	const double z = matrix.c3;
	return x > 0.0 &&
		NearlyEqual(x, y) &&
		NearlyEqual(x, z);
}

aiVector3D TransformDirection(const AssimpDirectionTransforms& transforms, const aiVector3D& direction)
{
	const auto sourceDirection = NormalizeDirection(direction);
	if (!IsFiniteVector(sourceDirection) || LengthSquared(sourceDirection) <= kAssimpDirectionLengthEpsilon)
		return {};

	const auto transformed = NormalizeDirection(TransformLinearDirection(transforms.linear, sourceDirection));
	if (LengthSquared(transformed) > kAssimpDirectionLengthEpsilon)
		return transformed;

	return sourceDirection;
}

aiVector3D TransformNormalDirection(const AssimpDirectionTransforms& transforms, const aiVector3D& normal)
{
	const auto sourceNormal = NormalizeDirection(normal);
	if (!IsFiniteVector(sourceNormal) || LengthSquared(sourceNormal) <= kAssimpDirectionLengthEpsilon)
		return {};

	auto transformed = NormalizeDirection(TransformLinearDirection(transforms.normal, sourceNormal));
	if (LengthSquared(transformed) > kAssimpDirectionLengthEpsilon)
		return transformed;

	return TransformDirection(transforms, sourceNormal);
}

void AddUniqueDependency(std::vector<std::string>* dependencies, const std::string& path)
{
	if (!dependencies || path.empty())
		return;

	if (std::find(dependencies->begin(), dependencies->end(), path) == dependencies->end())
		dependencies->push_back(path);
}

void AddTextureDependencies(
	const aiMaterial& material,
	const AssimpTextureSlot slot,
	std::vector<std::string>* externalDependencies)
{
	if (!externalDependencies)
		return;

	for (uint32_t textureIndex = 0u; textureIndex < material.GetTextureCount(slot.type); ++textureIndex)
	{
		aiString texturePath;
		if (material.GetTexture(slot.type, textureIndex, &texturePath) == AI_SUCCESS)
			AddUniqueDependency(externalDependencies, AiStringToStdString(texturePath));
	}
}

bool TryGetRawTexturePath(
	const aiMaterial& material,
	const char* propertyName,
	aiString& texturePath)
{
	if (material.Get((std::string("$raw.") + propertyName + "|file").c_str(), aiTextureType_UNKNOWN, 0, texturePath) == AI_SUCCESS)
		return true;
	return material.Get((std::string(propertyName) + "|file").c_str(), aiTextureType_UNKNOWN, 0, texturePath) == AI_SUCCESS;
}

void AddRawTextureDependency(
	const aiMaterial& material,
	const AssimpRawTextureSlot slot,
	std::vector<std::string>* externalDependencies)
{
	if (!externalDependencies)
		return;

	aiString texturePath;
	if (TryGetRawTexturePath(material, slot.propertyName, texturePath))
		AddUniqueDependency(externalDependencies, AiStringToStdString(texturePath));
}

ImportedSceneMaterialChannel* FindChannel(
	ImportedSceneNamedRecord& material,
	const std::string& name)
{
	const auto found = std::find_if(
		material.materialChannels.begin(),
		material.materialChannels.end(),
		[&name](const ImportedSceneMaterialChannel& channel)
		{
			return channel.name == name;
		});
	return found != material.materialChannels.end() ? &*found : nullptr;
}

ImportedSceneMaterialChannel& EnsureChannel(
	ImportedSceneNamedRecord& material,
	std::string name)
{
	if (auto* existing = FindChannel(material, name))
		return *existing;

	material.materialChannels.push_back({std::move(name), {}, {}, false, 0.0});
	return material.materialChannels.back();
}

std::string RegisterTexture(
	ImportedScene& scene,
	std::unordered_map<std::string, std::string>& textureKeysByUri,
	const aiString& texturePath)
{
	const auto uri = AiStringToStdString(texturePath);
	if (uri.empty())
		return {};

	const auto found = textureKeysByUri.find(uri);
	if (found != textureKeysByUri.end())
		return found->second;

	const auto key = IndexedKey("parser/texture", static_cast<uint32_t>(textureKeysByUri.size()));
	textureKeysByUri.emplace(uri, key);

	ImportedSceneNamedRecord texture;
	texture.sourceKey = key;
	texture.name = std::filesystem::path(uri).filename().generic_string();
	texture.uri = uri;
	scene.textures.push_back(std::move(texture));
	return key;
}

bool ChannelBindsTextureKey(
	const ImportedSceneNamedRecord& material,
	const std::string_view channelName,
	const std::string& textureKey)
{
	return !textureKey.empty() && std::any_of(
		material.materialChannels.begin(),
		material.materialChannels.end(),
		[channelName, &textureKey](const ImportedSceneMaterialChannel& channel)
		{
			return channel.name == channelName && channel.textureKey == textureKey;
		});
}

void AddTextureKeyChannel(
	ImportedSceneNamedRecord& material,
	const std::string_view channelName,
	std::string textureKey)
{
	if (textureKey.empty() || ChannelBindsTextureKey(material, channelName, textureKey))
		return;

	auto& channel = EnsureChannel(material, std::string(channelName));
	if (channel.textureKey.empty())
	{
		channel.textureKey = std::move(textureKey);
		return;
	}

	ImportedSceneMaterialChannel extraChannel;
	extraChannel.name = channelName;
	extraChannel.textureKey = std::move(textureKey);
	material.materialChannels.push_back(std::move(extraChannel));
}

void AddTexturePathChannel(
	ImportedScene& scene,
	ImportedSceneNamedRecord& material,
	std::unordered_map<std::string, std::string>& textureKeysByUri,
	std::vector<std::string>* externalDependencies,
	const std::string_view channelName,
	const aiString& texturePath)
{
	const auto uri = AiStringToStdString(texturePath);
	if (uri.empty())
		return;
	AddUniqueDependency(externalDependencies, uri);
	AddTextureKeyChannel(material, channelName, RegisterTexture(scene, textureKeysByUri, texturePath));
}

void AddTextureChannel(
	const aiMaterial& source,
	ImportedScene& scene,
	ImportedSceneNamedRecord& material,
	std::unordered_map<std::string, std::string>& textureKeysByUri,
	std::vector<std::string>* externalDependencies,
	const AssimpTextureSlot slot)
{
	for (uint32_t textureIndex = 0u; textureIndex < source.GetTextureCount(slot.type); ++textureIndex)
	{
		aiString texturePath;
		if (source.GetTexture(slot.type, textureIndex, &texturePath) != AI_SUCCESS)
			continue;

		AddTexturePathChannel(
			scene,
			material,
			textureKeysByUri,
			externalDependencies,
			slot.channelName,
			texturePath);
	}
}

void AddRawTextureChannel(
	const aiMaterial& source,
	ImportedScene& scene,
	ImportedSceneNamedRecord& material,
	std::unordered_map<std::string, std::string>& textureKeysByUri,
	std::vector<std::string>* externalDependencies,
	const AssimpRawTextureSlot slot)
{
	aiString texturePath;
	if (!TryGetRawTexturePath(source, slot.propertyName, texturePath))
		return;

	AddTexturePathChannel(
		scene,
		material,
		textureKeysByUri,
		externalDependencies,
		slot.channelName,
		texturePath);
}

bool AddColorChannel(
	const aiMaterial& source,
	ImportedSceneNamedRecord& material,
	const char* name,
	const char* key,
	const uint32_t type,
	const uint32_t index)
{
	aiColor4D color4;
	if (source.Get(key, type, index, color4) == AI_SUCCESS)
	{
		auto& channel = EnsureChannel(material, name);
		if (channel.values.empty())
			channel.values = {color4.r, color4.g, color4.b, color4.a};
		return true;
	}

	aiColor3D color3;
	if (source.Get(key, type, index, color3) == AI_SUCCESS)
	{
		auto& channel = EnsureChannel(material, name);
		if (channel.values.empty())
			channel.values = {color3.r, color3.g, color3.b};
		return true;
	}

	return false;
}

void AddScalarChannel(
	const aiMaterial& source,
	ImportedSceneNamedRecord& material,
	const char* name,
	const char* key,
	const uint32_t type,
	const uint32_t index)
{
	ai_real value = 0.0f;
	if (source.Get(key, type, index, value) == AI_SUCCESS)
	{
		auto& channel = EnsureChannel(material, name);
		channel.hasScalar = true;
		channel.scalar = static_cast<double>(value);
	}
}

void AddIntScalarChannel(
	const aiMaterial& source,
	ImportedSceneNamedRecord& material,
	const char* name,
	const char* key,
	const uint32_t type,
	const uint32_t index)
{
	int value = 0;
	if (source.Get(key, type, index, value) == AI_SUCCESS)
	{
		auto& channel = EnsureChannel(material, name);
		channel.hasScalar = true;
		channel.scalar = static_cast<double>(value);
	}
}

void ApplyNodeTransform(const aiNode& node, ImportedSceneNode& record)
{
	aiVector3D scaling;
	aiQuaternion rotation;
	aiVector3D position;
	node.mTransformation.Decompose(scaling, rotation, position);

	record.translation = {position.x, position.y, position.z};
	record.rotation = {rotation.x, rotation.y, rotation.z, rotation.w};
	record.scale = {scaling.x, scaling.y, scaling.z};
}

bool IsUniformPositiveUnitScale(const ImportedSceneNode& record)
{
	if (record.scale.size() != 3u)
		return false;

	const double x = record.scale[0];
	const double y = record.scale[1];
	const double z = record.scale[2];
	return x > 0.0 &&
		y > 0.0 &&
		z > 0.0 &&
		NearlyEqual(x, y) &&
		NearlyEqual(x, z) &&
		!NearlyEqual(x, 1.0);
}

bool IsAssimpFbxSyntheticRootUnitScale(
	const aiNode& node,
	const std::string& parentKey,
	const ImportedSceneNode& record,
	const SceneModelSourceFormat sourceFormat)
{
	if (sourceFormat != SceneModelSourceFormat::Fbx)
		return false;

	if (!parentKey.empty() || node.mParent || node.mNumMeshes != 0u)
		return false;

	return record.name == "RootNode" && IsUniformPositiveUnitScale(record);
}

const std::vector<AssimpTextureSlot>& MaterialTextureSlots()
{
	static const std::vector<AssimpTextureSlot> slots = {
		{aiTextureType_BASE_COLOR, "diffuse"},
		{aiTextureType_DIFFUSE, "diffuse"},
		{aiTextureType_NORMALS, "normal"},
		{aiTextureType_NORMAL_CAMERA, "normal"},
		{aiTextureType_HEIGHT, "bump"},
		{aiTextureType_DISPLACEMENT, "bump"},
		{aiTextureType_DIFFUSE_ROUGHNESS, "roughness"},
		{aiTextureType_SHININESS, "shininess"},
		{aiTextureType_METALNESS, "metallic"},
		{aiTextureType_OPACITY, "opacity"},
		{aiTextureType_LIGHTMAP, "occlusion"},
		{aiTextureType_AMBIENT_OCCLUSION, "occlusion"},
		{aiTextureType_EMISSIVE, "emissive"},
		{aiTextureType_EMISSION_COLOR, "emissive"},
		{aiTextureType_SPECULAR, "specular"},
		{aiTextureType_AMBIENT, "ambient"},
		{aiTextureType_REFLECTION, "reflection"}
	};
	return slots;
}

const std::vector<AssimpRawTextureSlot>& RawFbxCompatibilityTextureSlots()
{
	static const std::vector<AssimpRawTextureSlot> slots = {
		{"3dsMax|Parameters|transparency_map", "opacity"},
		{"3dsMax|Parameters|cutout_map", "opacity"},
		{kFbx3dsMaxBumpMapProperty, "bump"},
		{"3dsMax|Parameters|normalCamera", "normal"},
		{"3dsMax|main|norm_map", "normal"},
		{"Maya|normalCamera", "normal"},
		{"Maya|TEX_normal_map", "normal"}
	};
	return slots;
}

void BuildMaterials(
	const aiScene* source,
	const SceneModelSourceFormat sourceFormat,
	ImportedScene& scene,
	std::vector<std::string>& materialNames,
	std::vector<std::string>* externalDependencies)
{
	std::unordered_map<std::string, std::string> textureKeysByUri;
	for (uint32_t index = 0u; index < source->mNumMaterials; ++index)
	{
		const aiMaterial* material = source->mMaterials[index];
		if (!material)
			continue;

		ImportedSceneNamedRecord record;
		record.sourceKey = IndexedKey("parser/material", index);
		record.name = MaterialName(*material, index);
		materialNames.push_back(record.name);

		if (!AddColorChannel(*material, record, "diffuse", AI_MATKEY_COLOR_DIFFUSE))
			AddColorChannel(*material, record, "diffuse", AI_MATKEY_BASE_COLOR);
		AddColorChannel(*material, record, "emissive", AI_MATKEY_COLOR_EMISSIVE);
		AddColorChannel(*material, record, "specular", AI_MATKEY_COLOR_SPECULAR);
		AddScalarChannel(*material, record, "roughness", AI_MATKEY_ROUGHNESS_FACTOR);
		AddScalarChannel(*material, record, "metallic", AI_MATKEY_METALLIC_FACTOR);
		AddScalarChannel(*material, record, "opacity", AI_MATKEY_OPACITY);
		AddScalarChannel(*material, record, "shininess", AI_MATKEY_SHININESS);
		AddIntScalarChannel(*material, record, "doubleSided", AI_MATKEY_TWOSIDED);

		if (sourceFormat == SceneModelSourceFormat::Fbx)
		{
			for (const auto slot : RawFbxCompatibilityTextureSlots())
				AddRawTextureChannel(*material, scene, record, textureKeysByUri, externalDependencies, slot);
		}
		for (auto slot : MaterialTextureSlots())
		{
			if (sourceFormat == SceneModelSourceFormat::Fbx && slot.type == aiTextureType_NORMAL_CAMERA)
			{
				// Assimp merges FBX bump and explicit-normal properties into NORMAL_CAMERA.
				// Skip only candidates already consumed by raw FBX properties. Remaining
				// candidates stay conservative bump inputs for shared identity classification.
				for (uint32_t textureIndex = 0u; textureIndex < material->GetTextureCount(slot.type); ++textureIndex)
				{
					aiString texturePath;
					if (material->GetTexture(slot.type, textureIndex, &texturePath) != AI_SUCCESS)
						continue;
					const auto uri = AiStringToStdString(texturePath);
					if (uri.empty())
						continue;
					AddUniqueDependency(externalDependencies, uri);
					const auto textureKey = RegisterTexture(scene, textureKeysByUri, texturePath);
					if (ChannelBindsTextureKey(record, "normal", textureKey) ||
						ChannelBindsTextureKey(record, "bump", textureKey))
						continue;
					AddTextureKeyChannel(record, "bump", textureKey);
				}
				continue;
			}
			AddTextureChannel(*material, scene, record, textureKeysByUri, externalDependencies, slot);
		}

		scene.materials.push_back(std::move(record));
	}
}

void BuildMeshRecord(const aiScene* source, const uint32_t meshIndex, ImportedScene& scene)
{
	if (meshIndex >= source->mNumMeshes || !source->mMeshes[meshIndex])
		return;

	const auto meshKey = IndexedKey("parser/mesh", meshIndex);
	const auto existing = std::find_if(
		scene.meshes.begin(),
		scene.meshes.end(),
		[&meshKey](const ImportedSceneNamedRecord& mesh)
		{
			return mesh.sourceKey == meshKey;
		});
	if (existing != scene.meshes.end())
		return;

	const aiMesh* mesh = source->mMeshes[meshIndex];
	ImportedSceneNamedRecord record;
	record.sourceKey = meshKey;
	record.name = MeshName(*mesh, meshIndex);
	record.primitiveCount = 1u;

	ImportedScenePrimitive primitive;
	if (mesh->mMaterialIndex < scene.materials.size())
		primitive.materialKey = IndexedKey("parser/material", mesh->mMaterialIndex);
	record.primitives.push_back(std::move(primitive));
	scene.meshes.push_back(std::move(record));
}

void BuildNodeRecords(
	const aiNode* node,
	const aiScene* source,
	const std::string& parentKey,
	ImportedScene& scene,
	uint32_t& nextNodeIndex,
	const SceneModelSourceFormat sourceFormat)
{
	if (!node)
		return;

	const auto nodeIndex = nextNodeIndex++;
	const auto nodeKey = IndexedKey("parser/node", nodeIndex);

	if (node->mNumMeshes <= 1u)
	{
		ImportedSceneNode record;
		record.sourceKey = nodeKey;
		record.name = NodeName(*node, nodeIndex);
		record.parentKey = parentKey;
		ApplyNodeTransform(*node, record);
		if (IsAssimpFbxSyntheticRootUnitScale(*node, parentKey, record, sourceFormat))
			record.scale = {1.0, 1.0, 1.0};
		if (node->mNumMeshes == 1u)
		{
			const auto meshIndex = node->mMeshes[0u];
			BuildMeshRecord(source, meshIndex, scene);
			record.meshKey = IndexedKey("parser/mesh", meshIndex);
		}
		scene.nodes.push_back(std::move(record));
	}
	else
	{
		ImportedSceneNode parentRecord;
		parentRecord.sourceKey = nodeKey;
		parentRecord.name = NodeName(*node, nodeIndex);
		parentRecord.parentKey = parentKey;
		ApplyNodeTransform(*node, parentRecord);
		if (IsAssimpFbxSyntheticRootUnitScale(*node, parentKey, parentRecord, sourceFormat))
			parentRecord.scale = {1.0, 1.0, 1.0};
		scene.nodes.push_back(std::move(parentRecord));

		for (uint32_t meshSlot = 0u; meshSlot < node->mNumMeshes; ++meshSlot)
		{
			const auto meshIndex = node->mMeshes[meshSlot];
			BuildMeshRecord(source, meshIndex, scene);

			ImportedSceneNode record;
			record.sourceKey = nodeKey + "/mesh/" + std::to_string(meshSlot);
			record.name = NodeName(*node, nodeIndex) + " Mesh " + std::to_string(meshSlot);
			record.parentKey = nodeKey;
			record.meshKey = IndexedKey("parser/mesh", meshIndex);
			record.rotation = {0.0, 0.0, 0.0, 1.0};
			record.scale = {1.0, 1.0, 1.0};
			scene.nodes.push_back(std::move(record));
		}
	}

	for (uint32_t childIndex = 0u; childIndex < node->mNumChildren; ++childIndex)
		BuildNodeRecords(node->mChildren[childIndex], source, nodeKey, scene, nextNodeIndex, sourceFormat);
}
}

bool AssimpParser::LoadModel(const std::string & p_fileName, std::vector<Mesh*>& p_meshes, std::vector<std::string>& p_materials, EModelParserFlags p_parserFlags)
{
	p_meshes.clear();
	p_materials.clear();
	m_lastImportedScene = {};
	m_hasImportedSceneData = false;

	Assimp::Importer import;
	ConfigureAssimpImporterForEditorCache(import, p_fileName);
	const aiScene* scene = import.ReadFile(p_fileName, static_cast<unsigned int>(p_parserFlags));

	if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
	{
		NLS_LOG_ERROR("[AssetImport][Assimp] Failed to read " + p_fileName + ": " + import.GetErrorString());
		return false;
	}

	const auto sourceFormat = SourceFormatForPath(p_fileName);
	ProcessMaterials(scene, sourceFormat, p_materials, nullptr);

	std::vector<ParsedMeshData> parsedMeshes;
	aiMatrix4x4 identity;
	ProcessNode(&identity, scene->mRootNode, scene, parsedMeshes);

	for (auto& mesh : parsedMeshes)
		p_meshes.push_back(new Mesh(mesh.vertices, mesh.indices, mesh.materialIndex));

	BuildImportedSceneData(scene, sourceFormat, m_lastImportedScene);
	if (sourceFormat == SceneModelSourceFormat::Fbx)
		RecoverFbxBump2dTextureChains(p_fileName, m_lastImportedScene, nullptr);
	m_hasImportedSceneData = true;

	return true;
}

bool AssimpParser::LoadModelData(
	const std::string& p_fileName,
	std::vector<ParsedMeshData>& p_meshes,
	std::vector<std::string>& p_materials,
	EModelParserFlags p_parserFlags,
	std::vector<std::string>* p_externalDependencies,
	bool p_bakeNodeTransforms)
{
	p_meshes.clear();
	p_materials.clear();
	m_lastImportedScene = {};
	m_hasImportedSceneData = false;
	if (p_externalDependencies)
		p_externalDependencies->clear();

	Assimp::Importer import;
	ConfigureAssimpImporterForEditorCache(import, p_fileName);
	const auto readStart = std::chrono::steady_clock::now();
	const aiScene* scene = import.ReadFile(p_fileName, static_cast<unsigned int>(p_parserFlags));
	const auto readElapsed = MillisecondsSince(readStart);

	if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
	{
		NLS_LOG_ERROR("[AssetImport][Assimp] Failed to read " + p_fileName + ": " + import.GetErrorString());
		return false;
	}
	LogAssimpImportTiming(p_fileName, "ReadFile", readElapsed, scene->mNumMeshes, scene->mNumMaterials);

	const auto materialsStart = std::chrono::steady_clock::now();
	const auto sourceFormat = SourceFormatForPath(p_fileName);
	ProcessMaterials(scene, sourceFormat, p_materials, p_externalDependencies);
	LogAssimpImportTiming(
		p_fileName,
		"ProcessMaterials",
		MillisecondsSince(materialsStart),
		scene->mNumMeshes,
		scene->mNumMaterials);

	const auto meshesStart = std::chrono::steady_clock::now();
	if (p_bakeNodeTransforms)
	{
		aiMatrix4x4 identity;
		ProcessNode(&identity, scene->mRootNode, scene, p_meshes);
	}
	else
	{
		ProcessSourceMeshes(scene, p_meshes);
	}
	LogAssimpImportTiming(
		p_fileName,
		"ProcessMeshes",
		MillisecondsSince(meshesStart),
		scene->mNumMeshes,
		scene->mNumMaterials);

	const auto sceneDataStart = std::chrono::steady_clock::now();
	BuildImportedSceneData(scene, sourceFormat, m_lastImportedScene);
	if (sourceFormat == SceneModelSourceFormat::Fbx)
		RecoverFbxBump2dTextureChains(p_fileName, m_lastImportedScene, p_externalDependencies);
	m_hasImportedSceneData = true;
	LogAssimpImportTiming(
		p_fileName,
		"BuildImportedSceneData",
		MillisecondsSince(sceneDataStart),
		scene->mNumMeshes,
		scene->mNumMaterials);
	LogAssimpImportTiming(
		p_fileName,
		"LoadModelDataTotal",
		MillisecondsSince(readStart),
		scene->mNumMeshes,
		scene->mNumMaterials);

	return true;
}

void AssimpParser::ProcessMaterials(
	const aiScene * p_scene,
	const SceneModelSourceFormat p_sourceFormat,
	std::vector<std::string>& p_materials,
	std::vector<std::string>* p_externalDependencies)
{
	for (uint32_t i = 0; i < p_scene->mNumMaterials; ++i)
	{
		aiMaterial* material = p_scene->mMaterials[i];
		if (material)
		{
			aiString name;
			aiGetMaterialString(material, AI_MATKEY_NAME, &name);
			p_materials.push_back(name.C_Str());

			if (p_externalDependencies)
			{
				for (const auto slot : MaterialTextureSlots())
					AddTextureDependencies(*material, slot, p_externalDependencies);
				if (p_sourceFormat == SceneModelSourceFormat::Fbx)
				{
					for (const auto slot : RawFbxCompatibilityTextureSlots())
						AddRawTextureDependency(*material, slot, p_externalDependencies);
				}
			}
		}
	}
}

bool AssimpParser::PopulateImportedSceneData(
	const std::filesystem::path&,
	NLS::Render::Assets::SceneModelSourceFormat p_sourceFormat,
	NLS::Render::Assets::ImportedScene& p_scene)
{
	if (!m_hasImportedSceneData)
		return false;

	auto scene = m_lastImportedScene;
	scene.sourceAssetId = p_scene.sourceAssetId;
	scene.sceneKey = p_scene.sceneKey;
	scene.importSettings = p_scene.importSettings;
	p_scene = std::move(scene);

	if (p_sourceFormat == NLS::Render::Assets::SceneModelSourceFormat::Obj)
	{
		p_scene.diagnostics.push_back({
			"obj-no-skeleton-animation-support",
			"OBJ does not define native skeleton, skin, animation, or morph data."
		});
	}
	return true;
}

#if defined(NLS_ENABLE_TEST_HOOKS) && NLS_ENABLE_TEST_HOOKS
bool AssimpParser::CanReadFbxMaterialGraphForTesting(const std::filesystem::path& p_sourcePath) const
{
	FbxMaterialGraph graph;
	return ReadFbxMaterialGraph(p_sourcePath, graph);
}
#endif

void AssimpParser::BuildImportedSceneData(
	const aiScene* p_scene,
	NLS::Render::Assets::SceneModelSourceFormat p_sourceFormat,
	NLS::Render::Assets::ImportedScene& p_outScene)
{
	std::vector<std::string> unusedMaterialNames;
	BuildMaterials(p_scene, p_sourceFormat, p_outScene, unusedMaterialNames, nullptr);

	uint32_t nextNodeIndex = 0u;
	BuildNodeRecords(p_scene->mRootNode, p_scene, {}, p_outScene, nextNodeIndex, p_sourceFormat);
}

void AssimpParser::ProcessSourceMeshes(const aiScene* p_scene, std::vector<ParsedMeshData>& p_meshes)
{
	if (!p_scene)
		return;

	aiMatrix4x4 identity;
	for (uint32_t meshIndex = 0u; meshIndex < p_scene->mNumMeshes; ++meshIndex)
	{
		aiMesh* mesh = p_scene->mMeshes[meshIndex];
		if (!mesh)
			continue;

		ParsedMeshData parsedMesh;
		parsedMesh.materialIndex = mesh->mMaterialIndex;
		parsedMesh.sourceMeshIndex = meshIndex;
		parsedMesh.sourceKey = IndexedKey("parser/mesh", meshIndex);
		ProcessMesh(&identity, mesh, p_scene, parsedMesh.vertices, parsedMesh.indices);
		p_meshes.push_back(std::move(parsedMesh));
	}
}

void AssimpParser::ProcessNode(void* p_transform, aiNode * p_node, const aiScene * p_scene, std::vector<ParsedMeshData>& p_meshes)
{
	aiMatrix4x4 nodeTransformation = *reinterpret_cast<aiMatrix4x4*>(p_transform) * p_node->mTransformation;

	// Process all the node's meshes (if any)
	for (uint32_t i = 0; i < p_node->mNumMeshes; ++i)
	{
		const auto meshIndex = p_node->mMeshes[i];
		aiMesh* mesh = p_scene->mMeshes[meshIndex];
		ParsedMeshData parsedMesh;
		parsedMesh.materialIndex = mesh->mMaterialIndex;
		parsedMesh.sourceMeshIndex = meshIndex;
		parsedMesh.sourceKey = IndexedKey("parser/mesh", meshIndex);
		ProcessMesh(&nodeTransformation, mesh, p_scene, parsedMesh.vertices, parsedMesh.indices);
		p_meshes.push_back(std::move(parsedMesh));
	}

	// Then do the same for each of its children
	for (uint32_t i = 0; i < p_node->mNumChildren; ++i)
	{
		ProcessNode(&nodeTransformation, p_node->mChildren[i], p_scene, p_meshes);
	}
}

void AssimpParser::ProcessMesh(void* p_transform, aiMesh* p_mesh, const aiScene* p_scene, std::vector<Geometry::Vertex>& p_outVertices, std::vector<uint32_t>& p_outIndices)
{
	aiMatrix4x4 meshTransformation = *reinterpret_cast<aiMatrix4x4*>(p_transform);
	const bool copyDirectionStreams = ShouldCopyDirectionStreams(meshTransformation);
	AssimpDirectionTransforms directionTransforms{};
	if (!copyDirectionStreams)
		directionTransforms = BuildDirectionTransforms(meshTransformation);
	p_outVertices.reserve(p_outVertices.size() + p_mesh->mNumVertices);
	p_outIndices.reserve(p_outIndices.size() + static_cast<size_t>(p_mesh->mNumFaces) * 3u);

	for (uint32_t i = 0; i < p_mesh->mNumVertices; ++i)
	{
		aiVector3D position		= meshTransformation * p_mesh->mVertices[i];
		aiVector3D normal		= p_mesh->mNormals ? (copyDirectionStreams ? p_mesh->mNormals[i] : TransformNormalDirection(directionTransforms, p_mesh->mNormals[i])) : aiVector3D(0.0f, 0.0f, 0.0f);
		aiVector3D texCoords	= p_mesh->mTextureCoords[0] ? p_mesh->mTextureCoords[0][i] : aiVector3D(0.0f, 0.0f, 0.0f);
		aiVector3D tangent		= p_mesh->mTangents ? (copyDirectionStreams ? p_mesh->mTangents[i] : TransformDirection(directionTransforms, p_mesh->mTangents[i])) : aiVector3D(0.0f, 0.0f, 0.0f);
		aiVector3D bitangent	= p_mesh->mBitangents ? (copyDirectionStreams ? p_mesh->mBitangents[i] : TransformDirection(directionTransforms, p_mesh->mBitangents[i])) : aiVector3D(0.0f, 0.0f, 0.0f);

		p_outVertices.push_back
		(
			{
				position.x,
				position.y,
				position.z,
				texCoords.x,
				texCoords.y,
				normal.x,
				normal.y,
				normal.z,
				tangent.x,
				tangent.y,
				tangent.z,
				bitangent.x,
				bitangent.y,
				bitangent.z
			}
		);
	}

	for (uint32_t faceID = 0; faceID < p_mesh->mNumFaces; ++faceID)
	{
		auto& face = p_mesh->mFaces[faceID];

		for (size_t indexID = 0; indexID < 3; ++indexID)
			p_outIndices.push_back(face.mIndices[indexID]);
	}
}
}
