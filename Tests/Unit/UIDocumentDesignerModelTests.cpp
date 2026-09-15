#include <gtest/gtest.h>

#include "Components/UICanvasComponent.h"
#include "Engine/UI/UIDocument.h"
#include "Guid.h"
#include "ServiceLocator.h"
#include "UI/UIDocumentDesignerModel.h"
#include "Undo/UndoBlobStore.h"
#include "Undo/EditorUndoService.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
class ScopedUIDocumentTestDirectory final
{
public:
    ScopedUIDocumentTestDirectory()
        : m_path(std::filesystem::temp_directory_path() /
            ("NullusUIDocumentDesigner_" + NLS::Guid::New().ToString()))
    {
        std::error_code error;
        std::filesystem::create_directories(m_path, error);
        EXPECT_FALSE(error);
    }

    ~ScopedUIDocumentTestDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    [[nodiscard]] const std::filesystem::path& GetPath() const { return m_path; }

    std::filesystem::path Write(std::filesystem::path relativePath, const std::string& contents) const
    {
        const auto path = m_path / std::move(relativePath);
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        EXPECT_FALSE(error);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        EXPECT_TRUE(output.good());
        output << contents;
        EXPECT_TRUE(output.good());
        return path;
    }

private:
    std::filesystem::path m_path;
};

class ScopedEditorUndoService final
{
public:
    ScopedEditorUndoService()
    {
        NLS::Core::ServiceLocator::Remove<NLS::Editor::Undo::EditorUndoService>();
        NLS::Core::ServiceLocator::Provide(m_service);
    }

    ~ScopedEditorUndoService()
    {
        NLS::Core::ServiceLocator::Remove<NLS::Editor::Undo::EditorUndoService>();
    }

    [[nodiscard]] NLS::Editor::Undo::EditorUndoService& Get() { return m_service; }

private:
    NLS::Editor::Undo::EditorUndoService m_service;
};

}

TEST(UIDocumentDesignerModelTests, NewDocumentIsRuntimeValidAndEmpty)
{
    using NLS::Editor::UIDocumentTools::UIDocumentDesignerModel;

    std::vector<NLS::Engine::UI::UIDocumentDiagnostic> diagnostics;
    const auto document = NLS::Engine::UI::UIDocument::LoadFromJson(
        UIDocumentDesignerModel::CreateDocumentSource("Hud"),
        &diagnostics);
    ASSERT_TRUE(document.has_value());
    EXPECT_TRUE(diagnostics.empty());
    // Copy the imported constant before comparing: binding it to a reference
    // would ODR-use the DLL-imported static member, which MinGW does not export.
    const uint32_t expectedSchemaVersion = NLS::Engine::UI::UIDocument::CurrentSchemaVersion;
    EXPECT_EQ(document->GetSchemaVersion(), expectedSchemaVersion);
    EXPECT_EQ(document->GetRootDefinition().name, "Hud");
    EXPECT_TRUE(document->GetRootDefinition().children.empty());
}

TEST(UIDocumentDesignerModelTests, EditsUndoRedoAndSaveKeepStableIdentifiers)
{
    using NLS::Editor::UIDocumentTools::UIDocumentDesignerModel;

    ScopedUIDocumentTestDirectory directory;
    ScopedEditorUndoService undo;
    const auto sourcePath = directory.Write(
        "Assets/UI/Hud.ui",
        UIDocumentDesignerModel::CreateDocumentSource("Hud"));

    UIDocumentDesignerModel model;
    ASSERT_TRUE(model.Load(sourcePath));
    const auto documentId = model.GetDocument().at("documentId").get<std::string>();
    const auto rootGuid = model.GetDocument().at("root").at("nodeGuid").get<std::string>();

    ASSERT_TRUE(model.AddChild(rootGuid, "Button"));
    const auto childGuid = model.GetSelectedNodeGuid();
    ASSERT_FALSE(childGuid.empty());
    ASSERT_NE(model.FindNode(childGuid), nullptr);

    ASSERT_TRUE(undo.Get().RequestUndo().Succeeded());
    EXPECT_EQ(model.FindNode(childGuid), nullptr);
    ASSERT_TRUE(undo.Get().RequestRedo().Succeeded());
    ASSERT_NE(model.FindNode(childGuid), nullptr);
    EXPECT_EQ(model.GetDocument().at("documentId").get<std::string>(), documentId);

    ASSERT_TRUE(model.Save());

    UIDocumentDesignerModel reloaded;
    ASSERT_TRUE(reloaded.Load(sourcePath));
    EXPECT_EQ(reloaded.GetDocument().at("documentId").get<std::string>(), documentId);
    ASSERT_NE(reloaded.FindNode(childGuid), nullptr);

    NLS::Engine::UI::UIDocument runtimeDocument;
    std::vector<NLS::Engine::UI::UIDocumentDiagnostic> diagnostics;
    EXPECT_TRUE(reloaded.BuildRuntimeDocument(runtimeDocument, &diagnostics));
    EXPECT_TRUE(diagnostics.empty());
}

