#include "SpriteEditor.h"

#include "Sprites/SpriteDocument.h"
#include "Sprites/SpriteEditorValidation.h"
#include "Undo/EditorUndoService.h"
#include <Core/ServiceLocator.h>
#include <Rendering/Resources/Texture2D.h>
#include <Rendering/Resources/Loaders/TextureLoader.h>
#include <Rendering/Context/Driver.h>
#include <UI/UIManager.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <cfloat>
#include <cmath>

namespace NLS::Editor::Panels
{
namespace A = NLS::Core::Assets;
namespace
{
Undo::EditorUndoService* UndoService()
{ return NLS::Core::ServiceLocator::Contains<Undo::EditorUndoService>() ? &NLS::Core::ServiceLocator::Get<Undo::EditorUndoService>() : nullptr; }
ImVec2 Add(ImVec2 a,ImVec2 b){return {a.x+b.x,a.y+b.y};}
}

struct SpriteEditor::State
{
    Sprites::SpriteDocument document;
#if defined(NLS_ENABLE_TEST_HOOKS)
    Sprites::SpriteEditorValidation validation;
#endif
    std::unique_ptr<Render::Resources::Texture2D> preview;
    std::shared_ptr<Render::RHI::RHITextureView> view;
    std::string openPath,selected,error;
    A::SpriteGridSettings grid;
    int tool = 0,threshold = 0,minimumPixels = 1;
    float zoom = 1;
    ImVec2 pan{},startPixel{};
    bool drawing = false,moving = false,resizing = false;
    bool wasOpened = false,ensureVisible = true;
    uint64_t dragRevision = 0;
    uint64_t editSerial = 1;
    std::vector<A::SpriteSliceDefinition> working;
    A::SpriteSliceDefinition original;

