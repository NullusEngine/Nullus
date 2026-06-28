#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "Assets/AssetMeta.h"
#include "Assets/SourceAssetDatabase.h"
#include "Guid.h"

namespace
{
std::filesystem::path MakeAssetTestRoot()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_asset_foundation_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);
    return root;
}

void WriteTextFile(const std::filesystem::path& path, const std::string& contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
}
}

TEST(AssetFoundationTests, ScanCreatesStableMetaForModelSceneAssets)
{
    const auto root = MakeAssetTestRoot();
    const auto modelPath = root / "Models" / "Hero.gltf";
    WriteTextFile(modelPath, R"({"asset":{"version":"2.0"}})");

    NLS::Core::Assets::SourceAssetDatabase database;
    const auto firstResult = database.ScanRoot(root);

    ASSERT_TRUE(firstResult);
    ASSERT_EQ(database.GetRecords().size(), 1u);

    const auto& record = database.GetRecords().front();
    EXPECT_EQ(record.relativePath.generic_string(), "Models/Hero.gltf");
    EXPECT_EQ(record.assetType, NLS::Core::Assets::AssetType::ModelScene);
    EXPECT_EQ(record.importerId, "scene-model");
    EXPECT_TRUE(record.id.IsValid());

    const auto metaPath = NLS::Core::Assets::GetAssetMetaPath(modelPath);
    ASSERT_TRUE(std::filesystem::exists(metaPath));

    const auto loadedMeta = NLS::Core::Assets::AssetMeta::Load(metaPath);
    ASSERT_TRUE(loadedMeta.has_value());
    EXPECT_EQ(loadedMeta->id, record.id);
    EXPECT_EQ(loadedMeta->importerId, "scene-model");
    EXPECT_EQ(loadedMeta->assetType, NLS::Core::Assets::AssetType::ModelScene);

    const auto originalId = record.id;

    NLS::Core::Assets::SourceAssetDatabase secondScan;
    ASSERT_TRUE(secondScan.ScanRoot(root));
    ASSERT_EQ(secondScan.GetRecords().size(), 1u);
    EXPECT_EQ(secondScan.GetRecords().front().id, originalId);

    std::filesystem::remove_all(root);
}

TEST(AssetFoundationTests, ScanDoesNotRewriteSemanticallyCurrentMeta)
{
    const auto root = MakeAssetTestRoot();
    const auto modelPath = root / "Models" / "StableMetaHero.gltf";
    WriteTextFile(modelPath, R"({"asset":{"version":"2.0"}})");

    NLS::Core::Assets::SourceAssetDatabase firstScan;
    ASSERT_TRUE(firstScan.ScanRoot(root));

    const auto metaPath = NLS::Core::Assets::GetAssetMetaPath(modelPath);
    ASSERT_TRUE(std::filesystem::exists(metaPath));

    const auto oldWriteTime = std::filesystem::last_write_time(metaPath);
    std::filesystem::last_write_time(metaPath, oldWriteTime - std::chrono::hours(1));
    const auto preservedWriteTime = std::filesystem::last_write_time(metaPath);

    NLS::Core::Assets::SourceAssetDatabase secondScan;
    ASSERT_TRUE(secondScan.ScanRoot(root));
    EXPECT_EQ(std::filesystem::last_write_time(metaPath), preservedWriteTime);
    const auto preservedMeta = NLS::Core::Assets::AssetMeta::Load(metaPath);
    ASSERT_TRUE(preservedMeta.has_value());
    EXPECT_EQ(preservedMeta->id, firstScan.GetRecords().front().id);
    EXPECT_EQ(preservedMeta->importerId, "scene-model");
    EXPECT_EQ(preservedMeta->assetType, NLS::Core::Assets::AssetType::ModelScene);

    std::filesystem::remove_all(root);
}