TEST(UIDocumentDesignerModelTests, LargeSnapshotUsesGlobalUndoRedoAndClearAllReleasesBlob)
{
    using NLS::Editor::UIDocumentTools::UIDocumentDesignerModel;
    using NLS::Editor::Undo::UndoBlobStoreOptions;
    using NLS::Editor::Undo::UndoClearReason;

    ScopedUIDocumentTestDirectory directory;
    ScopedEditorUndoService undo;
    ASSERT_TRUE(undo.Get().ConfigureBlobStore(directory.GetPath() / "Library/Editor/UndoBlobs").Succeeded());
    const auto blobStore = undo.Get().GetSnapshotBlobStore();
    ASSERT_NE(blobStore, nullptr);

    const auto sourcePath = directory.Write(
        "Assets/UI/Hud.ui",
        UIDocumentDesignerModel::CreateDocumentSource("Hud"));

    UIDocumentDesignerModel model;
    ASSERT_TRUE(model.Load(sourcePath));
    const auto rootGuid = model.GetDocument().at("root").at("nodeGuid").get<std::string>();
    ASSERT_TRUE(model.AddChild(rootGuid, "Text"));
    const auto textGuid = model.GetSelectedNodeGuid();
    const auto beforeText = model.FindNode(textGuid)->value("text", std::string {});

    const auto spillThreshold = UndoBlobStoreOptions {}.spillThresholdBytes;
    const std::string afterText(spillThreshold + 4096u, 'x');
    ASSERT_TRUE(model.SetNodeText(textGuid, afterText));

    const auto retained = blobStore->GetStatistics();
    EXPECT_GE(retained.blobCount, 1u);
    EXPECT_GE(retained.totalReferenceCount, 1u);
    EXPECT_GT(retained.referencedByteCount, spillThreshold);
    EXPECT_GT(undo.Get().GetHistoryState().retainedBlobBytes, spillThreshold);

    ASSERT_TRUE(undo.Get().RequestUndo().Succeeded());
    EXPECT_EQ(model.FindNode(textGuid)->value("text", std::string {}), beforeText);
    ASSERT_TRUE(undo.Get().RequestRedo().Succeeded());
    EXPECT_EQ(model.FindNode(textGuid)->value("text", std::string {}), afterText);

    undo.Get().ClearAll(UndoClearReason::UserRequested);
    EXPECT_EQ(undo.Get().GetTransactionCount(), 0u);
    EXPECT_EQ(undo.Get().GetCursor(), 0u);
    EXPECT_EQ(undo.Get().GetHistoryState().retainedBlobBytes, 0u);

    const auto cleaned = blobStore->GetStatistics();
    EXPECT_EQ(cleaned.blobCount, 0u);
    EXPECT_EQ(cleaned.referencedBlobCount, 0u);
    EXPECT_EQ(cleaned.totalReferenceCount, 0u);
    EXPECT_EQ(cleaned.storedByteCount, 0u);
}

