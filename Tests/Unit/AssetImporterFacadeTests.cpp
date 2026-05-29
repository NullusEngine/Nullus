#include <gtest/gtest.h>

#include "Assets/AssetMeta.h"
#include "Assets/AssetImporterFacade.h"
#include "Guid.h"
#include "Rendering/Assets/SceneImportPipeline.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
std::filesystem::path MakeAssetImporterFacadeRoot()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_asset_importer_facade_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root / "Assets");
    return root;
}

void WriteTextFile(const std::filesystem::path& path, const std::string& contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
}

NLS::Core::Assets::AssetId MakeAssetId(const char* guid)
{
    return NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));
}
}

TEST(AssetImporterFacadeTests, GetAtPathExposesSerializedSettingsDirtyStateAndSaveAndReimport)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetImporterFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"}})");

    AssetImporterFacade facade({root});
    ASSERT_TRUE(facade.Refresh());

    auto importer = facade.GetAtPath("Assets/Models/Hero.gltf");
    ASSERT_TRUE(importer.has_value());
    EXPECT_EQ(importer->assetPath, "Assets/Models/Hero.gltf");
    EXPECT_EQ(importer->importerId, "scene-model");
    EXPECT_EQ(importer->assetType, NLS::Core::Assets::AssetType::ModelScene);
    EXPECT_FALSE(importer->dirty);

    ASSERT_TRUE(facade.SetSerializedSetting("Assets/Models/Hero.gltf", "globalScale", "0.01"));
    importer = facade.GetAtPath("Assets/Models/Hero.gltf");
    ASSERT_TRUE(importer.has_value());
    EXPECT_TRUE(importer->dirty);
    EXPECT_EQ(importer->serializedSettings.at("globalScale"), "0.01");
    EXPECT_EQ(facade.GetQueuedReimportCount(), 0u);

    ASSERT_TRUE(facade.SaveAndReimport("Assets/Models/Hero.gltf"));
    importer = facade.GetAtPath("Assets/Models/Hero.gltf");
    ASSERT_TRUE(importer.has_value());
    EXPECT_FALSE(importer->dirty);
    EXPECT_EQ(importer->serializedSettings.at("globalScale"), "0.01");
    EXPECT_EQ(facade.GetQueuedReimportCount(), 1u);

    const auto loadedMeta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(root / "Assets" / "Models" / "Hero.gltf"));
    ASSERT_TRUE(loadedMeta.has_value());
    EXPECT_EQ(loadedMeta->settings.at("globalScale"), "0.01");

    std::filesystem::remove_all(root);
}

TEST(AssetImporterFacadeTests, FailedSaveAndReimportPreservesDirtyStateAndQueuedReimport)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetImporterFacadeRoot();
    const auto assetPath = root / "Assets" / "Models" / "MissingAfterEdit.gltf";
    WriteTextFile(assetPath, R"({"asset":{"version":"2.0"}})");

    AssetImporterFacade facade({root});
    ASSERT_TRUE(facade.Refresh());
    ASSERT_TRUE(facade.SetSerializedSetting("Assets/Models/MissingAfterEdit.gltf", "globalScale", "0.25"));

    std::filesystem::remove(assetPath);

    EXPECT_FALSE(facade.SaveAndReimport("Assets/Models/MissingAfterEdit.gltf"));
    EXPECT_EQ(facade.GetQueuedReimportCount(), 1u);

    const auto loadedMeta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(assetPath));
    ASSERT_TRUE(loadedMeta.has_value());
    EXPECT_EQ(loadedMeta->settings.at("globalScale"), "0.25");
    EXPECT_EQ(loadedMeta->settings.at("NULLUS_IMPORTER_DIRTY"), "true");

    std::filesystem::remove_all(root);
}

