#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

#include "Assets/AssetId.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/AssetBrowserPresentation.h"
#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/AssetImporterFacade.h"
#include "Assets/AssetMeta.h"
#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Assets/EditorAssetDatabase.h"
#include "Assets/EditorStartupAssetPreimport.h"
#include "Assets/NativeArtifactContainer.h"
#include "Assets/PrefabEditorWorkflow.h"
#include "Core/AssetFileWatcher.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Core/ServiceLocator.h"
#include "GameObject.h"
#include "Guid.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "Tests/Unit/TestServiceLocatorOverrides.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

#define NLS_UNREGISTERED_TEST(suite, name) static void suite##_##name##_Unregistered()
#if defined(NLS_REGISTER_LONG_RUNNING_EDITOR_ASSET_DATABASE_TESTS)
#undef TEST
#define TEST(suite, name) NLS_UNREGISTERED_TEST(suite, name)
#define NLS_LONG_RUNNING_TEST(performanceSuite, name) GTEST_TEST(performanceSuite, name)
#else
#define NLS_LONG_RUNNING_TEST(performanceSuite, name) NLS_UNREGISTERED_TEST(performanceSuite, name)
#endif

namespace
{
std::string JoinDiagnosticSummaries(const NLS::Core::Assets::AssetDiagnostics& diagnostics)
{
    std::ostringstream stream;
    for (const auto& diagnostic : diagnostics)
    {
        stream << '[' << (diagnostic.severity == NLS::Core::Assets::AssetDiagnosticSeverity::Error ? "error" : "warning")
            << "] " << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    return stream.str();
}

std::string JoinImportProgressSummaries(const NLS::Editor::Assets::ImportProgressTracker& tracker)
{
    std::ostringstream stream;
    for (uint64_t jobId = 1u; jobId < 16u; ++jobId)
    {
        for (const auto& event : tracker.GetEvents(NLS::Editor::Assets::ImportJobId {jobId}))
        {
            stream << "job=" << jobId << " path=" << event.sourcePath
                << " message=" << event.message
                << " status=" << static_cast<int>(event.terminalStatus)
                << '\n';
        }
    }
    return stream.str();
}

using NLS::Tests::ScopedServiceOverride;

std::filesystem::path MakeEditorAssetTestRoot()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_editor_asset_database_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);
    return root;
}

bool HasExecutableShaderCompilerForEditorAssetTests()
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

void WriteText(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

void PrepareStandardPbrShaderLabDependency(const std::filesystem::path& root)
{
    NLS::Editor::Assets::AssetDatabaseFacade database(
        NLS::Editor::Assets::MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_FALSE(database.AssetPathToGUID("Assets/Engine/Shaders/ShaderLab/StandardPBR.shader").empty());
}

std::vector<NLS::Editor::Assets::EditorAssetRoot> AppendBuiltInShaderRootForTest(
    const std::filesystem::path& root,
    std::vector<NLS::Editor::Assets::EditorAssetRoot> roots)
{
    NLS::Editor::Assets::AppendBuiltInShaderAssetRoot(roots, root);
    return roots;
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
}

std::vector<uint8_t> ReadBinary(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
}

size_t CountArtifactTelemetryStage(
    const std::vector<NLS::Core::Assets::ArtifactLoadTelemetryRecord>& records,
    const NLS::Core::Assets::ArtifactLoadTelemetryStage stage)
{
    return static_cast<size_t>(std::count_if(
        records.begin(),
        records.end(),
        [stage](const NLS::Core::Assets::ArtifactLoadTelemetryRecord& record)
        {
            return record.stage == stage;
        }));
}

std::string EditorAssetDatabaseFileStamp(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return {};

    error.clear();
    const auto writeTime = std::filesystem::last_write_time(path, error);
    if (error)
        return {};

    return std::to_string(size) + ":" +
        std::to_string(static_cast<std::intmax_t>(writeTime.time_since_epoch().count()));
}

NLS::Core::Assets::ImportedArtifact MakeEditorAssetDatabaseArtifact(
    const NLS::Core::Assets::AssetId assetId,
    std::string subAssetKey,
    const NLS::Core::Assets::ArtifactType artifactType,
    std::string artifactPath)
{
    NLS::Core::Assets::ImportedArtifact artifact;
    artifact.sourceAssetId = assetId;
    artifact.subAssetKey = std::move(subAssetKey);
    artifact.artifactType = artifactType;
    artifact.artifactPath = std::move(artifactPath);
    artifact.loaderId = "test";
    artifact.targetPlatform = "editor";
    artifact.contentHash = artifact.subAssetKey + ":hash";
    return artifact;
}

std::string MakeEditorContentAddressedArtifactPath(const std::string& storageKey)
{
    return (std::filesystem::path("Library") /
        "Artifacts" /
        NLS::Core::Assets::BuildArtifactStorageRelativePath(
            NLS::Core::Assets::BuildArtifactStorageFileName(storageKey))).generic_string();
}

std::string ReadArtifactPayloadText(
    const std::filesystem::path& path,
    const NLS::Core::Assets::ArtifactType artifactType,
    const uint32_t schemaVersion)
{
    const auto bytes = ReadBinary(path);
    const auto container = NLS::Core::Assets::ReadNativeArtifactContainer(bytes, artifactType, schemaVersion);
    if (!container.has_value())
        return {};

    return std::string(container->payload.begin(), container->payload.end());
}

void WriteBinary(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

void WriteNativeMaterialSource(
    const std::filesystem::path& path,
    std::string displayName,
    const std::string& payloadText)
{
    NLS::Core::Assets::NativeArtifactMetadata metadata;
    metadata.artifactType = NLS::Core::Assets::ArtifactType::Material;
    metadata.schemaName = "material";
    metadata.schemaVersion = 1u;
    metadata.subAssetKey = "material:" + std::move(displayName);
    metadata.displayName = std::filesystem::path(path).stem().generic_string();
    metadata.importerId = "material";
    metadata.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(
        NLS::Core::Assets::AssetType::Material);
    metadata.targetPlatform = "editor";

    const std::vector<uint8_t> payload(payloadText.begin(), payloadText.end());
    WriteBinary(path, NLS::Core::Assets::WriteNativeArtifactContainer(std::move(metadata), payload));
}

std::optional<NLS::Core::Assets::ArtifactManifest> LoadArtifactManifestForTest(
    const std::filesystem::path& projectRoot,
    const NLS::Core::Assets::AssetId sourceAssetId)
{
    NLS::Core::Assets::ArtifactDatabase database;
    if (!database.Load(projectRoot / "Library" / "ArtifactDB"))
        return std::nullopt;
    return database.BuildManifestForSource(sourceAssetId);
}

void SaveArtifactManifestForTest(
    const std::filesystem::path& projectRoot,
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const std::string& sourcePath)
{
    NLS::Core::Assets::ArtifactDatabase database;
    const auto databasePath = projectRoot / "Library" / "ArtifactDB";
    ASSERT_TRUE(database.Load(databasePath));
    database.UpsertManifest(manifest, sourcePath, NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(database.Save(databasePath));
}

void ClearStartupPreimportSourceContentHashForTest(
    const std::filesystem::path& stampPath,
    const std::string& rootMount,
    const std::string& relativePath)
{
    std::ifstream input(stampPath, std::ios::binary);
    ASSERT_TRUE(input);

    std::vector<std::string> lines;
    std::string line;
    bool replaced = false;
    while (std::getline(input, line))
    {
        std::istringstream stream(line);
        std::string key;
        stream >> key;
        if (key == "source")
        {
            std::string parsedRootMount;
            std::string parsedRelativePath;
            std::string stamp;
            std::string contentHash;
            std::string fingerprint;
            stream >> std::quoted(parsedRootMount)
                >> std::quoted(parsedRelativePath)
                >> std::quoted(stamp)
                >> std::quoted(contentHash)
                >> std::quoted(fingerprint);
            if (!stream.fail() && parsedRootMount == rootMount && parsedRelativePath == relativePath)
            {
                std::ostringstream replacement;
                replacement << "source "
                    << std::quoted(parsedRootMount) << ' '
                    << std::quoted(parsedRelativePath) << ' '
                    << std::quoted(stamp) << ' '
                    << std::quoted(std::string {}) << ' '
                    << std::quoted(fingerprint);
                line = replacement.str();
                replaced = true;
            }
        }
        lines.push_back(std::move(line));
    }
    ASSERT_TRUE(replaced);

    std::ofstream output(stampPath, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output);
    for (const auto& outputLine : lines)
        output << outputLine << '\n';
}

void ReplaceArtifactDatabaseFieldForTest(
    const std::filesystem::path& databasePath,
    const std::string& matchField,
    const size_t fieldIndex,
    const std::string& replacement)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    ASSERT_EQ(fieldIndex, 7u);
    NLS::Core::Assets::ArtifactDatabase database;
    ASSERT_TRUE(database.Load(databasePath));
    const auto replaced = database.MutateRecordsForTesting(
        [&](NLS::Core::Assets::ArtifactDatabaseRecord& record)
        {
            if (record.subAssetKey != matchField)
                return false;
            record.artifactPath = replacement;
            return true;
        });
    ASSERT_EQ(replaced, 1u);
    ASSERT_TRUE(database.Save(databasePath));
#else
    (void)databasePath;
    (void)matchField;
    (void)fieldIndex;
    (void)replacement;
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inject raw ArtifactDB records.";
#endif
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

bool WaitForRefreshRequest(
    NLS::Editor::Assets::AssetRefreshScheduler& scheduler,
    NLS::Editor::Core::AssetFileWatcher& watcher,
    const std::filesystem::path& root)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline)
    {
        scheduler.PollWatcher(watcher, root);
        if (scheduler.HasPendingRefresh())
            return true;

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    return false;
}

void TouchMetaImporterVersion(const std::filesystem::path& metaPath)
{
    std::ifstream input(metaPath, std::ios::binary);
    ASSERT_TRUE(input.good());

    std::string text {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
    const auto key = std::string("IMPORTER_VERSION=");
    const auto begin = text.find(key);
    ASSERT_NE(begin, std::string::npos);
    const auto valueBegin = begin + key.size();
    const auto valueEnd = text.find('\n', valueBegin);
    text.replace(valueBegin, valueEnd - valueBegin, "30001");
    WriteText(metaPath, text);
}

NLS::Render::Context::Driver& EnsureEditorAssetDatabaseTestDriver()
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

class ScopedShaderManagerAssetPaths final
{
public:
    ScopedShaderManagerAssetPaths(
        const std::string& projectAssetsPath,
        const std::string& engineAssetsPath)
    {
        NLS::Core::ResourceManagement::ShaderManager::ProvideAssetPaths(
            projectAssetsPath,
            engineAssetsPath);
    }

    ~ScopedShaderManagerAssetPaths()
    {
        NLS::Core::ResourceManagement::ShaderManager::ProvideAssetPaths({}, {});
        NLS::Render::Resources::Loaders::ShaderLoader::SetDefaultProjectAssetsPath({});
    }

    ScopedShaderManagerAssetPaths(const ScopedShaderManagerAssetPaths&) = delete;
    ScopedShaderManagerAssetPaths& operator=(const ScopedShaderManagerAssetPaths&) = delete;
};
}

TEST(EditorAssetDatabaseTests, GeneratedModelPrefabStateIsReadOnlyWithVariantAndUnpackEscapes)
{
    using namespace NLS::Editor::Assets;

    const auto sourceAsset = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("11111111-1111-4111-8111-111111111111"));

    EditorAssetDatabase database;
    database.RegisterGeneratedPrefab({
        sourceAsset,
        "prefab:HeroScene",
        GeneratedPrefabEditPolicy::ReadOnlyGenerated
    });

    const auto* state = database.FindGeneratedPrefabState(sourceAsset, "prefab:HeroScene");
    ASSERT_NE(state, nullptr);
    EXPECT_TRUE(state->generatedReadOnly);
    EXPECT_FALSE(state->CanEditInPlace());
    EXPECT_TRUE(state->CanCreateEditableVariant());
    EXPECT_TRUE(state->CanUnpackToSceneObjects());
}

TEST(EditorAssetDatabaseTests, GeneratedModelPrefabExposesVariantAndUnpackCommands)
{
    using namespace NLS::Editor::Assets;

    const auto sourceAsset = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"));

    EditorAssetDatabase database;
    database.RegisterGeneratedPrefab({
        sourceAsset,
        "prefab:HeroScene",
        GeneratedPrefabEditPolicy::ReadOnlyGenerated
    });

    const auto commands = database.GetGeneratedPrefabCommands(sourceAsset, "prefab:HeroScene");

    ASSERT_EQ(commands.size(), 2u);
    EXPECT_EQ(commands[0].commandId, "prefab.create-variant");
    EXPECT_EQ(commands[0].displayName, "Create Variant");
    EXPECT_TRUE(commands[0].enabled);
    EXPECT_EQ(commands[1].commandId, "prefab.unpack");
    EXPECT_EQ(commands[1].displayName, "Unpack");
    EXPECT_TRUE(commands[1].enabled);
}

TEST(EditorAssetDatabaseTests, GeneratedModelPrefabRegistrationUpsertsExistingState)
{
    using namespace NLS::Editor::Assets;

    const auto sourceAsset = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("cccccccc-cccc-4ccc-8ccc-cccccccccccc"));

    EditorAssetDatabase database;
    database.RegisterGeneratedPrefab({
        sourceAsset,
        "prefab:HeroScene",
        GeneratedPrefabEditPolicy::ReadOnlyGenerated
    });
    database.RegisterGeneratedPrefab({
        sourceAsset,
        "prefab:HeroScene",
        GeneratedPrefabEditPolicy::EditableSource
    });

    const auto* state = database.FindGeneratedPrefabState(sourceAsset, "prefab:HeroScene");
    ASSERT_NE(state, nullptr);
    EXPECT_FALSE(state->generatedReadOnly);
    EXPECT_TRUE(state->CanEditInPlace());
    EXPECT_EQ(database.GetGeneratedPrefabStateCount(), 1u);
}

TEST(EditorAssetDatabaseTests, ManualRefreshRequestsAreQueuedAndConsumedInOrder)
{
    using namespace NLS::Editor::Assets;

    AssetRefreshScheduler scheduler;
    scheduler.RequestRefresh("Assets/Models/Hero.gltf", AssetRefreshReason::ManualReimport);
    scheduler.RequestRefresh("Assets/Prefabs/Hero.prefab", AssetRefreshReason::DependencyChanged);

    ASSERT_TRUE(scheduler.HasPendingRefresh());

    const auto requests = scheduler.ConsumeRefreshRequests();
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[0].path, std::filesystem::path("Assets/Models/Hero.gltf"));
    EXPECT_EQ(requests[0].reason, AssetRefreshReason::ManualReimport);
    EXPECT_EQ(requests[1].path, std::filesystem::path("Assets/Prefabs/Hero.prefab"));
    EXPECT_EQ(requests[1].reason, AssetRefreshReason::DependencyChanged);
    EXPECT_FALSE(scheduler.HasPendingRefresh());
}

TEST(EditorAssetDatabaseTests, AssetFileWatcherChangesCanScheduleAssetRefresh)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    NLS::Editor::Core::AssetFileWatcher watcher;
    ASSERT_TRUE(watcher.Start(root));

    AssetRefreshScheduler scheduler;
    WriteText(root / "Models" / "Hero.obj", "o Hero");

    ASSERT_TRUE(WaitForRefreshRequest(scheduler, watcher, root));
    const auto requests = scheduler.ConsumeRefreshRequests();
    ASSERT_FALSE(requests.empty());
    EXPECT_EQ(requests.front().path, root);
    EXPECT_EQ(requests.front().reason, AssetRefreshReason::FileWatcherChanged);

    watcher.Stop();
    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, AssetWatcherStartupReportMarksFailedWatcherStarts)
{
    using namespace NLS::Editor::Assets;

    const auto report = BuildAssetWatcherStartupReport(
        "EngineAssets",
        false,
        "ProjectAssets",
        true);

    EXPECT_FALSE(report.succeeded);
    ASSERT_EQ(report.diagnostics.size(), 1u);
    EXPECT_EQ(report.diagnostics[0].code, "asset-watcher-start-failed");
    EXPECT_EQ(report.diagnostics[0].path, std::filesystem::path("EngineAssets"));
    EXPECT_EQ(report.diagnostics[0].severity, NLS::Core::Assets::AssetDiagnosticSeverity::Error);
}

