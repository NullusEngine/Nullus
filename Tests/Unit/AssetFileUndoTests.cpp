#include <gtest/gtest.h>

#include <Assets/AssetMeta.h>
#include "Undo/AssetUndoScope.h"
#include "Undo/AssetFileUndo.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace NLS::Editor::Undo::AssetFile::Tests
{
namespace
{
class TemporaryAssetProject final
{
public:
    TemporaryAssetProject()
        : root(std::filesystem::temp_directory_path() /
            ("NullusAssetFileUndo_" + std::to_string(
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

void WriteText(const std::filesystem::path& path, const std::string& value)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(static_cast<bool>(output));
    output << value;
    ASSERT_TRUE(static_cast<bool>(output));
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    EXPECT_TRUE(static_cast<bool>(input));
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
} // namespace

TEST(AssetFileUndoTests, CreateFileRoundTripsPayloadAndStableMeta)
{
    TemporaryAssetProject project;
    const auto destination = project.root / "Assets" / "NewScript.cs";

    EditorUndoService undo;
    auto command = MakeCreateFileCommand(
        project.root,
        destination,
        "public class NewScript {}\n",
        "Assets/NewScript.cs",
        "Create C# Script");
    ASSERT_NE(command, nullptr);
    ASSERT_TRUE(undo.Execute(std::move(command)).Succeeded());
    EXPECT_EQ(ReadText(destination), "public class NewScript {}\n");

    const auto createdMeta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(destination));
    ASSERT_TRUE(createdMeta.has_value());
    ASSERT_TRUE(createdMeta->id.IsValid());

    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_FALSE(std::filesystem::exists(destination));
    EXPECT_FALSE(std::filesystem::exists(NLS::Core::Assets::GetAssetMetaPath(destination)));

    ASSERT_EQ(undo.RequestRedo().state, UndoRequestState::Completed);
    EXPECT_EQ(ReadText(destination), "public class NewScript {}\n");
    const auto redoneMeta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(destination));
    ASSERT_TRUE(redoneMeta.has_value());
    EXPECT_EQ(redoneMeta->id, createdMeta->id);
}

TEST(AssetFileUndoTests, CreateFileHonorsExplicitAssetIdAcrossUndoRedo)
{
    TemporaryAssetProject project;
    const auto destination = project.root / "Assets" / "Prefab.prefab";
    const auto expectedAssetId = NLS::Core::Assets::AssetId::New();

    EditorUndoService undo;
    auto command = MakeCreateFileCommand(
        project.root,
        destination,
        "{\"format\":\"NullusObjectGraph\"}",
        expectedAssetId.ToString(),
        "Save As Prefab",
        expectedAssetId);
    ASSERT_NE(command, nullptr);
    ASSERT_TRUE(undo.Execute(std::move(command)).Succeeded());

    auto metadata = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(destination));
    ASSERT_TRUE(metadata.has_value());
    EXPECT_EQ(metadata->id, expectedAssetId);

    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    ASSERT_EQ(undo.RequestRedo().state, UndoRequestState::Completed);
    metadata = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(destination));
    ASSERT_TRUE(metadata.has_value());
    EXPECT_EQ(metadata->id, expectedAssetId);
}

TEST(AssetFileUndoTests, CreateFileUndoRejectsExternalModification)
{
    TemporaryAssetProject project;
    const auto destination = project.root / "Assets" / "NewScript.cs";

    EditorUndoService undo;
    auto command = MakeCreateFileCommand(project.root, destination, "created");
    ASSERT_NE(command, nullptr);
    ASSERT_TRUE(undo.Execute(std::move(command)).Succeeded());
    WriteText(destination, "outside-change");

    const auto result = undo.RequestUndo();
    EXPECT_EQ(result.state, UndoRequestState::Conflict);
    EXPECT_EQ(undo.GetCursor(), 1u);
    EXPECT_TRUE(std::filesystem::exists(destination));
}

TEST(AssetFileUndoTests, AssetCommandUsesTheWatcherScopeForSameAssetInvalidation)
{
    TemporaryAssetProject project;
    const auto destination = project.root / "Assets" / "Created.prefab";
    const auto assetId = NLS::Core::Assets::AssetId::New();

    EditorUndoService undo;
    auto command = MakeCreateFileCommand(
        project.root,
        destination,
        "prefab-data",
        assetId.ToString(),
        "Save As Prefab",
        assetId);
    ASSERT_NE(command, nullptr);

    const auto watcherScope = MakeAssetUndoScope("Assets/Created.prefab", assetId.ToString());
    const auto info = command->GetInfo();
    EXPECT_TRUE(std::find(info.scopes.begin(), info.scopes.end(), watcherScope) != info.scopes.end());
    ASSERT_TRUE(undo.Execute(std::move(command)).Succeeded());

    undo.InvalidateScope(watcherScope, UndoInvalidationReason::AssetReimported);
    EXPECT_EQ(undo.GetTransactionCount(), 0u);
    EXPECT_EQ(undo.GetCursor(), 0u);
    EXPECT_TRUE(std::filesystem::exists(destination));
}

TEST(AssetFileUndoTests, AssetScopeInvalidationRetainsUnrelatedAssetHistory)
{
    TemporaryAssetProject project;
    const auto firstDestination = project.root / "Assets" / "First.prefab";
    const auto secondDestination = project.root / "Assets" / "Second.prefab";
    const auto firstAssetId = NLS::Core::Assets::AssetId::New();
    const auto secondAssetId = NLS::Core::Assets::AssetId::New();

    EditorUndoService undo;
    auto first = MakeCreateFileCommand(
        project.root,
        firstDestination,
        "first",
        firstAssetId.ToString(),
        "Create First",
        firstAssetId);
    auto second = MakeCreateFileCommand(
        project.root,
        secondDestination,
        "second",
        secondAssetId.ToString(),
        "Create Second",
        secondAssetId);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_TRUE(undo.Execute(std::move(first)).Succeeded());
    ASSERT_TRUE(undo.Execute(std::move(second)).Succeeded());

    undo.InvalidateScope(
        MakeAssetUndoScope("Assets/First.prefab", firstAssetId.ToString()),
        UndoInvalidationReason::AssetReimported);

    EXPECT_EQ(undo.GetTransactionCount(), 1u);
    EXPECT_EQ(undo.GetCursor(), 1u);
    EXPECT_TRUE(std::filesystem::exists(firstDestination));
    EXPECT_TRUE(std::filesystem::exists(secondDestination));

    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_TRUE(std::filesystem::exists(firstDestination));
    EXPECT_FALSE(std::filesystem::exists(secondDestination));
}

TEST(AssetFileUndoTests, CreateDirectoryUndoRequiresItToRemainEmpty)
{
    TemporaryAssetProject project;
    const auto directory = project.root / "Assets" / "New Folder";

    EditorUndoService undo;
    auto command = MakeCreateDirectoryCommand(project.root, directory, "Assets/New Folder");
    ASSERT_NE(command, nullptr);
    ASSERT_TRUE(undo.Execute(std::move(command)).Succeeded());
    EXPECT_TRUE(std::filesystem::is_directory(directory));

    WriteText(directory / "external.txt", "outside-change");
    const auto blockedUndo = undo.RequestUndo();
    EXPECT_EQ(blockedUndo.state, UndoRequestState::Conflict);
    EXPECT_EQ(undo.GetCursor(), 1u);
    EXPECT_TRUE(std::filesystem::exists(directory / "external.txt"));
}

TEST(AssetFileUndoTests, DuplicateRoundTripsSourceAndMetaAsOneBundle)
{
    TemporaryAssetProject project;
    const auto source = project.root / "Assets" / "Sample.txt";
    const auto destination = project.root / "Assets" / "Sample 1.txt";
    WriteText(source, "source-data");

    auto sourceMeta = NLS::Core::Assets::AssetMeta::CreateForAsset(source);
    sourceMeta.settings["compression"] = "none";
    ASSERT_TRUE(sourceMeta.Save(NLS::Core::Assets::GetAssetMetaPath(source)));

    EditorUndoService undo;
    auto command = MakeDuplicateCommand(project.root, source, destination, sourceMeta.id.ToString());
    ASSERT_NE(command, nullptr);
    ASSERT_TRUE(undo.Execute(std::move(command)).Succeeded());
    ASSERT_TRUE(std::filesystem::exists(destination));
    ASSERT_TRUE(std::filesystem::exists(NLS::Core::Assets::GetAssetMetaPath(destination)));
    const UndoScopeId scope {
        UndoScopeKind::Asset,
        project.root.lexically_normal().generic_string(),
        0u};
    EXPECT_FALSE(undo.GetScopeState(scope).IsDirty());

    const auto duplicatedMeta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(destination));
    ASSERT_TRUE(duplicatedMeta.has_value());
    EXPECT_NE(duplicatedMeta->id, sourceMeta.id);
    EXPECT_EQ(duplicatedMeta->settings, sourceMeta.settings);

    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_FALSE(std::filesystem::exists(destination));
    EXPECT_FALSE(std::filesystem::exists(NLS::Core::Assets::GetAssetMetaPath(destination)));
    EXPECT_FALSE(undo.GetScopeState(scope).IsDirty());

    ASSERT_EQ(undo.RequestRedo().state, UndoRequestState::Completed);
    const auto redoneMeta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(destination));
    ASSERT_TRUE(redoneMeta.has_value());
    EXPECT_EQ(redoneMeta->id, duplicatedMeta->id);
    EXPECT_FALSE(undo.GetScopeState(scope).IsDirty());
}

