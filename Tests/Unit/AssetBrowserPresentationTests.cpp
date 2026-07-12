#include <gtest/gtest.h>

#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/AssetMeta.h"
#include "Assets/AssetBrowserPresentation.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/AssetThumbnailService.h"
#include "Assets/ExternalAssetImporter.h"
#include "Assets/NativeArtifactContainer.h"
#include "Assets/PrefabEditorWorkflow.h"
#include "Core/EditorResources.h"
#include "GameObject.h"
#include "Guid.h"
#include "ImGui/imgui_internal.h"
#include "Widgets/Layout/Group.h"
#include "Panels/AssetBrowser.h"
#include "Utils/PathParser.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

uint32_t AssetBrowserThumbnailRequestSize(float thumbnailSize);
void DrawAssetBrowserSegmentPanel(
    ImDrawList* drawList,
    const ImVec2& min,
    const ImVec2& max,
    bool hovered,
    ImDrawFlags cornerFlags);

namespace NLS::Editor::Core::Testing
{
std::string BuildThumbnailTelemetrySummaryReportForTesting(
    const std::vector<NLS::Core::Assets::ArtifactLoadTelemetryRecord>& records);
}

void AssetBrowserRasterTestCallback(const ImDrawList*, const ImDrawCmd*)
{
}

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

std::string StableArtifactBlobFileName(
    const NLS::Core::Assets::AssetId owner,
    const std::string& subAssetKey)
{
    return NLS::Core::Assets::BuildArtifactStorageFileName(owner.ToString() + ":" + subAssetKey);
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

std::filesystem::path RepoPath(std::string_view relativePath)
{
    return std::filesystem::path(NLS_ROOT_DIR) / std::filesystem::path(relativePath);
}

void InitializeDrawListSharedData(ImDrawListSharedData& sharedData)
{
    sharedData.CurveTessellationTol = 1.25f;
    sharedData.SetCircleTessellationMaxError(0.30f);
}

std::string ReadSourceText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    EXPECT_TRUE(input.is_open()) << "Failed to open source file: " << path.string();
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

std::string ExtractFunctionBody(const std::string& source, std::string_view functionNeedle)
{
    const auto begin = source.find(functionNeedle);
    EXPECT_NE(begin, std::string::npos) << "Missing function: " << functionNeedle;
    if (begin == std::string::npos)
        return {};

    const auto bodyBegin = source.find('{', begin);
    EXPECT_NE(bodyBegin, std::string::npos) << "Missing function body: " << functionNeedle;
    if (bodyBegin == std::string::npos)
        return {};

    size_t depth = 0u;
    for (size_t offset = bodyBegin; offset < source.size(); ++offset)
    {
        if (source[offset] == '{')
        {
            ++depth;
        }
        else if (source[offset] == '}')
        {
            --depth;
            if (depth == 0u)
                return source.substr(bodyBegin, offset - bodyBegin + 1u);
        }
    }

    ADD_FAILURE() << "Unterminated function body: " << functionNeedle;
    return {};
}

std::vector<std::uint8_t> RasterizeDrawListAlpha(
    const ImDrawList& drawList,
    const int width,
    const int height,
    const ImVec2 displayPos,
    const ImVec2 framebufferScale)
{
    const auto finitePoint = [](const ImVec2& point)
    {
        return std::isfinite(point.x) && std::isfinite(point.y);
    };
    if (width <= 0 || height <= 0 ||
        !finitePoint(displayPos) ||
        !finitePoint(framebufferScale) ||
        framebufferScale.x <= 0.0f || framebufferScale.y <= 0.0f)
    {
        throw std::invalid_argument("invalid raster target or display transform");
    }
    if (static_cast<size_t>(width) > (std::numeric_limits<size_t>::max)() / static_cast<size_t>(height))
        throw std::overflow_error("raster target size overflow");

    std::vector<std::uint8_t> alpha(
        static_cast<size_t>(width) * static_cast<size_t>(height),
        0u);
    const auto edge = [](const ImVec2& a, const ImVec2& b, const ImVec2& point)
    {
        return (b.x - a.x) * (point.y - a.y) -
            (b.y - a.y) * (point.x - a.x);
    };
    const auto topLeft = [](const ImVec2& a, const ImVec2& b)
    {
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        return dy < 0.0f || (dy == 0.0f && dx > 0.0f);
    };
    const auto transformPoint = [displayPos, framebufferScale](const ImVec2& point)
    {
        return ImVec2(
            (point.x - displayPos.x) * framebufferScale.x,
            (point.y - displayPos.y) * framebufferScale.y);
    };

    for (const auto& command : drawList.CmdBuffer)
    {
        if (command.UserCallback != nullptr)
            throw std::runtime_error("draw callbacks are unsupported by the deterministic rasterizer");
        if (command.ElemCount == 0u)
            continue;
        if (command.ElemCount % 3u != 0u)
            throw std::runtime_error("draw command element count is not triangular");
        if (command.IdxOffset > static_cast<unsigned int>(drawList.IdxBuffer.Size) ||
            command.ElemCount > static_cast<unsigned int>(drawList.IdxBuffer.Size) - command.IdxOffset)
        {
            throw std::out_of_range("draw command index range is invalid");
        }

        const ImVec2 clipMin = transformPoint(ImVec2(command.ClipRect.x, command.ClipRect.y));
        const ImVec2 clipMax = transformPoint(ImVec2(command.ClipRect.z, command.ClipRect.w));
        if (!finitePoint(clipMin) || !finitePoint(clipMax) ||
            clipMax.x < clipMin.x || clipMax.y < clipMin.y)
        {
            throw std::runtime_error("draw command clip rectangle is invalid");
        }
        const int minX = static_cast<int>(std::floor((std::clamp)(clipMin.x, 0.0f, static_cast<float>(width))));
        const int minY = static_cast<int>(std::floor((std::clamp)(clipMin.y, 0.0f, static_cast<float>(height))));
        const int maxX = static_cast<int>(std::ceil((std::clamp)(clipMax.x, 0.0f, static_cast<float>(width))));
        const int maxY = static_cast<int>(std::ceil((std::clamp)(clipMax.y, 0.0f, static_cast<float>(height))));

        for (unsigned int element = 0u; element < command.ElemCount; element += 3u)
        {
            const auto vertex = [&](const unsigned int triangleElement)
            {
                const auto index = drawList.IdxBuffer[
                    static_cast<int>(command.IdxOffset + element + triangleElement)];
                if (command.VtxOffset > (std::numeric_limits<unsigned int>::max)() - static_cast<unsigned int>(index))
                    throw std::out_of_range("draw command vertex offset overflows");
                const auto vertexIndex = static_cast<unsigned int>(index) + command.VtxOffset;
                if (vertexIndex >= static_cast<unsigned int>(drawList.VtxBuffer.Size))
                    throw std::out_of_range("draw command vertex index is invalid");
                const auto& source = drawList.VtxBuffer[static_cast<int>(vertexIndex)];
                const ImVec2 position = transformPoint(source.pos);
                if (!finitePoint(position))
                    throw std::runtime_error("draw vertex position is invalid");
                return std::pair<ImVec2, float> {
                    position,
                    static_cast<float>((source.col >> IM_COL32_A_SHIFT) & 0xffu)
                };
            };
            auto [a, alphaA] = vertex(0u);
            auto [b, alphaB] = vertex(1u);
            auto [c, alphaC] = vertex(2u);
            float area = edge(a, b, c);
            if (!std::isfinite(area) || area == 0.0f)
                throw std::runtime_error("draw triangle geometry is invalid");
            if (area < 0.0f)
            {
                std::swap(b, c);
                std::swap(alphaB, alphaC);
                area = -area;
            }

            for (int y = minY; y < maxY; ++y)
            {
                for (int x = minX; x < maxX; ++x)
                {
                    const ImVec2 sample(
                        static_cast<float>(x) + 0.5f,
                        static_cast<float>(y) + 0.5f);
                    if (sample.x < clipMin.x || sample.y < clipMin.y ||
                        sample.x >= clipMax.x || sample.y >= clipMax.y)
                    {
                        continue;
                    }
                    const float edgeA = edge(b, c, sample);
                    const float edgeB = edge(c, a, sample);
                    const float edgeC = edge(a, b, sample);
                    const bool covered =
                        (edgeA > 0.0f || (edgeA == 0.0f && topLeft(b, c))) &&
                        (edgeB > 0.0f || (edgeB == 0.0f && topLeft(c, a))) &&
                        (edgeC > 0.0f || (edgeC == 0.0f && topLeft(a, b)));
                    if (covered)
                    {
                        const float sourceAlpha = (std::clamp)(
                            (alphaA * edgeA + alphaB * edgeB + alphaC * edgeC) / area,
                            0.0f,
                            255.0f);
                        auto& destination = alpha[static_cast<size_t>(y * width + x)];
                        const float destinationAlpha = static_cast<float>(destination) / 255.0f;
                        const float normalizedSource = sourceAlpha / 255.0f;
                        destination = static_cast<std::uint8_t>(std::lround(
                            (normalizedSource + destinationAlpha * (1.0f - normalizedSource)) * 255.0f));
                    }
                }
            }
        }
    }
    return alpha;
}

class ScopedImGuiTestContext
{
public:
    ScopedImGuiTestContext()
        : m_previous(ImGui::GetCurrentContext()),
          m_context(ImGui::CreateContext())
    {
        ImGui::SetCurrentContext(m_context);
    }

    ~ScopedImGuiTestContext()
    {
        ImGui::SetCurrentContext(m_context);
        ImGui::DestroyContext(m_context);
        ImGui::SetCurrentContext(m_previous);
    }

    ScopedImGuiTestContext(const ScopedImGuiTestContext&) = delete;
    ScopedImGuiTestContext& operator=(const ScopedImGuiTestContext&) = delete;

private:
    ImGuiContext* m_previous = nullptr;
    ImGuiContext* m_context = nullptr;
};

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
    std::string loaderId,
    std::string targetPlatform = "editor")
{
    const auto artifactPath =
        (std::filesystem::path("Library") /
            "Artifacts" /
            NLS::Core::Assets::BuildArtifactStorageRelativePath(StableArtifactBlobFileName(owner, subAssetKey)))
            .generic_string();
    return {
        owner,
        std::move(subAssetKey),
        type,
        std::move(loaderId),
        std::move(targetPlatform),
        artifactPath,
        "sha256:" + owner.ToString() + ":" + subAssetKey,
        {}
    };
}

std::string TextureArtifactTargetPlatformForTest()
{
#if defined(_WIN32)
    return "win64-dx12";
#else
    return "editor";
#endif
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

std::string SourceFileContentHashForTest(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};

    const std::vector<uint8_t> bytes{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    return NLS::Core::Assets::ComputeNativeArtifactPayloadHash(bytes);
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
        SourceFileContentHashForTest(sourcePath)
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

TEST(AssetBrowserPresentationTests, GeneratedThumbnailRequestSizeTracksVisibleThumbnailBands)
{
    EXPECT_EQ(AssetBrowserThumbnailRequestSize(64.0f), 72u);
    EXPECT_EQ(AssetBrowserThumbnailRequestSize(84.0f), 72u);
    EXPECT_EQ(AssetBrowserThumbnailRequestSize(96.0f), 96u);
    EXPECT_EQ(AssetBrowserThumbnailRequestSize(112.0f), 96u);
    EXPECT_EQ(AssetBrowserThumbnailRequestSize(128.0f), 128u);
    EXPECT_EQ(AssetBrowserThumbnailRequestSize(144.0f), 128u);
    EXPECT_EQ(AssetBrowserThumbnailRequestSize(160.0f), 160u);
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

TEST(AssetBrowserPresentationTests, DisplayFallbackIconsPreferTypeIconWhenThumbnailServiceReturnsDefault)
{
    using namespace NLS::Editor::Assets;

    EXPECT_EQ(
        ResolveAssetBrowserDisplayFallbackIconId(
            AssetBrowserItemType::Shader,
            "editor.icon.asset.default"),
        std::string_view("editor.icon.asset.shader"));
    EXPECT_EQ(
        ResolveAssetBrowserDisplayFallbackIconId(
            AssetBrowserItemType::Scene,
            "editor.icon.asset.default"),
        std::string_view("editor.icon.asset.scene"));
    EXPECT_EQ(
        ResolveAssetBrowserDisplayFallbackIconId(
            AssetBrowserItemType::Scene,
            ""),
        std::string_view("editor.icon.asset.scene"));
    EXPECT_EQ(
        ResolveAssetBrowserDisplayFallbackIconId(
            AssetBrowserItemType::Other,
            "editor.icon.asset.default"),
        std::string_view("editor.icon.asset.default"));
}

TEST(AssetBrowserPresentationTests, DisplayFallbackIconsPreserveExplicitThumbnailFallback)
{
    using namespace NLS::Editor::Assets;

    EXPECT_EQ(
        ResolveAssetBrowserDisplayFallbackIconId(
            AssetBrowserItemType::Shader,
            "editor.icon.asset.texture"),
        std::string_view("editor.icon.asset.texture"));
}

TEST(AssetBrowserPresentationTests, EditorResourcesUsePrefabFileIconIdForPrefabFiles)
{
    EXPECT_STREQ(
        NLS::Editor::Core::EditorResources::GetFileIconId("Hero.prefab"),
        "editor.icon.asset.prefab");
}

TEST(AssetBrowserPresentationTests, EditorResourcesUseShaderFileIconIdForShaderFiles)
{
    EXPECT_STREQ(
        NLS::Editor::Core::EditorResources::GetFileIconId("Hero.shader"),
        "editor.icon.asset.shader");
    EXPECT_STREQ(
        NLS::Editor::Core::EditorResources::GetFileIconId("Hero.SHADER"),
        "editor.icon.asset.shader");
}

TEST(AssetBrowserPresentationTests, EditorResourcesUseSceneFileIconIdForSceneFiles)
{
    EXPECT_STREQ(
        NLS::Editor::Core::EditorResources::GetFileIconId("Hero.scene"),
        "editor.icon.asset.scene");
}

TEST(AssetBrowserPresentationTests, OnlyModelAndPrefabSourceAssetsCanExposeGeneratedSubAssets)
{
    using namespace NLS::Editor::Assets;

    EXPECT_TRUE(AssetBrowserSourceAssetCanHaveGeneratedSubAssets("Assets/Models/Hero.fbx"));
    EXPECT_TRUE(AssetBrowserSourceAssetCanHaveGeneratedSubAssets("Assets/Models/Hero.gltf"));
    EXPECT_TRUE(AssetBrowserSourceAssetCanHaveGeneratedSubAssets("Assets/Prefabs/Hero.prefab"));

    EXPECT_FALSE(AssetBrowserSourceAssetCanHaveGeneratedSubAssets("Assets/Shaders/Hero.shader"));
    EXPECT_FALSE(AssetBrowserSourceAssetCanHaveGeneratedSubAssets("Assets/Scenes/New Scene.scene"));
}

TEST(AssetBrowserPresentationTests, ExpandableSourceShowsDisclosureBeforeAuthoritativeChildCountArrives)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    WriteTextFile(root / "Assets" / "Prefabs" / "Lamp.prefab", "prefab pending import");
    const auto fallbackItems = BuildCurrentFolderAssetItems(root, "Assets/Prefabs", nullptr);
    ASSERT_EQ(fallbackItems.size(), 1u);
    const auto fallbackDisplayItems = BuildAssetBrowserDisplayItems(fallbackItems, {});
    ASSERT_EQ(fallbackDisplayItems.size(), 1u);
    const auto& prefab = fallbackDisplayItems[0];
    EXPECT_TRUE(prefab.item.hasGeneratedSubAssets);
    EXPECT_EQ(prefab.childCount, 0u);
    EXPECT_TRUE(ShouldShowAssetBrowserSubAssetDisclosure(prefab));

    AssetBrowserDisplayItem material;
    material.item.kind = AssetBrowserItemKind::SourceAsset;
    material.item.type = AssetBrowserItemType::Material;
    material.item.hasGeneratedSubAssets = false;
    material.childCount = 0u;
    EXPECT_FALSE(ShouldShowAssetBrowserSubAssetDisclosure(material));

    material.childCount = 1u;
    EXPECT_TRUE(ShouldShowAssetBrowserSubAssetDisclosure(material))
        << "Authoritative children must remain the final source of truth for otherwise unknown source types.";

    auto generatedChild = prefab;
    generatedChild.subAsset = true;
    EXPECT_FALSE(ShouldShowAssetBrowserSubAssetDisclosure(generatedChild))
        << "Generated children must never expose another disclosure level.";

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, GridAndListUseSharedImmediateDisclosurePolicy)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto gridBody = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::DrawCurrentFolderGrid()");
    const auto listBody = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::DrawCurrentFolderList(");

    EXPECT_NE(gridBody.find("ShouldShowAssetBrowserSubAssetDisclosure"), std::string::npos);
    EXPECT_NE(listBody.find("ShouldShowAssetBrowserSubAssetDisclosure"), std::string::npos);
    EXPECT_EQ(gridBody.find("displayItem.childCount > 0u"), std::string::npos);
    EXPECT_EQ(listBody.find("displayItem.childCount > 0u"), std::string::npos);
}

TEST(AssetBrowserPresentationTests, DisclosureClickPublishesSingleGroupImmediatelyBeforeAsyncReconciliation)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto immediateBody = ExtractFunctionBody(
        source,
        "bool Editor::Panels::AssetBrowser::ApplyProjectAssetDisclosureImmediately(");
    const auto gridBody = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::DrawCurrentFolderGrid()");
    const auto listBody = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::DrawCurrentFolderList(");

    EXPECT_NE(immediateBody.find("BuildAssetBrowserPresentationBundle"), std::string::npos)
        << "A click should rebuild only the selected source group from the retained authoritative snapshot.";
    EXPECT_NE(immediateBody.find("m_projectAssetSubAssetSnapshotIndex.get()"), std::string::npos);
    EXPECT_NE(immediateBody.find("loadingPlaceholder = true"), std::string::npos)
        << "Pending imports need immediate expanded-state feedback without synthetic actionable children.";
    EXPECT_NE(
        immediateBody.find("ShouldShowAssetBrowserSubAssetDisclosure(group.front())"),
        std::string::npos)
        << "Pending feedback must use the same disclosure policy that made the source clickable.";
    EXPECT_NE(immediateBody.find("placeholder.groupId = group.front().groupId"), std::string::npos)
        << "The placeholder must inherit its source group instead of assuming the first group id.";
    EXPECT_EQ(immediateBody.find("placeholder.groupId = 1u"), std::string::npos);
    EXPECT_NE(immediateBody.find("StartCurrentFolderItemsRefresh("), std::string::npos)
        << "The expanded key must be queued during the click so an older completion cannot publish "
           "before the next draw and overwrite the immediately expanded group.";
    EXPECT_EQ(immediateBody.find("MarkProjectAssetDisplayItemsDirtyPreservingCurrentItems()"), std::string::npos)
        << "Deferring latest-request coordination through a dirty flag leaves a one-frame stale-completion race.";
    EXPECT_NE(gridBody.find("ApplyProjectAssetDisclosureImmediately"), std::string::npos);
    EXPECT_NE(listBody.find("ApplyProjectAssetDisclosureImmediately"), std::string::npos);
}

TEST(AssetBrowserPresentationTests, AssetTypeIconOverridesUseCatalogedNullusResources)
{
    const auto& records = NLS::Editor::Core::EditorResourceCatalog::DefaultRecords();
    bool foundSceneIcon = false;
    bool foundShaderIcon = false;
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
        if (record.id == "editor.icon.asset.shader")
        {
            foundShaderIcon = true;
            EXPECT_EQ(record.developmentPath.generic_string(), "Editor/Icons/asset-shader.png");
        }
        if (record.id == "editor.icon.asset.prefab")
            foundPrefabIcon = true;
        if (record.id == "editor.icon.asset.material")
            foundMaterialIcon = true;
    }

    EXPECT_TRUE(foundSceneIcon);
    EXPECT_TRUE(foundShaderIcon);
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

TEST(AssetBrowserPresentationTests, GeneratedGroupGeometryAssignsExplicitChildIdentity)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserItem sourceA;
    sourceA.kind = AssetBrowserItemKind::SourceAsset;
    sourceA.projectRelativePath = "Assets/Models/A.fbx";
    sourceA.sourceAssetPath = sourceA.projectRelativePath;
    AssetBrowserItem childA;
    childA.kind = AssetBrowserItemKind::GeneratedSubAsset;
    childA.sourceAssetPath = sourceA.sourceAssetPath;
    AssetBrowserItem secondChildA = childA;
    secondChildA.sourceAssetPath = "Assets\\Models\\A.fbx";
    AssetBrowserItem sourceB = sourceA;
    sourceB.projectRelativePath = "Assets/Models/B.fbx";
    sourceB.sourceAssetPath = sourceB.projectRelativePath;
    AssetBrowserItem childB = childA;
    childB.sourceAssetPath = sourceB.sourceAssetPath;

    const auto display = BuildAssetBrowserDisplayItems(
        {sourceA, childA, sourceB, childB, secondChildA},
        {sourceA.sourceAssetPath, sourceB.sourceAssetPath});

    ASSERT_EQ(display.size(), 5u);
    EXPECT_EQ(display[0].groupId, 0u);
    EXPECT_NE(display[1].groupId, 0u);
    EXPECT_EQ(display[1].groupId, display[2].groupId);
    EXPECT_EQ(display[3].groupId, 0u);
    EXPECT_NE(display[4].groupId, 0u);
    EXPECT_NE(display[1].groupId, display[4].groupId);
}

TEST(AssetBrowserPresentationTests, DisplayItemsUseOnlyProvenSubAssets)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserItem source;
    source.kind = AssetBrowserItemKind::SourceAsset;
    source.type = AssetBrowserItemType::Model;
    source.displayName = "City.fbx";
    source.projectRelativePath = "Assets/Models/City.fbx";
    source.sourceAssetPath = source.projectRelativePath;

    std::vector<AssetBrowserItem> items {source};
    for (size_t index = 0u; index < 3u; ++index)
    {
        AssetBrowserItem mesh;
        mesh.kind = AssetBrowserItemKind::GeneratedSubAsset;
        mesh.type = AssetBrowserItemType::Mesh;
        mesh.displayName = "Mesh " + std::to_string(index);
        mesh.projectRelativePath = source.sourceAssetPath + "::mesh:" + std::to_string(index);
        mesh.sourceAssetPath = source.sourceAssetPath;
        items.push_back(std::move(mesh));
    }

    const auto displayItems = BuildAssetBrowserDisplayItems(items, {source.sourceAssetPath});
    ASSERT_EQ(displayItems.size(), 4u);
    EXPECT_EQ(displayItems[0].childCount, 3u);
    EXPECT_TRUE(std::all_of(
        displayItems.begin() + 1,
        displayItems.end(),
        [](const AssetBrowserDisplayItem& item)
        {
            return item.subAsset && !item.loadingPlaceholder;
        }))
        << "Display construction must consume only sub-assets present in the proven snapshot.";
}

TEST(AssetBrowserPresentationTests, InteractiveDisplayRebuildPublishesPartialResultsLessOften)
{
    using namespace NLS::Editor::Assets;

    EXPECT_FALSE(ShouldPublishPartialAssetBrowserDisplayRebuild(63u, true));
    EXPECT_TRUE(ShouldPublishPartialAssetBrowserDisplayRebuild(64u, false));
    EXPECT_FALSE(ShouldPublishPartialAssetBrowserDisplayRebuild(255u, true));
    EXPECT_TRUE(ShouldPublishPartialAssetBrowserDisplayRebuild(256u, true));
}

TEST(AssetBrowserPresentationTests, ExpandedSourceAssetFilmstripRangeCoversOnlyContiguousChildren)
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

    AssetBrowserItem material = mesh;
    material.type = AssetBrowserItemType::Material;
    material.displayName = "Material A";
    material.projectRelativePath = "Assets/Models/Sponza.fbx::material:A";

    AssetBrowserItem texture = material;
    texture.type = AssetBrowserItemType::Texture;
    texture.displayName = "Texture A";
    texture.projectRelativePath = "Assets/Models/Sponza.fbx::texture:A";

    AssetBrowserItem otherSource = source;
    otherSource.displayName = "Other.fbx";
    otherSource.projectRelativePath = "Assets/Models/Other.fbx";
    otherSource.sourceAssetPath = otherSource.projectRelativePath;

    const std::vector<AssetBrowserItem> items { source, mesh, material, texture, otherSource };
    const auto expanded = BuildAssetBrowserDisplayItems(items, { source.sourceAssetPath });

    const auto range = ResolveAssetBrowserExpandedSubAssetRange(expanded, 0u);
    ASSERT_TRUE(range.has_value());
    EXPECT_EQ(range->begin, 1u);
    EXPECT_EQ(range->count, 3u);

    EXPECT_FALSE(ResolveAssetBrowserExpandedSubAssetRange(expanded, 1u).has_value());
    EXPECT_FALSE(ResolveAssetBrowserExpandedSubAssetRange(expanded, 4u).has_value());

    const auto collapsed = BuildAssetBrowserDisplayItems(items, {});
    EXPECT_FALSE(ResolveAssetBrowserExpandedSubAssetRange(collapsed, 0u).has_value());
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

TEST(AssetBrowserPresentationTests, GridThumbnailDrawDoesNotAddOpaqueLetterboxBackground)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto body = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::DrawProjectGridItemThumbnail(");

    EXPECT_EQ(body.find("ShouldDrawAssetBrowserThumbnailLetterboxBackground"), std::string::npos);
    EXPECT_EQ(body.find("IM_COL32(0, 0, 0, 255)"), std::string::npos);
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
    WriteTextFile(root / "Assets" / "Hero.scene", "scene");
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
        {"Hero.scene", AssetBrowserItemType::Scene, FileType::SCENE},
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
    WriteTextFile(root / "Library" / "Artifacts" / "88e2545b80aa4e56f084a0f496c700aaa4791c05ab17958bc9724c3c4809e5a4", "mesh");

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
    EXPECT_EQ(FindItem(items, "88e2545b80aa4e56f084a0f496c700aaa4791c05ab17958bc9724c3c4809e5a4"), nullptr);

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
    options.expandedSourceAssets.insert("Assets/Models/Hero.gltf");
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

TEST(AssetBrowserPresentationTests, InterleavedGeneratedChildrenRemainWithTheirOwnSource)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserItem sourceA;
    sourceA.kind = AssetBrowserItemKind::SourceAsset;
    sourceA.displayName = "A.fbx";
    sourceA.projectRelativePath = "Assets/Models/A.fbx";
    sourceA.sourceAssetPath = sourceA.projectRelativePath;

    AssetBrowserItem sourceB = sourceA;
    sourceB.displayName = "B.fbx";
    sourceB.projectRelativePath = "Assets/Models/B.fbx";
    sourceB.sourceAssetPath = sourceB.projectRelativePath;

    AssetBrowserItem childA1;
    childA1.kind = AssetBrowserItemKind::GeneratedSubAsset;
    childA1.displayName = "A1";
    childA1.sourceAssetPath = sourceA.sourceAssetPath;

    AssetBrowserItem childB1 = childA1;
    childB1.displayName = "B1";
    childB1.sourceAssetPath = sourceB.sourceAssetPath;

    AssetBrowserItem childA2 = childA1;
    childA2.displayName = "A2";

    const auto display = BuildAssetBrowserDisplayItems(
        {sourceA, childA1, sourceB, childB1, childA2},
        {sourceA.sourceAssetPath, sourceB.sourceAssetPath});

    ASSERT_EQ(display.size(), 5u);
    EXPECT_EQ(display[0].item.displayName, "A.fbx");
    EXPECT_EQ(display[0].childCount, 2u);
    EXPECT_EQ(display[1].item.displayName, "A1");
    EXPECT_EQ(display[2].item.displayName, "A2");
    EXPECT_NE(display[1].groupId, 0u);
    EXPECT_EQ(display[1].groupId, display[2].groupId);
    EXPECT_EQ(display[3].item.displayName, "B.fbx");
    EXPECT_EQ(display[3].childCount, 1u);
    EXPECT_EQ(display[4].item.displayName, "B1");
    EXPECT_NE(display[4].groupId, 0u);
    EXPECT_NE(display[1].groupId, display[4].groupId);
}

TEST(AssetBrowserPresentationTests, ChildCountUsesOnlyProvenDisplayChildren)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserItem source;
    source.kind = AssetBrowserItemKind::SourceAsset;
    source.projectRelativePath = "Assets/Models/Empty.fbx";
    source.sourceAssetPath = source.projectRelativePath;
    source.hasGeneratedSubAssets = true;

    const auto display = BuildAssetBrowserDisplayItems(
        {source},
        {source.sourceAssetPath});

    ASSERT_EQ(display.size(), 1u);
    EXPECT_EQ(display[0].childCount, 0u);
}