TEST(EditorAssetDatabaseTests, PreimportPlanIncludesColdModelAndPrefabAssets)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(root / "Assets" / "Models" / "ColdHero.gltf", R"({"asset":{"version":"2.0"}})");
    WriteText(root / "Assets" / "Prefabs" / "ColdLamp.prefab", R"({"objects":[]})");
    WriteBinary(root / "Assets" / "Textures" / "Ignored.png", TinyPng());

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    const auto plan = scheduler.BuildPlan(database);

    ASSERT_EQ(plan.assetPaths.size(), 3u);
    EXPECT_EQ(plan.assetPaths[0], "Assets/Textures/Ignored.png");
    EXPECT_EQ(plan.assetPaths[1], "Assets/Models/ColdHero.gltf");
    EXPECT_EQ(plan.assetPaths[2], "Assets/Prefabs/ColdLamp.prefab");

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, PreimportPlanIncludesColdMaterialAndTextureAssets)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteBinary(root / "Assets" / "Textures" / "ColdAlbedo.png", TinyPng());
    WriteNativeMaterialSource(
        root / "Assets" / "Materials" / "ColdMaterial.mat",
        "ColdMaterial",
        "shaderLabMaterialVersion=1\n"
        "shader=?\n"
        "surfaceMode=Opaque\n");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    const auto plan = scheduler.BuildPlan(database);

    EXPECT_NE(
        std::find(plan.assetPaths.begin(), plan.assetPaths.end(), "Assets/Materials/ColdMaterial.mat"),
        plan.assetPaths.end());
    EXPECT_NE(
        std::find(plan.assetPaths.begin(), plan.assetPaths.end(), "Assets/Textures/ColdAlbedo.png"),
        plan.assetPaths.end());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, PreimportPlanSkipsWarmMaterialAndTextureArtifacts)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteBinary(root / "Assets" / "Textures" / "WarmAlbedo.png", TinyPng());
    WriteNativeMaterialSource(
        root / "Assets" / "Materials" / "WarmMaterial.mat",
        "WarmMaterial",
        "shaderLabMaterialVersion=1\n"
        "shader=?\n"
        "surfaceMode=Opaque\n");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    auto coldPlan = scheduler.BuildPlan(database);
    EXPECT_NE(
        std::find(coldPlan.assetPaths.begin(), coldPlan.assetPaths.end(), "Assets/Materials/WarmMaterial.mat"),
        coldPlan.assetPaths.end());
    EXPECT_NE(
        std::find(coldPlan.assetPaths.begin(), coldPlan.assetPaths.end(), "Assets/Textures/WarmAlbedo.png"),
        coldPlan.assetPaths.end());

    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup))
        << JoinDiagnosticSummaries(database.GetDiagnostics())
        << JoinImportProgressSummaries(tracker);
    ASSERT_TRUE(database.Refresh());

    const auto warmPlan = scheduler.BuildPlan(database);
    EXPECT_EQ(
        std::find(warmPlan.assetPaths.begin(), warmPlan.assetPaths.end(), "Assets/Materials/WarmMaterial.mat"),
        warmPlan.assetPaths.end());
    EXPECT_EQ(
        std::find(warmPlan.assetPaths.begin(), warmPlan.assetPaths.end(), "Assets/Textures/WarmAlbedo.png"),
        warmPlan.assetPaths.end());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, PreimportBatchFlushesArtifactDatabaseOnceForMultipleColdAssets)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteBinary(root / "Assets" / "Textures" / "BatchAlbedo.png", TinyPng());
    WriteNativeMaterialSource(
        root / "Assets" / "Materials" / "BatchMaterial.mat",
        "BatchMaterial",
        "shaderLabMaterialVersion=1\n"
        "shader=?\n"
        "surfaceMode=Opaque\n");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());

    ResetArtifactDatabaseSaveAttemptCountForTesting();
    ImportProgressTracker tracker;
    AssetPreimportScheduler scheduler;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup))
        << JoinDiagnosticSummaries(database.GetDiagnostics())
        << JoinImportProgressSummaries(tracker);

    EXPECT_EQ(database.GetCompletedImportCount(), 2u);
    EXPECT_EQ(GetArtifactDatabaseSaveAttemptCountForTesting(), 1u);
    EXPECT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Materials/BatchMaterial.mat"));
    EXPECT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Textures/BatchAlbedo.png"));

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, StandaloneTextureImportReusesEncodedSourceHashForManifestDependency)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteBinary(root / "Assets" / "Textures" / "SingleReadAlbedo.png", TinyPng());

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());

    ResetAssetDatabaseSourceFileContentHashReadCountForTesting();
    ASSERT_TRUE(database.ImportAsset("Assets/Textures/SingleReadAlbedo.png"))
        << JoinDiagnosticSummaries(database.GetDiagnostics());

    EXPECT_EQ(GetAssetDatabaseSourceFileContentHashReadCountForTesting(), 0u)
        << "Standalone texture import already reads the encoded source for decode; manifest dependency hashing "
           "should reuse that byte buffer instead of opening and reading the source file a second time.";
    EXPECT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Textures/SingleReadAlbedo.png"));

    std::filesystem::remove_all(root);
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to count source hash reads.";
#endif
}

