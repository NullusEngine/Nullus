#include <gtest/gtest.h>

#include "LaunchArgs.h"
#include "RuntimeAssetManifestStartup.h"

#include "Assets/ArtifactManifest.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Guid.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "Rendering/Settings/GraphicsBackendUtils.h"

namespace
{
    NLS::Game::Launch::ParsedGameLaunchArgs Parse(std::initializer_list<const char*> args)
    {
        std::vector<char*> argv;
        argv.reserve(args.size());
        for (const char* arg : args)
            argv.push_back(const_cast<char*>(arg));
        return NLS::Game::Launch::ParseGameArgs(static_cast<int>(argv.size()), argv.data());
    }
}

TEST(GameLaunchArgsTests, RejectsNonDx12BackendOverrideDuringPhase1)
{
    const auto parsed = Parse({"Game.exe", "--backend", "vulkan", "TestProject.nullus"});

    EXPECT_TRUE(parsed.hasError);
    EXPECT_FALSE(parsed.showHelp);
}

TEST(GameLaunchArgsTests, ParsesShortBackendFlagAndRenderDocCaptureOptions)
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

TEST(GameLaunchArgsTests, RejectsRemovedThreadedRenderingFlag)
{
    const auto parsed = Parse({"Game.exe", "--threaded-rendering", "TestProject"});

    EXPECT_TRUE(parsed.hasError);
}

TEST(GameLaunchArgsTests, RejectsUnknownBackendName)
{
    const auto parsed = Parse({"Game.exe", "--backend", "mystery"});

    EXPECT_TRUE(parsed.hasError);
    EXPECT_FALSE(parsed.showHelp);
    EXPECT_FALSE(parsed.backendOverride.has_value());
}

TEST(GameLaunchArgsTests, ReturnsHelpWithoutTreatingItAsAnError)
{
    const auto parsed = Parse({"Game.exe", "--help"});

    EXPECT_FALSE(parsed.hasError);
    EXPECT_TRUE(parsed.showHelp);
    EXPECT_FALSE(parsed.backendOverride.has_value());
    EXPECT_FALSE(parsed.projectPathOverride.has_value());
}

TEST(GameLaunchArgsTests, ParsesMaterialValidationEvidenceOptions)
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

TEST(GameLaunchArgsTests, RejectsInvalidMaterialValidationFrameCount)
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

TEST(GameLaunchArgsTests, LoadsRuntimeAssetManifestFromProjectLibrary)
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_game_runtime_manifest_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root / "Library");

    const auto assetId = NLS::Guid::New();
    const auto manifestPath = root / "Library" / "RuntimeAssetManifest.json";
    std::ofstream output(manifestPath, std::ios::binary | std::ios::trunc);
    output
        << "{\n"
        << "  \"schemaVersion\": 1,\n"
        << "  \"targetPlatform\": \"win64\",\n"
        << "  \"assetPacks\": [\n"
        << "    {\n"
        << "      \"packName\": \"characters\",\n"
        << "      \"packVariant\": \"hd\",\n"
        << "      \"entries\": [\n"
        << "        {\n"
        << "          \"reference\": {\n"
        << "            \"assetId\": \"" << assetId.ToString() << "\",\n"
        << "            \"subAssetKey\": \"mesh:Body\"\n"
        << "          },\n"
        << "          \"artifactType\": \"mesh\",\n"
        << "          \"loaderId\": \"mesh\",\n"
        << "          \"artifactPath\": \"Artifacts/Hero/body.nmesh\",\n"
        << "          \"contentHash\": \"sha256:body\"\n"
        << "        }\n"
        << "      ]\n"
        << "    }\n"
        << "  ],\n"
        << "  \"entries\": [\n"
        << "    {\n"
        << "      \"assetId\": \"" << assetId.ToString() << "\",\n"
        << "      \"subAssetKey\": \"mesh:Body\",\n"
        << "      \"artifactType\": " << static_cast<int>(NLS::Core::Assets::ArtifactType::Mesh) << ",\n"
        << "      \"loaderId\": \"mesh\",\n"
        << "      \"artifactPath\": \"Artifacts/Hero/body.nmesh\",\n"
        << "      \"contentHash\": \"sha256:body\"\n"
        << "    }\n"
        << "  ]\n"
        << "}\n";
    output.close();

    const auto runtimeDatabase =
        NLS::Game::RuntimeAssets::LoadRuntimeAssetDatabaseForProjectSettings(root / "TestProject.nullus");

    ASSERT_TRUE(runtimeDatabase.has_value());
    const auto* entry = runtimeDatabase->Resolve(
        NLS::Core::Assets::AssetId(assetId),
        "mesh:Body");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->artifactPath, "Artifacts/Hero/body.nmesh");
    EXPECT_EQ(entry->loaderId, "mesh");
    ASSERT_EQ(runtimeDatabase->GetManifest().assetPacks.size(), 1u);
    EXPECT_EQ(runtimeDatabase->GetManifest().assetPacks.front().packName, "characters");
    ASSERT_EQ(runtimeDatabase->GetManifest().assetPacks.front().entries.size(), 1u);
    EXPECT_EQ(
        runtimeDatabase->GetManifest().assetPacks.front().entries.front().reference.subAssetKey,
        "mesh:Body");

    std::filesystem::remove_all(root);
}