TEST(AssetBrowserPresentationTests, GroupAwareFilteringKeepsCountsAndMembershipAligned)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserItem source;
    source.kind = AssetBrowserItemKind::SourceAsset;
    source.type = AssetBrowserItemType::Model;
    source.displayName = "Hero Shared.fbx";
    source.projectRelativePath = "Assets/Models/Hero.fbx";
    source.sourceAssetPath = source.projectRelativePath;

    AssetBrowserItem child;
    child.kind = AssetBrowserItemKind::GeneratedSubAsset;
    child.type = AssetBrowserItemType::Mesh;
    child.displayName = "Body Shared";
    child.sourceAssetPath = source.sourceAssetPath;

    const std::vector<AssetBrowserItem> items {source, child};
    const std::unordered_set<std::string> expanded {source.sourceAssetPath};

    const auto sourceOnly = BuildFilteredAssetBrowserDisplayItems(
        items, expanded, "hero", AssetBrowserItemType::All);
    ASSERT_EQ(sourceOnly.size(), 1u);
    EXPECT_EQ(sourceOnly[0].childCount, 0u);

    const auto childOnly = BuildFilteredAssetBrowserDisplayItems(
        items, expanded, "body", AssetBrowserItemType::All);
    ASSERT_EQ(childOnly.size(), 2u);
    EXPECT_EQ(childOnly[0].item.displayName, source.displayName);
    EXPECT_EQ(childOnly[0].childCount, 1u);
    EXPECT_EQ(childOnly[1].item.displayName, child.displayName);

    const auto both = BuildFilteredAssetBrowserDisplayItems(
        items, expanded, "shared", AssetBrowserItemType::All);
    ASSERT_EQ(both.size(), 2u);
    EXPECT_EQ(both[0].childCount, 1u);

    const auto neither = BuildFilteredAssetBrowserDisplayItems(
        items, expanded, "missing", AssetBrowserItemType::All);
    EXPECT_TRUE(neither.empty());

    const auto collapsedChildOnly = BuildFilteredAssetBrowserDisplayItems(
        items, {}, "body", AssetBrowserItemType::Mesh);
    ASSERT_EQ(collapsedChildOnly.size(), 1u);
    EXPECT_EQ(collapsedChildOnly[0].item.displayName, source.displayName);
    EXPECT_EQ(collapsedChildOnly[0].childCount, 1u);
    EXPECT_FALSE(collapsedChildOnly[0].expanded);
}

TEST(AssetBrowserPresentationTests, GeneratedGroupGeometryResolvesGridRowsAndListEdges)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserDisplayItem source;
    source.item.kind = AssetBrowserItemKind::SourceAsset;
    source.item.sourceAssetPath = "Assets/Models/A.fbx";

    AssetBrowserDisplayItem childA;
    childA.item.kind = AssetBrowserItemKind::GeneratedSubAsset;
    childA.item.sourceAssetPath = source.item.sourceAssetPath;
    childA.subAsset = true;
    childA.groupId = 1u;

    AssetBrowserDisplayItem childB = childA;
    childB.item.sourceAssetPath = "Assets/Models/B.fbx";
    childB.groupId = 2u;

    const std::vector<AssetBrowserDisplayItem> display {
        source,
        childA,
        childA,
        childA,
        childB
    };

    const auto firstGridRow = ResolveAssetBrowserGridRowGroupSegments(display, 0u, 2u);
    ASSERT_EQ(firstGridRow.size(), 1u);
    EXPECT_EQ(firstGridRow[0].range.begin, 1u);
    EXPECT_EQ(firstGridRow[0].range.count, 1u);
    EXPECT_TRUE(firstGridRow[0].trueSegmentStart);
    EXPECT_TRUE(firstGridRow[0].trueSegmentEnd);

    const auto secondGridRow = ResolveAssetBrowserGridRowGroupSegments(display, 2u, 2u);
    ASSERT_EQ(secondGridRow.size(), 1u);
    EXPECT_EQ(secondGridRow[0].range.begin, 2u);
    EXPECT_EQ(secondGridRow[0].range.count, 2u);
    EXPECT_TRUE(secondGridRow[0].trueSegmentStart);
    EXPECT_TRUE(secondGridRow[0].trueSegmentEnd);

    const auto clippedList = ResolveAssetBrowserVisibleListGroupSegments(display, 2u, 4u);
    ASSERT_EQ(clippedList.size(), 1u);
    EXPECT_EQ(clippedList[0].range.begin, 2u);
    EXPECT_EQ(clippedList[0].range.count, 2u);
    EXPECT_FALSE(clippedList[0].trueSegmentStart);
    EXPECT_TRUE(clippedList[0].trueSegmentEnd);

    const auto fullListGroup = ResolveAssetBrowserVisibleListGroupSegments(display, 1u, 4u);
    ASSERT_EQ(fullListGroup.size(), 1u);
    EXPECT_EQ(fullListGroup[0].range.begin, 1u);
    EXPECT_EQ(fullListGroup[0].range.count, 3u);
    EXPECT_TRUE(fullListGroup[0].trueSegmentStart);
    EXPECT_TRUE(fullListGroup[0].trueSegmentEnd);

    AssetBrowserDisplayItem zeroGroupChild = childA;
    zeroGroupChild.groupId = 0u;
    const auto zeroDoesNotJoin = ResolveAssetBrowserGridRowGroupSegments(
        {childA, zeroGroupChild, zeroGroupChild, childA},
        0u,
        4u);
    ASSERT_EQ(zeroDoesNotJoin.size(), 2u);
    EXPECT_EQ(zeroDoesNotJoin[0].range.begin, 0u);
    EXPECT_EQ(zeroDoesNotJoin[0].range.count, 1u);
    EXPECT_EQ(zeroDoesNotJoin[1].range.begin, 3u);
    EXPECT_EQ(zeroDoesNotJoin[1].range.count, 1u);

    const auto saturatedRow = ResolveAssetBrowserGridRowGroupSegments(
        {source, childA, childA},
        1u,
        (std::numeric_limits<size_t>::max)());
    ASSERT_EQ(saturatedRow.size(), 1u);
    EXPECT_EQ(saturatedRow[0].range.begin, 1u);
    EXPECT_EQ(saturatedRow[0].range.count, 2u);
}

TEST(AssetBrowserPresentationTests, GeneratedGroupGeometryGridDrawUsesResolvedBackgroundSegments)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto body = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::DrawCurrentFolderGrid()");

    const auto resolveSegments = body.find("ResolveAssetBrowserGridRowGroupSegments(");
    const auto captureParentClip = body.find("const ImRect parentClipRect =");
    const auto pushBackground = body.find("ImGui::PushColumnsBackground()", resolveSegments);
    const auto pushIntersectedClip = body.find(
        "PushClipRect(parentClipRect.Min, parentClipRect.Max, true)",
        pushBackground);
    const auto firstColumnOffset = body.find("ImGui::GetColumnOffset(firstColumn)", pushBackground);
    const auto lastColumnOffset = body.find("ImGui::GetColumnOffset(lastColumnExclusive)", firstColumnOffset);
    const auto drawSegment = body.find("DrawAssetBrowserSegmentPanel(", lastColumnOffset);
    const auto popIntersectedClip = body.find("PopClipRect()", drawSegment);
    const auto popBackground = body.find("ImGui::PopColumnsBackground()", popIntersectedClip);
    const auto drawItems = body.find("for (int column = 0; column < columns; ++column)", popBackground);

    EXPECT_NE(resolveSegments, std::string::npos)
        << "Each visible grid row must use the shared group segment resolver.";
    EXPECT_NE(captureParentClip, std::string::npos);
    EXPECT_LT(captureParentClip, pushBackground);
    EXPECT_NE(pushBackground, std::string::npos)
        << "Spanning fills must leave the first legacy ImGui column clip/channel.";
    EXPECT_NE(pushIntersectedClip, std::string::npos)
        << "Background geometry must intersect the retained parent content clip.";
    EXPECT_NE(firstColumnOffset, std::string::npos);
    EXPECT_NE(lastColumnOffset, std::string::npos)
        << "Segment bounds must follow actual ImGui column allocation, including residual width.";
    EXPECT_NE(drawSegment, std::string::npos);
    EXPECT_NE(popIntersectedClip, std::string::npos);
    EXPECT_NE(popBackground, std::string::npos)
        << "Item drawing must restore the current column channel after background fills.";
    EXPECT_NE(drawItems, std::string::npos);
    EXPECT_EQ(body.find("continuesLeft"), std::string::npos);
    EXPECT_EQ(body.find("segmentCount"), std::string::npos)
        << "Grid segment membership must not be rescanned per item.";
}

TEST(AssetBrowserPresentationTests, GeneratedGroupImGuiRasterCoversGapRoundsEdgeAndHonorsParentClip)
{
    ImDrawListSharedData sharedData;
    InitializeDrawListSharedData(sharedData);
    ImDrawList drawList(&sharedData);
    drawList._ResetForNewFrame();
    drawList.Flags = ImDrawListFlags_None;
    drawList.PushClipRect(ImVec2(0.0f, 6.0f), ImVec2(34.0f, 26.0f), false);
    DrawAssetBrowserSegmentPanel(
        &drawList,
        ImVec2(2.0f, 8.0f),
        ImVec2(38.0f, 24.0f),
        false,
        ImDrawFlags_RoundCornersAll);
    drawList.PopClipRect();

    constexpr int width = 48;
    constexpr int height = 32;
    const auto alpha = RasterizeDrawListAlpha(
        drawList,
        width,
        height,
        ImVec2(0.0f, 0.0f),
        ImVec2(1.0f, 1.0f));
    const auto covered = [&alpha](const int x, const int y)
    {
        return alpha[static_cast<size_t>(y * width + x)] != 0u;
    };

    EXPECT_TRUE(covered(20, 16));
    EXPECT_FALSE(covered(2, 8));
    EXPECT_TRUE(covered(3, 16));
    EXPECT_FALSE(covered(36, 16));
}

TEST(AssetBrowserPresentationTests, GeneratedGroupImGuiRasterTransformsAndCompositesTopLeftCoverage)
{
    ImDrawListSharedData sharedData;
    InitializeDrawListSharedData(sharedData);
    ImDrawList drawList(&sharedData);
    drawList._ResetForNewFrame();
    drawList.Flags = ImDrawListFlags_None;
    drawList.PushClipRect(ImVec2(2.0f, 3.0f), ImVec2(30.0f, 31.0f), false);
    drawList.AddRectFilled(ImVec2(6.0f, 7.0f), ImVec2(22.0f, 23.0f), IM_COL32(255, 255, 255, 128));
    drawList.AddRectFilled(ImVec2(6.0f, 7.0f), ImVec2(22.0f, 23.0f), IM_COL32(255, 255, 255, 128));
    drawList.PopClipRect();

    ImDrawVert dummyVertex {};
    drawList.VtxBuffer.insert(drawList.VtxBuffer.begin(), dummyVertex);
    drawList.IdxBuffer.insert(drawList.IdxBuffer.begin(), 0);
    drawList.IdxBuffer.insert(drawList.IdxBuffer.begin(), 0);
    drawList.IdxBuffer.insert(drawList.IdxBuffer.begin(), 0);
    for (auto& command : drawList.CmdBuffer)
    {
        if (command.ElemCount == 0u)
            continue;
        command.IdxOffset += 3u;
        command.VtxOffset += 1u;
    }

    const auto alpha = RasterizeDrawListAlpha(
        drawList,
        48,
        48,
        ImVec2(2.0f, 3.0f),
        ImVec2(2.0f, 2.0f));
    EXPECT_EQ(alpha[24u * 48u + 24u], 192u)
        << "The shared diagonal must use top-left ownership once per rectangle, then source-over compose the two 50% fills.";

    ImDrawList interpolated(&sharedData);
    interpolated._ResetForNewFrame();
    interpolated.Flags = ImDrawListFlags_None;
    interpolated.PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(24.0f, 24.0f), false);
    interpolated.AddTriangleFilled(
        ImVec2(2.0f, 2.0f),
        ImVec2(20.0f, 2.0f),
        ImVec2(2.0f, 20.0f),
        IM_COL32_WHITE);
    interpolated.PopClipRect();
    ASSERT_GE(interpolated.VtxBuffer.Size, 3);
    interpolated.VtxBuffer[0].col = IM_COL32(255, 255, 255, 0);
    interpolated.VtxBuffer[1].col = IM_COL32(255, 255, 255, 255);
    interpolated.VtxBuffer[2].col = IM_COL32(255, 255, 255, 255);
    const auto interpolatedAlpha = RasterizeDrawListAlpha(
        interpolated,
        24,
        24,
        ImVec2(0.0f, 0.0f),
        ImVec2(1.0f, 1.0f));
    EXPECT_GT(interpolatedAlpha[6u * 24u + 6u], 0u);
    EXPECT_LT(interpolatedAlpha[6u * 24u + 6u], 255u);
}

TEST(AssetBrowserPresentationTests, GeneratedGroupImGuiRasterHonorsFractionalClipSampleCenters)
{
    ImDrawListSharedData sharedData;
    InitializeDrawListSharedData(sharedData);
    ImDrawList drawList(&sharedData);
    drawList._ResetForNewFrame();
    drawList.Flags = ImDrawListFlags_None;
    drawList.PushClipRect(ImVec2(2.8f, 1.0f), ImVec2(10.2f, 8.0f), false);
    drawList.AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(16.0f, 10.0f), IM_COL32_WHITE);
    drawList.PopClipRect();

    const auto alpha = RasterizeDrawListAlpha(
        drawList,
        16,
        10,
        ImVec2(0.0f, 0.0f),
        ImVec2(1.0f, 1.0f));
    const auto covered = [&alpha](const int x)
    {
        return alpha[4u * 16u + static_cast<size_t>(x)] != 0u;
    };
    EXPECT_FALSE(covered(2));
    EXPECT_TRUE(covered(3));
    EXPECT_TRUE(covered(9));
    EXPECT_FALSE(covered(10));
}

TEST(AssetBrowserPresentationTests, GeneratedGroupImGuiRasterContextRestoresPreviousContext)
{
    auto* previous = ImGui::GetCurrentContext();
    {
        ScopedImGuiTestContext context;
        EXPECT_NE(ImGui::GetCurrentContext(), previous);
    }
    EXPECT_EQ(ImGui::GetCurrentContext(), previous);
}

TEST(AssetBrowserPresentationTests, GeneratedGroupImGuiRasterRejectsMalformedDrawData)
{
    ImDrawListSharedData sharedData;
    InitializeDrawListSharedData(sharedData);
    ImDrawList drawList(&sharedData);
    drawList._ResetForNewFrame();
    drawList.Flags = ImDrawListFlags_None;
    drawList.PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(24.0f, 24.0f), false);
    drawList.AddRectFilled(ImVec2(2.0f, 2.0f), ImVec2(20.0f, 20.0f), IM_COL32_WHITE);
    drawList.PopClipRect();
    ASSERT_GT(drawList.CmdBuffer.Size, 0);
    ASSERT_GT(drawList.IdxBuffer.Size, 0);
    ASSERT_GT(drawList.VtxBuffer.Size, 0);

    auto& command = drawList.CmdBuffer[0];
    const auto originalElemCount = command.ElemCount;
    ASSERT_GT(originalElemCount, 0u);
    command.ElemCount = originalElemCount - 1u;
    EXPECT_THROW(
        RasterizeDrawListAlpha(drawList, 24, 24, ImVec2(), ImVec2(1.0f, 1.0f)),
        std::runtime_error);
    command.ElemCount = originalElemCount;

    const auto originalIdxOffset = command.IdxOffset;
    command.IdxOffset = static_cast<unsigned int>(drawList.IdxBuffer.Size);
    EXPECT_THROW(
        RasterizeDrawListAlpha(drawList, 24, 24, ImVec2(), ImVec2(1.0f, 1.0f)),
        std::out_of_range);
    command.IdxOffset = originalIdxOffset;

    const auto originalIndex = drawList.IdxBuffer[0];
    drawList.IdxBuffer[0] = static_cast<ImDrawIdx>(drawList.VtxBuffer.Size);
    EXPECT_THROW(
        RasterizeDrawListAlpha(drawList, 24, 24, ImVec2(), ImVec2(1.0f, 1.0f)),
        std::out_of_range);
    drawList.IdxBuffer[0] = originalIndex;

    const auto originalVtxOffset = command.VtxOffset;
    command.VtxOffset = (std::numeric_limits<unsigned int>::max)();
    EXPECT_THROW(
        RasterizeDrawListAlpha(drawList, 24, 24, ImVec2(), ImVec2(1.0f, 1.0f)),
        std::out_of_range);
    command.VtxOffset = originalVtxOffset;

    const ImVec2 originalPosition = drawList.VtxBuffer[0].pos;
    drawList.VtxBuffer[0].pos.x = (std::numeric_limits<float>::quiet_NaN)();
    EXPECT_THROW(
        RasterizeDrawListAlpha(drawList, 24, 24, ImVec2(), ImVec2(1.0f, 1.0f)),
        std::runtime_error);
    drawList.VtxBuffer[0].pos = originalPosition;

    const auto firstIndex = static_cast<int>(drawList.IdxBuffer[0]);
    const auto secondIndex = static_cast<int>(drawList.IdxBuffer[1]);
    const auto thirdIndex = static_cast<int>(drawList.IdxBuffer[2]);
    const ImVec2 secondPosition = drawList.VtxBuffer[secondIndex].pos;
    const ImVec2 thirdPosition = drawList.VtxBuffer[thirdIndex].pos;
    drawList.VtxBuffer[secondIndex].pos = drawList.VtxBuffer[firstIndex].pos;
    drawList.VtxBuffer[thirdIndex].pos = drawList.VtxBuffer[firstIndex].pos;
    EXPECT_THROW(
        RasterizeDrawListAlpha(drawList, 24, 24, ImVec2(), ImVec2(1.0f, 1.0f)),
        std::runtime_error);
    drawList.VtxBuffer[secondIndex].pos = secondPosition;
    drawList.VtxBuffer[thirdIndex].pos = thirdPosition;

    EXPECT_THROW(
        RasterizeDrawListAlpha(drawList, 24, 24, ImVec2(), ImVec2(0.0f, 1.0f)),
        std::invalid_argument);
    drawList.AddCallback(AssetBrowserRasterTestCallback, nullptr);
    EXPECT_THROW(
        RasterizeDrawListAlpha(drawList, 24, 24, ImVec2(), ImVec2(1.0f, 1.0f)),
        std::runtime_error);
}

TEST(AssetBrowserPresentationTests, GeneratedGroupImGuiRasterIntegratesColumnsClipAndResidualWidth)
{
    ScopedImGuiTestContext context;
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(320.0f, 180.0f);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* fontPixels = nullptr;
    int fontWidth = 0;
    int fontHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&fontPixels, &fontWidth, &fontHeight);
    ASSERT_NE(fontPixels, nullptr);

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
    ImGui::SetNextWindowSize(ImVec2(300.0f, 130.0f));
    ASSERT_TRUE(ImGui::Begin(
        "##GeneratedGroupColumnsRaster",
        nullptr,
        ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoScrollbar));
    ImGui::PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(320.0f, 82.0f), true);
    ImGui::Columns(3, "##GeneratedGroupColumns", false);
    ImGui::SetColumnOffset(1, 76.0f);
    ImGui::SetColumnOffset(2, 158.0f);

    auto* drawList = ImGui::GetWindowDrawList();
    const float windowX = ImGui::GetWindowPos().x;
    const float firstOffset = ImGui::GetColumnOffset(0);
    const float internalOffset = ImGui::GetColumnOffset(1);
    const float lastOffset = ImGui::GetColumnOffset(3);
    const float segmentMinX = windowX + firstOffset + 4.0f;
    const float segmentMaxX = windowX + lastOffset - 4.0f;
    const float segmentMinY = ImGui::GetCursorScreenPos().y + 2.0f;
    const ImRect parentClipRect = ImGui::GetCurrentWindow()->DC.CurrentColumns->HostInitialClipRect;
    const float segmentMaxY = parentClipRect.Max.y + 18.0f;

    ImGui::PushColumnsBackground();
    drawList->PushClipRect(parentClipRect.Min, parentClipRect.Max, true);
    DrawAssetBrowserSegmentPanel(
        drawList,
        ImVec2(segmentMinX, segmentMinY),
        ImVec2(segmentMaxX, segmentMaxY),
        false,
        ImDrawFlags_RoundCornersAll);
    drawList->PopClipRect();
    ImGui::PopColumnsBackground();
    ImGui::Columns(1);
    ImGui::PopClipRect();
    ImGui::End();
    ImGui::Render();

    const auto* drawData = ImGui::GetDrawData();
    ASSERT_NE(drawData, nullptr);
    const int framebufferWidth = static_cast<int>(std::ceil(
        drawData->DisplaySize.x * drawData->FramebufferScale.x));
    const int framebufferHeight = static_cast<int>(std::ceil(
        drawData->DisplaySize.y * drawData->FramebufferScale.y));
    const auto alpha = RasterizeDrawListAlpha(
        *drawList,
        framebufferWidth,
        framebufferHeight,
        drawData->DisplayPos,
        drawData->FramebufferScale);
    const auto screenPixel = [drawData](const float x, const float y)
    {
        return std::pair<int, int> {
            static_cast<int>(std::floor((x - drawData->DisplayPos.x) * drawData->FramebufferScale.x)),
            static_cast<int>(std::floor((y - drawData->DisplayPos.y) * drawData->FramebufferScale.y))
        };
    };
    const auto covered = [&alpha, framebufferWidth](const std::pair<int, int> pixel)
    {
        return alpha[static_cast<size_t>(pixel.second * framebufferWidth + pixel.first)] != 0u;
    };

    const float sampleY = segmentMinY + 14.0f;
    EXPECT_TRUE(covered(screenPixel(windowX + internalOffset, sampleY)))
        << "The actual internal ImGui column gap must be covered by one fill.";
    EXPECT_FALSE(covered(screenPixel(segmentMinX, segmentMinY)))
        << "The real segment's outside corner must remain rounded.";
    EXPECT_FALSE(covered(screenPixel(
        (segmentMinX + segmentMaxX) * 0.5f,
        parentClipRect.Max.y + 2.0f)))
        << "The fill extends past the parent vertically but must not escape its clip.";

    constexpr float syntheticCellWidth = 60.0f;
    const float syntheticMaxX = segmentMinX + syntheticCellWidth * 3.0f;
    const float residualSampleX = segmentMaxX - 2.0f;
    EXPECT_GT(residualSampleX, syntheticMaxX + 20.0f)
        << "The fixture must contain meaningful residual width beyond synthetic cells.";
    EXPECT_TRUE(covered(screenPixel(residualSampleX, sampleY)))
        << "The segment must reach the actual last-column offset, including residual width.";
}

TEST(AssetBrowserPresentationTests, ReadyAssetDatabaseRefreshPublishesDuringInteraction)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto body = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::OnBeforeDrawWidgets()");
    const auto refreshBegin = body.find("if (m_projectAssetDatabaseRefresh.has_value())");
    const auto refreshEnd = body.find("PumpObjectReferencePickerEntriesRefresh()", refreshBegin);
    ASSERT_NE(refreshBegin, std::string::npos);
    ASSERT_NE(refreshEnd, std::string::npos);
    const auto refreshBlock = body.substr(refreshBegin, refreshEnd - refreshBegin);

    EXPECT_NE(
        refreshBlock.find("else if (refresh.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)"),
        std::string::npos)
        << "A ready database future is nonblocking and must publish the exact child snapshot even while the browser remains interactive.";
    EXPECT_EQ(refreshBlock.find("else if (!interactive &&"), std::string::npos)
        << "Continuous scrolling or dragging must not indefinitely suppress exact child counts and disclosure buttons.";
}

TEST(AssetBrowserPresentationTests, AssetDatabaseRefreshBuildsSnapshotOffTheUiThread)
{
    const auto header = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.h"));
    EXPECT_NE(header.find("struct AssetDatabaseRefreshResult"), std::string::npos);
    EXPECT_NE(
        header.find("std::future<AssetDatabaseRefreshResult> future"),
        std::string::npos)
        << "The ready future must carry both the refreshed database and its immutable snapshot.";

    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto rebuildBody = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::RebuildProjectAssetPresentation(");
    const auto schedule = rebuildBody.find("AssetBrowser.ProjectAssetDatabaseRefresh");
    const auto refresh = rebuildBody.find("database->Refresh()", schedule);
    const auto buildSnapshot = rebuildBody.find(
        "AssetDatabaseFacade::CreateReadOnlySnapshot(*result.database)",
        refresh);
    EXPECT_NE(schedule, std::string::npos);
    EXPECT_NE(refresh, std::string::npos);
    EXPECT_NE(buildSnapshot, std::string::npos)
        << "Snapshot deep-copy construction belongs in the background refresh job.";

    const auto beforeDrawBody = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::OnBeforeDrawWidgets()");
    const auto completionBegin = beforeDrawBody.find("if (m_projectAssetDatabaseRefresh.has_value())");
    const auto completionEnd = beforeDrawBody.find(
        "PumpObjectReferencePickerEntriesRefresh()",
        completionBegin);
    ASSERT_NE(completionBegin, std::string::npos);
    ASSERT_NE(completionEnd, std::string::npos);
    const auto completion = beforeDrawBody.substr(
        completionBegin,
        completionEnd - completionBegin);
    EXPECT_EQ(completion.find("CreateReadOnlySnapshot"), std::string::npos)
        << "Ready completion runs on the UI thread and must remain move-only.";
    EXPECT_NE(
        completion.find("m_projectAssetDatabase = std::move(result.database)"),
        std::string::npos);
    EXPECT_NE(
        completion.find("m_projectAssetDatabaseSnapshot = std::move(result.snapshot)"),
        std::string::npos);
}

TEST(AssetBrowserPresentationTests, AssetDatabaseRefreshResultsRetireOffTheUiThread)
{
    const auto header = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.h"));
    EXPECT_NE(header.find("struct AssetDatabaseRetirementState"), std::string::npos);
    EXPECT_NE(
        header.find("std::shared_ptr<AssetDatabaseRetirementState> m_projectAssetDatabaseRetirementState"),
        std::string::npos);
    EXPECT_EQ(
        header.find("std::future<void> m_projectAssetDatabaseRetirementFuture"),
        std::string::npos)
        << "A blocking retirement consumer must not occupy the shared BackgroundJobQueue.";
    EXPECT_NE(
        header.find("std::vector<std::future<AssetDatabaseRefreshResult>> pendingFutures"),
        std::string::npos);

    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto retireBody = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::RetireProjectAssetDatabaseResult(");
    EXPECT_NE(retireBody.find("pending.push_back(std::move(result))"), std::string::npos);
    EXPECT_NE(
        retireBody.find("ScheduleProjectAssetDatabaseRetirementWorker()"),
        std::string::npos);

    const auto schedulerBody = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::ScheduleProjectAssetDatabaseRetirementWorker()");
    EXPECT_NE(schedulerBody.find("workerRunning = true"), std::string::npos);
    EXPECT_NE(schedulerBody.find("ScheduleAssetBrowserJobFuture"), std::string::npos)
        << "Retirement must use the editor's unified JobSystem ownership path.";
    EXPECT_NE(
        schedulerBody.find("[retirementState = m_projectAssetDatabaseRetirementState]"),
        std::string::npos)
        << "The retirement job must not capture the panel lifetime.";
    EXPECT_NE(schedulerBody.find("pending.swap(retired)"), std::string::npos);
    EXPECT_NE(schedulerBody.find("pendingFutures.swap(retiredFutures)"), std::string::npos);
    EXPECT_NE(schedulerBody.find("result = future.get()"), std::string::npos)
        << "Only the retirement worker may wait for unfinished refresh results.";
    EXPECT_NE(schedulerBody.find("workerRunning = false"), std::string::npos)
        << "Scheduling rejection must retain queued ownership for a later enqueue event.";

    const auto pumpBody = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::PumpRetiredProjectAssetDatabaseRefreshes()");
    EXPECT_NE(pumpBody.find("result = iterator->future.get()"), std::string::npos);
    EXPECT_NE(
        pumpBody.find("RetireProjectAssetDatabaseResult(std::move(result))"),
        std::string::npos)
        << "A stale ready refresh must transfer ownership to background retirement.";

    const auto beforeDrawBody = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::OnBeforeDrawWidgets()");
    const auto retireCurrent = beforeDrawBody.find("RetireCurrentProjectAssetDatabase()");
    const auto installDatabase = beforeDrawBody.find(
        "m_projectAssetDatabase = std::move(result.database)",
        retireCurrent);
    const auto retireStale = beforeDrawBody.find(
        "RetireProjectAssetDatabaseResult(std::move(result))",
        installDatabase);
    EXPECT_NE(retireCurrent, std::string::npos);
    EXPECT_NE(installDatabase, std::string::npos);
    EXPECT_NE(retireStale, std::string::npos)
        << "Publication must retire old pointers first and stale results after the root guard.";
}

TEST(AssetBrowserPresentationTests, AssetDatabaseRefreshShutdownNeverWaitsOrOwnsRetirementLifetime)
{
    const auto header = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.h"));
    EXPECT_NE(
        header.find("SharedProjectAssetDatabaseRetirementState()"),
        std::string::npos);
    EXPECT_NE(
        header.find("SharedProjectAssetDatabaseRetirementState();"),
        std::string::npos)
        << "The panel member must alias process-lifetime retirement state.";

    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto sharedStateBody = ExtractFunctionBody(
        source,
        "Editor::Panels::AssetBrowser::SharedProjectAssetDatabaseRetirementState()");
    EXPECT_NE(sharedStateBody.find("static const auto retirementState"), std::string::npos);

    const auto abandonBody = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::AbandonProjectAssetDatabaseRefreshFuture(");
    const auto readyCheck = abandonBody.find("wait_for(std::chrono::seconds(0))");
    const auto readyGet = abandonBody.find("result = future.get()", readyCheck);
    const auto enqueueFuture = abandonBody.find(
        "pendingFutures.push_back(std::move(future))",
        readyGet);
    EXPECT_NE(readyCheck, std::string::npos);
    EXPECT_NE(readyGet, std::string::npos)
        << "UI consumption is permitted only after a zero-time readiness check.";
    EXPECT_NE(enqueueFuture, std::string::npos)
        << "Persistent reaper state must own unfinished futures before scheduling is attempted.";
    EXPECT_EQ(abandonBody.find("[future = std::move(future)]"), std::string::npos)
        << "Scheduling rejection must not destroy a move-captured consumer future on UI.";

    const auto destructorBody = ExtractFunctionBody(
        source,
        "Editor::Panels::AssetBrowser::~AssetBrowser()");
    EXPECT_NE(
        destructorBody.find("AbandonProjectAssetDatabaseRefreshFuture("),
        std::string::npos);
    EXPECT_EQ(destructorBody.find("future.get()"), std::string::npos)
        << "Panel destruction must not synchronously wait for database refreshes.";
}

TEST(AssetBrowserPresentationTests, GeneratedArtifactProjectionIsExhaustive)
{
    using NLS::Core::Assets::ArtifactType;
    using NLS::Editor::Assets::AssetBrowserGeneratedArtifactItemType;
    using NLS::Editor::Assets::AssetBrowserItemType;

    const std::array<std::optional<AssetBrowserItemType>,
        static_cast<size_t>(ArtifactType::Count) + 1u> expected {
        std::nullopt,
        std::nullopt,
        AssetBrowserItemType::Mesh,
        AssetBrowserItemType::Material,
        AssetBrowserItemType::Texture,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        AssetBrowserItemType::Prefab,
        std::nullopt,
        AssetBrowserItemType::Shader,
        std::nullopt,
        std::nullopt
    };

    for (size_t index = 0u; index < expected.size(); ++index)
    {
        EXPECT_EQ(AssetBrowserGeneratedArtifactItemType(static_cast<ArtifactType>(index)), expected[index])
            << "ArtifactType index " << index;
    }
}

TEST(AssetBrowserPresentationTests, GeneratedSubAssetsPreferImportedDisplayNames)
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
    auto prefab = MakeArtifact(modelId, "prefab:Hero", ArtifactType::Prefab, "prefab");
    prefab.displayName = "Hero Prefab";
    manifest.subAssets.push_back(std::move(prefab));
    WriteManifestArtifactFiles(root, manifest);
    AddCurrentSourceDependencies(root, manifest, "Assets/Models/Hero.gltf");
    database.AddArtifactManifest(manifest);

    AssetBrowserBuildOptions options;
    options.includeGeneratedSubAssets = true;
    options.expandedSourceAssets.insert("Assets/Models/Hero.gltf");
    const auto items = BuildCurrentFolderAssetItems(root, "Assets/Models", &database, options);

    const auto* generated = FindGeneratedSubAssetItem(items, "prefab:Hero");
    ASSERT_NE(generated, nullptr);
    EXPECT_EQ(generated->displayName, "Hero Prefab");

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, ShaderSourceAssetsDoNotExposeGeneratedSubAssets)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    WriteTextFile(root / "Assets" / "Shaders" / "HeroSurface.shader", "shader");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto shaderId = ParseAssetId(database.AssetPathToGUID("Assets/Shaders/HeroSurface.shader"));
    ArtifactManifest manifest;
    manifest.sourceAssetId = shaderId;
    manifest.importerId = "shader";
    manifest.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::Shader);
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "shader:HeroSurface";
    manifest.subAssets.push_back(MakeArtifact(shaderId, "shader:HeroSurface", ArtifactType::Shader, "shader"));
    WriteManifestArtifactFiles(root, manifest);
    AddCurrentSourceDependencies(root, manifest, "Assets/Shaders/HeroSurface.shader");
    database.AddArtifactManifest(manifest);

    AssetBrowserBuildOptions options;
    options.includeGeneratedSubAssets = true;
    options.expandedSourceAssets.insert("Assets/Shaders/HeroSurface.shader");
    const auto items = BuildCurrentFolderAssetItems(root, "Assets/Shaders", &database, options);

    ASSERT_EQ(items.size(), 1u);
    const auto* source = FindItem(items, "HeroSurface.shader");
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->kind, AssetBrowserItemKind::SourceAsset);
    EXPECT_EQ(source->type, AssetBrowserItemType::Shader);
    EXPECT_FALSE(source->hasGeneratedSubAssets);
    EXPECT_EQ(FindGeneratedSubAssetItem(items, "shader:HeroSurface"), nullptr);

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
    WriteTextFile(root / "Assets" / "Materials" / "LampShader.shader", "shader");

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
    EXPECT_NE(FindItem(allItems, "LampShader.shader"), nullptr);
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

