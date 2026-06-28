#include <gtest/gtest.h>

#include "LaunchArgs.h"
#include "RuntimeAssetManifestStartup.h"

#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/ArtifactWriter.h"
#include "Assets/NativeArtifactContainer.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Guid.h"
#include "Rendering/Assets/ShaderArtifact.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "Rendering/Settings/GraphicsBackendUtils.h"

namespace
{
    constexpr const char* kHeroArtifactHash = "db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb";

    NLS::Game::Launch::ParsedGameLaunchArgs Parse(std::initializer_list<const char*> args)
    {
        std::vector<char*> argv;
        argv.reserve(args.size());
        for (const char* arg : args)
            argv.push_back(const_cast<char*>(arg));
        return NLS::Game::Launch::ParseGameArgs(static_cast<int>(argv.size()), argv.data());
    }

    std::string ContentHashForArtifactPath(const std::string& path)
    {
        return "sha256:" + std::filesystem::path(path).filename().generic_string();
    }

    std::string RuntimeArtifactPath(const std::string& hash)
    {
        return (std::filesystem::path("Artifacts") /
            NLS::Core::Assets::BuildArtifactStorageRelativePath(hash)).generic_string();
    }

    std::string LibraryArtifactPath(const std::string& hash)
    {
        return (std::filesystem::path("Library") / "Artifacts" /
            NLS::Core::Assets::BuildArtifactStorageRelativePath(hash)).generic_string();
    }

    class GameLaunchArgsTests : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            ResetRuntimeArtifactAuthorization();
        }

        void TearDown() override
        {
            ResetRuntimeArtifactAuthorization();
            NLS::Core::ResourceManagement::ShaderManager::ProvideAssetPaths({}, {});
        }

    private:
        static void ResetRuntimeArtifactAuthorization()
        {
            NLS::Core::Assets::ClearRuntimeArtifactAuthorization();
            NLS::Core::Assets::SetRuntimeArtifactAuthorizationEnabled(false);
        }
    };
}

TEST_F(GameLaunchArgsTests, RejectsNonDx12BackendOverrideDuringPhase1)
{
    const auto parsed = Parse({"Game.exe", "--backend", "vulkan", "TestProject.nullus"});

    EXPECT_TRUE(parsed.hasError);
    EXPECT_FALSE(parsed.showHelp);
}

TEST_F(GameLaunchArgsTests, ParsesShortBackendFlagAndRenderDocCaptureOptions)
{
    const auto parsed = Parse({"Game.exe", "-b", "dx12", "--capture-after-frames", "42", "TestProject"});

    if (NLS::Render::Settings::IsBackendSelectableForPhase1(NLS::Render::Settings::EGraphicsBackend::DX12))
        EXPECT_FALSE(parsed.hasError);
    else
        EXPECT_TRUE(parsed.hasError);
    ASSERT_TRUE(parsed.backendOverride.has_value());
    EXPECT_EQ(parsed.backendOverride.value(), NLS::Render::Settings::EGraphicsBackend::DX12);

    if (NLS::Render::Settings::IsBackendSelectableForPhase1(NLS::Render::Settings::EGraphicsBackend::DX12))
    {
        EXPECT_TRUE(parsed.renderDocSettings.enabled);
        EXPECT_EQ(parsed.renderDocSettings.startupCaptureAfterFrames, 42u);
        EXPECT_TRUE(parsed.hasRenderDocOverride);
        ASSERT_TRUE(parsed.projectPathOverride.has_value());
        EXPECT_EQ(parsed.projectPathOverride.value(), "TestProject");
    }
    else
    {
        EXPECT_FALSE(parsed.renderDocSettings.enabled);
        EXPECT_EQ(parsed.renderDocSettings.startupCaptureAfterFrames, 0u);
        EXPECT_FALSE(parsed.projectPathOverride.has_value());
    }
}

TEST_F(GameLaunchArgsTests, RejectsRemovedThreadedRenderingFlag)
{
    const auto parsed = Parse({"Game.exe", "--threaded-rendering", "TestProject"});

    EXPECT_TRUE(parsed.hasError);
}

