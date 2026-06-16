#include <gtest/gtest.h>

#include "Assets/ArtifactManifest.h"
#include "Assets/AssetMeta.h"
#include "Assets/AssetBrowserPresentation.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/ExternalAssetImporter.h"
#include "Core/EditorResources.h"
#include "Guid.h"
#include "Utils/PathParser.h"

#include <Json/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
std::filesystem::path MakeAssetBrowserPresentationRoot()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_asset_browser_presentation_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root / "Assets");
    return root;
}

void WriteTextFile(const std::filesystem::path& path, const std::string& contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
}

NLS::Core::Assets::AssetId ParseAssetId(const std::string& guid)
{
    return NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));
}

std::string SafeArtifactPathToken(std::string value)
{
    for (auto& character : value)
    {
        if (character == ':' || character == '/' || character == '\\')
            character = '_';
    }
    return value;
}

std::string Utf8String(const char8_t* value)
{
    const auto text = std::u8string(value);
    return { reinterpret_cast<const char*>(text.data()), text.size() };
}

std::filesystem::path Utf8Path(const char8_t* value)
{
    return std::filesystem::path(std::u8string(value));
}

std::pair<std::uint32_t, std::uint32_t> ReadPngDimensions(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};

    std::array<unsigned char, 24> header {};
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (input.gcount() != static_cast<std::streamsize>(header.size()))
        return {};

    constexpr std::array<unsigned char, 8> pngSignature { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (!std::equal(pngSignature.begin(), pngSignature.end(), header.begin()))
        return {};
    if (header[12] != 'I' || header[13] != 'H' || header[14] != 'D' || header[15] != 'R')
        return {};

    const auto readBigEndian = [&header](const std::size_t offset)
    {
        return
            (static_cast<std::uint32_t>(header[offset]) << 24u) |
            (static_cast<std::uint32_t>(header[offset + 1u]) << 16u) |
            (static_cast<std::uint32_t>(header[offset + 2u]) << 8u) |
            static_cast<std::uint32_t>(header[offset + 3u]);
    };
    return { readBigEndian(16u), readBigEndian(20u) };
}

NLS::Core::Assets::ImportedArtifact MakeArtifact(
    NLS::Core::Assets::AssetId owner,
    std::string subAssetKey,
    NLS::Core::Assets::ArtifactType type,
    std::string loaderId)
{
    const auto artifactPath =
        "Library/Artifacts/" +
        owner.ToString() +
        "/" +
        SafeArtifactPathToken(subAssetKey);
    return {
        owner,
        std::move(subAssetKey),
        type,
        std::move(loaderId),
        "editor",
        artifactPath,
        "sha256:" + owner.ToString() + ":" + subAssetKey
    };
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

void WriteManifestArtifactFiles(
    const std::filesystem::path& root,
    const NLS::Core::Assets::ArtifactManifest& manifest)
{
    for (const auto& artifact : manifest.subAssets)
        WriteTextFile(root / artifact.artifactPath, artifact.subAssetKey);
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
        FileStamp(sourcePath)
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
    default: return "unknown";
    }
}

std::string DependencyKindToken(const NLS::Core::Assets::AssetDependencyKind kind)
{
    using NLS::Core::Assets::AssetDependencyKind;
    switch (kind)
    {
    case AssetDependencyKind::SourceFileHash: return "source-file-hash";
    case AssetDependencyKind::PathToGuidMapping: return "path-to-guid-mapping";
    case AssetDependencyKind::ImporterVersion: return "importer-version";
    case AssetDependencyKind::PostprocessorVersion: return "postprocessor-version";
    case AssetDependencyKind::BuildTarget: return "build-target";
    default: return "source-file-hash";
    }
}

void WritePersistedArtifactManifest(
    const std::filesystem::path& root,
    const NLS::Core::Assets::ArtifactManifest& manifest)
{
    nlohmann::json document;
    document["schema"] = 1;
    document["sourceAssetId"] = manifest.sourceAssetId.ToString();
    document["importerId"] = manifest.importerId;
    document["importerVersion"] = manifest.importerVersion;
    document["targetPlatform"] = manifest.targetPlatform;
    document["primarySubAssetKey"] = manifest.primarySubAssetKey;
    document["subAssets"] = nlohmann::json::array();
    for (const auto& artifact : manifest.subAssets)
    {
        document["subAssets"].push_back({
            {"sourceAssetId", artifact.sourceAssetId.ToString()},
            {"subAssetKey", artifact.subAssetKey},
            {"artifactType", ArtifactTypeToken(artifact.artifactType)},
            {"loaderId", artifact.loaderId},
            {"targetPlatform", artifact.targetPlatform},
            {"artifactPath", artifact.artifactPath},
            {"contentHash", artifact.contentHash}
        });
    }
    document["dependencies"] = nlohmann::json::array();
    for (const auto& dependency : manifest.dependencies)
    {
        document["dependencies"].push_back({
            {"kind", DependencyKindToken(dependency.kind)},
            {"value", dependency.value},
            {"hashOrVersion", dependency.hashOrVersion}
        });
    }

    WriteTextFile(
        root / "Library" / "Artifacts" / manifest.sourceAssetId.ToString() / "manifest.json",
        document.dump(2));
}

bool HasFolderChild(
    const NLS::Editor::Assets::AssetBrowserFolderNode& node,
    const std::string& projectRelativePath)
{
    return std::any_of(
        node.children.begin(),
        node.children.end(),
        [&projectRelativePath](const NLS::Editor::Assets::AssetBrowserFolderNode& child)
        {
            return child.projectRelativePath == projectRelativePath;
        });
}

const NLS::Editor::Assets::AssetBrowserItem* FindItem(
    const std::vector<NLS::Editor::Assets::AssetBrowserItem>& items,
    const std::string& displayName)
{
    const auto found = std::find_if(
        items.begin(),
        items.end(),
        [&displayName](const NLS::Editor::Assets::AssetBrowserItem& item)
        {
            return item.displayName == displayName;
        });
    return found == items.end() ? nullptr : &(*found);
}

const NLS::Editor::Assets::AssetBrowserItem* FindGeneratedSubAssetItem(
    const std::vector<NLS::Editor::Assets::AssetBrowserItem>& items,
    const std::string& subAssetKey)
{
    const auto found = std::find_if(
        items.begin(),
        items.end(),
        [&subAssetKey](const NLS::Editor::Assets::AssetBrowserItem& item)
        {
            return item.kind == NLS::Editor::Assets::AssetBrowserItemKind::GeneratedSubAsset &&
                item.subAssetKey == subAssetKey;
        });
    return found == items.end() ? nullptr : &(*found);
}
}

TEST(AssetBrowserPresentationTests, BuildsProjectOnlyFolderTreeFromAssetsRoot)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    std::filesystem::create_directories(root / "Assets" / "Models" / "Sponza");
    std::filesystem::create_directories(root / "Assets" / "Materials");
    std::filesystem::create_directories(root / "Engine" / "Builtin");
    std::filesystem::create_directories(root / "Library" / "Artifacts");

    const auto tree = BuildProjectAssetFolderTree(root);

    EXPECT_EQ(tree.displayName, "Assets");
    EXPECT_EQ(tree.projectRelativePath, "Assets");
    EXPECT_TRUE(HasFolderChild(tree, "Assets/Materials"));
    EXPECT_TRUE(HasFolderChild(tree, "Assets/Models"));
    EXPECT_FALSE(HasFolderChild(tree, "Engine"));
    EXPECT_FALSE(HasFolderChild(tree, "Library"));

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, BuildsFolderTreeLazilyByDefault)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    std::filesystem::create_directories(root / "Assets" / "Models" / "Sponza" / "Deep");
    std::filesystem::create_directories(root / "Assets" / "Materials");

    const auto tree = BuildProjectAssetFolderTree(root);

    const auto models = std::find_if(
        tree.children.begin(),
        tree.children.end(),
        [](const AssetBrowserFolderNode& node)
        {
            return node.projectRelativePath == "Assets/Models";
        });

    ASSERT_NE(models, tree.children.end());
    EXPECT_TRUE(models->children.empty());

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, BreadcrumbAcceptsUtf8ProjectFolderWithoutNarrowFilesystemConversion)
{
    using namespace NLS::Editor::Assets;

    const auto input = Utf8String(u8"Assets/模型\\子目录/../贴图/");
    const auto expected = Utf8String(u8"Assets/模型/贴图");

    std::vector<AssetBrowserBreadcrumbSegment> breadcrumb;
    ASSERT_NO_THROW(breadcrumb = BuildAssetBrowserBreadcrumb(input));

    ASSERT_EQ(breadcrumb.size(), 3u);
    EXPECT_EQ(breadcrumb[0].projectRelativePath, "Assets");
    EXPECT_EQ(breadcrumb[1].projectRelativePath, Utf8String(u8"Assets/模型"));
    EXPECT_EQ(breadcrumb[2].projectRelativePath, expected);
}

TEST(AssetBrowserPresentationTests, ProjectRelativePathNormalizerPreservesInvalidRootAndEscapePrefixes)
{
    using namespace NLS::Editor::Assets;

    EXPECT_EQ(NormalizeAssetBrowserProjectRelativePath("../Assets/Models"), "../Assets/Models");
    EXPECT_EQ(NormalizeAssetBrowserProjectRelativePath("/"), "/");
    EXPECT_EQ(NormalizeAssetBrowserProjectRelativePath("/Assets/Models"), "/Assets/Models");
    EXPECT_EQ(NormalizeAssetBrowserProjectRelativePath("Assets/.."), "..");
    EXPECT_EQ(NormalizeAssetBrowserProjectRelativePath("Assets/Models/../../.."), "..");
}

TEST(AssetBrowserPresentationTests, FolderTreeStopsDrawingInvalidatedNodeAfterSelectionChange)
{
    using namespace NLS::Editor::Assets;

    EXPECT_FALSE(ShouldStopDrawingAssetBrowserFolderNodeAfterSelection("Assets/Models", "Assets\\Models/"));
    EXPECT_TRUE(ShouldStopDrawingAssetBrowserFolderNodeAfterSelection("Assets", Utf8String(u8"Assets/模型")));
}

TEST(AssetBrowserPresentationTests, GridStopsDrawingInvalidatedItemsAfterOpeningFolder)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserItem folder;
    folder.kind = AssetBrowserItemKind::Folder;
    folder.projectRelativePath = Utf8String(u8"Assets/模型");

    AssetBrowserItem texture;
    texture.kind = AssetBrowserItemKind::SourceAsset;
    texture.projectRelativePath = Utf8String(u8"Assets/模型/英雄.png");

    EXPECT_TRUE(ShouldStopDrawingAssetBrowserGridAfterOpeningItem("Assets", folder));
    EXPECT_FALSE(ShouldStopDrawingAssetBrowserGridAfterOpeningItem(Utf8String(u8"Assets/模型"), folder));
    EXPECT_FALSE(ShouldStopDrawingAssetBrowserGridAfterOpeningItem("Assets", texture));
}

TEST(AssetBrowserPresentationTests, ThumbnailSizeMinimumUsesProjectBrowserListMode)
{
    using namespace NLS::Editor::Assets;

    EXPECT_EQ(ResolveAssetBrowserContentViewMode(64.0f), AssetBrowserContentViewMode::List);
    EXPECT_EQ(ResolveAssetBrowserContentViewMode(65.0f), AssetBrowserContentViewMode::Grid);
    EXPECT_EQ(ResolveAssetBrowserContentViewMode(96.0f), AssetBrowserContentViewMode::Grid);
}

