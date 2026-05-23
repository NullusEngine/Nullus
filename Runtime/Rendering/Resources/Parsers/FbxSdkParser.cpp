#include "Rendering/Resources/Parsers/FbxSdkParser.h"

#ifndef NLS_HAS_AUTODESK_FBX_SDK
#define NLS_HAS_AUTODESK_FBX_SDK 0
#endif

#if NLS_HAS_AUTODESK_FBX_SDK
#include <fbxsdk.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "Debug/Logger.h"
#include "Profiling/Profiler.h"
#include "Rendering/Resources/Mesh.h"

namespace NLS::Render::Resources::Parsers
{
#if NLS_HAS_AUTODESK_FBX_SDK
namespace
{
using NLS::Render::Assets::ImportedScene;
using NLS::Render::Assets::ImportedSceneMaterialChannel;
using NLS::Render::Assets::ImportedSceneNamedRecord;
using NLS::Render::Assets::ImportedSceneNode;
using NLS::Render::Assets::ImportedScenePrimitive;
using NLS::Render::Assets::SceneModelSourceFormat;

constexpr int64_t kFbxSdkImportTimingLogThresholdMilliseconds = 100;
constexpr double kGeometryEpsilon = 1e-8;
constexpr uint32_t kInvalidMaterialIndex = std::numeric_limits<uint32_t>::max();

struct FbxSdkObjectDeleter
{
	template<typename T>
	void operator()(T* object) const
	{
		if (object)
			object->Destroy();
	}
};

template<typename T>
using FbxPtr = std::unique_ptr<T, FbxSdkObjectDeleter>;

struct MeshImportRecord
{
	uint32_t meshIndex = std::numeric_limits<uint32_t>::max();
	std::string meshKey;
	std::vector<uint32_t> materialIndices;
	size_t firstPrimitiveIndex = 0u;
	size_t primitiveCount = 0u;
};

struct FbxMaterialRegistry
{
	std::unordered_map<const FbxSurfaceMaterial*, uint32_t> indexByMaterial;
	std::unordered_map<std::string, std::string> textureKeysByUri;
};

struct ProjectedPolygonVertex
{
	double x = 0.0;
	double y = 0.0;
	int polygonVertexIndex = 0;
};

std::string IndexedKey(const char* prefix, const uint32_t index)
{
	return std::string(prefix) + "/" + std::to_string(index);
}

int64_t MillisecondsSince(const std::chrono::steady_clock::time_point start)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - start).count();
}