TEST(AssetFileUndoTests, DuplicateCreatesStableMetaForAnExternalStyleSourceWithoutMeta)
{
    TemporaryAssetProject project;
    const auto source = project.root / "External.txt";
    const auto destination = project.root / "Assets" / "External.txt";
    WriteText(source, "external-data");

    EditorUndoService undo;
    auto command = MakeDuplicateCommand(project.root, source, destination, source.generic_string());
    ASSERT_NE(command, nullptr);
    ASSERT_TRUE(undo.Execute(std::move(command)).Succeeded());
    EXPECT_EQ(ReadText(destination), "external-data");

    const auto importedMeta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(destination));
    ASSERT_TRUE(importedMeta.has_value());
    ASSERT_TRUE(importedMeta->id.IsValid());

    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_FALSE(std::filesystem::exists(destination));
    EXPECT_FALSE(std::filesystem::exists(NLS::Core::Assets::GetAssetMetaPath(destination)));

    ASSERT_EQ(undo.RequestRedo().state, UndoRequestState::Completed);
    const auto redoneMeta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(destination));
    ASSERT_TRUE(redoneMeta.has_value());
    EXPECT_EQ(redoneMeta->id, importedMeta->id);
}

TEST(AssetFileUndoTests, DuplicateUndoRejectsExternalDestinationModification)
{
    TemporaryAssetProject project;
    const auto source = project.root / "Assets" / "Sample.txt";
    const auto destination = project.root / "Assets" / "Sample 1.txt";
    WriteText(source, "source-data");

    EditorUndoService undo;
    auto command = MakeDuplicateCommand(project.root, source, destination);
    ASSERT_NE(command, nullptr);
    ASSERT_TRUE(undo.Execute(std::move(command)).Succeeded());
    WriteText(destination, "outside-change");

    const auto result = undo.RequestUndo();
    EXPECT_EQ(result.state, UndoRequestState::Conflict);
    EXPECT_EQ(undo.GetCursor(), 1u);
    EXPECT_TRUE(std::filesystem::exists(destination));
}

