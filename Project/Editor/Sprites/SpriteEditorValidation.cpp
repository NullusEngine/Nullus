#include "SpriteEditorValidation.h"

#if defined(NLS_ENABLE_TEST_HOOKS)
#include "SpriteDocument.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/EditorAssetPath.h"
#include "Undo/EditorUndoService.h"
#include <Core/ServiceLocator.h>
#include <Engine/Assets/SpriteAsset.h>
#include <Json/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>

namespace NLS::Editor::Sprites
{
namespace A=NLS::Core::Assets;
struct SpriteEditorValidation::State
{
    std::filesystem::path output;
    std::string source;
    int phase=0,idleFrames=0;
    bool finished=false;
    size_t baselineCursor=0,baselineCount=0;
    bool baselineMultiple=false;
    std::chrono::steady_clock::time_point settle;
    A::AssetMeta baseline;
    std::vector<A::SpriteSliceDefinition> original,edited;
    std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
    nlohmann::json checks=nlohmann::json::array();

    void Finish(bool success,std::string error={})
    {
        finished=true;
        auto pending=output;pending+=".pending";
        std::ofstream file(pending,std::ios::trunc);
        file<<nlohmann::json{{"success",success},{"source",source},{"phase",phase},
            {"error",error},{"checks",checks},{"desktopInputInjected",false}}.dump(2)<<'\n';
        file.close();
        std::error_code ec;std::filesystem::rename(pending,output,ec);
    }
    bool Verify(SpriteDocument& document,const std::vector<A::SpriteSliceDefinition>& expected)
    {
        const auto disk=A::AssetMeta::Load(A::GetAssetMetaPath(document.Path()));
        const auto current=document.Read();
        if(!disk||!current||current->dirty||disk->id!=baseline.id||!document.Error().empty())
        {Finish(false,"Reimport did not leave clean metadata with the original asset identity.");return false;}
        std::string error;
        if(A::SerializeSpriteSlices(current->slices,error)!=A::SerializeSpriteSlices(expected,error)||
            current->multiple!=((phase==2||phase>=4)?baselineMultiple:true))
        {Finish(false,"The shared importer draft differs from the expected Sprite revision.");return false;}
        const auto history=NLS::Core::ServiceLocator::Get<Undo::EditorUndoService>().GetHistoryState();
        const auto expectedCursor=baselineCursor+((phase==1||phase==3)?2:1);
        if(history.poisoned||history.cursor!=expectedCursor||history.transactionCount!=baselineCount+2)
        {Finish(false,"The importer Undo history was invalidated or did not complete.");return false;}
        auto root=document.Path();
        for(const auto& part:std::filesystem::path(document.Target().resourcePath)){(void)part;root=root.parent_path();}
        Assets::AssetDatabaseFacade database(Assets::MakeProjectEditorAssetRoots(root));
        if(!database.Refresh()){Finish(false,"Cannot refresh the persisted artifact database.");return false;}
        const auto manifest=database.GetArtifactManifestForAssetPath(document.Target().resourcePath);
        if(!manifest||manifest->primarySubAssetKey!="texture:main")
        {Finish(false,"The primary Texture artifact was lost.");return false;}
        for(const auto& slice:expected)
        {
            A::SpriteArtifact key;key.id=slice.id;
            const auto* entry=manifest->FindSubAsset(key.SubAssetKey());
            if(!entry){Finish(false,"A persistent Sprite key was lost: "+slice.id);return false;}
            auto path=std::filesystem::path(entry->artifactPath);if(path.is_relative())path=root/path;
            NLS::Engine::Assets::SpriteAsset asset;
            if(!asset.Reload(path,entry->contentHash,error))
            {Finish(false,"Cannot load the imported Sprite: "+error);return false;}
            const auto& data=*asset.Capture();
            if(data.id!=slice.id||data.name!=slice.name||data.x!=slice.x||data.y!=slice.y||
                data.width!=slice.width||data.height!=slice.height||data.pivot!=slice.pivot||data.pixelsPerUnit!=slice.pixelsPerUnit||data.border!=slice.border)
            {Finish(false,"Published Sprite does not match the document: "+slice.id);return false;}
        }
        checks.push_back({{"phase",phase},{"sprites",expected.size()},{"metadataClean",true},{"artifactDescriptorsMatch",true}});
        return true;
    }
};

SpriteEditorValidation::SpriteEditorValidation()
{
    const auto* output=std::getenv("NLS_SPRITE_EDITOR_VALIDATION_OUTPUT");
    const auto* source=std::getenv("NLS_SPRITE_EDITOR_VALIDATION_SOURCE");
    if(output&&*output&&source&&*source)
    {m=std::make_unique<State>();m->output=output;m->source=source;}
}
SpriteEditorValidation::~SpriteEditorValidation()=default;
void SpriteEditorValidation::Tick(SpriteDocument& document)
{
    if(!m||m->finished)return;
    if(std::chrono::steady_clock::now()-m->start>std::chrono::seconds(120))
    {m->Finish(false,"Timed out waiting for the Editor import/Undo workflow.");return;}
    if(document.Target().resourcePath!=m->source)return;
    if(!NLS::Core::ServiceLocator::Contains<Undo::EditorUndoService>())
    {m->Finish(false,"The global Undo service is unavailable.");return;}
    auto& undo=NLS::Core::ServiceLocator::Get<Undo::EditorUndoService>();
    const auto state=document.Read();if(!state){m->Finish(false,document.Error());return;}
    if(state->reimporting||undo.IsPending())return;
    switch(m->phase)
    {
    case 0:
    {
        if(++m->idleFrames<4)return;
        if(state->dirty||state->slices.empty()){m->Finish(false,"Validation requires an unmodified, nonempty Sprite sheet.");return;}
        const auto baseline=A::AssetMeta::Load(A::GetAssetMetaPath(document.Path()));
        if(!baseline){m->Finish(false,"Cannot capture baseline metadata.");return;}
        m->baseline=*baseline;m->original=state->slices;m->edited=state->slices;
        m->baselineMultiple=state->multiple;
        m->baselineCursor=undo.GetCursor();m->baselineCount=m->baselineCursor;
        std::reverse(m->edited.begin(),m->edited.end());
        m->edited.front().name+="_workbench_validation";m->edited.front().pivot={.125f,.875f};
        if(m->edited.front().width>=2&&m->edited.front().height>=2)m->edited.front().border={1,1,1,1};
        m->checks.push_back({{"initialReadAndDrawClean",true}});
        if(!document.EditSlices(m->edited,"Validate Sprite Editor Apply")||!document.Apply())
        {m->Finish(false,document.Error());return;}
        m->phase=1;break;
    }
    case 1:
    case 3:
        if(!m->Verify(document,m->edited))return;
        if(const auto result=undo.RequestUndo();!result.Succeeded()){m->Finish(false,result.message);return;}
        ++m->phase;break;
    case 2:
        if(!m->Verify(document,m->original))return;
        if(const auto result=undo.RequestRedo();!result.Succeeded()){m->Finish(false,result.message);return;}
        ++m->phase;break;
    case 4:
        if(!m->Verify(document,m->original))return;
        if(const auto disk=A::AssetMeta::Load(A::GetAssetMetaPath(document.Path()));!disk||*disk!=m->baseline)
        {m->Finish(false,"The original importer metadata was not restored.");return;}
        m->settle=std::chrono::steady_clock::now();m->phase=5;break;
    case 5:
        if(std::chrono::steady_clock::now()-m->settle<std::chrono::seconds(2))return;
        if(!m->Verify(document,m->original))return;
        m->Finish(true);break;
    }
}
}
#endif
