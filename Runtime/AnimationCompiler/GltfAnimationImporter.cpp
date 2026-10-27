#include "GltfAnimationImporter.h"
#include <Json/json.hpp>
#include <Math/Vector4.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>

namespace NLS::AnimationCompiler
{
using namespace Animation;
namespace
{
using Json=nlohmann::json;
std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
{
    std::ifstream stream(path,std::ios::binary|std::ios::ate);if(!stream)throw std::runtime_error("Cannot open glTF buffer: "+path.string());
    const auto size=stream.tellg();if(size<0||uint64_t(size)>uint64_t(2)*1024*1024*1024)throw std::runtime_error("glTF buffer is too large");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));stream.seekg(0);if(!bytes.empty()&&!stream.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size())))throw std::runtime_error("Cannot read glTF buffer");return bytes;
}
uint32_t U32(std::span<const uint8_t> bytes,size_t offset)
{if(offset>bytes.size()||bytes.size()-offset<4)throw std::runtime_error("Truncated GLB");return uint32_t(bytes[offset])|(uint32_t(bytes[offset+1])<<8)|(uint32_t(bytes[offset+2])<<16)|(uint32_t(bytes[offset+3])<<24);}
std::vector<uint8_t> Base64(std::string_view input)
{
    std::vector<uint8_t> output;uint32_t accumulator=0;int bits=0;
    for(char c:input)
    {
        if(c=='=')break;if(c=='\r'||c=='\n'||c==' '||c=='\t')continue;
        const auto p=std::string_view("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/").find(c);
        if(p==std::string_view::npos)throw std::runtime_error("Invalid base64 buffer URI");
        accumulator=(accumulator<<6)|uint32_t(p);bits+=6;if(bits>=8){bits-=8;output.push_back(uint8_t(accumulator>>bits));}
    }
    return output;
}
std::string DecodeUri(std::string_view value)
{
    std::string result;for(size_t i=0;i<value.size();++i)
    {if(value[i]=='%'&&i+2<value.size()){auto digit=[](char c)->int{if(c>='0'&&c<='9')return c-'0';if(c>='A'&&c<='F')return c-'A'+10;if(c>='a'&&c<='f')return c-'a'+10;return -1;};const int a=digit(value[i+1]),b=digit(value[i+2]);if(a<0||b<0)throw std::runtime_error("Invalid escaped buffer URI");result+=char(a*16+b);i+=2;}else result+=value[i];}
    return result;
}
size_t Components(std::string_view type)
{if(type=="SCALAR")return 1;if(type=="VEC2")return 2;if(type=="VEC3")return 3;if(type=="VEC4")return 4;if(type=="MAT4")return 16;throw std::runtime_error("Unsupported glTF accessor shape");}
size_t ComponentBytes(uint32_t type)
{switch(type){case 5120:case 5121:return 1;case 5122:case 5123:return 2;case 5125:case 5126:return 4;default:throw std::runtime_error("Unsupported glTF component type");}}
double ReadComponent(const uint8_t* p,uint32_t type,bool normalized)
{
    switch(type)
    {
    case 5120:{int8_t v;std::memcpy(&v,p,1);return normalized?std::max(-1.,v/127.):double(v);}
    case 5121:{uint8_t v=*p;return normalized?v/255.:double(v);}
    case 5122:{int16_t v;std::memcpy(&v,p,2);return normalized?std::max(-1.,v/32767.):double(v);}
    case 5123:{uint16_t v;std::memcpy(&v,p,2);return normalized?v/65535.:double(v);}
    case 5125:{uint32_t v;std::memcpy(&v,p,4);return double(v);}
    case 5126:{float v;std::memcpy(&v,p,4);if(!std::isfinite(v))throw std::runtime_error("Non-finite glTF accessor");return v;}
    default:throw std::runtime_error("Unsupported glTF component type");
    }
}
struct Document
{
    Json root;
    std::vector<std::vector<uint8_t>> buffers;
    std::vector<int32_t> parents;
    std::vector<Transform> local;
    std::vector<Maths::Matrix4> global;
    std::vector<std::string> paths;
    std::map<size_t,std::vector<double>> decoded;
    const uint8_t* View(size_t index,size_t offset,size_t length)const
    {
        const auto& view=root.at("bufferViews").at(index);const size_t buffer=view.at("buffer").get<size_t>();
        const size_t base=view.value("byteOffset",size_t(0)),count=view.at("byteLength").get<size_t>();
        if(buffer>=buffers.size()||offset>count||length>count-offset||base>buffers[buffer].size()||count>buffers[buffer].size()-base)throw std::runtime_error("glTF bufferView is out of range");
        return buffers[buffer].data()+base+offset;
    }
    const std::vector<double>& Accessor(size_t index)
    {
        if(auto found=decoded.find(index);found!=decoded.end())return found->second;
        const auto& a=root.at("accessors").at(index);const size_t count=a.at("count").get<size_t>(),components=Components(a.at("type").get<std::string>());
        const auto component=a.at("componentType").get<uint32_t>();const size_t width=ComponentBytes(component),element=width*components;
        if(count>64000000/components)throw std::runtime_error("glTF accessor exceeds import limits");
        std::vector<double> output(count*components,0);const bool normalized=a.value("normalized",false);
        if(a.contains("bufferView")&&count)
        {
            const size_t view=a.at("bufferView").get<size_t>();const auto& v=root.at("bufferViews").at(view);const size_t stride=v.value("byteStride",element);
            if(stride<element||stride>252||stride%width)throw std::runtime_error("Invalid glTF accessor stride");
            if(count-1>(SIZE_MAX-element)/stride)throw std::runtime_error("glTF accessor length overflow");
            const auto* bytes=View(view,a.value("byteOffset",size_t(0)),(count-1)*stride+element);
            for(size_t i=0;i<count;++i)for(size_t k=0;k<components;++k)output[i*components+k]=ReadComponent(bytes+i*stride+k*width,component,normalized);
        }
        if(a.contains("sparse"))
        {
            const auto& s=a.at("sparse");const size_t n=s.at("count").get<size_t>();if(n>count)throw std::runtime_error("Sparse accessor exceeds base accessor");
            const auto& indices=s.at("indices");const uint32_t indexType=indices.at("componentType").get<uint32_t>();if(indexType!=5121&&indexType!=5123&&indexType!=5125)throw std::runtime_error("Invalid sparse accessor index type");
            const size_t indexBytes=ComponentBytes(indexType);const auto* ib=View(indices.at("bufferView").get<size_t>(),indices.value("byteOffset",size_t(0)),n*indexBytes);
            const auto& values=s.at("values");const auto* vb=View(values.at("bufferView").get<size_t>(),values.value("byteOffset",size_t(0)),n*element);size_t previous=0;
            for(size_t i=0;i<n;++i)
            {const size_t at=size_t(ReadComponent(ib+i*indexBytes,indexType,false));if(at>=count||(i&&at<=previous))throw std::runtime_error("Sparse accessor indices are invalid");previous=at;for(size_t k=0;k<components;++k)output[at*components+k]=ReadComponent(vb+i*element+k*width,component,normalized);}
        }
        return decoded.emplace(index,std::move(output)).first->second;
    }
};
void ReadDocument(const std::filesystem::path& path,Document& doc)
{
    auto source=ReadFile(path);std::vector<uint8_t> binary;
    if(source.size()>=12&&U32(source,0)==0x46546c67)
    {
        if(U32(source,4)!=2||U32(source,8)!=source.size())throw std::runtime_error("Invalid GLB 2 header");
        size_t offset=12;bool hasJson=false;
        while(offset<source.size())
        {const size_t length=U32(source,offset);const auto type=U32(source,offset+4);offset+=8;if(length>source.size()-offset)throw std::runtime_error("GLB chunk is out of range");if(type==0x4e4f534a){doc.root=Json::parse(source.begin()+offset,source.begin()+offset+length);hasJson=true;}else if(type==0x004e4942)binary.assign(source.begin()+offset,source.begin()+offset+length);offset+=length;}
        if(!hasJson)throw std::runtime_error("GLB has no JSON chunk");
    }
    else doc.root=Json::parse(source.begin(),source.end());
    if(!doc.root.is_object()||!doc.root.contains("asset")||doc.root.at("asset").value("version",std::string{})!="2.0")throw std::runtime_error("Expected glTF 2.0");
    if(doc.root.contains("extensionsRequired"))for(const auto& e:doc.root.at("extensionsRequired"))
        if(e!="KHR_materials_unlit"&&e!="KHR_texture_transform"&&e!="KHR_materials_pbrSpecularGlossiness"&&e!="KHR_materials_emissive_strength")throw std::runtime_error("Required glTF extension is unsupported by the animation importer: "+e.get<std::string>());
    for(const auto& buffer:doc.root.value("buffers",Json::array()))
    {
        std::vector<uint8_t> bytes;const auto uri=buffer.value("uri",std::string{});
        if(uri.empty())bytes=binary;
        else if(uri.starts_with("data:")){const auto comma=uri.find(',');if(comma==std::string::npos||uri.substr(0,comma).find(";base64")==std::string::npos)throw std::runtime_error("Expected base64 glTF data URI");bytes=Base64(std::string_view(uri).substr(comma+1));}
        else {if(uri.find("://")!=std::string::npos)throw std::runtime_error("Remote glTF buffer URIs are unsupported");bytes=ReadFile(path.parent_path()/std::filesystem::u8path(DecodeUri(uri)));}
        if(buffer.at("byteLength").get<uint64_t>()>bytes.size())throw std::runtime_error("glTF buffer is shorter than declared");doc.buffers.push_back(std::move(bytes));
    }
}
}
bool ReadGltfSourcePayload(const std::filesystem::path& path,const ImportSettings& settings,GltfSourcePayload& payload,std::string& error)
{
    try
    {
        Document d;ReadDocument(path,d);payload={};payload.json=d.root.dump();
        const auto meshes=d.root.value("meshes",Json::array());
        for(size_t meshIndex=0;meshIndex<meshes.size();++meshIndex)
        {
            const auto& primitives=meshes[meshIndex].at("primitives");
            for(size_t p=0;p<primitives.size();++p)
            {
                const auto& primitive=primitives[p];if(primitive.value("mode",4)!=4)throw std::runtime_error("Animation model geometry requires triangle primitives");
                const auto& attributes=primitive.at("attributes");const auto& positions=d.Accessor(attributes.at("POSITION").get<size_t>());
                if(positions.size()%3)throw std::runtime_error("Invalid glTF position stream");
                SkeletalMesh mesh;mesh.id="mesh/"+std::to_string(meshIndex)+(primitives.size()>1?"/primitive/"+std::to_string(p):std::string{});
                mesh.name=meshes[meshIndex].value("name",mesh.id);mesh.material=primitive.value("material",0u);mesh.vertices.resize(positions.size()/3);
                for(size_t i=0;i<mesh.vertices.size();++i)for(size_t k=0;k<3;++k)mesh.vertices[i].position[k]=float(positions[i*3+k]*settings.globalScale);
                auto copy=[&](const char* name,size_t width,auto write)
                {if(!attributes.contains(name))return;const auto& data=d.Accessor(attributes.at(name).get<size_t>());if(data.size()!=mesh.vertices.size()*width)throw std::runtime_error("glTF geometry attribute length mismatch");for(size_t i=0;i<mesh.vertices.size();++i)write(mesh.vertices[i],data.data()+i*width);};
                copy("NORMAL",3,[](Vertex& v,const double* values){for(size_t k=0;k<3;++k)v.normals[k]=float(values[k]);});
                copy("TEXCOORD_0",2,[](Vertex& v,const double* values){for(size_t k=0;k<2;++k)v.texCoords[k]=float(values[k]);});
                copy("TANGENT",4,[](Vertex& v,const double* values)
                {for(size_t k=0;k<3;++k)v.tangent[k]=float(values[k]);v.bitangent[0]=(v.normals[1]*v.tangent[2]-v.normals[2]*v.tangent[1])*float(values[3]);v.bitangent[1]=(v.normals[2]*v.tangent[0]-v.normals[0]*v.tangent[2])*float(values[3]);v.bitangent[2]=(v.normals[0]*v.tangent[1]-v.normals[1]*v.tangent[0])*float(values[3]);});
                if(primitive.contains("indices"))for(auto index:d.Accessor(primitive.at("indices").get<size_t>())){if(index<0||index>=mesh.vertices.size()||std::floor(index)!=index)throw std::runtime_error("Invalid glTF triangle index");mesh.indices.push_back(uint32_t(index));}
                else for(uint32_t i=0;i<mesh.vertices.size();++i)mesh.indices.push_back(i);
                if(mesh.indices.size()%3)throw std::runtime_error("Invalid glTF triangle index count");payload.primitives.push_back(std::move(mesh));
            }
        }
        const auto images=d.root.value("images",Json::array());
        for(size_t i=0;i<images.size();++i)
        {
            const auto& image=images[i];const auto key="image/"+std::to_string(i),uri=image.value("uri",std::string{});
            if(image.contains("bufferView"))
            {const auto index=image.at("bufferView").get<size_t>();const auto size=d.root.at("bufferViews").at(index).at("byteLength").get<size_t>();const auto* data=d.View(index,0,size);payload.embeddedImages[key]={data,data+size};}
            else if(uri.starts_with("data:")){const auto comma=uri.find(',');if(comma==std::string::npos)throw std::runtime_error("Invalid embedded glTF image");payload.embeddedImages[key]=Base64(std::string_view(uri).substr(comma+1));}
            else if(!uri.empty())payload.externalFiles.push_back((path.parent_path()/std::filesystem::u8path(DecodeUri(uri))).string());
        }
        for(const auto& buffer:d.root.value("buffers",Json::array()))
        {const auto uri=buffer.value("uri",std::string{});if(!uri.empty()&&!uri.starts_with("data:"))payload.externalFiles.push_back((path.parent_path()/std::filesystem::u8path(DecodeUri(uri))).string());}
        return true;
    }
    catch(const std::exception& exception){error=std::string("glTF source data: ")+exception.what();return false;}
}
bool BindAnimationSkeleton(Model& model,const Skeleton& target,std::string& error)
{
    if(!target.Validate(error))return false;
    std::vector<BoneIndex> map(model.skeleton.bones.size(),NoBone);
    for(size_t i=0;i<map.size();++i)
    {
        const auto& bone=model.skeleton.bones[i];map[i]=target.Find(bone.id);if(map[i]==NoBone)map[i]=target.Find(bone.name);
        if(map[i]==NoBone){error="Target skeleton has no unique match for bone: "+bone.id;return false;}
        if(bone.parent!=NoBone&&target.bones[map[i]].parent!=map[bone.parent]){error="Animation hierarchy differs from target skeleton at: "+bone.id;return false;}
        const auto sourceReference=ToMatrix(bone.reference),targetReference=ToMatrix(target.bones[map[i]].reference);
        for(size_t k=0;k<16;++k)if(std::abs(sourceReference.data[k]-targetReference.data[k])>1e-3f*(1+std::abs(targetReference.data[k])))
        {error="Animation reference pose or bone proportions differ from the target skeleton at: "+bone.id;return false;}
    }
    for(auto& clip:model.clips){for(auto& track:clip.tracks){auto i=model.skeleton.Find(track.bone);if(i==NoBone){error="Animation track has no source bone";return false;}track.bone=target.bones[map[i]].id;}clip.skeleton=target.id;}
    for(auto& skin:model.skins){for(auto& joint:skin.joints)joint=map.at(joint);skin.skeleton=target.id;}
    model.skeleton=target;return model.Validate(error);
}
std::shared_ptr<Model> ImportGltfAnimation(const std::filesystem::path& path,const ImportSettings& settings,std::string& error)
{
    try
    {
        Document d;ReadDocument(path,d);const auto& root=d.root;
        if((!root.contains("skins")||root.at("skins").empty())&&(!root.contains("animations")||root.at("animations").empty()))return {};
        if(!settings.importSkeleton&&!settings.importAnimations)return {};
        if(!std::isfinite(settings.globalScale)||settings.globalScale<=0)throw std::runtime_error("Animation import scale must be positive and finite");
        auto model=std::make_shared<Model>();model->reader="Nullus.glTF.2.Animation.v1";model->skeleton.id=settings.sourceId.empty()?path.stem().string():settings.sourceId;
        const auto& nodes=root.at("nodes");const size_t count=nodes.size();d.parents.assign(count,-1);d.local.resize(count);d.global.resize(count);d.paths.resize(count);
        for(size_t i=0;i<count;++i)for(const auto& child:nodes[i].value("children",Json::array()))
        {const auto index=child.get<size_t>();if(index>=count||d.parents[index]!=-1)throw std::runtime_error("glTF node has multiple parents or an invalid child");d.parents[index]=int32_t(i);}
        std::vector<uint8_t> visited(count);
        std::function<void(size_t)> transform=[&](size_t i)
        {
            if(visited[i]==1)throw std::runtime_error("glTF node hierarchy contains a cycle");if(visited[i]==2)return;visited[i]=1;
            const auto& node=nodes[i];auto& local=d.local[i];
            if(node.contains("matrix")){const auto values=node.at("matrix").get<std::array<float,16>>();Maths::Matrix4 matrix;for(size_t r=0;r<4;++r)for(size_t c=0;c<4;++c)matrix.data[r*4+c]=values[c*4+r];if(!Decompose(matrix,local,error))throw std::runtime_error(error);}
            else
            {
                if(node.contains("translation")){const auto t=node.at("translation").get<std::array<float,3>>();local.translation={t[0],t[1],t[2]};}
                if(node.contains("rotation")){const auto r=node.at("rotation").get<Value>();local.rotation=Maths::Quaternion::Normalize({r[0],r[1],r[2],r[3]});}
                if(node.contains("scale")){const auto s=node.at("scale").get<std::array<float,3>>();local.scale={s[0],s[1],s[2]};}
            }
            local.translation=local.translation*float(settings.globalScale);
            if(d.parents[i]>=0)transform(size_t(d.parents[i]));
            d.global[i]=d.parents[i]<0?ToMatrix(local):d.global[size_t(d.parents[i])]*ToMatrix(local);
            std::string name=node.value("name",std::string("Node"));size_t duplicates=0,ordinal=0;
            for(size_t k=0;k<count;++k)if(d.parents[k]==d.parents[i]&&nodes[k].value("name",std::string("Node"))==name){++duplicates;if(k<i)++ordinal;}
            if(duplicates>1)name+="#"+std::to_string(ordinal);d.paths[i]=(d.parents[i]<0?std::string{}:d.paths[size_t(d.parents[i])]+"/")+name;visited[i]=2;
        };
        for(size_t i=0;i<count;++i)transform(i);
        std::set<size_t> required;
        auto require=[&](size_t i){if(i>=count)throw std::runtime_error("glTF animation references an invalid node");while(required.insert(i).second&&d.parents[i]>=0)i=size_t(d.parents[i]);};
        if(settings.importSkeleton)for(const auto& skin:root.value("skins",Json::array()))for(const auto& joint:skin.at("joints"))require(joint.get<size_t>());
        if(settings.importAnimations)for(const auto& clip:root.value("animations",Json::array()))for(const auto& channel:clip.at("channels"))if(channel.at("target").value("path",std::string{})!="weights")require(channel.at("target").at("node").get<size_t>());
        if(required.empty())return {};
        std::vector<BoneIndex> map(count,NoBone);
        std::function<void(size_t)> add=[&](size_t i)
        {
            if(map[i]!=NoBone)return;
            const auto name=nodes[i].value("name",std::string("Node"));
            for(size_t k=0;k<count;++k)if(k!=i&&d.parents[k]==d.parents[i]&&nodes[k].value("name",std::string("Node"))==name)
                throw std::runtime_error("Ambiguous glTF skeletal path: "+d.paths[i]);
            if(d.parents[i]>=0)add(size_t(d.parents[i]));map[i]=uint32_t(model->skeleton.bones.size());model->skeleton.bones.push_back({d.paths[i],nodes[i].value("name",d.paths[i]),d.parents[i]<0?NoBone:map[size_t(d.parents[i])],d.local[i]});
        };
        for(auto i:required)add(i);
        if(settings.importAnimations)
        {
            size_t clipIndex=0;for(const auto& source:root.value("animations",Json::array()))
            {
                Clip clip;clip.name=source.value("name",std::string("Animation ")+std::to_string(clipIndex));clip.id="animation/"+std::to_string(clipIndex++);clip.skeleton=model->skeleton.id;
                if(!settings.takes.empty()&&std::find(settings.takes.begin(),settings.takes.end(),clip.name)==settings.takes.end())continue;
                std::map<size_t,size_t> tracks;
                for(const auto& channel:source.at("channels"))
                {
                    const auto& target=channel.at("target");const auto type=target.at("path").get<std::string>();if(type=="weights"){model->warnings.push_back("Morph target animation is preserved by the model importer but is not evaluated in P0/P1");continue;}
                    const size_t node=target.at("node").get<size_t>();const auto& sampler=source.at("samplers").at(channel.at("sampler").get<size_t>());
                    const auto input=sampler.at("input").get<size_t>(),output=sampler.at("output").get<size_t>();
                    if(root.at("accessors").at(input).at("type")!="SCALAR"||root.at("accessors").at(input).at("componentType")!=5126)throw std::runtime_error("Animation input must be float seconds");
                    Channel c;c.times=d.Accessor(input);const auto interpolation=sampler.value("interpolation",std::string("LINEAR"));
                    c.interpolation=interpolation=="STEP"?Interpolation::Step:interpolation=="LINEAR"?Interpolation::Linear:interpolation=="CUBICSPLINE"?Interpolation::CubicSpline:throw std::runtime_error("Unknown glTF animation interpolation");
                    const size_t width=type=="rotation"?4:3;if(type!="translation"&&type!="rotation"&&type!="scale")throw std::runtime_error("Unsupported animation target");
                    const auto& data=d.Accessor(output);const size_t multiplicity=c.interpolation==Interpolation::CubicSpline?3:1;
                    if(Components(root.at("accessors").at(output).at("type").get<std::string>())!=width||data.size()!=c.times.size()*width*multiplicity)throw std::runtime_error("Animation output shape does not match input times");
                    for(size_t key=0;key<c.times.size();++key)
                    {
                        auto value=[&](size_t part){Value v{};for(size_t k=0;k<width;++k)v[k]=float(data[(key*multiplicity+part)*width+k])*(type=="translation"?float(settings.globalScale):1.f);return v;};
                        c.values.push_back(value(multiplicity==3?1:0));if(multiplicity==3){c.inTangents.push_back(value(0));c.outTangents.push_back(value(2));}
                    }
                    if(!c.Validate(error))throw std::runtime_error(error);if(!c.times.empty())clip.duration=std::max(clip.duration,c.times.back());
                    auto [it,inserted]=tracks.emplace(node,clip.tracks.size());if(inserted)clip.tracks.push_back({d.paths[node],{},{},{}});auto& track=clip.tracks[it->second];
                    auto& destination=type=="translation"?track.translation:type=="rotation"?track.rotation:track.scale;if(!destination.times.empty())throw std::runtime_error("Duplicate animation channel for a bone property");destination=std::move(c);
                }
                model->clips.push_back(std::move(clip));
            }
        }
        if(settings.importSkeleton)
        {
            const auto skins=root.value("skins",Json::array());
            for(size_t nodeIndex=0;nodeIndex<count;++nodeIndex)
            {
                const auto& node=nodes[nodeIndex];if(!node.contains("skin")||!node.contains("mesh"))continue;const size_t skinIndex=node.at("skin").get<size_t>(),meshIndex=node.at("mesh").get<size_t>();const auto& skin=skins.at(skinIndex);
                SkinBinding binding;binding.id="skin/"+std::to_string(skinIndex)+"/node/"+std::to_string(nodeIndex);binding.skeleton=model->skeleton.id;binding.meshNode="node/"+std::to_string(nodeIndex);binding.meshFromSkeleton=Maths::Matrix4::Inverse(d.global[nodeIndex]);
                for(const auto& joint:skin.at("joints"))binding.joints.push_back(map.at(joint.get<size_t>()));
                binding.inverseBind.assign(binding.joints.size(),Maths::Matrix4::Identity);
                if(skin.contains("inverseBindMatrices"))
                {const auto& matrices=d.Accessor(skin.at("inverseBindMatrices").get<size_t>());if(matrices.size()!=binding.joints.size()*16)throw std::runtime_error("Inverse bind matrix count differs from joints");for(size_t i=0;i<binding.joints.size();++i){auto& m=binding.inverseBind[i];for(size_t r=0;r<4;++r)for(size_t c=0;c<4;++c)m.data[r*4+c]=float(matrices[i*16+c*4+r]);m.data[3]*=float(settings.globalScale);m.data[7]*=float(settings.globalScale);m.data[11]*=float(settings.globalScale);}}
                const auto& primitives=root.at("meshes").at(meshIndex).at("primitives");
                for(size_t primitiveIndex=0;primitiveIndex<primitives.size();++primitiveIndex)
                {
                    const auto& primitive=primitives[primitiveIndex];if(primitive.value("mode",4)!=4)throw std::runtime_error("Skinned geometry requires triangle primitives");const auto& attributes=primitive.at("attributes");
                    if(!attributes.contains("JOINTS_0")||!attributes.contains("WEIGHTS_0"))throw std::runtime_error("Skinned primitive is missing joint or weight attributes");
                    SkeletalMesh mesh;mesh.id="mesh/"+std::to_string(meshIndex)+"/primitive/"+std::to_string(primitiveIndex)+"/node/"+std::to_string(nodeIndex);mesh.name=root.at("meshes").at(meshIndex).value("name",mesh.id);mesh.skin=binding.id;mesh.material=primitive.value("material",0u);
                    const size_t positionAccessor=attributes.at("POSITION").get<size_t>();const auto& positions=d.Accessor(positionAccessor);if(positions.size()%3)throw std::runtime_error("Invalid position stream");mesh.vertices.resize(positions.size()/3);
                    for(size_t i=0;i<mesh.vertices.size();++i)for(size_t k=0;k<3;++k)mesh.vertices[i].position[k]=float(positions[i*3+k]*settings.globalScale);
                    auto copy=[&](const char* semantic,size_t width,auto write)
                    {if(!attributes.contains(semantic))return;const auto& values=d.Accessor(attributes.at(semantic).get<size_t>());if(values.size()!=mesh.vertices.size()*width)throw std::runtime_error("Vertex attribute length mismatch");for(size_t i=0;i<mesh.vertices.size();++i)write(mesh.vertices[i],values.data()+i*width);};
                    copy("NORMAL",3,[](Vertex& v,const double* a){for(size_t k=0;k<3;++k)v.normals[k]=float(a[k]);});
                    copy("TEXCOORD_0",2,[](Vertex& v,const double* a){for(size_t k=0;k<2;++k)v.texCoords[k]=float(a[k]);});
                    copy("TANGENT",4,[](Vertex& v,const double* a){for(size_t k=0;k<3;++k)v.tangent[k]=float(a[k]);v.bitangent[0]=(v.normals[1]*v.tangent[2]-v.normals[2]*v.tangent[1])*float(a[3]);v.bitangent[1]=(v.normals[2]*v.tangent[0]-v.normals[0]*v.tangent[2])*float(a[3]);v.bitangent[2]=(v.normals[0]*v.tangent[1]-v.normals[1]*v.tangent[0])*float(a[3]);});
                    std::vector<std::vector<std::pair<uint32_t,float>>> influences(mesh.vertices.size());
                    for(uint32_t set=0;;++set)
                    {
                        const auto joints="JOINTS_"+std::to_string(set),weights="WEIGHTS_"+std::to_string(set);if(!attributes.contains(joints)&&!attributes.contains(weights))break;
                        if(!attributes.contains(joints)||!attributes.contains(weights))throw std::runtime_error("Joint and weight attribute sets do not match");
                        const auto ji=attributes.at(joints).get<size_t>(),wi=attributes.at(weights).get<size_t>();const auto& ja=root.at("accessors").at(ji);const auto& wa=root.at("accessors").at(wi);
                        if(ja.at("type")!="VEC4"||wa.at("type")!="VEC4"||(ja.at("componentType")!=5121&&ja.at("componentType")!=5123)||ja.value("normalized",false))throw std::runtime_error("Invalid joint attribute format");
                        const auto& j=d.Accessor(ji);const auto& w=d.Accessor(wi);if(j.size()!=mesh.vertices.size()*4||w.size()!=j.size())throw std::runtime_error("Influence stream length mismatch");
                        for(size_t i=0;i<mesh.vertices.size();++i)for(size_t k=0;k<4;++k)influences[i].emplace_back(uint32_t(j[i*4+k]),float(w[i*4+k]));
                    }
                    bool trimmed=false;for(size_t i=0;i<mesh.vertices.size();++i){bool t=false;if(!PackInfluences(influences[i],uint32_t(binding.joints.size()),mesh.vertices[i].influence,t,error))throw std::runtime_error(error);trimmed|=t;}
                    if(trimmed)model->warnings.push_back("More than four influences were trimmed and renormalized: "+mesh.name);
                    if(primitive.contains("indices")){const auto accessor=primitive.at("indices").get<size_t>();const auto& a=root.at("accessors").at(accessor);if(a.at("type")!="SCALAR"||a.at("componentType")==5126)throw std::runtime_error("Invalid triangle index format");for(auto value:d.Accessor(accessor))mesh.indices.push_back(uint32_t(value));}
                    else for(uint32_t i=0;i<mesh.vertices.size();++i)mesh.indices.push_back(i);
                    if(mesh.indices.size()%3)throw std::runtime_error("Triangle index count is not divisible by three");model->meshes.push_back(std::move(mesh));
                }
                model->skins.push_back(std::move(binding));
            }
        }
        if(settings.targetSkeleton&&!BindAnimationSkeleton(*model,*settings.targetSkeleton,error))return {};
        if(!model->Validate(error))return {};return model;
    }
    catch(const std::exception& e){error=std::string("glTF animation import: ")+e.what();return {};}
}
}