TEST(AssetFileUndoTests, DuplicateUndoLeavesTheBundleIntactWhenStagingFails)
{
    TemporaryAssetProject project;
    const auto source = project.root / "Assets" / "Sample.txt";
    const auto destination = project.root / "Assets" / "Sample 1.txt";
    WriteText(source, "source-data");

    auto sourceMeta = NLS::Core::Assets::AssetMeta::CreateForAsset(source);
    ASSERT_TRUE(sourceMeta.Save(NLS::Core::Assets::GetAssetMetaPath(source)));

    EditorUndoService undo;
    auto command = MakeDuplicateCommand(project.root, source, destination);
    ASSERT_NE(command, nullptr);
    ASSERT_TRUE(undo.Execute(std::move(command)).Succeeded());

    // The first command receives ID 1. A file where its staging directory is
    // required forces the reversible move to fail before either bundle member
    // is changed.
    const auto blockedStagingDirectory = project.root / "Library" / "Editor" /
        "UndoTrash" / "asset-file" / "1" / "1";
    std::filesystem::create_directories(blockedStagingDirectory.parent_path());
    WriteText(blockedStagingDirectory, "blocked");

    const auto result = undo.RequestUndo();
    EXPECT_EQ(result.state, UndoRequestState::Failed);
    EXPECT_EQ(undo.GetCursor(), 1u);
    EXPECT_TRUE(std::filesystem::exists(destination));
    EXPECT_TRUE(std::filesystem::exists(NLS::Core::Assets::GetAssetMetaPath(destination)));
}

