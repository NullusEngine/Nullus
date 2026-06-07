#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/scene.h>
#include <assimp/matrix4x4.h>
#include <assimp/postprocess.h>
#include <assimp/quaternion.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

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

struct AssimpDirectionTransforms
{
	aiMatrix4x4 linear;
	aiMatrix4x4 normal;
};

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

		const auto uri = AiStringToStdString(texturePath);
		AddUniqueDependency(externalDependencies, uri);

		auto& channel = EnsureChannel(material, slot.channelName);
		if (channel.textureKey.empty())
		{
			channel.textureKey = RegisterTexture(scene, textureKeysByUri, texturePath);
		}
		else
		{
			ImportedSceneMaterialChannel extraChannel;
			extraChannel.name = slot.channelName;
			extraChannel.textureKey = RegisterTexture(scene, textureKeysByUri, texturePath);
			material.materialChannels.push_back(std::move(extraChannel));
		}
	}
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

void BuildMaterials(
	const aiScene* source,
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

		for (const auto slot : MaterialTextureSlots())
			AddTextureChannel(*material, scene, record, textureKeysByUri, externalDependencies, slot);

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
		return false;

	ProcessMaterials(scene, p_materials, nullptr);

	std::vector<ParsedMeshData> parsedMeshes;
	aiMatrix4x4 identity;
	ProcessNode(&identity, scene->mRootNode, scene, parsedMeshes);

	for (auto& mesh : parsedMeshes)
		p_meshes.push_back(new Mesh(mesh.vertices, mesh.indices, mesh.materialIndex));

	BuildImportedSceneData(scene, SourceFormatForPath(p_fileName), m_lastImportedScene);
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
		return false;
	LogAssimpImportTiming(p_fileName, "ReadFile", readElapsed, scene->mNumMeshes, scene->mNumMaterials);

	const auto materialsStart = std::chrono::steady_clock::now();
	ProcessMaterials(scene, p_materials, p_externalDependencies);
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
	BuildImportedSceneData(scene, SourceFormatForPath(p_fileName), m_lastImportedScene);
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

void AssimpParser::BuildImportedSceneData(
	const aiScene* p_scene,
	NLS::Render::Assets::SceneModelSourceFormat p_sourceFormat,
	NLS::Render::Assets::ImportedScene& p_outScene)
{
	std::vector<std::string> unusedMaterialNames;
	BuildMaterials(p_scene, p_outScene, unusedMaterialNames, nullptr);

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
