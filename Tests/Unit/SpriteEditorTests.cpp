#include <gtest/gtest.h>
#include "Sprites/SpriteDocument.h"
#include "Inspector/AssetTargetAdapter.h"
#include "Undo/EditorUndoService.h"
#include "Panels/SpriteEditor.h"
#include "Assets/AssetWatcherReimport.h"
#include <Core/ServiceLocator.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <Image.h>
#include <fstream>
#include <algorithm>

#if defined(NLS_ENABLE_TEST_HOOKS)
namespace
{
namespace A=NLS::Core::Assets;
namespace I=NLS::Editor::Inspector;
namespace U=NLS::Editor::Undo;
struct SpriteEditorFixture:testing::Test
{
    std::filesystem::path root,image;
    A::AssetMeta meta;
    I::InspectorTargetHandle target;
    U::EditorUndoService undo;
    void SetUp()override
    {
        root=std::filesystem::path(NLS_BUILD_DIR)/"sprite-editor-tests"/NLS::Guid::New().ToString();
        image=root/"Assets/Sheet.png";std::filesystem::create_directories(image.parent_path());
        NLS::Image pixels(8,8,4);
        for(int y=1;y<3;++y)for(int x=1;x<3;++x)pixels.GetData()[(y*8+x)*4+3]=255;
        for(int y=5;y<7;++y)for(int x=5;x<7;++x)pixels.GetData()[(y*8+x)*4+3]=255;
        pixels.Save(image.string());
        meta=A::AssetMeta::CreateForAsset(image);meta.settings["TEXTURE_TYPE"]="sprite";
        meta.settings["SPRITE_ID"]=NLS::Guid::New().ToString();ASSERT_TRUE(meta.Save(A::GetAssetMetaPath(image)));
        target=I::InspectorTargetHandle::FromAsset(I::InspectorTargetKind::AssetImporter,"Assets/Sheet.png",meta.id.ToString());
        I::TestHooks::SeedImporterDraftForTesting(target,meta.settings);
    }
    void TearDown()override{I::TestHooks::ClearImporterDraftForTesting(target);}
};

TEST(SpriteImporterWatcherDeferral, KeepsSourceAndMetadataNotificationsUntilTheImportCompletes)
{
    const std::filesystem::path source="Assets/Sheet.png",meta="Assets/Sheet.png.meta",other="Assets/Other.png";
    bool importing=true;
    const auto busy=[&](const std::filesystem::path& path){return importing&&(path==source||path==meta);};
    std::vector<std::filesystem::path> changes{source,meta,meta,other},deferred;
    NLS::Editor::Assets::DeferInProgressAssetWatcherChanges(changes,deferred,busy);
    EXPECT_EQ(changes,(std::vector<std::filesystem::path>{other}));
    EXPECT_EQ(deferred,(std::vector<std::filesystem::path>{source,meta}));
    // External edits arriving during an import are delayed, never discarded.
    changes=std::move(deferred);deferred.clear();
    NLS::Editor::Assets::DeferInProgressAssetWatcherChanges(changes,deferred,busy);
    EXPECT_TRUE(changes.empty());ASSERT_EQ(deferred.size(),2);
    importing=false;changes=std::move(deferred);deferred.clear();
    NLS::Editor::Assets::DeferInProgressAssetWatcherChanges(changes,deferred,busy);
    EXPECT_EQ(changes,(std::vector<std::filesystem::path>{source,meta}));EXPECT_TRUE(deferred.empty());
}

TEST_F(SpriteEditorFixture, GridEditsShareDraftUndoAndKeepTheOriginalSpriteReference)
{
    NLS::Editor::Sprites::SpriteDocument document(root,&undo);ASSERT_TRUE(document.Open("Assets/Sheet.png"))<<document.Error();
    const auto initial=document.Read();ASSERT_TRUE(initial);ASSERT_EQ(initial->slices.size(),1);
    const auto legacy=initial->slices.front().id;
    A::SpriteGridSettings grid;grid.cellWidth=grid.cellHeight=4;
    ASSERT_TRUE(document.Grid(grid))<<document.Error();
    const auto sliced=document.Read();ASSERT_TRUE(sliced);ASSERT_EQ(sliced->slices.size(),4);
    EXPECT_TRUE(sliced->multiple);EXPECT_EQ(sliced->slices.front().id,legacy);EXPECT_TRUE(sliced->dirty);
    EXPECT_EQ(A::AssetMeta::Load(A::GetAssetMetaPath(image))->settings,meta.settings);
    ASSERT_EQ(undo.RequestUndo().state,U::UndoRequestState::Completed);
    EXPECT_FALSE(document.Read()->multiple);EXPECT_EQ(document.Read()->slices.size(),1);
    ASSERT_EQ(undo.RequestRedo().state,U::UndoRequestState::Completed);
    EXPECT_EQ(document.Read()->slices.front().id,legacy);
    auto renamed=document.Read()->slices;renamed[0].name="Renamed";std::reverse(renamed.begin(),renamed.end());
    ASSERT_TRUE(document.EditSlices(renamed,"Rename and reorder"));
    EXPECT_EQ(document.Read()->slices.back().id,legacy);
    ASSERT_EQ(undo.RequestUndo().state,U::UndoRequestState::Completed);
    EXPECT_NE(document.Read()->slices.front().name,"Renamed");
}
TEST_F(SpriteEditorFixture, InvalidRectanglesAndSourceChangesLeaveTheSharedDraftUntouched)
{
    NLS::Editor::Sprites::SpriteDocument document(root,&undo);ASSERT_TRUE(document.Open(image));
    const auto before=I::ReadImporterEditingState(target);
    auto invalid=document.Read()->slices;invalid.front().width=999;
    EXPECT_FALSE(document.EditSlices(invalid,"Invalid rectangle"));
    EXPECT_EQ(I::ReadImporterEditingState(target).settings,before.settings);EXPECT_FALSE(undo.CanUndo());
    EXPECT_FALSE(document.Open(root/"../Outside.png"));
    {std::ofstream append(image,std::ios::app|std::ios::binary);append.put('\0');}
    EXPECT_TRUE(document.SourceChanged());EXPECT_FALSE(document.EditSingle({.25f,.5f},16));
    EXPECT_EQ(I::ReadImporterEditingState(target).settings,before.settings);
}

TEST_F(SpriteEditorFixture, AutomaticSlicingAndContinuousPropertyEditsRemainOneUndoOperation)
{
    NLS::Editor::Sprites::SpriteDocument document(root,&undo);ASSERT_TRUE(document.Open(image));
    ASSERT_TRUE(document.Automatic(0,1))<<document.Error();ASSERT_EQ(document.Read()->slices.size(),2);
    const auto old=document.Read()->slices;
    auto changed=old;changed[0].pivot={.1f,.2f};
    ASSERT_TRUE(document.EditSlices(changed,"Move pivot","pivot-drag"));
    changed[0].pivot={.4f,.8f};ASSERT_TRUE(document.EditSlices(changed,"Move pivot","pivot-drag"));
    ASSERT_EQ(undo.RequestUndo().state,U::UndoRequestState::Completed);
    EXPECT_EQ(document.Read()->slices.front().pivot,old.front().pivot);
    ASSERT_EQ(undo.RequestRedo().state,U::UndoRequestState::Completed);
    EXPECT_EQ(document.Read()->slices.front().pivot,changed.front().pivot);
    // Another editor changing the draft outside the timeline must not be erased.
    auto external=I::ReadImporterEditingState(target).settings;external["EXTERNAL"]="changed";
    I::TestHooks::SeedImporterDraftForTesting(target,external);
    EXPECT_NE(undo.RequestUndo().state,U::UndoRequestState::Completed);
    EXPECT_EQ(I::ReadImporterEditingState(target).settings,external);
}

TEST_F(SpriteEditorFixture, ExternalMetadataCannotBeAdoptedWhileTheSharedDraftIsStale)
{
    NLS::Editor::Sprites::SpriteDocument document(root,&undo);ASSERT_TRUE(document.Open(image));
    auto external=meta;external.settings["EXTERNAL_IMPORT_OPTION"]="preserve";
    ASSERT_TRUE(external.Save(A::GetAssetMetaPath(image)));
    ASSERT_TRUE(document.Read());
    EXPECT_FALSE(document.EditSingle({.25f,.5f},16));EXPECT_FALSE(undo.CanUndo());
    EXPECT_EQ(A::AssetMeta::Load(A::GetAssetMetaPath(image))->settings,external.settings);
    EXPECT_EQ(I::ReadImporterEditingState(target).settings,meta.settings);
}
TEST_F(SpriteEditorFixture, BorderEditsUseTheSharedSingleAndMultipleUndoDrafts)
{
    NLS::Editor::Sprites::SpriteDocument document(root,&undo);ASSERT_TRUE(document.Open(image));
    const std::array<uint32_t,4> border{1,2,1,2};
    ASSERT_TRUE(document.EditSingle({.5f,.5f},100,{},border))<<document.Error();
    ASSERT_TRUE(document.Read());EXPECT_EQ(document.Read()->slices.front().border,border);
    ASSERT_EQ(undo.RequestUndo().state,U::UndoRequestState::Completed);
    EXPECT_EQ(document.Read()->slices.front().border,(std::array<uint32_t,4>{}));
    auto slices=document.Read()->slices;slices.front().border=border;
    ASSERT_TRUE(document.EditSlices(slices,"Edit slice borders"));
    EXPECT_TRUE(document.Read()->multiple);EXPECT_EQ(document.Read()->slices.front().border,border);
    const auto invalid=std::array<uint32_t,4>{8,0,8,0};slices.front().border=invalid;
    EXPECT_FALSE(document.EditSlices(slices,"Invalid borders"));EXPECT_EQ(document.Read()->slices.front().border,border);
}

struct SpriteEditorUiFixture:SpriteEditorFixture
{
    ImGuiContext* previousContext=nullptr;
    U::EditorUndoService* previousUndo=nullptr;
    std::unique_ptr<NLS::Editor::Panels::SpriteEditor> panel;

