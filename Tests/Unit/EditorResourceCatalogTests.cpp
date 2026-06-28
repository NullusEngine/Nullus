#include <gtest/gtest.h>

#include "Core/EditorResourceCatalog.h"

#include <filesystem>
#include <string>
#include <system_error>

namespace
{
using NLS::Editor::Core::EditorResourceBackendMode;
using NLS::Editor::Core::EditorResourceCatalog;
using NLS::Editor::Core::EditorResourceRecord;
using NLS::Editor::Core::EditorResourceScope;
using NLS::Editor::Core::EditorResourceType;

EditorResourceRecord MakeIconRecord()
{
    EditorResourceRecord record;
    record.id = "editor.icon.asset.folder";
    record.type = EditorResourceType::Icon;
    record.scope = EditorResourceScope::Editor;
    record.developmentPath = "Editor/Icons/asset-folder.png";
    record.packagedPath = "editor/icons/asset-folder";
    return record;
}

std::filesystem::path MakeCatalogTestRoot()
{
    const auto root = std::filesystem::temp_directory_path() / "nullus_editor_resource_catalog_tests";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "App" / "Assets" / "Editor" / "Icons");
    std::filesystem::create_directories(root / "App" / "Assets" / "Editor" / "Fonts");
    std::filesystem::create_directories(root / "App" / "Assets" / "Engine" / "Brand");
    std::filesystem::create_directories(root / "bin");
    return root;
}

std::filesystem::path NormalizeForCatalogTest(std::filesystem::path path)
{
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(path, error);
    if (error)
        normalized = path.lexically_normal();
    return normalized;
}
}

TEST(EditorResourceCatalogTests, ResolvesDevelopmentResourcesRelativeToInstallAssetsRoot)
{
    const auto root = MakeCatalogTestRoot();
    EditorResourceCatalog catalog(root / "bin" / "Editor.exe");
    catalog.SetDevelopmentAssetsRoot(root / "App" / "Assets");
    catalog.AddRecord(MakeIconRecord());

    const auto resolved = catalog.ResolvePath("editor.icon.asset.folder", EditorResourceBackendMode::Development);

    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(
        NormalizeForCatalogTest(*resolved),
        NormalizeForCatalogTest(root / "App" / "Assets" / "Editor" / "Icons" / "asset-folder.png"));

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST(EditorResourceCatalogTests, ResolvesPackagedResourcesThroughSameId)
{
    const auto root = MakeCatalogTestRoot();
    EditorResourceCatalog catalog(root / "bin" / "Editor.exe");
    catalog.AddRecord(MakeIconRecord());

    const auto resolved = catalog.ResolvePath("editor.icon.asset.folder", EditorResourceBackendMode::Packaged);

    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->generic_string(), "editor/icons/asset-folder");

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST(EditorResourceCatalogTests, AllowsExternalResourceIdsWithoutBrandSpecificTokenFilters)
{
    const auto root = MakeCatalogTestRoot();
    EditorResourceCatalog catalog(root / "bin" / "Editor.exe");

    EditorResourceRecord record = MakeIconRecord();
    record.id = "editor.icon.unity.project.folder";
    record.developmentPath = "Editor/Icons/unity_project_folder.png";

    EXPECT_TRUE(catalog.AddRecord(record));
    EXPECT_TRUE(catalog.Contains("editor.icon.unity.project.folder"));

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST(EditorResourceCatalogTests, DefaultRecordsCoverCurrentEditorResourceGroups)
{
    const auto root = MakeCatalogTestRoot();
    EditorResourceCatalog catalog(root / "bin" / "Editor.exe");
    catalog.SetDevelopmentAssetsRoot(root / "App" / "Assets");
    catalog.AddDefaultRecords();

    EXPECT_TRUE(catalog.Contains("editor.brand.logo.mark"));
    EXPECT_TRUE(catalog.Contains("engine.brand.logo.mark"));
    EXPECT_TRUE(catalog.Contains("editor.font.ui.default"));
    EXPECT_TRUE(catalog.Contains("editor.layout.default"));
    EXPECT_TRUE(catalog.Contains("editor.icon.asset.prefab"));
    EXPECT_TRUE(catalog.Contains("editor.icon.asset.material"));
    EXPECT_TRUE(catalog.Contains("editor.icon.toolbar.move"));
    EXPECT_TRUE(catalog.Contains("editor.model.helper.camera"));
    EXPECT_TRUE(catalog.Contains("editor.shader.selection-outline-composite"));

    const auto fontPath = catalog.ResolvePath("editor.font.ui.default", EditorResourceBackendMode::Development);
    ASSERT_TRUE(fontPath.has_value());
    EXPECT_EQ(
        NormalizeForCatalogTest(*fontPath),
        NormalizeForCatalogTest(root / "App" / "Assets" / "Editor" / "Fonts" / "Ruda-Bold.ttf"));

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST(EditorResourceCatalogTests, DefaultRecordsUseNullusStyleNamesAndPackagedKeys)
{
    const auto& records = EditorResourceCatalog::DefaultRecords();
    ASSERT_FALSE(records.empty());

    for (const auto& record : records)
    {
        const auto id = record.id;
        const auto devPath = record.developmentPath.generic_string();
        const auto packagedPath = record.packagedPath.generic_string();

        EXPECT_EQ(id.find("unity"), std::string::npos) << id;
        EXPECT_EQ(devPath.find("unity"), std::string::npos) << devPath;
        EXPECT_EQ(packagedPath.find("unity"), std::string::npos) << packagedPath;
        EXPECT_FALSE(record.packagedPath.is_absolute()) << packagedPath;
    }
}

TEST(EditorResourceCatalogTests, RejectsDuplicateRecords)
{
    const auto root = MakeCatalogTestRoot();
    EditorResourceCatalog catalog(root / "bin" / "Editor.exe");

    EXPECT_TRUE(catalog.AddRecord(MakeIconRecord()));
    EXPECT_FALSE(catalog.AddRecord(MakeIconRecord()));

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST(EditorResourceCatalogTests, ResolvesInstallRootsFromExecutableWithoutCurrentWorkingDirectory)
{
    const auto root = MakeCatalogTestRoot();
    const auto roots = EditorResourceCatalog::ResolveRootsFromExecutable(root / "bin" / "Editor.exe");

    EXPECT_EQ(NormalizeForCatalogTest(roots.installRoot), NormalizeForCatalogTest(root));
    EXPECT_EQ(NormalizeForCatalogTest(roots.assetsRoot), NormalizeForCatalogTest(root / "App" / "Assets"));
    EXPECT_EQ(NormalizeForCatalogTest(roots.editorAssetsRoot), NormalizeForCatalogTest(root / "App" / "Assets" / "Editor"));
    EXPECT_EQ(NormalizeForCatalogTest(roots.engineAssetsRoot), NormalizeForCatalogTest(root / "App" / "Assets" / "Engine"));

    std::error_code error;
    std::filesystem::remove_all(root, error);
}