TEST(AssetImporterFacadeTests, SaveAndReimportReportsProgressWhenTrackerIsProvided)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetImporterFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "ProgressHero.gltf", R"({"asset":{"version":"2.0"}})");

    AssetImporterFacade facade({root});
    ASSERT_TRUE(facade.Refresh());
    ASSERT_TRUE(facade.SetSerializedSetting("Assets/Models/ProgressHero.gltf", "globalScale", "1.5"));

    ImportProgressTracker tracker;
    ASSERT_TRUE(facade.SaveAndReimport("Assets/Models/ProgressHero.gltf", tracker));

    const auto progress = tracker.GetBatchProgress();
    EXPECT_EQ(progress.completedAssets, 1u);
    EXPECT_FALSE(progress.activeJob.has_value());
    const auto importer = facade.GetAtPath("Assets/Models/ProgressHero.gltf");
    ASSERT_TRUE(importer.has_value());
    EXPECT_FALSE(importer->dirty);

    const auto loadedMeta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(root / "Assets" / "Models" / "ProgressHero.gltf"));
    ASSERT_TRUE(loadedMeta.has_value());
    EXPECT_EQ(loadedMeta->settings.find("NULLUS_IMPORTER_DIRTY"), loadedMeta->settings.end());

    std::filesystem::remove_all(root);
}

TEST(AssetImporterFacadeTests, SaveAndReimportRefreshesIndexerBeforeLoadingImporterState)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetImporterFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "DirectReimport.gltf", R"({"asset":{"version":"2.0"}})");

    AssetImporterFacade facade({root});
    ImportProgressTracker tracker;

    ASSERT_TRUE(facade.SaveAndReimport("Assets/Models/DirectReimport.gltf", tracker));

    const auto events = tracker.GetEvents({1u});
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.front().phase, ImportPhase::Queued);
    EXPECT_EQ(events.front().sourcePath, "Assets/Models/DirectReimport.gltf");
    EXPECT_EQ(events.back().terminalStatus, ImportJobTerminalStatus::Succeeded);

    const auto importer = facade.GetAtPath("Assets/Models/DirectReimport.gltf");
    ASSERT_TRUE(importer.has_value());
    EXPECT_FALSE(importer->dirty);

    std::filesystem::remove_all(root);
}

TEST(AssetImporterFacadeTests, RefreshScansAllConfiguredAssetRoots)
{
    using namespace NLS::Editor::Assets;

    const auto projectRoot = MakeAssetImporterFacadeRoot();
    const auto packageRoot =
        std::filesystem::temp_directory_path() /
        ("nullus_asset_importer_package_root_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(packageRoot / "Packages" / "Starter");
    WriteTextFile(projectRoot / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"}})");
    WriteTextFile(packageRoot / "Packages" / "Starter" / "Tree.obj", "o Tree");

    AssetImporterFacade facade({projectRoot, packageRoot});
    ASSERT_TRUE(facade.Refresh());

    auto projectImporter = facade.GetAtPath("Assets/Models/Hero.gltf");
    ASSERT_TRUE(projectImporter.has_value());
    EXPECT_EQ(projectImporter->importerId, "scene-model");

    auto packageImporter = facade.GetAtPath("Packages/Starter/Tree.obj");
    ASSERT_TRUE(packageImporter.has_value());
    EXPECT_EQ(packageImporter->assetPath, "Packages/Starter/Tree.obj");
    EXPECT_EQ(packageImporter->importerId, "scene-model");

    std::filesystem::remove_all(projectRoot);
    std::filesystem::remove_all(packageRoot);
}

TEST(AssetImporterFacadeTests, ImportSettingsRejectReadOnlyRoots)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetImporterFacadeRoot();
    std::filesystem::create_directories(root / "Packages" / "Starter");
    WriteTextFile(root / "Packages" / "Starter" / "Tree.obj", "o Tree");

    AssetImporterFacade facade({
        {root, false, {}},
        {root / "Packages", true, "Packages"}
    });
    ASSERT_TRUE(facade.Refresh());

    auto importer = facade.GetAtPath("Packages/Starter/Tree.obj");
    ASSERT_TRUE(importer.has_value());
    EXPECT_EQ(importer->assetPath, "Packages/Starter/Tree.obj");

    EXPECT_FALSE(facade.SetSerializedSetting("Packages/Starter/Tree.obj", "scale", "2"));
    EXPECT_FALSE(facade.SetModelImporterSettings("Packages/Starter/Tree.obj", {}));

    const auto meta = NLS::Core::Assets::AssetMeta::Load(root / "Packages" / "Starter" / "Tree.obj.meta");
    EXPECT_FALSE(meta.has_value());

    std::filesystem::remove_all(root);
}