TEST(EditorAssetDatabaseTests, PreimportPlanReimportsMaterialAndTextureWhenArtifactPayloadIsCorrupt)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteBinary(root / "Assets" / "Textures" / "CorruptAlbedo.png", TinyPng());
    WriteNativeMaterialSource(
        root / "Assets" / "Materials" / "CorruptMaterial.mat",
        "CorruptMaterial",
        "shaderLabMaterialVersion=1\n"
        "shader=?\n"
        "surfaceMode=Opaque\n");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup))
        << JoinDiagnosticSummaries(database.GetDiagnostics())
        << JoinImportProgressSummaries(tracker);
    ASSERT_TRUE(database.Refresh());

    const auto materialArtifactPath = database.ResolveArtifactPathAtPath(
        "Assets/Materials/CorruptMaterial.mat",
        "material:main");
    const auto textureArtifactPath = database.ResolveArtifactPathAtPath(
        "Assets/Textures/CorruptAlbedo.png",
        "texture:main");
    ASSERT_FALSE(materialArtifactPath.empty());
    ASSERT_FALSE(textureArtifactPath.empty());
    WriteText(materialArtifactPath, "not-a-material-artifact");
    WriteText(textureArtifactPath, "not-a-texture-artifact");

    const auto plan = scheduler.BuildPlan(database);
    EXPECT_NE(
        std::find(plan.assetPaths.begin(), plan.assetPaths.end(), "Assets/Materials/CorruptMaterial.mat"),
        plan.assetPaths.end());
    EXPECT_NE(
        std::find(plan.assetPaths.begin(), plan.assetPaths.end(), "Assets/Textures/CorruptAlbedo.png"),
        plan.assetPaths.end());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, PreimportPlanSkipsWarmModelArtifacts)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "WarmHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "WarmHeroRoot" }]
        })");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ASSERT_EQ(scheduler.BuildPlan(database).assetPaths.size(), 1u);

    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker));
    ASSERT_TRUE(database.Refresh());

    const auto planAfterImport = scheduler.BuildPlan(database);
    EXPECT_TRUE(planAfterImport.assetPaths.empty());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, StartupRefreshPlansPreimportWhenCentralArtifactDatabaseIsMissing)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "RollbackWarmHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "RollbackWarmHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    {
        AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
        ASSERT_TRUE(database.Refresh());
        ASSERT_TRUE(database.ImportAsset("Assets/Models/RollbackWarmHero.gltf"));
        ASSERT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/RollbackWarmHero.gltf"));

        const auto artifactDatabasePath = root / "Library" / "ArtifactDB";
        ASSERT_TRUE(std::filesystem::exists(artifactDatabasePath));

        const auto rollbackRoot = root / "Library" / "ArtifactDB.publish-rollback";
        std::error_code error;
        std::filesystem::remove_all(rollbackRoot, error);
        ASSERT_FALSE(error);
        std::filesystem::rename(artifactDatabasePath, rollbackRoot, error);
        ASSERT_FALSE(error);
        ASSERT_FALSE(std::filesystem::exists(artifactDatabasePath));
        ASSERT_TRUE(std::filesystem::exists(rollbackRoot / "data.mdb"));
    }

    AssetDatabaseFacade restartedDatabase(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(restartedDatabase.Refresh());

    const auto recoveredRollbackRoot = root / "Library" / "ArtifactDB.publish-rollback";
    EXPECT_TRUE(std::filesystem::exists(recoveredRollbackRoot / "data.mdb"));
    EXPECT_FALSE(restartedDatabase.IsArtifactManifestCurrentForAssetPath("Assets/Models/RollbackWarmHero.gltf"));
    EXPECT_FALSE(restartedDatabase
        .LoadPrefabArtifactAtPath("Assets/Models/RollbackWarmHero.gltf", "prefab:RollbackWarmHero")
        .has_value());

    const auto plan = AssetPreimportScheduler().BuildPlan(restartedDatabase);
    EXPECT_NE(
        std::find(plan.assetPaths.begin(), plan.assetPaths.end(), "Assets/Models/RollbackWarmHero.gltf"),
        plan.assetPaths.end());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, FileWatcherPreimportSkipsWarmSceneAssetsWhenDependenciesAreCurrent)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "ChangedHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "ChangedHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup))
        << JoinDiagnosticSummaries(database.GetDiagnostics())
        << JoinImportProgressSummaries(tracker);
    ASSERT_EQ(database.GetCompletedImportCount(), 1u);

    ASSERT_TRUE(scheduler.Run(database, tracker, {
        AssetPreimportReason::FileWatcherChanged,
        {std::filesystem::path("Assets") / "Models" / "ChangedHero.gltf"}
    }));
    EXPECT_EQ(database.GetCompletedImportCount(), 1u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, FileWatcherPreimportSkipsStalePlanWhenAssetBecameWarmBeforeExecution)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "ConcurrentHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "ConcurrentHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    AssetDatabaseFacade planningDatabase(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(planningDatabase.Refresh());

    AssetPreimportScheduler scheduler;
    const AssetPreimportRequest watcherRequest {
        AssetPreimportReason::FileWatcherChanged,
        {std::filesystem::path("Assets") / "Models" / "ConcurrentHero.gltf"}
    };
    const auto stalePlan = scheduler.BuildPlan(planningDatabase, watcherRequest);
    ASSERT_EQ(stalePlan.assetPaths, std::vector<std::string>({"Assets/Models/ConcurrentHero.gltf"}));

    AssetDatabaseFacade importingDatabase(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(importingDatabase.Refresh());
    ImportProgressTracker tracker;
    ASSERT_TRUE(importingDatabase.ImportAsset("Assets/Models/ConcurrentHero.gltf", tracker));
    ASSERT_EQ(importingDatabase.GetCompletedImportCount(), 1u);
    ASSERT_TRUE(importingDatabase.IsArtifactManifestCurrentForAssetPath("Assets/Models/ConcurrentHero.gltf"));

    ASSERT_TRUE(scheduler.RunAlreadyPlanned(planningDatabase, tracker, watcherRequest, stalePlan));
    EXPECT_EQ(planningDatabase.GetCompletedImportCount(), 0u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, FileWatcherPreimportSkipsStalePlanWhenAssetIsManuallyReimporting)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "GuardedConcurrentHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "GuardedConcurrentHeroRoot" }]
        })");

    AssetDatabaseFacade planningDatabase({root});
    ASSERT_TRUE(planningDatabase.Refresh());

    AssetPreimportScheduler scheduler;
    const AssetPreimportRequest watcherRequest {
        AssetPreimportReason::FileWatcherChanged,
        {std::filesystem::path("Assets") / "Models" / "GuardedConcurrentHero.gltf"}
    };
    const auto stalePlan = scheduler.BuildPlan(planningDatabase, watcherRequest);
    ASSERT_EQ(stalePlan.assetPaths, std::vector<std::string>({"Assets/Models/GuardedConcurrentHero.gltf"}));

    ImportProgressTracker tracker;
    {
        const auto guard = AssetImporterFacade::MarkReimportInProgressForTesting(
            "Assets/Models/GuardedConcurrentHero.gltf");
        ASSERT_TRUE(scheduler.RunAlreadyPlanned(planningDatabase, tracker, watcherRequest, stalePlan));
    }

    EXPECT_EQ(planningDatabase.GetCompletedImportCount(), 0u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, FileWatcherPreimportUsesFilteredBatchSizeAfterSkippingWarmStalePlanEntries)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "AlreadyWarmHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "AlreadyWarmHeroRoot" }]
        })");
    WriteText(
        root / "Assets" / "Models" / "StillColdHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "StillColdHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    AssetDatabaseFacade planningDatabase(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(planningDatabase.Refresh());

    AssetPreimportScheduler scheduler;
    const AssetPreimportRequest watcherRequest {
        AssetPreimportReason::FileWatcherChanged,
        {std::filesystem::path("Assets") / "Models"}
    };
    const auto stalePlan = scheduler.BuildPlan(planningDatabase, watcherRequest);
    ASSERT_EQ(stalePlan.assetPaths.size(), 2u);

    AssetDatabaseFacade importingDatabase(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(importingDatabase.Refresh());
    ImportProgressTracker importTracker;
    ASSERT_TRUE(importingDatabase.ImportAsset("Assets/Models/AlreadyWarmHero.gltf", importTracker));
    ASSERT_TRUE(importingDatabase.IsArtifactManifestCurrentForAssetPath("Assets/Models/AlreadyWarmHero.gltf"));

    ImportProgressTracker watcherTracker;
    ASSERT_TRUE(scheduler.RunAlreadyPlanned(planningDatabase, watcherTracker, watcherRequest, stalePlan));
    EXPECT_EQ(planningDatabase.GetCompletedImportCount(), 1u);

    const auto batch = watcherTracker.GetBatchProgress();
    EXPECT_EQ(batch.totalAssets, 1u);
    EXPECT_EQ(batch.completedAssets, 1u);
    EXPECT_FALSE(batch.activeJob.has_value());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, FileWatcherPreimportSkipsStartupGeneratedMetaForWarmSceneAssets)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "StartupGeneratedMetaHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "StartupGeneratedMetaHeroRoot" }]
        })");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup))
        << JoinDiagnosticSummaries(database.GetDiagnostics())
        << JoinImportProgressSummaries(tracker);
    ASSERT_EQ(database.GetCompletedImportCount(), 1u);
    ASSERT_TRUE(database.IsArtifactManifestCurrentForAssetPath(
        "Assets/Models/StartupGeneratedMetaHero.gltf"));
    ASSERT_TRUE(database
        .LoadPrefabArtifactAtPath(
            "Assets/Models/StartupGeneratedMetaHero.gltf",
            "prefab:StartupGeneratedMetaHero")
        .has_value());

    const AssetPreimportRequest startupGeneratedMetaChange {
        AssetPreimportReason::FileWatcherChanged,
        {root / "Assets" / "Models" / "StartupGeneratedMetaHero.gltf.meta"}
    };
    const auto plan = scheduler.BuildPlan(database, startupGeneratedMetaChange);
    EXPECT_TRUE(plan.assetPaths.empty());

    ASSERT_TRUE(scheduler.Run(database, tracker, startupGeneratedMetaChange));
    EXPECT_EQ(database.GetCompletedImportCount(), 1u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, FileWatcherPreimportReimportsWarmSceneAssetsWhenSourceChanges)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    const auto modelPath = root / "Assets" / "Models" / "ChangedHero.gltf";
    WriteText(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "ChangedHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup))
        << JoinDiagnosticSummaries(database.GetDiagnostics())
        << JoinImportProgressSummaries(tracker);
    ASSERT_EQ(database.GetCompletedImportCount(), 1u);

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    WriteText(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "ChangedHeroRoot" }, { "name": "ChangedChild" }]
        })");

    ASSERT_TRUE(scheduler.Run(database, tracker, {
        AssetPreimportReason::FileWatcherChanged,
        {std::filesystem::path("Assets") / "Models" / "ChangedHero.gltf"}
    }));
    EXPECT_EQ(database.GetCompletedImportCount(), 2u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, FileWatcherPreimportReimportsWarmSceneAssetsWhenArtifactIsMissing)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "MissingArtifactHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "MissingArtifactHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup))
        << JoinDiagnosticSummaries(database.GetDiagnostics())
        << JoinImportProgressSummaries(tracker);
    ASSERT_EQ(database.GetCompletedImportCount(), 1u);

    auto manifest = database.GetArtifactManifestForAssetPath("Assets/Models/MissingArtifactHero.gltf");
    ASSERT_TRUE(manifest.has_value());
    const auto* primary = manifest->FindPrimaryArtifact();
    ASSERT_NE(primary, nullptr);
    const auto primaryPath = database.ResolveArtifactPathAtPath(
        "Assets/Models/MissingArtifactHero.gltf",
        primary->subAssetKey);
    ASSERT_FALSE(primaryPath.empty());
    std::filesystem::remove(primaryPath);

    ASSERT_TRUE(scheduler.Run(database, tracker, {
        AssetPreimportReason::FileWatcherChanged,
        {std::filesystem::path("Assets") / "Models" / "MissingArtifactHero.gltf"}
    }));
    EXPECT_EQ(database.GetCompletedImportCount(), 2u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, EditorStartupPreimportReimportsWarmSceneAssetsWhenSourceChangedWhileClosed)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    const auto modelPath = root / "Assets" / "Models" / "StartupChangedHero.gltf";
    WriteText(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "StartupChangedHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup))
        << JoinDiagnosticSummaries(database.GetDiagnostics())
        << JoinImportProgressSummaries(tracker);
    ASSERT_EQ(database.GetCompletedImportCount(), 1u);

    WriteText(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "StartupChangedHeroRoot" }, { "name": "ChangedWhileClosed" }]
        })");

    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup))
        << JoinDiagnosticSummaries(database.GetDiagnostics())
        << JoinImportProgressSummaries(tracker);
    EXPECT_EQ(database.GetCompletedImportCount(), 2u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, EditorStartupPreimportReimportsWarmSceneAssetsWhenArtifactIsMissing)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "StartupMissingArtifactHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "StartupMissingArtifactHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup))
        << JoinDiagnosticSummaries(database.GetDiagnostics())
        << JoinImportProgressSummaries(tracker);
    ASSERT_EQ(database.GetCompletedImportCount(), 1u);

    auto manifest = database.GetArtifactManifestForAssetPath("Assets/Models/StartupMissingArtifactHero.gltf");
    ASSERT_TRUE(manifest.has_value());
    const auto* primary = manifest->FindPrimaryArtifact();
    ASSERT_NE(primary, nullptr);
    const auto primaryPath = database.ResolveArtifactPathAtPath(
        "Assets/Models/StartupMissingArtifactHero.gltf",
        primary->subAssetKey);
    ASSERT_FALSE(primaryPath.empty());
    std::filesystem::remove(primaryPath);

    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup))
        << JoinDiagnosticSummaries(database.GetDiagnostics())
        << JoinImportProgressSummaries(tracker);
    EXPECT_EQ(database.GetCompletedImportCount(), 2u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, EditorStartupPreimportReimportsMalformedManifestImporterVersion)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "StartupMalformedManifestHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "StartupMalformedManifestHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup))
        << JoinDiagnosticSummaries(database.GetDiagnostics())
        << JoinImportProgressSummaries(tracker);
    ASSERT_EQ(database.GetCompletedImportCount(), 1u);

    const auto guid = database.AssetPathToGUID("Assets/Models/StartupMalformedManifestHero.gltf");
    ASSERT_FALSE(guid.empty());
    auto manifest = LoadArtifactManifestForTest(root, NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid)));
    ASSERT_TRUE(manifest.has_value());
    ++manifest->importerVersion;
    SaveArtifactManifestForTest(root, *manifest, "Assets/Models/StartupMalformedManifestHero.gltf");

    ASSERT_NO_THROW({
        EXPECT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup));
    });
    EXPECT_EQ(database.GetCompletedImportCount(), 2u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, FileWatcherPreimportReimportsWarmSceneAssetsWhenMetaChanges)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    const auto modelPath = root / "Assets" / "Models" / "MetaChangedHero.gltf";
    WriteText(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "MetaChangedHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup))
        << JoinDiagnosticSummaries(database.GetDiagnostics())
        << JoinImportProgressSummaries(tracker);
    ASSERT_EQ(database.GetCompletedImportCount(), 1u);

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    TouchMetaImporterVersion(modelPath.string() + ".meta");

    ASSERT_TRUE(scheduler.Run(database, tracker, {
        AssetPreimportReason::FileWatcherChanged,
        {std::filesystem::path("Assets") / "Models" / "MetaChangedHero.gltf.meta"}
    }));
    EXPECT_EQ(database.GetCompletedImportCount(), 2u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, MountedExternalDependencyManifestStaysWarmWhenDependencyIsCurrent)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    std::filesystem::create_directories(root / "Assets");
    const auto packageRoot = root / "Packages" / "StarterContent";
    const auto modelPath = packageRoot / "Models" / "MountedDependencyHero.gltf";
    WriteBinary(packageRoot / "Textures" / "HeroBaseColor.png", TinyPng());
    WriteText(
        modelPath,
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
    WriteText(packageRoot / "Models" / "Hero.bin", "mesh-binary");
    PrepareStandardPbrShaderLabDependency(root);

    AssetDatabaseFacade database(AppendBuiltInShaderRootForTest(root, {
        EditorAssetRoot {root / "Assets", false, "Assets", root / "Library"},
        EditorAssetRoot {packageRoot, false, "Packages/StarterContent"}
    }));
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup))
        << JoinDiagnosticSummaries(database.GetDiagnostics())
        << JoinImportProgressSummaries(tracker);
    ASSERT_EQ(database.GetCompletedImportCount(), 2u);

    ASSERT_TRUE(scheduler.Run(database, tracker, {
        AssetPreimportReason::FileWatcherChanged,
        {modelPath}
    })) << JoinDiagnosticSummaries(database.GetDiagnostics())
        << JoinImportProgressSummaries(tracker);
    EXPECT_EQ(database.GetCompletedImportCount(), 2u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, MountedExternalDependencyUsesOwningRootWhenAnotherRootHasSameRelativePath)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    std::filesystem::create_directories(root / "Assets");
    const auto projectRoot = root / "Project";
    const auto packageRoot = root / "Packages" / "StarterContent";
    const auto modelPath = packageRoot / "Models" / "MountedDependencyCollisionHero.gltf";
    WriteBinary(projectRoot / "Textures" / "HeroBaseColor.png", TinyPng());
    WriteBinary(packageRoot / "Textures" / "HeroBaseColor.png", TinyPng());
    WriteText(
        modelPath,
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
    WriteText(packageRoot / "Models" / "Hero.bin", "mesh-binary");
    PrepareStandardPbrShaderLabDependency(root);

    AssetDatabaseFacade database(AppendBuiltInShaderRootForTest(root, {
        EditorAssetRoot {projectRoot, false, {}},
        EditorAssetRoot {packageRoot, false, "Packages/StarterContent"}
    }));
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup))
        << JoinDiagnosticSummaries(database.GetDiagnostics())
        << JoinImportProgressSummaries(tracker);
    ASSERT_EQ(database.GetCompletedImportCount(), 3u);

    ASSERT_TRUE(scheduler.Run(database, tracker, {
        AssetPreimportReason::FileWatcherChanged,
        {modelPath}
    })) << JoinDiagnosticSummaries(database.GetDiagnostics())
        << JoinImportProgressSummaries(tracker);
    EXPECT_EQ(database.GetCompletedImportCount(), 3u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, MountedExternalDependencyChangePlansOwningModel)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    std::filesystem::create_directories(root / "Assets");
    const auto packageRoot = root / "Packages" / "StarterContent";
    const auto texturePath = packageRoot / "Textures" / "HeroBaseColor.png";
    const auto modelPath = packageRoot / "Models" / "MountedDependencyChangedHero.gltf";
    WriteBinary(texturePath, TinyPng());
    WriteText(
        modelPath,
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
    WriteText(packageRoot / "Models" / "Hero.bin", "mesh-binary");
    PrepareStandardPbrShaderLabDependency(root);

    AssetDatabaseFacade database(AppendBuiltInShaderRootForTest(root, {
        EditorAssetRoot {root / "Assets", false, "Assets", root / "Library"},
        EditorAssetRoot {packageRoot, false, "Packages/StarterContent"}
    }));
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup))
        << JoinDiagnosticSummaries(database.GetDiagnostics())
        << JoinImportProgressSummaries(tracker);
    ASSERT_EQ(database.GetCompletedImportCount(), 2u);

    WriteText(texturePath, "texture-after");
    const auto plan = scheduler.BuildPlan(database, {
        AssetPreimportReason::FileWatcherChanged,
        {texturePath}
    });

    ASSERT_EQ(plan.assetPaths.size(), 2u);
    EXPECT_EQ(plan.assetPaths[0], "Packages/StarterContent/Textures/HeroBaseColor.png");
    EXPECT_EQ(plan.assetPaths[1], "Packages/StarterContent/Models/MountedDependencyChangedHero.gltf");

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, MountedExternalDependencyChangeIgnoresDisjointRootWithSameRelativePath)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    std::filesystem::create_directories(root / "Assets");
    const auto packageRoot = root / "Packages" / "StarterContent";
    const auto otherRoot = root / "Other";
    const auto modelPath = packageRoot / "Models" / "MountedDependencyDisjointHero.gltf";
    WriteBinary(packageRoot / "Textures" / "HeroBaseColor.png", TinyPng());
    WriteBinary(otherRoot / "Textures" / "HeroBaseColor.png", TinyPng());
    WriteText(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
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
    PrepareStandardPbrShaderLabDependency(root);

    AssetDatabaseFacade database(AppendBuiltInShaderRootForTest(root, {
        EditorAssetRoot {root / "Assets", false, "Assets", root / "Library"},
        EditorAssetRoot {packageRoot, false, "Packages/StarterContent"},
        EditorAssetRoot {otherRoot, false, "Other"}
    }));
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup));
    ASSERT_EQ(database.GetCompletedImportCount(), 3u);

    const auto plan = scheduler.BuildPlan(database, {
        AssetPreimportReason::FileWatcherChanged,
        {otherRoot / "Textures" / "HeroBaseColor.png"}
    });
    EXPECT_TRUE(plan.assetPaths.empty());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, MountedExternalDependencyChangeIgnoresNestedMountedRootWithSameRelativePath)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    std::filesystem::create_directories(root / "Assets");
    const auto parentRoot = root / "Packages";
    const auto childRoot = parentRoot / "StarterContent";
    const auto modelPath = parentRoot / "Models" / "ParentPackageHero.gltf";
    WriteBinary(parentRoot / "Textures" / "HeroBaseColor.png", TinyPng());
    WriteBinary(childRoot / "Textures" / "HeroBaseColor.png", TinyPng());
    WriteText(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
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
    PrepareStandardPbrShaderLabDependency(root);

    AssetDatabaseFacade database(AppendBuiltInShaderRootForTest(root, {
        EditorAssetRoot {root / "Assets", false, "Assets", root / "Library"},
        EditorAssetRoot {parentRoot, false, "Packages"},
        EditorAssetRoot {childRoot, false, "Packages/StarterContent"}
    }));
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup));
    ASSERT_EQ(database.GetCompletedImportCount(), 3u);

    const auto plan = scheduler.BuildPlan(database, {
        AssetPreimportReason::FileWatcherChanged,
        {childRoot / "Textures" / "HeroBaseColor.png"}
    });
    EXPECT_TRUE(plan.assetPaths.empty());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, AssetCopiedOrMovedPreimportSkipsWarmSceneAssetsWhenDependenciesAreCurrent)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "CopiedHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "CopiedHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup));
    ASSERT_EQ(database.GetCompletedImportCount(), 1u);

    ASSERT_TRUE(scheduler.Run(database, tracker, {
        AssetPreimportReason::AssetCopiedOrMoved,
        {std::filesystem::path("Assets") / "Models" / "CopiedHero.gltf"}
    }));
    EXPECT_EQ(database.GetCompletedImportCount(), 1u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, AbsoluteChangedPathsUseMountedRootInsteadOfFirstAssetsSegment)
{
    using namespace NLS::Editor::Assets;

    const auto outer = MakeEditorAssetTestRoot() / "Assets" / "NestedProject";
    const auto root = outer;
    const auto modelPath = root / "Assets" / "Models" / "NestedHero.gltf";
    WriteText(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "NestedHeroRoot" }]
        })");
    WriteText(root / "Assets" / "Models" / "StableHero.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    const auto plan = scheduler.BuildPlan(database, {
        AssetPreimportReason::FileWatcherChanged,
        {modelPath}
    });

    ASSERT_EQ(plan.assetPaths.size(), 1u);
    EXPECT_EQ(plan.assetPaths[0], "Assets/Models/NestedHero.gltf");

    std::filesystem::remove_all(root.parent_path().parent_path());
}