TEST(AssetBrowserPresentationTests, FallbackIconsUseProjectEditorIconIds)
{
    using namespace NLS::Editor::Assets;

    EXPECT_STREQ(AssetBrowserFallbackIconId(AssetBrowserItemType::Folder), "editor.icon.asset.folder");
    EXPECT_STREQ(AssetBrowserFallbackIconId(AssetBrowserItemType::Texture), "editor.icon.asset.texture");
    EXPECT_STREQ(AssetBrowserFallbackIconId(AssetBrowserItemType::Material), "editor.icon.asset.material");
    EXPECT_STREQ(AssetBrowserFallbackIconId(AssetBrowserItemType::Mesh), "editor.icon.asset.mesh");
    EXPECT_STREQ(AssetBrowserFallbackIconId(AssetBrowserItemType::Prefab), "editor.icon.asset.prefab");
    EXPECT_STREQ(AssetBrowserFallbackIconId(AssetBrowserItemType::Scene), "editor.icon.asset.scene");
    EXPECT_STREQ(AssetBrowserFallbackIconId(AssetBrowserItemType::Shader), "editor.icon.asset.shader");
    EXPECT_STREQ(AssetBrowserFallbackIconId(AssetBrowserItemType::Script), "editor.icon.asset.script");
    EXPECT_STREQ(AssetBrowserFallbackIconId(AssetBrowserItemType::Other), "editor.icon.asset.default");
}

TEST(AssetBrowserPresentationTests, EditorResourcesUsePrefabFileIconIdForPrefabFiles)
{
    EXPECT_STREQ(
        NLS::Editor::Core::EditorResources::GetFileIconId("Hero.prefab"),
        "editor.icon.asset.prefab");
}

TEST(AssetBrowserPresentationTests, AssetTypeIconOverridesUseCatalogedNullusResources)
{
    const auto& records = NLS::Editor::Core::EditorResourceCatalog::DefaultRecords();
    bool foundSceneIcon = false;
    bool foundPrefabIcon = false;
    bool foundMaterialIcon = false;
    for (const auto& record : records)
    {
        if (record.type != NLS::Editor::Core::EditorResourceType::Icon ||
            record.developmentPath.empty())
        {
            continue;
        }

        EXPECT_EQ(record.id.find("unity"), std::string::npos) << record.id;
        EXPECT_EQ(record.developmentPath.generic_string().find("unity"), std::string::npos)
            << record.developmentPath.generic_string();
        const auto iconPath =
            std::filesystem::path(NLS_ROOT_DIR) / "App" / "Assets" / record.developmentPath;
        const auto [width, height] = ReadPngDimensions(iconPath);
        EXPECT_GT(width, 0u) << iconPath.string();
        EXPECT_GT(height, 0u) << iconPath.string();

        if (record.id == "editor.icon.asset.scene")
        {
            foundSceneIcon = true;
            EXPECT_EQ(record.developmentPath.generic_string(), "Editor/Brand/NullusLogoMark.png");
        }
        if (record.id == "editor.icon.asset.prefab")
            foundPrefabIcon = true;
        if (record.id == "editor.icon.asset.material")
            foundMaterialIcon = true;
    }

    EXPECT_TRUE(foundSceneIcon);
    EXPECT_TRUE(foundPrefabIcon);
    EXPECT_TRUE(foundMaterialIcon);
}

TEST(AssetBrowserPresentationTests, GeneratedSubAssetsAreNestedUnderCollapsedSourceAssets)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserItem source;
    source.kind = AssetBrowserItemKind::SourceAsset;
    source.type = AssetBrowserItemType::Model;
    source.displayName = "Sponza.fbx";
    source.projectRelativePath = "Assets/Models/Sponza.fbx";
    source.sourceAssetPath = source.projectRelativePath;

    AssetBrowserItem mesh;
    mesh.kind = AssetBrowserItemKind::GeneratedSubAsset;
    mesh.type = AssetBrowserItemType::Mesh;
    mesh.displayName = "Mesh A";
    mesh.projectRelativePath = "Assets/Models/Sponza.fbx::mesh:A";
    mesh.sourceAssetPath = source.sourceAssetPath;

    AssetBrowserItem material;
    material.kind = AssetBrowserItemKind::GeneratedSubAsset;
    material.type = AssetBrowserItemType::Material;
    material.displayName = "Material A";
    material.projectRelativePath = "Assets/Models/Sponza.fbx::material:A";
    material.sourceAssetPath = source.sourceAssetPath;

    const std::vector<AssetBrowserItem> items { source, mesh, material };
    const auto collapsed = BuildAssetBrowserDisplayItems(items, {});
    ASSERT_EQ(collapsed.size(), 1u);
    EXPECT_EQ(collapsed[0].item.projectRelativePath, source.projectRelativePath);
    EXPECT_EQ(collapsed[0].childCount, 2u);
    EXPECT_FALSE(collapsed[0].subAsset);
    EXPECT_FALSE(collapsed[0].expanded);

    const auto expanded = BuildAssetBrowserDisplayItems(items, { source.sourceAssetPath });
    ASSERT_EQ(expanded.size(), 3u);
    EXPECT_EQ(expanded[0].childCount, 2u);
    EXPECT_TRUE(expanded[0].expanded);
    EXPECT_TRUE(expanded[1].subAsset);
    EXPECT_EQ(expanded[1].item.displayName, "Mesh A");
    EXPECT_TRUE(expanded[2].subAsset);
    EXPECT_EQ(expanded[2].item.displayName, "Material A");
}

TEST(AssetBrowserPresentationTests, GridThumbnailLayoutPreservesImageAspectRatio)
{
    const NLS::Editor::Assets::AssetBrowserRect bounds {
        { 10.0f, 20.0f },
        { 74.0f, 84.0f }
    };
    const auto result = NLS::Editor::Assets::ComputeAssetBrowserThumbnailRect(
        bounds,
        256u,
        128u);

    EXPECT_FLOAT_EQ(result.min.x, 10.0f);
    EXPECT_FLOAT_EQ(result.min.y, 36.0f);
    EXPECT_FLOAT_EQ(result.max.x, 74.0f);
    EXPECT_FLOAT_EQ(result.max.y, 68.0f);
}

TEST(AssetBrowserPresentationTests, ThumbnailLetterboxBackgroundOnlyAppliesToTextures)
{
    using namespace NLS::Editor::Assets;

    EXPECT_TRUE(ShouldDrawAssetBrowserThumbnailLetterboxBackground(AssetBrowserItemType::Texture));
    EXPECT_FALSE(ShouldDrawAssetBrowserThumbnailLetterboxBackground(AssetBrowserItemType::Material));
    EXPECT_FALSE(ShouldDrawAssetBrowserThumbnailLetterboxBackground(AssetBrowserItemType::Prefab));
    EXPECT_FALSE(ShouldDrawAssetBrowserThumbnailLetterboxBackground(AssetBrowserItemType::Model));
}

TEST(AssetBrowserPresentationTests, ExpandedFolderTreeNodesEnumerateOnlyRequestedBranches)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    std::filesystem::create_directories(root / "Assets" / "Models" / "Sponza" / "Deep");
    std::filesystem::create_directories(root / "Assets" / "Materials" / "Wood");

    AssetBrowserFolderTreeBuildOptions options;
    options.expandedFolders.insert("Assets/Models");
    const auto tree = BuildProjectAssetFolderTree(root, options);

    const auto models = std::find_if(
        tree.children.begin(),
        tree.children.end(),
        [](const AssetBrowserFolderNode& node)
        {
            return node.projectRelativePath == "Assets/Models";
        });
    const auto materials = std::find_if(
        tree.children.begin(),
        tree.children.end(),
        [](const AssetBrowserFolderNode& node)
        {
            return node.projectRelativePath == "Assets/Materials";
        });

    ASSERT_NE(models, tree.children.end());
    ASSERT_NE(materials, tree.children.end());
    EXPECT_TRUE(HasFolderChild(*models, "Assets/Models/Sponza"));
    EXPECT_TRUE(materials->children.empty());

    const auto sponza = std::find_if(
        models->children.begin(),
        models->children.end(),
        [](const AssetBrowserFolderNode& node)
        {
            return node.projectRelativePath == "Assets/Models/Sponza";
        });
    ASSERT_NE(sponza, models->children.end());
    EXPECT_TRUE(sponza->children.empty());

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, RejectsAssetsRootThatResolvesOutsideProject)
{
    using namespace NLS::Editor::Assets;

    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_asset_browser_project_" + NLS::Guid::New().ToString());
    const auto outside = root.parent_path() / ("nullus_asset_browser_outside_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(outside / "ExternalAssets" / "OutsideFolder");
    WriteTextFile(outside / "ExternalAssets" / "Outside.mat", "material");

    std::error_code error;
    std::filesystem::create_directory_symlink(outside / "ExternalAssets", root / "Assets", error);
    if (error)
    {
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(outside);
        GTEST_SKIP() << "Directory symlink creation is not available in this environment.";
    }

    const auto tree = BuildProjectAssetFolderTree(root);
    const auto items = BuildCurrentFolderAssetItems(root, "Assets", nullptr);
    const auto resolved = ResolveAssetBrowserFolderSelection(root, "Assets");

    EXPECT_EQ(tree.projectRelativePath, "Assets");
    EXPECT_TRUE(tree.children.empty());
    EXPECT_FALSE(HasFolderChild(tree, "Assets/OutsideFolder"));
    EXPECT_TRUE(items.empty());
    EXPECT_FALSE(resolved.exists);

    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside);
}

TEST(AssetBrowserPresentationTests, SkipsDirectoryLinksThatEscapeAssetsRoot)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    const auto outside = root.parent_path() / ("nullus_asset_browser_outside_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(outside / "Escaped");
    WriteTextFile(outside / "Escaped" / "Outside.mat", "material");

    std::error_code error;
    std::filesystem::create_directory_symlink(outside, root / "Assets" / "LinkedOutside", error);
    if (error)
    {
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(outside);
        GTEST_SKIP() << "Directory symlink creation is not available in this environment.";
    }

    const auto tree = BuildProjectAssetFolderTree(root);
    const auto items = BuildCurrentFolderAssetItems(root, "Assets", nullptr);

    EXPECT_FALSE(HasFolderChild(tree, "Assets/LinkedOutside"));
    EXPECT_EQ(FindItem(items, "LinkedOutside"), nullptr);
    EXPECT_EQ(FindItem(items, "Outside.mat"), nullptr);

    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside);
}

TEST(AssetBrowserPresentationTests, RejectsSelectedFoldersThatResolveThroughEscapingDirectoryLinks)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    const auto outside = root.parent_path() / ("nullus_asset_browser_outside_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(outside / "Escaped");
    WriteTextFile(outside / "Escaped" / "Outside.mat", "material");

    std::error_code error;
    std::filesystem::create_directory_symlink(outside, root / "Assets" / "LinkedOutside", error);
    if (error)
    {
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(outside);
        GTEST_SKIP() << "Directory symlink creation is not available in this environment.";
    }

    const auto resolved = ResolveAssetBrowserFolderSelection(root, "Assets/LinkedOutside/Escaped");
    const auto items = BuildCurrentFolderAssetItems(root, "Assets/LinkedOutside/Escaped", nullptr);

    EXPECT_EQ(resolved.projectRelativePath, "Assets");
    EXPECT_EQ(FindItem(items, "Outside.mat"), nullptr);
    EXPECT_TRUE(items.empty());

    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside);
}