TEST(AssetFoundationTests, ScanRewritesSemanticallyStaleMeta)
{
    const auto root = MakeAssetTestRoot();
    const auto modelPath = root / "Models" / "StaleMetaHero.gltf";
    WriteTextFile(modelPath, R"({"asset":{"version":"2.0"}})");
    WriteTextFile(
        NLS::Core::Assets::GetAssetMetaPath(modelPath),
        "GUID=" + NLS::Guid::New().ToString() + "\nIMPORTER_ID=unknown\nASSET_TYPE=unknown\n");

    NLS::Core::Assets::SourceAssetDatabase database;
    ASSERT_TRUE(database.ScanRoot(root));

    const auto meta = NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(modelPath));
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->importerId, "scene-model");
    EXPECT_EQ(meta->assetType, NLS::Core::Assets::AssetType::ModelScene);

    std::filesystem::remove_all(root);
}

TEST(AssetFoundationTests, ScanUsesSourceExtensionAsAuthoritativeWhenMetaHasDifferentType)
{
    const auto root = MakeAssetTestRoot();
    const auto shaderPath = root / "Shaders" / "RenamedFromMaterial.shader";
    const auto stableGuid = NLS::Guid::New();
    WriteTextFile(shaderPath, R"(Shader "Tests/Renamed" { SubShader { Pass { HLSLPROGRAM ENDHLSL } } })");
    WriteTextFile(
        NLS::Core::Assets::GetAssetMetaPath(shaderPath),
        "GUID=" + stableGuid.ToString() + "\nIMPORTER_ID=material\nASSET_TYPE=material\n");

    NLS::Core::Assets::SourceAssetDatabase database;
    ASSERT_TRUE(database.ScanRoot(root));
    ASSERT_EQ(database.GetRecords().size(), 1u);

    const auto& record = database.GetRecords().front();
    EXPECT_EQ(record.id, NLS::Core::Assets::AssetId(stableGuid));
    EXPECT_EQ(record.assetType, NLS::Core::Assets::AssetType::Shader);
    EXPECT_EQ(record.importerId, "shader");

    const auto meta = NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(shaderPath));
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->id, NLS::Core::Assets::AssetId(stableGuid));
    EXPECT_EQ(meta->assetType, NLS::Core::Assets::AssetType::Shader);
    EXPECT_EQ(meta->importerId, "shader");

    std::filesystem::remove_all(root);
}

TEST(AssetFoundationTests, ScanPreservesExistingMetaSettingsWhenAddingIdentity)
{
    const auto root = MakeAssetTestRoot();
    const auto texturePath = root / "Textures" / "Albedo.png";
    WriteTextFile(texturePath, "png bytes");
    WriteTextFile(NLS::Core::Assets::GetAssetMetaPath(texturePath), "MIN_FILTER=9729\nENABLE_MIPMAPPING=true\n");

    NLS::Core::Assets::SourceAssetDatabase database;
    ASSERT_TRUE(database.ScanRoot(root));
    ASSERT_EQ(database.GetRecords().size(), 1u);

    const auto loadedMeta = NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(texturePath));
    ASSERT_TRUE(loadedMeta.has_value());
    EXPECT_TRUE(loadedMeta->id.IsValid());
    EXPECT_EQ(loadedMeta->importerId, "texture");
    EXPECT_EQ(loadedMeta->assetType, NLS::Core::Assets::AssetType::Texture);
    EXPECT_EQ(loadedMeta->settings.at("MIN_FILTER"), "9729");
    EXPECT_EQ(loadedMeta->settings.at("ENABLE_MIPMAPPING"), "true");

    std::filesystem::remove_all(root);
}