TEST(EditorAssetDatabaseTests, AbsoluteChangedPathsResolveCustomMountedAssetRoots)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    const auto packageRoot = root / "Packages" / "StarterContent";
    const auto modelPath = packageRoot / "Models" / "MountedHero.gltf";
    WriteText(modelPath, R"({"asset":{"version":"2.0"}})");
    WriteText(packageRoot / "Models" / "StableHero.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({
        EditorAssetRoot {packageRoot, false, "Packages/StarterContent"}
    });
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    const auto plan = scheduler.BuildPlan(database, {
        AssetPreimportReason::FileWatcherChanged,
        {modelPath}
    });

    ASSERT_EQ(plan.assetPaths.size(), 1u);
    EXPECT_EQ(plan.assetPaths[0], "Packages/StarterContent/Models/MountedHero.gltf");

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, AbsolutePathsOutsideMountedRootsAreNotEditorAssetPaths)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    const auto outsideRoot = MakeEditorAssetTestRoot();
    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    EXPECT_FALSE(database.TryGetEditorAssetPath(outsideRoot / "Assets" / "Models" / "Hero.gltf").has_value());

    std::filesystem::remove_all(outsideRoot);
    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, FileWatcherPreimportOnlyPlansChangedSceneAssets)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(root / "Assets" / "Models" / "ChangedHero.gltf", R"({"asset":{"version":"2.0"}})");
    WriteText(root / "Assets" / "Models" / "StableHero.gltf", R"({"asset":{"version":"2.0"}})");
    WriteText(root / "Assets" / "Textures" / "Changed.png", "not-a-real-png");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    const auto plan = scheduler.BuildPlan(database, {
        AssetPreimportReason::FileWatcherChanged,
        {std::filesystem::path("Assets") / "Models" / "ChangedHero.gltf"}
    });

    ASSERT_EQ(plan.assetPaths.size(), 1u);
    EXPECT_EQ(plan.assetPaths[0], "Assets/Models/ChangedHero.gltf");

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, FileWatcherPreimportMatchesAbsoluteChangedScenePaths)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    const auto changedModel = root / "Assets" / "Models" / "ChangedHero.gltf";
    WriteText(changedModel, R"({"asset":{"version":"2.0"}})");
    WriteText(root / "Assets" / "Models" / "StableHero.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    const auto plan = scheduler.BuildPlan(database, {
        AssetPreimportReason::FileWatcherChanged,
        {changedModel}
    });

    ASSERT_EQ(plan.assetPaths.size(), 1u);
    EXPECT_EQ(plan.assetPaths[0], "Assets/Models/ChangedHero.gltf");

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, FileWatcherPreimportPlansModelWhenExternalDependencyChanges)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(root / "Assets" / "Models" / "Hero.bin", "mesh-binary");
    WriteBinary(root / "Assets" / "Textures" / "HeroBaseColor.png", TinyPng());
    WriteText(
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
    WriteText(root / "Assets" / "Models" / "Stable.gltf", R"({"asset":{"version":"2.0"}})");
    PrepareStandardPbrShaderLabDependency(root);

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup));
    ASSERT_TRUE(database.Refresh());

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    WriteText(root / "Assets" / "Textures" / "HeroBaseColor.png", "changed-texture-bytes");

    const auto plan = scheduler.BuildPlan(database, {
        AssetPreimportReason::FileWatcherChanged,
        {root / "Assets" / "Textures" / "HeroBaseColor.png"}
    });

    ASSERT_EQ(plan.assetPaths.size(), 2u);
    EXPECT_EQ(plan.assetPaths[0], "Assets/Textures/HeroBaseColor.png");
    EXPECT_EQ(plan.assetPaths[1], "Assets/Models/Hero.gltf");

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, PreimportedPrefabArtifactIsWarmAfterDatabaseRefresh)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    NLS::Engine::GameObject gameObject("Lamp", "Prefab");
    const auto created = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &gameObject,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee")),
        "Assets/Prefabs/Lamp.prefab"
    });
    ASSERT_EQ(created.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    WriteText(root / "Assets" / "Prefabs" / "Lamp.prefab", created.prefabSourceText);

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup));

    AssetDatabaseFacade refreshedDatabase({root});
    ASSERT_TRUE(refreshedDatabase.Refresh());
    EXPECT_TRUE(refreshedDatabase
        .LoadSubAssetAtPath("Assets/Prefabs/Lamp.prefab", "prefab:Lamp")
        .has_value());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, BlockingStartupPreimportWarmsColdModelBeforeReturning)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "StartupColdHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "StartupColdHeroRoot" }]
        })");

    StartupAssetPreimportOptions options;
    options.projectRoot = root;

    std::vector<ImportProgressEvent> events;
    const auto result = RunBlockingStartupAssetPreimport(
        options,
        [&events](const ImportProgressEvent& event)
        {
            events.push_back(event);
        });

    EXPECT_TRUE(result.succeeded);
    EXPECT_EQ(result.plannedAssetCount, 1u);
    EXPECT_EQ(result.importedAssetCount, 1u);
    EXPECT_FALSE(result.hadRunningJobsAfterCompletion);
    ASSERT_FALSE(events.empty());
    EXPECT_TRUE(std::any_of(
        events.begin(),
        events.end(),
        [](const ImportProgressEvent& event)
        {
            return event.terminalStatus == ImportJobTerminalStatus::Succeeded;
        }));

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    EXPECT_TRUE(database
        .LoadSubAssetAtPath("Assets/Models/StartupColdHero.gltf", "prefab:StartupColdHero")
        .has_value());
    EXPECT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/StartupColdHero.gltf"));
    EXPECT_FALSE(database.AssetPathToGUID("Assets/Engine/Shaders/ShaderLab/StandardPBR.shader").empty());
    EXPECT_FALSE(std::filesystem::exists(root / "Assets" / "Engine" / "Shaders"))
        << "Built-in ShaderLab sources must stay in App/Assets and only project Library artifacts may be generated.";

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, BlockingStartupPreimportSucceedsWhenNoProjectAssetsNeedImport)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    std::filesystem::create_directories(root / "Assets");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;

    const auto result = RunBlockingStartupAssetPreimport(options);

    EXPECT_TRUE(result.succeeded) << JoinDiagnosticSummaries(result.diagnostics);
    EXPECT_EQ(result.plannedAssetCount, 0u);
    EXPECT_EQ(result.importedAssetCount, 0u);
    EXPECT_FALSE(result.hadRunningJobsAfterCompletion);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, BlockingStartupPreimportImportsBuiltInShaderForEmptyProject)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    std::filesystem::create_directories(root / "Assets");

    StartupAssetPreimportOptions options;
    options.projectRoot = root;

    const auto result = RunBlockingStartupAssetPreimport(options);

    EXPECT_TRUE(result.succeeded) << JoinDiagnosticSummaries(result.diagnostics);
    EXPECT_EQ(result.plannedAssetCount, 0u);
    EXPECT_EQ(result.importedAssetCount, 0u);
    EXPECT_FALSE(result.hadRunningJobsAfterCompletion);
    EXPECT_FALSE(std::filesystem::exists(root / "Assets" / "Engine" / "Shaders"))
        << "Built-in ShaderLab imports must produce project Library artifacts without copying sources.";

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    EXPECT_FALSE(database.AssetPathToGUID("Assets/Engine/Shaders/ShaderLab/StandardPBR.shader").empty());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, BlockingStartupPreimportDoesNotOverwriteProjectShaderLabSources)
{
    using namespace NLS::Editor::Assets;

    if (!HasExecutableShaderCompilerForEditorAssetTests())
        GTEST_SKIP() << "Blocking ShaderLab startup preimport requires an executable DXC compiler.";

    const auto root = MakeEditorAssetTestRoot();
    const auto bundledShaderRoot =
        std::filesystem::path(NLS_ROOT_DIR) /
        "App" /
        "Assets" /
        "Engine" /
        "Shaders";
    const auto bundledStandardPbr = bundledShaderRoot / "ShaderLab" / "StandardPBR.shader";
    const auto bundledCore = bundledShaderRoot / "NullusShaderLibrary" / "Core.hlsl";
    ASSERT_TRUE(std::filesystem::is_regular_file(bundledStandardPbr));
    ASSERT_TRUE(std::filesystem::is_regular_file(bundledCore));

    WriteText(
        root / "Assets" / "Engine" / "Shaders" / "ShaderLab" / "StandardPBR.shader",
        R"(Shader "Project/StandardPBR"
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
            float4 VSMain(float4 position : POSITION) : SV_POSITION { return position; }
            float4 PSMain() : SV_Target0 { return float4(1, 1, 1, 1); }
            ENDHLSL
        }
    }
}
)");
    WriteText(
        root / "Assets" / "Engine" / "Shaders" / "NullusShaderLibrary" / "Core.hlsl",
        "// stale library\n");
    WriteText(
        root / "Assets" / "Engine" / "Shaders" / "NullusShaderLibrary" / "ProjectOnly.hlsl",
        "// project local include\n");
    WriteText(
        root / "Assets" / "Models" / "StaleBundledShaderHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "StaleBundledShaderHeroRoot" }]
        })");

    StartupAssetPreimportOptions options;
    options.projectRoot = root;

    const auto result = RunBlockingStartupAssetPreimport(options);

    EXPECT_TRUE(result.succeeded);
    EXPECT_GE(result.importedAssetCount, 1u);
    EXPECT_EQ(
        ReadText(root / "Assets" / "Engine" / "Shaders" / "ShaderLab" / "StandardPBR.shader"),
        R"(Shader "Project/StandardPBR"
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
            float4 VSMain(float4 position : POSITION) : SV_POSITION { return position; }
            float4 PSMain() : SV_Target0 { return float4(1, 1, 1, 1); }
            ENDHLSL
        }
    }
}
)");
    EXPECT_EQ(
        ReadText(root / "Assets" / "Engine" / "Shaders" / "NullusShaderLibrary" / "Core.hlsl"),
        "// stale library\n");
    EXPECT_EQ(
        ReadText(root / "Assets" / "Engine" / "Shaders" / "NullusShaderLibrary" / "ProjectOnly.hlsl"),
        "// project local include\n");
    EXPECT_FALSE(std::filesystem::exists(
        root / "Assets" / "Engine" / "Shaders" / "NullusShaderLibrary" / "Common.hlsl"))
        << "Startup preimport must not seed bundled shader library files into project Assets.";

    std::filesystem::remove_all(root);
}

#if NLS_HAS_ASSIMP_FBX_IMPORTER
TEST(EditorAssetDatabaseTests, BlockingStartupPreimportFallsBackForDefaultFbxReaderWhenAutodeskSdkUnavailable)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "StartupDefaultFbx.fbx";
    std::filesystem::create_directories(sourcePath.parent_path());
    std::filesystem::copy_file(
        std::filesystem::path(NLS_ROOT_DIR) / "App" / "Assets" / "Engine" / "Models" / "Cube.fbx",
        sourcePath,
        std::filesystem::copy_options::overwrite_existing);
    WriteText(
        sourcePath.string() + ".meta",
        "GUID=1e1e1e1e-1e1e-4e1e-8e1e-1e1e1e1e1e1e\n"
        "IMPORTER_ID=scene-model\n"
        "IMPORTER_VERSION=" +
            std::to_string(NLS::Core::Assets::GetCurrentImporterVersion(
                NLS::Core::Assets::AssetType::ModelScene)) +
            "\n"
        "ASSET_TYPE=model-scene\n");

    StartupAssetPreimportOptions options;
    options.projectRoot = root;

    const auto result = RunBlockingStartupAssetPreimport(options);

    EXPECT_TRUE(result.succeeded) << "Default FBX reader should not hard-fail startup when Autodesk SDK is unavailable.";
    EXPECT_EQ(result.plannedAssetCount, 1u);
    EXPECT_EQ(result.importedAssetCount, 1u);
    EXPECT_FALSE(result.hadRunningJobsAfterCompletion);

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    EXPECT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/StartupDefaultFbx.fbx"));
    EXPECT_TRUE(database
        .LoadSubAssetAtPath("Assets/Models/StartupDefaultFbx.fbx", "prefab:StartupDefaultFbx")
        .has_value());

    std::filesystem::remove_all(root);
}
#endif

TEST(EditorAssetDatabaseTests, StartupPreimportUsesAlreadyRefreshedPlanWithoutSecondFullRefresh)
{
    const auto projectRoot = std::filesystem::path(NLS_ROOT_DIR);
    const auto editorStartupSource = ReadText(projectRoot / "Project/Editor/Assets/EditorStartupAssetPreimport.cpp");
    const auto schedulerSource = ReadText(projectRoot / "Project/Editor/Assets/EditorAssetDatabase.cpp");

    EXPECT_NE(editorStartupSource.find("RunAlreadyPlanned("), std::string::npos);
    EXPECT_NE(schedulerSource.find("bool AssetPreimportScheduler::RunAlreadyPlanned("), std::string::npos);
}

TEST(EditorAssetDatabaseTests, StartupPreimportRunsPlannedImportsAgainstCurrentDatabaseSnapshot)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "PlannedHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "PlannedHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    const auto plan = scheduler.BuildPlan(database);
    ASSERT_EQ(plan.assetPaths, std::vector<std::string>({"Assets/Models/PlannedHero.gltf"}));

    WriteText(
        root / "Assets" / "Models" / "CreatedAfterPlan.gltf",
        R"({"asset":{"version":"2.0"}})");

    ImportProgressTracker progressTracker;
    EXPECT_TRUE(scheduler.RunAlreadyPlanned(
        database,
        progressTracker,
        {AssetPreimportReason::EditorStartup, {}},
        plan));

    EXPECT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/PlannedHero.gltf"));
    EXPECT_TRUE(database.AssetPathToGUID("Assets/Models/CreatedAfterPlan.gltf").empty());
    EXPECT_FALSE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/CreatedAfterPlan.gltf"));

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, StartupPreimportReportsPlanningAndCurrentAssetProgress)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "ProgressHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "ProgressHeroRoot" }]
        })");

    std::vector<ImportProgressEvent> events;
    const auto result = RunBlockingStartupAssetPreimport(
        {root},
        [&events](const ImportProgressEvent& event)
        {
            events.push_back(event);
        });

    EXPECT_TRUE(result.succeeded);
    ASSERT_FALSE(events.empty());
    EXPECT_TRUE(std::any_of(
        events.begin(),
        events.end(),
        [](const ImportProgressEvent& event)
        {
            return event.message == "Scanning project assets" &&
                event.sourcePath == "Assets";
        }));
    EXPECT_TRUE(std::any_of(
        events.begin(),
        events.end(),
        [](const ImportProgressEvent& event)
        {
            return event.message == "Planning startup asset imports" &&
                event.sourcePath == "Assets";
        }));
    EXPECT_TRUE(std::any_of(
        events.begin(),
        events.end(),
        [](const ImportProgressEvent& event)
        {
            return event.message == "Reading model source" &&
                event.sourcePath == "Assets/Models/ProgressHero.gltf";
        }));

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, StartupPreimportReportsOneAggregatedProgressForMultipleAssets)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "FirstHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "FirstHeroRoot" }]
        })");
    WriteText(
        root / "Assets" / "Models" / "SecondHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "SecondHeroRoot" }]
        })");

    std::vector<ImportProgressEvent> events;
    const auto result = RunBlockingStartupAssetPreimport(
        {root},
        [&events](const ImportProgressEvent& event)
        {
            events.push_back(event);
        });

    EXPECT_TRUE(result.succeeded);
    EXPECT_EQ(result.plannedAssetCount, 2u);
    EXPECT_EQ(result.importedAssetCount, 2u);

    std::vector<ImportProgressEvent> importEvents;
    std::copy_if(
        events.begin(),
        events.end(),
        std::back_inserter(importEvents),
        [](const ImportProgressEvent& event)
        {
            return event.jobId.IsValid();
        });
    ASSERT_FALSE(importEvents.empty());

    double previousProgress = 0.0;
    for (const auto& event : importEvents)
    {
        EXPECT_GE(event.normalizedProgress, previousProgress);
        previousProgress = event.normalizedProgress;
    }
    EXPECT_DOUBLE_EQ(importEvents.back().normalizedProgress, 1.0);

    const auto secondAssetFirstEvent = std::find_if(
        importEvents.begin(),
        importEvents.end(),
        [](const ImportProgressEvent& event)
        {
            return event.sourcePath == "Assets/Models/SecondHero.gltf";
        });
    ASSERT_NE(secondAssetFirstEvent, importEvents.end());
    EXPECT_GE(secondAssetFirstEvent->normalizedProgress, 0.5);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, StartupPreparedPrefabPreflightBudgetCapsAttempts)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "A_FirstHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "FirstHeroRoot" }]
        })");

    WriteText(
        root / "Assets" / "Models" / "B_SecondHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "SecondHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/A_FirstHero.gltf"));
    ASSERT_TRUE(database.ImportAsset("Assets/Models/B_SecondHero.gltf"));

#if defined(NLS_ENABLE_TEST_HOOKS)
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    const auto secondGuid = database.AssetPathToGUID("Assets/Models/B_SecondHero.gltf");
    ASSERT_FALSE(secondGuid.empty());
    NLS::Editor::Assets::UnifiedPrefabLoadRequest secondRequest;
    secondRequest.source = NLS::Editor::Assets::NormalizePrefabSourceIdentity(
        root,
        "Assets/Models/B_SecondHero.gltf",
        "prefab:B_SecondHero",
        NLS::Core::Assets::AssetId(NLS::Guid::Parse(secondGuid)),
        NLS::Core::Assets::AssetType::ModelScene);
    secondRequest.requiredReadiness = NLS::Editor::Assets::UnifiedPrefabReadiness::PrefabGraphOnly;
    secondRequest.allowPending = false;
    auto seededSecondCache = bridge.LoadUnifiedPrefabShared(secondRequest);
    ASSERT_NE(seededSecondCache.prefab, nullptr);
    NLS::Editor::Assets::ClearImportedPrefabHotCacheForTesting();
#endif

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.enablePreparedPrefabCachePreflight = true;
    options.maxPreparedPrefabCachePreflightCount = 1u;
    options.priorityPreparedPrefabAssetPaths = {"Assets/Models/B_SecondHero.gltf"};

    const auto result = RunBlockingStartupAssetPreimport(options);

    EXPECT_TRUE(result.succeeded);
    EXPECT_EQ(result.plannedAssetCount, 0u);
    EXPECT_EQ(result.importedAssetCount, 0u);
    EXPECT_EQ(result.preparedPrefabCachePreflightAttemptCount, 1u);
#if defined(NLS_ENABLE_TEST_HOOKS)
    EXPECT_EQ(result.preparedPrefabCachePreflightCount, 1u);
#else
    EXPECT_EQ(result.preparedPrefabCachePreflightCount, 0u);
#endif

#if defined(NLS_ENABLE_TEST_HOOKS)
    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    auto secondLoad = bridge.LoadUnifiedPrefabShared(secondRequest);
    ASSERT_NE(secondLoad.prefab, nullptr);
    const auto secondRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_GE(
        CountArtifactTelemetryStage(
            secondRecords,
            NLS::Core::Assets::ArtifactLoadTelemetryStage::CacheHit),
        1u);

    NLS::Editor::Assets::ClearImportedPrefabHotCacheForTesting();
    const auto firstGuid = database.AssetPathToGUID("Assets/Models/A_FirstHero.gltf");
    ASSERT_FALSE(firstGuid.empty());
    NLS::Editor::Assets::UnifiedPrefabLoadRequest firstRequest;
    firstRequest.source = NLS::Editor::Assets::NormalizePrefabSourceIdentity(
        root,
        "Assets/Models/A_FirstHero.gltf",
        "prefab:A_FirstHero",
        NLS::Core::Assets::AssetId(NLS::Guid::Parse(firstGuid)),
        NLS::Core::Assets::AssetType::ModelScene);
    firstRequest.requiredReadiness = NLS::Editor::Assets::UnifiedPrefabReadiness::PrefabGraphOnly;
    firstRequest.allowPending = false;

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    auto firstLoad = bridge.LoadUnifiedPrefabShared(firstRequest);
    ASSERT_NE(firstLoad.prefab, nullptr);
    const auto firstRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(
        CountArtifactTelemetryStage(
            firstRecords,
            NLS::Core::Assets::ArtifactLoadTelemetryStage::CacheHit),
        0u);
    EXPECT_GE(
        CountArtifactTelemetryStage(
            firstRecords,
            NLS::Core::Assets::ArtifactLoadTelemetryStage::PrefabGraphLoad),
        1u);
#endif

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, StartupPreimportProgressLabelIncludesCurrentAssetPath)
{
    using namespace NLS::Editor::Assets;

    ImportProgressEvent event;
    event.message = "Reading model source";
    event.sourcePath = "Assets/Model/main_sponza/NewSponza_Main_Zup_003.fbx";

    EXPECT_EQ(
        FormatStartupAssetPreimportProgressLabel(event),
        "Reading model source: NewSponza_Main_Zup_003.fbx (Assets/Model/main_sponza/NewSponza_Main_Zup_003.fbx)");
}

