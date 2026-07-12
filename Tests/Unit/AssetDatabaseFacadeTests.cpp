#include <gtest/gtest.h>

#include <sstream>

#ifndef NLS_HAS_AUTODESK_FBX_SDK
#define NLS_HAS_AUTODESK_FBX_SDK 0
#endif

#ifndef NLS_HAS_DIRECTXTEX
#define NLS_HAS_DIRECTXTEX 0
#endif

#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/ArtifactWriter.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/AssetBrowserPresentation.h"
#include "Assets/AssetDragDropWorkflow.h"
#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/EditorAssetDatabase.h"
#include "Assets/ExternalAssetImporter.h"
#include "Assets/ModelTextureReferenceResolver.h"
#include "Assets/PrefabEditorWorkflow.h"
#include "Core/ServiceLocator.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Engine/Assets/RuntimeAssetDatabase.h"
#include "GameObject.h"
#include "Guid.h"
#include "Assets/NativeArtifactContainer.h"
#include "Profiling/PerformanceStageStats.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/Resources/Loaders/TextureLoader.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "Rendering/ShaderCompiler/ShaderCompiler.h"
#include "SceneSystem/Scene.h"
#include "Serialize/PPtr.h"

#include <Json/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace
{
std::filesystem::path MakeAssetDatabaseFacadeRoot()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_asset_database_facade_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root / "Assets");
    return root;
}

void WriteTextFile(const std::filesystem::path& path, const std::string& contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
}

void WriteBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

std::vector<uint8_t> TinyPng()
{
    return {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x04, 0x00, 0x00, 0x00, 0xB5, 0x1C, 0x0C,
        0x02, 0x00, 0x00, 0x00, 0x0B, 0x49, 0x44, 0x41,
        0x54, 0x78, 0xDA, 0x63, 0xFC, 0xFF, 0x1F, 0x00,
        0x03, 0x03, 0x02, 0x00, 0xEF, 0xBF, 0x4A, 0x3B,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
        0xAE, 0x42, 0x60, 0x82
    };
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
}

bool HasExecutableShaderCompilerForTests()
{
    const auto tryPath =
        [](const std::filesystem::path& path)
    {
        if (path.empty())
            return false;
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error)
            return false;
#if defined(_WIN32)
        return true;
#else
        return access(path.string().c_str(), X_OK) == 0;
#endif
    };

    if (const char* dxcPath = std::getenv("DXC_PATH"); dxcPath != nullptr && *dxcPath != '\0')
    {
        if (tryPath(std::filesystem::path(dxcPath)))
            return true;
    }
    if (const char* vulkanSdk = std::getenv("VULKAN_SDK"); vulkanSdk != nullptr && *vulkanSdk != '\0')
    {
        if (tryPath(std::filesystem::path(vulkanSdk) / "bin" / "dxc") ||
            tryPath(std::filesystem::path(vulkanSdk) / "Bin" / "dxc"))
        {
            return true;
        }
    }
    if (const char* vkSdkPath = std::getenv("VK_SDK_PATH"); vkSdkPath != nullptr && *vkSdkPath != '\0')
    {
        if (tryPath(std::filesystem::path(vkSdkPath) / "bin" / "dxc") ||
            tryPath(std::filesystem::path(vkSdkPath) / "Bin" / "dxc"))
        {
            return true;
        }
    }

    return false;
}

std::string StableArtifactBlobFileName(
    const NLS::Core::Assets::AssetId owner,
    const std::string& subAssetKey)
{
    return NLS::Core::Assets::BuildArtifactStorageFileName(owner.ToString() + ":" + subAssetKey);
}

std::string FileStamp(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return {};

    error.clear();
    const auto writeTime = std::filesystem::last_write_time(path, error);
    if (error)
        return {};

    const auto writeTimeTicks = static_cast<std::intmax_t>(writeTime.time_since_epoch().count());
    return std::to_string(size) + ":" + std::to_string(writeTimeTicks);
}

std::string SourceFileContentHash(const std::filesystem::path& path)
{
    const auto bytes = ReadBinaryFile(path);
    if (bytes.empty() && !std::filesystem::is_regular_file(path))
        return {};
    return NLS::Core::Assets::ComputeNativeArtifactPayloadHash(bytes);
}

std::string ReadArtifactPayloadText(
    const std::filesystem::path& path,
    const NLS::Core::Assets::ArtifactType artifactType,
    const uint32_t schemaVersion)
{
    const auto bytes = ReadBinaryFile(path);
    const auto container = NLS::Core::Assets::ReadNativeArtifactContainer(bytes, artifactType, schemaVersion);
    if (!container.has_value())
        return {};

    return std::string(container->payload.begin(), container->payload.end());
}

NLS::Core::Assets::ImportedArtifact MakeArtifact(
    NLS::Core::Assets::AssetId owner,
    std::string subAssetKey,
    NLS::Core::Assets::ArtifactType type,
    std::string loaderId,
    std::string artifactPath = {},
    std::string contentHash = {},
    std::string targetPlatform = "editor")
{
    if (artifactPath.empty())
        artifactPath = (std::filesystem::path("Library") /
            "Artifacts" /
            NLS::Core::Assets::BuildArtifactStorageRelativePath(StableArtifactBlobFileName(owner, subAssetKey))).generic_string();
    if (contentHash.empty())
        contentHash = "sha256:" + owner.ToString() + ":" + subAssetKey;

    return {
        owner,
        std::move(subAssetKey),
        type,
        std::move(loaderId),
        std::move(targetPlatform),
        std::move(artifactPath),
        std::move(contentHash)
    };
}

void WriteManifestArtifactFiles(
    const std::filesystem::path& root,
    const NLS::Core::Assets::ArtifactManifest& manifest)
{
    for (const auto& artifact : manifest.subAssets)
        WriteTextFile(root / artifact.artifactPath, artifact.subAssetKey);
}

std::string ContentStorageArtifactPath(
    const NLS::Core::Assets::AssetId owner,
    const std::string& subAssetKey)
{
    return (std::filesystem::path("Library") /
        "Artifacts" /
        NLS::Core::Assets::BuildArtifactStorageRelativePath(
            StableArtifactBlobFileName(owner, subAssetKey))).generic_string();
}

std::string TextureArtifactTargetPlatformForTest()
{
#if defined(_WIN32)
    return "win64-dx12";
#else
    return "editor";
#endif
}

void AddCurrentExternalTextureBuildPipelineDependency(NLS::Core::Assets::ArtifactManifest& manifest)
{
    manifest.dependencies.push_back({
        NLS::Core::Assets::AssetDependencyKind::PostprocessorVersion,
        NLS::Editor::Assets::kExternalTextureBuildPipelineDependencyName,
        std::to_string(NLS::Editor::Assets::kExternalTexturePostprocessorVersion)
    });
}

std::string ArtifactTypeToken(const NLS::Core::Assets::ArtifactType type)
{
    using NLS::Core::Assets::ArtifactType;
    switch (type)
    {
    case ArtifactType::Model: return "model";
    case ArtifactType::Mesh: return "mesh";
    case ArtifactType::Material: return "material";
    case ArtifactType::Texture: return "texture";
    case ArtifactType::Shader: return "shader";
    case ArtifactType::Scene: return "scene";
    case ArtifactType::Prefab: return "prefab";
    case ArtifactType::Skeleton: return "skeleton";
    case ArtifactType::Skin: return "skin";
    case ArtifactType::AnimationClip: return "animation";
    case ArtifactType::MorphTarget: return "morph-target";
    case ArtifactType::Audio: return "audio";
    case ArtifactType::Unknown:
    case ArtifactType::Count:
        break;
    }
    return "unknown";
}

std::string DependencyKindToken(const NLS::Core::Assets::AssetDependencyKind kind)
{
    using NLS::Core::Assets::AssetDependencyKind;
    switch (kind)
    {
    case AssetDependencyKind::SourceFileHash: return "source-file-hash";
    case AssetDependencyKind::SourceAssetGuid: return "source-asset-guid";
    case AssetDependencyKind::ImportedArtifact: return "imported-artifact";
    case AssetDependencyKind::PathToGuidMapping: return "path-to-guid-mapping";
    case AssetDependencyKind::BuildTarget: return "build-target";
    case AssetDependencyKind::ImporterVersion: return "importer-version";
    case AssetDependencyKind::PostprocessorVersion: return "postprocessor-version";
    case AssetDependencyKind::PrefabBase: return "prefab-base";
    case AssetDependencyKind::NestedPrefab: return "nested-prefab";
    case AssetDependencyKind::PrefabOverrideTarget: return "prefab-override-target";
    case AssetDependencyKind::RuntimeComponentCapability: return "runtime-component-capability";
    case AssetDependencyKind::RawPackageFile: return "raw-package-file";
    }
    return "source-file-hash";
}

void WritePersistedArtifactManifest(
    const std::filesystem::path& root,
    const NLS::Core::Assets::ArtifactManifest& manifest)
{
    NLS::Core::Assets::ArtifactDatabase database;
    const auto databasePath = root / "Library" / "ArtifactDB";
    if (std::filesystem::exists(databasePath))
        (void)database.Load(databasePath);

    database.UpsertManifest(
        manifest,
        (std::filesystem::path("Assets") / manifest.sourceAssetId.ToString()).generic_string(),
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(database.Save(databasePath));
}

TEST(AssetDatabaseFacadeTests, ArtifactDatabaseRoundTripPreservesSubAssetSourceAssetIds)
{
    using namespace NLS::Core::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto modelAssetId = AssetId(NLS::Guid::Parse("83000000-0000-4000-8000-000000000001"));
    const auto meshAssetId = AssetId(NLS::Guid::Parse("83000000-0000-4000-8000-000000000002"));
    const auto materialAssetId = AssetId(NLS::Guid::Parse("83000000-0000-4000-8000-000000000003"));

    ArtifactManifest manifest;
    manifest.sourceAssetId = modelAssetId;
    manifest.importerId = "AssimpModelImporter";
    manifest.importerVersion = 12u;
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "prefab:Hero";
    manifest.dependencies.push_back({
        AssetDependencyKind::SourceFileHash,
        "Assets/Models/Hero.gltf",
        "stamp"
    });
    manifest.subAssets.push_back(MakeArtifact(
        modelAssetId,
        "prefab:Hero",
        ArtifactType::Prefab,
        "PrefabLoader"));
    manifest.subAssets.push_back(MakeArtifact(
        meshAssetId,
        "mesh:/0/primitive/0",
        ArtifactType::Mesh,
        "MeshLoader"));
    manifest.subAssets.push_back(MakeArtifact(
        materialAssetId,
        "material:/0",
        ArtifactType::Material,
        "MaterialLoader"));

    WritePersistedArtifactManifest(root, manifest);

    ArtifactDatabase loaded;
    ASSERT_TRUE(loaded.Load(root / "Library" / "ArtifactDB")) << loaded.GetLastError();
    const auto rebuilt = loaded.BuildManifestForSource(modelAssetId, "editor");
    ASSERT_TRUE(rebuilt.has_value());

    const auto* prefab = rebuilt->FindSubAsset("prefab:Hero");
    const auto* mesh = rebuilt->FindSubAsset("mesh:/0/primitive/0");
    const auto* material = rebuilt->FindSubAsset("material:/0");
    ASSERT_NE(prefab, nullptr);
    ASSERT_NE(mesh, nullptr);
    ASSERT_NE(material, nullptr);
    EXPECT_TRUE(prefab->sourceAssetId == modelAssetId);
    EXPECT_TRUE(mesh->sourceAssetId == meshAssetId);
    EXPECT_TRUE(material->sourceAssetId == materialAssetId);

    std::filesystem::remove_all(root);
}

void AddCurrentSourceDependencies(
    const std::filesystem::path& root,
    NLS::Core::Assets::ArtifactManifest& manifest,
    const std::string& assetPath)
{
    const auto hasTextureArtifact = std::any_of(
        manifest.subAssets.begin(),
        manifest.subAssets.end(),
        [](const NLS::Core::Assets::ImportedArtifact& artifact)
        {
            return artifact.artifactType == NLS::Core::Assets::ArtifactType::Texture;
        });
    const auto sourcePath = root / std::filesystem::path(assetPath);
    manifest.dependencies.push_back({
        NLS::Core::Assets::AssetDependencyKind::SourceFileHash,
        assetPath,
        SourceFileContentHash(sourcePath)
    });
    manifest.dependencies.push_back({
        NLS::Core::Assets::AssetDependencyKind::PathToGuidMapping,
        assetPath + ".meta",
        FileStamp(NLS::Core::Assets::GetAssetMetaPath(sourcePath))
    });
    manifest.dependencies.push_back({
        NLS::Core::Assets::AssetDependencyKind::ImporterVersion,
        manifest.importerId,
        std::to_string(manifest.importerVersion)
    });
    manifest.dependencies.push_back({
        NLS::Core::Assets::AssetDependencyKind::BuildTarget,
        manifest.targetPlatform,
        manifest.targetPlatform
    });
    if (hasTextureArtifact)
    {
        manifest.dependencies.push_back({
            NLS::Core::Assets::AssetDependencyKind::PostprocessorVersion,
            NLS::Editor::Assets::kExternalTextureBuildPipelineDependencyName,
            std::to_string(NLS::Editor::Assets::kExternalTexturePostprocessorVersion)
        });
    }
}

void RegisterCurrentMaterialManifestForMetaFallbackTest(
    NLS::Editor::Assets::AssetDatabaseFacade& database,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& sourcePath,
    const std::string& assetPath)
{
    using namespace NLS::Core::Assets;

    const auto assetId = AssetId(NLS::Guid::Parse(database.AssetPathToGUID(assetPath)));
    ASSERT_TRUE(assetId.IsValid());

    ArtifactManifest manifest;
    manifest.sourceAssetId = assetId;
    manifest.importerId = "material";
    manifest.importerVersion = GetCurrentImporterVersion(AssetType::Material);
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "material:main";
    manifest.subAssets.push_back(MakeArtifact(
        assetId,
        manifest.primarySubAssetKey,
        ArtifactType::Material,
        "material"));

    const auto metaPath = GetAssetMetaPath(sourcePath);
    const auto metaStamp = FileStamp(metaPath);
    manifest.dependencies = {
        {AssetDependencyKind::SourceFileHash, assetPath, SourceFileContentHash(sourcePath)},
        {
            AssetDependencyKind::PathToGuidMapping,
            assetPath + ".meta",
            metaStamp.empty() ? "guid:" + assetId.ToString() : metaStamp
        },
        {AssetDependencyKind::ImporterVersion, manifest.importerId, std::to_string(manifest.importerVersion)},
        {AssetDependencyKind::BuildTarget, manifest.targetPlatform, manifest.targetPlatform}
    };

    WriteManifestArtifactFiles(projectRoot, manifest);
    database.AddArtifactManifest(std::move(manifest));
}

NLS::Core::Assets::AssetId ParseAssetId(const std::string& guid)
{
    return NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));
}

NLS::Render::Assets::ShaderArtifactStage MakeFreshnessOnlyShaderStageForFacadeTests(
    const NLS::Render::ShaderCompiler::ShaderTargetPlatform targetPlatform)
{
    NLS::Render::Assets::ShaderArtifactStage stage;
    stage.stage = NLS::Render::ShaderCompiler::ShaderStage::Vertex;
    stage.targetPlatform = targetPlatform;
    stage.entryPoint = "VSMain";
    stage.targetProfile = targetPlatform == NLS::Render::ShaderCompiler::ShaderTargetPlatform::GLSL
        ? "glsl-450"
        : "vs_6_0";
    stage.output.status = NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded;
    // These deterministic placeholder bytes are serialized for freshness tests and never reach a rendering backend.
    stage.output.bytecode = {0x4eu, 0x4cu, 0x53u, static_cast<uint8_t>(targetPlatform)};
    return stage;
}

void RegisterStandardPbrFreshnessOnlyDependency(
    NLS::Editor::Assets::AssetDatabaseFacade& database,
    const std::filesystem::path& projectRoot)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    constexpr const char* standardPbrAssetPath =
        "Assets/Engine/Shaders/ShaderLab/StandardPBR.shader";
    const auto roots = MakeProjectEditorAssetRoots(projectRoot);
    const auto shaderRoot = std::find_if(
        roots.begin(),
        roots.end(),
        [](const EditorAssetRoot& candidate)
        {
            return candidate.readOnly &&
                candidate.mountPath == std::filesystem::path("Assets/Engine/Shaders");
        });
    ASSERT_NE(shaderRoot, roots.end());

    const auto shaderSourcePath = shaderRoot->path / "ShaderLab" / "StandardPBR.shader";
    ASSERT_TRUE(std::filesystem::is_regular_file(shaderSourcePath));

    const auto shaderId = ParseAssetId(
        database.AssetPathToGUID(standardPbrAssetPath));
    ASSERT_TRUE(shaderId.IsValid());

    NLS::Render::Assets::ShaderArtifact shaderArtifact;
    shaderArtifact.sourcePath = standardPbrAssetPath;
    shaderArtifact.subAssetKey = "shader:StandardPBR/Forward#0";
    shaderArtifact.stages = {
        MakeFreshnessOnlyShaderStageForFacadeTests(NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL),
        MakeFreshnessOnlyShaderStageForFacadeTests(NLS::Render::ShaderCompiler::ShaderTargetPlatform::SPIRV),
        MakeFreshnessOnlyShaderStageForFacadeTests(NLS::Render::ShaderCompiler::ShaderTargetPlatform::GLSL)
    };

    ArtifactWriteRequest request;
    request.sourceAssetId = shaderId;
    request.importerId = "shader";
    request.importerVersion = GetCurrentImporterVersion(AssetType::Shader);
    request.targetPlatform = "editor";
    request.primarySubAssetKey = shaderArtifact.subAssetKey;
    request.artifacts.push_back({
        shaderArtifact.subAssetKey,
        ArtifactType::Shader,
        "shader",
        "StandardPBR Forward",
        "shader",
        NLS::Render::Assets::SerializeShaderArtifact(shaderArtifact)
    });

    const auto shaderMetaPath = GetAssetMetaPath(shaderSourcePath);
    const auto metaStamp = FileStamp(shaderMetaPath);
    const auto sourceContentHash = SourceFileContentHash(shaderSourcePath);
    ASSERT_FALSE(sourceContentHash.empty());
    request.dependencies = {
        {AssetDependencyKind::SourceFileHash, standardPbrAssetPath, sourceContentHash},
        {
            AssetDependencyKind::PathToGuidMapping,
            std::string(standardPbrAssetPath) + ".meta",
            metaStamp.empty() ? "guid:" + shaderId.ToString() : metaStamp
        },
        {AssetDependencyKind::ImporterVersion, request.importerId, std::to_string(request.importerVersion)},
        {AssetDependencyKind::BuildTarget, request.targetPlatform, request.targetPlatform},
        {
            AssetDependencyKind::PostprocessorVersion,
            "shader-compiler-toolchain",
            NLS::Render::ShaderCompiler::BuildShaderCompilerToolchainDependencyFingerprint()
        }
    };

    ArtifactWriter writer(
        projectRoot / "Library" / "ArtifactStaging" / shaderId.ToString(),
        projectRoot / "Library" / "Artifacts");
    const auto writeResult = writer.WriteAndCommit(request, nullptr);
    ASSERT_TRUE(writeResult.committed);
    ASSERT_TRUE(writeResult.diagnostics.empty());
    database.AddArtifactManifest(writeResult.manifest);

    AssetDatabaseFacade reloadedDatabase(roots);
    ASSERT_TRUE(reloadedDatabase.Refresh());
    const auto persistedManifest = reloadedDatabase.GetArtifactManifestForAssetPath(standardPbrAssetPath);
    ASSERT_TRUE(persistedManifest.has_value());
    ASSERT_EQ(persistedManifest->primarySubAssetKey, shaderArtifact.subAssetKey);
    const auto artifactPath = reloadedDatabase.ResolveArtifactPathAtPath(
        standardPbrAssetPath,
        shaderArtifact.subAssetKey);
    ASSERT_TRUE(std::filesystem::is_regular_file(artifactPath));
    EXPECT_TRUE(IsArtifactStorageFileName(artifactPath.filename().generic_string()));
    const auto persistedArtifact = NLS::Render::Assets::LoadShaderArtifact(artifactPath);
    ASSERT_TRUE(persistedArtifact.has_value());
    ASSERT_EQ(persistedArtifact->stages.size(), shaderArtifact.stages.size());
    EXPECT_EQ(persistedArtifact->sourcePath, standardPbrAssetPath);
    EXPECT_EQ(persistedArtifact->subAssetKey, shaderArtifact.subAssetKey);
    ASSERT_TRUE(reloadedDatabase.IsArtifactManifestCurrentForAssetPath(standardPbrAssetPath));
}

template <typename T>
NLS::Engine::Serialize::PPtr<T> MakePPtr(const NLS::Engine::Serialize::ObjectIdentifier& identifier)
{
    return NLS::Engine::Serialize::PPtr<T>(
        NLS::Engine::Serialize::PersistentManager::Instance().ObjectIdentifierToInstanceID(identifier));
}

const NLS::Engine::Assets::RuntimeAssetPack* FindPack(
    const NLS::Engine::Assets::RuntimeAssetManifest& manifest,
    const std::string& packName,
    const std::string& packVariant)
{
    const auto found = std::find_if(
        manifest.assetPacks.begin(),
        manifest.assetPacks.end(),
        [&packName, &packVariant](const NLS::Engine::Assets::RuntimeAssetPack& pack)
        {
            return pack.packName == packName && pack.packVariant == packVariant;
        });
    return found != manifest.assetPacks.end() ? &(*found) : nullptr;
}

const NLS::Engine::Assets::RuntimeAssetPackEntry* FindPackEntry(
    const NLS::Engine::Assets::RuntimeAssetPack& pack,
    NLS::Core::Assets::AssetId assetId,
    const std::string& subAssetKey)
{
    const auto found = std::find_if(
        pack.entries.begin(),
        pack.entries.end(),
        [&assetId, &subAssetKey](const NLS::Engine::Assets::RuntimeAssetPackEntry& entry)
        {
            return entry.reference.assetId == assetId && entry.reference.subAssetKey == subAssetKey;
        });
    return found != pack.entries.end() ? &(*found) : nullptr;
}

bool ContainsDependency(
    const std::vector<NLS::Engine::Assets::RuntimeAssetRef>& dependencies,
    NLS::Core::Assets::AssetId assetId,
    const std::string& subAssetKey)
{
    return std::any_of(
        dependencies.begin(),
        dependencies.end(),
        [&assetId, &subAssetKey](const NLS::Engine::Assets::RuntimeAssetRef& dependency)
        {
            return dependency.assetId == assetId && dependency.subAssetKey == subAssetKey;
        });
}

bool ContainsAssetDiagnosticCode(
    const NLS::Core::Assets::AssetDiagnostics& diagnostics,
    const std::string& code)
{
    return std::any_of(
        diagnostics.begin(),
        diagnostics.end(),
        [&code](const NLS::Core::Assets::AssetDiagnostic& diagnostic)
        {
            return diagnostic.code == code;
        });
}

bool ContainsManifestDependency(
    const nlohmann::json& manifest,
    const std::string& kind,
    const std::string& value)
{
    const auto dependencies = manifest.find("dependencies");
    if (dependencies == manifest.end() || !dependencies->is_array())
        return false;

    return std::any_of(
        dependencies->begin(),
        dependencies->end(),
        [&kind, &value](const nlohmann::json& dependency)
        {
            return dependency.is_object() &&
                dependency.value("kind", std::string {}) == kind &&
                dependency.value("value", std::string {}) == value &&
                !dependency.value("hashOrVersion", std::string {}).empty();
            });
}

bool ContainsManifestDependency(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const NLS::Core::Assets::AssetDependencyKind kind,
    const std::string& value)
{
    return std::any_of(
        manifest.dependencies.begin(),
        manifest.dependencies.end(),
        [&](const NLS::Core::Assets::AssetDependencyRecord& dependency)
        {
            return dependency.kind == kind &&
                dependency.value == value &&
                !dependency.hashOrVersion.empty();
        });
}

std::optional<NLS::Core::Assets::ArtifactManifest> LoadPersistedArtifactManifest(
    const std::filesystem::path& root,
    const NLS::Core::Assets::AssetId sourceAssetId)
{
    NLS::Core::Assets::ArtifactDatabase database;
    if (!database.Load(root / "Library" / "ArtifactDB"))
        return std::nullopt;
    return database.BuildManifestForSource(sourceAssetId);
}

