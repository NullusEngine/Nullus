#include "Core/ResourceManagement/MeshManager.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>

#include <Filesystem/IniFile.h>
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Resources/Parsers/AssimpParser.h"
#include "Rendering/Resources/Parsers/FbxSdkParser.h"

namespace
{
using EModelParserFlags = NLS::Render::Resources::Parsers::EModelParserFlags;

std::string ToLower(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

bool IsBuiltinPath(const std::string& path)
{
    return !path.empty() && path[0] == ':';
}

bool IsPrimitiveAliasPath(const std::string& path)
{
    return path.rfind("builtin:Primitive/", 0) == 0;
}

std::optional<std::string> PrimitiveAliasToBuiltinModelPath(const std::string& path)
{
    if (!IsPrimitiveAliasPath(path))
        return std::nullopt;

    auto primitiveName = path.substr(std::string("builtin:Primitive/").size());
    if (primitiveName.empty())
        return std::nullopt;

    primitiveName[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(primitiveName[0])));
    return std::string(":Models/") + primitiveName + ".fbx";
}

bool IsSourceModelExtension(const std::filesystem::path& path)
{
    const auto extension = ToLower(path.extension().string());
    return extension == ".fbx" || extension == ".obj" || extension == ".gltf" || extension == ".glb";
}

bool IsMeshArtifactPath(const std::filesystem::path& path)
{
    return ToLower(path.extension().string()) == ".nmesh";
}

std::filesystem::path NormalizeVirtualBuiltinPath(const std::string& path)
{
    std::string relativePath = path;
    if (IsBuiltinPath(relativePath))
        relativePath.erase(relativePath.begin());
    std::replace(relativePath.begin(), relativePath.end(), '\\', '/');
    return std::filesystem::path(relativePath);
}

std::filesystem::path NormalizeConfiguredRoot(std::string path)
{
    while (!path.empty() && (path.back() == '/' || path.back() == '\\'))
        path.pop_back();
    return std::filesystem::path(path).lexically_normal();
}

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

bool WriteMeshArtifact(
    const std::filesystem::path& artifactPath,
    const NLS::Render::Resources::Parsers::ParsedMeshData& mesh)
{
    std::error_code error;
    std::filesystem::create_directories(artifactPath.parent_path(), error);
    if (error)
        return false;

    NLS::Render::Assets::MeshArtifactData artifact;
    artifact.vertices = mesh.vertices;
    artifact.indices = mesh.indices;
    artifact.materialIndex = mesh.materialIndex;
    const auto bytes = NLS::Render::Assets::SerializeMeshArtifact(artifact);
    if (bytes.empty())
        return false;

    std::ofstream output(artifactPath, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;

    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

bool GenerateBuiltinMeshArtifact(
    const std::string& sourcePath,
    const std::filesystem::path& artifactPath,
    const EModelParserFlags parserFlags)
{
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;
    const auto extension = ToLower(std::filesystem::path(sourcePath).extension().string());
    const bool loaded = [&]()
    {
        if (extension == ".fbx")
        {
            NLS::Render::Resources::Parsers::FbxSdkParser parser;
            return parser.LoadModelData(sourcePath, meshes, materials, parserFlags, nullptr, true);
        }

        NLS::Render::Resources::Parsers::AssimpParser parser;
        return parser.LoadModelData(sourcePath, meshes, materials, parserFlags, nullptr, true);
    }();
    if (!loaded || meshes.size() != 1u)
        return false;

    return WriteMeshArtifact(artifactPath, meshes.front());
}

std::string ResolveBuiltinMeshArtifactPath(const std::string& path)
{
    std::string sourcePathAlias = path;
    if (const auto primitiveSource = PrimitiveAliasToBuiltinModelPath(path))
        sourcePathAlias = *primitiveSource;

    if (!IsBuiltinPath(sourcePathAlias))
        return {};

    auto relativePath = NormalizeVirtualBuiltinPath(sourcePathAlias);
    if (!IsSourceModelExtension(relativePath))
        return {};

    relativePath.replace_extension(".nmesh");
    std::error_code error;
    const auto projectAssetsPath = NLS::Core::ResourceManagement::MeshManager::ProjectAssetsRoot();
    std::filesystem::path projectCachePath;
    if (!projectAssetsPath.empty())
    {
        const auto projectAssetsRoot = NormalizeConfiguredRoot(projectAssetsPath);
        projectCachePath =
            (projectAssetsRoot.parent_path() /
             "Library" / "BuiltinArtifacts" / relativePath).lexically_normal();
        if (std::filesystem::is_regular_file(projectCachePath, error))
            return projectCachePath.string();
    }

    const auto bundledArtifactPath = std::filesystem::path("Library") / "BuiltinArtifacts" / relativePath;
    const auto realPath = NLS::Core::ResourceManagement::MeshManager::ResolveResourcePath(
        ":" + bundledArtifactPath.generic_string());
    if (std::filesystem::is_regular_file(realPath, error))
        return realPath;

    if (!projectCachePath.empty())
    {
        const auto sourcePath = NLS::Core::ResourceManagement::MeshManager::ResolveResourcePath(sourcePathAlias);
        if (std::filesystem::is_regular_file(sourcePath, error) &&
            GenerateBuiltinMeshArtifact(sourcePath, projectCachePath, GetModelMetadata(sourcePath)))
        {
            return projectCachePath.string();
        }
    }

    return {};
}
}

namespace NLS::Core::ResourceManagement
{
std::string MeshManager::ResolveResourcePath(const std::string& path)
{
    return GetRealPath(path);
}

std::string MeshManager::ResolveArtifactResourcePath(const std::string& path)
{
    auto resolved = ResolveBuiltinMeshArtifactPath(path);
    if (!resolved.empty())
        return resolved;
    return ResolveResourcePath(path);
}

const std::string& MeshManager::ProjectAssetsRoot()
{
    return GetProjectAssetsPath();
}

MeshManager::Mesh* MeshManager::CreateResource(const std::string& path)
{
    const auto realPath = ResolveArtifactResourcePath(path);
    if (!IsMeshArtifactPath(realPath))
        return nullptr;

    auto meshArtifact = NLS::Render::Assets::LoadMeshArtifact(realPath);
    if (!meshArtifact.has_value())
        return nullptr;

    return new Mesh(
        meshArtifact->vertices,
        meshArtifact->indices,
        meshArtifact->materialIndex,
        NLS::Render::Resources::MeshBufferUploadMode::GpuOnly,
        meshArtifact->boundingSphere);
}

void MeshManager::DestroyResource(Mesh* resource)
{
    delete resource;
}

void MeshManager::ReloadResource(Mesh* resource, const std::string& path)
{
    if (resource == nullptr)
        return;

    const auto realPath = ResolveArtifactResourcePath(path);
    if (!IsMeshArtifactPath(realPath))
        return;

    auto meshArtifact = NLS::Render::Assets::LoadMeshArtifact(realPath);
    if (!meshArtifact.has_value())
        return;

    resource->Reload(
        meshArtifact->vertices,
        meshArtifact->indices,
        meshArtifact->materialIndex,
        NLS::Render::Resources::MeshBufferUploadMode::GpuOnly,
        meshArtifact->boundingSphere);
}
}
