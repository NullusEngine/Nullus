#include "AnimationWorkbench.h"
#include "Animation/AnimationDocument.h"
#include "Animation/AnimationPreview.h"
#include "Core/EditorActions.h"
#include <AnimationCompiler/AnimationSourceCompiler.h>
#include <Rendering/Assets/SceneModelReader.h>
#include <Core/ServiceLocator.h>
#include <Assets/NativeArtifactContainer.h>
#include <ImGuiExtensions/ImGuiNodeEditor/imgui_node_editor.h>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <fstream>

namespace NLS::Editor::Panels
{
namespace ed=ax::NodeEditor;
using namespace Animation;
using namespace AnimationTools;
namespace
{
Undo::EditorUndoService* UndoService(){return NLS::Core::ServiceLocator::Contains<Undo::EditorUndoService>()?&NLS::Core::ServiceLocator::Get<Undo::EditorUndoService>():nullptr;}
const char* NodeNames[]={"Reference Pose","Sequence Player","Sequence Evaluator","BlendSpace Player","Blend","Multi Blend","Layered Blend","Additive","State Machine","Save Cached Pose","Use Cached Pose","Output Pose"};
bool IdInput(const char* label,uint64_t& value){return ImGui::InputScalar(label,ImGuiDataType_U64,&value);}
bool Seconds(const char* label,double& value){return ImGui::DragScalar(label,ImGuiDataType_Double,&value,.01f,nullptr,nullptr,"%.3f");}
}
struct AnimationWorkbench::State
{
    struct Session
    {
        AnimationDocument document{UndoService()};
        ed::EditorContext* editor=nullptr;
        uint64_t selected=0,previewRevision=0;
        int newNode=1;
        std::string path;
        std::string renderedSource;
        AnimationCompiler::SourceCompileResult compiled;
        std::map<std::string,Value> previewParameters;
        bool opened=true,positionsLoaded=false;
        Session(){ed::Config config;config.SettingsFile=nullptr;editor=ed::CreateEditor(&config);}
        ~Session(){ed::DestroyEditor(editor);}
    };
    std::filesystem::path root;
    AnimationPreview* preview;
    NLS::Core::Assets::ArtifactDatabase database;
    std::map<std::string,std::unique_ptr<Session>> sessions;
    std::string active,error,openPath,previewPath,savePath,closeKey;
    bool paused=true,loop=true;
    float speed=1;
    size_t clip=0;
    uint64_t selectedNotify=0;
    bool selectedNotifyEnd=false;
    std::shared_ptr<std::vector<NotifyEvent>> events=std::make_shared<std::vector<NotifyEvent>>();
    void BindPreviewEvents()
    {
        if(auto* animator=preview->Animator())
        {
            animator->SetLooping(loop);animator->SetPlaybackSpeed(speed);animator->Pause(paused);
            animator->NotifyEvent.AddListener([weak=std::weak_ptr<std::vector<NotifyEvent>>(events)](const auto& event)
            {if(auto log=weak.lock()){log->push_back(event);if(log->size()>128)log->erase(log->begin());}});
        }
    }
    void PreviewParameters(Session& session)
    {
        auto* animator=preview->Animator();if(!animator||!session.compiled.program||!ImGui::CollapsingHeader("Preview Parameters"))return;
        for(const auto& parameter:session.compiled.program->parameters)
        {
            auto [it,inserted]=session.previewParameters.try_emplace(parameter.name,parameter.initial);auto& value=it->second;
            ImGui::PushID(parameter.name.c_str());
            switch(parameter.type)
            {
            case ParameterType::Float:if(ImGui::DragFloat(parameter.name.c_str(),&value[0],.01f))animator->SetFloat(parameter.name,value[0]);break;
            case ParameterType::Bool:{bool enabled=value[0]!=0;if(ImGui::Checkbox(parameter.name.c_str(),&enabled)){value[0]=enabled?1:0;animator->SetBool(parameter.name,enabled);}break;}
            case ParameterType::Vector:if(ImGui::DragFloat3(parameter.name.c_str(),value.data(),.01f))animator->SetVector(parameter.name,{value[0],value[1],value[2]});break;
            case ParameterType::Trigger:if(ImGui::Button(parameter.name.c_str()))animator->SetTrigger(parameter.name);break;
            }
            ImGui::PopID();
        }
    }
    bool Reference(const char* label,std::string& value,NLS::Core::Assets::ArtifactType type)
    {
        bool changed=ImGui::InputText(label,&value);ImGui::SameLine();ImGui::PushID(label);
        if(ImGui::BeginCombo("##pick","Choose"))
        {
            for(const auto& record:database.GetRecords())if(record.artifactType==type)
            {const auto reference=record.sourceAssetId.ToString()+"#"+record.subAssetKey;if(ImGui::Selectable((record.displayName+"##"+reference).c_str(),value==reference)){value=reference;changed=true;}}
            ImGui::EndCombo();
        }
        ImGui::PopID();return changed;
    }
    void Refresh(){database.Load(root/"Library"/"ArtifactDB");}
    bool Compile(Session& session)
    {
        Refresh();AnimationCompiler::SourceCompileResult result;
        if(!AnimationCompiler::CompileSource(root,database,"editor",session.document.State().path,session.document.State().source,result,error))return false;
        if(result.program){AnimationInstance validation;if(!validation.SetProgram(result.program,error))return false;}
        session.compiled=std::move(result);error.clear();return true;
    }
    void New(int type)
    {
        auto session=std::make_unique<Session>();Asset asset;std::string extension;
        if(type==0){asset=AnimationCompiler::CreateGraph();extension=".animgraph";}
        else if(type==1){ClipAsset a;a.id=NLS::Guid::New().ToString();asset=a;extension=".animclip";}
        else if(type==2){BlendSpaceAsset a;a.id=NLS::Guid::New().ToString();asset=a;extension=".blendspace";}
        else {BoneMaskAsset a;a.id=NLS::Guid::New().ToString();asset=a;extension=".bonemask";}
        const auto key=NLS::Guid::New().ToString();session->path=(root/"Assets"/("Animation-"+key.substr(0,8)+extension)).string();session->document.New(std::move(asset),session->path);active=key;sessions.emplace(key,std::move(session));
    }
    void ConditionEditor(Condition& condition,uint32_t depth=0)
    {
        if(depth>32){ImGui::TextUnformatted("Condition nesting limit reached");return;}
        int op=int(condition.operation);
        if(ImGui::Combo("Comparison",&op,"True\0False\0Less\0Less or Equal\0Greater\0Greater or Equal\0Equal\0Not Equal\0All\0Any\0Not\0"))condition.operation=Compare(op);
        if(op<8){ImGui::InputText("Condition Parameter",&condition.parameter);ImGui::DragFloat("Compare Value",&condition.value,.01f);}
        else
        {
            for(size_t i=0;i<condition.children.size();++i)
            {ImGui::PushID(int(i));if(ImGui::TreeNode("Condition")){ConditionEditor(condition.children[i],depth+1);ImGui::TreePop();}ImGui::SameLine();const bool remove=ImGui::SmallButton("Remove");ImGui::PopID();if(remove){condition.children.erase(condition.children.begin()+i);break;}}
            if((condition.operation!=Compare::Not||condition.children.empty())&&ImGui::Button("Add Condition"))condition.children.push_back({});
        }
    }
    void Graph(Session& session,GraphAsset& graph)
    {
        Reference("Skeleton",graph.skeleton,NLS::Core::Assets::ArtifactType::Skeleton);
        if(ImGui::CollapsingHeader("Parameters"))
        {
            for(size_t i=0;i<graph.parameters.size();++i)
            {auto& p=graph.parameters[i];ImGui::PushID(int(i));ImGui::InputText("Name",&p.name);int type=int(p.type);if(ImGui::Combo("Type",&type,"Float\0Bool\0Vector\0Trigger\0"))p.type=ParameterType(type);ImGui::DragFloat4("Default",p.initial.data(),.01f);ImGui::PopID();}
            if(ImGui::Button("Add Parameter")){uint64_t id=1;for(auto p:graph.parameters)id=std::max(id,p.id+1);graph.parameters.push_back({id,"Parameter"+std::to_string(id),ParameterType::Float,{}});}
        }
        ImGui::Combo("New Node",&session.newNode,NodeNames,IM_ARRAYSIZE(NodeNames));ImGui::SameLine();
        if(ImGui::Button("Add Node"))
        {
            Node node;node.id=1;for(const auto& n:graph.nodes)node.id=std::max(node.id,n.id+1);node.kind=NodeKind(session.newNode);node.name=NodeNames[session.newNode];
            const auto kind=node.kind;if(kind==NodeKind::Blend||kind==NodeKind::LayeredBlend||kind==NodeKind::Additive)node.inputs={0,0};else if(kind==NodeKind::OutputPose||kind==NodeKind::SaveCachedPose||kind==NodeKind::UseCachedPose)node.inputs={0};
            graph.nodes.push_back(node);session.selected=node.id;
        }
        ed::SetCurrentEditor(session.editor);ed::Begin("Animation Graph",{0,300});
        for(auto& node:graph.nodes)
        {
            float contribution=0;
            if(session.compiled.program)
                if(auto* animator=preview->Animator())if(auto frame=animator->CaptureFrame())
                    for(size_t i=0;i<session.compiled.program->nodes.size()&&i<frame->nodeWeights.size();++i)
                        if(session.compiled.program->nodes[i].source.id==node.id){contribution=frame->nodeWeights[i];break;}
            if(contribution>.01f)ed::PushStyleColor(ed::StyleColor_NodeBg,ImVec4(.08f,.22f,.16f,1));
            ed::BeginNode(ed::NodeId(node.id));ImGui::TextUnformatted((node.name.empty()?NodeNames[int(node.kind)]:node.name).c_str());
            if(contribution>.01f)ImGui::Text("Weight %.3f",contribution);
            for(size_t i=0;i<node.inputs.size();++i){ed::BeginPin(ed::PinId(node.id*32+i+1),ed::PinKind::Input);ImGui::Text("Pose %zu",i);ed::EndPin();}
            ed::BeginPin(ed::PinId(node.id*32+31),ed::PinKind::Output);ImGui::TextUnformatted("Pose");ed::EndPin();ed::EndNode();
            if(contribution>.01f)ed::PopStyleColor();
            if(!session.positionsLoaded)ed::SetNodePosition(ed::NodeId(node.id),{node.position[0],node.position[1]});
        }
        session.positionsLoaded=true;
        for(const auto& node:graph.nodes)for(size_t i=0;i<node.inputs.size();++i)if(node.inputs[i])ed::Link(ed::LinkId(node.id*32+i+1),ed::PinId(node.inputs[i]*32+31),ed::PinId(node.id*32+i+1));
        if(ed::BeginCreate())
        {
            ed::PinId a,b;if(ed::QueryNewLink(&a,&b))
            {
                uint64_t first=a.Get(),second=b.Get();if(first%32!=31)std::swap(first,second);
                const auto destination=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& node){return node.id==second/32;});
                if(first%32==31&&second%32>0&&second%32<31&&destination!=graph.nodes.end()&&second%32-1<destination->inputs.size()&&first/32!=second/32)
                {if(ed::AcceptNewItem())destination->inputs[size_t(second%32-1)]=first/32;}else ed::RejectNewItem();
            }
            ed::EndCreate();
        }
        if(ed::BeginDelete())
        {
            ed::LinkId link;while(ed::QueryDeletedLink(&link))if(ed::AcceptDeletedItem())for(auto& node:graph.nodes)for(size_t i=0;i<node.inputs.size();++i)if(node.id*32+i+1==link.Get())node.inputs[i]=0;
            ed::NodeId removed;while(ed::QueryDeletedNode(&removed))if(ed::AcceptDeletedItem())
            {std::erase_if(graph.nodes,[&](const auto& n){return n.id==removed.Get();});for(auto& n:graph.nodes)for(auto& input:n.inputs)if(input==removed.Get())input=0;}
            ed::EndDelete();
        }
        ed::NodeId selected;if(ed::GetSelectedNodes(&selected,1)>0)session.selected=selected.Get();
        for(auto& n:graph.nodes){const auto p=ed::GetNodePosition(ed::NodeId(n.id));n.position={p.x,p.y};}
        ed::End();ed::SetCurrentEditor(nullptr);
        auto node=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==session.selected;});
        if(node!=graph.nodes.end())
        {
            ImGui::SeparatorText("Node Properties");ImGui::InputText("Name",&node->name);
            if(node->kind==NodeKind::SequencePlayer||node->kind==NodeKind::SequenceEvaluator)Reference("Clip",node->clip,NLS::Core::Assets::ArtifactType::AnimationClip);
            if(node->kind==NodeKind::BlendSpacePlayer){Reference("Blend Space",node->blendSpace,NLS::Core::Assets::ArtifactType::BlendSpace);ImGui::InputText("X Parameter",&node->xParameter);ImGui::InputText("Y Parameter",&node->yParameter);}
            if(node->kind==NodeKind::LayeredBlend)Reference("Mask",node->mask,NLS::Core::Assets::ArtifactType::BoneMask);
            ImGui::InputText("Control Parameter",&node->parameter);ImGui::SliderFloat("Weight",&node->weight,0,1);Seconds("Sample Time",node->sampleTime);ImGui::Checkbox("Loop",&node->loop);
            if(node->kind==NodeKind::MultiBlend)
            {if(ImGui::Button("Add Blend Input")){node->inputs.push_back(0);node->weights.push_back(1);}for(size_t i=0;i<node->weights.size();++i){ImGui::PushID(int(i));ImGui::SliderFloat("Weight",&node->weights[i],0,1);ImGui::PopID();}}
            if(node->kind==NodeKind::StateMachine)
            {
                IdInput("Machine ID",node->machine);if(!node->machine&&ImGui::Button("Create State Machine")){node->machine=node->id;StateMachine machine;machine.id=node->id;machine.name="Locomotion";graph.machines.push_back(machine);}
            }
            if(ImGui::Button("Use as Graph Output"))graph.output=node->id;
        }
        for(auto& machine:graph.machines)
        {
            ImGui::PushID(int(machine.id));if(ImGui::CollapsingHeader(("State Machine: "+machine.name).c_str()))
            {
                ImGui::InputText("Machine Name",&machine.name);IdInput("Initial State",machine.initial);
                for(auto& state:machine.states){ImGui::PushID(int(state.id));ImGui::Text("State %llu",state.id);ImGui::InputText("State Name",&state.name);IdInput("Pose Node",state.pose);ImGui::PopID();}
                if(ImGui::Button("Add State")){uint64_t id=1;for(auto s:machine.states)id=std::max(id,s.id+1);machine.states.push_back({id,"State "+std::to_string(id),0});if(!machine.initial)machine.initial=id;}
                for(auto& t:machine.transitions)
                {
                    ImGui::PushID(int(t.id));ImGui::SeparatorText("Transition");IdInput("From (0 = Any State)",t.from);IdInput("To",t.to);
                    ConditionEditor(t.condition);
                    Seconds("Blend Duration",t.duration);Seconds("Exit Time (-1 = disabled)",t.exitTime);ImGui::InputInt("Priority",&t.priority);ImGui::Checkbox("Interruptible",&t.interruptible);
                    int curve=int(t.curve);if(ImGui::Combo("Blend Curve",&curve,"Linear\0Smooth Step\0"))t.curve=BlendCurve(curve);ImGui::PopID();
                }
                if(ImGui::Button("Add Transition")){Transition t;t.id=1;for(auto v:machine.transitions)t.id=std::max(t.id,v.id+1);machine.transitions.push_back(t);}
            }
            ImGui::PopID();
        }
    }
    void ClipEditor(ClipAsset& asset)
    {
        Reference("Source Clip",asset.source,NLS::Core::Assets::ArtifactType::AnimationClip);Seconds("Trim Start",asset.start);Seconds("Trim End (-1 = source end)",asset.end);ImGui::Checkbox("Loop",&asset.loop);
        ImGui::SeparatorText("Notify Timeline");
        double duration=asset.end>=0?asset.end-asset.start:1;
        if(auto model=preview->Model();asset.end<0&&model&&!model->clips.empty())duration=model->clips[std::min(clip,model->clips.size()-1)].duration-asset.start;
        duration=std::max(duration,.001);const auto origin=ImGui::GetCursorScreenPos();const ImVec2 size{std::max(120.f,ImGui::GetContentRegionAvail().x),100};
        ImGui::InvisibleButton("Notify Timeline Canvas",size);auto* draw=ImGui::GetWindowDrawList();draw->AddRectFilled(origin,{origin.x+size.x,origin.y+size.y},IM_COL32(27,30,36,255));
        auto x=[&](double time){return origin.x+float(std::clamp(time/duration,0.,1.))*size.x;};
        for(int tick=0;tick<=8;++tick){const float px=x(duration*tick/8);draw->AddLine({px,origin.y},{px,origin.y+size.y},IM_COL32(65,68,75,255));}
        if(ImGui::IsItemClicked())
        {
            selectedNotify=0;float distance=10;const float pointer=ImGui::GetIO().MousePos.x;
            for(const auto& n:asset.notifies)for(bool end:{false,true}){const float d=std::abs(pointer-x(end?n.end:n.start));if(d<distance){distance=d;selectedNotify=n.id;selectedNotifyEnd=end;}}
            if(!selectedNotify)if(auto* animator=preview->Animator())animator->Seek(float(std::clamp((pointer-origin.x)/size.x,0.f,1.f)*duration));
        }
        if(ImGui::IsItemActive()&&ImGui::IsMouseDragging(ImGuiMouseButton_Left,0))
            for(auto& n:asset.notifies)if(n.id==selectedNotify)
            {
                const double time=std::clamp(double((ImGui::GetIO().MousePos.x-origin.x)/size.x)*duration,0.,duration);
                if(selectedNotifyEnd)n.end=std::max(n.start,time);else {const bool instant=n.start==n.end;n.start=std::min(time,instant?duration:n.end);if(instant)n.end=n.start;}
            }
        for(size_t i=0;i<asset.notifies.size();++i)
        {const auto& n=asset.notifies[i];const float y=origin.y+12+float(i%3)*28;const auto color=n.id==selectedNotify?IM_COL32(255,190,80,255):IM_COL32(100,190,255,255);draw->AddRectFilled({x(n.start),y},{std::max(x(n.start)+3,x(n.end)),y+16},color);draw->AddText({x(n.start)+4,y},IM_COL32_WHITE,n.name.c_str());}
        for(size_t i=0;i<asset.notifies.size();++i)
        {auto& n=asset.notifies[i];ImGui::PushID(int(i));ImGui::InputText("Event",&n.name);Seconds("Start",n.start);Seconds("End (same = instant)",n.end);ImGui::InputText("Payload",&n.payload);ImGui::SliderFloat("Weight Threshold",&n.threshold,0,1);ImGui::SameLine();if(ImGui::Button("Remove")){asset.notifies.erase(asset.notifies.begin()+i);ImGui::PopID();break;}ImGui::PopID();}
        if(ImGui::Button("Add Notify")){Notify n;n.id=1;for(auto v:asset.notifies)n.id=std::max(n.id,v.id+1);n.name="Notify";asset.notifies.push_back(n);}
        ImGui::SeparatorText("Curves");for(size_t i=0;i<asset.curves.size();++i)
        {
            auto& curve=asset.curves[i];ImGui::PushID(int(i));ImGui::InputText("Curve Name",&curve.name);
            if(!curve.channel.times.empty()&&curve.channel.times.size()==curve.channel.values.size())
            {std::array<float,96> values{};for(size_t k=0;k<values.size();++k)values[k]=curve.channel.Sample(duration*k/(values.size()-1),{})[0];ImGui::PlotLines("Curve",values.data(),int(values.size()),0,nullptr,FLT_MAX,FLT_MAX,{0,70});}
            for(size_t k=0;k<curve.channel.times.size();++k){ImGui::PushID(int(k));Seconds("Time",curve.channel.times[k]);ImGui::DragFloat("Value",&curve.channel.values[k][0],.01f);ImGui::PopID();}
            if(ImGui::Button("Add Key")){curve.channel.times.push_back(curve.channel.times.empty()?0:curve.channel.times.back()+.1);curve.channel.values.push_back({});}ImGui::PopID();
        }
        if(ImGui::Button("Add Curve"))asset.curves.push_back({"Curve",{}});
    }
    void BlendEditor(BlendSpaceAsset& asset)
    {
        int axes=int(asset.dimensions);if(ImGui::Combo("Dimensions",&axes,"Unused\0One Dimension\0Two Dimensions\0"))asset.dimensions=uint32_t(std::clamp(axes,1,2));
        for(size_t i=0;i<asset.samples.size();++i)
        {auto& sample=asset.samples[i];ImGui::PushID(int(i));Reference("Clip",sample.clip,NLS::Core::Assets::ArtifactType::AnimationClip);ImGui::DragFloat("X",&sample.x,.01f);if(asset.dimensions==2)ImGui::DragFloat("Y",&sample.y,.01f);if(ImGui::Button("Remove Sample")){asset.samples.erase(asset.samples.begin()+i);ImGui::PopID();break;}ImGui::PopID();}
        if(ImGui::Button("Add Sample"))asset.samples.push_back({});
        const auto p=ImGui::GetCursorScreenPos();const ImVec2 size{ImGui::GetContentRegionAvail().x,160};auto* draw=ImGui::GetWindowDrawList();draw->AddRectFilled(p,{p.x+size.x,p.y+size.y},IM_COL32(30,33,40,255));
        for(const auto& s:asset.samples)draw->AddCircleFilled({p.x+size.x*.5f+s.x*30,p.y+size.y*.5f-s.y*30},5,IM_COL32(100,180,255,255));ImGui::Dummy(size);
    }
    void MaskEditor(BoneMaskAsset& asset)
    {
        Reference("Skeleton",asset.skeleton,NLS::Core::Assets::ArtifactType::Skeleton);ImGui::SliderFloat("Default Weight",&asset.defaultWeight,0,1);
        auto model=preview->Model();if(!model)return;
        for(const auto& bone:model->skeleton.bones)
        {ImGui::PushID(bone.id.c_str());auto found=asset.weights.find(bone.id);float weight=found==asset.weights.end()?asset.defaultWeight:found->second;if(ImGui::SliderFloat(bone.name.c_str(),&weight,0,1))asset.weights[bone.id]=weight;ImGui::PopID();}
    }
    void Playback()
    {
        auto* animator=preview->Animator();if(!animator)return;
        if(ImGui::Button(paused?"Play":"Pause")){paused=!paused;animator->Pause(paused);}ImGui::SameLine();if(ImGui::Button("Stop")){paused=true;animator->Stop();}
        ImGui::SameLine();if(ImGui::Checkbox("Loop Preview",&loop))animator->SetLooping(loop);ImGui::SameLine();if(ImGui::DragFloat("Speed",&speed,.05f,-4,4))animator->SetPlaybackSpeed(speed);
        auto model=preview->Model();double duration=model&&!model->clips.empty()?model->clips[std::min(clip,model->clips.size()-1)].duration:10;
        const double age=animator->GetTime();float time=float(loop&&duration>0?WrapTime(age,duration):std::clamp(age,0.,duration));
        if(ImGui::SliderFloat("Timeline",&time,0,float(duration),"%.3f s"))animator->Seek(time);
        if(model&&ImGui::BeginCombo("Clip / Take",model->clips.empty()?"Reference Pose":model->clips[std::min(clip,model->clips.size()-1)].name.c_str()))
        {for(size_t i=0;i<model->clips.size();++i)if(ImGui::Selectable(model->clips[i].name.c_str(),clip==i)){clip=i;preview->SelectClip(i);}ImGui::EndCombo();}
        if(model&&ImGui::Button("Reference Pose")){preview->SelectClip(model->clips.size());paused=true;animator->Pause();}
        if(ImGui::CollapsingHeader("Skeleton Tree")&&model)
        {
            std::vector<std::vector<BoneIndex>> children(model->skeleton.bones.size());
            for(BoneIndex i=0;i<children.size();++i)if(model->skeleton.bones[i].parent!=NoBone)children[model->skeleton.bones[i].parent].push_back(i);
            std::function<void(BoneIndex,uint32_t)> drawBone=[&](BoneIndex index,uint32_t depth)
            {
                if(depth>256)return;const auto& bone=model->skeleton.bones[index];
                if(ImGui::TreeNode((bone.name+"##"+bone.id).c_str()))
                {const auto& t=bone.reference;ImGui::Text("Reference T %.3f %.3f %.3f",t.translation.x,t.translation.y,t.translation.z);ImGui::Text("Reference Q %.3f %.3f %.3f %.3f",t.rotation.x,t.rotation.y,t.rotation.z,t.rotation.w);ImGui::Text("Reference S %.3f %.3f %.3f",t.scale.x,t.scale.y,t.scale.z);for(auto child:children[index])drawBone(child,depth+1);ImGui::TreePop();}
            };
            for(BoneIndex i=0;i<children.size();++i)if(model->skeleton.bones[i].parent==NoBone)drawBone(i,0);
        }
        auto frame=animator->CaptureFrame();if(frame&&ImGui::CollapsingHeader("Runtime Debug"))
        {for(auto state:frame->activeStates)ImGui::Text("Active State %llu",state);for(size_t i=0;i<frame->nodeWeights.size();++i)if(frame->nodeWeights[i]>0)ImGui::Text("Node %zu: %.3f",i,frame->nodeWeights[i]);for(const auto& event:*events)ImGui::Text("%.3f %s (%d)",event.time,event.name.c_str(),int(event.phase));}
    }
};
AnimationWorkbench::AnimationWorkbench(const std::string& title,bool opened,const UI::PanelWindowSettings& settings,std::filesystem::path root,AnimationPreview& preview)
    :PanelWindow(title,opened,settings),m(std::make_unique<State>()){m->root=std::move(root);m->preview=&preview;m->Refresh();}