void RemovePersistedArtifactDependency(
    const std::filesystem::path& root,
    const NLS::Core::Assets::AssetId sourceAssetId,
    const NLS::Core::Assets::AssetDependencyKind kind,
    const std::string& value)
{
    const auto databasePath = root / "Library" / "ArtifactDB";
    NLS::Core::Assets::ArtifactDatabase database;
    ASSERT_TRUE(database.Load(databasePath));
    auto manifest = database.BuildManifestForSource(sourceAssetId);
    ASSERT_TRUE(manifest.has_value());
    manifest->dependencies.erase(
        std::remove_if(
            manifest->dependencies.begin(),
            manifest->dependencies.end(),
            [&](const NLS::Core::Assets::AssetDependencyRecord& dependency)
            {
                return dependency.kind == kind && dependency.value == value;
            }),
        manifest->dependencies.end());
    database.UpsertManifest(
        *manifest,
        (std::filesystem::path("Assets") / sourceAssetId.ToString()).generic_string(),
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(database.Save(databasePath));
}

class TextureReimportTestAdapter final : public NLS::Render::RHI::RHIAdapter
{
public:
    std::string_view GetDebugName() const override { return "TextureReimportTestAdapter"; }
    NLS::Render::RHI::NativeBackendType GetBackendType() const override { return NLS::Render::RHI::NativeBackendType::DX12; }
    std::string_view GetVendor() const override { return "TestVendor"; }
    std::string_view GetHardware() const override { return "TestHardware"; }
};

class TextureReimportTestTexture final : public NLS::Render::RHI::RHITexture
{
public:
    explicit TextureReimportTestTexture(NLS::Render::RHI::RHITextureDesc desc)
        : m_desc(std::move(desc))
    {
    }

    std::string_view GetDebugName() const override { return m_desc.debugName; }
    const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
    NLS::Render::RHI::ResourceState GetState() const override { return NLS::Render::RHI::ResourceState::Unknown; }

private:
    NLS::Render::RHI::RHITextureDesc m_desc {};
};

class TextureReimportTestTextureView final : public NLS::Render::RHI::RHITextureView
{
public:
    TextureReimportTestTextureView(
        std::shared_ptr<NLS::Render::RHI::RHITexture> texture,
        NLS::Render::RHI::RHITextureViewDesc desc)
        : m_texture(std::move(texture))
        , m_desc(std::move(desc))
    {
    }

    std::string_view GetDebugName() const override { return m_desc.debugName; }
    const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
    const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override { return m_texture; }

private:
    std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
    NLS::Render::RHI::RHITextureViewDesc m_desc {};
};

class TextureReimportTestCommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
{
public:
    explicit TextureReimportTestCommandBuffer(std::string debugName)
        : m_debugName(std::move(debugName))
    {
    }

    std::string_view GetDebugName() const override { return m_debugName; }
    void Begin() override { m_recording = true; m_closed = false; }
    void End() override { m_recording = false; m_closed = true; }
    void Reset() override { m_recording = false; m_closed = false; }
    bool IsRecording() const override { return m_recording; }
    bool IsClosedForSubmission() const override { return m_closed; }
    NLS::Render::RHI::NativeHandle GetNativeCommandBuffer() const override { return {}; }
    void BeginRenderPass(const NLS::Render::RHI::RHIRenderPassDesc&) override {}
    void EndRenderPass() override {}
    void SetViewport(const NLS::Render::RHI::RHIViewport&) override {}
    void SetScissor(const NLS::Render::RHI::RHIRect2D&) override {}
    void BindGraphicsPipeline(const std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>&) override {}
    void BindComputePipeline(const std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>&) override {}
    void BindBindingSet(uint32_t, const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>&) override {}
    void PushConstants(NLS::Render::RHI::ShaderStageMask, uint32_t, uint32_t, const void*) override {}
    void BindVertexBuffer(uint32_t, const NLS::Render::RHI::RHIVertexBufferView&) override {}
    void BindIndexBuffer(const NLS::Render::RHI::RHIIndexBufferView&) override {}
    void Draw(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void DrawIndexed(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override {}
    void Dispatch(uint32_t, uint32_t, uint32_t) override {}
    void CopyBuffer(
        const std::shared_ptr<NLS::Render::RHI::RHIBuffer>&,
        const std::shared_ptr<NLS::Render::RHI::RHIBuffer>&,
        const NLS::Render::RHI::RHIBufferCopyRegion&) override {}
    void CopyBufferToTexture(const NLS::Render::RHI::RHIBufferToTextureCopyDesc&) override {}
    void CopyTexture(const NLS::Render::RHI::RHITextureCopyDesc&) override {}
    void Barrier(const NLS::Render::RHI::RHIBarrierDesc&) override {}

private:
    std::string m_debugName;
    bool m_recording = false;
    bool m_closed = false;
};

class TextureReimportTestCommandPool final : public NLS::Render::RHI::RHICommandPool
{
public:
    TextureReimportTestCommandPool(NLS::Render::RHI::QueueType queueType, std::string debugName)
        : m_queueType(queueType)
        , m_debugName(std::move(debugName))
    {
    }

    std::string_view GetDebugName() const override { return m_debugName; }
    NLS::Render::RHI::QueueType GetQueueType() const override { return m_queueType; }
    std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> CreateCommandBuffer(std::string debugName = {}) override
    {
        return std::make_shared<TextureReimportTestCommandBuffer>(std::move(debugName));
    }
    void Reset() override {}

private:
    NLS::Render::RHI::QueueType m_queueType = NLS::Render::RHI::QueueType::Graphics;
    std::string m_debugName;
};

class TextureReimportTestFence final : public NLS::Render::RHI::RHIFence
{
public:
    explicit TextureReimportTestFence(std::string debugName)
        : m_debugName(std::move(debugName))
    {
    }

    std::string_view GetDebugName() const override { return m_debugName; }
    bool IsSignaled() const override { return m_signaled; }
    void Reset() override { m_signaled = false; }
    bool Wait(uint64_t = 0u) override
    {
        m_signaled = true;
        return true;
    }

private:
    std::string m_debugName;
    bool m_signaled = true;
};

class TextureReimportTestSemaphore final : public NLS::Render::RHI::RHISemaphore
{
public:
    explicit TextureReimportTestSemaphore(std::string debugName)
        : m_debugName(std::move(debugName))
    {
    }

    std::string_view GetDebugName() const override { return m_debugName; }
    bool IsSignaled() const override { return false; }
    void Reset() override {}

private:
    std::string m_debugName;
};

class TextureReimportTestDevice final : public NLS::Render::RHI::RHIDevice
{
public:
    TextureReimportTestDevice()
        : m_adapter(std::make_shared<TextureReimportTestAdapter>())
    {
        m_nativeDeviceInfo.backend = NLS::Render::RHI::NativeBackendType::DX12;
        m_capabilities.backendReady = true;
        m_capabilities.supportsGraphics = true;
        m_capabilities.supportsCurrentSceneRenderer = true;
        for (const auto& descriptor : NLS::Render::RHI::kTextureFormatDescriptors)
        {
            m_capabilities.SetTextureFormatCapability(
                descriptor.format,
                {
                    descriptor.format,
                    descriptor.sampled,
                    descriptor.supportsUpload,
                    descriptor.colorAttachment,
                    descriptor.storage,
                    descriptor.supportsSrgbView,
                    descriptor.requiresAlignedTopLevelBlocks,
                    true,
                    {}
                });
        }
    }

    std::string_view GetDebugName() const override { return "TextureReimportTestDevice"; }
    const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_adapter; }
    const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
    NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override { return m_nativeDeviceInfo; }
    bool IsBackendReady() const override { return true; }
    std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(const NLS::Render::RHI::SwapchainDesc&) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(
        const NLS::Render::RHI::RHIBufferDesc&,
        const NLS::Render::RHI::RHIBufferUploadDesc&) override
    {
        return nullptr;
    }
    std::shared_ptr<NLS::Render::RHI::RHITexture> CreateTexture(
        const NLS::Render::RHI::RHITextureDesc& desc,
        const NLS::Render::RHI::RHITextureUploadDesc& uploadDesc) override
    {
        ++textureCreateCalls;
        lastTextureDesc = desc;
        lastTextureUploadDesc = uploadDesc;
        return std::make_shared<TextureReimportTestTexture>(desc);
    }
    std::shared_ptr<NLS::Render::RHI::RHITextureView> CreateTextureView(
        const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
        const NLS::Render::RHI::RHITextureViewDesc& desc) override
    {
        lastTextureViewDesc = desc;
        return std::make_shared<TextureReimportTestTextureView>(texture, desc);
    }
    std::shared_ptr<NLS::Render::RHI::RHISampler> CreateSampler(const NLS::Render::RHI::SamplerDesc&, std::string = {}) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateBindingLayout(const NLS::Render::RHI::RHIBindingLayoutDesc&) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateBindingSet(const NLS::Render::RHI::RHIBindingSetDesc&) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> CreatePipelineLayout(const NLS::Render::RHI::RHIPipelineLayoutDesc&) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHIShaderModule> CreateShaderModule(const NLS::Render::RHI::RHIShaderModuleDesc&) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> CreateGraphicsPipeline(const NLS::Render::RHI::RHIGraphicsPipelineDesc&) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> CreateComputePipeline(const NLS::Render::RHI::RHIComputePipelineDesc&) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHICommandPool> CreateCommandPool(
        NLS::Render::RHI::QueueType queueType,
        std::string debugName = {}) override
    {
        return std::make_shared<TextureReimportTestCommandPool>(queueType, std::move(debugName));
    }
    std::shared_ptr<NLS::Render::RHI::RHIFence> CreateFence(std::string debugName = {}) override
    {
        return std::make_shared<TextureReimportTestFence>(std::move(debugName));
    }
    std::shared_ptr<NLS::Render::RHI::RHISemaphore> CreateSemaphore(std::string debugName = {}) override
    {
        return std::make_shared<TextureReimportTestSemaphore>(std::move(debugName));
    }
    void ReadPixels(
        const std::shared_ptr<NLS::Render::RHI::RHITexture>&,
        uint32_t,
        uint32_t,
        uint32_t,
        uint32_t,
        NLS::Render::Settings::EPixelDataFormat,
        NLS::Render::Settings::EPixelDataType,
        void*) override {}

    size_t textureCreateCalls = 0u;
    NLS::Render::RHI::RHITextureDesc lastTextureDesc {};
    NLS::Render::RHI::RHITextureUploadDesc lastTextureUploadDesc {};
    NLS::Render::RHI::RHITextureViewDesc lastTextureViewDesc {};

private:
    std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
    NLS::Render::RHI::NativeRenderDeviceInfo m_nativeDeviceInfo {};
    NLS::Render::RHI::RHIDeviceCapabilities m_capabilities {};
};

class ScopedAssetDatabaseFacadeDriverService final
{
public:
    explicit ScopedAssetDatabaseFacadeDriverService(NLS::Render::Context::Driver& driver)
    {
        NLS::Core::ServiceLocator::Provide(driver);
    }

    ~ScopedAssetDatabaseFacadeDriverService()
    {
        NLS::Core::ServiceLocator::Remove<NLS::Render::Context::Driver>();
    }
};

NLS::Render::Context::Driver& EnsureAssetDatabaseFacadeTestDriver()
{
    static auto driver = std::make_unique<NLS::Render::Context::Driver>([]()
    {
        NLS::Render::Settings::DriverSettings settings;
        settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        return settings;
    }());
    NLS::Core::ServiceLocator::Provide(*driver);
    return *driver;
}
}

TEST(AssetDatabaseFacadeTests, GuidPathAndMainSubAssetQueriesMatchEditorWorkflow)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto guid = database.AssetPathToGUID("Assets/Models/Hero.gltf");
    ASSERT_FALSE(guid.empty());
    EXPECT_EQ(database.GUIDToAssetPath(guid), "Assets/Models/Hero.gltf");
    EXPECT_TRUE(database.AssetPathToGUID("Assets/Missing.gltf").empty());

    const auto modelId = ParseAssetId(guid);
    ArtifactManifest manifest;
    manifest.sourceAssetId = modelId;
    manifest.importerId = "scene-model";
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "model:Hero";
    manifest.subAssets.push_back(MakeArtifact(modelId, "model:Hero", ArtifactType::Model, "model"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "mesh:Body", ArtifactType::Mesh, "mesh"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "material:Body", ArtifactType::Material, "material"));
    database.AddArtifactManifest(manifest);

    const auto mainAsset = database.LoadMainAssetAtPath("Assets/Models/Hero.gltf");
    ASSERT_TRUE(mainAsset.has_value());
    EXPECT_EQ(mainAsset->assetId, modelId);
    EXPECT_EQ(mainAsset->subAssetKey, "model:Hero");
    EXPECT_EQ(mainAsset->artifactType, ArtifactType::Model);
    EXPECT_TRUE(mainAsset->mainAsset);

    const auto allAssets = database.LoadAllAssetsAtPath("Assets/Models/Hero.gltf");
    ASSERT_EQ(allAssets.size(), 3u);
    EXPECT_EQ(allAssets[0].subAssetKey, "model:Hero");
    EXPECT_EQ(allAssets[1].subAssetKey, "mesh:Body");
    EXPECT_EQ(allAssets[2].subAssetKey, "material:Body");

    const auto mesh = database.LoadSubAssetAtPath("Assets/Models/Hero.gltf", "mesh:Body");
    ASSERT_TRUE(mesh.has_value());
    EXPECT_EQ(mesh->artifactType, ArtifactType::Mesh);
    EXPECT_FALSE(database.LoadSubAssetAtPath("Assets/Models/Hero.gltf", "mesh:Missing").has_value());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ManifestQueriesExposeOnlyContentStorageArtifactPayloads)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto modelId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Hero.gltf"));
    ArtifactManifest manifest;
    manifest.sourceAssetId = modelId;
    manifest.importerId = "scene-model";
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "mesh:Body";
    manifest.subAssets.push_back(MakeArtifact(
        modelId,
        "mesh:Body",
        ArtifactType::Mesh,
        "mesh",
        "Library/Artifacts/" + modelId.ToString() + "/meshes/not-a-content-addressed-blob"));
    manifest.subAssets.push_back(MakeArtifact(
        modelId,
        "material:Body",
        ArtifactType::Material,
        "material"));

    database.AddArtifactManifest(manifest);

    const auto allAssets = database.LoadAllAssetsAtPath("Assets/Models/Hero.gltf");
    ASSERT_EQ(allAssets.size(), 1u);
    EXPECT_EQ(allAssets[0].subAssetKey, "material:Body");
    EXPECT_EQ(allAssets[0].artifactType, ArtifactType::Material);
    EXPECT_FALSE(database.LoadSubAssetAtPath("Assets/Models/Hero.gltf", "mesh:Body").has_value());
    EXPECT_TRUE(database.LoadSubAssetAtPath("Assets/Models/Hero.gltf", "material:Body").has_value());

    NLS::Core::Assets::ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB"));
    EXPECT_EQ(artifactDatabase.Find(modelId, "mesh:Body", "editor"), nullptr);
    EXPECT_NE(artifactDatabase.Find(modelId, "material:Body", "editor"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, RefreshScansAllConfiguredAssetRoots)
{
    using namespace NLS::Editor::Assets;

    const auto projectRoot = MakeAssetDatabaseFacadeRoot();
    const auto engineRoot =
        std::filesystem::temp_directory_path() /
        ("nullus_asset_database_engine_root_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(engineRoot / "EngineAssets");
    WriteTextFile(projectRoot / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"}})");
    WriteTextFile(engineRoot / "EngineAssets" / "Materials" / "Default.mat", "material");

    AssetDatabaseFacade database({projectRoot, engineRoot});
    ASSERT_TRUE(database.Refresh());

    EXPECT_FALSE(database.AssetPathToGUID("Assets/Models/Hero.gltf").empty());
    const auto engineMaterialGuid = database.AssetPathToGUID("EngineAssets/Materials/Default.mat");
    ASSERT_FALSE(engineMaterialGuid.empty());
    EXPECT_EQ(database.GUIDToAssetPath(engineMaterialGuid), "EngineAssets/Materials/Default.mat");

    std::filesystem::remove_all(projectRoot);
    std::filesystem::remove_all(engineRoot);
}

TEST(AssetDatabaseFacadeTests, FileOperationsPreserveOrRegenerateMetaIdentity)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Materials" / "Hero.mat", "material");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto originalGuid = database.AssetPathToGUID("Assets/Materials/Hero.mat");
    ASSERT_FALSE(originalGuid.empty());

    ASSERT_TRUE(database.MoveAsset("Assets/Materials/Hero.mat", "Assets/Materials/RenamedHero.mat"));
    EXPECT_EQ(database.AssetPathToGUID("Assets/Materials/RenamedHero.mat"), originalGuid);

    ASSERT_TRUE(database.RenameAsset("Assets/Materials/RenamedHero.mat", "FinalHero.mat"));
    EXPECT_EQ(database.AssetPathToGUID("Assets/Materials/FinalHero.mat"), originalGuid);

    ASSERT_TRUE(database.CopyAsset("Assets/Materials/FinalHero.mat", "Assets/Materials/CopyHero.mat"));
    const auto copyGuid = database.AssetPathToGUID("Assets/Materials/CopyHero.mat");
    ASSERT_FALSE(copyGuid.empty());
    EXPECT_NE(copyGuid, originalGuid);

    ASSERT_TRUE(database.DeleteAsset("Assets/Materials/CopyHero.mat"));
    EXPECT_TRUE(database.AssetPathToGUID("Assets/Materials/CopyHero.mat").empty());
    EXPECT_TRUE(database.GUIDToAssetPath(copyGuid).empty());

    EXPECT_EQ(database.CreateFolder("Assets", "Prefabs"), "Assets/Prefabs");
    EXPECT_TRUE(database.IsValidFolder("Assets/Prefabs"));
    WriteTextFile(root / "Assets" / "Prefabs" / "Lamp.prefab", "{}");
    EXPECT_EQ(database.GenerateUniqueAssetPath("Assets/Prefabs/Lamp.prefab"), "Assets/Prefabs/Lamp 1.prefab");

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, FileOperationsRejectPathsOutsideAssetRoots)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto outside = root.parent_path() / ("outside_" + NLS::Guid::New().ToString() + ".mat");
    const auto movedName = "MovedHero_" + NLS::Guid::New().ToString() + ".mat";
    const auto escapedFolder = "Escaped_" + NLS::Guid::New().ToString();
    const auto escapedRename = "EscapedHero_" + NLS::Guid::New().ToString() + ".mat";
    const auto nestedRename = "NestedHero_" + NLS::Guid::New().ToString() + ".mat";
    const auto nestedFolder = "NestedFolder_" + NLS::Guid::New().ToString();
    WriteTextFile(root / "Assets" / "Materials" / "Hero.mat", "material");
    WriteTextFile(outside, "outside");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    EXPECT_FALSE(database.CopyAsset("../" + outside.filename().generic_string(), "Assets/Materials/Stolen.mat"));
    EXPECT_FALSE(database.MoveAsset("Assets/Materials/Hero.mat", "../" + movedName));
    EXPECT_FALSE(database.DeleteAsset("../" + outside.filename().generic_string()));
    EXPECT_FALSE(database.DeleteAsset(""));
    EXPECT_FALSE(database.DeleteAsset("."));
    EXPECT_EQ(database.CreateFolder("..", escapedFolder), "");
    EXPECT_FALSE(database.RenameAsset("Assets/Materials/Hero.mat", "../" + escapedRename));
    EXPECT_FALSE(database.RenameAsset("Assets/Materials/Hero.mat", "Nested/" + nestedRename));
    EXPECT_EQ(database.CreateFolder("Assets", "../" + escapedFolder), "");
    EXPECT_EQ(database.CreateFolder("Assets", "Nested/" + nestedFolder), "");
    EXPECT_FALSE(std::filesystem::exists(root.parent_path() / movedName));
    EXPECT_FALSE(std::filesystem::exists(root.parent_path() / escapedFolder));
    EXPECT_FALSE(std::filesystem::exists(root / "Assets" / escapedRename));
    EXPECT_FALSE(std::filesystem::exists(root / "Assets" / "Materials" / "Nested" / nestedRename));
    EXPECT_FALSE(std::filesystem::exists(root / escapedFolder));
    EXPECT_FALSE(std::filesystem::exists(root / "Assets" / "Nested" / nestedFolder));
    EXPECT_TRUE(std::filesystem::exists(outside));
    EXPECT_TRUE(std::filesystem::exists(root));
    EXPECT_FALSE(database.AssetPathToGUID("Assets/Materials/Hero.mat").empty());

    std::filesystem::remove_all(outside);
    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, FileOperationsRejectReadOnlyRootsAndPathAliases)
{
    using namespace NLS::Editor::Assets;

    const auto projectRoot = MakeAssetDatabaseFacadeRoot();
    const auto packageRoot =
        std::filesystem::temp_directory_path() /
        ("nullus_asset_database_readonly_root_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(packageRoot / "Assets" / "Shared");
    std::filesystem::create_directories(packageRoot / "Packages" / "Starter");
    WriteTextFile(projectRoot / "Assets" / "Shared" / "Hero.mat", "project");
    WriteTextFile(packageRoot / "Assets" / "Shared" / "Hero.mat", "package");
    WriteTextFile(packageRoot / "Packages" / "Starter" / "ReadOnly.mat", "readonly");

    AssetDatabaseFacade database({
        {projectRoot, false},
        {packageRoot, true}
    });
    EXPECT_FALSE(database.Refresh());
    EXPECT_TRUE(ContainsAssetDiagnosticCode(database.GetDiagnostics(), "assetdatabase-editor-path-alias"));

    EXPECT_FALSE(database.DeleteAsset("Packages/Starter/ReadOnly.mat"));
    EXPECT_FALSE(database.CreateTextAsset("new", "Packages/Starter/NewReadonly.mat"));
    EXPECT_TRUE(std::filesystem::exists(packageRoot / "Packages" / "Starter" / "ReadOnly.mat"));
    EXPECT_FALSE(std::filesystem::exists(packageRoot / "Packages" / "Starter" / "NewReadonly.mat"));

    std::filesystem::remove_all(projectRoot);
    std::filesystem::remove_all(packageRoot);
}

TEST(AssetDatabaseFacadeTests, MetadataOperationsRejectNestedReadOnlyRoots)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    std::filesystem::create_directories(root / "Packages" / "Starter");
    WriteTextFile(root / "Packages" / "Starter" / "ReadOnly.mat", "readonly");

    AssetDatabaseFacade database({
        {root, false, {}},
        {root / "Packages", true, "Packages"}
    });
    ASSERT_TRUE(database.Refresh());

    EXPECT_FALSE(database.DeleteAsset("Packages/Starter/ReadOnly.mat"));
    EXPECT_FALSE(database.SetLabels("Packages/Starter/ReadOnly.mat", {"locked"}));
    EXPECT_FALSE(database.SetAssetPackNameAndVariant("Packages/Starter/ReadOnly.mat", "locked", ""));
    EXPECT_FALSE(database.CreateTextAsset("new", "Packages/Starter/New.mat"));
    EXPECT_TRUE(std::filesystem::exists(root / "Packages" / "Starter" / "ReadOnly.mat"));
    EXPECT_FALSE(std::filesystem::exists(root / "Packages" / "Starter" / "New.mat"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, EmptyOrFilesystemRootConfiguredRootsAreRejected)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Materials" / "Hero.mat", "material");

    AssetDatabaseFacade database({
        {{}, false, {}},
        {root.root_path(), false, {}},
        {root, false, {}}
    });
    ASSERT_TRUE(database.Refresh());

    EXPECT_FALSE(database.DeleteAsset(""));
    EXPECT_FALSE(database.DeleteAsset("."));
    EXPECT_TRUE(std::filesystem::exists(root));
    EXPECT_FALSE(database.AssetPathToGUID("Assets/Materials/Hero.mat").empty());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, FileOperationsRejectSymlinkEscapesWhenSupported)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto outside =
        std::filesystem::temp_directory_path() /
        ("nullus_asset_database_symlink_outside_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root / "Assets");
    std::filesystem::create_directories(outside);
    WriteTextFile(outside / "Outside.mat", "outside");

    std::error_code error;
    std::filesystem::create_directory_symlink(outside, root / "Assets" / "LinkedOutside", error);
    if (error)
    {
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(outside);
        GTEST_SKIP() << "Directory symlink creation is not available in this environment.";
    }

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    EXPECT_FALSE(database.DeleteAsset("Assets/LinkedOutside/Outside.mat"));
    EXPECT_FALSE(database.CreateTextAsset("new", "Assets/LinkedOutside/New.mat"));
    EXPECT_TRUE(std::filesystem::exists(outside / "Outside.mat"));
    EXPECT_FALSE(std::filesystem::exists(outside / "New.mat"));

    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside);
}

TEST(AssetDatabaseFacadeTests, FileOperationsCreateNewAssetsInMatchingNonPrimaryRoot)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto projectRoot = MakeAssetDatabaseFacadeRoot();
    const auto engineRoot =
        std::filesystem::temp_directory_path() /
        ("nullus_asset_database_engine_write_root_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(engineRoot / "EngineAssets");

    AssetDatabaseFacade database({projectRoot, engineRoot});
    ASSERT_TRUE(database.Refresh());

    const auto assetId = ParseAssetId("e2020202-0202-4202-8202-020202020202");
    ASSERT_TRUE(database.CreateTextAsset("generated", "EngineAssets/Generated/Tool.asset", assetId));

    EXPECT_TRUE(std::filesystem::exists(engineRoot / "EngineAssets" / "Generated" / "Tool.asset"));
    EXPECT_FALSE(std::filesystem::exists(projectRoot / "EngineAssets" / "Generated" / "Tool.asset"));
    EXPECT_EQ(database.AssetPathToGUID("EngineAssets/Generated/Tool.asset"), assetId.ToString());

    WriteTextFile(projectRoot / "Assets" / "Materials" / "Hero.mat", "material");
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.CopyAsset("Assets/Materials/Hero.mat", "EngineAssets/Materials/HeroCopy.mat"));

    EXPECT_TRUE(std::filesystem::exists(engineRoot / "EngineAssets" / "Materials" / "HeroCopy.mat"));
    EXPECT_FALSE(std::filesystem::exists(projectRoot / "EngineAssets" / "Materials" / "HeroCopy.mat"));

    std::filesystem::remove_all(projectRoot);
    std::filesystem::remove_all(engineRoot);
}

TEST(AssetDatabaseFacadeTests, RefreshAndImportBatchingQueueWorkUntilStopAssetEditing)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Textures" / "Existing.png", "png");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    RegisterStandardPbrFreshnessOnlyDependency(database, root);
    ASSERT_FALSE(database.AssetPathToGUID("Assets/Textures/Existing.png").empty());

    database.StartAssetEditing();
    WriteTextFile(
        root / "Assets" / "Models" / "Queued.obj",
        R"(
o Queued
v 0 0 0
v 1 0 0
v 0 1 0
f 1 2 3
)");
    EXPECT_TRUE(database.ImportAsset("Assets/Models/Queued.obj"));
    EXPECT_EQ(database.GetQueuedImportCount(), 1u);
    EXPECT_EQ(database.GetCompletedImportCount(), 0u);
    EXPECT_TRUE(database.AssetPathToGUID("Assets/Models/Queued.obj").empty());

    EXPECT_TRUE(database.StopAssetEditing());
    EXPECT_EQ(database.GetQueuedImportCount(), 0u);
    EXPECT_EQ(database.GetCompletedImportCount(), 1u);
    EXPECT_FALSE(database.AssetPathToGUID("Assets/Models/Queued.obj").empty());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ImportModelSceneWritesInternalArtifactsAndGeneratedPrefabSubAsset)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "Hero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
            "buffers": [
                {
                    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA",
                    "byteLength": 42
                }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "materials": [
                {
                    "name": "HeroMaterial",
                    "pbrMetallicRoughness": {
                        "baseColorFactor": [0.8, 0.7, 0.6, 1.0],
                        "metallicFactor": 0.25,
                        "roughnessFactor": 0.5
                    }
                }
            ],
            "meshes": [
                {
                    "name": "Body",
                    "primitives": [
                        {
                            "attributes": { "POSITION": 0 },
                            "indices": 1,
                            "material": 0
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "HeroRoot", "mesh": 0 }
            ]
        })");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    RegisterStandardPbrFreshnessOnlyDependency(database, root);
    ASSERT_TRUE(database.ImportAsset("Assets/Models/Hero.gltf"));

    const auto allAssets = database.LoadAllAssetsAtPath("Assets/Models/Hero.gltf");
    ASSERT_EQ(allAssets.size(), 3u);

    const auto hasSubAsset = [&allAssets](const std::string& key, ArtifactType type)
    {
        return std::any_of(
            allAssets.begin(),
            allAssets.end(),
            [&key, type](const AssetDatabaseRecord& record)
            {
                return record.subAssetKey == key && record.artifactType == type;
            });
    };

    EXPECT_TRUE(hasSubAsset("material:material/0", ArtifactType::Material));
    EXPECT_TRUE(hasSubAsset("mesh:mesh/0", ArtifactType::Mesh));
    EXPECT_TRUE(hasSubAsset("prefab:Hero", ArtifactType::Prefab));

    const auto mainAsset = database.LoadMainAssetAtPath("Assets/Models/Hero.gltf");
    ASSERT_TRUE(mainAsset.has_value());
    EXPECT_EQ(mainAsset->subAssetKey, "prefab:Hero");
    EXPECT_EQ(mainAsset->artifactType, ArtifactType::Prefab);

    const auto prefabRecord = database.LoadSubAssetAtPath("Assets/Models/Hero.gltf", "prefab:Hero");
    ASSERT_TRUE(prefabRecord.has_value());
    EXPECT_TRUE(std::filesystem::exists(prefabRecord->artifactPath));
    EXPECT_FALSE(std::filesystem::path(prefabRecord->artifactPath).filename().has_extension());
    const auto prefabContainer = ReadNativeArtifactContainer(
        ReadBinaryFile(prefabRecord->artifactPath),
        ArtifactType::Prefab,
        1u);
    ASSERT_TRUE(prefabContainer.has_value());
    const auto validationProof = NLS::Engine::Assets::FindPrefabValidationProofFingerprint(
        prefabContainer->metadata.dependencies,
        "prefab:Hero");
    EXPECT_FALSE(validationProof.empty())
        << "Generated model prefab containers should carry importer validation proof metadata for safe fast loads.";
    const auto meshRecordAsset = database.LoadSubAssetAtPath("Assets/Models/Hero.gltf", "mesh:mesh/0");
    ASSERT_TRUE(meshRecordAsset.has_value());
    const auto meshArtifactPath = std::filesystem::path(meshRecordAsset->artifactPath);
    EXPECT_TRUE(std::filesystem::exists(meshArtifactPath));
    EXPECT_FALSE(meshArtifactPath.filename().has_extension());
    const auto materialRecordAsset = database.LoadSubAssetAtPath("Assets/Models/Hero.gltf", "material:material/0");
    ASSERT_TRUE(materialRecordAsset.has_value());
    EXPECT_TRUE(std::filesystem::exists(materialRecordAsset->artifactPath));
    EXPECT_FALSE(std::filesystem::path(materialRecordAsset->artifactPath).filename().has_extension());

    const auto meshArtifact = NLS::Render::Assets::LoadMeshArtifact(meshArtifactPath);
    ASSERT_TRUE(meshArtifact.has_value());
    EXPECT_EQ(meshArtifact->vertices.size(), 3u);
    EXPECT_EQ(meshArtifact->indices.size(), 3u);
    EXPECT_EQ(meshArtifact->materialIndex, 0u);
    EXPECT_FLOAT_EQ(meshArtifact->vertices[1].position[0], 1.0f);
    EXPECT_FLOAT_EQ(meshArtifact->vertices[2].position[1], 1.0f);

    NLS::Core::Assets::ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB"));
    const auto sourceId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Hero.gltf"));
    const auto* meshRecord = artifactDatabase.Find(sourceId, "mesh:mesh/0", "editor");
    ASSERT_NE(meshRecord, nullptr);
    EXPECT_EQ(meshRecord->sourcePath, "Assets/Models/Hero.gltf");
    EXPECT_EQ(meshRecord->artifactPath, std::filesystem::path(meshRecordAsset->artifactPath).lexically_relative(root).generic_string());
    EXPECT_EQ(meshRecord->loaderId, "mesh");
    EXPECT_EQ(meshRecord->status, NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    EXPECT_EQ(artifactDatabase.FindBySource(sourceId).size(), allAssets.size());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ProjectLibraryArtifactDatabaseStoresModelMaterialAndTexturePathsRelativeToProject)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "materials": [
                { "name": "Body" }
            ],
            "meshes": [
                {
                    "name": "HeroMesh",
                    "primitives": [
                        { "attributes": {}, "material": 0 }
                    ]
                }
            ],
            "nodes": [
                { "name": "HeroRoot", "mesh": 0 }
            ]
        })");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    RegisterStandardPbrFreshnessOnlyDependency(database, root);
    ASSERT_TRUE(database.ImportAsset("Assets/Models/Hero.gltf"));

    ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB"));
    const auto sourceId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Hero.gltf"));
    const auto records = artifactDatabase.FindBySource(sourceId);
    ASSERT_FALSE(records.empty());

    bool sawMaterial = false;
    bool sawMesh = false;
    bool sawPrefab = false;
    for (const auto* record : records)
    {
        ASSERT_NE(record, nullptr);
        EXPECT_FALSE(std::filesystem::path(record->artifactPath).is_absolute()) << record->artifactPath;
        EXPECT_TRUE(IsContentStorageArtifactPath(record->artifactPath)) << record->artifactPath;
        EXPECT_EQ(record->artifactPath.find("Library/Artifacts/"), 0u) << record->artifactPath;
        const auto blobName = std::filesystem::path(record->artifactPath).filename().generic_string();
        EXPECT_TRUE(IsArtifactStorageFileName(blobName)) << record->artifactPath;
        EXPECT_EQ(std::filesystem::path(record->artifactPath).parent_path().filename().generic_string(), blobName.substr(0u, 2u))
            << record->artifactPath;
        EXPECT_EQ(record->artifactPath.find('\\'), std::string::npos) << record->artifactPath;

        sawMaterial = sawMaterial || record->artifactType == ArtifactType::Material;
        sawMesh = sawMesh || record->artifactType == ArtifactType::Mesh;
        sawPrefab = sawPrefab || record->artifactType == ArtifactType::Prefab;
    }

    EXPECT_TRUE(sawMaterial);
    EXPECT_TRUE(sawMesh);
    EXPECT_TRUE(sawPrefab);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ImportedModelMaterialReferencesAuthoritativeShaderLabSource)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Engine" / "Shaders" / "ShaderLab" / "StandardPBR.shader",
        R"(
