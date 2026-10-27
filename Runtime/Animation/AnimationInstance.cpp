#include "Graph.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <unordered_map>

namespace NLS::Animation
{
namespace
{
float Square(float x){return x*x;}
uint32_t FindState(const StateMachine& machine,uint64_t id)
{for(uint32_t i=0;i<machine.states.size();++i)if(machine.states[i].id==id)return i;return UINT32_MAX;}
}
std::vector<BlendWeight> CalculateBlendWeights(const BlendSpace& b,float x,float y)
{
    const auto& samples=b.source.samples;if(samples.empty())return {};
    if(samples.size()==1)return {{0,1}};
    if(b.source.dimensions==1)
    {
        std::vector<uint32_t> order(samples.size());for(uint32_t i=0;i<order.size();++i)order[i]=i;
        std::sort(order.begin(),order.end(),[&](auto a,auto c){return samples[a].x<samples[c].x;});
        if(x<=samples[order.front()].x)return {{order.front(),1}};
        if(x>=samples[order.back()].x)return {{order.back(),1}};
        for(size_t i=1;i<order.size();++i)if(x<=samples[order[i]].x)
        {float w=(x-samples[order[i-1]].x)/(samples[order[i]].x-samples[order[i-1]].x);return {{order[i-1],1-w},{order[i],w}};}
    }
    for(const auto& t:b.triangles)
    {
        const auto& a=samples[t[0]];const auto& c=samples[t[1]];const auto& d=samples[t[2]];
        const float denominator=(c.y-d.y)*(a.x-d.x)+(d.x-c.x)*(a.y-d.y);
        if(std::abs(denominator)<1e-12f)continue;
        float wa=((c.y-d.y)*(x-d.x)+(d.x-c.x)*(y-d.y))/denominator;
        float wb=((d.y-a.y)*(x-d.x)+(a.x-d.x)*(y-d.y))/denominator;float wc=1-wa-wb;
        if(wa>=-1e-6f && wb>=-1e-6f && wc>=-1e-6f)
        {wa=std::max(0.f,wa);wb=std::max(0.f,wb);wc=std::max(0.f,wc);const float sum=wa+wb+wc;return {{t[0],wa/sum},{t[1],wb/sum},{t[2],wc/sum}};}
    }
    // Degenerate sample sets and outside-hull coordinates use the closest edge.
    float best=std::numeric_limits<float>::max(),bestT=0;uint32_t first=0,second=0;
    std::map<std::pair<uint32_t,uint32_t>,uint32_t> edges;
    for(const auto& t:b.triangles)for(size_t i=0;i<3;++i){auto a=t[i],c=t[(i+1)%3];if(c<a)std::swap(a,c);++edges[{a,c}];}
    if(edges.empty())for(uint32_t i=0;i<samples.size();++i)for(uint32_t k=i+1;k<samples.size();++k)edges[{i,k}]=1;
    for(const auto& [edge,count]:edges)
    {
        if(count!=1)continue;const auto& a=samples[edge.first];const auto& c=samples[edge.second];
        const float length=Square(c.x-a.x)+Square(c.y-a.y);
        const float t=length>0?std::clamp(((x-a.x)*(c.x-a.x)+(y-a.y)*(c.y-a.y))/length,0.f,1.f):0;
        const float distance=Square(x-a.x-t*(c.x-a.x))+Square(y-a.y-t*(c.y-a.y));
        if(distance<best){best=distance;first=edge.first;second=edge.second;bestT=t;}
    }
    return {{first,1-bestT},{second,bestT}};
}

struct AnimationInstance::Impl
{
    struct NodeState
    {
        Pose pose,scratch,frozen;
        SamplingContext sampler;
        std::vector<SamplingContext> blendSamplers;
        std::vector<double> previousSamples,currentSamples;
        std::vector<BlendWeight> blendWeights;
        std::array<Pose,3> samplePoses;
        std::vector<const Pose*> inputPoses;
        double time=0,previous=0;
        bool evaluated=false,hasAdvanced=false;
    };
    struct MachineState
    {
        uint32_t state=0,from=UINT32_MAX;
        double age=0,blendAge=0,duration=0;
        BlendCurve curve=BlendCurve::Linear;
        bool interruptible=true,frozen=false;
        Pose interrupted;
    };
    std::shared_ptr<const AnimationProgram> program;
    std::vector<Value> pending,parameters;
    std::vector<NodeState> nodes;
    std::vector<MachineState> machines;
    std::unordered_map<uint64_t,uint32_t> nodeIds;
    std::shared_ptr<const AnimationFrameResult> frame;
    std::vector<float> weights;
    std::map<std::pair<uint64_t,uint64_t>,NotifyEvent> activeNotifies;
    std::vector<NotifyEvent> cleanup;
    double age=0,dt=0;
    float speed=1;
    bool paused=false,playing=true,looping=true;
    uint64_t revision=0,history=1;