TEST(AssetFoundationTests, ScanClassifiesPrefabAssets)
{
    const auto root = MakeAssetTestRoot();
    const auto prefabPath = root / "Prefabs" / "Lamp.prefab";
    WriteTextFile(prefabPath, R"({"format":"Nullus.ObjectGraph.Prefab","version":1})");

    NLS::Core::Assets::SourceAssetDatabase database;
    ASSERT_TRUE(database.ScanRoot(root));
    ASSERT_EQ(database.GetRecords().size(), 1u);

    const auto& record = database.GetRecords().front();
    EXPECT_EQ(record.relativePath.generic_string(), "Prefabs/Lamp.prefab");
    EXPECT_EQ(record.assetType, NLS::Core::Assets::AssetType::Prefab);
    EXPECT_EQ(record.importerId, "prefab");

    const auto loadedMeta = NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(prefabPath));
    ASSERT_TRUE(loadedMeta.has_value());
    EXPECT_EQ(loadedMeta->assetType, NLS::Core::Assets::AssetType::Prefab);
    EXPECT_EQ(loadedMeta->importerId, "prefab");

    std::filesystem::remove_all(root);
}

TEST(AssetFoundationTests, InferAssetTypesMatchAssetBrowserProjectExtensions)
{
    using NLS::Core::Assets::AssetType;

    struct Case
    {
        const char* path;
        AssetType expectedType;
        const char* expectedImporter;
    };

    const Case cases[] {
        {"Models/Hero.fbx", AssetType::ModelScene, "scene-model"},
        {"Models/Hero.gltf", AssetType::ModelScene, "scene-model"},
        {"Models/Hero.glb", AssetType::ModelScene, "scene-model"},
        {"Textures/Hero.png", AssetType::Texture, "texture"},
        {"Textures/Hero.bmp", AssetType::Texture, "texture"},
        {"Textures/Hero.dds", AssetType::Texture, "texture"},
        {"Shaders/Hero.shader", AssetType::Shader, "shader"},
        {"Materials/Hero.mat", AssetType::Material, "material"},
        {"Scenes/Hero.scene", AssetType::Scene, "scene"},
        {"Scenes/Hero.objectgraph.json", AssetType::Scene, "scene"},
        {"Scripts/Hero.lua", AssetType::Script, "script"},
        {"Scripts/Hero.cs", AssetType::Script, "script"},
        {"Scripts/Hero.py", AssetType::Script, "script"},
        {"Prefabs/Hero.prefab", AssetType::Prefab, "prefab"}
    };

    for (const auto& testCase : cases)
    {
        const auto assetPath = std::filesystem::path(testCase.path);
        const auto inferredType = NLS::Core::Assets::InferAssetType(assetPath);
        EXPECT_EQ(inferredType, testCase.expectedType) << testCase.path;
        EXPECT_EQ(NLS::Core::Assets::InferImporterId(inferredType), testCase.expectedImporter) << testCase.path;
        EXPECT_EQ(NLS::Core::Assets::AssetTypeFromString(NLS::Core::Assets::ToString(inferredType)), inferredType)
            << testCase.path;
    }
}

TEST(AssetFoundationTests, ScanWritesMetaForAssetBrowserProjectExtensions)
{
    using NLS::Core::Assets::AssetType;

    const auto root = MakeAssetTestRoot();
    const auto shaderPath = root / "Shaders" / "Hero.shader";
    const auto scenePath = root / "Scenes" / "Hero.scene";
    const auto objectGraphScenePath = root / "Scenes" / "Hero.objectgraph.json";
    const auto bmpPath = root / "Textures" / "Hero.bmp";
    const auto scriptPath = root / "Scripts" / "Hero.cs";
    WriteTextFile(shaderPath, "shader");
    WriteTextFile(scenePath, "{}");
    WriteTextFile(objectGraphScenePath, "{}");
    WriteTextFile(bmpPath, "bmp");
    WriteTextFile(scriptPath, "script");

    NLS::Core::Assets::SourceAssetDatabase database;
    ASSERT_TRUE(database.ScanRoot(root));
    ASSERT_EQ(database.GetRecords().size(), 5u);

    const auto assertMeta = [](const std::filesystem::path& path, const AssetType type, const char* importerId)
    {
        const auto meta = NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(path));
        ASSERT_TRUE(meta.has_value()) << path;
        EXPECT_TRUE(meta->id.IsValid()) << path;
        EXPECT_EQ(meta->assetType, type) << path;
        EXPECT_EQ(meta->importerId, importerId) << path;
    };

    assertMeta(shaderPath, AssetType::Shader, "shader");
    assertMeta(scenePath, AssetType::Scene, "scene");
    assertMeta(objectGraphScenePath, AssetType::Scene, "scene");
    assertMeta(bmpPath, AssetType::Texture, "texture");
    assertMeta(scriptPath, AssetType::Script, "script");

    std::filesystem::remove_all(root);
}