TEST_F(GameLaunchArgsTests, RejectsUnknownBackendName)
{
    const auto parsed = Parse({"Game.exe", "--backend", "mystery"});

    EXPECT_TRUE(parsed.hasError);
    EXPECT_FALSE(parsed.showHelp);
    EXPECT_FALSE(parsed.backendOverride.has_value());
}

TEST_F(GameLaunchArgsTests, ReturnsHelpWithoutTreatingItAsAnError)
{
    const auto parsed = Parse({"Game.exe", "--help"});

    EXPECT_FALSE(parsed.hasError);
    EXPECT_TRUE(parsed.showHelp);
    EXPECT_FALSE(parsed.backendOverride.has_value());
    EXPECT_FALSE(parsed.projectPathOverride.has_value());
}

TEST_F(GameLaunchArgsTests, ParsesMaterialValidationEvidenceOptions)
{
    const auto parsed = Parse({
        "Game.exe",
        "--material-validation-output",
        "Evidence/T189.png",
        "--material-validation-summary",
        "Evidence/T189.txt",
        "--material-validation-after-frames",
        "42",
        "Build/T189MaterialValidation/T189MaterialValidation.nullus"
    });

    ASSERT_FALSE(parsed.hasError);
    ASSERT_TRUE(parsed.materialValidation.has_value());
    EXPECT_EQ(parsed.materialValidation->outputPath, "Evidence/T189.png");
    EXPECT_EQ(parsed.materialValidation->summaryPath, "Evidence/T189.txt");
    EXPECT_EQ(parsed.materialValidation->captureAfterFrames, 42u);
    ASSERT_TRUE(parsed.projectPathOverride.has_value());
}

TEST_F(GameLaunchArgsTests, RejectsInvalidMaterialValidationFrameCount)
{
    const auto parsed = Parse({
        "Game.exe",
        "--material-validation-output",
        "Evidence/T189.png",
        "--material-validation-after-frames",
        "not-a-number"
    });

    EXPECT_TRUE(parsed.hasError);
}

TEST_F(GameLaunchArgsTests, LoadsRuntimeAssetDatabaseFromProjectArtifactDB)
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_game_runtime_artifact_db_" + NLS::Guid::New().ToString());

    const auto assetId = NLS::Guid::New();
    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = NLS::Core::Assets::AssetId(assetId);
    manifest.importerId = "scene-model";
    manifest.importerVersion = 1u;
    manifest.targetPlatform = "win64";
    manifest.primarySubAssetKey = "mesh:Body";
    manifest.subAssets.push_back({
        manifest.sourceAssetId,
        "mesh:Body",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "win64",
        RuntimeArtifactPath(kHeroArtifactHash),
        "sha256:" + std::string(kHeroArtifactHash),
        "Body"
    });

    NLS::Core::Assets::ArtifactDatabase artifactDatabase;
    artifactDatabase.UpsertManifest(
        manifest,
        "Assets/Models/Hero.gltf",
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(artifactDatabase.Save(root / "Library" / "ArtifactDB"));

    const auto runtimeDatabase =
        NLS::Game::RuntimeAssets::LoadRuntimeAssetDatabaseForProjectSettings(root / "TestProject.nullus");

    ASSERT_TRUE(runtimeDatabase.has_value());
    const auto* entry = runtimeDatabase->Resolve(
        NLS::Core::Assets::AssetId(assetId),
        "mesh:Body");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->artifactPath, RuntimeArtifactPath(kHeroArtifactHash));
    EXPECT_EQ(entry->loaderId, "mesh");

    std::filesystem::remove_all(root);
}