Shader "Nullus/StandardPBR"
{
    Properties
    {
        _BaseColor("Base Color", Color) = (1, 1, 1, 1)
    }

    SubShader
    {
        Pass
        {
            Name "Forward"
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
            ENDHLSL
        }
    }
}
)");
    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "materials": [{ "name": "Body" }],
            "meshes": [{ "name": "HeroMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "HeroRoot", "mesh": 0 }]
        })");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/Hero.gltf"));

    const auto modelId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Hero.gltf"));
    const auto modelManifest = database.GetArtifactManifestForAssetPath("Assets/Models/Hero.gltf");
    ASSERT_TRUE(modelManifest.has_value());
    const auto* materialArtifact = modelManifest->FindSubAsset("material:material/0");
    ASSERT_NE(materialArtifact, nullptr);

    const auto materialPayload = ReadArtifactPayloadText(
        root / materialArtifact->artifactPath,
        NLS::Core::Assets::ArtifactType::Material,
        1u);
    const auto shaderId = database.AssetPathToGUID("Assets/Engine/Shaders/ShaderLab/StandardPBR.shader");
    ASSERT_FALSE(shaderId.empty());
    EXPECT_NE(
        materialPayload.find("shader=Assets/Engine/Shaders/ShaderLab/StandardPBR.shader"),
        std::string::npos);
    EXPECT_EQ(materialPayload.find("shader=Library/Artifacts/"), std::string::npos);
    EXPECT_EQ(materialPayload.find(":Shaders/StandardPBR.hlsl"), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, PathConstructorMountsBuiltInShaderRootForAssetsOnlyProjectRoots)
{
    if (!HasExecutableShaderCompilerForTests())
        GTEST_SKIP() << "Built-in ShaderLab artifact import requires an executable DXC compiler.";

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    EXPECT_TRUE(database.ImportAsset("Assets/Engine/Shaders/ShaderLab/StandardPBR.shader"))
        << "Project-root path construction must mount the read-only built-in shader root even "
           "when lightweight test projects do not have a .nullus file.";
    EXPECT_TRUE(database.GetArtifactManifestForAssetPath(
        "Assets/Engine/Shaders/ShaderLab/StandardPBR.shader").has_value());
    EXPECT_FALSE(std::filesystem::exists(root / "Assets" / "Engine"))
        << "Built-in ShaderLab source must stay mounted read-only instead of being copied into project Assets.";

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ImportShaderSourceWritesShaderArtifactManifestAndCentralIndex)
{
    if (!HasExecutableShaderCompilerForTests())
        GTEST_SKIP() << "Shader artifact import success requires an executable DXC compiler.";

    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Shaders" / "HeroSurface.shader",
        R"(Shader "Tests/HeroSurface"
{
    SubShader
    {
        Pass
        {
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
            ENDHLSL
        }
    }
})");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Shaders/HeroSurface.shader"));

    const auto sourceId = ParseAssetId(database.AssetPathToGUID("Assets/Shaders/HeroSurface.shader"));
    ASSERT_TRUE(sourceId.IsValid());

    const auto mainAsset = database.LoadMainAssetAtPath("Assets/Shaders/HeroSurface.shader");
    ASSERT_TRUE(mainAsset.has_value());
    EXPECT_EQ(mainAsset->subAssetKey, "shader:HeroSurface");
    EXPECT_EQ(mainAsset->artifactType, ArtifactType::Shader);
    EXPECT_TRUE(mainAsset->mainAsset);
    EXPECT_TRUE(std::filesystem::exists(mainAsset->artifactPath));
    EXPECT_FALSE(std::filesystem::path(mainAsset->artifactPath).filename().has_extension());

    const auto artifactPayload = ReadTextFile(mainAsset->artifactPath);
    ASSERT_FALSE(artifactPayload.empty());
    EXPECT_NE(artifactPayload.find("NULLUS_IMPORTED_SHADER_ARTIFACT=1"), std::string::npos);
    EXPECT_NE(artifactPayload.find("SOURCE=Assets/Shaders/HeroSurface.shader"), std::string::npos);
    EXPECT_NE(artifactPayload.find("SUB_ASSET=shader:HeroSurface"), std::string::npos);
    EXPECT_NE(artifactPayload.find("ENTRY=VSMain"), std::string::npos);
    EXPECT_NE(artifactPayload.find("ENTRY=PSMain"), std::string::npos);
    EXPECT_NE(artifactPayload.find("TARGET=GLSL"), std::string::npos);
    EXPECT_NE(artifactPayload.find("PROFILE=glsl_430"), std::string::npos);

    const auto shaderArtifact = NLS::Render::Assets::LoadShaderArtifact(mainAsset->artifactPath);
    ASSERT_TRUE(shaderArtifact.has_value());
    EXPECT_TRUE(std::any_of(
        shaderArtifact->stages.begin(),
        shaderArtifact->stages.end(),
        [](const NLS::Render::Assets::ShaderArtifactStage& stage)
        {
            return stage.stage == NLS::Render::ShaderCompiler::ShaderStage::Vertex &&
                stage.targetPlatform == NLS::Render::ShaderCompiler::ShaderTargetPlatform::GLSL &&
                stage.output.status == NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded &&
                !stage.output.bytecode.empty();
        }));
    EXPECT_TRUE(std::any_of(
        shaderArtifact->stages.begin(),
        shaderArtifact->stages.end(),
        [](const NLS::Render::Assets::ShaderArtifactStage& stage)
        {
            return stage.stage == NLS::Render::ShaderCompiler::ShaderStage::Pixel &&
                stage.targetPlatform == NLS::Render::ShaderCompiler::ShaderTargetPlatform::GLSL &&
                stage.output.status == NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded &&
                !stage.output.bytecode.empty();
        }));

    const auto manifest = database.GetArtifactManifestForAssetPath("Assets/Shaders/HeroSurface.shader");
    ASSERT_TRUE(manifest.has_value());
    EXPECT_EQ(manifest->sourceAssetId, sourceId);
    EXPECT_EQ(manifest->importerId, "shader");
    EXPECT_EQ(manifest->primarySubAssetKey, "shader:HeroSurface");
    ASSERT_NE(manifest->FindSubAsset("shader:HeroSurface"), nullptr);
    EXPECT_EQ(manifest->FindSubAsset("shader:HeroSurface")->artifactPath.find("Library/Artifacts/"), 0u);
    EXPECT_FALSE(std::filesystem::path(manifest->FindSubAsset("shader:HeroSurface")->artifactPath).is_absolute());
    EXPECT_TRUE(std::any_of(
        manifest->dependencies.begin(),
        manifest->dependencies.end(),
        [](const AssetDependencyRecord& dependency)
        {
            return dependency.kind == AssetDependencyKind::BuildTarget &&
                dependency.value == "editor";
        }));
    EXPECT_TRUE(std::any_of(
        manifest->dependencies.begin(),
        manifest->dependencies.end(),
        [](const AssetDependencyRecord& dependency)
        {
            return dependency.kind == AssetDependencyKind::PostprocessorVersion &&
                dependency.value == "shader-compiler-toolchain" &&
                !dependency.hashOrVersion.empty();
        }));

    ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB"));
    const auto* record = artifactDatabase.Find(sourceId, "shader:HeroSurface", "editor");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->sourcePath, "Assets/Shaders/HeroSurface.shader");
    EXPECT_EQ(record->artifactType, ArtifactType::Shader);
    EXPECT_EQ(record->loaderId, "shader");
    EXPECT_EQ(record->artifactPath, std::filesystem::path(mainAsset->artifactPath).lexically_relative(root).generic_string());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ShaderArtifactManifestCurrentRejectsMissingCompilerToolchainDependency)
{
    if (!HasExecutableShaderCompilerForTests())
        GTEST_SKIP() << "Shader artifact import success requires an executable DXC compiler.";

    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Shaders" / "ToolchainFreshness.shader",
        R"(Shader "Tests/ToolchainFreshness"
{
    SubShader
    {
        Pass
        {
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
            ENDHLSL
        }
    }
})");

    AssetDatabaseFacade importer(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(importer.Refresh());
    ASSERT_TRUE(importer.ImportAsset("Assets/Shaders/ToolchainFreshness.shader"));
    ASSERT_TRUE(importer.IsArtifactManifestCurrentForAssetPath("Assets/Shaders/ToolchainFreshness.shader"));

    RemovePersistedArtifactDependency(
        root,
        ParseAssetId(importer.AssetPathToGUID("Assets/Shaders/ToolchainFreshness.shader")),
        NLS::Core::Assets::AssetDependencyKind::PostprocessorVersion,
        "shader-compiler-toolchain");

    AssetDatabaseFacade restarted(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(restarted.Refresh());
    EXPECT_FALSE(restarted.IsArtifactManifestCurrentForAssetPath("Assets/Shaders/ToolchainFreshness.shader"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ShaderLabImportWritesMultiCompileVariantsButNotMaterialFeatures)
{
    if (!HasExecutableShaderCompilerForTests())
        GTEST_SKIP() << "ShaderLab import success requires an executable DXC compiler.";

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Shaders" / "KeywordCombo.shader",
        R"(Shader "Tests/KeywordCombo"
{
    SubShader
    {
        Pass
        {
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            #pragma shader_feature _ALPHATEST_ON
            #pragma multi_compile _ MAIN_LIGHT_SHADOWS
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target
            {
                float4 color = float4(1, 1, 1, 1);
            #if defined(_ALPHATEST_ON)
                color.r = 0.5;
            #endif
            #if defined(MAIN_LIGHT_SHADOWS)
                color.g = 0.25;
            #endif
                return color;
            }
            ENDHLSL
        }
    }
}
)");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Shaders/KeywordCombo.shader"));

    const auto manifest = database.GetArtifactManifestForAssetPath("Assets/Shaders/KeywordCombo.shader");
    ASSERT_TRUE(manifest.has_value());
    const auto* shaderArtifact = manifest->FindPrimaryArtifact();
    ASSERT_NE(shaderArtifact, nullptr);
    const auto artifact = NLS::Render::Assets::LoadShaderArtifact(root / shaderArtifact->artifactPath);
    ASSERT_TRUE(artifact.has_value());

    NLS::Render::ShaderLab::ShaderLabKeywordSet combination;
    combination.Enable("MAIN_LIGHT_SHADOWS");
    const auto multiCompileHash = combination.Hash();
    NLS::Render::ShaderLab::ShaderLabKeywordSet materialFeature;
    materialFeature.Enable("_ALPHATEST_ON");
    const auto materialFeatureHash = materialFeature.Hash();

    const auto hasMultiCompilePixelStage = std::any_of(
        artifact->stages.begin(),
        artifact->stages.end(),
        [multiCompileHash](const NLS::Render::Assets::ShaderArtifactStage& stage)
        {
            return stage.stage == NLS::Render::ShaderCompiler::ShaderStage::Pixel &&
                stage.targetPlatform == NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL &&
                stage.keywordHash == multiCompileHash &&
                stage.output.status == NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded;
        });
    EXPECT_TRUE(hasMultiCompilePixelStage);

    const auto hasMaterialFeaturePixelStage = std::any_of(
        artifact->stages.begin(),
        artifact->stages.end(),
        [materialFeatureHash](const NLS::Render::Assets::ShaderArtifactStage& stage)
        {
            return stage.stage == NLS::Render::ShaderCompiler::ShaderStage::Pixel &&
                stage.keywordHash == materialFeatureHash;
        });
    EXPECT_FALSE(hasMaterialFeaturePixelStage);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ShaderLabImportReflectionIncludesVariantOnlyResources)
{
    if (!HasExecutableShaderCompilerForTests())
        GTEST_SKIP() << "ShaderLab import success requires an executable DXC compiler.";

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Shaders" / "KeywordResource.shader",
        R"(Shader "Tests/KeywordResource"
{
    SubShader
    {
        Pass
        {
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            #pragma multi_compile _ MAIN_LIGHT_SHADOWS

            struct Attributes { float3 positionOS : POSITION; };
            struct Varyings { float4 positionCS : SV_POSITION; };

            Texture2D _BaseMap : register(t0, space2);
            SamplerState sampler_BaseMap : register(s0, space2);

            #if defined(MAIN_LIGHT_SHADOWS)
            Texture2D _ShadowMap : register(t1, space2);
            SamplerState sampler_ShadowMap : register(s1, space2);
            #endif

            Varyings VSMain(Attributes input)
            {
                Varyings output;
                output.positionCS = float4(input.positionOS, 1.0);
                return output;
            }

            float4 PSMain(Varyings input) : SV_Target
            {
                float4 color = _BaseMap.Sample(sampler_BaseMap, float2(0.0, 0.0));
            #if defined(MAIN_LIGHT_SHADOWS)
                color *= _ShadowMap.Sample(sampler_ShadowMap, float2(0.0, 0.0));
            #endif
                return color;
            }
            ENDHLSL
        }
    }
}
)");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Shaders/KeywordResource.shader"));

    const auto manifest = database.GetArtifactManifestForAssetPath("Assets/Shaders/KeywordResource.shader");
    ASSERT_TRUE(manifest.has_value());
    const auto* shaderArtifact = manifest->FindPrimaryArtifact();
    ASSERT_NE(shaderArtifact, nullptr);
    const auto artifact = NLS::Render::Assets::LoadShaderArtifact(root / shaderArtifact->artifactPath);
    ASSERT_TRUE(artifact.has_value());

    const auto hasTexture =
        [&artifact](const std::string& name)
    {
        return std::any_of(
            artifact->reflection.properties.begin(),
            artifact->reflection.properties.end(),
            [&name](const NLS::Render::Resources::ShaderPropertyDesc& property)
            {
                return property.name == name;
            });
    };

    EXPECT_TRUE(hasTexture("_BaseMap"));
    EXPECT_TRUE(hasTexture("_ShadowMap"))
        << "A shared pipeline layout must include resources referenced only by non-default variants.";

    NLS::Render::ShaderLab::ShaderLabKeywordSet shadows;
    shadows.Enable("MAIN_LIGHT_SHADOWS");
    const auto shadowKeywordHash = shadows.Hash();
    EXPECT_TRUE(std::any_of(
        artifact->stages.begin(),
        artifact->stages.end(),
        [shadowKeywordHash](const NLS::Render::Assets::ShaderArtifactStage& stage)
        {
            return stage.stage == NLS::Render::ShaderCompiler::ShaderStage::Pixel &&
                stage.targetPlatform == NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL &&
                stage.keywordHash == shadowKeywordHash &&
                stage.output.status == NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded;
        }));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ShaderLabImportWritesLightModePassSubAssets)
{
    if (!HasExecutableShaderCompilerForTests())
        GTEST_SKIP() << "ShaderLab artifact import success requires an executable DXC compiler.";

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Shaders" / "MultiPass.shader",
        R"(Shader "Tests/MultiPass"
{
    SubShader
    {
        Pass
        {
            Name "Forward"
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
            ENDHLSL
        }
        Pass
        {
            Name "DepthOnly"
            Tags { "LightMode" = "DepthOnly" }
            ZWrite On
            ZTest LessEqual
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
            ENDHLSL
        }
    }
})");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Shaders/MultiPass.shader"));

    const auto manifest = database.GetArtifactManifestForAssetPath("Assets/Shaders/MultiPass.shader");
    ASSERT_TRUE(manifest.has_value());
    EXPECT_EQ(manifest->primarySubAssetKey, "shader:MultiPass");
    ASSERT_NE(manifest->FindSubAsset("shader:MultiPass"), nullptr);
    const auto* depthOnly = manifest->FindSubAsset("shader:MultiPass/DepthOnly#1");
    ASSERT_NE(depthOnly, nullptr);
    EXPECT_EQ(depthOnly->artifactType, NLS::Core::Assets::ArtifactType::Shader);
    EXPECT_EQ(depthOnly->artifactPath.find("Library/Artifacts/"), 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ShaderLabImportDisambiguatesDuplicateLightModePassSubAssets)
{
    if (!HasExecutableShaderCompilerForTests())
        GTEST_SKIP() << "ShaderLab artifact import success requires an executable DXC compiler.";

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Shaders" / "DuplicateForward.shader",
        R"(Shader "Tests/DuplicateForward"
{
    SubShader
    {
        Pass
        {
            Name "ForwardOpaque"
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4(1, 0, 0, 1); }
            ENDHLSL
        }
        Pass
        {
            Name "ForwardAlphaTest"
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4(0, 1, 0, 1); }
            ENDHLSL
        }
    }
})");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Shaders/DuplicateForward.shader"));

    const auto manifest = database.GetArtifactManifestForAssetPath("Assets/Shaders/DuplicateForward.shader");
    ASSERT_TRUE(manifest.has_value());
    EXPECT_NE(manifest->FindSubAsset("shader:DuplicateForward"), nullptr);
    EXPECT_NE(manifest->FindSubAsset("shader:DuplicateForward/ForwardAlphaTest#1"), nullptr);
    EXPECT_EQ(manifest->subAssets.size(), 2u);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ShaderLabImportGeneratedSourcePathIncludesAssetPathToAvoidSameStemCollisions)
{
    if (!HasExecutableShaderCompilerForTests())
        GTEST_SKIP() << "ShaderLab artifact import success requires an executable DXC compiler.";

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto shaderSource = [](const char* name, const char* red)
    {
        std::ostringstream stream;
        stream << "Shader \"Tests/" << name << R"("
{
    SubShader
    {
        Pass
        {
            Name "Forward"
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4()" << red << R"(, 0, 0, 1); }
            ENDHLSL
        }
    }
})";
        return stream.str();
    };

    WriteTextFile(root / "Assets" / "Shaders" / "A" / "Foo.shader", shaderSource("A/Foo", "0.25"));
    WriteTextFile(root / "Assets" / "Shaders" / "B" / "Foo.shader", shaderSource("B/Foo", "0.75"));

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Shaders/A/Foo.shader"));
    ASSERT_TRUE(database.ImportAsset("Assets/Shaders/B/Foo.shader"));

    const auto shaderCacheRoot = root / "Library" / "ShaderCache" / "ImportedShaderLab";
    std::vector<std::filesystem::path> generatedFooSources;
    for (const auto& entry : std::filesystem::directory_iterator(shaderCacheRoot))
    {
        if (!entry.is_regular_file())
            continue;
        const auto fileName = entry.path().filename().generic_string();
        if (fileName.rfind("Foo_", 0u) == 0u && entry.path().extension() == ".hlsl")
            generatedFooSources.push_back(entry.path());
    }

    ASSERT_GE(generatedFooSources.size(), 2u);
    std::unordered_set<std::string> generatedNames;
    bool foundA = false;
    bool foundB = false;
    for (const auto& generatedPath : generatedFooSources)
    {
        generatedNames.insert(generatedPath.filename().generic_string());
        const auto generatedText = ReadTextFile(generatedPath);
        foundA = foundA || generatedText.find("Assets/Shaders/A/Foo.shader") != std::string::npos;
        foundB = foundB || generatedText.find("Assets/Shaders/B/Foo.shader") != std::string::npos;
    }

    EXPECT_EQ(generatedNames.size(), generatedFooSources.size());
    EXPECT_TRUE(foundA);
    EXPECT_TRUE(foundB);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ShaderArtifactRoundTripsDependencyPathsWithSemicolons)
{
    NLS::Render::Assets::ShaderArtifact artifact;
    artifact.sourcePath = "Assets/Shaders/HeroSurface.hlsl";
    artifact.subAssetKey = "shader:HeroSurface";

    NLS::Render::Assets::ShaderArtifactStage stage;
    stage.stage = NLS::Render::ShaderCompiler::ShaderStage::Pixel;
    stage.targetPlatform = NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL;
    stage.entryPoint = "PSMain";
    stage.targetProfile = "ps_6_0";
    stage.output.status = NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded;
    stage.output.bytecode = {1u, 2u, 3u, 4u};
    stage.output.dependencyPaths = {
        "C:/Project/Assets/Shaders/Shared;Lighting.hlsli",
        "C:/Project/Assets/Shaders/Common.hlsli"
    };
    artifact.stages.push_back(std::move(stage));

    const auto serialized = NLS::Render::Assets::SerializeShaderArtifact(artifact);
    const auto restored = NLS::Render::Assets::DeserializeShaderArtifact(serialized);
    ASSERT_TRUE(restored.has_value());
    ASSERT_EQ(restored->stages.size(), 1u);
    EXPECT_EQ(restored->stages.front().output.dependencyPaths, artifact.stages.front().output.dependencyPaths);
}