TEST(AssetFileUndoTests, RenameRoundTripsSourceAndMetaWithoutChangingTheAssetId)
{
    TemporaryAssetProject project;
    const auto source = project.root / "Assets" / "Before.txt";
    const auto destination = project.root / "Assets" / "After.txt";
    WriteText(source, "source-data");

    const auto sourceMeta = NLS::Core::Assets::AssetMeta::CreateForAsset(source);
    ASSERT_TRUE(sourceMeta.Save(NLS::Core::Assets::GetAssetMetaPath(source)));

    EditorUndoService undo;
    auto command = MakeRenameCommand(project.root, source, destination, sourceMeta.id.ToString());
    ASSERT_NE(command, nullptr);
    ASSERT_TRUE(undo.Execute(std::move(command)).Succeeded());
    EXPECT_FALSE(std::filesystem::exists(source));
    EXPECT_FALSE(std::filesystem::exists(NLS::Core::Assets::GetAssetMetaPath(source)));
    ASSERT_TRUE(std::filesystem::exists(destination));

    const auto renamedMeta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(destination));
    ASSERT_TRUE(renamedMeta.has_value());
    EXPECT_EQ(renamedMeta->id, sourceMeta.id);

    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    ASSERT_TRUE(std::filesystem::exists(source));
    const auto restoredMeta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(source));
    ASSERT_TRUE(restoredMeta.has_value());
    EXPECT_EQ(restoredMeta->id, sourceMeta.id);

    ASSERT_EQ(undo.RequestRedo().state, UndoRequestState::Completed);
    ASSERT_TRUE(std::filesystem::exists(destination));
    const auto redoneMeta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(destination));
    ASSERT_TRUE(redoneMeta.has_value());
    EXPECT_EQ(redoneMeta->id, sourceMeta.id);
}

TEST(AssetFileUndoTests, RenameUndoRejectsExternalDestinationModification)
{
    TemporaryAssetProject project;
    const auto source = project.root / "Assets" / "Before.txt";
    const auto destination = project.root / "Assets" / "After.txt";
    WriteText(source, "source-data");

    EditorUndoService undo;
    auto command = MakeRenameCommand(project.root, source, destination);
    ASSERT_NE(command, nullptr);
    ASSERT_TRUE(undo.Execute(std::move(command)).Succeeded());
    WriteText(destination, "outside-change");

    const auto result = undo.RequestUndo();
    EXPECT_EQ(result.state, UndoRequestState::Conflict);
    EXPECT_EQ(undo.GetCursor(), 1u);
    EXPECT_TRUE(std::filesystem::exists(destination));
}