TEST(AssetBrowserPresentationTests, ThumbnailGenerationScopeKeyStaysCompactForLargeVisibleSets)
{
    using namespace NLS::Editor::Assets;

    std::vector<AssetBrowserItem> items;
    items.reserve(512u);
    for (size_t index = 0u; index < 512u; ++index)
    {
        AssetBrowserItem item;
        item.displayName = "GeneratedMesh_" + std::to_string(index);
        item.projectRelativePath =
            "Assets/Models/HeroWithManyParts.fbx::mesh:GeneratedMesh_" + std::to_string(index);
        item.sourceAssetPath = "Assets/Models/HeroWithManyParts.fbx";
        item.subAssetKey = "mesh:GeneratedMesh_" + std::to_string(index);
        item.assetId = NLS::Core::Assets::AssetId(
            NLS::Guid::NewDeterministic("thumbnail-scope-item-" + std::to_string(index)));
        item.kind = AssetBrowserItemKind::GeneratedSubAsset;
        item.type = AssetBrowserItemType::Mesh;
        item.artifactType = NLS::Core::Assets::ArtifactType::Mesh;
        items.push_back(std::move(item));
    }

    const auto scope = BuildAssetBrowserThumbnailGenerationScopeKey(
        "Assets/Models",
        96u,
        items);

    EXPECT_LT(scope.size(), 256u);
    EXPECT_EQ(
        scope,
        BuildAssetBrowserThumbnailGenerationScopeKey("Assets/Models", 96u, items));
}

TEST(AssetBrowserPresentationTests, ThumbnailRequestBuildContextCachesRepeatedFileStamps)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    WriteTextFile(root / "Assets" / "Models" / "Hero.fbx", "model");

    AssetThumbnailRequestBuildContext context;
    context.deferManifestLookups = true;
    const auto assetId = AssetId(NLS::Guid::NewDeterministic("thumbnail-request-stamp-cache"));
    for (size_t index = 0u; index < 64u; ++index)
    {
        AssetBrowserItem item;
        item.displayName = "Mesh_" + std::to_string(index);
        item.projectRelativePath = "Assets/Models/Hero.fbx::mesh:Mesh_" + std::to_string(index);
        item.sourceAssetPath = "Assets/Models/Hero.fbx";
        item.subAssetKey = "mesh:Mesh_" + std::to_string(index);
        item.assetId = assetId;
        item.kind = AssetBrowserItemKind::GeneratedSubAsset;
        item.type = AssetBrowserItemType::Mesh;
        item.artifactType = ArtifactType::Mesh;

        const auto request = BuildAssetThumbnailRequestForItem(root, item, 96u, context);
        ASSERT_TRUE(request.has_value());
        EXPECT_NE(request->dependencyStamp.find("source-file="), std::string::npos);
        EXPECT_NE(request->dependencyStamp.find("source-meta="), std::string::npos);
        EXPECT_NE(request->dependencyStamp.find("artifact-db="), std::string::npos);
    }

    EXPECT_TRUE(context.artifactDatabaseStampCached);
    const auto artifactDatabasePath = context.artifactDatabasePath.lexically_normal().generic_string();
    EXPECT_NE(artifactDatabasePath.find("/Library/ArtifactDB/data.mdb"), std::string::npos);
    EXPECT_EQ(context.fileStampsByPath.size(), 2u);
    EXPECT_EQ(context.sourcePathsByProjectAndAssetPath.size(), 1u);
    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, ThumbnailRequestBuildContextRefreshesArtifactDatabaseStampPerProjectRoot)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto firstRoot = MakeAssetBrowserPresentationRoot();
    const auto secondRoot = MakeAssetBrowserPresentationRoot();
    WriteTextFile(firstRoot / "Assets" / "Models" / "Hero.fbx", "first");
    WriteTextFile(secondRoot / "Assets" / "Models" / "Hero.fbx", "second");

    AssetBrowserItem item;
    item.displayName = "Hero.fbx";
    item.projectRelativePath = "Assets/Models/Hero.fbx";
    item.sourceAssetPath = item.projectRelativePath;
    item.subAssetKey = "mesh:Body";
    item.assetId = AssetId(NLS::Guid::NewDeterministic("thumbnail-request-artifact-db-root-cache"));
    item.kind = AssetBrowserItemKind::GeneratedSubAsset;
    item.type = AssetBrowserItemType::Mesh;
    item.artifactType = ArtifactType::Mesh;

    AssetThumbnailRequestBuildContext context;
    context.deferManifestLookups = true;
    ASSERT_TRUE(BuildAssetThumbnailRequestForItem(firstRoot, item, 96u, context).has_value());
    const auto firstArtifactDatabasePath = context.artifactDatabasePath;

    ASSERT_TRUE(BuildAssetThumbnailRequestForItem(secondRoot, item, 96u, context).has_value());

    EXPECT_NE(context.artifactDatabasePath, firstArtifactDatabasePath);
    const auto artifactDatabasePath = context.artifactDatabasePath.lexically_normal().generic_string();
    EXPECT_NE(artifactDatabasePath.find("/Library/ArtifactDB/data.mdb"), std::string::npos);
    EXPECT_NE(artifactDatabasePath.find(secondRoot.lexically_normal().generic_string()), std::string::npos);

    std::filesystem::remove_all(firstRoot);
    std::filesystem::remove_all(secondRoot);
}

TEST(AssetBrowserPresentationTests, ThumbnailRequestBuildContextCachesSourcePathPerProjectRoot)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto firstRoot = MakeAssetBrowserPresentationRoot();
    const auto secondRoot = MakeAssetBrowserPresentationRoot();
    WriteTextFile(firstRoot / "Assets" / "Hero.fbx", "first");
    WriteTextFile(secondRoot / "Assets" / "Hero.fbx", "second");

    AssetBrowserItem item;
    item.displayName = "Hero.fbx";
    item.projectRelativePath = "Assets/Hero.fbx";
    item.sourceAssetPath = item.projectRelativePath;
    item.assetId = AssetId(NLS::Guid::NewDeterministic("thumbnail-request-source-root-cache"));
    item.kind = AssetBrowserItemKind::SourceAsset;
    item.type = AssetBrowserItemType::Model;

    AssetThumbnailRequestBuildContext context;
    context.deferManifestLookups = true;
    ASSERT_TRUE(BuildAssetThumbnailRequestForItem(firstRoot, item, 96u, context).has_value());
    ASSERT_TRUE(BuildAssetThumbnailRequestForItem(secondRoot, item, 96u, context).has_value());

    EXPECT_EQ(context.sourcePathsByProjectAndAssetPath.size(), 2u);

    std::filesystem::remove_all(firstRoot);
    std::filesystem::remove_all(secondRoot);
}

TEST(AssetBrowserPresentationTests, ThumbnailSourceFreshnessRejectsSymlinkedAssetParent)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    const auto outside = root.parent_path() / ("nullus_thumbnail_source_outside_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(outside);
    WriteTextFile(outside / "Hero.fbx", "outside");

    std::error_code error;
    std::filesystem::create_directory_symlink(outside, root / "Assets" / "Linked", error);
    if (error)
    {
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(outside);
        GTEST_SKIP() << "Directory symlink creation is not available in this environment.";
    }

    AssetBrowserItem item;
    item.displayName = "Hero.fbx";
    item.projectRelativePath = "Assets/Linked/Hero.fbx";
    item.sourceAssetPath = item.projectRelativePath;
    item.assetId = AssetId(NLS::Guid::NewDeterministic("thumbnail-source-symlink-parent"));
    item.kind = AssetBrowserItemKind::SourceAsset;
    item.type = AssetBrowserItemType::Model;

    AssetThumbnailRequestBuildContext context;
    context.deferManifestLookups = true;
    const auto request = BuildAssetThumbnailRequestForItem(root, item, 96u, context);
    ASSERT_TRUE(request.has_value());
    EXPECT_NE(request->dependencyStamp.find("source-file=missing;"), std::string::npos);

    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside);
}

TEST(AssetBrowserPresentationTests, ThumbnailCacheKeyBindingsBackfillAllVisibleItems)
{
    using namespace NLS::Editor::Assets;

    std::unordered_map<std::string, std::vector<std::string>> itemKeysByCacheKey;
    std::unordered_map<std::string, std::string> thumbnailStatusByItemKey;

    RegisterAssetBrowserThumbnailCacheKeyBinding(
        itemKeysByCacheKey,
        "shared-cache-key",
        "Assets/Models/Hero.fbx#96");
    RegisterAssetBrowserThumbnailCacheKeyBinding(
        itemKeysByCacheKey,
        "shared-cache-key",
        "Assets/Models/Hero.fbx::mesh:Body#96");
    RegisterAssetBrowserThumbnailCacheKeyBinding(
        itemKeysByCacheKey,
        "shared-cache-key",
        "Assets/Models/Hero.fbx#96");

    ApplyAssetBrowserThumbnailCacheKeyResult(
        itemKeysByCacheKey,
        thumbnailStatusByItemKey,
        "shared-cache-key",
        "fresh");

    ASSERT_EQ(itemKeysByCacheKey["shared-cache-key"].size(), 2u);
    EXPECT_EQ(thumbnailStatusByItemKey["Assets/Models/Hero.fbx#96"], "fresh");
    EXPECT_EQ(thumbnailStatusByItemKey["Assets/Models/Hero.fbx::mesh:Body#96"], "fresh");
}

TEST(AssetBrowserPresentationTests, GridThumbnailDrawPathTelemetrySeparatesThumbnailFromFallback)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto body = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::DrawProjectGridItemThumbnail(");

    EXPECT_EQ(body.find("RecordAssetBrowserArtifactTelemetryStage"), std::string::npos)
        << "Draw outcome diagnostics must not push per-item records through the global artifact telemetry vector on the UI draw hot path.";
    EXPECT_NE(body.find("RecordAssetBrowserThumbnailDrawOutcomeTelemetry"), std::string::npos);
    EXPECT_NE(body.find("GetFrameCount"), std::string::npos)
        << "Draw outcome diagnostics should be sampled so validation does not add per-item work every UI frame.";
    EXPECT_NE(body.find("kAssetBrowserThumbnailDrawOutcomeSamplePeriodFrames"), std::string::npos);
    EXPECT_NE(body.find("|draw=thumbnail"), std::string::npos)
        << "Fresh thumbnail image draws need explicit telemetry so UI fallback regressions can be proven from validation runs.";
    EXPECT_NE(body.find("|draw=fallback"), std::string::npos)
        << "Explicit thumbnail fallback draws should remain distinguishable from correct generated thumbnail images.";
    EXPECT_NE(body.find("|draw=type-fallback"), std::string::npos)
        << "The type-icon path is the screenshot failure mode and must be counted separately.";
}

TEST(AssetBrowserPresentationTests, ThumbnailTelemetrySummaryReportsGridDrawOutcomes)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to isolate draw outcome telemetry.";
#else
    using namespace NLS::Editor::Panels;

    const bool wasEnabled = NLS::Core::Assets::IsArtifactLoadTelemetryEnabled();
    NLS::Core::Assets::SetArtifactLoadTelemetryEnabled(true);
    ClearAssetBrowserThumbnailDrawOutcomeTelemetryForTesting();
    RecordAssetBrowserThumbnailDrawOutcomeTelemetry("Assets/thumbnail.prefab", AssetBrowserThumbnailDrawOutcome::Thumbnail);
    RecordAssetBrowserThumbnailDrawOutcomeTelemetry("Assets/fallback.prefab", AssetBrowserThumbnailDrawOutcome::Fallback);
    RecordAssetBrowserThumbnailDrawOutcomeTelemetry("Assets/type-fallback.prefab", AssetBrowserThumbnailDrawOutcome::TypeFallback);

    const auto report = NLS::Editor::Core::Testing::BuildThumbnailTelemetrySummaryReportForTesting({});

    ClearAssetBrowserThumbnailDrawOutcomeTelemetryForTesting();
    NLS::Core::Assets::SetArtifactLoadTelemetryEnabled(wasEnabled);
    EXPECT_NE(report.find("Thumbnail draw outcomes"), std::string::npos);
    EXPECT_NE(report.find("- |draw=thumbnail records=1"), std::string::npos);
    EXPECT_NE(report.find("- |draw=fallback records=1"), std::string::npos);
    EXPECT_NE(report.find("- |draw=type-fallback records=1"), std::string::npos);
    EXPECT_NE(report.find("Assets/thumbnail.prefab|draw=thumbnail records=1"), std::string::npos);
    EXPECT_NE(report.find("Assets/fallback.prefab|draw=fallback records=1"), std::string::npos);
    EXPECT_NE(report.find("Assets/type-fallback.prefab|draw=type-fallback records=1"), std::string::npos);
#endif
}

TEST(AssetBrowserPresentationTests, ThumbnailTelemetrySummaryIncludesGlobalArtifactStageTotals)
{
    using namespace std::chrono_literals;
    using NLS::Core::Assets::ArtifactLoadTelemetryRecord;
    using NLS::Core::Assets::ArtifactLoadTelemetryStage;

    const std::vector<ArtifactLoadTelemetryRecord> records {
        { ArtifactLoadTelemetryStage::GpuUpload, 1500us, 10u, "texture-a" },
        { ArtifactLoadTelemetryStage::GpuUpload, 500us, 14u, "texture-b" },
        { ArtifactLoadTelemetryStage::CpuDeserialize, 2000us, 7u, "mesh-a" },
        { ArtifactLoadTelemetryStage::CacheHit, 2000us, 3u, "cache-a" },
        { ArtifactLoadTelemetryStage::ThumbnailGpuPreviewRender, 300us, 4u, "prefab-a" },
        { ArtifactLoadTelemetryStage::ThumbnailGpuPreviewRender, 200us, 6u, "prefab-b" }
    };

    const auto report = NLS::Editor::Core::Testing::BuildThumbnailTelemetrySummaryReportForTesting(records);
    EXPECT_EQ(
        report.find("- GpuUpload records=2 totalMs=2.000 totalBytes=24"),
        report.rfind("- GpuUpload records=2 totalMs=2.000 totalBytes=24"))
        << "One stage total must combine every path exactly once.";
    EXPECT_NE(
        report.find("ThumbnailGpuPreviewRender path=prefab-a records=1"),
        std::string::npos);
    EXPECT_NE(
        report.find("ThumbnailGpuPreviewRender path=prefab-b records=1"),
        std::string::npos);

    const auto cacheHit = report.find("- CacheHit records=1 totalMs=2.000 totalBytes=3");
    const auto cpuDeserialize = report.find("- CpuDeserialize records=1 totalMs=2.000 totalBytes=7");
    const auto gpuUpload = report.find("- GpuUpload records=2 totalMs=2.000 totalBytes=24");
    ASSERT_NE(cacheHit, std::string::npos);
    ASSERT_NE(cpuDeserialize, std::string::npos);
    ASSERT_NE(gpuUpload, std::string::npos);
    EXPECT_LT(cacheHit, cpuDeserialize);
    EXPECT_LT(cpuDeserialize, gpuUpload)
        << "Equal elapsed totals must use the stage name as a deterministic tie-break.";
}

TEST(AssetBrowserPresentationTests, TextureRevivalTestsUseBackendNeutralExplicitDevice)
{
    const auto source = ReadSourceText(RepoPath("Tests/Unit/AssetThumbnailBehaviorTests.cpp"));
    const auto helper = ExtractFunctionBody(
        source,
        "DeterministicThumbnailGpuTestContext EnsureDeterministicThumbnailGpuTestDriver()");
    const auto sharedRevival = ExtractFunctionBody(
        source,
        "TEST(AssetThumbnailBehaviorTests, TextureManagerSharedRequestRevivesCanceledInFlightArtifactBeforePump)");
    const auto cancelableRevival = ExtractFunctionBody(
        source,
        "TEST(AssetThumbnailBehaviorTests, TextureManagerCancelableRequestRevivesCanceledInFlightArtifactBeforePump)");

    EXPECT_NE(helper.find("EGraphicsBackend::NONE"), std::string::npos);
    EXPECT_NE(helper.find("DriverTestAccess::SetExplicitDevice"), std::string::npos);
    EXPECT_EQ(helper.find("defined(_WIN32)"), std::string::npos);
    EXPECT_EQ(helper.find("defined(__APPLE__)"), std::string::npos);
    EXPECT_EQ(helper.find("defined(__linux__)"), std::string::npos);
    EXPECT_NE(sharedRevival.find("EnsureDeterministicThumbnailGpuTestDriver"), std::string::npos);
    EXPECT_NE(cancelableRevival.find("EnsureDeterministicThumbnailGpuTestDriver"), std::string::npos);
    EXPECT_EQ(sharedRevival.find("EnsureThumbnailPerformanceGpuTestDriver"), std::string::npos);
    EXPECT_EQ(cancelableRevival.find("EnsureThumbnailPerformanceGpuTestDriver"), std::string::npos);
}

TEST(AssetBrowserPresentationTests, PendingThumbnailResultDoesNotReplaceFreshVisibleResult)
{
    using namespace NLS::Editor::Assets;

    std::unordered_map<std::string, std::vector<std::string>> itemKeysByCacheKey;
    std::unordered_map<std::string, AssetThumbnailServiceResult> resultsByItemKey;

    RegisterAssetBrowserThumbnailCacheKeyBinding(
        itemKeysByCacheKey,
        "shared-cache-key",
        "Assets/Models/Hero.fbx#96");

    AssetThumbnailServiceResult fresh;
    fresh.status = AssetThumbnailServiceStatus::Fresh;
    fresh.imagePath = "Library/AssetThumbnails/Hero.png";
    ApplyAssetBrowserThumbnailCacheKeyResult(
        itemKeysByCacheKey,
        resultsByItemKey,
        "shared-cache-key",
        fresh);

    AssetThumbnailServiceResult pending;
    pending.status = AssetThumbnailServiceStatus::Pending;
    pending.fallbackIcon = "editor.icon.asset.model";
    ApplyAssetBrowserThumbnailCacheKeyResult(
        itemKeysByCacheKey,
        resultsByItemKey,
        "shared-cache-key",
        pending);

    ASSERT_EQ(resultsByItemKey["Assets/Models/Hero.fbx#96"].status, AssetThumbnailServiceStatus::Fresh)
        << "A late pending GPU/readback result must not make an already-visible thumbnail disappear.";
    EXPECT_EQ(resultsByItemKey["Assets/Models/Hero.fbx#96"].imagePath, "Library/AssetThumbnails/Hero.png");
}

TEST(AssetBrowserPresentationTests, ThumbnailItemKeySeparatesGeneratedSubAssets)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserItem source;
    source.kind = AssetBrowserItemKind::SourceAsset;
    source.projectRelativePath = "Assets/Models/Hero.fbx";

    AssetBrowserItem mesh = source;
    mesh.kind = AssetBrowserItemKind::GeneratedSubAsset;
    mesh.sourceAssetPath = source.projectRelativePath;
    mesh.projectRelativePath = "Assets/Models/Hero.fbx::mesh:Body";
    mesh.subAssetKey = "mesh:Body";

    AssetBrowserItem material = source;
    material.kind = AssetBrowserItemKind::GeneratedSubAsset;
    material.sourceAssetPath = source.projectRelativePath;
    material.projectRelativePath = "Assets/Models/Hero.fbx::material:Body";
    material.subAssetKey = "material:Body";

    EXPECT_EQ(
        BuildAssetBrowserThumbnailItemKey(source, 96u),
        "Assets/Models/Hero.fbx#kind=1|type=10|artifact=0");
    EXPECT_EQ(
        BuildAssetBrowserThumbnailItemKey(mesh, 96u),
        "Assets/Models/Hero.fbx::mesh:Body#kind=2|type=10|artifact=0");
    EXPECT_EQ(
        BuildAssetBrowserThumbnailItemKey(material, 96u),
        "Assets/Models/Hero.fbx::material:Body#kind=2|type=10|artifact=0");
    EXPECT_EQ(BuildAssetBrowserThumbnailItemKey(source, 64u), BuildAssetBrowserThumbnailItemKey(source, 96u))
        << "Changing thumbnail size should keep displaying the previous cached image while the resized thumbnail is regenerated.";
    EXPECT_NE(BuildAssetBrowserThumbnailItemKey(mesh, 96u), BuildAssetBrowserThumbnailItemKey(material, 96u));

    mesh.type = AssetBrowserItemType::Mesh;
    mesh.artifactType = NLS::Core::Assets::ArtifactType::Mesh;
    material.type = AssetBrowserItemType::Material;
    material.artifactType = NLS::Core::Assets::ArtifactType::Material;
    EXPECT_NE(BuildAssetBrowserThumbnailItemKey(mesh, 96u), BuildAssetBrowserThumbnailItemKey(material, 96u));

    AssetBrowserItem staleWrongType = material;
    staleWrongType.type = AssetBrowserItemType::Prefab;
    staleWrongType.artifactType = NLS::Core::Assets::ArtifactType::Prefab;
    EXPECT_NE(
        BuildAssetBrowserThumbnailItemKey(material, 96u),
        BuildAssetBrowserThumbnailItemKey(staleWrongType, 96u))
        << "Sub-asset thumbnail results must not survive an artifact type correction and show a prefab icon for a material.";

    AssetBrowserItem replacement = material;
    material.assetId = ParseAssetId("11111111-1111-4111-8111-111111111111");
    replacement.assetId = ParseAssetId("22222222-2222-4222-8222-222222222222");
    EXPECT_NE(
        BuildAssetBrowserThumbnailItemKey(material, 96u),
        BuildAssetBrowserThumbnailItemKey(replacement, 96u))
        << "Replacing an asset at the same path must not bind a thumbnail produced for the old authoritative AssetId.";
}

TEST(AssetBrowserPresentationTests, ActionIdentityUsesAuthoritativeFieldsWithoutDelimiterParsing)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserItem source;
    source.kind = AssetBrowserItemKind::SourceAsset;
    source.projectRelativePath = "Assets/Models/Hero#Variant.fbx";
    source.sourceAssetPath = source.projectRelativePath;
    source.assetId = ParseAssetId("11111111-1111-4111-8111-111111111111");

    const auto sourceIdentity = BuildAssetBrowserActionIdentity(source);
    ASSERT_TRUE(sourceIdentity.has_value());
    EXPECT_EQ(sourceIdentity->kind, AssetBrowserItemKind::SourceAsset);
    EXPECT_EQ(sourceIdentity->canonicalSourcePath, source.sourceAssetPath);
    EXPECT_EQ(sourceIdentity->assetId, source.assetId);
    EXPECT_TRUE(sourceIdentity->subAssetKey.empty());
    EXPECT_TRUE(AssetBrowserItemMatchesActionIdentity(source, *sourceIdentity));

    AssetBrowserItem nonCanonicalSource = source;
    nonCanonicalSource.sourceAssetPath = "Assets\\Models\\Hero#Variant.fbx";
    EXPECT_TRUE(AssetBrowserItemMatchesActionIdentity(nonCanonicalSource, *sourceIdentity));

    AssetBrowserItem replacement = source;
    replacement.assetId = ParseAssetId("22222222-2222-4222-8222-222222222222");
    EXPECT_FALSE(AssetBrowserItemMatchesActionIdentity(replacement, *sourceIdentity));

    AssetBrowserItem child = source;
    child.kind = AssetBrowserItemKind::GeneratedSubAsset;
    child.projectRelativePath = source.sourceAssetPath + "::mesh:Body";
    child.subAssetKey = "mesh:Body#LOD::0";
    const auto childIdentity = BuildAssetBrowserActionIdentity(child);
    ASSERT_TRUE(childIdentity.has_value());
    EXPECT_EQ(childIdentity->canonicalSourcePath, source.sourceAssetPath);
    EXPECT_EQ(childIdentity->subAssetKey, child.subAssetKey);
    EXPECT_TRUE(AssetBrowserItemMatchesActionIdentity(child, *childIdentity));
    EXPECT_FALSE(AssetBrowserItemMatchesActionIdentity(source, *childIdentity));

    AssetBrowserItem sibling = child;
    sibling.subAssetKey = "mesh:Body#LOD::1";
    EXPECT_FALSE(AssetBrowserItemMatchesActionIdentity(sibling, *childIdentity));

    source.assetId = {};
    EXPECT_FALSE(BuildAssetBrowserActionIdentity(source).has_value())
        << "A source without an authoritative AssetId must fail closed instead of rebinding by path.";
}

TEST(AssetBrowserPresentationTests, PresentationBundleCountsOnlyMatchingAuthoritativeSnapshotChildren)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    AssetBrowserItem source;
    source.displayName = "Hero.fbx";
    source.kind = AssetBrowserItemKind::SourceAsset;
    source.type = AssetBrowserItemType::Model;
    source.projectRelativePath = "Assets/Models/Hero.fbx";
    source.sourceAssetPath = source.projectRelativePath;
    source.assetId = ParseAssetId("11111111-1111-4111-8111-111111111111");

    EditorAssetSnapshot snapshot;
    snapshot.sourceAssetPath = source.sourceAssetPath;
    snapshot.assetId = source.assetId;
    snapshot.subAssets = {
        { "mesh:Body", "Library/Artifacts/body.mesh", ArtifactType::Mesh, "Body" },
        { "material:Body", "Library/Artifacts/body.mat", ArtifactType::Material, "Body" },
        { "animation:Idle", "Library/Artifacts/idle.anim", ArtifactType::AnimationClip, "Idle" }
    };
    const auto index = BuildValidatedEditorAssetSnapshotIndex({snapshot});
    ASSERT_NE(index, nullptr);
    ASSERT_EQ(index->status, EditorAssetSnapshotStatus::Valid);

    AssetBrowserBuildOptions options;
    options.includeGeneratedSubAssets = true;
    auto collapsed = BuildAssetBrowserPresentationBundle({source}, index.get(), options);
    ASSERT_EQ(collapsed.displayItems.size(), 1u);
    EXPECT_EQ(collapsed.displayItems[0].childCount, 2u)
        << "Unsupported generated artifacts must affect neither disclosure count nor membership.";
    EXPECT_FALSE(collapsed.displayItems[0].expanded);

    options.expandedSourceAssets.insert(source.sourceAssetPath);
    auto expanded = BuildAssetBrowserPresentationBundle({source}, index.get(), options);
    ASSERT_EQ(expanded.displayItems.size(), 3u);
    EXPECT_EQ(expanded.displayItems[0].childCount, 2u);
    EXPECT_EQ(expanded.displayItems[1].item.subAssetKey, "mesh:Body");
    EXPECT_EQ(expanded.displayItems[2].item.subAssetKey, "material:Body");

    AssetBrowserItem replacement = source;
    replacement.assetId = ParseAssetId("22222222-2222-4222-8222-222222222222");
    auto staleSnapshot = BuildAssetBrowserPresentationBundle({replacement}, index.get(), options);
    ASSERT_EQ(staleSnapshot.displayItems.size(), 1u);
    EXPECT_EQ(staleSnapshot.displayItems[0].childCount, 0u)
        << "A same-path replacement must not inherit the old asset's disclosure button.";
}