TEST(AssetBrowserPresentationTests, ProjectAssetFileMoveGuardRequiresSourceAndTargetInsideAssetsRoot)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    const auto assetsRoot = root / "Assets";
    const auto targetFolder = assetsRoot / "Materials";
    const auto outside = root.parent_path() / ("nullus_asset_browser_outside_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(targetFolder);
    std::filesystem::create_directories(outside);
    WriteTextFile(assetsRoot / "Hero.mat", "material");
    WriteTextFile(outside / "Rogue.mat", "material");

    EXPECT_TRUE(CanMovePhysicalProjectAssetFileIntoFolder(
        assetsRoot,
        assetsRoot / "Hero.mat",
        targetFolder));
    EXPECT_FALSE(CanMovePhysicalProjectAssetFileIntoFolder(
        assetsRoot,
        outside / "Rogue.mat",
        targetFolder));
    EXPECT_FALSE(CanMovePhysicalProjectAssetFileIntoFolder(
        assetsRoot,
        assetsRoot / "Hero.mat",
        outside));

    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside);
}

TEST(AssetBrowserPresentationTests, ProjectAssetFolderMoveGuardRequiresProjectFoldersAndRejectsSelfMoves)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    const auto assetsRoot = root / "Assets";
    const auto sourceFolder = assetsRoot / "Models" / "Hero";
    const auto targetFolder = assetsRoot / "Imported";
    const auto outside = root.parent_path() / ("nullus_asset_browser_outside_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(sourceFolder / "Nested");
    std::filesystem::create_directories(targetFolder);
    std::filesystem::create_directories(outside / "Rogue");

    EXPECT_TRUE(CanMovePhysicalProjectAssetFolderIntoFolder(
        assetsRoot,
        sourceFolder,
        targetFolder));
    EXPECT_FALSE(CanMovePhysicalProjectAssetFolderIntoFolder(
        assetsRoot,
        outside / "Rogue",
        targetFolder));
    EXPECT_FALSE(CanMovePhysicalProjectAssetFolderIntoFolder(
        assetsRoot,
        sourceFolder,
        outside));
    EXPECT_FALSE(CanMovePhysicalProjectAssetFolderIntoFolder(
        assetsRoot,
        sourceFolder,
        sourceFolder / "Nested"));
    EXPECT_FALSE(CanMovePhysicalProjectAssetFolderIntoFolder(
        assetsRoot,
        sourceFolder,
        sourceFolder));

    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside);
}