    void SetUp()override
    {
        SpriteEditorFixture::SetUp();
        if(NLS::Core::ServiceLocator::Contains<U::EditorUndoService>())
            previousUndo=&NLS::Core::ServiceLocator::Get<U::EditorUndoService>();
        NLS::Core::ServiceLocator::Provide(undo);
        previousContext=ImGui::GetCurrentContext();ImGui::CreateContext();
        auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;
        io.DisplaySize={1280,900};io.DeltaTime=1.f/60;io.MouseDoubleClickTime=.001f;
        io.Fonts->AddFontDefault();unsigned char* pixels;int width,height;
        io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    }
    void TearDown()override
    {
        panel.reset();ImGui::DestroyContext();ImGui::SetCurrentContext(previousContext);
        if(previousUndo)NLS::Core::ServiceLocator::Provide(*previousUndo);
        else NLS::Core::ServiceLocator::Remove<U::EditorUndoService>();
        SpriteEditorFixture::TearDown();
    }
    void Open()
    {
        panel=std::make_unique<NLS::Editor::Panels::SpriteEditor>("Sprite Editor",false,NLS::UI::PanelWindowSettings{},root);
        ASSERT_TRUE(panel->OpenDocument(image));
        Frame();Frame();Frame();
        EXPECT_FALSE(I::ReadImporterEditingState(target).dirty);
    }
    void SeedRectangle(uint32_t x,uint32_t y,uint32_t width,uint32_t height)
    {
        A::SpriteSliceDefinition slice{meta.settings.at("SPRITE_ID"),"Frame",x,y,width,height};
        std::string error;meta.settings[A::SpriteImportModeSetting]="multiple";
        meta.settings[A::SpriteSlicesSetting]=A::SerializeSpriteSlices(std::span(&slice,1),error);
        ASSERT_TRUE(error.empty());ASSERT_TRUE(meta.Save(A::GetAssetMetaPath(image)));
        I::TestHooks::SeedImporterDraftForTesting(target,meta.settings);
    }
    void Frame()
    {ImGui::NewFrame();panel->Draw();ImGui::Render();}
    ImGuiWindow* Child(const char* name)
    {
        auto* parent=ImGui::FindWindowByName("Sprite Editor##Sprite Editor");
        for(auto* window:GImGui->Windows)
            if(window->ParentWindow==parent&&std::string_view(window->Name).find(name)!=std::string_view::npos)return window;
        return nullptr;
    }
    ImVec2 Pixel(float x,float y)
    {
        auto* child=Child("/SpritePreview_");
        if(!child){ADD_FAILURE()<<"Sprite preview child is unavailable";return {};}
        const auto origin=child->DC.CursorStartPos;
        const float scale=std::min((child->WorkRect.Max.x-origin.x)/8.f,(child->WorkRect.Max.y-origin.y)/8.f);
        return {origin.x+(child->WorkRect.Max.x-origin.x-8*scale)*.5f+x*scale,
            origin.y+(child->WorkRect.Max.y-origin.y-8*scale)*.5f+y*scale};
    }
    void Move(ImVec2 point){ImGui::GetIO().AddMousePosEvent(point.x,point.y);Frame();}
    void Button(bool down){ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left,down);Frame();}
    void Click(ImVec2 point){Move(point);Button(true);Button(false);}
    std::vector<A::SpriteSliceDefinition> Slices()
    {
        NLS::Editor::Sprites::SpriteDocument document(root,&undo);
        if(!document.Open(image)){ADD_FAILURE()<<document.Error();return {};}
        const auto state=document.Read();if(!state){ADD_FAILURE()<<document.Error();return {};}
        return state->slices;
    }
};

