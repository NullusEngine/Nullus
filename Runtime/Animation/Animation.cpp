#include "Animation.h"
#include <Assets/ArtifactManifest.h>
#include <Json/json.hpp>
#include <Math/Vector4.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <charconv>
#include <limits>
#include <map>
#include <numeric>
#include <unordered_set>

namespace NLS::Animation
{
namespace
{
bool Finite(const Value& v) {return std::all_of(v.begin(),v.end(),[](float f){return std::isfinite(f);});}
Maths::Quaternion Quaternion(Value v)
{
    float length = 0; for(float f:v) length += f*f;
    if (!(length > 1e-16f) || !std::isfinite(length)) return Maths::Quaternion::Identity;
    const float inv = 1/std::sqrt(length);
    return {v[0]*inv,v[1]*inv,v[2]*inv,v[3]*inv};
}
Value Values(const Maths::Quaternion& q) {return {q.x,q.y,q.z,q.w};}
Value Values(const Maths::Vector3& v) {return {v.x,v.y,v.z,0};}
Maths::Vector3 Vector(Value v) {return {v[0],v[1],v[2]};}
Maths::Vector3 Unit(const Maths::Vector3& v)
{
    const float l = std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);
    return l > 1e-12f ? v*(1/l) : Maths::Vector3{};
}
using Json = nlohmann::json;
Json WriteTransform(const Transform& v) {return Json{{"t",Values(v.translation)},{"r",Values(v.rotation)},{"s",Values(v.scale)}};}
Transform ReadTransform(const Json& j)
{
    Transform v; v.translation=Vector(j.at("t").get<Value>()); v.rotation=Quaternion(j.at("r").get<Value>());
    v.scale=Vector(j.at("s").get<Value>()); return v;
}
Json WriteChannel(const Channel& c) {return Json{{"interpolation",int(c.interpolation)},{"times",c.times},{"values",c.values},{"in",c.inTangents},{"out",c.outTangents}};}
Channel ReadChannel(const Json& j)
{
    Channel c; c.interpolation=static_cast<Interpolation>(j.at("interpolation").get<int>());
    c.times=j.at("times").get<std::vector<double>>(); c.values=j.at("values").get<std::vector<Value>>();
    c.inTangents=j.at("in").get<std::vector<Value>>(); c.outTangents=j.at("out").get<std::vector<Value>>(); return c;
}
Json Matrix(const Maths::Matrix4& m) {return std::vector<float>(m.data,m.data+16);}
Maths::Matrix4 ReadMatrix(const Json& j)
{
    if (!j.is_array() || j.size()!=16) throw std::runtime_error("Expected a 4x4 matrix");
    Maths::Matrix4 m; for(size_t i=0;i<16;++i) m.data[i]=j.at(i).get<float>(); return m;
}
}

