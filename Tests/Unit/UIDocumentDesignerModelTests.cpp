#include <gtest/gtest.h>

#include "Components/UICanvasComponent.h"
#include "Engine/UI/UIDocument.h"
#include "Guid.h"
#include "Panels/Inspector.h"
#include "UI/UIDocumentDesignerModel.h"

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
    EXPECT_EQ(document->GetSchemaVersion(), NLS::Engine::UI::UIDocument::CurrentSchemaVersion);
    EXPECT_EQ(document->GetRootDefinition().name, "Hud");
    EXPECT_TRUE(document->GetRootDefinition().children.empty());
}

TEST(UIDocumentDesignerModelTests, EditsUndoRedoAndSaveKeepStableIdentifiers)
{
    using NLS::Editor::UIDocumentTools::UIDocumentDesignerModel;

    ScopedUIDocumentTestDirectory directory;
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

    ASSERT_TRUE(model.Undo());
    EXPECT_EQ(model.FindNode(childGuid), nullptr);
    ASSERT_TRUE(model.Redo());
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

TEST(UIDocumentDesignerModelTests, CanvasInspectorDocumentPickerKeepsAssignedDocumentVisible)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect the Canvas document picker.";
#else
    ScopedUIDocumentTestDirectory directory;
    const auto sourcePath = directory.Write(
        "Assets/UI/Hud.ui",
        R"json({
            "version": 1,
            "documentId": "11111111-1111-4111-8111-111111111111",
            "root": { "type": "Panel", "name": "Root" }
        })json");
    const auto projectAssetsPath = directory.GetPath() / "Assets";

    NLS::Engine::Components::UICanvasComponent canvas;
    ASSERT_TRUE(NLS::Editor::Panels::TestHooks::AssignUICanvasDocumentReferenceForTesting(
        canvas,
        projectAssetsPath,
        sourcePath.string()));
    EXPECT_EQ(canvas.GetDocumentPath(), "Assets/UI/Hud.ui");
    EXPECT_NE(canvas.GetDocument(), nullptr);

    const auto choices = NLS::Editor::Panels::TestHooks::BuildUICanvasDocumentChoicesForTesting(
        projectAssetsPath,
        canvas.GetDocumentPath());
    const auto selectedChoice = NLS::Editor::Panels::TestHooks::FindUICanvasDocumentChoiceForTesting(
        choices,
        projectAssetsPath,
        canvas.GetDocumentPath());
    ASSERT_NE(selectedChoice, 0);
    EXPECT_EQ(choices.at(selectedChoice), "Assets/UI/Hud.ui");

    // The Asset Browser can publish a replacement index while the menu is
    // open. The click must resolve through the displayed choice snapshot.
    NLS::Engine::Components::UICanvasComponent choiceCanvas;
    ASSERT_TRUE(NLS::Editor::Panels::TestHooks::AssignUICanvasDocumentChoiceForTesting(
        choiceCanvas,
        projectAssetsPath,
        choices,
        selectedChoice));
    EXPECT_EQ(choiceCanvas.GetDocumentPath(), "Assets/UI/Hud.ui");
    EXPECT_NE(choiceCanvas.GetDocument(), nullptr);

    ASSERT_TRUE(NLS::Editor::Panels::TestHooks::AssignUICanvasDocumentReferenceForTesting(
        canvas,
        projectAssetsPath,
        "Assets\\UI\\Hud.ui"));
    EXPECT_EQ(canvas.GetDocumentPath(), "Assets/UI/Hud.ui");
    EXPECT_NE(NLS::Editor::Panels::TestHooks::FindUICanvasDocumentChoiceForTesting(
        choices,
        projectAssetsPath,
        canvas.GetDocumentPath()), 0);
#endif
}
