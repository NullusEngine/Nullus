#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

#include <Assets/AssetId.h>
#include <Assets/AssetMeta.h>
#include <GameObject.h>
#include <SceneSystem/Scene.h>

#include "Assets/PrefabEditorWorkflow.h"
#include "Undo/AssetFileUndo.h"
#include "Undo/EditorUndoService.h"
#include "Undo/PrefabSaveUndoSupport.h"

namespace NLS::Editor::Undo::PrefabSave::Tests
{
namespace
{
class TemporaryAssetProject final
{
public:
    TemporaryAssetProject()
        : root(std::filesystem::temp_directory_path() /
            ("NullusPrefabSaveUndo_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        std::filesystem::create_directories(root / "Assets");
    }

    ~TemporaryAssetProject()
    {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    std::filesystem::path root;
};

class SavePrefabIdentityCommand final : public IEditorUndoCommand
{
public:
    SavePrefabIdentityCommand(
        NLS::Editor::Assets::PrefabInstanceRecord& instance,
        const NLS::Core::Assets::AssetId prefabAssetId,
        std::string prefabSubAssetKey,
        const NLS::Core::Assets::AssetId sceneAssetId,
        UndoCommandPtr fileCommand)
        : m_instance(instance)
        , m_prefabAssetId(prefabAssetId)
        , m_prefabSubAssetKey(std::move(prefabSubAssetKey))
        , m_sceneAssetId(sceneAssetId)
        , m_fileCommand(std::move(fileCommand))
    {
        if (m_fileCommand)
            m_info = m_fileCommand->GetInfo();
    }

    [[nodiscard]] UndoCommandInfo GetInfo() const override
    {
        return m_info;
    }

    [[nodiscard]] UndoValidationResult Validate(
        const UndoDirection direction,
        const UndoExecutionContext& context) const override
    {
        if (!m_fileCommand)
            return UndoValidationResult::Failed("Prefab asset command is incomplete.");
        if (direction == UndoDirection::Undo && !MatchesInstanceIdentity(
                m_instance,
                m_prefabAssetId,
                m_prefabSubAssetKey,
                m_sceneAssetId))
        {
            return UndoValidationResult::Conflict("Prefab connection changed outside the Undo timeline.");
        }
        return m_fileCommand->Validate(direction, context);
    }

    UndoStartResult Apply(
        const UndoDirection direction,
        const UndoExecutionContext& context,
        UndoCompletion completion) override
    {
        return m_fileCommand
            ? m_fileCommand->Apply(direction, context, std::move(completion))
            : UndoStartResult::Failed("Prefab asset command is incomplete.");
    }

    bool TryMergeFrom(const IEditorUndoCommand&) override
    {
        return false;
    }

private:
    UndoCommandInfo m_info;
    NLS::Editor::Assets::PrefabInstanceRecord& m_instance;
    NLS::Core::Assets::AssetId m_prefabAssetId;
    std::string m_prefabSubAssetKey;
    NLS::Core::Assets::AssetId m_sceneAssetId;
    UndoCommandPtr m_fileCommand;
};
} // namespace

TEST(PrefabSaveUndoTests, UndoRejectsAConnectionWithTheSameAssetButDifferentSubAsset)
{
    TemporaryAssetProject project;
    const auto prefabAssetId = NLS::Core::Assets::AssetId::New();
    const auto sceneAssetId = NLS::Core::Assets::AssetId::New();
    const auto prefabPath = project.root / "Assets" / "Saved.prefab";
    NLS::Editor::Assets::PrefabInstanceRecord instance;
    instance.prefabAssetId = prefabAssetId;
    instance.prefabSubAssetKey = "prefab:SavedRoot";
    instance.sceneAssetId = sceneAssetId;

    auto fileCommand = NLS::Editor::Undo::AssetFile::MakeCreateFileCommand(
        project.root,
        prefabPath,
        "prefab-data",
        prefabAssetId.ToString(),
        "Save As Prefab",
        prefabAssetId);
    ASSERT_NE(fileCommand, nullptr);

    EditorUndoService undo;
    ASSERT_TRUE(undo.Execute(std::make_unique<SavePrefabIdentityCommand>(
        instance,
        prefabAssetId,
        instance.prefabSubAssetKey,
        sceneAssetId,
        std::move(fileCommand))).Succeeded());
    ASSERT_TRUE(std::filesystem::exists(prefabPath));

    instance.prefabSubAssetKey = "prefab:ExternallyConnectedRoot";
    const auto result = undo.RequestUndo();

    EXPECT_EQ(result.state, UndoRequestState::Conflict);
    EXPECT_EQ(undo.GetCursor(), 1u);
    EXPECT_TRUE(std::filesystem::exists(prefabPath));
}

TEST(PrefabSaveUndoTests, ResolvesAStageOwnedObjectToTheStagePrefabAsset)
{
    NLS::Engine::SceneSystem::Scene currentScene;
    NLS::Editor::Assets::PrefabStageState stage;
    stage.prefabAssetId = NLS::Core::Assets::AssetId::New();
    stage.stageScene = std::make_unique<NLS::Engine::SceneSystem::Scene>();
    auto& root = stage.stageScene->CreateGameObject("Stage root");

    std::string error;
    const auto resolved = ResolveOwningSceneAssetId(
        root,
        &currentScene,
        {},
        &stage,
        error);

    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(*resolved, stage.prefabAssetId);
    EXPECT_TRUE(error.empty());
}

TEST(PrefabSaveUndoTests, ResolvesTheCurrentSceneUsingItsOwnMetaAsset)
{
    TemporaryAssetProject project;
    const auto scenePath = project.root / "Assets" / "Current.scene";
    {
        std::ofstream source(scenePath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(static_cast<bool>(source));
        source << "scene-data";
        ASSERT_TRUE(static_cast<bool>(source));
    }
    const auto sceneMeta = NLS::Core::Assets::AssetMeta::CreateForAsset(scenePath);
    ASSERT_TRUE(sceneMeta.Save(NLS::Core::Assets::GetAssetMetaPath(scenePath)));

    NLS::Engine::SceneSystem::Scene currentScene;
    auto& root = currentScene.CreateGameObject("Current scene root");
    std::string error;
    const auto resolved = ResolveOwningSceneAssetId(
        root,
        &currentScene,
        scenePath,
        nullptr,
        error);

    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(*resolved, sceneMeta.id);
    EXPECT_TRUE(error.empty());
}

TEST(PrefabSaveUndoTests, AllowsAnUnsavedCurrentSceneWithoutAnAssetId)
{
    NLS::Engine::SceneSystem::Scene currentScene;
    auto& root = currentScene.CreateGameObject("Unsaved scene root");

    std::string error;
    const auto resolved = ResolveOwningSceneAssetId(
        root,
        &currentScene,
        {},
        nullptr,
        error);

    ASSERT_TRUE(resolved.has_value());
    EXPECT_FALSE(resolved->IsValid());
    EXPECT_TRUE(error.empty());
}

TEST(PrefabSaveUndoTests, RejectsAnObjectFromAnUnrelatedScene)
{
    NLS::Engine::SceneSystem::Scene currentScene;
    NLS::Engine::SceneSystem::Scene unrelatedScene;
    auto& root = unrelatedScene.CreateGameObject("Other scene root");

    std::string error;
    const auto resolved = ResolveOwningSceneAssetId(
        root,
        &currentScene,
        {},
        nullptr,
        error);

    EXPECT_FALSE(resolved.has_value());
    EXPECT_FALSE(error.empty());
}
} // namespace NLS::Editor::Undo::PrefabSave::Tests