TEST(AssetBrowserPresentationTests, RefreshedPrefabSourceRetainsPrimaryArtifactAsExpandableChild)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    NLS::Engine::GameObject gameObject("Lamp", "Prefab");
    const auto created = PrefabEditorWorkflow().CreatePrefabFromSelection({
        &gameObject,
        {},
        ParseAssetId("e5050505-0505-4505-8505-050505050505"),
        "Assets/Prefabs/Lamp.prefab"
    });
    ASSERT_EQ(created.status, PrefabEditorOperationStatus::Committed);
    ASSERT_FALSE(created.prefabSourceText.empty());

    {
        AssetDatabaseFacade database({root});
        ASSERT_TRUE(database.Refresh());
        ASSERT_TRUE(database.CreateTextAsset(
            created.prefabSourceText,
            "Assets/Prefabs/Lamp.prefab",
            ParseAssetId("e6060606-0606-4606-8606-060606060606")));
        ASSERT_TRUE(database.ImportAsset("Assets/Prefabs/Lamp.prefab"));
    }

    AssetDatabaseFacade refreshedDatabase({root});
    ASSERT_TRUE(refreshedDatabase.Refresh());
    const auto databaseSnapshot = AssetDatabaseFacade::CreateReadOnlySnapshot(refreshedDatabase);
    ASSERT_NE(databaseSnapshot, nullptr);
    const auto publishedState = refreshedDatabase.GetPublishedState();
    ASSERT_NE(publishedState, nullptr);
    ASSERT_NE(publishedState->snapshotIndex, nullptr);
    ASSERT_EQ(publishedState->snapshotIndex->status, EditorAssetSnapshotStatus::Valid);

    AssetBrowserBuildOptions rootOptions;
    rootOptions.includeGeneratedSubAssets = false;
    const auto rootItems = BuildCurrentFolderAssetItems(
        root,
        "Assets/Prefabs",
        databaseSnapshot.get(),
        rootOptions);
    ASSERT_EQ(rootItems.size(), 1u);
    EXPECT_EQ(rootItems[0].kind, AssetBrowserItemKind::SourceAsset);
    EXPECT_EQ(rootItems[0].type, AssetBrowserItemType::Prefab);
    ASSERT_TRUE(rootItems[0].assetId.IsValid());

    AssetBrowserBuildOptions presentationOptions;
    presentationOptions.includeGeneratedSubAssets = true;
    auto collapsed = BuildAssetBrowserPresentationBundle(
        rootItems,
        publishedState->snapshotIndex.get(),
        presentationOptions);
    ASSERT_EQ(collapsed.displayItems.size(), 1u);
    EXPECT_EQ(collapsed.displayItems[0].childCount, 1u);

    presentationOptions.expandedSourceAssets.insert("Assets/Prefabs/Lamp.prefab");
    auto expanded = BuildAssetBrowserPresentationBundle(
        rootItems,
        publishedState->snapshotIndex.get(),
        presentationOptions);
    ASSERT_EQ(expanded.displayItems.size(), 2u);
    EXPECT_EQ(expanded.displayItems[0].childCount, 1u);
    EXPECT_EQ(expanded.displayItems[1].item.subAssetKey, "prefab:Lamp");
    EXPECT_EQ(expanded.displayItems[1].item.artifactType, ArtifactType::Prefab);

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, PanelSelectionAndLegacyPathsDoNotParseActionIdentityDelimiters)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto resourcePath = ExtractFunctionBody(source, "std::string ProjectBrowserResourcePathForItem(");
    const auto selectionPath = ExtractFunctionBody(source, "std::string ProjectBrowserSelectionPathForItem(");
    EXPECT_EQ(resourcePath.find("find('#')"), std::string::npos);
    EXPECT_EQ(selectionPath.find("find('#')"), std::string::npos);
    EXPECT_NE(resourcePath.find("item.sourceAssetPath"), std::string::npos);
    EXPECT_NE(selectionPath.find("item.subAssetKey"), std::string::npos);

    const auto header = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.h"));
    EXPECT_NE(header.find("std::optional<NLS::Editor::Assets::AssetBrowserActionIdentity> m_selectedProjectItem"),
        std::string::npos);
    EXPECT_EQ(header.find("std::string m_selectedProjectItem"), std::string::npos);
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

    ASSERT_EQ(selected.size(), 3u);
    EXPECT_EQ(selected[0].type, AssetBrowserItemType::Material);
    EXPECT_EQ(selected[1].type, AssetBrowserItemType::Prefab);
    EXPECT_EQ(selected[2].type, AssetBrowserItemType::Texture);
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
    ASSERT_EQ(knownEmptyVisibleScope.size(), 2u);
    EXPECT_EQ(knownEmptyVisibleScope[0].projectRelativePath, material.projectRelativePath);
    EXPECT_EQ(knownEmptyVisibleScope[1].projectRelativePath, texture.projectRelativePath);

    const auto knownVisibleScope = SelectAssetBrowserThumbnailGenerationItems(
        currentFolderItems,
        std::vector<AssetBrowserItem> { texture },
        true);
    ASSERT_EQ(knownVisibleScope.size(), 2u);
    EXPECT_EQ(knownVisibleScope[0].projectRelativePath, texture.projectRelativePath);
    EXPECT_EQ(knownVisibleScope[1].projectRelativePath, material.projectRelativePath);
}

TEST(AssetBrowserPresentationTests, ThumbnailGenerationKeepsOffscreenSubAssetsOutOfVisibleScope)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserItem source;
    source.displayName = "Hero.gltf";
    source.projectRelativePath = "Assets/Models/Hero.gltf";
    source.sourceAssetPath = "Assets/Models/Hero.gltf";
    source.kind = AssetBrowserItemKind::SourceAsset;
    source.type = AssetBrowserItemType::Model;

    AssetBrowserItem visibleMesh;
    visibleMesh.displayName = "Body";
    visibleMesh.projectRelativePath = "Assets/Models/Hero.gltf::mesh:Body";
    visibleMesh.sourceAssetPath = "Assets/Models/Hero.gltf";
    visibleMesh.kind = AssetBrowserItemKind::GeneratedSubAsset;
    visibleMesh.type = AssetBrowserItemType::Mesh;

    AssetBrowserItem offscreenMaterial;
    offscreenMaterial.displayName = "BodyMat";
    offscreenMaterial.projectRelativePath = "Assets/Models/Hero.gltf::material:Body";
    offscreenMaterial.sourceAssetPath = "Assets/Models/Hero.gltf";
    offscreenMaterial.kind = AssetBrowserItemKind::GeneratedSubAsset;
    offscreenMaterial.type = AssetBrowserItemType::Material;

    const auto selected = SelectAssetBrowserThumbnailGenerationItems(
        std::vector<AssetBrowserItem> { source, visibleMesh, offscreenMaterial },
        std::vector<AssetBrowserItem> { source, visibleMesh },
        true);

    ASSERT_EQ(selected.size(), 2u);
    EXPECT_EQ(selected[0].projectRelativePath, source.projectRelativePath);
    EXPECT_EQ(selected[1].projectRelativePath, visibleMesh.projectRelativePath);
    EXPECT_NE(selected[0].projectRelativePath, offscreenMaterial.projectRelativePath);
    EXPECT_NE(selected[1].projectRelativePath, offscreenMaterial.projectRelativePath);
}

TEST(AssetBrowserPresentationTests, ThumbnailGenerationInteractiveScopeKeepsVisibleItemsOnly)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserItem source;
    source.displayName = "Hero.fbx";
    source.projectRelativePath = "Assets/Models/Hero.fbx";
    source.sourceAssetPath = "Assets/Models/Hero.fbx";
    source.kind = AssetBrowserItemKind::SourceAsset;
    source.type = AssetBrowserItemType::Model;

    AssetBrowserItem visibleMesh;
    visibleMesh.displayName = "Body";
    visibleMesh.projectRelativePath = "Assets/Models/Hero.fbx::mesh:Body";
    visibleMesh.sourceAssetPath = "Assets/Models/Hero.fbx";
    visibleMesh.kind = AssetBrowserItemKind::GeneratedSubAsset;
    visibleMesh.type = AssetBrowserItemType::Mesh;

    AssetBrowserItem offscreenMaterial = visibleMesh;
    offscreenMaterial.displayName = "BodyMat";
    offscreenMaterial.projectRelativePath = "Assets/Models/Hero.fbx::material:Body";
    offscreenMaterial.type = AssetBrowserItemType::Material;

    const auto selected = SelectAssetBrowserThumbnailGenerationItems(
        std::vector<AssetBrowserItem> {},
        std::vector<AssetBrowserItem> { source, visibleMesh },
        true);

    ASSERT_EQ(selected.size(), 2u);
    EXPECT_EQ(selected[0].projectRelativePath, source.projectRelativePath);
    EXPECT_EQ(selected[1].projectRelativePath, visibleMesh.projectRelativePath);
    EXPECT_NE(selected[0].projectRelativePath, offscreenMaterial.projectRelativePath);
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

TEST(AssetBrowserPresentationTests, GpuThumbnailPumpsRunOnlyFromBudgetedNonInteractivePump)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserLightGpuThumbnailPumpInput lightInput;
    lightInput.hasQueuedWork = true;
    lightInput.hasPreviewRenderer = true;
    lightInput.nowSeconds = 10.0;
    lightInput.deferredUntilSeconds = 8.0;
    lightInput.nextAllowedSeconds = 9.0;

    EXPECT_TRUE(PlanAssetBrowserLightGpuThumbnailPump(lightInput).shouldPump)
        << "Material GPU thumbnails are light preview work and must still run when prefab heavy previews are disabled.";

    lightInput.standardPbrShaderPassPrewarmPending = true;
    EXPECT_FALSE(PlanAssetBrowserLightGpuThumbnailPump(lightInput).shouldPump)
        << "Material GPU preview starts should let the background Standard PBR pass prewarm finish first; "
           "otherwise the first visible material thumbnail can still pay the cold shader pass load.";

    lightInput.standardPbrShaderPassPrewarmPending = false;
    lightInput.allowGpuPreviewStart = false;
    EXPECT_FALSE(PlanAssetBrowserLightGpuThumbnailPump(lightInput).shouldPump)
        << "Pumps that have not yet queued a visible scope must only consume completed GPU preview work, never synchronously start material previews.";

    lightInput.allowGpuPreviewStart = true;
    lightInput.interactive = true;
    EXPECT_FALSE(PlanAssetBrowserLightGpuThumbnailPump(lightInput).shouldPump)
        << "Visible material thumbnails should not synchronously start GPU preview work while the browser is interactive.";

    lightInput.interactive = false;
    lightInput.hasInFlightWork = true;
    EXPECT_TRUE(PlanAssetBrowserLightGpuThumbnailPump(lightInput).shouldPump)
        << "Background CPU thumbnail writes must not starve visible material GPU previews in large folders.";

    lightInput.hasInFlightWork = false;
    lightInput.interactive = false;
    lightInput.deferredUntilSeconds = 8.0;
    lightInput.nowSeconds = 8.5;
    EXPECT_FALSE(PlanAssetBrowserLightGpuThumbnailPump(lightInput).shouldPump);

    lightInput.nowSeconds = 9.5;
    lightInput.nextAllowedSeconds = 9.0;
    lightInput.deferredUntilSeconds = 10.0;
    EXPECT_FALSE(PlanAssetBrowserLightGpuThumbnailPump(lightInput).shouldPump)
        << "Light GPU preview starts still share the visible thumbnail pump budget, so they must wait until the browser has been idle.";

    AssetBrowserHeavyGpuThumbnailPumpInput input;
    input.hasQueuedWork = true;
    input.hasPreviewRenderer = true;
    input.nowSeconds = 10.0;
    input.deferredUntilSeconds = 8.0;
    input.nextAllowedSeconds = 9.0;

    EXPECT_TRUE(PlanAssetBrowserHeavyGpuThumbnailPump(input).shouldPump)
        << "GPU preview work must have a non-draw, budgeted pump so prefab/model thumbnails can finish.";

    input.allowHeavyGpuPreview = false;
    EXPECT_FALSE(PlanAssetBrowserHeavyGpuThumbnailPump(input).shouldPump)
        << "Pumps that have not yet queued a visible scope must not start prefab/model GPU preview work.";

    input.allowHeavyGpuPreview = true;
    input.interactive = true;
    EXPECT_FALSE(PlanAssetBrowserHeavyGpuThumbnailPump(input).shouldPump)
        << "Visible prefab/model thumbnails should wait for the browser to go idle before starting GPU preview work.";

    input.interactive = false;
    input.nowSeconds = 7.9;
    EXPECT_FALSE(PlanAssetBrowserHeavyGpuThumbnailPump(input).shouldPump);

    input.nowSeconds = 8.5;
    input.nextAllowedSeconds = 9.0;
    EXPECT_FALSE(PlanAssetBrowserHeavyGpuThumbnailPump(input).shouldPump);

    input.nowSeconds = 10.0;
    input.hasQueuedWork = false;
    input.hasInFlightWork = false;
    EXPECT_FALSE(PlanAssetBrowserHeavyGpuThumbnailPump(input).shouldPump);

    input.hasQueuedWork = true;
    input.hasInFlightWork = true;
    EXPECT_TRUE(PlanAssetBrowserHeavyGpuThumbnailPump(input).shouldPump)
        << "Background CPU thumbnail writes must not starve visible prefab GPU previews in large imported-model folders.";

    input.hasInFlightWork = false;
    input.sceneLoadRendererResourcesPending = true;
    EXPECT_FALSE(PlanAssetBrowserHeavyGpuThumbnailPump(input).shouldPump)
        << "Heavy prefab/model GPU preview work must not run while scene-load renderer resources still share the frame budget.";

    input.hasQueuedReadback = true;
    EXPECT_TRUE(PlanAssetBrowserHeavyGpuThumbnailPump(input).shouldPump)
        << "A pending GPU readback is a lightweight poll, not a new heavy prefab render, so it must continue even while scene-load resources are pending.";

    input.nextAllowedSeconds = 999.0;
    EXPECT_TRUE(PlanAssetBrowserHeavyGpuThumbnailPump(input).shouldPump)
        << "GPU readback polling must not wait behind the heavy preview cooldown; otherwise a completed readback can stay invisible indefinitely.";

    input.interactive = true;
    EXPECT_FALSE(PlanAssetBrowserHeavyGpuThumbnailPump(input).shouldPump)
        << "The browser still avoids even readback polling while interactive to keep drag/scroll responsive.";

    input.interactive = false;
    input.hasQueuedReadback = false;
    input.sceneLoadRendererResourcesPending = false;
    input.hasPreviewRenderer = false;
    EXPECT_FALSE(PlanAssetBrowserHeavyGpuThumbnailPump(input).shouldPump);

    input.hasPreviewRenderer = true;
    input.nowSeconds = 10.0;
    input.nextAllowedSeconds = 10.1;
    EXPECT_FALSE(PlanAssetBrowserHeavyGpuThumbnailPump(input).shouldPump);
}

TEST(AssetBrowserPresentationTests, PostDrawThumbnailPumpAllowsVisibleGpuPreviewWork)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserPostDrawThumbnailPumpInput input;
    input.nowSeconds = 10.0;
    input.lightDeferredUntilSeconds = 9.0;
    input.heavyDeferredUntilSeconds = 9.0;

    const auto permissions = PlanAssetBrowserPostDrawThumbnailPump(input);

    EXPECT_TRUE(permissions.allowGpuPreviewStart)
        << "Material sub-asset thumbnails are queued after visible items are drawn, so the post-draw pump must be allowed to start light GPU previews.";
    EXPECT_TRUE(permissions.allowHeavyGpuPreview)
        << "Heavy prefab/model previews can be scheduled post-draw because GPU preview rendering is dispatched through the background thumbnail path.";

    input.interactive = true;
    const auto interactivePermissions = PlanAssetBrowserPostDrawThumbnailPump(input);
    EXPECT_FALSE(interactivePermissions.allowGpuPreviewStart);
    EXPECT_FALSE(interactivePermissions.allowHeavyGpuPreview)
        << "Post-draw heavy GPU preview starts must wait while the browser is still scrolling or expanding items.";

    input.interactive = false;
    input.nowSeconds = 9.5;
    input.lightDeferredUntilSeconds = 10.0;
    input.heavyDeferredUntilSeconds = 10.0;
    const auto deferredPermissions = PlanAssetBrowserPostDrawThumbnailPump(input);
    EXPECT_FALSE(deferredPermissions.allowGpuPreviewStart);
    EXPECT_FALSE(deferredPermissions.allowHeavyGpuPreview)
        << "Post-draw GPU preview starts must wait for their matching idle cooldown.";

    input.lightDeferredUntilSeconds = 10.0;
    input.heavyDeferredUntilSeconds = 9.0;
    const auto heavyReadyPermissions = PlanAssetBrowserPostDrawThumbnailPump(input);
    EXPECT_FALSE(heavyReadyPermissions.allowGpuPreviewStart)
        << "Light material GPU previews must still honor their own idle cooldown.";
    EXPECT_TRUE(heavyReadyPermissions.allowHeavyGpuPreview)
        << "Heavy prefab/model previews must not be blocked by the lighter preview cooldown once their own cooldown has elapsed.";
}

TEST(AssetBrowserPresentationTests, DrawViewsPumpPostDrawGpuPreviewWorkAfterVisibleScopeIsQueued)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const std::array<std::string_view, 2> functions = {
        "void Editor::Panels::AssetBrowser::DrawCurrentFolderGrid()",
        "void Editor::Panels::AssetBrowser::DrawCurrentFolderList("
    };

    for (const auto function : functions)
    {
        const auto body = ExtractFunctionBody(source, function);
        const auto setVisible = body.find("SetVisibleThumbnailItems(");
        const auto updateScope = body.find("UpdateThumbnailGenerationScope()");
        const auto planPostDraw = body.find("PlanAssetBrowserPostDrawThumbnailPump({");
        const auto pump = body.find("PumpThumbnailGeneration(");

        EXPECT_NE(setVisible, std::string::npos) << function;
        EXPECT_NE(updateScope, std::string::npos) << function;
        EXPECT_NE(planPostDraw, std::string::npos) << function;
        EXPECT_NE(pump, std::string::npos) << function;
        if (setVisible != std::string::npos &&
            updateScope != std::string::npos &&
            planPostDraw != std::string::npos &&
            pump != std::string::npos)
        {
            EXPECT_LT(setVisible, updateScope) << function;
            EXPECT_LT(updateScope, planPostDraw) << function;
            EXPECT_LT(planPostDraw, pump) << function;
        }

        EXPECT_EQ(body.find("PumpThumbnailGeneration(false, false, false)"), std::string::npos)
            << "Visible post-draw thumbnail pumps must be allowed to start GPU preview work for material sub-assets and imported model previews.";
        EXPECT_NE(body.find("PumpThumbnailGeneration("), std::string::npos) << function;
        const auto heavyPermissionArgument = body.find("thumbnailPumpPermissions.allowHeavyGpuPreview", pump);
        EXPECT_NE(heavyPermissionArgument, std::string::npos) << function;
        EXPECT_NE(body.find("true);", heavyPermissionArgument), std::string::npos)
            << "Visible post-draw thumbnail pumps are the only place allowed to run render-path warmup.";

        EXPECT_NE(body.find("m_lightGpuThumbnailGenerationDeferredUntil"), std::string::npos) << function;
        EXPECT_NE(body.find("m_heavyGpuThumbnailGenerationDeferredUntil"), std::string::npos) << function;
    }
}

TEST(AssetBrowserPresentationTests, GpuThumbnailCooldownAdvancesOnlyAfterSuccessfulStart)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto body = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::PumpThumbnailGeneration(");

    const auto lightStart = body.find("m_thumbnailService.GenerateNextThumbnail(*m_thumbnailPreviewRenderer, false)");
    const auto lightStartedBlock = body.find("if (generated.has_value())", lightStart);
    const auto lightCooldown = body.find("m_nextGpuThumbnailGenerationTime = now + kAssetBrowserGpuThumbnailIntervalSeconds", lightStartedBlock);
    const auto heavyStart = body.find("m_thumbnailService.GenerateNextThumbnail(*m_thumbnailPreviewRenderer, true)");
    const auto heavyStartedBlock = body.find("if (generated.has_value())", heavyStart);
    const auto heavyCooldown = body.find("m_nextHeavyGpuThumbnailGenerationTime", heavyStartedBlock);

    ASSERT_NE(lightStart, std::string::npos);
    ASSERT_NE(lightStartedBlock, std::string::npos);
    ASSERT_NE(lightCooldown, std::string::npos)
        << "A failed light GPU preview start, usually because another preview is in flight, must not delay the next poll.";
    ASSERT_NE(heavyStart, std::string::npos);
    ASSERT_NE(heavyStartedBlock, std::string::npos);
    ASSERT_NE(heavyCooldown, std::string::npos)
        << "A failed heavy GPU preview start must not extend the prefab/model preview cooldown.";

    EXPECT_LT(lightStart, lightStartedBlock);
    EXPECT_LT(lightStartedBlock, lightCooldown);
    EXPECT_LT(heavyStart, heavyStartedBlock);
    EXPECT_LT(heavyStartedBlock, heavyCooldown);
}

TEST(AssetBrowserPresentationTests, PendingGpuPreviewWaitsForThreadedRetirementWithoutSynchronousDrain)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp"));
    const auto renderBody = ExtractFunctionBody(
        source,
        "void RenderCurrentPreviewScene(");
    const auto queuedReadback = renderBody.find("SetNextFramePostSubmitTextureReadback");
    const auto publishResult = renderBody.find("WasLastThreadedFramePublished", queuedReadback);
    const auto keepAlive = renderBody.find("threadedReadback.renderInputsKeepAlive = CapturePreviewRenderInputsKeepAlive()", publishResult);
    const auto pendingDiagnostic = renderBody.find("thumbnail-gpu-preview-readback-pending", publishResult);

    EXPECT_EQ(renderBody.find("TryDrainThreadedRendering"), std::string::npos)
        << "GPU thumbnail submission must not synchronously drain the threaded renderer on the editor UI thread.";
    EXPECT_NE(keepAlive, std::string::npos)
        << "Threaded preview commands can outlive Render(), so their scene inputs must remain alive.";
    EXPECT_NE(queuedReadback, std::string::npos)
        << "The texture readback must travel with the threaded GPU frame that renders the preview.";
    EXPECT_NE(publishResult, std::string::npos)
        << "The preview must distinguish a published frame from lifecycle backpressure without comparing non-unique frame ids.";
    EXPECT_NE(pendingDiagnostic, std::string::npos);

    const auto topLevelRenderBody = ExtractFunctionBody(
        source,
        "EditorThumbnailPreviewResult Render(const AssetThumbnailRequest& request)");
    EXPECT_NE(topLevelRenderBody.find("m_pendingReadback.active"), std::string::npos)
        << "Later Render() pumps must poll the post-submit readback without blocking.";
}

TEST(AssetBrowserPresentationTests, RetiredGpuPreviewReadbacksKeepSceneInputsUntilCompletionOnly)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp"));
    const auto retireBody = ExtractFunctionBody(
        source,
        "bool RetirePreviewReadback(");

    EXPECT_EQ(retireBody.find("pop_front"), std::string::npos)
        << "A superseded readback may still have GPU work using its preview inputs; capacity pruning must never drop incomplete keep-alives.";
    EXPECT_NE(retireBody.find("Poll().IsComplete()"), std::string::npos)
        << "Retired readbacks should only release keep-alives after completion polling says the GPU/readback work is done.";
    EXPECT_NE(retireBody.find("return false"), std::string::npos)
        << "When every retired readback is still incomplete, retirement must apply backpressure instead of growing the queue past its cap.";
    EXPECT_LT(retireBody.find("return false"), retireBody.find("readbacks.push_back"))
        << "The cap check must happen before storing another retired readback.";

    const auto retirePendingBody = ExtractFunctionBody(source, "bool RetirePendingReadback()");
    EXPECT_NE(retirePendingBody.find("return false"), std::string::npos)
        << "Callers need to know when a pending readback could not be retired safely.";

    const auto renderBody = ExtractFunctionBody(
        source,
        "EditorThumbnailPreviewResult Render(const AssetThumbnailRequest& request)");
    EXPECT_NE(renderBody.find("if (!RetirePendingReadback())"), std::string::npos)
        << "Starting a different preview must wait when the retired readback queue is full of incomplete GPU work.";
    EXPECT_NE(renderBody.find("thumbnail-gpu-preview-readback-pending"), std::string::npos);

    const auto destructorBody = ExtractFunctionBody(
        source,
        "~Impl()");
    const auto resetKeepAlive = destructorBody.find("m_pendingReadback.renderInputsKeepAlive.reset()");
    const auto retirePending = destructorBody.find("RetirePendingReadback()");

    EXPECT_NE(resetKeepAlive, std::string::npos)
        << "Renderer teardown must not move scene-owned keep-alives into a global retired queue.";
    EXPECT_NE(retirePending, std::string::npos);
    if (resetKeepAlive != std::string::npos && retirePending != std::string::npos)
        EXPECT_LT(resetKeepAlive, retirePending);
}

TEST(AssetBrowserPresentationTests, GpuPreviewReadbackWaitsForRetiredReadbacksBeforeStartingAnotherReadback)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp"));
    const auto beginPreviewReadbackBody = ExtractFunctionBody(
        source,
        "void BeginPreviewReadback(");

    const auto beforeBeginReadback = beginPreviewReadbackBody.find("WaitForRetiredPreviewReadbacksBeforeStartingReadback()");
    const auto beginReadback = beginPreviewReadbackBody.find("BeginReadPixels(");
    const auto pendingDiagnostic = beginPreviewReadbackBody.find(
        "thumbnail-gpu-preview-readback-pending",
        beforeBeginReadback);

    ASSERT_NE(beginReadback, std::string::npos);
    EXPECT_NE(beforeBeginReadback, std::string::npos)
        << "DX12 exposes one async readback slot; a retired thumbnail readback must be polled/finished before starting the next GPU preview readback.";
    EXPECT_NE(pendingDiagnostic, std::string::npos)
        << "If the retired readback is still pending, the renderer must return pending instead of calling BeginReadPixels and converting backend backpressure into a failure.";
    if (beforeBeginReadback != std::string::npos)
        EXPECT_LT(beforeBeginReadback, beginReadback);
}

TEST(AssetBrowserPresentationTests, StableThumbnailItemKeyStillRequestsChangedThumbnailSize)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto body = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::UpdateThumbnailGenerationScope()");

    const auto buildRequest = body.find("BuildAssetThumbnailRequestForItem");
    const auto currentCacheKey = body.find("BuildAssetThumbnailCacheKey(*request)", buildRequest);
    const auto matchingHelper = body.find("ThumbnailResultMatchesRequestCacheKey", currentCacheKey);
    const auto freshContinue = body.find("continue;", matchingHelper);

    ASSERT_NE(buildRequest, std::string::npos)
        << "The generation scope must build a current-size request before deciding whether an existing stable item-key result can skip work.";
    ASSERT_NE(currentCacheKey, std::string::npos)
        << "The current requested size only lives in the cache key now that the UI item key is stable.";
    ASSERT_NE(matchingHelper, std::string::npos);
    ASSERT_NE(freshContinue, std::string::npos);
    EXPECT_LT(buildRequest, currentCacheKey);
    EXPECT_LT(currentCacheKey, matchingHelper);
    EXPECT_EQ(body.find(
                  "AssetBrowserThumbnailResultHasDisplayImage(foundThumbnail->second))\n\t\t\t\t{\n\t\t\t\t\tconst auto key",
                  body.find("m_thumbnailResultsByItemKey.find")),
              std::string::npos)
        << "A fresh display image may be queued for display, but it must not skip generation until its cache entry matches the current request size.";
}

TEST(AssetBrowserPresentationTests, PendingThumbnailResultDoesNotSkipFreshCachePromotion)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto body = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::UpdateThumbnailGenerationScope()");

    const auto currentCacheKey = body.find("BuildAssetThumbnailCacheKey(*request)");
    const auto pendingStatus = body.find("AssetThumbnailServiceStatus::Pending", currentCacheKey);
    const auto cacheEvaluation = body.find("EvaluateAssetThumbnailCache", currentCacheKey);
    const auto pendingPromotion = body.find("PromoteFreshThumbnailCache(evaluation)", cacheEvaluation);
    const auto guardedContinue = body.find("if (PromoteFreshThumbnailCache(evaluation))", cacheEvaluation);
    const auto pendingContinue = body.find("continue;", pendingPromotion);

    ASSERT_NE(currentCacheKey, std::string::npos);
    ASSERT_NE(pendingStatus, std::string::npos)
        << "The stale Pending-result fast path must stay explicit so it can refresh completed cache files.";
    ASSERT_NE(cacheEvaluation, std::string::npos)
        << "Visible thumbnail requests must re-check the on-disk cache so completed GPU thumbnails can be promoted to Fresh.";
    ASSERT_NE(pendingPromotion, std::string::npos);
    ASSERT_NE(guardedContinue, std::string::npos)
        << "Only a successful Fresh cache promotion may skip the normal thumbnail service request path.";
    ASSERT_NE(pendingContinue, std::string::npos);
    EXPECT_GT(pendingContinue, cacheEvaluation)
        << "A stale Pending UI result with a matching cache key must not skip before the fresh cache check.";
}