TEST(AssetDatabaseFacadeTests, StartupPreimportPlanIncludesShaderSourceAssetsAndSkipsWarmShaderArtifacts)
{
    if (!HasExecutableShaderCompilerForTests())
        GTEST_SKIP() << "Warm shader artifact preimport requires an executable DXC compiler.";

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Shaders" / "Warmup.shader",
        R"(Shader "Tests/Warmup"
{
    SubShader
    {
        Pass
        {
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
            ENDHLSL
        }
    }
})");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    auto coldPlan = scheduler.BuildPlan(database, AssetPreimportReason::EditorStartup);
    EXPECT_NE(
        std::find(coldPlan.assetPaths.begin(), coldPlan.assetPaths.end(), "Assets/Shaders/Warmup.shader"),
        coldPlan.assetPaths.end());

    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup));
    auto warmPlan = scheduler.BuildPlan(database, AssetPreimportReason::EditorStartup);
    EXPECT_EQ(
        std::find(warmPlan.assetPaths.begin(), warmPlan.assetPaths.end(), "Assets/Shaders/Warmup.shader"),
        warmPlan.assetPaths.end());

    WriteTextFile(
        root / "Assets" / "Shaders" / "Warmup.shader",
        R"(Shader "Tests/Warmup"
{
    SubShader
    {
        Pass
        {
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(1, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4(0, 1, 0, 1); }
            ENDHLSL
        }
    }
})");
    ASSERT_TRUE(database.Refresh());
    auto changedPlan = scheduler.BuildPlan(database, {AssetPreimportReason::FileWatcherChanged, {root / "Assets" / "Shaders" / "Warmup.shader"}});
    EXPECT_NE(
        std::find(changedPlan.assetPaths.begin(), changedPlan.assetPaths.end(), "Assets/Shaders/Warmup.shader"),
        changedPlan.assetPaths.end());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, StartupPreimportPlanReimportsShaderArtifactsWithoutUsableStages)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Shaders" / "Broken.shader", R"(Shader "Tests/Broken"
{
    SubShader
    {
        Pass
        {
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            float4 NotAnEntry() : SV_Target { return 0; }
            ENDHLSL
        }
    }
})");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    EXPECT_FALSE(database.ImportAsset("Assets/Shaders/Broken.shader"));
    const auto sourceRecord = database.LoadMainAssetAtPath("Assets/Shaders/Broken.shader");
    ASSERT_TRUE(sourceRecord.has_value());
    EXPECT_TRUE(sourceRecord->artifactPath.empty());
    EXPECT_FALSE(database.GetArtifactManifestForAssetPath("Assets/Shaders/Broken.shader").has_value());

    AssetPreimportScheduler scheduler;
    auto plan = scheduler.BuildPlan(database, AssetPreimportReason::EditorStartup);
    EXPECT_NE(
        std::find(plan.assetPaths.begin(), plan.assetPaths.end(), "Assets/Shaders/Broken.shader"),
        plan.assetPaths.end());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ShaderLabParseFailureDoesNotPublishFailedArtifactOrFallbackCompileWholeShaderSource)
{
    using namespace NLS::Editor::Assets;

    if (!HasExecutableShaderCompilerForTests())
        GTEST_SKIP() << "ShaderLab import success requires an executable DXC compiler.";

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Shaders" / "Malformed.shader", R"(Shader "Tests/Malformed"
{
    SubShader
    {
        Pass
        {
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            struct Attributes { float3 positionOS : POSITION; };
            struct Varyings { float4 positionCS : SV_POSITION; };
            Varyings VSMain(Attributes input)
            {
                Varyings output;
                output.positionCS = float4(input.positionOS, 1);
                return output;
            }
            float4 PSMain(Varyings input) : SV_Target0 { return 1.xxxx; }
            ENDHLSL
        }
    }
})");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Shaders/Malformed.shader"));

    const auto previousMainAsset = database.LoadMainAssetAtPath("Assets/Shaders/Malformed.shader");
    ASSERT_TRUE(previousMainAsset.has_value());
    const auto previousArtifactPath = previousMainAsset->artifactPath;

    WriteTextFile(root / "Assets" / "Shaders" / "Malformed.shader", R"(Shader "Tests/Malformed"
{
    SubShader
    {
        Pass
        {
            HLSLPROGRAM
            float4 PSMain() : SV_Target0 { return 1.xxxx; }
        }
    }
})");

    ASSERT_TRUE(database.Refresh());
    EXPECT_FALSE(database.ImportAsset("Assets/Shaders/Malformed.shader"));

    const auto mainAsset = database.LoadMainAssetAtPath("Assets/Shaders/Malformed.shader");
    ASSERT_TRUE(mainAsset.has_value());
    EXPECT_EQ(mainAsset->artifactPath, previousArtifactPath);
    const auto artifact = NLS::Render::Assets::LoadShaderArtifact(mainAsset->artifactPath);
    ASSERT_TRUE(artifact.has_value());
    EXPECT_TRUE(NLS::Render::Assets::HasUsableShaderArtifactStage(*artifact));
    const auto diagnostics = database.GetDiagnostics();
    const auto foundMissingEndHlsl = std::find_if(
        diagnostics.begin(),
        diagnostics.end(),
        [](const NLS::Core::Assets::AssetDiagnostic& diagnostic)
        {
            return diagnostic.message.find("missing ENDHLSL") != std::string::npos;
        });
    EXPECT_NE(foundMissingEndHlsl, diagnostics.end());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ShaderLabImportUsesFirstLightModePassWhenForwardIsMissing)
{
    using namespace NLS::Editor::Assets;

    if (!HasExecutableShaderCompilerForTests())
        GTEST_SKIP() << "ShaderLab import success requires an executable DXC compiler.";

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Shaders" / "DepthOnly.shader", R"(Shader "Tests/DepthOnly"
{
    SubShader
    {
        Pass
        {
            Name "DepthOnly"
            Tags { "LightMode" = "DepthOnly" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
            ENDHLSL
        }
    }
})");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Shaders/DepthOnly.shader"));

    const auto mainAsset = database.LoadMainAssetAtPath("Assets/Shaders/DepthOnly.shader");
    ASSERT_TRUE(mainAsset.has_value());
    const auto artifact = NLS::Render::Assets::LoadShaderArtifact(mainAsset->artifactPath);
    ASSERT_TRUE(artifact.has_value());
    EXPECT_TRUE(NLS::Render::Assets::HasUsableShaderArtifactStage(*artifact));
    ASSERT_TRUE(artifact->shaderLabPassState.has_value());
    EXPECT_TRUE(artifact->shaderLabPassState->depthWrite);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, StartupPreimportPlanReimportsShaderArtifactsMissingGlslStages)
{
    using namespace NLS::Editor::Assets;

    if (!HasExecutableShaderCompilerForTests())
        GTEST_SKIP() << "ShaderLab import success requires an executable DXC compiler.";

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Shaders" / "LegacyWarm.shader",
        R"(Shader "Tests/LegacyWarm"
{
    SubShader
    {
        Pass
        {
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
            ENDHLSL
        }
    }
})");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Shaders/LegacyWarm.shader"));

    const auto mainAsset = database.LoadMainAssetAtPath("Assets/Shaders/LegacyWarm.shader");
    ASSERT_TRUE(mainAsset.has_value());
    auto shaderArtifact = NLS::Render::Assets::LoadShaderArtifact(mainAsset->artifactPath);
    ASSERT_TRUE(shaderArtifact.has_value());
    shaderArtifact->stages.erase(
        std::remove_if(
            shaderArtifact->stages.begin(),
            shaderArtifact->stages.end(),
            [](const NLS::Render::Assets::ShaderArtifactStage& stage)
            {
                return stage.targetPlatform == NLS::Render::ShaderCompiler::ShaderTargetPlatform::GLSL;
            }),
        shaderArtifact->stages.end());
    const auto serializedShaderArtifact = NLS::Render::Assets::SerializeShaderArtifact(*shaderArtifact);
    WriteTextFile(
        mainAsset->artifactPath,
        std::string(serializedShaderArtifact.begin(), serializedShaderArtifact.end()));

    AssetPreimportScheduler scheduler;
    auto plan = scheduler.BuildPlan(database, AssetPreimportReason::EditorStartup);
    EXPECT_NE(
        std::find(plan.assetPaths.begin(), plan.assetPaths.end(), "Assets/Shaders/LegacyWarm.shader"),
        plan.assetPaths.end());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ArtifactDatabaseKeepsConcurrentManifestRecords)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "HeroA.gltf", R"({"asset":{"version":"2.0"}})");
    WriteTextFile(root / "Assets" / "Models" / "HeroB.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto heroAId = ParseAssetId(database.AssetPathToGUID("Assets/Models/HeroA.gltf"));
    const auto heroBId = ParseAssetId(database.AssetPathToGUID("Assets/Models/HeroB.gltf"));
    ASSERT_TRUE(heroAId.IsValid());
    ASSERT_TRUE(heroBId.IsValid());

    ArtifactManifest heroA;
    heroA.sourceAssetId = heroAId;
    heroA.importerId = "scene-model";
    heroA.targetPlatform = "editor";
    heroA.primarySubAssetKey = "model:HeroA";
    heroA.subAssets.push_back(MakeArtifact(heroAId, "model:HeroA", ArtifactType::Model, "model"));

    ArtifactManifest heroB;
    heroB.sourceAssetId = heroBId;
    heroB.importerId = "scene-model";
    heroB.targetPlatform = "editor";
    heroB.primarySubAssetKey = "model:HeroB";
    heroB.subAssets.push_back(MakeArtifact(heroBId, "model:HeroB", ArtifactType::Model, "model"));

    auto first = std::async(std::launch::async, [&database, heroA]()
    {
        database.AddArtifactManifest(heroA);
    });
    auto second = std::async(std::launch::async, [&database, heroB]()
    {
        database.AddArtifactManifest(heroB);
    });
    first.get();
    second.get();

    ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB"));
    EXPECT_NE(artifactDatabase.Find(heroAId, "model:HeroA", "editor"), nullptr);
    EXPECT_NE(artifactDatabase.Find(heroBId, "model:HeroB", "editor"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ArtifactDatabaseBatchUpsertsDoNotReloadCentralIndexPerManifest)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "HeroA.gltf", R"({"asset":{"version":"2.0"}})");
    WriteTextFile(root / "Assets" / "Models" / "HeroB.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto heroAId = ParseAssetId(database.AssetPathToGUID("Assets/Models/HeroA.gltf"));
    const auto heroBId = ParseAssetId(database.AssetPathToGUID("Assets/Models/HeroB.gltf"));

    ArtifactManifest heroA;
    heroA.sourceAssetId = heroAId;
    heroA.importerId = "scene-model";
    heroA.targetPlatform = "editor";
    heroA.primarySubAssetKey = "model:HeroA";
    heroA.subAssets.push_back(MakeArtifact(heroAId, "model:HeroA", ArtifactType::Model, "model"));

    ArtifactManifest heroB;
    heroB.sourceAssetId = heroBId;
    heroB.importerId = "scene-model";
    heroB.targetPlatform = "editor";
    heroB.primarySubAssetKey = "model:HeroB";
    heroB.subAssets.push_back(MakeArtifact(heroBId, "model:HeroB", ArtifactType::Model, "model"));

    database.AddArtifactManifest(heroA);
    WriteTextFile(root / "Library" / "ArtifactDB", "corrupted central index\n");
    database.AddArtifactManifest(heroB);

    ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB"));
    EXPECT_NE(artifactDatabase.Find(heroAId, "model:HeroA", "editor"), nullptr);
    EXPECT_NE(artifactDatabase.Find(heroBId, "model:HeroB", "editor"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ArtifactDatabaseBatchUpsertsFlushCentralIndexOnceOnStopAssetEditing)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "HeroA.gltf", R"({"asset":{"version":"2.0"}})");
    WriteTextFile(root / "Assets" / "Models" / "HeroB.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto heroAId = ParseAssetId(database.AssetPathToGUID("Assets/Models/HeroA.gltf"));
    const auto heroBId = ParseAssetId(database.AssetPathToGUID("Assets/Models/HeroB.gltf"));

    ArtifactManifest heroA;
    heroA.sourceAssetId = heroAId;
    heroA.importerId = "scene-model";
    heroA.targetPlatform = "editor";
    heroA.primarySubAssetKey = "model:HeroA";
    heroA.subAssets.push_back(MakeArtifact(heroAId, "model:HeroA", ArtifactType::Model, "model"));

    ArtifactManifest heroB;
    heroB.sourceAssetId = heroBId;
    heroB.importerId = "scene-model";
    heroB.targetPlatform = "editor";
    heroB.primarySubAssetKey = "model:HeroB";
    heroB.subAssets.push_back(MakeArtifact(heroBId, "model:HeroB", ArtifactType::Model, "model"));

    database.StartAssetEditing();
    database.AddArtifactManifest(heroA);
    database.AddArtifactManifest(heroB);
    EXPECT_FALSE(std::filesystem::exists(root / "Library" / "ArtifactDB"));
    EXPECT_TRUE(database.StopAssetEditing());

    ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB"));
    EXPECT_NE(artifactDatabase.Find(heroAId, "model:HeroA", "editor"), nullptr);
    EXPECT_NE(artifactDatabase.Find(heroBId, "model:HeroB", "editor"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, AutoImportedTextureManifestSurvivesDeferredArtifactDatabaseFlush)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    if (!HasExecutableShaderCompilerForTests())
        GTEST_SKIP() << "ShaderLab import success requires an executable DXC compiler.";

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Engine" / "Shaders" / "ShaderLab" / "StandardPBR.shader",
        R"(
Shader "Nullus/StandardPBR"
{
    Properties
    {
        _BaseColor("Base Color", Color) = (1, 1, 1, 1)
    }

    SubShader
    {
        Pass
        {
            Name "Forward"
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
            ENDHLSL
        }
    }
}
)");
    WriteBinaryFile(root / "Assets" / "Textures" / "AutoAlbedo.png", TinyPng());
    WriteTextFile(root / "Assets" / "Models" / "AutoTextureHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/AutoAlbedo.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Engine/Shaders/ShaderLab/StandardPBR.shader"));

    const auto modelId = ParseAssetId(database.AssetPathToGUID("Assets/Models/AutoTextureHero.gltf"));
    ASSERT_TRUE(modelId.IsValid());
    ArtifactManifest dirtyModelManifest;
    dirtyModelManifest.sourceAssetId = modelId;
    dirtyModelManifest.importerId = "scene-model";
    dirtyModelManifest.targetPlatform = "editor";
    dirtyModelManifest.primarySubAssetKey = "model:Dirty";
    dirtyModelManifest.subAssets.push_back(MakeArtifact(modelId, "model:Dirty", ArtifactType::Model, "model"));

    auto textureMeta = AssetMeta::Load(root / "Assets" / "Textures" / "AutoAlbedo.png.meta")
        .value_or(AssetMeta::CreateForAsset(root / "Assets" / "Textures" / "AutoAlbedo.png"));
    textureMeta.assetType = AssetType::Texture;
    textureMeta.importerId = "texture";
    ASSERT_TRUE(textureMeta.Save(root / "Assets" / "Textures" / "AutoAlbedo.png.meta"));

    auto modelMeta = AssetMeta::Load(root / "Assets" / "Models" / "AutoTextureHero.gltf.meta");
    ASSERT_TRUE(modelMeta.has_value());
    ModelTextureResolutionSettings settings;
    settings.autoImportMissingTextureFiles = true;
    StoreModelTextureResolutionSettings(*modelMeta, settings);
    ASSERT_TRUE(modelMeta->Save(root / "Assets" / "Models" / "AutoTextureHero.gltf.meta"));

    database.StartAssetEditing();
    database.AddArtifactManifest(dirtyModelManifest);
    ASSERT_TRUE(database.ImportAsset("Assets/Models/AutoTextureHero.gltf"));
    ASSERT_TRUE(database.StopAssetEditing());

    ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB"));
    EXPECT_NE(artifactDatabase.Find(modelId, "prefab:AutoTextureHero", "editor"), nullptr);
    EXPECT_NE(artifactDatabase.Find(textureMeta.id, "texture:main", TextureArtifactTargetPlatformForTest()), nullptr)
        << "Facade-level ArtifactDB cache flush must not overwrite auto-imported dependency manifests.";

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ExternalModelAutoImportedTextureManifestsFlushArtifactDatabaseOnce)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Engine" / "Shaders" / "ShaderLab" / "StandardPBR.shader",
        R"(
Shader "Nullus/StandardPBR"
{
    Properties
    {
        _BaseColor("Base Color", Color) = (1, 1, 1, 1)
    }

    SubShader
    {
        Pass
        {
            Name "Forward"
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
            ENDHLSL
        }
    }
}
)");
    WriteBinaryFile(root / "Assets" / "Textures" / "AutoAlbedoA.png", TinyPng());
    WriteBinaryFile(root / "Assets" / "Textures" / "AutoAlbedoB.png", TinyPng());
    WriteTextFile(root / "Assets" / "Models" / "BatchTextureHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/AutoAlbedoA.png", "mimeType": "image/png" },
                { "uri": "../Textures/AutoAlbedoB.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 },
                { "source": 1 }
            ],
            "materials": [
                {
                    "name": "BodyA",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                },
                {
                    "name": "BodyB",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 1 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0, 1] }],
            "meshes": [
                { "name": "BodyMeshA", "primitives": [{ "attributes": {}, "material": 0 }] },
                { "name": "BodyMeshB", "primitives": [{ "attributes": {}, "material": 1 }] }
            ],
            "nodes": [
                { "name": "RootA", "mesh": 0 },
                { "name": "RootB", "mesh": 1 }
            ]
        })");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    const auto shaderId = ParseAssetId(database.AssetPathToGUID("Assets/Engine/Shaders/ShaderLab/StandardPBR.shader"));
    ASSERT_TRUE(shaderId.IsValid());
    ArtifactManifest shaderManifest;
    shaderManifest.sourceAssetId = shaderId;
    shaderManifest.importerId = "shaderlab";
    shaderManifest.targetPlatform = "editor";
    shaderManifest.primarySubAssetKey = "shader:StandardPBR/Forward#0";
    shaderManifest.subAssets.push_back(MakeArtifact(
        shaderId,
        "shader:StandardPBR/Forward#0",
        ArtifactType::Shader,
        "shader"));
    database.AddArtifactManifest(shaderManifest);

    for (const auto* textureName : {"AutoAlbedoA.png", "AutoAlbedoB.png"})
    {
        const auto texturePath = root / "Assets" / "Textures" / textureName;
        const auto textureMetaPath = texturePath.generic_string() + ".meta";
        auto textureMeta = AssetMeta::Load(textureMetaPath)
            .value_or(AssetMeta::CreateForAsset(texturePath));
        textureMeta.assetType = AssetType::Texture;
        textureMeta.importerId = "texture";
        ASSERT_TRUE(textureMeta.Save(textureMetaPath));
    }

    auto modelMeta = AssetMeta::Load(root / "Assets" / "Models" / "BatchTextureHero.gltf.meta");
    ASSERT_TRUE(modelMeta.has_value());
    ModelTextureResolutionSettings settings;
    settings.autoImportMissingTextureFiles = true;
    StoreModelTextureResolutionSettings(*modelMeta, settings);
    ASSERT_TRUE(modelMeta->Save(root / "Assets" / "Models" / "BatchTextureHero.gltf.meta"));

    ResetArtifactDatabaseSaveAttemptCountForTesting();
    ResetAssetDatabaseArtifactManifestCurrentCheckCountForTesting();
    ImportProgressTracker importProgress;
    ASSERT_TRUE(database.ImportAssetFromCurrentDatabase(
        "Assets/Models/BatchTextureHero.gltf",
        importProgress,
        1u));
    EXPECT_EQ(GetArtifactDatabaseSaveAttemptCountForTesting(), 1u)
        << "Model import should batch the model and auto-imported texture manifests into one ArtifactDB flush.";
    EXPECT_EQ(GetAssetDatabaseArtifactManifestCurrentCheckCountForTesting(), 0u)
        << "Freshly imported model and texture manifests should publish as known-current without rehashing sources.";

    const auto textureAId = ParseAssetId(database.AssetPathToGUID("Assets/Textures/AutoAlbedoA.png"));
    const auto textureBId = ParseAssetId(database.AssetPathToGUID("Assets/Textures/AutoAlbedoB.png"));
    ASSERT_TRUE(textureAId.IsValid());
    ASSERT_TRUE(textureBId.IsValid());

    ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB"));
    EXPECT_NE(artifactDatabase.Find(textureAId, "texture:main", TextureArtifactTargetPlatformForTest()), nullptr);
    EXPECT_NE(artifactDatabase.Find(textureBId, "texture:main", TextureArtifactTargetPlatformForTest()), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, FailedExternalModelDeferredArtifactDatabaseFlushRestoresDirtyCache)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Engine" / "Shaders" / "ShaderLab" / "StandardPBR.shader",
        R"(Shader "Nullus/StandardPBR" {})");
    WriteBinaryFile(root / "Assets" / "Textures" / "RollbackAlbedo.png", TinyPng());
    WriteTextFile(root / "Assets" / "Models" / "RollbackHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/RollbackAlbedo.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [
                { "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }
            ],
            "nodes": [
                { "name": "Root", "mesh": 0 }
            ]
        })");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());

    const auto shaderId = ParseAssetId(database.AssetPathToGUID("Assets/Engine/Shaders/ShaderLab/StandardPBR.shader"));
    ASSERT_TRUE(shaderId.IsValid());
    ArtifactManifest shaderManifest;
    shaderManifest.sourceAssetId = shaderId;
    shaderManifest.importerId = "shaderlab";
    shaderManifest.targetPlatform = "editor";
    shaderManifest.primarySubAssetKey = "shader:StandardPBR/Forward#0";
    shaderManifest.subAssets.push_back(MakeArtifact(
        shaderId,
        "shader:StandardPBR/Forward#0",
        ArtifactType::Shader,
        "shader"));
    database.AddArtifactManifest(shaderManifest);

    const auto texturePath = root / "Assets" / "Textures" / "RollbackAlbedo.png";
    const auto textureMetaPath = texturePath.generic_string() + ".meta";
    auto textureMeta = AssetMeta::Load(textureMetaPath)
        .value_or(AssetMeta::CreateForAsset(texturePath));
    textureMeta.assetType = AssetType::Texture;
    textureMeta.importerId = "texture";
    ASSERT_TRUE(textureMeta.Save(textureMetaPath));

    auto modelMeta = AssetMeta::Load(root / "Assets" / "Models" / "RollbackHero.gltf.meta");
    ASSERT_TRUE(modelMeta.has_value());
    ModelTextureResolutionSettings settings;
    settings.autoImportMissingTextureFiles = true;
    StoreModelTextureResolutionSettings(*modelMeta, settings);
    ASSERT_TRUE(modelMeta->Save(root / "Assets" / "Models" / "RollbackHero.gltf.meta"));

    const auto modelId = ParseAssetId(database.AssetPathToGUID("Assets/Models/RollbackHero.gltf"));
    const auto textureId = ParseAssetId(database.AssetPathToGUID("Assets/Textures/RollbackAlbedo.png"));
    ASSERT_TRUE(modelId.IsValid());
    ASSERT_TRUE(textureId.IsValid());

    SetArtifactDatabaseFailNextSaveForTesting(true);
    ImportProgressTracker importProgress;
    EXPECT_FALSE(database.ImportAssetFromCurrentDatabase(
        "Assets/Models/RollbackHero.gltf",
        importProgress,
        1u));
    SetArtifactDatabaseFailNextSaveForTesting(false);

    database.AddArtifactManifest(shaderManifest);

    ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB"));
    EXPECT_EQ(artifactDatabase.Find(modelId, "prefab:RollbackHero", "editor"), nullptr)
        << "A failed deferred model import must not leak its prefab manifest into a later ArtifactDB flush.";
    EXPECT_EQ(artifactDatabase.Find(textureId, "texture:main", TextureArtifactTargetPlatformForTest()), nullptr)
        << "A failed deferred model import must not leak auto-imported texture manifests into a later ArtifactDB flush.";

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, FailedExternalModelDeferredArtifactDatabaseFlushRestoresPartiallySavedDatabases)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto textureRoot = root / "MountedTextures";
    WriteTextFile(
        root / "Assets" / "Engine" / "Shaders" / "ShaderLab" / "StandardPBR.shader",
        R"(Shader "Nullus/StandardPBR" {})");
    WriteBinaryFile(textureRoot / "Textures" / "CrossDbAlbedo.png", TinyPng());
    WriteTextFile(root / "Assets" / "Models" / "CrossDbHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../../MountedTextures/Textures/CrossDbAlbedo.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [
                { "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }
            ],
            "nodes": [
                { "name": "Root", "mesh": 0 }
            ]
        })");

    std::vector<EditorAssetRoot> roots {{
        root / "Assets",
        false,
        "Assets",
        root / "Library"
    }, {
        textureRoot,
        false,
        "MountedTextures",
        root / "TextureLibrary"
    }};
    AssetDatabaseFacade database(std::move(roots));
    ASSERT_TRUE(database.Refresh());

    const auto shaderId = ParseAssetId(database.AssetPathToGUID("Assets/Engine/Shaders/ShaderLab/StandardPBR.shader"));
    ASSERT_TRUE(shaderId.IsValid());
    ArtifactManifest shaderManifest;
    shaderManifest.sourceAssetId = shaderId;
    shaderManifest.importerId = "shaderlab";
    shaderManifest.targetPlatform = "editor";
    shaderManifest.primarySubAssetKey = "shader:StandardPBR/Forward#0";
    shaderManifest.subAssets.push_back(MakeArtifact(
        shaderId,
        "shader:StandardPBR/Forward#0",
        ArtifactType::Shader,
        "shader"));
    database.AddArtifactManifest(shaderManifest);

    const auto texturePath = textureRoot / "Textures" / "CrossDbAlbedo.png";
    const auto textureMetaPath = texturePath.generic_string() + ".meta";
    auto textureMeta = AssetMeta::Load(textureMetaPath)
        .value_or(AssetMeta::CreateForAsset(texturePath));
    textureMeta.assetType = AssetType::Texture;
    textureMeta.importerId = "texture";
    ASSERT_TRUE(textureMeta.Save(textureMetaPath));

    auto modelMeta = AssetMeta::Load(root / "Assets" / "Models" / "CrossDbHero.gltf.meta");
    ASSERT_TRUE(modelMeta.has_value());
    ModelTextureResolutionSettings settings;
    settings.autoImportMissingTextureFiles = true;
    StoreModelTextureResolutionSettings(*modelMeta, settings);
    ASSERT_TRUE(modelMeta->Save(root / "Assets" / "Models" / "CrossDbHero.gltf.meta"));

    const auto modelId = ParseAssetId(database.AssetPathToGUID("Assets/Models/CrossDbHero.gltf"));
    const auto textureId = ParseAssetId(database.AssetPathToGUID("MountedTextures/Textures/CrossDbAlbedo.png"));
    ASSERT_TRUE(modelId.IsValid());
    ASSERT_TRUE(textureId.IsValid());

    ResetArtifactDatabaseSaveAttemptCountForTesting();
    SetArtifactDatabaseFailSaveAttemptForTesting(2u);
    ImportProgressTracker importProgress;
    EXPECT_FALSE(database.ImportAssetFromCurrentDatabase(
        "Assets/Models/CrossDbHero.gltf",
        importProgress,
        1u));
    SetArtifactDatabaseFailSaveAttemptForTesting(0u);
    EXPECT_EQ(GetArtifactDatabaseSaveAttemptCountForTesting(), 2u);

    ArtifactDatabase modelDatabase;
    ASSERT_TRUE(modelDatabase.Load(root / "Library" / "ArtifactDB"));
    EXPECT_EQ(modelDatabase.Find(modelId, "prefab:CrossDbHero", "editor"), nullptr)
        << "A failed multi-ArtifactDB import must not persist the model manifest if a later DB save fails.";

    const auto textureDatabasePath = root / "TextureLibrary" / "ArtifactDB";
    if (std::filesystem::exists(textureDatabasePath))
    {
        ArtifactDatabase textureDatabase;
        ASSERT_TRUE(textureDatabase.Load(textureDatabasePath));
        EXPECT_EQ(textureDatabase.Find(textureId, "texture:main", TextureArtifactTargetPlatformForTest()), nullptr)
            << "A failed multi-ArtifactDB import must roll back any dependency DB saved before the failure.";
    }

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, FailedExternalModelAutoImportRestoresDependencySourceIndexes)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Engine" / "Shaders" / "ShaderLab" / "StandardPBR.shader",
        R"(Shader "Nullus/StandardPBR" {})");
    WriteTextFile(root / "Assets" / "Models" / "SourceRollbackHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/AutoCreatedRollback.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [
                { "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }
            ],
            "nodes": [
                { "name": "Root", "mesh": 0 }
            ]
        })");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());

    const auto shaderId = ParseAssetId(database.AssetPathToGUID("Assets/Engine/Shaders/ShaderLab/StandardPBR.shader"));
    ASSERT_TRUE(shaderId.IsValid());
    ArtifactManifest shaderManifest;
    shaderManifest.sourceAssetId = shaderId;
    shaderManifest.importerId = "shaderlab";
    shaderManifest.targetPlatform = "editor";
    shaderManifest.primarySubAssetKey = "shader:StandardPBR/Forward#0";
    shaderManifest.subAssets.push_back(MakeArtifact(
        shaderId,
        "shader:StandardPBR/Forward#0",
        ArtifactType::Shader,
        "shader"));
    database.AddArtifactManifest(shaderManifest);

    WriteBinaryFile(root / "Assets" / "Textures" / "AutoCreatedRollback.png", TinyPng());
    auto modelMeta = AssetMeta::Load(root / "Assets" / "Models" / "SourceRollbackHero.gltf.meta");
    ASSERT_TRUE(modelMeta.has_value());
    ModelTextureResolutionSettings settings;
    settings.autoImportMissingTextureFiles = true;
    StoreModelTextureResolutionSettings(*modelMeta, settings);
    ASSERT_TRUE(modelMeta->Save(root / "Assets" / "Models" / "SourceRollbackHero.gltf.meta"));
    ASSERT_TRUE(database.AssetPathToGUID("Assets/Textures/AutoCreatedRollback.png").empty());

    SetArtifactDatabaseFailNextSaveForTesting(true);
    ImportProgressTracker importProgress;
    EXPECT_FALSE(database.ImportAssetFromCurrentDatabase(
        "Assets/Models/SourceRollbackHero.gltf",
        importProgress,
        1u));
    SetArtifactDatabaseFailNextSaveForTesting(false);

    EXPECT_TRUE(database.AssetPathToGUID("Assets/Textures/AutoCreatedRollback.png").empty())
        << "Failed auto-import cleanup must also remove the dependency from the in-memory source/path indexes.";
    EXPECT_FALSE(std::filesystem::exists(root / "Assets" / "Textures" / "AutoCreatedRollback.png.meta"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, RefreshKnownSourceAssetsRejectsOnlyMetaPathsWithoutClearingDatabase)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    const auto heroGuid = database.AssetPathToGUID("Assets/Models/Hero.gltf");
    ASSERT_FALSE(heroGuid.empty());

    const std::vector<std::filesystem::path> paths {
        root / "Assets" / "Models" / "Hero.gltf.meta"
    };
    EXPECT_FALSE(database.RefreshKnownSourceAssets(std::span<const std::filesystem::path>(
        paths.data(),
        paths.size())));
    EXPECT_EQ(database.AssetPathToGUID("Assets/Models/Hero.gltf"), heroGuid)
        << "Rejected targeted refresh input must not replace the existing source database with an empty one.";

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, RefreshKnownSourceAssetsPreservesUntouchedSourceRecords)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "HeroA.gltf", R"({"asset":{"version":"2.0"}})");
    WriteTextFile(root / "Assets" / "Models" / "HeroB.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    const auto heroAGuid = database.AssetPathToGUID("Assets/Models/HeroA.gltf");
    const auto heroBGuid = database.AssetPathToGUID("Assets/Models/HeroB.gltf");
    ASSERT_FALSE(heroAGuid.empty());
    ASSERT_FALSE(heroBGuid.empty());

    WriteTextFile(root / "Assets" / "Models" / "HeroA.gltf", R"({"asset":{"version":"2.0"},"scene":0})");
    const std::vector<std::filesystem::path> paths {
        root / "Assets" / "Models" / "HeroA.gltf"
    };
    ASSERT_TRUE(database.RefreshKnownSourceAssets(std::span<const std::filesystem::path>(
        paths.data(),
        paths.size())));

    EXPECT_EQ(database.AssetPathToGUID("Assets/Models/HeroA.gltf"), heroAGuid);
    EXPECT_EQ(database.AssetPathToGUID("Assets/Models/HeroB.gltf"), heroBGuid)
        << "Targeted refresh of a visible/changed subset must not replace the source database with that subset.";

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, RefreshKnownSourceAssetsRejectsOutsideRootWithoutFlushingDeferredArtifactDatabase)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    const auto heroId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Hero.gltf"));
    ASSERT_TRUE(heroId.IsValid());

    ArtifactManifest manifest;
    manifest.sourceAssetId = heroId;
    manifest.importerId = "scene-model";
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "model:Hero";
    manifest.subAssets.push_back(MakeArtifact(heroId, "model:Hero", ArtifactType::Model, "model"));

    database.StartAssetEditing();
    database.AddArtifactManifest(manifest);
    ResetArtifactDatabaseSaveAttemptCountForTesting();

    const std::vector<std::filesystem::path> paths {
        root / "Outside.gltf"
    };
    EXPECT_FALSE(database.RefreshKnownSourceAssets(std::span<const std::filesystem::path>(
        paths.data(),
        paths.size())));
    EXPECT_EQ(GetArtifactDatabaseSaveAttemptCountForTesting(), 0u)
        << "Rejected targeted refresh input must not flush StartAssetEditing-deferred ArtifactDB changes.";
    EXPECT_FALSE(std::filesystem::exists(root / "Library" / "ArtifactDB"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, StopAssetEditingReportsArtifactDatabaseSaveFailure)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto heroId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Hero.gltf"));

    ArtifactManifest manifest;
    manifest.sourceAssetId = heroId;
    manifest.importerId = "scene-model";
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "model:Hero";
    manifest.subAssets.push_back(MakeArtifact(heroId, "model:Hero", ArtifactType::Model, "model"));

    database.StartAssetEditing();
    database.AddArtifactManifest(manifest);

    const auto databasePath = root / "Library" / "ArtifactDB";
    std::filesystem::create_directories(databasePath.parent_path());
    WriteTextFile(databasePath, "blocked by file\n");

    EXPECT_FALSE(database.StopAssetEditing());
    EXPECT_TRUE(std::filesystem::is_regular_file(databasePath));
    ASSERT_FALSE(database.GetDiagnostics().empty());
    EXPECT_NE(database.GetDiagnostics().back().message.find("ArtifactDB could not be saved"), std::string::npos);
    EXPECT_NE(database.GetDiagnostics().back().message.find("not a directory"), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, RefreshDoesNotWarnWhenCentralArtifactDatabaseIsMissing)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Prefabs" / "Fresh.prefab", "Prefab \"Fresh\" {}\n");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto hasArtifactDbReadFailure = std::any_of(
        database.GetDiagnostics().begin(),
        database.GetDiagnostics().end(),
        [](const NLS::Core::Assets::AssetDiagnostic& diagnostic)
        {
            return diagnostic.code == "assetdatabase-artifactdb-read-failed";
        });
    EXPECT_FALSE(hasArtifactDbReadFailure)
        << "A missing ArtifactDB is normal for a fresh Library and should not be reported as corruption.";

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, RefreshReportsStaleArtifactDatabaseSchemaForReimport)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Prefabs" / "Stale.prefab", "Prefab \"Stale\" {}\n");
    std::filesystem::create_directories(root / "Library" / "ArtifactDB");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    NLS::Core::Assets::ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Save(root / "Library" / "ArtifactDB"));
    WriteTextFile(root / "Library" / "ArtifactDB" / "data.mdb", "stale artifact database schema\n");

    AssetDatabaseFacade reload({root});
    ASSERT_TRUE(reload.Refresh());

    const auto foundDiagnostic = std::find_if(
        reload.GetDiagnostics().begin(),
        reload.GetDiagnostics().end(),
        [](const NLS::Core::Assets::AssetDiagnostic& diagnostic)
        {
            return diagnostic.code == "assetdatabase-artifactdb-read-failed" &&
                diagnostic.message.find("reimport source assets") != std::string::npos;
        });
    ASSERT_NE(foundDiagnostic, reload.GetDiagnostics().end());
    EXPECT_NE(foundDiagnostic->message.find("ArtifactDB could not be read"), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ArtifactDatabaseRefreshFlushesDeferredCentralIndexBeforeClearingCache)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "HeroA.gltf", R"({"asset":{"version":"2.0"}})");
    WriteTextFile(root / "Assets" / "Models" / "HeroB.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto heroAId = ParseAssetId(database.AssetPathToGUID("Assets/Models/HeroA.gltf"));
    const auto heroBId = ParseAssetId(database.AssetPathToGUID("Assets/Models/HeroB.gltf"));

    ArtifactManifest heroA;
    heroA.sourceAssetId = heroAId;
    heroA.importerId = "scene-model";
    heroA.targetPlatform = "editor";
    heroA.primarySubAssetKey = "model:HeroA";
    heroA.subAssets.push_back(MakeArtifact(heroAId, "model:HeroA", ArtifactType::Model, "model"));

    ArtifactManifest heroB;
    heroB.sourceAssetId = heroBId;
    heroB.importerId = "scene-model";
    heroB.targetPlatform = "editor";
    heroB.primarySubAssetKey = "model:HeroB";
    heroB.subAssets.push_back(MakeArtifact(heroBId, "model:HeroB", ArtifactType::Model, "model"));

    database.StartAssetEditing();
    database.AddArtifactManifest(heroA);
    database.AddArtifactManifest(heroB);
    EXPECT_FALSE(std::filesystem::exists(root / "Library" / "ArtifactDB"));

    ASSERT_TRUE(database.Refresh());

    ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB"));
    EXPECT_NE(artifactDatabase.Find(heroAId, "model:HeroA", "editor"), nullptr);
    EXPECT_NE(artifactDatabase.Find(heroBId, "model:HeroB", "editor"), nullptr);

    EXPECT_TRUE(database.StopAssetEditing());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ImportedModelManifestReloadsInFreshFacadeAfterRefresh)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "Hero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "meshes": [
                {
                    "name": "Body",
                    "primitives": [
                        { "attributes": { "POSITION": 0 } }
                    ]
                }
            ],
            "nodes": [
                { "name": "HeroRoot", "mesh": 0 }
            ]
        })");

    {
        AssetDatabaseFacade importer({root});
        ASSERT_TRUE(importer.Refresh());
        RegisterStandardPbrFreshnessOnlyDependency(importer, root);
        ASSERT_TRUE(importer.ImportAsset("Assets/Models/Hero.gltf"));
    }

    AssetDatabaseFacade reloaded({root});
    ASSERT_TRUE(reloaded.Refresh());

    const auto allAssets = reloaded.LoadAllAssetsAtPath("Assets/Models/Hero.gltf");
    const auto hasGeneratedPrefab = std::any_of(
        allAssets.begin(),
        allAssets.end(),
        [](const AssetDatabaseRecord& record)
        {
            return record.subAssetKey == "prefab:Hero" &&
                record.artifactType == ArtifactType::Prefab &&
                !record.artifactPath.empty();
        });
    EXPECT_TRUE(hasGeneratedPrefab);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ArtifactManifestCurrentRejectsStaleImporterMetadata)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "StaleManifestHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "StaleManifestHeroRoot" }]
        })");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    RegisterStandardPbrFreshnessOnlyDependency(database, root);
    ASSERT_TRUE(database.ImportAsset("Assets/Models/StaleManifestHero.gltf"));
    EXPECT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/StaleManifestHero.gltf"));

    auto manifest = database.GetArtifactManifestForAssetPath("Assets/Models/StaleManifestHero.gltf");
    ASSERT_TRUE(manifest.has_value());
    manifest->importerVersion += 1u;
    database.AddArtifactManifest(*manifest);

    EXPECT_FALSE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/StaleManifestHero.gltf"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ArtifactManifestCurrentRejectsPreDx12TextureBuildModelImporterVersion)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "LegacyTextureBuildHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "LegacyTextureBuildHeroRoot" }]
        })");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    RegisterStandardPbrFreshnessOnlyDependency(database, root);
    ASSERT_TRUE(database.ImportAsset("Assets/Models/LegacyTextureBuildHero.gltf"));
    EXPECT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/LegacyTextureBuildHero.gltf"));

    auto manifest = database.GetArtifactManifestForAssetPath("Assets/Models/LegacyTextureBuildHero.gltf");
    ASSERT_TRUE(manifest.has_value());
    manifest->importerVersion = 5u;
    database.AddArtifactManifest(*manifest);

    EXPECT_FALSE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/LegacyTextureBuildHero.gltf"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ArtifactManifestCurrentRejectsReadOnlyMetaBelowCurrentImporterVersion)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto packageRoot = root / "Packages";
    WriteTextFile(
        packageRoot / "Models" / "LegacyReadOnlyHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "LegacyReadOnlyHeroRoot" }]
        })");

    auto meta = AssetMeta::CreateForAsset(packageRoot / "Models" / "LegacyReadOnlyHero.gltf");
    meta.importerVersion = 5u;
    ASSERT_TRUE(meta.Save(packageRoot / "Models" / "LegacyReadOnlyHero.gltf.meta"));

    auto writableRoots = MakeProjectEditorAssetRoots(root);
    writableRoots.push_back({packageRoot, false, "Packages", root / "Library"});
    AssetDatabaseFacade importer(writableRoots);
    ASSERT_TRUE(importer.Refresh());
    RegisterStandardPbrFreshnessOnlyDependency(importer, root);
    ASSERT_TRUE(importer.ImportAsset("Packages/Models/LegacyReadOnlyHero.gltf"));
    {
        const auto sourceId = ParseAssetId(importer.AssetPathToGUID("Packages/Models/LegacyReadOnlyHero.gltf"));
        const auto databasePath = root / "Library" / "ArtifactDB";
        ArtifactDatabase artifactDatabase;
        ASSERT_TRUE(artifactDatabase.Load(databasePath));
        auto manifest = artifactDatabase.BuildManifestForSource(sourceId);
        ASSERT_TRUE(manifest.has_value());
        manifest->importerVersion = 5u;
        artifactDatabase.UpsertManifest(
            *manifest,
            "Packages/Models/LegacyReadOnlyHero.gltf",
            ArtifactRecordStatus::UpToDate);
        ASSERT_TRUE(artifactDatabase.Save(databasePath));
    }

    auto readOnlyRoots = MakeProjectEditorAssetRoots(root);
    readOnlyRoots.push_back({packageRoot, true, "Packages", root / "Library"});
    AssetDatabaseFacade readOnlyDatabase(readOnlyRoots);
    ASSERT_TRUE(readOnlyDatabase.Refresh());

    EXPECT_FALSE(readOnlyDatabase.IsArtifactManifestCurrentForAssetPath("Packages/Models/LegacyReadOnlyHero.gltf"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ArtifactManifestCurrentUsesGuidFallbackForMissingReadOnlyPrimaryMeta)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto packageRoot = root / "Packages";
    const auto sourcePath = packageRoot / "Materials" / "MissingMeta.mat";
    WriteTextFile(sourcePath, "material");

    auto roots = MakeProjectEditorAssetRoots(root);
    roots.push_back({packageRoot, true, "Packages", root / "Library"});
    AssetDatabaseFacade database(roots);
    ASSERT_TRUE(database.Refresh());
    RegisterCurrentMaterialManifestForMetaFallbackTest(
        database,
        root,
        sourcePath,
        "Packages/Materials/MissingMeta.mat");

    AssetDatabaseFacade restarted(roots);
    ASSERT_TRUE(restarted.Refresh());
    EXPECT_TRUE(restarted.IsArtifactManifestCurrentForAssetPath("Packages/Materials/MissingMeta.mat"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ArtifactManifestCurrentAcceptsContainedReadOnlyPrimaryMeta)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto packageRoot = root / "Packages";
    const auto sourcePath = packageRoot / "Materials" / "ContainedMeta.mat";
    WriteTextFile(sourcePath, "material");
    auto meta = AssetMeta::CreateForAsset(sourcePath);
    meta.id = ParseAssetId("a1111111-1111-4111-8111-111111111111");
    ASSERT_TRUE(meta.Save(GetAssetMetaPath(sourcePath)));

    auto roots = MakeProjectEditorAssetRoots(root);
    roots.push_back({packageRoot, true, "Packages", root / "Library"});
    AssetDatabaseFacade database(roots);
    ASSERT_TRUE(database.Refresh());
    RegisterCurrentMaterialManifestForMetaFallbackTest(
        database,
        root,
        sourcePath,
        "Packages/Materials/ContainedMeta.mat");

    AssetDatabaseFacade restarted(roots);
    ASSERT_TRUE(restarted.Refresh());
    EXPECT_TRUE(restarted.IsArtifactManifestCurrentForAssetPath("Packages/Materials/ContainedMeta.mat"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ArtifactManifestCurrentRejectsReadOnlyPrimaryMetaSymlinkEscape)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto packageRoot = root / "Packages";
    const auto outside = std::filesystem::temp_directory_path() /
        ("nullus_readonly_meta_symlink_outside_" + NLS::Guid::New().ToString());
    const auto sourcePath = packageRoot / "Materials" / "EscapingMeta.mat";
    const auto outsideMetaPath = outside / "EscapingMeta.mat.meta";
    WriteTextFile(sourcePath, "material");
    auto meta = AssetMeta::CreateForAsset(sourcePath);
    meta.id = ParseAssetId("a2222222-2222-4222-8222-222222222222");
    ASSERT_TRUE(meta.Save(outsideMetaPath));

    std::error_code error;
    std::filesystem::create_symlink(outsideMetaPath, GetAssetMetaPath(sourcePath), error);
    if (error)
    {
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(outside);
        GTEST_SKIP() << "File symlink creation is not available in this environment.";
    }

    auto roots = MakeProjectEditorAssetRoots(root);
    roots.push_back({packageRoot, true, "Packages", root / "Library"});
    AssetDatabaseFacade database(roots);
    ASSERT_TRUE(database.Refresh());
    RegisterCurrentMaterialManifestForMetaFallbackTest(
        database,
        root,
        sourcePath,
        "Packages/Materials/EscapingMeta.mat");

    AssetDatabaseFacade restarted(roots);
    ASSERT_TRUE(restarted.Refresh());
    EXPECT_FALSE(restarted.IsArtifactManifestCurrentForAssetPath("Packages/Materials/EscapingMeta.mat"));

    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside);
}

TEST(AssetDatabaseFacadeTests, ArtifactManifestCurrentRejectsMissingWritablePrimaryMeta)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto sourcePath = root / "Assets" / "Materials" / "WritableMissingMeta.mat";
    WriteTextFile(sourcePath, "material");

    const auto roots = MakeProjectEditorAssetRoots(root);
    AssetDatabaseFacade database(roots);
    ASSERT_TRUE(database.Refresh());
    RegisterCurrentMaterialManifestForMetaFallbackTest(
        database,
        root,
        sourcePath,
        "Assets/Materials/WritableMissingMeta.mat");
    ASSERT_TRUE(std::filesystem::remove(GetAssetMetaPath(sourcePath)));

    EXPECT_FALSE(database.IsArtifactManifestCurrentForAssetPath("Assets/Materials/WritableMissingMeta.mat"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ArtifactManifestCurrentRejectsTextureModelMissingTexturePipelineDependency)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroDiffuse.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "Hero.mtl",
        R"(
newmtl HeroMaterial
map_Kd ../Textures/HeroDiffuse.png
)");
    WriteTextFile(
        root / "Assets" / "Models" / "TexturePipelineHero.obj",
        R"(
mtllib Hero.mtl
o Hero
v 0 0 0
v 1 0 0
v 0 1 0
vt 0 0
vt 1 0
vt 0 1
vn 0 0 1
usemtl HeroMaterial
f 1/1/1 2/2/1 3/3/1
)");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    RegisterStandardPbrFreshnessOnlyDependency(database, root);
    ASSERT_TRUE(database.ImportAsset("Assets/Models/TexturePipelineHero.obj"));
    ASSERT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/TexturePipelineHero.obj"));

    RemovePersistedArtifactDependency(
        root,
        ParseAssetId(database.AssetPathToGUID("Assets/Models/TexturePipelineHero.obj")),
        NLS::Core::Assets::AssetDependencyKind::PostprocessorVersion,
        "external-texture-build-pipeline");

    AssetDatabaseFacade restartedDatabase({root});
    ASSERT_TRUE(restartedDatabase.Refresh());
    EXPECT_FALSE(restartedDatabase.IsArtifactManifestCurrentForAssetPath("Assets/Models/TexturePipelineHero.obj"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ArtifactManifestCurrentRejectsStaleModelTextureNameSearchCandidateSet)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto textureId = ParseAssetId("81818181-8181-4181-8181-818181818181");
    const auto modelId = ParseAssetId("82828282-8282-4282-8282-828282828282");
    WriteBinaryFile(root / "Assets" / "Textures" / "SharedWood.png", TinyPng());
    auto textureMeta = AssetMeta::CreateForAsset(root / "Assets" / "Textures" / "SharedWood.png");
    textureMeta.id = textureId;
    textureMeta.assetType = AssetType::Texture;
    ASSERT_TRUE(textureMeta.Save(root / "Assets" / "Textures" / "SharedWood.png.meta"));

    WriteTextFile(
        root / "Assets" / "Models" / "NameSearchHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");
    auto modelMeta = AssetMeta::CreateForAsset(root / "Assets" / "Models" / "NameSearchHero.gltf");
    modelMeta.id = modelId;
    modelMeta.assetType = AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = GetCurrentImporterVersion(AssetType::ModelScene);
    ASSERT_TRUE(modelMeta.Save(root / "Assets" / "Models" / "NameSearchHero.gltf.meta"));

    ArtifactManifest textureManifest;
    textureManifest.sourceAssetId = textureId;
    textureManifest.importerId = "texture";
    textureManifest.importerVersion = GetCurrentImporterVersion(AssetType::Texture);
    textureManifest.targetPlatform = "editor";
    textureManifest.primarySubAssetKey = "texture:main";
    textureManifest.subAssets.push_back({
        textureId,
        "texture:main",
        ArtifactType::Texture,
        "texture",
        "editor",
        ContentStorageArtifactPath(textureId, "texture:main"),
        "hash:shared-wood",
        "SharedWood"
    });
    WritePersistedArtifactManifest(root, textureManifest);
    WriteManifestArtifactFiles(root, textureManifest);

    std::vector<ModelTextureAssetCandidate> candidates;
    candidates.push_back({
        textureId,
        "texture:main",
        "Assets/Textures/SharedWood.png",
        ContentStorageArtifactPath(textureId, "texture:main"),
        "SharedWood",
        AssetType::Texture,
        true,
        0u,
        "hash:shared-wood",
        "SharedWood"
    });

    ArtifactManifest modelManifest;
    modelManifest.sourceAssetId = modelId;
    modelManifest.importerId = modelMeta.importerId;
    modelManifest.importerVersion = modelMeta.importerVersion;
    modelManifest.targetPlatform = "editor";
    modelManifest.primarySubAssetKey = "prefab:NameSearchHero";
    modelManifest.subAssets.push_back(MakeArtifact(
        modelId,
        "prefab:NameSearchHero",
        ArtifactType::Prefab,
        "prefab"));
    AddCurrentSourceDependencies(root, modelManifest, "Assets/Models/NameSearchHero.gltf");
    AddCurrentExternalTextureBuildPipelineDependency(modelManifest);
    modelManifest.dependencies.push_back({
        AssetDependencyKind::PathToGuidMapping,
        MakeModelTextureMappingDependencyValue("SharedWood", "name-search"),
        BuildModelTextureMappingFingerprint(candidates)
    });
    WritePersistedArtifactManifest(root, modelManifest);
    WriteManifestArtifactFiles(root, modelManifest);

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    EXPECT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/NameSearchHero.gltf"));

    WriteBinaryFile(root / "Assets" / "Textures" / "sharedwood.png", TinyPng());
    auto secondTextureMeta = AssetMeta::CreateForAsset(root / "Assets" / "Textures" / "sharedwood.png");
    secondTextureMeta.id = ParseAssetId("83838383-8383-4383-8383-838383838383");
    secondTextureMeta.assetType = AssetType::Texture;
    ASSERT_TRUE(secondTextureMeta.Save(root / "Assets" / "Textures" / "sharedwood.png.meta"));

    ArtifactManifest secondTextureManifest;
    secondTextureManifest.sourceAssetId = secondTextureMeta.id;
    secondTextureManifest.importerId = "texture";
    secondTextureManifest.importerVersion = GetCurrentImporterVersion(AssetType::Texture);
    secondTextureManifest.targetPlatform = "editor";
    secondTextureManifest.primarySubAssetKey = "texture:main";
    secondTextureManifest.subAssets.push_back({
        secondTextureMeta.id,
        "texture:main",
        ArtifactType::Texture,
        "texture",
        "editor",
        ContentStorageArtifactPath(secondTextureMeta.id, "texture:main"),
        "hash:shared-wood-2",
        "sharedwood"
    });
    WritePersistedArtifactManifest(root, secondTextureManifest);
    WriteManifestArtifactFiles(root, secondTextureManifest);

    ASSERT_TRUE(database.Refresh());
    EXPECT_FALSE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/NameSearchHero.gltf"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ArtifactManifestCurrentMatchesNameSearchCandidatesCaseInsensitively)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto textureId = ParseAssetId("84848484-8484-4484-8484-848484848484");
    const auto modelId = ParseAssetId("85858585-8585-4585-8585-858585858585");

    WriteBinaryFile(root / "Assets" / "Textures" / "SharedWood.png", TinyPng());
    auto textureMeta = AssetMeta::CreateForAsset(root / "Assets" / "Textures" / "SharedWood.png");
    textureMeta.id = textureId;
    textureMeta.assetType = AssetType::Texture;
    ASSERT_TRUE(textureMeta.Save(root / "Assets" / "Textures" / "SharedWood.png.meta"));

    WriteTextFile(root / "Assets" / "Models" / "NameSearchCaseHero.gltf", R"({
        "asset": { "version": "2.0" },
        "scene": 0,
        "scenes": [{ "nodes": [0] }],
        "nodes": [{ "name": "Root", "mesh": 0 }]
    })");
    auto modelMeta = AssetMeta::CreateForAsset(root / "Assets" / "Models" / "NameSearchCaseHero.gltf");
    modelMeta.id = modelId;
    modelMeta.assetType = AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = GetCurrentImporterVersion(AssetType::ModelScene);
    ASSERT_TRUE(modelMeta.Save(root / "Assets" / "Models" / "NameSearchCaseHero.gltf.meta"));

    ArtifactManifest textureManifest;
    textureManifest.sourceAssetId = textureId;
    textureManifest.importerId = "texture";
    textureManifest.importerVersion = GetCurrentImporterVersion(AssetType::Texture);
    textureManifest.targetPlatform = "editor";
    textureManifest.primarySubAssetKey = "texture:main";
    textureManifest.subAssets.push_back({
        textureId,
        "texture:main",
        ArtifactType::Texture,
        "texture",
        "editor",
        ContentStorageArtifactPath(textureId, "texture:main"),
        "hash:shared-wood",
        "SharedWood"
    });
    WritePersistedArtifactManifest(root, textureManifest);
    WriteManifestArtifactFiles(root, textureManifest);

    ArtifactManifest modelManifest;
    modelManifest.sourceAssetId = modelId;
    modelManifest.importerId = modelMeta.importerId;
    modelManifest.importerVersion = modelMeta.importerVersion;
    modelManifest.targetPlatform = "editor";
    modelManifest.primarySubAssetKey = "prefab:NameSearchCaseHero";
    modelManifest.subAssets.push_back(MakeArtifact(
        modelId,
        "prefab:NameSearchCaseHero",
        ArtifactType::Prefab,
        "prefab"));
    AddCurrentSourceDependencies(root, modelManifest, "Assets/Models/NameSearchCaseHero.gltf");
    AddCurrentExternalTextureBuildPipelineDependency(modelManifest);
    modelManifest.dependencies.push_back({
        AssetDependencyKind::PathToGuidMapping,
        MakeModelTextureMappingDependencyValue("SHAREDWOOD", "name-search"),
        BuildModelTextureMappingFingerprint({
            {
                textureId,
                "texture:main",
                "Assets/Textures/SharedWood.png",
                ContentStorageArtifactPath(textureId, "texture:main"),
                "SharedWood",
                AssetType::Texture,
                true,
                0u,
                "hash:shared-wood",
                "SharedWood"
            }
        })
    });
    WritePersistedArtifactManifest(root, modelManifest);
    WriteManifestArtifactFiles(root, modelManifest);

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    EXPECT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/NameSearchCaseHero.gltf"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ArtifactManifestCurrentKeepsUnimportedTextureMappingCandidates)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto textureId = ParseAssetId("86868686-8686-4686-8686-868686868686");
    const auto modelId = ParseAssetId("87878787-8787-4787-8787-878787878787");

    WriteBinaryFile(root / "Assets" / "Textures" / "MissingArtifactAlbedo.png", TinyPng());
    auto textureMeta = AssetMeta::CreateForAsset(root / "Assets" / "Textures" / "MissingArtifactAlbedo.png");
    textureMeta.id = textureId;
    textureMeta.assetType = AssetType::Texture;
    textureMeta.importerId = "texture";
    textureMeta.importerVersion = GetCurrentImporterVersion(AssetType::Texture);
    ASSERT_TRUE(textureMeta.Save(root / "Assets" / "Textures" / "MissingArtifactAlbedo.png.meta"));

    WriteTextFile(root / "Assets" / "Models" / "MissingArtifactHero.gltf", R"({
        "asset": { "version": "2.0" },
        "scene": 0,
        "scenes": [{ "nodes": [0] }],
        "nodes": [{ "name": "Root", "mesh": 0 }]
    })");
    auto modelMeta = AssetMeta::CreateForAsset(root / "Assets" / "Models" / "MissingArtifactHero.gltf");
    modelMeta.id = modelId;
    modelMeta.assetType = AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = GetCurrentImporterVersion(AssetType::ModelScene);
    ASSERT_TRUE(modelMeta.Save(root / "Assets" / "Models" / "MissingArtifactHero.gltf.meta"));

    ArtifactManifest textureManifest;
    textureManifest.sourceAssetId = textureId;
    textureManifest.importerId = "texture";
    textureManifest.importerVersion = GetCurrentImporterVersion(AssetType::Texture);
    textureManifest.targetPlatform = "editor";
    textureManifest.primarySubAssetKey = "texture:main";

    ArtifactManifest modelManifest;
    modelManifest.sourceAssetId = modelId;
    modelManifest.importerId = modelMeta.importerId;
    modelManifest.importerVersion = modelMeta.importerVersion;
    modelManifest.targetPlatform = "editor";
    modelManifest.primarySubAssetKey = "prefab:MissingArtifactHero";
    modelManifest.subAssets.push_back(MakeArtifact(
        modelId,
        "prefab:MissingArtifactHero",
        ArtifactType::Prefab,
        "prefab"));
    AddCurrentSourceDependencies(root, modelManifest, "Assets/Models/MissingArtifactHero.gltf");
    AddCurrentExternalTextureBuildPipelineDependency(modelManifest);
    modelManifest.dependencies.push_back({
        AssetDependencyKind::PathToGuidMapping,
        MakeModelTextureMappingDependencyValue("Assets/Textures/MissingArtifactAlbedo.png", "source-path"),
        BuildModelTextureMappingFingerprint({
            {
                textureId,
                {},
                "Assets/Textures/MissingArtifactAlbedo.png",
                {},
                "MissingArtifactAlbedo",
                AssetType::Texture,
                false,
                0u
            }
        })
    });
    WritePersistedArtifactManifest(root, modelManifest);
    WriteManifestArtifactFiles(root, modelManifest);

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    EXPECT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/MissingArtifactHero.gltf"));

    textureManifest.subAssets.push_back({
        textureId,
        "texture:main",
        ArtifactType::Texture,
        "texture",
        "editor",
        ContentStorageArtifactPath(textureId, "texture:main"),
        "hash:now-imported",
        "MissingArtifactAlbedo"
    });
    WritePersistedArtifactManifest(root, textureManifest);
    WriteManifestArtifactFiles(root, textureManifest);

    ASSERT_TRUE(database.Refresh());
    EXPECT_FALSE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/MissingArtifactHero.gltf"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ImportedModelManifestRecordsExternalSourceDependencies)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "Hero.bin", "mesh-binary");
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroBaseColor.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "Hero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "buffers": [
                { "uri": "Hero.bin", "byteLength": 11 }
            ],
            "images": [
                { "uri": "../Textures/HeroBaseColor.png", "mimeType": "image/png" }
            ],
            "materials": [
                {
                    "name": "HeroMaterial",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "textures": [
                { "source": 0 }
            ]
        })");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    RegisterStandardPbrFreshnessOnlyDependency(database, root);
    ASSERT_TRUE(database.ImportAsset("Assets/Models/Hero.gltf"));

    const auto sourceId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Hero.gltf"));
    const auto manifest = LoadPersistedArtifactManifest(root, sourceId);
    ASSERT_TRUE(manifest.has_value());
    EXPECT_TRUE(ContainsManifestDependency(*manifest, NLS::Core::Assets::AssetDependencyKind::SourceFileHash, "Assets/Models/Hero.gltf"));
    EXPECT_TRUE(ContainsManifestDependency(*manifest, NLS::Core::Assets::AssetDependencyKind::SourceFileHash, "Assets/Models/Hero.bin"));
    EXPECT_TRUE(ContainsManifestDependency(*manifest, NLS::Core::Assets::AssetDependencyKind::SourceFileHash, "Assets/Textures/HeroBaseColor.png"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ImportedObjManifestRecordsMtlAndTextureDependencies)
{
    using namespace NLS::Editor::Assets;

    EnsureAssetDatabaseFacadeTestDriver();
    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroDiffuse.png", TinyPng());
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroNormal.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "Hero.mtl",
        R"(
newmtl HeroMaterial
Kd 1.0 1.0 1.0
map_Kd -s 1 1 1 ../Textures/HeroDiffuse.png
)");
    WriteTextFile(
        root / "Assets" / "Models" / "HeroExtra.mtl",
        R"(
newmtl HeroMaterialExtra
map_Bump ../Textures/HeroNormal.png
)");
    WriteTextFile(
        root / "Assets" / "Models" / "Hero.obj",
        R"(
mtllib Hero.mtl HeroExtra.mtl
o Hero
v 0 0 0
v 1 0 0
v 0 1 0
vt 0 0
vt 1 0
vt 0 1
vn 0 0 1
usemtl HeroMaterial
f 1/1/1 2/2/1 3/3/1
)");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    RegisterStandardPbrFreshnessOnlyDependency(database, root);
    ASSERT_TRUE(database.ImportAsset("Assets/Models/Hero.obj"));

    const auto sourceId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Hero.obj"));
    const auto manifest = LoadPersistedArtifactManifest(root, sourceId);
    ASSERT_TRUE(manifest.has_value());
    EXPECT_TRUE(ContainsManifestDependency(*manifest, NLS::Core::Assets::AssetDependencyKind::SourceFileHash, "Assets/Models/Hero.obj"));
    EXPECT_TRUE(ContainsManifestDependency(*manifest, NLS::Core::Assets::AssetDependencyKind::SourceFileHash, "Assets/Models/Hero.mtl"));
    EXPECT_TRUE(ContainsManifestDependency(*manifest, NLS::Core::Assets::AssetDependencyKind::SourceFileHash, "Assets/Models/HeroExtra.mtl"));
    EXPECT_TRUE(ContainsManifestDependency(*manifest, NLS::Core::Assets::AssetDependencyKind::SourceFileHash, "Assets/Textures/HeroDiffuse.png"));
    EXPECT_TRUE(ContainsManifestDependency(*manifest, NLS::Core::Assets::AssetDependencyKind::SourceFileHash, "Assets/Textures/HeroNormal.png"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ImportedAssimpModelManifestRecordsParserTextureDependencies)
{
#if !NLS_HAS_AUTODESK_FBX_SDK
    GTEST_SKIP() << "FBX import success requires Autodesk FBX SDK.";
#endif

    using namespace NLS::Editor::Assets;

    EnsureAssetDatabaseFacadeTestDriver();
    const auto sourceRoot =
        std::filesystem::current_path() /
        "ThirdParty" / "assimp" / "test" / "models-nonbsd" / "FBX" / "2013_ASCII";
    const auto sourceFbx = sourceRoot / "jeep1.fbx";
    const auto sourceTexture = sourceRoot / "jeep1.jpg";
    ASSERT_TRUE(std::filesystem::exists(sourceFbx));
    ASSERT_TRUE(std::filesystem::exists(sourceTexture));

    const auto root = MakeAssetDatabaseFacadeRoot();
    std::filesystem::create_directories(root / "Assets" / "Models");
    std::filesystem::copy_file(
        sourceFbx,
        root / "Assets" / "Models" / "jeep1.fbx",
        std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(
        sourceTexture,
        root / "Assets" / "Models" / "jeep1.jpg",
        std::filesystem::copy_options::overwrite_existing);

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ImportProgressTracker tracker;
    ASSERT_TRUE(database.ImportAsset("Assets/Models/jeep1.fbx", tracker));

    bool reportedSecondSourceMeshBuild = false;
    for (const auto& event : tracker.GetEvents({1u}))
    {
        if (event.message == "Building native mesh cache")
            reportedSecondSourceMeshBuild = true;
    }
    EXPECT_FALSE(reportedSecondSourceMeshBuild);

    const auto sourceId = ParseAssetId(database.AssetPathToGUID("Assets/Models/jeep1.fbx"));
    const auto manifest = LoadPersistedArtifactManifest(root, sourceId);
    ASSERT_TRUE(manifest.has_value());
    EXPECT_TRUE(ContainsManifestDependency(*manifest, NLS::Core::Assets::AssetDependencyKind::SourceFileHash, "Assets/Models/jeep1.fbx"));
    EXPECT_TRUE(ContainsManifestDependency(*manifest, NLS::Core::Assets::AssetDependencyKind::SourceFileHash, "Assets/Models/jeep1.jpg"));

    const auto allAssets = database.LoadAllAssetsAtPath("Assets/Models/jeep1.fbx");
    const auto meshAsset = std::find_if(
        allAssets.begin(),
        allAssets.end(),
        [](const AssetDatabaseRecord& asset)
        {
            return asset.artifactType == NLS::Core::Assets::ArtifactType::Mesh;
        });
    ASSERT_NE(meshAsset, allAssets.end());
    const auto meshArtifact = NLS::Render::Assets::LoadMeshArtifact(meshAsset->artifactPath);
    ASSERT_TRUE(meshArtifact.has_value());
    EXPECT_GT(meshArtifact->vertices.size(), 0u);
    EXPECT_GT(meshArtifact->indices.size(), 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, FailedAssimpModelImportDoesNotCommitEmptyArtifacts)
{
    using namespace NLS::Editor::Assets;

    EnsureAssetDatabaseFacadeTestDriver();
    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "Broken.fbx", "not a valid fbx model");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    RegisterStandardPbrFreshnessOnlyDependency(database, root);
    ImportProgressTracker tracker;
    EXPECT_FALSE(database.ImportAsset("Assets/Models/Broken.fbx", tracker));

    NLS::Core::Assets::ArtifactDatabase artifactDatabase;
    if (artifactDatabase.Load(root / "Library" / "ArtifactDB"))
    {
        const auto sourceId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Broken.fbx"));
        EXPECT_FALSE(artifactDatabase.BuildManifestForSource(sourceId).has_value());
    }
    EXPECT_FALSE(database.GetArtifactManifestForAssetPath("Assets/Models/Broken.fbx").has_value());

    bool reportedFailure = false;
    for (const auto& event : tracker.GetEvents({1u}))
    {
        if (event.terminalStatus == ImportJobTerminalStatus::Failed)
            reportedFailure = true;
    }
    EXPECT_TRUE(reportedFailure);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ImportedModelMeshArtifactMergesMultiplePrimitives)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "TwoTriangles.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
            "buffers": [
                {
                    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIAAAAAAAAAAAAAAIA/AAAAPwAAgD8AAAAAAAAAPwAAAAAAAIA/AAABAAIA",
                    "byteLength": 84
                }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 },
                { "buffer": 0, "byteOffset": 42, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 78, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" },
                { "bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "meshes": [
                {
                    "name": "Double",
                    "primitives": [
                        { "attributes": { "POSITION": 0 }, "indices": 1 },
                        { "attributes": { "POSITION": 2 }, "indices": 3 }
                    ]
                }
            ],
            "nodes": [
                { "name": "DoubleRoot", "mesh": 0 }
            ]
        })");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    RegisterStandardPbrFreshnessOnlyDependency(database, root);
    ASSERT_TRUE(database.ImportAsset("Assets/Models/TwoTriangles.gltf"));

    const auto firstPrimitiveRecord = database.LoadSubAssetAtPath(
        "Assets/Models/TwoTriangles.gltf",
        "mesh:mesh/0/primitive/0");
    const auto secondPrimitiveRecord = database.LoadSubAssetAtPath(
        "Assets/Models/TwoTriangles.gltf",
        "mesh:mesh/0/primitive/1");
    ASSERT_TRUE(firstPrimitiveRecord.has_value());
    ASSERT_TRUE(secondPrimitiveRecord.has_value());
    const auto firstPrimitiveArtifact = NLS::Render::Assets::LoadMeshArtifact(
        firstPrimitiveRecord->artifactPath);
    const auto secondPrimitiveArtifact = NLS::Render::Assets::LoadMeshArtifact(
        secondPrimitiveRecord->artifactPath);

    ASSERT_TRUE(firstPrimitiveArtifact.has_value());
    ASSERT_TRUE(secondPrimitiveArtifact.has_value());
    EXPECT_EQ(firstPrimitiveArtifact->vertices.size(), 3u);
    EXPECT_EQ(firstPrimitiveArtifact->indices.size(), 3u);
    EXPECT_EQ(secondPrimitiveArtifact->vertices.size(), 3u);
    EXPECT_EQ(secondPrimitiveArtifact->indices.size(), 3u);
    EXPECT_EQ(secondPrimitiveArtifact->indices[0], 0u);
    EXPECT_EQ(secondPrimitiveArtifact->indices[1], 1u);
    EXPECT_EQ(secondPrimitiveArtifact->indices[2], 2u);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ReimportAssetRefreshesStaleNativeMeshArtifact)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto assetPath = root / "Assets" / "Models" / "Reimported.gltf";
    WriteTextFile(
        assetPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
            "buffers": [
                {
                    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA",
                    "byteLength": 42
                }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "meshes": [
                {
                    "name": "Single",
                    "primitives": [
                        { "attributes": { "POSITION": 0 }, "indices": 1 }
                    ]
                }
            ],
            "nodes": [
                { "name": "SingleRoot", "mesh": 0 }
            ]
        })");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    RegisterStandardPbrFreshnessOnlyDependency(database, root);
    ASSERT_TRUE(database.ImportAsset("Assets/Models/Reimported.gltf"));
    auto meshRecord = database.LoadSubAssetAtPath("Assets/Models/Reimported.gltf", "mesh:mesh/0");
    ASSERT_TRUE(meshRecord.has_value());
    const auto meshPath = std::filesystem::path(meshRecord->artifactPath);
    auto meshArtifact = NLS::Render::Assets::LoadMeshArtifact(meshPath);
    ASSERT_TRUE(meshArtifact.has_value());
    EXPECT_EQ(meshArtifact->vertices.size(), 3u);

    WriteTextFile(
        assetPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
            "buffers": [
                {
                    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIAAAAAAAAAAAAAAIA/AAAAPwAAgD8AAAAAAAAAPwAAAAAAAIA/AAABAAIA",
                    "byteLength": 84
                }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 },
                { "buffer": 0, "byteOffset": 42, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 78, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" },
                { "bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "meshes": [
                {
                    "name": "Double",
                    "primitives": [
                        { "attributes": { "POSITION": 0 }, "indices": 1 },
                        { "attributes": { "POSITION": 2 }, "indices": 3 }
                    ]
                }
            ],
            "nodes": [
                { "name": "DoubleRoot", "mesh": 0 }
            ]
        })");

    ASSERT_TRUE(database.ReimportAsset("Assets/Models/Reimported.gltf"));
    const auto firstPrimitiveRecord = database.LoadSubAssetAtPath(
        "Assets/Models/Reimported.gltf",
        "mesh:mesh/0/primitive/0");
    const auto secondPrimitiveRecord = database.LoadSubAssetAtPath(
        "Assets/Models/Reimported.gltf",
        "mesh:mesh/0/primitive/1");
    ASSERT_TRUE(firstPrimitiveRecord.has_value());
    ASSERT_TRUE(secondPrimitiveRecord.has_value());
    const auto firstPrimitivePath = std::filesystem::path(firstPrimitiveRecord->artifactPath);
    const auto secondPrimitivePath = std::filesystem::path(secondPrimitiveRecord->artifactPath);
    EXPECT_FALSE(database.LoadSubAssetAtPath("Assets/Models/Reimported.gltf", "mesh:mesh/0").has_value());
    const auto reimportedManifest = database.GetArtifactManifestForAssetPath("Assets/Models/Reimported.gltf");
    ASSERT_TRUE(reimportedManifest.has_value());
    EXPECT_EQ(reimportedManifest->FindSubAsset("mesh:mesh/0"), nullptr);
    EXPECT_NE(reimportedManifest->FindSubAsset("mesh:mesh/0/primitive/0"), nullptr);
    EXPECT_NE(reimportedManifest->FindSubAsset("mesh:mesh/0/primitive/1"), nullptr);

    const auto firstPrimitiveArtifact = NLS::Render::Assets::LoadMeshArtifact(firstPrimitivePath);
    const auto secondPrimitiveArtifact = NLS::Render::Assets::LoadMeshArtifact(secondPrimitivePath);
    ASSERT_TRUE(firstPrimitiveArtifact.has_value());
    ASSERT_TRUE(secondPrimitiveArtifact.has_value());
    EXPECT_EQ(firstPrimitiveArtifact->vertices.size(), 3u);
    EXPECT_EQ(firstPrimitiveArtifact->indices.size(), 3u);
    EXPECT_EQ(secondPrimitiveArtifact->vertices.size(), 3u);
    EXPECT_EQ(secondPrimitiveArtifact->indices.size(), 3u);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ReimportAssetRefreshesNativeTextureArtifactsAndCentralIndex)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto assetPath = root / "Assets" / "Models" / "Textured.gltf";
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroBaseColor.png", TinyPng());
    WriteTextFile(
        assetPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "images": [
                { "uri": "../Textures/HeroBaseColor.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "HeroMaterial",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "meshes": [
                {
                    "name": "HeroMesh",
                    "primitives": [
                        { "attributes": {}, "material": 0 }
                    ]
                }
            ],
            "nodes": [
                { "name": "HeroRoot", "mesh": 0 }
            ]
        })");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    RegisterStandardPbrFreshnessOnlyDependency(database, root);
    ASSERT_TRUE(database.ImportAsset("Assets/Models/Textured.gltf"));
    ASSERT_TRUE(database.ReimportAsset("Assets/Models/Textured.gltf"));

    ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB"));
    const auto sourceId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Textured.gltf"));
    const auto textureAssetId = ParseAssetId(database.AssetPathToGUID("Assets/Textures/HeroBaseColor.png"));
    const auto* textureRecord = artifactDatabase.Find(textureAssetId, "texture:main", "win64-dx12");
    if (textureRecord == nullptr)
        textureRecord = artifactDatabase.Find(textureAssetId, "texture:main", "editor");
    ASSERT_NE(textureRecord, nullptr);
    EXPECT_EQ(textureRecord->artifactType, ArtifactType::Texture);
    EXPECT_EQ(textureRecord->loaderId, "texture");
    EXPECT_FALSE(std::filesystem::path(textureRecord->artifactPath).is_absolute());
    EXPECT_TRUE(IsContentStorageArtifactPath(textureRecord->artifactPath)) << textureRecord->artifactPath;
    EXPECT_EQ(textureRecord->artifactPath.find("Library/Artifacts/"), 0u) << textureRecord->artifactPath;

    const auto texturePath = root / textureRecord->artifactPath;
    EXPECT_TRUE(std::filesystem::exists(texturePath));

    const auto textureManifest = database.GetArtifactManifestForAssetPath("Assets/Textures/HeroBaseColor.png");
    ASSERT_TRUE(textureManifest.has_value());
    ASSERT_NE(textureManifest->FindSubAsset("texture:main"), nullptr);

    const auto textureSubAsset = database.LoadSubAssetAtPath("Assets/Textures/HeroBaseColor.png", "texture:main");
    ASSERT_TRUE(textureSubAsset.has_value());
    EXPECT_EQ(textureSubAsset->artifactType, ArtifactType::Texture);
    EXPECT_TRUE(std::filesystem::equivalent(textureSubAsset->artifactPath, texturePath));

    const auto artifact = NLS::Render::Assets::LoadTextureArtifact(texturePath);
    ASSERT_TRUE(artifact.has_value());
    ASSERT_FALSE(artifact->mips.empty());
    EXPECT_EQ(artifact->width, 1u);
    EXPECT_EQ(artifact->height, 1u);
    EXPECT_FALSE(artifact->targetPlatform.empty());
    EXPECT_FALSE(artifact->encoderId.empty());