TEST(AssetBrowserPresentationTests, ProjectBrowserResourceDropGuardRejectsEngineResourcePaths)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    const auto assetsRoot = root / "Assets";
    const auto targetFolder = assetsRoot / "Materials";
    std::filesystem::create_directories(targetFolder);
    WriteTextFile(assetsRoot / "Hero.mat", "material");

    EXPECT_TRUE(CanMoveProjectBrowserResourcePathIntoFolder(
        assetsRoot,
        "Assets/Hero.mat",
        targetFolder,
        false));
    EXPECT_FALSE(CanMoveProjectBrowserResourcePathIntoFolder(
        assetsRoot,
        ":Engine/Builtin/Hero.mat",
        targetFolder,
        false));
    EXPECT_FALSE(CanMoveProjectBrowserResourcePathIntoFolder(
        assetsRoot,
        ":Engine/Builtin",
        targetFolder,
        true));

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, ProjectBrowserResourceDropGuardAcceptsUtf8ProjectPaths)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    const auto assetsRoot = root / "Assets";
    const auto sourceFolder = assetsRoot / Utf8Path(u8"模型");
    const auto targetFolder = assetsRoot / Utf8Path(u8"材质");
    std::filesystem::create_directories(sourceFolder);
    std::filesystem::create_directories(targetFolder);
    WriteTextFile(sourceFolder / Utf8Path(u8"英雄_😀.mat"), "material");

    EXPECT_NO_THROW({
        EXPECT_TRUE(CanMoveProjectBrowserResourcePathIntoFolder(
            assetsRoot,
            Utf8String(u8"Assets/模型/英雄_😀.mat"),
            targetFolder,
            false));
    });

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, CurrentFolderSourceAssetTypesMatchPathParser)
{
    using namespace NLS::Editor::Assets;
    using FileType = NLS::Utils::PathParser::EFileType;

    const auto root = MakeAssetBrowserPresentationRoot();
    WriteTextFile(root / "Assets" / "Hero.fbx", "model");
    WriteTextFile(root / "Assets" / "Hero.bmp", "texture");
    WriteTextFile(root / "Assets" / "Hero.dds", "texture");
    WriteTextFile(root / "Assets" / "Hero.shader", "shader");
    WriteTextFile(root / "Assets" / "Hero.nscene", "scene");
    WriteTextFile(root / "Assets" / "Hero.cs", "script");
    WriteTextFile(root / "Assets" / "Hero.py", "script");
    WriteTextFile(root / "Assets" / "Hero.wav", "sound");
    WriteTextFile(root / "Assets" / "Hero.ttf", "font");

    const auto items = BuildCurrentFolderAssetItems(root, "Assets", nullptr);

    struct Case
    {
        const char* displayName;
        AssetBrowserItemType browserType;
        FileType parserType;
    };
    const Case cases[] {
        {"Hero.fbx", AssetBrowserItemType::Model, FileType::MODEL},
        {"Hero.bmp", AssetBrowserItemType::Texture, FileType::TEXTURE},
        {"Hero.dds", AssetBrowserItemType::Texture, FileType::TEXTURE},
        {"Hero.shader", AssetBrowserItemType::Shader, FileType::SHADER},
        {"Hero.nscene", AssetBrowserItemType::Scene, FileType::SCENE},
        {"Hero.cs", AssetBrowserItemType::Script, FileType::SCRIPT},
        {"Hero.py", AssetBrowserItemType::Script, FileType::SCRIPT},
        {"Hero.wav", AssetBrowserItemType::Other, FileType::SOUND},
        {"Hero.ttf", AssetBrowserItemType::Other, FileType::FONT}
    };

    for (const auto& testCase : cases)
    {
        const auto* item = FindItem(items, testCase.displayName);
        ASSERT_NE(item, nullptr) << testCase.displayName;
        EXPECT_EQ(item->type, testCase.browserType) << testCase.displayName;
        EXPECT_EQ(
            item->type,
            AssetBrowserItemTypeFromPathParserFileType(testCase.parserType)) << testCase.displayName;
        EXPECT_EQ(
            NLS::Utils::PathParser::GetFileType(item->projectRelativePath),
            testCase.parserType) << testCase.displayName;
    }

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, BuildsCurrentFolderDirectContentsAndHidesImplementationFiles)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    std::filesystem::create_directories(root / "Assets" / "Models" / "Sponza");
    std::filesystem::create_directories(root / "Assets" / "Models" / "Nested");
    WriteTextFile(root / "Assets" / "Models" / "Hero.fbx", "fbx");
    WriteTextFile(root / "Assets" / "Models" / "Hero.fbx.meta", "meta");
    WriteTextFile(root / "Assets" / "Models" / "Notes.txt", "notes");
    WriteTextFile(root / "Assets" / "Models" / "Nested" / "Hidden.mat", "material");
    WriteTextFile(root / "Library" / "Artifacts" / "Hidden.nmesh", "mesh");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto items = BuildCurrentFolderAssetItems(root, "Assets/Models", &database);

    ASSERT_EQ(items.size(), 4u);
    ASSERT_NE(FindItem(items, "Hero.fbx"), nullptr);
    ASSERT_NE(FindItem(items, "Nested"), nullptr);
    ASSERT_NE(FindItem(items, "Notes.txt"), nullptr);
    ASSERT_NE(FindItem(items, "Sponza"), nullptr);
    EXPECT_EQ(FindItem(items, "Nested")->kind, AssetBrowserItemKind::Folder);
    EXPECT_EQ(FindItem(items, "Nested")->dragResourcePath, "Assets/Models/Nested");
    EXPECT_EQ(FindItem(items, "Sponza")->kind, AssetBrowserItemKind::Folder);
    EXPECT_EQ(FindItem(items, "Sponza")->dragResourcePath, "Assets/Models/Sponza");
    EXPECT_EQ(FindItem(items, "Hero.fbx")->kind, AssetBrowserItemKind::SourceAsset);
    EXPECT_EQ(FindItem(items, "Hero.fbx")->type, AssetBrowserItemType::Model);
    EXPECT_EQ(FindItem(items, "Hero.fbx.meta"), nullptr);
    EXPECT_EQ(FindItem(items, "Hidden.mat"), nullptr);
    EXPECT_EQ(FindItem(items, "Hidden.nmesh"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, CanBuildCurrentFolderWithoutPerFileMetadataWhenDatabaseIsPending)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    const auto texturePath = root / "Assets" / "Textures" / "Hero.png";
    WriteTextFile(texturePath, "texture");
    const auto meta = NLS::Core::Assets::AssetMeta::CreateForAsset(texturePath);
    ASSERT_TRUE(meta.Save(NLS::Core::Assets::GetAssetMetaPath(texturePath)));

    AssetBrowserBuildOptions options;
    options.loadSourceAssetMetadataWithoutDatabase = false;
    const auto items = BuildCurrentFolderAssetItems(root, "Assets/Textures", nullptr, options);

    const auto* hero = FindItem(items, "Hero.png");
    ASSERT_NE(hero, nullptr);
    EXPECT_EQ(hero->type, AssetBrowserItemType::Texture);
    EXPECT_FALSE(hero->assetId.IsValid());

    options.loadSourceAssetMetadataWithoutDatabase = true;
    const auto metadataItems = BuildCurrentFolderAssetItems(root, "Assets/Textures", nullptr, options);
    const auto* heroWithMetadata = FindItem(metadataItems, "Hero.png");
    ASSERT_NE(heroWithMetadata, nullptr);
    EXPECT_TRUE(heroWithMetadata->assetId.IsValid());
    EXPECT_EQ(heroWithMetadata->assetId, meta.id);

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, DirectFolderItemsDoNotImplicitlyExpandGeneratedRecords)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"}})");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto modelId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Hero.gltf"));
    ArtifactManifest manifest;
    manifest.sourceAssetId = modelId;
    manifest.importerId = "scene-model";
    manifest.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "model:Hero";
    manifest.subAssets.push_back(MakeArtifact(modelId, "model:Hero", ArtifactType::Model, "model"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "prefab:Hero", ArtifactType::Prefab, "prefab"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "mesh:Body", ArtifactType::Mesh, "mesh"));
    database.AddArtifactManifest(manifest);

    const auto items = BuildCurrentFolderAssetItems(root, "Assets/Models", &database);

    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].displayName, "Hero.gltf");
    EXPECT_EQ(items[0].kind, AssetBrowserItemKind::SourceAsset);
    EXPECT_EQ(FindItem(items, "Hero"), nullptr);
    EXPECT_EQ(FindItem(items, "Body"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, BuildsCurrentFolderItemsWithGeneratedSubAssetsWhenRequested)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
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
    manifest.subAssets.push_back(MakeArtifact(modelId, "model:Hero", ArtifactType::Model, "model"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "prefab:Hero", ArtifactType::Prefab, "prefab"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "mesh:Body", ArtifactType::Mesh, "mesh"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "material:Body", ArtifactType::Material, "material"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "texture:Albedo", ArtifactType::Texture, "texture"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "shader:HeroSurface", ArtifactType::Shader, "shader"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "scene:HeroScene", ArtifactType::Scene, "scene"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "animation:Idle", ArtifactType::AnimationClip, "animation"));
    WriteManifestArtifactFiles(root, manifest);
    AddCurrentSourceDependencies(root, manifest, "Assets/Models/Hero.gltf");
    database.AddArtifactManifest(manifest);

    AssetBrowserBuildOptions options;
    options.includeGeneratedSubAssets = true;
    const auto items = BuildCurrentFolderAssetItems(root, "Assets/Models", &database, options);

    ASSERT_EQ(items.size(), 6u);
    ASSERT_NE(FindItem(items, "Hero.gltf"), nullptr);
    ASSERT_NE(FindItem(items, "Hero"), nullptr);
    ASSERT_NE(FindItem(items, "Body"), nullptr);
    ASSERT_NE(FindItem(items, "Albedo"), nullptr);
    ASSERT_NE(FindItem(items, "HeroSurface"), nullptr);
    EXPECT_EQ(FindGeneratedSubAssetItem(items, "model:Hero"), nullptr);
    EXPECT_EQ(FindGeneratedSubAssetItem(items, "scene:HeroScene"), nullptr);
    EXPECT_EQ(FindItem(items, "Idle"), nullptr);

    const auto* source = FindItem(items, "Hero.gltf");
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->type, AssetBrowserItemType::Model);
    EXPECT_EQ(source->selectionResourcePath, "Assets/Models/Hero.gltf");
    EXPECT_TRUE(source->previewableInAssetView);

    const auto* prefab = FindGeneratedSubAssetItem(items, "prefab:Hero");
    ASSERT_NE(prefab, nullptr);
    EXPECT_EQ(prefab->kind, AssetBrowserItemKind::GeneratedSubAsset);
    EXPECT_EQ(prefab->type, AssetBrowserItemType::Prefab);
    EXPECT_EQ(prefab->projectRelativePath, "Assets/Models/Hero.gltf::prefab:Hero");
    EXPECT_EQ(prefab->sourceAssetPath, "Assets/Models/Hero.gltf");
    EXPECT_EQ(prefab->dragResourcePath, "Assets/Models/Hero.gltf");
    EXPECT_EQ(prefab->assetId, modelId);
    EXPECT_EQ(prefab->subAssetKey, "prefab:Hero");
    EXPECT_EQ(prefab->artifactType, ArtifactType::Prefab);
    EXPECT_TRUE(prefab->generatedReadOnly);
    EXPECT_EQ(prefab->selectionResourcePath, "Assets/Models/Hero.gltf#prefab:Hero");
    EXPECT_FALSE(prefab->previewableInAssetView);

    const auto* mesh = FindGeneratedSubAssetItem(items, "mesh:Body");
    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(mesh->kind, AssetBrowserItemKind::GeneratedSubAsset);
    EXPECT_EQ(mesh->type, AssetBrowserItemType::Mesh);
    EXPECT_EQ(mesh->projectRelativePath, "Assets/Models/Hero.gltf::mesh:Body");
    EXPECT_EQ(mesh->sourceAssetPath, "Assets/Models/Hero.gltf");
    EXPECT_EQ(mesh->dragResourcePath, "Assets/Models/Hero.gltf");
    EXPECT_EQ(mesh->assetId, modelId);
    EXPECT_EQ(mesh->subAssetKey, "mesh:Body");
    EXPECT_EQ(mesh->artifactType, ArtifactType::Mesh);
    EXPECT_TRUE(mesh->generatedReadOnly);
    EXPECT_EQ(mesh->selectionResourcePath, "Assets/Models/Hero.gltf#mesh:Body");
    EXPECT_FALSE(mesh->previewableInAssetView);

    const auto* material = FindGeneratedSubAssetItem(items, "material:Body");
    ASSERT_NE(material, nullptr);
    EXPECT_EQ(material->kind, AssetBrowserItemKind::GeneratedSubAsset);
    EXPECT_EQ(material->type, AssetBrowserItemType::Material);
    EXPECT_EQ(material->projectRelativePath, "Assets/Models/Hero.gltf::material:Body");
    EXPECT_EQ(material->sourceAssetPath, "Assets/Models/Hero.gltf");
    EXPECT_EQ(material->dragResourcePath, "Assets/Models/Hero.gltf");
    EXPECT_EQ(material->assetId, modelId);
    EXPECT_EQ(material->subAssetKey, "material:Body");
    EXPECT_EQ(material->artifactType, ArtifactType::Material);
    EXPECT_TRUE(material->generatedReadOnly);
    EXPECT_EQ(material->selectionResourcePath, "Assets/Models/Hero.gltf#material:Body");
    EXPECT_FALSE(material->previewableInAssetView);

    const auto* texture = FindGeneratedSubAssetItem(items, "texture:Albedo");
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->kind, AssetBrowserItemKind::GeneratedSubAsset);
    EXPECT_EQ(texture->type, AssetBrowserItemType::Texture);
    EXPECT_EQ(texture->projectRelativePath, "Assets/Models/Hero.gltf::texture:Albedo");
    EXPECT_EQ(texture->sourceAssetPath, "Assets/Models/Hero.gltf");
    EXPECT_EQ(texture->dragResourcePath, "Assets/Models/Hero.gltf");
    EXPECT_EQ(texture->assetId, modelId);
    EXPECT_EQ(texture->subAssetKey, "texture:Albedo");
    EXPECT_EQ(texture->artifactType, ArtifactType::Texture);
    EXPECT_TRUE(texture->generatedReadOnly);
    EXPECT_EQ(texture->selectionResourcePath, "Assets/Models/Hero.gltf#texture:Albedo");
    EXPECT_FALSE(texture->previewableInAssetView);

    const auto* shader = FindGeneratedSubAssetItem(items, "shader:HeroSurface");
    ASSERT_NE(shader, nullptr);
    EXPECT_EQ(shader->kind, AssetBrowserItemKind::GeneratedSubAsset);
    EXPECT_EQ(shader->type, AssetBrowserItemType::Shader);
    EXPECT_EQ(shader->projectRelativePath, "Assets/Models/Hero.gltf::shader:HeroSurface");
    EXPECT_EQ(shader->sourceAssetPath, "Assets/Models/Hero.gltf");
    EXPECT_EQ(shader->dragResourcePath, "Assets/Models/Hero.gltf");
    EXPECT_EQ(shader->assetId, modelId);
    EXPECT_EQ(shader->subAssetKey, "shader:HeroSurface");
    EXPECT_EQ(shader->artifactType, ArtifactType::Shader);
    EXPECT_TRUE(shader->generatedReadOnly);
    EXPECT_EQ(shader->selectionResourcePath, "Assets/Models/Hero.gltf#shader:HeroSurface");
    EXPECT_FALSE(shader->previewableInAssetView);

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, FiltersCurrentFolderItemsByCaseInsensitiveSearchQuery)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    std::filesystem::create_directories(root / "Assets" / "Models" / "LampParts");
    std::filesystem::create_directories(root / "Assets" / "Models" / "Nested");
    WriteTextFile(root / "Assets" / "Models" / "Lamp_Base.mat", "material");
    WriteTextFile(root / "Assets" / "Models" / "CeilingLamp.fbx", "fbx");
    WriteTextFile(root / "Assets" / "Models" / "Table.png", "png");
    WriteTextFile(root / "Assets" / "Models" / "Nested" / "LampHidden.mat", "material");

    AssetBrowserBuildOptions options;
    options.searchQuery = "lamp";
    const auto items = BuildCurrentFolderAssetItems(root, "Assets/Models", nullptr, options);

    ASSERT_EQ(items.size(), 3u);
    EXPECT_NE(FindItem(items, "CeilingLamp.fbx"), nullptr);
    EXPECT_NE(FindItem(items, "Lamp_Base.mat"), nullptr);
    EXPECT_NE(FindItem(items, "LampParts"), nullptr);
    EXPECT_EQ(FindItem(items, "Table.png"), nullptr);
    EXPECT_EQ(FindItem(items, "LampHidden.mat"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, FiltersCurrentFolderItemsByTypeAndSearchQuery)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    std::filesystem::create_directories(root / "Assets" / "Materials" / "LampFolder");
    WriteTextFile(root / "Assets" / "Materials" / "LampGlass.mat", "material");
    WriteTextFile(root / "Assets" / "Materials" / "LampIcon.png", "texture");
    WriteTextFile(root / "Assets" / "Materials" / "Metal.mat", "material");
    WriteTextFile(root / "Assets" / "Materials" / "LampShader.hlsl", "shader");

    AssetBrowserBuildOptions options;
    options.searchQuery = "lamp";
    options.typeFilter = AssetBrowserItemType::Material;
    const auto materialItems = BuildCurrentFolderAssetItems(root, "Assets/Materials", nullptr, options);

    ASSERT_EQ(materialItems.size(), 1u);
    EXPECT_EQ(materialItems[0].displayName, "LampGlass.mat");
    EXPECT_EQ(materialItems[0].type, AssetBrowserItemType::Material);

    options.typeFilter = AssetBrowserItemType::All;
    const auto allItems = BuildCurrentFolderAssetItems(root, "Assets/Materials", nullptr, options);
    ASSERT_EQ(allItems.size(), 4u);
    EXPECT_NE(FindItem(allItems, "LampFolder"), nullptr);
    EXPECT_NE(FindItem(allItems, "LampGlass.mat"), nullptr);
    EXPECT_NE(FindItem(allItems, "LampIcon.png"), nullptr);
    EXPECT_NE(FindItem(allItems, "LampShader.hlsl"), nullptr);
    EXPECT_EQ(FindItem(allItems, "Metal.mat"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, FiltersCachedCurrentFolderItemsWithoutRebuildingGeneratedEntries)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserItem sourceModel;
    sourceModel.displayName = "Hero.gltf";
    sourceModel.projectRelativePath = "Assets/Models/Hero.gltf";
    sourceModel.sourceAssetPath = sourceModel.projectRelativePath;
    sourceModel.kind = AssetBrowserItemKind::SourceAsset;
    sourceModel.type = AssetBrowserItemType::Model;

    AssetBrowserItem generatedMaterial;
    generatedMaterial.displayName = "Body";
    generatedMaterial.projectRelativePath = "Assets/Models/Hero.gltf::material:Body";
    generatedMaterial.sourceAssetPath = sourceModel.projectRelativePath;
    generatedMaterial.subAssetKey = "material:Body";
    generatedMaterial.kind = AssetBrowserItemKind::GeneratedSubAsset;
    generatedMaterial.type = AssetBrowserItemType::Material;
    generatedMaterial.generatedReadOnly = true;

    AssetBrowserItem texture;
    texture.displayName = "Hero_D.png";
    texture.projectRelativePath = "Assets/Models/Hero_D.png";
    texture.sourceAssetPath = texture.projectRelativePath;
    texture.kind = AssetBrowserItemKind::SourceAsset;
    texture.type = AssetBrowserItemType::Texture;

    const std::vector<AssetBrowserItem> cachedItems {
        sourceModel,
        generatedMaterial,
        texture
    };

    AssetBrowserBuildOptions options;
    options.includeGeneratedSubAssets = true;
    options.searchQuery = "body";
    options.typeFilter = AssetBrowserItemType::Material;
    const auto materialItems = FilterAssetBrowserItems(cachedItems, options);

    ASSERT_EQ(materialItems.size(), 1u);
    EXPECT_EQ(materialItems[0].projectRelativePath, generatedMaterial.projectRelativePath);
    EXPECT_EQ(materialItems[0].subAssetKey, generatedMaterial.subAssetKey);

    options.includeGeneratedSubAssets = false;
    const auto sourceOnlyItems = FilterAssetBrowserItems(cachedItems, options);
    EXPECT_TRUE(sourceOnlyItems.empty());
}

TEST(AssetBrowserPresentationTests, ThumbnailGenerationScopeKeyChangesWhenVisibleItemsChange)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserItem material;
    material.displayName = "LampGlass.mat";
    material.projectRelativePath = "Assets/Materials/LampGlass.mat";
    material.sourceAssetPath = material.projectRelativePath;
    material.kind = AssetBrowserItemKind::SourceAsset;
    material.type = AssetBrowserItemType::Material;

    AssetBrowserItem texture;
    texture.displayName = "LampIcon.png";
    texture.projectRelativePath = "Assets/Materials/LampIcon.png";
    texture.sourceAssetPath = texture.projectRelativePath;
    texture.kind = AssetBrowserItemKind::SourceAsset;
    texture.type = AssetBrowserItemType::Texture;

    const std::vector<AssetBrowserItem> materialItems { material };
    const std::vector<AssetBrowserItem> textureItems { texture };

    const auto materialScope = BuildAssetBrowserThumbnailGenerationScopeKey(
        "Assets/Materials",
        96u,
        materialItems);
    const auto textureScope = BuildAssetBrowserThumbnailGenerationScopeKey(
        "Assets/Materials",
        96u,
        textureItems);

    EXPECT_NE(materialScope, textureScope);
    EXPECT_EQ(
        materialScope,
        BuildAssetBrowserThumbnailGenerationScopeKey("Assets/Materials", 96u, materialItems));
}

