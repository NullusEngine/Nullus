#include "AnimationCompiler.h"
#include <Assets/ArtifactManifest.h>
#include <Guid.h>
#include <Json/json.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <unordered_map>

namespace NLS::Animation
{
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Channel,interpolation,times,values,inTangents,outTangents)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Curve,name,channel)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Notify,id,name,payload,start,end,threshold)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Parameter,id,name,type,initial)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Condition,parameter,operation,value,children)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Node,id,kind,name,clip,blendSpace,mask,parameter,xParameter,yParameter,inputs,weights,weight,sampleTime,machine,loop,position)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(State,id,name,pose)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Transition,id,from,to,condition,duration,exitTime,priority,interruptible,curve)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(StateMachine,id,initial,name,states,transitions)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(GraphAsset,version,id,name,skeleton,output,parameters,nodes,machines)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(BlendSample,clip,x,y)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(BlendSpaceAsset,version,id,name,skeleton,dimensions,samples)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(BoneMaskAsset,version,id,name,skeleton,defaultWeight,weights)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ClipAsset,version,id,name,source,skeleton,start,end,loop,curves,notifies)
}
namespace NLS::AnimationCompiler
{
using namespace Animation;
namespace
{
using Json=nlohmann::json;
std::string Hash(std::span<const uint8_t> bytes){return Core::Assets::ToHexString(Core::Assets::ComputeSha256(bytes.data(),bytes.size()));}
template<class T>bool Read(std::string_view source,T& asset,std::string& error)
{
    try{auto j=Json::parse(source);auto value=j.get<T>();if(value.version!=1)throw std::runtime_error("Unsupported animation source version");asset=std::move(value);return true;}
    catch(const std::exception& e){error=e.what();return false;}
}
bool Compatible(std::string_view id,const Skeleton& skeleton){return id.empty()||id==skeleton.id||id==skeleton.Signature();}
float Orient(const BlendSample& a,const BlendSample& b,const BlendSample& c){return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);}
bool InCircle(const BlendSample& a,const BlendSample& b,const BlendSample& c,const BlendSample& p)
{
    const double ax=a.x-p.x,ay=a.y-p.y,bx=b.x-p.x,by=b.y-p.y,cx=c.x-p.x,cy=c.y-p.y;
    const double determinant=(ax*ax+ay*ay)*(bx*cy-by*cx)-(bx*bx+by*by)*(ax*cy-ay*cx)+(cx*cx+cy*cy)*(ax*by-ay*bx);
    return determinant*Orient(a,b,c)>1e-10;
}
}
std::string Serialize(const GraphAsset& a){return Json(a).dump(2);}
std::string Serialize(const BlendSpaceAsset& a){return Json(a).dump(2);}
std::string Serialize(const BoneMaskAsset& a){return Json(a).dump(2);}
std::string Serialize(const ClipAsset& a){return Json(a).dump(2);}
bool Deserialize(std::string_view s,GraphAsset& a,std::string& e){return Read(s,a,e);}
bool Deserialize(std::string_view s,BlendSpaceAsset& a,std::string& e){return Read(s,a,e);}
bool Deserialize(std::string_view s,BoneMaskAsset& a,std::string& e){return Read(s,a,e);}
bool Deserialize(std::string_view s,ClipAsset& a,std::string& e){return Read(s,a,e);}
GraphAsset CreateGraph(std::string name)
{
    GraphAsset graph;graph.id=NLS::Guid::New().ToString();graph.name=std::move(name);
    Node pose;pose.id=1;pose.kind=NodeKind::ReferencePose;pose.name="Reference Pose";graph.nodes.push_back(pose);
    Node output;output.id=2;output.kind=NodeKind::OutputPose;output.name="Output Pose";output.inputs={1};output.position={300,0};graph.nodes.push_back(output);graph.output=2;return graph;
}
std::shared_ptr<BlendSpace> CompileBlendSpace(const BlendSpaceAsset& asset,const Resources& resources,std::string& error)
{
    if(!resources.skeleton||!Compatible(asset.skeleton,*resources.skeleton)||(asset.dimensions!=1&&asset.dimensions!=2)||asset.samples.empty()||asset.samples.size()>4096)
    {error="BlendSpace requires a compatible skeleton, 1D/2D axes and 1..4096 samples";return {};}
    auto blend=std::make_shared<BlendSpace>();blend->source=asset;
    for(size_t i=0;i<asset.samples.size();++i)
    {
        const auto& s=asset.samples[i];auto clip=resources.clips.find(s.clip);
        if(!std::isfinite(s.x)||!std::isfinite(s.y)||clip==resources.clips.end()||!clip->second||!clip->second->Validate(*resources.skeleton,error))
        {if(error.empty())error="BlendSpace sample references a missing clip";return {};}
        for(size_t k=0;k<i;++k)if(s.x==asset.samples[k].x&&(asset.dimensions==1||s.y==asset.samples[k].y)){error="BlendSpace has duplicate sample coordinates";return {};}
        blend->clips.push_back(clip->second);
    }
    if(asset.dimensions==1||asset.samples.size()<3)return blend;
    // Deterministic Bowyer-Watson triangulation. Collinear sets use segment interpolation.
    auto points=asset.samples;float minX=points[0].x,maxX=minX,minY=points[0].y,maxY=minY;
    for(auto p:points){minX=std::min(minX,p.x);maxX=std::max(maxX,p.x);minY=std::min(minY,p.y);maxY=std::max(maxY,p.y);}
    const float span=std::max(maxX-minX,maxY-minY);if(span<=0)return blend;
    const float cx=(minX+maxX)*.5f,cy=(minY+maxY)*.5f;const uint32_t count=uint32_t(points.size());
    points.push_back({{},cx-32*span,cy-16*span});points.push_back({{},cx+32*span,cy-16*span});points.push_back({{},cx,cy+32*span});
    std::vector<std::array<uint32_t,3>> triangles{{count,count+1,count+2}};
    for(uint32_t i=0;i<count;++i)
    {
        std::map<std::pair<uint32_t,uint32_t>,uint32_t> edges;
        std::erase_if(triangles,[&](auto t)
        {
            if(!InCircle(points[t[0]],points[t[1]],points[t[2]],points[i]))return false;
            for(size_t k=0;k<3;++k){auto a=t[k],b=t[(k+1)%3];if(b<a)std::swap(a,b);++edges[{a,b}];}return true;
        });
        for(auto [edge,n]:edges)if(n==1){std::array<uint32_t,3> t{edge.first,edge.second,i};if(Orient(points[t[0]],points[t[1]],points[t[2]])<0)std::swap(t[0],t[1]);if(std::abs(Orient(points[t[0]],points[t[1]],points[t[2]]))>1e-10f)triangles.push_back(t);}
    }
    for(auto t:triangles)if(t[0]<count&&t[1]<count&&t[2]<count)blend->triangles.push_back(t);
    return blend;
}
std::shared_ptr<Clip> CompileClip(const ClipAsset& asset,const Resources& resources,std::string& error)
{
    const auto source=resources.clips.find(asset.source);
    if(!resources.skeleton||source==resources.clips.end()||!source->second||!Compatible(asset.skeleton,*resources.skeleton))
    {error="Authored animation clip requires a valid source clip and compatible skeleton";return {};}
    const auto& original=*source->second;
    const double end=asset.end<0?original.duration:asset.end;
    if(!std::isfinite(asset.start)||!std::isfinite(end)||asset.start<0||end<asset.start||end>original.duration)
    {error="Clip trim range is outside its source";return {};}
    auto clip=std::make_shared<Clip>(original);clip->id=asset.id;clip->name=asset.name;clip->duration=end-asset.start;
    // Clipping keeps cubic boundary derivatives by evaluating the original Hermite segment.
    auto trim=[&](Channel& c,bool quaternion)
    {
        if(c.times.empty())return;
        Channel result;result.interpolation=c.interpolation;
        std::vector<double> times{asset.start};for(auto t:c.times)if(t>asset.start&&t<end)times.push_back(t);if(end>asset.start)times.push_back(end);
        auto derivative=[&](double t)
        {
            Value v{};if(c.times.size()<2||t<c.times.front()||t>c.times.back())return v;
            const auto next=std::clamp<size_t>(size_t(std::upper_bound(c.times.begin(),c.times.end(),t)-c.times.begin()),1,c.times.size()-1),prev=next-1;
            const float dt=float(c.times[next]-c.times[prev]),u=float((t-c.times[prev])/dt);
            for(size_t k=0;k<4;++k)v[k]=((6*u*u-6*u)*c.values[prev][k]+(3*u*u-4*u+1)*dt*c.outTangents[prev][k]+(-6*u*u+6*u)*c.values[next][k]+(3*u*u-2*u)*dt*c.inTangents[next][k])/dt;return v;
        };
        for(auto t:times){result.times.push_back(t-asset.start);result.values.push_back(c.Sample(t,{},quaternion&&c.interpolation!=Interpolation::CubicSpline));if(c.interpolation==Interpolation::CubicSpline){auto d=derivative(t);result.inTangents.push_back(d);result.outTangents.push_back(d);}}
        c=std::move(result);
    };
    for(auto& track:clip->tracks){trim(track.translation,false);trim(track.rotation,true);trim(track.scale,false);}
    clip->curves=asset.curves;clip->notifies=asset.notifies;
    if(!clip->Validate(*resources.skeleton,error))return {};return clip;
}
CompileResult Compile(const GraphAsset& graph,const Resources& resources)
{
    CompileResult result;auto fail=[&](uint64_t node,std::string error){result.diagnostics.push_back({node,std::move(error)});};
    std::string error;
    if(graph.version!=1||!resources.skeleton||!resources.skeleton->Validate(error)||!Compatible(graph.skeleton,*resources.skeleton))
    {fail(0,error.empty()?"Animation graph requires a compatible skeleton and source version":error);return result;}
    if(graph.nodes.size()>16384||graph.machines.size()>1024){fail(0,"Animation graph exceeds compiler limits");return result;}
    auto program=std::make_shared<AnimationProgram>();program->id=graph.id;program->skeleton=resources.skeleton;program->parameters=graph.parameters;
    std::unordered_map<uint64_t,uint32_t> nodes,machines;std::map<std::string,int> parameters;std::set<uint64_t> parameterIds;
    for(size_t i=0;i<graph.parameters.size();++i)
    {
        const auto& p=graph.parameters[i];
        if(!p.id||p.name.empty()||!parameterIds.insert(p.id).second||!parameters.emplace(p.name,int(i)).second||int(p.type)>int(ParameterType::Trigger)||!std::all_of(p.initial.begin(),p.initial.end(),[](float f){return std::isfinite(f);}))fail(0,"Invalid or duplicate animation parameter: "+p.name);
    }
    for(uint32_t i=0;i<graph.nodes.size();++i)if(!graph.nodes[i].id||!nodes.emplace(graph.nodes[i].id,i).second)fail(graph.nodes[i].id,"Node identities must be unique and nonzero");
    for(uint32_t i=0;i<graph.machines.size();++i)if(!graph.machines[i].id||!machines.emplace(graph.machines[i].id,i).second)fail(0,"State machine identities must be unique and nonzero");
    if(!nodes.contains(graph.output))fail(graph.output,"Output Pose is missing");
    if(!result.diagnostics.empty())return result;
    // Traverse pose dependencies, including state subgraphs, but never control transition edges.
    std::vector<uint8_t> visited(graph.nodes.size());std::vector<uint32_t> order;
    std::function<bool(uint32_t,uint32_t)> visit=[&](uint32_t index,uint32_t depth)
    {
        const auto& n=graph.nodes[index];if(depth>512){fail(n.id,"Pose graph nesting exceeds 512");return false;}
        if(visited[index]==1){fail(n.id,"Pose graph contains a data cycle");return false;}if(visited[index]==2)return true;visited[index]=1;
        for(auto input:n.inputs){if(!nodes.contains(input)){fail(n.id,"Pose input references a missing node");return false;}if(!visit(nodes.at(input),depth+1))return false;}
        if(n.kind==NodeKind::StateMachine)
        {
            if(!machines.contains(n.machine)){fail(n.id,"State machine is missing");return false;}
            for(auto state:graph.machines[machines.at(n.machine)].states){if(!nodes.contains(state.pose)){fail(n.id,"State references a missing Pose subgraph");return false;}if(!visit(nodes.at(state.pose),depth+1))return false;}
        }
        visited[index]=2;order.push_back(index);return true;
    };
    if(!visit(nodes.at(graph.output),0))return result;
    std::unordered_map<uint64_t,uint32_t> compiled;
    for(auto index:order)compiled.emplace(graph.nodes[index].id,uint32_t(compiled.size()));
    auto resolveParameter=[&](std::string_view name,uint64_t node)->int
    {if(name.empty())return -1;auto p=parameters.find(std::string(name));if(p==parameters.end()){fail(node,"Unknown animation parameter: "+std::string(name));return -1;}if(program->parameters[size_t(p->second)].type!=ParameterType::Float)fail(node,"Pose control requires a float parameter: "+std::string(name));return p->second;};
    std::function<void(Condition&,uint64_t,uint32_t)> condition=[&](Condition& c,uint64_t node,uint32_t depth)
    {
        if(depth>64){fail(node,"Transition condition nesting exceeds 64");return;}
        if(int(c.operation)>int(Compare::Not)){fail(node,"Unknown transition condition operation");return;}
        if(c.operation==Compare::All||c.operation==Compare::Any||c.operation==Compare::Not)
        {if(c.operation==Compare::Not&&c.children.size()!=1)fail(node,"Not condition requires one child");for(auto& child:c.children)condition(child,node,depth+1);return;}
        if(c.parameter=="$NormalizedTime"||c.parameter=="$TimeRemaining")return;
        auto p=parameters.find(c.parameter);if(p==parameters.end()){fail(node,"Unknown transition parameter: "+c.parameter);return;}c.parameterIndex=p->second;
        if(program->parameters[size_t(p->second)].type==ParameterType::Vector)fail(node,"Transition condition cannot compare a vector parameter");
    };
    std::unordered_map<uint64_t,int32_t> compiledMachines;
    for(auto index:order)
    {
        const auto& source=graph.nodes[index];CompiledNode n;n.source=source;
        for(auto input:source.inputs)n.inputs.push_back(compiled.at(input));
        const auto kind=source.kind;
        const bool unary=kind==NodeKind::SaveCachedPose||kind==NodeKind::UseCachedPose||kind==NodeKind::OutputPose;
        const bool binary=kind==NodeKind::Blend||kind==NodeKind::LayeredBlend;
        if(int(kind)>int(NodeKind::OutputPose)||(unary&&n.inputs.size()!=1)||(binary&&n.inputs.size()!=2)||(kind==NodeKind::Additive&&(n.inputs.size()<2||n.inputs.size()>3))||(kind==NodeKind::MultiBlend&&(n.inputs.empty()||source.weights.size()!=n.inputs.size()))||
            ((kind==NodeKind::ReferencePose||kind==NodeKind::SequencePlayer||kind==NodeKind::SequenceEvaluator||kind==NodeKind::BlendSpacePlayer||kind==NodeKind::StateMachine)&&!n.inputs.empty()))fail(source.id,"Node has invalid Pose inputs");
        if(!std::isfinite(source.weight)||!std::isfinite(source.sampleTime))fail(source.id,"Node contains a non-finite control");
        for(float weight:source.weights)if(!std::isfinite(weight)||weight<0)fail(source.id,"Blend weights must be finite and nonnegative");
        n.parameter=resolveParameter(source.parameter,source.id);n.xParameter=resolveParameter(source.xParameter,source.id);n.yParameter=resolveParameter(source.yParameter,source.id);
        if(kind==NodeKind::SequencePlayer||kind==NodeKind::SequenceEvaluator)
        {
            auto c=resources.clips.find(source.clip);if(c==resources.clips.end()||!c->second)fail(source.id,"Animation clip is missing: "+source.clip);
            else if(!c->second->Validate(*resources.skeleton,error))fail(source.id,error);else n.clip=c->second;
        }
        if(kind==NodeKind::BlendSpacePlayer)
        {auto b=resources.blendSpaces.find(source.blendSpace);if(b==resources.blendSpaces.end())fail(source.id,"BlendSpace is missing: "+source.blendSpace);else {n.blendSpace=CompileBlendSpace(b->second,resources,error);if(!n.blendSpace)fail(source.id,error);}}
        if(kind==NodeKind::LayeredBlend)
        {
            auto b=resources.masks.find(source.mask);
            if(b==resources.masks.end()||!Compatible(b->second.skeleton,*resources.skeleton))fail(source.id,"Bone mask is missing or incompatible");
            else
            {
                const auto& mask=b->second;
                if(!std::isfinite(mask.defaultWeight)||mask.defaultWeight<0||mask.defaultWeight>1)fail(source.id,"Bone mask default weight must be finite and between zero and one");
                n.mask.resize(resources.skeleton->bones.size(),mask.defaultWeight);
                for(const auto& [bone,w]:mask.weights)if(resources.skeleton->Find(bone)==NoBone||!std::isfinite(w)||w<0||w>1)fail(source.id,"Invalid bone mask weight: "+bone);
                for(size_t i=0;i<n.mask.size();++i)
                {const auto& bone=resources.skeleton->bones[i];auto weight=mask.weights.find(bone.id);if(weight==mask.weights.end())weight=mask.weights.find(bone.name);n.mask[i]=weight!=mask.weights.end()?weight->second:bone.parent!=NoBone?n.mask[bone.parent]:mask.defaultWeight;}
            }
        }
        if(kind==NodeKind::StateMachine)
        {
            auto found=compiledMachines.find(source.machine);
            if(found!=compiledMachines.end())fail(source.id,"A State Machine definition may have only one runtime owner node");
            else
            {
                n.machine=int32_t(program->machines.size());compiledMachines.emplace(source.machine,n.machine);auto machine=graph.machines[machines.at(source.machine)];
                std::set<uint64_t> stateIds,transitionIds;
                for(const auto& state:machine.states)if(!state.id||!stateIds.insert(state.id).second)fail(source.id,"State identities must be unique and nonzero");
                if(!stateIds.contains(machine.initial))fail(source.id,"State machine has no valid initial state");
                for(auto& t:machine.transitions)
                {
                    if(!t.id||!transitionIds.insert(t.id).second||!stateIds.contains(t.to)||(t.from&&!stateIds.contains(t.from))||!std::isfinite(t.duration)||t.duration<0||!std::isfinite(t.exitTime)||int(t.curve)>int(BlendCurve::SmoothStep))fail(source.id,"Transition identity, state or timing is invalid");
                    condition(t.condition,source.id,0);
                }
                program->machines.push_back(std::move(machine));
            }
        }
        program->nodes.push_back(std::move(n));
    }
    if(!result.diagnostics.empty())return result;
    program->output=compiled.at(graph.output);const auto bytes=SerializeProgram(*program);program->dependencySnapshot=Hash(bytes);result.program=std::move(program);return result;
}
std::vector<uint8_t> SerializeProgram(const AnimationProgram& program)
{
    Model model;model.skeleton=*program.skeleton;model.reader="Nullus.AnimationCompiler.v1";
    Json blendSpaces=Json::object(),clipKeys=Json::object();std::set<std::string> clipNames;
    auto addClip=[&](const std::string& key,const std::shared_ptr<const Clip>& clip)
    {if(!clip||!clipNames.insert(key).second)return;clipKeys[key]=model.clips.size();auto value=*clip;value.id="compiled/"+key;model.clips.push_back(std::move(value));};
    for(const auto& node:program.nodes)
    {
        if(node.clip)addClip(node.source.clip,node.clip);
        if(node.blendSpace){blendSpaces[node.source.blendSpace]=node.blendSpace->source;for(size_t i=0;i<node.blendSpace->clips.size();++i)addClip(node.blendSpace->source.samples[i].clip,node.blendSpace->clips[i]);}
    }
    Json nodes=Json::array();
    for(const auto& node:program.nodes)
    {
        Json n{{"source",node.source},{"inputs",node.inputs},{"parameter",node.parameter},{"xParameter",node.xParameter},{"yParameter",node.yParameter},{"machine",node.machine},{"mask",node.mask}};
        if(node.blendSpace)n["triangles"]=node.blendSpace->triangles;
        nodes.push_back(std::move(n));
    }
    return Json::to_cbor(Json{{"version",1},{"id",program.id},{"output",program.output},{"parameters",program.parameters},{"machines",program.machines},{"nodes",nodes},
        {"model",Json::binary(SerializeModel(model))},{"clipKeys",clipKeys},{"blendSpaces",blendSpaces}});
}
std::shared_ptr<const AnimationProgram> DeserializeProgram(std::span<const uint8_t> bytes,std::string& error)
{
    try
    {
        const auto j=Json::from_cbor(bytes.begin(),bytes.end());if(j.at("version")!=1)throw std::runtime_error("Unsupported animation program version");
        const auto& data=j.at("model").get_binary();auto model=DeserializeModel(std::span<const uint8_t>(data.data(),data.size()),error);if(!model)return {};
        Resources resources;resources.skeleton=std::make_shared<Skeleton>(model->skeleton);
        for(auto it=j.at("clipKeys").begin();it!=j.at("clipKeys").end();++it)resources.clips[it.key()]=std::make_shared<Clip>(model->clips.at(it.value().get<size_t>()));
        for(auto it=j.at("blendSpaces").begin();it!=j.at("blendSpaces").end();++it)resources.blendSpaces[it.key()]=it.value().get<BlendSpaceAsset>();
        auto program=std::make_shared<AnimationProgram>();program->id=j.at("id").get<std::string>();program->skeleton=resources.skeleton;
        program->parameters=j.at("parameters").get<std::vector<Parameter>>();program->machines=j.at("machines").get<std::vector<StateMachine>>();program->output=j.at("output").get<uint32_t>();
        std::map<std::string,int32_t> parameters;for(size_t i=0;i<program->parameters.size();++i)
        {const auto& p=program->parameters[i];if(!p.id||p.name.empty()||!parameters.emplace(p.name,int32_t(i)).second)throw std::runtime_error("Invalid program parameter layout");}
        std::map<uint64_t,uint32_t> nodeIds;
        for(const auto& n:j.at("nodes"))
        {
            CompiledNode node;node.source=n.at("source").get<Node>();node.inputs=n.at("inputs").get<std::vector<uint32_t>>();node.parameter=n.at("parameter").get<int32_t>();
            node.xParameter=n.at("xParameter").get<int32_t>();node.yParameter=n.at("yParameter").get<int32_t>();node.machine=n.at("machine").get<int32_t>();node.mask=n.at("mask").get<std::vector<float>>();
            for(auto parameter:{node.parameter,node.xParameter,node.yParameter})if(parameter< -1||(parameter>=0&&size_t(parameter)>=program->parameters.size()))throw std::runtime_error("Compiled parameter index is out of range");
            if(!node.source.id||!nodeIds.emplace(node.source.id,uint32_t(program->nodes.size())).second)throw std::runtime_error("Duplicate compiled node identity");
            if(!node.source.clip.empty())
            {auto found=resources.clips.find(node.source.clip);if(found==resources.clips.end())throw std::runtime_error("Compiled clip dependency is missing");node.clip=found->second;}
            if(!node.source.blendSpace.empty())
            {
                const auto found=resources.blendSpaces.find(node.source.blendSpace);if(found==resources.blendSpaces.end())throw std::runtime_error("Compiled BlendSpace dependency is missing");
                auto blend=std::make_shared<BlendSpace>();blend->source=found->second;blend->triangles=n.at("triangles").get<std::vector<std::array<uint32_t,3>>>();
                for(const auto& sample:blend->source.samples){auto clip=resources.clips.find(sample.clip);if(clip==resources.clips.end())throw std::runtime_error("Compiled BlendSpace clip is missing");blend->clips.push_back(clip->second);}
                for(auto triangle:blend->triangles)for(auto index:triangle)if(index>=blend->clips.size())throw std::runtime_error("Compiled BlendSpace index is invalid");node.blendSpace=std::move(blend);
            }
            const auto kind=node.source.kind;const bool unary=kind==NodeKind::SaveCachedPose||kind==NodeKind::UseCachedPose||kind==NodeKind::OutputPose;
            const bool binary=kind==NodeKind::Blend||kind==NodeKind::LayeredBlend;
            if(int(kind)>int(NodeKind::OutputPose)||(unary&&node.inputs.size()!=1)||(binary&&node.inputs.size()!=2)||
                (kind==NodeKind::Additive&&(node.inputs.size()<2||node.inputs.size()>3))||
                (kind==NodeKind::MultiBlend&&(node.inputs.empty()||node.inputs.size()!=node.source.weights.size()))||
                ((kind==NodeKind::SequencePlayer||kind==NodeKind::SequenceEvaluator)&&!node.clip)||
                (kind==NodeKind::BlendSpacePlayer&&(!node.blendSpace||node.blendSpace->clips.empty()))||
                (!node.mask.empty()&&node.mask.size()!=program->skeleton->bones.size()))throw std::runtime_error("Compiled node layout is invalid");
            if(!std::isfinite(node.source.weight)||!std::isfinite(node.source.sampleTime))throw std::runtime_error("Compiled node control is invalid");
            for(float w:node.mask)if(!std::isfinite(w)||w<0||w>1)throw std::runtime_error("Compiled bone mask weight is invalid");
            program->nodes.push_back(std::move(node));
        }
        if(program->nodes.empty()||program->nodes.size()>16384||program->output>=program->nodes.size())throw std::runtime_error("Compiled output index is invalid");
        std::function<void(Condition&,uint32_t)> condition=[&](Condition& c,uint32_t depth)
        {
            if(depth>64)throw std::runtime_error("Compiled condition depth exceeds its limit");
            if(c.operation==Compare::All||c.operation==Compare::Any||c.operation==Compare::Not){for(auto& child:c.children)condition(child,depth+1);return;}
            if(c.parameter=="$NormalizedTime"||c.parameter=="$TimeRemaining")return;
            auto found=parameters.find(c.parameter);if(found==parameters.end())throw std::runtime_error("Compiled condition parameter is missing");c.parameterIndex=found->second;
        };
        for(auto& machine:program->machines)
        {
            std::set<uint64_t> states;for(const auto& state:machine.states){if(!states.insert(state.id).second||!nodeIds.contains(state.pose))throw std::runtime_error("Compiled state is invalid");}
            if(!states.contains(machine.initial))throw std::runtime_error("Compiled initial state is missing");
            for(auto& t:machine.transitions){if(!states.contains(t.to)||(t.from&&!states.contains(t.from))||!std::isfinite(t.duration)||t.duration<0)throw std::runtime_error("Compiled transition is invalid");condition(t.condition,0);}
        }
        std::vector<uint8_t> visited(program->nodes.size());
        std::function<void(uint32_t,uint32_t)> validate=[&](uint32_t index,uint32_t depth)
        {
            if(index>=program->nodes.size()||depth>512||visited[index]==1)throw std::runtime_error("Compiled Pose dependency is invalid or cyclic");if(visited[index]==2)return;visited[index]=1;
            const auto& n=program->nodes[index];for(auto input:n.inputs)validate(input,depth+1);
            if(n.source.kind==NodeKind::StateMachine)
            {if(n.machine<0||size_t(n.machine)>=program->machines.size())throw std::runtime_error("Compiled machine index is invalid");for(const auto& state:program->machines[size_t(n.machine)].states)validate(nodeIds.at(state.pose),depth+1);}
            visited[index]=2;
        };
        validate(program->output,0);program->dependencySnapshot=Hash(bytes);return program;
    }
    catch(const std::exception& e){error=std::string("Invalid animation program: ")+e.what();return {};}
}
}