AnimationWorkbench::~AnimationWorkbench()=default;
bool AnimationWorkbench::OwnsShortcuts()const{return IsOpened()&&IsFocused();}
bool AnimationWorkbench::OpenDocument(const std::filesystem::path& path)
{
    const auto extension=path.extension().string();Open();Focus();
    if(extension==".fbx"||extension==".gltf"||extension==".glb")
    {
        auto meta=NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(path));if(!meta)meta=NLS::Core::Assets::AssetMeta::CreateForAsset(path);
        Render::Assets::ImportedScene scene;std::vector<Render::Resources::Parsers::ParsedMeshData> meshes;std::vector<std::string> materials,dependencies;
        if(!Render::Assets::ReadModelSource(path,*meta,Render::Resources::Parsers::EModelParserFlags::TRIANGULATE,meshes,materials,dependencies,scene,m->error)||!scene.animationData)return false;
        m->preview->SetModel(scene.animationData);m->previewPath=path.string();m->clip=0;m->paused=true;
        m->BindPreviewEvents();return true;
    }
    const auto key=path.lexically_normal().generic_string();if(!m->sessions.contains(key)){auto session=std::make_unique<State::Session>();if(!session->document.Load(path)){m->error=session->document.Error();return false;}session->path=path.string();m->sessions[key]=std::move(session);}m->active=key;return true;
}
void AnimationWorkbench::OnBeforeDrawWidgets()
{
    if(ImGui::BeginMenuBar())
    {if(ImGui::BeginMenu("New")){if(ImGui::MenuItem("Animation Graph"))m->New(0);if(ImGui::MenuItem("Animation Clip"))m->New(1);if(ImGui::MenuItem("Blend Space"))m->New(2);if(ImGui::MenuItem("Bone Mask"))m->New(3);ImGui::EndMenu();}ImGui::EndMenuBar();}
    if(ImGui::Button("New Graph"))m->New(0);ImGui::SameLine();if(ImGui::Button("New Clip"))m->New(1);ImGui::SameLine();if(ImGui::Button("New BlendSpace"))m->New(2);ImGui::SameLine();if(ImGui::Button("New BoneMask"))m->New(3);
    ImGui::InputText("Open Source / Preview Character",&m->openPath);ImGui::SameLine();if(ImGui::Button("Open"))OpenDocument(m->openPath);
    if(auto model=m->preview->Model();model&&!model->clips.empty()&&ImGui::Button("Create Clip from Preview Take"))
    {
        const auto meta=NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(m->previewPath));
        if(meta)
        {
            const auto& source=model->clips[std::min(m->clip,model->clips.size()-1)];m->New(1);auto& session=*m->sessions.at(m->active);
            ClipAsset asset;asset.id=NLS::Guid::New().ToString();asset.name=source.name;asset.source=meta->id.ToString()+"#animation:"+source.id;asset.end=source.duration;
            session.document.New(asset,session.path);
        }
        else m->error="Import the preview model before creating a persistent Clip asset";
    }
    m->Playback();
    if(ImGui::BeginTabBar("Animation Documents"))
    {
        for(auto& [key,session]:m->sessions)
        {
            const auto& state=session->document.State();const auto label=state.path.filename().string()+(state.source!=state.saved?" *":"")+"##"+key;
            if(ImGui::BeginTabItem(label.c_str(),&session->opened,key==m->active?ImGuiTabItemFlags_SetSelected:0))
            {
                m->active.clear();ImGui::InputText("Save Path",&session->path);
                if(ImGui::Button("Save")){if(!session->document.Save(session->path))m->error=session->document.Error();}
                ImGui::SameLine();if(ImGui::Button("Compile / Preview")&&m->Compile(*session))
                {
                    if(session->compiled.program)m->preview->SetProgram(session->compiled.program,m->error);
                    else if(session->compiled.model)
                    {
                        if(auto existing=m->preview->Model();existing&&!existing->meshes.empty()&&existing->skeleton.Signature()==session->compiled.model->skeleton.Signature())
                        {auto previewModel=std::make_shared<Model>(*existing);previewModel->clips=session->compiled.model->clips;m->preview->SetModel(previewModel);}
                        else m->preview->SetModel(session->compiled.model);
                        m->paused=true;m->clip=0;m->BindPreviewEvents();
                    }
                }
                ImGui::SameLine();if(ImGui::Button("Apply")&&m->Compile(*session))
                {
                    if(session->compiled.program)
                    {
                        if(auto meta=NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(session->document.State().path)))
                        {Engine::Components::AnimatorComponent::PublishAppliedProgram(meta->id.ToString(),session->compiled.program);session->document.MarkApplied(session->compiled.program->dependencySnapshot);}
                        else m->error="Save this animation graph before applying it to scene instances";
                        if(m->preview->Animator())m->preview->SetProgram(session->compiled.program,m->error);
                    }
                    else if(session->compiled.model&&!session->compiled.model->clips.empty())
                    {
                        AnimationInstance instance;
                        if(instance.PlayClip(std::make_shared<Skeleton>(session->compiled.model->skeleton),std::make_shared<Clip>(session->compiled.model->clips.front()),m->error))
                            if(auto meta=NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(session->document.State().path)))
                            {
                                Engine::Components::AnimatorComponent::PublishAppliedProgram(meta->id.ToString(),instance.Program());
                                session->document.MarkApplied(NLS::Core::Assets::ComputeNativeArtifactPayloadHash(session->compiled.payload.payload));
                                if(m->preview->Animator())m->preview->SetProgram(instance.Program(),m->error);
                            }
                    }
                }
                ImGui::SameLine();ImGui::TextUnformatted(state.source==state.applied?"Applied":"Working");
                m->PreviewParameters(*session);
                if(session->renderedSource!=state.source)session->positionsLoaded=false;
                Asset candidate=session->document.Value();std::visit([&](auto& asset)
                {ImGui::InputText("Asset Name",&asset.name);using T=std::decay_t<decltype(asset)>;if constexpr(std::is_same_v<T,GraphAsset>)m->Graph(*session,asset);else if constexpr(std::is_same_v<T,ClipAsset>)m->ClipEditor(asset);else if constexpr(std::is_same_v<T,BlendSpaceAsset>)m->BlendEditor(asset);else m->MaskEditor(asset);},candidate);
                if(!session->document.Edit(std::move(candidate),"Edit Animation"))m->error=session->document.Error();
                session->renderedSource=session->document.State().source;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    for(auto& [key,session]:m->sessions)if(!session->opened&&session->document.State().source!=session->document.State().saved)
    {session->opened=true;m->closeKey=key;ImGui::OpenPopup("Unsaved Animation");break;}
    if(ImGui::BeginPopupModal("Unsaved Animation",nullptr,ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Save changes before closing this animation document?");
        auto found=m->sessions.find(m->closeKey);
        if(ImGui::Button("Save")&&found!=m->sessions.end())
        {if(found->second->document.Save(found->second->path)){found->second->opened=false;ImGui::CloseCurrentPopup();}else m->error=found->second->document.Error();}
        ImGui::SameLine();if(ImGui::Button("Discard")){m->sessions.erase(m->closeKey);ImGui::CloseCurrentPopup();}
        ImGui::SameLine();if(ImGui::Button("Cancel"))ImGui::CloseCurrentPopup();ImGui::EndPopup();
    }
    std::erase_if(m->sessions,[](const auto& entry){return !entry.second->opened&&entry.second->document.State().source==entry.second->document.State().saved;});
    if(!m->error.empty())ImGui::TextWrapped("%s",m->error.c_str());
    if(m->preview->IsOpened()){m->preview->Update(ImGui::GetIO().DeltaTime);m->preview->Render();}
}
bool AnimationWorkbench::OpenImportedAsset(const std::filesystem::path& path,std::string_view key)
{
    if(!OpenDocument(path))return false;auto model=m->preview->Model();if(!model)return false;
    for(size_t i=0;i<model->clips.size();++i)if("animation:"+model->clips[i].id==key)
    {m->clip=i;m->preview->SelectClip(i);break;}
    return true;
}
}