TEST(AssetFileUndoTests, DeleteRoundTripsSourceAndMetaThroughUndoTrash)
{
    TemporaryAssetProject project;
    const auto source = project.root / "Assets" / "DeleteMe.txt";
    WriteText(source, "source-data");

    const auto sourceMeta = NLS::Core::Assets::AssetMeta::CreateForAsset(source);
    ASSERT_TRUE(sourceMeta.Save(NLS::Core::Assets::GetAssetMetaPath(source)));

    EditorUndoService undo;
    auto command = MakeDeleteCommand(project.root, source, sourceMeta.id.ToString());
    ASSERT_NE(command, nullptr);
	ASSERT_TRUE(undo.Execute(std::move(command)).Succeeded());
	EXPECT_FALSE(std::filesystem::exists(source));
	EXPECT_FALSE(std::filesystem::exists(NLS::Core::Assets::GetAssetMetaPath(source)));
	const auto undoTrash = project.root / "Library" / "Editor" / "UndoTrash" /
		"asset-file" / "1" / "1";
	EXPECT_TRUE(std::filesystem::exists(undoTrash / source.filename()));
	EXPECT_TRUE(std::filesystem::exists(undoTrash / NLS::Core::Assets::GetAssetMetaPath(source).filename()));

	ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
	ASSERT_TRUE(std::filesystem::exists(source));
    const auto restoredMeta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(source));
    ASSERT_TRUE(restoredMeta.has_value());
    EXPECT_EQ(restoredMeta->id, sourceMeta.id);

    ASSERT_EQ(undo.RequestRedo().state, UndoRequestState::Completed);
    EXPECT_FALSE(std::filesystem::exists(source));
    EXPECT_FALSE(std::filesystem::exists(NLS::Core::Assets::GetAssetMetaPath(source)));
}

TEST(AssetFileUndoTests, DeleteUndoRejectsAnExternallyRecreatedPath)
{
    TemporaryAssetProject project;
    const auto source = project.root / "Assets" / "DeleteMe.txt";
    WriteText(source, "source-data");

    EditorUndoService undo;
    auto command = MakeDeleteCommand(project.root, source);
    ASSERT_NE(command, nullptr);
    ASSERT_TRUE(undo.Execute(std::move(command)).Succeeded());
    WriteText(source, "outside-recreated");

    const auto result = undo.RequestUndo();
    EXPECT_EQ(result.state, UndoRequestState::Conflict);
	EXPECT_EQ(undo.GetCursor(), 1u);
	EXPECT_TRUE(std::filesystem::exists(source));
}

TEST(AssetFileUndoTests, DeleteUndoRejectsAnExternallyModifiedUndoTrashMetaFile)
{
	TemporaryAssetProject project;
	const auto source = project.root / "Assets" / "DeleteMe.txt";
	WriteText(source, "source-data");

	const auto sourceMeta = NLS::Core::Assets::AssetMeta::CreateForAsset(source);
	ASSERT_TRUE(sourceMeta.Save(NLS::Core::Assets::GetAssetMetaPath(source)));

	EditorUndoService undo;
	auto command = MakeDeleteCommand(project.root, source, sourceMeta.id.ToString());
	ASSERT_NE(command, nullptr);
	ASSERT_TRUE(undo.Execute(std::move(command)).Succeeded());

	const auto stagedMeta = project.root / "Library" / "Editor" / "UndoTrash" /
		"asset-file" / "1" / "1" / NLS::Core::Assets::GetAssetMetaPath(source).filename();
	ASSERT_TRUE(std::filesystem::exists(stagedMeta));
	WriteText(stagedMeta, "outside-change");

	const auto result = undo.RequestUndo();
	EXPECT_EQ(result.state, UndoRequestState::Conflict);
	EXPECT_EQ(undo.GetCursor(), 1u);
	EXPECT_FALSE(std::filesystem::exists(source));
	EXPECT_TRUE(std::filesystem::exists(stagedMeta));
}

