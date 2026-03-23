#include "Core/ResourceManagement/ModelManager.h"

#include <Filesystem/IniFile.h>

namespace
{
using EModelParserFlags = NLS::Render::Resources::Parsers::EModelParserFlags;
using Mesh = NLS::Render::Resources::Mesh;
using Model = NLS::Render::Resources::Model;
using ModelLoader = NLS::Render::Resources::Loaders::ModelLoader;

EModelParserFlags GetModelMetadata(const std::string& path)
{
	auto metaFile = NLS::Filesystem::IniFile(path + ".meta");
	EModelParserFlags flags = EModelParserFlags::NONE;

	if (metaFile.GetOrDefault("CALC_TANGENT_SPACE", true)) flags |= EModelParserFlags::CALC_TANGENT_SPACE;
	if (metaFile.GetOrDefault("JOIN_IDENTICAL_VERTICES", true)) flags |= EModelParserFlags::JOIN_IDENTICAL_VERTICES;
	if (metaFile.GetOrDefault("MAKE_LEFT_HANDED", false)) flags |= EModelParserFlags::MAKE_LEFT_HANDED;
	if (metaFile.GetOrDefault("TRIANGULATE", true)) flags |= EModelParserFlags::TRIANGULATE;
	if (metaFile.GetOrDefault("REMOVE_COMPONENT", false)) flags |= EModelParserFlags::REMOVE_COMPONENT;
	if (metaFile.GetOrDefault("GEN_NORMALS", false)) flags |= EModelParserFlags::GEN_NORMALS;
	if (metaFile.GetOrDefault("GEN_SMOOTH_NORMALS", true)) flags |= EModelParserFlags::GEN_SMOOTH_NORMALS;
	if (metaFile.GetOrDefault("SPLIT_LARGE_MESHES", false)) flags |= EModelParserFlags::SPLIT_LARGE_MESHES;
	if (metaFile.GetOrDefault("PRE_TRANSFORM_VERTICES", false)) flags |= EModelParserFlags::PRE_TRANSFORM_VERTICES;
	if (metaFile.GetOrDefault("LIMIT_BONE_WEIGHTS", false)) flags |= EModelParserFlags::LIMIT_BONE_WEIGHTS;
	if (metaFile.GetOrDefault("VALIDATE_DATA_STRUCTURE", false)) flags |= EModelParserFlags::VALIDATE_DATA_STRUCTURE;
	if (metaFile.GetOrDefault("IMPROVE_CACHE_LOCALITY", true)) flags |= EModelParserFlags::IMPROVE_CACHE_LOCALITY;
	if (metaFile.GetOrDefault("REMOVE_REDUNDANT_MATERIALS", false)) flags |= EModelParserFlags::REMOVE_REDUNDANT_MATERIALS;
	if (metaFile.GetOrDefault("FIX_INFACING_NORMALS", false)) flags |= EModelParserFlags::FIX_INFACING_NORMALS;
	if (metaFile.GetOrDefault("SORT_BY_PTYPE", false)) flags |= EModelParserFlags::SORT_BY_PTYPE;
	if (metaFile.GetOrDefault("FIND_DEGENERATES", false)) flags |= EModelParserFlags::FIND_DEGENERATES;
	if (metaFile.GetOrDefault("FIND_INVALID_DATA", true)) flags |= EModelParserFlags::FIND_INVALID_DATA;
	if (metaFile.GetOrDefault("GEN_UV_COORDS", true)) flags |= EModelParserFlags::GEN_UV_COORDS;
	if (metaFile.GetOrDefault("TRANSFORM_UV_COORDS", false)) flags |= EModelParserFlags::TRANSFORM_UV_COORDS;
	if (metaFile.GetOrDefault("FIND_INSTANCES", true)) flags |= EModelParserFlags::FIND_INSTANCES;
	if (metaFile.GetOrDefault("OPTIMIZE_MESHES", true)) flags |= EModelParserFlags::OPTIMIZE_MESHES;
	if (metaFile.GetOrDefault("OPTIMIZE_GRAPH", false)) flags |= EModelParserFlags::OPTIMIZE_GRAPH;
	if (metaFile.GetOrDefault("FLIP_UVS", false)) flags |= EModelParserFlags::FLIP_UVS;
	if (metaFile.GetOrDefault("FLIP_WINDING_ORDER", false)) flags |= EModelParserFlags::FLIP_WINDING_ORDER;
	if (metaFile.GetOrDefault("SPLIT_BY_BONE_COUNT", false)) flags |= EModelParserFlags::SPLIT_BY_BONE_COUNT;
	if (metaFile.GetOrDefault("DEBONE", true)) flags |= EModelParserFlags::DEBONE;
	if (metaFile.GetOrDefault("GLOBAL_SCALE", true)) flags |= EModelParserFlags::GLOBAL_SCALE;
	if (metaFile.GetOrDefault("EMBED_TEXTURES", false)) flags |= EModelParserFlags::EMBED_TEXTURES;
	if (metaFile.GetOrDefault("FORCE_GEN_NORMALS", false)) flags |= EModelParserFlags::FORCE_GEN_NORMALS;
	if (metaFile.GetOrDefault("DROP_NORMALS", false)) flags |= EModelParserFlags::DROP_NORMALS;
	if (metaFile.GetOrDefault("GEN_BOUNDING_BOXES", false)) flags |= EModelParserFlags::GEN_BOUNDING_BOXES;

	return flags;
}
}

namespace NLS::Core::ResourceManagement
{
Model* ModelManager::CreateResource(const std::string& path)
{
	std::string realPath = GetRealPath(path);
	Model* model = ModelLoader::Create(realPath, GetModelMetadata(realPath));
	if (model)
	{
		const_cast<std::string&>(model->path) = path;
	}

	return model;
}

Model* ModelManager::CreateResource(const std::string& name, const std::vector<Mesh*>& meshes)
{
	Model* model = ModelLoader::Create(meshes);
	if (model)
	{
		const_cast<std::string&>(model->path) = name;
	}

	return model;
}

void ModelManager::DestroyResource(Model* resource)
{
	ModelLoader::Destroy(resource);
}

void ModelManager::ReloadResource(Model* resource, const std::string& path)
{
	std::string realPath = GetRealPath(path);
	ModelLoader::Reload(*resource, realPath, GetModelMetadata(realPath));
}
}