TEST_F(SpriteEditorUiFixture, FirstOpenAndSelectionStayInsideTheViewportWithoutEditingSingleSprite)
{
    Open();const auto before=I::ReadImporterEditingState(target);
    auto* window=ImGui::FindWindowByName("Sprite Editor##Sprite Editor");ASSERT_NE(window,nullptr);
    EXPECT_GE(window->Pos.x,0);EXPECT_GE(window->Pos.y,0);
    EXPECT_LE(window->Pos.x+window->Size.x,1280);EXPECT_LE(window->Pos.y+window->Size.y,900);
    Click(Pixel(2,2));Frame();
    EXPECT_EQ(I::ReadImporterEditingState(target).settings,before.settings);
    EXPECT_FALSE(I::ReadImporterEditingState(target).dirty);EXPECT_FALSE(undo.CanUndo());
    EXPECT_TRUE(panel->OwnsShortcuts());
    // Focus in the nested tools child still belongs to this panel.
    auto* tools=Child("/SpriteTools_");ASSERT_NE(tools,nullptr);
    ImGui::SetWindowFocus(tools->Name);Frame();EXPECT_TRUE(panel->OwnsShortcuts());
    panel->Close();Frame();EXPECT_FALSE(panel->OwnsShortcuts());
}

TEST_F(SpriteEditorUiFixture, ReopeningAnOffscreenWindowClampsItToTheWorkArea)
{
    Open();auto* window=ImGui::FindWindowByName("Sprite Editor##Sprite Editor");ASSERT_NE(window,nullptr);
    ImGui::SetWindowPos(window,{1000,700});ImGui::SetWindowSize(window,{1600,1200});
    panel->Close();Frame();panel->Open();Frame();Frame();
    EXPECT_GE(window->Pos.x,0);EXPECT_GE(window->Pos.y,0);
    EXPECT_LE(window->Pos.x+window->Size.x,1280);EXPECT_LE(window->Pos.y+window->Size.y,900);
    EXPECT_FALSE(undo.CanUndo());
}