TEST(AssetFileUndoTests, ProjectMutationRenameUpdatesSerializedReferencesAcrossAssetAndSceneScopes)
{
    TemporaryAssetProject project;
    const auto textureFolder = project.root / "Assets" / "Textures";
    const auto materialsFolder = project.root / "Assets" / "Materials";
    const auto scenesFolder = project.root / "Assets" / "Scenes";
    std::filesystem::create_directories(textureFolder);
    std::filesystem::create_directories(materialsFolder);
    std::filesystem::create_directories(scenesFolder);

    const auto source = textureFolder / "Before.png";
    const auto destination = textureFolder / "After.png";
    const auto material = materialsFolder / "UsesTexture.mat";
    const auto scene = scenesFolder / "UsesTexture.scene";
    WriteText(source, "texture-data");
    auto sourceMeta = NLS::Core::Assets::AssetMeta::CreateForAsset(source);
    ASSERT_TRUE(sourceMeta.Save(NLS::Core::Assets::GetAssetMetaPath(source)));
    WriteText(material, "_BaseMap >/Textures/Before.png<\n");
    WriteText(scene, "texture >/Textures/Before.png<\n");

    EditorUndoService undo;
    auto command = MakeProjectAssetMutationCommand(
        project.root,
        source,
        destination,
        sourceMeta.id.ToString(),
        "Rename Referenced Texture");
    ASSERT_NE(command, nullptr);
    const auto info = command->GetInfo();
    EXPECT_TRUE(std::any_of(info.scopes.begin(), info.scopes.end(), [](const UndoScopeId& scope)
    {
        return scope.kind == UndoScopeKind::Asset;
    }));
    EXPECT_TRUE(std::any_of(info.scopes.begin(), info.scopes.end(), [&scene](const UndoScopeId& scope)
    {
        return scope.kind == UndoScopeKind::Scene && scope.stableKey == scene.lexically_normal().generic_string();
    }));

    ASSERT_TRUE(undo.Execute(std::move(command)).Succeeded());
    EXPECT_FALSE(std::filesystem::exists(source));
    EXPECT_TRUE(std::filesystem::exists(destination));
    EXPECT_EQ(ReadText(material), "_BaseMap >/Textures/After.png<\n");
    EXPECT_EQ(ReadText(scene), "texture >/Textures/After.png<\n");

    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_TRUE(std::filesystem::exists(source));
    EXPECT_FALSE(std::filesystem::exists(destination));
    EXPECT_EQ(ReadText(material), "_BaseMap >/Textures/Before.png<\n");
    EXPECT_EQ(ReadText(scene), "texture >/Textures/Before.png<\n");

    ASSERT_EQ(undo.RequestRedo().state, UndoRequestState::Completed);
    EXPECT_TRUE(std::filesystem::exists(destination));
    EXPECT_EQ(ReadText(material), "_BaseMap >/Textures/After.png<\n");
    EXPECT_EQ(ReadText(scene), "texture >/Textures/After.png<\n");
}

TEST(AssetFileUndoTests, ProjectMutationPreservesUnmatchedReferenceDelimitersBetweenMatches)
{
    TemporaryAssetProject project;
    const auto textures = project.root / "Assets" / "Textures";
    const auto materials = project.root / "Assets" / "Materials";
    std::filesystem::create_directories(textures);
    std::filesystem::create_directories(materials);

    const auto source = textures / "Before.png";
    const auto destination = textures / "After.png";
    const auto material = materials / "UsesTexture.mat";
    WriteText(source, "texture-data");
    const auto sourceMeta = NLS::Core::Assets::AssetMeta::CreateForAsset(source);
    ASSERT_TRUE(sourceMeta.Save(NLS::Core::Assets::GetAssetMetaPath(source)));

    const std::string before =
        "first >/Textures/Before.png< ignored >/Textures/Other.png< second >/Textures/Before.png<\n";
    const std::string after =
        "first >/Textures/After.png< ignored >/Textures/Other.png< second >/Textures/After.png<\n";
    WriteText(material, before);

    EditorUndoService undo;
    auto command = MakeProjectAssetMutationCommand(
        project.root,
        source,
        destination,
        sourceMeta.id.ToString(),
        "Rename Referenced Texture");
    ASSERT_NE(command, nullptr);
    ASSERT_TRUE(undo.Execute(std::move(command)).Succeeded());
    EXPECT_EQ(ReadText(material), after);

    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_EQ(ReadText(material), before);
    ASSERT_EQ(undo.RequestRedo().state, UndoRequestState::Completed);
    EXPECT_EQ(ReadText(material), after);
}