TEST(UIDocumentDesignerModelTests, MergedLargeSnapshotsKeepBlobBudgetAndReleaseSupersededPayload)
{
    using NLS::Editor::UIDocumentTools::UIDocumentDesignerModel;
    using NLS::Editor::Undo::UndoBlobStoreOptions;
    using NLS::Editor::Undo::UndoClearReason;

    ScopedUIDocumentTestDirectory directory;
    ScopedEditorUndoService undo;
    ASSERT_TRUE(undo.Get().ConfigureBlobStore(directory.GetPath() / "Library/Editor/UndoBlobs").Succeeded());
    const auto blobStore = undo.Get().GetSnapshotBlobStore();
    ASSERT_NE(blobStore, nullptr);

    const auto sourcePath = directory.Write(
        "Assets/UI/Hud.ui",
        UIDocumentDesignerModel::CreateDocumentSource("Hud"));

    UIDocumentDesignerModel model;
    ASSERT_TRUE(model.Load(sourcePath));
    const auto rootGuid = model.GetDocument().at("root").at("nodeGuid").get<std::string>();
    ASSERT_TRUE(model.AddChild(rootGuid, "Text"));
    const auto textGuid = model.GetSelectedNodeGuid();
    const auto beforeText = model.FindNode(textGuid)->value("text", std::string {});
    undo.Get().ClearAll(UndoClearReason::UserRequested);

    const auto spillThreshold = UndoBlobStoreOptions {}.spillThresholdBytes;
    const std::string firstText(spillThreshold + 4096u, 'a');
    const std::string finalText(spillThreshold + 8192u, 'b');
    ASSERT_TRUE(model.SetNodeText(textGuid, firstText));
    ASSERT_TRUE(model.SetNodeText(textGuid, finalText));

    ASSERT_EQ(undo.Get().GetTransactionCount(), 1u);
    const auto history = undo.Get().GetHistoryState();
    const auto storage = blobStore->GetStatistics();
    EXPECT_EQ(storage.blobCount, 1u);
    EXPECT_EQ(storage.referencedBlobCount, 1u);
    EXPECT_EQ(storage.totalReferenceCount, 1u);
    EXPECT_EQ(history.retainedBlobBytes, storage.logicalRetainedByteCount);

    ASSERT_TRUE(undo.Get().RequestUndo().Succeeded());
    EXPECT_EQ(model.FindNode(textGuid)->value("text", std::string {}), beforeText);
    ASSERT_TRUE(undo.Get().RequestRedo().Succeeded());
    EXPECT_EQ(model.FindNode(textGuid)->value("text", std::string {}), finalText);
}

TEST(UIDocumentDesignerModelTests, GlobalHistoryUsesScopeSavePointsAndReloadInvalidation)
{
    using NLS::Editor::UIDocumentTools::UIDocumentDesignerModel;

    ScopedUIDocumentTestDirectory directory;
    ScopedEditorUndoService undo;
    const auto sourcePath = directory.Write(
        "Assets/UI/Hud.ui",
        UIDocumentDesignerModel::CreateDocumentSource("Hud"));

    UIDocumentDesignerModel model;
    ASSERT_TRUE(model.Load(sourcePath));
    const auto rootGuid = model.GetDocument().at("root").at("nodeGuid").get<std::string>();
    const auto originalScope = model.GetUndoScope();

    ASSERT_TRUE(model.SetNodeName(rootGuid, "Updated Hud"));
    EXPECT_TRUE(undo.Get().CanUndo());
    EXPECT_TRUE(model.IsDirty());
    ASSERT_TRUE(model.Save());
    EXPECT_FALSE(model.IsDirty());
    EXPECT_FALSE(undo.Get().GetScopeState(originalScope).IsDirty());

    ASSERT_TRUE(undo.Get().RequestUndo().Succeeded());
    EXPECT_TRUE(model.IsDirty());
    ASSERT_TRUE(undo.Get().RequestRedo().Succeeded());
    EXPECT_FALSE(model.IsDirty());

    ASSERT_TRUE(model.Reload());
    EXPECT_NE(model.GetUndoScope().generation, originalScope.generation);
    EXPECT_FALSE(undo.Get().CanUndo());
    EXPECT_FALSE(model.IsDirty());
}