TEST(AssetImporterFacadeTests, RefreshReportsDuplicateEditorPathAliases)
{
    using namespace NLS::Editor::Assets;

    const auto projectRoot = MakeAssetImporterFacadeRoot();
    const auto packageRoot =
        std::filesystem::temp_directory_path() /
        ("nullus_asset_importer_alias_root_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(packageRoot / "Assets" / "Models");
    WriteTextFile(projectRoot / "Assets" / "Models" / "Hero.obj", "project");
    WriteTextFile(packageRoot / "Assets" / "Models" / "Hero.obj", "package");

    AssetImporterFacade facade({projectRoot, packageRoot});
    EXPECT_FALSE(facade.Refresh());

    const auto hasAliasDiagnostic = std::any_of(
        facade.GetDiagnostics().begin(),
        facade.GetDiagnostics().end(),
        [](const NLS::Core::Assets::AssetDiagnostic& diagnostic)
        {
            return diagnostic.code == "assetimporter-editor-path-alias";
        });
    EXPECT_TRUE(hasAliasDiagnostic);

    std::filesystem::remove_all(projectRoot);
    std::filesystem::remove_all(packageRoot);
}

TEST(AssetImporterFacadeTests, ExternalObjectRemapsPersistForModelMaterialAndTextureOutputs)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetImporterFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "Hero.fbx", "fbx");
    WriteTextFile(root / "Assets" / "Materials" / "Hero.mat", "material");
    WriteTextFile(root / "Assets" / "Textures" / "Hero.png", "png");

    AssetImporterFacade facade({root});
    ASSERT_TRUE(facade.Refresh());

    const ExternalObjectRemap materialRemap {
        "material:fbx/material/HeroSurface",
        "material",
        MakeAssetId("11111111-1111-4111-8111-111111111111"),
        "material:Hero"
    };
    const ExternalObjectRemap textureRemap {
        "texture:fbx/texture/HeroDiffuse",
        "texture",
        MakeAssetId("22222222-2222-4222-8222-222222222222"),
        "texture:HeroDiffuse"
    };

    ASSERT_TRUE(facade.AddRemap("Assets/Models/Hero.fbx", materialRemap));
    ASSERT_TRUE(facade.AddRemap("Assets/Models/Hero.fbx", textureRemap));

    auto remaps = facade.GetExternalObjectMap("Assets/Models/Hero.fbx");
    ASSERT_EQ(remaps.size(), 2u);
    EXPECT_EQ(remaps[0].sourceObjectKey, "material:fbx/material/HeroSurface");
    EXPECT_EQ(remaps[0].targetAssetId, materialRemap.targetAssetId);
    EXPECT_EQ(remaps[1].sourceObjectKey, "texture:fbx/texture/HeroDiffuse");

    ASSERT_TRUE(facade.RemoveRemap("Assets/Models/Hero.fbx", "material:fbx/material/HeroSurface"));
    remaps = facade.GetExternalObjectMap("Assets/Models/Hero.fbx");
    ASSERT_EQ(remaps.size(), 1u);
    EXPECT_EQ(remaps[0].sourceObjectKey, "texture:fbx/texture/HeroDiffuse");

    AssetImporterFacade reloaded({root});
    ASSERT_TRUE(reloaded.Refresh());
    remaps = reloaded.GetExternalObjectMap("Assets/Models/Hero.fbx");
    ASSERT_EQ(remaps.size(), 1u);
    EXPECT_EQ(remaps[0].targetAssetId, textureRemap.targetAssetId);
    EXPECT_EQ(remaps[0].targetSubAssetKey, "texture:HeroDiffuse");

    std::filesystem::remove_all(root);
}