TEST(AssetBrowserPresentationTests, PreDrawThumbnailPumpDoesNotStartHeavyGpuPreviewWork)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto body = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::OnBeforeDrawWidgets()");

    EXPECT_NE(body.find("PumpThumbnailGeneration(true, false, false)"), std::string::npos)
        << "The pre-draw pump runs before the current visible grid/list scope is rebuilt, so it must not start heavy prefab/model previews or render-path warmup from the previous visible range.";
    EXPECT_EQ(body.find("PumpThumbnailGeneration(true, true"), std::string::npos)
        << "Heavy GPU previews are reserved for the post-draw pump after visible thumbnail requests are queued.";
}

TEST(AssetBrowserPresentationTests, ThumbnailPumpRendersGpuPreviewOnMainThread)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto body = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::PumpThumbnailGeneration(");

    EXPECT_NE(body.find("GenerateNextThumbnail(*m_thumbnailPreviewRenderer, false)"), std::string::npos)
        << "Light GPU previews must render through the main-thread thumbnail pump because the preview renderer uses the graphics driver.";
    EXPECT_NE(body.find("GenerateNextThumbnail(*m_thumbnailPreviewRenderer, true)"), std::string::npos)
        << "Heavy GPU previews must render through the main-thread thumbnail pump because the preview renderer uses the graphics driver.";
    EXPECT_EQ(body.find("StartNextThumbnailGeneration(m_thumbnailPreviewRenderer"), std::string::npos)
        << "The asset browser must not schedule GPU preview rendering on a background worker.";
}

TEST(AssetBrowserPresentationTests, ThumbnailPumpConsumesCompletedThumbnailBatchPerPump)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto body = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::PumpThumbnailGeneration(");

    EXPECT_NE(source.find("kMaxAssetBrowserCompletedThumbnailConsumesPerPump"), std::string::npos)
        << "Completed thumbnails should be drained in a small UI pump budget; consuming only one result per pump makes cached/generated previews appear slowly.";
    EXPECT_NE(body.find("completedThumbnailsConsumedThisPump"), std::string::npos);
    EXPECT_NE(body.find("while (completedThumbnailsConsumedThisPump < kMaxAssetBrowserCompletedThumbnailConsumesPerPump)"), std::string::npos)
        << "The thumbnail pump should apply a bounded batch of already-completed background results before starting more work.";
}

TEST(AssetBrowserPresentationTests, ThumbnailPumpStartsTextureDecodeAfterCompletedThumbnailResults)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto body = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::PumpThumbnailGeneration(");

    const auto completedFlag = body.find("completedThumbnailQueuedTextureLoad");
    const auto applyResult = body.find("ApplyThumbnailServiceResult(*generated)");
    const auto startDecode = body.find("StartQueuedCachedThumbnailTextureDecodes(", applyResult);

    EXPECT_NE(completedFlag, std::string::npos)
        << "Fresh completed thumbnail images should immediately request cached texture decoding instead of waiting for the next frame.";
    EXPECT_NE(applyResult, std::string::npos);
    EXPECT_NE(startDecode, std::string::npos)
        << "Starting background thumbnail texture decodes in the same pump removes a frame of display latency without doing GPU upload synchronously.";
}

TEST(AssetBrowserPresentationTests, ThumbnailLatencyTelemetryStagesAreRecordable)
{
    using namespace std::chrono_literals;
    using NLS::Core::Assets::ArtifactLoadTelemetryStage;

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailGpuPreviewRender,
        100us,
        1u,
        "gpu-preview"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareResources,
        110us,
        1u,
        "gpu-preview-prepare"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareMaterialResources,
        111us,
        1u,
        "gpu-preview-prepare-material"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareSceneObjects,
        112us,
        1u,
        "gpu-preview-prepare-scene"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpDependencies,
        115us,
        1u,
        "gpu-preview-pump"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMeshDependencies,
        116us,
        1u,
        "gpu-preview-pump-mesh"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialDependencies,
        117us,
        1u,
        "gpu-preview-pump-material"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpTextureDependencies,
        118us,
        1u,
        "gpu-preview-pump-texture"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailGpuPreviewRecord,
        120us,
        1u,
        "gpu-preview-record"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailGpuPreviewSubmit,
        130us,
        1u,
        "gpu-preview-submit"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailGpuPreviewDrain,
        135us,
        1u,
        "gpu-preview-drain"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailGpuPreviewCleanup,
        136us,
        1u,
        "gpu-preview-cleanup"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailGpuPreviewReadback,
        140us,
        1u,
        "gpu-preview-readback"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPollReadback,
        145us,
        1u,
        "gpu-preview-poll-readback"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTextureDecode,
        200us,
        2u,
        "decode"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTextureUploadEnqueue,
        250us,
        3u,
        "upload-enqueue"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTextureUpload,
        300us,
        3u,
        "upload"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTextureUploadCreate,
        310us,
        3u,
        "upload-create"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTextureUploadPreparePixels,
        315us,
        3u,
        "upload-prepare-pixels"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTextureUploadSubmit,
        318us,
        3u,
        "upload-submit"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTextureUploadCreateView,
        320us,
        3u,
        "upload-view"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTextureUploadPublish,
        325us,
        3u,
        "upload-publish"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTextureUploadResolveUiId,
        330us,
        3u,
        "upload-resolve"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiDraw,
        400us,
        4u,
        "draw"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiDrawGridVisibleRows,
        401us,
        4u,
        "draw-visible-rows"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiDrawGridItemInteractions,
        402us,
        4u,
        "draw-interactions"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiDrawGridItemThumbnail,
        403us,
        4u,
        "draw-thumbnail"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiDrawGridItemLabel,
        404us,
        4u,
        "draw-label"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSet,
        405us,
        4u,
        "draw-visible-set"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSetHash,
        405us,
        4u,
        "draw-visible-set-hash"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSetApply,
        405us,
        4u,
        "draw-visible-set-apply"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSetHotCacheFlush,
        405us,
        4u,
        "draw-visible-set-flush"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScope,
        406us,
        4u,
        "draw-generation-scope"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeSelectItems,
        407us,
        4u,
        "draw-generation-scope-select"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildKey,
        408us,
        4u,
        "draw-generation-scope-key"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeItemKey,
        408us,
        4u,
        "draw-generation-scope-item-key"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeResultLookup,
        408us,
        4u,
        "draw-generation-scope-result-lookup"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequest,
        409us,
        4u,
        "draw-generation-scope-request"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestValidate,
        409us,
        4u,
        "draw-generation-scope-request-validate"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestMetaId,
        409us,
        4u,
        "draw-generation-scope-request-meta"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestManifestLookup,
        409us,
        4u,
        "draw-generation-scope-request-manifest"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestItemIdentity,
        409us,
        4u,
        "draw-generation-scope-request-identity"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshness,
        409us,
        4u,
        "draw-generation-scope-request-source"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestArtifactFreshness,
        409us,
        4u,
        "draw-generation-scope-request-artifact"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestDependencyStamp,
        409us,
        4u,
        "draw-generation-scope-request-stamp"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeRequestPreview,
        410us,
        4u,
        "draw-generation-scope-preview"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailServiceRequestStableLookup,
        410us,
        4u,
        "service-stable-lookup"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailServiceRequestCacheEvaluate,
        411us,
        4u,
        "service-cache-evaluate"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailServiceRequestQueue,
        412us,
        4u,
        "service-queue"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTexturePump,
        411us,
        4u,
        "texture-pump"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTexturePumpConsumeCompleted,
        412us,
        4u,
        "texture-pump-consume"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadPoll,
        413us,
        4u,
        "texture-pump-pending-upload"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadConsumeResult,
        413us,
        4u,
        "texture-pump-pending-upload-consume"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadResolveUiId,
        413us,
        4u,
        "texture-pump-pending-upload-resolve"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadWrapTexture,
        413us,
        4u,
        "texture-pump-pending-upload-wrap"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadCachePublish,
        413us,
        4u,
        "texture-pump-pending-upload-publish"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTexturePumpReadyDecodePoll,
        414us,
        4u,
        "texture-pump-ready-decode"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTexturePumpReadyDecodeLoad,
        415us,
        4u,
        "texture-pump-ready-decode-load"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTexturePumpStartDecodes,
        416us,
        4u,
        "texture-pump-start-decodes"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTexturePumpBuildResidentSet,
        417us,
        4u,
        "texture-pump-resident-set"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTexturePumpSelectDecodeCandidates,
        418us,
        4u,
        "texture-pump-select-decodes"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTexturePumpScheduleDecodeJobs,
        419us,
        4u,
        "texture-pump-schedule-decodes"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTextureUploadDeferred,
        0us,
        1u,
        "upload-deferred"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPump,
        420us,
        4u,
        "post-draw-pump"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpConsumeCompleted,
        421us,
        4u,
        "post-draw-pump-consume"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpCreatePreviewRenderer,
        422us,
        4u,
        "post-draw-pump-create-renderer"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpStartLightGpu,
        423us,
        4u,
        "post-draw-pump-light"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpStartHeavyGpu,
        423us,
        4u,
        "post-draw-pump-heavy"});
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpStartBackground,
        424us,
        4u,
        "post-draw-pump-background"});

    const auto summaries = NLS::Core::Assets::SummarizeArtifactLoadTelemetry();
    auto hasStage = [&summaries](const ArtifactLoadTelemetryStage stage)
    {
        return std::any_of(
            summaries.begin(),
            summaries.end(),
            [stage](const NLS::Core::Assets::ArtifactLoadTelemetryStageSummary& summary)
            {
                return summary.stage == stage && summary.recordCount == 1u;
            });
    };

    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewRender));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareResources));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareMaterialResources));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareSceneObjects));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpDependencies));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMeshDependencies));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialDependencies));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpTextureDependencies));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewRecord));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewSubmit));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewDrain));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewCleanup));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewReadback));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPollReadback));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailTextureDecode));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailTextureUploadEnqueue));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailTextureUpload));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailTextureUploadCreate));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailTextureUploadPreparePixels));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailTextureUploadSubmit));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailTextureUploadCreateView));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailTextureUploadPublish));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailTextureUploadResolveUiId));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiDraw));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiDrawGridVisibleRows));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiDrawGridItemInteractions));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiDrawGridItemThumbnail));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiDrawGridItemLabel));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSet));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSetHash));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSetApply));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSetHotCacheFlush));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScope));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeSelectItems));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildKey));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeItemKey));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeResultLookup));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequest));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestValidate));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestMetaId));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestManifestLookup));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestItemIdentity));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshness));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestArtifactFreshness));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestDependencyStamp));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeRequestPreview));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailServiceRequestStableLookup));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailServiceRequestCacheEvaluate));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailServiceRequestQueue));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailTexturePump));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailTexturePumpConsumeCompleted));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadPoll));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadConsumeResult));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadResolveUiId));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadWrapTexture));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadCachePublish));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailTexturePumpReadyDecodePoll));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailTexturePumpReadyDecodeLoad));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailTexturePumpStartDecodes));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailTexturePumpBuildResidentSet));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailTexturePumpSelectDecodeCandidates));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailTexturePumpScheduleDecodeJobs));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailTextureUploadDeferred));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPump));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpConsumeCompleted));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpCreatePreviewRenderer));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpStartLightGpu));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpStartHeavyGpu));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpStartBackground));
    NLS::Core::Assets::ClearArtifactLoadTelemetry();
}

TEST(AssetBrowserPresentationTests, ThumbnailLatencyTelemetryIsWiredToPreviewDecodeUploadAndDraw)
{
    const auto thumbnailService = ReadSourceText(RepoPath("Project/Editor/Assets/AssetThumbnailService.cpp"));
    const auto assetBrowser = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto rhiThreadCoordinator = ReadSourceText(RepoPath("Runtime/Rendering/Context/RhiThreadCoordinator.cpp"));
    const auto editorCore = ReadSourceText(RepoPath("Project/Editor/Core/Editor.cpp"));

    EXPECT_NE(
        thumbnailService.find("ArtifactLoadTelemetryStage::ThumbnailGpuPreviewRender"),
        std::string::npos)
        << "GPU preview render telemetry should wrap previewRenderer.Render so prefab/material render cost is visible.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailTextureDecode"),
        std::string::npos)
        << "PNG decode telemetry should measure background cached thumbnail image decode.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailTextureUploadEnqueue"),
        std::string::npos)
        << "Texture upload enqueue telemetry should measure cached thumbnail upload request submission separately from renderer-thread upload work.";
    EXPECT_NE(
        rhiThreadCoordinator.find("ArtifactLoadTelemetryStage::ThumbnailTextureUpload"),
        std::string::npos)
        << "Texture upload telemetry should measure renderer-thread texture creation/upload recording after UI enqueue.";
    EXPECT_NE(
        rhiThreadCoordinator.find("ArtifactLoadTelemetryStage::ThumbnailTextureUploadCreate"),
        std::string::npos)
        << "Texture upload telemetry should identify renderer-thread texture creation/upload recording.";
    EXPECT_NE(
        rhiThreadCoordinator.find("ArtifactLoadTelemetryStage::ThumbnailTextureUploadPreparePixels"),
        std::string::npos)
        << "Texture upload telemetry should identify staging allocation and row-copy cost separately from GPU submission.";
    EXPECT_NE(
        rhiThreadCoordinator.find("ArtifactLoadTelemetryStage::ThumbnailTextureUploadSubmit"),
        std::string::npos)
        << "Texture upload telemetry should identify SubmitUploadTexture backend recording cost separately from staging copies.";
    EXPECT_NE(
        rhiThreadCoordinator.find("ArtifactLoadTelemetryStage::ThumbnailTextureUploadCreateView"),
        std::string::npos)
        << "Texture upload telemetry should identify renderer-thread texture-view creation.";
    EXPECT_NE(
        rhiThreadCoordinator.find("ArtifactLoadTelemetryStage::ThumbnailTextureUploadPublish"),
        std::string::npos)
        << "Texture upload telemetry should identify completion publication cost so mutex contention is visible.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailTextureUploadResolveUiId"),
        std::string::npos)
        << "Texture upload telemetry should identify UI texture-id registration as its own UI-thread cost.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailTexturePump"),
        std::string::npos)
        << "Texture pump telemetry should measure OnBeforeDraw decode/upload pumping separately from UI draw.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailTexturePumpConsumeCompleted"),
        std::string::npos)
        << "Texture pump telemetry should split completed decode/upload consumption from decode scheduling.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadPoll"),
        std::string::npos)
        << "Texture pump telemetry should isolate pending renderer-thread upload completion polling.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadConsumeResult"),
        std::string::npos)
        << "Texture pump telemetry should isolate pending upload result consumption from UI texture publication.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadResolveUiId"),
        std::string::npos)
        << "Texture pump telemetry should isolate pending upload UI texture-id registration.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadWrapTexture"),
        std::string::npos)
        << "Texture pump telemetry should isolate Texture2D wrapping for completed thumbnail uploads.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadCachePublish"),
        std::string::npos)
        << "Texture pump telemetry should isolate thumbnail texture cache/LRU publication.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailTexturePumpReadyDecodePoll"),
        std::string::npos)
        << "Texture pump telemetry should isolate ready decode future polling.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailTexturePumpReadyDecodeLoad"),
        std::string::npos)
        << "Texture pump telemetry should isolate decoded PNG handoff into UI texture upload requests.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailTexturePumpStartDecodes"),
        std::string::npos)
        << "Texture pump telemetry should isolate decode-job scheduling work.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailTexturePumpBuildResidentSet"),
        std::string::npos)
        << "Texture pump telemetry should isolate resident texture set construction.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailTexturePumpSelectDecodeCandidates"),
        std::string::npos)
        << "Texture pump telemetry should isolate queued thumbnail decode candidate selection.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailTexturePumpScheduleDecodeJobs"),
        std::string::npos)
        << "Texture pump telemetry should isolate background decode job submission.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailTextureUploadDeferred"),
        std::string::npos)
        << "Deferred upload telemetry should show when decoded thumbnails are held back during interactive scrolling.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailUiDraw"),
        std::string::npos)
        << "Asset Browser draw telemetry should measure visible grid/list UI draw cost.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGridVisibleRows"),
        std::string::npos)
        << "Grid draw telemetry should isolate visible row/item loop cost from thumbnail pump work.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGridItemInteractions"),
        std::string::npos)
        << "Grid draw telemetry should isolate per-item ImGui interaction, drag, context menu, and drop-target cost.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGridItemThumbnail"),
        std::string::npos)
        << "Grid draw telemetry should isolate thumbnail handle resolution and image draw cost.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGridItemLabel"),
        std::string::npos)
        << "Grid draw telemetry should isolate label ellipsize, text measurement, and text draw cost.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSet"),
        std::string::npos)
        << "Grid draw telemetry should isolate visible thumbnail set hashing/update cost.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSetHash"),
        std::string::npos)
        << "Visible-set telemetry should isolate visible thumbnail fingerprint cost.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSetApply"),
        std::string::npos)
        << "Visible-set telemetry should isolate state replacement cost.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSetHotCacheFlush"),
        std::string::npos)
        << "Visible-set telemetry should isolate prefab hot-cache flush cost.";
    EXPECT_NE(
        thumbnailService.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestMetaId"),
        std::string::npos)
        << "Request-build telemetry should identify slow source .meta GUID resolution.";
    EXPECT_NE(
        thumbnailService.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestManifestLookup"),
        std::string::npos)
        << "Request-build telemetry should identify manifest lookup cost separately from file stamps.";
    EXPECT_NE(
        thumbnailService.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestItemIdentity"),
        std::string::npos)
        << "Request-build telemetry should identify item freshness identity cost.";
    EXPECT_NE(
        thumbnailService.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshness"),
        std::string::npos)
        << "Request-build telemetry should identify source/meta file stamp cost.";
    EXPECT_NE(
        thumbnailService.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestArtifactFreshness"),
        std::string::npos)
        << "Request-build telemetry should identify artifact DB/file stamp cost.";
    EXPECT_NE(
        thumbnailService.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestDependencyStamp"),
        std::string::npos)
        << "Request-build telemetry should identify dependency stamp sorting/string build cost.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScope"),
        std::string::npos)
        << "Grid draw telemetry should isolate thumbnail generation scope update cost.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeSelectItems"),
        std::string::npos)
        << "Generation-scope telemetry should isolate visible/current item selection and copy cost.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildKey"),
        std::string::npos)
        << "Generation-scope telemetry should isolate scope-key construction cost.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeItemKey"),
        std::string::npos)
        << "Generation-scope telemetry should isolate per-item thumbnail key construction cost.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeResultLookup"),
        std::string::npos)
        << "Generation-scope telemetry should isolate cached thumbnail result lookup cost.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequest"),
        std::string::npos)
        << "Generation-scope telemetry should isolate request construction cost.";
    EXPECT_NE(
        thumbnailService.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestValidate"),
        std::string::npos)
        << "Request-build telemetry should identify validation and early-return cost.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeRequestPreview"),
        std::string::npos)
        << "Generation-scope telemetry should isolate thumbnail service request/lookup cost.";
    EXPECT_NE(
        thumbnailService.find("ArtifactLoadTelemetryStage::ThumbnailServiceRequestStableLookup"),
        std::string::npos)
        << "Thumbnail service request telemetry should identify stable-result lookup and freshness checks.";
    EXPECT_NE(
        thumbnailService.find("ArtifactLoadTelemetryStage::ThumbnailServiceRequestCacheEvaluate"),
        std::string::npos)
        << "Thumbnail service request telemetry should identify cache evaluation cost inside RequestAssetPreview.";
    EXPECT_NE(
        thumbnailService.find("ArtifactLoadTelemetryStage::ThumbnailServiceRequestQueue"),
        std::string::npos)
        << "Thumbnail service request telemetry should identify queue/coalesce cost after cache misses.";
    EXPECT_NE(
        assetBrowser.find("ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPump"),
        std::string::npos)
        << "Post-draw thumbnail pump telemetry should separate request scheduling from visible UI draw cost.";
    EXPECT_NE(editorCore.find("ArtifactLoadTelemetryStageName(stage)"), std::string::npos)
        << "Thumbnail telemetry summary should use the central stage-name mapping instead of a duplicated enum whitelist.";
    EXPECT_NE(editorCore.find(".starts_with(\"Thumbnail\")"), std::string::npos)
        << "All current and future Thumbnail* telemetry stages should be included in exported summaries.";
    const auto drawGridBody = ExtractFunctionBody(
        assetBrowser,
        "void Editor::Panels::AssetBrowser::DrawCurrentFolderGrid()");
    EXPECT_NE(
        drawGridBody.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGridVisibleRows"),
        std::string::npos)
        << "DrawCurrentFolderGrid should measure the visible row/item loop as a substage.";
    EXPECT_NE(
        drawGridBody.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGridItemThumbnail"),
        std::string::npos)
        << "DrawCurrentFolderGrid should measure thumbnail drawing as a substage.";
    EXPECT_NE(
        drawGridBody.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGridItemLabel"),
        std::string::npos)
        << "DrawCurrentFolderGrid should measure label drawing as a substage.";
    EXPECT_NE(
        drawGridBody.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSet"),
        std::string::npos)
        << "DrawCurrentFolderGrid should measure visible set updates as a substage.";
    const auto generationScopeBody = ExtractFunctionBody(
        assetBrowser,
        "void Editor::Panels::AssetBrowser::UpdateThumbnailGenerationScope()");
    EXPECT_NE(
        generationScopeBody.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeSelectItems"),
        std::string::npos)
        << "UpdateThumbnailGenerationScope should measure item selection separately.";
    EXPECT_NE(
        generationScopeBody.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildKey"),
        std::string::npos)
        << "UpdateThumbnailGenerationScope should measure scope key construction separately.";
    EXPECT_NE(
        generationScopeBody.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeItemKey"),
        std::string::npos)
        << "UpdateThumbnailGenerationScope should measure item key construction separately.";
    EXPECT_NE(
        generationScopeBody.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeResultLookup"),
        std::string::npos)
        << "UpdateThumbnailGenerationScope should measure cached result lookup separately.";
    EXPECT_NE(
        generationScopeBody.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequest"),
        std::string::npos)
        << "UpdateThumbnailGenerationScope should measure request construction separately.";
    EXPECT_NE(
        generationScopeBody.find("ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeRequestPreview"),
        std::string::npos)
        << "UpdateThumbnailGenerationScope should measure preview request/lookup separately.";
    const auto onBeforeDrawBody = ExtractFunctionBody(
        assetBrowser,
        "void Editor::Panels::AssetBrowser::OnBeforeDrawWidgets()");
    EXPECT_NE(
        onBeforeDrawBody.find("ArtifactLoadTelemetryStage::ThumbnailTexturePump"),
        std::string::npos)
        << "OnBeforeDraw texture decode/upload pumping must be timed separately from visible draw.";
    const auto previewPrewarmRendererMissing = onBeforeDrawBody.find("m_thumbnailPreviewRenderer == nullptr");
    const auto previewPrewarmEnsure = onBeforeDrawBody.find("EnsureThumbnailPreviewRenderer()");
    ASSERT_NE(previewPrewarmRendererMissing, std::string::npos)
        << "Preview renderer construction is expensive; it should be prewarmed in idle OnBeforeDraw instead of first visible post-draw pump.";
    ASSERT_NE(previewPrewarmEnsure, std::string::npos)
        << "Idle AssetBrowser frames should create the GPU preview renderer before thumbnail generation needs it.";
    EXPECT_LT(previewPrewarmRendererMissing, previewPrewarmEnsure);
    const auto consumeDecodeBody = ExtractFunctionBody(
        assetBrowser,
        "void Editor::Panels::AssetBrowser::ConsumeCompletedCachedThumbnailTextureDecodes()");
    EXPECT_NE(
        consumeDecodeBody.find("ArtifactLoadTelemetryStage::ThumbnailTextureUploadDeferred"),
        std::string::npos)
        << "Ready decoded thumbnail uploads skipped by the interactive upload budget must be observable.";
    const auto pumpBody = ExtractFunctionBody(
        assetBrowser,
        "void Editor::Panels::AssetBrowser::PumpThumbnailGeneration(");
    EXPECT_NE(
        pumpBody.find("ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPump"),
        std::string::npos)
        << "Thumbnail scheduling after grid/list draw must be measured separately from ImGui draw.";
}

TEST(AssetBrowserPresentationTests, ThumbnailLatencyTelemetryCoversMainThreadGpuPreviewPath)
{
    const auto thumbnailService = ReadSourceText(RepoPath("Project/Editor/Assets/AssetThumbnailService.cpp"));

    EXPECT_NE(thumbnailService.find("ArtifactLoadTelemetryStage::ThumbnailGpuPreviewRender"), std::string::npos)
        << "Main-thread GPU preview rendering is now the prefab/material thumbnail path; "
           "it must record render telemetry or the slow stage disappears from diagnostics.";
    EXPECT_EQ(thumbnailService.find("GenerateGpuPreviewThumbnailResult"), std::string::npos)
        << "GPU preview telemetry should belong to the main-thread renderer path, not the removed background GPU job.";
}

TEST(AssetBrowserPresentationTests, ThumbnailLatencyTelemetrySplitsGpuPreviewIntoPrepareRecordSubmitAndReadback)
{
    const auto previewRenderer = ReadSourceText(RepoPath("Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp"));

    EXPECT_NE(
        previewRenderer.find("ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareResources"),
        std::string::npos);
    EXPECT_NE(
        previewRenderer.find("ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareMaterialResources"),
        std::string::npos)
        << "Material preview resource preparation needs substage telemetry so outer render spikes do not hide material cold-load cost.";
    EXPECT_NE(
        previewRenderer.find("ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareSceneObjects"),
        std::string::npos)
        << "Material/prefab preview scene object construction must be separated from material/resource loading.";
    EXPECT_NE(
        previewRenderer.find("SceneRendererKind::Forward"),
        std::string::npos)
        << "Thumbnail previews should use the lightweight forward renderer; constructing the deferred renderer "
           "loads full-frame pipeline resources and stalls the UI before any thumbnail can appear.";
    EXPECT_NE(
        previewRenderer.find("ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpDependencies"),
        std::string::npos);
    EXPECT_NE(
        previewRenderer.find("ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMeshDependencies"),
        std::string::npos);
    EXPECT_NE(
        previewRenderer.find("ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialDependencies"),
        std::string::npos);
    EXPECT_NE(
        previewRenderer.find("ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpTextureDependencies"),
        std::string::npos);
    EXPECT_NE(
        previewRenderer.find("ArtifactLoadTelemetryStage::ThumbnailGpuPreviewRecord"),
        std::string::npos);
    EXPECT_NE(
        previewRenderer.find("ArtifactLoadTelemetryStage::ThumbnailGpuPreviewSubmit"),
        std::string::npos);
    EXPECT_NE(
        previewRenderer.find("SetNextFramePostSubmitTextureReadback"),
        std::string::npos)
        << "Threaded thumbnail readback should retire asynchronously after submit instead of synchronously draining the renderer.";
    EXPECT_NE(
        previewRenderer.find("ArtifactLoadTelemetryStage::ThumbnailGpuPreviewCleanup"),
        std::string::npos);
    EXPECT_NE(
        previewRenderer.find("ArtifactLoadTelemetryStage::ThumbnailGpuPreviewReadback"),
        std::string::npos);
    EXPECT_NE(
        previewRenderer.find("ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPollReadback"),
        std::string::npos)
        << "GpuPreviewRender is still the dominant thumbnail hotspot; we need substage telemetry to see "
           "whether the remaining cost lives in resource preparation, recording, drain, or readback polling.";
}

TEST(AssetBrowserPresentationTests, SuccessfulGpuPreviewDoesNotRedrainBeforeClearingPreviewObjects)
{
    const auto previewRenderer = ReadSourceText(RepoPath("Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp"));

    EXPECT_NE(previewRenderer.find("ClearPreviewObjects(false)"), std::string::npos)
        << "A successful GPU preview render already drains threaded rendering before readback; "
           "re-draining again during teardown adds avoidable thumbnail latency.";
}

TEST(AssetBrowserPresentationTests, ThumbnailGpuPreviewDependencyPumpSkipsResourcesWithoutPendingWork)
{
    const auto previewRenderer = ReadSourceText(RepoPath("Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp"));

    EXPECT_NE(previewRenderer.find("IsAsyncArtifactLoadPending"), std::string::npos)
        << "GPU preview dependency pumping should filter to paths that still have pending async work; "
           "re-pumping already-ready resources burns time inside the dominant pump-dependencies stage.";
}