TEST(EditorAssetDatabaseTests, BlockingStartupPreimportProducesWarmDragHandleBeforeEditorOpens)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "StartupDragHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "StartupDragHeroRoot" }]
        })");

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    const auto result = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(result.succeeded);
    ASSERT_FALSE(result.hadRunningJobsAfterCompletion);

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    const auto guid = database.AssetPathToGUID("Assets/Models/StartupDragHero.gltf");
    ASSERT_FALSE(guid.empty());

    const auto payload = MakeEditorAssetDragPayload(
        "Assets/Models/StartupDragHero.gltf",
        NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid)),
        "prefab:StartupDragHero",
        NLS::Core::Assets::ArtifactType::Prefab,
        true,
        true);

    NLS::Engine::SceneSystem::Scene scene;
    EditorAssetDragDropBridge bridge(root / "Assets");
    const auto drop = bridge.DropImportedAssetHandleIntoHierarchy(payload, scene);

    ASSERT_TRUE(drop.handled);
    EXPECT_FALSE(drop.pendingImport);
    ASSERT_EQ(drop.dragDrop.status, DragDropOperationStatus::Committed);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front()->GetName(), "StartupDragHeroRoot");

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, BlockingStartupPreimportRepairsUnreadableWarmPrefabBeforeEditorOpens)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "UnreadableWarmHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "UnreadableWarmHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    {
        AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
        ASSERT_TRUE(database.Refresh());
        ASSERT_TRUE(database.ImportAsset("Assets/Models/UnreadableWarmHero.gltf"));
        ASSERT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/UnreadableWarmHero.gltf"));
        ASSERT_TRUE(database
            .LoadPrefabArtifactAtPath("Assets/Models/UnreadableWarmHero.gltf", "prefab:UnreadableWarmHero")
            .has_value());

        const auto artifactPath = database.ResolveArtifactPathAtPath(
            "Assets/Models/UnreadableWarmHero.gltf",
            "prefab:UnreadableWarmHero");
        ASSERT_FALSE(artifactPath.empty());
        WriteText(artifactPath, "not a prefab graph");
    }

    {
        AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
        ASSERT_TRUE(database.Refresh());
        ASSERT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/UnreadableWarmHero.gltf"));
        EXPECT_FALSE(database
            .LoadPrefabArtifactAtPath("Assets/Models/UnreadableWarmHero.gltf", "prefab:UnreadableWarmHero")
            .has_value());

        const auto plan = AssetPreimportScheduler().BuildPlan(database);
        ASSERT_EQ(plan.assetPaths.size(), 1u);
        EXPECT_EQ(plan.assetPaths[0], "Assets/Models/UnreadableWarmHero.gltf");
    }

    const auto result = RunBlockingStartupAssetPreimport({root});

    ASSERT_TRUE(result.succeeded);
    EXPECT_EQ(result.plannedAssetCount, 1u);
    EXPECT_EQ(result.importedAssetCount, 1u);
    EXPECT_FALSE(result.hadRunningJobsAfterCompletion);

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    const auto guid = database.AssetPathToGUID("Assets/Models/UnreadableWarmHero.gltf");
    ASSERT_FALSE(guid.empty());

    const auto payload = MakeEditorAssetDragPayload(
        "Assets/Models/UnreadableWarmHero.gltf",
        NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid)),
        "prefab:UnreadableWarmHero",
        NLS::Core::Assets::ArtifactType::Prefab,
        true,
        true);

    NLS::Engine::SceneSystem::Scene scene;
    EditorAssetDragDropBridge bridge(root / "Assets");
    const auto drop = bridge.DropImportedAssetHandleIntoHierarchy(payload, scene);

    ASSERT_TRUE(drop.handled);
    EXPECT_FALSE(drop.pendingImport);
    ASSERT_EQ(drop.dragDrop.status, DragDropOperationStatus::Committed);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front()->GetName(), "UnreadableWarmHeroRoot");

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, BlockingStartupPreimportRejectsEscapingDatabaseArtifactPath)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    const auto outsideRoot =
        std::filesystem::temp_directory_path() /
        ("nullus_database_escaping_artifact_" + NLS::Guid::New().ToString());
    WriteText(
        root / "Assets" / "Models" / "EscapingDatabaseHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "EscapingDatabaseHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    {
        AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
        ASSERT_TRUE(database.Refresh());
        ASSERT_TRUE(database.ImportAsset("Assets/Models/EscapingDatabaseHero.gltf"));

        const auto artifactPath = database.ResolveArtifactPathAtPath(
            "Assets/Models/EscapingDatabaseHero.gltf",
            "prefab:EscapingDatabaseHero");
        ASSERT_FALSE(artifactPath.empty());

        std::filesystem::create_directories(outsideRoot);
        const auto outsideArtifactPath = outsideRoot / artifactPath.filename();
        std::filesystem::copy_file(
            artifactPath,
            outsideArtifactPath,
            std::filesystem::copy_options::overwrite_existing);

        ReplaceArtifactDatabaseFieldForTest(
            root / "Library" / "ArtifactDB",
            "prefab:EscapingDatabaseHero",
            7u,
            outsideArtifactPath.string());
    }

    {
        AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
        ASSERT_TRUE(database.Refresh());
        EXPECT_FALSE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/EscapingDatabaseHero.gltf"));
        EXPECT_FALSE(database
            .LoadPrefabArtifactAtPath("Assets/Models/EscapingDatabaseHero.gltf", "prefab:EscapingDatabaseHero")
            .has_value());

        const auto plan = AssetPreimportScheduler().BuildPlan(database);
        ASSERT_EQ(plan.assetPaths.size(), 1u);
        EXPECT_NE(
            std::find(plan.assetPaths.begin(), plan.assetPaths.end(), "Assets/Models/EscapingDatabaseHero.gltf"),
            plan.assetPaths.end());
    }

    const auto result = RunBlockingStartupAssetPreimport({root});
    ASSERT_TRUE(result.succeeded);
    EXPECT_EQ(result.plannedAssetCount, 1u);
    EXPECT_EQ(result.importedAssetCount, 1u);

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    EXPECT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/EscapingDatabaseHero.gltf"));
    EXPECT_TRUE(database
        .LoadPrefabArtifactAtPath("Assets/Models/EscapingDatabaseHero.gltf", "prefab:EscapingDatabaseHero")
        .has_value());

    std::filesystem::remove_all(outsideRoot);
    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, DatabaseArtifactPathRejectsProjectRelativeTypedPayloadNameInsideOwnerArtifactRoot)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "ProjectRelativeArtifactHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "ProjectRelativeArtifactHeroRoot" }]
        })");

    {
        AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
        ASSERT_TRUE(database.Refresh());
        ASSERT_TRUE(database.ImportAsset("Assets/Models/ProjectRelativeArtifactHero.gltf"));

        ReplaceArtifactDatabaseFieldForTest(
            root / "Library" / "ArtifactDB",
            "prefab:ProjectRelativeArtifactHero",
            7u,
            "Library/Artifacts/ProjectRelativeArtifactHero/670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772");
    }

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    EXPECT_FALSE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/ProjectRelativeArtifactHero.gltf"));
    EXPECT_FALSE(database
        .LoadPrefabArtifactAtPath(
            "Assets/Models/ProjectRelativeArtifactHero.gltf",
            "prefab:ProjectRelativeArtifactHero")
        .has_value());

    const auto plan = AssetPreimportScheduler().BuildPlan(database);
    ASSERT_EQ(plan.assetPaths.size(), 1u);
    EXPECT_NE(
        std::find(plan.assetPaths.begin(), plan.assetPaths.end(), "Assets/Models/ProjectRelativeArtifactHero.gltf"),
        plan.assetPaths.end());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, BlockingStartupPreimportRejectsArtifactRootRelativeTypedPayloadName)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Model" / "ArtifactRootRelativeHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "ArtifactRootRelativeHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    {
        AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
        ASSERT_TRUE(database.Refresh());
        ASSERT_TRUE(database.ImportAsset("Assets/Model/ArtifactRootRelativeHero.gltf"));

        ReplaceArtifactDatabaseFieldForTest(
            root / "Library" / "ArtifactDB",
            "prefab:ArtifactRootRelativeHero",
            7u,
            "670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772");
    }

    {
        AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
        ASSERT_TRUE(database.Refresh());
        ASSERT_FALSE(database
            .LoadPrefabArtifactAtPath(
                "Assets/Model/ArtifactRootRelativeHero.gltf",
                "prefab:ArtifactRootRelativeHero")
            .has_value());

        const auto plan = AssetPreimportScheduler().BuildPlan(database);
        ASSERT_EQ(plan.assetPaths.size(), 1u);
        EXPECT_NE(
            std::find(plan.assetPaths.begin(), plan.assetPaths.end(), "Assets/Model/ArtifactRootRelativeHero.gltf"),
            plan.assetPaths.end());
    }

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, BlockingStartupPreimportScansAssetsButDoesNotImportLibraryArtifacts)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "LibraryIsCacheHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "LibraryIsCacheHeroRoot" }]
        })");
    WriteText(root / "Library" / "Artifacts" / "cached" / "670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772", "cached-prefab");
    WriteText(root / "Library" / "Artifacts" / "cached" / "36eee85124b95361c55a48634e6956a87607d0b6a69bfd04ffcd04f145ffa8d7", "cached-mesh");

    const auto result = RunBlockingStartupAssetPreimport({root});

    EXPECT_TRUE(result.succeeded);
    EXPECT_EQ(result.plannedAssetCount, 1u);
    EXPECT_EQ(result.importedAssetCount, 1u);
    EXPECT_FALSE(std::filesystem::exists(
        root / "Library" / "Artifacts" / "cached" /
            "670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772.meta"));
    EXPECT_FALSE(std::filesystem::exists(
        root / "Library" / "Artifacts" / "cached" / "36eee85124b95361c55a48634e6956a87607d0b6a69bfd04ffcd04f145ffa8d7.meta"));

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    const auto plan = AssetPreimportScheduler().BuildPlan(database);
    EXPECT_TRUE(plan.assetPaths.empty());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, BlockingStartupPreimportUsesCacheForUnchangedRepeatStartup)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "CachedStartupHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "CachedStartupHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    EXPECT_FALSE(firstResult.usedCache);
    EXPECT_EQ(firstResult.plannedAssetCount, 1u);
    EXPECT_EQ(firstResult.importedAssetCount, 1u);

    std::vector<std::string> progressMessages;
    const auto secondResult = RunBlockingStartupAssetPreimport(
        options,
        [&progressMessages](const ImportProgressEvent& event)
        {
            progressMessages.push_back(event.message);
        });

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_TRUE(secondResult.usedCache);
    EXPECT_EQ(secondResult.plannedAssetCount, 0u);
    EXPECT_EQ(secondResult.importedAssetCount, 0u);
    ASSERT_FALSE(progressMessages.empty());
    EXPECT_EQ(progressMessages.front(), "Checking startup asset cache");
    EXPECT_NE(
        std::find(progressMessages.begin(), progressMessages.end(), "Startup asset artifacts are current"),
        progressMessages.end());
    EXPECT_EQ(
        std::find(progressMessages.begin(), progressMessages.end(), "Scanning project assets"),
        progressMessages.end());
    EXPECT_EQ(
        std::find(progressMessages.begin(), progressMessages.end(), "Planning startup asset imports"),
        progressMessages.end());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, StartupPreimportSkipsAlreadyCurrentAssetsDuringPlanning)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "PlanSkipWarmHeroA.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "PlanSkipWarmHeroARoot" }]
        })");
    WriteText(
        root / "Assets" / "Models" / "PlanSkipWarmHeroB.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "PlanSkipWarmHeroBRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);
    EXPECT_EQ(firstResult.plannedAssetCount, 2u);
    EXPECT_EQ(firstResult.importedAssetCount, 2u);

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    WriteText(
        root / "Assets" / "Models" / "PlanSkipWarmHeroB.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "PlanSkipWarmHeroBRootChanged" }]
        })");

    const auto secondResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_FALSE(secondResult.usedCache);
    EXPECT_EQ(secondResult.plannedAssetCount, 1u);
    EXPECT_EQ(secondResult.importedAssetCount, 1u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, StartupPreimportLoadsCacheIndexOnceForChangedSourceMiss)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    const auto stableModelPath = root / "Assets" / "Models" / "SinglePassStableHero.gltf";
    const auto changedModelPath = root / "Assets" / "Models" / "SinglePassChangedHero.gltf";
    WriteText(
        stableModelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "SinglePassStableHeroRoot" }]
        })");
    WriteText(
        changedModelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "SinglePassChangedHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);
    ASSERT_EQ(firstResult.plannedAssetCount, 2u);
    ASSERT_EQ(firstResult.importedAssetCount, 2u);

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    WriteText(
        changedModelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "SinglePassChangedHeroRootUpdated" }]
        })");

    ResetStartupAssetPreimportIndexLoadCountForTesting();
    const auto secondResult = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_FALSE(secondResult.usedCache);
    EXPECT_EQ(secondResult.plannedAssetCount, 1u);
    EXPECT_EQ(secondResult.importedAssetCount, 1u);
    EXPECT_EQ(GetStartupAssetPreimportIndexLoadCountForTesting(), 1u);

    std::filesystem::remove_all(root);
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to count startup cache index loads.";
#endif
}

TEST(EditorAssetDatabaseTests, StartupPreimportAvoidsFullSourceRefreshForChangedSourceMiss)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    const auto stableModelPath = root / "Assets" / "Models" / "TargetedRefreshStableHero.gltf";
    const auto changedModelPath = root / "Assets" / "Models" / "TargetedRefreshChangedHero.gltf";
    WriteText(
        stableModelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "TargetedRefreshStableHeroRoot" }]
        })");
    WriteText(
        changedModelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "TargetedRefreshChangedHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);
    ASSERT_EQ(firstResult.plannedAssetCount, 2u);
    ASSERT_EQ(firstResult.importedAssetCount, 2u);

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    WriteText(
        changedModelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "TargetedRefreshChangedHeroRootUpdated" }]
        })");

    ResetAssetDatabaseFullSourceRefreshCountForTesting();
    ResetAssetDatabaseLastKnownSourceRefreshAssetCountForTesting();
    const auto secondResult = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_FALSE(secondResult.usedCache);
    EXPECT_EQ(secondResult.plannedAssetCount, 1u);
    EXPECT_EQ(secondResult.importedAssetCount, 1u);
    EXPECT_EQ(GetAssetDatabaseFullSourceRefreshCountForTesting(), 0u);

    std::filesystem::remove_all(root);
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to count full source refreshes.";
#endif
}

TEST(EditorAssetDatabaseTests, TargetedRefreshLazilyLoadsPersistedManifestForKnownSource)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    const auto modelPath = root / "Assets" / "Models" / "LazyTargetedManifestHero.gltf";
    WriteText(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "LazyTargetedManifestHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    {
        AssetDatabaseFacade warmDatabase(MakeProjectEditorAssetRoots(root));
        ASSERT_TRUE(warmDatabase.Refresh());
        ASSERT_TRUE(warmDatabase.ImportAsset("Assets/Models/LazyTargetedManifestHero.gltf"));
        ASSERT_TRUE(warmDatabase.IsArtifactManifestCurrentForAssetPath(
            "Assets/Models/LazyTargetedManifestHero.gltf"));
    }

    ResetAssetDatabaseFullSourceRefreshCountForTesting();
    ResetAssetDatabasePersistedManifestLoadCountForTesting();
    AssetDatabaseFacade targetedDatabase(MakeProjectEditorAssetRoots(root));
    const std::array knownSourcePaths {modelPath};
    ASSERT_TRUE(targetedDatabase.RefreshKnownSourceAssets(knownSourcePaths));
    EXPECT_EQ(GetAssetDatabaseFullSourceRefreshCountForTesting(), 0u);
    EXPECT_EQ(GetAssetDatabasePersistedManifestLoadCountForTesting(), 0u);

    const auto manifest = targetedDatabase.GetArtifactManifestForAssetPath(
        "Assets/Models/LazyTargetedManifestHero.gltf");
    ASSERT_TRUE(manifest.has_value());
    EXPECT_TRUE(manifest->sourceAssetId.IsValid());
    EXPECT_NE(manifest->FindSubAsset("prefab:LazyTargetedManifestHero"), nullptr);

    const auto artifactPath = targetedDatabase.ResolveArtifactPathAtPath(
        "Assets/Models/LazyTargetedManifestHero.gltf",
        "prefab:LazyTargetedManifestHero");
    EXPECT_FALSE(artifactPath.empty());
    EXPECT_TRUE(std::filesystem::exists(artifactPath));

    std::filesystem::remove_all(root);
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to count full source refreshes.";
#endif
}