TEST(UIDocumentDesignerModelTests, ActiveDesignerEditIsCancelledBeforeGlobalHistoryMoves)
{
    using NLS::Editor::UIDocumentTools::UIDocumentDesignerModel;

    ScopedUIDocumentTestDirectory directory;
    ScopedEditorUndoService undo;
    const auto sourcePath = directory.Write(
        "Assets/UI/Hud.ui",
        UIDocumentDesignerModel::CreateDocumentSource("Hud"));

    UIDocumentDesignerModel model;
    ASSERT_TRUE(model.Load(sourcePath));
    const auto rootGuid = model.GetDocument().at("root").at("nodeGuid").get<std::string>();
    const auto originalName = model.GetDocument().at("root").at("name").get<std::string>();

    model.BeginTransaction("Rename UI Element");
    ASSERT_TRUE(model.SetNodeName(rootGuid, "Previewed Name", false));
    EXPECT_EQ(model.FindNode(rootGuid)->at("name").get<std::string>(), "Previewed Name");

    ASSERT_TRUE(undo.Get().RequestUndo().Succeeded());
    EXPECT_EQ(model.FindNode(rootGuid)->at("name").get<std::string>(), originalName);
    EXPECT_FALSE(model.IsDirty());

    model.EndTransaction();
    EXPECT_FALSE(undo.Get().CanUndo());
}

TEST(UIDocumentDesignerModelTests, TransactionBatchesMultiplePreviewUpdatesIntoOneGlobalCommand)
{
    using NLS::Editor::UIDocumentTools::UIDocumentDesignerModel;

    ScopedUIDocumentTestDirectory directory;
    ScopedEditorUndoService undo;
    const auto sourcePath = directory.Write(
        "Assets/UI/Hud.ui",
        UIDocumentDesignerModel::CreateDocumentSource("Hud"));

    UIDocumentDesignerModel model;
    ASSERT_TRUE(model.Load(sourcePath));
    const auto rootGuid = model.GetDocument().at("root").at("nodeGuid").get<std::string>();
    const auto before = model.GetDocument();

    model.BeginTransaction("Edit HUD");
    ASSERT_TRUE(model.SetNodeName(rootGuid, "Edited HUD", false));
    ASSERT_TRUE(model.SetNodeLayoutSize(rootGuid, 320.0f, 180.0f, false));
    EXPECT_TRUE(undo.Get().HasActiveEditSession());
    model.EndTransaction();

    EXPECT_FALSE(undo.Get().HasActiveEditSession());
    ASSERT_EQ(undo.Get().GetTransactionCount(), 1u);
    EXPECT_EQ(model.FindNode(rootGuid)->at("name").get<std::string>(), "Edited HUD");
    EXPECT_FLOAT_EQ(model.FindNode(rootGuid)->at("layout").at("size").at(0).get<float>(), 320.0f);
    EXPECT_FLOAT_EQ(model.FindNode(rootGuid)->at("layout").at("size").at(1).get<float>(), 180.0f);

    ASSERT_TRUE(undo.Get().RequestUndo().Succeeded());
    EXPECT_EQ(model.GetDocument(), before);

    ASSERT_TRUE(undo.Get().RequestRedo().Succeeded());
    EXPECT_EQ(model.FindNode(rootGuid)->at("name").get<std::string>(), "Edited HUD");
    EXPECT_FLOAT_EQ(model.FindNode(rootGuid)->at("layout").at("size").at(0).get<float>(), 320.0f);
    EXPECT_FLOAT_EQ(model.FindNode(rootGuid)->at("layout").at("size").at(1).get<float>(), 180.0f);
}