TEST(AssetFileUndoTests, ProjectMutationMovesDirectoryTreeAndRoundTripsNestedReferences)
{
    TemporaryAssetProject project;
    const auto source = project.root / "Assets" / "Models" / "Before";
    const auto destination = project.root / "Assets" / "Models" / "After";
    const auto scene = project.root / "Assets" / "Scenes" / "Main.scene";
    const auto material = project.root / "Assets" / "Materials" / "Preview.mat";
    std::filesystem::create_directories(source / "Nested");
    std::filesystem::create_directories(scene.parent_path());
    std::filesystem::create_directories(material.parent_path());
    WriteText(source / "Nested" / "Mesh.model", "mesh-data");
    WriteText(source / "Nested" / "UsesOwnMesh.prefab", "mesh >/Models/Before/Nested/Mesh.model<\n");
    WriteText(scene, "model >/Models/Before/Nested/Mesh.model<\n");
    WriteText(material, "model >/Models/Before/Nested/Mesh.model<\n");

    EditorUndoService undo;
    auto command = MakeProjectAssetMutationCommand(
        project.root,
        source,
        destination,
        "Assets/Models/Before",
        "Move Model Folder");
    ASSERT_NE(command, nullptr);
    ASSERT_TRUE(undo.Execute(std::move(command)).Succeeded());
    EXPECT_TRUE(std::filesystem::exists(destination / "Nested" / "Mesh.model"));
    EXPECT_EQ(ReadText(destination / "Nested" / "UsesOwnMesh.prefab"),
        "mesh >/Models/After/Nested/Mesh.model<\n");
    EXPECT_EQ(ReadText(scene), "model >/Models/After/Nested/Mesh.model<\n");
    EXPECT_EQ(ReadText(material), "model >/Models/After/Nested/Mesh.model<\n");

    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_TRUE(std::filesystem::exists(source / "Nested" / "Mesh.model"));
    EXPECT_EQ(ReadText(source / "Nested" / "UsesOwnMesh.prefab"),
        "mesh >/Models/Before/Nested/Mesh.model<\n");
    EXPECT_EQ(ReadText(scene), "model >/Models/Before/Nested/Mesh.model<\n");

    ASSERT_EQ(undo.RequestRedo().state, UndoRequestState::Completed);
    EXPECT_TRUE(std::filesystem::exists(destination / "Nested" / "Mesh.model"));
    EXPECT_EQ(ReadText(scene), "model >/Models/After/Nested/Mesh.model<\n");
}

TEST(AssetFileUndoTests, ProjectMutationDeleteUsesUndoTrashAndRejectsExternalReferenceChanges)
{
    TemporaryAssetProject project;
    const auto source = project.root / "Assets" / "Textures" / "DeleteMe.png";
    const auto material = project.root / "Assets" / "Materials" / "UsesTexture.mat";
    std::filesystem::create_directories(source.parent_path());
    std::filesystem::create_directories(material.parent_path());
    WriteText(source, "texture-data");
    auto sourceMeta = NLS::Core::Assets::AssetMeta::CreateForAsset(source);
    ASSERT_TRUE(sourceMeta.Save(NLS::Core::Assets::GetAssetMetaPath(source)));
    WriteText(material, "_BaseMap >/Textures/DeleteMe.png<\n");

    EditorUndoService undo;
    auto command = MakeProjectAssetMutationCommand(project.root, source, {}, sourceMeta.id.ToString(), "Delete Texture");
    ASSERT_NE(command, nullptr);
    ASSERT_TRUE(undo.Execute(std::move(command)).Succeeded());
    EXPECT_FALSE(std::filesystem::exists(source));
    EXPECT_TRUE(std::filesystem::exists(
        project.root / "Library" / "Editor" / "UndoTrash" / "asset-tree" / "1" / "payload"));
    EXPECT_EQ(ReadText(material), "_BaseMap >?<\n");

    WriteText(material, "outside-change");
    const auto blockedUndo = undo.RequestUndo();
    EXPECT_EQ(blockedUndo.state, UndoRequestState::Conflict);
    EXPECT_EQ(undo.GetCursor(), 1u);
    EXPECT_FALSE(std::filesystem::exists(source));
}
} // namespace NLS::Editor::Undo::AssetFile::Tests
