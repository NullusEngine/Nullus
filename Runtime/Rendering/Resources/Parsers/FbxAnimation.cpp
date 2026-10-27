#include "FbxAnimation.h"
#include <AnimationCompiler/GltfAnimationImporter.h>
#include <Math/Vector4.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <unordered_map>
#if NLS_HAS_AUTODESK_FBX_SDK
#include <fbxsdk.h>
#endif

namespace NLS::Render::Resources::Parsers
{
#if NLS_HAS_AUTODESK_FBX_SDK
namespace
{
Maths::Matrix4 MatrixFromFbx(const FbxAMatrix& matrix,double scale)
{
    // Evaluate basis points instead of assuming the SDK's physical matrix layout.
    const auto o=matrix.MultT(FbxVector4(0,0,0,1));
    const auto x=matrix.MultT(FbxVector4(1,0,0,1)),y=matrix.MultT(FbxVector4(0,1,0,1)),z=matrix.MultT(FbxVector4(0,0,1,1));
    return {float(x[0]-o[0]),float(y[0]-o[0]),float(z[0]-o[0]),float(o[0]*scale),
        float(x[1]-o[1]),float(y[1]-o[1]),float(z[1]-o[1]),float(o[1]*scale),
        float(x[2]-o[2]),float(y[2]-o[2]),float(z[2]-o[2]),float(o[2]*scale),0,0,0,1};
}
}
#endif
std::shared_ptr<Animation::Model> BuildFbxSdkAnimation(void* sourceScene,
    const Assets::ImportedScene& imported,const std::vector<ParsedMeshData>& meshes,
    const Animation::ImportSettings& settings,double meshUnitScale,std::string& error)
{
#if NLS_HAS_AUTODESK_FBX_SDK
    using namespace Animation;
    auto* scene=static_cast<FbxScene*>(sourceScene);if(!scene)return {};
    if(!settings.importSkeleton&&!settings.importAnimations)return {};
    try
    {
        auto model=std::make_shared<Model>();model->reader="Autodesk.FBX.2020.3.9.Animation.v1";model->skeleton.id=settings.sourceId.empty()?"imported-skeleton":settings.sourceId;
        const double scale=meshUnitScale*settings.globalScale;
        std::vector<FbxNode*> nodes;std::unordered_map<FbxNode*,BoneIndex> indices;
        std::function<void(FbxNode*,BoneIndex,std::string)> add=[&](FbxNode* node,BoneIndex parent,std::string prefix)
        {
            if(!node)return;std::string name=node->GetName(),path=prefix+name;
            if(auto* owner=node->GetParent()){uint32_t count=0;for(int i=0;i<owner->GetChildCount();++i)if(std::string(owner->GetChild(i)->GetName())==name)++count;if(count>1)throw std::runtime_error("Ambiguous FBX skeletal path: "+path);}
            Transform reference;if(!Decompose(MatrixFromFbx(node->EvaluateLocalTransform(),scale),reference,error))throw std::runtime_error(path+": "+error);
            auto index=uint32_t(nodes.size());indices[node]=index;nodes.push_back(node);model->skeleton.bones.push_back({path,name,parent,reference});
            for(int i=0;i<node->GetChildCount();++i)add(node->GetChild(i),index,path+"/");
        };
        add(scene->GetRootNode(),NoBone,{});
        // Skin extraction uses the exact control-point map emitted during triangulation.
        if(settings.importSkeleton)for(size_t nodeIndex=0;nodeIndex<nodes.size();++nodeIndex)
        {
            auto* node=nodes[nodeIndex];auto* mesh=node->GetMesh();if(!mesh||!mesh->GetDeformerCount(FbxDeformer::eSkin))continue;
            const auto key="parser/node/"+std::to_string(nodeIndex);auto record=std::find_if(imported.nodes.begin(),imported.nodes.end(),[&](const auto& n){return n.sourceKey==key;});if(record==imported.nodes.end())throw std::runtime_error("FBX mesh node has no imported source identity");
            SkinBinding binding;binding.id="parser/skin/"+key;binding.skeleton=model->skeleton.id;binding.meshNode=key;
            binding.meshFromSkeleton=Maths::Matrix4::Inverse(MatrixFromFbx(node->EvaluateGlobalTransform(),scale));
            std::vector<std::vector<std::pair<uint32_t,float>>> influences(size_t(mesh->GetControlPointsCount()));
            std::map<FbxNode*,uint32_t> joints;
            for(int d=0;d<mesh->GetDeformerCount(FbxDeformer::eSkin);++d)
            {
                auto* skin=static_cast<FbxSkin*>(mesh->GetDeformer(d,FbxDeformer::eSkin));
                if(skin->GetSkinningType()!=FbxSkin::eLinear&&skin->GetSkinningType()!=FbxSkin::eRigid)throw std::runtime_error("FBX P0 supports linear or rigid skinning");
                for(int c=0;c<skin->GetClusterCount();++c)
                {
                    auto* cluster=skin->GetCluster(c);auto* link=cluster->GetLink();if(!link||!indices.contains(link))throw std::runtime_error("FBX skin cluster has no valid joint");
                    if(cluster->GetLinkMode()==FbxCluster::eAdditive)throw std::runtime_error("Additive FBX skin clusters require export as normalized linear skinning");
                    auto [found,inserted]=joints.emplace(link,uint32_t(joints.size()));
                    if(inserted){FbxAMatrix meshBind,jointBind;cluster->GetTransformMatrix(meshBind);cluster->GetTransformLinkMatrix(jointBind);binding.joints.push_back(indices.at(link));binding.inverseBind.push_back(MatrixFromFbx(jointBind.Inverse()*meshBind,scale));}
                    const auto* pointIndices=cluster->GetControlPointIndices();const auto* weights=cluster->GetControlPointWeights();
                    for(int k=0;k<cluster->GetControlPointIndicesCount();++k){if(pointIndices[k]<0||size_t(pointIndices[k])>=influences.size())throw std::runtime_error("FBX cluster control point is out of range");influences[size_t(pointIndices[k])].emplace_back(found->second,float(weights[k]));}
                }
            }
            FbxAMatrix geometric;geometric.SetIdentity();geometric.SetT(node->GetGeometricTranslation(FbxNode::eSourcePivot));geometric.SetR(node->GetGeometricRotation(FbxNode::eSourcePivot));geometric.SetS(node->GetGeometricScaling(FbxNode::eSourcePivot));
            const auto geometryMatrix=MatrixFromFbx(geometric,scale);
            for(const auto& parsed:meshes)if(parsed.sourceKey==record->meshKey||parsed.sourceKey.starts_with(record->meshKey+"/primitive/"))
            {
                if(parsed.sourceVertexIndices.size()!=parsed.vertices.size())throw std::runtime_error("FBX final vertices lost their control-point mapping");
                SkeletalMesh output;output.id=parsed.sourceKey+"/node/"+std::to_string(nodeIndex);output.name=node->GetName();output.material=parsed.materialIndex;output.skin=binding.id;output.indices=parsed.indices;
                bool trimmed=false;
                for(size_t v=0;v<parsed.vertices.size();++v)
                {
                    const auto& input=parsed.vertices[v];Vertex vertex;
                    auto p=geometryMatrix*Maths::Vector4(input.position[0]*float(settings.globalScale),input.position[1]*float(settings.globalScale),input.position[2]*float(settings.globalScale),1);
                    vertex.position[0]=p.x;vertex.position[1]=p.y;vertex.position[2]=p.z;std::copy_n(input.texCoords,2,vertex.texCoords);
                    const auto normalMatrix=Maths::Matrix4::Transpose(Maths::Matrix4::Inverse(geometryMatrix));
                    auto vector=[&](const float* a,float* b,const Maths::Matrix4& matrix){auto value=matrix*Maths::Vector4(a[0],a[1],a[2],0);const float length=std::sqrt(value.x*value.x+value.y*value.y+value.z*value.z);b[0]=length>0?value.x/length:0;b[1]=length>0?value.y/length:0;b[2]=length>0?value.z/length:0;};
                    vector(input.normals,vertex.normals,normalMatrix);vector(input.tangent,vertex.tangent,geometryMatrix);vector(input.bitangent,vertex.bitangent,geometryMatrix);
                    bool clipped=false;if(!PackInfluences(influences.at(parsed.sourceVertexIndices[v]),uint32_t(binding.joints.size()),vertex.influence,clipped,error))throw std::runtime_error(error);trimmed|=clipped;output.vertices.push_back(vertex);
                }
                if(trimmed)model->warnings.push_back("More than four Autodesk FBX influences were trimmed: "+output.name);model->meshes.push_back(std::move(output));
            }
            model->skins.push_back(std::move(binding));
        }
        if(settings.importAnimations)
        {
            double rate=settings.sampleRate;
            if(!(rate>0)){const auto mode=scene->GetGlobalSettings().GetTimeMode();rate=mode==FbxTime::eCustom?scene->GetGlobalSettings().GetCustomFrameRate():FbxTime::GetFrameRate(mode);}
            if(!(rate>0)){rate=30;model->warnings.push_back("FBX has no source frame rate; using 30 FPS");}
            if(!std::isfinite(rate)||rate<1||rate>1000)throw std::runtime_error("FBX animation sample rate must be 1..1000 FPS");
            std::map<std::string,uint32_t> takeNames;
            for(int stackIndex=0;stackIndex<scene->GetSrcObjectCount<FbxAnimStack>();++stackIndex)
            {
                auto* stack=scene->GetSrcObject<FbxAnimStack>(stackIndex);Clip clip;clip.name=stack->GetName();
                const auto ordinal=takeNames[clip.name]++;clip.id="parser/animation/"+clip.name+(ordinal?"#"+std::to_string(ordinal):std::string{});clip.skeleton=model->skeleton.id;
                if(!settings.takes.empty()&&std::find(settings.takes.begin(),settings.takes.end(),clip.name)==settings.takes.end())continue;
                scene->SetCurrentAnimationStack(stack);scene->GetAnimationEvaluator()->Reset();
                const auto span=stack->GetLocalTimeSpan();const double begin=span.GetStart().GetSecondDouble(),end=span.GetStop().GetSecondDouble();
                clip.duration=std::max(0.,end-begin);if(!std::isfinite(clip.duration)||clip.duration*rate>10000000)throw std::runtime_error("FBX take exceeds animation sample limits");
                clip.tracks.resize(nodes.size());for(size_t i=0;i<nodes.size();++i)clip.tracks[i].bone=model->skeleton.bones[i].id;
                const uint64_t count=uint64_t(std::ceil(clip.duration*rate));
                for(uint64_t sample=0;sample<=count;++sample)
                {
                    const double offset=sample==count?clip.duration:double(sample)/rate;FbxTime time;time.SetSecondDouble(begin+offset);
                    for(size_t i=0;i<nodes.size();++i)
                    {
                        auto* node=nodes[i];auto local=node->EvaluateGlobalTransform(time);
                        if(auto* parent=node->GetParent())local=parent->EvaluateGlobalTransform(time).Inverse()*local;
                        Transform transform;if(!Decompose(MatrixFromFbx(local,scale),transform,error))throw std::runtime_error(clip.name+" / "+model->skeleton.bones[i].id+": "+error);
                        auto& track=clip.tracks[i];if(!track.translation.times.empty()&&offset<=track.translation.times.back())continue;
                        track.translation.times.push_back(offset);track.rotation.times.push_back(offset);track.scale.times.push_back(offset);
                        track.translation.values.push_back({transform.translation.x,transform.translation.y,transform.translation.z,0});
                        track.rotation.values.push_back({transform.rotation.x,transform.rotation.y,transform.rotation.z,transform.rotation.w});
                        track.scale.values.push_back({transform.scale.x,transform.scale.y,transform.scale.z,0});
                    }
                }
                model->clips.push_back(std::move(clip));
            }
        }
        if(model->skins.empty()&&model->clips.empty())return {};
        if(settings.targetSkeleton&&!AnimationCompiler::BindAnimationSkeleton(*model,*settings.targetSkeleton,error))return {};
        if(!model->Validate(error))return {};return model;
    }
    catch(const std::exception& e){error=std::string("Autodesk FBX animation import: ")+e.what();return {};}
#else
    (void)sourceScene;(void)imported;(void)meshes;(void)settings;(void)meshUnitScale;(void)error;return {};
#endif
}
}