TEST_F(GameLaunchArgsTests, RuntimeAssetDatabaseLoadedFromArtifactDbUsesArtifactSourceAssetId)
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_game_runtime_artifact_db_subasset_identity_" + NLS::Guid::New().ToString());

    const auto modelId = NLS::Core::Assets::AssetId(NLS::Guid::New());
    const auto materialId = NLS::Core::Assets::AssetId(NLS::Guid::New());

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = modelId;
    manifest.importerId = "scene-model";
    manifest.importerVersion = 1u;
    manifest.targetPlatform = "win64";
    manifest.primarySubAssetKey = "prefab:Hero";
    manifest.subAssets.push_back({
        modelId,
        "prefab:Hero",
        NLS::Core::Assets::ArtifactType::Prefab,
        "prefab",
        "win64",
        RuntimeArtifactPath(kHeroArtifactHash),
        "sha256:" + std::string(kHeroArtifactHash),
        "Hero"
    });
    constexpr const char* kMaterialArtifactHash = "abababababababababababababababababababababababababababababababab";
    manifest.subAssets.push_back({
        materialId,
        "material:Body",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "win64",
        RuntimeArtifactPath(kMaterialArtifactHash),
        "sha256:" + std::string(kMaterialArtifactHash),
        "Body"
    });

    NLS::Core::Assets::ArtifactDatabase artifactDatabase;
    artifactDatabase.UpsertManifest(
        manifest,
        "Assets/Models/Hero.gltf",
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(artifactDatabase.Save(root / "Library" / "ArtifactDB"));

    const auto runtimeDatabase =
        NLS::Game::RuntimeAssets::LoadRuntimeAssetDatabaseForProjectSettings(root / "TestProject.nullus");

    ASSERT_TRUE(runtimeDatabase.has_value());
    EXPECT_NE(runtimeDatabase->Resolve(modelId, "prefab:Hero"), nullptr);
    EXPECT_NE(runtimeDatabase->Resolve(materialId, "material:Body"), nullptr);
    EXPECT_EQ(runtimeDatabase->Resolve(modelId, "material:Body"), nullptr);

    std::filesystem::remove_all(root);
}

TEST_F(GameLaunchArgsTests, LoadsRuntimeAssetDatabaseByIgnoringEditorTargetRecords)
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_game_runtime_artifact_db_mixed_targets_" + NLS::Guid::New().ToString());

    const auto winAssetId = NLS::Core::Assets::AssetId(NLS::Guid::New());
    NLS::Core::Assets::ArtifactManifest winManifest;
    winManifest.sourceAssetId = winAssetId;
    winManifest.importerId = "scene-model";
    winManifest.importerVersion = 1u;
    winManifest.targetPlatform = "win64";
    winManifest.primarySubAssetKey = "mesh:Body";
    winManifest.subAssets.push_back({
        winAssetId,
        "mesh:Body",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "win64",
        RuntimeArtifactPath(kHeroArtifactHash),
        "sha256:" + std::string(kHeroArtifactHash),
        "Body"
    });

    const auto editorHash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const auto editorAssetId = NLS::Core::Assets::AssetId(NLS::Guid::New());
    NLS::Core::Assets::ArtifactManifest editorManifest;
    editorManifest.sourceAssetId = editorAssetId;
    editorManifest.importerId = "scene-model";
    editorManifest.importerVersion = 1u;
    editorManifest.targetPlatform = "editor";
    editorManifest.primarySubAssetKey = "mesh:Preview";
    editorManifest.subAssets.push_back({
        editorAssetId,
        "mesh:Preview",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor",
        RuntimeArtifactPath(editorHash),
        "sha256:" + std::string(editorHash),
        "Preview"
    });

    NLS::Core::Assets::ArtifactDatabase artifactDatabase;
    artifactDatabase.UpsertManifest(
        winManifest,
        "Assets/Models/Hero.gltf",
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    artifactDatabase.UpsertManifest(
        editorManifest,
        "Assets/Models/Preview.gltf",
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(artifactDatabase.Save(root / "Library" / "ArtifactDB"));

    const auto runtimeDatabase =
        NLS::Game::RuntimeAssets::LoadRuntimeAssetDatabaseForProjectSettings(root / "TestProject.nullus");

    ASSERT_TRUE(runtimeDatabase.has_value())
        << "Runtime startup should ignore editor-only records in the unified project ArtifactDB "
           "instead of rejecting the entire database.";
    EXPECT_NE(runtimeDatabase->Resolve(winAssetId, "mesh:Body"), nullptr);
    EXPECT_EQ(runtimeDatabase->Resolve(editorAssetId, "mesh:Preview"), nullptr);

    std::filesystem::remove_all(root);
}

TEST_F(GameLaunchArgsTests, RejectsEditorOnlyRuntimeArtifactDatabase)
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_game_runtime_artifact_db_editor_only_" + NLS::Guid::New().ToString());

    const auto editorAssetId = NLS::Core::Assets::AssetId(NLS::Guid::New());
    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = editorAssetId;
    manifest.importerId = "scene-model";
    manifest.importerVersion = 1u;
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "mesh:Preview";
    manifest.subAssets.push_back({
        editorAssetId,
        "mesh:Preview",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor",
        RuntimeArtifactPath(kHeroArtifactHash),
        "sha256:" + std::string(kHeroArtifactHash),
        "Preview"
    });

    NLS::Core::Assets::ArtifactDatabase artifactDatabase;
    artifactDatabase.UpsertManifest(
        manifest,
        "Assets/Models/Preview.gltf",
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(artifactDatabase.Save(root / "Library" / "ArtifactDB"));

    const auto runtimeDatabase =
        NLS::Game::RuntimeAssets::LoadRuntimeAssetDatabaseForProjectSettings(root / "TestProject.nullus");

    EXPECT_FALSE(runtimeDatabase.has_value())
        << "Runtime startup must reject editor target artifacts even when every record uses that same target.";
    EXPECT_FALSE(NLS::Core::Assets::IsRuntimeArtifactAuthorizationEnabled());

    std::filesystem::remove_all(root);
}

TEST_F(GameLaunchArgsTests, RejectsMissingRuntimeArtifactDatabase)
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_game_runtime_artifact_db_missing_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root / "Library");

    NLS::Core::Assets::ClearRuntimeArtifactAuthorization();
    NLS::Core::Assets::RegisterRuntimeAuthorizedArtifactPath(RuntimeArtifactPath(kHeroArtifactHash));
    NLS::Core::Assets::SetRuntimeArtifactAuthorizationEnabled(true);

    const auto runtimeDatabase =
        NLS::Game::RuntimeAssets::LoadRuntimeAssetDatabaseForProjectSettings(root / "TestProject.nullus");

    EXPECT_FALSE(runtimeDatabase.has_value());
    EXPECT_FALSE(NLS::Core::Assets::IsRuntimeArtifactAuthorizationEnabled());

    std::filesystem::remove_all(root);
}