TEST(AssetBrowserPresentationTests, ThumbnailMaterialPreviewPrewarmsShaderLabPassArtifactsBeforeGpuPreview)
{
    const auto materialLoader = ReadSourceText(RepoPath("Runtime/Rendering/Resources/Loaders/MaterialLoader.h"));
    const auto assetBrowserHeader = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.h"));
    const auto assetBrowser = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));

    EXPECT_NE(materialLoader.find("ResolveShaderLabPassArtifactPaths"), std::string::npos)
        << "Material preview thumbnails should be able to resolve ShaderLab pass artifacts without constructing a material.";
    EXPECT_NE(assetBrowserHeader.find("PumpStandardPbrShaderPassPrewarm"), std::string::npos)
        << "AssetBrowser should own a one-shot idle prewarm pump instead of doing shader pass cold loads in the first material preview.";
    EXPECT_NE(assetBrowser.find("kDefaultShaderLabMaterialShaderPath"), std::string::npos);
    const auto pumpBody = ExtractFunctionBody(
        assetBrowser,
        "void Editor::Panels::AssetBrowser::PumpStandardPbrShaderPassPrewarm()");
    const auto mainThreadLoad = pumpBody.find("GetResource");
    const auto backgroundSchedule = pumpBody.find("ScheduleAssetBrowserJobFuture");
    EXPECT_NE(backgroundSchedule, std::string::npos);
    EXPECT_EQ(mainThreadLoad, std::string::npos)
        << "ShaderManager pass loads must stay out of the UI pump; telemetry showed this path could spend ~28ms synchronously.";
    EXPECT_EQ(pumpBody.find("ResolveShaderLabPassArtifactPaths"), std::string::npos)
        << "The UI pump should not split pass resolution and pass loading; the whole shader prewarm belongs in background work.";
    EXPECT_NE(pumpBody.find("PreloadShaderLabPassArtifacts", backgroundSchedule), std::string::npos);
    EXPECT_NE(pumpBody.find("ThumbnailGpuPreviewBackgroundMaterialShaderPassLoad", backgroundSchedule), std::string::npos)
        << "Background shader pass prewarm must use a distinct telemetry stage so remaining UI stalls are not confused with worker-thread prewarm cost.";
}

TEST(AssetBrowserPresentationTests, ThumbnailMaterialPreviewWarmsRenderPathBeforeGpuPreviewStart)
{
    const auto previewRendererHeader = ReadSourceText(RepoPath("Project/Editor/Assets/EditorThumbnailPreviewRenderer.h"));
    const auto previewRenderer = ReadSourceText(RepoPath("Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp"));
    const auto assetBrowserHeader = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.h"));
    const auto assetBrowser = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));

    EXPECT_NE(previewRendererHeader.find("PrewarmMaterialPreviewRenderPath"), std::string::npos)
        << "The preview renderer should own first-use render warmup details so AssetBrowser does not duplicate scene setup.";
    EXPECT_NE(previewRenderer.find("material-preview-render-warmup"), std::string::npos)
        << "The warmup pass should use the same material preview camera/render path as real material thumbnails.";
    EXPECT_NE(previewRenderer.find("m_materialPreviewMaterial = CreateStablePreviewMaterial(defaultMaterial)"), std::string::npos)
        << "Warmup must bind a material owned by the preview renderer so pending render keep-alive can retain it.";
    EXPECT_NE(previewRenderer.find("keepAlive->objects.push_back(m_materialPreviewObject)"), std::string::npos)
        << "Pending warmup/render work must keep the persistent material sphere object alive instead of deactivating it while threaded scene sync may still read it.";
    EXPECT_NE(previewRenderer.find("m_materialPreviewObject = nullptr"), std::string::npos)
        << "Once captured for a pending render, the persistent material sphere must not be mutated by later preview setup until the keep-alive destroys it.";
    EXPECT_EQ(previewRenderer.find("return true;\n    }\n\n    PreviewResourcePathSet CollectRequestedPreviewResourcePaths"), std::string::npos)
        << "Warmup should not be marked complete after render-busy/readback-failed paths.";
    const auto warmupBody = ExtractFunctionBody(
        previewRenderer,
        "bool PrewarmMaterialPreviewRenderPath");
    const auto warmupTelemetry = warmupBody.find("TryGetThreadedFrameTelemetry(m_driver)");
    const auto warmupInFlightCheck = warmupBody.find("inFlightFrameCount != 0u", warmupTelemetry);
    const auto warmupClearPreviewObjects = warmupBody.find("ClearPreviewObjects(false)");
    ASSERT_NE(warmupTelemetry, std::string::npos)
        << "Render-path warmup must query render-thread idleness without draining or blocking the UI; "
           "prefab previews can leave queued render work that is unsafe to overlap with material sphere reuse.";
    ASSERT_NE(warmupInFlightCheck, std::string::npos)
        << "Busy threaded rendering should skip warmup for this frame and retry from the next post-draw pump.";
    ASSERT_NE(warmupClearPreviewObjects, std::string::npos);
    EXPECT_LT(warmupTelemetry, warmupClearPreviewObjects)
        << "Warmup should prove the render thread is idle before clearing/reusing preview scene objects.";
    EXPECT_EQ(warmupBody.find("TryDrainThreadedRendering"), std::string::npos)
        << "DriverRendererAccess::TryDrainThreadedRendering can synchronously drain lifecycle work; warmup must not call it.";
    EXPECT_EQ(warmupBody.find("DrainThreadedRendering(m_driver)"), std::string::npos)
        << "Warmup is a latency optimization and must skip busy frames instead of blocking them.";
    EXPECT_NE(previewRenderer.find("PostSubmitTextureReadbackState"), std::string::npos)
        << "Preview readback completion must be tied to the submitted thumbnail frame.";
    EXPECT_EQ(previewRenderer.find("latestRetiredFrameId"), std::string::npos)
        << "Renderer-local frame ids are not globally ordered and cannot identify thumbnail completion.";
    EXPECT_NE(assetBrowserHeader.find("m_thumbnailPreviewRenderWarmupCompleted"), std::string::npos);

    const auto beforeDrawBody = ExtractFunctionBody(
        assetBrowser,
        "void Editor::Panels::AssetBrowser::OnBeforeDrawWidgets()");
    EXPECT_EQ(beforeDrawBody.find("PumpThumbnailPreviewRenderWarmup()"), std::string::npos)
        << "Render-path warmup can submit background preview work, so it belongs in the post-draw thumbnail pump, not before UI drawing.";
    EXPECT_NE(beforeDrawBody.find("PumpThumbnailGeneration(true, false, false)"), std::string::npos)
        << "The pre-draw thumbnail pump must explicitly disable render-path warmup because warmup can submit preview render work.";

    const auto pumpBody = ExtractFunctionBody(
        assetBrowser,
        "void Editor::Panels::AssetBrowser::PumpThumbnailGeneration(");
    const auto renderWarmup = pumpBody.find("PumpThumbnailPreviewRenderWarmup()");
    const auto lightGpuStart = pumpBody.find("GenerateNextThumbnail(*m_thumbnailPreviewRenderer, false)");
    const auto heavyGpuStart = pumpBody.find("GenerateNextThumbnail(*m_thumbnailPreviewRenderer, true)");
    EXPECT_NE(renderWarmup, std::string::npos);
    EXPECT_NE(lightGpuStart, std::string::npos);
    EXPECT_NE(heavyGpuStart, std::string::npos);
    EXPECT_NE(pumpBody.find("allowPreviewRenderWarmup"), std::string::npos)
        << "Warmup permission must be explicit so pre-draw thumbnail pumping cannot trigger it indirectly.";
    EXPECT_NE(pumpBody.find("m_standardPbrShaderPassPrewarmCompleted"), std::string::npos)
        << "Render-path warmup should run only after the ShaderLab pass prewarm has had a chance to complete.";
    EXPECT_LT(renderWarmup, lightGpuStart)
        << "Material preview first-use render cost should be moved ahead of visible GPU thumbnail generation.";
    EXPECT_LT(renderWarmup, heavyGpuStart)
        << "Material preview warmup should not share a frame with heavier prefab/model GPU preview starts.";
}

TEST(AssetBrowserPresentationTests, ThumbnailForwardBackgroundPreviewUsesThreadedGpuScenePath)
{
    const auto source = ReadSourceText(RepoPath("Runtime/Engine/Rendering/ForwardSceneRenderer.cpp"));
    const auto beginFrame = source.find("void ForwardSceneRenderer::BeginSceneFrame");
    ASSERT_NE(beginFrame, std::string::npos);
    const auto drawFrame = source.find("void ForwardSceneRenderer::DrawFrame", beginFrame);
    ASSERT_NE(drawFrame, std::string::npos);
    const auto drawOpaques = source.find("void ForwardSceneRenderer::DrawOpaques", drawFrame);
    ASSERT_NE(drawOpaques, std::string::npos);
    const auto beginFrameBody = source.substr(beginFrame, drawFrame - beginFrame);
    const auto drawFrameBody = source.substr(drawFrame, drawOpaques - drawFrame);

    EXPECT_NE(beginFrameBody.find("BaseSceneRenderer::BeginFrameForBackgroundPreview"), std::string::npos)
        << "Background previews still need the non-owning global-frame entrypoint.";
    EXPECT_EQ(beginFrameBody.find("!m_backgroundPreviewFrame"), std::string::npos)
        << "A threaded driver cannot execute direct Forward draws without an active explicit frame context.";
    EXPECT_NE(beginFrameBody.find("DriverRendererAccess::IsThreadedRenderingEnabled(m_driver)"), std::string::npos)
        << "Background previews must capture immutable GPU draw commands for the renderer thread.";
    EXPECT_EQ(drawFrameBody.find("!m_backgroundPreviewFrame"), std::string::npos)
        << "DrawFrame must use the same threaded execution decision as BeginSceneFrame.";
    EXPECT_NE(drawFrameBody.find("DriverRendererAccess::IsThreadedRenderingEnabled(m_driver)"), std::string::npos);
}

TEST(AssetBrowserPresentationTests, ThumbnailGenerationStopsStartingWorkWhenEditorWindowIsClosing)
{
    const auto assetBrowserHeader = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.h"));
    const auto assetBrowser = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));

    EXPECT_NE(assetBrowserHeader.find("IsEditorWindowClosing"), std::string::npos)
        << "AssetBrowser needs a single lifecycle guard for validation frames that set ShouldClose during UI draw.";

    const auto closingBody = ExtractFunctionBody(
        assetBrowser,
        "bool Editor::Panels::AssetBrowser::IsEditorWindowClosing() const");
    EXPECT_NE(closingBody.find("EDITOR_CONTEXT(window)"), std::string::npos);
    EXPECT_NE(closingBody.find("ShouldClose()"), std::string::npos);

    const auto ensureBody = ExtractFunctionBody(
        assetBrowser,
        "bool Editor::Panels::AssetBrowser::EnsureThumbnailPreviewRenderer()");
    const auto ensureClosingCheck = ensureBody.find("IsEditorWindowClosing()");
    const auto ensureLocateDriver = ensureBody.find("TryGetLocatedDriver()");
    ASSERT_NE(ensureClosingCheck, std::string::npos);
    ASSERT_NE(ensureLocateDriver, std::string::npos);
    EXPECT_LT(ensureClosingCheck, ensureLocateDriver)
        << "A closing validation frame must not create a thumbnail preview renderer that captures Driver&.";

    const auto pumpBody = ExtractFunctionBody(
        assetBrowser,
        "void Editor::Panels::AssetBrowser::PumpThumbnailGeneration(");
    const auto pumpClosingCheck = pumpBody.find("IsEditorWindowClosing()");
    const auto previewWarmup = pumpBody.find("PumpThumbnailPreviewRenderWarmup()");
    const auto lightGpuStart = pumpBody.find("GenerateNextThumbnail(*m_thumbnailPreviewRenderer, false)");
    const auto heavyGpuStart = pumpBody.find("GenerateNextThumbnail(*m_thumbnailPreviewRenderer, true)");
    const auto backgroundStart = pumpBody.find("StartNextThumbnailGeneration()");
    ASSERT_NE(pumpClosingCheck, std::string::npos);
    ASSERT_NE(previewWarmup, std::string::npos);
    ASSERT_NE(lightGpuStart, std::string::npos);
    ASSERT_NE(heavyGpuStart, std::string::npos);
    ASSERT_NE(backgroundStart, std::string::npos);
    EXPECT_LT(pumpClosingCheck, previewWarmup);
    EXPECT_LT(pumpClosingCheck, lightGpuStart);
    EXPECT_LT(pumpClosingCheck, heavyGpuStart);
    EXPECT_LT(pumpClosingCheck, backgroundStart);

    const auto beforeDrawBody = ExtractFunctionBody(
        assetBrowser,
        "void Editor::Panels::AssetBrowser::OnBeforeDrawWidgets()");
    const auto beforeDrawClosing = beforeDrawBody.find("IsEditorWindowClosing()");
    const auto beforeDrawEnsure = beforeDrawBody.find("EnsureThumbnailPreviewRenderer()");
    ASSERT_NE(beforeDrawClosing, std::string::npos);
    ASSERT_NE(beforeDrawEnsure, std::string::npos);
    EXPECT_LT(beforeDrawClosing, beforeDrawEnsure)
        << "Idle preview renderer prewarm must be disabled once another validation panel has requested close.";
}

TEST(AssetBrowserPresentationTests, SceneReadbackValidationDefersGpuThumbnailPreviewWork)
{
    const auto assetBrowserHeader = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.h"));
    const auto assetBrowser = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));

    EXPECT_NE(assetBrowserHeader.find("IsEditorSceneReadbackValidationActive"), std::string::npos)
        << "Scene readback validation measures the Scene View startup frame; AssetBrowser must not start GPU preview work during it.";

    const auto validationBody = ExtractFunctionBody(
        assetBrowser,
        "bool Editor::Panels::AssetBrowser::IsEditorSceneReadbackValidationActive() const");
    EXPECT_NE(validationBody.find("editorValidationSceneReadbackOutput"), std::string::npos);
    EXPECT_NE(validationBody.find("editorValidationSceneReadbackSummary"), std::string::npos);

    const auto ensureBody = ExtractFunctionBody(
        assetBrowser,
        "bool Editor::Panels::AssetBrowser::EnsureThumbnailPreviewRenderer()");
    const auto ensureValidationCheck = ensureBody.find("IsEditorSceneReadbackValidationActive()");
    const auto ensureCreate = ensureBody.find("std::make_shared<NLS::Editor::Assets::EditorThumbnailPreviewRenderer>");
    ASSERT_NE(ensureValidationCheck, std::string::npos);
    ASSERT_NE(ensureCreate, std::string::npos);
    EXPECT_LT(ensureValidationCheck, ensureCreate);

    const auto warmupBody = ExtractFunctionBody(
        assetBrowser,
        "void Editor::Panels::AssetBrowser::PumpThumbnailPreviewRenderWarmup()");
    EXPECT_NE(warmupBody.find("IsEditorSceneReadbackValidationActive()"), std::string::npos);

    const auto pumpBody = ExtractFunctionBody(
        assetBrowser,
        "void Editor::Panels::AssetBrowser::PumpThumbnailGeneration(");
    const auto pumpValidationCheck = pumpBody.find("IsEditorSceneReadbackValidationActive()");
    const auto previewWarmup = pumpBody.find("PumpThumbnailPreviewRenderWarmup()");
    const auto lightGpuStart = pumpBody.find("GenerateNextThumbnail(*m_thumbnailPreviewRenderer, false)");
    ASSERT_NE(pumpValidationCheck, std::string::npos);
    ASSERT_NE(previewWarmup, std::string::npos);
    ASSERT_NE(lightGpuStart, std::string::npos);
    EXPECT_LT(pumpValidationCheck, previewWarmup);
    EXPECT_LT(pumpValidationCheck, lightGpuStart);
}

TEST(AssetBrowserPresentationTests, ThumbnailTextureUploadUsesRendererOwnedRgba8UploadQueue)
{
    const auto assetBrowser = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto uploadBody = ExtractFunctionBody(
        assetBrowser,
        "bool Editor::Panels::AssetBrowser::LoadDecodedCachedThumbnailTexture");

    EXPECT_NE(uploadBody.find("RequestUiRgba8TextureUpload"), std::string::npos)
        << "Cached thumbnail PNG decode already produces RGBA8; upload should be queued for renderer-owned RHI upload instead of creating an immediate UI-thread texture.";
    EXPECT_EQ(uploadBody.find("CreateFromRgba8Memory"), std::string::npos)
        << "Immediate RGBA8 texture creation can still block the UI thread through RHI upload work.";
    EXPECT_EQ(uploadBody.find("CreateFromMemory"), std::string::npos)
        << "The generic CreateFromMemory path re-wraps RGBA8 bytes as Image and copies them again before upload.";
}

TEST(AssetBrowserPresentationTests, TransientThumbnailTextureUploadFailuresDoNotPermanentlySuppressCachePath)
{
    const auto assetBrowser = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto assetBrowserHeader = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.h"));
    const auto uploadBody = ExtractFunctionBody(
        assetBrowser,
        "bool Editor::Panels::AssetBrowser::LoadDecodedCachedThumbnailTexture");
    const auto consumeBody = ExtractFunctionBody(
        assetBrowser,
        "void Editor::Panels::AssetBrowser::ConsumeCompletedCachedThumbnailTextureDecodes()");

    const auto noDriver = uploadBody.find("if (driver == nullptr)");
    const auto requestIdFailed = uploadBody.find("if (requestId == 0u)");
    const auto resolveTextureIdFailed = consumeBody.find("if (textureId == nullptr)");

    ASSERT_NE(noDriver, std::string::npos);
    ASSERT_NE(requestIdFailed, std::string::npos);
    ASSERT_NE(resolveTextureIdFailed, std::string::npos);
    EXPECT_EQ(uploadBody.find("m_thumbnailTexturesFailedToLoad.insert", noDriver), std::string::npos)
        << "A missing renderer driver can be a startup timing condition; it must not permanently suppress a valid cached thumbnail path.";
    EXPECT_EQ(uploadBody.find("m_thumbnailTexturesFailedToLoad.insert", requestIdFailed), std::string::npos)
        << "A refused async UI upload request should retry later instead of making the disk cache invisible forever.";
    EXPECT_EQ(consumeBody.find("m_thumbnailTexturesFailedToLoad.insert", resolveTextureIdFailed), std::string::npos)
        << "UI texture id resolution can be temporarily unavailable; keep the cached thumbnail retryable.";
    EXPECT_NE(assetBrowserHeader.find("m_thumbnailTextureRetryAfterFrameByPath"), std::string::npos)
        << "Retryable thumbnail upload failures need a cooldown separate from permanent decode/file suppression.";
    EXPECT_NE(assetBrowser.find("MarkCachedThumbnailTextureUploadRetryableFailure"), std::string::npos)
        << "Transient GPU/UI failures should mark a retry-after frame so valid cached thumbnails retry without hot-looping.";
}

TEST(AssetBrowserPresentationTests, CompletedThumbnailTextureUploadResolveIsFrameBudgeted)
{
    const auto assetBrowser = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto consumeBody = ExtractFunctionBody(
        assetBrowser,
        "void Editor::Panels::AssetBrowser::ConsumeCompletedCachedThumbnailTextureDecodes()");

    const auto completedUploadResult = consumeBody.find("ConsumeUiRgba8TextureUploadResult");
    const auto resolveTextureId = consumeBody.find("ResolveTextureId(result.textureView)");
    const auto budgetCheck = consumeBody.find("ShouldContinueAssetBrowserThumbnailTextureUploads");

    ASSERT_NE(completedUploadResult, std::string::npos);
    ASSERT_NE(resolveTextureId, std::string::npos);
    ASSERT_NE(budgetCheck, std::string::npos);
    EXPECT_LT(completedUploadResult, resolveTextureId);
    EXPECT_LT(budgetCheck, resolveTextureId)
        << "Completed RHI uploads can all become ready in one frame; UI texture-id registration must share the thumbnail upload frame budget instead of resolving every ready texture at once.";
}

TEST(AssetBrowserPresentationTests, PendingThumbnailTextureUploadPollsAreFrameBudgeted)
{
    const auto assetBrowser = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto consumeBody = ExtractFunctionBody(
        assetBrowser,
        "void Editor::Panels::AssetBrowser::ConsumeCompletedCachedThumbnailTextureDecodes()");

    const auto polledCounter = consumeBody.find("pendingUploadsPolledThisFrame");
    const auto budgetInput = consumeBody.find("pendingUploadsPolledThisFrame", consumeBody.find("ShouldContinueAssetBrowserThumbnailTextureUploads"));
    const auto consumeResult = consumeBody.find("ConsumeUiRgba8TextureUploadResult");

    ASSERT_NE(polledCounter, std::string::npos);
    ASSERT_NE(budgetInput, std::string::npos);
    ASSERT_NE(consumeResult, std::string::npos);
    EXPECT_LT(polledCounter, consumeResult)
        << "Not-yet-ready uploads still cost map iteration and driver polling; large texture folders must cap polled pending uploads per UI frame.";
}

TEST(AssetBrowserPresentationTests, PendingThumbnailTextureUploadPollingRotatesAcrossFrames)
{
    const auto assetBrowserHeader = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.h"));
    const auto assetBrowser = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto consumeBody = ExtractFunctionBody(
        assetBrowser,
        "void Editor::Panels::AssetBrowser::ConsumeCompletedCachedThumbnailTextureDecodes()");

    EXPECT_NE(assetBrowserHeader.find("m_nextPendingThumbnailTextureUploadPollPath"), std::string::npos)
        << "A per-frame poll cap must remember where it stopped; otherwise the same early not-ready uploads can starve later ready thumbnails.";
    EXPECT_NE(consumeBody.find("m_nextPendingThumbnailTextureUploadPollPath"), std::string::npos)
        << "Pending thumbnail upload polling should rotate across the map instead of restarting from begin() every frame.";
}

TEST(AssetBrowserPresentationTests, ThumbnailScopeRequeryDoesNotClearQueuedThumbnailWork)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto body = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::UpdateThumbnailGenerationScope()");

    const auto scopeChangedBranch = body.find("if (decision.scopeChanged)");
    const auto clearQueued = body.find("ClearQueuedRequests()", scopeChangedBranch);

    EXPECT_NE(scopeChangedBranch, std::string::npos);
    EXPECT_EQ(clearQueued, std::string::npos)
        << "Dirty same-scope thumbnail refreshes happen after visible draw registration; clearing queued work there drops the just-requested previews.";
}

TEST(AssetBrowserPresentationTests, ThumbnailScopeSkipsOnlyCacheKeyMatchingResultsAfterBuildingRequests)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto body = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::UpdateThumbnailGenerationScope()");

    const auto itemKeyBuild = body.find("BuildAssetBrowserThumbnailItemKey(item, nextSize)");
    const auto cachedResultLookup = body.find("m_thumbnailResultsByItemKey.find(itemThumbnailKey)");
    const auto pendingShortCircuit = body.find(
        "foundThumbnail->second.status == NLS::Editor::Assets::AssetThumbnailServiceStatus::Pending");
    const auto requestBuild = body.find("BuildAssetThumbnailRequestForItem(");
    const auto currentCacheKey = body.find("BuildAssetThumbnailCacheKey(*request)", requestBuild);
    const auto matchingHelper = body.find("ThumbnailResultMatchesRequestCacheKey", currentCacheKey);

    ASSERT_NE(itemKeyBuild, std::string::npos);
    ASSERT_NE(cachedResultLookup, std::string::npos);
    ASSERT_NE(pendingShortCircuit, std::string::npos);
    ASSERT_NE(requestBuild, std::string::npos);
    ASSERT_NE(currentCacheKey, std::string::npos);
    ASSERT_NE(matchingHelper, std::string::npos);
    EXPECT_LT(itemKeyBuild, requestBuild)
        << "The stable UI item key should still find any previous visible result before current-size request construction.";
    EXPECT_LT(cachedResultLookup, requestBuild);
    EXPECT_LT(requestBuild, currentCacheKey)
        << "Skipping a stable item-key result must compare the current request cache key, which includes thumbnail size.";
    EXPECT_LT(currentCacheKey, matchingHelper);
    EXPECT_LT(matchingHelper, pendingShortCircuit)
        << "Pending thumbnails should only coalesce when the pending cache entry matches the current requested size.";
}

TEST(AssetBrowserPresentationTests, ThumbnailCacheContainmentMemoRejectsWindowsReparsePoints)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Assets/AssetThumbnailCache.cpp"));
    const auto stampBody = ExtractFunctionBody(
        source,
        "FilesystemContainmentStamp BuildFilesystemContainmentStamp(");

    EXPECT_NE(source.find("FILE_ATTRIBUTE_REPARSE_POINT"), std::string::npos)
        << "Windows junction/reparse paths must not be memoized as ordinary directories.";
    EXPECT_NE(stampBody.find("PathHasWindowsReparsePoint(path)"), std::string::npos)
        << "The read-side containment memo must reject reparse points before reusing a validation result.";
    EXPECT_NE(stampBody.find("std::filesystem::file_type::symlink"), std::string::npos)
        << "Reparse points should be treated like symlinks so they remain outside the cacheable stamp set.";
}

TEST(AssetBrowserPresentationTests, VisiblePrefabHotCachePreloadWaitsForStableIdleScope)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto setVisibleBody = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::SetVisibleThumbnailItems(");
    const auto flushBody = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::FlushPendingVisiblePrefabHotCachePreload()");
    const auto scheduleBody = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::SchedulePrefabHotCachePreloadForVisibleItems(");

    const auto sameScopeBranch = setVisibleBody.find("nextFingerprint == m_visibleThumbnailFingerprint");
    const auto directScheduleInSameScope = setVisibleBody.find(
        "SchedulePrefabHotCachePreloadForVisibleItems(visibleItems)",
        sameScopeBranch);
    EXPECT_NE(sameScopeBranch, std::string::npos);
    EXPECT_EQ(directScheduleInSameScope, std::string::npos)
        << "Stable visible scopes are hit every scroll frame; they must not rebuild prefab drag payloads repeatedly.";

    EXPECT_NE(setVisibleBody.find("m_visiblePrefabHotCachePreloadPending = true;"), std::string::npos)
        << "A changed visible scope should mark prefab hot-cache preloading for a later idle frame.";
    EXPECT_NE(setVisibleBody.find("FlushPendingVisiblePrefabHotCachePreload();"), std::string::npos);
    EXPECT_NE(flushBody.find("IsAssetBrowserInteractive()"), std::string::npos)
        << "Visible prefab hot-cache preloading is drag latency optimization, so it should yield while scrolling or expanding.";
    EXPECT_NE(
        flushBody.find("SchedulePrefabHotCachePreloadForVisibleItems(m_visibleThumbnailItems)"),
        std::string::npos);
    EXPECT_NE(
        scheduleBody.find("MakeAssetBrowserItemDragPayload(\n\t\t\titem,\n\t\t\tnullptr)"),
        std::string::npos)
        << "Visible prewarm is opportunistic background work; payload construction must not run AssetDatabase freshness checks on the UI thread.";
    EXPECT_EQ(scheduleBody.find("m_projectAssetDatabaseSnapshot"), std::string::npos)
        << "Freshness validation belongs to drag and final-drop paths, not visible-cell prewarm scheduling.";
}

TEST(AssetBrowserPresentationTests, ThumbnailPumpAllowsBoundedInteractiveVisibleWork)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserThumbnailPumpInput input;
    input.hasQueuedWork = true;

    EXPECT_TRUE(PlanAssetBrowserThumbnailPump(input).shouldStartBackgroundWork);

    input.interactive = true;
    EXPECT_TRUE(PlanAssetBrowserThumbnailPump(input).shouldStartBackgroundWork);

    input.hasInFlightWork = true;
    EXPECT_TRUE(PlanAssetBrowserThumbnailPump(input).shouldStartBackgroundWork)
        << "Visible thumbnail display should keep filling the small interactive start budget even while an earlier thumbnail is in flight.";

    input.hasInFlightWork = false;
    input.interactiveStartsThisFrame = 1u;
    input.maxInteractiveStartsPerFrame = 1u;
    EXPECT_FALSE(PlanAssetBrowserThumbnailPump(input).shouldStartBackgroundWork);

    input.hasQueuedWork = false;
    input.interactiveStartsThisFrame = 0u;
    EXPECT_FALSE(PlanAssetBrowserThumbnailPump(input).shouldStartBackgroundWork);
}

TEST(AssetBrowserPresentationTests, CachedThumbnailTexturePumpRunsDuringInteractiveFramesWithBudget)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserCachedThumbnailTexturePumpInput input;
    input.queuedTextureLoads = 3u;

    EXPECT_TRUE(PlanAssetBrowserCachedThumbnailTexturePump(input).shouldPump);

    input.interactive = true;
    EXPECT_TRUE(PlanAssetBrowserCachedThumbnailTexturePump(input).shouldPump);

    input.interactiveStartsThisFrame = 1u;
    input.maxInteractiveStartsPerFrame = 1u;
    EXPECT_FALSE(PlanAssetBrowserCachedThumbnailTexturePump(input).shouldPump);

    input.queuedTextureLoads = 0u;
    input.inFlightDecodes = 1u;
    input.interactiveStartsThisFrame = 0u;
    EXPECT_TRUE(PlanAssetBrowserCachedThumbnailTexturePump(input).shouldPump);

    input.inFlightDecodes = 0u;
    EXPECT_FALSE(PlanAssetBrowserCachedThumbnailTexturePump(input).shouldPump);
}

TEST(AssetBrowserPresentationTests, CachedThumbnailTexturePumpRunsForPendingUploads)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserCachedThumbnailTexturePumpInput input;
    input.pendingTextureUploads = 1u;

    EXPECT_TRUE(PlanAssetBrowserCachedThumbnailTexturePump(input).shouldPump);

    input.interactive = true;
    EXPECT_TRUE(PlanAssetBrowserCachedThumbnailTexturePump(input).shouldPump);

    input.interactiveStartsThisFrame = 1u;
    input.maxInteractiveStartsPerFrame = 1u;
    EXPECT_FALSE(PlanAssetBrowserCachedThumbnailTexturePump(input).shouldPump);
}