TEST(AssetImporterFacadeTests, ModelImporterSettingsPersistAndDriveSceneConversion)
{
    using namespace NLS::Editor::Assets;
    using namespace NLS::Render::Assets;

    const auto root = MakeAssetImporterFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "RiggedHero.gltf", R"({"asset":{"version":"2.0"}})");

    AssetImporterFacade facade({root});
    ASSERT_TRUE(facade.Refresh());

    ModelImporterSettings settings;
    settings.globalScale = 0.01;
    settings.axisConversion = "z-up-to-y-up";
    settings.unitConversion = "centimeter-to-meter";
    settings.hierarchyPolicy = "preserve";
    settings.importNormals = false;
    settings.importTangents = false;
    settings.importUvs = false;
    settings.importMaterials = false;
    settings.importSkeleton = false;
    settings.importAnimations = false;
    settings.importMorphTargets = false;
    settings.importCameras = true;
    settings.importLights = true;

    ASSERT_TRUE(facade.SetModelImporterSettings("Assets/Models/RiggedHero.gltf", settings));
    const auto stored = facade.GetModelImporterSettings("Assets/Models/RiggedHero.gltf");
    ASSERT_TRUE(stored.has_value());
    EXPECT_DOUBLE_EQ(stored->globalScale, 0.01);
    EXPECT_EQ(stored->axisConversion, "z-up-to-y-up");
    EXPECT_FALSE(stored->importNormals);
    EXPECT_TRUE(stored->importCameras);

    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "materials": [{ "name": "HeroMat" }],
      "meshes": [
        {
          "name": "Body",
          "primitives": [
            {
              "attributes": { "POSITION": 0, "NORMAL": 1, "TANGENT": 2, "TEXCOORD_0": 3 },
              "material": 0,
              "targets": [{ "POSITION": 4 }]
            }
          ]
        }
      ],
      "nodes": [{ "mesh": 0, "skin": 0 }],
      "skins": [{ "name": "HeroSkin", "joints": [0] }],
      "animations": [{ "name": "Idle" }],
      "cameras": [{ "name": "ShotCamera" }],
      "lights": [{ "name": "KeyLight" }]
    })";

    auto scene = ImportGltfSceneJson(
        gltf,
        MakeAssetId("33333333-3333-4333-8333-333333333333"),
        "RiggedHero",
        ToSceneImportSettings(settings));

    EXPECT_DOUBLE_EQ(scene.importSettings.globalScale, 0.01);
    EXPECT_EQ(scene.importSettings.axisConversion, "z-up-to-y-up");
    ASSERT_EQ(scene.meshes.size(), 1u);
    EXPECT_EQ(scene.meshes[0].attributes, std::vector<std::string>({"POSITION"}));
    ASSERT_EQ(scene.meshes[0].primitives.size(), 1u);
    ASSERT_EQ(scene.meshes[0].primitives[0].vertexStreams.size(), 1u);
    EXPECT_EQ(scene.meshes[0].primitives[0].vertexStreams[0].semantic, "POSITION");
    EXPECT_TRUE(scene.meshes[0].primitives[0].materialKey.empty());
    EXPECT_TRUE(scene.materials.empty());
    EXPECT_TRUE(scene.skeletons.empty());
    EXPECT_TRUE(scene.skins.empty());
    EXPECT_TRUE(scene.animations.empty());
    EXPECT_TRUE(scene.morphTargets.empty());
    ASSERT_EQ(scene.diagnostics.size(), 2u);
    EXPECT_EQ(scene.diagnostics[0].code, "model-import-cameras-unsupported");
    EXPECT_EQ(scene.diagnostics[1].code, "model-import-lights-unsupported");

    std::filesystem::remove_all(root);
}

TEST(AssetImporterFacadeTests, ModelImporterSettingsDefaultToAssimpFbxReader)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetImporterFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "LegacyHero.fbx", "fbx");

    AssetImporterFacade facade({root});
    ASSERT_TRUE(facade.Refresh());

    const auto settings = facade.GetModelImporterSettings("Assets/Models/LegacyHero.fbx");
    ASSERT_TRUE(settings.has_value());
    EXPECT_EQ(settings->fbxReaderSelection, FbxReaderSelection::Assimp);

    std::filesystem::remove_all(root);
}