TEST(AssetBrowserPresentationTests, ThumbnailGenerationSelectionKeepsVisibleMaterialAndPrefabPreviewItems)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserItem material;
    material.displayName = "Lamp.mat";
    material.projectRelativePath = "Assets/Materials/Lamp.mat";
    material.sourceAssetPath = material.projectRelativePath;
    material.kind = AssetBrowserItemKind::SourceAsset;
    material.type = AssetBrowserItemType::Material;

    AssetBrowserItem prefab;
    prefab.displayName = "Lamp.prefab";
    prefab.projectRelativePath = "Assets/Prefabs/Lamp.prefab";
    prefab.sourceAssetPath = prefab.projectRelativePath;
    prefab.kind = AssetBrowserItemKind::SourceAsset;
    prefab.type = AssetBrowserItemType::Prefab;

    AssetBrowserItem hiddenTexture;
    hiddenTexture.displayName = "Hidden.png";
    hiddenTexture.projectRelativePath = "Assets/Textures/Hidden.png";
    hiddenTexture.sourceAssetPath = hiddenTexture.projectRelativePath;
    hiddenTexture.kind = AssetBrowserItemKind::SourceAsset;
    hiddenTexture.type = AssetBrowserItemType::Texture;

    const std::vector<AssetBrowserItem> currentFolderItems { material, prefab, hiddenTexture };
    const std::vector<AssetBrowserItem> visibleItems { material, prefab };

    const auto selected = SelectAssetBrowserThumbnailGenerationItems(
        currentFolderItems,
        visibleItems,
        true);

    ASSERT_EQ(selected.size(), 2u);
    EXPECT_EQ(selected[0].type, AssetBrowserItemType::Material);
    EXPECT_EQ(selected[1].type, AssetBrowserItemType::Prefab);
}

TEST(AssetBrowserPresentationTests, ThumbnailGenerationItemsWaitForVisibleScope)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserItem material;
    material.displayName = "Lamp.mat";
    material.projectRelativePath = "Assets/Materials/Lamp.mat";
    material.kind = AssetBrowserItemKind::SourceAsset;
    material.type = AssetBrowserItemType::Material;

    AssetBrowserItem texture;
    texture.displayName = "Lamp.png";
    texture.projectRelativePath = "Assets/Materials/Lamp.png";
    texture.kind = AssetBrowserItemKind::SourceAsset;
    texture.type = AssetBrowserItemType::Texture;

    const std::vector<AssetBrowserItem> currentFolderItems {material, texture};
    const std::vector<AssetBrowserItem> visibleItems {};

    const auto unknownVisibleScope = SelectAssetBrowserThumbnailGenerationItems(
        currentFolderItems,
        visibleItems,
        false);
    EXPECT_TRUE(unknownVisibleScope.empty());

    const auto knownEmptyVisibleScope = SelectAssetBrowserThumbnailGenerationItems(
        currentFolderItems,
        visibleItems,
        true);
    EXPECT_TRUE(knownEmptyVisibleScope.empty());

    const auto knownVisibleScope = SelectAssetBrowserThumbnailGenerationItems(
        currentFolderItems,
        std::vector<AssetBrowserItem> { texture },
        true);
    ASSERT_EQ(knownVisibleScope.size(), 1u);
    EXPECT_EQ(knownVisibleScope[0].projectRelativePath, texture.projectRelativePath);
}

TEST(AssetBrowserPresentationTests, ThumbnailGenerationScopeDecisionRequeriesDirtySameScope)
{
    using namespace NLS::Editor::Assets;

    const auto decision = EvaluateAssetBrowserThumbnailGenerationScope(
        "scope-a",
        96u,
        true,
        "scope-a",
        96u);

    EXPECT_FALSE(decision.canSkip);
    EXPECT_FALSE(decision.scopeChanged);
    EXPECT_TRUE(decision.requerySameScope);

    const auto cleanDecision = EvaluateAssetBrowserThumbnailGenerationScope(
        "scope-a",
        96u,
        false,
        "scope-a",
        96u);
    EXPECT_TRUE(cleanDecision.canSkip);
    EXPECT_FALSE(cleanDecision.scopeChanged);
    EXPECT_FALSE(cleanDecision.requerySameScope);

    const auto changedDecision = EvaluateAssetBrowserThumbnailGenerationScope(
        "scope-a",
        96u,
        true,
        "scope-b",
        96u);
    EXPECT_FALSE(changedDecision.canSkip);
    EXPECT_TRUE(changedDecision.scopeChanged);
    EXPECT_FALSE(changedDecision.requerySameScope);

    const auto resizedDecision = EvaluateAssetBrowserThumbnailGenerationScope(
        "scope-a",
        96u,
        false,
        "scope-a",
        128u);
    EXPECT_FALSE(resizedDecision.canSkip);
    EXPECT_TRUE(resizedDecision.scopeChanged);
    EXPECT_FALSE(resizedDecision.requerySameScope);
}

TEST(AssetBrowserPresentationTests, ThumbnailTextureEvictionSkipsTexturesUsedThisFrame)
{
    using namespace NLS::Editor::Assets;

    const std::vector<std::string> lru {
        "old-used",
        "old-free",
        "new-free"
    };
    const std::unordered_set<std::string> usedThisFrame {
        "old-used"
    };

    const auto evictions = SelectAssetBrowserThumbnailTextureEvictionCandidates(lru, usedThisFrame, 1u);

    ASSERT_EQ(evictions.size(), 2u);
    EXPECT_EQ(evictions[0], "old-free");
    EXPECT_EQ(evictions[1], "new-free");
}

TEST(AssetBrowserPresentationTests, ThumbnailTextureLoadCandidatesSkipResidentAndRespectBudget)
{
    using namespace NLS::Editor::Assets;

    const std::vector<std::string> queued {
        "already-loaded",
        "first-pending",
        "first-pending",
        "second-pending",
        "third-pending"
    };
    const std::unordered_set<std::string> resident {
        "already-loaded"
    };

    const auto candidates = SelectAssetBrowserThumbnailTextureLoadCandidates(
        queued,
        resident,
        2u);

    ASSERT_EQ(candidates.size(), 2u);
    EXPECT_EQ(candidates[0], "first-pending");
    EXPECT_EQ(candidates[1], "second-pending");

    EXPECT_TRUE(SelectAssetBrowserThumbnailTextureLoadCandidates(
        queued,
        resident,
        0u).empty());
}

TEST(AssetBrowserPresentationTests, ThumbnailTextureDecodeCandidatesSkipResidentAndAlreadyDecoding)
{
    using namespace NLS::Editor::Assets;

    const std::vector<std::string> queued {
        "already-loaded",
        "first-pending",
        "currently-decoding",
        "second-pending",
        "first-pending",
        ""
    };
    const std::unordered_set<std::string> resident {
        "already-loaded"
    };
    const std::unordered_set<std::string> decoding {
        "currently-decoding"
    };

    const auto candidates = SelectAssetBrowserThumbnailTextureDecodeCandidates(
        queued,
        resident,
        decoding,
        2u);

    ASSERT_EQ(candidates.size(), 2u);
    EXPECT_EQ(candidates[0], "first-pending");
    EXPECT_EQ(candidates[1], "second-pending");

    EXPECT_TRUE(SelectAssetBrowserThumbnailTextureDecodeCandidates(
        queued,
        resident,
        decoding,
        0u).empty());
}

TEST(AssetBrowserPresentationTests, ThumbnailTextureDecodeCapacityAccountsForInFlightWork)
{
    using namespace NLS::Editor::Assets;

    EXPECT_EQ(AssetBrowserThumbnailTextureDecodeStartBudget(0u, 4u), 4u);
    EXPECT_EQ(AssetBrowserThumbnailTextureDecodeStartBudget(2u, 4u), 2u);
    EXPECT_EQ(AssetBrowserThumbnailTextureDecodeStartBudget(4u, 4u), 0u);
    EXPECT_EQ(AssetBrowserThumbnailTextureDecodeStartBudget(7u, 4u), 0u);
    EXPECT_EQ(AssetBrowserThumbnailTextureDecodeStartBudget(0u, 0u), 0u);
}

TEST(AssetBrowserPresentationTests, CachedThumbnailTextureSizeRejectsOversizedDecodedImages)
{
    using namespace NLS::Editor::Assets;

    EXPECT_TRUE(IsAssetBrowserCachedThumbnailTextureSizeAllowed(512u, 512u));
    EXPECT_TRUE(IsAssetBrowserCachedThumbnailTextureSizeAllowed(1u, 512u));
    EXPECT_FALSE(IsAssetBrowserCachedThumbnailTextureSizeAllowed(0u, 128u));
    EXPECT_FALSE(IsAssetBrowserCachedThumbnailTextureSizeAllowed(128u, 0u));
    EXPECT_FALSE(IsAssetBrowserCachedThumbnailTextureSizeAllowed(513u, 512u));
    EXPECT_FALSE(IsAssetBrowserCachedThumbnailTextureSizeAllowed(512u, 513u));
    EXPECT_FALSE(IsAssetBrowserCachedThumbnailTextureSizeAllowed(1u, 1u, 0u));
}

TEST(AssetBrowserPresentationTests, ThumbnailTexturePendingReleaseRunsAfterUsedFrameClears)
{
    using namespace NLS::Editor::Assets;

    std::unordered_set<std::string> usedThisFrame { "Library/AssetThumbnails/Hero.png" };
    std::unordered_set<std::string> pendingRelease {
        "Library/AssetThumbnails/Hero.png",
        "Library/AssetThumbnails/Old.png"
    };

    const auto plan = BeginAssetBrowserThumbnailTextureFrame(
        std::move(usedThisFrame),
        std::move(pendingRelease));

    EXPECT_TRUE(plan.usedThisFrame.empty());
    EXPECT_TRUE(plan.pendingRelease.empty());
    EXPECT_EQ(
        plan.releaseNow,
        (std::vector<std::string> {
            "Library/AssetThumbnails/Hero.png",
        "Library/AssetThumbnails/Old.png"
        }));
}

TEST(AssetBrowserPresentationTests, ThumbnailTextureFullClearDefersTexturesUsedThisFrame)
{
    using namespace NLS::Editor::Assets;

    const std::vector<std::string> resident {
        "Library/AssetThumbnails/Hero.png",
        "Library/AssetThumbnails/Free.png",
        "Library/AssetThumbnails/Hero.png"
    };
    const std::unordered_set<std::string> usedThisFrame {
        "Library/AssetThumbnails/Hero.png"
    };
    const std::unordered_set<std::string> pendingRelease {
        "Library/AssetThumbnails/AlreadyPending.png"
    };

    const auto plan = PlanAssetBrowserThumbnailTextureFullClear(
        resident,
        usedThisFrame,
        pendingRelease);

    ASSERT_EQ(plan.releaseNow.size(), 1u);
    EXPECT_EQ(plan.releaseNow[0], "Library/AssetThumbnails/Free.png");
    EXPECT_EQ(plan.pendingRelease.size(), 2u);
    EXPECT_NE(plan.pendingRelease.find("Library/AssetThumbnails/AlreadyPending.png"), plan.pendingRelease.end());
    EXPECT_NE(plan.pendingRelease.find("Library/AssetThumbnails/Hero.png"), plan.pendingRelease.end());
    EXPECT_EQ(plan.usedThisFrame.size(), 1u);
    EXPECT_NE(plan.usedThisFrame.find("Library/AssetThumbnails/Hero.png"), plan.usedThisFrame.end());
}