#if defined(_WIN32) && NLS_HAS_DIRECTXTEX
    EXPECT_EQ(artifact->targetPlatform, "win64-dx12");
    EXPECT_EQ(artifact->format, NLS::Render::RHI::TextureFormat::BC1);
    EXPECT_EQ(artifact->encoderId, "directxtex-bc");
#else
    EXPECT_EQ(artifact->format, NLS::Render::RHI::TextureFormat::RGBA8);
    EXPECT_EQ(artifact->encoderId, "rgba8-passthrough");
#endif

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedAssetDatabaseFacadeDriverService driverService(driver);
    auto explicitDevice = std::make_shared<TextureReimportTestDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto* texture = NLS::Render::Resources::Loaders::TextureLoader::Create(
        texturePath.string(),
        NLS::Render::Settings::ETextureFilteringMode::LINEAR,
        NLS::Render::Settings::ETextureFilteringMode::LINEAR,
        false);
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->width, artifact->width);
    EXPECT_EQ(texture->height, artifact->height);
    EXPECT_EQ(texture->isMimapped, artifact->mips.size() > 1u);
    EXPECT_EQ(texture->path, texturePath.string());
    EXPECT_GE(explicitDevice->textureCreateCalls, 1u);
    EXPECT_EQ(explicitDevice->lastTextureDesc.format, artifact->format);
    EXPECT_EQ(explicitDevice->lastTextureDesc.mipLevels, artifact->mips.size());
    EXPECT_GT(explicitDevice->lastTextureUploadDesc.dataSize, 0u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(texture));
    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, StandaloneTextureImportEmitsAssetImportStages)
{
    using namespace NLS::Base::Profiling;
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroBaseColor.png", TinyPng());

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    PerformanceStageStats stats;
    {
        PerformanceStageStatsCapture capture(stats);
        ASSERT_TRUE(database.ImportAsset("Assets/Textures/HeroBaseColor.png"));
    }

    const auto warnings = FindMissingPerformanceStages(
        stats.Snapshot(),
        PerformanceStageDomain::AssetImport,
        {
            "ImportStandaloneTextureArtifact",
            "StandaloneTextureReadSource",
            "StandaloneTextureDecodeAndMip",
            "StandaloneTextureAlphaScan",
            "StandaloneTextureResolveBuildSettings",
            "StandaloneTextureBuildIdentity",
            "StandaloneTextureSerializeArtifact",
            "StandaloneTextureCommitArtifact",
        });
    EXPECT_TRUE(warnings.empty());

    const auto manifest = database.GetArtifactManifestForAssetPath("Assets/Textures/HeroBaseColor.png");
    ASSERT_TRUE(manifest.has_value());
    const auto* artifact = manifest->FindPrimaryArtifact();
    ASSERT_NE(artifact, nullptr);
    EXPECT_EQ(artifact->artifactType, ArtifactType::Texture);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, FailedReimportKeepsPreviousNativeMeshArtifact)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto assetPath = root / "Assets" / "Models" / "Stable.gltf";
    WriteTextFile(
        assetPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
            "buffers": [
                {
                    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA",
                    "byteLength": 42
                }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "meshes": [
                {
                    "name": "StableMesh",
                    "primitives": [
                        { "attributes": { "POSITION": 0 }, "indices": 1 }
                    ]
                }
            ],
            "nodes": [
                { "name": "StableRoot", "mesh": 0 }
            ]
        })");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    RegisterStandardPbrFreshnessOnlyDependency(database, root);
    ASSERT_TRUE(database.ImportAsset("Assets/Models/Stable.gltf"));
    const auto meshRecord = database.LoadSubAssetAtPath("Assets/Models/Stable.gltf", "mesh:mesh/0");
    ASSERT_TRUE(meshRecord.has_value());
    const auto meshPath = std::filesystem::path(meshRecord->artifactPath);
    auto meshArtifact = NLS::Render::Assets::LoadMeshArtifact(meshPath);
    ASSERT_TRUE(meshArtifact.has_value());
    EXPECT_EQ(meshArtifact->vertices.size(), 3u);

    std::filesystem::remove(assetPath);

    EXPECT_FALSE(database.ReimportAsset("Assets/Models/Stable.gltf"));
    meshArtifact = NLS::Render::Assets::LoadMeshArtifact(meshPath);
    ASSERT_TRUE(meshArtifact.has_value());
    EXPECT_EQ(meshArtifact->vertices.size(), 3u);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, FailedReimportRollsBackCommittedArtifactsWhenManifestCannotBeSaved)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto assetPath = root / "Assets" / "Models" / "Transactional.gltf";
    WriteTextFile(
        assetPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
            "buffers": [
                {
                    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA",
                    "byteLength": 42
                }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "meshes": [
                {
                    "name": "StableMesh",
                    "primitives": [
                        { "attributes": { "POSITION": 0 }, "indices": 1 }
                    ]
                }
            ],
            "nodes": [
                { "name": "StableRoot", "mesh": 0 }
            ]
        })");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    RegisterStandardPbrFreshnessOnlyDependency(database, root);
    ASSERT_TRUE(database.ImportAsset("Assets/Models/Transactional.gltf"));
    const auto meshRecord = database.LoadSubAssetAtPath("Assets/Models/Transactional.gltf", "mesh:mesh/0");
    ASSERT_TRUE(meshRecord.has_value());
    const auto meshPath = std::filesystem::path(meshRecord->artifactPath);
    const auto databasePath = root / "Library" / "ArtifactDB";
    auto meshArtifact = NLS::Render::Assets::LoadMeshArtifact(meshPath);
    ASSERT_TRUE(meshArtifact.has_value());
    ASSERT_EQ(meshArtifact->vertices.size(), 3u);
    const auto originalMeshBytes = std::filesystem::file_size(meshPath);

    WriteTextFile(
        assetPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
            "buffers": [
                {
                    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIAAAAAAAAAAAAAAIA/AAAAPwAAgD8AAAAAAAAAPwAAAAAAAIA/AAABAAIA",
                    "byteLength": 84
                }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 },
                { "buffer": 0, "byteOffset": 42, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 78, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" },
                { "bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "meshes": [
                {
                    "name": "DoubleMesh",
                    "primitives": [
                        { "attributes": { "POSITION": 0 }, "indices": 1 },
                        { "attributes": { "POSITION": 2 }, "indices": 3 }
                    ]
                }
            ],
            "nodes": [
                { "name": "DoubleRoot", "mesh": 0 }
            ]
        })");

    std::filesystem::remove_all(databasePath);
    WriteTextFile(databasePath, "not an lmdb environment\n");

    EXPECT_FALSE(database.ReimportAsset("Assets/Models/Transactional.gltf"));
    EXPECT_TRUE(std::filesystem::is_regular_file(databasePath));
    EXPECT_EQ(std::filesystem::file_size(meshPath), originalMeshBytes);

    meshArtifact = NLS::Render::Assets::LoadMeshArtifact(meshPath);
    ASSERT_TRUE(meshArtifact.has_value());
    EXPECT_EQ(meshArtifact->vertices.size(), 3u);
    EXPECT_EQ(meshArtifact->indices.size(), 3u);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, FailedPrefabReimportRollsBackCommittedPayloadWhenManifestCannotBeSaved)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto assetPath = root / "Assets" / "Prefabs" / "Transactional.prefab";
    NLS::Engine::GameObject stable("Stable", "Prefab");
    auto stablePrefab = PrefabEditorWorkflow().CreatePrefabFromSelection({
        &stable,
        {},
        ParseAssetId("11111111-1111-4111-8111-111111111111"),
        "Assets/Prefabs/Transactional.prefab"
    });
    ASSERT_EQ(stablePrefab.status, PrefabEditorOperationStatus::Committed);
    WriteTextFile(assetPath, stablePrefab.prefabSourceText);

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Prefabs/Transactional.prefab"));

    const auto prefabRecord = database.LoadSubAssetAtPath("Assets/Prefabs/Transactional.prefab", "prefab:Transactional");
    ASSERT_TRUE(prefabRecord.has_value());
    const auto prefabPayloadPath = std::filesystem::path(prefabRecord->artifactPath);
    const auto databasePath = root / "Library" / "ArtifactDB";
    ASSERT_TRUE(std::filesystem::exists(prefabPayloadPath));
    ASSERT_TRUE(std::filesystem::exists(databasePath));
    const auto originalPayloadBytes = std::filesystem::file_size(prefabPayloadPath);

    NLS::Engine::GameObject changed("ChangedWithLongerPayload", "UpdatedPrefabTag");
    auto changedPrefab = PrefabEditorWorkflow().CreatePrefabFromSelection({
        &changed,
        {},
        ParseAssetId("22222222-2222-4222-8222-222222222222"),
        "Assets/Prefabs/Transactional.prefab"
    });
    ASSERT_EQ(changedPrefab.status, PrefabEditorOperationStatus::Committed);
    WriteTextFile(assetPath, changedPrefab.prefabSourceText);
    std::filesystem::remove_all(databasePath);
    WriteTextFile(databasePath, "not an lmdb environment\n");

    EXPECT_FALSE(database.ReimportAsset("Assets/Prefabs/Transactional.prefab"));
    EXPECT_TRUE(std::filesystem::is_regular_file(databasePath));
    EXPECT_EQ(std::filesystem::file_size(prefabPayloadPath), originalPayloadBytes);

    auto prefab = database.LoadPrefabArtifactAtPath(
        "Assets/Prefabs/Transactional.prefab",
        "prefab:Transactional");
    EXPECT_FALSE(prefab.has_value());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, RefreshClearsWarmPrefabStateWhenPersistedManifestCannotBeRead)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto assetPath = root / "Assets" / "Prefabs" / "BrokenManifest.prefab";
    NLS::Engine::GameObject stable("Stable", "Prefab");
    auto stablePrefab = PrefabEditorWorkflow().CreatePrefabFromSelection({
        &stable,
        {},
        ParseAssetId("33333333-3333-4333-8333-333333333333"),
        "Assets/Prefabs/BrokenManifest.prefab"
    });
    ASSERT_EQ(stablePrefab.status, PrefabEditorOperationStatus::Committed);
    WriteTextFile(assetPath, stablePrefab.prefabSourceText);

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Prefabs/BrokenManifest.prefab"));
    ASSERT_TRUE(database
        .LoadPrefabArtifactAtPath("Assets/Prefabs/BrokenManifest.prefab", "prefab:BrokenManifest")
        .has_value());

    const auto databasePath = root / "Library" / "ArtifactDB";
    std::filesystem::remove_all(databasePath);
    std::filesystem::create_directories(databasePath);

    ASSERT_TRUE(database.Refresh());
    EXPECT_FALSE(database
        .LoadPrefabArtifactAtPath("Assets/Prefabs/BrokenManifest.prefab", "prefab:BrokenManifest")
        .has_value());
    ASSERT_FALSE(database.GetDiagnostics().empty());
    EXPECT_NE(database.GetDiagnostics().back().message.find("ArtifactDB could not be read"), std::string::npos);
    EXPECT_NE(database.GetDiagnostics().back().message.find("mdb_env_open"), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ImportsSameStemGltfAndFbxIntoSeparateGuidArtifactRoots)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "Sponza.gltf",
        R"({
            "asset": { "version": "2.0" },
            "meshes": [
                {
                    "name": "GltfBody",
                    "primitives": [
                        { "attributes": { "POSITION": 0 } }
                    ]
                }
            ],
            "nodes": [
                { "name": "SponzaGltfRoot", "mesh": 0 }
            ]
        })");
    WriteTextFile(root / "Assets" / "Models" / "Sponza.fbx", "placeholder fbx");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto gltfGuid = database.AssetPathToGUID("Assets/Models/Sponza.gltf");
    const auto fbxGuid = database.AssetPathToGUID("Assets/Models/Sponza.fbx");
    ASSERT_FALSE(gltfGuid.empty());
    ASSERT_FALSE(fbxGuid.empty());
    ASSERT_NE(gltfGuid, fbxGuid);

    const auto gltfRoot = database.GetArtifactRootForAssetPathForTesting("Assets/Models/Sponza.gltf");
    const auto fbxRoot = database.GetArtifactRootForAssetPathForTesting("Assets/Models/Sponza.fbx");
    EXPECT_EQ(gltfRoot, root / "Library" / "Artifacts");
    EXPECT_EQ(fbxRoot, root / "Library" / "Artifacts");
    EXPECT_EQ(gltfRoot, fbxRoot);
    EXPECT_NE(gltfRoot, root / "Library" / "Artifacts" / "Sponza");
    EXPECT_NE(fbxRoot, root / "Library" / "Artifacts" / "Sponza");

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, AssetBrowserHidesImportedModelGeneratedPrefabSubAsset)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "Hero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "meshes": [
                {
                    "name": "Body",
                    "primitives": [
                        { "attributes": { "POSITION": 0 } }
                    ]
                }
            ],
            "nodes": [
                { "name": "HeroRoot", "mesh": 0 }
            ]
        })");

    {
        AssetDatabaseFacade importer({root});
        ASSERT_TRUE(importer.Refresh());
        RegisterStandardPbrFreshnessOnlyDependency(importer, root);
        ASSERT_TRUE(importer.ImportAsset("Assets/Models/Hero.gltf"));
    }

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto entries = BuildAssetBrowserSubAssetEntries(database, "Assets/Models/Hero.gltf");
    EXPECT_TRUE(std::none_of(
        entries.begin(),
        entries.end(),
        [](const AssetBrowserSubAssetEntry& entry)
        {
            return entry.artifactType == ArtifactType::Prefab || entry.subAssetKey.rfind("prefab:", 0) == 0;
        }));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, AssetBrowserExposesImportedModelReferenceableSubAssetsForInspectorDrag)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto modelId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Hero.gltf"));
    ArtifactManifest manifest;
    manifest.sourceAssetId = modelId;
    manifest.importerId = "scene-model";
    manifest.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "model:Hero";
    auto makeSafeArtifact = [modelId](
        const std::string& subAssetKey,
        const ArtifactType artifactType,
        const std::string& loaderId)
    {
        return MakeArtifact(modelId, subAssetKey, artifactType, loaderId);
    };
    manifest.subAssets.push_back(makeSafeArtifact("model:Hero", ArtifactType::Model, "model"));
    manifest.subAssets.push_back(makeSafeArtifact("prefab:Hero", ArtifactType::Prefab, "prefab"));
    manifest.subAssets.push_back(makeSafeArtifact("mesh:Body", ArtifactType::Mesh, "mesh"));
    manifest.subAssets.push_back(makeSafeArtifact("material:Body", ArtifactType::Material, "material"));
    manifest.subAssets.push_back(makeSafeArtifact("texture:Albedo", ArtifactType::Texture, "texture"));
    manifest.subAssets.push_back(makeSafeArtifact("shader:HeroSurface", ArtifactType::Shader, "shader"));
    manifest.subAssets.push_back(makeSafeArtifact("animation:Idle", ArtifactType::AnimationClip, "animation"));
    WriteManifestArtifactFiles(root, manifest);
    AddCurrentSourceDependencies(root, manifest, "Assets/Models/Hero.gltf");
    AddCurrentExternalTextureBuildPipelineDependency(manifest);
    database.AddArtifactManifest(manifest);

    ASSERT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/Hero.gltf"));
    ASSERT_EQ(database.LoadAllAssetsAtPath("Assets/Models/Hero.gltf").size(), 7u);
    const auto entries = BuildAssetBrowserSubAssetEntries(database, "Assets/Models/Hero.gltf");
    ASSERT_EQ(entries.size(), 4u);

    EXPECT_EQ(entries[0].displayName, "Body");
    EXPECT_EQ(entries[0].sourceAssetPath, "Assets/Models/Hero.gltf");
    EXPECT_EQ(entries[0].subAssetKey, "mesh:Body");
    EXPECT_EQ(entries[0].dragResourcePath, "Assets/Models/Hero.gltf");
    EXPECT_EQ(entries[0].assetId, modelId);
    EXPECT_EQ(entries[0].artifactType, ArtifactType::Mesh);
    EXPECT_TRUE(entries[0].generatedReadOnly);

    EXPECT_EQ(entries[1].displayName, "Body");
    EXPECT_EQ(entries[1].subAssetKey, "material:Body");
    EXPECT_EQ(entries[1].artifactType, ArtifactType::Material);

    EXPECT_EQ(entries[2].displayName, "Albedo");
    EXPECT_EQ(entries[2].subAssetKey, "texture:Albedo");
    EXPECT_EQ(entries[2].artifactType, ArtifactType::Texture);

    EXPECT_EQ(entries[3].displayName, "HeroSurface");
    EXPECT_EQ(entries[3].subAssetKey, "shader:HeroSurface");
    EXPECT_EQ(entries[3].artifactType, ArtifactType::Shader);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ImportedModelGeneratedPrefabLoadsAndInstantiatesThroughDragDropWorkflow)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "Hero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "materials": [
                {
                    "name": "HeroMaterial",
                    "pbrMetallicRoughness": {
                        "baseColorFactor": [1.0, 0.2, 0.1, 1.0]
                    }
                }
            ],
            "meshes": [
                {
                    "name": "Body",
                    "primitives": [
                        {
                            "attributes": { "POSITION": 0 },
                            "material": 0
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "HeroRoot", "mesh": 0 }
            ]
        })");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    RegisterStandardPbrFreshnessOnlyDependency(database, root);
    ASSERT_TRUE(database.ImportAsset("Assets/Models/Hero.gltf"));

    auto prefab = database.LoadPrefabArtifactAtPath("Assets/Models/Hero.gltf", "prefab:Hero");
    ASSERT_TRUE(prefab.has_value());
    EXPECT_TRUE(prefab->generatedModelPrefab);
    EXPECT_FALSE(prefab->Validate().HasErrors());

    NLS::Engine::SceneSystem::Scene scene;
    AssetDragDropWorkflow workflow;
    const auto sceneId = ParseAssetId("e4040404-0404-4404-8404-040404040404");
    const auto result = workflow.Execute({
        {DragPayloadKind::GeneratedModelPrefabAsset, prefab->assetId, "prefab:Hero", &*prefab},
        {DropTargetKind::Hierarchy, &scene, nullptr, 0u, false},
        sceneId
    });

    ASSERT_EQ(result.status, DragDropOperationStatus::Committed);
    ASSERT_TRUE(result.instance.has_value());
    ASSERT_NE(result.instance->instanceRoot, nullptr);
    EXPECT_EQ(result.instance->instanceRoot->GetName(), "HeroRoot");
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front(), result.instance->instanceRoot);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, EditorDragDropBridgeInstantiatesPreimportedModelGeneratedPrefab)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const std::string bridgeHeroGltf = R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
            "buffers": [
                {
                    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA",
                    "byteLength": 42
                }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "meshes": [
                {
                    "name": "Body",
                    "primitives": [
                        {
                            "attributes": { "POSITION": 0 },
                            "indices": 1
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "BridgeHeroRoot", "mesh": 0 }
            ]
        })";
    WriteTextFile(
        root / "Assets" / "Models" / "BridgeHero.gltf",
        bridgeHeroGltf);

    NLS::Engine::SceneSystem::Scene scene;
    EditorAssetDragDropBridge bridge(root / "Assets");
    {
        AssetDatabaseFacade database({root});
        ASSERT_TRUE(database.Refresh());
        RegisterStandardPbrFreshnessOnlyDependency(database, root);
        ASSERT_TRUE(database.ImportAsset("Assets/Models/BridgeHero.gltf"));
    }
    const auto result = bridge.DropModelAssetIntoHierarchy("Models/BridgeHero.gltf", scene);

    ASSERT_TRUE(result.handled);
    ASSERT_EQ(result.dragDrop.status, DragDropOperationStatus::Committed);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front()->GetName(), "BridgeHeroRoot");
    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    const auto prefabRecord = database.LoadSubAssetAtPath("Assets/Models/BridgeHero.gltf", "prefab:BridgeHero");
    ASSERT_TRUE(prefabRecord.has_value());
    EXPECT_TRUE(std::filesystem::exists(prefabRecord->artifactPath));
    EXPECT_FALSE(std::filesystem::path(prefabRecord->artifactPath).filename().has_extension());
    EXPECT_TRUE(std::filesystem::exists(root / "Library" / "ArtifactDB"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, EditorDragDropBridgeInstantiatesGeneratedPrefabSubAssetResource)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const std::string bridgeHeroGltf = R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
            "buffers": [
                {
                    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA",
                    "byteLength": 42
                }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "meshes": [
                {
                    "name": "Body",
                    "primitives": [
                        {
                            "attributes": { "POSITION": 0 },
                            "indices": 1
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "BridgeHeroRoot", "mesh": 0 }
            ]
        })";
    WriteTextFile(
        root / "Assets" / "Models" / "BridgeHero.gltf",
        bridgeHeroGltf);

    {
        AssetDatabaseFacade importer({root});
        ASSERT_TRUE(importer.Refresh());
        RegisterStandardPbrFreshnessOnlyDependency(importer, root);
        ASSERT_TRUE(importer.ImportAsset("Assets/Models/BridgeHero.gltf"));
    }

    NLS::Engine::SceneSystem::Scene scene;
    EditorAssetDragDropBridge bridge(root / "Assets");
    const auto result = bridge.DropModelAssetIntoHierarchy(
        "Assets/Models/BridgeHero.gltf#prefab:BridgeHero.prefab",
        scene);

    ASSERT_TRUE(result.handled);
    ASSERT_EQ(result.dragDrop.status, DragDropOperationStatus::Committed);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front()->GetName(), "BridgeHeroRoot");

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, EditorDragDropBridgeInstantiatesPreimportedPrefabSource)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    NLS::Engine::GameObject gameObject("Lamp", "Prefab");
    const auto created = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &gameObject,
        {},
        ParseAssetId("e7070707-0707-4707-8707-070707070707"),
        "Assets/Prefabs/Lamp.prefab"
    });
    ASSERT_EQ(created.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);

    {
        AssetDatabaseFacade database({root});
        ASSERT_TRUE(database.Refresh());
        ASSERT_TRUE(database.CreateTextAsset(
            created.prefabSourceText,
            "Assets/Prefabs/Lamp.prefab",
            ParseAssetId("e8080808-0808-4808-8808-080808080808")));
    }

    NLS::Engine::SceneSystem::Scene scene;
    EditorAssetDragDropBridge bridge(root / "Assets");
    {
        AssetDatabaseFacade database({root});
        ASSERT_TRUE(database.Refresh());
        ASSERT_TRUE(database.ImportAsset("Assets/Prefabs/Lamp.prefab"));
    }
    const auto result = bridge.DropModelAssetIntoHierarchy("Assets/Prefabs/Lamp.prefab", scene);

    ASSERT_TRUE(result.handled);
    ASSERT_EQ(result.dragDrop.status, DragDropOperationStatus::Committed);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front()->GetName(), "Lamp");

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, DependencyQueriesReturnDirectAndRecursiveAssetPaths)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Prefabs" / "Hero.prefab", "{}");
    WriteTextFile(root / "Assets" / "Materials" / "Body.mat", "material");
    WriteTextFile(root / "Assets" / "Textures" / "Body.png", "texture");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto prefabId = ParseAssetId(database.AssetPathToGUID("Assets/Prefabs/Hero.prefab"));
    const auto materialId = ParseAssetId(database.AssetPathToGUID("Assets/Materials/Body.mat"));
    const auto textureId = ParseAssetId(database.AssetPathToGUID("Assets/Textures/Body.png"));

    ArtifactManifest prefabManifest;
    prefabManifest.sourceAssetId = prefabId;
    prefabManifest.primarySubAssetKey = "prefab:Hero";
    prefabManifest.subAssets.push_back(MakeArtifact(prefabId, "prefab:Hero", ArtifactType::Prefab, "prefab", {}, {}, "win64"));
    prefabManifest.dependencies.push_back({AssetDependencyKind::ImportedArtifact, materialId.ToString(), "material:Body"});

    ArtifactManifest materialManifest;
    materialManifest.sourceAssetId = materialId;
    materialManifest.primarySubAssetKey = "material:Body";
    materialManifest.subAssets.push_back(MakeArtifact(materialId, "material:Body", ArtifactType::Material, "material"));
    materialManifest.dependencies.push_back({AssetDependencyKind::ImportedArtifact, textureId.ToString(), "texture:Body"});

    database.AddArtifactManifest(prefabManifest);
    database.AddArtifactManifest(materialManifest);

    EXPECT_EQ(
        database.GetDependencies("Assets/Prefabs/Hero.prefab", false),
        std::vector<std::string>({"Assets/Materials/Body.mat"}));

    EXPECT_EQ(
        database.GetDependencies("Assets/Prefabs/Hero.prefab", true),
        std::vector<std::string>({"Assets/Materials/Body.mat", "Assets/Textures/Body.png"}));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, SearchFiltersUseNameTypeLabelFolderAndDeterministicOrdering)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Characters" / "Hero.prefab", "{}");
    WriteTextFile(root / "Assets" / "Characters" / "Hero.mat", "material");
    WriteTextFile(root / "Assets" / "Environment" / "HeroRock.prefab", "{}");
    WriteTextFile(root / "Assets" / "Characters" / "Villain.prefab", "{}");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.SetLabels("Assets/Characters/Hero.prefab", {"character", "player"}));
    ASSERT_TRUE(database.SetLabels("Assets/Environment/HeroRock.prefab", {"environment"}));

    EXPECT_EQ(
        database.FindAssets("name:Hero type:prefab label:character", {"Assets/Characters"}),
        std::vector<std::string>({"Assets/Characters/Hero.prefab"}));

    EXPECT_EQ(
        database.FindAssets("type:prefab", {}),
        std::vector<std::string>({
            "Assets/Characters/Hero.prefab",
            "Assets/Characters/Villain.prefab",
            "Assets/Environment/HeroRock.prefab"
        }));

    EXPECT_EQ(
        database.GetLabels("Assets/Characters/Hero.prefab"),
        std::vector<std::string>({"character", "player"}));
    EXPECT_EQ(
        database.GetAllLabels(),
        std::vector<std::string>({"character", "environment", "player"}));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, BundleMetadataMapsToRuntimeAssetPacks)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;
    using namespace NLS::Engine::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Prefabs" / "Hero.prefab", "{}");
    WriteTextFile(root / "Assets" / "Materials" / "Hero.mat", "material");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.SetAssetPackNameAndVariant("Assets/Prefabs/Hero.prefab", "characters", "hd"));
    ASSERT_TRUE(database.SetAssetPackNameAndVariant("Assets/Materials/Hero.mat", "characters", "hd"));

    const auto prefabId = ParseAssetId(database.AssetPathToGUID("Assets/Prefabs/Hero.prefab"));
    const auto materialId = ParseAssetId(database.AssetPathToGUID("Assets/Materials/Hero.mat"));

    ArtifactManifest prefabManifest;
    prefabManifest.sourceAssetId = prefabId;
    prefabManifest.targetPlatform = "win64";
    prefabManifest.primarySubAssetKey = "prefab:Hero";
    prefabManifest.subAssets.push_back(MakeArtifact(prefabId, "prefab:Hero", ArtifactType::Prefab, "prefab", {}, {}, "win64"));

    ArtifactManifest materialManifest;
    materialManifest.sourceAssetId = materialId;
    materialManifest.targetPlatform = "win64";
    materialManifest.primarySubAssetKey = "material:Hero";
    materialManifest.subAssets.push_back(MakeArtifact(materialId, "material:Hero", ArtifactType::Material, "material", {}, {}, "win64"));

    database.AddArtifactManifest(prefabManifest);
    database.AddArtifactManifest(materialManifest);

    const auto packInfo = database.GetAssetPackNameAndVariant("Assets/Prefabs/Hero.prefab");
    ASSERT_TRUE(packInfo.has_value());
    EXPECT_EQ(packInfo->name, "characters");
    EXPECT_EQ(packInfo->variant, "hd");

    RuntimeManifestBuilder builder;
    builder.AddArtifactManifest(prefabManifest);
    builder.AddArtifactManifest(materialManifest);
    const auto result = builder.BuildAssetPacks(database.GetAssetPackBuildInputs(), "win64");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_EQ(result.manifest.assetPacks.size(), 1u);
    EXPECT_EQ(result.manifest.assetPacks[0].packName, "characters");
    EXPECT_EQ(result.manifest.assetPacks[0].packVariant, "hd");
    EXPECT_EQ(result.manifest.assetPacks[0].entries.size(), 2u);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, AssetPacksIncludeDependencyClosureLoaderHashesAndVariants)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;
    using namespace NLS::Engine::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Prefabs" / "Hero.prefab", "{}");
    WriteTextFile(root / "Assets" / "Prefabs" / "Villain.prefab", "{}");
    WriteTextFile(root / "Assets" / "Materials" / "Body.mat", "material");
    WriteTextFile(root / "Assets" / "Textures" / "Body.png", "texture");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.SetAssetPackNameAndVariant("Assets/Prefabs/Hero.prefab", "characters", "hd"));
    ASSERT_TRUE(database.SetAssetPackNameAndVariant("Assets/Materials/Body.mat", "characters", "hd"));
    ASSERT_TRUE(database.SetAssetPackNameAndVariant("Assets/Prefabs/Villain.prefab", "characters", "sd"));

    const auto prefabId = ParseAssetId(database.AssetPathToGUID("Assets/Prefabs/Hero.prefab"));
    const auto villainId = ParseAssetId(database.AssetPathToGUID("Assets/Prefabs/Villain.prefab"));
    const auto materialId = ParseAssetId(database.AssetPathToGUID("Assets/Materials/Body.mat"));
    const auto textureId = ParseAssetId(database.AssetPathToGUID("Assets/Textures/Body.png"));

    ArtifactManifest prefabManifest;
    prefabManifest.sourceAssetId = prefabId;
    prefabManifest.targetPlatform = "win64";
    prefabManifest.primarySubAssetKey = "prefab:Hero";
    prefabManifest.subAssets.push_back(MakeArtifact(
        prefabId,
        "prefab:Hero",
        ArtifactType::Prefab,
        "prefab",
        "Artifacts/11/1111111111111111111111111111111111111111111111111111111111111111",
        "sha256:hero-prefab",
        "win64"));
    prefabManifest.dependencies.push_back({AssetDependencyKind::ImportedArtifact, materialId.ToString(), "material:Body"});

    ArtifactManifest materialManifest;
    materialManifest.sourceAssetId = materialId;
    materialManifest.targetPlatform = "win64";
    materialManifest.primarySubAssetKey = "material:Body";
    materialManifest.subAssets.push_back(MakeArtifact(
        materialId,
        "material:Body",
        ArtifactType::Material,
        "material",
        "Artifacts/22/2222222222222222222222222222222222222222222222222222222222222222",
        "sha256:body-material",
        "win64"));
    materialManifest.dependencies.push_back({AssetDependencyKind::ImportedArtifact, textureId.ToString(), "texture:Body"});

    ArtifactManifest textureManifest;
    textureManifest.sourceAssetId = textureId;
    textureManifest.targetPlatform = "win64";
    textureManifest.primarySubAssetKey = "texture:Body";
    textureManifest.subAssets.push_back(MakeArtifact(
        textureId,
        "texture:Body",
        ArtifactType::Texture,
        "texture",
        "Artifacts/33/3333333333333333333333333333333333333333333333333333333333333333",
        "sha256:body-texture",
        "win64"));

    ArtifactManifest villainManifest;
    villainManifest.sourceAssetId = villainId;
    villainManifest.targetPlatform = "win64";
    villainManifest.primarySubAssetKey = "prefab:Villain";
    villainManifest.subAssets.push_back(MakeArtifact(
        villainId,
        "prefab:Villain",
        ArtifactType::Prefab,
        "prefab",
        "Artifacts/44/4444444444444444444444444444444444444444444444444444444444444444",
        "sha256:villain-prefab",
        "win64"));

    database.AddArtifactManifest(prefabManifest);
    database.AddArtifactManifest(materialManifest);
    database.AddArtifactManifest(textureManifest);
    database.AddArtifactManifest(villainManifest);

    RuntimeManifestBuilder builder;
    builder.AddArtifactManifest(prefabManifest);
    builder.AddArtifactManifest(materialManifest);
    builder.AddArtifactManifest(textureManifest);
    builder.AddArtifactManifest(villainManifest);

    const auto result = builder.BuildAssetPacks(database.GetAssetPackBuildInputs(), "win64");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_EQ(result.manifest.assetPacks.size(), 2u);

    const auto* hdPack = FindPack(result.manifest, "characters", "hd");
    ASSERT_NE(hdPack, nullptr);
    ASSERT_EQ(hdPack->entries.size(), 3u);

    const auto* prefabEntry = FindPackEntry(*hdPack, prefabId, "prefab:Hero");
    ASSERT_NE(prefabEntry, nullptr);
    EXPECT_EQ(prefabEntry->artifactType, ArtifactType::Prefab);
    EXPECT_EQ(prefabEntry->loaderId, "prefab");
    EXPECT_EQ(prefabEntry->artifactPath, "Artifacts/11/1111111111111111111111111111111111111111111111111111111111111111");
    EXPECT_EQ(prefabEntry->contentHash, "sha256:hero-prefab");
    EXPECT_TRUE(ContainsDependency(prefabEntry->dependencies, materialId, "material:Body"));

    const auto* materialEntry = FindPackEntry(*hdPack, materialId, "material:Body");
    ASSERT_NE(materialEntry, nullptr);
    EXPECT_EQ(materialEntry->loaderId, "material");
    EXPECT_EQ(materialEntry->contentHash, "sha256:body-material");
    EXPECT_TRUE(ContainsDependency(materialEntry->dependencies, textureId, "texture:Body"));

    const auto* textureEntry = FindPackEntry(*hdPack, textureId, "texture:Body");
    ASSERT_NE(textureEntry, nullptr);
    EXPECT_EQ(textureEntry->loaderId, "texture");
    EXPECT_EQ(textureEntry->contentHash, "sha256:body-texture");
    EXPECT_TRUE(textureEntry->dependencies.empty());

    const auto* sdPack = FindPack(result.manifest, "characters", "sd");
    ASSERT_NE(sdPack, nullptr);
    ASSERT_EQ(sdPack->entries.size(), 1u);
    EXPECT_NE(FindPackEntry(*sdPack, villainId, "prefab:Villain"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, RuntimeLoadsPackagedAssetsFromManifestAndRejectsEditorOnlyApis)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;
    using namespace NLS::Engine::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Prefabs" / "Hero.prefab", "{}");

    const auto prefabId = NLS::Core::Assets::AssetId::New();
    RuntimeAssetManifest manifest;
    manifest.targetPlatform = "win64";
    manifest.entries.push_back({
        prefabId,
        "prefab:Hero",
        ArtifactType::Prefab,
        "prefab",
        (std::filesystem::path("Artifacts") /
            NLS::Core::Assets::BuildArtifactStorageRelativePath(
                "1111111111111111111111111111111111111111111111111111111111111111")).generic_string(),
        "sha256:hero-prefab",
        {}
    });

    RuntimeAssetDatabase runtimeDatabase(manifest);
    const auto* entry = runtimeDatabase.Resolve({prefabId, "prefab:Hero"});
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->loaderId, "prefab");
    EXPECT_EQ(entry->contentHash, "sha256:hero-prefab");
    EXPECT_TRUE(IsRuntimePackagedAssetPath(entry->artifactPath));

    EXPECT_TRUE(IsRuntimeAssetApiAvailable("RuntimeAssetDatabase.Resolve"));
    EXPECT_FALSE(IsRuntimeAssetApiAvailable("AssetDatabase.Refresh"));
    EXPECT_FALSE(IsRuntimeAssetApiAvailable("AssetDatabase.ImportAsset"));
    EXPECT_FALSE(IsRuntimeAssetApiAvailable("AssetImporter.SaveAndReimport"));
    EXPECT_FALSE(IsRuntimeAssetApiAvailable("ModelImporter.SaveAndReimport"));
    EXPECT_FALSE(IsRuntimeAssetApiAvailable("TextureImporter.SaveAndReimport"));

    AssetDatabaseFacade runtimeFacade({root}, AssetDatabaseAccessMode::Runtime);
    EXPECT_FALSE(runtimeFacade.Refresh());
    EXPECT_FALSE(runtimeFacade.ImportAsset("Assets/Prefabs/Hero.prefab"));
    EXPECT_TRUE(runtimeFacade.AssetPathToGUID("Assets/Prefabs/Hero.prefab").empty());
    EXPECT_FALSE(runtimeFacade.SetAssetPackNameAndVariant("Assets/Prefabs/Hero.prefab", "characters", "hd"));
    EXPECT_TRUE(ContainsAssetDiagnosticCode(
        runtimeFacade.GetDiagnostics(),
        "assetdatabase-editor-api-unavailable-at-runtime"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, CreateAddExtractAndContainmentUseAssetObjectSemantics)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    AssetObjectRecord material;
    material.name = "HeroMaterial";
    material.artifactType = ArtifactType::Material;
    material.loaderId = "material";
    material.serializedPayload =
        "shaderLabMaterialVersion=1\n"
        "shader=Assets/Shaders/Unlit.shader\n"
        "property _BaseColor Color 1,1,1,1\n";

    ASSERT_TRUE(database.CreateAsset(material, "Assets/Materials/Hero.mat"));
    const auto materialSourcePath = root / "Assets" / "Materials" / "Hero.mat";
    ASSERT_TRUE(std::filesystem::exists(materialSourcePath));
    const auto materialSourceBytes = ReadBinaryFile(materialSourcePath);
    const auto materialSourceContainer = ReadNativeArtifactContainer(
        materialSourceBytes,
        ArtifactType::Material,
        1u);
    ASSERT_TRUE(materialSourceContainer.has_value());
    EXPECT_EQ(materialSourceContainer->metadata.schemaName, "material");
    EXPECT_EQ(materialSourceContainer->metadata.subAssetKey, "material:HeroMaterial");
    EXPECT_EQ(materialSourceContainer->metadata.displayName, "HeroMaterial");
    EXPECT_EQ(
        std::string(materialSourceContainer->payload.begin(), materialSourceContainer->payload.end()),
        material.serializedPayload);
    const auto materialGuid = database.AssetPathToGUID("Assets/Materials/Hero.mat");
    ASSERT_FALSE(materialGuid.empty());
    EXPECT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Materials/Hero.mat"));

    const auto mainAsset = database.LoadMainAssetAtPath("Assets/Materials/Hero.mat");
    ASSERT_TRUE(mainAsset.has_value());
    EXPECT_EQ(mainAsset->artifactType, ArtifactType::Material);
    EXPECT_TRUE(database.Contains(*mainAsset));
    EXPECT_TRUE(database.IsMainAsset(*mainAsset));
    EXPECT_FALSE(database.IsSubAsset(*mainAsset));
    EXPECT_FALSE(std::filesystem::path(mainAsset->artifactPath).filename().has_extension());

    const auto materialManifest = database.GetArtifactManifestForAssetPath("Assets/Materials/Hero.mat");
    ASSERT_TRUE(materialManifest.has_value());
    EXPECT_TRUE(std::any_of(
        materialManifest->dependencies.begin(),
        materialManifest->dependencies.end(),
        [](const AssetDependencyRecord& dependency)
        {
            return dependency.kind == AssetDependencyKind::PathToGuidMapping &&
                dependency.value == "Assets/Materials/Hero.mat.meta" &&
                !dependency.hashOrVersion.empty();
        }));
    EXPECT_TRUE(std::any_of(
        materialManifest->dependencies.begin(),
        materialManifest->dependencies.end(),
        [](const AssetDependencyRecord& dependency)
        {
            return dependency.kind == AssetDependencyKind::ImporterVersion &&
                dependency.value == "material" &&
                !dependency.hashOrVersion.empty();
        }));

    AssetObjectRecord embeddedTexture;
    embeddedTexture.name = "EmbeddedMask";
    embeddedTexture.artifactType = ArtifactType::Texture;
    embeddedTexture.loaderId = "texture";
    embeddedTexture.serializedPayload = "mask-bytes";

    ASSERT_TRUE(database.AddObjectToAsset(embeddedTexture, "Assets/Materials/Hero.mat"));
    auto allAssets = database.LoadAllAssetsAtPath("Assets/Materials/Hero.mat");
    ASSERT_EQ(allAssets.size(), 2u);
    const auto reloadedMainAsset = database.LoadMainAssetAtPath("Assets/Materials/Hero.mat");
    ASSERT_TRUE(reloadedMainAsset.has_value());
    EXPECT_EQ(
        ReadArtifactPayloadText(reloadedMainAsset->artifactPath, ArtifactType::Material, 1u),
        material.serializedPayload);
    const auto subAsset = database.LoadSubAssetAtPath("Assets/Materials/Hero.mat", "texture:EmbeddedMask");
    ASSERT_TRUE(subAsset.has_value());
    EXPECT_TRUE(database.Contains(*subAsset));
    EXPECT_FALSE(database.IsMainAsset(*subAsset));
    EXPECT_TRUE(database.IsSubAsset(*subAsset));

    const auto uniquePath = database.GenerateUniqueAssetPath("Assets/Materials/Hero.mat");
    EXPECT_EQ(uniquePath, "Assets/Materials/Hero 1.mat");

    ASSERT_TRUE(database.ExtractAsset(*subAsset, "Assets/Textures/ExtractedMask.png"));
    EXPECT_FALSE(database.LoadSubAssetAtPath("Assets/Materials/Hero.mat", "texture:EmbeddedMask").has_value());
    const auto extracted = database.LoadMainAssetAtPath("Assets/Textures/ExtractedMask.png");
    ASSERT_TRUE(extracted.has_value());
    EXPECT_EQ(extracted->artifactType, ArtifactType::Texture);
    EXPECT_TRUE(database.Contains(*extracted));
    EXPECT_TRUE(database.IsNativeAsset(*extracted));
    EXPECT_FALSE(database.IsForeignAsset(*extracted));

    AssetDatabaseRecord foreign;
    foreign.assetId = AssetId::New();
    foreign.assetPath = "Packages/External/Foreign.mat";
    foreign.subAssetKey = "material:Foreign";
    foreign.artifactType = ArtifactType::Material;
    foreign.mainAsset = true;
    EXPECT_FALSE(database.Contains(foreign));
    EXPECT_TRUE(database.IsForeignAsset(foreign));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, StandaloneMaterialImportUsesNativePayloadAndRejectsLegacyTextPayload)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    AssetObjectRecord material;
    material.name = "HeroMaterial";
    material.artifactType = ArtifactType::Material;
    material.loaderId = "material";
    material.serializedPayload =
        "shaderLabMaterialVersion=1\n"
        "shader=Assets/Shaders/Unlit.shader\n"
        "property _BaseColor Color 1 1 1 1\n";

    ASSERT_TRUE(database.CreateAsset(material, "Assets/Materials/Hero.mat"));
    ASSERT_TRUE(database.ImportAsset("Assets/Materials/Hero.mat"));

    const auto mainAsset = database.LoadMainAssetAtPath("Assets/Materials/Hero.mat");
    ASSERT_TRUE(mainAsset.has_value());
    EXPECT_EQ(
        ReadArtifactPayloadText(mainAsset->artifactPath, ArtifactType::Material, 1u),
        material.serializedPayload);

    const auto legacyPath = root / "Assets" / "Materials" / "Legacy.mat";
    WriteTextFile(
        legacyPath,
        "NULLUS_NATIVE_ASSET=1\n"
        "NAME=Legacy\n"
        "SUB_ASSET_KEY=material:Legacy\n"
        "LOADER_ID=material\n"
        "PAYLOAD=<root><shader>?</shader></root>\n");
    auto legacyMeta = AssetMeta::CreateForAsset(legacyPath);
    legacyMeta.importerId = "material";
    ASSERT_TRUE(legacyMeta.Save(GetAssetMetaPath(legacyPath)));

    ASSERT_TRUE(database.Refresh());
    EXPECT_FALSE(database.ImportAsset("Assets/Materials/Legacy.mat"));
    const auto legacyMainAsset = database.LoadMainAssetAtPath("Assets/Materials/Legacy.mat");
    ASSERT_TRUE(legacyMainAsset.has_value());
    EXPECT_EQ(legacyMainAsset->artifactType, ArtifactType::Unknown);
    EXPECT_TRUE(legacyMainAsset->artifactPath.empty());
    EXPECT_TRUE(ContainsAssetDiagnosticCode(
        database.GetDiagnostics(),
        "assetdatabase-material-source-native-container-required"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, CreateTextAssetWritesSourceAndPreservesRequestedGuid)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto prefabId = ParseAssetId("d1010101-0101-4101-8101-010101010101");
    ASSERT_TRUE(database.CreateTextAsset(
        "{\n  \"format\": \"Nullus.ObjectGraph.Prefab\"\n}\n",
        "Assets/Prefabs/TextCreated.prefab",
        prefabId));

    EXPECT_TRUE(std::filesystem::exists(root / "Assets" / "Prefabs" / "TextCreated.prefab"));
    EXPECT_EQ(database.AssetPathToGUID("Assets/Prefabs/TextCreated.prefab"), prefabId.ToString());
    const auto loaded = AssetMeta::Load(root / "Assets" / "Prefabs" / "TextCreated.prefab.meta");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->assetType, AssetType::Prefab);
    EXPECT_EQ(loaded->importerId, "prefab");

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ImportPrefabSourceAssetBuildsLoadablePrefabArtifact)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    NLS::Engine::GameObject gameObject("Lamp", "Prefab");
    const auto created = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &gameObject,
        {},
        ParseAssetId("e5050505-0505-4505-8505-050505050505"),
        "Assets/Prefabs/Lamp.prefab"
    });
    ASSERT_EQ(created.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_FALSE(created.prefabSourceText.empty());

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.CreateTextAsset(
        created.prefabSourceText,
        "Assets/Prefabs/Lamp.prefab",
        ParseAssetId("e6060606-0606-4606-8606-060606060606")));

    ASSERT_TRUE(database.ImportAsset("Assets/Prefabs/Lamp.prefab"));

    const auto allAssets = database.LoadAllAssetsAtPath("Assets/Prefabs/Lamp.prefab");
    ASSERT_EQ(allAssets.size(), 1u);
    EXPECT_TRUE(allAssets.front().mainAsset);
    EXPECT_EQ(allAssets.front().subAssetKey, "prefab:Lamp");
    EXPECT_EQ(allAssets.front().artifactType, ArtifactType::Prefab);

    auto prefab = database.LoadPrefabArtifactAtPath("Assets/Prefabs/Lamp.prefab", "prefab:Lamp");
    ASSERT_TRUE(prefab.has_value());
    EXPECT_FALSE(prefab->generatedModelPrefab);
    EXPECT_FALSE(prefab->Validate().HasErrors());
    EXPECT_EQ(prefab->graph.root.GetGuid().ToString(), created.artifact->graph.root.GetGuid().ToString());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, LoadsPersistedPrefabArtifactByAssetIdWhenSourcePathIndexIsMissing)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    NLS::Engine::GameObject gameObject("SceneOnlyLamp", "Prefab");
    const auto prefabId = ParseAssetId("e6161616-1616-4616-8616-161616161616");
    const auto created = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &gameObject,
        {},
        prefabId,
        "Assets/Prefabs/SceneOnlyLamp.prefab"
    });
    ASSERT_EQ(created.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_FALSE(created.prefabSourceText.empty());

    {
        AssetDatabaseFacade database({root});
        ASSERT_TRUE(database.Refresh());
        ASSERT_TRUE(database.CreateTextAsset(
            created.prefabSourceText,
            "Assets/Prefabs/SceneOnlyLamp.prefab",
            prefabId));
        ASSERT_TRUE(database.ImportAsset("Assets/Prefabs/SceneOnlyLamp.prefab"));
        ASSERT_TRUE(database.LoadPrefabArtifactAtPath(
            "Assets/Prefabs/SceneOnlyLamp.prefab",
            "prefab:SceneOnlyLamp").has_value());
    }

    std::filesystem::remove(root / "Assets" / "Prefabs" / "SceneOnlyLamp.prefab");
    std::filesystem::remove(root / "Assets" / "Prefabs" / "SceneOnlyLamp.prefab.meta");

    {
        const auto manifest = LoadPersistedArtifactManifest(root, prefabId);
        ASSERT_TRUE(manifest.has_value());
        ASSERT_FALSE(manifest->subAssets.empty());
        const auto persistedArtifactPath = manifest->subAssets[0].artifactPath;
        ASSERT_TRUE(NLS::Core::Assets::IsContentStorageArtifactPath(persistedArtifactPath));
        ASSERT_EQ(persistedArtifactPath.find("Library/Artifacts/"), 0u);
        ASSERT_FALSE(std::filesystem::path(persistedArtifactPath).is_absolute());
        ASSERT_FALSE(std::filesystem::path(persistedArtifactPath).filename().has_extension());
        ASSERT_TRUE(std::filesystem::is_regular_file(root / persistedArtifactPath));
    }

    AssetDatabaseFacade freshDatabase({root});
    ASSERT_TRUE(freshDatabase.Refresh());
    ASSERT_TRUE(freshDatabase.GUIDToAssetPath(prefabId.ToString()).empty());

    auto prefab = freshDatabase.LoadPrefabArtifactByAssetId(prefabId, "prefab:SceneOnlyLamp");
    ASSERT_TRUE(prefab.has_value());
    EXPECT_EQ(prefab->assetId, prefabId);
    EXPECT_FALSE(prefab->generatedModelPrefab);
    EXPECT_FALSE(prefab->Validate().HasErrors());
    EXPECT_EQ(prefab->graph.root.GetGuid(), created.artifact->graph.root.GetGuid());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, LoadsPersistedPrefabArtifactByAssetIdWithRelativeArtifactPath)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto prefabId = ParseAssetId("e7171717-1717-4717-8717-171717171717");
    const std::string subAssetKey = "prefab:RelativeArtifact";
    NLS::Engine::GameObject gameObject("RelativeArtifact", "Prefab");
    const auto created = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &gameObject,
        {},
        prefabId,
        "Assets/Prefabs/RelativeArtifact.prefab"
    });
    ASSERT_EQ(created.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_FALSE(created.prefabSourceText.empty());

    ArtifactManifest manifest;
    manifest.sourceAssetId = prefabId;
    manifest.importerId = "prefab";
    manifest.importerVersion = GetCurrentImporterVersion(AssetType::Prefab);
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = subAssetKey;
    NativeArtifactMetadata metadata;
    metadata.artifactType = ArtifactType::Prefab;
    metadata.schemaName = "prefab-artifact";
    metadata.schemaVersion = 1u;
    metadata.sourceAssetId = prefabId;
    metadata.subAssetKey = subAssetKey;
    metadata.importerId = "prefab";
    metadata.importerVersion = GetCurrentImporterVersion(AssetType::Prefab);
    metadata.targetPlatform = "editor";
    const auto payload = WriteNativeArtifactContainer(
        std::move(metadata),
        std::vector<uint8_t>(created.prefabSourceText.begin(), created.prefabSourceText.end()));
    const auto artifactPath = (std::filesystem::path("Library") /
        "Artifacts" /
        BuildArtifactStorageRelativePath(BuildArtifactStorageFileName(payload.data(), payload.size()))).generic_string();
    manifest.subAssets.push_back(MakeArtifact(
        prefabId,
        subAssetKey,
        ArtifactType::Prefab,
        "prefab",
        artifactPath));
    WritePersistedArtifactManifest(root, manifest);
    WriteBinaryFile(root / artifactPath, payload);

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.GUIDToAssetPath(prefabId.ToString()).empty());

    auto prefab = database.LoadPrefabArtifactByAssetId(prefabId, subAssetKey);
    ASSERT_TRUE(prefab.has_value());
    EXPECT_EQ(prefab->assetId, prefabId);
    EXPECT_FALSE(prefab->Validate().HasErrors());
    EXPECT_EQ(prefab->graph.root.GetGuid(), created.artifact->graph.root.GetGuid());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, TargetedRefreshReimportUsesPersistedPreviousManifest)
{
    using namespace NLS::Base::Profiling;
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto prefabPath = root / "Assets" / "Prefabs" / "CachedReuse.prefab";
    const std::string editorAssetPath = "Assets/Prefabs/CachedReuse.prefab";

    NLS::Engine::GameObject gameObject("CachedReuse", "Prefab");
    const auto created = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &gameObject,
        {},
        ParseAssetId("e9191919-1919-4919-8919-191919191919"),
        editorAssetPath
    });
    ASSERT_EQ(created.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_FALSE(created.prefabSourceText.empty());
    WriteTextFile(prefabPath, created.prefabSourceText);

    {
        AssetDatabaseFacade database({root});
        ASSERT_TRUE(database.Refresh());
        ASSERT_TRUE(database.ImportAsset(editorAssetPath));
    }

    AssetDatabaseFacade database({root});
    const std::vector<std::filesystem::path> refreshPaths {prefabPath};
    ASSERT_TRUE(database.RefreshKnownSourceAssets(refreshPaths));

    PerformanceStageStats stats;
    {
        PerformanceStageStatsCapture capture(stats);
        NLS::Editor::Assets::ImportProgressTracker importProgress;
        ASSERT_TRUE(database.ImportAssetFromCurrentDatabase(editorAssetPath, importProgress, 1u));
    }

    const auto snapshot = stats.Snapshot();
    const auto stage = std::find_if(
        snapshot.stages.begin(),
        snapshot.stages.end(),
        [](const PerformanceStageEntry& entry)
        {
            return entry.domain == PerformanceStageDomain::Prefab &&
                entry.stageName == "WriteAndCommitArtifactPayloads";
        });
    ASSERT_NE(stage, snapshot.stages.end());
    ASSERT_TRUE(stage->counters.contains("contentPathReusedCount"));
    EXPECT_EQ(stage->counters.at("contentPathReusedCount"), 1u)
        << "Targeted refresh clears in-memory manifests, so reimport must recover the previous manifest from ArtifactDB.";
    ASSERT_TRUE(stage->counters.contains("contentPathHashedCount"));
    EXPECT_EQ(stage->counters.at("contentPathHashedCount"), 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, RejectsPersistedPrefabArtifactOutsidePhysicalArtifactRoot)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto prefabId = ParseAssetId("e6262626-2626-4626-8626-262626262626");
    const std::string subAssetKey = "prefab:Escaped";
    NLS::Engine::GameObject gameObject("Escaped", "Prefab");
    const auto created = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &gameObject,
        {},
        prefabId,
        "Assets/Prefabs/Escaped.prefab"
    });
    ASSERT_EQ(created.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_FALSE(created.prefabSourceText.empty());

    WriteTextFile(root / "Assets" / "Prefabs" / "Escaped.prefab", created.prefabSourceText);
    auto meta = AssetMeta::CreateForAsset(root / "Assets" / "Prefabs" / "Escaped.prefab");
    meta.id = prefabId;
    ASSERT_TRUE(meta.Save(root / "Assets" / "Prefabs" / "Escaped.prefab.meta"));

    ArtifactManifest manifest;
    manifest.sourceAssetId = prefabId;
    manifest.importerId = "prefab";
    manifest.importerVersion = GetCurrentImporterVersion(AssetType::Prefab);
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = subAssetKey;
    manifest.subAssets.push_back(MakeArtifact(
        prefabId,
        subAssetKey,
        ArtifactType::Prefab,
        "prefab",
        "Assets/Escaped.prefab"));
    WritePersistedArtifactManifest(root, manifest);

    NativeArtifactMetadata metadata;
    metadata.artifactType = ArtifactType::Prefab;
    metadata.schemaName = "prefab-artifact";
    metadata.schemaVersion = 1u;
    metadata.sourceAssetId = prefabId;
    metadata.subAssetKey = subAssetKey;
    metadata.importerId = "prefab";
    metadata.importerVersion = GetCurrentImporterVersion(AssetType::Prefab);
    metadata.targetPlatform = "editor";
    WriteBinaryFile(
        root / "Assets" / "Escaped.prefab",
        WriteNativeArtifactContainer(
            std::move(metadata),
            std::vector<uint8_t>(created.prefabSourceText.begin(), created.prefabSourceText.end())));

    std::filesystem::remove(root / "Assets" / "Prefabs" / "Escaped.prefab");
    std::filesystem::remove(root / "Assets" / "Prefabs" / "Escaped.prefab.meta");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.GUIDToAssetPath(prefabId.ToString()).empty());
    EXPECT_FALSE(database.LoadPrefabArtifactByAssetId(prefabId, subAssetKey).has_value());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, RejectsPersistedPrefabArtifactUnderArbitraryRelativeHashDirectory)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto prefabId = ParseAssetId("e6262626-2727-4627-8627-262626262627");
    const std::string subAssetKey = "prefab:RelativeEscape";
    const std::string blobName =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    NLS::Engine::GameObject gameObject("RelativeEscape", "Prefab");
    const auto created = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &gameObject,
        {},
        prefabId,
        "Assets/Prefabs/RelativeEscape.prefab"
    });
    ASSERT_EQ(created.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_FALSE(created.prefabSourceText.empty());

    WriteTextFile(root / "Assets" / "Prefabs" / "RelativeEscape.prefab", created.prefabSourceText);
    auto meta = AssetMeta::CreateForAsset(root / "Assets" / "Prefabs" / "RelativeEscape.prefab");
    meta.id = prefabId;
    ASSERT_TRUE(meta.Save(root / "Assets" / "Prefabs" / "RelativeEscape.prefab.meta"));

    ArtifactManifest manifest;
    manifest.sourceAssetId = prefabId;
    manifest.importerId = "prefab";
    manifest.importerVersion = GetCurrentImporterVersion(AssetType::Prefab);
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = subAssetKey;
    manifest.subAssets.push_back(MakeArtifact(
        prefabId,
        subAssetKey,
        ArtifactType::Prefab,
        "prefab",
        "foo/" + blobName));
    WritePersistedArtifactManifest(root, manifest);

    NativeArtifactMetadata metadata;
    metadata.artifactType = ArtifactType::Prefab;
    metadata.schemaName = "prefab-artifact";
    metadata.schemaVersion = 1u;
    metadata.sourceAssetId = prefabId;
    metadata.subAssetKey = subAssetKey;
    metadata.importerId = "prefab";
    metadata.importerVersion = GetCurrentImporterVersion(AssetType::Prefab);
    metadata.targetPlatform = "editor";
    WriteBinaryFile(
        root / "foo" / blobName,
        WriteNativeArtifactContainer(
            std::move(metadata),
            std::vector<uint8_t>(created.prefabSourceText.begin(), created.prefabSourceText.end())));

    std::filesystem::remove(root / "Assets" / "Prefabs" / "RelativeEscape.prefab");
    std::filesystem::remove(root / "Assets" / "Prefabs" / "RelativeEscape.prefab.meta");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.GUIDToAssetPath(prefabId.ToString()).empty());
    EXPECT_FALSE(database.LoadPrefabArtifactByAssetId(prefabId, subAssetKey).has_value());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, RejectsPersistedPrefabArtifactSymlinkInsidePhysicalArtifactRoot)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto outside =
        std::filesystem::temp_directory_path() /
        ("nullus_prefab_artifact_symlink_outside_" + NLS::Guid::New().ToString());
    const auto prefabId = ParseAssetId("e6363636-3636-4636-8636-363636363636");
    const std::string subAssetKey = "prefab:LinkedEscape";
    NLS::Engine::GameObject gameObject("LinkedEscape", "Prefab");
    const auto created = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &gameObject,
        {},
        prefabId,
        "Assets/Prefabs/LinkedEscape.prefab"
    });
    ASSERT_EQ(created.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_FALSE(created.prefabSourceText.empty());

    WriteTextFile(root / "Assets" / "Prefabs" / "LinkedEscape.prefab", created.prefabSourceText);
    auto meta = AssetMeta::CreateForAsset(root / "Assets" / "Prefabs" / "LinkedEscape.prefab");
    meta.id = prefabId;
    ASSERT_TRUE(meta.Save(root / "Assets" / "Prefabs" / "LinkedEscape.prefab.meta"));

    ArtifactManifest manifest;
    manifest.sourceAssetId = prefabId;
    manifest.importerId = "prefab";
    manifest.importerVersion = GetCurrentImporterVersion(AssetType::Prefab);
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = subAssetKey;
    manifest.subAssets.push_back(MakeArtifact(
        prefabId,
        subAssetKey,
        ArtifactType::Prefab,
        "prefab",
        "Library/Artifacts/" + prefabId.ToString() + "/5d4b4d6c2b6c4a6c9b91d90753df2a8d5d4b4d6c2b6c4a6c9b91d90753df2a8d"));
    WritePersistedArtifactManifest(root, manifest);

    NativeArtifactMetadata metadata;
    metadata.artifactType = ArtifactType::Prefab;
    metadata.schemaName = "prefab-artifact";
    metadata.schemaVersion = 1u;
    metadata.sourceAssetId = prefabId;
    metadata.subAssetKey = subAssetKey;
    metadata.importerId = "prefab";
    metadata.importerVersion = GetCurrentImporterVersion(AssetType::Prefab);
    metadata.targetPlatform = "editor";
    WriteBinaryFile(
        outside / "5d4b4d6c2b6c4a6c9b91d90753df2a8d5d4b4d6c2b6c4a6c9b91d90753df2a8d",
        WriteNativeArtifactContainer(
            std::move(metadata),
            std::vector<uint8_t>(created.prefabSourceText.begin(), created.prefabSourceText.end())));

    const auto linkPath = root / "Library" / "Artifacts" / prefabId.ToString() / "5d4b4d6c2b6c4a6c9b91d90753df2a8d5d4b4d6c2b6c4a6c9b91d90753df2a8d";
    std::filesystem::create_directories(linkPath.parent_path());
    std::error_code error;
    std::filesystem::create_symlink(outside / "5d4b4d6c2b6c4a6c9b91d90753df2a8d5d4b4d6c2b6c4a6c9b91d90753df2a8d", linkPath, error);
    if (error)
    {
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(outside);
        GTEST_SKIP() << "File symlink creation is not available in this environment.";
    }

    std::filesystem::remove(root / "Assets" / "Prefabs" / "LinkedEscape.prefab");
    std::filesystem::remove(root / "Assets" / "Prefabs" / "LinkedEscape.prefab.meta");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.GUIDToAssetPath(prefabId.ToString()).empty());
    EXPECT_FALSE(database.LoadPrefabArtifactByAssetId(prefabId, subAssetKey).has_value());

    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside);
}