TEST_F(SpriteEditorUiFixture, DragCommitsOnReleaseAndUndoRedoRestoresTheSameIdentity)
{
    SeedRectangle(1,1,3,3);Open();const auto original=Slices().front();
    Move(Pixel(2,2));Button(true);Move(Pixel(3,3));
    EXPECT_EQ(Slices().front().x,original.x);EXPECT_FALSE(undo.CanUndo());
    Button(false);auto moved=Slices().front();EXPECT_EQ(moved.x,2);EXPECT_EQ(moved.y,2);EXPECT_EQ(moved.id,original.id);
    ASSERT_TRUE(panel->ExecuteCommand("undo"));EXPECT_EQ(Slices().front().x,original.x);EXPECT_FALSE(undo.CanUndo());
    ASSERT_TRUE(panel->ExecuteCommand("redo"));EXPECT_EQ(Slices().front().x,moved.x);
}

TEST_F(SpriteEditorUiFixture, ResizeAndCreateUseTheCanvasAndCancelNeverCommitsLater)
{
    SeedRectangle(1,1,3,3);Open();
    Move(Pixel(3.98f,3.98f));Button(true);Move(Pixel(5,5));Button(false);
    EXPECT_EQ(Slices().front().width,4);EXPECT_EQ(Slices().front().height,4);
    ASSERT_TRUE(panel->ExecuteCommand("undo"));
    auto* tools=Child("/SpriteTools_");ASSERT_NE(tools,nullptr);
    const auto start=tools->DC.CursorStartPos;const float row=ImGui::GetFontSize()+ImGui::GetStyle().FramePadding.y*2;
    Click({start.x+8,start.y+row*1.5f+ImGui::GetStyle().ItemSpacing.y});
    Click(Pixel(6,6));EXPECT_EQ(Slices().size(),1);EXPECT_FALSE(undo.CanUndo());
    Click(Pixel(5.3f,5.3f));EXPECT_EQ(Slices().size(),1);EXPECT_FALSE(undo.CanUndo());
    Move(Pixel(5,5));Button(true);Move(Pixel(7,7));Button(false);
    ASSERT_EQ(Slices().size(),2);EXPECT_EQ(Slices().back().x,5);EXPECT_EQ(Slices().back().width,2);
    ASSERT_TRUE(panel->ExecuteCommand("undo"));
    Move(Pixel(5,5));Button(true);Move(Pixel(7,7));ASSERT_TRUE(panel->ExecuteCommand("cancel"));Button(false);
    EXPECT_EQ(Slices().size(),1);EXPECT_FALSE(undo.CanUndo());
}

TEST_F(SpriteEditorUiFixture, ConcurrentDraftEditCancelsAnUnreleasedDrag)
{
    SeedRectangle(1,1,3,3);Open();
    Move(Pixel(2,2));Button(true);Move(Pixel(3,3));
    NLS::Editor::Sprites::SpriteDocument other(root,&undo);ASSERT_TRUE(other.Open(image));
    auto changed=other.Read()->slices;changed.front().name="Inspector edit";
    ASSERT_TRUE(other.EditSlices(changed,"Edit from Inspector"));
    Frame();Button(false);
    EXPECT_EQ(Slices().front().name,"Inspector edit");EXPECT_EQ(Slices().front().x,1);
    ASSERT_TRUE(panel->ExecuteCommand("undo"));EXPECT_FALSE(undo.CanUndo());
}
}
#else
TEST(SpriteEditor, RequiresEditorTestHooks){GTEST_SKIP()<<"Editor test hooks are disabled.";}
#endif