TEST(AssetBrowserPresentationTests, ObjectReferencePickerDirtyMarkDefersProviderRebuild)
{
    using namespace NLS::Editor::Assets;

    ObjectReferencePickerEntry entry;
    entry.displayName = "Assets/Hero.fbx / Mesh";
    std::strncpy(
        entry.payload.assetPath,
        "Assets/Hero.fbx",
        sizeof(entry.payload.assetPath) - 1u);

    size_t providerCalls = 0u;
    SetObjectReferencePickerEntries({});
    SetObjectReferencePickerEntriesProvider(
        [&providerCalls, entry]
        {
            ++providerCalls;
            return std::vector<ObjectReferencePickerEntry> { entry };
        });

    MarkObjectReferencePickerEntriesDirty();

    EXPECT_TRUE(ShouldDeferObjectReferencePickerEntriesRefresh());
    EXPECT_EQ(providerCalls, 0u);
    EXPECT_TRUE(GetObjectReferencePickerEntries().empty());
    EXPECT_EQ(providerCalls, 0u);

    ASSERT_TRUE(RefreshObjectReferencePickerEntries());
    EXPECT_EQ(providerCalls, 1u);
    EXPECT_FALSE(ShouldDeferObjectReferencePickerEntriesRefresh());

    SetObjectReferencePickerEntriesProvider({});
    SetObjectReferencePickerEntries({});
}

TEST(AssetBrowserPresentationTests, ItemTypeDisplayLabelsCoverAllFilterOptions)
{
    using namespace NLS::Editor::Assets;

    struct ExpectedTypeDescriptor
    {
        AssetBrowserItemType type;
        const char* label;
        AssetBrowserItemTypeColor color;
    };

    const std::array<ExpectedTypeDescriptor, kAssetBrowserItemTypeCount> cases {{
        { AssetBrowserItemType::All, "All", { 135u, 142u, 150u, 255u } },
        { AssetBrowserItemType::Folder, "Folder", { 224u, 170u, 72u, 255u } },
        { AssetBrowserItemType::Model, "Model", { 90u, 162u, 214u, 255u } },
        { AssetBrowserItemType::Prefab, "Prefab", { 114u, 175u, 112u, 255u } },
        { AssetBrowserItemType::Mesh, "Mesh", { 90u, 162u, 214u, 255u } },
        { AssetBrowserItemType::Material, "Material", { 196u, 132u, 92u, 255u } },
        { AssetBrowserItemType::Texture, "Texture", { 184u, 112u, 184u, 255u } },
        { AssetBrowserItemType::Shader, "Shader", { 125u, 158u, 190u, 255u } },
        { AssetBrowserItemType::Scene, "Scene", { 100u, 170u, 160u, 255u } },
        { AssetBrowserItemType::Script, "Script", { 170u, 170u, 170u, 255u } },
        { AssetBrowserItemType::Other, "Other", { 135u, 142u, 150u, 255u } }
    }};
    static_assert(cases.size() == kAssetBrowserItemTypeCount);

    const auto& filterOptions = AssetBrowserItemTypeFilterOptions();
    ASSERT_EQ(filterOptions.size(), cases.size());
    for (size_t index = 0u; index < cases.size(); ++index)
    {
        const auto& [type, label, color] = cases[index];
        EXPECT_EQ(type, filterOptions[index]);
        EXPECT_STREQ(label, AssetBrowserItemTypeDisplayLabel(type));
        const auto actualColor = AssetBrowserItemTypeDisplayColor(type);
        EXPECT_EQ(color.red, actualColor.red);
        EXPECT_EQ(color.green, actualColor.green);
        EXPECT_EQ(color.blue, actualColor.blue);
        EXPECT_EQ(color.alpha, actualColor.alpha);
    }
    EXPECT_STREQ("", AssetBrowserItemTypeDisplayLabel(AssetBrowserItemType::Count));
    const auto fallbackColor = AssetBrowserItemTypeDisplayColor(AssetBrowserItemType::Count);
    EXPECT_EQ(cases.back().color.red, fallbackColor.red);
    EXPECT_EQ(cases.back().color.green, fallbackColor.green);
    EXPECT_EQ(cases.back().color.blue, fallbackColor.blue);
    EXPECT_EQ(cases.back().color.alpha, fallbackColor.alpha);
}

TEST(AssetBrowserPresentationTests, WorkflowCapabilitiesExposePhysicalFolderAndSourceAssetActionsOnly)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserItem folder;
    folder.kind = AssetBrowserItemKind::Folder;
    folder.type = AssetBrowserItemType::Folder;
    folder.absolutePath = "C:/Project/Assets/Prefabs";
    const auto folderCapabilities = BuildAssetBrowserWorkflowCapabilities(folder);
    EXPECT_TRUE(folderCapabilities.canShowInExplorer);
    EXPECT_TRUE(folderCapabilities.canImportHere);
    EXPECT_TRUE(folderCapabilities.canCreateChildren);
    EXPECT_TRUE(folderCapabilities.canRename);
    EXPECT_TRUE(folderCapabilities.canDelete);
    EXPECT_TRUE(folderCapabilities.canAcceptAssetDrops);
    EXPECT_TRUE(folderCapabilities.canAcceptHierarchyDrops);
    EXPECT_FALSE(folderCapabilities.canDuplicate);
    EXPECT_FALSE(folderCapabilities.canReimport);

    AssetBrowserItem model;
    model.kind = AssetBrowserItemKind::SourceAsset;
    model.type = AssetBrowserItemType::Model;
    model.absolutePath = "C:/Project/Assets/Models/Hero.fbx";
    model.selectionResourcePath = "Assets/Models/Hero.fbx";
    model.previewableInAssetView = true;
    const auto modelCapabilities = BuildAssetBrowserWorkflowCapabilities(model);
    EXPECT_TRUE(modelCapabilities.canOpenExternal);
    EXPECT_TRUE(modelCapabilities.canRename);
    EXPECT_TRUE(modelCapabilities.canDelete);
    EXPECT_TRUE(modelCapabilities.canDuplicate);
    EXPECT_TRUE(modelCapabilities.canOpenProperties);
    EXPECT_TRUE(modelCapabilities.canPreview);
    EXPECT_TRUE(modelCapabilities.canReimport);
    EXPECT_FALSE(modelCapabilities.canAcceptAssetDrops);
    EXPECT_FALSE(modelCapabilities.canAcceptHierarchyDrops);

    AssetBrowserItem generatedPrefab;
    generatedPrefab.kind = AssetBrowserItemKind::GeneratedSubAsset;
    generatedPrefab.type = AssetBrowserItemType::Prefab;
    generatedPrefab.sourceAssetPath = "Assets/Models/Hero.fbx";
    generatedPrefab.selectionResourcePath = "Assets/Models/Hero.fbx#prefab:Hero";
    generatedPrefab.generatedReadOnly = true;
    const auto generatedCapabilities = BuildAssetBrowserWorkflowCapabilities(generatedPrefab);
    EXPECT_TRUE(generatedCapabilities.canOpenProperties);
    EXPECT_TRUE(generatedCapabilities.canEdit);
    EXPECT_FALSE(generatedCapabilities.canRename);
    EXPECT_FALSE(generatedCapabilities.canDelete);
    EXPECT_FALSE(generatedCapabilities.canDuplicate);
    EXPECT_FALSE(generatedCapabilities.canReimport);
    EXPECT_FALSE(generatedCapabilities.canAcceptAssetDrops);
    EXPECT_FALSE(generatedCapabilities.canAcceptHierarchyDrops);

    AssetBrowserItem generatedMaterial;
    generatedMaterial.kind = AssetBrowserItemKind::GeneratedSubAsset;
    generatedMaterial.type = AssetBrowserItemType::Material;
    generatedMaterial.sourceAssetPath = "Assets/Models/Hero.fbx";
    generatedMaterial.selectionResourcePath = "Assets/Models/Hero.fbx#material:Hero";
    generatedMaterial.generatedReadOnly = true;
    const auto generatedMaterialCapabilities = BuildAssetBrowserWorkflowCapabilities(generatedMaterial);
    EXPECT_TRUE(generatedMaterialCapabilities.canOpenProperties);
    EXPECT_FALSE(generatedMaterialCapabilities.canEdit);
    EXPECT_FALSE(generatedMaterialCapabilities.canRename);
    EXPECT_FALSE(generatedMaterialCapabilities.canDelete);
}

TEST(AssetBrowserPresentationTests, RefreshPlanKeepsFilterAndFolderSelectionLocal)
{
    using namespace NLS::Editor::Assets;

    const auto initial = BuildAssetBrowserRefreshPlan(AssetBrowserRefreshReason::InitialBuild);
    EXPECT_TRUE(initial.refreshAssetDatabase);
    EXPECT_TRUE(initial.rebuildFolderTree);

    const auto mutation = BuildAssetBrowserRefreshPlan(AssetBrowserRefreshReason::AssetDatabaseMutation);
    EXPECT_TRUE(mutation.refreshAssetDatabase);
    EXPECT_TRUE(mutation.rebuildFolderTree);

    const auto databaseReady = BuildAssetBrowserRefreshPlan(AssetBrowserRefreshReason::AssetDatabaseReady);
    EXPECT_FALSE(databaseReady.refreshAssetDatabase);
    EXPECT_FALSE(databaseReady.rebuildFolderTree);
    EXPECT_TRUE(databaseReady.rebuildCurrentFolderItems);

    const auto folderSelection = BuildAssetBrowserRefreshPlan(AssetBrowserRefreshReason::FolderSelection);
    EXPECT_FALSE(folderSelection.refreshAssetDatabase);
    EXPECT_TRUE(folderSelection.rebuildFolderTree);

    const auto filterChange = BuildAssetBrowserRefreshPlan(AssetBrowserRefreshReason::FilterChange);
    EXPECT_FALSE(filterChange.refreshAssetDatabase);
    EXPECT_FALSE(filterChange.rebuildFolderTree);
    EXPECT_FALSE(filterChange.rebuildCurrentFolderItems);

    EXPECT_TRUE(initial.rebuildCurrentFolderItems);
    EXPECT_TRUE(mutation.rebuildCurrentFolderItems);
    EXPECT_TRUE(folderSelection.rebuildCurrentFolderItems);
}

TEST(AssetBrowserPresentationTests, RefreshSchedulingQueuesExplicitRefreshBehindInFlightSameRoot)
{
    using namespace NLS::Editor::Assets;

    const auto initial = PlanAssetDatabaseRefreshScheduling(
        false,
        false,
        false,
        false);
    EXPECT_TRUE(initial.startRefresh);
    EXPECT_FALSE(initial.queueRefreshAfterInFlight);

    const auto alreadyLoading = PlanAssetDatabaseRefreshScheduling(
        false,
        false,
        false,
        true);
    EXPECT_FALSE(alreadyLoading.startRefresh);
    EXPECT_FALSE(alreadyLoading.queueRefreshAfterInFlight);

    const auto mutationDuringLoading = PlanAssetDatabaseRefreshScheduling(
        false,
        true,
        false,
        true);
    EXPECT_FALSE(mutationDuringLoading.startRefresh);
    EXPECT_TRUE(mutationDuringLoading.queueRefreshAfterInFlight);

    const auto noWork = PlanAssetDatabaseRefreshScheduling(
        false,
        false,
        true,
        false);
    EXPECT_FALSE(noWork.startRefresh);
    EXPECT_FALSE(noWork.queueRefreshAfterInFlight);
}

