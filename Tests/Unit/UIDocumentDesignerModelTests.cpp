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

TEST(UIDocumentDesignerModelTests, PropertyApiValidatesNormalizesAndUndoes)
{
    using NLS::Editor::UIDocumentTools::UIDocumentDesignerModel;
    using Json = UIDocumentDesignerModel::Json;

    ScopedUIDocumentTestDirectory directory;
    ScopedEditorUndoService undo;
    const auto sourcePath = directory.Write(
        "Assets/UI/Hud.ui",
        UIDocumentDesignerModel::CreateDocumentSource("Hud"));

    UIDocumentDesignerModel model;
    ASSERT_TRUE(model.Load(sourcePath));
    const auto rootGuid = model.GetRootGuid();

    EXPECT_FALSE(model.SetNodeProperty(rootGuid, "layout.bogus", 1.0f));
    EXPECT_FALSE(model.SetNodeProperty(rootGuid, "layout.anchorMin", Json::array({ 0.25f })));
    EXPECT_FALSE(model.SetNodeProperty(rootGuid, "layout.mode", std::string("Bogus")));

    ASSERT_TRUE(model.SetNodeProperty(
        rootGuid,
        "layout.anchorMin",
        Json::array({ 0.25f, 0.75f })));
    const auto* anchor = model.GetNodeProperty(rootGuid, "layout.anchorMin");
    ASSERT_NE(anchor, nullptr);
    EXPECT_FLOAT_EQ((*anchor)[0].get<float>(), 0.25f);
    EXPECT_FLOAT_EQ((*anchor)[1].get<float>(), 0.75f);

    ASSERT_TRUE(model.SetNodeProperty(rootGuid, "style.opacity", 4.0f));
    ASSERT_NE(model.GetNodeProperty(rootGuid, "style.opacity"), nullptr);
    EXPECT_FLOAT_EQ(model.GetNodeProperty(rootGuid, "style.opacity")->get<float>(), 1.0f);

    ASSERT_TRUE(model.SetNodeProperty(rootGuid, "layout.mode", std::string("Grid")));
    EXPECT_EQ(
        model.GetNodeProperty(rootGuid, "layout.mode")->get<std::string>(),
        "Grid");

    ASSERT_TRUE(undo.Get().RequestUndo().Succeeded());
    EXPECT_EQ(
        model.GetNodeProperty(rootGuid, "layout.mode")->get<std::string>(),
        "Overlay");
    ASSERT_TRUE(undo.Get().RequestUndo().Succeeded());
    EXPECT_EQ(model.GetNodeProperty(rootGuid, "style.opacity"), nullptr);
    ASSERT_TRUE(undo.Get().RequestUndo().Succeeded());
    EXPECT_EQ(model.GetNodeProperty(rootGuid, "layout.anchorMin"), nullptr);
}

TEST(UIDocumentDesignerModelTests, DuplicateNodeCreatesFreshIdentifiersAndUndoRemovesCopy)
{
    using NLS::Editor::UIDocumentTools::UIDocumentDesignerModel;

    ScopedUIDocumentTestDirectory directory;
    ScopedEditorUndoService undo;
    const auto sourcePath = directory.Write(
        "Assets/UI/Hud.ui",
        UIDocumentDesignerModel::CreateDocumentSource("Hud"));

    UIDocumentDesignerModel model;
    ASSERT_TRUE(model.Load(sourcePath));
    const auto rootGuid = model.GetRootGuid();
    ASSERT_TRUE(model.AddChild(rootGuid, "Text"));
    const auto sourceGuid = model.GetSelectedNodeGuid();
    ASSERT_TRUE(model.SetNodeName(sourceGuid, "Title"));
    ASSERT_EQ(model.GetChildCount(rootGuid), 1u);

    ASSERT_TRUE(model.DuplicateNode(sourceGuid));
    const auto copyGuid = model.GetSelectedNodeGuid();
    EXPECT_NE(copyGuid, sourceGuid);
    EXPECT_EQ(model.GetChildCount(rootGuid), 2u);
    EXPECT_EQ(model.FindNode(copyGuid)->value("name", std::string {}), "Title");

    std::string parentGuid;
    size_t index = 0u;
    ASSERT_TRUE(model.TryGetNodeLocation(copyGuid, parentGuid, index));
    EXPECT_EQ(parentGuid, rootGuid);
    EXPECT_EQ(index, 1u);
    EXPECT_FALSE(model.DuplicateNode(rootGuid));

    ASSERT_TRUE(undo.Get().RequestUndo().Succeeded());
    EXPECT_EQ(model.GetChildCount(rootGuid), 1u);
    EXPECT_EQ(model.FindNode(copyGuid), nullptr);
}