TEST(AssetFoundationTests, ScanRootsPathOverloadDoesNotRepairSameAssetThroughDuplicateRoots)
{
    const auto root = MakeAssetTestRoot();
    const auto modelPath = root / "Models" / "Hero.fbx";
    WriteTextFile(modelPath, "hero");

    const auto stableGuid = NLS::Guid::New();
    WriteTextFile(
        NLS::Core::Assets::GetAssetMetaPath(modelPath),
        "GUID=" + stableGuid.ToString() + "\nIMPORTER_ID=scene-model\nASSET_TYPE=model-scene\n");

    std::vector<std::filesystem::path> roots{ root, root };
    NLS::Core::Assets::SourceAssetDatabase database;
    ASSERT_TRUE(database.ScanRoots(roots, false));

    EXPECT_EQ(database.GetRecords().size(), 1u);
    EXPECT_TRUE(database.GetDiagnostics().empty());

    const auto meta = NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(modelPath));
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->id, NLS::Core::Assets::AssetId(stableGuid));

    std::filesystem::remove_all(root);
}

TEST(AssetFoundationTests, ScanRootsPathOverloadUsesMountedRootContextForOverlappingRoots)
{
    const auto root = MakeAssetTestRoot();
    const auto modelRoot = root / "Models";
    const auto modelPath = modelRoot / "Hero.fbx";
    WriteTextFile(modelPath, "hero");

    const auto stableGuid = NLS::Guid::New();
    WriteTextFile(
        NLS::Core::Assets::GetAssetMetaPath(modelPath),
        "GUID=" + stableGuid.ToString() + "\nIMPORTER_ID=scene-model\nASSET_TYPE=model-scene\n");

    std::vector<std::filesystem::path> roots{ root, modelRoot };
    NLS::Core::Assets::SourceAssetDatabase database;
    ASSERT_TRUE(database.ScanRoots(roots, false));

    EXPECT_EQ(database.GetRecords().size(), 1u);
    EXPECT_TRUE(database.GetDiagnostics().empty());
    ASSERT_FALSE(database.GetRecords().empty());
    EXPECT_EQ(database.GetRecords().front().relativePath.generic_string(), "Hero.fbx");

    const auto meta = NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(modelPath));
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->id, NLS::Core::Assets::AssetId(stableGuid));

    std::filesystem::remove_all(root);
}

TEST(AssetFoundationTests, ScanResolvesRenamedAssetsByGuidWhenMetaMovesWithAsset)
{
    const auto root = MakeAssetTestRoot();
    const auto originalPath = root / "Models" / "Hero.obj";
    WriteTextFile(originalPath, "o Hero\n");

    NLS::Core::Assets::SourceAssetDatabase firstScan;
    ASSERT_TRUE(firstScan.ScanRoot(root));
    ASSERT_EQ(firstScan.GetRecords().size(), 1u);
    const auto id = firstScan.GetRecords().front().id;

    const auto renamedPath = root / "Models" / "RenamedHero.obj";
    std::filesystem::rename(originalPath, renamedPath);
    std::filesystem::rename(
        NLS::Core::Assets::GetAssetMetaPath(originalPath),
        NLS::Core::Assets::GetAssetMetaPath(renamedPath));

    NLS::Core::Assets::SourceAssetDatabase secondScan;
    ASSERT_TRUE(secondScan.ScanRoot(root));

    const auto* record = secondScan.FindById(id);
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->absolutePath.lexically_normal(), renamedPath.lexically_normal());
    EXPECT_EQ(record->relativePath.generic_string(), "Models/RenamedHero.obj");

    std::filesystem::remove_all(root);
}