TEST(EditorAssetDatabaseTests, StartupPreimportCacheHitAvoidsRecursiveSourceEnumeration)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "EnumerationFastPathHeroA.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "EnumerationFastPathHeroARoot" }]
        })");
    WriteText(
        root / "Assets" / "Models" / "EnumerationFastPathHeroB.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "EnumerationFastPathHeroBRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);
    ASSERT_EQ(firstResult.plannedAssetCount, 2u);
    ASSERT_EQ(firstResult.importedAssetCount, 2u);
    ASSERT_TRUE(std::filesystem::exists(root / "Library" / "Editor" / "StartupAssetPreimport.stamp"));

    ResetStartupAssetPreimportSourceEnumerationCountForTesting();
    const auto secondResult = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_TRUE(secondResult.usedCache);
    EXPECT_EQ(secondResult.plannedAssetCount, 0u);
    EXPECT_EQ(secondResult.importedAssetCount, 0u);
    EXPECT_EQ(GetStartupAssetPreimportSourceEnumerationCountForTesting(), 0u);

    std::filesystem::remove_all(root);
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to count startup source enumeration.";
#endif
}

TEST(EditorAssetDatabaseTests, StartupPreimportCacheHitAvoidsContentHashReads)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "FingerprintFastPathHeroA.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "FingerprintFastPathHeroARoot" }]
        })");
    WriteText(
        root / "Assets" / "Models" / "FingerprintFastPathHeroB.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "FingerprintFastPathHeroBRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);
    ASSERT_EQ(firstResult.plannedAssetCount, 2u);
    ASSERT_EQ(firstResult.importedAssetCount, 2u);
    ASSERT_TRUE(std::filesystem::exists(root / "Library" / "Editor" / "StartupAssetPreimport.stamp"));

    ResetStartupAssetPreimportContentHashReadCountForTesting();
    const auto secondResult = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_TRUE(secondResult.usedCache);
    EXPECT_EQ(secondResult.plannedAssetCount, 0u);
    EXPECT_EQ(secondResult.importedAssetCount, 0u);
    EXPECT_EQ(GetStartupAssetPreimportContentHashReadCountForTesting(), 0u);

#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to count startup content hash reads.";
#endif
}

TEST(EditorAssetDatabaseTests, StartupPreimportIndexRebuildUsesManifestContentHashes)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "IndexManifestHashHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "IndexManifestHashHeroRoot" }]
        })");
    WriteText(
        root / "Assets" / "Models" / "IndexManifestHashSidekick.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "IndexManifestHashSidekickRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto result = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(result.succeeded) << JoinDiagnosticSummaries(result.diagnostics);
    ASSERT_FALSE(result.usedCache);

    std::unordered_map<std::string, std::string> manifestHashesByArtifactPath;
    {
        AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
        ASSERT_TRUE(database.Refresh());
        for (const auto* assetPath : {
            "Assets/Models/IndexManifestHashHero.gltf",
            "Assets/Models/IndexManifestHashSidekick.gltf"})
        {
            const auto manifest = database.GetArtifactManifestForAssetPath(assetPath);
            ASSERT_TRUE(manifest.has_value());
            for (const auto& artifact : manifest->subAssets)
            {
                const auto artifactPath = database.ResolveArtifactPathAtPath(assetPath, artifact.subAssetKey);
                ASSERT_FALSE(artifactPath.empty());
                manifestHashesByArtifactPath.emplace(
                    NLS::Core::Assets::NormalizeAssetPath(artifactPath).generic_string(),
                    artifact.contentHash);
            }
        }
    }
    ASSERT_FALSE(manifestHashesByArtifactPath.empty());

    ResetStartupAssetPreimportContentHashReadCountForTesting();
    ASSERT_TRUE(RewriteStartupAssetPreimportIndexForTesting(root));

    std::ifstream indexInput(root / "Library" / "Editor" / "StartupAssetPreimport.stamp", std::ios::binary);
    ASSERT_TRUE(indexInput);
    std::string line;
    size_t checkedArtifactLines = 0u;
    while (std::getline(indexInput, line))
    {
        std::istringstream stream(line);
        std::string key;
        stream >> key;
        if (key != "artifact")
            continue;

        std::string artifactPath;
        std::string stamp;
        std::string contentHash;
        std::string fingerprint;
        stream >> std::quoted(artifactPath)
            >> std::quoted(stamp)
            >> std::quoted(contentHash)
            >> std::quoted(fingerprint);
        ASSERT_FALSE(stream.fail());
        const auto found = manifestHashesByArtifactPath.find(artifactPath);
        if (found == manifestHashesByArtifactPath.end())
            continue;
        EXPECT_EQ(contentHash, found->second);
        ++checkedArtifactLines;
    }
    EXPECT_EQ(checkedArtifactLines, manifestHashesByArtifactPath.size());

#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to count startup content hash reads.";
#endif
}

TEST(EditorAssetDatabaseTests, StartupPreimportMissingIndexSkipsImporterFingerprintComputation)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    std::filesystem::create_directories(root / "Assets");

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto result = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(result.succeeded) << JoinDiagnosticSummaries(result.diagnostics);
    EXPECT_EQ(result.cacheValidationProfile.missReason, "index-unavailable");
    EXPECT_EQ(result.cacheValidationProfile.importerFingerprintComputeCount, 0u);

    std::filesystem::remove_all(root);
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to count startup importer fingerprint computation.";
#endif
}

TEST(EditorAssetDatabaseTests, StartupPreimportCacheHitQueriesFileMetadataOncePerEntry)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "MetadataFastPathHeroA.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "MetadataFastPathHeroARoot" }]
        })");
    WriteText(
        root / "Assets" / "Models" / "MetadataFastPathHeroB.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "MetadataFastPathHeroBRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);
    ASSERT_EQ(firstResult.plannedAssetCount, 2u);
    ASSERT_EQ(firstResult.importedAssetCount, 2u);

    ResetStartupAssetPreimportFileMetadataQueryCountForTesting();
    const auto secondResult = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_TRUE(secondResult.usedCache);
    EXPECT_EQ(secondResult.plannedAssetCount, 0u);
    EXPECT_EQ(secondResult.importedAssetCount, 0u);
    const size_t expectedMetadataQueryUpperBound =
        secondResult.cacheValidationProfile.trackedFileEntryCount +
        secondResult.cacheValidationProfile.sourceDirectoryEntryCount;
    EXPECT_LE(
        GetStartupAssetPreimportFileMetadataQueryCountForTesting(),
        expectedMetadataQueryUpperBound);

    std::filesystem::remove_all(root);
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to count startup file metadata queries.";
#endif
}

TEST(EditorAssetDatabaseTests, StartupPreimportCacheHitReportsValidationProfile)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "ProfiledCacheHeroA.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "ProfiledCacheHeroARoot" }]
        })");
    WriteText(
        root / "Assets" / "Models" / "ProfiledCacheHeroB.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "ProfiledCacheHeroBRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);

    ResetStartupAssetPreimportFileMetadataQueryCountForTesting();
    ResetStartupAssetPreimportContentHashReadCountForTesting();
    const auto secondResult = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    ASSERT_TRUE(secondResult.usedCache);
    EXPECT_EQ(secondResult.cacheValidationProfile.sourceEntryCount, 5u);
    EXPECT_EQ(secondResult.cacheValidationProfile.sourceDirectoryEntryCount, 2u);
    EXPECT_EQ(secondResult.cacheValidationProfile.dependencyEntryCount, 0u);
    EXPECT_EQ(secondResult.cacheValidationProfile.artifactEntryCount, 4u);
    EXPECT_EQ(secondResult.cacheValidationProfile.trackedFileEntryCount, 9u);
    EXPECT_EQ(secondResult.cacheValidationProfile.fileMetadataQueryCount, GetStartupAssetPreimportFileMetadataQueryCountForTesting());
    EXPECT_EQ(secondResult.cacheValidationProfile.contentHashReadCount, GetStartupAssetPreimportContentHashReadCountForTesting());

    std::filesystem::remove_all(root);
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to report startup cache validation profile counters.";
#endif
}

TEST(EditorAssetDatabaseTests, StartupPreimportCacheIndexRebuildSkipsUnchangedShardWrites)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "ShardWriteHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "ShardWriteHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);
    EXPECT_GT(GetStartupAssetPreimportShardWriteCountForTesting(), 0u);

    ResetStartupAssetPreimportShardWriteCountForTesting();
    ASSERT_TRUE(RewriteStartupAssetPreimportIndexForTesting(root));
    EXPECT_EQ(GetStartupAssetPreimportShardWriteCountForTesting(), 0u);

    const auto secondResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    ASSERT_TRUE(secondResult.usedCache);

    std::filesystem::remove_all(root);
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to count startup index shard writes.";
#endif
}

TEST(EditorAssetDatabaseTests, StartupPreimportPatchesChangedSourceIndexWhenPlanIsEmpty)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    const auto modelPath = root / "Assets" / "Models" / "StampOnlyStartupHero.gltf";
    WriteText(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "StampOnlyStartupHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);
    ASSERT_EQ(firstResult.plannedAssetCount, 1u);
    ASSERT_EQ(firstResult.importedAssetCount, 1u);

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto text = ReadText(modelPath);
    WriteText(modelPath, text);

    ResetStartupAssetPreimportFullIndexRebuildCountForTesting();
    ResetStartupAssetPreimportPatchedIndexWriteCountForTesting();

    const auto secondResult = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_FALSE(secondResult.usedCache);
    EXPECT_EQ(secondResult.plannedAssetCount, 0u);
    EXPECT_EQ(secondResult.importedAssetCount, 0u);
    EXPECT_EQ(GetStartupAssetPreimportFullIndexRebuildCountForTesting(), 0u);
    EXPECT_EQ(GetStartupAssetPreimportPatchedIndexWriteCountForTesting(), 1u);

    const auto thirdResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(thirdResult.succeeded) << JoinDiagnosticSummaries(thirdResult.diagnostics);
    EXPECT_TRUE(thirdResult.usedCache);

    std::filesystem::remove_all(root);
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to count startup index patching.";
#endif
}

TEST(EditorAssetDatabaseTests, StartupPreimportPatchesChangedSourceIndexWhenPreviousContentHashIsEmpty)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    const auto modelPath = root / "Assets" / "Models" / "StampOnlyStartupHeroMissingHash.gltf";
    WriteText(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "StampOnlyStartupHeroMissingHashRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);
    ASSERT_EQ(firstResult.plannedAssetCount, 1u);
    ASSERT_EQ(firstResult.importedAssetCount, 1u);

    const auto stampPath = root / "Library" / "Editor" / "StartupAssetPreimport.stamp";
    ClearStartupPreimportSourceContentHashForTest(
        stampPath,
        "Assets",
        "Models/StampOnlyStartupHeroMissingHash.gltf");

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto text = ReadText(modelPath);
    WriteText(modelPath, text);

    ResetStartupAssetPreimportFullIndexRebuildCountForTesting();
    ResetStartupAssetPreimportPatchedIndexWriteCountForTesting();

    const auto secondResult = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_FALSE(secondResult.usedCache);
    EXPECT_EQ(secondResult.plannedAssetCount, 0u);
    EXPECT_EQ(secondResult.importedAssetCount, 0u);
    EXPECT_EQ(GetStartupAssetPreimportFullIndexRebuildCountForTesting(), 0u);
    EXPECT_EQ(GetStartupAssetPreimportPatchedIndexWriteCountForTesting(), 1u);

    const auto thirdResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(thirdResult.succeeded) << JoinDiagnosticSummaries(thirdResult.diagnostics);
    EXPECT_TRUE(thirdResult.usedCache);

    std::filesystem::remove_all(root);
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to count startup index patching.";
#endif
}

NLS_LONG_RUNNING_TEST(EditorAssetDatabaseIntegrationPerformanceTests, StartupPreimportPatchesBuiltInShaderSourceWhenProjectPrefixAlsoMatches)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    std::filesystem::create_directories(root / "Assets");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);
    ASSERT_EQ(firstResult.plannedAssetCount, 0u);
    ASSERT_EQ(firstResult.importedAssetCount, 0u);

    const auto stampPath = root / "Library" / "Editor" / "StartupAssetPreimport.stamp";
    ClearStartupPreimportSourceContentHashForTest(
        stampPath,
        "Assets/Engine/Shaders/ShaderLab",
        "StandardPBR.shader");

    ResetStartupAssetPreimportFullIndexRebuildCountForTesting();
    ResetStartupAssetPreimportPatchedIndexWriteCountForTesting();

    const auto secondResult = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_FALSE(secondResult.usedCache);
    EXPECT_EQ(secondResult.plannedAssetCount, 0u);
    EXPECT_EQ(secondResult.importedAssetCount, 0u);
    EXPECT_EQ(GetStartupAssetPreimportFullIndexRebuildCountForTesting(), 0u);
    EXPECT_EQ(GetStartupAssetPreimportPatchedIndexWriteCountForTesting(), 1u);

    std::filesystem::remove_all(root);
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to count startup index patching.";
#endif
}

TEST(EditorAssetDatabaseTests, StartupPreimportCacheIndexRecordsManifestFreshnessKey)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "ManifestFreshnessKeyHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "ManifestFreshnessKeyHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto result = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(result.succeeded) << JoinDiagnosticSummaries(result.diagnostics);
    ASSERT_FALSE(result.usedCache);

    const auto indexText = ReadText(root / "Library" / "Editor" / "StartupAssetPreimport.stamp");
    EXPECT_NE(indexText.find("startup-manifest-freshness-v1"), std::string::npos);
    EXPECT_NE(indexText.find("postprocessor:external-texture-build-pipeline"), std::string::npos);
    EXPECT_NE(indexText.find("postprocessor:shader-compiler-toolchain"), std::string::npos);
    const auto textureType = static_cast<uint32_t>(NLS::Core::Assets::AssetType::Texture);
    EXPECT_NE(indexText.find("build-target:" + std::to_string(textureType) + "=win64-dx12"), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, StartupPreimportCacheIgnoresBuiltInShaderTimestampOnlyChanges)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(root / "Assets" / "Models" / "ColdHero.gltf", R"({"asset":{"version":"2.0"}})");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);

    const auto standardPbrPath =
        std::filesystem::path(NLS_ROOT_DIR) /
        "App" /
        "Assets" /
        "Engine" /
        "Shaders" /
        "ShaderLab" /
        "StandardPBR.shader";
    ASSERT_TRUE(std::filesystem::is_regular_file(standardPbrPath));
    std::error_code error;
    const auto originalWriteTime = std::filesystem::last_write_time(standardPbrPath, error);
    ASSERT_FALSE(error);
    std::filesystem::last_write_time(
        standardPbrPath,
        originalWriteTime + std::chrono::seconds(1),
        error);
    ASSERT_FALSE(error);

    const auto secondResult = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_TRUE(secondResult.usedCache)
        << "Built-in shader timestamp-only changes are build noise and should not force project startup to "
           "replan assets.";
    EXPECT_EQ(secondResult.plannedAssetCount, 0u);
    EXPECT_EQ(secondResult.importedAssetCount, 0u);

    std::filesystem::last_write_time(standardPbrPath, originalWriteTime, error);
    ASSERT_FALSE(error);

    std::filesystem::remove_all(root);
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to verify startup cache behavior.";
#endif
}