TEST(AssetDatabaseFacadeTests, FileWatcherPreimportImportsSavedPrefabWithExternalAssetReferences)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;
    using namespace NLS::Engine::Serialize;

    const auto root = MakeAssetDatabaseFacadeRoot();
    NLS::Engine::GameObject gameObject("RenderableCube", "Prefab");
    auto* meshFilter = gameObject.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = gameObject.AddComponent<NLS::Engine::Components::MeshRenderer>();

    const auto meshAssetId = ParseAssetId("e7070707-0707-4707-8707-070707070707");
    const auto materialAssetId = ParseAssetId("e8080808-0808-4808-8808-080808080808");
    const std::string meshArtifactPath = "Library/Artifacts/Cube/7e0aaf65f74245f291bdf6a0c3f6c4e8";
    const std::string materialArtifactPath = "Library/Artifacts/47/47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae";
    const auto meshReference = ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(meshAssetId.GetGuid()),
        MakeLocalIdentifierInFile(meshAssetId.GetGuid(), meshArtifactPath),
        meshArtifactPath);
    const auto materialReference = ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(materialAssetId.GetGuid()),
        MakeLocalIdentifierInFile(materialAssetId.GetGuid(), materialArtifactPath),
        materialArtifactPath);
    meshFilter->SetMeshReference(MakePPtr<NLS::Render::Resources::Mesh>(meshReference));
    meshRenderer->SetMaterialReferences({
        MakePPtr<NLS::Render::Resources::Material>(materialReference)
    });

    const auto prefabId = ParseAssetId("e9090909-0909-4909-8909-090909090909");
    const auto created = PrefabEditorWorkflow().CreatePrefabFromSelection({
        &gameObject,
        {},
        prefabId,
        "Assets/Prefabs/RenderableCube.prefab"
    });
    ASSERT_EQ(created.status, PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(created.artifact.has_value());
    ASSERT_FALSE(created.artifact->Validate().HasErrors());

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.CreateTextAsset(
        created.prefabSourceText,
        "Assets/Prefabs/RenderableCube.prefab",
        prefabId));

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    EXPECT_TRUE(scheduler.Run(database, tracker, {
        AssetPreimportReason::FileWatcherChanged,
        {root / "Assets" / "Prefabs" / "RenderableCube.prefab"}
    }));

    auto prefab = database.LoadPrefabArtifactAtPath(
        "Assets/Prefabs/RenderableCube.prefab",
        "prefab:RenderableCube");
    ASSERT_TRUE(prefab.has_value());
    EXPECT_FALSE(prefab->Validate().HasErrors());

    std::filesystem::remove_all(root);
}