BoneIndex Skeleton::Find(std::string_view name) const
{
    for(BoneIndex i=0;i<bones.size();++i) if(bones[i].id==name) return i;
    BoneIndex found=NoBone;
    for(BoneIndex i=0;i<bones.size();++i) if(bones[i].name==name) {if(found!=NoBone) return NoBone; found=i;}
    return found;
}
bool Skeleton::Validate(std::string& error) const
{
    if (bones.empty() || bones.size()>65535) {error="Skeleton needs 1..65535 bones"; return false;}
    std::unordered_set<std::string> ids;
    for(size_t i=0;i<bones.size();++i)
    {
        const auto& b=bones[i];
        if(b.id.empty() || !ids.insert(b.id).second || (b.parent!=NoBone && b.parent>=i))
        {error="Skeleton has duplicate identities or is not ordered parent before child: "+b.id; return false;}
        if(!Finite(Values(b.reference.translation)) || !Finite(Values(b.reference.rotation)) || !Finite(Values(b.reference.scale)))
        {error="Skeleton reference transform is not finite: "+b.id; return false;}
    }
    return true;
}
Pose Skeleton::ReferencePose() const {Pose p; p.reserve(bones.size()); for(const auto& b:bones)p.push_back(b.reference); return p;}
std::string Skeleton::Signature() const
{
    std::string value; for(const auto& b:bones)
    {
        value+=b.id;value+='\0';value+=std::to_string(b.parent);value+='\0';
        const auto matrix=ToMatrix(b.reference);
        for(float component:matrix.data)
        {std::array<char,128> text{};const auto converted=std::to_chars(text.data(),text.data()+text.size(),component==0?0.f:component,std::chars_format::fixed,4);value.append(text.data(),converted.ptr);value+='\0';}
    }
    return Core::Assets::ToHexString(Core::Assets::ComputeSha256(reinterpret_cast<const uint8_t*>(value.data()),value.size()));
}
bool Channel::Validate(std::string& error) const
{
    if(int(interpolation)>int(Interpolation::CubicSpline) || times.size()!=values.size() ||
       (interpolation==Interpolation::CubicSpline && (inTangents.size()!=times.size() || outTangents.size()!=times.size())))
    {error="Animation channel has an invalid interpolation or key count"; return false;}
    for(size_t i=0;i<times.size();++i)
        if(!std::isfinite(times[i]) || times[i]<0 || (i && times[i]<=times[i-1]) || !Finite(values[i]) ||
           (interpolation==Interpolation::CubicSpline && (!Finite(inTangents[i]) || !Finite(outTangents[i]))))
        {error="Animation channel contains unordered times or non-finite values"; return false;}
    return true;
}
Value Channel::Sample(double time, Value fallback, bool quaternion) const
{
    if(times.empty()) return fallback;
    if(time<=times.front() || times.size()==1) return quaternion?Values(Quaternion(values.front())):values.front();
    if(time>=times.back()) return quaternion?Values(Quaternion(values.back())):values.back();
    const auto upper=std::upper_bound(times.begin(),times.end(),time);
    const size_t next=size_t(upper-times.begin()), previous=next-1;
    if(interpolation==Interpolation::Step) return quaternion?Values(Quaternion(values[previous])):values[previous];
    const float dt=float(times[next]-times[previous]);
    const float t=float((time-times[previous])/dt);
    if(quaternion && interpolation==Interpolation::Linear)
    {
        auto a=Quaternion(values[previous]),b=Quaternion(values[next]);
        if(Maths::Quaternion::DotProduct(a,b)<0) b=b*(-1.f);
        return Values(Maths::Quaternion::Slerp(a,b,t));
    }
    Value result{};
    for(size_t i=0;i<4;++i)
        result[i]=interpolation==Interpolation::CubicSpline ?
            (2*t*t*t-3*t*t+1)*values[previous][i]+(t*t*t-2*t*t+t)*dt*outTangents[previous][i]+
            (-2*t*t*t+3*t*t)*values[next][i]+(t*t*t-t*t)*dt*inTangents[next][i] :
            values[previous][i]+(values[next][i]-values[previous][i])*t;
    return quaternion?Values(Quaternion(result)):result;
}
bool Clip::Validate(const Skeleton& target,std::string& error) const
{
    if(!std::isfinite(duration) || duration<0 || (!skeleton.empty() && skeleton!=target.id))
    {error="Animation duration or skeleton binding is invalid"; return false;}
    std::unordered_set<BoneIndex> mapped;
    for(const auto& t:tracks)
    {
        const auto bone=target.Find(t.bone);
        if(bone==NoBone || !mapped.insert(bone).second) {error="Missing, ambiguous or duplicate animation bone: "+t.bone; return false;}
        for(const auto* c:{&t.translation,&t.rotation,&t.scale})
            if(!c->Validate(error) || (!c->times.empty() && c->times.back()>duration+1e-6))
            {if(error.empty()) error="Animation key exceeds clip duration"; return false;}
    }
    for(const auto& c:curves) if(!c.channel.Validate(error)) return false;
    std::unordered_set<uint64_t> ids;
    for(const auto& n:notifies)
        if(!n.id || !ids.insert(n.id).second || !std::isfinite(n.start) || !std::isfinite(n.end) ||
            n.start<0 || n.end<n.start || n.end>duration || !std::isfinite(n.threshold) || n.threshold<0 || n.threshold>1)
        {error="Notify has an invalid identity, interval or weight threshold"; return false;}
    return true;
}
bool SamplingContext::Bind(const Skeleton& target,const Clip& source,std::string& error)
{
    if(!source.Validate(target,error)) return false;
    trackBones.clear(); trackBones.reserve(source.tracks.size());
    for(const auto& t:source.tracks) trackBones.push_back(target.Find(t.bone));
    clip=&source; skeletonSignature=target.Signature(); return true;
}
Maths::Matrix4 ToMatrix(const Transform& t)
{return Maths::Matrix4::Translation(t.translation)*Maths::Matrix4::Rotation(t.rotation)*Maths::Matrix4::Scaling(t.scale);}
bool Decompose(const Maths::Matrix4& m,Transform& t,std::string& error)
{
    for(float f:m.data) if(!std::isfinite(f)) {error="Transform matrix is not finite"; return false;}
    t.translation={m.data[3],m.data[7],m.data[11]};
    auto length=[&](size_t i){return std::sqrt(m.data[i]*m.data[i]+m.data[i+4]*m.data[i+4]+m.data[i+8]*m.data[i+8]);};
    t.scale={length(0),length(1),length(2)};
    if(t.scale.x<1e-8f || t.scale.y<1e-8f || t.scale.z<1e-8f) {error="Transform matrix has a singular scale"; return false;}
    auto r=m; if(Maths::Matrix4::Determinant(m)<0) t.scale.x=-t.scale.x;
    for(size_t row=0;row<3;++row) {r.data[row*4]/=t.scale.x; r.data[row*4+1]/=t.scale.y; r.data[row*4+2]/=t.scale.z;}
    r.data[3]=r.data[7]=r.data[11]=0; t.rotation=Maths::Quaternion::Normalize(Maths::Quaternion(r));
    const auto reconstructed=ToMatrix(t);
    for(size_t i=0;i<16;++i) if(std::abs(reconstructed.data[i]-m.data[i])>1e-4f*(1+std::abs(m.data[i])))
    {error="Transform matrix contains shear or a non-TRS transform"; return false;}
    return true;
}
double WrapTime(double t,double duration) {if(!(duration>0))return 0; auto v=std::fmod(t,duration); return v<0?v+duration:v;}
bool Sample(const Skeleton& skeleton,const Clip& clip,double time,Pose& output,SamplingContext& context,std::string& error)
{
    if(!std::isfinite(time)) {error="Animation sample time is not finite";return false;}
    if(context.clip!=&clip || context.trackBones.size()!=clip.tracks.size()) if(!context.Bind(skeleton,clip,error))return false;
    output.resize(skeleton.bones.size());
    for(size_t i=0;i<output.size();++i) output[i]=skeleton.bones[i].reference;
    for(size_t i=0;i<clip.tracks.size();++i)
    {
        const auto& track=clip.tracks[i]; auto& t=output[context.trackBones[i]];
        t.translation=Vector(track.translation.Sample(time,Values(t.translation)));
        t.rotation=Quaternion(track.rotation.Sample(time,Values(t.rotation),true));
        t.scale=Vector(track.scale.Sample(time,Values(t.scale)));
    }
    return true;
}
bool LocalToModel(const Skeleton& skeleton,std::span<const Transform> local,std::vector<Maths::Matrix4>& model,std::string& error)
{
    if(local.size()!=skeleton.bones.size()) {error="Pose size does not match skeleton"; return false;}
    model.resize(local.size());
    for(size_t i=0;i<local.size();++i)
    {
        const auto parent=skeleton.bones[i].parent;
        if(parent!=NoBone && parent>=i) {error="Invalid skeleton hierarchy";return false;}
        model[i]=parent==NoBone?ToMatrix(local[i]):model[parent]*ToMatrix(local[i]);
    }
    return true;
}
Transform Blend(const Transform& a,const Transform& b,float w)
{
    w=std::clamp(w,0.f,1.f); auto q=b.rotation;
    if(Maths::Quaternion::DotProduct(a.rotation,q)<0)q=q*(-1.f);
    return {a.translation+(b.translation-a.translation)*w,Maths::Quaternion::Nlerp(a.rotation,q,w),a.scale+(b.scale-a.scale)*w};
}
bool BlendPoses(std::span<const Transform> a,std::span<const Transform> b,float w,Pose& out,std::span<const float> mask)
{
    if(a.size()!=b.size() || (!mask.empty() && mask.size()!=a.size()) || !std::isfinite(w))return false;
    out.resize(a.size()); for(size_t i=0;i<a.size();++i)out[i]=Blend(a[i],b[i],w*(mask.empty()?1:mask[i]));return true;
}
bool Additive(std::span<const Transform> base,std::span<const Transform> value,std::span<const Transform> reference,float w,Pose& out)
{
    if(base.size()!=value.size() || base.size()!=reference.size() || !std::isfinite(w))return false;
    w=std::clamp(w,0.f,1.f);out.resize(base.size());
    for(size_t i=0;i<base.size();++i)
    {
        const auto& a=base[i];const auto& b=value[i];const auto& r=reference[i];
        if(std::abs(r.scale.x)<1e-8f||std::abs(r.scale.y)<1e-8f||std::abs(r.scale.z)<1e-8f)return false;
        auto delta=Maths::Quaternion::Inverse(r.rotation)*b.rotation;if(delta.w<0)delta=delta*(-1.f);
        out[i]={a.translation+(b.translation-r.translation)*w,
            Maths::Quaternion::Normalize(a.rotation*Maths::Quaternion::Nlerp(Maths::Quaternion::Identity,delta,w)),
            {a.scale.x*(1+(b.scale.x/r.scale.x-1)*w),a.scale.y*(1+(b.scale.y/r.scale.y-1)*w),a.scale.z*(1+(b.scale.z/r.scale.z-1)*w)}};
    }
    return true;
}
bool BlendWeightedPoses(std::span<const Pose* const> poses,std::span<const float> weights,Pose& output)
{
    if(poses.empty()||poses.size()!=weights.size())return false;float total=0;size_t first=0;
    for(size_t i=0;i<poses.size();++i)
    {if(!poses[i]||!std::isfinite(weights[i])||weights[i]<0||poses[i]->size()!=poses[0]->size())return false;if(weights[i]>0&&total==0)first=i;total+=weights[i];}
    if(!(total>0)||!std::isfinite(total))return false;output.resize(poses[0]->size());
    for(size_t bone=0;bone<output.size();++bone)
    {
        Maths::Vector3 translation{},scale{};Maths::Quaternion rotation{0,0,0,0};const auto reference=(*poses[first])[bone].rotation;
        for(size_t i=0;i<poses.size();++i)
        {
            const auto value=(*poses[i])[bone];const float weight=weights[i]/total;translation=translation+value.translation*weight;scale=scale+value.scale*weight;
            auto q=value.rotation;if(Maths::Quaternion::DotProduct(reference,q)<0)q=q*(-1.f);rotation=rotation+q*weight;
        }
        output[bone]={translation,Maths::Quaternion::Normalize(rotation),scale};
    }
    return true;
}
bool BuildPalette(std::span<const Maths::Matrix4> model,const SkinBinding& binding,SkinningPalette& out,std::string& error)
{
    if(binding.joints.size()!=binding.inverseBind.size()) {error="Inverse bind matrix count does not match joints"; return false;}
    out.position.resize(binding.joints.size());out.normal.resize(binding.joints.size());
    for(size_t i=0;i<binding.joints.size();++i)
    {
        if(binding.joints[i]>=model.size()) {error="Skin joint index is out of range";return false;}
        out.position[i]=binding.meshFromSkeleton*model[binding.joints[i]]*binding.inverseBind[i];
        out.normal[i]=std::abs(Maths::Matrix4::Determinant(out.position[i]))>1e-12f?
            Maths::Matrix4::Transpose(Maths::Matrix4::Inverse(out.position[i])):Maths::Matrix4::Identity;
    }
    return true;
}
bool PackInfluences(std::span<const std::pair<uint32_t,float>> input,uint32_t count,Influence& output,bool& truncated,std::string& error)
{
    output={};truncated=false;std::map<uint32_t,double> merged;
    for(auto [joint,weight]:input)
    {
        if(joint>=count || joint>65535 || !std::isfinite(weight) || weight<0) {error="Invalid skin joint or weight";return false;}
        if(weight>0)merged[joint]+=weight;
    }
    std::vector<std::pair<uint32_t,double>> weights(merged.begin(),merged.end());
    std::sort(weights.begin(),weights.end(),[](auto a,auto b){return a.second!=b.second?a.second>b.second:a.first<b.first;});
    truncated=weights.size()>4;if(truncated)weights.resize(4);
    double sum=0;for(auto w:weights)sum+=w.second;
    if(!(sum>0) || !std::isfinite(sum)) {error="Skinned vertex has no positive weights";return false;}
    uint32_t total=0;
    for(size_t i=0;i<weights.size();++i) {output.joints[i]=uint16_t(weights[i].first);output.weights[i]=uint16_t(std::floor(weights[i].second/sum*65535));total+=output.weights[i];}
    output.weights[0]=uint16_t(output.weights[0]+65535-total);return true;
}
void CollectNotifies(const Clip& clip,double prev,double next,uint64_t player,float weight,bool looping,std::vector<NotifyEvent>& events)
{
    if(prev==next || !std::isfinite(prev) || !std::isfinite(next))return;
    const bool forward=next>prev;
    const double low=std::min(prev,next),high=std::max(prev,next);
    for(const auto& n:clip.notifies)
    {
        if(weight<n.threshold)continue;
        auto collect=[&](double at,NotifyPhase phase)
        {
            const bool loop=looping && clip.duration>0;
            const int64_t first=loop?int64_t(std::floor((low-at)/clip.duration)):0;
            const int64_t last=loop?int64_t(std::ceil((high-at)/clip.duration)):0;
            for(int64_t cycle=first;cycle<=last;++cycle)
            {
                const double t=at+(loop?cycle*clip.duration:0);
                if(forward?(t>prev && t<=next):(t>=next && t<prev))
                {
                    auto emitted=phase;
                    if(!forward && phase!=NotifyPhase::Instant)emitted=phase==NotifyPhase::Begin?NotifyPhase::End:NotifyPhase::Begin;
                    events.push_back({player,n.id,n.name,n.payload,emitted,cycle,t});
                }
            }
        };
        collect(n.start,n.end>n.start?NotifyPhase::Begin:NotifyPhase::Instant);
        if(n.end>n.start)collect(n.end,NotifyPhase::End);
    }
    std::stable_sort(events.begin(),events.end(),[&](const auto& a,const auto& b)
    {
        if(a.time!=b.time)return forward?a.time<b.time:a.time>b.time;
        const auto rank=[](NotifyPhase phase){return phase==NotifyPhase::End?0:phase==NotifyPhase::Instant?1:2;};
        return rank(a.phase)<rank(b.phase);
    });
}
void SkinVertices(std::span<const Vertex> source,const SkinningPalette& palette,std::vector<Vertex>& out)
{
    out.assign(source.begin(),source.end());
    for(auto& vertex:out)
    {
        Maths::Vector3 position{},normal{},tangent{},bitangent{};
        for(size_t k=0;k<4;++k)
        {
            const auto joint=vertex.influence.joints[k];const float weight=vertex.influence.weights[k]/65535.f;
            if(weight==0 || joint>=palette.position.size())continue;
            auto p=palette.position[joint]*Maths::Vector4(vertex.position[0],vertex.position[1],vertex.position[2],1);
            auto n=palette.normal[joint]*Maths::Vector4(vertex.normals[0],vertex.normals[1],vertex.normals[2],0);
            auto t=palette.position[joint]*Maths::Vector4(vertex.tangent[0],vertex.tangent[1],vertex.tangent[2],0);
            auto b=palette.position[joint]*Maths::Vector4(vertex.bitangent[0],vertex.bitangent[1],vertex.bitangent[2],0);
            position=position+Maths::Vector3{p.x,p.y,p.z}*weight;normal=normal+Maths::Vector3{n.x,n.y,n.z}*weight;
            tangent=tangent+Maths::Vector3{t.x,t.y,t.z}*weight;bitangent=bitangent+Maths::Vector3{b.x,b.y,b.z}*weight;
        }
        normal=Unit(normal);tangent=Unit(tangent-normal*(normal.x*tangent.x+normal.y*tangent.y+normal.z*tangent.z));bitangent=Unit(bitangent);
        std::copy_n(&position.x,3,vertex.position);std::copy_n(&normal.x,3,vertex.normals);
        std::copy_n(&tangent.x,3,vertex.tangent);std::copy_n(&bitangent.x,3,vertex.bitangent);
    }
}
bool Model::Validate(std::string& error) const
{
    if(!skeleton.Validate(error))return false;
    std::unordered_set<std::string> skinIds,clipIds;
    for(const auto& skin:skins)
    {
        if(skin.id.empty() || !skinIds.insert(skin.id).second || skin.skeleton!=skeleton.id || skin.joints.empty() || skin.joints.size()>65535 || skin.joints.size()!=skin.inverseBind.size())
        {error="Invalid skin identity, skeleton or binding count";return false;}
        for(auto joint:skin.joints)if(joint>=skeleton.bones.size()) {error="Skin joint is out of range";return false;}
        for(const auto& m:skin.inverseBind)for(float f:m.data)if(!std::isfinite(f)){error="Inverse bind matrix is not finite";return false;}
    }
    for(const auto& c:clips)if(!clipIds.insert(c.id).second || !c.Validate(skeleton,error))return false;
    for(const auto& mesh:meshes)
    {
        const auto skin=std::find_if(skins.begin(),skins.end(),[&](const auto& s){return s.id==mesh.skin;});
        if(skin==skins.end()) {error="Skeletal mesh has no valid skin binding";return false;}
        for(auto index:mesh.indices)if(index>=mesh.vertices.size()) {error="Skeletal mesh index is out of range";return false;}
        for(const auto& v:mesh.vertices)
        {
            uint32_t sum=0;for(size_t k=0;k<4;++k){if(v.influence.weights[k] && v.influence.joints[k]>=skin->joints.size()){error="Skeletal vertex joint is out of range";return false;}sum+=v.influence.weights[k];}
            if(sum!=65535){error="Skeletal vertex weights must sum to 65535";return false;}
        }
    }
    return true;
}
std::vector<uint8_t> SerializeModel(const Model& model)
{
    Json j{{"version",AnimationFormatVersion},{"id",model.skeleton.id},{"reader",model.reader}};
    j["bones"]=Json::array();for(const auto& b:model.skeleton.bones)j["bones"].push_back({{"id",b.id},{"name",b.name},{"parent",b.parent},{"reference",WriteTransform(b.reference)}});
    j["skins"]=Json::array();for(const auto& s:model.skins)
    {Json m=Json::array();for(const auto& a:s.inverseBind)m.push_back(Matrix(a));j["skins"].push_back({{"id",s.id},{"skeleton",s.skeleton},{"node",s.meshNode},{"joints",s.joints},{"inverseBind",m},{"meshFromSkeleton",Matrix(s.meshFromSkeleton)}});}
    j["clips"]=Json::array();for(const auto& c:model.clips)
    {
        Json a{{"id",c.id},{"name",c.name},{"skeleton",c.skeleton},{"duration",c.duration}};a["tracks"]=Json::array();a["curves"]=Json::array();a["notifies"]=Json::array();
        for(const auto& t:c.tracks)a["tracks"].push_back({{"bone",t.bone},{"t",WriteChannel(t.translation)},{"r",WriteChannel(t.rotation)},{"s",WriteChannel(t.scale)}});
        for(const auto& v:c.curves)a["curves"].push_back({{"name",v.name},{"channel",WriteChannel(v.channel)}});
        for(const auto& n:c.notifies)a["notifies"].push_back({{"id",n.id},{"name",n.name},{"payload",n.payload},{"start",n.start},{"end",n.end},{"threshold",n.threshold}});
        j["clips"].push_back(std::move(a));
    }
    j["meshes"]=Json::array();for(const auto& m:model.meshes)
    {
        Json vertices=Json::array();for(const auto& v:m.vertices)
        {std::array<float,14> attributes{};std::copy_n(v.position,3,attributes.begin());std::copy_n(v.texCoords,2,attributes.begin()+3);std::copy_n(v.normals,3,attributes.begin()+5);std::copy_n(v.tangent,3,attributes.begin()+8);std::copy_n(v.bitangent,3,attributes.begin()+11);vertices.push_back({{"attributes",attributes},{"joints",v.influence.joints},{"weights",v.influence.weights}});}
        j["meshes"].push_back({{"id",m.id},{"name",m.name},{"skin",m.skin},{"material",m.material},{"vertices",vertices},{"indices",m.indices}});
    }
    return Json::to_cbor(j);
}
std::shared_ptr<Model> DeserializeModel(std::span<const uint8_t> bytes,std::string& error)
{
    try
    {
        const auto j=Json::from_cbor(bytes.begin(),bytes.end());
        if(j.at("version")!=AnimationFormatVersion)throw std::runtime_error("Unsupported animation artifact version");
        auto m=std::make_shared<Model>();m->skeleton.id=j.at("id").get<std::string>();m->reader=j.value("reader",std::string{});
        for(const auto& b:j.at("bones"))m->skeleton.bones.push_back({b.at("id").get<std::string>(),b.at("name").get<std::string>(),b.at("parent").get<BoneIndex>(),ReadTransform(b.at("reference"))});
        for(const auto& s:j.at("skins"))
        {SkinBinding b;b.id=s.at("id").get<std::string>();b.skeleton=s.at("skeleton").get<std::string>();b.meshNode=s.value("node",std::string{});b.joints=s.at("joints").get<std::vector<BoneIndex>>();b.meshFromSkeleton=ReadMatrix(s.at("meshFromSkeleton"));for(const auto& a:s.at("inverseBind"))b.inverseBind.push_back(ReadMatrix(a));m->skins.push_back(std::move(b));}
        for(const auto& a:j.at("clips"))
        {
            Clip c;c.id=a.at("id").get<std::string>();c.name=a.at("name").get<std::string>();c.skeleton=a.at("skeleton").get<std::string>();c.duration=a.at("duration").get<double>();
            for(const auto& t:a.at("tracks"))c.tracks.push_back({t.at("bone").get<std::string>(),ReadChannel(t.at("t")),ReadChannel(t.at("r")),ReadChannel(t.at("s"))});
            for(const auto& v:a.at("curves"))c.curves.push_back({v.at("name").get<std::string>(),ReadChannel(v.at("channel"))});
            for(const auto& n:a.at("notifies"))c.notifies.push_back({n.at("id").get<uint64_t>(),n.at("name").get<std::string>(),n.at("payload").get<std::string>(),n.at("start").get<double>(),n.at("end").get<double>(),n.at("threshold").get<float>()});
            m->clips.push_back(std::move(c));
        }
        for(const auto& a:j.at("meshes"))
        {
            SkeletalMesh s;s.id=a.at("id").get<std::string>();s.name=a.at("name").get<std::string>();s.skin=a.at("skin").get<std::string>();s.material=a.at("material").get<uint32_t>();s.indices=a.at("indices").get<std::vector<uint32_t>>();
            for(const auto& b:a.at("vertices"))
            {Vertex v;const auto data=b.at("attributes").get<std::array<float,14>>();std::copy_n(data.begin(),3,v.position);std::copy_n(data.begin()+3,2,v.texCoords);std::copy_n(data.begin()+5,3,v.normals);std::copy_n(data.begin()+8,3,v.tangent);std::copy_n(data.begin()+11,3,v.bitangent);v.influence.joints=b.at("joints").get<std::array<uint16_t,4>>();v.influence.weights=b.at("weights").get<std::array<uint16_t,4>>();s.vertices.push_back(v);}
            m->meshes.push_back(std::move(s));
        }
        if(!m->Validate(error))return {};return m;
    }
    catch(const std::exception& exception) {error=std::string("Invalid animation artifact: ")+exception.what();return {};}
}
}