TEST(AssetImporterFacadeTests, ModelImporterSettingsPersistFbxReaderSelectionAndDirtyState)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetImporterFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "AssimpHero.fbx", "fbx");

    AssetImporterFacade facade({root});
    ASSERT_TRUE(facade.Refresh());

    auto settings = facade.GetModelImporterSettings("Assets/Models/AssimpHero.fbx");
    ASSERT_TRUE(settings.has_value());
    settings->fbxReaderSelection = FbxReaderSelection::Assimp;
    ASSERT_TRUE(facade.SetModelImporterSettings("Assets/Models/AssimpHero.fbx", *settings));

    const auto stored = facade.GetModelImporterSettings("Assets/Models/AssimpHero.fbx");
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->fbxReaderSelection, FbxReaderSelection::Assimp);

    const auto importer = facade.GetAtPath("Assets/Models/AssimpHero.fbx");
    ASSERT_TRUE(importer.has_value());
    EXPECT_TRUE(importer->dirty);

    const auto loadedMeta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(root / "Assets" / "Models" / "AssimpHero.fbx"));
    ASSERT_TRUE(loadedMeta.has_value());
    EXPECT_EQ(loadedMeta->settings.at("MODEL_FBX_READER"), "assimp");

    std::filesystem::remove_all(root);
}

TEST(AssetImporterFacadeTests, TextureImporterSettingsPersistSamplerColorSpaceAndPlatformOverrides)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetImporterFacadeRoot();
    WriteTextFile(root / "Assets" / "Textures" / "Hero.png", "png");

    AssetImporterFacade facade({root});
    ASSERT_TRUE(facade.Refresh());

    TextureImporterSettings settings;
    settings.textureType = "normal-map";
    settings.srgbTexture = false;
    settings.alphaIsTransparency = true;
    settings.mipmapEnabled = true;
    settings.wrapMode = "clamp";
    settings.filterMode = "trilinear";
    settings.maxTextureSize = 2048u;
    settings.compressionIntent = "high-quality";
    settings.platformOverrides.push_back({"win64", 1024u, "bc7", "high"});

    ASSERT_TRUE(facade.SetTextureImporterSettings("Assets/Textures/Hero.png", settings));
    const auto stored = facade.GetTextureImporterSettings("Assets/Textures/Hero.png");
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->textureType, "normal-map");
    EXPECT_FALSE(stored->srgbTexture);
    EXPECT_TRUE(stored->alphaIsTransparency);
    EXPECT_EQ(stored->wrapMode, "clamp");
    EXPECT_EQ(stored->filterMode, "trilinear");
    ASSERT_EQ(stored->platformOverrides.size(), 1u);
    EXPECT_EQ(stored->platformOverrides[0].platform, "win64");
    EXPECT_EQ(stored->platformOverrides[0].maxTextureSize, 1024u);
    EXPECT_EQ(stored->platformOverrides[0].format, "bc7");

    const auto loadedMeta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(root / "Assets" / "Textures" / "Hero.png"));
    ASSERT_TRUE(loadedMeta.has_value());
    EXPECT_EQ(loadedMeta->settings.at("TEXTURE_TYPE"), "normal-map");
    EXPECT_EQ(loadedMeta->settings.at("TEXTURE_SRGB"), "false");
    EXPECT_EQ(loadedMeta->settings.at("TEXTURE_PLATFORM.win64"), "1024|bc7|high");

    std::filesystem::remove_all(root);
}

TEST(AssetImporterFacadeTests, WriteImportSettingsIfDirtyPersistsDirtyImportSettingsWithoutQueueingReimport)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetImporterFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"}})");

    AssetImporterFacade facade({root});
    ASSERT_TRUE(facade.Refresh());

    EXPECT_FALSE(facade.WriteImportSettingsIfDirty("Assets/Models/Hero.gltf"));

    ASSERT_TRUE(facade.SetSerializedSetting("Assets/Models/Hero.gltf", "globalScale", "2.0"));
    auto importer = facade.GetAtPath("Assets/Models/Hero.gltf");
    ASSERT_TRUE(importer.has_value());
    EXPECT_TRUE(importer->dirty);

    ASSERT_TRUE(facade.WriteImportSettingsIfDirty("Assets/Models/Hero.gltf"));
    importer = facade.GetAtPath("Assets/Models/Hero.gltf");
    ASSERT_TRUE(importer.has_value());
    EXPECT_FALSE(importer->dirty);
    EXPECT_EQ(importer->serializedSettings.at("globalScale"), "2.0");
    EXPECT_EQ(facade.GetQueuedReimportCount(), 0u);

    const auto loadedMeta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(root / "Assets" / "Models" / "Hero.gltf"));
    ASSERT_TRUE(loadedMeta.has_value());
    EXPECT_EQ(loadedMeta->settings.at("globalScale"), "2.0");
    EXPECT_EQ(loadedMeta->settings.find("NULLUS_IMPORTER_DIRTY"), loadedMeta->settings.end());

    std::filesystem::remove_all(root);
}