TEST(UIDocumentDesignerModelTests, GlobalUndoRejectionsRestoreDocumentSelectionAndDirtyState)
{
    using NLS::Editor::UIDocumentTools::UIDocumentDesignerModel;
    using NLS::Editor::Undo::UndoFreezeReason;

    ScopedUIDocumentTestDirectory directory;
    ScopedEditorUndoService undo;
    const auto sourcePath = directory.Write(
        "Assets/UI/Hud.ui",
        UIDocumentDesignerModel::CreateDocumentSource("Hud"));

    UIDocumentDesignerModel model;
    ASSERT_TRUE(model.Load(sourcePath));
    const auto rootGuid = model.GetDocument().at("root").at("nodeGuid").get<std::string>();
    ASSERT_TRUE(model.AddChild(rootGuid, "Button"));
    const auto childGuid = model.GetSelectedNodeGuid();
    ASSERT_TRUE(model.Save());
    const auto historyCount = undo.Get().GetTransactionCount();

    const auto expectRestored = [&]
    {
        EXPECT_NE(model.FindNode(childGuid), nullptr);
        EXPECT_EQ(model.GetSelectedNodeGuid(), childGuid);
        EXPECT_FALSE(model.IsDirty());
        EXPECT_EQ(undo.Get().GetTransactionCount(), historyCount);
        EXPECT_FALSE(undo.Get().HasActiveEditSession());
    };

    // BeginEditSession rejected.
    undo.Get().Freeze(UndoFreezeReason::User);
    model.BeginTransaction("Delete UI Element");
    EXPECT_FALSE(model.RemoveNode(childGuid));
    expectRestored();
    model.EndTransaction();
    undo.Get().Unfreeze(UndoFreezeReason::User);

    // PreviewEditSession rejected after the model has applied its first value.
    model.BeginTransaction("Delete UI Element");
    ASSERT_TRUE(undo.Get().HasActiveEditSession());
    undo.Get().Freeze(UndoFreezeReason::User);
    EXPECT_FALSE(model.RemoveNode(childGuid));
    expectRestored();
    model.EndTransaction();
    undo.Get().Unfreeze(UndoFreezeReason::User);

    // CommitEditSession rejected after a successful preview.
    model.BeginTransaction("Delete UI Element");
    ASSERT_TRUE(model.RemoveNode(childGuid));
    undo.Get().Freeze(UndoFreezeReason::User);
    model.EndTransaction();
    expectRestored();
    undo.Get().Unfreeze(UndoFreezeReason::User);

    // RecordApplied rejected for a discrete edit outside a transaction.
    undo.Get().Freeze(UndoFreezeReason::User);
    EXPECT_FALSE(model.RemoveNode(childGuid));
    expectRestored();
    undo.Get().Unfreeze(UndoFreezeReason::User);
}

TEST(UIDocumentDesignerModelTests, IsolatedModelRejectsMutationsWhenUndoServiceIsAbsent)
{
    using NLS::Editor::UIDocumentTools::UIDocumentDesignerModel;

    NLS::Core::ServiceLocator::Remove<NLS::Editor::Undo::EditorUndoService>();
    ScopedUIDocumentTestDirectory directory;
    const auto sourcePath = directory.Write(
        "Assets/UI/Hud.ui",
        UIDocumentDesignerModel::CreateDocumentSource("Hud"));

    UIDocumentDesignerModel model;
    ASSERT_TRUE(model.Load(sourcePath));
    const auto rootGuid = model.GetDocument().at("root").at("nodeGuid").get<std::string>();
    const auto documentBefore = model.GetDocument();
    const auto selectionBefore = model.GetSelectedNodeGuid();
    const auto revisionBefore = model.GetDocumentRevision();

    const auto expectRestored = [&]
    {
        EXPECT_EQ(model.GetDocument(), documentBefore);
        EXPECT_EQ(model.GetSelectedNodeGuid(), selectionBefore);
        EXPECT_EQ(model.GetDocumentRevision(), revisionBefore);
        EXPECT_FALSE(model.IsDirty());
    };

    EXPECT_FALSE(model.SetNodeName(rootGuid, "Rejected discrete edit"));
    expectRestored();

    EXPECT_FALSE(model.AddChild(rootGuid, "Text"));
    expectRestored();

    model.BeginTransaction("Rejected preview edit");
    EXPECT_FALSE(model.SetNodeName(rootGuid, "Rejected preview edit", false));
    model.EndTransaction();
    expectRestored();
}

