#include <gtest/gtest.h>
#include <fstream>
#include "Materials/MaterialDocument.h"
#include "Guid.h"

TEST(MaterialDocumentTests, SavingAndDiscardingWorkingDoNotChangeApplied)
{
    NLS::Editor::Materials::MaterialDocument document;
    const auto path=std::filesystem::temp_directory_path()/("NullusMaterialDocument_"+NLS::Guid::New().ToString()+".mat");
    ASSERT_TRUE(document.Save(path)) << document.Error();
    const auto original=document.Source();
    document.MarkApplied("snapshot-1",original);
    auto candidate=document.Asset();
    candidate.parameters.push_back({0,"_Roughness",NLS::Render::Resources::UniformType::UNIFORM_FLOAT,0.4f});
    ASSERT_TRUE(document.Apply(candidate,"Edit roughness"));
    EXPECT_TRUE(document.IsDirty()); EXPECT_FALSE(document.IsApplied());
    ASSERT_TRUE(document.Save());
    EXPECT_FALSE(document.IsDirty()); EXPECT_FALSE(document.IsApplied());
    EXPECT_EQ(document.AppliedSource(),original);
    EXPECT_EQ(document.AppliedSnapshot(),"snapshot-1");
    candidate.parameters[0].value=0.9f;
    ASSERT_TRUE(document.Apply(candidate,"Edit again"));
    ASSERT_TRUE(document.DiscardWorking());
    EXPECT_EQ(document.Source(),document.SavedSource());
    EXPECT_EQ(document.AppliedSource(),original);
    std::filesystem::remove(path);
}

TEST(MaterialDocumentTests, ContinuousParameterEditMakesOneUndoAndPreservesAppliedVersion)
{
    NLS::Editor::Undo::EditorUndoService undo;
    NLS::Editor::Materials::MaterialDocument document(&undo);
    const auto original=document.Source();
    document.MarkApplied("snapshot",original);
    ASSERT_TRUE(document.BeginEdit("Drag roughness"));
    auto candidate=document.Asset();
    candidate.parameters.push_back({0,"_Roughness",NLS::Render::Resources::UniformType::UNIFORM_FLOAT,0.2f});
    ASSERT_TRUE(document.PreviewEdit(candidate));
    candidate.parameters[0].value=0.7f;
    ASSERT_TRUE(document.PreviewEdit(candidate));
    ASSERT_TRUE(document.EndEdit());
    ASSERT_TRUE(undo.RequestUndo().Succeeded());
    EXPECT_EQ(document.Source(),original);
    EXPECT_FALSE(undo.CanUndo());
    ASSERT_TRUE(undo.RequestRedo().Succeeded());
    EXPECT_EQ(document.AppliedSource(),original);
    EXPECT_NE(document.Source(),original);
}

TEST(MaterialDocumentTests, ExternalSourceEditBlocksSaveWithoutOverwritingIt)
{
    NLS::Editor::Materials::MaterialDocument document;
    const auto path=std::filesystem::temp_directory_path()/("NullusMaterialConflict_"+NLS::Guid::New().ToString()+".mat");
    ASSERT_TRUE(document.Save(path));
    { std::ofstream file(path,std::ios::binary|std::ios::app); file << "external edit"; }
    EXPECT_FALSE(document.Save());
    EXPECT_NE(document.Error().find("outside"),std::string::npos);
    std::ifstream file(path,std::ios::binary);
    const std::string content{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    EXPECT_TRUE(content.ends_with("external edit"));
    file.close(); std::filesystem::remove(path);
}