TEST_F(GameLaunchArgsTests, RejectsRuntimeArtifactDatabaseMismatchedArtifactMetadata)
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_game_runtime_artifact_db_metadata_reject_" + NLS::Guid::New().ToString());

    const auto assetId = NLS::Guid::New();
    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = NLS::Core::Assets::AssetId(assetId);
    manifest.importerId = "material";
    manifest.importerVersion = 1u;
    manifest.targetPlatform = "win64";
    manifest.primarySubAssetKey = "material:Hero";
    manifest.subAssets.push_back({
        manifest.sourceAssetId,
        "material:Hero",
        NLS::Core::Assets::ArtifactType::Material,
        "shader",
        "win64",
        RuntimeArtifactPath(kHeroArtifactHash),
        "sha256:" + std::string(kHeroArtifactHash),
        "Hero"
    });

    NLS::Core::Assets::ArtifactDatabase artifactDatabase;
    artifactDatabase.UpsertManifest(
        manifest,
        "Assets/Materials/Hero.mat",
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(artifactDatabase.Save(root / "Library" / "ArtifactDB"));

    const auto runtimeDatabase =
        NLS::Game::RuntimeAssets::LoadRuntimeAssetDatabaseForProjectSettings(root / "TestProject.nullus");

    EXPECT_FALSE(runtimeDatabase.has_value());

    std::filesystem::remove_all(root);
}