TEST(AssetBrowserPresentationTests, CachedThumbnailTexturePumpCallsiteIncludesPendingUploads)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto body = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::OnBeforeDrawWidgets()");

    const auto call = body.find("PlanAssetBrowserCachedThumbnailTexturePump({");
    ASSERT_NE(call, std::string::npos);
    const auto queued = body.find("m_thumbnailTextureLoadQueue.size()", call);
    const auto decodes = body.find("m_thumbnailTextureDecodes.size()", call);
    const auto pending = body.find("m_pendingThumbnailTextureUploadsByPath.size()", call);
    const auto interactiveStarts = body.find("0u,", pending);
    const auto interactiveMax = body.find("kMaxAssetBrowserInteractiveCachedThumbnailTexturePumpsPerFrame", interactiveStarts);

    ASSERT_NE(queued, std::string::npos);
    ASSERT_NE(decodes, std::string::npos);
    ASSERT_NE(pending, std::string::npos);
    ASSERT_NE(interactiveStarts, std::string::npos);
    ASSERT_NE(interactiveMax, std::string::npos);
    EXPECT_LT(queued, decodes);
    EXPECT_LT(decodes, pending);
    EXPECT_LT(pending, interactiveStarts)
        << "The aggregate argument after pendingTextureUploads is interactiveStartsThisFrame; keep it explicitly zero.";
    EXPECT_LT(interactiveStarts, interactiveMax);

    const auto telemetryCount = body.find("texturePumpTelemetryItemCount");
    ASSERT_NE(telemetryCount, std::string::npos);
    EXPECT_NE(
        body.find("m_pendingThumbnailTextureUploadsByPath.size()", telemetryCount),
        std::string::npos)
        << "Texture pump telemetry should include pending GPU uploads so pending-only frames remain visible in diagnostics.";
}

TEST(AssetBrowserPresentationTests, CachedThumbnailTextureUploadBudgetStopsAfterElapsedFrameSlice)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserThumbnailTextureUploadBudgetInput input;
    input.uploadedThisFrame = 0u;
    input.maxUploadsPerFrame = 1u;
    input.elapsedMicroseconds = 0u;
    input.maxElapsedMicroseconds = 2000u;
    EXPECT_TRUE(ShouldContinueAssetBrowserThumbnailTextureUploads(input));

    input.elapsedMicroseconds = 2000u;
    EXPECT_FALSE(ShouldContinueAssetBrowserThumbnailTextureUploads(input));

    input.elapsedMicroseconds = 1u;
    input.uploadedThisFrame = 1u;
    EXPECT_FALSE(ShouldContinueAssetBrowserThumbnailTextureUploads(input));

    input.maxUploadsPerFrame = 0u;
    input.uploadedThisFrame = 0u;
    EXPECT_FALSE(ShouldContinueAssetBrowserThumbnailTextureUploads(input));
}

TEST(AssetBrowserPresentationTests, CachedThumbnailTextureUploadBudgetStopsAfterPendingPollSlice)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserThumbnailTextureUploadBudgetInput input;
    input.uploadedThisFrame = 0u;
    input.maxUploadsPerFrame = 1u;
    input.polledThisFrame = 0u;
    input.maxPollsPerFrame = 8u;
    input.elapsedMicroseconds = 0u;
    input.maxElapsedMicroseconds = 2000u;
    EXPECT_TRUE(ShouldContinueAssetBrowserThumbnailTextureUploads(input));

    input.polledThisFrame = 8u;
    EXPECT_FALSE(ShouldContinueAssetBrowserThumbnailTextureUploads(input))
        << "A folder with many not-yet-ready thumbnail uploads must not scan every pending upload in one UI frame.";
}

TEST(AssetBrowserPresentationTests, ThumbnailRequestBudgetStopsAfterElapsedFrameSlice)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserThumbnailRequestBudgetInput input;
    input.requestedThisFrame = 0u;
    input.maxRequestsPerFrame = 4u;
    input.elapsedMicroseconds = 0u;
    input.maxElapsedMicroseconds = 2000u;
    EXPECT_TRUE(ShouldContinueAssetBrowserThumbnailRequests(input));

    input.elapsedMicroseconds = 2000u;
    EXPECT_FALSE(ShouldContinueAssetBrowserThumbnailRequests(input));

    input.elapsedMicroseconds = 1u;
    input.requestedThisFrame = 4u;
    EXPECT_FALSE(ShouldContinueAssetBrowserThumbnailRequests(input));

    input.maxRequestsPerFrame = 0u;
    input.requestedThisFrame = 0u;
    EXPECT_FALSE(ShouldContinueAssetBrowserThumbnailRequests(input));
}

TEST(AssetBrowserPresentationTests, ThumbnailGenerationScopeReusesRequestBuildFileStampCacheAcrossFrameSlices)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto body = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::UpdateThumbnailGenerationScope()");

    EXPECT_EQ(body.find("fileStampsByPath.clear()"), std::string::npos)
        << "Generation scope request building is sliced across frames; clearing file stamp cache inside the per-frame pump forces repeated source/meta/artifact stat work and shows up in ThumbnailUiDrawGenerationScopeBuildRequest telemetry.";
}

TEST(AssetBrowserPresentationTests, InteractiveThumbnailDisplayUsesSmallBatchBudgets)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));

    EXPECT_NE(
        source.find("constexpr size_t kMaxAssetBrowserInteractiveThumbnailRequestsPerFrame = 4u;"),
        std::string::npos)
        << "A visible folder scope should queue several thumbnails per interactive frame; a budget of 1 makes a full row wait multiple frames before generation can even start.";
    EXPECT_NE(
        source.find("constexpr size_t kMaxAssetBrowserInteractiveCachedThumbnailTexturePumpsPerFrame = 4u;"),
        std::string::npos)
        << "Fresh thumbnail files still need decode/upload work before they become visible; interactive browsing should pump a small batch instead of one texture per frame.";
    EXPECT_NE(
        source.find("constexpr size_t kMaxAssetBrowserThumbnailTextureUploadsPerFrame = 1u;"),
        std::string::npos)
        << "A single cached thumbnail RHI upload can exceed the frame budget; idle frames should upload at most one texture.";
	EXPECT_NE(
		source.find("constexpr size_t kMaxAssetBrowserInteractiveThumbnailTextureUploadsPerFrame = 1u;"),
		std::string::npos)
		<< "Interactive browsing should trickle one decoded thumbnail into async RHI upload per frame; after WrapExternal stopped creating fallback RHI textures, the UI-side upload completion path is sub-millisecond and a zero budget keeps ready thumbnails invisible until scrolling fully idles.";
    EXPECT_NE(
        source.find("constexpr size_t kMaxAssetBrowserInteractiveThumbnailStartsPerFrame = 2u;"),
        std::string::npos)
        << "Interactive visible thumbnail generation should start a tiny batch of background workers instead of serializing an entire visible row through one in-flight request.";
    EXPECT_NE(
        source.find("constexpr uint64_t kMaxAssetBrowserThumbnailRequestMicrosecondsPerFrame = 2000u;"),
        std::string::npos)
        << "Thumbnail request construction/cache lookup is a measured UI-thread hotspot; requests need a frame time slice in addition to a count budget.";
	EXPECT_NE(
		source.find("ShouldContinueAssetBrowserThumbnailRequests"),
		std::string::npos)
		<< "Generation scope updates should stop queuing thumbnail requests once their UI-thread time slice is exhausted.";
}

TEST(AssetBrowserPresentationTests, CompletedThumbnailUploadPublishDoesNotConsumeNewUploadBudget)
{
	const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
	const auto body = ExtractFunctionBody(
		source,
		"void Editor::Panels::AssetBrowser::ConsumeCompletedCachedThumbnailTextureDecodes()");

	EXPECT_NE(body.find("completedUploadsPublishedThisFrame"), std::string::npos)
		<< "Publishing renderer-thread upload results is now a cheap UI operation and should have its own counter.";
	EXPECT_NE(body.find("queuedUploadsThisFrame"), std::string::npos)
		<< "Starting a new RHI upload is the expensive operation that should consume the upload enqueue budget.";
	const auto pendingConsume = body.find("ConsumeUiRgba8TextureUploadResult");
	ASSERT_NE(pendingConsume, std::string::npos);
	const auto readyDecode = body.find("ThumbnailTexturePumpReadyDecodePoll");
	ASSERT_NE(readyDecode, std::string::npos);
	const auto pendingBudget = body.rfind("completedUploadsPublishedThisFrame", pendingConsume);
	const auto readyBudget = body.find("queuedUploadsThisFrame", readyDecode);
	EXPECT_NE(pendingBudget, std::string::npos)
		<< "Pending upload result polling should budget completed publish work separately from new upload enqueue work.";
	EXPECT_NE(readyBudget, std::string::npos)
		<< "Ready decode handling should use the new upload enqueue counter, not the completed publish counter.";
	const auto oldSharedIncrement = body.find("++uploadedThisFrame");
	EXPECT_EQ(oldSharedIncrement, std::string::npos)
		<< "A shared upload counter makes frames alternate between publishing an uploaded texture and starting the next upload, which keeps ready thumbnails deferred.";
}

TEST(AssetBrowserPresentationTests, DisplayDirtyDoesNotScrollDelayHeavyPrefabPreviewPump)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto body = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::MarkProjectAssetDisplayItemsDirty()");

    const auto heavyDelay = body.find("m_heavyGpuThumbnailGenerationDeferredUntil");
    ASSERT_NE(heavyDelay, std::string::npos);
    const auto heavyScrollDelay = body.find("kAssetBrowserScrollIdleDelaySeconds", heavyDelay);
    const auto lightDelay = body.find("m_lightGpuThumbnailGenerationDeferredUntil");

    EXPECT_TRUE(lightDelay == std::string::npos || heavyScrollDelay == std::string::npos || heavyScrollDelay > lightDelay)
        << "Display rebuild/partial publish dirty events happen while expanding large imported models; "
           "they must not keep pushing heavy prefab/model GPU previews behind the longer scroll idle delay.";
    EXPECT_NE(body.find("kAssetBrowserHeavyGpuThumbnailIdleDelaySeconds"), std::string::npos)
        << "Heavy GPU prefab/model previews should use the shorter heavy-preview idle delay after display rebuilds.";
}

TEST(AssetBrowserPresentationTests, ResourcePendingPrefabUsesNonBlockingFastPollCadence)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));

    EXPECT_NE(
        source.find("kAssetBrowserHeavyGpuThumbnailResourcePendingIntervalSeconds = 0.05"),
        std::string::npos)
        << "Once mesh runtime creation runs on the RHI worker, a pending prefab must not wait two seconds between small resource-progress polls.";
    EXPECT_NE(
        source.find("kMaxAssetBrowserThumbnailRequestMicrosecondsPerFrame = 2000u"),
        std::string::npos)
        << "Fast resource polling must retain the Asset Browser UI-thread time budget.";
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

TEST(AssetBrowserPresentationTests, ObjectReferencePickerBackgroundRefreshUsesPublishedSnapshotIndex)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto requestBody = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::RequestObjectReferencePickerEntriesRefresh()");
    const auto pumpBody = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::PumpObjectReferencePickerEntriesRefresh()");
    const auto workerBody = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::StartNextObjectReferencePickerEntriesRefresh()");

    EXPECT_NE(requestBody.find("GetPublishedState()"), std::string::npos)
        << "Picker refresh should retain the facade's coherent immutable state.";
    EXPECT_NE(requestBody.find("snapshotIndex"), std::string::npos)
        << "Picker refresh should build from the validated snapshot index in that state.";
    EXPECT_EQ(requestBody.find("m_projectAssetSubAssetSnapshotView"), std::string::npos)
        << "The panel-local legacy snapshot view must not remain a second source of truth.";
    EXPECT_NE(workerBody.find("BuildObjectReferencePickerEntriesFromSnapshots"), std::string::npos)
        << "Object reference picker startup work should build from immutable snapshot records.";
    EXPECT_EQ(requestBody.find("m_projectAssetDatabaseSnapshot"), std::string::npos)
        << "Snapshot-view picker refresh must not recapture the asset database snapshot and redo freshness scans.";
    EXPECT_EQ(workerBody.find("databaseSnapshot"), std::string::npos)
        << "Snapshot-view picker refresh must not recapture the asset database snapshot and redo freshness scans.";
    EXPECT_NE(pumpBody.find("m_projectAssetDatabaseRefresh.has_value()"), std::string::npos)
        << "Picker snapshot-view refresh must wait while the asset database is being rebuilt.";
    EXPECT_NE(pumpBody.find("m_projectAssetDatabaseRefreshQueuedAfterInFlight"), std::string::npos)
        << "Picker snapshot-view refresh must wait for a queued asset database rebuild instead of publishing stale entries.";
    EXPECT_EQ(workerBody.find("BuildObjectReferencePickerEntries(*databaseSnapshot)"), std::string::npos)
        << "Picker refresh must not redo fresh database manifest checks after startup cache validation.";
    EXPECT_EQ(workerBody.find("ForEachFreshObjectReferencePickerAssetSnapshot"), std::string::npos)
        << "The Asset Browser startup path should not trigger an object-reference freshness scan.";
}

TEST(AssetBrowserPresentationTests, ProjectAssetDisplayRebuildSchedulesOffUiPresentationBundle)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto body = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::RebuildProjectAssetDisplayItemsIfNeeded()");

	EXPECT_NE(body.find("StartCurrentFolderItemsRefresh"), std::string::npos);
	EXPECT_EQ(body.find("BuildFilteredAssetBrowserDisplayItems"), std::string::npos);
	EXPECT_EQ(body.find("snapshotValue.subAssets"), std::string::npos)
		<< "The UI draw path must not rebuild every generated child.";
	EXPECT_EQ(body.find("m_projectAssetSubAssetItems"), std::string::npos)
		<< "The panel must not copy every project child into a second mutable cache.";

	const auto workerStart = ExtractFunctionBody(
		source,
		"void Editor::Panels::AssetBrowser::StartNextCurrentFolderItemsRefresh()");
	EXPECT_NE(workerStart.find("BuildAssetBrowserPresentationBundle"), std::string::npos);
	EXPECT_EQ(workerStart.find("[this"), std::string::npos);
}

TEST(AssetBrowserPresentationTests, ObjectReferencePickerRefreshInvalidatesWhenDatabaseRefreshStarts)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto body = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::RebuildProjectAssetPresentation(");
    const auto refreshStart = body.find("if (refreshScheduling.startRefresh)");
    ASSERT_NE(refreshStart, std::string::npos);
    const auto refreshStartEnd = body.find("if (refreshPlan.rebuildCurrentFolderItems)", refreshStart);
    ASSERT_NE(refreshStartEnd, std::string::npos);
    const auto refreshStartBlock = body.substr(refreshStart, refreshStartEnd - refreshStart);

    const auto databaseSchedule = refreshStartBlock.find(
        "m_projectAssetDatabaseRefresh = AssetDatabaseRefresh");
    ASSERT_NE(databaseSchedule, std::string::npos);
    const auto expectInvalidatedBeforeScheduling =
        [&refreshStartBlock, databaseSchedule](const std::string_view statement)
        {
            const auto invalidation = refreshStartBlock.find(statement);
            EXPECT_NE(invalidation, std::string::npos) << statement;
            EXPECT_LT(invalidation, databaseSchedule) << statement;
        };

    expectInvalidatedBeforeScheduling("InvalidateObjectReferencePickerEntriesRefresh();");
    expectInvalidatedBeforeScheduling("NLS::Editor::Assets::SetObjectReferencePickerEntries({});");
    expectInvalidatedBeforeScheduling("m_objectReferencePickerRefreshRequested = false;");
    expectInvalidatedBeforeScheduling("m_projectAssetDatabaseReady = false;");
    expectInvalidatedBeforeScheduling("m_projectAssetSubAssetSnapshotIndex.reset();");
    expectInvalidatedBeforeScheduling("RetireCurrentProjectAssetDatabase();");
    expectInvalidatedBeforeScheduling("m_selectedProjectItem.reset();");
    expectInvalidatedBeforeScheduling("m_unfilteredCurrentFolderItems.clear();");
    expectInvalidatedBeforeScheduling("m_currentFolderItems.clear();");
    expectInvalidatedBeforeScheduling("m_projectDisplayItems.clear();");
    expectInvalidatedBeforeScheduling("MarkProjectAssetDisplayItemsDirty();");
    expectInvalidatedBeforeScheduling("m_visibleThumbnailItems.clear();");
    expectInvalidatedBeforeScheduling("m_visibleThumbnailItemsKnown = false;");
    expectInvalidatedBeforeScheduling("m_thumbnailResultsByItemKey.clear();");
    expectInvalidatedBeforeScheduling("m_thumbnailItemKeyByCacheKey.clear();");
    expectInvalidatedBeforeScheduling("m_thumbnailService.ClearQueuedRequests();");
    EXPECT_EQ(refreshStartBlock.find("selectedGeneratedItem"), std::string::npos)
        << "Until selection has structured identity, every stale selection must fail closed.";
    EXPECT_EQ(refreshStartBlock.find("m_expandedProjectAssetItems.clear()"), std::string::npos)
        << "Database refresh should preserve the user's expanded source paths.";
    EXPECT_EQ(body.find("hasStableProjectAssetDatabase"), std::string::npos)
        << "A stable old database is still stale as soon as its replacement refresh starts.";
}

TEST(AssetBrowserPresentationTests, AssetBrowserAsyncRefreshStateRequiresExplicitRetryAfterFailure)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserAsyncRefreshState state;
    EXPECT_EQ(state.status, AssetBrowserAsyncRefreshStatus::Idle);
    EXPECT_TRUE(state.diagnostic.empty());

    BeginAssetBrowserAsyncRefresh(state);
    EXPECT_EQ(state.status, AssetBrowserAsyncRefreshStatus::Loading);
    FailAssetBrowserAsyncRefresh(state, "refresh failed: fixture");
    EXPECT_EQ(state.status, AssetBrowserAsyncRefreshStatus::Failure);
    EXPECT_EQ(state.diagnostic, "refresh failed: fixture");

    ResetAssetBrowserAsyncRefresh(state);
    EXPECT_EQ(state.status, AssetBrowserAsyncRefreshStatus::Idle);
    EXPECT_TRUE(state.diagnostic.empty());
    BeginAssetBrowserAsyncRefresh(state);
    CompleteAssetBrowserAsyncRefresh(state);
    EXPECT_EQ(state.status, AssetBrowserAsyncRefreshStatus::Success);
    EXPECT_TRUE(state.diagnostic.empty());

    CloseAssetBrowserAsyncRefresh(state);
    EXPECT_EQ(state.status, AssetBrowserAsyncRefreshStatus::Closed);
    BeginAssetBrowserAsyncRefresh(state);
    EXPECT_EQ(state.status, AssetBrowserAsyncRefreshStatus::Closed)
        << "Late work must not reopen a closed refresh state.";
}

TEST(AssetBrowserPresentationTests, LatestAsyncRequestCoordinatorCoalescesToOneActiveAndNewestPendingKey)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserLatestRequestCoordinator<std::string> coordinator;
    EXPECT_EQ(
        QueueAssetBrowserLatestRequest(coordinator, std::string("folder:A")),
        AssetBrowserLatestRequestDisposition::StartNow);
    EXPECT_FALSE(coordinator.activeKey.has_value());
    const auto activeA = ActivateAssetBrowserLatestRequest(coordinator);
    ASSERT_TRUE(activeA.has_value());
    ASSERT_TRUE(coordinator.activeKey.has_value());
    EXPECT_EQ(*coordinator.activeKey, "folder:A");

    EXPECT_EQ(
        QueueAssetBrowserLatestRequest(coordinator, std::string("folder:B")),
        AssetBrowserLatestRequestDisposition::PendingReplaced);
    EXPECT_EQ(
        QueueAssetBrowserLatestRequest(coordinator, std::string("folder:C")),
        AssetBrowserLatestRequestDisposition::PendingReplaced);
    ASSERT_TRUE(coordinator.pendingKey.has_value());
    EXPECT_EQ(*coordinator.pendingKey, "folder:C");

    const auto staleCompletion = CompleteAssetBrowserLatestRequest(coordinator, std::string("folder:A"));
    EXPECT_FALSE(staleCompletion.publish);
    ASSERT_TRUE(staleCompletion.nextKey.has_value());
    EXPECT_EQ(*staleCompletion.nextKey, "folder:C");
    EXPECT_FALSE(coordinator.activeKey.has_value());
    EXPECT_TRUE(coordinator.pendingKey.has_value());

    const auto activeC = ActivateAssetBrowserLatestRequest(coordinator);
    ASSERT_TRUE(activeC.has_value());
    const auto latestCompletion = CompleteAssetBrowserLatestRequest(coordinator, std::string("folder:C"));
    EXPECT_TRUE(latestCompletion.publish);
    EXPECT_FALSE(latestCompletion.nextKey.has_value());
}

TEST(AssetBrowserPresentationTests, LatestAsyncRequestCoordinatorPreservesActiveOnMismatchedCompletion)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserLatestRequestCoordinator<std::string> coordinator;
    ASSERT_EQ(
        QueueAssetBrowserLatestRequest(coordinator, std::string("folder:A")),
        AssetBrowserLatestRequestDisposition::StartNow);
    ASSERT_TRUE(ActivateAssetBrowserLatestRequest(coordinator).has_value());
    ASSERT_EQ(
        QueueAssetBrowserLatestRequest(coordinator, std::string("folder:B")),
        AssetBrowserLatestRequestDisposition::PendingReplaced);

    const auto mismatched = CompleteAssetBrowserLatestRequest(coordinator, std::string("folder:wrong"));
    EXPECT_FALSE(mismatched.publish);
    EXPECT_FALSE(mismatched.nextKey.has_value());
    ASSERT_TRUE(coordinator.activeKey.has_value());
    EXPECT_EQ(*coordinator.activeKey, "folder:A");
    ASSERT_TRUE(coordinator.pendingKey.has_value());
    EXPECT_EQ(*coordinator.pendingKey, "folder:B");
}

TEST(AssetBrowserPresentationTests, LatestAsyncRequestCoordinatorCoalescesAtoBtoAAndClosesActiveState)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserLatestRequestCoordinator<std::string> coordinator;
    ASSERT_EQ(
        QueueAssetBrowserLatestRequest(coordinator, std::string("folder:A")),
        AssetBrowserLatestRequestDisposition::StartNow);
    ASSERT_TRUE(ActivateAssetBrowserLatestRequest(coordinator).has_value());
    ASSERT_EQ(
        QueueAssetBrowserLatestRequest(coordinator, std::string("folder:B")),
        AssetBrowserLatestRequestDisposition::PendingReplaced);
    EXPECT_EQ(
        QueueAssetBrowserLatestRequest(coordinator, std::string("folder:A")),
        AssetBrowserLatestRequestDisposition::ActiveUnchanged);
    EXPECT_FALSE(coordinator.pendingKey.has_value());

    const auto activeCompletion = CompleteAssetBrowserLatestRequest(coordinator, std::string("folder:A"));
    EXPECT_TRUE(activeCompletion.publish);
    EXPECT_FALSE(activeCompletion.nextKey.has_value());

    ASSERT_EQ(
        QueueAssetBrowserLatestRequest(coordinator, std::string("folder:C")),
        AssetBrowserLatestRequestDisposition::StartNow);
    ASSERT_TRUE(ActivateAssetBrowserLatestRequest(coordinator).has_value());
    CloseAssetBrowserLatestRequestCoordinator(coordinator);
    EXPECT_FALSE(coordinator.desiredKey.has_value());
    EXPECT_FALSE(coordinator.activeKey.has_value());
    EXPECT_FALSE(coordinator.pendingKey.has_value());
}

TEST(AssetBrowserPresentationTests, AssetBrowserBuildOptionsEqualityCoversEverySemanticField)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserBuildOptions baseline;
    AssetBrowserBuildOptions changed = baseline;
    EXPECT_EQ(baseline, changed);

    changed.includeGeneratedSubAssets = !baseline.includeGeneratedSubAssets;
    EXPECT_NE(baseline, changed);
    changed = baseline;
    changed.verifyGeneratedSubAssetManifests = !baseline.verifyGeneratedSubAssetManifests;
    EXPECT_NE(baseline, changed);
    changed = baseline;
    changed.loadSourceAssetMetadataWithoutDatabase = !baseline.loadSourceAssetMetadataWithoutDatabase;
    EXPECT_NE(baseline, changed);
    changed = baseline;
    changed.expandedSourceAssets.insert("Assets/Models/Hero.fbx");
    EXPECT_NE(baseline, changed);
    changed = baseline;
    changed.searchQuery = "mesh";
    EXPECT_NE(baseline, changed);
    changed = baseline;
    changed.typeFilter = AssetBrowserItemType::Mesh;
    EXPECT_NE(baseline, changed);
}

TEST(AssetBrowserPresentationTests, FolderAndPickerUseStructuredKeysWithoutRetiredWorkersOrEpochs)
{
    const auto header = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.h"));
    EXPECT_NE(header.find("struct CurrentFolderItemsRefreshKey"), std::string::npos);
    EXPECT_NE(header.find("databaseSnapshotIdentity"), std::string::npos);
    EXPECT_NE(header.find("snapshotIndexIdentity"), std::string::npos);
    EXPECT_NE(header.find("sourceStateIdentity"), std::string::npos)
        << "Each explicit folder refresh must carry a retained identity because the worker reads the live filesystem.";
    EXPECT_NE(header.find("buildOptions"), std::string::npos);
    EXPECT_NE(header.find("struct ObjectReferencePickerRefreshKey"), std::string::npos);
    EXPECT_NE(header.find("snapshotIndexIdentity"), std::string::npos);
    EXPECT_NE(header.find("lifetimeIdentity"), std::string::npos);
    EXPECT_EQ(header.find("m_retiredCurrentFolderItemsRefreshes"), std::string::npos);
    EXPECT_EQ(header.find("m_retiredObjectReferencePickerRefreshes"), std::string::npos);
    EXPECT_EQ(header.find("m_objectReferencePickerRefreshGeneration"), std::string::npos)
        << "Semantic identities must not regress to wrapping numeric epochs.";

    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto folderPump = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::PumpCurrentFolderItemsRefresh()");
    EXPECT_NE(folderPump.find("CompleteAssetBrowserLatestRequest"), std::string::npos);
    EXPECT_EQ(folderPump.find("m_currentFolderItemsRefreshGeneration"), std::string::npos);
    const auto folderStart = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::StartNextCurrentFolderItemsRefresh()");
    EXPECT_NE(folderStart.find("m_currentFolderItemsRefresh.has_value()"), std::string::npos);
    EXPECT_EQ(folderStart.find("[this"), std::string::npos);
    const auto pickerPump = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::PumpObjectReferencePickerEntriesRefresh()");
    EXPECT_NE(pickerPump.find("CompleteAssetBrowserLatestRequest"), std::string::npos);
    EXPECT_EQ(pickerPump.find("generation"), std::string::npos);
    const auto pickerStart = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::StartNextObjectReferencePickerEntriesRefresh()");
    EXPECT_NE(pickerStart.find("m_objectReferencePickerRefresh.has_value()"), std::string::npos);
    EXPECT_EQ(pickerStart.find("[this"), std::string::npos);
    const auto pickerRequest = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::RequestObjectReferencePickerEntriesRefresh()");
    EXPECT_NE(pickerRequest.find("request.key.snapshotIndexIdentity = request.snapshotIndex.get()"), std::string::npos);
    EXPECT_NE(pickerRequest.find("request.key.lifetimeIdentity = m_objectReferencePickerLifetimeIdentity.get()"), std::string::npos);
}

