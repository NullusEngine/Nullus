#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
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

namespace
{
using NLS::Tests::ScopedServiceOverride;

std::filesystem::path MakeEditorAssetTestRoot()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_editor_asset_database_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);
    return root;
}

void WriteText(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

void CopyDirectoryRecursive(
    const std::filesystem::path& source,
    const std::filesystem::path& destination)
{
    std::filesystem::create_directories(destination);
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source))
    {
        const auto relative = entry.path().lexically_relative(source);
        const auto target = destination / relative;
        if (entry.is_directory())
        {
            std::filesystem::create_directories(target);
            continue;
        }
        if (!entry.is_regular_file())
            continue;

        std::filesystem::create_directories(target.parent_path());
        std::filesystem::copy_file(
            entry.path(),
            target,
            std::filesystem::copy_options::overwrite_existing);
    }
}

void PrepareStandardPbrShaderLabDependency(const std::filesystem::path& root)
{
    const auto shaderRoot =
        std::filesystem::path(NLS_ROOT_DIR) /
        "App" /
        "Assets" /
        "Engine" /
        "Shaders";
    const auto shaderDestination =
        root /
        "Assets" /
        "Engine" /
        "Shaders" /
        "ShaderLab" /
        "StandardPBR.shader";
    std::filesystem::create_directories(shaderDestination.parent_path());
    std::filesystem::copy_file(
        shaderRoot / "ShaderLab" / "StandardPBR.shader",
        shaderDestination,
        std::filesystem::copy_options::overwrite_existing);
    CopyDirectoryRecursive(
        shaderRoot / "NullusShaderLibrary",
        root / "Assets" / "Engine" / "Shaders" / "NullusShaderLibrary");

    NLS::Editor::Assets::AssetDatabaseFacade database(
        NLS::Editor::Assets::MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Engine/Shaders/ShaderLab/StandardPBR.shader"));
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
    WriteText(root / "Assets" / "Textures" / "Ignored.png", "not-a-real-png");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    const auto plan = scheduler.BuildPlan(database);

    ASSERT_EQ(plan.assetPaths.size(), 2u);
    EXPECT_EQ(plan.assetPaths[0], "Assets/Models/ColdHero.gltf");
    EXPECT_EQ(plan.assetPaths[1], "Assets/Prefabs/ColdLamp.prefab");

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

    AssetDatabaseFacade database({root});
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

    {
        AssetDatabaseFacade database({root});
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

    AssetDatabaseFacade restartedDatabase({root});
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

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup));
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

    AssetDatabaseFacade planningDatabase({root});
    ASSERT_TRUE(planningDatabase.Refresh());

    AssetPreimportScheduler scheduler;
    const AssetPreimportRequest watcherRequest {
        AssetPreimportReason::FileWatcherChanged,
        {std::filesystem::path("Assets") / "Models" / "ConcurrentHero.gltf"}
    };
    const auto stalePlan = scheduler.BuildPlan(planningDatabase, watcherRequest);
    ASSERT_EQ(stalePlan.assetPaths, std::vector<std::string>({"Assets/Models/ConcurrentHero.gltf"}));

    AssetDatabaseFacade importingDatabase({root});
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

    AssetDatabaseFacade planningDatabase({root});
    ASSERT_TRUE(planningDatabase.Refresh());

    AssetPreimportScheduler scheduler;
    const AssetPreimportRequest watcherRequest {
        AssetPreimportReason::FileWatcherChanged,
        {std::filesystem::path("Assets") / "Models"}
    };
    const auto stalePlan = scheduler.BuildPlan(planningDatabase, watcherRequest);
    ASSERT_EQ(stalePlan.assetPaths.size(), 2u);

    AssetDatabaseFacade importingDatabase({root});
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
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup));
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

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup));
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

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup));
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

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup));
    ASSERT_EQ(database.GetCompletedImportCount(), 1u);

    WriteText(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "StartupChangedHeroRoot" }, { "name": "ChangedWhileClosed" }]
        })");

    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup));
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

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup));
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

    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup));
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

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup));
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

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup));
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

    AssetDatabaseFacade database({
        EditorAssetRoot {root / "Assets", false, "Assets", root / "Library"},
        EditorAssetRoot {packageRoot, false, "Packages/StarterContent"}
    });
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup));
    ASSERT_EQ(database.GetCompletedImportCount(), 1u);

    ASSERT_TRUE(scheduler.Run(database, tracker, {
        AssetPreimportReason::FileWatcherChanged,
        {modelPath}
    }));
    EXPECT_EQ(database.GetCompletedImportCount(), 1u);

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

    AssetDatabaseFacade database({
        EditorAssetRoot {projectRoot, false, {}},
        EditorAssetRoot {packageRoot, false, "Packages/StarterContent"}
    });
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup));
    ASSERT_EQ(database.GetCompletedImportCount(), 1u);

    ASSERT_TRUE(scheduler.Run(database, tracker, {
        AssetPreimportReason::FileWatcherChanged,
        {modelPath}
    }));
    EXPECT_EQ(database.GetCompletedImportCount(), 1u);

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

    AssetDatabaseFacade database({
        EditorAssetRoot {root / "Assets", false, "Assets", root / "Library"},
        EditorAssetRoot {packageRoot, false, "Packages/StarterContent"}
    });
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup));
    ASSERT_EQ(database.GetCompletedImportCount(), 1u);

    WriteText(texturePath, "texture-after");
    const auto plan = scheduler.BuildPlan(database, {
        AssetPreimportReason::FileWatcherChanged,
        {texturePath}
    });

    ASSERT_EQ(plan.assetPaths.size(), 1u);
    EXPECT_EQ(plan.assetPaths[0], "Packages/StarterContent/Models/MountedDependencyChangedHero.gltf");

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

    AssetDatabaseFacade database({
        EditorAssetRoot {root / "Assets", false, "Assets", root / "Library"},
        EditorAssetRoot {packageRoot, false, "Packages/StarterContent"},
        EditorAssetRoot {otherRoot, false, "Other"}
    });
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup));
    ASSERT_EQ(database.GetCompletedImportCount(), 1u);

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

    AssetDatabaseFacade database({
        EditorAssetRoot {root / "Assets", false, "Assets", root / "Library"},
        EditorAssetRoot {parentRoot, false, "Packages"},
        EditorAssetRoot {childRoot, false, "Packages/StarterContent"}
    });
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup));
    ASSERT_EQ(database.GetCompletedImportCount(), 1u);

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

    AssetDatabaseFacade database({root});
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

    AssetDatabaseFacade database({root});
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

    ASSERT_EQ(plan.assetPaths.size(), 1u);
    EXPECT_EQ(plan.assetPaths[0], "Assets/Models/Hero.gltf");

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

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    EXPECT_TRUE(database
        .LoadSubAssetAtPath("Assets/Models/StartupColdHero.gltf", "prefab:StartupColdHero")
        .has_value());
    EXPECT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/StartupColdHero.gltf"));
    EXPECT_TRUE(std::filesystem::is_regular_file(
        root / "Assets" / "Engine" / "Shaders" / "NullusShaderLibrary" / "Core.hlsl"));
    EXPECT_TRUE(database.IsArtifactManifestCurrentForAssetPath(
        "Assets/Engine/Shaders/ShaderLab/StandardPBR.shader"));

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDatabaseTests, BlockingStartupPreimportRefreshesStaleBundledShaderLabSources)
{
    using namespace NLS::Editor::Assets;

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
        "Shader \"Stale/StandardPBR\" {}\n");
    WriteText(
        root / "Assets" / "Engine" / "Shaders" / "NullusShaderLibrary" / "Core.hlsl",
        "// stale library\n");
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
        ReadText(bundledStandardPbr));
    EXPECT_EQ(
        ReadText(root / "Assets" / "Engine" / "Shaders" / "NullusShaderLibrary" / "Core.hlsl"),
        ReadText(bundledCore));

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

    AssetDatabaseFacade database({root});
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
    EXPECT_FALSE(database.AssetPathToGUID("Assets/Models/CreatedAfterPlan.gltf").empty());
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

    AssetDatabaseFacade database({root});
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
        EXPECT_EQ(plan.assetPaths[0], "Assets/Models/EscapingDatabaseHero.gltf");
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
    EXPECT_EQ(plan.assetPaths, std::vector<std::string>({"Assets/Models/ProjectRelativeArtifactHero.gltf"}));

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
        ASSERT_EQ(plan.assetPaths, std::vector<std::string>({"Assets/Model/ArtifactRootRelativeHero.gltf"}));
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