TEST_F(GameLaunchArgsTests, RuntimeMaterialPrewarmLoadsMaterialEntriesOutsideRenderPath)
{
    struct CountingMaterialManager final : NLS::Core::ResourceManagement::MaterialManager
    {
        Material* LoadArtifactWithoutTextures(const std::string& path) override
        {
            loadedPaths.push_back(path);
            return reinterpret_cast<Material*>(1);
        }

        std::vector<std::string> loadedPaths;
    };

    const auto materialId = NLS::Core::Assets::AssetId(NLS::Guid::New());
    const auto meshId = NLS::Core::Assets::AssetId(NLS::Guid::New());

    NLS::Engine::Assets::RuntimeAssetManifest manifest;
    manifest.roots.push_back({materialId, "material:Body"});
    manifest.entries.push_back({
        materialId,
        "material:Body",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        LibraryArtifactPath("47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae"),
        "sha256:material",
        {}
    });
    manifest.entries.push_back({
        meshId,
        "mesh:Body",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        LibraryArtifactPath("eab993d3e507e9b6427246cc0f936120bbb30a9cbc60e1c782e8eda361f75f3b"),
        "sha256:mesh",
        {}
    });

    NLS::Engine::Assets::RuntimeAssetDatabase runtimeDatabase(std::move(manifest));
    CountingMaterialManager materialManager;

    EXPECT_EQ(
        NLS::Game::RuntimeAssets::PrewarmRuntimeMaterialAssets(runtimeDatabase, materialManager),
        1u);
    ASSERT_EQ(materialManager.loadedPaths.size(), 1u);
    EXPECT_EQ(
        materialManager.loadedPaths.front(),
        LibraryArtifactPath("47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae"));
}

TEST_F(GameLaunchArgsTests, RuntimeShaderManagerRejectsUnmanifestedContentArtifactPaths)
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_game_runtime_manifest_unlisted_artifact_reject_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root / "Project" / "Assets");
    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    shaderManager.ProvideAssetPaths(
        (root / "Project" / "Assets").string() + std::string(1, std::filesystem::path::preferred_separator),
        (root / "App" / "Assets" / "Engine").string() + std::string(1, std::filesystem::path::preferred_separator));

    NLS::Core::Assets::NativeArtifactMetadata metadata;
    metadata.artifactType = NLS::Core::Assets::ArtifactType::Shader;
    metadata.schemaName = "shader";
    metadata.schemaVersion = 1u;
    metadata.sourceAssetId = NLS::Core::Assets::AssetId(NLS::Guid::New());
    metadata.subAssetKey = "shader:Unlisted";
    metadata.displayName = "Unlisted";
    metadata.importerId = "shader";
    metadata.importerVersion = 1u;
    metadata.targetPlatform = "editor";
    const std::vector<uint8_t> payload{'s', 'h', 'a', 'd', 'e', 'r'};
    const auto stored = NLS::Core::Assets::WriteNativeArtifactContainer(metadata, payload);
    const auto artifactName = NLS::Core::Assets::BuildArtifactStorageFileName(stored.data(), stored.size());
    const auto portableArtifactPath = LibraryArtifactPath(artifactName);
    const auto artifactPath = root / portableArtifactPath;
    {
        std::filesystem::create_directories(artifactPath.parent_path());
        std::ofstream output(artifactPath, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(stored.data()), static_cast<std::streamsize>(stored.size()));
    }

    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::New());
    const auto listedHash = std::string(64u, 'a');
    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = assetId;
    manifest.importerId = "shader";
    manifest.importerVersion = 1u;
    manifest.targetPlatform = "win64";
    manifest.primarySubAssetKey = "shader:Listed";
    manifest.subAssets.push_back({
        assetId,
        "shader:Listed",
        NLS::Core::Assets::ArtifactType::Shader,
        "shader",
        "win64",
        LibraryArtifactPath(listedHash),
        "sha256:" + listedHash,
        "Listed"
    });

    NLS::Core::Assets::ArtifactDatabase artifactDatabase;
    artifactDatabase.UpsertManifest(
        manifest,
        "Assets/Shaders/Listed.shader",
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(artifactDatabase.Save(root / "Library" / "ArtifactDB"));
    const auto runtimeDatabase =
        NLS::Game::RuntimeAssets::LoadRuntimeAssetDatabaseForProjectSettings(root / "TestProject.nullus");
    ASSERT_TRUE(runtimeDatabase.has_value());

    auto* shader = shaderManager.GetResource(
        portableArtifactPath,
        true);

    EXPECT_EQ(shader, nullptr);

    shaderManager.UnloadResources();
    std::filesystem::remove_all(root);
}