TEST(AssetFoundationTests, ScanDeletesOrphanedMetaForMissingEditableAsset)
{
    const auto root = MakeAssetTestRoot();
    const auto modelPath = root / "Models" / "DeletedHero.gltf";
    WriteTextFile(modelPath, R"({"asset":{"version":"2.0"}})");

    NLS::Core::Assets::SourceAssetDatabase firstScan;
    ASSERT_TRUE(firstScan.ScanRoot(root));

    const auto metaPath = NLS::Core::Assets::GetAssetMetaPath(modelPath);
    ASSERT_TRUE(std::filesystem::exists(metaPath));

    ASSERT_TRUE(std::filesystem::remove(modelPath));
    ASSERT_FALSE(std::filesystem::exists(modelPath));

    NLS::Core::Assets::SourceAssetDatabase secondScan;
    ASSERT_TRUE(secondScan.ScanRoot(root));
    EXPECT_TRUE(secondScan.GetRecords().empty());
    EXPECT_FALSE(std::filesystem::exists(metaPath));

    std::filesystem::remove_all(root);
}

TEST(AssetFoundationTests, ScanPreservesOrphanedMetaForMissingReadOnlyAsset)
{
    const auto root = MakeAssetTestRoot();
    const auto modelPath = root / "Models" / "PackageHero.gltf";
    WriteTextFile(modelPath, R"({"asset":{"version":"2.0"}})");

    NLS::Core::Assets::SourceAssetDatabase firstScan;
    ASSERT_TRUE(firstScan.ScanRoot(root));

    const auto metaPath = NLS::Core::Assets::GetAssetMetaPath(modelPath);
    ASSERT_TRUE(std::filesystem::exists(metaPath));

    std::filesystem::remove(modelPath);

    NLS::Core::Assets::SourceAssetDatabase secondScan;
    ASSERT_TRUE(secondScan.ScanRoot(root, true));
    EXPECT_TRUE(secondScan.GetRecords().empty());
    EXPECT_TRUE(std::filesystem::exists(metaPath));

    std::filesystem::remove_all(root);
}

