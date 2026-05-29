#pragma once

#include "Rendering/Assets/ImportedScene.h"
#include "RenderDef.h"

#include <filesystem>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace NLS::Render::Resources::Parsers
{
class IModelParser;
struct ParsedMeshData;
}

namespace NLS::Render::Assets
{
enum class SceneModelSourceFormat
{
    Gltf,
    Fbx,
    Obj
};

struct SceneImporterDescriptor
{
    std::string importerId;
    uint32_t importerVersion = 1u;
    std::vector<std::string> extensions;
};

struct ScriptedSceneImportRequest
{
    std::filesystem::path sourcePath;
    NLS::Core::Assets::AssetId sourceAssetId;
    std::string sceneKey;
};

struct ScriptedSceneImporterDescriptor
{
    std::string importerId;
    uint32_t importerVersion = 1u;
    std::vector<std::string> extensions;
    std::function<ImportedScene(const ScriptedSceneImportRequest&)> import;
};

class IImportedSceneParserDataProvider
{
public:
    virtual ~IImportedSceneParserDataProvider() = default;

    virtual bool PopulateImportedSceneData(
        const std::filesystem::path& sourcePath,
        SceneModelSourceFormat sourceFormat,
        ImportedScene& scene) = 0;
};

class NLS_RENDER_API SceneImporterRegistry
{
public:
    static SceneImporterRegistry CreateDefault();

    void Register(SceneImporterDescriptor descriptor);
    void RegisterScripted(ScriptedSceneImporterDescriptor descriptor);
    const SceneImporterDescriptor* FindImporterForPath(const std::filesystem::path& sourcePath) const;
    std::optional<ImportedScene> ImportScripted(const ScriptedSceneImportRequest& request) const;

    const std::vector<SceneImporterDescriptor>& GetImporters() const;

private:
    std::vector<SceneImporterDescriptor> m_importers;
    std::vector<ScriptedSceneImporterDescriptor> m_scriptedImporters;
};

NLS_RENDER_API std::vector<GeneratedSceneSubAsset> GenerateSceneSubAssets(const ImportedScene& scene);
NLS_RENDER_API const char* ToSubAssetPrefix(ImportedSceneSubAssetType type);
NLS_RENDER_API std::string BuildPrimitiveMeshSourceKey(const std::string& meshKey, size_t primitiveIndex);
NLS_RENDER_API std::optional<std::pair<std::string, size_t>> ParsePrimitiveMeshSourceKey(const std::string& meshKey);
NLS_RENDER_API ImportedScene ImportGltfSceneJson(
    const std::string& jsonText,
    NLS::Core::Assets::AssetId sourceAssetId,
    std::string sceneKey);
NLS_RENDER_API ImportedScene ImportGltfSceneJson(
    const std::string& jsonText,
    NLS::Core::Assets::AssetId sourceAssetId,
    std::string sceneKey,
    const SceneImportSettings& settings);
NLS_RENDER_API ImportedScene ImportGltfSceneBytes(
    const std::vector<uint8_t>& sourceBytes,
    NLS::Core::Assets::AssetId sourceAssetId,
    std::string sceneKey);
NLS_RENDER_API ImportedScene ImportGltfSceneBytes(
    const std::vector<uint8_t>& sourceBytes,
    NLS::Core::Assets::AssetId sourceAssetId,
    std::string sceneKey,
    const SceneImportSettings& settings);
NLS_RENDER_API void ApplySceneImportSettings(ImportedScene& scene);
NLS_RENDER_API ImportedScene ImportParserModelScene(
    Resources::Parsers::IModelParser& parser,
    const std::filesystem::path& sourcePath,
    SceneModelSourceFormat sourceFormat,
    NLS::Core::Assets::AssetId sourceAssetId,
    std::string sceneKey);
NLS_RENDER_API ImportedScene ImportParsedModelScene(
    const std::vector<Resources::Parsers::ParsedMeshData>& meshes,
    const std::vector<std::string>& materialNames,
    const std::filesystem::path& sourcePath,
    SceneModelSourceFormat sourceFormat,
    NLS::Core::Assets::AssetId sourceAssetId,
    std::string sceneKey);
}