TEST_F(GameLaunchArgsTests, RuntimeDirectShaderArtifactLoadRequiresManifestAuthorization)
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_game_runtime_direct_shader_artifact_auth_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root / "Library" / "Artifacts");

    NLS::Render::Assets::ShaderArtifact artifact;
    artifact.sourcePath = "Assets/Shaders/Direct.shader";
    artifact.subAssetKey = "shader:Direct";
    artifact.targetPlatform = "editor";
    const auto payload = NLS::Render::Assets::SerializeShaderArtifact(artifact);

    NLS::Core::Assets::NativeArtifactMetadata metadata;
    metadata.artifactType = NLS::Core::Assets::ArtifactType::Shader;
    metadata.schemaName = "shader";
    metadata.schemaVersion = 1u;
    metadata.sourceAssetId = NLS::Core::Assets::AssetId(NLS::Guid::New());
    metadata.subAssetKey = artifact.subAssetKey;
    metadata.displayName = "Direct";
    metadata.importerId = "shader";
    metadata.importerVersion = 1u;
    metadata.targetPlatform = "editor";
    const auto stored = NLS::Core::Assets::WriteNativeArtifactContainer(metadata, payload);
    const auto artifactName = NLS::Core::Assets::BuildArtifactStorageFileName(stored.data(), stored.size());
    const auto portablePath = LibraryArtifactPath(artifactName);
    const auto artifactPath = root / portablePath;
    {
        std::filesystem::create_directories(artifactPath.parent_path());
        std::ofstream output(artifactPath, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(stored.data()), static_cast<std::streamsize>(stored.size()));
    }

    NLS::Core::Assets::ClearRuntimeArtifactAuthorization();
    NLS::Core::Assets::SetRuntimeArtifactAuthorizationEnabled(true);
    EXPECT_FALSE(NLS::Render::Assets::LoadShaderArtifact(artifactPath).has_value());

    NLS::Core::Assets::RegisterRuntimeAuthorizedArtifactPath(portablePath);
    const auto loaded = NLS::Render::Assets::LoadShaderArtifact(artifactPath);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->subAssetKey, artifact.subAssetKey);

    NLS::Core::Assets::ClearRuntimeArtifactAuthorization();
    NLS::Core::Assets::SetRuntimeArtifactAuthorizationEnabled(false);
    std::filesystem::remove_all(root);
}

TEST_F(GameLaunchArgsTests, RuntimeMaterialPrewarmOnlyLoadsRootDependencyClosureAndSelectedPacks)
{
    struct CountingMaterialManager final : NLS::Core::ResourceManagement::MaterialManager
    {
        Material* LoadArtifactWithoutTextures(const std::string& path) override
        {
            loadedPaths.push_back(path);
            return reinterpret_cast<Material*>(1);
        }

        std::vector<std::string> loadedPaths;
    };

    const auto rootId = NLS::Core::Assets::AssetId(NLS::Guid::New());
    const auto dependencyId = NLS::Core::Assets::AssetId(NLS::Guid::New());
    const auto inactiveId = NLS::Core::Assets::AssetId(NLS::Guid::New());
    const auto selectedPackId = NLS::Core::Assets::AssetId(NLS::Guid::New());
    const auto inactivePackId = NLS::Core::Assets::AssetId(NLS::Guid::New());

    NLS::Engine::Assets::RuntimeAssetManifest manifest;
    manifest.roots.push_back({rootId, "prefab:Hero"});
    manifest.entries.push_back({
        rootId,
        "prefab:Hero",
        NLS::Core::Assets::ArtifactType::Prefab,
        "prefab",
        "Library/Artifacts/root/670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772",
        "sha256:root",
        {{dependencyId, "material:Body"}}
    });
    manifest.entries.push_back({
        dependencyId,
        "material:Body",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "Library/Artifacts/dependency/47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae",
        "sha256:dependency",
        {}
    });
    manifest.entries.push_back({
        inactiveId,
        "material:Unused",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "Library/Artifacts/inactive/c71f9ced4ae9d79ba056ed893c377e4e95db4821ada21be6ffe5efcda5643098",
        "sha256:inactive",
        {}
    });
    manifest.entries.push_back({
        selectedPackId,
        "material:SelectedPack",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "Library/Artifacts/pack/adb91de731a17ce4ca8979427a445db48a46097ff1a5251f9059d84649c2acac",
        "sha256:selected",
        {}
    });
    manifest.entries.push_back({
        inactivePackId,
        "material:InactivePack",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "Library/Artifacts/pack/08ef9a3a7fd5a9f1a40d3cf084d219d1cb50ba0080a2d46a1d89c07499769a21",
        "sha256:inactive-pack",
        {}
    });
    manifest.assetPacks.push_back({
        "characters",
        "hd",
        {{
            {selectedPackId, "material:SelectedPack"},
            NLS::Core::Assets::ArtifactType::Material,
            "material",
            "Library/Artifacts/pack/adb91de731a17ce4ca8979427a445db48a46097ff1a5251f9059d84649c2acac",
            "sha256:selected",
            {}
        }}
    });
    manifest.assetPacks.push_back({
        "characters",
        "sd",
        {{
            {inactivePackId, "material:InactivePack"},
            NLS::Core::Assets::ArtifactType::Material,
            "material",
            "Library/Artifacts/pack/08ef9a3a7fd5a9f1a40d3cf084d219d1cb50ba0080a2d46a1d89c07499769a21",
            "sha256:inactive-pack",
            {}
        }}
    });

    NLS::Engine::Assets::RuntimeAssetDatabase runtimeDatabase(std::move(manifest));
    CountingMaterialManager materialManager;
    NLS::Game::RuntimeAssets::RuntimeMaterialPrewarmOptions options;
    options.assetPacks.push_back({"characters", "hd"});

    EXPECT_EQ(
        NLS::Game::RuntimeAssets::PrewarmRuntimeMaterialAssets(runtimeDatabase, materialManager, options),
        1u);
    EXPECT_EQ(
        materialManager.loadedPaths,
        std::vector<std::string>({
            "Library/Artifacts/pack/adb91de731a17ce4ca8979427a445db48a46097ff1a5251f9059d84649c2acac"
        }));
}