NLS_LONG_RUNNING_TEST(EditorAssetDatabaseIntegrationPerformanceTests, StartupPreimportCacheMissReportsManifestFreshnessReason)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "MissReasonHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "MissReasonHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);

    const auto indexPath = root / "Library" / "Editor" / "StartupAssetPreimport.stamp";
    auto indexText = ReadText(indexPath);
    const auto importersLine = indexText.find("importers ");
    ASSERT_NE(importersLine, std::string::npos);
    const auto importersLineEnd = indexText.find('\n', importersLine);
    ASSERT_NE(importersLineEnd, std::string::npos);
    indexText.replace(importersLine, importersLineEnd - importersLine, "importers \"stale-manifest-freshness\"");
    const auto staleImportersLineEnd = indexText.find('\n', importersLine);
    ASSERT_NE(staleImportersLineEnd, std::string::npos);
    indexText.replace(staleImportersLineEnd + 1u, std::string::npos, "sourceCount not-a-number\n");
    WriteText(indexPath, indexText);

    const auto secondResult = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_FALSE(secondResult.usedCache);
    EXPECT_EQ(secondResult.cacheValidationProfile.missReason, "manifest-freshness-mismatch");

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, StartupPreimportManifestFreshnessMismatchTargetsAffectedAssetTypesOnly)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "ToolchainOnlyHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "ToolchainOnlyHeroRoot" }]
        })");
    WriteBinary(root / "Assets" / "Textures" / "ToolchainOnlyAlbedo.png", TinyPng());
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);

    const auto indexPath = root / "Library" / "Editor" / "StartupAssetPreimport.stamp";
    auto indexText = ReadText(indexPath);
    const auto toolchainKey = std::string("postprocessor:shader-compiler-toolchain=");
    const auto toolchainOffset = indexText.find(toolchainKey);
    ASSERT_NE(toolchainOffset, std::string::npos);
    const auto toolchainValueBegin = toolchainOffset + toolchainKey.size();
    const auto toolchainValueEnd = indexText.find(';', toolchainValueBegin);
    ASSERT_NE(toolchainValueEnd, std::string::npos);
    indexText.replace(toolchainValueBegin, toolchainValueEnd - toolchainValueBegin, "stale-toolchain");
    WriteText(indexPath, indexText);

    ResetAssetDatabaseFullSourceRefreshCountForTesting();
    const auto secondResult = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_FALSE(secondResult.usedCache);
    EXPECT_EQ(secondResult.cacheValidationProfile.missReason, "manifest-freshness-mismatch");
    EXPECT_EQ(secondResult.plannedAssetCount, 0u)
        << "Shader toolchain freshness changes should not force project model/texture startup assets to replan.";
    EXPECT_EQ(secondResult.importedAssetCount, 0u);
    EXPECT_EQ(GetAssetDatabaseFullSourceRefreshCountForTesting(), 0u)
        << "Manifest-freshness targeted startup misses should stay on the known-source refresh path.";
    EXPECT_EQ(GetAssetDatabaseLastKnownSourceRefreshAssetCountForTesting(), 1u)
        << "Shader toolchain freshness should refresh only the affected shader source, not every cached startup source.";

    std::filesystem::remove_all(root);
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to count targeted refresh behavior.";
#endif
}

TEST(EditorAssetDatabaseTests, StartupPreimportArtifactDatabaseStampMismatchRebuildsWarmIndex)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "ArtifactStampHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "ArtifactStampHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);

    const auto indexPath = root / "Library" / "Editor" / "StartupAssetPreimport.stamp";
    auto indexText = ReadText(indexPath);
    const auto artifactDbLine = indexText.find("artifactDb ");
    ASSERT_NE(artifactDbLine, std::string::npos);
    const auto artifactDbLineEnd = indexText.find('\n', artifactDbLine);
    ASSERT_NE(artifactDbLineEnd, std::string::npos);
    indexText.replace(artifactDbLine, artifactDbLineEnd - artifactDbLine, "artifactDb \"stale-artifact-db\"");
    WriteText(indexPath, indexText);

    ResetAssetDatabaseFullSourceRefreshCountForTesting();
    ResetStartupAssetPreimportPatchedIndexWriteCountForTesting();
    const auto secondResult = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_FALSE(secondResult.usedCache);
    EXPECT_EQ(secondResult.cacheValidationProfile.missReason, "artifact-database-stamp-mismatch");
    EXPECT_EQ(secondResult.plannedAssetCount, 0u);
    EXPECT_EQ(secondResult.importedAssetCount, 0u);
    EXPECT_EQ(GetAssetDatabaseFullSourceRefreshCountForTesting(), 1u);
    EXPECT_EQ(GetStartupAssetPreimportPatchedIndexWriteCountForTesting(), 0u);

    std::filesystem::remove_all(root);
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to verify startup cache index patching.";
#endif
}

TEST(EditorAssetDatabaseTests, StartupPreimportDirectoryChangeFallsBackToSourceEnumeration)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "DirectoryStampStableHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "DirectoryStampStableHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);
    ASSERT_EQ(firstResult.plannedAssetCount, 1u);
    ASSERT_EQ(firstResult.importedAssetCount, 1u);

    std::error_code error;
    const auto modelsDirectory = root / "Assets" / "Models";
    const auto originalDirectoryWriteTime = std::filesystem::last_write_time(modelsDirectory, error);
    ASSERT_FALSE(error);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    WriteText(
        modelsDirectory / "DirectoryStampAddedHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "DirectoryStampAddedHeroRoot" }]
        })");
    std::filesystem::last_write_time(modelsDirectory, originalDirectoryWriteTime, error);
    ASSERT_FALSE(error);

    ResetStartupAssetPreimportSourceEnumerationCountForTesting();
    const auto secondResult = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_FALSE(secondResult.usedCache);
    EXPECT_EQ(secondResult.plannedAssetCount, 1u);
    EXPECT_EQ(secondResult.importedAssetCount, 1u);
    EXPECT_GT(GetStartupAssetPreimportSourceEnumerationCountForTesting(), 0u);

    std::filesystem::remove_all(root);
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to count startup source enumeration.";
#endif
}

TEST(EditorAssetDatabaseTests, BlockingStartupPreimportCacheIgnoresNonStartupAssetChanges)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "CacheIgnoresTextHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "CacheIgnoresTextHeroRoot" }]
        })");
    WriteText(root / "Assets" / "Docs" / "notes.txt", "startup cache should not track this file\n");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);
    EXPECT_EQ(firstResult.plannedAssetCount, 1u);
    EXPECT_EQ(firstResult.importedAssetCount, 1u);

    WriteText(root / "Assets" / "Docs" / "notes.txt", "changed non-startup source\n");

    std::vector<std::string> progressMessages;
    const auto secondResult = RunBlockingStartupAssetPreimport(
        options,
        [&progressMessages](const ImportProgressEvent& event)
        {
            progressMessages.push_back(event.message);
        });

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_TRUE(secondResult.usedCache);
    EXPECT_EQ(secondResult.plannedAssetCount, 0u);
    EXPECT_EQ(secondResult.importedAssetCount, 0u);
    EXPECT_EQ(
        std::find(progressMessages.begin(), progressMessages.end(), "Scanning project assets"),
        progressMessages.end());
    EXPECT_EQ(
        std::find(progressMessages.begin(), progressMessages.end(), "Planning startup asset imports"),
        progressMessages.end());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, BlockingStartupPreimportInvalidatesCacheWhenAssetChanges)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    const auto modelPath = root / "Assets" / "Models" / "InvalidatedStartupHero.gltf";
    WriteText(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "InvalidatedStartupHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    WriteText(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "InvalidatedStartupHeroRootChanged" }]
        })");

    const auto secondResult = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_FALSE(secondResult.usedCache);
    EXPECT_EQ(secondResult.plannedAssetCount, 1u);
    EXPECT_EQ(secondResult.importedAssetCount, 1u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, BlockingStartupPreimportInvalidatesCacheWhenMetaChanges)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    const auto modelPath = root / "Assets" / "Models" / "MetaInvalidatedStartupHero.gltf";
    WriteText(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "MetaInvalidatedStartupHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    WriteText(modelPath.string() + ".meta", "guid=11111111111111111111111111111111\nimporter=scene-model\nversion=9\n");

    const auto secondResult = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_FALSE(secondResult.usedCache);
    EXPECT_EQ(secondResult.plannedAssetCount, 1u);
    EXPECT_EQ(secondResult.importedAssetCount, 1u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, BlockingStartupPreimportInvalidatesCacheWhenArtifactPayloadChanges)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "MissingPayloadStartupHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "MissingPayloadStartupHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    const auto artifactPath = database.ResolveArtifactPathAtPath(
        "Assets/Models/MissingPayloadStartupHero.gltf",
        "prefab:MissingPayloadStartupHero");
    ASSERT_FALSE(artifactPath.empty());
    ASSERT_TRUE(std::filesystem::remove(artifactPath));

    const auto secondResult = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_FALSE(secondResult.usedCache);
    EXPECT_EQ(secondResult.plannedAssetCount, 1u);
    EXPECT_EQ(secondResult.importedAssetCount, 1u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, BlockingStartupPreimportInvalidatesCacheWhenArtifactPayloadContentChanges)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "CorruptPayloadStartupHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "CorruptPayloadStartupHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    const auto artifactPath = database.ResolveArtifactPathAtPath(
        "Assets/Models/CorruptPayloadStartupHero.gltf",
        "prefab:CorruptPayloadStartupHero");
    ASSERT_FALSE(artifactPath.empty());
    std::error_code error;
    const auto originalWriteTime = std::filesystem::last_write_time(artifactPath, error);
    ASSERT_FALSE(error);
    const auto originalSize = std::filesystem::file_size(artifactPath, error);
    ASSERT_FALSE(error);
    WriteText(artifactPath, std::string(static_cast<size_t>(originalSize), 'x'));
    std::filesystem::last_write_time(artifactPath, originalWriteTime, error);
    ASSERT_FALSE(error);

    const auto secondResult = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_FALSE(secondResult.usedCache);
    EXPECT_EQ(secondResult.plannedAssetCount, 1u);
    EXPECT_EQ(secondResult.importedAssetCount, 1u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, BlockingStartupPreimportInvalidatesTruncatedCacheIndex)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "TruncatedIndexStartupHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "TruncatedIndexStartupHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);

    WriteText(
        root / "Library" / "Editor" / "StartupAssetPreimport.stamp",
        "version \"3\"\n"
        "projectRoot \"truncated\"\n"
        "importers \"truncated\"\n"
        "artifactDb \"truncated\"\n");

    const auto secondResult = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_FALSE(secondResult.usedCache);
    EXPECT_EQ(secondResult.plannedAssetCount, 0u);
    EXPECT_EQ(secondResult.importedAssetCount, 0u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, BlockingStartupPreimportRejectsEscapingArtifactPathInCacheIndex)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "EscapingIndexStartupHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "EscapingIndexStartupHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);

    const auto indexPath = root / "Library" / "Editor" / "StartupAssetPreimport.stamp";
    auto indexText = ReadText(indexPath);
    const auto artifactLine = indexText.find("artifact ");
    ASSERT_NE(artifactLine, std::string::npos);
    const auto artifactLineEnd = indexText.find('\n', artifactLine);
    ASSERT_NE(artifactLineEnd, std::string::npos);
    const auto originalArtifactLine = indexText.substr(artifactLine, artifactLineEnd - artifactLine);

    std::istringstream artifactStream(originalArtifactLine);
    std::string key;
    std::string artifactPath;
    std::string stamp;
    std::string contentHash;
    std::string fingerprint;
    artifactStream >> key
        >> std::quoted(artifactPath)
        >> std::quoted(stamp)
        >> std::quoted(contentHash)
        >> std::quoted(fingerprint);
    ASSERT_FALSE(artifactStream.fail());

    const auto escapedPath = (root / "Library" / "Artifacts" / ".." / "EscapedStartupPayload").lexically_normal();
    std::ostringstream tamperedLineStream;
    tamperedLineStream << "artifact "
        << std::quoted(escapedPath.generic_string()) << ' '
        << std::quoted(stamp) << ' '
        << std::quoted(contentHash) << ' '
        << std::quoted(fingerprint);
    const auto tamperedLine = tamperedLineStream.str();
    indexText.replace(artifactLine, artifactLineEnd - artifactLine, tamperedLine);
    WriteText(indexPath, indexText);

    const auto secondResult = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_FALSE(secondResult.usedCache);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, BlockingStartupPreimportFallsBackToFullPlanWhenArtifactPayloadChangesDuringSourceChange)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "ChangedSourceStartupHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "ChangedSourceStartupHeroRoot" }]
        })");
    WriteText(
        root / "Assets" / "Models" / "MissingPayloadDuringSourceChangeHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "MissingPayloadDuringSourceChangeHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);
    ASSERT_EQ(firstResult.plannedAssetCount, 2u);
    ASSERT_EQ(firstResult.importedAssetCount, 2u);

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    const auto artifactPath = database.ResolveArtifactPathAtPath(
        "Assets/Models/MissingPayloadDuringSourceChangeHero.gltf",
        "prefab:MissingPayloadDuringSourceChangeHero");
    ASSERT_FALSE(artifactPath.empty());
    ASSERT_TRUE(std::filesystem::remove(artifactPath));

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    WriteText(
        root / "Assets" / "Models" / "ChangedSourceStartupHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "ChangedSourceStartupHeroRootUpdated" }]
        })");

    const auto secondResult = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_FALSE(secondResult.usedCache);
    EXPECT_EQ(secondResult.plannedAssetCount, 2u);
    EXPECT_EQ(secondResult.importedAssetCount, 2u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, BlockingStartupPreimportInvalidatesCacheWhenExternalModelDependencyChanges)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(root / "Assets" / "Models" / "ExternalDependencyHero.bin", std::string(64, '\0'));
    WriteText(
        root / "Assets" / "Models" / "ExternalDependencyHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "buffers": [{ "uri": "ExternalDependencyHero.bin", "byteLength": 64 }],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "ExternalDependencyHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);
    ASSERT_EQ(firstResult.plannedAssetCount, 1u);
    ASSERT_EQ(firstResult.importedAssetCount, 1u);

    WriteText(root / "Assets" / "Models" / "ExternalDependencyHero.bin", std::string(64, '\1'));

    const auto secondResult = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_FALSE(secondResult.usedCache);
    EXPECT_EQ(secondResult.plannedAssetCount, 1u);
    EXPECT_EQ(secondResult.importedAssetCount, 1u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, BlockingStartupPreimportInvalidatesCacheWhenManifestImporterVersionChanges)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    const auto assetPath = std::string("Assets/Models/ManifestVersionStartupHero.gltf");
    WriteText(
        root / assetPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "ManifestVersionStartupHeroRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);
    ASSERT_EQ(firstResult.plannedAssetCount, 1u);
    ASSERT_EQ(firstResult.importedAssetCount, 1u);

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    const auto manifest = database.GetArtifactManifestForAssetPath(assetPath);
    ASSERT_TRUE(manifest.has_value());
    auto staleManifest = *manifest;
    ++staleManifest.importerVersion;
    for (auto& dependency : staleManifest.dependencies)
    {
        if (dependency.kind == NLS::Core::Assets::AssetDependencyKind::ImporterVersion &&
            dependency.value == staleManifest.importerId)
        {
            dependency.hashOrVersion = std::to_string(staleManifest.importerVersion);
        }
    }
    SaveArtifactManifestForTest(root, staleManifest, assetPath);

    const auto secondResult = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_FALSE(secondResult.usedCache);
    EXPECT_EQ(secondResult.plannedAssetCount, 1u);
    EXPECT_EQ(secondResult.importedAssetCount, 1u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, BlockingStartupPreimportInvalidatesCacheWhenSameSizeAssetContentChanges)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    const auto modelPath = root / "Assets" / "Models" / "ContentHashStartupHero.gltf";
    WriteText(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "ContentHashStartupHeroA" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    StartupAssetPreimportOptions options;
    options.projectRoot = root;
    options.maxPreparedPrefabCachePreflightCount = 0u;

    const auto firstResult = RunBlockingStartupAssetPreimport(options);
    ASSERT_TRUE(firstResult.succeeded) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_FALSE(firstResult.usedCache);

    std::error_code error;
    const auto originalWriteTime = std::filesystem::last_write_time(modelPath, error);
    ASSERT_FALSE(error);
    WriteText(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "ContentHashStartupHeroB" }]
        })");
    std::filesystem::last_write_time(modelPath, originalWriteTime, error);
    ASSERT_FALSE(error);

    const auto secondResult = RunBlockingStartupAssetPreimport(options);

    ASSERT_TRUE(secondResult.succeeded) << JoinDiagnosticSummaries(secondResult.diagnostics);
    EXPECT_FALSE(secondResult.usedCache);
    EXPECT_EQ(secondResult.plannedAssetCount, 1u);
    EXPECT_EQ(secondResult.importedAssetCount, 1u);

    std::filesystem::remove_all(root);
}