TEST(UIDocumentDesignerModelTests, MoveNodeRejectsCyclesAndReordersSiblings)
{
    using NLS::Editor::UIDocumentTools::UIDocumentDesignerModel;

    ScopedUIDocumentTestDirectory directory;
    ScopedEditorUndoService undo;
    const auto sourcePath = directory.Write(
        "Assets/UI/Hud.ui",
        UIDocumentDesignerModel::CreateDocumentSource("Hud"));

    UIDocumentDesignerModel model;
    ASSERT_TRUE(model.Load(sourcePath));
    const auto rootGuid = model.GetRootGuid();
    ASSERT_TRUE(model.AddChild(rootGuid, "Panel"));
    const auto panelGuid = model.GetSelectedNodeGuid();
    ASSERT_TRUE(model.AddChild(panelGuid, "Text"));
    const auto textGuid = model.GetSelectedNodeGuid();

    EXPECT_FALSE(model.MoveNode(panelGuid, textGuid, 0u));
    EXPECT_EQ(model.GetNodeParentGuid(textGuid), panelGuid);
    EXPECT_FALSE(model.MoveNode(rootGuid, panelGuid, 0u));

    ASSERT_TRUE(model.MoveNode(textGuid, rootGuid, 0u));
    EXPECT_EQ(model.GetNodeParentGuid(textGuid), rootGuid);
    const auto& children = model.GetDocument().at("root").at("children");
    ASSERT_EQ(children.size(), 2u);
    EXPECT_EQ(children[0].value("nodeGuid", std::string {}), textGuid);

    ASSERT_TRUE(undo.Get().RequestUndo().Succeeded());
    EXPECT_EQ(model.GetNodeParentGuid(textGuid), panelGuid);
}

TEST(UIDocumentDesignerModelTests, MultiSelectionPropertyEditsBatchIntoOneCommand)
{
    using NLS::Editor::UIDocumentTools::UIDocumentDesignerModel;
    using Json = UIDocumentDesignerModel::Json;

    ScopedUIDocumentTestDirectory directory;
    ScopedEditorUndoService undo;
    const auto sourcePath = directory.Write(
        "Assets/UI/Hud.ui",
        UIDocumentDesignerModel::CreateDocumentSource("Hud"));

    UIDocumentDesignerModel model;
    ASSERT_TRUE(model.Load(sourcePath));
    const auto rootGuid = model.GetRootGuid();
    ASSERT_TRUE(model.AddChild(rootGuid, "Text"));
    const auto firstGuid = model.GetSelectedNodeGuid();
    ASSERT_TRUE(model.AddChild(rootGuid, "Text"));
    const auto secondGuid = model.GetSelectedNodeGuid();

    model.SetSelection({ firstGuid, secondGuid });
    ASSERT_EQ(model.GetSelectedNodes().size(), 2u);
    const auto historyBefore = undo.Get().GetTransactionCount();

    ASSERT_TRUE(model.SetNodePropertyOnSelection(
        "layout.size",
        Json::array({ 40.0f, 20.0f }),
        true,
        "Edit UI Size"));
    EXPECT_EQ(undo.Get().GetTransactionCount(), historyBefore + 1u);
    EXPECT_FLOAT_EQ(
        model.GetNodeProperty(firstGuid, "layout.size")->at(0).get<float>(),
        40.0f);
    EXPECT_FLOAT_EQ(
        model.GetNodeProperty(secondGuid, "layout.size")->at(0).get<float>(),
        40.0f);

    ASSERT_TRUE(undo.Get().RequestUndo().Succeeded());
    EXPECT_FLOAT_EQ(
        model.GetNodeProperty(firstGuid, "layout.size")->at(0).get<float>(),
        160.0f);
    EXPECT_FLOAT_EQ(
        model.GetNodeProperty(secondGuid, "layout.size")->at(0).get<float>(),
        160.0f);
}

TEST(UIDocumentDesignerModelTests, DiagnosticPathsMapBackToNodes)
{
    using NLS::Editor::UIDocumentTools::UIDocumentDesignerModel;

    ScopedUIDocumentTestDirectory directory;
    ScopedEditorUndoService undo;
    const auto sourcePath = directory.Write(
        "Assets/UI/Hud.ui",
        UIDocumentDesignerModel::CreateDocumentSource("Hud"));

    UIDocumentDesignerModel model;
    ASSERT_TRUE(model.Load(sourcePath));
    const auto rootGuid = model.GetRootGuid();
    ASSERT_TRUE(model.AddChild(rootGuid, "Panel"));
    const auto panelGuid = model.GetSelectedNodeGuid();
    ASSERT_TRUE(model.AddChild(panelGuid, "Text"));
    const auto textGuid = model.GetSelectedNodeGuid();

    const auto path = model.BuildDiagnosticPath(textGuid);
    EXPECT_EQ(path, "$.root.children[0].children[0]");

    const auto* node = model.FindNodeByDiagnosticPath(path);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->value("nodeGuid", std::string {}), textGuid);

    const auto* field = model.FindNodeByDiagnosticPath(path + ".layout.size");
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(field->value("nodeGuid", std::string {}), textGuid);
    EXPECT_EQ(model.FindNodeByDiagnosticPath("$.version"), nullptr);
}

