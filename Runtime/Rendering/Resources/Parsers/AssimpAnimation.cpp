#include "AssimpAnimation.h"
#include <AnimationCompiler/GltfAnimationImporter.h>
#include <assimp/scene.h>
#include <assimp/metadata.h>
#include <assimp/version.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <unordered_map>

namespace NLS::Render::Resources::Parsers
{
using namespace Animation;
namespace
{
Maths::Matrix4 Matrix(const aiMatrix4x4& a,double scale)
{
    Maths::Matrix4 m;for(size_t r=0;r<4;++r)for(size_t c=0;c<4;++c)m.data[r*4+c]=a[unsigned(r)][unsigned(c)];
    for(size_t i=3;i<12;i+=4)m.data[i]*=float(scale);return m;
}
void SortKeys(Channel& c)
{
    std::map<double,Value> keys;for(size_t i=0;i<c.times.size();++i)keys[c.times[i]]=c.values[i];
    c.times.clear();c.values.clear();for(auto [time,value]:keys){c.times.push_back(time);c.values.push_back(value);}
}
}
std::shared_ptr<Model> BuildAssimpAnimation(const aiScene& scene,const std::vector<ParsedMeshData>& parsed,
    const ImportSettings& settings,std::string& error)
{
    bool skinned=false;for(unsigned i=0;i<scene.mNumMeshes;++i)skinned|=scene.mMeshes[i]->HasBones();
    if((!skinned||!settings.importSkeleton)&&(!scene.HasAnimations()||!settings.importAnimations))return {};
    try
    {
        auto model=std::make_shared<Model>();model->reader="Assimp."+std::to_string(aiGetVersionMajor())+"."+std::to_string(aiGetVersionMinor())+".Animation.v2";
        double fbxUnit=1;bool hasFbxUnits=scene.mMetaData&&scene.mMetaData->Get("UnitScaleFactor",fbxUnit);
        if(scene.mMetaData&&!hasFbxUnits){float unit=1;if(scene.mMetaData->Get("UnitScaleFactor",unit)){fbxUnit=unit;hasFbxUnits=true;}}
        if(hasFbxUnits&&(!std::isfinite(fbxUnit)||fbxUnit<=0))throw std::runtime_error("FBX unit scale is invalid");
        const double coordinateScale=settings.globalScale*(hasFbxUnits?fbxUnit*.01:1.);
        model->skeleton.id=settings.sourceId.empty()?"imported-skeleton":settings.sourceId;
        std::unordered_map<const aiNode*,BoneIndex> indices;std::unordered_map<std::string,std::vector<BoneIndex>> names;
        std::vector<const aiNode*> nodes;std::vector<Maths::Matrix4> global;
        std::function<void(const aiNode*,BoneIndex,const std::string&)> add=[&](const aiNode* node,BoneIndex parent,const std::string& prefix)
        {
            if(!node)throw std::runtime_error("FBX contains a null node");std::string name=node->mName.C_Str(),path=prefix+name;
            if(node->mParent){size_t duplicates=0;for(unsigned i=0;i<node->mParent->mNumChildren;++i)if(node->mParent->mChildren[i]->mName==node->mName)++duplicates;if(duplicates>1)throw std::runtime_error("Ambiguous FBX skeletal path: "+path);}
            Transform reference;if(!Decompose(Matrix(node->mTransformation,coordinateScale),reference,error))throw std::runtime_error(path+": "+error);
            // Assimp's FBX converter puts centimeters-per-source-unit on the
            // synthetic root. Vertices, bind translations and tracks are now
            // meters; retain its axis rotation without applying units twice.
            if(!node->mParent&&hasFbxUnits&&std::abs(std::abs(reference.scale.x)-fbxUnit)<1e-4&&
               std::abs(std::abs(reference.scale.y)-fbxUnit)<1e-4&&std::abs(std::abs(reference.scale.z)-fbxUnit)<1e-4)
                reference.scale=reference.scale/float(fbxUnit);
            const auto index=uint32_t(nodes.size());indices[node]=index;names[name].push_back(index);nodes.push_back(node);
            model->skeleton.bones.push_back({path,name,parent,reference});global.push_back(parent==NoBone?ToMatrix(reference):global[parent]*ToMatrix(reference));
            for(unsigned i=0;i<node->mNumChildren;++i)add(node->mChildren[i],index,path+"/");
        };
        add(scene.mRootNode,NoBone,{});
        auto findBone=[&](const aiBone& bone)->BoneIndex
        {
#ifndef ASSIMP_BUILD_NO_ARMATUREPOPULATE_PROCESS
            if(bone.mNode){auto found=indices.find(bone.mNode);if(found!=indices.end())return found->second;}
#endif
            auto found=names.find(bone.mName.C_Str());
            if(found==names.end()||found->second.size()!=1)throw std::runtime_error("FBX skin bone is missing or ambiguous: "+std::string(bone.mName.C_Str()));
            return found->second.front();
        };
        if(settings.importSkeleton)for(size_t nodeIndex=0;nodeIndex<nodes.size();++nodeIndex)
        {
            const auto* node=nodes[nodeIndex];for(unsigned slot=0;slot<node->mNumMeshes;++slot)
            {
                const unsigned meshIndex=node->mMeshes[slot];const auto* mesh=scene.mMeshes[meshIndex];if(!mesh->HasBones())continue;
                const auto source=std::find_if(parsed.begin(),parsed.end(),[&](const auto& m){return m.sourceMeshIndex==meshIndex;});
                if(source==parsed.end()||source->vertices.size()!=mesh->mNumVertices)throw std::runtime_error("FBX weights do not match final mesh vertices");
                SkinBinding binding;binding.id="parser/skin/mesh/"+std::to_string(meshIndex)+"/node/"+std::to_string(nodeIndex);binding.skeleton=model->skeleton.id;
                binding.meshNode="parser/node/"+std::to_string(nodeIndex);binding.meshFromSkeleton=Maths::Matrix4::Inverse(global[nodeIndex]);
                std::vector<std::vector<std::pair<uint32_t,float>>> weights(mesh->mNumVertices);
                for(unsigned i=0;i<mesh->mNumBones;++i)
                {
                    const auto* bone=mesh->mBones[i];binding.joints.push_back(findBone(*bone));binding.inverseBind.push_back(Matrix(bone->mOffsetMatrix,coordinateScale));
                    for(unsigned k=0;k<bone->mNumWeights;++k){const auto& w=bone->mWeights[k];if(w.mVertexId>=weights.size())throw std::runtime_error("FBX weight vertex is out of range");weights[w.mVertexId].emplace_back(i,w.mWeight);}
                }
                SkeletalMesh geometry;geometry.id="parser/mesh/"+std::to_string(meshIndex)+"/node/"+std::to_string(nodeIndex);geometry.name=mesh->mName.C_Str();geometry.skin=binding.id;geometry.material=mesh->mMaterialIndex;geometry.indices=source->indices;
                bool truncated=false;for(size_t i=0;i<source->vertices.size();++i)
                {
                    const auto& input=source->vertices[i];Vertex output;
                    for(size_t k=0;k<3;++k){output.position[k]=input.position[k]*float(coordinateScale);output.normals[k]=input.normals[k];output.tangent[k]=input.tangent[k];output.bitangent[k]=input.bitangent[k];}
                    std::copy_n(input.texCoords,2,output.texCoords);bool clipped=false;
                    if(!PackInfluences(weights[i],mesh->mNumBones,output.influence,clipped,error))throw std::runtime_error(error);
                    truncated|=clipped;geometry.vertices.push_back(output);
                }
                if(truncated)model->warnings.push_back("More than four FBX influences were trimmed: "+geometry.name);
                model->skins.push_back(std::move(binding));model->meshes.push_back(std::move(geometry));
            }
        }
        if(settings.importAnimations)
        {
            std::map<std::string,uint32_t> takeNames;
            for(unsigned i=0;i<scene.mNumAnimations;++i)
            {
                const auto& source=*scene.mAnimations[i];Clip clip;
                clip.name=source.mName.length?source.mName.C_Str():"Take "+std::to_string(i);const auto ordinal=takeNames[clip.name]++;
                clip.id="parser/animation/"+clip.name+(ordinal?"#"+std::to_string(ordinal):std::string{});clip.skeleton=model->skeleton.id;
                if(!settings.takes.empty()&&std::find(settings.takes.begin(),settings.takes.end(),clip.name)==settings.takes.end())continue;
                double ticks=source.mTicksPerSecond;
                if(!(ticks>0)||!std::isfinite(ticks)){ticks=30;model->warnings.push_back("FBX take has no frame rate; using 30 FPS: "+clip.name);}
                double rate=settings.sampleRate>0?settings.sampleRate:ticks;
                if(!std::isfinite(rate)||rate<1||rate>1000)throw std::runtime_error("FBX sample rate must be 1..1000 FPS");
                double begin=std::numeric_limits<double>::max(),end=source.mDuration/ticks;
                for(unsigned k=0;k<source.mNumChannels;++k)
                {
                    const auto& c=*source.mChannels[k];
                    if(c.mNumPositionKeys){begin=std::min(begin,c.mPositionKeys[0].mTime/ticks);end=std::max(end,c.mPositionKeys[c.mNumPositionKeys-1].mTime/ticks);}
                    if(c.mNumRotationKeys){begin=std::min(begin,c.mRotationKeys[0].mTime/ticks);end=std::max(end,c.mRotationKeys[c.mNumRotationKeys-1].mTime/ticks);}
                    if(c.mNumScalingKeys){begin=std::min(begin,c.mScalingKeys[0].mTime/ticks);end=std::max(end,c.mScalingKeys[c.mNumScalingKeys-1].mTime/ticks);}
                }
                if(begin==std::numeric_limits<double>::max())begin=0;clip.duration=std::max(0.,end-begin);
                if(clip.duration*rate>10000000)throw std::runtime_error("FBX take exceeds sample limits");
                for(unsigned k=0;k<source.mNumChannels;++k)
                {
                    const auto& channel=*source.mChannels[k];const auto match=names.find(channel.mNodeName.C_Str());
                    if(match==names.end()||match->second.size()!=1)throw std::runtime_error("FBX animated node is missing or ambiguous: "+std::string(channel.mNodeName.C_Str()));
                    const auto bone=match->second.front();BoneTrack track;track.bone=model->skeleton.bones[bone].id;Channel translation,rotation,scale;
                    for(unsigned n=0;n<channel.mNumPositionKeys;++n)
                    {const auto& key=channel.mPositionKeys[n];translation.times.push_back(std::max(0.,key.mTime/ticks-begin));translation.values.push_back({key.mValue.x*float(coordinateScale),key.mValue.y*float(coordinateScale),key.mValue.z*float(coordinateScale),0});}
                    for(unsigned n=0;n<channel.mNumRotationKeys;++n)
                    {const auto& key=channel.mRotationKeys[n];rotation.times.push_back(std::max(0.,key.mTime/ticks-begin));rotation.values.push_back({key.mValue.x,key.mValue.y,key.mValue.z,key.mValue.w});}
                    for(unsigned n=0;n<channel.mNumScalingKeys;++n)
                    {const auto& key=channel.mScalingKeys[n];scale.times.push_back(std::max(0.,key.mTime/ticks-begin));scale.values.push_back({key.mValue.x,key.mValue.y,key.mValue.z,0});}
                    SortKeys(translation);SortKeys(rotation);SortKeys(scale);const auto& rest=model->skeleton.bones[bone].reference;
                    const uint64_t samples=uint64_t(std::ceil(clip.duration*rate));
                    for(uint64_t n=0;n<=samples;++n)
                    {
                        const double time=n==samples?clip.duration:double(n)/rate;
                        if(!track.translation.times.empty()&&time<=track.translation.times.back())continue;
                        track.translation.times.push_back(time);track.rotation.times.push_back(time);track.scale.times.push_back(time);
                        track.translation.values.push_back(translation.Sample(time,{rest.translation.x,rest.translation.y,rest.translation.z,0}));
                        track.rotation.values.push_back(rotation.Sample(time,{rest.rotation.x,rest.rotation.y,rest.rotation.z,rest.rotation.w},true));
                        track.scale.values.push_back(scale.Sample(time,{rest.scale.x,rest.scale.y,rest.scale.z,0}));
                    }
                    clip.tracks.push_back(std::move(track));
                }
                model->clips.push_back(std::move(clip));
            }
        }
        if(settings.targetSkeleton&&!AnimationCompiler::BindAnimationSkeleton(*model,*settings.targetSkeleton,error))return {};
        if(!model->Validate(error))return {};return model;
    }
    catch(const std::exception& e){error=std::string("FBX/Assimp animation import: ")+e.what();return {};}
}
}