NLS_LONG_RUNNING_TEST(EditorAssetDatabaseIntegrationPerformanceTests, BlockingStartupPreimportReimportsLegacyModelMaterialsToTextureArtifacts)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    const auto modelPath = root / "Assets" / "Models" / "LegacyTextureHero.gltf";
    WriteText(root / "Assets" / "Models" / "Hero.bin", "mesh-binary");
    WriteBinary(root / "Assets" / "Textures" / "HeroBaseColor.png", TinyPng());
    WriteText(
        modelPath,
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
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "LegacyTextureHeroRoot", "mesh": 0 }],
            "meshes": [{ "primitives": [{ "attributes": {}, "material": 0 }] }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    {
        AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
        ASSERT_TRUE(database.Refresh());
        ASSERT_TRUE(database.ImportAsset("Assets/Models/LegacyTextureHero.gltf"));
    }

    const auto metaPath = modelPath.string() + ".meta";
    auto metaText = ReadText(metaPath);
    auto version = metaText.find("IMPORTER_VERSION=");
    ASSERT_NE(version, std::string::npos);
    auto valueBegin = version + std::string("IMPORTER_VERSION=").size();
    auto valueEnd = metaText.find('\n', valueBegin);
    metaText.replace(valueBegin, valueEnd - valueBegin, "2");
    WriteText(metaPath, metaText);

    {
        AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
        ASSERT_TRUE(database.Refresh());
        const auto manifest = database.GetArtifactManifestForAssetPath("Assets/Models/LegacyTextureHero.gltf");
        ASSERT_TRUE(manifest.has_value());
        const auto* material = manifest->FindSubAsset("material:material/0");
        ASSERT_NE(material, nullptr);
        WriteNativeMaterialSource(
            material->artifactPath,
            "material/0",
            "shaderLabMaterialVersion=1\n"
            "shader=?\n"
            "property _BaseMap Texture2D Models/../Textures/HeroBaseColor.png\n");
    }

    const auto result = RunBlockingStartupAssetPreimport({root});
    ASSERT_TRUE(result.succeeded);
    EXPECT_EQ(result.plannedAssetCount, 2u);
    EXPECT_EQ(result.importedAssetCount, 2u);

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/LegacyTextureHero.gltf"));

    const auto manifest = database.GetArtifactManifestForAssetPath("Assets/Models/LegacyTextureHero.gltf");
    ASSERT_TRUE(manifest.has_value());
    const auto* material = manifest->FindSubAsset("material:material/0");
    ASSERT_NE(material, nullptr);
    const auto materialPath = database.ResolveArtifactPathAtPath(
        "Assets/Models/LegacyTextureHero.gltf",
        material->subAssetKey);
    ASSERT_FALSE(materialPath.empty());
    const auto materialPayload = ReadArtifactPayloadText(
        materialPath,
        NLS::Core::Assets::ArtifactType::Material,
        1u);
    EXPECT_NE(materialPayload.find("Library/Artifacts/"), std::string::npos);
    EXPECT_EQ(materialPayload.find("Textures/HeroBaseColor"), std::string::npos);
    EXPECT_EQ(materialPayload.find("Textures/HeroBaseColor.png"), std::string::npos);

    const auto* texture = manifest->FindSubAsset("texture:image/0");
    ASSERT_NE(texture, nullptr);
    const auto texturePath = database.ResolveArtifactPathAtPath(
        "Assets/Models/LegacyTextureHero.gltf",
        texture->subAssetKey);
    ASSERT_FALSE(texturePath.empty());
    const auto texturePayload = ReadBinary(texturePath);
    EXPECT_TRUE(NLS::Render::Assets::IsNativeTextureArtifact(texturePayload));
    const auto nativeTexture = NLS::Render::Assets::DeserializeTextureArtifact(texturePayload);
    ASSERT_TRUE(nativeTexture.has_value());
#if defined(_WIN32) && NLS_HAS_DIRECTXTEX
    EXPECT_EQ(nativeTexture->format, NLS::Render::RHI::TextureFormat::BC1);
#else
    EXPECT_EQ(nativeTexture->format, NLS::Render::RHI::TextureFormat::RGBA8);
#endif

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, DatabaseArtifactPathAcceptsOnlyExtensionlessContentBlobNameInsideOwnerArtifactRoot)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "ExtensionlessArtifactHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "ExtensionlessArtifactHeroRoot" }]
        })");

    {
        AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
        ASSERT_TRUE(database.Refresh());
        ASSERT_TRUE(database.ImportAsset("Assets/Models/ExtensionlessArtifactHero.gltf"));

        const auto artifactRoot = database.GetArtifactRootForAssetPathForTesting(
            "Assets/Models/ExtensionlessArtifactHero.gltf");
        ASSERT_FALSE(artifactRoot.empty());
        const auto artifactPath = database.ResolveArtifactPathAtPath(
            "Assets/Models/ExtensionlessArtifactHero.gltf",
            "prefab:ExtensionlessArtifactHero");
        ASSERT_FALSE(artifactPath.empty());
        ASSERT_EQ(artifactPath.parent_path().parent_path(), artifactRoot);
        ASSERT_EQ(artifactPath.parent_path().filename().generic_string(), artifactPath.filename().generic_string().substr(0u, 2u));
        ASSERT_FALSE(artifactPath.filename().has_extension());
        ASSERT_TRUE(std::filesystem::is_regular_file(artifactPath));

        const auto manifest = database.GetArtifactManifestForAssetPath("Assets/Models/ExtensionlessArtifactHero.gltf");
        ASSERT_TRUE(manifest.has_value());
        const auto* prefab = manifest->FindSubAsset("prefab:ExtensionlessArtifactHero");
        ASSERT_NE(prefab, nullptr);
        EXPECT_EQ(prefab->artifactPath.find("Library/Artifacts/"), 0u);
        EXPECT_FALSE(std::filesystem::path(prefab->artifactPath).filename().has_extension());
    }

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, BlockingStartupPreimportDoesNotPrewarmRuntimeMeshOrMaterialResources)
{
    using namespace NLS::Editor::Assets;

    EnsureEditorAssetDatabaseTestDriver();

    const auto root = MakeEditorAssetTestRoot();
    WriteText(
        root / "Assets" / "Models" / "WarmHero.gltf",
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
                { "name": "Stone" }
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
                { "name": "WarmHeroRoot", "mesh": 0 }
            ]
        })");

    const auto projectAssetsRoot = (root / "Assets").string() + "/";
    NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths(
        projectAssetsRoot,
        "App/Assets/Engine/");
    NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths(
        projectAssetsRoot,
        "App/Assets/Engine/");
    const ScopedShaderManagerAssetPaths shaderAssetPaths(
        projectAssetsRoot,
        "App/Assets/Engine/");
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths(
        projectAssetsRoot,
        "App/Assets/Engine/");
    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    ScopedServiceOverride<NLS::Core::ResourceManagement::MeshManager> meshManagerScope(meshManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::MaterialManager> materialManagerScope(materialManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::ShaderManager> shaderManagerScope(shaderManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::TextureManager> textureManagerScope(textureManager);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto result = RunBlockingStartupAssetPreimport({root});
    const auto startupRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();

    EXPECT_TRUE(result.succeeded);
    EXPECT_EQ(result.plannedAssetCount, 1u);
    EXPECT_EQ(result.importedAssetCount, 1u);
    EXPECT_EQ(result.preparedPrefabCachePreflightAttemptCount, 0u)
        << "When Library/PreparedPrefabCache does not exist, startup preflight has no L2 entries to load "
           "and should not enumerate model prefabs just to discover cold misses.";
    EXPECT_EQ(result.preparedPrefabCachePreflightCount, 0u);
    EXPECT_EQ(result.prewarmedMaterialArtifactCount, 0u);
    EXPECT_EQ(
        CountArtifactTelemetryStage(
            startupRecords,
            NLS::Core::Assets::ArtifactLoadTelemetryStage::DependencyScan),
        0u);
    EXPECT_TRUE(meshManager.GetResources().empty());
    EXPECT_TRUE(materialManager.GetResources().empty());

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    auto manifest = database.GetArtifactManifestForAssetPath("Assets/Models/WarmHero.gltf");
    ASSERT_TRUE(manifest.has_value());
    const auto* prefab = manifest->FindPrimaryArtifact();
    ASSERT_NE(prefab, nullptr);
    ASSERT_EQ(prefab->artifactType, NLS::Core::Assets::ArtifactType::Prefab);
    ASSERT_FALSE(prefab->artifactPath.empty());

    const auto mesh = std::find_if(
        manifest->subAssets.begin(),
        manifest->subAssets.end(),
        [](const NLS::Core::Assets::ImportedArtifact& artifact)
        {
            return artifact.artifactType == NLS::Core::Assets::ArtifactType::Mesh;
        });
    ASSERT_NE(mesh, manifest->subAssets.end());
    EXPECT_FALSE(meshManager.IsResourceRegistered(std::filesystem::path(mesh->artifactPath)
        .lexically_relative(root)
        .generic_string()));

    const auto material = std::find_if(
        manifest->subAssets.begin(),
        manifest->subAssets.end(),
        [](const NLS::Core::Assets::ImportedArtifact& artifact)
        {
            return artifact.artifactType == NLS::Core::Assets::ArtifactType::Material;
        });
    ASSERT_NE(material, manifest->subAssets.end());
    EXPECT_FALSE(materialManager.IsResourceRegistered(std::filesystem::path(material->artifactPath)
        .lexically_relative(root)
        .generic_string()));

    EXPECT_FALSE(std::filesystem::exists(root / "Library" / "PreparedPrefabCache"))
        << "Startup preflight must be cache-only; cold prepared cache generation belongs to the "
           "normal scene/drag loading path so editor startup cannot block on .prefab graph parsing.";

    meshManager.UnloadResources();
    materialManager.UnloadResources();
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, LegacyProjectRootFacadeDoesNotImportLibraryArtifactsAsSourceAssets)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(root / "Library" / "Artifacts" / "cached" / "670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772", "cached-prefab");
    WriteText(root / "Library" / "Artifacts" / "cached" / "36eee85124b95361c55a48634e6956a87607d0b6a69bfd04ffcd04f145ffa8d7", "cached-mesh");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    EXPECT_FALSE(std::filesystem::exists(
        root / "Library" / "Artifacts" / "cached" /
            "670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772.meta"));
    EXPECT_FALSE(std::filesystem::exists(
        root / "Library" / "Artifacts" / "cached" / "36eee85124b95361c55a48634e6956a87607d0b6a69bfd04ffcd04f145ffa8d7.meta"));
    EXPECT_TRUE(database.FindAssets("name:prefab", {}).empty());
    EXPECT_TRUE(database.FindAssets("name:mesh", {}).empty());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, LegacyProjectRootFacadeOnlyGeneratesMetaBelowAssets)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(root / "TestProject.nullus", "name=TestProject\n");
    WriteText(root / "help.txt", "project notes");
    WriteText(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    EXPECT_FALSE(std::filesystem::exists(root / "TestProject.nullus.meta"));
    EXPECT_FALSE(std::filesystem::exists(root / "help.txt.meta"));
    EXPECT_TRUE(std::filesystem::exists(root / "Assets" / "Models" / "Hero.gltf.meta"));
    EXPECT_TRUE(database.AssetPathToGUID("TestProject.nullus").empty());
    EXPECT_TRUE(database.AssetPathToGUID("help.txt").empty());
    EXPECT_FALSE(database.AssetPathToGUID("Assets/Models/Hero.gltf").empty());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, ProjectRootWithAssetsChildDoesNotScanSiblingFoldersAsAssets)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"}})");
    WriteText(root / "Packages" / "Starter" / "Tree.obj", "o Tree");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    EXPECT_TRUE(std::filesystem::exists(root / "Assets" / "Models" / "Hero.gltf.meta"));
    EXPECT_FALSE(std::filesystem::exists(root / "Packages" / "Starter" / "Tree.obj.meta"));
    EXPECT_FALSE(database.AssetPathToGUID("Assets/Models/Hero.gltf").empty());
    EXPECT_TRUE(database.AssetPathToGUID("Packages/Starter/Tree.obj").empty());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, ReadOnlySnapshotSharesLargeObjectReferenceSnapshots)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "Heavy.gltf";
    WriteText(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "HeavyRoot" }]
        })");
    PrepareStandardPbrShaderLabDependency(root);

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/Heavy.gltf"));

    const auto assetId = AssetId(NLS::Guid::Parse(database.AssetPathToGUID("Assets/Models/Heavy.gltf")));
    ASSERT_TRUE(assetId.IsValid());

    auto sourceManifest = database.GetArtifactManifestForAssetPath("Assets/Models/Heavy.gltf");
    ASSERT_TRUE(sourceManifest.has_value());
    ASSERT_EQ(sourceManifest->sourceAssetId, assetId);

    constexpr size_t kLargeSnapshotSubAssetCount = 256u;
    const auto existingCount = sourceManifest->subAssets.size();
    sourceManifest->subAssets.reserve(kLargeSnapshotSubAssetCount);
    for (size_t index = existingCount; index < kLargeSnapshotSubAssetCount; ++index)
    {
        const auto subAssetKey = "mesh:Part" + std::to_string(index);
        const auto artifactPath = MakeEditorContentAddressedArtifactPath(
            "Heavy.mesh:" + assetId.ToString() + ":" + std::to_string(index));
        sourceManifest->subAssets.push_back(MakeEditorAssetDatabaseArtifact(
            assetId,
            subAssetKey,
            ArtifactType::Mesh,
            artifactPath));
        WriteText(root / artifactPath, subAssetKey);
    }
    database.AddArtifactManifest(std::move(*sourceManifest));
    ASSERT_TRUE(database.IsArtifactManifestKnownCurrentForAssetPath("Assets/Models/Heavy.gltf"));

    const auto* sourceStorage = database.GetObjectReferencePickerAssetSnapshotsStorageForTesting();
    const auto* sourceManifestStorage = database.GetArtifactManifestMapStorageForTesting();
    auto snapshot = AssetDatabaseFacade::CreateReadOnlySnapshot(database);
    ASSERT_NE(snapshot, nullptr);

    EXPECT_EQ(
        snapshot->GetObjectReferencePickerAssetSnapshotsStorageForTesting(),
        sourceStorage)
        << "AssetBrowser database-ready frames must not deep-copy huge model subasset snapshots.";
    EXPECT_EQ(
        snapshot->GetArtifactManifestMapStorageForTesting(),
        sourceManifestStorage)
        << "AssetBrowser database-ready frames must not deep-copy huge artifact manifests.";
    const auto snapshotManifest = snapshot->GetArtifactManifestForAssetPath("Assets/Models/Heavy.gltf");
    ASSERT_TRUE(snapshotManifest.has_value());
    size_t visited = 0u;
    snapshot->ForEachObjectReferencePickerAssetSnapshot(
        [&visited](const ObjectReferencePickerAssetSnapshot& item)
        {
            ++visited;
        });
    EXPECT_GE(visited, 1u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, EmptyFacadePublishesValidSharedState)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    std::filesystem::create_directories(root / "Assets");

    AssetDatabaseFacade database({root});
    const auto state = database.GetPublishedState();
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(database.GetPublishedState(), state);
    ASSERT_NE(state->artifactManifests, nullptr);
    ASSERT_NE(state->snapshotIndex, nullptr);
    EXPECT_EQ(state->snapshotIndex->status, EditorAssetSnapshotStatus::Valid);
    EXPECT_TRUE(state->snapshotIndex->diagnostic.Empty());
    EXPECT_TRUE(state->snapshotIndex->assets.empty());
    EXPECT_TRUE(state->snapshotIndex->assetIndexByCanonicalSourcePath.empty());

    const auto snapshot = AssetDatabaseFacade::CreateReadOnlySnapshot(database);
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->GetPublishedState(), state);

    WriteText(root / "Assets" / "Models" / "PublishedState.gltf", R"({"asset":{"version":"2.0"}})");
    ASSERT_TRUE(database.Refresh());
    const auto sourceAssetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(
        database.AssetPathToGUID("Assets/Models/PublishedState.gltf")));
    ASSERT_TRUE(sourceAssetId.IsValid());

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = sourceAssetId;
    database.AddArtifactManifest(manifest);
    const auto changedState = database.GetPublishedState();
    ASSERT_NE(changedState, nullptr);
    EXPECT_NE(changedState, state);
    EXPECT_TRUE(state->artifactManifests->empty());
    EXPECT_EQ(changedState->artifactManifests->count(manifest.sourceAssetId), 1u);
    EXPECT_EQ(snapshot->GetPublishedState(), state);

    std::filesystem::remove_all(root);
}