TEST(UIDocumentDesignerModelTests, CancelTransactionRestoresDocumentSelectionAndHistory)
{
    using NLS::Editor::UIDocumentTools::UIDocumentDesignerModel;

    ScopedUIDocumentTestDirectory directory;
    ScopedEditorUndoService undo;
    const auto sourcePath = directory.Write(
        "Assets/UI/Hud.ui",
        UIDocumentDesignerModel::CreateDocumentSource("Hud"));

    UIDocumentDesignerModel model;
    ASSERT_TRUE(model.Load(sourcePath));
    const auto rootGuid = model.GetRootGuid();
    ASSERT_TRUE(model.AddChild(rootGuid, "Button"));
    const auto childGuid = model.GetSelectedNodeGuid();
    const auto documentBefore = model.GetDocument();
    const auto historyBefore = undo.Get().GetTransactionCount();

    model.BeginTransaction("Edit UI Element");
    ASSERT_TRUE(model.SetNodeName(childGuid, "Renamed", false));
    ASSERT_TRUE(model.SetNodeLayoutSize(childGuid, 10.0f, 10.0f, false));
    ASSERT_TRUE(model.HasActiveTransaction());
    model.CancelTransaction();

    EXPECT_EQ(model.GetDocument(), documentBefore);
    EXPECT_EQ(model.GetSelectedNodeGuid(), childGuid);
    EXPECT_EQ(undo.Get().GetTransactionCount(), historyBefore);
    EXPECT_FALSE(model.HasActiveTransaction());
}

TEST(UIDocumentDesignerModelTests, AddChildAtPastesAndVisibilityUseOneCommandEach)
{
    using NLS::Editor::UIDocumentTools::UIDocumentDesignerModel;

    ScopedUIDocumentTestDirectory directory;
    ScopedEditorUndoService undo;
    const auto sourcePath = directory.Write(
        "Assets/UI/Hud.ui",
        UIDocumentDesignerModel::CreateDocumentSource("Hud"));

    UIDocumentDesignerModel model;
    ASSERT_TRUE(model.Load(sourcePath));
    const auto rootGuid = model.GetRootGuid();

    ASSERT_TRUE(model.AddChildAt(rootGuid, "Image", 64.0f, 32.0f));
    const auto placedGuid = model.GetSelectedNodeGuid();
    const auto* offset = model.GetNodeProperty(placedGuid, "layout.offsetMin");
    ASSERT_NE(offset, nullptr);
    EXPECT_FLOAT_EQ((*offset)[0].get<float>(), 64.0f);
    EXPECT_FLOAT_EQ((*offset)[1].get<float>(), 32.0f);

    ASSERT_TRUE(model.SetNodeVisible(placedGuid, false));
    ASSERT_NE(model.GetNodeProperty(placedGuid, "style.visible"), nullptr);
    EXPECT_FALSE(model.GetNodeProperty(placedGuid, "style.visible")->get<bool>());
    EXPECT_FALSE(model.SetNodeVisible(placedGuid, false));
    ASSERT_TRUE(undo.Get().RequestUndo().Succeeded());
    EXPECT_EQ(model.GetNodeProperty(placedGuid, "style.visible"), nullptr);

    const auto serialized = model.SerializeNode(placedGuid);
    ASSERT_FALSE(serialized.empty());
    ASSERT_TRUE(model.PasteNode(rootGuid, 0u, serialized));
    const auto pastedGuid = model.GetSelectedNodeGuid();
    EXPECT_NE(pastedGuid, placedGuid);
    EXPECT_EQ(model.GetChildCount(rootGuid), 2u);
    std::string parentGuid;
    size_t index = 99u;
    ASSERT_TRUE(model.TryGetNodeLocation(pastedGuid, parentGuid, index));
    EXPECT_EQ(parentGuid, rootGuid);
    EXPECT_EQ(index, 0u);
    EXPECT_FALSE(model.PasteNode(rootGuid, 0u, "{}"));
}