    explicit State(std::filesystem::path root):document(std::move(root),UndoService()){}
    void CancelGesture()
    {
        drawing=moving=resizing=false;
        working.clear();
    }
    void Upload()
    {
        if(view || document.Pixels().empty() || !NLS::Core::ServiceLocator::Contains<Render::Context::Driver>())return;
        preview.reset(Render::Resources::Loaders::TextureLoader::CreateFromRgba8Memory(
            document.Pixels().data(),document.Pixels().size(),document.Width(),document.Height(),
            Render::Settings::ETextureFilteringMode::NEAREST,Render::Settings::ETextureFilteringMode::NEAREST,false));
        if(preview)view=preview->GetOrCreateExplicitTextureView("SpriteEditorPreview");
    }
    void CommitProperty(const Sprites::SpriteDocumentView& state,const A::SpriteSliceDefinition& value,
        const char* field,bool singleProperties)
    {
        const auto merge="sprite-editor:"+document.Target().assetId+":"+selected+":"+field+":"+std::to_string(editSerial);
        if(!state.multiple&&singleProperties)document.EditSingle(value.pivot,value.pixelsPerUnit,merge,value.border);
        else
        {
            auto changed=state.slices;
            for(auto& slice:changed)if(slice.id==selected){slice=value;break;}
            document.EditSlices(std::move(changed),"Edit Sprite "+std::string(field),merge);
        }
    }
    void Properties(const Sprites::SpriteDocumentView& state)
    {
        const auto found=std::find_if(state.slices.begin(),state.slices.end(),[&](const auto& value){return value.id==selected;});
        if(found==state.slices.end())return;
        ImGui::SeparatorText("Selected Sprite");
        auto value=*found;
        const auto field=[&](bool changed,const char* name,bool single=false)
        {
            if(changed)CommitProperty(state,value,name,single);
            if(ImGui::IsItemDeactivated())++editSerial;
        };
        ImGui::BeginDisabled(!state.multiple);
        ImGui::TextUnformatted("Name");ImGui::SetNextItemWidth(-FLT_MIN);
        field(ImGui::InputText("##Name",&value.name),"name");ImGui::EndDisabled();
        uint32_t rect[]{value.x,value.y,value.width,value.height};
        const char* names[]{"X","Y","Width","Height"};
        if(ImGui::BeginTable("SpriteRectangle",2,ImGuiTableFlags_SizingStretchSame))
        {
            for(size_t index=0;index<4;++index)
            {
                ImGui::TableNextColumn();ImGui::PushID(names[index]);
                ImGui::TextUnformatted(names[index]);ImGui::SetNextItemWidth(-FLT_MIN);
                if(ImGui::InputScalar("##Value",ImGuiDataType_U32,&rect[index]))
                {
                    value.x=rect[0];value.y=rect[1];value.width=rect[2];value.height=rect[3];
                    CommitProperty(state,value,names[index],false);
                }
                if(ImGui::IsItemDeactivated())++editSerial;
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::TextUnformatted("Pivot (X, Y)");ImGui::SetNextItemWidth(-FLT_MIN);
        field(ImGui::DragFloat2("##Pivot",value.pivot.data(),.005f,0,1,"%.3f",ImGuiSliderFlags_AlwaysClamp),"pivot",true);
        ImGui::TextUnformatted("Pixels Per Unit");ImGui::SetNextItemWidth(-FLT_MIN);
        field(ImGui::DragFloat("##PPU",&value.pixelsPerUnit,.1f,.0001f,1000000,"%.3f",ImGuiSliderFlags_AlwaysClamp),"PPU",true);
        ImGui::TextWrapped("Border (Left, Bottom, Right, Top)");
        if(ImGui::BeginTable("SpriteBorders",2,ImGuiTableFlags_SizingStretchSame))
        {
            const char* names[]{"Left","Bottom","Right","Top"};
            for(size_t index=0;index<4;++index)
            {
                ImGui::TableNextColumn();ImGui::PushID(names[index]);
                ImGui::TextUnformatted(names[index]);ImGui::SetNextItemWidth(-FLT_MIN);
                field(ImGui::InputScalar("##Border",ImGuiDataType_U32,&value.border[index]),"border",true);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if(ImGui::Button("Delete Sprite"))
        {
            auto changed=state.slices;std::erase_if(changed,[&](const auto& slice){return slice.id==selected;});
            if(document.EditSlices(std::move(changed),"Delete Sprite"))selected.clear();
        }
    }
    void SlicingControls()
    {
        if(ImGui::Button("Grid Slice..."))ImGui::OpenPopup("Grid slicing");
        if(ImGui::BeginPopup("Grid slicing"))
        {
            ImGui::InputScalar("Cell Width",ImGuiDataType_U32,&grid.cellWidth);
            ImGui::InputScalar("Cell Height",ImGuiDataType_U32,&grid.cellHeight);
            ImGui::InputScalar("Offset X",ImGuiDataType_U32,&grid.offsetX);ImGui::InputScalar("Offset Y",ImGuiDataType_U32,&grid.offsetY);
            ImGui::InputScalar("Padding X",ImGuiDataType_U32,&grid.paddingX);ImGui::InputScalar("Padding Y",ImGuiDataType_U32,&grid.paddingY);
            ImGui::Checkbox("Include partial cells",&grid.includePartialCells);
            ImGui::InputText("Name Prefix",&grid.namePrefix);
            if(ImGui::Button("Generate"))if(document.Grid(grid))ImGui::CloseCurrentPopup();
            if(!document.Error().empty())ImGui::TextWrapped("%s",document.Error().c_str());
            ImGui::EndPopup();
        }
        if(ImGui::Button("Auto Slice..."))ImGui::OpenPopup("Automatic slicing");
        if(ImGui::BeginPopup("Automatic slicing"))
        {
            ImGui::SliderInt("Alpha threshold",&threshold,0,254);
            ImGui::InputInt("Minimum opaque pixels",&minimumPixels);minimumPixels=std::max(1,minimumPixels);
            ImGui::TextWrapped("Each connected opaque region becomes a Sprite. Matching rectangles retain their identity.");
            if(ImGui::Button("Generate"))if(document.Automatic(uint8_t(threshold),uint32_t(minimumPixels)))ImGui::CloseCurrentPopup();
            if(!document.Error().empty())ImGui::TextWrapped("%s",document.Error().c_str());
            ImGui::EndPopup();
        }
    }
    void Canvas(const Sprites::SpriteDocumentView& state)
    {
        if((moving||resizing||drawing)&&state.revision!=dragRevision)
        {
            CancelGesture();
            error="Sprite settings changed during the drag. The drag was cancelled.";
        }
        const auto origin=ImGui::GetCursorScreenPos();auto available=ImGui::GetContentRegionAvail();
        available.x=std::max(1.f,available.x);available.y=std::max(1.f,available.y);
        ImGui::InvisibleButton("SpriteCanvas",available,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered=ImGui::IsItemHovered();const auto& io=ImGui::GetIO();
        auto* draw=ImGui::GetWindowDrawList();draw->PushClipRect(origin,Add(origin,available),true);
        draw->AddRectFilled(origin,Add(origin,available),IM_COL32(35,35,35,255));
        const float fit=std::min(available.x/document.Width(),available.y/document.Height());
        float scale=fit*zoom;
        if(hovered && io.MouseWheel!=0)
        {
            const float old=zoom;zoom=std::clamp(zoom*std::pow(1.15f,io.MouseWheel),.1f,32.f);
            pan.x*=zoom/old;pan.y*=zoom/old;scale=fit*zoom;
        }
        if(hovered&&ImGui::IsMouseDragging(ImGuiMouseButton_Middle)){pan.x+=io.MouseDelta.x;pan.y+=io.MouseDelta.y;}
        const ImVec2 size{document.Width()*scale,document.Height()*scale};
        const ImVec2 imageOrigin{origin.x+(available.x-size.x)*.5f+pan.x,origin.y+(available.y-size.y)*.5f+pan.y};
        if(view&&NLS::Core::ServiceLocator::Contains<UI::UIManager>())
        {
            const auto id=NLS::Core::ServiceLocator::Get<UI::UIManager>().ResolveTextureId(view);
            if(id)draw->AddImage(id,imageOrigin,Add(imageOrigin,size));
        }
        const auto point=[&](ImVec2 p){return ImVec2{std::clamp((p.x-imageOrigin.x)/scale,0.f,float(document.Width())),
            std::clamp((p.y-imageOrigin.y)/scale,0.f,float(document.Height()))};};
        const auto mouse=point(io.MousePos);
        const bool overImage=io.MousePos.x>=imageOrigin.x&&io.MousePos.y>=imageOrigin.y&&io.MousePos.x<=imageOrigin.x+size.x&&io.MousePos.y<=imageOrigin.y+size.y;
        if(hovered&&overImage&&ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            startPixel=mouse;working=state.slices;moving=resizing=drawing=false;dragRevision=state.revision;
            if(tool==1)drawing=true;
            else
            {
                for(auto it=state.slices.rbegin();it!=state.slices.rend();++it)
                    if(mouse.x>=it->x&&mouse.y>=it->y&&mouse.x<=it->x+it->width&&mouse.y<=it->y+it->height)
                    {
                        selected=it->id;original=*it;
                        resizing=std::abs((mouse.x-it->x-it->width)*scale)<9&&std::abs((mouse.y-it->y-it->height)*scale)<9;
                        moving=!resizing;break;
                    }
            }
        }
        if((moving||resizing)&&ImGui::IsMouseDown(ImGuiMouseButton_Left))
            for(auto& slice:working)if(slice.id==selected)
            {
                const int dx=int(std::round(mouse.x-startPixel.x)),dy=int(std::round(mouse.y-startPixel.y));
                if(resizing){slice.width=uint32_t(std::clamp(int(original.width)+dx,1,int(document.Width()-slice.x)));
                    slice.height=uint32_t(std::clamp(int(original.height)+dy,1,int(document.Height()-slice.y)));}
                else{slice.x=uint32_t(std::clamp(int(original.x)+dx,0,int(document.Width()-slice.width)));
                    slice.y=uint32_t(std::clamp(int(original.y)+dy,0,int(document.Height()-slice.height)));}
            }
        if((moving||resizing||drawing)&&ImGui::IsKeyPressed(ImGuiKey_Escape))CancelGesture();
        const auto& visible=(moving||resizing)?working:state.slices;
        for(const auto& slice:visible)
        {
            const ImVec2 a{imageOrigin.x+slice.x*scale,imageOrigin.y+slice.y*scale};
            const ImVec2 b{a.x+slice.width*scale,a.y+slice.height*scale};
            draw->AddRect(a,b,slice.id==selected?IM_COL32(255,215,64,255):IM_COL32(80,180,255,255),0,0,slice.id==selected?2.f:1.f);
            if(slice.id==selected){draw->AddRectFilled({b.x-4,b.y-4},{b.x+4,b.y+4},IM_COL32(255,215,64,255));
                if(slice.border!=std::array<uint32_t,4>{})
                {
                    const auto color=IM_COL32(120,255,140,255);
                    for(const float x:{a.x+slice.border[0]*scale,b.x-slice.border[2]*scale})draw->AddLine({x,a.y},{x,b.y},color);
                    for(const float y:{a.y+slice.border[3]*scale,b.y-slice.border[1]*scale})draw->AddLine({a.x,y},{b.x,y},color);
                }
                const ImVec2 pivot{a.x+slice.pivot[0]*(b.x-a.x),b.y-slice.pivot[1]*(b.y-a.y)};
                draw->AddCircle(pivot,4,IM_COL32(255,215,64,255));}
        }
        if(drawing)
        {
            const ImVec2 a{imageOrigin.x+std::min(startPixel.x,mouse.x)*scale,imageOrigin.y+std::min(startPixel.y,mouse.y)*scale};
            const ImVec2 b{imageOrigin.x+std::max(startPixel.x,mouse.x)*scale,imageOrigin.y+std::max(startPixel.y,mouse.y)*scale};
            draw->AddRect(a,b,IM_COL32(255,215,64,255),0,0,2);
        }
        if((moving||resizing||drawing)&&ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            bool changed = false;
            if(drawing)
            {
                A::SpriteSliceDefinition slice;
                // ImGui rounds pointer positions to screen pixels. Snap both
                // ends to source pixels, as move/resize do, before comparing.
                // A stationary click inside a source pixel has zero area.
                const auto ax=uint32_t(std::round(startPixel.x)),ay=uint32_t(std::round(startPixel.y));
                const auto bx=uint32_t(std::round(mouse.x)),by=uint32_t(std::round(mouse.y));
                slice.x=std::min(ax,bx);slice.y=std::min(ay,by);
                slice.width=std::max(ax,bx)-slice.x;slice.height=std::max(ay,by)-slice.y;
                if(slice.width&&slice.height)
                {
                    if(!state.multiple&&!working.empty()){const auto rect=slice;slice=working.front();slice.x=rect.x;slice.y=rect.y;slice.width=rect.width;slice.height=rect.height;working.clear();}
                    else{slice.id=Guid::New().ToString();slice.name=document.Path().stem().string()+"_"+std::to_string(working.size());}
                    working.push_back(slice);selected=slice.id;
                    changed = true;
                }
            }
            else for(const auto& slice:working)if(slice.id==selected)
                changed = slice.x!=original.x||slice.y!=original.y||slice.width!=original.width||slice.height!=original.height;
            if(changed)document.EditSlices(working,drawing?"Create Sprite rectangle":resizing?"Resize Sprite rectangle":"Move Sprite rectangle");
            CancelGesture();
        }
        draw->PopClipRect();
    }
};

SpriteEditor::SpriteEditor(const std::string& title,bool opened,const UI::PanelWindowSettings& settings,std::filesystem::path root)
    :PanelWindow(title,opened,settings),m(std::make_unique<State>(std::move(root))) {autoSize=false;minSize={760,500};}
SpriteEditor::~SpriteEditor()=default;
void SpriteEditor::_Draw_Impl()
{
    if(IsOpened()&&(!m->wasOpened||m->ensureVisible))
    {
        const auto* viewport=ImGui::GetMainViewport();
        const ImVec2 available{std::max(1.f,viewport->WorkSize.x-24.f),std::max(1.f,viewport->WorkSize.y-24.f)};
        minSize={std::min(760.f,available.x),std::min(500.f,available.y)};
        ImVec2 size{std::min(1120.f,available.x),std::min(780.f,available.y)};
        ImVec2 position{viewport->WorkPos.x+(viewport->WorkSize.x-size.x)*.5f,viewport->WorkPos.y+(viewport->WorkSize.y-size.y)*.5f};
        const auto* window=ImGui::FindWindowByName((name+GetPanelID()).c_str());
        if(window&&!window->DockIsActive)
        {
            size={std::clamp(window->Size.x,minSize.x,available.x),std::clamp(window->Size.y,minSize.y,available.y)};
            position={std::clamp(window->Pos.x,viewport->WorkPos.x+12.f,viewport->WorkPos.x+viewport->WorkSize.x-size.x-12.f),
                std::clamp(window->Pos.y,viewport->WorkPos.y+12.f,viewport->WorkPos.y+viewport->WorkSize.y-size.y-12.f)};
        }
        if(!window||!window->DockIsActive)
        {
            ImGui::SetNextWindowSize(size);
            ImGui::SetNextWindowPos(position);
        }
        m->ensureVisible=false;
    }
    m->wasOpened=IsOpened();
    PanelWindow::_Draw_Impl();
    if(!IsOpened())m->CancelGesture();
}
bool SpriteEditor::OpenDocument(const std::filesystem::path& path)
{
    if(!m->document.Path().empty())if(const auto current=m->document.Read();current&&(current->dirty||current->reimporting))
    {m->error="Apply or revert the current Sprite changes before opening another texture.";Open();Focus();return false;}
    if(!m->document.Open(path)){m->error=m->document.Error();Open();Focus();return false;}
    m->openPath=m->document.Target().resourcePath;m->selected.clear();m->CancelGesture();m->preview.reset();m->view.reset();
    if(const auto initial=m->document.Read();initial&&!initial->slices.empty())m->selected=initial->slices.front().id;
    m->zoom=1;m->pan={};m->error.clear();m->ensureVisible=true;Open();Focus();return true;
}
bool SpriteEditor::OwnsShortcuts()const{return IsOpened()&&HasCurrentKeyboardFocus();}
bool SpriteEditor::ExecuteCommand(std::string_view command)
{
    if(command=="save"||command=="revert"||command=="undo"||command=="redo"||command=="delete"||command=="frame-all")
    m->CancelGesture();
    if(command=="save")return m->document.Apply();
    if(command=="revert")return m->document.Revert();
    if(command=="frame-all"){m->zoom=1;m->pan={};return true;}
    if(command=="cancel"){m->CancelGesture();return true;}
    if(auto* undo=UndoService())
    {
        if(command=="undo")return undo->RequestUndo().Succeeded();
        if(command=="redo")return undo->RequestRedo().Succeeded();
    }
    if(command=="delete")
    {
        auto state=m->document.Read();if(!state||m->selected.empty())return false;
        std::erase_if(state->slices,[&](const auto& slice){return slice.id==m->selected;});
        if(!m->document.EditSlices(std::move(state->slices),"Delete Sprite"))return false;
        m->selected.clear();return true;
    }
    return false;
}
void SpriteEditor::OnBeforeDrawWidgets()
{
    ImGui::TextUnformatted("Texture");ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(120.f,ImGui::GetContentRegionAvail().x-ImGui::CalcTextSize("Open").x-32.f));
    ImGui::InputText("##Texture",&m->openPath);ImGui::SameLine();
    if(ImGui::Button("Open"))OpenDocument(m->openPath);
    if(m->document.Path().empty()){ImGui::TextWrapped("Open a project texture here, or choose Open Sprite Editor in the Asset Browser.");return;}
#if defined(NLS_ENABLE_TEST_HOOKS)
    m->validation.Tick(m->document);
#endif
    auto state=m->document.Read();
    if(!state)
    {
        ImGui::TextWrapped("%s",m->document.Error().c_str());
        if(ImGui::Button("Revert Importer Changes"))m->document.Revert();
        return;
    }
    m->Upload();
    ImGui::BeginDisabled(state->reimporting);
    ImGui::BeginDisabled(!state->dirty);
    if(ImGui::Button("Apply & Reimport"))ExecuteCommand("save");ImGui::EndDisabled();ImGui::SameLine();
    if(ImGui::Button("Revert"))ExecuteCommand("revert");ImGui::SameLine();
    if(ImGui::Button("Reload Source"))
    {
        if(state->dirty)m->error="Revert or apply changes before reloading the source image.";
        else if(ExecuteCommand("revert"))OpenDocument(m->document.Path());
    }
    ImGui::SameLine();if(auto* undo=UndoService())
    {
        ImGui::BeginDisabled(!undo->CanUndo());if(ImGui::Button("Undo"))ExecuteCommand("undo");ImGui::EndDisabled();
        ImGui::SameLine();ImGui::BeginDisabled(!undo->CanRedo());if(ImGui::Button("Redo"))ExecuteCommand("redo");ImGui::EndDisabled();
    }
    ImGui::EndDisabled();
    state=m->document.Read();if(!state)return;
    ImGui::Text("%u x %u | %zu Sprites%s",m->document.Width(),m->document.Height(),state->slices.size(),state->reimporting?" | Importing...":state->dirty?" | Modified":"");
    if(m->document.SourceChanged())ImGui::TextWrapped("Source image changed. Reload it before applying Sprite edits.");
    if(!m->error.empty())ImGui::TextWrapped("%s",m->error.c_str());
    if(!m->document.Error().empty())ImGui::TextWrapped("%s",m->document.Error().c_str());
    ImGui::BeginDisabled(state->reimporting);
    const float sidebar=std::clamp(ImGui::GetContentRegionAvail().x*.32f,280.f,360.f);
    ImGui::BeginChild("SpriteTools",{sidebar,0},true);
    ImGui::RadioButton("Select / Move",&m->tool,0);ImGui::RadioButton("Create",&m->tool,1);
    m->SlicingControls();ImGui::Separator();
    const float listHeight=std::min(180.f,ImGui::GetContentRegionAvail().y*.35f);
    ImGui::BeginChild("SpriteList",{0,std::max(40.f,listHeight)},true);
    ImGuiListClipper clipper;clipper.Begin(int(state->slices.size()));
    while(clipper.Step())for(int index=clipper.DisplayStart;index<clipper.DisplayEnd;++index)
    {const auto& slice=state->slices[index];ImGui::PushID(slice.id.c_str());if(ImGui::Selectable(slice.name.c_str(),slice.id==m->selected))m->selected=slice.id;ImGui::PopID();}
    ImGui::EndChild();m->Properties(*state);ImGui::EndChild();ImGui::SameLine();
    ImGui::BeginChild("SpritePreview",{0,0},true,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    m->Canvas(*state);ImGui::EndChild();ImGui::EndDisabled();
}
}