TEST(AssetImporterFacadeTests, AssetPostprocessorsRunInOrderDeclareDependenciesAndVersion)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    AssetPostprocessorRegistry registry;
    std::vector<std::string> callbacks;

    registry.Register({
        "late",
        20,
        7,
        [&](AssetPostprocessContext& context)
        {
            callbacks.push_back("late-pre:" + context.assetPath);
            context.DeclareDependency({AssetDependencyKind::PostprocessorVersion, "late", "7"});
        },
        [&](AssetPostprocessContext& context)
        {
            callbacks.push_back("late-post:" + context.assetPath);
            context.EmitDiagnostic("late-post", "Late postprocessor ran.");
        }
    });

    registry.Register({
        "early",
        -10,
        3,
        [&](AssetPostprocessContext& context)
        {
            callbacks.push_back("early-pre:" + context.assetPath);
            context.DeclareDependency({AssetDependencyKind::RawPackageFile, "Packages/shared.asset", "sha256:pkg"});
        },
        [&](AssetPostprocessContext& context)
        {
            callbacks.push_back("early-post:" + context.assetPath);
        }
    });

    auto result = registry.Run("Assets/Models/Hero.gltf");

    EXPECT_EQ(
        callbacks,
        std::vector<std::string>({
            "early-pre:Assets/Models/Hero.gltf",
            "late-pre:Assets/Models/Hero.gltf",
            "early-post:Assets/Models/Hero.gltf",
            "late-post:Assets/Models/Hero.gltf"
        }));
    EXPECT_EQ(result.versionToken, "early:3|late:7");
    ASSERT_EQ(result.dependencies.size(), 2u);
    EXPECT_EQ(result.dependencies[0].kind, AssetDependencyKind::RawPackageFile);
    EXPECT_EQ(result.dependencies[1].kind, AssetDependencyKind::PostprocessorVersion);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics[0].code, "late-post");
}

TEST(AssetImporterFacadeTests, ScriptedImporterRegistryCreatesArtifactsDependenciesAndDiagnostics)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Render::Assets;

    SceneImporterRegistry registry;
    registry.Register({"dialogue-scripted", 5u, {".dialogue"}});
    registry.RegisterScripted({
        "dialogue-scripted",
        5u,
        {".dialogue"},
        [](const ScriptedSceneImportRequest& request)
        {
            ImportedScene scene;
            scene.sourceAssetId = request.sourceAssetId;
            scene.sceneKey = request.sceneKey;
            scene.materials.push_back({"dialogue/material/default", "DialogueDefault"});
            scene.diagnostics.push_back({"dialogue-imported", request.sourcePath.generic_string()});
            scene.dependencies.push_back({AssetDependencyKind::RawPackageFile, "Packages/dialogue.schema", "v1"});
            return scene;
        }
    });

    const auto* importer = registry.FindImporterForPath("Assets/Dialogues/Intro.dialogue");
    ASSERT_NE(importer, nullptr);
    EXPECT_EQ(importer->importerId, "dialogue-scripted");

    const auto scene = registry.ImportScripted({
        "Assets/Dialogues/Intro.dialogue",
        MakeAssetId("44444444-4444-4444-8444-444444444444"),
        "IntroDialogue"
    });

    ASSERT_TRUE(scene.has_value());
    EXPECT_EQ(scene->sceneKey, "IntroDialogue");
    ASSERT_EQ(scene->materials.size(), 1u);
    EXPECT_EQ(scene->materials[0].sourceKey, "dialogue/material/default");
    ASSERT_EQ(scene->dependencies.size(), 1u);
    EXPECT_EQ(scene->dependencies[0].value, "Packages/dialogue.schema");
    ASSERT_EQ(scene->diagnostics.size(), 1u);
    EXPECT_EQ(scene->diagnostics[0].code, "dialogue-imported");
}