TEST(AssetBrowserPresentationTests, InFlightDatabaseRefreshDiscardIsRetiredUntilReady)
{
    using namespace NLS::Editor::Assets;

    EXPECT_EQ(
        PlanAssetDatabaseRefreshDiscardAction(false, false),
        AssetDatabaseRefreshDiscardAction::Drop);
    EXPECT_EQ(
        PlanAssetDatabaseRefreshDiscardAction(true, true),
        AssetDatabaseRefreshDiscardAction::Drop);
    EXPECT_EQ(
        PlanAssetDatabaseRefreshDiscardAction(true, false),
        AssetDatabaseRefreshDiscardAction::Retire);
}

TEST(AssetBrowserPresentationTests, SkipsGeneratedSubAssetsWhenManifestIsStale)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto modelId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Hero.gltf"));
    ArtifactManifest manifest;
    manifest.sourceAssetId = modelId;
    manifest.importerId = "scene-model";
    manifest.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "prefab:Hero";
    manifest.subAssets.push_back(MakeArtifact(modelId, "prefab:Hero", ArtifactType::Prefab, "prefab"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "mesh:Body", ArtifactType::Mesh, "mesh"));
    WriteManifestArtifactFiles(root, manifest);
    AddCurrentSourceDependencies(root, manifest, "Assets/Models/Hero.gltf");
    database.AddArtifactManifest(manifest);
    ASSERT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/Hero.gltf"));

    AssetBrowserBuildOptions options;
    options.includeGeneratedSubAssets = true;
    const auto freshItems = BuildCurrentFolderAssetItems(root, "Assets/Models", &database, options);
    ASSERT_NE(FindGeneratedSubAssetItem(freshItems, "prefab:Hero"), nullptr);
    ASSERT_NE(FindGeneratedSubAssetItem(freshItems, "mesh:Body"), nullptr);

    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"},"nodes":[]})");
    ASSERT_FALSE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/Hero.gltf"));

    const auto staleItems = BuildCurrentFolderAssetItems(root, "Assets/Models", &database, options);
    ASSERT_NE(FindItem(staleItems, "Hero.gltf"), nullptr);
    EXPECT_EQ(FindGeneratedSubAssetItem(staleItems, "prefab:Hero"), nullptr);
    EXPECT_EQ(FindGeneratedSubAssetItem(staleItems, "mesh:Body"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, FastGeneratedSubAssetBuildUsesKnownCurrentManifestSnapshot)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto modelId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Hero.gltf"));
    ArtifactManifest manifest;
    manifest.sourceAssetId = modelId;
    manifest.importerId = "scene-model";
    manifest.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "prefab:Hero";
    manifest.subAssets.push_back(MakeArtifact(modelId, "prefab:Hero", ArtifactType::Prefab, "prefab"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "mesh:Body", ArtifactType::Mesh, "mesh"));
    WriteManifestArtifactFiles(root, manifest);
    AddCurrentSourceDependencies(root, manifest, "Assets/Models/Hero.gltf");
    database.AddArtifactManifest(manifest);
    ASSERT_TRUE(database.IsArtifactManifestKnownCurrentForAssetPath("Assets/Models/Hero.gltf"));

    AssetBrowserBuildOptions options;
    options.includeGeneratedSubAssets = true;
    options.verifyGeneratedSubAssetManifests = false;
    const auto freshItems = BuildCurrentFolderAssetItems(root, "Assets/Models", &database, options);

    EXPECT_NE(FindGeneratedSubAssetItem(freshItems, "prefab:Hero"), nullptr);
    EXPECT_NE(FindGeneratedSubAssetItem(freshItems, "mesh:Body"), nullptr);

    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"},"nodes":[]})");
    ASSERT_FALSE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/Hero.gltf"));
    database.AddArtifactManifest(manifest);
    ASSERT_FALSE(database.IsArtifactManifestKnownCurrentForAssetPath("Assets/Models/Hero.gltf"));
    const auto items = BuildCurrentFolderAssetItems(root, "Assets/Models", &database, options);

    EXPECT_NE(FindItem(items, "Hero.gltf"), nullptr);
    EXPECT_EQ(FindGeneratedSubAssetItem(items, "prefab:Hero"), nullptr);
    EXPECT_EQ(FindGeneratedSubAssetItem(items, "mesh:Body"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, RestartedFastGeneratedSubAssetBuildRejectsStalePersistedManifestSnapshot)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto modelId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Hero.gltf"));
    ArtifactManifest manifest;
    manifest.sourceAssetId = modelId;
    manifest.importerId = "scene-model";
    manifest.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "prefab:Hero";
    manifest.subAssets.push_back(MakeArtifact(modelId, "prefab:Hero", ArtifactType::Prefab, "prefab"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "mesh:Body", ArtifactType::Mesh, "mesh"));
    WriteManifestArtifactFiles(root, manifest);
    AddCurrentSourceDependencies(root, manifest, "Assets/Models/Hero.gltf");
    WritePersistedArtifactManifest(root, manifest);
    database.AddArtifactManifest(manifest);
    ASSERT_TRUE(database.IsArtifactManifestKnownCurrentForAssetPath("Assets/Models/Hero.gltf"));

    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"},"nodes":[]})");

    AssetDatabaseFacade restartedDatabase({root});
    ASSERT_TRUE(restartedDatabase.Refresh());
    EXPECT_FALSE(restartedDatabase.IsArtifactManifestCurrentForAssetPath("Assets/Models/Hero.gltf"));
    EXPECT_FALSE(restartedDatabase.IsArtifactManifestKnownCurrentForAssetPath("Assets/Models/Hero.gltf"));

    AssetBrowserBuildOptions options;
    options.includeGeneratedSubAssets = true;
    options.verifyGeneratedSubAssetManifests = false;
    const auto items = BuildCurrentFolderAssetItems(root, "Assets/Models", &restartedDatabase, options);

    EXPECT_NE(FindItem(items, "Hero.gltf"), nullptr);
    EXPECT_EQ(FindGeneratedSubAssetItem(items, "prefab:Hero"), nullptr);
    EXPECT_EQ(FindGeneratedSubAssetItem(items, "mesh:Body"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, SourceModelCardKeepsFileTypeWhenPrimaryArtifactIsPrefab)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto modelId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Hero.gltf"));
    ArtifactManifest manifest;
    manifest.sourceAssetId = modelId;
    manifest.importerId = "scene-model";
    manifest.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "prefab:Hero";
    manifest.subAssets.push_back(MakeArtifact(modelId, "prefab:Hero", ArtifactType::Prefab, "prefab"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "mesh:Body", ArtifactType::Mesh, "mesh"));
    WriteManifestArtifactFiles(root, manifest);
    AddCurrentSourceDependencies(root, manifest, "Assets/Models/Hero.gltf");
    database.AddArtifactManifest(manifest);

    AssetBrowserBuildOptions options;
    options.includeGeneratedSubAssets = true;
    const auto items = BuildCurrentFolderAssetItems(root, "Assets/Models", &database, options);

    const auto* source = FindItem(items, "Hero.gltf");
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->kind, AssetBrowserItemKind::SourceAsset);
    EXPECT_EQ(source->type, AssetBrowserItemType::Model);
    EXPECT_EQ(source->artifactType, ArtifactType::Prefab);
    EXPECT_EQ(source->subAssetKey, "prefab:Hero");

    const auto* generatedPrefab = FindGeneratedSubAssetItem(items, "prefab:Hero");
    ASSERT_NE(generatedPrefab, nullptr);
    EXPECT_EQ(generatedPrefab->kind, AssetBrowserItemKind::GeneratedSubAsset);
    EXPECT_EQ(generatedPrefab->type, AssetBrowserItemType::Prefab);

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, GeneratedSubAssetItemsPreserveEditorDragPayloadMetadata)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
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
    manifest.subAssets.push_back(MakeArtifact(modelId, "prefab:Hero", ArtifactType::Prefab, "prefab"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "mesh:Body", ArtifactType::Mesh, "mesh"));
    WriteManifestArtifactFiles(root, manifest);
    AddCurrentSourceDependencies(root, manifest, "Assets/Models/Hero.gltf");
    database.AddArtifactManifest(manifest);

    AssetBrowserBuildOptions options;
    options.includeGeneratedSubAssets = true;
    const auto items = BuildCurrentFolderAssetItems(root, "Assets/Models", &database, options);

    const auto* prefab = FindGeneratedSubAssetItem(items, "prefab:Hero");
    ASSERT_NE(prefab, nullptr);
    const auto prefabPayload = MakeAssetBrowserItemDragPayload(*prefab, &database);
    ASSERT_TRUE(prefabPayload.has_value());
    EXPECT_EQ(GetEditorAssetDragPayloadPath(*prefabPayload), "Assets/Models/Hero.gltf");
    EXPECT_EQ(GetEditorAssetDragPayloadAssetId(*prefabPayload), modelId);
    EXPECT_EQ(GetEditorAssetDragPayloadSubAssetKey(*prefabPayload), "prefab:Hero");
    EXPECT_EQ(GetEditorAssetDragPayloadArtifactType(*prefabPayload), ArtifactType::Prefab);
    EXPECT_EQ(prefabPayload->generatedModelPrefab, 1u);
    EXPECT_EQ(prefabPayload->imported, 1u);
    EXPECT_EQ(prefabPayload->previewPrefabReady, 1u);
    EXPECT_TRUE(IsEditorAssetDragPayloadGeneratedBrowserSubAsset(*prefabPayload));
    EXPECT_FALSE(CanMoveEditorAssetDragPayloadAsPhysicalProjectFile(*prefabPayload));

    const auto* mesh = FindGeneratedSubAssetItem(items, "mesh:Body");
    ASSERT_NE(mesh, nullptr);
    const auto meshPayload = MakeAssetBrowserItemDragPayload(*mesh, &database);
    ASSERT_TRUE(meshPayload.has_value());
    EXPECT_EQ(GetEditorAssetDragPayloadPath(*meshPayload), "Assets/Models/Hero.gltf");
    EXPECT_EQ(GetEditorAssetDragPayloadSubAssetKey(*meshPayload), "mesh:Body");
    EXPECT_EQ(GetEditorAssetDragPayloadArtifactType(*meshPayload), ArtifactType::Mesh);
    EXPECT_EQ(meshPayload->generatedModelPrefab, 0u);
    EXPECT_EQ(meshPayload->imported, 1u);
    EXPECT_EQ(meshPayload->previewPrefabReady, 0u);
    EXPECT_TRUE(IsEditorAssetDragPayloadGeneratedBrowserSubAsset(*meshPayload));
    EXPECT_FALSE(CanMoveEditorAssetDragPayloadAsPhysicalProjectFile(*meshPayload));

    const auto sourceFilePayload = MakeEditorAssetDragPayload(
        "Assets/Models/Hero.gltf",
        modelId,
        "prefab:Hero",
        ArtifactType::Prefab,
        true,
        true,
        true);
    EXPECT_FALSE(IsEditorAssetDragPayloadGeneratedBrowserSubAsset(sourceFilePayload));
    EXPECT_TRUE(CanMoveEditorAssetDragPayloadAsPhysicalProjectFile(sourceFilePayload));

    const auto* source = FindItem(items, "Hero.gltf");
    ASSERT_NE(source, nullptr);
    EXPECT_FALSE(MakeAssetBrowserItemDragPayload(*source, nullptr).has_value());

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, RejectsGeneratedDragPayloadWhenCachedItemManifestBecomesStale)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto modelId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Hero.gltf"));
    ArtifactManifest manifest;
    manifest.sourceAssetId = modelId;
    manifest.importerId = "scene-model";
    manifest.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "prefab:Hero";
    manifest.subAssets.push_back(MakeArtifact(modelId, "prefab:Hero", ArtifactType::Prefab, "prefab"));
    WriteManifestArtifactFiles(root, manifest);
    AddCurrentSourceDependencies(root, manifest, "Assets/Models/Hero.gltf");
    database.AddArtifactManifest(manifest);

    AssetBrowserBuildOptions options;
    options.includeGeneratedSubAssets = true;
    const auto items = BuildCurrentFolderAssetItems(root, "Assets/Models", &database, options);
    const auto* cachedPrefab = FindGeneratedSubAssetItem(items, "prefab:Hero");
    ASSERT_NE(cachedPrefab, nullptr);
    ASSERT_TRUE(MakeAssetBrowserItemDragPayload(*cachedPrefab, &database).has_value());

    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"},"nodes":[]})");
    ASSERT_FALSE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/Hero.gltf"));

    EXPECT_FALSE(MakeAssetBrowserItemDragPayload(*cachedPrefab, &database).has_value());

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, SkipsSubAssetPickerEntriesWhenManifestIsStale)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
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
    manifest.subAssets.push_back(MakeArtifact(modelId, "mesh:Body", ArtifactType::Mesh, "mesh"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "material:Body", ArtifactType::Material, "material"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "texture:Albedo", ArtifactType::Texture, "texture"));
    WriteManifestArtifactFiles(root, manifest);
    AddCurrentSourceDependencies(root, manifest, "Assets/Models/Hero.gltf");
    database.AddArtifactManifest(manifest);

    ASSERT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/Hero.gltf"));
    ASSERT_EQ(BuildAssetBrowserSubAssetEntries(database, "Assets/Models/Hero.gltf").size(), 3u);
    const auto pickerEntries = BuildObjectReferencePickerEntries(database);
    ASSERT_EQ(pickerEntries.size(), 3u);
    for (const auto& entry : pickerEntries)
    {
        EXPECT_TRUE(IsEditorAssetDragPayloadGeneratedBrowserSubAsset(entry.payload));
        EXPECT_FALSE(CanMoveEditorAssetDragPayloadAsPhysicalProjectFile(entry.payload));
    }

    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"},"nodes":[]})");
    ASSERT_FALSE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/Hero.gltf"));

    EXPECT_TRUE(BuildAssetBrowserSubAssetEntries(database, "Assets/Models/Hero.gltf").empty());
    EXPECT_TRUE(BuildObjectReferencePickerEntries(database).empty());

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, ObjectReferencePickerPrunesStaleSnapshotWithoutPriorManifestCheck)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
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
    manifest.subAssets.push_back(MakeArtifact(modelId, "mesh:Body", ArtifactType::Mesh, "mesh"));
    WriteManifestArtifactFiles(root, manifest);
    AddCurrentSourceDependencies(root, manifest, "Assets/Models/Hero.gltf");
    database.AddArtifactManifest(manifest);

    ASSERT_EQ(BuildObjectReferencePickerEntries(database).size(), 1u);

    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"},"nodes":[]})");

    EXPECT_TRUE(BuildObjectReferencePickerEntries(database).empty());
    EXPECT_TRUE(database.GetObjectReferencePickerAssetSnapshots().empty());

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, ObjectReferencePickerBuildsFromKnownCurrentManifestSnapshot)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    WriteTextFile(root / "Assets" / "Models" / "Fresh.gltf", R"({"asset":{"version":"2.0"}})");
    WriteTextFile(root / "Assets" / "Models" / "Stale.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto freshId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Fresh.gltf"));
    ArtifactManifest freshManifest;
    freshManifest.sourceAssetId = freshId;
    freshManifest.importerId = "scene-model";
    freshManifest.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
    freshManifest.targetPlatform = "editor";
    freshManifest.primarySubAssetKey = "model:Fresh";
    freshManifest.subAssets.push_back(MakeArtifact(freshId, "mesh:FreshBody", ArtifactType::Mesh, "mesh"));
    WriteManifestArtifactFiles(root, freshManifest);
    AddCurrentSourceDependencies(root, freshManifest, "Assets/Models/Fresh.gltf");
    database.AddArtifactManifest(freshManifest);

    const auto staleId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Stale.gltf"));
    ArtifactManifest staleManifest;
    staleManifest.sourceAssetId = staleId;
    staleManifest.importerId = "scene-model";
    staleManifest.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
    staleManifest.targetPlatform = "editor";
    staleManifest.primarySubAssetKey = "model:Stale";
    staleManifest.subAssets.push_back(MakeArtifact(staleId, "mesh:StaleBody", ArtifactType::Mesh, "mesh"));
    WriteManifestArtifactFiles(root, staleManifest);
    AddCurrentSourceDependencies(root, staleManifest, "Assets/Models/Stale.gltf");
    WriteTextFile(root / "Assets" / "Models" / "Stale.gltf", R"({"asset":{"version":"2.0"},"nodes":[]})");
    database.AddArtifactManifest(staleManifest);

    const auto knownCurrent = database.GetKnownCurrentArtifactManifestAssetPaths();
    EXPECT_NE(
        std::find(knownCurrent.begin(), knownCurrent.end(), "Assets/Models/Fresh.gltf"),
        knownCurrent.end());
    EXPECT_EQ(
        std::find(knownCurrent.begin(), knownCurrent.end(), "Assets/Models/Stale.gltf"),
        knownCurrent.end());

    const auto snapshots = database.GetObjectReferencePickerAssetSnapshots();
    ASSERT_EQ(snapshots.size(), 1u);
    EXPECT_EQ(snapshots[0].sourceAssetPath, "Assets/Models/Fresh.gltf");
    EXPECT_EQ(snapshots[0].assetId, freshId);
    ASSERT_EQ(snapshots[0].subAssets.size(), 1u);
    EXPECT_EQ(snapshots[0].subAssets[0].subAssetKey, "mesh:FreshBody");
    EXPECT_EQ(snapshots[0].subAssets[0].artifactType, ArtifactType::Mesh);

    const auto pickerEntries = BuildObjectReferencePickerEntries(database);
    ASSERT_EQ(pickerEntries.size(), 1u);
    EXPECT_NE(pickerEntries[0].displayName.find("Fresh.gltf"), std::string::npos);
    EXPECT_NE(pickerEntries[0].displayName.find("mesh:FreshBody"), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, ObjectReferencePickerBuildsFromSnapshotRecordsWithoutDatabaseScan)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto meshAssetId = ParseAssetId("a1010101-0101-4101-8101-010101010101");
    ObjectReferencePickerAssetSnapshot snapshot;
    snapshot.sourceAssetPath = "Assets/Models/Hero.fbx";
    snapshot.assetId = meshAssetId;
    snapshot.subAssets.push_back({
        "Mesh:Body",
        "Library/Artifacts/a1010101-0101-4101-8101-010101010101/meshes/body.nmesh",
        ArtifactType::Mesh
    });
    snapshot.subAssets.push_back({
        "Texture:Body",
        "Library/Artifacts/a1010101-0101-4101-8101-010101010101/textures/body.ntex",
        ArtifactType::Texture
    });
    snapshot.subAssets.push_back({
        "Audio:Skip",
        "Library/Artifacts/a1010101-0101-4101-8101-010101010101/audio/skip.wav",
        ArtifactType::Audio
    });

    const auto entries = BuildObjectReferencePickerEntriesFromSnapshots({ snapshot });

    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].displayName, "Assets/Models/Hero.fbx / Mesh:Body");
    EXPECT_EQ(GetEditorAssetDragPayloadPath(entries[0].payload), "Assets/Models/Hero.fbx");
    EXPECT_EQ(GetEditorAssetDragPayloadSubAssetKey(entries[0].payload), "Mesh:Body");
    EXPECT_EQ(GetEditorAssetDragPayloadArtifactType(entries[0].payload), ArtifactType::Mesh);
    EXPECT_TRUE(IsEditorAssetDragPayloadGeneratedBrowserSubAsset(entries[0].payload));
    EXPECT_EQ(entries[1].displayName, "Assets/Models/Hero.fbx / Texture:Body");
}