TEST(AssetBrowserPresentationTests, AsyncRefreshFailuresRetainDiagnosticsWithoutUnchangedPickerRetry)
{
    const auto header = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.h"));
    EXPECT_NE(header.find("AssetBrowserAsyncRefreshState m_projectAssetDatabaseRefreshState"), std::string::npos);
    EXPECT_NE(header.find("AssetBrowserAsyncRefreshState m_currentFolderItemsRefreshState"), std::string::npos);
    EXPECT_NE(header.find("AssetBrowserAsyncRefreshState m_objectReferencePickerRefreshState"), std::string::npos);
    EXPECT_NE(header.find("std::string diagnostic"), std::string::npos)
        << "Worker results must carry contextual failure diagnostics across the future boundary.";

    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto rebuildBody = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::RebuildProjectAssetPresentation(");
    EXPECT_NE(rebuildBody.find("BeginAssetBrowserAsyncRefresh(m_projectAssetDatabaseRefreshState)"), std::string::npos);
    EXPECT_NE(rebuildBody.find("result.diagnostic"), std::string::npos);
    EXPECT_EQ(rebuildBody.find("if (!result.database->Refresh())\n\t\t\t\t\t\treturn {};"), std::string::npos)
        << "Database worker failure must not collapse into an undiagnosed empty result.";

    const auto folderStart = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::StartNextCurrentFolderItemsRefresh()");
    EXPECT_NE(folderStart.find("BeginAssetBrowserAsyncRefresh(m_currentFolderItemsRefreshState)"), std::string::npos);
    EXPECT_NE(folderStart.find("result.diagnostic"), std::string::npos);
    const auto folderPump = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::PumpCurrentFolderItemsRefresh()");
    EXPECT_NE(folderPump.find("FailAssetBrowserAsyncRefresh("), std::string::npos);
    EXPECT_NE(folderPump.find("m_currentFolderItemsRefreshState"), std::string::npos);
    EXPECT_EQ(folderPump.find("items.clear()"), std::string::npos)
        << "A folder exception must not publish an indistinguishable empty success.";

    const auto pickerRequest = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::RequestObjectReferencePickerEntriesRefresh()");
    EXPECT_NE(pickerRequest.find("ResetAssetBrowserAsyncRefresh(m_objectReferencePickerRefreshState)"), std::string::npos);
    const auto pickerPump = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::PumpObjectReferencePickerEntriesRefresh()");
    EXPECT_NE(pickerPump.find("FailAssetBrowserAsyncRefresh("), std::string::npos);
    EXPECT_NE(pickerPump.find("m_objectReferencePickerRefreshState"), std::string::npos);
    EXPECT_EQ(pickerPump.find("m_objectReferencePickerRefreshRequested = true"), std::string::npos)
        << "Failure must wait for an explicit new request rather than retry unchanged every frame.";

    const auto destructor = ExtractFunctionBody(
        source,
        "Editor::Panels::AssetBrowser::~AssetBrowser()");
    EXPECT_NE(destructor.find("CloseAssetBrowserAsyncRefresh(m_projectAssetDatabaseRefreshState)"), std::string::npos);
    EXPECT_NE(destructor.find("CloseAssetBrowserAsyncRefresh(m_currentFolderItemsRefreshState)"), std::string::npos);
    EXPECT_NE(destructor.find("CloseAssetBrowserAsyncRefresh(m_objectReferencePickerRefreshState)"), std::string::npos);
}

TEST(AssetBrowserPresentationTests, PendingDatabaseRefreshUsesSourceOnlyFolderFallbackAndRejectsStaleResults)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto rebuildBody = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::RebuildProjectAssetPresentation(");

    const auto refreshStart = rebuildBody.find("if (refreshScheduling.startRefresh)");
    const auto retireDatabase = rebuildBody.find("RetireCurrentProjectAssetDatabase();", refreshStart);
    const auto fallbackOptions = rebuildBody.find(
        "fallbackBuildOptions.includeGeneratedSubAssets = false;",
        refreshStart);
    const auto fallbackItems = rebuildBody.find("BuildCurrentFolderAssetItems(", fallbackOptions);
    const auto fallbackNullDatabase = rebuildBody.find("nullptr", fallbackItems);
    const auto fallbackItemsEnd = rebuildBody.find("fallbackBuildOptions);", fallbackItems);
    const auto folderRefresh = rebuildBody.find("StartCurrentFolderItemsRefresh(", refreshStart);
    ASSERT_NE(refreshStart, std::string::npos);
    ASSERT_NE(retireDatabase, std::string::npos);
    ASSERT_NE(fallbackOptions, std::string::npos);
    ASSERT_NE(fallbackItems, std::string::npos)
        << "Pending database refresh must immediately publish source-only folder roots.";
    ASSERT_NE(fallbackNullDatabase, std::string::npos);
    ASSERT_NE(fallbackItemsEnd, std::string::npos);
    ASSERT_NE(folderRefresh, std::string::npos);
    EXPECT_LT(retireDatabase, fallbackItems)
        << "The old database snapshot must be retired before fallback roots are published.";
    EXPECT_LT(fallbackNullDatabase, fallbackItemsEnd)
        << "Fallback roots must not observe the database while its replacement is pending.";
    EXPECT_LT(retireDatabase, folderRefresh)
        << "The folder job must capture the null snapshot left by database retirement.";

    const auto startFolderBody = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::StartCurrentFolderItemsRefresh(");
    EXPECT_NE(
        startFolderBody.find("request.databaseSnapshot = m_projectAssetDatabaseSnapshot;"),
        std::string::npos);
    EXPECT_NE(
        startFolderBody.find("request.snapshotIndex = m_projectAssetSubAssetSnapshotIndex;"),
        std::string::npos);
    EXPECT_NE(startFolderBody.find("request.key.databaseSnapshotIdentity = request.databaseSnapshot.get();"), std::string::npos);
    const auto startNextFolderBody = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::StartNextCurrentFolderItemsRefresh()");
    EXPECT_NE(startNextFolderBody.find("request.databaseSnapshot.get()"), std::string::npos);

    const auto pumpFolderBody = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::PumpCurrentFolderItemsRefresh()");
    const auto generationCheck = pumpFolderBody.find("CompleteAssetBrowserLatestRequest(");
    const auto rejectStale = pumpFolderBody.find("if (completion.publish", generationCheck);
    const auto publishItems = pumpFolderBody.find(
        "m_unfilteredCurrentFolderItems = std::move(result.bundle.rootItems)",
        rejectStale);
    ASSERT_NE(generationCheck, std::string::npos);
    ASSERT_NE(rejectStale, std::string::npos);
    ASSERT_NE(publishItems, std::string::npos);
    EXPECT_LT(generationCheck, rejectStale);
    EXPECT_LT(rejectStale, publishItems)
        << "A prior folder semantic key must be rejected before it can republish stale children.";
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
    EXPECT_TRUE(folderSelection.clearCurrentFolderItemsBeforeAsyncRefresh);

    const auto filterChange = BuildAssetBrowserRefreshPlan(AssetBrowserRefreshReason::FilterChange);
    EXPECT_FALSE(filterChange.refreshAssetDatabase);
    EXPECT_FALSE(filterChange.rebuildFolderTree);
    EXPECT_FALSE(filterChange.rebuildCurrentFolderItems);
    EXPECT_FALSE(filterChange.clearCurrentFolderItemsBeforeAsyncRefresh);

    EXPECT_TRUE(initial.rebuildCurrentFolderItems);
    EXPECT_TRUE(initial.clearCurrentFolderItemsBeforeAsyncRefresh);
    EXPECT_TRUE(mutation.rebuildCurrentFolderItems);
    EXPECT_TRUE(mutation.clearCurrentFolderItemsBeforeAsyncRefresh);
    EXPECT_TRUE(folderSelection.rebuildCurrentFolderItems);
    EXPECT_TRUE(databaseReady.rebuildCurrentFolderItems);
    EXPECT_FALSE(databaseReady.clearCurrentFolderItemsBeforeAsyncRefresh);
}

TEST(AssetBrowserPresentationTests, CurrentFolderAsyncRefreshDoesNotOwnPresentationGeneration)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto body = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::StartCurrentFolderItemsRefresh(");

    EXPECT_EQ(body.find("++m_projectAssetPresentationGeneration"), std::string::npos)
        << "Current folder jobs must use the generation assigned by RebuildProjectAssetPresentation; "
           "incrementing it in StartCurrentFolderItemsRefresh can make the completed job reject its own result "
           "and leave the Asset Browser stuck on loading placeholders.";
}

TEST(AssetBrowserPresentationTests, CurrentFolderAsyncRefreshBuildsFilteredBundleOffUiAndPublishesAtomically)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto workerBody = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::StartNextCurrentFolderItemsRefresh()");
    const auto completionBody = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::PumpCurrentFolderItemsRefresh()");

    EXPECT_NE(workerBody.find("BuildAssetBrowserPresentationBundle"), std::string::npos);
    EXPECT_NE(workerBody.find("request.key.buildOptions"), std::string::npos)
        << "The worker must apply the captured expansion, search, and type-filter options.";
    EXPECT_NE(completionBody.find("result.bundle.rootItems"), std::string::npos);
    EXPECT_NE(completionBody.find("result.bundle.visibleItems"), std::string::npos);
    EXPECT_NE(completionBody.find("result.bundle.displayItems"), std::string::npos);
    EXPECT_EQ(completionBody.find("FilterAssetBrowserItems"), std::string::npos)
        << "Completion should publish the coherent worker-built bundle without rebuilding it on the UI thread.";
}

TEST(AssetBrowserPresentationTests, CurrentFolderRefreshPublishesFilesystemFallbackBeforeAsyncRefresh)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto body = ExtractFunctionBody(
        source,
        "void Editor::Panels::AssetBrowser::RebuildProjectAssetPresentation(");
    const auto fallback = body.find("BuildCurrentFolderAssetItems(");
    const auto asyncStart = body.find("StartCurrentFolderItemsRefresh(");

    EXPECT_NE(fallback, std::string::npos)
        << "Initial and folder-selection refreshes must synchronously publish a filesystem fallback "
           "so the Asset Browser never stays blank while the richer async refresh is queued.";
    EXPECT_NE(asyncStart, std::string::npos);
    if (fallback != std::string::npos && asyncStart != std::string::npos)
        EXPECT_LT(fallback, asyncStart);
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

TEST(AssetBrowserPresentationTests, ExternalDroppedFileQueuePreservesAcceptedIntentUntilHovered)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserExternalDroppedFileQueue queue;

    EnqueueAssetBrowserExternalDroppedFiles(queue, {"C:/Imports/Hero.fbx"});
    EnqueueAssetBrowserExternalDroppedFiles(queue, {"C:/Imports/Hero.png", ""});

    EXPECT_FALSE(ConsumeAssetBrowserExternalDroppedFiles(queue, false).has_value())
        << "A window-level file drop must stay queued until the asset folder can consume it.";
    ASSERT_EQ(queue.size(), 2u);

    auto first = ConsumeAssetBrowserExternalDroppedFiles(queue, true);
    ASSERT_TRUE(first.has_value());
    ASSERT_EQ(first->size(), 1u);
    EXPECT_EQ((*first)[0], "C:/Imports/Hero.fbx");
    ASSERT_EQ(queue.size(), 1u);

    auto second = ConsumeAssetBrowserExternalDroppedFiles(queue, true);
    ASSERT_TRUE(second.has_value());
    ASSERT_EQ(second->size(), 2u);
    EXPECT_EQ((*second)[0], "C:/Imports/Hero.png");
    EXPECT_TRUE((*second)[1].empty());
    EXPECT_TRUE(queue.empty());
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

TEST(AssetBrowserPresentationTests, PublishedSnapshotRejectsInvalidAssetIdAndSubAssetKeys)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    EditorAssetSnapshot snapshot;
    snapshot.sourceAssetPath = "Assets/Models/Hero.gltf";
    auto index = BuildValidatedEditorAssetSnapshotIndex({snapshot});
    ASSERT_NE(index, nullptr);
    EXPECT_EQ(index->status, EditorAssetSnapshotStatus::Error);
    EXPECT_EQ(index->diagnostic.code, "asset-snapshot-invalid-asset-id");
    EXPECT_TRUE(index->assets.empty());

    snapshot.assetId = ParseAssetId("11111111-1111-4111-8111-111111111111");
    snapshot.subAssets = {{ {}, "Library/Artifacts/mesh.bin", ArtifactType::Mesh, "Body" }};
    index = BuildValidatedEditorAssetSnapshotIndex({snapshot});
    ASSERT_NE(index, nullptr);
    EXPECT_EQ(index->status, EditorAssetSnapshotStatus::Error);
    EXPECT_EQ(index->diagnostic.code, "asset-snapshot-empty-sub-asset-key");
    EXPECT_TRUE(index->assets.empty());

    snapshot.subAssets = {
        { "mesh:Body", "Library/Artifacts/mesh.bin", ArtifactType::Mesh, "Body" },
        { "mesh:Body", "Library/Artifacts/material.bin", ArtifactType::Material, "Body" }
    };
    index = BuildValidatedEditorAssetSnapshotIndex({snapshot});
    ASSERT_NE(index, nullptr);
    EXPECT_EQ(index->status, EditorAssetSnapshotStatus::Error);
    EXPECT_EQ(index->diagnostic.code, "asset-snapshot-duplicate-sub-asset-key");
    EXPECT_TRUE(index->assets.empty());

    snapshot.subAssets = {{ "mesh:Body", "Library/Artifacts/mesh.bin", ArtifactType::Mesh, "Body" }};
    auto duplicateSource = snapshot;
    duplicateSource.sourceAssetPath = "Assets\\Models\\Hero.gltf";
    duplicateSource.assetId = ParseAssetId("22222222-2222-4222-8222-222222222222");
    index = BuildValidatedEditorAssetSnapshotIndex({snapshot, duplicateSource});
    ASSERT_NE(index, nullptr);
    EXPECT_EQ(index->status, EditorAssetSnapshotStatus::Error);
    EXPECT_EQ(index->diagnostic.code, "asset-snapshot-duplicate-source-path");
    EXPECT_TRUE(index->assets.empty())
        << "Canonical source-path conflicts must reject both identities instead of selecting one arbitrarily.";
}

TEST(AssetBrowserPresentationTests, PublishedSnapshotQuarantinesInvalidSourceWithoutHidingValidPrefabChildren)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    EditorAssetSnapshot prefabSnapshot;
    prefabSnapshot.sourceAssetPath = "Assets/Prefabs/Lamp.prefab";
    prefabSnapshot.assetId = ParseAssetId("11111111-1111-4111-8111-111111111111");
    prefabSnapshot.subAssets = {{
        "prefab:Lamp",
        "Library/Artifacts/lamp.prefab",
        ArtifactType::Prefab,
        "Lamp"
    }};

    EditorAssetSnapshot invalidModelSnapshot;
    invalidModelSnapshot.sourceAssetPath = "Assets/Models/Broken.fbx";
    invalidModelSnapshot.assetId = ParseAssetId("22222222-2222-4222-8222-222222222222");
    invalidModelSnapshot.subAssets = {
        { "mesh:Body", "Library/Artifacts/body.mesh", ArtifactType::Mesh, "Body" },
        { "mesh:Body", "Library/Artifacts/body-duplicate.mesh", ArtifactType::Mesh, "Body" }
    };

    const auto index = BuildValidatedEditorAssetSnapshotIndex({
        prefabSnapshot,
        invalidModelSnapshot
    });
    ASSERT_NE(index, nullptr);
    EXPECT_EQ(index->status, EditorAssetSnapshotStatus::Valid);
    EXPECT_EQ(index->diagnostic.code, "asset-snapshot-duplicate-sub-asset-key");
    ASSERT_EQ(index->assets.size(), 1u);
    EXPECT_EQ(index->assets[0].sourceAssetPath, prefabSnapshot.sourceAssetPath);
    EXPECT_TRUE(index->assetIndexByCanonicalSourcePath.contains(prefabSnapshot.sourceAssetPath));
    EXPECT_FALSE(index->assetIndexByCanonicalSourcePath.contains(invalidModelSnapshot.sourceAssetPath));

    AssetBrowserItem prefab;
    prefab.displayName = "Lamp.prefab";
    prefab.projectRelativePath = prefabSnapshot.sourceAssetPath;
    prefab.sourceAssetPath = prefabSnapshot.sourceAssetPath;
    prefab.kind = AssetBrowserItemKind::SourceAsset;
    prefab.type = AssetBrowserItemType::Prefab;
    prefab.assetId = prefabSnapshot.assetId;

    AssetBrowserBuildOptions options;
    options.includeGeneratedSubAssets = true;
    const auto bundle = BuildAssetBrowserPresentationBundle({prefab}, index.get(), options);
    ASSERT_EQ(bundle.displayItems.size(), 1u);
    EXPECT_EQ(bundle.displayItems[0].childCount, 1u)
        << "One malformed source must not remove disclosure buttons from unrelated valid prefabs.";
}

TEST(AssetBrowserPresentationTests, FacadeBuildsPublishedCandidatesOffLockAndClearsBeforeRefreshFailure)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Assets/AssetDatabaseFacade.cpp"));
    const auto invalidateBody = ExtractFunctionBody(
        source,
        "void AssetDatabaseFacade::PublishCurrentStateLocked() const");
    EXPECT_NE(invalidateBody.find("m_currentStateIdentity"), std::string::npos);
    EXPECT_EQ(invalidateBody.find("EditorAssetSnapshotIndex"), std::string::npos);
    EXPECT_EQ(invalidateBody.find("for ("), std::string::npos)
        << "The locked writer path must remain an O(1) identity invalidation.";

    const auto publishBody = ExtractFunctionBody(
        source,
        "std::shared_ptr<const FacadePublishedState> AssetDatabaseFacade::GetPublishedState() const");
    const auto captureLock = publishBody.find("std::lock_guard manifestLock");
    const auto buildCandidate = publishBody.find("BuildValidatedEditorAssetSnapshotIndex");
    const auto commitLock = publishBody.find("std::lock_guard manifestLock", captureLock + 1u);
    ASSERT_NE(captureLock, std::string::npos);
    ASSERT_NE(buildCandidate, std::string::npos);
    ASSERT_NE(commitLock, std::string::npos);
    EXPECT_LT(captureLock, buildCandidate);
    EXPECT_LT(buildCandidate, commitLock)
        << "Snapshot validation and deep identity reuse comparison must execute between capture and commit locks.";

    const auto refreshBody = ExtractFunctionBody(source, "bool AssetDatabaseFacade::Refresh()");
    const auto clearSnapshots = refreshBody.find("m_objectReferencePickerAssetSnapshots =");
    const auto publishCleared = refreshBody.find("(void)GetPublishedState()", clearSnapshots);
    const auto firstFailure = refreshBody.find("if (!FlushArtifactDatabaseCache())", clearSnapshots);
    ASSERT_NE(clearSnapshots, std::string::npos);
    ASSERT_NE(publishCleared, std::string::npos);
    ASSERT_NE(firstFailure, std::string::npos);
    EXPECT_LT(clearSnapshots, publishCleared);
    EXPECT_LT(publishCleared, firstFailure);
}

TEST(AssetBrowserPresentationTests, FacadePublishedStateReusesWholeAndSnapshotIdentitiesAtTheirOwnLevels)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto first = database.GetPublishedState();
    const auto unchanged = database.GetPublishedState();
    ASSERT_NE(first, nullptr);
    ASSERT_NE(first->snapshotIndex, nullptr);
    EXPECT_EQ(first, unchanged);

    ASSERT_TRUE(database.Refresh());
    const auto refreshed = database.GetPublishedState();
    ASSERT_NE(refreshed, nullptr);
    ASSERT_NE(refreshed->snapshotIndex, nullptr);
    EXPECT_NE(first, refreshed)
        << "A new manifest-map base identity must produce a new aggregate state.";
    EXPECT_EQ(first->snapshotIndex, refreshed->snapshotIndex)
        << "Equal validated snapshot payloads should retain their independent immutable identity.";

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, FailedFullRefreshClearsPreviouslyPublishedSnapshotPayload)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    const std::string sourcePath = "Assets/Models/Hero.gltf";
    WriteTextFile(root / sourcePath, R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    const auto modelId = ParseAssetId(database.AssetPathToGUID(sourcePath));
    ASSERT_TRUE(modelId.IsValid());

    ArtifactManifest manifest;
    manifest.sourceAssetId = modelId;
    manifest.importerId = "scene-model";
    manifest.importerVersion =
        NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "mesh:Body";
    manifest.subAssets.push_back(MakeArtifact(modelId, "mesh:Body", ArtifactType::Mesh, "mesh"));
    WriteManifestArtifactFiles(root, manifest);
    AddCurrentSourceDependencies(root, manifest, sourcePath);
    database.AddArtifactManifest(manifest);
    ASSERT_TRUE(database.IsArtifactManifestKnownCurrentForAssetPath(sourcePath));

    const auto populated = database.GetPublishedState();
    ASSERT_NE(populated, nullptr);
    ASSERT_NE(populated->snapshotIndex, nullptr);
    ASSERT_EQ(populated->snapshotIndex->status, EditorAssetSnapshotStatus::Valid);
    ASSERT_EQ(populated->snapshotIndex->assets.size(), 1u);

    std::filesystem::remove_all(root / "Assets");
    WriteTextFile(root / "Assets", "not a directory\n");
    EXPECT_FALSE(database.Refresh());
    const auto failed = database.GetPublishedState();
    ASSERT_NE(failed, nullptr);
    ASSERT_NE(failed->artifactManifests, nullptr);
    ASSERT_NE(failed->knownCurrentAssetPaths, nullptr);
    ASSERT_NE(failed->snapshotIndex, nullptr);
    EXPECT_TRUE(failed->artifactManifests->empty());
    EXPECT_TRUE(failed->knownCurrentAssetPaths->empty());
    EXPECT_TRUE(failed->snapshotIndex->assets.empty());
    EXPECT_TRUE(failed->snapshotIndex->assetIndexByCanonicalSourcePath.empty());

    std::filesystem::remove_all(root);
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
    options.expandedSourceAssets.insert("Assets/Models/Hero.gltf");
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
    options.expandedSourceAssets.insert("Assets/Models/Hero.gltf");
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
    options.expandedSourceAssets.insert("Assets/Models/Hero.gltf");
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
    options.expandedSourceAssets.insert("Assets/Models/Hero.gltf");
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
    options.expandedSourceAssets.insert("Assets/Models/Hero.gltf");
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
    EXPECT_TRUE(IsEditorAssetDragPayloadGeneratedBrowserSubAsset(*meshPayload));
    EXPECT_FALSE(CanMoveEditorAssetDragPayloadAsPhysicalProjectFile(*meshPayload));

    const auto sourceFilePayload = MakeEditorAssetDragPayload(
        "Assets/Models/Hero.gltf",
        modelId,
        "prefab:Hero",
        ArtifactType::Prefab,
        true,
        true);
    EXPECT_FALSE(IsEditorAssetDragPayloadGeneratedBrowserSubAsset(sourceFilePayload));
    EXPECT_TRUE(CanMoveEditorAssetDragPayloadAsPhysicalProjectFile(sourceFilePayload));

    const auto* source = FindItem(items, "Hero.gltf");
    ASSERT_NE(source, nullptr);
    const auto sourcePayload = MakeAssetBrowserItemDragPayload(*source, nullptr);
    ASSERT_TRUE(sourcePayload.has_value());
    EXPECT_EQ(GetEditorAssetDragPayloadPath(*sourcePayload), "Assets/Models/Hero.gltf");
    EXPECT_EQ(GetEditorAssetDragPayloadAssetId(*sourcePayload), modelId);
    EXPECT_EQ(GetEditorAssetDragPayloadSubAssetKey(*sourcePayload), "prefab:Hero");
    EXPECT_EQ(GetEditorAssetDragPayloadArtifactType(*sourcePayload), ArtifactType::Prefab);
    EXPECT_EQ(sourcePayload->generatedModelPrefab, 0u);
    EXPECT_EQ(sourcePayload->imported, 0u);
    EXPECT_FALSE(IsEditorAssetDragPayloadGeneratedBrowserSubAsset(*sourcePayload));
    EXPECT_TRUE(CanMoveEditorAssetDragPayloadAsPhysicalProjectFile(*sourcePayload));

    AssetBrowserItem sourceMaterial;
    sourceMaterial.kind = AssetBrowserItemKind::SourceAsset;
    sourceMaterial.type = AssetBrowserItemType::Material;
    sourceMaterial.displayName = "Body.mat";
    sourceMaterial.projectRelativePath = "Assets/Materials/Body.mat";
    sourceMaterial.sourceAssetPath = sourceMaterial.projectRelativePath;
    sourceMaterial.dragResourcePath = sourceMaterial.projectRelativePath;
    sourceMaterial.assetId = modelId;

    const auto materialSourcePayload = MakeAssetBrowserItemDragPayload(sourceMaterial, nullptr);
    ASSERT_TRUE(materialSourcePayload.has_value())
        << "Source .mat files must infer a material sub-asset identity before payload validation.";
    EXPECT_EQ(GetEditorAssetDragPayloadPath(*materialSourcePayload), "Assets/Materials/Body.mat");
    EXPECT_EQ(GetEditorAssetDragPayloadAssetId(*materialSourcePayload), modelId);
    EXPECT_EQ(GetEditorAssetDragPayloadSubAssetKey(*materialSourcePayload), "material:Body");
    EXPECT_EQ(GetEditorAssetDragPayloadArtifactType(*materialSourcePayload), ArtifactType::Material);
    EXPECT_EQ(materialSourcePayload->generatedModelPrefab, 0u);
    EXPECT_EQ(materialSourcePayload->imported, 0u);
    EXPECT_FALSE(IsEditorAssetDragPayloadGeneratedBrowserSubAsset(*materialSourcePayload));
    EXPECT_TRUE(CanMoveEditorAssetDragPayloadAsPhysicalProjectFile(*materialSourcePayload));

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
    options.expandedSourceAssets.insert("Assets/Models/Hero.gltf");
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

TEST(AssetBrowserPresentationTests, StandaloneTextureManifestLoadsWithoutExpandingKnownCurrentSnapshots)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    WriteTextFile(root / "Assets" / "Textures" / "Albedo.png", "texture");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto textureId = ParseAssetId(database.AssetPathToGUID("Assets/Textures/Albedo.png"));
    ArtifactManifest textureManifest;
    textureManifest.sourceAssetId = textureId;
    textureManifest.importerId = "texture";
    textureManifest.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::Texture);
    textureManifest.targetPlatform = TextureArtifactTargetPlatformForTest();
    textureManifest.primarySubAssetKey = "texture:main";
    textureManifest.subAssets.push_back(MakeArtifact(
        textureId,
        "texture:main",
        ArtifactType::Texture,
        "texture",
        TextureArtifactTargetPlatformForTest()));
    WriteManifestArtifactFiles(root, textureManifest);
    database.AddArtifactManifest(textureManifest);

    const auto manifest = database.GetArtifactManifestForAssetPath("Assets/Textures/Albedo.png");
    ASSERT_TRUE(manifest.has_value());
    ASSERT_NE(manifest->FindSubAsset("texture:main"), nullptr);
    EXPECT_TRUE(database.LoadSubAssetAtPath("Assets/Textures/Albedo.png", "texture:main").has_value());
    EXPECT_FALSE(database.IsArtifactManifestKnownCurrentForAssetPath("Assets/Textures/Albedo.png"));
    EXPECT_TRUE(database.GetObjectReferencePickerAssetSnapshots().empty());

    std::filesystem::remove_all(root);
}

TEST(AssetBrowserPresentationTests, RestartedBrowserBuildsTextureThumbnailRequestFromPersistedManifest)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetBrowserPresentationRoot();
    WriteTextFile(root / "Assets" / "Textures" / "Albedo.png", "texture");

    {
        AssetDatabaseFacade database({root});
        ASSERT_TRUE(database.Refresh());

        const auto textureId = ParseAssetId(database.AssetPathToGUID("Assets/Textures/Albedo.png"));
        ArtifactManifest textureManifest;
        textureManifest.sourceAssetId = textureId;
        textureManifest.importerId = "texture";
        textureManifest.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::Texture);
        textureManifest.targetPlatform = TextureArtifactTargetPlatformForTest();
        textureManifest.primarySubAssetKey = "texture:main";
        textureManifest.subAssets.push_back(MakeArtifact(
            textureId,
            "texture:main",
            ArtifactType::Texture,
            "texture",
            TextureArtifactTargetPlatformForTest()));
        WriteManifestArtifactFiles(root, textureManifest);
        WritePersistedArtifactManifest(root, textureManifest);
        database.AddArtifactManifest(textureManifest);
    }

    AssetDatabaseFacade restartedDatabase({root});
    ASSERT_TRUE(restartedDatabase.Refresh());

    AssetBrowserBuildOptions options;
    options.includeGeneratedSubAssets = true;
    options.verifyGeneratedSubAssetManifests = false;
    const auto items = BuildCurrentFolderAssetItems(root, "Assets/Textures", &restartedDatabase, options);
    const auto* texture = FindItem(items, "Albedo.png");
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->kind, AssetBrowserItemKind::SourceAsset);
    EXPECT_EQ(texture->type, AssetBrowserItemType::Texture);
    ASSERT_TRUE(texture->assetId.IsValid());
    EXPECT_FALSE(texture->hasGeneratedSubAssets);
    const auto persistedManifest = restartedDatabase.GetArtifactManifestForAssetPath("Assets/Textures/Albedo.png");
    ASSERT_TRUE(persistedManifest.has_value());
    ASSERT_NE(persistedManifest->FindSubAsset("texture:main"), nullptr);

    const auto request = BuildAssetThumbnailRequestForItem(root, *texture, 96u);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->kind, AssetThumbnailKind::Texture);
    EXPECT_EQ(request->assetId, texture->assetId);
    EXPECT_EQ(request->subAssetKey, "texture:main");
    EXPECT_TRUE(std::filesystem::path(request->artifactPath).is_absolute());
    EXPECT_TRUE(std::filesystem::exists(request->artifactPath));
    EXPECT_NE(request->dependencyStamp.find("artifact-file="), std::string::npos);
    EXPECT_TRUE(restartedDatabase.GetObjectReferencePickerAssetSnapshots().empty());

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
        "Library/Artifacts/a1010101-0101-4101-8101-010101010101/" +
            StableArtifactBlobFileName(meshAssetId, "Mesh:Body"),
        ArtifactType::Mesh
    });
    snapshot.subAssets.push_back({
        "Texture:Body",
        "Library/Artifacts/a1010101-0101-4101-8101-010101010101/" +
            StableArtifactBlobFileName(meshAssetId, "Texture:Body"),
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

TEST(AssetBrowserPresentationTests, NormalizesOnlyCanonicalProjectAssetPaths)
{
    using namespace NLS::Editor::Assets;

    EXPECT_EQ(NormalizeEditorProjectAssetPath("Assets"), "Assets");
    EXPECT_EQ(NormalizeEditorProjectAssetPath("Assets/Models/../Textures/Albedo.png"),
        "Assets/Textures/Albedo.png");
    EXPECT_EQ(NormalizeEditorProjectAssetPath("Assets\\Models\\Hero.fbx"),
        "Assets/Models/Hero.fbx");
    EXPECT_TRUE(NormalizeEditorProjectAssetPath({}).empty());
    EXPECT_TRUE(NormalizeEditorProjectAssetPath("Packages/Model.fbx").empty());
    EXPECT_TRUE(NormalizeEditorProjectAssetPath("Assets/../../Outside.fbx").empty());
    EXPECT_TRUE(NormalizeEditorProjectAssetPath(std::filesystem::absolute("Assets/Model.fbx")).empty());
}

TEST(AssetBrowserPresentationTests, AssetBrowserItemEqualityIncludesEverySemanticField)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserItem item;
    item.displayName = "Body";
    item.projectRelativePath = "Assets/Models/Hero.fbx";
    item.sourceAssetPath = item.projectRelativePath;
    item.absolutePath = "D:/Project/Assets/Models/Hero.fbx";
    item.artifactPath = "Library/Artifacts/body";
    item.kind = AssetBrowserItemKind::GeneratedSubAsset;
    item.type = AssetBrowserItemType::Mesh;
    item.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("01010101-0101-4101-8101-010101010101"));
    item.subAssetKey = "mesh:Body";
    item.dragResourcePath = "Assets/Models/Hero.fbx#mesh:Body";
    item.selectionResourcePath = item.dragResourcePath;
    item.artifactType = NLS::Core::Assets::ArtifactType::Mesh;
    item.generatedReadOnly = true;
    item.previewableInAssetView = true;
    item.hasGeneratedSubAssets = true;

    const auto same = item;
    EXPECT_EQ(item, same);

    auto changed = item;
    changed.subAssetKey = "mesh:Head";
    EXPECT_NE(item, changed);
}