TEST(AssetFoundationTests, ScanRepairsDuplicateGuidAliasesForEditableAssets)
{
    const auto root = MakeAssetTestRoot();
    const auto firstPath = root / "Models" / "First.fbx";
    const auto secondPath = root / "Models" / "Second.fbx";
    WriteTextFile(firstPath, "first");
    WriteTextFile(secondPath, "second");

    const auto duplicatedGuid = NLS::Guid::New();
    WriteTextFile(
        NLS::Core::Assets::GetAssetMetaPath(firstPath),
        "GUID=" + duplicatedGuid.ToString() + "\nIMPORTER_ID=scene-model\nASSET_TYPE=model-scene\n");
    WriteTextFile(
        NLS::Core::Assets::GetAssetMetaPath(secondPath),
        "GUID=" + duplicatedGuid.ToString() + "\nIMPORTER_ID=scene-model\nASSET_TYPE=model-scene\n");

    NLS::Core::Assets::SourceAssetDatabase database;
    ASSERT_TRUE(database.ScanRoot(root));

    EXPECT_EQ(database.GetRecords().size(), 2u);
    ASSERT_EQ(database.GetDiagnostics().size(), 1u);
    EXPECT_EQ(database.GetDiagnostics().front().severity, NLS::Core::Assets::AssetDiagnosticSeverity::Warning);
    EXPECT_EQ(database.GetDiagnostics().front().code, "duplicate-asset-guid-repaired");
    EXPECT_NE(database.FindById(NLS::Core::Assets::AssetId(duplicatedGuid)), nullptr);

    const auto firstMeta = NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(firstPath));
    const auto secondMeta = NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(secondPath));
    ASSERT_TRUE(firstMeta.has_value());
    ASSERT_TRUE(secondMeta.has_value());
    EXPECT_TRUE(firstMeta->id.IsValid());
    EXPECT_TRUE(secondMeta->id.IsValid());
    EXPECT_NE(firstMeta->id, secondMeta->id);
    EXPECT_TRUE(
        firstMeta->id == NLS::Core::Assets::AssetId(duplicatedGuid) ||
        secondMeta->id == NLS::Core::Assets::AssetId(duplicatedGuid));
    EXPECT_NE(database.FindById(firstMeta->id), nullptr);
    EXPECT_NE(database.FindById(secondMeta->id), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetFoundationTests, ScanReportsDuplicateGuidAliasesInReadOnlyRootsWithoutRepairing)
{
    const auto root = MakeAssetTestRoot();
    const auto firstPath = root / "Models" / "First.fbx";
    const auto secondPath = root / "Models" / "Second.fbx";
    WriteTextFile(firstPath, "first");
    WriteTextFile(secondPath, "second");

    const auto duplicatedGuid = NLS::Guid::New();
    WriteTextFile(
        NLS::Core::Assets::GetAssetMetaPath(firstPath),
        "GUID=" + duplicatedGuid.ToString() + "\nIMPORTER_ID=scene-model\nASSET_TYPE=model-scene\n");
    WriteTextFile(
        NLS::Core::Assets::GetAssetMetaPath(secondPath),
        "GUID=" + duplicatedGuid.ToString() + "\nIMPORTER_ID=scene-model\nASSET_TYPE=model-scene\n");

    NLS::Core::Assets::SourceAssetDatabase database;
    ASSERT_TRUE(database.ScanRoot(root, true));

    EXPECT_EQ(database.GetRecords().size(), 1u);
    ASSERT_EQ(database.GetDiagnostics().size(), 1u);
    EXPECT_EQ(database.GetDiagnostics().front().severity, NLS::Core::Assets::AssetDiagnosticSeverity::Error);
    EXPECT_EQ(database.GetDiagnostics().front().code, "duplicate-asset-guid");

    const auto secondMeta = NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(secondPath));
    ASSERT_TRUE(secondMeta.has_value());
    EXPECT_EQ(secondMeta->id, NLS::Core::Assets::AssetId(duplicatedGuid));

    std::filesystem::remove_all(root);
}

TEST(AssetFoundationTests, ScanSkipsFileSymlinkEscapesWhenSupported)
{
    const auto root = MakeAssetTestRoot();
    const auto outside =
        std::filesystem::temp_directory_path() /
        ("nullus_asset_foundation_outside_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root / "Assets");
    std::filesystem::create_directories(outside);
    WriteTextFile(outside / "Outside.mat", "outside");

    std::error_code error;
    std::filesystem::create_symlink(outside / "Outside.mat", root / "Assets" / "OutsideLink.mat", error);
    if (error)
    {
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(outside);
        GTEST_SKIP() << "File symlink creation is not available in this environment.";
    }

    NLS::Core::Assets::SourceAssetDatabase database;
    EXPECT_TRUE(database.ScanRoot(root));
    EXPECT_TRUE(database.GetRecords().empty());
    ASSERT_FALSE(database.GetDiagnostics().empty());
    EXPECT_EQ(database.GetDiagnostics().front().code, "asset-scan-entry-outside-root");
    EXPECT_FALSE(std::filesystem::exists(root / "Assets" / "OutsideLink.mat.meta"));

    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside);
}

TEST(AssetFoundationTests, ScanRejectsFilesystemRoot)
{
    const auto root = MakeAssetTestRoot();

    NLS::Core::Assets::SourceAssetDatabase database;
    EXPECT_FALSE(database.ScanRoot(root.root_path()));
    EXPECT_TRUE(database.GetRecords().empty());
    ASSERT_FALSE(database.GetDiagnostics().empty());
    EXPECT_EQ(database.GetDiagnostics().front().code, "asset-root-invalid");

    std::filesystem::remove_all(root);
}