TEST(EditorAssetDatabaseTests, BlockingStartupPreimportReimportsLegacyModelMaterialsToTextureArtifacts)
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
        WriteText(
            material->artifactPath,
            "shaderLabMaterialVersion=1\n"
            "shader=?\n"
            "property _BaseMap Texture2D Models/../Textures/HeroBaseColor.png\n");
    }

    const auto result = RunBlockingStartupAssetPreimport({root});
    ASSERT_TRUE(result.succeeded);
    EXPECT_EQ(result.plannedAssetCount, 1u);
    EXPECT_EQ(result.importedAssetCount, 1u);

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
    EXPECT_EQ(result.preparedPrefabCachePreflightAttemptCount, 1u);
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

TEST(EditorAssetDatabaseTests, LegacyFilesystemRootWithAssetsChildKeepsScanningSiblings)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeEditorAssetTestRoot();
    WriteText(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"}})");
    WriteText(root / "Packages" / "Starter" / "Tree.obj", "o Tree");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    EXPECT_TRUE(std::filesystem::exists(root / "Assets" / "Models" / "Hero.gltf.meta"));
    EXPECT_TRUE(std::filesystem::exists(root / "Packages" / "Starter" / "Tree.obj.meta"));
    EXPECT_FALSE(database.AssetPathToGUID("Assets/Models/Hero.gltf").empty());
    EXPECT_FALSE(database.AssetPathToGUID("Packages/Starter/Tree.obj").empty());

    std::filesystem::remove_all(root);
}