    int ParameterIndex(uint64_t id) const
    {if(program)for(size_t i=0;i<program->parameters.size();++i)if(program->parameters[i].id==id)return int(i);return -1;}
    int ParameterIndex(std::string_view name) const
    {if(program)for(size_t i=0;i<program->parameters.size();++i)if(program->parameters[i].name==name)return int(i);return -1;}
    bool Set(int i,ParameterType type,Value v)
    {
        if(i<0 || program->parameters[size_t(i)].type!=type || !std::all_of(v.begin(),v.end(),[](float f){return std::isfinite(f);}))return false;
        pending[size_t(i)]=v;return true;
    }
    float Scalar(int i,float fallback)const{return i<0?fallback:parameters[size_t(i)][0];}
    float Alpha(const MachineState& s)const
    {float a=s.duration>0?float(std::clamp(s.blendAge/s.duration,0.,1.)):1.f;return s.curve==BlendCurve::SmoothStep?a*a*(3-2*a):a;}
    double Duration(uint32_t node,std::set<uint32_t>& seen) const
    {
        if(!seen.insert(node).second)return 0;const auto& n=program->nodes[node];
        if(n.clip)return n.clip->duration;
        if(n.blendSpace && !n.blendSpace->clips.empty())return n.blendSpace->clips.front()->duration;
        for(auto i:n.inputs){const double duration=Duration(i,seen);if(duration>0)return duration;}return 0;
    }
    bool Test(const Condition& c,double normalized,double remaining,std::vector<int>& triggers)const
    {
        if(c.operation==Compare::All){for(const auto& child:c.children)if(!Test(child,normalized,remaining,triggers))return false;return true;}
        if(c.operation==Compare::Any){for(const auto& child:c.children){auto used=triggers;if(Test(child,normalized,remaining,used)){triggers=std::move(used);return true;}}return false;}
        if(c.operation==Compare::Not){auto used=triggers;return !c.children.empty()&&!Test(c.children.front(),normalized,remaining,used);}
        float v=c.parameter=="$NormalizedTime"?float(normalized):c.parameter=="$TimeRemaining"?float(remaining):Scalar(c.parameterIndex,0);
        bool result=false;switch(c.operation)
        {
        case Compare::True:result=v!=0;break;case Compare::False:result=v==0;break;
        case Compare::Less:result=v<c.value;break;case Compare::LessEqual:result=v<=c.value;break;
        case Compare::Greater:result=v>c.value;break;case Compare::GreaterEqual:result=v>=c.value;break;
        case Compare::Equal:result=v==c.value;break;case Compare::NotEqual:result=v!=c.value;break;default:break;
        }
        if(result&&c.parameterIndex>=0&&program->parameters[size_t(c.parameterIndex)].type==ParameterType::Trigger)triggers.push_back(c.parameterIndex);
        return result;
    }
    void CloseNotifies()
    {for(auto& [key,event]:activeNotifies){event.phase=NotifyPhase::End;cleanup.push_back(event);}activeNotifies.clear();}
    void ResetNode(uint32_t index,std::set<uint32_t>& seen)
    {
        if(!seen.insert(index).second)return;auto& state=nodes[index];state.time=state.previous=0;state.hasAdvanced=false;
        std::fill(state.currentSamples.begin(),state.currentSamples.end(),0);std::fill(state.previousSamples.begin(),state.previousSamples.end(),0);
        for(auto input:program->nodes[index].inputs)ResetNode(input,seen);
    }
    void UpdateMachine(uint32_t index)
    {
        auto& state=machines[index];const auto& machine=program->machines[index];state.age+=std::abs(dt);
        state.blendAge+=std::abs(dt);if(state.duration>0&&state.blendAge>=state.duration){state.duration=0;state.from=UINT32_MAX;state.frozen=false;}
        for(uint32_t count=0;count<8;++count)
        {
            if(state.duration>0&&!state.interruptible)break;
            std::set<uint32_t> seen;const double duration=Duration(nodeIds.at(machine.states[state.state].pose),seen);
            const double normalized=duration>0?state.age/duration:1;
            const Transition* selected=nullptr;std::vector<int> consume;
            for(const auto& transition:machine.transitions)
            {
                if(transition.to==machine.states[state.state].id || (transition.from && transition.from!=machine.states[state.state].id) || (transition.exitTime>=0&&normalized<transition.exitTime))continue;
                std::vector<int> triggers;if(!Test(transition.condition,normalized,std::max(0.,duration-state.age),triggers))continue;
                if(!selected || transition.priority>selected->priority || (transition.priority==selected->priority&&transition.id<selected->id)){selected=&transition;consume=std::move(triggers);}
            }
            if(!selected)break;
            for(auto parameter:consume)parameters[size_t(parameter)][0]=pending[size_t(parameter)][0]=0;
            const uint32_t next=FindState(machine,selected->to);if(next==UINT32_MAX)break;
            // The previous output is the precise interruption source, including an in-flight crossfade.
            for(size_t n=0;n<program->nodes.size();++n)if(program->nodes[n].machine==int(index))state.interrupted=nodes[n].pose;
            state.frozen=state.duration>0;state.from=state.state;state.state=next;state.age=0;
            state.duration=selected->duration;state.blendAge=0;state.curve=selected->curve;state.interruptible=selected->interruptible;
            std::set<uint32_t> reset;ResetNode(nodeIds.at(machine.states[next].pose),reset);
            if(state.duration>0)break;
        }
    }
    void Accumulate(uint32_t index,float contribution)
    {
        const auto& n=program->nodes[index];weights[index]+=contribution;
        switch(n.source.kind)
        {
        case NodeKind::Blend:case NodeKind::LayeredBlend:case NodeKind::Additive:
        {const float w=std::clamp(Scalar(n.parameter,n.source.weight),0.f,1.f);Accumulate(n.inputs[0],contribution*(n.source.kind==NodeKind::Blend?1-w:1));Accumulate(n.inputs[1],contribution*w);break;}
        case NodeKind::MultiBlend:
        {float sum=0;for(float w:n.source.weights)sum+=std::max(0.f,w);for(size_t k=0;k<n.inputs.size();++k)Accumulate(n.inputs[k],sum>0?contribution*std::max(0.f,n.source.weights[k])/sum:0);break;}
        case NodeKind::StateMachine:
        {const auto& s=machines[size_t(n.machine)];const auto& machine=program->machines[size_t(n.machine)];const float a=Alpha(s);Accumulate(nodeIds.at(machine.states[s.state].pose),contribution*a);if(s.from!=UINT32_MAX&&!s.frozen)Accumulate(nodeIds.at(machine.states[s.from].pose),contribution*(1-a));break;}
        default:for(auto child:n.inputs)Accumulate(child,contribution);break;
        }
    }
    bool Evaluate(uint32_t index,std::string& error)
    {
        auto& s=nodes[index];if(s.evaluated)return true;s.evaluated=true;
        const auto& n=program->nodes[index];const auto& skeleton=*program->skeleton;
        auto evaluate=[&](uint32_t child){return Evaluate(child,error);};
        auto source=[&](size_t i)->Pose& {return nodes[n.inputs[i]].pose;};
        switch(n.source.kind)
        {
        case NodeKind::ReferencePose:s.pose=skeleton.ReferencePose();break;
        case NodeKind::SequencePlayer:case NodeKind::SequenceEvaluator:
        {
            s.previous=s.time;if(n.source.kind==NodeKind::SequencePlayer)
            {s.time+=dt;if(dt!=0&&!s.hasAdvanced){s.previous=std::nextafter(s.previous,dt>0?-std::numeric_limits<double>::infinity():std::numeric_limits<double>::infinity());s.hasAdvanced=true;}}
            else s.time=Scalar(n.parameter,float(n.source.sampleTime));
            const double time=n.source.loop&&looping?WrapTime(s.time,n.clip->duration):std::clamp(s.time,0.,n.clip->duration);
            if(!Sample(skeleton,*n.clip,time,s.pose,s.sampler,error))return false;break;
        }
        case NodeKind::BlendSpacePlayer:
        {
            s.blendWeights=CalculateBlendWeights(*n.blendSpace,Scalar(n.xParameter,0),Scalar(n.yParameter,0));
            double duration=0;for(auto w:s.blendWeights)duration+=n.blendSpace->clips[w.sample]->duration*w.weight;
            s.previous=s.time;s.time+=duration>0?dt/duration:0;s.pose=skeleton.ReferencePose();
            std::array<const Pose*,3> poses{};std::array<float,3> blendWeights{};size_t sampleIndex=0;
            for(auto w:s.blendWeights)
            {
                if(w.weight<=0)continue;const auto& clip=*n.blendSpace->clips[w.sample];
                s.previousSamples[w.sample]=s.previous*clip.duration;s.currentSamples[w.sample]=s.time*clip.duration;
                const auto time=n.source.loop&&looping?WrapTime(s.currentSamples[w.sample],clip.duration):std::clamp(s.currentSamples[w.sample],0.,clip.duration);
                if(sampleIndex>=s.samplePoses.size()){error="BlendSpace exceeded three active samples";return false;}
                if(!Sample(skeleton,clip,time,s.samplePoses[sampleIndex],s.blendSamplers[w.sample],error))return false;
                poses[sampleIndex]=&s.samplePoses[sampleIndex];blendWeights[sampleIndex]=w.weight;++sampleIndex;
            }
            if(sampleIndex)BlendWeightedPoses(std::span<const Pose* const>(poses.data(),sampleIndex),std::span<const float>(blendWeights.data(),sampleIndex),s.pose);
            break;
        }
        case NodeKind::Blend:case NodeKind::LayeredBlend:case NodeKind::Additive:
        {
            if(!evaluate(n.inputs[0])||!evaluate(n.inputs[1]))return false;const auto weight=Scalar(n.parameter,n.source.weight);
            if(n.source.kind==NodeKind::Additive){s.scratch=skeleton.ReferencePose();if(n.inputs.size()>2){if(!evaluate(n.inputs[2]))return false;s.scratch=source(2);}if(!Additive(source(0),source(1),s.scratch,weight,s.pose)){error="Additive reference pose has a singular scale or incompatible pose";return false;}}
            else BlendPoses(source(0),source(1),weight,s.pose,n.mask);break;
        }
        case NodeKind::MultiBlend:
        {
            for(size_t k=0;k<n.inputs.size();++k){if(!evaluate(n.inputs[k]))return false;s.inputPoses[k]=&source(k);}
            if(!BlendWeightedPoses(s.inputPoses,n.source.weights,s.pose))s.pose=skeleton.ReferencePose();break;
        }
        case NodeKind::StateMachine:
        {
            const auto& state=machines[size_t(n.machine)];const auto& machine=program->machines[size_t(n.machine)];const auto current=nodeIds.at(machine.states[state.state].pose);
            if(!evaluate(current))return false;s.pose=nodes[current].pose;
            if(state.from!=UINT32_MAX)
            {
                if(state.frozen)BlendPoses(state.interrupted,s.pose,Alpha(state),s.pose);
                else {const auto from=nodeIds.at(machine.states[state.from].pose);if(!evaluate(from))return false;BlendPoses(nodes[from].pose,s.pose,Alpha(state),s.pose);}
            }
            break;
        }
        default:if(!evaluate(n.inputs[0]))return false;s.pose=source(0);break;
        }
        return true;
    }
};
AnimationInstance::AnimationInstance():m(std::make_unique<Impl>()){}
AnimationInstance::~AnimationInstance()=default;
bool AnimationInstance::SetProgram(std::shared_ptr<const AnimationProgram> program,std::string& error)
{
    if(!program||!program->skeleton||program->nodes.empty()||program->output>=program->nodes.size()||!program->skeleton->Validate(error))
    {if(error.empty())error="Animation program is incomplete";return false;}
    auto next=std::make_unique<Impl>();next->program=std::move(program);next->history=m->history+1;next->revision=m->revision;
    next->speed=m->speed;next->paused=m->paused;next->playing=m->playing;next->looping=m->looping;
    next->pending.reserve(next->program->parameters.size());for(const auto& p:next->program->parameters)
    {auto previous=m->ParameterIndex(p.id);next->pending.push_back(previous>=0&&m->program->parameters[size_t(previous)].type==p.type?m->pending[size_t(previous)]:p.initial);}
    next->parameters=next->pending;next->nodes.resize(next->program->nodes.size());next->weights.resize(next->nodes.size());
    for(uint32_t i=0;i<next->nodes.size();++i)
    {
        const auto& n=next->program->nodes[i];next->nodeIds.emplace(n.source.id,i);auto& s=next->nodes[i];
        s.pose=next->program->skeleton->ReferencePose();s.scratch=s.pose;
        s.inputPoses.resize(n.inputs.size());for(auto& sample:s.samplePoses)sample.resize(next->program->skeleton->bones.size());
        if(n.clip&&!s.sampler.Bind(*next->program->skeleton,*n.clip,error))return false;
        if(n.blendSpace){s.blendSamplers.resize(n.blendSpace->clips.size());s.previousSamples.resize(s.blendSamplers.size());s.currentSamples.resize(s.blendSamplers.size());for(size_t k=0;k<s.blendSamplers.size();++k)if(!s.blendSamplers[k].Bind(*next->program->skeleton,*n.blendSpace->clips[k],error))return false;}
    }
    next->machines.resize(next->program->machines.size());for(size_t i=0;i<next->machines.size();++i)
    {auto index=FindState(next->program->machines[i],next->program->machines[i].initial);if(index==UINT32_MAX){error="State machine has no valid initial state";return false;}next->machines[i].state=index;}
    auto previous=std::move(m);m=std::move(next);
    if(!Tick(0,error)){m=std::move(previous);return false;}
    previous->CloseNotifies();
    if(!previous->cleanup.empty())
    {auto frame=std::make_shared<AnimationFrameResult>(*m->frame);frame->notifies.insert(frame->notifies.end(),previous->cleanup.begin(),previous->cleanup.end());m->frame=std::move(frame);}
    return true;
}
bool AnimationInstance::PlayClip(std::shared_ptr<const Skeleton> skeleton,std::shared_ptr<const Clip> clip,std::string& error)
{
    if(!skeleton||!clip||!clip->Validate(*skeleton,error))return false;
    auto program=std::make_shared<AnimationProgram>();program->skeleton=std::move(skeleton);program->id=clip->id;
    CompiledNode n;n.source.id=1;n.source.kind=NodeKind::SequencePlayer;n.clip=std::move(clip);program->nodes.push_back(std::move(n));
    return SetProgram(std::move(program),error);
}
bool AnimationInstance::SetFloat(uint64_t id,float v){return m->Set(m->ParameterIndex(id),ParameterType::Float,{v,0,0,0});}
bool AnimationInstance::SetBool(uint64_t id,bool v){return m->Set(m->ParameterIndex(id),ParameterType::Bool,{v?1.f:0.f,0,0,0});}
bool AnimationInstance::SetVector(uint64_t id,Value v){return m->Set(m->ParameterIndex(id),ParameterType::Vector,v);}
bool AnimationInstance::SetTrigger(uint64_t id){return m->Set(m->ParameterIndex(id),ParameterType::Trigger,{1,0,0,0});}
bool AnimationInstance::SetFloat(std::string_view name,float v){return m->Set(m->ParameterIndex(name),ParameterType::Float,{v,0,0,0});}
bool AnimationInstance::SetBool(std::string_view name,bool v){return m->Set(m->ParameterIndex(name),ParameterType::Bool,{v?1.f:0.f,0,0,0});}
bool AnimationInstance::SetVector(std::string_view name,Value v){return m->Set(m->ParameterIndex(name),ParameterType::Vector,v);}
bool AnimationInstance::SetTrigger(std::string_view name){return m->Set(m->ParameterIndex(name),ParameterType::Trigger,{1,0,0,0});}
void AnimationInstance::Play(){m->playing=true;m->paused=false;}
void AnimationInstance::Pause(bool paused){m->paused=paused;}
void AnimationInstance::Stop(){m->CloseNotifies();m->playing=false;Seek(0);}
void AnimationInstance::Seek(double seconds)
{
    if(!std::isfinite(seconds))return;m->CloseNotifies();auto cleanup=std::move(m->cleanup);m->cleanup.clear();
    m->age=seconds;for(auto& n:m->nodes){n.time=n.previous=seconds;n.hasAdvanced=true;}
    for(auto& s:m->machines){s.age=seconds;s.from=UINT32_MAX;s.duration=0;s.frozen=false;}++m->history;
    std::string ignored;if(m->program)Tick(0,ignored);
    m->cleanup.insert(m->cleanup.end(),cleanup.begin(),cleanup.end());
}
void AnimationInstance::SetLooping(bool loop){m->looping=loop;}
void AnimationInstance::SetSpeed(float speed){if(std::isfinite(speed))m->speed=speed;}
bool AnimationInstance::IsPaused()const{return m->paused||!m->playing;}
double AnimationInstance::Age()const{return m->age;}
const std::shared_ptr<const AnimationProgram>& AnimationInstance::Program()const{return m->program;}
std::shared_ptr<const AnimationFrameResult> AnimationInstance::Capture()const{return m->frame;}
bool AnimationInstance::Tick(double deltaTime,std::string& error)
{
    if(!m->program){error="Animation instance has no program";return false;}
    if(!std::isfinite(deltaTime)||deltaTime<0){error="Animation delta must be finite and nonnegative";return false;}
    m->dt=m->paused||!m->playing?0:deltaTime*m->speed;m->age+=m->dt;m->parameters=m->pending;
    for(auto& state:m->nodes)state.evaluated=false;
    if(m->dt!=0)for(uint32_t i=0;i<m->machines.size();++i)m->UpdateMachine(i);
    std::fill(m->weights.begin(),m->weights.end(),0);m->Accumulate(m->program->output,1);
    if(!m->Evaluate(m->program->output,error))return false;
    auto result=std::make_shared<AnimationFrameResult>();result->revision=++m->revision;result->historyGeneration=m->history;
    result->age=m->age;result->local=m->nodes[m->program->output].pose;result->nodeWeights=m->weights;
    result->skeletonSignature=m->program->skeleton->Signature();
    if(!LocalToModel(*m->program->skeleton,result->local,result->model,error))return false;
    result->notifies=std::move(m->cleanup);m->cleanup.clear();
    const size_t cleanupCount=result->notifies.size();
    std::set<std::pair<uint64_t,uint64_t>> contributing;
    for(size_t i=0;i<m->nodes.size();++i)
    {
        const auto& n=m->program->nodes[i];const auto& s=m->nodes[i];const float weight=std::clamp(m->weights[i],0.f,1.f);
        if(!s.evaluated||weight<=0)continue;
        auto contribute=[&](const Clip& clip,double previous,double current,float w,uint64_t player)
        {
            if(m->dt!=0&&n.source.kind!=NodeKind::SequenceEvaluator)CollectNotifies(clip,previous,current,player,w,n.source.loop&&m->looping,result->notifies);
            const double time=n.source.loop&&m->looping?WrapTime(current,clip.duration):std::clamp(current,0.,clip.duration);
            for(const auto& c:clip.curves)result->curves[c.name]+=c.channel.Sample(time,{})[0]*w;
            for(const auto& notify:clip.notifies)if(w>=notify.threshold)contributing.insert({player,notify.id});
            if(m->dt!=0&&n.source.kind!=NodeKind::SequenceEvaluator)
                for(const auto& notify:clip.notifies)
                    if(notify.end>notify.start&&w>=notify.threshold&&time>=notify.start&&time<notify.end&&!m->activeNotifies.contains({player,notify.id})&&
                        std::none_of(result->notifies.begin(),result->notifies.end(),[&](const auto& event){return event.player==player&&event.id==notify.id&&event.phase==NotifyPhase::Begin;}))
                        result->notifies.push_back({player,notify.id,notify.name,notify.payload,NotifyPhase::Begin,
                            n.source.loop&&m->looping&&clip.duration>0?int64_t(std::floor(current/clip.duration)):0,current});
        };
        if(n.clip)contribute(*n.clip,s.previous,s.time,weight,n.source.id);
        if(n.blendSpace)for(auto w:s.blendWeights)contribute(*n.blendSpace->clips[w.sample],s.previousSamples[w.sample],s.currentSamples[w.sample],weight*w.weight,n.source.id*65536+w.sample+1);
    }
    for(auto it=m->activeNotifies.begin();it!=m->activeNotifies.end();)
        if(!contributing.contains(it->first)){auto event=it->second;event.phase=NotifyPhase::End;result->notifies.push_back(event);it=m->activeNotifies.erase(it);}else ++it;
    for(size_t i=0;i<result->notifies.size();)
    {
        const auto& event=result->notifies[i];const auto key=std::pair{event.player,event.id};
        // A looping interval ending at zero also belongs to the preceding
        // cycle. Do not close a state that this player never opened. Explicit
        // Seek/Stop/interruption cleanup has already removed its active entry.
        const bool redundant=i>=cleanupCount&&
            ((event.phase==NotifyPhase::End&&!m->activeNotifies.contains(key))||
             (event.phase==NotifyPhase::Begin&&m->activeNotifies.contains(key)));
        if(redundant){result->notifies.erase(result->notifies.begin()+i);continue;}
        if(event.phase==NotifyPhase::Begin)m->activeNotifies[key]=event;
        else if(event.phase==NotifyPhase::End)m->activeNotifies.erase(key);
        ++i;
    }
    for(size_t i=0;i<m->machines.size();++i)result->activeStates.push_back(m->program->machines[i].states[m->machines[i].state].id);
    m->frame=std::move(result);return true;
}
void AnimationSystem::Register(AnimationInstance& instance){if(std::find(m_instances.begin(),m_instances.end(),&instance)==m_instances.end())m_instances.push_back(&instance);}
void AnimationSystem::Unregister(AnimationInstance& instance){std::erase(m_instances,&instance);m_dispatched.erase(&instance);}
bool AnimationSystem::Update(double dt,std::string& error){for(auto* instance:m_instances)if(!instance->Tick(dt,error))return false;return true;}
void AnimationSystem::Finalize(const std::function<void(AnimationInstance&,const NotifyEvent&)>& sink)
{
    for(auto* instance:m_instances){auto frame=instance->Capture();if(!frame||m_dispatched[instance]==frame->revision)continue;m_dispatched[instance]=frame->revision;for(const auto& event:frame->notifies)sink(*instance,event);}
}
}