void LogFbxSdkImportTiming(
	const std::string& sourcePath,
	const char* stage,
	const int64_t elapsedMilliseconds,
	const uint32_t meshCount,
	const uint32_t materialCount)
{
	if (elapsedMilliseconds < kFbxSdkImportTimingLogThresholdMilliseconds)
		return;

	NLS_LOG_INFO(
		"[AssetImport][FbxSdk] " +
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

std::string FbxName(const char* name, const char* fallbackPrefix, const uint32_t index)
{
	if (name && name[0] != '\0')
		return name;
	return std::string(fallbackPrefix) + " " + std::to_string(index);
}

std::string FbxStringValue(const FbxString& value)
{
	return value.IsEmpty() ? std::string{} : std::string(value.Buffer());
}

bool HasFlag(const EModelParserFlags flags, const EModelParserFlags flag)
{
	return (flags & flag) == flag;
}

FbxAMatrix IdentityMatrix()
{
	FbxAMatrix matrix;
	matrix.SetIdentity();
	return matrix;
}

double ResolveGlobalScale(FbxScene* scene, const EModelParserFlags parserFlags)
{
	if (!scene || !HasFlag(parserFlags, EModelParserFlags::GLOBAL_SCALE))
		return 1.0;

	const double unitScaleFactor = scene->GetGlobalSettings().GetSystemUnit().GetScaleFactor();
	return unitScaleFactor != 0.0 ? unitScaleFactor * 0.01 : 1.0;
}

void ApplyGlobalScale(FbxVector4& position, const double globalScale)
{
	if (globalScale == 1.0)
		return;

	position[0] *= globalScale;
	position[1] *= globalScale;
	position[2] *= globalScale;
}

Geometry::Vertex MakeVertex(
	const FbxVector4& position,
	const FbxVector2& uv,
	const FbxVector4& normal,
	const FbxVector4& tangent,
	const FbxVector4& bitangent)
{
	Geometry::Vertex vertex {};
	vertex.position[0] = static_cast<float>(position[0]);
	vertex.position[1] = static_cast<float>(position[1]);
	vertex.position[2] = static_cast<float>(position[2]);
	vertex.texCoords[0] = static_cast<float>(uv[0]);
	vertex.texCoords[1] = static_cast<float>(uv[1]);
	vertex.normals[0] = static_cast<float>(normal[0]);
	vertex.normals[1] = static_cast<float>(normal[1]);
	vertex.normals[2] = static_cast<float>(normal[2]);
	vertex.tangent[0] = static_cast<float>(tangent[0]);
	vertex.tangent[1] = static_cast<float>(tangent[1]);
	vertex.tangent[2] = static_cast<float>(tangent[2]);
	vertex.bitangent[0] = static_cast<float>(bitangent[0]);
	vertex.bitangent[1] = static_cast<float>(bitangent[1]);
	vertex.bitangent[2] = static_cast<float>(bitangent[2]);
	return vertex;
}

FbxVector4 TransformDirection(const FbxAMatrix& transform, const FbxVector4& direction)
{
	if (std::fabs(direction[0]) <= kGeometryEpsilon &&
		std::fabs(direction[1]) <= kGeometryEpsilon &&
		std::fabs(direction[2]) <= kGeometryEpsilon)
	{
		return FbxVector4(0.0, 0.0, 0.0, direction[3]);
	}

	FbxVector4 result = transform.MultT(direction);
	result.Normalize();
	return result;
}

FbxVector4 CrossDirection(const FbxVector4& a, const FbxVector4& b)
{
	FbxVector4 result(
		a[1] * b[2] - a[2] * b[1],
		a[2] * b[0] - a[0] * b[2],
		a[0] * b[1] - a[1] * b[0],
		0.0);
	if (std::fabs(result[0]) > kGeometryEpsilon ||
		std::fabs(result[1]) > kGeometryEpsilon ||
		std::fabs(result[2]) > kGeometryEpsilon)
	{
		result.Normalize();
	}
	return result;
}

void AddUniqueDependency(std::vector<std::string>* dependencies, const std::string& path)
{
	if (!dependencies || path.empty())
		return;

	if (std::find(dependencies->begin(), dependencies->end(), path) == dependencies->end())
		dependencies->push_back(path);
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
	const std::string& uri)
{
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

void AddColorChannel(
	ImportedSceneNamedRecord& material,
	const char* channelName,
	const FbxDouble3& value)
{
	auto& channel = EnsureChannel(material, channelName);
	if (channel.values.empty())
		channel.values = {value[0], value[1], value[2]};
}

void AddScalarChannel(
	ImportedSceneNamedRecord& material,
	const char* channelName,
	const double value)
{
	auto& channel = EnsureChannel(material, channelName);
	channel.hasScalar = true;
	channel.scalar = value;
}

void AddTextureChannel(
	FbxProperty property,
	ImportedScene& scene,
	ImportedSceneNamedRecord& material,
	std::unordered_map<std::string, std::string>& textureKeysByUri,
	std::vector<std::string>* externalDependencies,
	const char* channelName)
{
	if (!property.IsValid())
		return;

	const int textureCount = property.GetSrcObjectCount<FbxFileTexture>();
	for (int textureIndex = 0; textureIndex < textureCount; ++textureIndex)
	{
		const FbxFileTexture* texture = property.GetSrcObject<FbxFileTexture>(textureIndex);
		if (!texture)
			continue;

		std::string uri = texture->GetRelativeFileName();
		if (uri.empty())
			uri = texture->GetFileName();
		AddUniqueDependency(externalDependencies, uri);

		auto& channel = EnsureChannel(material, channelName);
		if (channel.textureKey.empty())
		{
			channel.textureKey = RegisterTexture(scene, textureKeysByUri, uri);
		}
		else
		{
			ImportedSceneMaterialChannel extraChannel;
			extraChannel.name = channelName;
			extraChannel.textureKey = RegisterTexture(scene, textureKeysByUri, uri);
			material.materialChannels.push_back(std::move(extraChannel));
		}
	}
}

void AddSurfaceMaterialChannels(
	FbxSurfaceMaterial* source,
	ImportedScene& scene,
	ImportedSceneNamedRecord& material,
	std::unordered_map<std::string, std::string>& textureKeysByUri,
	std::vector<std::string>* externalDependencies)
{
	if (!source)
		return;

	if (const auto* lambert = FbxCast<FbxSurfaceLambert>(source))
	{
		AddColorChannel(material, "diffuse", lambert->Diffuse.Get());
		AddColorChannel(material, "emissive", lambert->Emissive.Get());
		AddScalarChannel(material, "opacity", 1.0 - lambert->TransparencyFactor.Get());
		AddTextureChannel(lambert->Diffuse, scene, material, textureKeysByUri, externalDependencies, "diffuse");
		AddTextureChannel(lambert->NormalMap, scene, material, textureKeysByUri, externalDependencies, "normal");
		AddTextureChannel(lambert->Bump, scene, material, textureKeysByUri, externalDependencies, "bump");
		AddTextureChannel(lambert->Emissive, scene, material, textureKeysByUri, externalDependencies, "emissive");
		AddTextureChannel(lambert->TransparentColor, scene, material, textureKeysByUri, externalDependencies, "opacity");
	}

	if (const auto* phong = FbxCast<FbxSurfacePhong>(source))
	{
		AddColorChannel(material, "specular", phong->Specular.Get());
		AddScalarChannel(material, "shininess", phong->Shininess.Get());
		AddTextureChannel(phong->Specular, scene, material, textureKeysByUri, externalDependencies, "specular");
	}
}

void BuildMaterials(
	FbxScene* source,
	ImportedScene& scene,
	std::vector<std::string>& materialNames,
	std::vector<std::string>* externalDependencies,
	FbxMaterialRegistry& registry)
{
	const int materialCount = source ? source->GetMaterialCount() : 0;
	for (int index = 0; index < materialCount; ++index)
	{
		FbxSurfaceMaterial* material = source->GetMaterial(index);
		if (!material)
			continue;

		ImportedSceneNamedRecord record;
		record.sourceKey = IndexedKey("parser/material", static_cast<uint32_t>(index));
		record.name = FbxName(material->GetName(), "Material", static_cast<uint32_t>(index));
		materialNames.push_back(record.name);
		registry.indexByMaterial.emplace(material, static_cast<uint32_t>(index));
		AddSurfaceMaterialChannels(material, scene, record, registry.textureKeysByUri, externalDependencies);
		scene.materials.push_back(std::move(record));
	}
}

uint32_t ResolveNodeMaterialIndex(
	FbxNode* node,
	const uint32_t nodeMaterialSlot,
	const FbxMaterialRegistry& registry)
{
	if (!node || nodeMaterialSlot >= static_cast<uint32_t>(node->GetMaterialCount()))
		return kInvalidMaterialIndex;

	const auto* material = node->GetMaterial(static_cast<int>(nodeMaterialSlot));
	if (!material)
		return kInvalidMaterialIndex;

	const auto found = registry.indexByMaterial.find(material);
	return found != registry.indexByMaterial.end() ? found->second : kInvalidMaterialIndex;
}

std::vector<uint32_t> ResolveNodeMaterialIndices(
	FbxNode* node,
	const FbxMaterialRegistry& registry)
{
	std::vector<uint32_t> materialIndices;
	if (!node)
		return materialIndices;

	const auto materialCount = node->GetMaterialCount();
	materialIndices.reserve(static_cast<size_t>(std::max(0, materialCount)));
	for (int index = 0; index < materialCount; ++index)
		materialIndices.push_back(ResolveNodeMaterialIndex(node, static_cast<uint32_t>(index), registry));
	return materialIndices;
}

uint32_t ResolvePolygonMaterialIndex(FbxMesh* mesh, const int polygonIndex)
{
	if (!mesh)
		return 0u;

	FbxLayerElementMaterial* materials = mesh->GetElementMaterial();
	if (!materials)
		return 0u;

	switch (materials->GetMappingMode())
	{
	case FbxLayerElement::eByPolygon:
		if (polygonIndex >= 0 && polygonIndex < materials->GetIndexArray().GetCount())
			return static_cast<uint32_t>(std::max(0, materials->GetIndexArray().GetAt(polygonIndex)));
		break;
	case FbxLayerElement::eAllSame:
		if (materials->GetIndexArray().GetCount() > 0)
			return static_cast<uint32_t>(std::max(0, materials->GetIndexArray().GetAt(0)));
		break;
	default:
		break;
	}
	return 0u;
}

FbxVector4 ReadNormal(FbxMesh* mesh, const int controlPointIndex, const int vertexCounter)
{
	FbxVector4 normal(0.0, 0.0, 0.0, 0.0);
	if (!mesh)
		return normal;

	mesh->GetPolygonVertexNormal(controlPointIndex, vertexCounter, normal);
	normal.Normalize();
	return normal;
}

template<typename TElement>
int DirectElementIndexForPolygonVertex(
	const FbxLayerElement::EMappingMode mappingMode,
	const FbxLayerElement::EReferenceMode referenceMode,
	const TElement* element,
	FbxMesh* mesh,
	const int polygonIndex,
	const int polygonVertexIndex,
	const int controlPointIndex)
{
	if (!element || !mesh)
		return -1;

	int mappedIndex = -1;
	switch (mappingMode)
	{
	case FbxLayerElement::eByControlPoint:
		mappedIndex = controlPointIndex;
		break;
	case FbxLayerElement::eByPolygonVertex:
		mappedIndex = mesh->GetPolygonVertexIndex(polygonIndex) + polygonVertexIndex;
		break;
	case FbxLayerElement::eByPolygon:
		mappedIndex = polygonIndex;
		break;
	case FbxLayerElement::eAllSame:
		mappedIndex = 0;
		break;
	default:
		break;
	}

	if (mappedIndex < 0)
	{
		const int polygonVertexGlobalIndex = mesh->GetPolygonVertexIndex(polygonIndex) + polygonVertexIndex;
		const int directCount = element->GetDirectArray().GetCount();
		const int indexCount = element->GetIndexArray().GetCount();

		if ((referenceMode == FbxLayerElement::eIndex || referenceMode == FbxLayerElement::eIndexToDirect) &&
			polygonVertexGlobalIndex >= 0 &&
			polygonVertexGlobalIndex < indexCount)
		{
			return element->GetIndexArray().GetAt(polygonVertexGlobalIndex);
		}
		if (polygonVertexGlobalIndex >= 0 && polygonVertexGlobalIndex < directCount)
			return polygonVertexGlobalIndex;
		if (controlPointIndex >= 0 && controlPointIndex < directCount)
			return controlPointIndex;
		if (polygonIndex >= 0 && polygonIndex < directCount)
			return polygonIndex;
		return directCount == 1 ? 0 : -1;
	}

	switch (referenceMode)
	{
	case FbxLayerElement::eDirect:
		if (mappedIndex < element->GetDirectArray().GetCount())
			return mappedIndex;
		if (element->GetDirectArray().GetCount() == 1)
			return 0;
		break;
	case FbxLayerElement::eIndex:
	case FbxLayerElement::eIndexToDirect:
		if (mappedIndex < element->GetIndexArray().GetCount())
			return element->GetIndexArray().GetAt(mappedIndex);
		if (mappedIndex < element->GetDirectArray().GetCount())
			return mappedIndex;
		if (element->GetDirectArray().GetCount() == 1)
			return 0;
		break;
	default:
		break;
	}
	return -1;
}

template<typename TElement>
FbxVector4 ReadLayerVector4(
	const TElement* element,
	FbxMesh* mesh,
	const int polygonIndex,
	const int polygonVertexIndex,
	const int controlPointIndex)
{
	FbxVector4 value(0.0, 0.0, 0.0, 0.0);
	if (!element)
		return value;

	const int directIndex = DirectElementIndexForPolygonVertex(
		element->GetMappingMode(),
		element->GetReferenceMode(),
		element,
		mesh,
		polygonIndex,
		polygonVertexIndex,
		controlPointIndex);
	if (directIndex >= 0 && directIndex < element->GetDirectArray().GetCount())
		value = element->GetDirectArray().GetAt(directIndex);
	return value;
}

FbxVector4 ReadTangent(
	FbxMesh* mesh,
	const int polygonIndex,
	const int polygonVertexIndex,
	const int controlPointIndex)
{
	if (!mesh || mesh->GetElementTangentCount() <= 0)
		return FbxVector4(0.0, 0.0, 0.0, 0.0);

	for (int elementIndex = 0; elementIndex < mesh->GetElementTangentCount(); ++elementIndex)
	{
		auto tangent = ReadLayerVector4(
			mesh->GetElementTangent(elementIndex),
			mesh,
			polygonIndex,
			polygonVertexIndex,
			controlPointIndex);
		if (std::fabs(tangent[0]) <= kGeometryEpsilon &&
			std::fabs(tangent[1]) <= kGeometryEpsilon &&
			std::fabs(tangent[2]) <= kGeometryEpsilon)
		{
			continue;
		}

		tangent.Normalize();
		return tangent;
	}
	return FbxVector4(0.0, 0.0, 0.0, 0.0);
}

FbxVector4 ReadBitangent(
	FbxMesh* mesh,
	const int polygonIndex,
	const int polygonVertexIndex,
	const int controlPointIndex)
{
	if (!mesh || mesh->GetElementBinormalCount() <= 0)
		return FbxVector4(0.0, 0.0, 0.0, 0.0);

	for (int elementIndex = 0; elementIndex < mesh->GetElementBinormalCount(); ++elementIndex)
	{
		auto bitangent = ReadLayerVector4(
			mesh->GetElementBinormal(elementIndex),
			mesh,
			polygonIndex,
			polygonVertexIndex,
			controlPointIndex);
		if (std::fabs(bitangent[0]) <= kGeometryEpsilon &&
			std::fabs(bitangent[1]) <= kGeometryEpsilon &&
			std::fabs(bitangent[2]) <= kGeometryEpsilon)
		{
			continue;
		}

		bitangent.Normalize();
		return bitangent;
	}
	return FbxVector4(0.0, 0.0, 0.0, 0.0);
}

FbxVector2 ReadUv(FbxMesh* mesh, const int polygonIndex, const int polygonVertexIndex, const int controlPointIndex)
{
	FbxVector2 uv(0.0, 0.0);
	if (!mesh || mesh->GetElementUVCount() <= 0)
		return uv;

	FbxStringList uvSetNames;
	mesh->GetUVSetNames(uvSetNames);
	if (uvSetNames.GetCount() <= 0)
		return uv;

	bool unmapped = false;
	mesh->GetPolygonVertexUV(
		polygonIndex,
		polygonVertexIndex,
		uvSetNames[0],
		uv,
		unmapped);
	(void)controlPointIndex;
	return unmapped ? FbxVector2(0.0, 0.0) : uv;
}

void AddUniqueAttribute(std::vector<std::string>& attributes, std::string semantic)
{
	if (semantic.empty())
		return;

	if (std::find(attributes.begin(), attributes.end(), semantic) == attributes.end())
		attributes.push_back(std::move(semantic));
}

std::vector<std::string> BuildMeshAttributes(FbxMesh* mesh)
{
	std::vector<std::string> attributes;
	if (!mesh)
		return attributes;

	AddUniqueAttribute(attributes, "POSITION");
	if (mesh->GetElementNormalCount() > 0)
		AddUniqueAttribute(attributes, "NORMAL");
	if (mesh->GetElementTangentCount() > 0)
		AddUniqueAttribute(attributes, "TANGENT");
	if (mesh->GetElementBinormalCount() > 0)
		AddUniqueAttribute(attributes, "BINORMAL");

	FbxStringList uvSetNames;
	mesh->GetUVSetNames(uvSetNames);
	for (int uvIndex = 0; uvIndex < uvSetNames.GetCount(); ++uvIndex)
		AddUniqueAttribute(attributes, "TEXCOORD_" + std::to_string(uvIndex));

	return attributes;
}

template<typename TElement>
bool HasNonZeroVector4Element(const TElement* element)
{
	if (!element)
		return false;

	for (int index = 0; index < element->GetDirectArray().GetCount(); ++index)
	{
		const auto value = element->GetDirectArray().GetAt(index);
		if (std::fabs(value[0]) > kGeometryEpsilon ||
			std::fabs(value[1]) > kGeometryEpsilon ||
			std::fabs(value[2]) > kGeometryEpsilon)
		{
			return true;
		}
	}
	return false;
}

bool HasUsableTangents(FbxMesh* mesh)
{
	return mesh &&
		mesh->GetElementTangentCount() > 0 &&
		HasNonZeroVector4Element(mesh->GetElementTangent(0));
}

void PrepareMeshGeometry(FbxMesh* mesh)
{
	if (!mesh)
		return;

	if (mesh->GetElementNormalCount() <= 0)
		mesh->GenerateNormals(false, false, false);

	if (HasUsableTangents(mesh) || mesh->GetElementUVCount() <= 0)
		return;

	FbxStringList uvSetNames;
	mesh->GetUVSetNames(uvSetNames);
	if (uvSetNames.GetCount() > 0)
		mesh->GenerateTangentsData(uvSetNames[0], true, false);
}

FbxVector4 PolygonNormal(FbxMesh* mesh, const FbxVector4* controlPoints, const int polygonIndex)
{
	FbxVector4 normal(0.0, 0.0, 0.0, 0.0);
	if (!mesh || !controlPoints)
		return normal;

	const int polygonSize = mesh->GetPolygonSize(polygonIndex);
	for (int index = 0; index < polygonSize; ++index)
	{
		const int currentIndex = mesh->GetPolygonVertex(polygonIndex, index);
		const int nextIndex = mesh->GetPolygonVertex(polygonIndex, (index + 1) % polygonSize);
		if (currentIndex < 0 || nextIndex < 0)
			continue;

		const auto& current = controlPoints[currentIndex];
		const auto& next = controlPoints[nextIndex];
		normal[0] += (current[1] - next[1]) * (current[2] + next[2]);
		normal[1] += (current[2] - next[2]) * (current[0] + next[0]);
		normal[2] += (current[0] - next[0]) * (current[1] + next[1]);
	}
	normal.Normalize();
	return normal;
}

std::vector<ProjectedPolygonVertex> ProjectPolygon(
	FbxMesh* mesh,
	const FbxVector4* controlPoints,
	const int polygonIndex)
{
	std::vector<ProjectedPolygonVertex> projected;
	if (!mesh || !controlPoints)
		return projected;

	const auto normal = PolygonNormal(mesh, controlPoints, polygonIndex);
	const auto absX = std::fabs(normal[0]);
	const auto absY = std::fabs(normal[1]);
	const auto absZ = std::fabs(normal[2]);

	enum class DropAxis
	{
		X,
		Y,
		Z
	};
	const auto dropAxis = absX > absY && absX > absZ
		? DropAxis::X
		: absY > absZ
			? DropAxis::Y
			: DropAxis::Z;

	const int polygonSize = mesh->GetPolygonSize(polygonIndex);
	projected.reserve(static_cast<size_t>(polygonSize));
	for (int polygonVertexIndex = 0; polygonVertexIndex < polygonSize; ++polygonVertexIndex)
	{
		const int controlPointIndex = mesh->GetPolygonVertex(polygonIndex, polygonVertexIndex);
		if (controlPointIndex < 0)
			continue;

		const auto& position = controlPoints[controlPointIndex];
		ProjectedPolygonVertex vertex;
		vertex.polygonVertexIndex = polygonVertexIndex;
		switch (dropAxis)
		{
		case DropAxis::X:
			vertex.x = position[1];
			vertex.y = position[2];
			break;
		case DropAxis::Y:
			vertex.x = position[0];
			vertex.y = position[2];
			break;
		case DropAxis::Z:
			vertex.x = position[0];
			vertex.y = position[1];
			break;
		}
		projected.push_back(vertex);
	}
	return projected;
}

double SignedArea(const std::vector<ProjectedPolygonVertex>& polygon, const std::vector<size_t>* indices = nullptr)
{
	double area = 0.0;
	const size_t count = indices ? indices->size() : polygon.size();
	for (size_t index = 0u; index < count; ++index)
	{
		const size_t currentIndex = indices ? (*indices)[index] : index;
		const size_t nextIndex = indices ? (*indices)[(index + 1u) % count] : (index + 1u) % count;
		const auto& current = polygon[currentIndex];
		const auto& next = polygon[nextIndex];
		area += current.x * next.y - next.x * current.y;
	}
	return area * 0.5;
}

double Cross2D(
	const ProjectedPolygonVertex& a,
	const ProjectedPolygonVertex& b,
	const ProjectedPolygonVertex& c)
{
	return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool PointInTriangle2D(
	const ProjectedPolygonVertex& point,
	const ProjectedPolygonVertex& a,
	const ProjectedPolygonVertex& b,
	const ProjectedPolygonVertex& c)
{
	const double ab = Cross2D(a, b, point);
	const double bc = Cross2D(b, c, point);
	const double ca = Cross2D(c, a, point);
	const bool hasNegative = ab < -kGeometryEpsilon || bc < -kGeometryEpsilon || ca < -kGeometryEpsilon;
	const bool hasPositive = ab > kGeometryEpsilon || bc > kGeometryEpsilon || ca > kGeometryEpsilon;
	return !(hasNegative && hasPositive);
}

bool IsEar(
	const std::vector<ProjectedPolygonVertex>& polygon,
	const std::vector<size_t>& remaining,
	const size_t remainingIndex,
	const double orientation)
{
	const size_t previous = remaining[(remainingIndex + remaining.size() - 1u) % remaining.size()];
	const size_t current = remaining[remainingIndex];
	const size_t next = remaining[(remainingIndex + 1u) % remaining.size()];

	const auto& a = polygon[previous];
	const auto& b = polygon[current];
	const auto& c = polygon[next];
	if (Cross2D(a, b, c) * orientation <= kGeometryEpsilon)
		return false;

	for (const auto candidate : remaining)
	{
		if (candidate == previous || candidate == current || candidate == next)
			continue;

		if (PointInTriangle2D(polygon[candidate], a, b, c))
			return false;
	}
	return true;
}

std::optional<std::vector<int>> TriangulatePolygonEarClipping(
	FbxMesh* mesh,
	const FbxVector4* controlPoints,
	const int polygonIndex)
{
	auto projected = ProjectPolygon(mesh, controlPoints, polygonIndex);
	if (projected.size() < 3u)
		return std::nullopt;

	if (projected.size() == 3u)
		return std::vector<int> {
			projected[0].polygonVertexIndex,
			projected[1].polygonVertexIndex,
			projected[2].polygonVertexIndex
		};

	const double area = SignedArea(projected);
	if (std::fabs(area) <= kGeometryEpsilon)
		return std::nullopt;
	const double orientation = area > 0.0 ? 1.0 : -1.0;

	std::vector<size_t> remaining(projected.size());
	std::iota(remaining.begin(), remaining.end(), 0u);

	std::vector<int> triangles;
	triangles.reserve((projected.size() - 2u) * 3u);
	size_t guard = 0u;
	while (remaining.size() > 3u && guard++ < projected.size() * projected.size())
	{
		bool clipped = false;
		for (size_t index = 0u; index < remaining.size(); ++index)
		{
			if (!IsEar(projected, remaining, index, orientation))
				continue;

			const size_t previous = remaining[(index + remaining.size() - 1u) % remaining.size()];
			const size_t current = remaining[index];
			const size_t next = remaining[(index + 1u) % remaining.size()];
			triangles.push_back(projected[previous].polygonVertexIndex);
			triangles.push_back(projected[current].polygonVertexIndex);
			triangles.push_back(projected[next].polygonVertexIndex);
			remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(index));
			clipped = true;
			break;
		}

		if (!clipped)
			return std::nullopt;
	}

	if (remaining.size() != 3u)
		return std::nullopt;

	triangles.push_back(projected[remaining[0]].polygonVertexIndex);
	triangles.push_back(projected[remaining[1]].polygonVertexIndex);
	triangles.push_back(projected[remaining[2]].polygonVertexIndex);
	return triangles;
}

std::vector<int> TriangulatePolygon(
	FbxMesh* mesh,
	const FbxVector4* controlPoints,
	const int polygonIndex)
{
	const int polygonSize = mesh ? mesh->GetPolygonSize(polygonIndex) : 0;
	std::vector<int> triangles;
	if (polygonSize < 3)
		return triangles;

	if (polygonSize == 3)
		return {0, 1, 2};

	if (polygonSize > 4)
	{
		if (auto earClipped = TriangulatePolygonEarClipping(mesh, controlPoints, polygonIndex))
			return *earClipped;
	}

	triangles.reserve(static_cast<size_t>(polygonSize - 2) * 3u);
	for (int polygonVertexIndex = 1; polygonVertexIndex < polygonSize - 1; ++polygonVertexIndex)
	{
		triangles.push_back(0);
		triangles.push_back(polygonVertexIndex);
		triangles.push_back(polygonVertexIndex + 1);
	}
	return triangles;
}

size_t CountTriangulatedPolygonVertices(FbxMesh* mesh)
{
	if (!mesh)
		return 0u;

	size_t vertexCount = 0u;
	const int polygonCount = mesh->GetPolygonCount();
	for (int polygonIndex = 0; polygonIndex < polygonCount; ++polygonIndex)
	{
		const int polygonSize = mesh->GetPolygonSize(polygonIndex);
		if (polygonSize >= 3)
			vertexCount += static_cast<size_t>(polygonSize - 2) * 3u;
	}
	return vertexCount;
}

Geometry::Vertex ReadPolygonVertex(
	FbxMesh* mesh,
	const FbxVector4* controlPoints,
	const int polygonIndex,
	const int polygonVertexIndex,
	const FbxAMatrix& nodeTransform,
	const bool bakeNodeTransforms,
	const double globalScale)
{
	const int controlPointIndex = mesh->GetPolygonVertex(polygonIndex, polygonVertexIndex);
	FbxVector4 position = controlPoints && controlPointIndex >= 0 ? controlPoints[controlPointIndex] : FbxVector4();
	FbxVector4 normal = ReadNormal(mesh, polygonIndex, polygonVertexIndex);
	const FbxVector2 uv = ReadUv(mesh, polygonIndex, polygonVertexIndex, controlPointIndex);
	FbxVector4 tangent = ReadTangent(mesh, polygonIndex, polygonVertexIndex, controlPointIndex);
	FbxVector4 bitangent = ReadBitangent(mesh, polygonIndex, polygonVertexIndex, controlPointIndex);
	if ((std::fabs(bitangent[0]) <= kGeometryEpsilon &&
		std::fabs(bitangent[1]) <= kGeometryEpsilon &&
		std::fabs(bitangent[2]) <= kGeometryEpsilon) &&
		(std::fabs(tangent[0]) > kGeometryEpsilon ||
			std::fabs(tangent[1]) > kGeometryEpsilon ||
			std::fabs(tangent[2]) > kGeometryEpsilon))
	{
		bitangent = CrossDirection(normal, tangent);
	}

	if (bakeNodeTransforms)
	{
		position = nodeTransform.MultT(position);
		normal = TransformDirection(nodeTransform, normal);
		tangent = TransformDirection(nodeTransform, tangent);
		bitangent = TransformDirection(nodeTransform, bitangent);
	}
	ApplyGlobalScale(position, globalScale);

	return MakeVertex(position, uv, normal, tangent, bitangent);
}

void EmitPolygonVertex(
	FbxMesh* mesh,
	const FbxVector4* controlPoints,
	const int polygonIndex,
	const int polygonVertexIndex,
	const FbxAMatrix& nodeTransform,
	const bool bakeNodeTransforms,
	const double globalScale,
	ParsedMeshData& parsedMesh)
{
	parsedMesh.indices.push_back(static_cast<uint32_t>(parsedMesh.vertices.size()));
	parsedMesh.vertices.push_back(ReadPolygonVertex(
		mesh,
		controlPoints,
		polygonIndex,
		polygonVertexIndex,
		nodeTransform,
		bakeNodeTransforms,
		globalScale));
}

void ProcessMesh(
	FbxMesh* mesh,
	const FbxAMatrix& nodeTransform,
	const bool bakeNodeTransforms,
	const double globalScale,
	FbxNode* node,
	const FbxMaterialRegistry& materialRegistry,
	const uint32_t meshIndex,
	std::vector<ParsedMeshData>& parsedMeshes,
	std::vector<uint32_t>& primitiveMaterialIndices)
{
	if (!mesh)
		return;

	const int polygonCount = mesh->GetPolygonCount();
	if (polygonCount <= 0)
		return;

	PrepareMeshGeometry(mesh);

	const FbxVector4* controlPoints = mesh->GetControlPoints();
	std::unordered_map<uint32_t, size_t> primitiveByMaterialIndex;
	for (int polygonIndex = 0; polygonIndex < polygonCount; ++polygonIndex)
	{
		const int polygonSize = mesh->GetPolygonSize(polygonIndex);
		if (polygonSize < 3)
			continue;

		const uint32_t polygonMaterialIndex = ResolveNodeMaterialIndex(
			node,
			ResolvePolygonMaterialIndex(mesh, polygonIndex),
			materialRegistry);
		const auto foundPrimitive = primitiveByMaterialIndex.find(polygonMaterialIndex);
		size_t primitiveIndex = 0u;
		if (foundPrimitive == primitiveByMaterialIndex.end())
		{
			primitiveIndex = parsedMeshes.size();
			primitiveByMaterialIndex.emplace(polygonMaterialIndex, primitiveIndex);

			ParsedMeshData parsedMesh;
			parsedMesh.materialIndex = polygonMaterialIndex;
			parsedMesh.sourceMeshIndex = meshIndex;
			parsedMesh.sourceKey = IndexedKey("parser/mesh", meshIndex) + "/primitive/" + std::to_string(primitiveMaterialIndices.size());
			const auto reserveCount = CountTriangulatedPolygonVertices(mesh);
			parsedMesh.vertices.reserve(reserveCount);
			parsedMesh.indices.reserve(reserveCount);
			parsedMeshes.push_back(std::move(parsedMesh));
			primitiveMaterialIndices.push_back(polygonMaterialIndex);
		}
		else
		{
			primitiveIndex = foundPrimitive->second;
		}

		auto triangleVertices = TriangulatePolygon(mesh, controlPoints, polygonIndex);
		auto& parsedMesh = parsedMeshes[primitiveIndex];
		for (const int polygonVertexIndex : triangleVertices)
			EmitPolygonVertex(
				mesh,
				controlPoints,
				polygonIndex,
				polygonVertexIndex,
				nodeTransform,
				bakeNodeTransforms,
				globalScale,
				parsedMesh);
	}

	if (primitiveMaterialIndices.size() == 1u && !parsedMeshes.empty())
		parsedMeshes.back().sourceKey = IndexedKey("parser/mesh", meshIndex);
}

void ConvertNurbsAndPatches(FbxManager* manager, FbxNode* node)
{
	if (!manager || !node)
		return;

	FbxGeometryConverter geometryConverter(manager);
	for (int attributeIndex = 0; attributeIndex < node->GetNodeAttributeCount(); ++attributeIndex)
	{
		FbxNodeAttribute* attribute = node->GetNodeAttributeByIndex(attributeIndex);
		if (!attribute)
			continue;

		const auto attributeType = attribute->GetAttributeType();
		if (attributeType == FbxNodeAttribute::eNurbs || attributeType == FbxNodeAttribute::ePatch)
			geometryConverter.Triangulate(attribute, true);
	}

	for (int childIndex = 0; childIndex < node->GetChildCount(); ++childIndex)
		ConvertNurbsAndPatches(manager, node->GetChild(childIndex));
}

void ConfigureModelOnlyFbxImport(FbxIOSettings& ioSettings)
{
	ioSettings.SetBoolProp(IMP_FBX_MODEL, true);
	ioSettings.SetBoolProp(IMP_FBX_MATERIAL, true);
	ioSettings.SetBoolProp(IMP_FBX_TEXTURE, true);
	ioSettings.SetBoolProp(IMP_FBX_GLOBAL_SETTINGS, true);
	ioSettings.SetBoolProp(IMP_FBX_NORMAL, true);
	ioSettings.SetBoolProp(IMP_FBX_TANGENT, true);
	ioSettings.SetBoolProp(IMP_FBX_BINORMAL, true);
	ioSettings.SetBoolProp(IMP_FBX_VERTEXCOLOR, true);
	ioSettings.SetBoolProp(IMP_FBX_LINK, false);
	ioSettings.SetBoolProp(IMP_FBX_ANIMATION, false);
	ioSettings.SetBoolProp(IMP_FBX_CHARACTER, false);
	ioSettings.SetBoolProp(IMP_FBX_CONSTRAINT, false);
	ioSettings.SetBoolProp(IMP_FBX_AUDIO, false);
	ioSettings.SetBoolProp(IMP_FBX_EXTRACT_EMBEDDED_DATA, false);
	ioSettings.SetBoolProp(IMP_FBX_SHAPE, false);
	ioSettings.SetBoolProp(IMP_FBX_GOBO, false);
}

void BuildMeshRecord(
	const uint32_t meshIndex,
	FbxMesh* sourceMesh,
	const std::vector<uint32_t>& primitiveMaterialIndices,
	ImportedScene& scene,
	const std::string& meshName)
{
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

	ImportedSceneNamedRecord record;
	record.sourceKey = meshKey;
	record.name = meshName.empty() ? "Mesh " + std::to_string(meshIndex) : meshName;
	record.primitiveCount = static_cast<uint32_t>(primitiveMaterialIndices.size());
	record.attributes = BuildMeshAttributes(sourceMesh);

	record.primitives.reserve(primitiveMaterialIndices.size());
	for (const auto materialIndex : primitiveMaterialIndices)
	{
		ImportedScenePrimitive primitive;
		if (materialIndex < scene.materials.size())
			primitive.materialKey = IndexedKey("parser/material", materialIndex);
		record.primitives.push_back(std::move(primitive));
	}
	scene.meshes.push_back(std::move(record));
}

void ApplyNodeTransform(FbxNode* node, ImportedSceneNode& record, const double globalScale)
{
	if (!node)
		return;

	const auto transform = node->EvaluateLocalTransform();
	const auto translation = transform.GetT();
	const auto rotation = transform.GetQ();
	const auto scale = transform.GetS();

	record.translation = {
		translation[0] * globalScale,
		translation[1] * globalScale,
		translation[2] * globalScale
	};
	record.rotation = {rotation[0], rotation[1], rotation[2], rotation[3]};
	record.scale = {scale[0], scale[1], scale[2]};
}

void ProcessNode(
	FbxNode* node,
	const std::string& parentKey,
	const FbxAMatrix& parentTransform,
	const bool bakeNodeTransforms,
	const double globalScale,
	std::vector<ParsedMeshData>& parsedMeshes,
	ImportedScene& scene,
	uint32_t& nextNodeIndex,
	uint32_t& nextMeshIndex,
	const FbxMaterialRegistry& materialRegistry,
	std::unordered_map<FbxMesh*, MeshImportRecord>& importedMeshes)
{
	if (!node)
		return;

	const auto nodeIndex = nextNodeIndex++;
	const auto nodeKey = IndexedKey("parser/node", nodeIndex);
	const auto nodeTransform = parentTransform * node->EvaluateLocalTransform();

	ImportedSceneNode nodeRecord;
	nodeRecord.sourceKey = nodeKey;
	nodeRecord.name = FbxName(node->GetName(), "Node", nodeIndex);
	nodeRecord.parentKey = parentKey;
	ApplyNodeTransform(node, nodeRecord, globalScale);

	if (FbxMesh* mesh = node->GetMesh())
	{
		const auto foundMesh = !bakeNodeTransforms ? importedMeshes.find(mesh) : importedMeshes.end();
		const auto nodeMaterialIndices = ResolveNodeMaterialIndices(node, materialRegistry);
		if (foundMesh != importedMeshes.end() &&
			foundMesh->second.materialIndices == nodeMaterialIndices)
		{
			nodeRecord.meshKey = foundMesh->second.meshKey;
		}
		else
		{
			const auto meshIndex = nextMeshIndex++;
			const size_t firstPrimitiveIndex = parsedMeshes.size();
			std::vector<uint32_t> primitiveMaterialIndices;
			ProcessMesh(
				mesh,
				nodeTransform,
				bakeNodeTransforms,
				globalScale,
				node,
				materialRegistry,
				meshIndex,
				parsedMeshes,
				primitiveMaterialIndices);

			for (size_t primitiveIndex = firstPrimitiveIndex; primitiveIndex < parsedMeshes.size(); ++primitiveIndex)
				parsedMeshes[primitiveIndex].sourceMeshIndex = meshIndex;

			BuildMeshRecord(
				meshIndex,
				mesh,
				primitiveMaterialIndices,
				scene,
				FbxName(mesh->GetName(), "Mesh", meshIndex));

			MeshImportRecord meshRecord;
			meshRecord.meshIndex = meshIndex;
			meshRecord.meshKey = IndexedKey("parser/mesh", meshIndex);
			meshRecord.materialIndices = std::move(primitiveMaterialIndices);
			meshRecord.firstPrimitiveIndex = firstPrimitiveIndex;
			meshRecord.primitiveCount = parsedMeshes.size() - firstPrimitiveIndex;
			if (!bakeNodeTransforms)
				importedMeshes.emplace(mesh, meshRecord);
			nodeRecord.meshKey = meshRecord.meshKey;
		}
	}

	scene.nodes.push_back(std::move(nodeRecord));

	for (int childIndex = 0; childIndex < node->GetChildCount(); ++childIndex)
		ProcessNode(
			node->GetChild(childIndex),
			nodeKey,
			nodeTransform,
			bakeNodeTransforms,
			globalScale,
			parsedMeshes,
			scene,
			nextNodeIndex,
			nextMeshIndex,
			materialRegistry,
			importedMeshes);
}

bool LoadFbxScene(
	const std::string& sourcePath,
	FbxPtr<FbxManager>& outManager,
	FbxPtr<FbxScene>& outScene)
{
	NLS_PROFILE_NAMED_SCOPE("AssetImport::FbxSdk::LoadFbxScene");
	const auto managerStart = std::chrono::steady_clock::now();
	{
		NLS_PROFILE_NAMED_SCOPE("AssetImport::FbxSdk::CreateManager");
		outManager.reset(FbxManager::Create());
	}
	LogFbxSdkImportTiming(sourcePath, "CreateManager", MillisecondsSince(managerStart), 0u, 0u);
	if (!outManager)
		return false;

	FbxPtr<FbxIOSettings> ioSettings(FbxIOSettings::Create(outManager.get(), IOSROOT));
	if (!ioSettings)
		return false;
	ConfigureModelOnlyFbxImport(*ioSettings);
	outManager->SetIOSettings(ioSettings.get());

	FbxPtr<FbxImporter> importer(FbxImporter::Create(outManager.get(), ""));
	if (!importer)
		return false;

	const auto initializeStart = std::chrono::steady_clock::now();
	const bool initialized =
		[&]()
		{
			NLS_PROFILE_NAMED_SCOPE("AssetImport::FbxSdk::InitializeImporter");
			return importer->Initialize(sourcePath.c_str(), -1, outManager->GetIOSettings());
		}();
	LogFbxSdkImportTiming(sourcePath, "InitializeImporter", MillisecondsSince(initializeStart), 0u, 0u);
	if (!initialized)
	{
		NLS_LOG_ERROR("[AssetImport][FbxSdk] Initialize failed: " + std::string(importer->GetStatus().GetErrorString()));
		return false;
	}

	const auto sceneStart = std::chrono::steady_clock::now();
	{
		NLS_PROFILE_NAMED_SCOPE("AssetImport::FbxSdk::CreateScene");
		outScene.reset(FbxScene::Create(outManager.get(), "NullusImportedFbxScene"));
	}
	LogFbxSdkImportTiming(sourcePath, "CreateScene", MillisecondsSince(sceneStart), 0u, 0u);
	if (!outScene)
		return false;

	const auto importStart = std::chrono::steady_clock::now();
	const bool imported =
		[&]()
		{
			NLS_PROFILE_NAMED_SCOPE("AssetImport::FbxSdk::ImportScene");
			return importer->Import(outScene.get());
		}();
	LogFbxSdkImportTiming(
		sourcePath,
		"ImportScene",
		MillisecondsSince(importStart),
		imported ? static_cast<uint32_t>(outScene->GetGeometryCount()) : 0u,
		imported ? static_cast<uint32_t>(outScene->GetMaterialCount()) : 0u);
	if (!imported)
		NLS_LOG_ERROR("[AssetImport][FbxSdk] Import failed: " + std::string(importer->GetStatus().GetErrorString()));
	return imported;
}

bool LoadSceneData(
	const std::string& fileName,
	const EModelParserFlags parserFlags,
	std::vector<ParsedMeshData>& meshes,
	std::vector<std::string>& materials,
	std::vector<std::string>* externalDependencies,
	const bool bakeNodeTransforms,
	ImportedScene& importedScene)
{
	NLS_PROFILE_NAMED_SCOPE("AssetImport::FbxSdk::LoadSceneData");
	FbxPtr<FbxManager> manager;
	FbxPtr<FbxScene> scene;
	const auto readStart = std::chrono::steady_clock::now();
	if (!LoadFbxScene(fileName, manager, scene))
	{
		LogFbxSdkImportTiming(fileName, "LoadFbxSceneFailed", MillisecondsSince(readStart), 0u, 0u);
		return false;
	}
	const auto readElapsed = MillisecondsSince(readStart);

	if (HasFlag(parserFlags, EModelParserFlags::TRIANGULATE))
	{
		const auto convertStart = std::chrono::steady_clock::now();
		{
			NLS_PROFILE_NAMED_SCOPE("AssetImport::FbxSdk::ConvertNurbsAndPatches");
			ConvertNurbsAndPatches(manager.get(), scene->GetRootNode());
		}
		LogFbxSdkImportTiming(
			fileName,
			"ConvertNurbsAndPatches",
			MillisecondsSince(convertStart),
			static_cast<uint32_t>(scene->GetGeometryCount()),
			static_cast<uint32_t>(scene->GetMaterialCount()));
	}
	const double globalScale = ResolveGlobalScale(scene.get(), parserFlags);

	const auto meshCount = static_cast<uint32_t>(scene->GetGeometryCount());
	const auto materialCount = static_cast<uint32_t>(scene->GetMaterialCount());
	LogFbxSdkImportTiming(fileName, "ReadFile", readElapsed, meshCount, materialCount);

	const auto materialStart = std::chrono::steady_clock::now();
	FbxMaterialRegistry materialRegistry;
	{
		NLS_PROFILE_NAMED_SCOPE("AssetImport::FbxSdk::ProcessMaterials");
		BuildMaterials(scene.get(), importedScene, materials, externalDependencies, materialRegistry);
	}
	LogFbxSdkImportTiming(
		fileName,
		"ProcessMaterials",
		MillisecondsSince(materialStart),
		meshCount,
		materialCount);

	const auto meshStart = std::chrono::steady_clock::now();
	{
		NLS_PROFILE_NAMED_SCOPE("AssetImport::FbxSdk::ProcessMeshes");
		uint32_t nextNodeIndex = 0u;
		uint32_t nextMeshIndex = 0u;
		std::unordered_map<FbxMesh*, MeshImportRecord> importedMeshes;
		if (FbxNode* rootNode = scene->GetRootNode())
			ProcessNode(
				rootNode,
				{},
				IdentityMatrix(),
				bakeNodeTransforms,
				globalScale,
				meshes,
				importedScene,
				nextNodeIndex,
				nextMeshIndex,
				materialRegistry,
				importedMeshes);
	}

	LogFbxSdkImportTiming(
		fileName,
		"ProcessMeshes",
		MillisecondsSince(meshStart),
		meshCount,
		materialCount);
	LogFbxSdkImportTiming(
		fileName,
		"LoadModelDataTotal",
		MillisecondsSince(readStart),
		meshCount,
		materialCount);

	return !meshes.empty();
}
}

bool FbxSdkParser::LoadModel(
	const std::string& p_fileName,
	std::vector<Mesh*>& p_meshes,
	std::vector<std::string>& p_materials,
	EModelParserFlags p_parserFlags)
{
	p_meshes.clear();
	p_materials.clear();
	m_lastImportedScene = {};
	m_hasImportedSceneData = false;

	std::vector<ParsedMeshData> parsedMeshes;
	if (!LoadModelData(p_fileName, parsedMeshes, p_materials, p_parserFlags, nullptr, true))
		return false;

	for (auto& mesh : parsedMeshes)
		p_meshes.push_back(new Mesh(mesh.vertices, mesh.indices, mesh.materialIndex));
	return true;
}

bool FbxSdkParser::LoadModelData(
	const std::string& p_fileName,
	std::vector<ParsedMeshData>& p_meshes,
	std::vector<std::string>& p_materials,
	EModelParserFlags p_parserFlags,
	std::vector<std::string>* p_externalDependencies,
	bool p_bakeNodeTransforms)
{
	NLS_PROFILE_NAMED_SCOPE("AssetImport::FbxSdk::LoadModelData");
	p_meshes.clear();
	p_materials.clear();
	m_lastImportedScene = {};
	m_hasImportedSceneData = false;
	if (p_externalDependencies)
		p_externalDependencies->clear();

	ImportedScene importedScene;
	const auto loaded = LoadSceneData(
		p_fileName,
		p_parserFlags,
		p_meshes,
		p_materials,
		p_externalDependencies,
		p_bakeNodeTransforms,
		importedScene);
	if (!loaded)
		return false;

	m_lastImportedScene = std::move(importedScene);
	m_hasImportedSceneData = true;
	return true;
}

bool FbxSdkParser::PopulateImportedSceneData(
	const std::filesystem::path&,
	SceneModelSourceFormat p_sourceFormat,
	ImportedScene& p_scene)
{
	if (!m_hasImportedSceneData || p_sourceFormat != SceneModelSourceFormat::Fbx)
		return false;

	auto scene = m_lastImportedScene;
	scene.sourceAssetId = p_scene.sourceAssetId;
	scene.sceneKey = p_scene.sceneKey;
	scene.importSettings = p_scene.importSettings;
	p_scene = std::move(scene);
	return true;
}
#else
bool FbxSdkParser::LoadModel(
	const std::string&,
	std::vector<Mesh*>& p_meshes,
	std::vector<std::string>& p_materials,
	EModelParserFlags)
{
	p_meshes.clear();
	p_materials.clear();
	m_lastImportedScene = {};
	m_hasImportedSceneData = false;
	NLS_LOG_ERROR("[AssetImport][FbxSdk] Autodesk FBX SDK is unavailable in this build.");
	return false;
}

bool FbxSdkParser::LoadModelData(
	const std::string&,
	std::vector<ParsedMeshData>& p_meshes,
	std::vector<std::string>& p_materials,
	EModelParserFlags,
	std::vector<std::string>* p_externalDependencies,
	bool)
{
	p_meshes.clear();
	p_materials.clear();
	m_lastImportedScene = {};
	m_hasImportedSceneData = false;
	if (p_externalDependencies)
		p_externalDependencies->clear();

	NLS_LOG_ERROR("[AssetImport][FbxSdk] Autodesk FBX SDK is unavailable in this build.");
	return false;
}

bool FbxSdkParser::PopulateImportedSceneData(
	const std::filesystem::path&,
	NLS::Render::Assets::SceneModelSourceFormat,
	NLS::Render::Assets::ImportedScene&)
{
	return false;
}
#endif
}