TEST(UIDocumentDesignerModelTests, LoadNormalizesMissingIdentifiersBeforeSaving)
{
    ScopedUIDocumentTestDirectory directory;
    const auto sourcePath = directory.Write(
        "Assets/UI/Legacy.ui",
        R"json({
            "root": {
                "type": "Panel",
                "children": [ { "type": "Text", "text": "Hello" } ]
            }
        })json");

    NLS::Editor::UIDocumentTools::UIDocumentDesignerModel model;
    ASSERT_TRUE(model.Load(sourcePath));
    EXPECT_TRUE(model.IsDirty());
    EXPECT_TRUE(model.GetDocument().contains("documentId"));
    EXPECT_TRUE(model.GetDocument().at("root").contains("nodeGuid"));
    ASSERT_TRUE(model.Save());

    std::vector<NLS::Engine::UI::UIDocumentDiagnostic> diagnostics;
    EXPECT_TRUE(NLS::Engine::UI::UIDocument::LoadFromFile(sourcePath, &diagnostics).has_value());
    EXPECT_TRUE(diagnostics.empty());
}

TEST(UIDocumentDesignerModelTests, CanvasResolvedLoadKeepsLogicalDocumentReference)
{
    ScopedUIDocumentTestDirectory directory;
    const auto sourcePath = directory.Write(
        "Assets/UI/Hud.ui",
        R"json({
            "version": 1,
            "documentId": "11111111-1111-4111-8111-111111111111",
            "root": { "type": "Panel", "name": "Root" }
        })json");

    NLS::Engine::Components::UICanvasComponent canvas;
    canvas.SetDocumentPath("Assets/UI/Hud.ui");
    EXPECT_EQ(canvas.GetDocumentPath(), "Assets/UI/Hud.ui");

    std::vector<NLS::Engine::UI::UIDocumentDiagnostic> diagnostics;
    ASSERT_TRUE(canvas.LoadDocumentResolvedPath(sourcePath.string(), &diagnostics));
    EXPECT_TRUE(diagnostics.empty());
    EXPECT_EQ(canvas.GetDocumentPath(), "Assets/UI/Hud.ui");
    EXPECT_NE(canvas.GetDocument(), nullptr);
}

TEST(UIDocumentDesignerModelTests, CanvasDocumentReferenceAssignmentIsAtomicOnParseFailure)
{
    ScopedUIDocumentTestDirectory directory;
    const auto validPath = directory.Write(
        "Assets/UI/Hud.ui",
        R"json({
            "version": 1,
            "documentId": "11111111-1111-4111-8111-111111111111",
            "root": { "type": "Panel", "name": "Root" }
        })json");
    const auto invalidPath = directory.Write("Assets/UI/Broken.ui", "{}");
    NLS::Engine::Components::UICanvasComponent canvas;
    const auto validReference = NLS::Engine::UI::UIDocumentReference(
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("0f3f794e-7578-4bcb-92cb-b3afb43ee861")),
        "Assets/UI/Hud.ui");
    ASSERT_TRUE(canvas.LoadDocumentResolvedPathAsReference(validPath.string(), validReference));
    ASSERT_NE(canvas.GetDocument(), nullptr);
    const auto* originalDocument = canvas.GetDocument();

    const auto invalidReference = NLS::Engine::UI::UIDocumentReference(
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("861b35f8-3170-4d0b-9fdb-111b8e089a3d")),
        "Assets/UI/Broken.ui");
    EXPECT_FALSE(canvas.LoadDocumentResolvedPathAsReference(invalidPath.string(), invalidReference));
    EXPECT_EQ(canvas.GetDocumentPath(), "Assets/UI/Hud.ui");
    EXPECT_EQ(canvas.GetDocumentReference(), validReference);
    EXPECT_EQ(canvas.GetDocument(), originalDocument);
}