TEST(AssetBrowserPresentationTests, ObjectReferencePickerReadsSnapshotWithoutRebuildingOnOpen)
{
    using namespace NLS::Editor::Assets;

    ObjectReferencePickerEntry entry;
    entry.displayName = "Assets/Models/Hero.gltf / mesh:Body";

    int buildCount = 0;
    SetObjectReferencePickerEntries({});
    SetObjectReferencePickerEntriesProvider(
        [&buildCount, entry]()
        {
            ++buildCount;
            return std::vector<ObjectReferencePickerEntry> { entry };
        });

    MarkObjectReferencePickerEntriesDirty();
    EXPECT_EQ(buildCount, 0);

    EXPECT_TRUE(GetObjectReferencePickerEntries().empty());
    EXPECT_EQ(buildCount, 0);

    ASSERT_TRUE(RefreshObjectReferencePickerEntries());
    const auto firstEntries = GetObjectReferencePickerEntries();
    ASSERT_EQ(firstEntries.size(), 1u);
    EXPECT_EQ(firstEntries[0].displayName, entry.displayName);
    EXPECT_EQ(buildCount, 1);

    const auto cachedEntries = GetObjectReferencePickerEntries();
    ASSERT_EQ(cachedEntries.size(), 1u);
    EXPECT_EQ(buildCount, 1);

    MarkObjectReferencePickerEntriesDirty();
    EXPECT_EQ(buildCount, 1);
    const auto staleEntries = GetObjectReferencePickerEntries();
    EXPECT_TRUE(staleEntries.empty());
    EXPECT_EQ(buildCount, 1);

    ASSERT_TRUE(RefreshObjectReferencePickerEntries());
    const auto rebuiltEntries = GetObjectReferencePickerEntries();
    ASSERT_EQ(rebuiltEntries.size(), 1u);
    EXPECT_EQ(buildCount, 2);

    SetObjectReferencePickerEntriesProvider({});
    SetObjectReferencePickerEntries({});
}

TEST(AssetBrowserPresentationTests, BuildsBreadcrumbAndFallsBackToNearestExistingFolder)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    std::filesystem::create_directories(root / "Assets" / "Models");

    const auto breadcrumb = BuildAssetBrowserBreadcrumb("Assets/Models/Sponza");

    ASSERT_EQ(breadcrumb.size(), 3u);
    EXPECT_EQ(breadcrumb[0].displayName, "Assets");
    EXPECT_EQ(breadcrumb[0].projectRelativePath, "Assets");
    EXPECT_EQ(breadcrumb[1].displayName, "Models");
    EXPECT_EQ(breadcrumb[1].projectRelativePath, "Assets/Models");
    EXPECT_EQ(breadcrumb[2].displayName, "Sponza");
    EXPECT_EQ(breadcrumb[2].projectRelativePath, "Assets/Models/Sponza");

    const auto resolved = ResolveAssetBrowserFolderSelection(root, "Assets/Models/Sponza/Deleted");

    EXPECT_TRUE(resolved.exists);
    EXPECT_EQ(resolved.projectRelativePath, "Assets/Models");
    EXPECT_EQ(resolved.absolutePath.lexically_normal(), (root / "Assets" / "Models").lexically_normal());

    std::filesystem::remove_all(root);
}