TEST(GameLaunchArgsTests, RuntimeMaterialPrewarmLoadsMaterialEntriesOutsideRenderPath)
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
        "Library/Artifacts/material-guid/materials/Body.nmat",
        "sha256:material",
        {}
    });
    manifest.entries.push_back({
        meshId,
        "mesh:Body",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "Library/Artifacts/mesh-guid/meshes/Body.nmesh",
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
        "Library/Artifacts/material-guid/materials/Body.nmat");
}

TEST(GameLaunchArgsTests, RuntimeMaterialPrewarmOnlyLoadsRootDependencyClosureAndSelectedPacks)
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
        "Library/Artifacts/root/Hero.nprefab",
        "sha256:root",
        {{dependencyId, "material:Body"}}
    });
    manifest.entries.push_back({
        dependencyId,
        "material:Body",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "Library/Artifacts/dependency/Body.nmat",
        "sha256:dependency",
        {}
    });
    manifest.entries.push_back({
        inactiveId,
        "material:Unused",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "Library/Artifacts/inactive/Unused.nmat",
        "sha256:inactive",
        {}
    });
    manifest.entries.push_back({
        selectedPackId,
        "material:SelectedPack",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "Library/Artifacts/pack/Selected.nmat",
        "sha256:selected",
        {}
    });
    manifest.entries.push_back({
        inactivePackId,
        "material:InactivePack",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "Library/Artifacts/pack/Inactive.nmat",
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
            "Library/Artifacts/pack/Selected.nmat",
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
            "Library/Artifacts/pack/Inactive.nmat",
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
            "Library/Artifacts/pack/Selected.nmat"
        }));
}

TEST(GameLaunchArgsTests, RuntimeMaterialPrewarmCombinesExplicitRootsWithSelectedPacks)
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
        "Library/Artifacts/root/Hero.nprefab",
        "sha256:root",
        {{dependencyId, "material:Body"}}
    });
    manifest.entries.push_back({
        dependencyId,
        "material:Body",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "Library/Artifacts/dependency/Body.nmat",
        "sha256:dependency",
        {}
    });
    manifest.entries.push_back({
        selectedPackId,
        "material:SelectedPack",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "Library/Artifacts/pack/Selected.nmat",
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
            "Library/Artifacts/pack/Selected.nmat",
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
            "Library/Artifacts/dependency/Body.nmat",
            "Library/Artifacts/pack/Selected.nmat"
        }));
}