TEST_F(GameLaunchArgsTests, RuntimeAssetDatabaseLoadedFromArtifactDbPreservesDependencyClosure)
{
    struct CountingMaterialManager final : NLS::Core::ResourceManagement::MaterialManager
    {
        Material* LoadArtifactWithoutTextures(const std::string& path) override
        {
            loadedPaths.push_back(path);
            return reinterpret_cast<Material*>(1);
        }

        std::vector<std::string> loadedPaths;
    };

    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_runtime_artifactdb_dependency_closure_" + NLS::Guid::New().ToString());
    const auto prefabId = NLS::Core::Assets::AssetId(NLS::Guid::New());
    const auto materialId = NLS::Core::Assets::AssetId(NLS::Guid::New());
    const auto prefabHash = "670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772";
    const auto materialHash = "47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae";
    const auto prefabArtifactPath = LibraryArtifactPath(prefabHash);
    const auto materialArtifactPath = LibraryArtifactPath(materialHash);

    NLS::Core::Assets::ArtifactManifest prefabManifest;
    prefabManifest.sourceAssetId = prefabId;
    prefabManifest.importerId = "prefab";
    prefabManifest.importerVersion = 1u;
    prefabManifest.targetPlatform = "win64";
    prefabManifest.primarySubAssetKey = "prefab:Hero";
    prefabManifest.subAssets.push_back({
        prefabId,
        "prefab:Hero",
        NLS::Core::Assets::ArtifactType::Prefab,
        "prefab",
        "win64",
        prefabArtifactPath,
        "sha256:" + std::string(prefabHash),
        "Hero"
    });
    prefabManifest.dependencies.push_back({
        NLS::Core::Assets::AssetDependencyKind::ImportedArtifact,
        materialId.ToString(),
        "material:Body"
    });

    NLS::Core::Assets::ArtifactManifest materialManifest;
    materialManifest.sourceAssetId = materialId;
    materialManifest.importerId = "material";
    materialManifest.importerVersion = 1u;
    materialManifest.targetPlatform = "win64";
    materialManifest.primarySubAssetKey = "material:Body";
    materialManifest.subAssets.push_back({
        materialId,
        "material:Body",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "win64",
        materialArtifactPath,
        "sha256:" + std::string(materialHash),
        "Body"
    });

    NLS::Core::Assets::ArtifactDatabase artifactDatabase;
    artifactDatabase.UpsertManifest(
        prefabManifest,
        "Assets/Prefabs/Hero.prefab",
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    artifactDatabase.UpsertManifest(
        materialManifest,
        "Assets/Materials/Body.mat",
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(artifactDatabase.Save(root / "Library" / "ArtifactDB"));

    const auto runtimeDatabase =
        NLS::Game::RuntimeAssets::LoadRuntimeAssetDatabaseForProjectSettings(root / "TestProject.nullus");
    ASSERT_TRUE(runtimeDatabase.has_value());
    const auto* prefabEntry = runtimeDatabase->Resolve({prefabId, "prefab:Hero"});
    ASSERT_NE(prefabEntry, nullptr);
    ASSERT_EQ(prefabEntry->dependencies.size(), 1u);
    EXPECT_EQ(prefabEntry->dependencies.front().assetId, materialId);
    EXPECT_EQ(prefabEntry->dependencies.front().subAssetKey, "material:Body");

    CountingMaterialManager materialManager;
    NLS::Game::RuntimeAssets::RuntimeMaterialPrewarmOptions options;
    options.roots.push_back({prefabId, "prefab:Hero"});
    EXPECT_EQ(
        NLS::Game::RuntimeAssets::PrewarmRuntimeMaterialAssets(*runtimeDatabase, materialManager, options),
        1u);
    EXPECT_EQ(materialManager.loadedPaths, std::vector<std::string>({materialArtifactPath}));

    std::filesystem::remove_all(root);
}

TEST_F(GameLaunchArgsTests, RuntimeMaterialPrewarmCombinesExplicitRootsWithSelectedPacks)
{
    struct CountingMaterialManager final : NLS::Core::ResourceManagement::MaterialManager
    {
        Material* LoadArtifactWithoutTextures(const std::string& path) override
        {
            loadedPaths.push_back(path);
            return reinterpret_cast<Material*>(1);
        }

        std::vector<std::string> loadedPaths;
    };

    const auto rootId = NLS::Core::Assets::AssetId(NLS::Guid::New());
    const auto dependencyId = NLS::Core::Assets::AssetId(NLS::Guid::New());
    const auto selectedPackId = NLS::Core::Assets::AssetId(NLS::Guid::New());

    NLS::Engine::Assets::RuntimeAssetManifest manifest;
    manifest.roots.push_back({rootId, "prefab:Hero"});
    manifest.entries.push_back({
        rootId,
        "prefab:Hero",
        NLS::Core::Assets::ArtifactType::Prefab,
        "prefab",
        "Library/Artifacts/root/670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772",
        "sha256:root",
        {{dependencyId, "material:Body"}}
    });
    manifest.entries.push_back({
        dependencyId,
        "material:Body",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "Library/Artifacts/dependency/47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae",
        "sha256:dependency",
        {}
    });
    manifest.entries.push_back({
        selectedPackId,
        "material:SelectedPack",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "Library/Artifacts/pack/adb91de731a17ce4ca8979427a445db48a46097ff1a5251f9059d84649c2acac",
        "sha256:selected",
        {}
    });
    manifest.assetPacks.push_back({
        "characters",
        "hd",
        {{
            {selectedPackId, "material:SelectedPack"},
            NLS::Core::Assets::ArtifactType::Material,
            "material",
            "Library/Artifacts/pack/adb91de731a17ce4ca8979427a445db48a46097ff1a5251f9059d84649c2acac",
            "sha256:selected",
            {}
        }}
    });

    NLS::Engine::Assets::RuntimeAssetDatabase runtimeDatabase(std::move(manifest));
    CountingMaterialManager materialManager;
    NLS::Game::RuntimeAssets::RuntimeMaterialPrewarmOptions options;
    options.roots.push_back({rootId, "prefab:Hero"});
    options.assetPacks.push_back({"characters", "hd"});

    EXPECT_EQ(
        NLS::Game::RuntimeAssets::PrewarmRuntimeMaterialAssets(runtimeDatabase, materialManager, options),
        2u);
    std::sort(materialManager.loadedPaths.begin(), materialManager.loadedPaths.end());
    EXPECT_EQ(
        materialManager.loadedPaths,
        std::vector<std::string>({
            "Library/Artifacts/dependency/47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae",
            "Library/Artifacts/pack/adb91de731a17ce4ca8979427a445db48a46097ff1a5251f9059d84649c2acac"
        }));
}
